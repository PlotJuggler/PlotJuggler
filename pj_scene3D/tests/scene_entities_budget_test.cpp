// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_budget.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace pj::scene3d {
namespace {

TEST(SceneEntitiesBudget, LayerBoundariesAreInclusive) {
  EXPECT_TRUE(markerLayerCountsFit(
      kBrowserSceneLimits.max_marker_instances_per_layer, kBrowserSceneLimits.max_marker_stream_vertices_per_layer,
      kBrowserSceneLimits.max_marker_frames_per_layer, kBrowserSceneLimits));
  EXPECT_FALSE(
      markerLayerCountsFit(kBrowserSceneLimits.max_marker_instances_per_layer + 1U, 0, 0, kBrowserSceneLimits));
  EXPECT_FALSE(
      markerLayerCountsFit(0, kBrowserSceneLimits.max_marker_stream_vertices_per_layer + 1U, 0, kBrowserSceneLimits));
  EXPECT_FALSE(markerLayerCountsFit(0, 0, kBrowserSceneLimits.max_marker_frames_per_layer + 1U, kBrowserSceneLimits));
}

TEST(SceneEntitiesBudget, AggregateConsumptionCannotUnderflow) {
  std::uint64_t instances = 7;
  EXPECT_FALSE(tryConsumeBudget(8, instances));
  EXPECT_EQ(instances, 7U);
  EXPECT_TRUE(tryConsumeBudget(7, instances));
  EXPECT_EQ(instances, 0U);

  std::uint64_t vertices = 11;
  EXPECT_FALSE(tryConsumeBudget(12, vertices));
  EXPECT_EQ(vertices, 11U);
  EXPECT_TRUE(tryConsumeBudget(11, vertices));
  EXPECT_EQ(vertices, 0U);
}

TEST(SceneEntitiesBudget, SubmittedInstancesIncludeNormalCubeEdgeOverlay) {
  const auto at_normal_limit = markerSubmittedInstanceCount(25'000, 25'000, false);
  ASSERT_TRUE(at_normal_limit.has_value());
  EXPECT_EQ(*at_normal_limit, kBrowserSceneLimits.max_marker_instances_per_layer);
  EXPECT_TRUE(markerLayerCountsFit(*at_normal_limit, 0, 1, kBrowserSceneLimits));

  const auto above_normal_limit = markerSubmittedInstanceCount(25'001, 25'001, false);
  ASSERT_TRUE(above_normal_limit.has_value());
  EXPECT_FALSE(markerLayerCountsFit(*above_normal_limit, 0, 1, kBrowserSceneLimits));

  const auto wireframe = markerSubmittedInstanceCount(
      kBrowserSceneLimits.max_marker_instances_per_layer, kBrowserSceneLimits.max_marker_instances_per_layer, true);
  ASSERT_TRUE(wireframe.has_value());
  EXPECT_EQ(*wireframe, kBrowserSceneLimits.max_marker_instances_per_layer);
  EXPECT_TRUE(markerLayerCountsFit(*wireframe, 0, 1, kBrowserSceneLimits));

  EXPECT_FALSE(markerSubmittedInstanceCount(std::numeric_limits<std::uint64_t>::max(), 1, false).has_value());
}

}  // namespace
}  // namespace pj::scene3d
