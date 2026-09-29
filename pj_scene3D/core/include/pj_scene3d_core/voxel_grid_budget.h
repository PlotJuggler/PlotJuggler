// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// Voxel admission against SceneLimits. A scalar layer retains one float per voxel on the
// CPU and one R32F texel on the GPU; RGBA uses the same four bytes on each side.
// The browser's four million voxels therefore cost about 32 MiB across the
// retained pack and volume texture, excluding the zero-copy source payload.

[[nodiscard]] constexpr bool voxelDimensionsFit(
    std::uint64_t columns, std::uint64_t rows, std::uint64_t slices,
    const SceneLimits& limits = kSceneLimits) noexcept {
  if (columns == 0U || rows == 0U || slices == 0U || columns > limits.max_voxels_per_layer) {
    return false;
  }
  if (rows > limits.max_voxels_per_layer / columns) {
    return false;
  }
  const std::uint64_t columns_rows = columns * rows;
  return slices <= limits.max_voxels_per_layer / columns_rows;
}

}  // namespace pj::scene3d
