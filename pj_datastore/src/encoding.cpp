// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/encoding.hpp"

#include <tsl/robin_map.h>

#include <cstring>

namespace PJ::encoding {

using PJ::Span;

namespace {

// Write an index in the given byte width
void writeIndex(RawBuffer& buf, uint32_t index, uint8_t bytes) {
  switch (bytes) {
    case 1: {
      auto v = static_cast<uint8_t>(index);
      buf.append(&v, sizeof(v));
      break;
    }
    case 2: {
      auto v = static_cast<uint16_t>(index);
      buf.append(&v, sizeof(v));
      break;
    }
    default: {
      buf.append(&index, sizeof(index));
      break;
    }
  }
}

// Read an index at the given byte width
[[nodiscard]] uint32_t readIndex(const uint8_t* data, std::size_t row, uint8_t bytes) {
  switch (bytes) {
    case 1: {
      uint8_t v = 0;
      std::memcpy(&v, data + row, sizeof(v));
      return v;
    }
    case 2: {
      uint16_t v = 0;
      std::memcpy(&v, data + row * 2, sizeof(v));
      return v;
    }
    default: {
      uint32_t v = 0;
      std::memcpy(&v, data + row * 4, sizeof(v));
      return v;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Constant encoding
// ---------------------------------------------------------------------------

ConstantEncoded constantEncode(Span<const uint8_t> data, StorageKind kind, std::size_t count) {
  ConstantEncoded result;
  result.value_kind = kind;
  result.count = count;

  const std::size_t esize = storageKindSize(kind);
  result.value_size = static_cast<uint8_t>(esize);
  if (esize > 0 && count > 0) {
    std::memcpy(result.value_bytes.data(), data.data(), esize);
  }
  return result;
}

double constantDecodeAsDouble(const ConstantEncoded& enc) {
  const uint8_t* ptr = enc.value_bytes.data();

  auto load = [&]<typename T>(const T* /*tag*/) -> double {
    T v{};
    std::memcpy(&v, ptr, sizeof(v));
    return static_cast<double>(v);
  };

  switch (enc.value_kind) {
    case StorageKind::kFloat32:
      return load(static_cast<const float*>(nullptr));
    case StorageKind::kFloat64:
      return load(static_cast<const double*>(nullptr));
    case StorageKind::kInt32:
      return load(static_cast<const int32_t*>(nullptr));
    case StorageKind::kInt64:
      return load(static_cast<const int64_t*>(nullptr));
    case StorageKind::kUint64:
      return load(static_cast<const uint64_t*>(nullptr));
    case StorageKind::kBool:
    case StorageKind::kString:
      break;
  }
  return 0.0;
}

int64_t constantDecodeAsInt64(const ConstantEncoded& enc) {
  const uint8_t* ptr = enc.value_bytes.data();
  switch (enc.value_kind) {
    case StorageKind::kInt32: {
      int32_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<int64_t>(v);
    }
    case StorageKind::kInt64: {
      int64_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return v;
    }
    case StorageKind::kUint64: {
      uint64_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<int64_t>(v);
    }
    case StorageKind::kFloat32: {
      float v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<int64_t>(v);
    }
    case StorageKind::kFloat64: {
      double v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<int64_t>(v);
    }
    default:
      return 0;
  }
}

uint64_t constantDecodeAsUint64(const ConstantEncoded& enc) {
  const uint8_t* ptr = enc.value_bytes.data();
  switch (enc.value_kind) {
    case StorageKind::kUint64: {
      uint64_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return v;
    }
    case StorageKind::kInt32: {
      int32_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<uint64_t>(v);
    }
    case StorageKind::kInt64: {
      int64_t v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<uint64_t>(v);
    }
    case StorageKind::kFloat32: {
      float v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<uint64_t>(v);
    }
    case StorageKind::kFloat64: {
      double v{};
      std::memcpy(&v, ptr, sizeof(v));
      return static_cast<uint64_t>(v);
    }
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Frame of Reference encoding
// Data must be int64_t values.
// ---------------------------------------------------------------------------

FrameOfReferenceEncoded forEncode(
    Span<const uint8_t> data, StorageKind kind, std::size_t count, int64_t min_val, int64_t max_val) {
  FrameOfReferenceEncoded result;
  result.reference = min_val;
  result.count = count;

  const uint64_t range = static_cast<uint64_t>(max_val) - static_cast<uint64_t>(min_val);  // no int64 overflow
  result.offset_bytes = offsetBytesFor(range);

  const std::size_t esize = storageKindSize(kind);
  result.offsets.reserve(count * result.offset_bytes);

  for (std::size_t i = 0; i < count; ++i) {
    int64_t val{};
    if (kind == StorageKind::kInt32) {
      int32_t tmp{};
      std::memcpy(&tmp, data.data() + i * esize, sizeof(tmp));
      val = tmp;  // sign-extend
    } else {
      std::memcpy(&val, data.data() + i * esize, sizeof(val));
    }
    const auto offset = static_cast<uint64_t>(val - min_val);

    switch (result.offset_bytes) {
      case 1: {
        auto v = static_cast<uint8_t>(offset);
        result.offsets.append(&v, sizeof(v));
        break;
      }
      case 2: {
        auto v = static_cast<uint16_t>(offset);
        result.offsets.append(&v, sizeof(v));
        break;
      }
      default: {
        auto v = static_cast<uint32_t>(offset);
        result.offsets.append(&v, sizeof(v));
        break;
      }
    }
  }

  return result;
}

namespace {

uint64_t forReadOffset(const uint8_t* data, std::size_t row, uint8_t offset_bytes) {
  switch (offset_bytes) {
    case 1: {
      uint8_t v = 0;
      std::memcpy(&v, data + row, sizeof(v));
      return v;
    }
    case 2: {
      uint16_t v = 0;
      std::memcpy(&v, data + row * 2, sizeof(v));
      return v;
    }
    default: {
      uint32_t v = 0;
      std::memcpy(&v, data + row * 4, sizeof(v));
      return v;
    }
  }
}

}  // namespace

int64_t forDecodeOneAsInt64(const FrameOfReferenceEncoded& enc, std::size_t row) {
  const uint64_t offset = forReadOffset(enc.offsets.data(), row, enc.offset_bytes);
  return enc.reference + static_cast<int64_t>(offset);
}

double forDecodeOneAsDouble(const FrameOfReferenceEncoded& enc, std::size_t row) {
  // Add in integer arithmetic, then convert once: double(reference) + offset
  // rounds twice and can miss the value above 2^53.
  return static_cast<double>(forDecodeOneAsInt64(enc, row));
}

void forDecodeRangeAsDoubles(const FrameOfReferenceEncoded& enc, Span<double> out, std::size_t row_start) {
  const std::size_t count = out.size();
  // Integer add, one conversion per value (see forDecodeOneAsDouble).
  const int64_t ref = enc.reference;
  const uint8_t* base = enc.offsets.data();

  switch (enc.offset_bytes) {
    case 1: {
      const uint8_t* src = base + row_start;
      for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<double>(ref + static_cast<int64_t>(src[i]));
      }
      break;
    }
    case 2: {
      const uint8_t* src = base + row_start * 2;
      for (std::size_t i = 0; i < count; ++i) {
        uint16_t v{};
        std::memcpy(&v, src + i * 2, sizeof(v));
        out[i] = static_cast<double>(ref + static_cast<int64_t>(v));
      }
      break;
    }
    default: {
      const uint8_t* src = base + row_start * 4;
      for (std::size_t i = 0; i < count; ++i) {
        uint32_t v{};
        std::memcpy(&v, src + i * 4, sizeof(v));
        out[i] = static_cast<double>(ref + static_cast<int64_t>(v));
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Dictionary encoding for strings
// ---------------------------------------------------------------------------

DictionaryEncoded dictionaryEncodeStrings(
    Span<const uint8_t> offsets_data, Span<const uint8_t> values_data, std::size_t row_count) {
  DictionaryEncoded result;
  result.count = row_count;

  if (row_count == 0) {
    result.index_bytes = 1;
    return result;
  }

  // First pass: build dictionary to determine size
  tsl::robin_map<std::string, uint32_t> lookup;
  std::vector<uint32_t> temp_indices;
  temp_indices.reserve(row_count);

  for (std::size_t row = 0; row < row_count; ++row) {
    uint32_t start_offset = 0;
    uint32_t end_offset = 0;
    std::memcpy(&start_offset, offsets_data.data() + row * sizeof(uint32_t), sizeof(uint32_t));
    std::memcpy(&end_offset, offsets_data.data() + (row + 1) * sizeof(uint32_t), sizeof(uint32_t));

    std::string_view str_view(
        reinterpret_cast<const char*>(values_data.data() + start_offset), end_offset - start_offset);

    std::string key_str(str_view);
    auto it = lookup.find(key_str);
    uint32_t index = 0;
    if (it != lookup.end()) {
      index = it->second;
    } else {
      index = static_cast<uint32_t>(result.dictionary.size());
      result.dictionary.push_back(key_str);
      lookup[std::move(key_str)] = index;
    }
    temp_indices.push_back(index);
  }

  // Determine narrowed index width
  result.index_bytes = indexBytesFor(result.dictionary.size());
  result.indices.reserve(row_count * result.index_bytes);

  for (uint32_t idx : temp_indices) {
    writeIndex(result.indices, idx, result.index_bytes);
  }

  return result;
}

std::string_view dictionaryLookup(const DictionaryEncoded& encoded, std::size_t row) {
  uint32_t index = readIndex(encoded.indices.data(), row, encoded.index_bytes);
  if (index >= encoded.dictionary.size()) {
    return {};
  }
  return encoded.dictionary[index];
}

// ---------------------------------------------------------------------------
// Packed bools
// ---------------------------------------------------------------------------

PackedBools packBools(Span<const uint8_t> values) {
  const std::size_t count = values.size();
  PackedBools result;
  result.count = count;

  if (count == 0) {
    return result;
  }

  std::size_t num_bytes = (count + 7) / 8;
  result.bits.resize(num_bytes);

  uint8_t* bit_data = result.bits.mutableData();
  std::memset(bit_data, 0, num_bytes);

  for (std::size_t i = 0; i < count; ++i) {
    if (values[i] != 0) {
      std::size_t byte_idx = i / 8;
      std::size_t bit_idx = i % 8;
      bit_data[byte_idx] |= static_cast<uint8_t>(1u << bit_idx);
    }
  }

  return result;
}

bool unpackBool(const PackedBools& packed, std::size_t index) {
  std::size_t byte_idx = index / 8;
  std::size_t bit_idx = index % 8;
  uint8_t byte_val = 0;
  std::memcpy(&byte_val, packed.bits.data() + byte_idx, sizeof(uint8_t));
  return (byte_val & (1u << bit_idx)) != 0;
}

}  // namespace PJ::encoding
