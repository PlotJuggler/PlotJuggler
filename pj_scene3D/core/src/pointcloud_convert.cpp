// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/pointcloud_convert.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace pj::scene3d {

using PJ::sdk::bytesPerElement;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;

float readFloat32At(const uint8_t* data) {
  const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return std::bit_cast<float>(bits);
}

// Hot-path float read for the packing loop. On a little-endian host (every PJ target) a memcpy
// is a single (possibly unaligned) load the compiler vectorizes — ~10x cheaper than
// readFloat32At's byte-by-byte assembly — and stays alignment-safe / UB-free (no
// reinterpret_cast). The explicit byte assembly is kept only for a hypothetical big-endian host.
namespace {
inline float readFloat32Host(const uint8_t* data) {
  if constexpr (std::endian::native == std::endian::little) {
    float value;
    std::memcpy(&value, data, sizeof(value));
    return value;
  } else {
    return readFloat32At(data);
  }
}
}  // namespace

double readFloat64At(const uint8_t* data) {
  uint64_t bits = 0;
  for (int byte_index = 0; byte_index < 8; ++byte_index) {
    bits |= static_cast<uint64_t>(data[byte_index]) << (8 * byte_index);
  }
  return std::bit_cast<double>(bits);
}

float readScalarAt(const uint8_t* data, PointField::Datatype datatype) {
  using DT = PointField::Datatype;
  switch (datatype) {
    case DT::kInt8:
      return static_cast<float>(std::bit_cast<int8_t>(*data));
    case DT::kUint8:
      return static_cast<float>(*data);
    case DT::kInt16: {
      const uint16_t bits = static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
      return static_cast<float>(std::bit_cast<int16_t>(bits));
    }
    case DT::kUint16:
      return static_cast<float>(
          static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8));
    case DT::kInt32: {
      const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      return static_cast<float>(std::bit_cast<int32_t>(bits));
    }
    case DT::kUint32: {
      const uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      return static_cast<float>(bits);
    }
    case DT::kFloat32:
      return readFloat32At(data);
    case DT::kFloat64:
      return static_cast<float>(readFloat64At(data));
    case DT::kUnknown:
    default:
      return 0.0f;
  }
}

const PointField* findField(const std::vector<PointField>& fields, std::string_view name) {
  const auto it = std::find_if(
      fields.begin(), fields.end(), [name](const PointField& field) { return std::string_view(field.name) == name; });
  return it == fields.end() ? nullptr : &*it;
}

namespace {

// True when reading `field` stays inside one point: offset + width*count <= point_step.
// Computed in uint64 so the offset+size sum cannot overflow uint32 for an adversarial
// header. A zero-width datatype (kUnknown) fails this (0-byte element is never readable).
bool fieldFitsInPoint(const PointField& field, uint32_t point_step) {
  const uint64_t width = static_cast<uint64_t>(bytesPerElement(field.datatype));
  if (width == 0) {
    return false;
  }
  const uint64_t field_end =
      static_cast<uint64_t>(field.offset) + width * static_cast<uint64_t>(std::max<uint32_t>(field.count, 1u));
  return field_end <= static_cast<uint64_t>(point_step);
}

}  // namespace

ColorLayout detectColorLayout(const PointCloud& src) {
  ColorLayout layout;
  // 1. Canonical packed color: a single uint32 field named "rgba" (with alpha) or
  //    "rgb" (alpha forced opaque). Its 4 bytes are R,G,B,A in increasing address.
  for (const std::string_view name : {std::string_view("rgba"), std::string_view("rgb")}) {
    const PointField* packed = findField(src.fields, name);
    if (packed != nullptr && packed->datatype == PointField::Datatype::kUint32) {
      layout.valid = true;
      layout.r_offset = packed->offset;
      layout.g_offset = packed->offset + 1;
      layout.b_offset = packed->offset + 2;
      layout.has_alpha = (name == "rgba");
      layout.a_offset = packed->offset + 3;
      return layout;
    }
  }
  // 2. Separate uint8 channels (the raw, un-normalized foxglove layout).
  const PointField* red = findField(src.fields, "red");
  const PointField* green = findField(src.fields, "green");
  const PointField* blue = findField(src.fields, "blue");
  const PointField* alpha = findField(src.fields, "alpha");
  const auto is_u8 = [](const PointField* field) {
    return field != nullptr && field->datatype == PointField::Datatype::kUint8;
  };
  if (is_u8(red) && is_u8(green) && is_u8(blue)) {
    layout.valid = true;
    layout.r_offset = red->offset;
    layout.g_offset = green->offset;
    layout.b_offset = blue->offset;
    layout.has_alpha = is_u8(alpha);
    layout.a_offset = layout.has_alpha ? alpha->offset : 0;
    return layout;
  }
  return layout;  // no recognizable color field
}

std::optional<PointVertexPlan> planPointVertices(
    const PointCloud& src, std::string_view scalar_field, bool extract_rgba, bool store_scalar) {
  if (src.is_bigendian) {
    return std::nullopt;  // big-endian assembly unsupported; drop
  }
  PointVertexPlan plan;
  plan.point_count = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height);
  plan.step = src.point_step;
  if (plan.point_count == 0 || plan.step == 0 || src.data.empty()) {
    return std::nullopt;
  }
  if (src.data.size() < plan.point_count * plan.step) {
    return std::nullopt;  // declared geometry exceeds the backing buffer
  }
  plan.x = findField(src.fields, "x");
  plan.y = findField(src.fields, "y");
  plan.z = findField(src.fields, "z");
  if (plan.x == nullptr || plan.y == nullptr || plan.z == nullptr) {
    return std::nullopt;  // no spatial fields
  }
  // M.23/M.24: validate field offsets against point_step BEFORE indexing the buffer.
  // Field offsets come from the wire/parser verbatim; an offset whose read crosses
  // point_step is an out-of-bounds heap read on the tail point (and far worse for an
  // adversarial uint32 offset). Reject every field we actually read here.
  if (!fieldFitsInPoint(*plan.x, src.point_step) || !fieldFitsInPoint(*plan.y, src.point_step) ||
      !fieldFitsInPoint(*plan.z, src.point_step)) {
    return std::nullopt;
  }
  // RGB-direct mode (extract_rgba) takes precedence over scalar extraction: it reads
  // the cloud's color field instead of a colormap scalar. Validate every color byte we
  // will read against point_step, the same OOB guard applied to x/y/z above.
  if (extract_rgba) {
    plan.color = detectColorLayout(src);
    if (plan.color.valid) {
      const auto byte_fits = [&](uint32_t offset) {
        return static_cast<uint64_t>(offset) + 1u <= static_cast<uint64_t>(src.point_step);
      };
      if (!byte_fits(plan.color.r_offset) || !byte_fits(plan.color.g_offset) || !byte_fits(plan.color.b_offset) ||
          (plan.color.has_alpha && !byte_fits(plan.color.a_offset))) {
        return std::nullopt;  // a malformed color offset is the same OOB hazard as x/y/z
      }
    }
  } else if (!scalar_field.empty()) {
    plan.scalar = findField(src.fields, scalar_field);
    if (plan.scalar != nullptr && !fieldFitsInPoint(*plan.scalar, src.point_step)) {
      return std::nullopt;  // a malformed scalar offset is the same OOB hazard as x/y/z
    }
  }
  if (plan.color.valid) {
    plan.format = PointVertexFormat::kXyzRgba;
  } else if (store_scalar && plan.scalar != nullptr) {
    plan.format = PointVertexFormat::kXyzScalar;
  }
  return plan;
}

namespace {

float readCoord(const PointField* field, const uint8_t* base) {
  if (field->datatype == PointField::Datatype::kFloat32) {
    return readFloat32At(base + field->offset);
  }
  if (field->datatype == PointField::Datatype::kFloat64) {
    return static_cast<float>(readFloat64At(base + field->offset));
  }
  return 0.0f;
}

uint32_t readRgba(const ColorLayout& color, const uint8_t* base) {
  const uint32_t r = base[color.r_offset];
  const uint32_t g = base[color.g_offset];
  const uint32_t b = base[color.b_offset];
  const uint32_t a = color.has_alpha ? base[color.a_offset] : 255u;
  return r | (g << 8) | (b << 16) | (a << 24);
}

// The packing loop, specialized on what each point reads: float32 x/y/z read as host
// loads (every other coordinate type through readCoord), and the optional scalar and
// colour. Bounds and scalar range accumulate in locals so the loop stays in registers.
template <bool kFloat32Xyz, PointVertexFormat kFormat, bool kReadScalar>
PackedPointVertices packLoop(
    const PointCloud& src, const PointVertexPlan& plan, std::size_t first, std::size_t last, std::byte* out) {
  constexpr std::size_t kStride = pointVertexStride(kFormat);
  const uint32_t x_offset = plan.x->offset;
  const uint32_t y_offset = plan.y->offset;
  const uint32_t z_offset = plan.z->offset;
  const PointField::Datatype scalar_type = kReadScalar ? plan.scalar->datatype : PointField::Datatype::kUnknown;
  const uint32_t scalar_offset = kReadScalar ? plan.scalar->offset : 0U;
  glm::vec3 lo(std::numeric_limits<float>::max());
  glm::vec3 hi(std::numeric_limits<float>::lowest());
  float scalar_lo = std::numeric_limits<float>::max();
  float scalar_hi = std::numeric_limits<float>::lowest();
  bool any_scalar = false;
  std::byte* write = out;
  const uint8_t* base = src.data.data() + first * plan.step;
  for (std::size_t index = first; index < last; ++index, base += plan.step) {
    PointVertex vertex;
    if constexpr (kFloat32Xyz) {
      vertex.x = readFloat32Host(base + x_offset);
      vertex.y = readFloat32Host(base + y_offset);
      vertex.z = readFloat32Host(base + z_offset);
    } else {
      vertex.x = readCoord(plan.x, base);
      vertex.y = readCoord(plan.y, base);
      vertex.z = readCoord(plan.z, base);
    }
    if constexpr (kReadScalar) {
      const float scalar = readScalarAt(base + scalar_offset, scalar_type);
      if (std::isfinite(scalar)) {
        scalar_lo = std::min(scalar_lo, scalar);
        scalar_hi = std::max(scalar_hi, scalar);
        any_scalar = true;
      }
      if constexpr (kFormat == PointVertexFormat::kXyzScalar) {
        vertex.attribute = std::bit_cast<uint32_t>(scalar);
      }
    }
    if constexpr (kFormat == PointVertexFormat::kXyzRgba) {
      vertex.attribute = readRgba(plan.color, base);
    }
    if (std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z)) {
      const glm::vec3 position{vertex.x, vertex.y, vertex.z};
      lo = glm::min(lo, position);
      hi = glm::max(hi, position);
      std::memcpy(write, &vertex, kStride);
      write += kStride;
    }
  }
  PackedPointVertices result;
  result.format = kFormat;
  result.count = static_cast<std::size_t>(write - out) / kStride;
  if (result.count > 0) {
    result.bounds = AABB{lo, hi, true};
  }
  if (any_scalar) {
    result.scalar_range = std::pair<float, float>{scalar_lo, scalar_hi};
  }
  return result;
}

using PackFn = PackedPointVertices (*)(const PointCloud&, const PointVertexPlan&, std::size_t, std::size_t, std::byte*);

template <bool kFloat32Xyz>
PackFn packLoopFor(const PointVertexPlan& plan) {
  switch (plan.format) {
    case PointVertexFormat::kXyzRgba:
      return &packLoop<kFloat32Xyz, PointVertexFormat::kXyzRgba, false>;
    case PointVertexFormat::kXyzScalar:
      return &packLoop<kFloat32Xyz, PointVertexFormat::kXyzScalar, true>;
    case PointVertexFormat::kXyz:
      break;
  }
  return plan.scalar != nullptr ? &packLoop<kFloat32Xyz, PointVertexFormat::kXyz, true>
                                : &packLoop<kFloat32Xyz, PointVertexFormat::kXyz, false>;
}

// Appends `part` (the next points in wire order) to `total`. min/max keep the first of
// equal values either way, so the merged bounds and range are the serial pass's bits.
void mergePacked(PackedPointVertices& total, const PackedPointVertices& part) {
  total.count += part.count;
  total.bounds = unionAABB(total.bounds, part.bounds);
  if (part.scalar_range) {
    total.scalar_range = total.scalar_range
                             ? std::pair<float, float>{std::min(total.scalar_range->first, part.scalar_range->first),
                                                       std::max(total.scalar_range->second, part.scalar_range->second)}
                             : part.scalar_range;
  }
}

}  // namespace

PackedPointVertices packPointVertices(
    const PointCloud& src, const PointVertexPlan& plan, std::span<std::byte> out, const ParallelFor& parallel_for,
    std::size_t max_chunks) {
  PackedPointVertices total;
  total.format = plan.format;
  if (out.size() < plan.maxBytes()) {
    return total;
  }
  const std::size_t stride = pointVertexStride(plan.format);
  const auto is_float32 = [](const PointField* field) { return field->datatype == PointField::Datatype::kFloat32; };
  const PackFn pack = is_float32(plan.x) && is_float32(plan.y) && is_float32(plan.z) ? packLoopFor<true>(plan)
                                                                                     : packLoopFor<false>(plan);
  const std::size_t chunks =
      parallel_for
          ? std::min(std::max<std::size_t>(max_chunks, 1), (plan.point_count + kPackChunkPoints - 1) / kPackChunkPoints)
          : 1;
  if (chunks <= 1) {
    return pack(src, plan, 0, plan.point_count, out.data());
  }
  // Each chunk packs its points at its own start in `out` (its finite points never
  // outnumber its slots), then the chunks close the gaps left by dropped points.
  const std::size_t per_chunk = (plan.point_count + chunks - 1) / chunks;
  std::vector<PackedPointVertices> parts(chunks);
  parallel_for(chunks, [&](std::size_t chunk) {
    const std::size_t first = chunk * per_chunk;
    const std::size_t last = std::min(plan.point_count, first + per_chunk);
    parts[chunk] = first < last ? pack(src, plan, first, last, out.data() + first * stride) : PackedPointVertices{};
  });
  for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
    const std::size_t first = chunk * per_chunk;
    if (total.count != first && parts[chunk].count > 0) {
      std::memmove(out.data() + total.count * stride, out.data() + first * stride, parts[chunk].count * stride);
    }
    mergePacked(total, parts[chunk]);
  }
  return total;
}

ConvertedPointCloud convertCanonical(const PointCloud& src, std::string_view scalar_field, bool extract_rgba) {
  ConvertedPointCloud out;
  out.cloud.frame_id = src.frame_id;
  const std::optional<PointVertexPlan> plan = planPointVertices(src, scalar_field, extract_rgba, true);
  if (!plan.has_value()) {
    return out;
  }
  const bool want_scalar = plan->scalar != nullptr;
  const bool want_rgba = plan->color.valid;
  out.cloud.positions.reserve(plan->point_count);
  if (want_scalar) {
    out.cloud.scalar.reserve(plan->point_count);
    out.cloud.scalar_field_name = std::string(scalar_field);
  }
  if (want_rgba) {
    out.cloud.rgba.reserve(plan->point_count);
  }

  // Single pass over the point buffer: positions, optional scalar, and the finite-point
  // AABB all read from the same `base` so large clouds touch each point once (L.50).
  for (std::size_t point_index = 0; point_index < plan->point_count; ++point_index) {
    const uint8_t* base = src.data.data() + point_index * plan->step;
    const glm::vec3 position{readCoord(plan->x, base), readCoord(plan->y, base), readCoord(plan->z, base)};
    out.cloud.positions.push_back(position);
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
      expandAABB(out.bounds, position);
    }
    if (want_scalar) {
      out.cloud.scalar.push_back(readScalarAt(base + plan->scalar->offset, plan->scalar->datatype));
    }
    if (want_rgba) {
      out.cloud.rgba.push_back(readRgba(plan->color, base));
    }
  }
  return out;
}

}  // namespace pj::scene3d
