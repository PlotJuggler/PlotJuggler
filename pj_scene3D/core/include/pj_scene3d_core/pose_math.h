// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>

#include "pj_base/builtin/frame_transforms.hpp"  // sdk::Pose
#include "pj_tf/transform.h"

namespace pj::scene3d {

using pj::tf::Transform;

// True when a wire pose can place geometry: finite position and a finite,
// non-degenerate quaternion (any norm; consumers normalize).
[[nodiscard]] inline bool finitePose(const PJ::sdk::Pose& pose) {
  const auto& p = pose.position;
  const auto& q = pose.orientation;
  const double norm2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(q.x) && std::isfinite(q.y) &&
         std::isfinite(q.z) && std::isfinite(q.w) && std::isfinite(norm2) && norm2 > 1.0e-18;
}

[[nodiscard]] inline bool finite(const glm::dmat4& matrix) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

// The pose as a rigid matrix, its quaternion normalized; nullopt for a
// non-finite position, a non-finite or near-zero quaternion, or a non-finite result.
[[nodiscard]] inline std::optional<glm::dmat4> poseMatrix(const PJ::sdk::Pose& pose) {
  const glm::dvec3 translation{pose.position.x, pose.position.y, pose.position.z};
  const glm::dquat orientation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
  const double norm = glm::length(orientation);
  if (!std::isfinite(translation.x) || !std::isfinite(translation.y) || !std::isfinite(translation.z) ||
      !std::isfinite(norm) || norm < 1e-15) {
    return std::nullopt;
  }
  const glm::dmat4 matrix = glm::translate(glm::dmat4(1.0), translation) * glm::mat4_cast(orientation / norm);
  return finite(matrix) ? std::optional<glm::dmat4>{matrix} : std::nullopt;
}

// The pose as a Transform, its quaternion normalized. Unchecked: validate a wire
// pose with finitePose() first.
[[nodiscard]] inline Transform poseTransform(const PJ::sdk::Pose& pose) {
  const auto& p = pose.position;
  const auto& q = pose.orientation;
  return {glm::dvec3(p.x, p.y, p.z), glm::normalize(glm::dquat(q.w, q.x, q.y, q.z))};
}

}  // namespace pj::scene3d
