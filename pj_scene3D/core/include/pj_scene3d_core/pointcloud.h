#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace pj::scene3d {

// Pure render struct: the decoded geometry a render pass consumes. Field-layout
// metadata (name/offset/datatype) belongs to the canonical sdk::PointCloud and is
// read straight from it by the decode adapter, so nothing wire-shaped lives here.
struct DecodedPointCloud {
  std::string frame_id;
  std::vector<glm::vec3> positions;  // one entry per point
  std::vector<float> scalar;         // colorize-by-field values; empty when no field is selected
  std::string scalar_field_name;     // canonical field `scalar` was extracted from
  // Packed per-point color for RGB-direct mode: byte0=R, byte1=G, byte2=B, byte3=A
  // (matches the canonical 'rgba' field's little-endian layout). Empty unless the
  // cloud was decoded with RGB extraction requested AND it carries a color field.
  std::vector<uint32_t> rgba;
};

// How the QRhi renderer's retained points are laid out (the point pass's
// instance formats). Every format decodes to one (x, y, z, scalar, colour):
// what a format does not store reads as scalar 0 and opaque white.
enum class PointVertexFormat : uint8_t {
  kXyz,        // 12 B: x, y, z
  kXyzScalar,  // 16 B: x, y, z, the colormap scalar (float bits)
  kXyzRgba,    // 16 B: x, y, z, packed colour: byte0=R, byte1=G, byte2=B, byte3=A
};

[[nodiscard]] constexpr std::size_t pointVertexStride(PointVertexFormat format) {
  return format == PointVertexFormat::kXyz ? 12 : 16;
}

// One point in the 16-byte formats; kXyz stores only its first 12 bytes.
struct PointVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  uint32_t attribute = 0;  // the scalar's bits or the packed colour, per PointVertexFormat
};
static_assert(sizeof(PointVertex) == 16);

}  // namespace pj::scene3d
