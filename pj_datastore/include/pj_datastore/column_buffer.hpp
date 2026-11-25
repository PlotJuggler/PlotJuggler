#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "pj_base/span.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/buffer.hpp"

namespace PJ {

using PJ::BitSpan;
using PJ::FieldId;
using PJ::PrimitiveType;
using PJ::Span;

/// Physical storage of a column; narrow integers widen (see storageKindOf()).
enum class StorageKind : uint8_t {
  kFloat32,
  kFloat64,
  kInt32,
  kInt64,
  kUint64,
  kBool,
  kString,
};

[[nodiscard]] constexpr StorageKind storageKindOf(PrimitiveType t) noexcept {
  switch (t) {
    case PrimitiveType::kFloat32:
      return StorageKind::kFloat32;
    case PrimitiveType::kFloat64:
      return StorageKind::kFloat64;
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
      return StorageKind::kInt64;
    case PrimitiveType::kInt32:
      return StorageKind::kInt32;
    case PrimitiveType::kInt64:
      return StorageKind::kInt64;
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kUint64:
      return StorageKind::kUint64;
    case PrimitiveType::kBool:
      return StorageKind::kBool;
    case PrimitiveType::kString:
      return StorageKind::kString;
    case PrimitiveType::kUnspecified:
      break;
  }
  return StorageKind::kFloat64;
}

/// Element size in bytes; 0 for kString.
[[nodiscard]] constexpr std::size_t storageKindSize(StorageKind k) noexcept {
  switch (k) {
    case StorageKind::kFloat32:
      return sizeof(float);
    case StorageKind::kFloat64:
      return sizeof(double);
    case StorageKind::kInt32:
      return sizeof(int32_t);
    case StorageKind::kInt64:
      return sizeof(int64_t);
    case StorageKind::kUint64:
      return sizeof(uint64_t);
    case StorageKind::kBool:
      return sizeof(uint8_t);
    case StorageKind::kString:
      return 0;
  }
  return 0;
}

enum class EncodingType : uint8_t {
  kRaw,
  kDictionary,  // strings
  kPackedBool,  // bools
  kConstant,
  kFrameOfReference,
};

/// One column of a topic's flattened schema.
struct ColumnDescriptor {
  /// Dense from 0 in column order.
  FieldId field_id;
  PrimitiveType logical_type;
  /// Dotted path of the leaf from the schema root ("pose.position.x"); a fixed-array element adds "[i]".
  std::string field_path;
};

/// One column's rows before sealing: fixed-width values, or strings as a byte buffer plus uint32 offsets. The validity
/// bitmap exists only once a null or a bitmap is appended. The typed appends assert (debug builds only) that the type
/// matches the column's StorageKind.
class TypedColumnBuffer {
 public:
  explicit TypedColumnBuffer(ColumnDescriptor descriptor);

  [[nodiscard]] const ColumnDescriptor& descriptor() const noexcept;

  [[nodiscard]] std::size_t rowCount() const noexcept;

  [[nodiscard]] bool hasNulls() const noexcept;

  [[nodiscard]] bool isValid(std::size_t row) const noexcept;

  void appendFloat32(float value);

  void appendFloat64(double value);

  void appendInt32(int32_t value);

  void appendInt64(int64_t value);

  void appendUint64(uint64_t value);

  void appendBool(bool value);

  void appendString(std::string_view value);

  /// The value slot is zero-filled (an empty string for strings).
  void appendNull();
  /// `count` nulls in one step: the buffers grow once, not per row.
  void appendNulls(std::size_t count);

  [[nodiscard]] float readFloat32(std::size_t row) const;

  [[nodiscard]] double readFloat64(std::size_t row) const;

  [[nodiscard]] int32_t readInt32(std::size_t row) const;

  [[nodiscard]] int64_t readInt64(std::size_t row) const;

  [[nodiscard]] uint64_t readUint64(std::size_t row) const;

  [[nodiscard]] bool readBool(std::size_t row) const;

  /// Views the value buffer: invalid after the next append.
  [[nodiscard]] std::string_view readString(std::size_t row) const;

  [[nodiscard]] bool isNull(std::size_t row) const;

  /// Does not check for nulls (a null slot reads as 0). NaN for strings, 0 or 1 for bools.
  [[nodiscard]] double readAsDouble(std::size_t row) const;

  void appendFloat32Bulk(Span<const float> data);

  void appendFloat64Bulk(Span<const double> data);

  void appendInt32Bulk(Span<const int32_t> data);

  void appendInt64Bulk(Span<const int64_t> data);

  void appendUint64Bulk(Span<const uint64_t> data);

  /// Bytes of 0 or 1.
  void appendBoolBulk(Span<const uint8_t> data);

  /// Arrow layout: `offsets` has count + 1 entries into `data` and need not start at 0.
  void appendStringsBulk(Span<const uint32_t> offsets, Span<const char> data);

  /// Validity of the last `validity.bit_length` rows appended: call it right after their values. Arrow bit layout.
  void appendValidityBulk(BitSpan validity);

  // Raw buffers, read by TopicChunkBuilder::seal().
  /// Fixed-width values, or the concatenated string bytes.
  [[nodiscard]] const RawBuffer& valueBuffer() const noexcept;

  /// Empty until a null or a bitmap is appended.
  [[nodiscard]] const BitVector& validityBuffer() const noexcept;

  /// Strings only: uint32 offsets into valueBuffer(), row_count + 1 entries once there is a row.
  [[nodiscard]] const RawBuffer& offsetsBuffer() const noexcept;

 private:
  ColumnDescriptor descriptor_;
  RawBuffer values_;
  BitVector validity_;
  RawBuffer offsets_;
  std::size_t row_count_ = 0;
  std::size_t null_count_ = 0;
  bool validity_initialized_ = false;

  void ensureValidityInitialized();

  template <typename T>
  void appendFixed(T value);

  template <typename T>
  void appendFixedBulk(Span<const T> data);

  template <typename T>
  [[nodiscard]] T readFixed(std::size_t row) const;
};

}  // namespace PJ
