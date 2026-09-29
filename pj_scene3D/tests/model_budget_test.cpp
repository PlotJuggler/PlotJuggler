// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/model_budget.h"

#include <gtest/gtest.h>

#include <limits>

namespace pj::scene3d {

TEST(ModelBudgetTest, SourceAndMeshBoundariesAreInclusive) {
  EXPECT_TRUE(modelMeshCountsFit(
      kBrowserSceneLimits.max_model_vertices_per_mesh, kBrowserSceneLimits.max_model_indices_per_mesh,
      kBrowserSceneLimits.max_model_submeshes_per_mesh, kBrowserSceneLimits));
  EXPECT_FALSE(modelMeshCountsFit(kBrowserSceneLimits.max_model_vertices_per_mesh + 1U, 0U, 0U, kBrowserSceneLimits));
  EXPECT_FALSE(modelMeshCountsFit(0U, kBrowserSceneLimits.max_model_indices_per_mesh + 1U, 0U, kBrowserSceneLimits));
  EXPECT_FALSE(modelMeshCountsFit(0U, 0U, kBrowserSceneLimits.max_model_submeshes_per_mesh + 1U, kBrowserSceneLimits));
  EXPECT_TRUE(modelStructureFits(
      kBrowserSceneLimits.max_model_materials_per_mesh, kBrowserSceneLimits.max_model_nodes_per_mesh,
      kBrowserSceneLimits.max_model_node_depth, kBrowserSceneLimits));
  EXPECT_FALSE(modelStructureFits(kBrowserSceneLimits.max_model_materials_per_mesh + 1U, 0U, 0U, kBrowserSceneLimits));
  EXPECT_FALSE(modelStructureFits(0U, kBrowserSceneLimits.max_model_nodes_per_mesh + 1U, 0U, kBrowserSceneLimits));
  EXPECT_FALSE(modelStructureFits(0U, 0U, kBrowserSceneLimits.max_model_node_depth + 1U, kBrowserSceneLimits));
}

TEST(ModelBudgetTest, RetainedByteAccountingRejectsOverflow) {
  const auto normal = modelRetainedByteCount(10U, 64U, 30U, 2U, 24U, 128U);
  ASSERT_TRUE(normal.has_value());
  // Includes the retained two-edge-index expansion for every triangle index.
  EXPECT_EQ(*normal, 1'176U);
  EXPECT_FALSE(modelRetainedByteCount(std::numeric_limits<std::uint64_t>::max(), 2U, 0U, 0U, 0U, 0U).has_value());
  EXPECT_FALSE(modelRetainedByteCount(0U, 0U, std::numeric_limits<std::uint64_t>::max(), 0U, 0U, 0U).has_value());
}

TEST(ModelBudgetTest, DecodedTexturePixelConsumptionIsAggregateAndTransactional) {
  std::uint64_t remaining = kBrowserSceneLimits.max_model_texture_pixels_per_mesh;
  EXPECT_TRUE(tryConsumeBudget(6U * 1024U * 1024U, remaining));
  EXPECT_EQ(remaining, 10U * 1024U * 1024U);
  EXPECT_FALSE(tryConsumeBudget(10U * 1024U * 1024U + 1U, remaining));
  EXPECT_EQ(remaining, 10U * 1024U * 1024U);
  EXPECT_TRUE(tryConsumeBudget(remaining, remaining));
  EXPECT_EQ(remaining, 0U);
}

TEST(ModelBudgetTest, DecodedTextureConsumptionChecksMeshAndLayerTransactionally) {
  std::uint64_t mesh_remaining = 8U;
  std::uint64_t layer_remaining = 6U;
  EXPECT_FALSE(tryConsumeModelTexturePixels(7U, mesh_remaining, layer_remaining));
  EXPECT_EQ(mesh_remaining, 8U);
  EXPECT_EQ(layer_remaining, 6U);
  EXPECT_TRUE(tryConsumeModelTexturePixels(6U, mesh_remaining, layer_remaining));
  EXPECT_EQ(mesh_remaining, 2U);
  EXPECT_EQ(layer_remaining, 0U);
}

TEST(ModelBudgetTest, FiveSlotMeshPreflightRollsBackWhenLastMapExceedsLayerBudget) {
  std::uint64_t committed_layer_remaining = 10U;
  std::uint64_t candidate_layer_remaining = committed_layer_remaining;
  std::uint64_t mesh_remaining = 16U;
  for (int slot = 0; slot < 4; ++slot) {
    ASSERT_TRUE(tryConsumeModelTexturePixels(2U, mesh_remaining, candidate_layer_remaining));
  }
  EXPECT_EQ(candidate_layer_remaining, 2U);
  EXPECT_FALSE(tryConsumeModelTexturePixels(3U, mesh_remaining, candidate_layer_remaining));
  EXPECT_EQ(candidate_layer_remaining, 2U);
  EXPECT_EQ(mesh_remaining, 8U);
  EXPECT_EQ(committed_layer_remaining, 10U);
}

TEST(ModelBudgetTest, AggregateConsumptionIsTransactional) {
  std::uint64_t draws = 8U;
  std::uint64_t triangles = 20U;
  EXPECT_FALSE(tryConsumeModels(9U, 1U, draws, triangles));
  EXPECT_EQ(draws, 8U);
  EXPECT_EQ(triangles, 20U);
  EXPECT_TRUE(tryConsumeModels(3U, 7U, draws, triangles));
  EXPECT_EQ(draws, 5U);
  EXPECT_EQ(triangles, 13U);
}

}  // namespace pj::scene3d
