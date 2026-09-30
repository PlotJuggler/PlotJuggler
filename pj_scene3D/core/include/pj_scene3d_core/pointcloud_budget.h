// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// Point-cloud admission against SceneLimits. Kept in the GL/Qt-free core so
// the exact boundary and aggregate subtraction are unit-testable.

[[nodiscard]] constexpr bool pointCountFits(
    std::uint64_t declared_points, const SceneLimits& limits = kSceneLimits) noexcept {
  return declared_points <= limits.max_points_per_cloud;
}

[[nodiscard]] constexpr bool pointPayloadFits(
    std::uint64_t wire_bytes, const SceneLimits& limits = kSceneLimits) noexcept {
  return wire_bytes <= limits.max_point_wire_bytes_per_cloud;
}

// DepthCloud contributes one candidate point per depth pixel and shares the
// point renderer's retained/view budgets. Widen before multiplication so
// hostile uint32 dimensions cannot wrap a smaller accepted count.
[[nodiscard]] constexpr bool depthDimensionsFit(
    std::uint32_t width, std::uint32_t height, std::uint64_t& pixel_count,
    const SceneLimits& limits = kSceneLimits) noexcept {
  pixel_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  return width != 0U && height != 0U && pointCountFits(pixel_count, limits);
}

}  // namespace pj::scene3d
