// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/column_buffer.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace PJ {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TypedColumnBuffer::TypedColumnBuffer(ColumnDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const ColumnDescriptor& TypedColumnBuffer::descriptor() const noexcept {
  return descriptor_;
}

std::size_t TypedColumnBuffer::rowCount() const noexcept {
  return row_count_;
}

bool TypedColumnBuffer::hasNulls() const noexcept {
  return null_count_ > 0;
}

bool TypedColumnBuffer::isValid(std::size_t row) const noexcept {
  if (!validity_initialized_) {
    return true;
  }
  return validity_.isValid(row);
}

// ---------------------------------------------------------------------------
// Underlying buffers
// ---------------------------------------------------------------------------

const RawBuffer& TypedColumnBuffer::valueBuffer() const noexcept {
  return values_;
}

const BitVector& TypedColumnBuffer::validityBuffer() const noexcept {
  return validity_;
}

const RawBuffer& TypedColumnBuffer::offsetsBuffer() const noexcept {
  return offsets_;
}

// ---------------------------------------------------------------------------
// Validity (lazy initialization)
// ---------------------------------------------------------------------------

void TypedColumnBuffer::ensureValidityInitialized() {
  if (validity_initialized_) {
    return;
  }
  // Rows appended before the first null are all valid.
  validity_.initValid(row_count_);
  validity_initialized_ = true;
}

// ---------------------------------------------------------------------------
// Fixed-size append / read templates
// ---------------------------------------------------------------------------

template <typename T>
void TypedColumnBuffer::appendFixed(T value) {
  values_.append(&value, sizeof(T));
  if (validity_initialized_) {
    validity_.ensureSize(row_count_ + 1);
    validity_.setValid(row_count_);
  }
  ++row_count_;
}

template <typename T>
T TypedColumnBuffer::readFixed(std::size_t row) const {
  T result{};
  std::memcpy(&result, values_.data() + row * sizeof(T), sizeof(T));
  return result;
}

// ---------------------------------------------------------------------------
// Typed append functions
// ---------------------------------------------------------------------------

void TypedColumnBuffer::appendFloat32(float value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kFloat32);
  appendFixed(value);
}

void TypedColumnBuffer::appendFloat64(double value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kFloat64);
  appendFixed(value);
}

void TypedColumnBuffer::appendInt32(int32_t value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kInt32);
  appendFixed(value);
}

void TypedColumnBuffer::appendInt64(int64_t value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kInt64);
  appendFixed(value);
}

void TypedColumnBuffer::appendUint64(uint64_t value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kUint64);
  appendFixed(value);
}

void TypedColumnBuffer::appendBool(bool value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kBool);
  const auto byte = static_cast<uint8_t>(value ? 1 : 0);
  appendFixed(byte);
}

void TypedColumnBuffer::appendString(std::string_view value) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kString);
  if (row_count_ == 0) {
    const uint32_t zero = 0;
    offsets_.append(&zero, sizeof(zero));
  }

  values_.append(value.data(), value.size());

  const auto end_offset = static_cast<uint32_t>(values_.size());
  offsets_.append(&end_offset, sizeof(end_offset));

  if (validity_initialized_) {
    validity_.ensureSize(row_count_ + 1);
    validity_.setValid(row_count_);
  }

  ++row_count_;
}

void TypedColumnBuffer::appendNull() {
  ensureValidityInitialized();

  validity_.ensureSize(row_count_ + 1);
  validity_.setNull(row_count_);

  // A null still takes a value slot, so values stay row-aligned.
  const StorageKind kind = storageKindOf(descriptor_.logical_type);
  if (kind == StorageKind::kString) {
    if (row_count_ == 0) {
      const uint32_t zero = 0;
      offsets_.append(&zero, sizeof(zero));
    }
    const auto current_offset = static_cast<uint32_t>(values_.size());
    offsets_.append(&current_offset, sizeof(current_offset));
  } else {
    const std::size_t type_size = storageKindSize(kind);
    const uint64_t zero = 0;  // 8 bytes, enough for any fixed type
    values_.append(&zero, type_size);
  }

  ++row_count_;
  ++null_count_;
}

// ---------------------------------------------------------------------------
// Bulk fixed-size append template
// ---------------------------------------------------------------------------

template <typename T>
void TypedColumnBuffer::appendFixedBulk(Span<const T> data) {
  const std::size_t count = data.size();
  if (count == 0) {
    return;
  }
  values_.reserve(values_.size() + count * sizeof(T));
  values_.append(data.data(), count * sizeof(T));
  if (validity_initialized_) {
    const std::size_t new_total = row_count_ + count;
    validity_.ensureSize(new_total);
    for (std::size_t i = row_count_; i < new_total; ++i) {
      validity_.setValid(i);
    }
  }
  row_count_ += count;
}

// ---------------------------------------------------------------------------
// Typed bulk append functions
// ---------------------------------------------------------------------------

void TypedColumnBuffer::appendFloat32Bulk(Span<const float> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kFloat32);
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendFloat64Bulk(Span<const double> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kFloat64);
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendInt32Bulk(Span<const int32_t> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kInt32);
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendInt64Bulk(Span<const int64_t> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kInt64);
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendUint64Bulk(Span<const uint64_t> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kUint64);
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendBoolBulk(Span<const uint8_t> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kBool);
  // One byte per bool, unpacked.
  appendFixedBulk(data);
}

void TypedColumnBuffer::appendStringsBulk(Span<const uint32_t> offsets, Span<const char> data) {
  assert(storageKindOf(descriptor_.logical_type) == StorageKind::kString);
  if (offsets.empty()) {
    return;
  }
  const std::size_t count = offsets.size() - 1;
  if (count == 0) {
    return;
  }

  const uint32_t base_data_offset = static_cast<uint32_t>(values_.size());
  if (row_count_ == 0) {
    const uint32_t zero = 0;
    offsets_.append(&zero, sizeof(zero));
  }

  const uint32_t total_string_bytes = offsets[count] - offsets[0];
  assert(offsets[0] <= static_cast<uint32_t>(data.size()));
  assert(offsets[count] <= static_cast<uint32_t>(data.size()));
  values_.reserve(values_.size() + total_string_bytes);
  values_.append(data.data() + offsets[0], total_string_bytes);

  // Rebase the offsets onto this buffer.
  const uint32_t src_base = offsets[0];
  for (std::size_t i = 1; i <= count; ++i) {
    const uint32_t adjusted = base_data_offset + (offsets[i] - src_base);
    offsets_.append(&adjusted, sizeof(adjusted));
  }

  if (validity_initialized_) {
    const std::size_t new_total = row_count_ + count;
    validity_.ensureSize(new_total);
    for (std::size_t i = row_count_; i < new_total; ++i) {
      validity_.setValid(i);
    }
  }

  row_count_ += count;
}

void TypedColumnBuffer::appendValidityBulk(BitSpan validity) {
  const std::size_t count = validity.bit_length;
  if (count == 0) {
    return;
  }
  ensureValidityInitialized();

  // Called after the values: covers the last `count` rows, already counted in row_count_.
  const std::size_t start_row = row_count_ - count;
  validity_.ensureSize(row_count_);

  for (std::size_t i = 0; i < count; ++i) {
    const bool valid = validity.test(i);
    if (valid) {
      validity_.setValid(start_row + i);
    } else {
      validity_.setNull(start_row + i);
      ++null_count_;
    }
  }
}

// ---------------------------------------------------------------------------
// Typed read functions
// ---------------------------------------------------------------------------

float TypedColumnBuffer::readFloat32(std::size_t row) const {
  return readFixed<float>(row);
}

double TypedColumnBuffer::readFloat64(std::size_t row) const {
  return readFixed<double>(row);
}

int32_t TypedColumnBuffer::readInt32(std::size_t row) const {
  return readFixed<int32_t>(row);
}

int64_t TypedColumnBuffer::readInt64(std::size_t row) const {
  return readFixed<int64_t>(row);
}

uint64_t TypedColumnBuffer::readUint64(std::size_t row) const {
  return readFixed<uint64_t>(row);
}

bool TypedColumnBuffer::readBool(std::size_t row) const {
  return readFixed<uint8_t>(row) != 0;
}

std::string_view TypedColumnBuffer::readString(std::size_t row) const {
  uint32_t start_offset = 0;
  uint32_t end_offset = 0;
  std::memcpy(&start_offset, offsets_.data() + row * sizeof(uint32_t), sizeof(uint32_t));
  std::memcpy(&end_offset, offsets_.data() + (row + 1) * sizeof(uint32_t), sizeof(uint32_t));
  return {reinterpret_cast<const char*>(values_.data()) + start_offset, end_offset - start_offset};
}

bool TypedColumnBuffer::isNull(std::size_t row) const {
  if (!validity_initialized_) {
    return false;
  }
  return !validity_.isValid(row);
}

double TypedColumnBuffer::readAsDouble(std::size_t row) const {
  switch (storageKindOf(descriptor_.logical_type)) {
    case StorageKind::kFloat32:
      return static_cast<double>(readFloat32(row));
    case StorageKind::kFloat64:
      return readFloat64(row);
    case StorageKind::kInt32:
      return static_cast<double>(readInt32(row));
    case StorageKind::kInt64:
      return static_cast<double>(readInt64(row));
    case StorageKind::kUint64:
      return static_cast<double>(readUint64(row));
    case StorageKind::kBool:
      return readBool(row) ? 1.0 : 0.0;
    case StorageKind::kString:
      return std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();  // unreachable
}

}  // namespace PJ
