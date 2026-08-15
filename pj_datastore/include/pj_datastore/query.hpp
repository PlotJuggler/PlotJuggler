#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Read-side query primitives over committed chunk deques: RangeCursor (rows),
// SeriesReader/SeriesCursor (one column as a value-bearing time series), and
// latestAt(). Reached via DataReader. See docs/USER_GUIDE.md §5.

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {

struct QueryRange {
  /// Topic to query.
  PJ::TopicId topic_id = 0;
  /// Inclusive range start.
  PJ::Timestamp t_min = 0;
  /// Inclusive range end.
  PJ::Timestamp t_max = 0;
};

/// Point query descriptor for latest-at lookup.
struct QueryPoint {
  /// Topic to query.
  PJ::TopicId topic_id = 0;
  /// Query timestamp.
  PJ::Timestamp t = 0;
};

/// One materialized series sample. A series is a topic column viewed as a
/// time series; rows where the column is null are not samples.
struct SeriesSample {
  /// Sample timestamp.
  PJ::Timestamp timestamp = 0;
  /// Numeric sample value, converted to double for display/analysis.
  double value = 0.0;
  /// Pointer to source chunk containing this sample.
  const TopicChunk* chunk = nullptr;
  /// Physical row index inside `chunk`.
  std::size_t row_index = 0;
};

/// Bounds for a series over a query window.
struct SeriesBounds {
  /// Time range covered by value-bearing samples.
  PJ::Range<PJ::Timestamp> time;
  /// Finite value range covered by value-bearing samples.
  PJ::Range<double> value;
  /// Number of value-bearing samples in the range.
  std::size_t sample_count = 0;
};

/// One materialized row reference returned by cursors.
struct SampleRow {
  /// Sample timestamp.
  PJ::Timestamp timestamp = 0;
  /// Pointer to source chunk containing this row.
  const TopicChunk* chunk = nullptr;
  /// Row index inside `chunk`.
  std::size_t row_index = 0;
};

/// A latest-at result with the row's column values already read out (under the
/// engine lock), so no raw TopicChunk* escapes to the caller. Returned by
/// DataReader::latestAt; column N's value is `values[N]` (a scalar topic's value
/// is `values[0]`). `values.size()` is the column count of the row's chunk.
struct MaterializedSample {
  /// Sample timestamp.
  PJ::Timestamp timestamp = 0;
  /// Numeric value of each column at that row, read as double (NaN-free: nulls
  /// read as 0.0, matching readNumericAsDouble).
  std::vector<double> values;
};

/// Contiguous row interval inside one chunk.
struct ChunkRowRange {
  /// Source chunk.
  const TopicChunk* chunk = nullptr;
  /// Inclusive start row.
  std::size_t row_start = 0;
  /// Exclusive end row.
  std::size_t row_end = 0;  // exclusive
};

/// One chunk's next unread row during a cursor merge. Chunks of a topic may
/// overlap in time (out-of-order ingest), so row-at-a-time cursors keep one
/// frontier per intersecting chunk in a min-heap on (ts, chunk index) — the
/// tie-break keeps duplicate timestamps in commit order.
struct CursorFrontier {
  PJ::Timestamp ts = 0;
  std::size_t chunk = 0;
  std::size_t row = 0;
};

// Cursor for iterating range query results across chunks
class RangeCursor {
 public:
  /// Construct cursor over [t_min, t_max] from committed chunks.
  RangeCursor(const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max);

  /// Same, but ADOPTS the engine lock for the cursor's lifetime so the backing
  /// chunks stay stable through the lazy iteration (which happens after rangeQuery
  /// returns). DataReader uses this; single-threaded callers use the overload above.
  RangeCursor(
      const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max,
      std::unique_lock<std::recursive_mutex> lock);

  // Move-only: the adopted engine lock is not copyable.
  RangeCursor(RangeCursor&&) = default;
  RangeCursor& operator=(RangeCursor&&) = default;
  RangeCursor(const RangeCursor&) = delete;
  RangeCursor& operator=(const RangeCursor&) = delete;

  [[nodiscard]] bool valid() const noexcept;

  /// Advance to next matching row.
  void advance();

  /// Return current row descriptor.
  [[nodiscard]] SampleRow current() const;

  /// Iterate all results via callback (per-row), in globally ascending
  /// timestamp order even when chunk time ranges overlap.
  void forEach(std::function<void(const SampleRow&)> callback);

  /// Iterate chunk-at-a-time (bulk path). Runs are per-chunk sorted and
  /// delivered in commit order; under out-of-order ingest, runs from different
  /// chunks may overlap in time — bulk consumers must tolerate that (or use
  /// forEach for a globally ordered stream). Exhausts the cursor.
  void forEachChunk(std::function<void(const ChunkRowRange&)> callback);

 private:
  const std::deque<TopicChunk>* chunks_;
  PJ::Timestamp t_min_;
  PJ::Timestamp t_max_;
  // Min-heap; empty == exhausted. See CursorFrontier.
  std::vector<CursorFrontier> frontiers_;
  // Engine lock held for this cursor's lifetime when built via DataReader (empty
  // for the lock-free constructor), keeping the chunk deque stable during iteration.
  std::unique_lock<std::recursive_mutex> lock_;

  void initFrontiers();
};

/// Cursor for iterating a topic column as a time series. It skips null rows by
/// definition; every current() value is a value-bearing sample for the bound
/// column. Created only via SeriesReader::samples(): it BORROWS that reader's chunk
/// deque and holds no lock of its own, so it MUST NOT outlive the SeriesReader
/// (whose adopted lock, if any, is what keeps the chunks stable while it iterates).
class SeriesCursor {
 public:
  /// Construct cursor over [time_range.min, time_range.max] from committed chunks.
  /// The effective lower bound is raised to `retention_floor` (the "no floor"
  /// value is kNoRetentionFloor), so logically-evicted rows are never yielded and
  /// a window lying entirely below the floor yields nothing.
  SeriesCursor(
      const std::deque<TopicChunk>& chunks, std::size_t column_index, PJ::Range<PJ::Timestamp> time_range,
      PJ::Timestamp retention_floor = kNoRetentionFloor);

  [[nodiscard]] bool valid() const noexcept;

  /// Advance to next matching sample.
  void advance();

  /// Return current sample descriptor.
  [[nodiscard]] SeriesSample current() const;

  /// Iterate all results via callback.
  void forEach(std::function<void(const SeriesSample&)> callback);

 private:
  const std::deque<TopicChunk>* chunks_;
  std::size_t column_index_ = 0;
  PJ::Range<PJ::Timestamp> time_range_;
  // Min-heap of value-bearing frontiers; empty == exhausted. Yields samples in
  // globally ascending timestamp order across overlapping chunks.
  std::vector<CursorFrontier> frontiers_;

  void initFrontiers();
  // Move `frontier` to its next value-bearing row inside the time range.
  // Returns false when this chunk is exhausted for the cursor's range.
  [[nodiscard]] bool nextSample(CursorFrontier& frontier) const;
};

/// View a topic column as a virtual vector of value-bearing time series
/// samples. Null rows are storage details and are not visible through this API.
class SeriesReader {
 public:
  /// Construct a series reader over committed chunks. Samples with timestamp <
  /// `retention_floor` are logically evicted and are never exposed by ANY method
  /// (size/sampleAt/index lookups/samples/bounds) — the retention-window
  /// contract. The "no floor" value is kNoRetentionFloor; the floor only raises
  /// the read lower bound, never mutating the chunks.
  SeriesReader(
      const std::deque<TopicChunk>& chunks, std::size_t column_index,
      PJ::Timestamp retention_floor = kNoRetentionFloor);

  /// Same, but ADOPTS the engine lock for this reader's lifetime — and so for any
  /// SeriesCursor it spawns via samples() (which borrows this reader's chunks).
  /// DataReader::series uses this; the overload above is for single-threaded callers.
  SeriesReader(
      const std::deque<TopicChunk>& chunks, std::size_t column_index, PJ::Timestamp retention_floor,
      std::unique_lock<std::recursive_mutex> lock);

  // Move-only: the adopted engine lock is not copyable.
  SeriesReader(SeriesReader&&) = default;
  SeriesReader& operator=(SeriesReader&&) = default;
  SeriesReader(const SeriesReader&) = delete;
  SeriesReader& operator=(const SeriesReader&) = delete;

  /// Number of samples in the virtual series.
  [[nodiscard]] std::size_t size() const;

  /// True when the virtual series contains no samples.
  [[nodiscard]] bool empty() const;

  /// Return the sample at a virtual series index.
  [[nodiscard]] std::optional<SeriesSample> sampleAt(std::size_t index) const;

  /// Return the virtual series index of the latest sample at or before `t`.
  [[nodiscard]] std::optional<std::size_t> indexAtOrBeforeTime(PJ::Timestamp t) const;

  /// Return the virtual series index of the first sample at or after `t`.
  [[nodiscard]] std::optional<std::size_t> indexAtOrAfterTime(PJ::Timestamp t) const;

  /// Return the latest sample at or before `t`. Merge-aware: correct even when
  /// chunk time ranges overlap after out-of-order ingest (unlike the
  /// virtual-index lookups above, which assume chronological chunks).
  [[nodiscard]] std::optional<SeriesSample> sampleAtOrBeforeTime(PJ::Timestamp t) const;

  /// Return the first sample at or after `t`. Merge-aware (see above).
  [[nodiscard]] std::optional<SeriesSample> sampleAtOrAfterTime(PJ::Timestamp t) const;

  /// Return the latest sample strictly before `t`. Merge-aware (see above).
  [[nodiscard]] std::optional<SeriesSample> sampleBeforeTime(PJ::Timestamp t) const;

  /// Return the first sample strictly after `t`. Merge-aware (see above).
  [[nodiscard]] std::optional<SeriesSample> sampleAfterTime(PJ::Timestamp t) const;

  /// Iterate samples in an inclusive time range.
  [[nodiscard]] SeriesCursor samples(PJ::Range<PJ::Timestamp> time_range) const;

  /// Retention floor this reader applies (kNoRetentionFloor = none).
  [[nodiscard]] PJ::Timestamp retentionFloor() const noexcept {
    return retention_floor_;
  }

  /// Topic series-content generation at snapshot time (populated by
  /// DataReader::series; 0 for the lock-free constructor). A consumer whose
  /// cache was built under an equal generation may absorb the delta as
  /// strictly in-order appends plus a retention-floor rise — see
  /// TopicStorage::seriesGeneration().
  [[nodiscard]] uint64_t seriesGeneration() const noexcept {
    return series_generation_;
  }
  void setSeriesGeneration(uint64_t generation) noexcept {
    series_generation_ = generation;
  }

  /// Return bounds over the entire series.
  [[nodiscard]] std::optional<SeriesBounds> bounds() const;

  /// Return bounds over an inclusive time range.
  [[nodiscard]] std::optional<SeriesBounds> bounds(PJ::Range<PJ::Timestamp> time_range) const;

 private:
  const std::deque<TopicChunk>* chunks_;
  std::size_t column_index_ = 0;
  // Samples with timestamp < this are invisible to every method (retention
  // floor). kNoRetentionFloor = no floor. Set by DataReader::series() from the
  // topic's floor.
  PJ::Timestamp retention_floor_ = kNoRetentionFloor;
  uint64_t series_generation_ = 0;  // see seriesGeneration()
  // Engine lock held for this reader's lifetime when built via DataReader (empty
  // for the lock-free constructor). A SeriesCursor from samples() borrows chunks_
  // and relies on this reader (and this lock) outliving it.
  std::unique_lock<std::recursive_mutex> lock_;
};

// Find the most recent sample at or before time t; nullopt if none exists, or
// if the latest such sample is below `retention_floor` (logically evicted) —
// this keeps a zero-order hold from reaching across the retention boundary. The
// "no floor" value is kNoRetentionFloor.
[[nodiscard]] std::optional<SampleRow> latestAt(
    const std::deque<TopicChunk>& chunks, PJ::Timestamp t, PJ::Timestamp retention_floor = kNoRetentionFloor);

// Create a range cursor
[[nodiscard]] RangeCursor rangeQuery(const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max);

}  // namespace PJ
