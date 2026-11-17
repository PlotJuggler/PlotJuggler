// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Streaming pause/resume two-engine lockstep mirror: the secondary DataEngine holds the same (TopicId, FieldId)
// for the same (topic, field) pairs as the primary, so a plugin's cached handles keep resolving after
// setTarget swaps which engine is the active write target.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using namespace PJ::sdk;

// Mirrors the real wiring done by StreamingSourceManager::startSession: the
// dataset is created on BOTH engines with the same DatasetId, so a topic
// mirror later can address it via the same id on either side.
struct TwoEngineFixture {
  DataEngine primary;
  DataEngine secondary;
  ObjectStore primary_store;
  DatastoreToolboxHost toolbox_impl{primary, primary_store};
  ToolboxHostView toolbox{toolbox_impl.raw()};

  DataSourceHandle bootstrapSourceWithLockstepDataset(std::string_view name) {
    const auto source = *toolbox.createDataSource(name);
    auto mirror = secondary.createDataset(DatasetDescriptor{.source_name = std::string(name)}, source.id);
    EXPECT_TRUE(mirror.has_value()) << "secondary lockstep createDataset failed";
    return source;
  }
};

// A TopicHandle/FieldHandle cached on primary keeps working after setTarget(secondary) and back. Without
// mirroring at ensureTopic/ensureField the secondary has no matching id and the first post-swap write fails.
TEST(StreamingMirrorTest, CachedFieldHandleResolvesAcrossSetTargetSwap) {
  TwoEngineFixture f;
  const auto source = f.bootstrapSourceWithLockstepDataset("sensor");

  DatastoreSourceWriteHost source_impl(f.primary, source);
  source_impl.setSecondaryEngine(&f.secondary);
  SourceWriteHostView writer(source_impl.raw());

  // The plugin caches these handles on primary.
  const auto topic = *writer.ensureTopic("imu");
  const auto field_ax = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  const auto field_ay = *writer.ensureField(topic, "ay", PrimitiveType::kFloat32);

  // Both engines must agree on the assigned ids.
  ASSERT_NE(f.primary.getTopicStorage(topic.id), nullptr);
  ASSERT_NE(f.secondary.getTopicStorage(topic.id), nullptr);
  // FieldId 0 is a valid auto-assigned id (the first field of any topic). A
  // naive `requested_id == 0` "auto" sentinel would mis-assign the first mirrored field.
  EXPECT_EQ(field_ax.id, 0U);
  EXPECT_EQ(field_ay.id, 1U);

  const std::vector<BoundFieldValue> primary_fields = {{.field = field_ax, .value = 1.0F}};
  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, primary_fields).has_value());
  source_impl.flushPending();

  // Swap target to secondary (mimics StreamingSourceManager::onPauseToggled).
  source_impl.setTarget(&f.secondary);

  // Write through the same cached handles: they must resolve on the secondary.
  const std::vector<BoundFieldValue> secondary_fields = {
      {.field = field_ax, .value = 2.0F},
      {.field = field_ay, .value = 3.0F},
  };
  ASSERT_TRUE(writer.appendBoundRecord(topic, 20, secondary_fields).has_value());
  source_impl.flushPending();

  // Swap back to primary (resume): bidirectional mirror keeps both directions
  // consistent. The cached handles still resolve.
  source_impl.setTarget(&f.primary);
  ASSERT_TRUE(writer.appendBoundRecord(topic, 30, secondary_fields).has_value());
  source_impl.flushPending();

  const auto* sec_storage = f.secondary.getTopicStorage(topic.id);
  ASSERT_NE(sec_storage, nullptr);
  const auto& sec_cols = sec_storage->columnDescriptors();
  ASSERT_GE(sec_cols.size(), 2U);
  EXPECT_EQ(sec_cols[0].field_path, "ax");
  EXPECT_EQ(sec_cols[0].field_id, 0U);
  EXPECT_EQ(sec_cols[1].field_path, "ay");
  EXPECT_EQ(sec_cols[1].field_id, 1U);
}

// Cache-hit retry path: if setSecondaryEngine is wired after the WriteCore cache has been populated,
// subsequent ensureTopic/ensureField calls must still mirror; otherwise a cache hit short-circuits the retry
// forever (an earlier mirror failed, or the secondary was wired late).
TEST(StreamingMirrorTest, SecondaryEngineWiredAfterFirstEnsureStillMirrors) {
  TwoEngineFixture f;
  const auto source = f.bootstrapSourceWithLockstepDataset("sensor");

  DatastoreSourceWriteHost source_impl(f.primary, source);
  SourceWriteHostView writer(source_impl.raw());

  // Pre-populate the WriteCore cache while no secondary is wired — handles
  // land only on primary.
  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  ASSERT_EQ(f.secondary.getTopicStorage(topic.id), nullptr);

  // Wire the secondary. The next ensureTopic/ensureField (cache hit) must
  // retry the mirror.
  source_impl.setSecondaryEngine(&f.secondary);
  const auto topic_again = *writer.ensureTopic("imu");
  EXPECT_EQ(topic_again.id, topic.id);
  EXPECT_NE(f.secondary.getTopicStorage(topic.id), nullptr);

  const auto field_again = *writer.ensureField(topic_again, "ax", PrimitiveType::kFloat32);
  EXPECT_EQ(field_again.id, field.id);
  const auto* sec_storage = f.secondary.getTopicStorage(topic.id);
  ASSERT_NE(sec_storage, nullptr);
  ASSERT_FALSE(sec_storage->columnDescriptors().empty());
  EXPECT_EQ(sec_storage->columnDescriptors()[0].field_path, "ax");
  EXPECT_EQ(sec_storage->columnDescriptors()[0].field_id, field.id);
}

// DataEngine::createTopicField directly: optional id sentinel, idempotency, type mismatch, non-dense id rejection.
TEST(StreamingMirrorTest, CreateTopicFieldOptionalSentinelHandlesIdZero) {
  DataEngine engine;
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl{engine, object_store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
  const auto source = *toolbox.createDataSource("sensor");

  // Create a topic + a single field to occupy FieldId 0.
  DatastoreSourceWriteHost host(engine, source);
  SourceWriteHostView writer(host.raw());
  const auto topic = *writer.ensureTopic("imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  EXPECT_EQ(field.id, 0U);

  // Idempotent re-mirror: same (name, type) returns existing id.
  auto same = engine.createTopicField(topic.id, "ax", PrimitiveType::kFloat32, std::optional<FieldId>{0U});
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(*same, 0U);

  // Mismatched name with requested_id=0 must fail (would create a second field
  // with non-dense id 0).
  auto clash = engine.createTopicField(topic.id, "different", PrimitiveType::kFloat32, std::optional<FieldId>{0U});
  EXPECT_FALSE(clash.has_value()) << "requested_id=0 for a new field name must fail (non-dense)";

  // Type mismatch on an existing field must fail.
  auto type_clash = engine.createTopicField(topic.id, "ax", PrimitiveType::kInt32);
  EXPECT_FALSE(type_clash.has_value()) << "type mismatch on existing field must fail";

  // Auto-assign (default std::nullopt) gets the next dense id.
  auto auto_next = engine.createTopicField(topic.id, "ay", PrimitiveType::kFloat32);
  ASSERT_TRUE(auto_next.has_value());
  EXPECT_EQ(*auto_next, 1U);

  // Forcing a non-dense id (e.g. 5 when next dense is 2) must fail.
  auto non_dense = engine.createTopicField(topic.id, "az", PrimitiveType::kFloat32, std::optional<FieldId>{5U});
  EXPECT_FALSE(non_dense.has_value()) << "non-dense requested_id must fail";

  // Topic-not-found must fail.
  auto bad_topic = engine.createTopicField(999, "x", PrimitiveType::kFloat32);
  EXPECT_FALSE(bad_topic.has_value()) << "unknown topic id must fail";
}

}  // namespace
}  // namespace PJ
