// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/voxel_grid_budget.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace pj::scene3d {
namespace {

TEST(VoxelGridBudget, DimensionsAcceptExactLayerLimit) {
  EXPECT_TRUE(voxelDimensionsFit(256, 128, 128, kBrowserSceneLimits));
  EXPECT_FALSE(voxelDimensionsFit(257, 128, 128, kBrowserSceneLimits));
}

TEST(VoxelGridBudget, DimensionsRejectDegenerateAndOverflowingProducts) {
  EXPECT_FALSE(voxelDimensionsFit(0, 1, 1, kBrowserSceneLimits));
  EXPECT_FALSE(voxelDimensionsFit(1, 0, 1, kBrowserSceneLimits));
  EXPECT_FALSE(voxelDimensionsFit(1, 1, 0, kBrowserSceneLimits));
  EXPECT_FALSE(voxelDimensionsFit(
      std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), 2, kBrowserSceneLimits));
}

TEST(VoxelGridBudget, AggregateConsumptionCannotUnderflow) {
  std::uint64_t remaining = 10;
  EXPECT_FALSE(tryConsumeBudget(11, remaining));
  EXPECT_EQ(remaining, 10U);
  EXPECT_TRUE(tryConsumeBudget(10, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
