// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>
#include <optional>

#include "pj_scene3d_core/camera/camera.h"

namespace pj::scene3d {

// Default cube LOD threshold, in device pixels, for point-cloud cubes.
// One pixel is the conservative choice on both counts: cosmetically a cube keeps its
// real geometry until it is genuinely sub-pixel, and a threshold sweep on the benchmark
// showed the speedup is already at its maximum by 1 px (it comes from dropping the fan
// draw for the whole cloud, not from how many cubes convert), so raising it buys nothing.
inline constexpr float kDefaultCubeLodThresholdPx = 1.0f;

// How to draw a cloud of axis-aligned cubes this frame.
//
// Two passes render such a cloud — the point cloud (instance data from a VBO) and the
// voxel grid (derived from gl_InstanceID plus a 3D texture) — and that difference is the
// ONLY thing separating them. Which geometry to use, and whether to draw at all, is the
// same question for both, so it is answered here once instead of twice.
//
// Qt-free and GL-free on purpose: this is the part worth unit-testing exhaustively, and
// it should not need a context to do it.
enum class CubeDrawMode {
  kSkip,    // nothing in the cloud can reach the screen
  kSprite,  // every cube is sub-pixel: one matched point sprite each
  kFan,     // the 3 camera-facing faces, 7 attributeless vertices
  kSolid,   // the full 24-vertex cube — needed when the camera can be inside one
};

// What the calling pass is actually able to draw. chooseCubeDrawMode() picks the ideal
// mode and then degrades to the best available, so a pass that has not implemented a
// geometry yet still gets every decision that does not depend on it.
struct CubeDrawCapabilities {
  bool sprite{false};
  bool fan{false};
  bool solid{false};
};

// Everything the decision depends on, in the cloud's OWN frame.
struct CubeCloudView {
  // Bounds of the instance CENTRES — what a bounds scan or a lattice extent reports.
  // chooseCubeDrawMode inflates it by half a cube itself; do not pre-inflate.
  AABB centre_bounds;
  glm::mat4 clip_from_cloud{1.0f};  // proj * view * model
  // Camera position in the same frame as centre_bounds. EMPTY under an orthographic
  // camera, which has no eye position — and needs none: there the fan picks its faces
  // from the view DIRECTION, which is the same for every cube however the volume is
  // placed, so the camera-inside case simply cannot arise.
  std::optional<glm::vec3> eye_in_cloud;
  float cube_size_m{0.0f};  // side length (sphere DIAMETER for the sprite shapes)
  // proj[1][1] * device_height / 2 — pixels a one-metre object spans at clip w == 1.
  float pixels_per_metre{0.0f};
  float lod_threshold_px{0.0f};  // 0 disables the sprite degradation
  // How far each instance can reach past its world extent ON SCREEN, in NDC units per
  // axis: a screen-sized sprite of S device pixels reaches S / viewport_px past its
  // centre. Pixel-sized points have no world extent at all, and a sphere's minimum
  // size can outgrow its projection, so without this the frustum reject drops a sprite
  // whose centre is just off screen while its quad still overlaps the viewport.
  glm::vec2 screen_reach_ndc{0.0f};
};

// The whole decision, in the order the cases actually nest.
//
// The inflation in step 1 is not cosmetic: a bounds scan reports where cube CENTRES are,
// but a cube reaches half its side past each one, so testing the raw box culls clouds
// whose geometry still pokes into view — invisible at a 1 cm cube, half a metre of missing
// data at a 1 m one.
//
// Step 3 is what lets a voxel grid use the fan at all. The fan picks its three faces by
// comparing the camera against the cube's CENTRE, which is only right while the camera is
// outside the cube; a voxel is easily large enough to swallow the viewpoint, which is why
// that pass used to be pinned to the solid unconditionally. Asking the question per frame
// instead means it gets the cheap geometry in the overwhelmingly common case and the
// correct geometry in the rare one. The point cloud gains the same protection, which it
// previously lacked entirely.
[[nodiscard]] inline CubeDrawMode chooseCubeDrawMode(const CubeCloudView& view, CubeDrawCapabilities available) {
  // Degrade an ideal choice to the best geometry this caller owns. Solid is the safe
  // fallback (it is correct everywhere), then fan, then sprite.
  const auto best = [available](CubeDrawMode ideal) {
    switch (ideal) {
      case CubeDrawMode::kSprite:
        if (available.sprite) {
          return CubeDrawMode::kSprite;
        }
        [[fallthrough]];
      case CubeDrawMode::kFan:
        if (available.fan) {
          return CubeDrawMode::kFan;
        }
        [[fallthrough]];
      case CubeDrawMode::kSolid:
        return available.solid ? CubeDrawMode::kSolid : CubeDrawMode::kFan;
      case CubeDrawMode::kSkip:
        break;
    }
    return CubeDrawMode::kSkip;
  };

  // 1. What the geometry actually occupies, not where its centres are.
  if (!view.centre_bounds.valid) {
    return best(CubeDrawMode::kFan);  // unknown extent must mean "draw it"
  }
  const glm::vec3 reach(view.cube_size_m * 0.5f);
  const AABB drawn{view.centre_bounds.min - reach, view.centre_bounds.max + reach, true};

  // 2. Off screen entirely. The screen reach widens the side planes: scaling clip x
  //    by 1 / (1 + reach) turns |x| <= w * (1 + reach) into the plain |x| <= w test.
  glm::mat4 cull_from_cloud = view.clip_from_cloud;
  for (int column = 0; column < 4; ++column) {
    cull_from_cloud[column][0] /= 1.0f + view.screen_reach_ndc.x;
    cull_from_cloud[column][1] /= 1.0f + view.screen_reach_ndc.y;
  }
  if (aabbOutsideFrustum(drawn, cull_from_cloud)) {
    return CubeDrawMode::kSkip;
  }

  // 3. Camera inside the cloud's own volume: any cube MIGHT contain it, and the fan's
  //    centre-vs-eye face pick would be wrong for that one. Conservative — being inside
  //    the cloud's box is far weaker than being inside a cube — but it is the test that
  //    needs no per-instance work, and the mode it selects is correct either way.
  const bool eye_inside = view.eye_in_cloud.has_value() &&
                          glm::all(glm::greaterThanEqual(*view.eye_in_cloud, drawn.min)) &&
                          glm::all(glm::lessThanEqual(*view.eye_in_cloud, drawn.max));
  if (eye_inside) {
    return best(CubeDrawMode::kSolid);
  }

  // 4. Every cube sub-pixel: sprites. Inverting size_px = pixels_per_metre * size / clip_w
  //    gives the nearest the cloud may come and still qualify. Clip w is the view-axis
  //    depth under perspective and exactly 1 under ortho, so one test covers both.
  if (view.lod_threshold_px > 0.0f && view.cube_size_m > 0.0f && view.pixels_per_metre > 0.0f) {
    const float size_px_scale = view.cube_size_m * view.pixels_per_metre;
    const float split_clip_w = size_px_scale / view.lod_threshold_px;
    if (aabbClipWRange(drawn, view.clip_from_cloud).first >= split_clip_w) {
      return best(CubeDrawMode::kSprite);
    }
  }

  return best(CubeDrawMode::kFan);
}

}  // namespace pj::scene3d
