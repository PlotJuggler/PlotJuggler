#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_datastore/buffer.hpp"
#include "pj_datastore/column_buffer.hpp"  // StorageKind

namespace PJ::encoding {

// ---------------------------------------------------------------------------
// Constant encoding — stores a single repeated value
// ---------------------------------------------------------------------------
struct ConstantEncoded {
  /// Raw bytes of the repeated value (up to 8 bytes).
  std::array<uint8_t, 8> value_bytes{};  // Single value (max StorageKind size)
  /// Storage kind used to interpret `value_bytes`.
  StorageKind value_kind;
  /// Number of valid bytes in `value_bytes`.
  uint8_t value_size = 0;
  /// Number of rows represented by this constant.
  std::size_t count = 0;
};

// ---------------------------------------------------------------------------
// Frame of Reference — subtract min, store narrowed offsets
// (Applied to kInt32 and kInt64 columns, and to float32/float64 chunks whose
//  values are all whole numbers; kUint64 is excluded because large unsigned
//  values overflow the int64_t reference.)
// ---------------------------------------------------------------------------
struct FrameOfReferenceEncoded {
  /// Base value added to each offset during decode.
  int64_t reference = 0;  // min value
  /// Bytes per encoded offset (1, 2, or 4).
  uint8_t offset_bytes = 0;  // 1, 2, or 4
  /// Packed offset payload.
  RawBuffer offsets;  // packed as uint8/uint16/uint32
  /// Number of encoded rows.
  std::size_t count = 0;
};

// ---------------------------------------------------------------------------
// Dictionary encoding for strings (with narrowed indices)
// ---------------------------------------------------------------------------
struct DictionaryEncoded {
  /// Unique dictionary entries.
  std::vector<std::string> dictionary;  // unique values in insertion order
  /// Packed dictionary indices.
  RawBuffer indices;  // stored as uint8/uint16/uint32
  /// Bytes per index (1, 2, or 4).
  uint8_t index_bytes = 4;  // 1, 2, or 4
  /// Number of encoded rows.
  std::size_t count = 0;
};

// Encode a string column into dictionary form.
// Takes raw string data (offsets buffer + value buffer from TypedColumnBuffer).
[[nodiscard]] DictionaryEncoded dictionaryEncodeStrings(
    PJ::Span<const uint8_t> offsets_data, PJ::Span<const uint8_t> values_data, std::size_t row_count);

[[nodiscard]] std::string_view dictionaryLookup(const DictionaryEncoded& encoded, std::size_t row);

// ---------------------------------------------------------------------------
// Packed bitfield for bools (1 bit per value, LSB first like Arrow validity bitmaps)
// ---------------------------------------------------------------------------
struct PackedBools {
  /// Packed bit values (LSB first).
  RawBuffer bits;
  /// Number of boolean rows.
  std::size_t count = 0;
};

// Pack bool values (stored as uint8_t 0/1) into a bitfield
/// Pack bool bytes into a compact bitfield.
[[nodiscard]] PackedBools packBools(PJ::Span<const uint8_t> values);

/// Read one bool from a packed bitfield.
[[nodiscard]] bool unpackBool(const PackedBools& packed, std::size_t index);

// ---------------------------------------------------------------------------
// Unified per-column encoding data variant
// ---------------------------------------------------------------------------
using EncodedData = std::variant<RawBuffer, ConstantEncoded, FrameOfReferenceEncoded, DictionaryEncoded, PackedBools>;

// ---------------------------------------------------------------------------
// Constant encoding functions
// ---------------------------------------------------------------------------
/// Build constant encoding from one repeated storage-kind value.
[[nodiscard]] ConstantEncoded constantEncode(PJ::Span<const uint8_t> data, StorageKind kind, std::size_t count);

/// Decode constant numeric value as double.
[[nodiscard]] double constantDecodeAsDouble(const ConstantEncoded& enc);

/// Decode constant numeric value as int64_t (no precision loss for integer types).
[[nodiscard]] int64_t constantDecodeAsInt64(const ConstantEncoded& enc);

/// Decode constant numeric value as uint64_t (no precision loss for integer types).
[[nodiscard]] uint64_t constantDecodeAsUint64(const ConstantEncoded& enc);

// ---------------------------------------------------------------------------
// Frame of Reference encoding functions
// Data must be kInt32 or kInt64 values (whole-number float chunks are converted
// to int64 before encoding).
// ---------------------------------------------------------------------------
/// Encode signed integers as offsets from `min_val`.
[[nodiscard]] FrameOfReferenceEncoded forEncode(
    PJ::Span<const uint8_t> data, StorageKind kind, std::size_t count, int64_t min_val, int64_t max_val);
/// Decode one FOR value as double.
[[nodiscard]] double forDecodeOneAsDouble(const FrameOfReferenceEncoded& enc, std::size_t row);

/// Decode one FOR value as int64_t (no precision loss).
[[nodiscard]] int64_t forDecodeOneAsInt64(const FrameOfReferenceEncoded& enc, std::size_t row);

/// Decode a contiguous FOR range into `out`.
void forDecodeRangeAsDoubles(const FrameOfReferenceEncoded& enc, PJ::Span<double> out, std::size_t row_start);

/// True when `v` is a whole number representable as int64: finite, inside
/// [-2^63, 2^63), no fractional part. Such a float is an exact integer, which is
/// what lets whole-number float chunks use frame-of-reference.
[[nodiscard]] inline bool isWholeInt64(double v) noexcept {
  return v >= -0x1p63 && v < 0x1p63 && v == std::trunc(v);  // false for NaN
}

// ---------------------------------------------------------------------------
// Byte-width helpers
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr uint8_t indexBytesFor(std::size_t dict_size) noexcept {
  if (dict_size <= 256) {
    return 1;
  }
  if (dict_size <= 65536) {
    return 2;
  }
  return 4;
}

[[nodiscard]] constexpr uint8_t offsetBytesFor(uint64_t range) noexcept {
  if (range < 256) {
    return 1;
  }
  if (range < 65536) {
    return 2;
  }
  if (range < uint64_t{1} << 32) {
    return 4;
  }
  return 8;  // signals: stay kRaw
}

}  // namespace PJ::encoding
