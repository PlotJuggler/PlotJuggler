// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/pointcloud_budget.h"

#include <gtest/gtest.h>

#include <limits>

namespace pj::scene3d {
namespace {

TEST(PointCloudBrowserBudget, ViewBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserSceneLimits.max_point_vertices_per_view;
  EXPECT_TRUE(tryConsumeBudget(400'000U, remaining));
  EXPECT_EQ(remaining, 600'000U);
  EXPECT_TRUE(tryConsumeBudget(600'000U, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBudget(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

TEST(PointCloudBrowserBudget, DepthDimensionsWidenBeforeApplyingSharedPointLimit) {
  std::uint64_t pixels = 0;
  EXPECT_TRUE(depthDimensionsFit(1000U, 1000U, pixels, kBrowserSceneLimits));
  EXPECT_EQ(pixels, kBrowserSceneLimits.max_points_per_cloud);
  EXPECT_FALSE(depthDimensionsFit(1001U, 1000U, pixels, kBrowserSceneLimits));
  EXPECT_EQ(pixels, 1'001'000U);
  EXPECT_FALSE(depthDimensionsFit(0U, 1000U, pixels, kBrowserSceneLimits));
  EXPECT_EQ(pixels, 0U);
  EXPECT_FALSE(depthDimensionsFit(
      std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), pixels,
      kBrowserSceneLimits));
  EXPECT_GT(pixels, std::numeric_limits<std::uint32_t>::max());
}

}  // namespace
}  // namespace pj::scene3d
