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

/// One value repeated `count` times.
struct ConstantEncoded {
  /// The value, `value_size` bytes of `value_kind`.
  std::array<uint8_t, 8> value_bytes{};
  StorageKind value_kind;
  uint8_t value_size = 0;
  std::size_t count = 0;
};

/// `reference` plus a narrowed unsigned offset per row. Used for kInt32/kInt64 columns and for float columns whose
/// non-null values are all whole numbers; not kUint64, whose large values overflow the int64_t reference.
struct FrameOfReferenceEncoded {
  /// The column minimum; decode adds the offset to it.
  int64_t reference = 0;
  /// 1, 2 or 4.
  uint8_t offset_bytes = 0;
  /// `count` offsets of `offset_bytes` each.
  RawBuffer offsets;
  std::size_t count = 0;
};

/// Unique strings in first-seen order plus a narrowed index per row.
struct DictionaryEncoded {
  std::vector<std::string> dictionary;
  /// `count` indices of `index_bytes` each.
  RawBuffer indices;
  /// 1, 2 or 4 (see indexBytesFor()).
  uint8_t index_bytes = 4;
  std::size_t count = 0;
};

/// `offsets_data` holds row_count + 1 uint32 offsets into `values_data`, the layout of a string column buffer.
[[nodiscard]] DictionaryEncoded dictionaryEncodeStrings(
    PJ::Span<const uint8_t> offsets_data, PJ::Span<const uint8_t> values_data, std::size_t row_count);

/// The same rows with each string once, in first-seen order, and the narrowest indices: what
/// dictionaryEncodeStrings() gives them. For a dictionary that grew row by row (TopicChunk::compact()).
[[nodiscard]] DictionaryEncoded dictionaryRecode(const DictionaryEncoded& encoded);

/// The view points into `encoded`'s dictionary.
[[nodiscard]] std::string_view dictionaryLookup(const DictionaryEncoded& encoded, std::size_t row);

/// One bit per bool, LSB first like Arrow validity bitmaps.
struct PackedBools {
  RawBuffer bits;
  std::size_t count = 0;
};

/// `values` are bytes of 0 or 1.
[[nodiscard]] PackedBools packBools(PJ::Span<const uint8_t> values);

[[nodiscard]] bool unpackBool(const PackedBools& packed, std::size_t index);

using EncodedData = std::variant<RawBuffer, ConstantEncoded, FrameOfReferenceEncoded, DictionaryEncoded, PackedBools>;

/// Copies the value from the start of `data`.
[[nodiscard]] ConstantEncoded constantEncode(PJ::Span<const uint8_t> data, StorageKind kind, std::size_t count);

/// Rounds integers above 2^53; the Int64/Uint64 decoders are exact for integer kinds.
[[nodiscard]] double constantDecodeAsDouble(const ConstantEncoded& enc);

[[nodiscard]] int64_t constantDecodeAsInt64(const ConstantEncoded& enc);

[[nodiscard]] uint64_t constantDecodeAsUint64(const ConstantEncoded& enc);

/// `data` holds kInt32 or kInt64 values (a whole-number float column is converted to int64 first). The range
/// `max_val - min_val` must be narrower than 8 offset bytes (offsetBytesFor() < 8): the offsets are written as at most
/// uint32, so a wider range stays raw.
[[nodiscard]] FrameOfReferenceEncoded forEncode(
    PJ::Span<const uint8_t> data, StorageKind kind, std::size_t count, int64_t min_val, int64_t max_val);

/// Rounds integers above 2^53; forDecodeOneAsInt64() is exact.
[[nodiscard]] double forDecodeOneAsDouble(const FrameOfReferenceEncoded& enc, std::size_t row);

[[nodiscard]] int64_t forDecodeOneAsInt64(const FrameOfReferenceEncoded& enc, std::size_t row);

/// Decodes rows [row_start, row_start + out.size()).
void forDecodeRangeAsDoubles(const FrameOfReferenceEncoded& enc, PJ::Span<double> out, std::size_t row_start);

/// True when `v` converts to int64_t exactly: finite, inside [-2^63, 2^63), no fractional part (false for NaN). Lets
/// whole-number float columns use frame-of-reference.
[[nodiscard]] inline bool isWholeInt64(double v) noexcept {
  return v >= -0x1p63 && v < 0x1p63 && v == std::trunc(v);
}

[[nodiscard]] constexpr uint8_t indexBytesFor(std::size_t dict_size) noexcept {
  if (dict_size <= 256) {
    return 1;
  }
  if (dict_size <= 65536) {
    return 2;
  }
  return 4;
}

/// Offset width for values spanning `range`. 8 means more than 4 bytes: the column stays raw.
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
  return 8;
}

}  // namespace PJ::encoding
