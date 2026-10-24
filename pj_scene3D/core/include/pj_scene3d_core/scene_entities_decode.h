#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>

#include "pj_base/builtin/scene_entities.hpp"

namespace pj::scene3d {

// Shared canonical->glm converters: the single source of truth for the trivial
// Pose/Point3/Vector3/ColorRGBA narrowings the core and the widgets need. No Qt,
// no GPU — pure geometry, safe in the core.

// Compose a primitive's LOCAL model matrix from its pose: T * R, so the rotation
// acts about the pose's own origin and is then offset to the position (the correct
// rigid-body-pose semantics; R * T would orbit the frame origin). The rotation
// stays a quaternion all the way to the matrix (no Euler round-trip, no gimbal
// lock). Two gotchas handled here: the canonical sdk::Quaternion is {x, y, z, w}
// but glm::quat takes (w, x, y, z); and we normalize defensively in case a producer
// emits a non-unit quaternion (mat4_cast assumes unit length).
[[nodiscard]] glm::mat4 poseToMat4(const PJ::sdk::Pose& pose);

[[nodiscard]] glm::vec3 toVec3(const PJ::sdk::Point3& point);
[[nodiscard]] glm::vec3 toVec3(const PJ::sdk::Vector3& vec);

// ColorRGBA channels are 0..255; the result is the conventional 0..1 float color.
[[nodiscard]] glm::vec4 toVec4(const PJ::sdk::ColorRGBA& color);

}  // namespace pj::scene3d
