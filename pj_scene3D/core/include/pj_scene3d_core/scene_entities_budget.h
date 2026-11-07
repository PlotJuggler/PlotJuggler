// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// SceneEntities admission against SceneLimits. Procedural solids stay instanced, while
// line and triangle primitives retain double-precision vertices until the view
// has subtracted its render origin. The two independent view budgets prevent a
// marker-heavy topic from allocating unbounded CPU staging and QRhi buffers.

[[nodiscard]] constexpr bool markerLayerCountsFit(
    std::uint64_t instances, std::uint64_t stream_vertices, std::uint64_t frames,
    const SceneLimits& limits = kSceneLimits) noexcept {
  return instances <= limits.max_marker_instances_per_layer &&
         stream_vertices <= limits.max_marker_stream_vertices_per_layer && frames <= limits.max_marker_frames_per_layer;
}

// Solid cubes submit one fill instance. Normal (non-wireframe) rendering also
// submits one instance for the twelve-edge overlay, whereas explicit wireframe
// uses only the original cube instance. Keep this renderer expansion inside the
// same per-layer and per-view budgets that bound the staging buffer.
[[nodiscard]] constexpr std::optional<std::uint64_t> markerSubmittedInstanceCount(
    std::uint64_t logical_instances, std::uint64_t cube_instances, bool wireframe) noexcept {
  if (wireframe) {
    return logical_instances;
  }
  if (cube_instances > std::numeric_limits<std::uint64_t>::max() - logical_instances) {
    return std::nullopt;
  }
  return logical_instances + cube_instances;
}

}  // namespace pj::scene3d
