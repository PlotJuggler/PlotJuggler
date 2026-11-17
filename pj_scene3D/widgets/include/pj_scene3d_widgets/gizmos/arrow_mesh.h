#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <vector>

namespace pj::scene3d {

// Tessellation parameters for a solid arrow pointing along +X in model space
// (tail at the origin).
struct ArrowMeshParams {
  float length = 1.0f;  // origin -> tip (cylinder + cone)
  float shaft_radius = 0.08f;
  float head_length = 0.25f;  // cone axial length (subset of length)
  float head_radius = 0.18f;  // cone base radius
  int segments = 24;          // tessellation around the axis (clamped to >= 3)
};

struct ArrowMeshData {
  std::vector<float> vertices;
  std::vector<std::uint32_t> indices;
};

// Builds the solid arrow mesh (cylinder shaft + cone head + back cap): the single source for every arrow the view draws
// (markers, frame and pose triads). Pure CPU geometry; the per-section normal derivation is documented in the .cpp.
[[nodiscard]] ArrowMeshData buildArrowMesh(const ArrowMeshParams& params);

}  // namespace pj::scene3d
