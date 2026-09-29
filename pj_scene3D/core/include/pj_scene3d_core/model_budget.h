// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// Mesh/model admission against SceneLimits: bounds decoded model memory (both
// renderers) and QRhi work.

[[nodiscard]] constexpr bool modelMeshCountsFit(
    std::uint64_t vertices, std::uint64_t indices, std::uint64_t submeshes,
    const SceneLimits& limits = kSceneLimits) noexcept {
  return vertices <= limits.max_model_vertices_per_mesh && indices <= limits.max_model_indices_per_mesh &&
         submeshes <= limits.max_model_submeshes_per_mesh;
}

[[nodiscard]] constexpr bool modelStructureFits(
    std::uint64_t materials, std::uint64_t nodes, std::uint64_t node_depth,
    const SceneLimits& limits = kSceneLimits) noexcept {
  return materials <= limits.max_model_materials_per_mesh && nodes <= limits.max_model_nodes_per_mesh &&
         node_depth <= limits.max_model_node_depth;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> modelRetainedByteCount(
    std::uint64_t vertices, std::uint64_t vertex_stride, std::uint64_t indices, std::uint64_t submeshes,
    std::uint64_t submesh_stride, std::uint64_t encoded_texture_bytes) noexcept {
  std::uint64_t total = encoded_texture_bytes;
  const auto add_product = [&total](std::uint64_t count, std::uint64_t stride) constexpr {
    if (count != 0U && stride > std::numeric_limits<std::uint64_t>::max() / count) {
      return false;
    }
    const std::uint64_t bytes = count * stride;
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
      return false;
    }
    total += bytes;
    return true;
  };
  if (!add_product(vertices, vertex_stride) || !add_product(indices, sizeof(std::uint32_t)) ||
      !add_product(submeshes, submesh_stride) ||
      // The QRhi path retains a six-index edge expansion for every input
      // triangle so wireframe can be toggled without re-importing the mesh.
      !add_product(indices, 2U * sizeof(std::uint32_t))) {
    return std::nullopt;
  }
  return total;
}

// Whether one imported mesh retaining `mesh_bytes` (kSaturated when the estimate
// overflowed) fits both the per-mesh limit and what is left of the layer's
// retained-byte budget after `layer_bytes`.
[[nodiscard]] constexpr bool retainedMeshFits(
    std::uint64_t mesh_bytes, std::uint64_t layer_bytes, const SceneLimits& limits = kSceneLimits) noexcept {
  return mesh_bytes <= limits.max_model_retained_bytes_per_mesh &&
         layer_bytes <= limits.max_model_retained_bytes_per_layer &&
         mesh_bytes <= limits.max_model_retained_bytes_per_layer - layer_bytes;
}

[[nodiscard]] constexpr bool tryConsumeModelTexturePixels(
    std::uint64_t pixels, std::uint64_t& remaining_mesh_pixels, std::uint64_t& remaining_layer_pixels) noexcept {
  if (pixels > remaining_mesh_pixels || pixels > remaining_layer_pixels) {
    return false;
  }
  remaining_mesh_pixels -= pixels;
  remaining_layer_pixels -= pixels;
  return true;
}

[[nodiscard]] constexpr bool tryConsumeModels(
    std::uint64_t draws, std::uint64_t triangles, std::uint64_t& remaining_draws,
    std::uint64_t& remaining_triangles) noexcept {
  if (draws > remaining_draws || triangles > remaining_triangles) {
    return false;
  }
  remaining_draws -= draws;
  remaining_triangles -= triangles;
  return true;
}

}  // namespace pj::scene3d
