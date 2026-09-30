// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/poses_budget.h"

#include <gtest/gtest.h>

namespace pj::scene3d {
namespace {

TEST(PosesBrowserBudget, LayerLimitsAcceptTheirExactBoundaryOnly) {
  EXPECT_TRUE(poseCountFits(kBrowserSceneLimits.max_poses_per_layer, kBrowserSceneLimits));
  EXPECT_FALSE(poseCountFits(kBrowserSceneLimits.max_poses_per_layer + 1U, kBrowserSceneLimits));
  EXPECT_TRUE(posePayloadFits(kBrowserSceneLimits.max_pose_wire_bytes_per_layer, kBrowserSceneLimits));
  EXPECT_FALSE(posePayloadFits(kBrowserSceneLimits.max_pose_wire_bytes_per_layer + 1U, kBrowserSceneLimits));
}

TEST(PosesBrowserBudget, ViewArmBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserSceneLimits.max_pose_arms_per_view;
  EXPECT_TRUE(tryConsumeBudget(90'000U, remaining));
  EXPECT_EQ(remaining, 210'000U);
  EXPECT_TRUE(tryConsumeBudget(210'000U, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBudget(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
