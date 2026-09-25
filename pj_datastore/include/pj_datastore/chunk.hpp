#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/buffer.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/encoding.hpp"

namespace PJ {

// Import base types into engine namespace
using PJ::BitSpan;
using PJ::ChunkId;
using PJ::kInvalidChunkId;
using PJ::SchemaId;
using PJ::Span;
using PJ::Timestamp;
using PJ::TopicId;

/// "No retention floor" sentinel. A read accessor treats a row as logically
/// evicted only when its timestamp is < the floor, so the sentinel must sit
/// below every real stamp: numeric_limits<Timestamp>::min(), NOT 0 (Timestamp
/// is signed and 0 is a valid timestamp).
inline constexpr Timestamp kNoRetentionFloor = std::numeric_limits<Timestamp>::min();

struct ColumnStats {
  /// Number of null rows in this column within the chunk.
  uint32_t null_count = 0;
  /// Number of value runs (for compression heuristics).
  uint32_t run_count = 0;
  /// True if all non-null values are equal.
  bool is_constant = true;
  /// Minimum non-null numeric value, if applicable.
  std::optional<double> min_value;
  /// Maximum non-null numeric value, if applicable.
  std::optional<double> max_value;
  /// Float columns only (false otherwise): every non-null value in the chunk is a
  /// whole number (encoding::isWholeInt64; -0.0 counts). Set at seal; drives
  /// frame-of-reference encoding and TopicStorage::wholeNumbersOnly().
  bool all_integral = false;
};

/// Per-chunk aggregate statistics.
struct ChunkStats {
  /// Minimum timestamp in chunk.
  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  /// Maximum timestamp in chunk.
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  /// Number of rows in chunk.
  uint32_t row_count = 0;
  /// Approximate encoded bytes of the sealed chunk (timestamps + column
  /// buffers + validity bitmaps). Memoized by TopicStorage::appendSealedChunk
  /// (0 until then) so metadata scans never re-walk column buffers; travels
  /// with the chunk across flushTo/detach moves.
  uint64_t encoded_byte_size = 0;
  /// Per-column statistics aligned to schema columns.
  std::vector<ColumnStats> column_stats;
};

/// Immutable sealed storage unit produced by TopicChunkBuilder::seal(): a
/// timestamp column plus per-column EncodedData + optional validity bitmap.
/// Read accessors do NOT null-check unless noted (readColumnAsDoubles fills NaN);
/// readString() views chunk-internal dictionary memory.
struct TopicChunk {
  /// Chunk identifier.
  ChunkId id = 0;
  /// Owning topic id.
  TopicId topic_id = 0;
  /// Schema version used when chunk was produced.
  SchemaId schema_version = 0;
  /// Chunk-level and column-level stats.
  ChunkStats stats;

  // Raw timestamp column (one int64 per row)
  /// One timestamp per row.
  std::vector<Timestamp> timestamps;

  struct Column {
    encoding::EncodedData data;
    std::optional<BitVector> validity_bitmap;
    std::shared_ptr<const ColumnDescriptor> descriptor;
  };

  std::vector<Column> columns;

  /// Derive encoding type from the EncodedData variant index.
  [[nodiscard]] EncodingType columnEncoding(std::size_t index) const;

  /// Read timestamp at row index.
  [[nodiscard]] Timestamp readTimestamp(std::size_t row) const;

  /// Bulk-read timestamps into `out` starting at `row_start`.
  void readTimestamps(Span<Timestamp> out, std::size_t row_start) const;

  /// Read numeric value at `col_index,row` as double.
  [[nodiscard]] double readNumericAsDouble(std::size_t col_index, std::size_t row) const;

  /// Read numeric value at `col_index,row` as int64_t.
  [[nodiscard]] int64_t readNumericAsInt64(std::size_t col_index, std::size_t row) const;

  /// Read numeric value at `col_index,row` as uint64_t.
  [[nodiscard]] uint64_t readNumericAsUint64(std::size_t col_index, std::size_t row) const;

  /// Read string value at `col_index,row`.
  [[nodiscard]] std::string_view readString(std::size_t col_index, std::size_t row) const;

  /// Read boolean value at `col_index,row`.
  [[nodiscard]] bool readBool(std::size_t col_index, std::size_t row) const;

  /// Return true if value at `col_index,row` is null.
  [[nodiscard]] bool isNull(std::size_t col_index, std::size_t row) const;

  // Bulk read: switch on type once, then tight inner loop.
  // For kBool/kString columns, fills NaN.
  /// Decode a numeric range into `out` starting at `row_start`.
  void readColumnAsDoubles(std::size_t col_index, Span<double> out, std::size_t row_start) const;
};

/// Mutable accumulator for one topic's rows; sealed into an immutable TopicChunk.
/// Two append paths (row-at-a-time begin/set/finish, and bulk
/// appendTimestamps/appendColumn/finishBulkAppend) that must not be interleaved
/// within a row. Picks per-column encoding at seal() time from accumulated stats.
class TopicChunkBuilder {
 public:
  /// Create a builder for one topic/schema pair.
  TopicChunkBuilder(TopicId topic_id, SchemaId schema_id, std::vector<ColumnDescriptor> columns, uint32_t max_rows);

  // Start a new row with the given timestamp
  /// Begin a new row at `timestamp`.
  void beginRow(Timestamp timestamp);

  /// Set a typed value for the current row.
  /// Supported T: float, double, int32_t, int64_t, uint64_t, bool, std::string_view.
  template <typename T>
  void set(std::size_t col_index, T value);

  /// Mark value as null for current row.
  void setNull(std::size_t col_index);

  // Finalize the current row (append all columns)
  /// Finalize current row, auto-filling unset columns with null.
  void finishRow();

  // ---- Bulk column append ----
  // Call appendTimestamps first, then appendColumn for each column,
  // then appendColumnValidity for columns with nulls, then finishBulkAppend.
  // Stats are computed in finishBulkAppend using the column's validity bitmap.

  /// Append a contiguous timestamp batch.
  void appendTimestamps(Span<const Timestamp> timestamps);

  /// Append a typed column batch.
  /// Supported T: float, double, int32_t, int64_t, uint64_t, uint8_t (bool bytes).
  template <typename T>
  void appendColumn(std::size_t col_index, Span<const T> data);

  /// Append a string column batch from offsets+data views.
  void appendColumnStrings(std::size_t col_index, Span<const uint32_t> offsets, Span<const char> data);

  /// Append validity bits for last appended rows of this column.
  void appendColumnValidity(std::size_t col_index, BitSpan validity);

  /// Finalize pending bulk append and compute stats.
  void finishBulkAppend();

  /// Remaining row capacity before auto-seal.
  [[nodiscard]] uint32_t remainingCapacity() const noexcept;

  /// True if no more rows can be appended.
  [[nodiscard]] bool isFull() const noexcept;

  /// Number of finalized rows.
  [[nodiscard]] uint32_t rowCount() const noexcept;

  /// True if beginRow() has been called but finishRow() has not yet been called.
  [[nodiscard]] bool isRowInProgress() const noexcept;

  /// Current chunk statistics.
  [[nodiscard]] const ChunkStats& stats() const noexcept;

  /// Last appended timestamp.
  [[nodiscard]] Timestamp lastTimestamp() const noexcept;

  // Seal: finalize stats, apply encodings, produce immutable TopicChunk
  /// Seal builder into immutable chunk.
  [[nodiscard]] TopicChunk seal();

 private:
  TopicId topic_id_;
  SchemaId schema_id_;
  uint32_t max_rows_;
  static inline std::atomic<ChunkId> next_chunk_id{1};  // monotonic counter

  std::vector<Timestamp> timestamps_;
  std::vector<TypedColumnBuffer> columns_;
  std::vector<ColumnDescriptor> column_descriptors_;
  ChunkStats stats_;

  // Track per-row state during begin_row/finish_row
  bool row_in_progress_ = false;
  Timestamp current_timestamp_ = 0;

  Timestamp last_timestamp_ = std::numeric_limits<Timestamp>::min();
  std::vector<double> last_column_values_;
  std::size_t bulk_pending_rows_ = 0;  // rows added via bulk but not yet finished

  void updateColumnStats(std::size_t col_index, double value);

  // Stable-sort buffered rows by timestamp (no-op when already sorted).
  // Called by seal() so out-of-order ingest still produces sorted chunks.
  void sortRowsByTimestamp();

  // Bulk stats computation: single pass over column buffer data.
  // Called by finishBulkAppend() after both data and validity are set.
  // Reads from column buffer and skips null positions via validity bitmap.
  void computeBulkNumericStats(std::size_t col_index, StorageKind kind, std::size_t first_row, std::size_t count);

  void computeBulkStringStats(std::size_t col_index, std::size_t first_row, std::size_t count);
};

}  // namespace PJ
