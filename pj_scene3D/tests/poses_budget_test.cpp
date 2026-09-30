// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_scene3d_core/scene_limits.h"

namespace pj::scene3d {
namespace {

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
