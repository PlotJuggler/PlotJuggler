// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// Occupancy-grid admission against SceneLimits. At the per-layer boundary the
// reconstructor retains the live grid, a pristine base, and at most
// occupancy_snapshot_budget_bytes of snapshots; QRhi retains one R8 texel per
// cell.

[[nodiscard]] constexpr bool occupancyDimensionsFit(
    std::uint64_t width, std::uint64_t height, const SceneLimits& limits = kSceneLimits) noexcept {
  return width > 0U && height > 0U && width <= limits.max_occupancy_texture_dimension &&
         height <= limits.max_occupancy_texture_dimension && width * height <= limits.max_occupancy_cells_per_layer;
}

[[nodiscard]] constexpr bool occupancyPayloadFits(
    std::uint64_t wire_bytes, const SceneLimits& limits = kSceneLimits) noexcept {
  return wire_bytes <= limits.max_occupancy_wire_bytes;
}

[[nodiscard]] constexpr bool occupancyUpdateWindowFits(
    std::uint64_t decoded_bytes, const SceneLimits& limits = kSceneLimits) noexcept {
  return decoded_bytes <= limits.max_occupancy_update_window_bytes;
}

}  // namespace pj::scene3d
