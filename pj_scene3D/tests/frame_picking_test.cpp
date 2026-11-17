// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Unit tests for the TF frame hover-pick helpers (frame_picking.h): the GL-free math that turns a frame origin plus
// the camera matrices into a screen pixel and picks the frame under the cursor.

#include "pj_scene3d_core/tf/frame_picking.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace pj::scene3d {
namespace {

constexpr glm::vec2 kViewport{800.0f, 600.0f};

// With an identity view_proj, the world origin sits at NDC (0,0,0) → the exact
// centre of the viewport, with +y measured downward from the top-left.
TEST(ProjectFrameOrigin, OriginMapsToViewportCentre) {
  const auto p = projectFrameOrigin(glm::mat4(1.0f), kViewport, glm::vec3(0.0f));
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(p->x, 400.0f, 1e-3f);
  EXPECT_NEAR(p->y, 300.0f, 1e-3f);
}

TEST(ProjectFrameOrigin, PlusXMapsToRightEdge) {
  const auto p = projectFrameOrigin(glm::mat4(1.0f), kViewport, glm::vec3(1.0f, 0.0f, 0.0f));
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(p->x, 800.0f, 1e-3f);
  EXPECT_NEAR(p->y, 300.0f, 1e-3f);
}

TEST(ProjectFrameOrigin, PlusYMapsToUpperHalf) {
  const auto p = projectFrameOrigin(glm::mat4(1.0f), kViewport, glm::vec3(0.0f, 1.0f, 0.0f));
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(p->x, 400.0f, 1e-3f);
  EXPECT_NEAR(p->y, 0.0f, 1e-3f);
}

// A frame behind the camera (clip.w <= 0) must not project — otherwise a frame
// at your back would draw a phantom label at a mirrored screen position.
TEST(ProjectFrameOrigin, BehindCameraReturnsNullopt) {
  const glm::mat4 proj = glm::perspective(glm::radians(60.0f), kViewport.x / kViewport.y, 0.1f, 100.0f);
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const auto p = projectFrameOrigin(proj * view, kViewport, glm::vec3(0.0f, 0.0f, 10.0f));
  EXPECT_FALSE(p.has_value());
}

TEST(PickNearestFrame, PicksTheClosestWithinRadius) {
  const std::vector<glm::vec2> points{{100.0f, 100.0f}, {400.0f, 300.0f}, {700.0f, 500.0f}};
  const auto idx = pickNearestFrame(points, glm::vec2(405.0f, 298.0f), 12.0f);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1u);
}

TEST(PickNearestFrame, NothingWithinRadiusReturnsNullopt) {
  const std::vector<glm::vec2> points{{100.0f, 100.0f}};
  const auto idx = pickNearestFrame(points, glm::vec2(400.0f, 300.0f), 12.0f);
  EXPECT_FALSE(idx.has_value());
}

TEST(PickNearestFrame, PicksStrictlyClosestAmongSeveral) {
  const std::vector<glm::vec2> points{{200.0f, 200.0f}, {207.0f, 200.0f}};
  const auto idx = pickNearestFrame(points, glm::vec2(205.0f, 200.0f), 12.0f);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1u);  // (207,200) is 2px away vs 5px for (200,200)
}

// Two frames at the exact same pixel are equidistant: the first wins, so the
// pick is deterministic rather than order-dependent on equal distances.
TEST(PickNearestFrame, StackedAtSamePixelPicksFirst) {
  const std::vector<glm::vec2> points{{200.0f, 200.0f}, {200.0f, 200.0f}};
  const auto idx = pickNearestFrame(points, glm::vec2(200.0f, 200.0f), 12.0f);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 0u);
}

TEST(PickNearestFrame, EmptyInputReturnsNullopt) {
  const auto idx = pickNearestFrame(std::span<const glm::vec2>{}, glm::vec2(0.0f), 12.0f);
  EXPECT_FALSE(idx.has_value());
}

}  // namespace
}  // namespace pj::scene3d
