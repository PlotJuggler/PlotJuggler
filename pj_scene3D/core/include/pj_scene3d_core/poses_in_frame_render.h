#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <vector>

#include "pj_base/builtin/poses_in_frame.hpp"

namespace pj::scene3d {

// One instanced arrow arm of a pose's coordinate triad. `model` is the
// FRAME-LOCAL transform (pose * arm-rotation * uniform scale) — the fixed-frame
// TF transform is applied later as a render-time uniform, NOT baked here, so TF
// and camera motion cost zero instance re-upload. `color` is RGBA with the
// gizmo opacity in alpha (consumed by the annotation blend path).
struct PoseTriadInstance {
  glm::mat4 model;
  glm::vec4 color;
};

// How a PosesInFrame is expanded into arm instances. Geometry (`x_arrow_only`) and colouring (`override_color`) are
// independent. Kept separate from the message so a knob edit re-expands the same decoded sample cheaply.
struct PoseTriadStyle {
  // Arm length in meters — a uniform scale of the unit arrow, so thickness stays
  // proportional, matching the TF "Frames" gizmos.
  float axis_length = 0.15f;
  // Clamped to [0,1] and written to every arm's color alpha.
  float opacity = 1.0f;
  bool x_arrow_only = false;
  // Replace the per-axis R/G/B with `color`, in BOTH triad and X-only modes, so
  // every arm of every pose shares one user color.
  bool override_color = false;
  // The shared color used iff `override_color` (rgb; alpha comes from `opacity`).
  glm::vec3 color = glm::vec3(0.95f, 0.30f, 0.30f);
};

// Appends the arm instances of one triad rooted at `base` (a frame-local or world model matrix) per `style`. The X/Y/Z
// arm rotations and per-axis colors (X red, Y green, Z blue) are the single source of truth for both the PosesInFrame
// layers and the 3D scene's TF "Frames" gizmos. Pure CPU, no GL.
void appendTriadArms(const glm::mat4& base, const PoseTriadStyle& style, std::vector<PoseTriadInstance>& out);

// Expands a PosesInFrame into arm instances per `style`: 3 arms per pose, or 1 with `x_arrow_only`. Empty in, empty
// out.
[[nodiscard]] std::vector<PoseTriadInstance> buildPoseTriadInstances(
    const PJ::sdk::PosesInFrame& msg, const PoseTriadStyle& style);

}  // namespace pj::scene3d
