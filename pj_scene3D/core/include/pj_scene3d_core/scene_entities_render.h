#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>

namespace pj::scene3d {

// Viewer-side display overrides of one SceneEntities layer (a per-topic
// preference, NOT marker data: the marker protocol has no such fields), applied
// at draw time:
//  - opacity   : multiplies every primitive's alpha (transparency slider).
//  - color_*   : when set, replaces every primitive's rgb with override_color.
//  - wireframe : draws marker primitives as edges. View-only.
struct MarkerDisplayOverrides {
  float opacity = 1.0F;
  bool color_override = false;
  glm::vec4 override_color{1.0F};
  bool wireframe = false;
};

}  // namespace pj::scene3d
