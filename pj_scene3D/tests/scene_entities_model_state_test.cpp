// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_model_state.h"

#include <gtest/gtest.h>

#include <limits>

namespace pj::scene3d {
namespace {

PJ::sdk::SceneEntity entity(std::string id, std::int64_t timestamp, std::int64_t lifetime = 0) {
  PJ::sdk::SceneEntity result;
  result.id = std::move(id);
  result.timestamp = timestamp;
  result.lifetime_ns = lifetime;
  result.models.emplace_back();
  return result;
}

PJ::sdk::SceneEntities batch(std::initializer_list<PJ::sdk::SceneEntity> entities) {
  PJ::sdk::SceneEntities result;
  result.entities.assign(entities);
  return result;
}

}  // namespace

TEST(SceneEntitiesModelStateTest, ReplacesByIdAndAnchorsLifetimeOnIngestClock) {
  SceneEntitiesModelState state;
  EXPECT_TRUE(state.applySnapshot(batch({entity("car", 10, 50)}), 1'000));
  EXPECT_FALSE(state.dropExpired(1'050));
  EXPECT_TRUE(state.entities().contains("car"));
  EXPECT_TRUE(state.dropExpired(1'051));
  EXPECT_TRUE(state.entities().empty());

  EXPECT_TRUE(state.applySnapshot(batch({entity("car", 20)}), 2'000));
  EXPECT_TRUE(state.applySnapshot(batch({entity("car", 30)}), 3'000));
  ASSERT_EQ(state.entities().size(), 1U);
  EXPECT_EQ(state.entities().at("car").timestamp, 30);
}

// Expiry skips entities that never expire; a replacement or a deletion moves an
// id between finite and infinite lifetimes, and expiry must keep following it.
TEST(SceneEntitiesModelStateTest, ExpiryFollowsLifetimeChangesOfAnId) {
  SceneEntitiesModelState state;
  EXPECT_TRUE(state.applySnapshot(batch({entity("a", 10, 50), entity("b", 10)}), 1'000));
  EXPECT_TRUE(state.applySnapshot(batch({entity("a", 20)}), 1'010));  // now never expires
  EXPECT_FALSE(state.dropExpired(std::numeric_limits<std::int64_t>::max()));
  EXPECT_EQ(state.entities().size(), 2U);

  EXPECT_TRUE(state.applySnapshot(batch({entity("b", 30, 5)}), 2'000));  // now expires
  EXPECT_FALSE(state.dropExpired(2'005));
  EXPECT_TRUE(state.dropExpired(2'006));
  EXPECT_FALSE(state.entities().contains("b"));
  EXPECT_TRUE(state.entities().contains("a"));

  state.clear();
  EXPECT_TRUE(state.applySnapshot(batch({entity("c", 40, 5)}), 3'000));
  EXPECT_TRUE(state.dropExpired(3'006));
}

TEST(SceneEntitiesModelStateTest, AppliesDeletionsBeforeSameBatchReAdd) {
  SceneEntitiesModelState state;
  EXPECT_TRUE(state.applySnapshot(batch({entity("old", 10)}), 10));
  PJ::sdk::SceneEntities replacement = batch({entity("new", 20)});
  PJ::sdk::SceneEntityDeletion deletion;
  deletion.type = PJ::sdk::SceneEntityDeletion::Type::kAll;
  deletion.timestamp = 20;
  replacement.deletions.push_back(deletion);
  EXPECT_TRUE(state.applySnapshot(replacement, 20));
  EXPECT_FALSE(state.entities().contains("old"));
  EXPECT_TRUE(state.entities().contains("new"));
}

TEST(SceneEntitiesModelStateTest, MatchingDeletionUsesEntityTimestampAndNegativeLifetimeExpiresImmediately) {
  SceneEntitiesModelState state;
  EXPECT_TRUE(state.applySnapshot(batch({entity("keep", 200), entity("drop", 100)}), 1'000));
  PJ::sdk::SceneEntities deletion_batch;
  PJ::sdk::SceneEntityDeletion deletion;
  deletion.type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId;
  deletion.timestamp = 150;
  deletion.id = "drop";
  deletion_batch.deletions.push_back(deletion);
  deletion.id = "keep";
  deletion_batch.deletions.push_back(deletion);
  EXPECT_TRUE(state.applySnapshot(deletion_batch, 1'100));
  EXPECT_FALSE(state.entities().contains("drop"));
  EXPECT_TRUE(state.entities().contains("keep"));

  EXPECT_TRUE(state.applySnapshot(batch({entity("negative", 1, -1)}), 500));
  EXPECT_TRUE(state.dropExpired(500));
  EXPECT_FALSE(state.entities().contains("negative"));
}

TEST(SceneEntitiesModelStateTest, LifetimeOverflowSaturatesSafely) {
  SceneEntitiesModelState state;
  EXPECT_TRUE(state.applySnapshot(batch({entity("positive", 1, 10)}), std::numeric_limits<std::int64_t>::max() - 5));
  EXPECT_FALSE(state.dropExpired(std::numeric_limits<std::int64_t>::max()));
  state.clear();
  EXPECT_TRUE(state.applySnapshot(batch({entity("negative", 1, -10)}), std::numeric_limits<std::int64_t>::min() + 5));
  EXPECT_TRUE(state.dropExpired(std::numeric_limits<std::int64_t>::min() + 5));
}

TEST(SceneEntitiesModelStateTest, RetainsOnlyModelFieldsAndProjectsReplacementOrDeletion) {
  SceneEntitiesModelState state;
  PJ::sdk::SceneEntity first = entity("car", 10);
  first.frame_id = "map";
  first.models.front().data.assign(128U, 0x42U);
  first.cubes.resize(1'000U);
  const PJ::sdk::SceneEntities initial = batch({first});
  const auto projected_initial = state.projectedRetainedBytes(initial);
  ASSERT_TRUE(projected_initial.has_value());
  EXPECT_TRUE(state.applySnapshot(initial, 10));
  EXPECT_EQ(state.retainedBytes(), *projected_initial);
  ASSERT_EQ(state.entities().at("car").models.front().data.size(), 128U);
  EXPECT_TRUE(state.entities().at("car").cubes.empty());

  PJ::sdk::SceneEntity replacement = entity("car", 20);
  replacement.models.front().data.assign(32U, 0x24U);
  const PJ::sdk::SceneEntities replacement_batch = batch({replacement});
  const auto projected_replacement = state.projectedRetainedBytes(replacement_batch);
  ASSERT_TRUE(projected_replacement.has_value());
  EXPECT_LT(*projected_replacement, state.retainedBytes());
  EXPECT_TRUE(state.applySnapshot(replacement_batch, 20));
  EXPECT_EQ(state.retainedBytes(), *projected_replacement);

  PJ::sdk::SceneEntities deletion_batch;
  PJ::sdk::SceneEntityDeletion deletion;
  deletion.type = PJ::sdk::SceneEntityDeletion::Type::kAll;
  deletion.timestamp = 20;
  deletion_batch.deletions.push_back(deletion);
  EXPECT_EQ(state.projectedRetainedBytes(deletion_batch), 0U);
  EXPECT_TRUE(state.applySnapshot(deletion_batch, 30));
  EXPECT_EQ(state.retainedBytes(), 0U);
}

}  // namespace pj::scene3d
