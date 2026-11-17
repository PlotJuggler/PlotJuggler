// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pj::scene3d {

ArrowMeshData buildArrowMesh(const ArrowMeshParams& params) {
  ArrowMeshData mesh;
  std::vector<float>& vertex_data = mesh.vertices;
  std::vector<std::uint32_t>& index_data = mesh.indices;

  const int segments = std::max(params.segments, 3);
  const float length = std::max(params.length, 0.0f);
  const float head_length = std::clamp(params.head_length, 0.0f, length);
  const float shaft_length = length - head_length;
  const float shaft_radius = std::max(params.shaft_radius, 0.0f);
  const float head_radius = std::max(params.head_radius, 0.0f);
  const float two_pi = 2.0f * std::numbers::pi_v<float>;

  auto push_vertex = [&](float px, float py, float pz, float nx, float ny, float nz) {
    vertex_data.insert(vertex_data.end(), {px, py, pz, nx, ny, nz});
  };

  // Two rings of `segments` vertices: x=0 (back) and x=shaft_length (front).
  // Per-vertex radial normals -> smooth shading around the cylinder.
  const uint32_t shaft_back = static_cast<uint32_t>(vertex_data.size() / 6);
  for (int ring = 0; ring < 2; ++ring) {
    const float x = (ring == 0) ? 0.0f : shaft_length;
    for (int i = 0; i < segments; ++i) {
      const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
      const float c = std::cos(theta);
      const float s = std::sin(theta);
      push_vertex(x, shaft_radius * c, shaft_radius * s, 0.0f, c, s);
    }
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    const uint32_t current_index = static_cast<uint32_t>(i);
    const uint32_t next_index = static_cast<uint32_t>(next);
    const uint32_t segment_count = static_cast<uint32_t>(segments);
    const uint32_t a = shaft_back + current_index;
    const uint32_t b = shaft_back + next_index;
    const uint32_t c = shaft_back + segment_count + next_index;
    const uint32_t d = shaft_back + segment_count + current_index;
    index_data.insert(index_data.end(), {a, b, c, a, c, d});
  }

  // Cone base disc (faces -X, between shaft and head): closes the gap when head_radius > shaft_radius.
  const uint32_t disc_center = static_cast<uint32_t>(vertex_data.size() / 6);
  push_vertex(shaft_length, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    push_vertex(shaft_length, head_radius * std::cos(theta), head_radius * std::sin(theta), -1.0f, 0.0f, 0.0f);
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data.insert(
        index_data.end(),
        {disc_center, disc_center + 1U + static_cast<uint32_t>(next), disc_center + 1U + static_cast<uint32_t>(i)});
  }

  // Normal at a base-ring point: (head_radius, head_length*cos, head_length*sin) / sqrt(R^2 + H^2)
  // (derived from circumferential × axial cross product).
  const float norm_len = std::sqrt(head_radius * head_radius + head_length * head_length);
  const float nx_cone = (norm_len > 0.0f) ? head_radius / norm_len : 1.0f;
  const uint32_t cone_ring = static_cast<uint32_t>(vertex_data.size() / 6);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    const float ny = (norm_len > 0.0f) ? head_length * c / norm_len : 0.0f;
    const float nz = (norm_len > 0.0f) ? head_length * s / norm_len : 0.0f;
    push_vertex(shaft_length, head_radius * c, head_radius * s, nx_cone, ny, nz);
  }
  const uint32_t cone_tip = static_cast<uint32_t>(vertex_data.size() / 6);
  push_vertex(length, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data.insert(
        index_data.end(), {cone_ring + static_cast<uint32_t>(i), cone_ring + static_cast<uint32_t>(next), cone_tip});
  }

  // Back cap on the shaft (faces -X): cosmetic, closes the arrow seen end-on. Wound CCW from -X (outside) so backface
  // culling keeps it visible.
  const uint32_t back_center = static_cast<uint32_t>(vertex_data.size() / 6);
  push_vertex(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
  for (int i = 0; i < segments; ++i) {
    const float theta = two_pi * static_cast<float>(i) / static_cast<float>(segments);
    push_vertex(0.0f, shaft_radius * std::cos(theta), shaft_radius * std::sin(theta), -1.0f, 0.0f, 0.0f);
  }
  for (int i = 0; i < segments; ++i) {
    const int next = (i + 1) % segments;
    index_data.insert(
        index_data.end(),
        {back_center, back_center + 1U + static_cast<uint32_t>(next), back_center + 1U + static_cast<uint32_t>(i)});
  }

  return mesh;
}

}  // namespace pj::scene3d
