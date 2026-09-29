#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <glm/glm.hpp>

#include "pj_scene3d_widgets/layer_style.h"

namespace pj::scene3d {

// Point-cloud shading inputs the point pass feeds its shaders.

// Sphere perspective falloff softens beyond this view depth (metres).
inline constexpr float kSphereDepthThreshold = 5.0f;
// Physical point-size clamps (px); callers scale them by the device pixel ratio.
inline constexpr float kMinPointPx = 0.5f;
inline constexpr float kMaxPointPx = 32.0f;

// The shaders' colour-mode selector: 0 = colormap-from-scalar, 1 = solid, 2 = per-point rgb.
[[nodiscard]] inline std::int32_t pointColorMode(PointColorType type) {
  switch (type) {
    case PointColorType::kSolid:
      return 1;
    case PointColorType::kRgb:
      return 2;
    case PointColorType::kField:
      break;
  }
  return 0;
}

// What the cube shaders' face-pick reference carries, in homogeneous form: the eye
// POSITION in render space (w = 1) for a perspective camera, or the scene->camera
// DIRECTION as a point at infinity (w = 0) for an orthographic one, where which faces
// a cube shows depends on the view direction and not on where along it the eye sits.
// projection[3][3] is 0 only for perspective; it must be read off the PROJECTION:
// folding view into it leaves the eye's view-axis distance in that slot, not zero.
[[nodiscard]] inline glm::vec4 facePickReference(const glm::mat4& view, const glm::mat4& projection) {
  // The view rotation is rigid, so its inverse is its transpose.
  const glm::mat3 inverse_rotation = glm::transpose(glm::mat3(view));
  if (projection[3][3] == 0.0f) {
    return glm::vec4(-inverse_rotation * glm::vec3(view[3]), 1.0f);
  }
  // View space looks down -Z, so +Z points back at the camera.
  return glm::vec4(inverse_rotation * glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
}

}  // namespace pj::scene3d
