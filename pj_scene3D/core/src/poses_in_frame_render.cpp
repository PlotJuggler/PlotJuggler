// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/poses_in_frame_render.h"

#include <algorithm>
#include <array>
#include <glm/gtc/matrix_transform.hpp>

#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4

namespace pj::scene3d {

void appendTriadArms(const glm::mat4& base, const PoseTriadStyle& style, std::vector<PoseTriadInstance>& out) {
  // The unit arrow points +X. Rotate +X -> +Y (+90 deg about +Z) and +X -> +Z
  // (-90 deg about +Y) — same convention as the view's frame triads, so pose triads
  // look identical to the TF "Frames" gizmos, with the same desaturated R/G/B
  // per-axis colors. Both are pure viewer conventions, kept here so the
  // expansion stays GPU-free and unit-testable.
  static const std::array<glm::mat4, 3> k_arm = {{
      glm::mat4(1.0f),
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
      glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
  }};
  static const std::array<glm::vec3, 3> k_rgb = {{
      {0.95f, 0.30f, 0.30f},
      {0.30f, 0.85f, 0.30f},
      {0.35f, 0.50f, 1.00f},
  }};

  const float alpha = std::clamp(style.opacity, 0.0f, 1.0f);
  const glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(std::max(style.axis_length, 0.0f)));

  // Geometry: one X-axis arm (kArm[0] is identity) or the full triad. Coloring
  // is orthogonal: override_color recolors every produced arm with the shared
  // color; otherwise each arm keeps its natural per-axis color (so an
  // un-overridden X-only arm is still red).
  const std::size_t arm_count = style.x_arrow_only ? 1 : 3;
  for (std::size_t arm = 0; arm < arm_count; ++arm) {
    const glm::vec3 rgb = style.override_color ? style.color : k_rgb[arm];
    out.push_back({base * k_arm[arm] * scale, glm::vec4(rgb, alpha)});
  }
}

std::vector<PoseTriadInstance> buildPoseTriadInstances(const PJ::sdk::PosesInFrame& msg, const PoseTriadStyle& style) {
  const std::size_t arm_count = style.x_arrow_only ? 1 : 3;
  std::vector<PoseTriadInstance> instances;
  instances.reserve(msg.poses.size() * arm_count);
  for (const PJ::sdk::Pose& pose : msg.poses) {
    appendTriadArms(poseToMat4(pose), style, instances);
  }
  return instances;
}

}  // namespace pj::scene3d
