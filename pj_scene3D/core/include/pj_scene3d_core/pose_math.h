// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>

#include "pj_base/builtin/frame_transforms.hpp"  // sdk::Pose

namespace pj::scene3d {

// True when a wire pose can place geometry: finite position and a finite,
// non-degenerate quaternion (any norm; consumers normalize).
[[nodiscard]] inline bool finitePose(const PJ::sdk::Pose& pose) {
  const auto& p = pose.position;
  const auto& q = pose.orientation;
  const double norm2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(q.x) && std::isfinite(q.y) &&
         std::isfinite(q.z) && std::isfinite(q.w) && std::isfinite(norm2) && norm2 > 1.0e-18;
}

}  // namespace pj::scene3d
