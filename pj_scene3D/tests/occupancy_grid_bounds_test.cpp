// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Headless tests for the scene-bounds helpers in pj_scene3d_core/camera/camera.h (occupancyGridBounds, expandAABB,
// unionAABB), which back the 3D entities' worldBounds().

#include <gtest/gtest.h>

#include "pj_scene3d_core/camera/camera.h"

using pj::scene3d::AABB;
using pj::scene3d::expandAABB;
using pj::scene3d::occupancyGridBounds;
using pj::scene3d::unionAABB;

TEST(OccupancyGridBounds, KnownOriginResolutionSize) {
  // Origin at (-5, -3, 0), 0.05 m cells, 200 x 100 cells → spans 10 m x 5 m.
  const AABB box = occupancyGridBounds(glm::vec3{-5.0f, -3.0f, 0.0f}, 0.05, 200, 100);
  ASSERT_TRUE(box.valid);
  EXPECT_FLOAT_EQ(box.min.x, -5.0f);
  EXPECT_FLOAT_EQ(box.min.y, -3.0f);
  EXPECT_FLOAT_EQ(box.min.z, 0.0f);
  EXPECT_FLOAT_EQ(box.max.x, 5.0f);  // -5 + 0.05*200
  EXPECT_FLOAT_EQ(box.max.y, 2.0f);  // -3 + 0.05*100
  EXPECT_FLOAT_EQ(box.max.z, 0.0f);  // planar grid
}

TEST(OccupancyGridBounds, EmptyOrDegenerateIsInvalid) {
  EXPECT_FALSE(occupancyGridBounds(glm::vec3{0.0f}, 0.05, 0, 100).valid);
  EXPECT_FALSE(occupancyGridBounds(glm::vec3{0.0f}, 0.05, 100, 0).valid);
  EXPECT_FALSE(occupancyGridBounds(glm::vec3{0.0f}, 0.0, 100, 100).valid);
  EXPECT_FALSE(occupancyGridBounds(glm::vec3{0.0f}, -0.05, 100, 100).valid);
}

TEST(AabbExpand, FromInvalidAccumulatesMinMax) {
  AABB box;  // invalid
  EXPECT_FALSE(box.valid);
  expandAABB(box, glm::vec3{1.0f, 2.0f, 3.0f});
  ASSERT_TRUE(box.valid);
  EXPECT_EQ(box.min, box.max);
  expandAABB(box, glm::vec3{-1.0f, 5.0f, 0.0f});
  EXPECT_FLOAT_EQ(box.min.x, -1.0f);
  EXPECT_FLOAT_EQ(box.min.y, 2.0f);
  EXPECT_FLOAT_EQ(box.min.z, 0.0f);
  EXPECT_FLOAT_EQ(box.max.x, 1.0f);
  EXPECT_FLOAT_EQ(box.max.y, 5.0f);
  EXPECT_FLOAT_EQ(box.max.z, 3.0f);
}

TEST(AabbUnion, InvalidIsIdentity) {
  const AABB invalid;
  const AABB b = occupancyGridBounds(glm::vec3{0.0f, 0.0f, 0.0f}, 1.0, 2, 2);  // (0,0,0)-(2,2,0)
  ASSERT_TRUE(b.valid);

  const AABB left = unionAABB(invalid, b);
  const AABB right = unionAABB(b, invalid);
  EXPECT_EQ(left.min, b.min);
  EXPECT_EQ(left.max, b.max);
  EXPECT_EQ(right.min, b.min);
  EXPECT_EQ(right.max, b.max);

  EXPECT_FALSE(unionAABB(invalid, AABB{}).valid);
}

TEST(AabbUnion, MergesExtents) {
  const AABB b = occupancyGridBounds(glm::vec3{0.0f, 0.0f, 0.0f}, 1.0, 2, 2);    // (0,0,0)-(2,2,0)
  const AABB c = occupancyGridBounds(glm::vec3{-1.0f, -1.0f, 0.0f}, 1.0, 1, 1);  // (-1,-1,0)-(0,0,0)
  const AABB u = unionAABB(b, c);
  ASSERT_TRUE(u.valid);
  EXPECT_FLOAT_EQ(u.min.x, -1.0f);
  EXPECT_FLOAT_EQ(u.min.y, -1.0f);
  EXPECT_FLOAT_EQ(u.max.x, 2.0f);
  EXPECT_FLOAT_EQ(u.max.y, 2.0f);
}
