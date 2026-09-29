// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/occupancy_grid_budget.h"

#include <gtest/gtest.h>

namespace pj::scene3d {
namespace {

TEST(OccupancyGridBrowserBudget, LayerLimitsAcceptExactBoundariesOnly) {
  EXPECT_TRUE(occupancyDimensionsFit(8192U, 1024U, kBrowserSceneLimits));
  EXPECT_FALSE(occupancyDimensionsFit(8192U, 1025U, kBrowserSceneLimits));
  EXPECT_FALSE(occupancyDimensionsFit(8193U, 1U, kBrowserSceneLimits));
  EXPECT_FALSE(occupancyDimensionsFit(0U, 1024U, kBrowserSceneLimits));
}

TEST(OccupancyGridBrowserBudget, ViewBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserSceneLimits.max_occupancy_cells_per_view;
  EXPECT_TRUE(tryConsumeBudget(kBrowserSceneLimits.max_occupancy_cells_per_layer, remaining));
  EXPECT_EQ(remaining, kBrowserSceneLimits.max_occupancy_cells_per_layer);
  EXPECT_TRUE(tryConsumeBudget(kBrowserSceneLimits.max_occupancy_cells_per_layer, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBudget(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
