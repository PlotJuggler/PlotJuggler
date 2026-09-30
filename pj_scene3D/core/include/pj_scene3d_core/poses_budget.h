// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {

// Pose admission against SceneLimits. A full pose triad expands to three
// 80-byte PoseTriadInstances, so the browser's 300k arms are about 24 MiB in
// the CPU staging vector and another 24 MiB in the QRhi instance buffer.

[[nodiscard]] constexpr bool poseCountFits(std::uint64_t poses, const SceneLimits& limits = kSceneLimits) noexcept {
  return poses <= limits.max_poses_per_layer;
}

[[nodiscard]] constexpr bool posePayloadFits(
    std::uint64_t wire_bytes, const SceneLimits& limits = kSceneLimits) noexcept {
  return wire_bytes <= limits.max_pose_wire_bytes_per_layer;
}

}  // namespace pj::scene3d
