// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// aabbOutsideFrustum() decides whether a whole cloud's draw is skipped, so a false positive silently HIDES data:
// everything the camera can see must survive, and the obvious off-screen placements must be culled.
//
// aabbClipWRange() feeds the cube LOD: its lower bound must never come out too high, or a cloud with near cubes would
// be drawn as sprites.

#include <gtest/gtest.h>

#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

#include "pj_scene3d_core/camera/camera.h"

namespace {

using pj::scene3d::AABB;
using pj::scene3d::aabbClipWRange;
using pj::scene3d::aabbOutsideFrustum;

AABB boxAt(const glm::vec3& center, float half_extent = 0.5f) {
  AABB box;
  box.min = center - glm::vec3(half_extent);
  box.max = center + glm::vec3(half_extent);
  box.valid = true;
  return box;
}

glm::mat4 perspectiveClipFromWorld(const glm::vec3& eye = glm::vec3(0.0f, 0.0f, 10.0f)) {
  const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 4.0f / 3.0f, 0.1f, 100.0f);
  const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  return proj * view;
}

TEST(FrustumCullTest, InvalidBoxIsNeverCulled) {
  EXPECT_FALSE(aabbOutsideFrustum(AABB{}, perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, BoxAtTheFocusIsVisible) {
  EXPECT_FALSE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f)), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, BoxBehindTheCameraIsCulled) {
  EXPECT_TRUE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f, 0.0f, 30.0f)), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, BoxBeyondTheFarPlaneIsCulled) {
  EXPECT_TRUE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f, 0.0f, -200.0f)), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, BoxFarOffToTheSideIsCulled) {
  EXPECT_TRUE(aabbOutsideFrustum(boxAt(glm::vec3(100.0f, 0.0f, 0.0f)), perspectiveClipFromWorld()));
  EXPECT_TRUE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f, 100.0f, 0.0f)), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, BoxStraddlingTheEdgeIsKept) {
  // Half in view: the test must be conservative, never clipping what it cannot prove
  // is fully outside. At z = 0 the frustum half-height is 10*tan(30 deg) ~= 5.77.
  EXPECT_FALSE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f, 5.77f, 0.0f), 1.0f), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, HugeBoxEnclosingTheCameraIsKept) {
  // The camera sits inside the box, so no plane has all corners on its outside.
  EXPECT_FALSE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f), 1000.0f), perspectiveClipFromWorld()));
}

TEST(FrustumCullTest, OrthographicFrustumCullsSideways) {
  const glm::mat4 proj = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, 0.1f, 100.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 clip_from_world = proj * view;
  EXPECT_FALSE(aabbOutsideFrustum(boxAt(glm::vec3(0.0f)), clip_from_world));
  EXPECT_TRUE(aabbOutsideFrustum(boxAt(glm::vec3(20.0f, 0.0f, 0.0f)), clip_from_world));
}

TEST(FrustumCullTest, ModelTransformIsHonoured) {
  // The matrix argument takes the box's OWN frame to clip space, which is how the
  // pass avoids transforming the box: a visible box translated far away is culled.
  const glm::mat4 clip_from_world = perspectiveClipFromWorld();
  const AABB source_box = boxAt(glm::vec3(0.0f));
  EXPECT_FALSE(aabbOutsideFrustum(source_box, clip_from_world));

  const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(500.0f, 0.0f, 0.0f));
  EXPECT_TRUE(aabbOutsideFrustum(source_box, clip_from_world * model));
}

// Under perspective, clip w is the view-axis depth, so the range brackets the box's near and far faces: camera at
// z = +10 looking down -Z at a unit box on the origin, depths 9.5 to 10.5.
TEST(ClipWRangeTest, PerspectiveRangeIsTheDepthSpan) {
  const auto [near_w, far_w] = aabbClipWRange(boxAt(glm::vec3(0.0f)), perspectiveClipFromWorld());
  EXPECT_NEAR(near_w, 9.5f, 1e-4f);
  EXPECT_NEAR(far_w, 10.5f, 1e-4f);
}

// A parallel projection has no perspective divide: w is 1 everywhere and the collapsed range is the correct answer
// (the LOD decision it feeds is global).
TEST(ClipWRangeTest, OrthographicRangeCollapsesToOne) {
  const glm::mat4 proj = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, 0.1f, 100.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const auto [near_w, far_w] = aabbClipWRange(boxAt(glm::vec3(0.0f), 3.0f), proj * view);
  EXPECT_NEAR(near_w, 1.0f, 1e-5f);
  EXPECT_NEAR(far_w, 1.0f, 1e-5f);
}

// The lower bound gates the LOD, so it must be a true minimum over the box (the centre, or the wrong corner, would let
// near cubes draw as sprites): checked against a brute-force sweep of all eight corners.
TEST(ClipWRangeTest, BracketsEveryCornerUnderAnObliqueView) {
  const glm::mat4 proj = glm::perspective(glm::radians(55.0f), 16.0f / 9.0f, 0.1f, 500.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(12.0f, -18.0f, 7.0f), glm::vec3(1.0f, 2.0f, 0.0f), glm::vec3(0, 0, 1));
  const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 4.0f, 1.5f));
  const glm::mat4 clip_from_box = proj * view * model;

  const AABB box{glm::vec3(-2.0f, -5.0f, -1.0f), glm::vec3(6.0f, 3.0f, 4.0f), true};
  const auto [near_w, far_w] = aabbClipWRange(box, clip_from_box);

  float brute_min = std::numeric_limits<float>::max();
  float brute_max = std::numeric_limits<float>::lowest();
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 point{
        (corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
        (corner & 4) != 0 ? box.max.z : box.min.z};
    const float w = (clip_from_box * glm::vec4(point, 1.0f)).w;
    brute_min = std::min(brute_min, w);
    brute_max = std::max(brute_max, w);
  }
  EXPECT_NEAR(near_w, brute_min, 1e-3f);
  EXPECT_NEAR(far_w, brute_max, 1e-3f);
}

TEST(ClipWRangeTest, InvalidBoxHasNoRange) {
  const auto [near_w, far_w] = aabbClipWRange(AABB{}, perspectiveClipFromWorld());
  EXPECT_EQ(near_w, 0.0f);
  EXPECT_EQ(far_w, 0.0f);
}

}  // namespace
