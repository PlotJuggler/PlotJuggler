// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/chunk.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <variant>

#include "pj_base/assert.hpp"

namespace PJ {

namespace {

// Whole-number detection and frame-of-reference for one non-constant float
// column. Sets `all_integral` (every non-null value passes
// encoding::isWholeInt64) and returns the FOR encoding when, in addition, no
// value is -0.0 (FOR would drop its sign) and the offsets are narrower than the
// float; nullopt otherwise. The detection pass does not allocate, so rejecting a
// fractional column costs one early-exit scan. A whole float is an exact
// integer, so decoding reference + offset reproduces it bit for bit.
template <typename Float>
std::optional<encoding::FrameOfReferenceEncoded> encodeWholeFloats(const TypedColumnBuffer& col, bool& all_integral) {
  const uint8_t* data = col.valueBuffer().data();
  const auto value_at = [data](std::size_t row) {
    Float v{};
    std::memcpy(&v, data + row * sizeof(Float), sizeof(Float));
    return v;
  };
  all_integral = false;
  int64_t min = std::numeric_limits<int64_t>::max();
  int64_t max = std::numeric_limits<int64_t>::min();
  bool negative_zero = false;
  for (std::size_t r = 0; r < col.rowCount(); ++r) {
    if (!col.isValid(r)) {
      continue;  // a null slot's payload is not a value
    }
    const Float v = value_at(r);
    if (!encoding::isWholeInt64(v)) {
      return std::nullopt;
    }
    negative_zero = negative_zero || (v == 0 && std::signbit(v));
    min = std::min(min, static_cast<int64_t>(v));
    max = std::max(max, static_cast<int64_t>(v));
  }
  all_integral = true;
  if (negative_zero || min > max ||
      encoding::offsetBytesFor(static_cast<uint64_t>(max) - static_cast<uint64_t>(min)) >= sizeof(Float)) {
    return std::nullopt;
  }
  // Null slots encode as the reference (offset 0); the validity bitmap keeps them null.
  std::vector<int64_t> values(col.rowCount(), min);
  for (std::size_t r = 0; r < values.size(); ++r) {
    if (col.isValid(r)) {
      values[r] = static_cast<int64_t>(value_at(r));
    }
  }
  return encoding::forEncode(
      Span<const uint8_t>(reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(int64_t)),
      StorageKind::kInt64, values.size(), min, max);
}

// The bytes a constant-encoded column repeats: the FIRST NON-NULL row's value.
// Row 0 can be a null slot (bulk appends, or a null sorted to the front at
// seal), whose payload is not a value. An all-null column returns row 0. The
// non-null values must also be bit-identical: ColumnStats::is_constant compares
// doubles, so it accepts +0.0 next to -0.0 and distinct uint64 values above
// 2^53 that round to the same double — nullopt then, and the column is not
// constant-encoded.
std::optional<Span<const uint8_t>> constantValueBytes(const TypedColumnBuffer& col, StorageKind kind) {
  const std::size_t esize = storageKindSize(kind);
  const uint8_t* data = col.valueBuffer().data();
  std::size_t first = 0;
  while (first < col.rowCount() && !col.isValid(first)) {
    ++first;
  }
  if (first == col.rowCount()) {
    return Span<const uint8_t>(data, esize);
  }
  const uint8_t* value = data + first * esize;
  for (std::size_t r = first + 1; r < col.rowCount(); ++r) {
    if (col.isValid(r) && std::memcmp(data + r * esize, value, esize) != 0) {
      return std::nullopt;
    }
  }
  return Span<const uint8_t>(value, esize);
}

// Dispatch a callable with the correct numeric type tag.
// Returns true if kind is numeric (kFloat32..kUint64), false for kBool/kString.
template <typename F>
bool dispatchNumericKind(StorageKind kind, F&& fn) {
  switch (kind) {
    case StorageKind::kFloat32:
      fn(static_cast<const float*>(nullptr));
      return true;
    case StorageKind::kFloat64:
      fn(static_cast<const double*>(nullptr));
      return true;
    case StorageKind::kInt32:
      fn(static_cast<const int32_t*>(nullptr));
      return true;
    case StorageKind::kInt64:
      fn(static_cast<const int64_t*>(nullptr));
      return true;
    case StorageKind::kUint64:
      fn(static_cast<const uint64_t*>(nullptr));
      return true;
    default:
      return false;
  }
}

// Read a raw numeric value from a buffer, dispatching on StorageKind.
template <typename R>
[[nodiscard]] R readRawAs(const RawBuffer& buf, StorageKind kind, std::size_t row) {
  const std::size_t elem_size = storageKindSize(kind);
  const uint8_t* ptr = buf.data() + row * elem_size;

  R result{};
  dispatchNumericKind(kind, [&]<typename T>(const T* /*tag*/) {
    T v{};
    std::memcpy(&v, ptr, sizeof(v));
    result = static_cast<R>(v);
  });
  return result;
}

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

// ===========================================================================
// TopicChunkBuilder
// ===========================================================================

TopicChunkBuilder::TopicChunkBuilder(
    TopicId topic_id, SchemaId schema_id, std::vector<ColumnDescriptor> columns, uint32_t max_rows)
    : topic_id_(topic_id), schema_id_(schema_id), max_rows_(max_rows), column_descriptors_(std::move(columns)) {
  columns_.reserve(column_descriptors_.size());
  for (const auto& desc : column_descriptors_) {
    columns_.emplace_back(desc);
  }
  stats_.column_stats.resize(column_descriptors_.size());
  last_column_values_.resize(column_descriptors_.size(), 0.0);
}

void TopicChunkBuilder::beginRow(Timestamp timestamp) {
  PJ_ASSERT(!row_in_progress_, "begin_row called while row already in progress");
  // Out-of-order timestamps are accepted: multi-publisher topics interleave
  // regressing stamps. Rows are stable-sorted at seal().
  row_in_progress_ = true;
  current_timestamp_ = timestamp;
  last_timestamp_ = timestamp;
}

// ---------------------------------------------------------------------------
// set<T> — templatized value setters
// ---------------------------------------------------------------------------

template <>
void TopicChunkBuilder::set<float>(std::size_t col_index, float value) {
  PJ_ASSERT(row_in_progress_, "set<float> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendFloat32(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<double>(std::size_t col_index, double value) {
  PJ_ASSERT(row_in_progress_, "set<double> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendFloat64(value);
  updateColumnStats(col_index, value);
}

template <>
void TopicChunkBuilder::set<int32_t>(std::size_t col_index, int32_t value) {
  PJ_ASSERT(row_in_progress_, "set<int32_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendInt32(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<int64_t>(std::size_t col_index, int64_t value) {
  PJ_ASSERT(row_in_progress_, "set<int64_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendInt64(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<uint64_t>(std::size_t col_index, uint64_t value) {
  PJ_ASSERT(row_in_progress_, "set<uint64_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendUint64(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<bool>(std::size_t col_index, bool value) {
  PJ_ASSERT(row_in_progress_, "set<bool> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendBool(value);
  updateColumnStats(col_index, value ? 1.0 : 0.0);
}

template <>
void TopicChunkBuilder::set<std::string_view>(std::size_t col_index, std::string_view value) {
  PJ_ASSERT(row_in_progress_, "set<string_view> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendString(value);
  auto& cs = stats_.column_stats[col_index];
  const std::size_t current_row = columns_[col_index].rowCount() - 1;
  if (current_row == 0) {
    cs.run_count = 1;
  } else {
    std::string_view prev = columns_[col_index].readString(current_row - 1);
    if (value != prev) {
      cs.is_constant = false;
      cs.run_count++;
    }
  }
}

void TopicChunkBuilder::setNull(std::size_t col_index) {
  PJ_ASSERT(row_in_progress_, "set_null called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendNull();
  stats_.column_stats[col_index].null_count++;
}

void TopicChunkBuilder::finishRow() {
  PJ_ASSERT(row_in_progress_, "finish_row called without begin_row");
  const std::size_t expected = rowCount() + 1;
  for (std::size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].rowCount() < expected) {
      columns_[i].appendNull();
      stats_.column_stats[i].null_count++;
    }
  }

  timestamps_.push_back(current_timestamp_);
  stats_.t_min = std::min(stats_.t_min, current_timestamp_);
  stats_.t_max = std::max(stats_.t_max, current_timestamp_);
  stats_.row_count++;
  row_in_progress_ = false;
}

// ---------------------------------------------------------------------------
// Bulk column append
// ---------------------------------------------------------------------------

void TopicChunkBuilder::appendTimestamps(Span<const Timestamp> timestamps) {
  PJ_ASSERT(!row_in_progress_, "append_timestamps called while row in progress");
  const std::size_t count = timestamps.size();
  if (count == 0) {
    return;
  }

  timestamps_.reserve(timestamps_.size() + count);
  timestamps_.insert(timestamps_.end(), timestamps.begin(), timestamps.end());

  // The batch may be unsorted (out-of-order ingest): take true extrema, not
  // the endpoints. Rows are stable-sorted at seal().
  const auto [min_it, max_it] = std::minmax_element(timestamps.begin(), timestamps.end());
  stats_.t_min = std::min(stats_.t_min, *min_it);
  stats_.t_max = std::max(stats_.t_max, *max_it);
  last_timestamp_ = timestamps[count - 1];

  bulk_pending_rows_ = count;
}

template <>
void TopicChunkBuilder::appendColumn<float>(std::size_t col_index, Span<const float> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendFloat32Bulk(data);
}

template <>
void TopicChunkBuilder::appendColumn<double>(std::size_t col_index, Span<const double> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendFloat64Bulk(data);
}

template <>
void TopicChunkBuilder::appendColumn<int32_t>(std::size_t col_index, Span<const int32_t> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendInt32Bulk(data);
}

template <>
void TopicChunkBuilder::appendColumn<int64_t>(std::size_t col_index, Span<const int64_t> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendInt64Bulk(data);
}

template <>
void TopicChunkBuilder::appendColumn<uint64_t>(std::size_t col_index, Span<const uint64_t> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendUint64Bulk(data);
}

template <>
void TopicChunkBuilder::appendColumn<uint8_t>(std::size_t col_index, Span<const uint8_t> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendBoolBulk(data);
}

void TopicChunkBuilder::appendColumnStrings(
    std::size_t col_index, Span<const uint32_t> offsets, Span<const char> data) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendStringsBulk(offsets, data);
}

void TopicChunkBuilder::appendColumnValidity(std::size_t col_index, BitSpan validity) {
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  columns_[col_index].appendValidityBulk(validity);
}

void TopicChunkBuilder::finishBulkAppend() {
  PJ_ASSERT(!row_in_progress_, "finish_bulk_append called while row in progress");
  if (bulk_pending_rows_ == 0) {
    return;
  }

  const std::size_t count = bulk_pending_rows_;
  const std::size_t expected_rows = static_cast<std::size_t>(stats_.row_count) + count;

  for (std::size_t col = 0; col < columns_.size(); ++col) {
    // A batch may omit columns (an Arrow stream carrying a subset of the topic's
    // fields): pad them with nulls, as finishRow() does for unset columns. The
    // stats pass below counts the padded slots as nulls.
    while (columns_[col].rowCount() < expected_rows) {
      columns_[col].appendNull();
    }
    PJ_ASSERT(
        columns_[col].rowCount() == expected_rows,
        "finishBulkAppend: column has more rows than the batch — appendColumn*() called twice for one column?");
    const std::size_t first_row = expected_rows - count;
    const auto kind = storageKindOf(column_descriptors_[col].logical_type);

    if (kind == StorageKind::kString) {
      computeBulkStringStats(col, first_row, count);
    } else {
      computeBulkNumericStats(col, kind, first_row, count);
    }
  }

  stats_.row_count += static_cast<uint32_t>(count);
  bulk_pending_rows_ = 0;
}

uint32_t TopicChunkBuilder::remainingCapacity() const noexcept {
  return max_rows_ - rowCount();
}

// ---------------------------------------------------------------------------
// Bulk stats helpers
// ---------------------------------------------------------------------------

void TopicChunkBuilder::computeBulkNumericStats(
    std::size_t col_index, StorageKind kind, std::size_t first_row, std::size_t count) {
  if (count == 0) {
    return;
  }

  auto& cs = stats_.column_stats[col_index];
  const auto& col = columns_[col_index];
  const bool has_validity = col.hasNulls();

  auto process = [&]<typename T>(const T* /*tag*/) {
    const auto* buf = reinterpret_cast<const T*>(col.valueBuffer().data());
    double local_min = cs.min_value.value_or(std::numeric_limits<double>::max());
    double local_max = cs.max_value.value_or(std::numeric_limits<double>::lowest());
    double prev = last_column_values_[col_index];
    bool had_valid = cs.run_count > 0;

    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t row = first_row + i;
      if (has_validity && !col.isValid(row)) {
        cs.null_count++;
        continue;
      }
      const double v = static_cast<double>(buf[row]);
      if (v < local_min) {
        local_min = v;
      }
      if (v > local_max) {
        local_max = v;
      }
      if (!had_valid) {
        cs.run_count = 1;
        had_valid = true;
      } else if (v != prev) {
        cs.is_constant = false;
        cs.run_count++;
      }
      prev = v;
    }
    if (had_valid) {
      cs.min_value = local_min;
      cs.max_value = local_max;
    }
    last_column_values_[col_index] = prev;
  };

  if (!dispatchNumericKind(kind, process)) {
    if (kind == StorageKind::kBool) {
      const auto* buf = col.valueBuffer().data();
      double prev = last_column_values_[col_index];
      double local_min = cs.min_value.value_or(std::numeric_limits<double>::max());
      double local_max = cs.max_value.value_or(std::numeric_limits<double>::lowest());
      bool had_valid = cs.run_count > 0;
      for (std::size_t i = 0; i < count; ++i) {
        const std::size_t row = first_row + i;
        if (has_validity && !col.isValid(row)) {
          cs.null_count++;
          continue;
        }
        const double v = buf[row] ? 1.0 : 0.0;
        if (v < local_min) {
          local_min = v;
        }
        if (v > local_max) {
          local_max = v;
        }
        if (!had_valid) {
          cs.run_count = 1;
          had_valid = true;
        } else if (v != prev) {
          cs.is_constant = false;
          cs.run_count++;
        }
        prev = v;
      }
      if (had_valid) {
        cs.min_value = local_min;
        cs.max_value = local_max;
      }
      last_column_values_[col_index] = prev;
    }
  }
}

void TopicChunkBuilder::computeBulkStringStats(std::size_t col_index, std::size_t first_row, std::size_t count) {
  if (count == 0) {
    return;
  }

  auto& cs = stats_.column_stats[col_index];
  const auto& col = columns_[col_index];
  const bool has_validity = col.hasNulls();

  std::optional<std::string_view> last_valid;

  if (first_row > 0 && cs.run_count > 0) {
    for (std::size_t j = first_row; j > 0; --j) {
      if (!has_validity || col.isValid(j - 1)) {
        last_valid = col.readString(j - 1);
        break;
      }
    }
  }

  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t row = first_row + i;
    if (has_validity && !col.isValid(row)) {
      cs.null_count++;
      continue;
    }

    std::string_view current = col.readString(row);
    if (!last_valid.has_value()) {
      cs.run_count = 1;
    } else if (current != *last_valid) {
      cs.is_constant = false;
      cs.run_count++;
    }
    last_valid = current;
  }
}

bool TopicChunkBuilder::isFull() const noexcept {
  return rowCount() >= max_rows_;
}

uint32_t TopicChunkBuilder::rowCount() const noexcept {
  return stats_.row_count;
}

bool TopicChunkBuilder::isRowInProgress() const noexcept {
  return row_in_progress_;
}

const ChunkStats& TopicChunkBuilder::stats() const noexcept {
  return stats_;
}

Timestamp TopicChunkBuilder::lastTimestamp() const noexcept {
  return last_timestamp_;
}

void TopicChunkBuilder::updateColumnStats(std::size_t col_index, double value) {
  auto& cs = stats_.column_stats[col_index];

  if (!cs.min_value.has_value() || value < *cs.min_value) {
    cs.min_value = value;
  }
  if (!cs.max_value.has_value() || value > *cs.max_value) {
    cs.max_value = value;
  }

  // Constancy starts at the first NON-NULL value (as in the bulk path): a
  // leading null must not make a constant column look varying.
  if (cs.run_count == 0) {
    cs.run_count = 1;
  } else if (value != last_column_values_[col_index]) {
    cs.is_constant = false;
    cs.run_count++;
  }
  last_column_values_[col_index] = value;
}

// ---------------------------------------------------------------------------
// seal
// ---------------------------------------------------------------------------

// Rows arrive in commit order, which under multi-publisher recordings is not
// timestamp order. Seal sorted chunks so per-chunk binary searches and cursor
// merging stay valid; stable, so duplicate timestamps keep arrival order.
// Column stats are order-invariant (min/max/null_count) and encodings are
// derived from the buffers during seal, so only the buffers are permuted.
void TopicChunkBuilder::sortRowsByTimestamp() {
  PJ_ASSERT(!row_in_progress_, "seal called while row in progress");
  if (std::is_sorted(timestamps_.begin(), timestamps_.end())) {
    return;
  }
  std::vector<std::size_t> perm(timestamps_.size());
  std::iota(perm.begin(), perm.end(), 0);
  std::stable_sort(
      perm.begin(), perm.end(), [this](std::size_t a, std::size_t b) { return timestamps_[a] < timestamps_[b]; });

  std::vector<Timestamp> sorted_ts;
  sorted_ts.reserve(timestamps_.size());
  for (const std::size_t row : perm) {
    sorted_ts.push_back(timestamps_[row]);
  }
  timestamps_ = std::move(sorted_ts);

  for (std::size_t i = 0; i < columns_.size(); ++i) {
    TypedColumnBuffer& col = columns_[i];
    PJ_ASSERT(col.rowCount() == perm.size(), "column/timestamp row count mismatch at seal");
    const StorageKind kind = storageKindOf(column_descriptors_[i].logical_type);
    TypedColumnBuffer sorted(column_descriptors_[i]);
    for (const std::size_t row : perm) {
      if (!col.isValid(row)) {
        sorted.appendNull();
        continue;
      }
      switch (kind) {
        case StorageKind::kFloat32:
          sorted.appendFloat32(col.readFloat32(row));
          break;
        case StorageKind::kFloat64:
          sorted.appendFloat64(col.readFloat64(row));
          break;
        case StorageKind::kInt32:
          sorted.appendInt32(col.readInt32(row));
          break;
        case StorageKind::kInt64:
          sorted.appendInt64(col.readInt64(row));
          break;
        case StorageKind::kUint64:
          sorted.appendUint64(col.readUint64(row));
          break;
        case StorageKind::kBool:
          sorted.appendBool(col.readBool(row));
          break;
        case StorageKind::kString:
          sorted.appendString(col.readString(row));
          break;
      }
    }
    col = std::move(sorted);
  }
}

TopicChunk TopicChunkBuilder::seal() {
  sortRowsByTimestamp();
  TopicChunk chunk;
  chunk.id = next_chunk_id++;
  chunk.topic_id = topic_id_;
  chunk.schema_version = schema_id_;
  chunk.stats = stats_;

  chunk.timestamps = std::move(timestamps_);

  const std::size_t num_cols = columns_.size();
  chunk.columns.resize(num_cols);

  for (std::size_t i = 0; i < num_cols; ++i) {
    const auto& col = columns_[i];
    const StorageKind kind = storageKindOf(column_descriptors_[i].logical_type);
    const auto& cs = stats_.column_stats[i];

    chunk.columns[i].descriptor = std::make_shared<const ColumnDescriptor>(column_descriptors_[i]);

    switch (kind) {
      case StorageKind::kString: {
        chunk.columns[i].data = encoding::dictionaryEncodeStrings(
            Span<const uint8_t>(col.offsetsBuffer().data(), col.offsetsBuffer().size()),
            Span<const uint8_t>(col.valueBuffer().data(), col.valueBuffer().size()), col.rowCount());
        break;
      }
      case StorageKind::kBool: {
        if (const auto value = cs.is_constant && col.rowCount() > 0 ? constantValueBytes(col, kind) : std::nullopt;
            value.has_value()) {
          chunk.columns[i].data = encoding::constantEncode(*value, kind, col.rowCount());
        } else {
          chunk.columns[i].data = encoding::packBools(Span<const uint8_t>(col.valueBuffer().data(), col.rowCount()));
        }
        break;
      }
      case StorageKind::kInt32:
      case StorageKind::kInt64: {
        // Compute exact integer min/max from the raw column buffer to avoid
        // precision loss from the double-based stats (BUG-1/2).
        const std::size_t row_count = col.rowCount();
        const uint8_t* buf_data = col.valueBuffer().data();
        const std::size_t esize = storageKindSize(kind);

        int64_t exact_min = std::numeric_limits<int64_t>::max();
        int64_t exact_max = std::numeric_limits<int64_t>::min();
        bool exact_is_constant = true;
        int64_t first_val{};

        for (std::size_t r = 0; r < row_count; ++r) {
          int64_t v{};
          if (kind == StorageKind::kInt32) {
            int32_t tmp{};
            std::memcpy(&tmp, buf_data + r * esize, sizeof(tmp));
            v = tmp;
          } else {
            std::memcpy(&v, buf_data + r * esize, sizeof(v));
          }
          if (r == 0) {
            first_val = v;
          } else if (v != first_val) {
            exact_is_constant = false;
          }
          exact_min = std::min(exact_min, v);
          exact_max = std::max(exact_max, v);
        }

        if (exact_is_constant && row_count > 0) {
          chunk.columns[i].data =
              encoding::constantEncode(Span<const uint8_t>(buf_data, col.valueBuffer().size()), kind, row_count);
        } else if (row_count > 0) {
          // Unsigned subtraction: exact_max - exact_min overflows int64 for spans
          // like [INT64_MIN, 0].
          const uint64_t range = static_cast<uint64_t>(exact_max) - static_cast<uint64_t>(exact_min);
          const uint8_t ob = encoding::offsetBytesFor(range);

          if (ob < storageKindSize(kind)) {
            chunk.columns[i].data = encoding::forEncode(
                Span<const uint8_t>(buf_data, col.valueBuffer().size()), kind, row_count, exact_min, exact_max);
          } else {
            RawBuffer raw;
            raw.append(buf_data, col.valueBuffer().size());
            chunk.columns[i].data = std::move(raw);
          }
        } else {
          RawBuffer raw;
          raw.append(buf_data, col.valueBuffer().size());
          chunk.columns[i].data = std::move(raw);
        }
        break;
      }
      default: {
        const std::size_t row_count = col.rowCount();
        const uint8_t* buf_data = col.valueBuffer().data();
        const bool is_float = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;
        if (cs.is_constant && row_count > 0) {
          if (const auto value = constantValueBytes(col, kind); value.has_value()) {
            if (is_float) {
              // The constant is the recorded min (absent when every row is null).
              chunk.stats.column_stats[i].all_integral = !cs.min_value || encoding::isWholeInt64(*cs.min_value);
            }
            chunk.columns[i].data = encoding::constantEncode(*value, kind, row_count);
            break;
          }
        }
        // Float chunks holding only whole numbers (e.g. JSON integers parsed as
        // double) are flagged all_integral and, when that narrows them, stored
        // frame-of-reference like an integer column. The declared type is
        // unchanged and reads decode back to the same floats.
        if (is_float) {
          bool all_integral = false;
          auto encoded = kind == StorageKind::kFloat64 ? encodeWholeFloats<double>(col, all_integral)
                                                       : encodeWholeFloats<float>(col, all_integral);
          chunk.stats.column_stats[i].all_integral = all_integral;
          if (encoded.has_value()) {
            chunk.columns[i].data = std::move(*encoded);
            break;
          }
        }
        RawBuffer raw;
        raw.append(buf_data, col.valueBuffer().size());
        chunk.columns[i].data = std::move(raw);
        break;
      }
    }

    if (col.hasNulls()) {
      BitVector bv;
      bv.assignBytes(
          Span<const uint8_t>(col.validityBuffer().data(), col.validityBuffer().sizeBytes()),
          col.validityBuffer().sizeBits());
      chunk.columns[i].validity_bitmap = std::move(bv);
    }
  }

  return chunk;
}

// ===========================================================================
// TopicChunk decode helpers
// ===========================================================================

EncodingType TopicChunk::columnEncoding(std::size_t index) const {
  return std::visit(
      overloaded{
          [](const RawBuffer&) { return EncodingType::kRaw; },
          [](const encoding::ConstantEncoded&) { return EncodingType::kConstant; },
          [](const encoding::FrameOfReferenceEncoded&) { return EncodingType::kFrameOfReference; },
          [](const encoding::DictionaryEncoded&) { return EncodingType::kDictionary; },
          [](const encoding::PackedBools&) { return EncodingType::kPackedBool; },
      },
      columns[index].data);
}

Timestamp TopicChunk::readTimestamp(std::size_t row) const {
  return timestamps[row];
}

void TopicChunk::readTimestamps(Span<Timestamp> out, std::size_t row_start) const {
  std::memcpy(out.data(), timestamps.data() + row_start, out.size() * sizeof(Timestamp));
}

double TopicChunk::readNumericAsDouble(std::size_t col_index, std::size_t row) const {
  const auto& col = columns[col_index];
  return std::visit(
      overloaded{
          [&](const RawBuffer& buf) {
            return readRawAs<double>(buf, storageKindOf(col.descriptor->logical_type), row);
          },
          [](const encoding::ConstantEncoded& enc) { return encoding::constantDecodeAsDouble(enc); },
          [row](const encoding::FrameOfReferenceEncoded& enc) { return encoding::forDecodeOneAsDouble(enc, row); },
          [](const auto&) { return 0.0; },
      },
      col.data);
}

int64_t TopicChunk::readNumericAsInt64(std::size_t col_index, std::size_t row) const {
  const auto& col = columns[col_index];
  return std::visit(
      overloaded{
          [&](const RawBuffer& buf) {
            return readRawAs<int64_t>(buf, storageKindOf(col.descriptor->logical_type), row);
          },
          [](const encoding::ConstantEncoded& enc) { return encoding::constantDecodeAsInt64(enc); },
          [row](const encoding::FrameOfReferenceEncoded& enc) { return encoding::forDecodeOneAsInt64(enc, row); },
          [](const auto&) { return static_cast<int64_t>(0); },
      },
      col.data);
}

uint64_t TopicChunk::readNumericAsUint64(std::size_t col_index, std::size_t row) const {
  const auto& col = columns[col_index];
  return std::visit(
      overloaded{
          [&](const RawBuffer& buf) {
            return readRawAs<uint64_t>(buf, storageKindOf(col.descriptor->logical_type), row);
          },
          [](const encoding::ConstantEncoded& enc) { return encoding::constantDecodeAsUint64(enc); },
          [row](const encoding::FrameOfReferenceEncoded& enc) {
            return static_cast<uint64_t>(encoding::forDecodeOneAsInt64(enc, row));
          },
          [](const auto&) { return static_cast<uint64_t>(0); },
      },
      col.data);
}

std::string_view TopicChunk::readString(std::size_t col_index, std::size_t row) const {
  return encoding::dictionaryLookup(std::get<encoding::DictionaryEncoded>(columns[col_index].data), row);
}

bool TopicChunk::readBool(std::size_t col_index, std::size_t row) const {
  return std::visit(
      overloaded{
          [](const encoding::ConstantEncoded& enc) {
            uint8_t v = 0;
            std::memcpy(&v, enc.value_bytes.data(), sizeof(v));
            return v != 0;
          },
          [row](const encoding::PackedBools& enc) { return encoding::unpackBool(enc, row); },
          [](const auto&) { return false; },
      },
      columns[col_index].data);
}

bool TopicChunk::isNull(std::size_t col_index, std::size_t row) const {
  const auto& bm = columns[col_index].validity_bitmap;
  if (!bm.has_value() || bm->empty()) {
    return false;
  }
  return !bm->isValid(row);
}

void TopicChunk::readColumnAsDoubles(std::size_t col_index, Span<double> out, std::size_t row_start) const {
  const std::size_t count = out.size();
  const auto& col = columns[col_index];
  std::visit(
      overloaded{
          [&](const encoding::ConstantEncoded& enc) {
            std::fill(out.begin(), out.end(), encoding::constantDecodeAsDouble(enc));
          },
          [&](const encoding::FrameOfReferenceEncoded& enc) { encoding::forDecodeRangeAsDoubles(enc, out, row_start); },
          [&](const RawBuffer& buf) {
            const StorageKind kind = storageKindOf(col.descriptor->logical_type);
            const uint8_t* base = buf.data();
            const std::size_t esize = storageKindSize(kind);
            auto convert = [&]<typename T>(const T* /*tag*/) {
              const uint8_t* src = base + row_start * esize;
              for (std::size_t i = 0; i < count; ++i) {
                T v{};
                std::memcpy(&v, src + i * esize, sizeof(v));
                out[i] = static_cast<double>(v);
              }
            };
            if (!dispatchNumericKind(kind, convert)) {
              std::fill(out.begin(), out.end(), std::numeric_limits<double>::quiet_NaN());
            }
          },
          [&](const auto&) { std::fill(out.begin(), out.end(), std::numeric_limits<double>::quiet_NaN()); },
      },
      col.data);

  // Replace values at null positions with NaN so consumers don't confuse
  // null (no data) with actual zero values.
  if (col.validity_bitmap.has_value()) {
    const auto& bm = *col.validity_bitmap;
    for (std::size_t i = 0; i < count; ++i) {
      if (!bm.isValid(row_start + i)) {
        out[i] = std::numeric_limits<double>::quiet_NaN();
      }
    }
  }
}

}  // namespace PJ
