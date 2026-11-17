// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>

#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// A world-space ray: origin on the near plane, dir normalized toward far.
struct Ray {
  glm::vec3 origin{0.0f};
  glm::vec3 dir{0.0f, 0.0f, -1.0f};
};

// Express a world position relative to the camera-relative RENDER ORIGIN, subtracting in DOUBLE before the float
// downcast: that ordering keeps the small eye→geometry delta when both sit at large coordinates (~1e6, where float32
// has ~0.1 m resolution), which a `vec3 - vec3` would lose to cancellation. With origin {0,0,0} it is just the float of
// `world_pos`.
[[nodiscard]] inline glm::vec3 toRenderSpace(const glm::vec3& world_pos, const glm::dvec3& origin) {
  return glm::vec3(glm::dvec3(world_pos) - origin);
}

// How far the visible scene extends from the eye, used to size the far plane and
// the zoom-out limit: the scene's own diagonal plus the focal's offset from the
// scene center (so framing stays correct even when the pivot is off to one side).
// Returns 0 for an invalid (unknown) AABB → callers fall back to working distance.
[[nodiscard]] float sceneReach(const AABB& bounds, const glm::vec3& focal);

// Decoupled adaptive near/far for a perspective camera. `working_distance` is the eye-to-focal distance (orbit
// radius); `scene_reach` is sceneReach() (0 = unknown).
//   near_plane = max(working_distance * 1e-2, 1e-3): working distance ONLY, so close inspection never clips
//   far_plane  = max(working_distance * 4, scene_reach) * 1.5
//   near_plane = max(near_plane, far_plane / 1e5): ratio cap bounding depth precision
// Writes out-params (testable without constructing a projection).
void adaptiveNearFar(float working_distance, float scene_reach, float& near_plane, float& far_plane);

// Builds the world-space ray through a pixel. `cursor_px` is in Qt widget coordinates (origin top-left, y down); the
// y-flip into NDC is done here. Returns a default ray (origin 0, dir -Z) for a degenerate viewport or non-invertible
// transform. Works for perspective and orthographic `proj`.
[[nodiscard]] Ray unprojectRay(
    glm::vec2 cursor_px, int viewport_w, int viewport_h, const glm::mat4& view, const glm::mat4& proj);

// Intersect a ray (origin `ro`, direction `rd`) with the infinite plane through
// point `p` with normal `n`. Returns false when the ray is parallel to the plane;
// otherwise sets `t` to the signed ray parameter (hit = ro + t*rd) and returns
// true. Callers that need a forward hit should check `t > 0`.
[[nodiscard]] bool rayPlane(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& p, const glm::vec3& n, float& t);

}  // namespace pj::scene3d
