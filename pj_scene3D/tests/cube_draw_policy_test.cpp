// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// chooseCubeDrawMode() is the one place that decides how a cloud of cubes is drawn, for
// both the point-cloud and the voxel-grid passes. Every decision it makes can silently
// lose data or silently cost performance, and none of them needs a GL context to check —
// which is the whole reason the policy is Qt-free and lives in core.

#include "pj_scene3d_core/cube_draw_policy.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using pj::scene3d::AABB;
using pj::scene3d::chooseCubeDrawMode;
using pj::scene3d::CubeCloudView;
using pj::scene3d::CubeDrawCapabilities;
using pj::scene3d::CubeDrawMode;

constexpr float kFovYDegrees = 60.0f;
constexpr int kHeightPx = 720;

// A pass that owns every geometry, so the tests see the IDEAL choice rather than a
// degradation. The capability clamping gets its own tests below.
constexpr CubeDrawCapabilities kAll{/*sprite=*/true, /*fan=*/true, /*solid=*/true};

float pixelsPerMetre(const glm::mat4& proj) {
  return proj[1][1] * static_cast<float>(kHeightPx) * 0.5f;
}

// Camera at the origin looking down -Z, cloud placed `distance` in front of it.
CubeCloudView viewAt(float distance, float half_extent, float cube_size_m, float lod_threshold_px) {
  const glm::mat4 proj = glm::perspective(glm::radians(kFovYDegrees), 1.0f, 0.05f, 1000.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  CubeCloudView out;
  out.centre_bounds = AABB{
      glm::vec3(-half_extent, -half_extent, -distance - half_extent),
      glm::vec3(half_extent, half_extent, -distance + half_extent), true};
  out.clip_from_cloud = proj * view;
  out.eye_in_cloud = glm::vec3(0.0f);  // camera at the origin
  out.cube_size_m = cube_size_m;
  out.pixels_per_metre = pixelsPerMetre(proj);
  out.lod_threshold_px = lod_threshold_px;
  return out;
}

TEST(CubeDrawPolicyTest, UnknownExtentIsDrawnRatherThanCulled) {
  CubeCloudView view = viewAt(20.0f, 1.0f, 0.05f, 1.0f);
  view.centre_bounds = AABB{};  // no bounds scan has landed yet
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kFan) << "an unknown extent must never cull";
}

TEST(CubeDrawPolicyTest, OffScreenCloudIsSkipped) {
  CubeCloudView view = viewAt(20.0f, 1.0f, 0.05f, 1.0f);
  view.centre_bounds.min.x += 500.0f;
  view.centre_bounds.max.x += 500.0f;
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
}

TEST(CubeDrawPolicyTest, CloudBehindTheCameraIsSkipped) {
  CubeCloudView view = viewAt(-40.0f, 1.0f, 0.05f, 1.0f);
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
}

// The bug this policy exists to prevent: culling against the CENTRE box drops geometry
// that still reaches into the frustum. With a 1 m cube the overhang is half a metre.
TEST(CubeDrawPolicyTest, CubeOverhangingTheFrustumEdgeSurvives) {
  const float distance = 10.0f;
  const float half_height = distance * std::tan(glm::radians(kFovYDegrees) * 0.5f);
  constexpr float kBigCube = 2.0f;

  CubeCloudView view = viewAt(distance, 0.0f, kBigCube, /*lod_threshold_px=*/0.0f);
  // Centre just outside the top plane; the cube around it still crosses the edge.
  const float centre_y = half_height + 0.4f * kBigCube;
  view.centre_bounds.min.y = centre_y;
  view.centre_bounds.max.y = centre_y;

  EXPECT_NE(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip)
      << "the centre is outside the frustum but the cube is not — this is the inflation step";

  // Shrink the cube and the same centre really is all there is: now it may be culled.
  view.cube_size_m = 0.01f;
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
}

// A screen-sized sprite has no world extent to inflate by: its screen reach keeps a
// centre just past the side plane, and only that far.
TEST(CubeDrawPolicyTest, SpriteReachingIntoTheViewSurvives) {
  const float distance = 10.0f;
  const float half_height = distance * std::tan(glm::radians(kFovYDegrees) * 0.5f);
  CubeCloudView view = viewAt(distance, 0.0f, /*cube_size_m=*/0.0f, /*lod_threshold_px=*/0.0f);
  const float centre_y = half_height * 1.02f;  // 1% of the view height (7 px) past the top edge
  view.centre_bounds.min.y = centre_y;
  view.centre_bounds.max.y = centre_y;
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
  view.screen_reach_ndc = glm::vec2(30.0f / static_cast<float>(kHeightPx));  // a 30 px sprite reaches 15 px
  EXPECT_NE(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
  view.screen_reach_ndc = glm::vec2(10.0f / static_cast<float>(kHeightPx));  // 5 px falls short
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSkip);
}

TEST(CubeDrawPolicyTest, DistantCloudDegradesToSprites) {
  // 5 cm cubes at 200 m: well under a pixel at this projection.
  EXPECT_EQ(chooseCubeDrawMode(viewAt(200.0f, 2.0f, 0.05f, 1.0f), kAll), CubeDrawMode::kSprite);
}

TEST(CubeDrawPolicyTest, NearCloudKeepsRealGeometry) {
  EXPECT_EQ(chooseCubeDrawMode(viewAt(3.0f, 0.5f, 0.05f, 1.0f), kAll), CubeDrawMode::kFan);
}

// The gate is whole-cloud: one above-threshold cube anywhere keeps the entire cloud on
// the fan, because splitting a cloud between two programs measured slower than not.
TEST(CubeDrawPolicyTest, CloudStraddlingTheThresholdKeepsTheFan) {
  // Deep in Z, so the near face is well above the threshold and the far face well below.
  EXPECT_EQ(chooseCubeDrawMode(viewAt(60.0f, 55.0f, 0.05f, 1.0f), kAll), CubeDrawMode::kFan);
}

TEST(CubeDrawPolicyTest, ZeroThresholdDisablesTheSpriteDegradation) {
  EXPECT_EQ(chooseCubeDrawMode(viewAt(200.0f, 2.0f, 0.05f, /*lod_threshold_px=*/0.0f), kAll), CubeDrawMode::kFan);
}

// The reason the voxel grid can use the fan at all: the answer is per frame, not a
// blanket policy. Inside the volume the fan's centre-vs-eye face pick is unsafe.
TEST(CubeDrawPolicyTest, CameraInsideTheCloudFallsBackToTheSolid) {
  CubeCloudView view = viewAt(0.0f, 20.0f, 0.5f, 1.0f);  // box straddles the origin = the eye
  EXPECT_EQ(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSolid);

  // Step the camera outside the same box and the cheap geometry comes back.
  view.eye_in_cloud = glm::vec3(0.0f, 0.0f, 500.0f);
  EXPECT_NE(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSolid);

  // An orthographic camera has no eye position and needs none — the fan's face pick is
  // global there, so a volume enclosing the viewpoint is still safe on the fan.
  view.eye_in_cloud.reset();
  EXPECT_NE(chooseCubeDrawMode(view, kAll), CubeDrawMode::kSolid);
}

// A pass that has not implemented a geometry still gets every decision that does not
// depend on it, degrading to what it can actually draw.
TEST(CubeDrawPolicyTest, DegradesToTheBestAvailableGeometry) {
  const CubeCloudView distant = viewAt(200.0f, 2.0f, 0.05f, 1.0f);
  constexpr CubeDrawCapabilities kFanOnly{/*sprite=*/false, /*fan=*/true, /*solid=*/false};
  EXPECT_EQ(chooseCubeDrawMode(distant, kFanOnly), CubeDrawMode::kFan) << "no sprite program: fall back to the fan";

  constexpr CubeDrawCapabilities kSolidOnly{/*sprite=*/false, /*fan=*/false, /*solid=*/true};
  EXPECT_EQ(chooseCubeDrawMode(distant, kSolidOnly), CubeDrawMode::kSolid);
  // ...but the frustum reject still applies, which is the point of sharing the policy.
  CubeCloudView off_screen = distant;
  off_screen.centre_bounds.min.x += 5000.0f;
  off_screen.centre_bounds.max.x += 5000.0f;
  EXPECT_EQ(chooseCubeDrawMode(off_screen, kSolidOnly), CubeDrawMode::kSkip);

  // A caller with no solid must still get something drawable when the eye is inside.
  CubeCloudView inside = viewAt(0.0f, 20.0f, 0.5f, 1.0f);
  EXPECT_EQ(chooseCubeDrawMode(inside, kFanOnly), CubeDrawMode::kFan);
}

// Ortho has no perspective divide: clip w is 1 for every cube, so the LOD decision is
// correctly global rather than depth-dependent.
TEST(CubeDrawPolicyTest, OrthographicProjectionDecidesGlobally) {
  const glm::mat4 proj = glm::ortho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 1000.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 100.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  CubeCloudView cloud;
  cloud.centre_bounds = AABB{glm::vec3(-10.0f), glm::vec3(10.0f), true};
  cloud.clip_from_cloud = proj * view;
  cloud.eye_in_cloud.reset();  // ortho: no eye position
  cloud.pixels_per_metre = pixelsPerMetre(proj);
  cloud.lod_threshold_px = 1.0f;

  cloud.cube_size_m = 0.01f;  // 0.07 px under this ortho scale
  EXPECT_EQ(chooseCubeDrawMode(cloud, kAll), CubeDrawMode::kSprite);
  cloud.cube_size_m = 5.0f;  // decisively above a pixel
  EXPECT_EQ(chooseCubeDrawMode(cloud, kAll), CubeDrawMode::kFan);
}

}  // namespace
