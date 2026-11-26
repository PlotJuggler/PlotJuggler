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
#include <type_traits>
#include <utility>
#include <variant>

#include "pj_base/assert.hpp"

namespace PJ {

namespace {

// A column's fixed-width values and validity, wherever they are stored: a builder's buffers or a growable chunk's.
// The validity bit is read here, not through BitVector::isValid(): these loops run once per cell and that call is not
// inlined across translation units.
[[nodiscard]] inline bool bitIsSet(const uint8_t* bits, std::size_t index) {
  return ((bits[index / 8] >> (index % 8)) & 1U) != 0;
}

struct FixedColumnView {
  const uint8_t* values = nullptr;
  std::size_t rows = 0;
  std::size_t byte_size = 0;
  const uint8_t* validity = nullptr;  // Arrow bitmap; null: every row is valid

  [[nodiscard]] std::size_t rowCount() const {
    return rows;
  }
  [[nodiscard]] bool isValid(std::size_t row) const {
    return validity == nullptr || bitIsSet(validity, row);
  }
};

[[nodiscard]] FixedColumnView viewOf(const TypedColumnBuffer& col) {
  return {
      col.valueBuffer().data(), col.rowCount(), col.valueBuffer().size(),
      col.hasNulls() ? col.validityBuffer().data() : nullptr};
}

// For a non-constant float column: sets `all_integral` when every non-null value passes encoding::isWholeInt64, and
// returns a FOR encoding when, besides, no value is -0.0 (FOR drops its sign) and the offsets are narrower than the
// float. A whole float is an exact integer, so decoding reproduces it bit for bit.
template <typename Float>
std::optional<encoding::FrameOfReferenceEncoded> encodeWholeFloats(const FixedColumnView& col, bool& all_integral) {
  const uint8_t* data = col.values;
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

// The first non-null row's bytes (row 0 can be a null slot); row 0's for an all-null column. nullopt unless the
// non-null values are bit-identical: ColumnStats::is_constant compares doubles, so it accepts +0.0 beside -0.0, and
// distinct uint64 values above 2^53 that round to the same double.
std::optional<Span<const uint8_t>> constantValueBytes(const FixedColumnView& col, StorageKind kind) {
  const std::size_t esize = storageKindSize(kind);
  const uint8_t* data = col.values;
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

// The encoding of a column of numbers, chosen from its values and `cs`; nullopt when it stays raw. Shared by seal()
// and TopicChunk::compact(), so a chunk that grew is stored as one sealed with the same rows. `all_integral` is set
// for float columns only.
std::optional<encoding::EncodedData> encodeNumbers(
    const FixedColumnView& col, StorageKind kind, const ColumnStats& cs, bool& all_integral) {
  const std::size_t row_count = col.rowCount();
  if (row_count == 0) {
    return std::nullopt;
  }
  const uint8_t* buf_data = col.values;
  if (kind == StorageKind::kInt32 || kind == StorageKind::kInt64) {
    // Exact integer min/max from the raw buffer: the double-based stats lose precision above 2^53.
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
    if (exact_is_constant) {
      return encoding::constantEncode(Span<const uint8_t>(buf_data, col.byte_size), kind, row_count);
    }
    // Unsigned: exact_max - exact_min overflows int64 for spans like [INT64_MIN, 0].
    const uint64_t range = static_cast<uint64_t>(exact_max) - static_cast<uint64_t>(exact_min);
    if (encoding::offsetBytesFor(range) < storageKindSize(kind)) {
      return encoding::forEncode(Span<const uint8_t>(buf_data, col.byte_size), kind, row_count, exact_min, exact_max);
    }
    return std::nullopt;
  }

  const bool is_float = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;
  if (cs.is_constant) {
    if (const auto value = constantValueBytes(col, kind); value.has_value()) {
      if (is_float) {
        // Of the one value, or of none when every row is null.
        double constant = 0.0;
        if (kind == StorageKind::kFloat32) {
          float narrow = 0;
          std::memcpy(&narrow, value->data(), sizeof(narrow));
          constant = narrow;
        } else {
          std::memcpy(&constant, value->data(), sizeof(constant));
        }
        all_integral = cs.null_count >= row_count || encoding::isWholeInt64(constant);
      }
      return encoding::constantEncode(*value, kind, row_count);
    }
  }
  // Whole-number floats (JSON integers parsed as double) are flagged all_integral and stored frame-of-reference when
  // that is narrower; the declared type stays and reads decode the same floats.
  if (is_float) {
    auto encoded = kind == StorageKind::kFloat64 ? encodeWholeFloats<double>(col, all_integral)
                                                 : encodeWholeFloats<float>(col, all_integral);
    if (encoded.has_value()) {
      return std::move(*encoded);
    }
  }
  return std::nullopt;
}

// The same for a column of bools, from one byte a row; nullopt when it is stored as packed bits.
std::optional<encoding::EncodedData> encodeConstantBools(const FixedColumnView& col, const ColumnStats& cs) {
  if (cs.is_constant && col.rowCount() > 0) {
    if (const auto value = constantValueBytes(col, StorageKind::kBool); value.has_value()) {
      return encoding::constantEncode(*value, StorageKind::kBool, col.rowCount());
    }
  }
  return std::nullopt;
}

}  // namespace

TopicChunkBuilder::TopicChunkBuilder(
    TopicId topic_id, SchemaId schema_id, std::vector<ColumnDescriptor> columns, uint32_t max_rows)
    : topic_id_(topic_id), schema_id_(schema_id), max_rows_(max_rows), column_descriptors_(std::move(columns)) {
  columns_.reserve(column_descriptors_.size());
  for (const auto& desc : column_descriptors_) {
    columns_.emplace_back(desc);
  }
  resetStats();
}

void TopicChunkBuilder::resetStats() {
  stats_ = ChunkStats{};
  stats_.column_stats.resize(column_descriptors_.size());
  last_column_values_.assign(column_descriptors_.size(), 0.0);
  // Of no value yet. Kept while rows arrive, so appendRowsTo() takes it without reading the values; seal() computes it
  // again while it picks the encoding.
  for (std::size_t i = 0; i < column_descriptors_.size(); ++i) {
    const StorageKind kind = storageKindOf(column_descriptors_[i].logical_type);
    stats_.column_stats[i].all_integral = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;
  }
}

void TopicChunkBuilder::clearRows(uint32_t max_rows) {
  PJ_ASSERT(!row_in_progress_, "clearRows called while row in progress");
  timestamps_.clear();
  for (TypedColumnBuffer& column : columns_) {
    column.clear();
  }
  resetStats();
  max_rows_ = max_rows;
  last_timestamp_ = std::numeric_limits<Timestamp>::min();
  sorted_ = true;
  bulk_pending_rows_ = 0;
}

void TopicChunkBuilder::beginRow(Timestamp timestamp) {
  PJ_ASSERT(!row_in_progress_, "begin_row called while row already in progress");
  // Out-of-order stamps (multi-publisher topics) are accepted; seal() stable-sorts the rows.
  if (!timestamps_.empty() && timestamp < timestamps_.back()) {
    sorted_ = false;
  }
  row_in_progress_ = true;
  current_timestamp_ = timestamp;
  last_timestamp_ = timestamp;
}

template <>
void TopicChunkBuilder::set<float>(std::size_t col_index, float value) {
  PJ_ASSERT(row_in_progress_, "set<float> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendFloat32(value);
  updateColumnStats(col_index, static_cast<double>(value));
  noteFloat(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<double>(std::size_t col_index, double value) {
  PJ_ASSERT(row_in_progress_, "set<double> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendFloat64(value);
  updateColumnStats(col_index, value);
  noteFloat(col_index, value);
}

template <>
void TopicChunkBuilder::set<int32_t>(std::size_t col_index, int32_t value) {
  PJ_ASSERT(row_in_progress_, "set<int32_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendInt32(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<int64_t>(std::size_t col_index, int64_t value) {
  PJ_ASSERT(row_in_progress_, "set<int64_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendInt64(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<uint64_t>(std::size_t col_index, uint64_t value) {
  PJ_ASSERT(row_in_progress_, "set<uint64_t> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendUint64(value);
  updateColumnStats(col_index, static_cast<double>(value));
}

template <>
void TopicChunkBuilder::set<bool>(std::size_t col_index, bool value) {
  PJ_ASSERT(row_in_progress_, "set<bool> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendBool(value);
  updateColumnStats(col_index, value ? 1.0 : 0.0);
}

template <>
void TopicChunkBuilder::set<std::string_view>(std::size_t col_index, std::string_view value) {
  PJ_ASSERT(row_in_progress_, "set<string_view> called without begin_row");
  PJ_ASSERT(col_index < columns_.size(), "col_index out of bounds");
  padColumn(col_index).appendString(value);
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
  padColumn(col_index).appendNull();
  stats_.column_stats[col_index].null_count++;
}

void TopicChunkBuilder::finishRow() {
  PJ_ASSERT(row_in_progress_, "finish_row called without begin_row");
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
  // The batch's columns append after the rows written so far.
  padAllColumns();
  if ((!timestamps_.empty() && timestamps[0] < timestamps_.back()) ||
      !std::is_sorted(timestamps.begin(), timestamps.end())) {
    sorted_ = false;
  }

  timestamps_.reserve(timestamps_.size() + count);
  timestamps_.insert(timestamps_.end(), timestamps.begin(), timestamps.end());

  // The batch may be unsorted: true extrema, not the endpoints.
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
    // A batch may omit columns (an Arrow stream with a subset of the fields): they read null.
    if (columns_[col].rowCount() < expected_rows) {
      columns_[col].appendNulls(expected_rows - columns_[col].rowCount());
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
    bool has_extremes = cs.min_value.has_value();  // false while every value so far is a NaN
    double prev = last_column_values_[col_index];
    bool had_valid = cs.run_count > 0;

    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t row = first_row + i;
      if (has_validity && !col.isValid(row)) {
        cs.null_count++;
        continue;
      }
      const double v = static_cast<double>(buf[row]);
      if constexpr (std::is_floating_point_v<T>) {
        if (cs.all_integral && !encoding::isWholeInt64(v)) {
          cs.all_integral = false;
        }
      }
      if (!std::isnan(v)) {
        local_min = std::min(local_min, v);
        local_max = std::max(local_max, v);
        has_extremes = true;
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
    if (has_extremes) {
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

const ChunkStats& TopicChunkBuilder::stats() {
  padAllColumns();
  return stats_;
}

TypedColumnBuffer& TopicChunkBuilder::padColumn(std::size_t col_index) {
  TypedColumnBuffer& column = columns_[col_index];
  const std::size_t finished_rows = stats_.row_count;
  if (column.rowCount() < finished_rows) {
    const std::size_t owed = finished_rows - column.rowCount();
    column.appendNulls(owed);
    stats_.column_stats[col_index].null_count += static_cast<uint32_t>(owed);  // at most row_count, a uint32_t
  }
  return column;
}

void TopicChunkBuilder::padAllColumns() {
  for (std::size_t i = 0; i < columns_.size(); ++i) {
    padColumn(i);
  }
}

Timestamp TopicChunkBuilder::lastTimestamp() const noexcept {
  return last_timestamp_;
}

void TopicChunkBuilder::noteFloat(std::size_t col_index, double value) {
  ColumnStats& cs = stats_.column_stats[col_index];
  // Tested only until the first fraction, which most float columns hold in their first row.
  if (cs.all_integral && !encoding::isWholeInt64(value)) {
    cs.all_integral = false;
  }
}

void TopicChunkBuilder::updateColumnStats(std::size_t col_index, double value) {
  auto& cs = stats_.column_stats[col_index];

  // A NaN is in neither extreme, in any of the three places that compute them (here, the bulk path, appendRowsTo()):
  // a chunk's stats do not depend on which one built it.
  if (!std::isnan(value)) {
    cs.min_value = cs.min_value.has_value() ? std::min(*cs.min_value, value) : value;
    cs.max_value = cs.max_value.has_value() ? std::max(*cs.max_value, value) : value;
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

// Per-chunk binary searches and cursor merges need sorted chunks; stable, so equal timestamps keep arrival order.
// Only the buffers are permuted: min, max, null count and constancy do not depend on row order, and the encodings
// are derived from the buffers during seal.
void TopicChunkBuilder::sortRowsByTimestamp() {
  PJ_ASSERT(!row_in_progress_, "seal called while row in progress");
  if (sorted_) {
    return;
  }
  sorted_ = true;
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
  padAllColumns();
  sortRowsByTimestamp();
  TopicChunk chunk;
  chunk.id = nextChunkId();
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

    if (kind == StorageKind::kString) {
      chunk.columns[i].data = encoding::dictionaryEncodeStrings(
          Span<const uint8_t>(col.offsetsBuffer().data(), col.offsetsBuffer().size()),
          Span<const uint8_t>(col.valueBuffer().data(), col.valueBuffer().size()), col.rowCount());
    } else if (kind == StorageKind::kBool) {
      if (auto constant = encodeConstantBools(viewOf(col), cs); constant.has_value()) {
        chunk.columns[i].data = std::move(*constant);
      } else {
        chunk.columns[i].data = encoding::packBools(Span<const uint8_t>(col.valueBuffer().data(), col.rowCount()));
      }
    } else {
      bool all_integral = false;
      auto encoded = encodeNumbers(viewOf(col), kind, cs, all_integral);
      if (kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64) {
        chunk.stats.column_stats[i].all_integral = all_integral;
      }
      if (encoded.has_value()) {
        chunk.columns[i].data = std::move(*encoded);
      } else {
        RawBuffer raw;
        raw.append(col.valueBuffer().data(), col.valueBuffer().size());
        chunk.columns[i].data = std::move(raw);
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

// ---------------------------------------------------------------------------
// Growing a chunk in place
// ---------------------------------------------------------------------------

void TopicChunkBuilder::sortRows() {
  padAllColumns();
  sortRowsByTimestamp();
}

bool TopicChunkBuilder::fitsLayoutOf(const TopicChunk& chunk) const {
  if (chunk.schema_version != schema_id_) {
    return false;
  }
  for (const TopicChunk::Column& column : chunk.columns) {
    if (column.descriptor == nullptr) {
      return false;
    }
  }
  const std::size_t shared = std::min(chunk.columns.size(), column_descriptors_.size());
  for (std::size_t i = 0; i < shared; ++i) {
    if (chunk.columns[i].descriptor->logical_type != column_descriptors_[i].logical_type) {
      return false;
    }
  }
  return true;
}

namespace {

// A growable column of `rows` nulls: what a column holds for the rows written before it existed.
TopicChunk::Column makeNullColumn(const ColumnDescriptor& descriptor, std::size_t rows) {
  TopicChunk::Column column;
  column.descriptor = std::make_shared<const ColumnDescriptor>(descriptor);
  const StorageKind kind = storageKindOf(descriptor.logical_type);
  if (kind == StorageKind::kString) {
    encoding::DictionaryEncoded dict;  // 4-byte indices
    if (rows > 0) {
      dict.dictionary.emplace_back();
      dict.indices.resize(rows * sizeof(uint32_t));  // zero: the empty string, a null's slot
      dict.count = rows;
    }
    column.data = std::move(dict);
  } else if (kind == StorageKind::kBool) {
    encoding::PackedBools packed;
    packed.bits.resize(BitVector::bytesForBits(rows));
    packed.count = rows;
    column.data = std::move(packed);
  } else {
    RawBuffer raw;
    raw.resize(rows * storageKindSize(kind));
    column.data = std::move(raw);
  }
  if (rows > 0) {
    column.validity_bitmap.emplace();
    column.validity_bitmap->ensureSize(rows);  // new bytes are zero: every row null
  }
  return column;
}

[[nodiscard]] ColumnStats nullColumnStats(const ColumnDescriptor& descriptor, std::size_t rows) {
  const StorageKind kind = storageKindOf(descriptor.logical_type);
  ColumnStats stats;
  stats.null_count = static_cast<uint32_t>(rows);
  stats.all_integral = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;  // of no value yet
  return stats;
}

}  // namespace

TopicChunk TopicChunkBuilder::makeGrowableChunk() const {
  TopicChunk chunk;
  chunk.id = nextChunkId();
  chunk.topic_id = topic_id_;
  chunk.schema_version = schema_id_;
  chunk.columns.reserve(column_descriptors_.size());
  chunk.stats.column_stats.reserve(column_descriptors_.size());
  for (const ColumnDescriptor& descriptor : column_descriptors_) {
    chunk.columns.push_back(makeNullColumn(descriptor, 0));
    chunk.stats.column_stats.push_back(nullColumnStats(descriptor, 0));
  }
  return chunk;
}

void TopicChunkBuilder::addColumnsTo(TopicChunk& chunk) const {
  // Columns the topic gained since the chunk was started.
  const std::size_t rows = chunk.stats.row_count;
  for (std::size_t c = chunk.columns.size(); c < column_descriptors_.size(); ++c) {
    chunk.columns.push_back(makeNullColumn(column_descriptors_[c], rows));
    chunk.stats.column_stats.push_back(nullColumnStats(column_descriptors_[c], rows));
  }
}

void TopicChunkBuilder::appendRowsTo(TopicChunk& chunk, std::size_t first, std::size_t count) const {
  PJ_ASSERT(!row_in_progress_ && sorted_, "appendRowsTo: call sortRows() first");
  PJ_ASSERT(first + count <= timestamps_.size(), "appendRowsTo: row range out of bounds");
  PJ_ASSERT(fitsLayoutOf(chunk) && chunk.isGrowable(), "appendRowsTo: chunk cannot take these rows");
  if (count == 0) {
    return;
  }
  const std::size_t old_rows = chunk.stats.row_count;
  PJ_ASSERT(old_rows == 0 || timestamps_[first] >= chunk.stats.t_max, "appendRowsTo: rows older than the chunk's");
  addColumnsTo(chunk);
  // The usual case, and the cheap one: this builder's own stats cover exactly the rows appended, so no value is read
  // to compute them. Only a batch cut at a chunk boundary is appended in parts.
  const bool whole_batch = first == 0 && count == timestamps_.size();

  // A column of the chunk this builder does not have (its layout is the older one) takes nulls, through the same path
  // as a column none of these rows set. Never read past these checks.
  const TypedColumnBuffer no_column{ColumnDescriptor{}};
  for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
    const bool missing = c >= columns_.size();
    const TypedColumnBuffer& col = missing ? no_column : columns_[c];
    PJ_ASSERT(missing || col.rowCount() == timestamps_.size(), "appendRowsTo: column not padded");
    TopicChunk::Column& out = chunk.columns[c];
    ColumnStats& cs = chunk.stats.column_stats[c];
    const StorageKind kind = storageKindOf(out.descriptor->logical_type);
    const std::size_t esize = storageKindSize(kind);

    // Validity first. A column no row of the batch set, the usual case of a wide sparse topic, and one every row
    // set, take no pass over their rows. The chunk has a bitmap only once it holds a null, as a sealed chunk does.
    const uint8_t* validity = col.validityBuffer().data();
    std::size_t nulls = 0;
    if (missing || col.nullCount() == col.rowCount()) {
      nulls = count;
    } else if (whole_batch) {
      nulls = col.nullCount();
    } else if (col.hasNulls()) {
      for (std::size_t i = 0; i < count; ++i) {
        nulls += bitIsSet(validity, first + i) ? 0U : 1U;
      }
    }
    if (nulls > 0 && !out.validity_bitmap.has_value()) {
      out.validity_bitmap.emplace();
      out.validity_bitmap->initValid(old_rows);
    }
    if (out.validity_bitmap.has_value()) {
      // Every new bit is written: the spare bits of the old last byte are not known to be clear.
      BitVector& bitmap = *out.validity_bitmap;
      bitmap.ensureSize(old_rows + count);
      if (nulls == count) {
        bitmap.setNullRange(old_rows, old_rows + count);
      } else if (nulls == 0) {
        bitmap.setValidRange(old_rows, old_rows + count);
      } else if (old_rows % 8 == 0 && first % 8 == 0) {
        // Byte-aligned on both sides: whole bytes, whose spare bits past the last row no reader looks at.
        std::memcpy(bitmap.mutableData() + old_rows / 8, validity + first / 8, BitVector::bytesForBits(count));
      } else {
        uint8_t* bits = bitmap.mutableData();
        for (std::size_t i = 0; i < count; ++i) {
          const std::size_t bit = old_rows + i;
          const auto mask = static_cast<uint8_t>(1U << (bit % 8));
          if (bitIsSet(validity, first + i)) {
            bits[bit / 8] |= mask;
          } else {
            bits[bit / 8] &= static_cast<uint8_t>(~mask);
          }
        }
      }
    }
    cs.null_count += static_cast<uint32_t>(nulls);
    const bool all_null = nulls == count;
    const auto is_null = [&](std::size_t i) { return nulls > 0 && (all_null || !bitIsSet(validity, first + i)); };

    if (kind == StorageKind::kString) {
      // The dictionary is not searched: a string equal to the row before it shares its entry, any other gets a new
      // one. compact() leaves each string once. A null's slot is the empty string, as dictionaryEncodeStrings() has it.
      auto& dict = std::get<encoding::DictionaryEncoded>(out.data);
      constexpr uint32_t kNoRow = std::numeric_limits<uint32_t>::max();
      uint32_t last_index = kNoRow;
      if (old_rows > 0) {
        std::memcpy(&last_index, dict.indices.data() + (old_rows - 1) * sizeof(uint32_t), sizeof(uint32_t));
      }
      uint32_t previous_index = kNoRow;  // of the last non-null string, for the run stats
      if (cs.run_count > 0) {
        for (std::size_t r = old_rows; r > 0 && previous_index == kNoRow; --r) {
          if (!chunk.isNull(c, r - 1)) {
            std::memcpy(&previous_index, dict.indices.data() + (r - 1) * sizeof(uint32_t), sizeof(uint32_t));
          }
        }
      }
      dict.indices.reserve(dict.indices.size() + count * sizeof(uint32_t));
      for (std::size_t i = 0; i < count; ++i) {
        const std::string_view value = all_null ? std::string_view{} : col.readString(first + i);
        uint32_t index = last_index;
        if (last_index == kNoRow || dict.dictionary[last_index] != value) {
          index = static_cast<uint32_t>(dict.dictionary.size());
          dict.dictionary.emplace_back(value);
        }
        dict.indices.append(&index, sizeof(index));
        last_index = index;
        if (is_null(i)) {
          continue;
        }
        if (cs.run_count == 0) {
          cs.run_count = 1;
        } else if (previous_index == kNoRow || dict.dictionary[previous_index] != value) {
          cs.is_constant = false;
          cs.run_count++;
        }
        previous_index = index;
      }
      dict.count = old_rows + count;
      continue;
    }

    const uint8_t* values = col.valueBuffer().data();
    if (kind == StorageKind::kBool) {
      auto& packed = std::get<encoding::PackedBools>(out.data);
      packed.bits.resize(BitVector::bytesForBits(old_rows + count));  // new bytes are zero
      packed.count = old_rows + count;
    } else if (all_null) {
      RawBuffer& raw = std::get<RawBuffer>(out.data);
      raw.resize(raw.size() + count * esize);  // zero-filled, as the builder's null slots are
    } else {
      std::get<RawBuffer>(out.data).append(values + first * esize, count * esize);
    }
    if (all_null) {
      continue;
    }

    const bool is_float = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;
    if (whole_batch) {
      // Numbers and bools, from the builder's stats. While a column is constant its one value is its minimum (or a
      // NaN, which has no minimum and equals nothing), so two constant runs are one when their minimums are equal.
      const ColumnStats& batch = stats_.column_stats[c];
      if (batch.run_count > 0) {
        if (cs.run_count == 0) {
          cs.is_constant = batch.is_constant;
          cs.run_count = batch.run_count;
        } else {
          const bool one_value = cs.is_constant && batch.is_constant && cs.min_value.has_value() &&
                                 batch.min_value.has_value() && *cs.min_value == *batch.min_value;
          cs.is_constant = one_value;
          cs.run_count += batch.run_count - (one_value ? 1U : 0U);
        }
      }
      if (batch.min_value.has_value()) {
        cs.min_value = cs.min_value.has_value() ? std::min(*cs.min_value, *batch.min_value) : *batch.min_value;
        cs.max_value = cs.max_value.has_value() ? std::max(*cs.max_value, *batch.max_value) : *batch.max_value;
      }
      if (is_float) {
        cs.all_integral = cs.all_integral && batch.all_integral;
      }
      if (kind == StorageKind::kBool) {
        uint8_t* bits = std::get<encoding::PackedBools>(out.data).bits.mutableData();
        for (std::size_t i = 0; i < count; ++i) {
          if (values[i] != 0) {
            bits[(old_rows + i) / 8] |= static_cast<uint8_t>(1U << ((old_rows + i) % 8));
          }
        }
      }
      continue;
    }

    // A part of the batch: the same stats, computed over the rows appended as the builder's bulk path computes them,
    // so compact() leaves them as they are. A NaN is in neither extreme.
    double previous =
        cs.is_constant && cs.min_value.has_value() ? *cs.min_value : std::numeric_limits<double>::quiet_NaN();
    const auto add_value = [&](double v) {
      if (!std::isnan(v)) {
        cs.min_value = cs.min_value.has_value() ? std::min(*cs.min_value, v) : v;
        cs.max_value = cs.max_value.has_value() ? std::max(*cs.max_value, v) : v;
      }
      if (cs.run_count == 0) {
        cs.run_count = 1;
      } else if (v != previous) {
        cs.is_constant = false;
        cs.run_count++;
      }
      previous = v;
      if (is_float && !encoding::isWholeInt64(v)) {
        cs.all_integral = false;
      }
    };

    if (kind == StorageKind::kBool) {
      uint8_t* bits = std::get<encoding::PackedBools>(out.data).bits.mutableData();
      for (std::size_t i = 0; i < count; ++i) {
        const bool value = values[first + i] != 0;
        if (value) {
          bits[(old_rows + i) / 8] |= static_cast<uint8_t>(1U << ((old_rows + i) % 8));
        }
        if (!is_null(i)) {
          add_value(value ? 1.0 : 0.0);
        }
      }
      continue;
    }
    dispatchNumericKind(kind, [&]<typename T>(const T* /*tag*/) {
      for (std::size_t i = 0; i < count; ++i) {
        if (is_null(i)) {
          continue;
        }
        T v{};
        std::memcpy(&v, values + (first + i) * sizeof(T), sizeof(T));
        add_value(static_cast<double>(v));
      }
    });
  }

  chunk.timestamps.insert(
      chunk.timestamps.end(), timestamps_.begin() + static_cast<std::ptrdiff_t>(first),
      timestamps_.begin() + static_cast<std::ptrdiff_t>(first + count));
  chunk.stats.t_min = std::min(chunk.stats.t_min, timestamps_[first]);
  chunk.stats.t_max = std::max(chunk.stats.t_max, timestamps_[first + count - 1]);
  chunk.stats.row_count += static_cast<uint32_t>(count);
}

void TopicChunkBuilder::appendRowsOf(const TopicChunk& chunk) {
  const std::size_t rows = chunk.stats.row_count;
  if (rows == 0) {
    return;
  }
  appendTimestamps(Span<const Timestamp>(chunk.timestamps.data(), rows));
  std::vector<uint8_t> bool_bytes;
  std::vector<uint32_t> offsets;
  std::string text;
  for (std::size_t c = 0; c < columns_.size(); ++c) {
    const TopicChunk::Column& column = chunk.columns[c];
    const StorageKind kind = storageKindOf(column_descriptors_[c].logical_type);
    if (kind == StorageKind::kString) {
      const auto& dict = std::get<encoding::DictionaryEncoded>(column.data);
      offsets.assign(1, 0);
      text.clear();
      for (std::size_t r = 0; r < rows; ++r) {
        text.append(encoding::dictionaryLookup(dict, r));
        offsets.push_back(static_cast<uint32_t>(text.size()));
      }
      appendColumnStrings(
          c, Span<const uint32_t>(offsets.data(), offsets.size()), Span<const char>(text.data(), text.size()));
    } else if (kind == StorageKind::kBool) {
      bool_bytes.resize(rows);
      for (std::size_t r = 0; r < rows; ++r) {
        bool_bytes[r] = chunk.readBool(c, r) ? 1 : 0;
      }
      appendColumn<uint8_t>(c, Span<const uint8_t>(bool_bytes.data(), rows));
    } else {
      const RawBuffer& raw = std::get<RawBuffer>(column.data);
      dispatchNumericKind(kind, [&]<typename T>(const T* /*tag*/) {
        appendColumn<T>(c, Span<const T>(reinterpret_cast<const T*>(raw.data()), rows));
      });
    }
    if (column.validity_bitmap.has_value()) {
      appendColumnValidity(
          c,
          BitSpan{Span<const uint8_t>(column.validity_bitmap->data(), column.validity_bitmap->sizeBytes()), 0, rows});
    }
  }
  finishBulkAppend();
}

void TopicChunkBuilder::appendRowsOf(const TopicChunkBuilder& other) {
  const std::size_t rows = other.timestamps_.size();
  if (rows == 0) {
    return;
  }
  appendTimestamps(Span<const Timestamp>(other.timestamps_.data(), rows));
  for (std::size_t c = 0; c < columns_.size(); ++c) {
    const TypedColumnBuffer& col = other.columns_[c];
    const StorageKind kind = storageKindOf(column_descriptors_[c].logical_type);
    if (kind == StorageKind::kString) {
      appendColumnStrings(
          c, Span<const uint32_t>(reinterpret_cast<const uint32_t*>(col.offsetsBuffer().data()), rows + 1),
          Span<const char>(reinterpret_cast<const char*>(col.valueBuffer().data()), col.valueBuffer().size()));
    } else if (kind == StorageKind::kBool) {
      appendColumn<uint8_t>(c, Span<const uint8_t>(col.valueBuffer().data(), rows));
    } else {
      dispatchNumericKind(kind, [&]<typename T>(const T* /*tag*/) {
        appendColumn<T>(c, Span<const T>(reinterpret_cast<const T*>(col.valueBuffer().data()), rows));
      });
    }
    if (col.hasNulls()) {
      appendColumnValidity(
          c, BitSpan{Span<const uint8_t>(col.validityBuffer().data(), col.validityBuffer().sizeBytes()), 0, rows});
    }
  }
  finishBulkAppend();
}

TopicChunk TopicChunkBuilder::mergedWith(const TopicChunk& chunk) const {
  PJ_ASSERT(!row_in_progress_ && sorted_, "mergedWith: call sortRows() first");
  PJ_ASSERT(chunk.columns.size() == columns_.size() && fitsLayoutOf(chunk), "mergedWith: layouts differ");
  PJ_ASSERT(chunk.isGrowable(), "mergedWith: chunk is not in growable form");
  const std::size_t rows = chunk.stats.row_count + timestamps_.size();
  TopicChunkBuilder merged(topic_id_, schema_id_, column_descriptors_, static_cast<uint32_t>(rows));
  merged.appendRowsOf(chunk);  // first, so the stable sort keeps them first among equal timestamps
  merged.appendRowsOf(*this);
  merged.sortRows();
  TopicChunk result = merged.makeGrowableChunk();
  merged.appendRowsTo(result, 0, rows);
  return result;
}

bool TopicChunk::isGrowable() const {
  for (const Column& column : columns) {
    if (column.descriptor == nullptr) {
      return false;
    }
    switch (storageKindOf(column.descriptor->logical_type)) {
      case StorageKind::kString: {
        const auto* dict = std::get_if<encoding::DictionaryEncoded>(&column.data);
        if (dict == nullptr || dict->index_bytes != sizeof(uint32_t)) {
          return false;
        }
        break;
      }
      case StorageKind::kBool:
        if (!std::holds_alternative<encoding::PackedBools>(column.data)) {
          return false;
        }
        break;
      default:
        if (!std::holds_alternative<RawBuffer>(column.data)) {
          return false;
        }
        break;
    }
  }
  return true;
}

void TopicChunk::makeGrowable() {
  const std::size_t rows = stats.row_count;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    Column& column = columns[c];
    PJ_ASSERT(column.descriptor != nullptr, "makeGrowable: column without a descriptor");
    const StorageKind kind = storageKindOf(column.descriptor->logical_type);
    if (kind == StorageKind::kString) {
      auto& dict = std::get<encoding::DictionaryEncoded>(column.data);
      if (dict.index_bytes != sizeof(uint32_t)) {
        RawBuffer wide;
        wide.reserve(rows * sizeof(uint32_t));
        for (std::size_t r = 0; r < rows; ++r) {
          uint32_t index = 0;
          if (dict.index_bytes == sizeof(uint8_t)) {
            index = dict.indices.data()[r];
          } else {
            uint16_t narrow = 0;
            std::memcpy(&narrow, dict.indices.data() + r * sizeof(narrow), sizeof(narrow));
            index = narrow;
          }
          wide.append(&index, sizeof(index));
        }
        dict.indices = std::move(wide);
        dict.index_bytes = sizeof(uint32_t);
      }
      continue;
    }
    if (kind == StorageKind::kBool) {
      if (!std::holds_alternative<encoding::PackedBools>(column.data)) {
        std::vector<uint8_t> bytes(rows);
        for (std::size_t r = 0; r < rows; ++r) {
          bytes[r] = readBool(c, r) ? 1 : 0;
        }
        column.data = encoding::packBools(Span<const uint8_t>(bytes.data(), bytes.size()));
      }
      continue;
    }
    if (std::holds_alternative<RawBuffer>(column.data)) {
      continue;
    }
    RawBuffer raw;
    const std::size_t esize = storageKindSize(kind);
    raw.reserve(rows * esize);
    if (const auto* constant = std::get_if<encoding::ConstantEncoded>(&column.data)) {
      for (std::size_t r = 0; r < rows; ++r) {
        raw.append(constant->value_bytes.data(), esize);  // the bytes themselves: a NaN keeps its payload
      }
    } else {
      // Frame-of-reference holds integers, or floats that are whole numbers: both go through int64 exactly.
      dispatchNumericKind(kind, [&]<typename T>(const T* /*tag*/) {
        for (std::size_t r = 0; r < rows; ++r) {
          const T v = static_cast<T>(readNumericAsInt64(c, r));
          raw.append(&v, sizeof(v));
        }
      });
    }
    column.data = std::move(raw);
  }
}

void TopicChunk::compact() {
  const std::size_t rows = stats.row_count;
  if (rows == 0) {
    return;
  }
  makeGrowable();
  // The buffers that stay as they are grew by appends: their slack goes, as a chunk sealed whole has none.
  if (timestamps.capacity() > timestamps.size()) {
    timestamps.shrink_to_fit();
  }
  std::vector<uint8_t> bool_bytes;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    Column& column = columns[c];
    ColumnStats& cs = stats.column_stats[c];
    const StorageKind kind = storageKindOf(column.descriptor->logical_type);
    if (column.validity_bitmap.has_value() && cs.null_count == 0) {
      column.validity_bitmap.reset();
    }
    if (column.validity_bitmap.has_value()) {
      column.validity_bitmap->shrinkToFit();
    }
    const uint8_t* validity = column.validity_bitmap.has_value() ? column.validity_bitmap->data() : nullptr;
    if (kind == StorageKind::kString) {
      column.data = encoding::dictionaryRecode(std::get<encoding::DictionaryEncoded>(column.data));
    } else if (kind == StorageKind::kBool) {
      bool_bytes.resize(rows);
      for (std::size_t r = 0; r < rows; ++r) {
        bool_bytes[r] = readBool(c, r) ? 1 : 0;
      }
      if (auto constant = encodeConstantBools({bool_bytes.data(), rows, rows, validity}, cs); constant.has_value()) {
        column.data = std::move(*constant);
      } else {
        std::get<encoding::PackedBools>(column.data).bits.shrinkToFit();
      }
    } else {
      RawBuffer& raw = std::get<RawBuffer>(column.data);
      bool all_integral = false;
      auto encoded = encodeNumbers({raw.data(), rows, raw.size(), validity}, kind, cs, all_integral);
      if (kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64) {
        cs.all_integral = all_integral;
      }
      if (encoded.has_value()) {
        column.data = std::move(*encoded);
      } else {
        raw.shrinkToFit();
      }
    }
  }
}

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

  // Nulls read as NaN, not as the zero their slot holds.
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
