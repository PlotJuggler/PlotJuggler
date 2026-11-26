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

using PJ::BitSpan;
using PJ::ChunkId;
using PJ::kInvalidChunkId;
using PJ::SchemaId;
using PJ::Span;
using PJ::Timestamp;
using PJ::TopicId;

/// "No retention floor": below every real stamp. Not 0, because Timestamp is signed and 0 is a valid stamp.
inline constexpr Timestamp kNoRetentionFloor = std::numeric_limits<Timestamp>::min();

struct ColumnStats {
  uint32_t null_count = 0;
  /// Number of value runs (for compression heuristics), counted in arrival order: seal sorts the rows by timestamp
  /// afterwards, so it is not the stored rows' run count once rows arrived out of order.
  uint32_t run_count = 0;
  /// True if all non-null values are equal.
  bool is_constant = true;
  /// min and max cover the non-null numeric values.
  std::optional<double> min_value;
  std::optional<double> max_value;
  /// Float columns only: every non-null value is a whole number (-0.0 counts). Drives frame-of-reference encoding
  /// and TopicStorage::wholeNumbersOnly().
  bool all_integral = false;
};

struct ChunkStats {
  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  uint32_t row_count = 0;
  /// Approximate encoded size. 0 until TopicStorage::appendSealedChunk() memoizes it, so metadata scans never walk
  /// the column buffers; it moves with the chunk.
  uint64_t encoded_byte_size = 0;
  /// Aligned with the chunk's columns.
  std::vector<ColumnStats> column_stats;
};

/// A topic's rows, sorted by timestamp, in one column layout. A row never changes under its chunk's id; only a topic's
/// last chunk changes, by gaining rows at its end (TopicStorage::appendRows) and by re-encoding, so a consumer that
/// remembers a chunk id also remembers how many of its rows it read. The read accessors do not check for nulls:
/// readNumericAsDouble() returns 0.0 at a null, so call isNull() first or use readColumnAsDoubles().
struct TopicChunk {
  ChunkId id = 0;
  /// 0, or the first id these rows had in their topic: set when TopicStorage gives the chunk a new id in place (late
  /// rows merged in, or an id above those a consumer may have read). A consumer whose watermark id is in
  /// [replaced_id, id) may have read some of these rows already.
  ChunkId replaced_id = 0;
  TopicId topic_id = 0;
  /// The topic's schema when the chunk was sealed; the topic may have moved on.
  SchemaId schema_version = 0;
  ChunkStats stats;

  /// One per row, ascending (seal() sorts the rows).
  std::vector<Timestamp> timestamps;

  struct Column {
    encoding::EncodedData data;
    std::optional<BitVector> validity_bitmap;
    std::shared_ptr<const ColumnDescriptor> descriptor;
  };

  std::vector<Column> columns;

  /// The id these rows first had in their topic.
  [[nodiscard]] ChunkId firstId() const noexcept {
    return replaced_id != 0 ? replaced_id : id;
  }

  [[nodiscard]] EncodingType columnEncoding(std::size_t index) const;

  [[nodiscard]] Timestamp readTimestamp(std::size_t row) const;

  void readTimestamps(Span<Timestamp> out, std::size_t row_start) const;

  /// 0.0 at a null.
  [[nodiscard]] double readNumericAsDouble(std::size_t col_index, std::size_t row) const;

  [[nodiscard]] int64_t readNumericAsInt64(std::size_t col_index, std::size_t row) const;

  [[nodiscard]] uint64_t readNumericAsUint64(std::size_t col_index, std::size_t row) const;

  /// Views chunk-owned memory: valid while the chunk lives.
  [[nodiscard]] std::string_view readString(std::size_t col_index, std::size_t row) const;

  [[nodiscard]] bool readBool(std::size_t col_index, std::size_t row) const;

  [[nodiscard]] bool isNull(std::size_t col_index, std::size_t row) const;

  /// NaN at nulls, and for bool and string columns.
  void readColumnAsDoubles(std::size_t col_index, Span<double> out, std::size_t row_start) const;

  // ---- Growing in place: TopicStorage only ----

  /// True when every column is stored in a form rows can be appended to: raw numbers, packed bools, a dictionary with
  /// 4-byte indices. seal() and compact() pick tighter encodings.
  [[nodiscard]] bool isGrowable() const;

  /// Re-stores every column in growable form. Rows and values are unchanged (a null's placeholder may differ).
  void makeGrowable();

  /// Picks each column's encoding as seal() does for the same rows, and gives back the buffers' spare capacity. Id,
  /// rows, values and stats are unchanged. Frees and reallocates the column buffers.
  void compact();
};

/// Accumulates one topic's rows and seals them into a TopicChunk, choosing each column's encoding from its stats.
/// Rows come row by row (beginRow/set/finishRow) or in bulk (appendTimestamps/appendColumn/finishBulkAppend); never
/// mix the two within a row.
class TopicChunkBuilder {
 public:
  TopicChunkBuilder(TopicId topic_id, SchemaId schema_id, std::vector<ColumnDescriptor> columns, uint32_t max_rows);

  /// The same, with descriptors the caller keeps: every chunk this builder makes points at them instead of holding a
  /// copy of each column's descriptor, so a writer gives all the builders of a topic the same ones (DataWriter does).
  [[nodiscard]] static TopicChunkBuilder withSharedColumns(
      TopicId topic_id, SchemaId schema_id, std::vector<std::shared_ptr<const ColumnDescriptor>> columns,
      uint32_t max_rows) {
    return {topic_id, schema_id, max_rows, std::move(columns)};
  }

  void beginRow(Timestamp timestamp);

  /// T: float, double, int32_t, int64_t, uint64_t, bool, std::string_view.
  template <typename T>
  void set(std::size_t col_index, T value);

  void setNull(std::size_t col_index);

  /// Columns not set become null.
  void finishRow();

  // ---- Bulk column append ----
  // Order: appendTimestamps, appendColumn per column, appendColumnValidity for columns with nulls, then
  // finishBulkAppend, which computes the stats from the validity bitmaps.

  void appendTimestamps(Span<const Timestamp> timestamps);

  /// T: float, double, int32_t, int64_t, uint64_t, uint8_t (bool bytes).
  template <typename T>
  void appendColumn(std::size_t col_index, Span<const T> data);

  void appendColumnStrings(std::size_t col_index, Span<const uint32_t> offsets, Span<const char> data);

  /// Validity of this column's last appended batch.
  void appendColumnValidity(std::size_t col_index, BitSpan validity);

  void finishBulkAppend();

  /// Remaining row capacity before auto-seal.
  [[nodiscard]] uint32_t remainingCapacity() const noexcept;

  [[nodiscard]] bool isFull() const noexcept;

  [[nodiscard]] uint32_t rowCount() const noexcept;

  /// Between beginRow() and finishRow().
  [[nodiscard]] bool isRowInProgress() const noexcept;

  /// Writes the nulls the columns still owe first (see padColumn), so the null counts are whole.
  [[nodiscard]] const ChunkStats& stats();

  /// Last appended timestamp.
  [[nodiscard]] Timestamp lastTimestamp() const noexcept;

  /// Sorts the rows by timestamp (stable), picks the encodings and assigns the next ChunkId.
  [[nodiscard]] TopicChunk seal();

  // ---- Handing rows to a chunk that grows in place: TopicStorage only ----

  /// Writes the nulls the columns still owe and sorts the rows by timestamp (stable). timestampAt() and appendRowsTo()
  /// need it first.
  void sortRows();

  [[nodiscard]] Timestamp timestampAt(std::size_t row) const {
    return timestamps_[row];
  }

  /// Whether `chunk` has this builder's schema version, column count and column types. A chunk has one layout, and a
  /// reader tells a column the chunk lacks (it did not exist when the rows were written) from a null in it: rows of
  /// another layout are never put in a chunk.
  [[nodiscard]] bool hasLayoutOf(const TopicChunk& chunk) const;

  /// An empty growable chunk with this builder's layout and the next ChunkId.
  [[nodiscard]] TopicChunk makeGrowableChunk() const;

  /// A new growable chunk, with the next ChunkId, holding `chunk`'s rows and this builder's, sorted by timestamp:
  /// `chunk`'s rows come first among equal timestamps, as rows that arrived first do. For rows that arrive older than
  /// `chunk`'s newest: they cannot be appended, since that would leave the chunk unsorted, and a row that moves must
  /// not keep its chunk id. Needs sortRows() and hasLayoutOf(chunk), which must be growable.
  [[nodiscard]] TopicChunk mergedWith(const TopicChunk& chunk) const;

  /// Empties the builder for the next rows, keeping its layout and its buffers' capacity; setMaxRows() before the
  /// next row. Also after seal().
  void clearRows();

  [[nodiscard]] std::size_t columnCount() const noexcept {
    return columns_.size();
  }

  /// The rows the builder takes before isFull(); for a builder with no row yet.
  void setMaxRows(uint32_t max_rows) noexcept {
    max_rows_ = max_rows;
  }

  /// Appends rows [first, first + count) to the end of `chunk` and updates its stats (not encoded_byte_size). Needs
  /// sortRows(), hasLayoutOf(chunk), chunk.isGrowable() and no row older than the chunk's newest: a chunk stays sorted
  /// and its earlier rows keep their positions.
  void appendRowsTo(TopicChunk& chunk, std::size_t first, std::size_t count) const;

  /// An id above every chunk id handed out so far; seal() draws from the same counter.
  [[nodiscard]] static ChunkId nextChunkId() noexcept {
    return next_chunk_id++;
  }

 private:
  // Not an overload of the public constructor: a braced column list would fit both.
  TopicChunkBuilder(
      TopicId topic_id, SchemaId schema_id, uint32_t max_rows,
      std::vector<std::shared_ptr<const ColumnDescriptor>> columns);

  TopicId topic_id_;
  SchemaId schema_id_;
  uint32_t max_rows_;
  static inline std::atomic<ChunkId> next_chunk_id{1};  // monotonic counter

  std::vector<Timestamp> timestamps_;
  std::vector<TypedColumnBuffer> columns_;
  // Shared with the chunks made here (TopicChunk::Column::descriptor).
  std::vector<std::shared_ptr<const ColumnDescriptor>> shared_descriptors_;
  ChunkStats stats_;

  bool row_in_progress_ = false;
  Timestamp current_timestamp_ = 0;

  Timestamp last_timestamp_ = std::numeric_limits<Timestamp>::min();
  // False once a row arrived older than the one before it; lets seal() and sortRows() skip the sort.
  bool sorted_ = true;
  std::vector<double> last_column_values_;
  std::size_t bulk_pending_rows_ = 0;  // rows added via bulk but not yet finished

  void resetStats();
  // Bulk-appends the rows of a growable chunk, or of another builder with every column padded, of this layout.
  void appendRowsOf(const TopicChunk& chunk);
  void appendRowsOf(const TopicChunkBuilder& other);
  void updateColumnStats(std::size_t col_index, double value);
  // Float columns only: keeps ColumnStats::all_integral.
  void noteFloat(std::size_t col_index, double value);

  // A column a row does not set owes it a null, written when the column is next set or before the columns are read
  // (bulk append, stats(), seal()). Every path touching columns_ pads first, or its rows misalign.
  TypedColumnBuffer& padColumn(std::size_t col_index);
  void padAllColumns();

  // Stable, so equal timestamps keep arrival order; a no-op when already sorted.
  void sortRowsByTimestamp();

  // One pass over the column data, skipping nulls through the validity bitmap.
  void computeBulkNumericStats(std::size_t col_index, StorageKind kind, std::size_t first_row, std::size_t count);

  void computeBulkStringStats(std::size_t col_index, std::size_t first_row, std::size_t count);
};

}  // namespace PJ
