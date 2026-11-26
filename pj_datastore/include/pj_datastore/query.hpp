#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Read primitives over a topic's chunks, normally reached through DataReader, which hands them the engine lock.

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {

/// Both ends inclusive.
struct QueryRange {
  PJ::TopicId topic_id = 0;
  PJ::Timestamp t_min = 0;
  PJ::Timestamp t_max = 0;
};

struct QueryPoint {
  PJ::TopicId topic_id = 0;
  PJ::Timestamp t = 0;
};

/// A series is one topic column seen as a time series; rows where the column is null are not samples.
struct SeriesSample {
  PJ::Timestamp timestamp = 0;
  double value = 0.0;
  /// Valid while the lock that produced it is held.
  const TopicChunk* chunk = nullptr;
  std::size_t row_index = 0;
};

struct SeriesBounds {
  PJ::Range<PJ::Timestamp> time;
  /// Finite values only.
  PJ::Range<double> value;
  std::size_t sample_count = 0;
};

struct SampleRow {
  PJ::Timestamp timestamp = 0;
  /// Valid while the lock that produced it is held.
  const TopicChunk* chunk = nullptr;
  std::size_t row_index = 0;
};

/// DataReader::latestAt()'s result, read out under the engine lock so no TopicChunk* escapes. `values[N]` is column
/// N (a scalar topic's value is `values[0]`); the size is the column count of the row's chunk.
struct MaterializedSample {
  PJ::Timestamp timestamp = 0;
  /// Nulls read as 0.0, as readNumericAsDouble() does.
  std::vector<double> values;
};

/// One string cell, its row's time and its row (`DataReader::latestStringAt`). A row never changes under its chunk's
/// id, and an id is never reused, so `chunk_id` and `row_index` name the cell's content for as long as it is retained.
struct StringSample {
  PJ::Timestamp timestamp = 0;
  std::optional<std::string> value;  ///< nullopt: the cell is null
  ChunkId chunk_id = 0;
  std::size_t row_index = 0;
};

struct ChunkRowRange {
  const TopicChunk* chunk = nullptr;
  std::size_t row_start = 0;
  std::size_t row_end = 0;  // exclusive
};

/// One chunk's next unread row. Chunks may overlap in time (out-of-order ingest), so row cursors keep one frontier
/// per chunk in a min-heap on (ts, chunk index); the tie-break keeps equal timestamps in commit order.
struct CursorFrontier {
  PJ::Timestamp ts = 0;
  std::size_t chunk = 0;
  std::size_t row = 0;
};

/// Rows of [t_min, t_max], iterated lazily.
class RangeCursor {
 public:
  /// Without a lock: single-threaded callers only.
  RangeCursor(const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max);

  /// Keeps the engine lock for the cursor's lifetime: iteration happens after the query returns.
  RangeCursor(
      const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max,
      std::unique_lock<std::recursive_mutex> lock);

  RangeCursor(RangeCursor&&) = default;
  RangeCursor& operator=(RangeCursor&&) = default;
  RangeCursor(const RangeCursor&) = delete;
  RangeCursor& operator=(const RangeCursor&) = delete;

  [[nodiscard]] bool valid() const noexcept;

  void advance();

  [[nodiscard]] SampleRow current() const;

  /// Ascending timestamp order across overlapping chunks.
  void forEach(std::function<void(const SampleRow&)> callback);

  /// One sorted run per chunk, in commit order: runs of different chunks may overlap in time, so a consumer that
  /// needs global order uses forEach(). Exhausts the cursor.
  void forEachChunk(std::function<void(const ChunkRowRange&)> callback);

  /// forEach()'s order, delivered as maximal runs of one chunk: a chunk that overlaps no other comes as one run.
  /// Exhausts the cursor.
  void forEachOrderedRun(const std::function<void(const ChunkRowRange&)>& callback);

 private:
  const std::deque<TopicChunk>* chunks_;
  PJ::Timestamp t_min_;
  PJ::Timestamp t_max_;
  // Empty when exhausted.
  std::vector<CursorFrontier> frontiers_;
  std::unique_lock<std::recursive_mutex> lock_;

  void initFrontiers();
};

/// One column's non-null samples in ascending timestamp order across overlapping chunks. Made by
/// SeriesReader::samples(); it borrows that reader's chunks and holds no lock, so it must not outlive the reader.
class SeriesCursor {
 public:
  /// Rows below `retention_floor` are never yielded.
  SeriesCursor(
      const std::deque<TopicChunk>& chunks, std::size_t column_index, PJ::Range<PJ::Timestamp> time_range,
      PJ::Timestamp retention_floor = kNoRetentionFloor);

  [[nodiscard]] bool valid() const noexcept;

  void advance();

  [[nodiscard]] SeriesSample current() const;

  void forEach(std::function<void(const SeriesSample&)> callback);

 private:
  const std::deque<TopicChunk>* chunks_;
  std::size_t column_index_ = 0;
  PJ::Range<PJ::Timestamp> time_range_;
  // Empty when exhausted.
  std::vector<CursorFrontier> frontiers_;

  void initFrontiers();
  // False when the chunk has no more samples in range.
  [[nodiscard]] bool nextSample(CursorFrontier& frontier) const;
};

/// One column as a virtual vector of its non-null samples. No method exposes a sample below the retention floor.
class SeriesReader {
 public:
  /// Without a lock: single-threaded callers only.
  SeriesReader(
      const std::deque<TopicChunk>& chunks, std::size_t column_index,
      PJ::Timestamp retention_floor = kNoRetentionFloor);

  /// Keeps the engine lock for the reader's lifetime, which also covers the SeriesCursors it makes.
  SeriesReader(
      const std::deque<TopicChunk>& chunks, std::size_t column_index, PJ::Timestamp retention_floor,
      std::unique_lock<std::recursive_mutex> lock);

  SeriesReader(SeriesReader&&) = default;
  SeriesReader& operator=(SeriesReader&&) = default;
  SeriesReader(const SeriesReader&) = delete;
  SeriesReader& operator=(const SeriesReader&) = delete;

  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] bool empty() const;

  /// O(n) per call (it scans from the first chunk): iterate with samples() instead of looping over indices. Assumes
  /// chunks in time order.
  [[nodiscard]] std::optional<SeriesSample> sampleAt(std::size_t index) const;

  /// Assumes chunks in time order.
  [[nodiscard]] std::optional<std::size_t> indexAtOrBeforeTime(PJ::Timestamp t) const;

  /// Assumes chunks in time order.
  [[nodiscard]] std::optional<std::size_t> indexAtOrAfterTime(PJ::Timestamp t) const;

  /// Correct across overlapping chunks, like the three below (unlike the index lookups above).
  [[nodiscard]] std::optional<SeriesSample> sampleAtOrBeforeTime(PJ::Timestamp t) const;

  [[nodiscard]] std::optional<SeriesSample> sampleAtOrAfterTime(PJ::Timestamp t) const;

  [[nodiscard]] std::optional<SeriesSample> sampleBeforeTime(PJ::Timestamp t) const;

  [[nodiscard]] std::optional<SeriesSample> sampleAfterTime(PJ::Timestamp t) const;

  /// Both ends inclusive.
  [[nodiscard]] SeriesCursor samples(PJ::Range<PJ::Timestamp> time_range) const;

  [[nodiscard]] PJ::Timestamp retentionFloor() const noexcept {
    return retention_floor_;
  }

  /// TopicStorage::seriesGeneration() when DataReader::series() made this reader (0 otherwise). A cache built under
  /// the same generation can absorb the change as in-order appends plus a rising retention floor.
  [[nodiscard]] uint64_t seriesGeneration() const noexcept {
    return series_generation_;
  }
  void setSeriesGeneration(uint64_t generation) noexcept {
    series_generation_ = generation;
  }

  /// Reads each chunk wholly inside the window from its statistics; only edge chunks, and chunks with nulls or
  /// non-finite values, are scanned row by row.
  [[nodiscard]] std::optional<SeriesBounds> bounds() const;

  /// Both ends inclusive.
  [[nodiscard]] std::optional<SeriesBounds> bounds(PJ::Range<PJ::Timestamp> time_range) const;

 private:
  const std::deque<TopicChunk>* chunks_;
  std::size_t column_index_ = 0;
  PJ::Timestamp retention_floor_ = kNoRetentionFloor;
  uint64_t series_generation_ = 0;
  std::unique_lock<std::recursive_mutex> lock_;
};

// nullopt also when the newest sample at or before t is below `retention_floor`, so a zero-order hold never reaches
// across the retention boundary.
[[nodiscard]] std::optional<SampleRow> latestAt(
    const std::deque<TopicChunk>& chunks, PJ::Timestamp t, PJ::Timestamp retention_floor = kNoRetentionFloor);

// latestAt among the chunks `include` accepts only, e.g. those holding a column that appeared mid-stream.
[[nodiscard]] std::optional<SampleRow> latestAt(
    const std::deque<TopicChunk>& chunks, PJ::Timestamp t, PJ::Timestamp retention_floor,
    const std::function<bool(const TopicChunk&)>& include);

[[nodiscard]] RangeCursor rangeQuery(const std::deque<TopicChunk>& chunks, PJ::Timestamp t_min, PJ::Timestamp t_max);

}  // namespace PJ
