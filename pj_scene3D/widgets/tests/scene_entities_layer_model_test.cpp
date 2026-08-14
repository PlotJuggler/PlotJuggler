// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Model-path tests for SceneEntitiesLayer (ModelPrimitive / glTF meshes):
// stateful entity replay (replace-by-id, deletions, lifetime expiry) and the
// TF × pose × scale draw-call composition. The marker path is GL-rendered and
// covered by scene_entities_decode_test at the core layer.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/time.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/render_pass.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

// Counts parseObject invocations so a test can prove forward playback parses each
// stored batch O(1) times (incremental) rather than O(history) per frame.
std::atomic<int> g_parse_count{0};

// Rebind-test parse counters. vtableWithCreate() requires a non-capturing create
// function, so the counting parsers reach these via file scope.
std::atomic<int> g_first_scene_parser_calls{0};
std::atomic<int> g_second_scene_parser_calls{0};

// Object-construction body shared by every scene-entities mock parser in this
// file: deserialize the payload into a SceneEntities batch and wrap it as an
// ObjectRecord with no per-record timestamp (the store entry timestamp governs).
// The per-parse counter bump is supplied by CountingObjectParser, not here.
PJ::Expected<PJ::sdk::ObjectRecord> emitSceneEntities(PJ::Timestamp /*ts*/, PJ::sdk::PayloadView payload) {
  auto decoded = PJ::deserializeSceneEntities(payload.bytes.data(), payload.bytes.size());
  if (!decoded.has_value()) {
    return PJ::unexpected(std::move(decoded).error());
  }
  return PJ::sdk::ObjectRecord{
      .ts = std::nullopt,
      .object = PJ::sdk::BuiltinObject{std::move(*decoded)},
  };
}

PJ::ObjectTopicId registerTopic(PJ::SessionManager& session) {
  return registerObjectTopic(session, "/scene_entities");
}

void registerParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  auto parser = makeBoundHandle("scene_entities", []() noexcept -> void* {
    return new CountingObjectParser(
        "scene_entities", PJ::sdk::BuiltinObjectType::kSceneEntities, &g_parse_count, &emitSceneEntities);
  });
  session.registerObjectTopicParser(topic_id, std::move(parser));
}

pj::scene3d::Scene3DLayerContext makeContext(PJ::SessionManager& session) {
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  return ctx;
}

void pushSceneEntities(
    PJ::SessionManager& session, PJ::ObjectTopicId topic_id, PJ::Timestamp store_ts,
    const PJ::sdk::SceneEntities& scene_entities) {
  ASSERT_TRUE(session.objectStore().pushOwned(topic_id, store_ts, PJ::serializeSceneEntities(scene_entities)));
}

PJ::sdk::ModelPrimitive makeModelPrimitive() {
  PJ::sdk::ModelPrimitive primitive;
  primitive.scale = {.x = 1.0, .y = 1.0, .z = 1.0};
  primitive.media_type = "model/gltf-binary";
  primitive.data = {0x67, 0x6c, 0x54, 0x46};
  return primitive;
}

PJ::sdk::SceneEntity makeEntity(std::string id, PJ::Timestamp timestamp, std::string frame_id = "base_link") {
  PJ::sdk::SceneEntity entity;
  entity.id = std::move(id);
  entity.timestamp = timestamp;
  entity.frame_id = std::move(frame_id);
  entity.models.push_back(makeModelPrimitive());
  return entity;
}

PJ::sdk::SceneEntities batchWithEntities(std::vector<PJ::sdk::SceneEntity> entities) {
  PJ::sdk::SceneEntities batch;
  batch.entities = std::move(entities);
  return batch;
}

PJ::sdk::SceneEntities batchWithDeletions(std::vector<PJ::sdk::SceneEntityDeletion> deletions) {
  PJ::sdk::SceneEntities batch;
  batch.deletions = std::move(deletions);
  return batch;
}

glm::mat4 poseToMat4(const PJ::sdk::Pose& pose) {
  const glm::quat q(
      static_cast<float>(pose.orientation.w), static_cast<float>(pose.orientation.x),
      static_cast<float>(pose.orientation.y), static_cast<float>(pose.orientation.z));
  const glm::mat4 rot = glm::mat4_cast(q);
  const glm::mat4 trans = glm::translate(
      glm::mat4(1.0f), glm::vec3(
                           static_cast<float>(pose.position.x), static_cast<float>(pose.position.y),
                           static_cast<float>(pose.position.z)));
  return trans * rot;
}

void expectMatrixNear(const glm::mat4& actual, const glm::mat4& expected) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(actual[c][r], expected[c][r], 1e-5f) << "at column " << c << ", row " << r;
    }
  }
}

// Point default-constructed QSettings (which the layer's remote-fetch gate
// reads) at a throwaway INI file under a temp dir, so these tests neither read
// nor pollute the developer's real settings.
void isolateSettings() {
  static QTemporaryDir settings_dir;
  ASSERT_TRUE(settings_dir.isValid());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QCoreApplication::setOrganizationName(u"pj_scene3d_tests"_s);
  QCoreApplication::setApplicationName(u"scene_entities_layer_model_test"_s);
}

PJ::sdk::SceneEntity makeUrlEntity(std::string id, PJ::Timestamp timestamp, std::string url, std::string media_type) {
  PJ::sdk::SceneEntity entity;
  entity.id = std::move(id);
  entity.timestamp = timestamp;
  entity.frame_id = "base_link";
  PJ::sdk::ModelPrimitive primitive;
  primitive.scale = {.x = 1.0, .y = 1.0, .z = 1.0};
  primitive.media_type = std::move(media_type);
  primitive.url = std::move(url);
  entity.models.push_back(std::move(primitive));
  return entity;
}

}  // namespace

// Resolve-before-memo regression: an unchanged active batch must skip on
// metadata alone WITHOUT resolving payload bytes — a resolve is a cold refetch
// (file read + whole-chunk decompress on the GUI thread) once the
// ResidentPayloadPool has evicted the entry's seed. The store's warm latestAt
// cache would mask a redundant resolve, so it is reset via an out-of-order push
// stamped ABOVE the playhead (below it, the model path's retroactive-ingest
// guard would legitimately rebuild and re-resolve).
TEST(SceneEntitiesLayerModelTest, UnchangedActiveBatchDoesNotResolvePayload) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(
      session.objectStore(), topic_id, 10, PJ::serializeSceneEntities(batchWithEntities({makeEntity("car", 10)})),
      fetch_count);
  pushSceneEntities(session, topic_id, 100, batchWithEntities({makeEntity("truck", 100)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(20));  // active batch: the ts=10 entry
  ASSERT_EQ(layer.currentEntities().size(), 1u);
  const int fetches_after_first_render = *fetch_count;
  const int parses_after_first_render = g_parse_count.load();
  ASSERT_GT(fetches_after_first_render, 0);

  // Bust the warm latestAt cache: ts=50 is out-of-order relative to ts=100 (so
  // the insert resets the cache) but stays ABOVE the playhead, so neither the
  // render gate nor the model fold has any reason to touch it.
  auto future_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(
      session.objectStore(), topic_id, 50, PJ::serializeSceneEntities(batchWithEntities({makeEntity("bike", 50)})),
      future_fetch_count);

  layer.setTrackerTime(PJ::fromRaw(21));  // same active batch -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_first_render) << "unchanged batch resolved payload bytes on a later tick";
  EXPECT_EQ(g_parse_count.load(), parses_after_first_render);
  EXPECT_EQ(*future_fetch_count, 0) << "the not-yet-reached batch must not be resolved at all";
}

TEST(SceneEntitiesLayerModelTest, AccumulatesSnapshotsAndReplacesMatchingEntityId) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("car", 10, "old_frame")}));
  pushSceneEntities(session, topic_id, 20, batchWithEntities({makeEntity("car", 20, "new_frame")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(25));

  ASSERT_EQ(layer.currentEntities().size(), 1u);
  const auto it = layer.currentEntities().find("car");
  ASSERT_NE(it, layer.currentEntities().end());
  EXPECT_EQ(it->second.timestamp, 20);
  EXPECT_EQ(it->second.frame_id, "new_frame");
}

TEST(SceneEntitiesLayerModelTest, MatchingIdDeletionRemovesTargetEntity) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("delete_me", 10), makeEntity("keep_me", 10)}));
  PJ::sdk::SceneEntityDeletion deletion;
  deletion.type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId;
  deletion.timestamp = 20;
  deletion.id = "delete_me";
  pushSceneEntities(session, topic_id, 20, batchWithDeletions({deletion}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(25));

  EXPECT_EQ(layer.currentEntities().count("delete_me"), 0u);
  EXPECT_EQ(layer.currentEntities().count("keep_me"), 1u);
}

TEST(SceneEntitiesLayerModelTest, LifetimeDropsEntityAfterExpiry) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity entity = makeEntity("short_lived", 10);
  entity.lifetime_ns = 5;
  pushSceneEntities(session, topic_id, 10, batchWithEntities({entity}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(15));
  EXPECT_EQ(layer.currentEntities().count("short_lived"), 1u);

  layer.setTrackerTime(PJ::fromRaw(16));
  EXPECT_TRUE(layer.currentEntities().empty());
}

// The dual-clock regression guard. Under live streaming the ObjectStore entry is
// host-stamped (tracker clock) while entity.timestamp keeps the sensor epoch; expiry
// must anchor on the ingest timestamp, not entity.timestamp — otherwise a finite
// lifetime expires the instant the entity is folded (the bug that hid the streamed car
// mesh). Pre-fix this failed at the FIRST assert: 1000 + 50 = 1050 < 1'000'040.
TEST(SceneEntitiesLayerModelTest, LifetimeAnchorsOnStoreTimeNotEntityTimestamp) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity entity = makeEntity("streamed", /*timestamp=*/1'000);  // sensor epoch, far in the past
  entity.lifetime_ns = 50;
  pushSceneEntities(session, topic_id, /*store_ts=*/1'000'000, batchWithEntities({entity}));  // host-clock anchor

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(1'000'040));  // within [anchor, anchor + lifetime) = [1'000'000, 1'000'050)
  EXPECT_EQ(layer.currentEntities().count("streamed"), 1u);

  layer.setTrackerTime(PJ::fromRaw(1'000'051));  // past anchor + lifetime
  EXPECT_TRUE(layer.currentEntities().empty());
}

// Guards CachedSnapshot::store_ns: a backward scrub rebuilds the entity map by re-folding
// decoded batches from the cache (no re-parse), and must restore each entity's ingest-time
// anchor from store_ns. If the anchor were not cached, the re-fold would fall back to a
// wrong epoch and "keep" (anchored at store_ts 2000 ⇒ expires 2100) would vanish at 2060.
TEST(SceneEntitiesLayerModelTest, CacheHitReFoldRestoresStoreAnchor) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity keep = makeEntity("keep", /*timestamp=*/1'000);
  keep.lifetime_ns = 100;  // anchored at store_ts 2000 ⇒ expires at 2100
  pushSceneEntities(session, topic_id, /*store_ts=*/2'000, batchWithEntities({keep}));
  pushSceneEntities(session, topic_id, /*store_ts=*/2'050, batchWithEntities({makeEntity("other", 1'001)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(2'060));  // play forward past both batches: caches both
  ASSERT_EQ(layer.currentEntities().count("keep"), 1u);
  layer.setTrackerTime(PJ::fromRaw(2'500));  // overshoot so the next move is a backward full rebuild

  g_parse_count.store(0, std::memory_order_relaxed);
  layer.setTrackerTime(PJ::fromRaw(2'060));              // scrub back → rebuild re-folds from cache
  EXPECT_EQ(layer.currentEntities().count("keep"), 1u);  // restored anchor 2000, NOT entity.timestamp 1000
  EXPECT_LE(g_parse_count.load(std::memory_order_relaxed), 2)
      << "backward scrub re-parsed the history instead of re-folding from the cache";

  layer.setTrackerTime(PJ::fromRaw(2'101));  // just past the restored expiry boundary (2000 + 100)
  EXPECT_EQ(layer.currentEntities().count("keep"), 0u);
}

// lifetime_ns == 0 is the SceneEntity contract's "persist until replaced/deleted"
// sentinel: the entity must survive arbitrarily far past its anchor.
TEST(SceneEntitiesLayerModelTest, ZeroLifetimeNeverExpires) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity entity = makeEntity("forever", /*timestamp=*/10);
  entity.lifetime_ns = 0;  // never expires
  pushSceneEntities(session, topic_id, /*store_ts=*/10, batchWithEntities({entity}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(10'000'000));
  EXPECT_EQ(layer.currentEntities().count("forever"), 1u);
}

// A negative lifetime_ns means the entity is already past expiry at its anchor; the
// overflow-safe branch must drop it for any tracker time at-or-after the anchor.
TEST(SceneEntitiesLayerModelTest, NegativeLifetimeExpiresImmediately) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity entity = makeEntity("doomed", /*timestamp=*/10);
  entity.lifetime_ns = -5;
  pushSceneEntities(session, topic_id, /*store_ts=*/100, batchWithEntities({entity}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(100));  // anchor + (-5) = 95 < 100 ⇒ expired
  EXPECT_TRUE(layer.currentEntities().empty());
}

// The fix re-anchored EXPIRY on the ingest timestamp but deliberately left DELETION
// gating on entity.timestamp (deletions carry sensor-epoch stamps). This pins that
// asymmetry: a future "make it consistent" refactor that switched the gate to the anchor
// would wrongly erase "survivor" (anchor 1400 <= 1500) and fail here.
TEST(SceneEntitiesLayerModelTest, MatchingIdDeletionGateUsesEntityTimestampNotAnchor) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  // Anchors (store_ts) and entity.timestamps deliberately disagree across the deletion
  // boundary 1500: survivor's ts is past it, its anchor is before it.
  pushSceneEntities(
      session, topic_id, /*store_ts=*/1'400, batchWithEntities({makeEntity("survivor", /*timestamp=*/1'600)}));
  pushSceneEntities(
      session, topic_id, /*store_ts=*/2'000, batchWithEntities({makeEntity("target", /*timestamp=*/1'000)}));
  PJ::sdk::SceneEntityDeletion del_target;
  del_target.type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId;
  del_target.timestamp = 1'500;
  del_target.id = "target";
  PJ::sdk::SceneEntityDeletion del_survivor = del_target;
  del_survivor.id = "survivor";
  pushSceneEntities(session, topic_id, /*store_ts=*/3'000, batchWithDeletions({del_target, del_survivor}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(3'500));
  EXPECT_EQ(layer.currentEntities().count("target"), 0u);    // entity.ts 1000 <= 1500 ⇒ deleted
  EXPECT_EQ(layer.currentEntities().count("survivor"), 1u);  // entity.ts 1600 > 1500 ⇒ survives (anchor 1400 unused)
}

// The lockstep invariant under churn: deleting an entity drops its anchor (eraseEntity),
// and re-adding it writes a FRESH anchor (applySnapshot upsert). A stale anchor (100) would
// expire "blink" at 1100 and lose it by 5500; the refreshed anchor (5000) keeps it to 6000.
TEST(SceneEntitiesLayerModelTest, ReAddAfterDeletionRefreshesAnchor) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity first = makeEntity("blink", /*timestamp=*/10);
  first.lifetime_ns = 1'000;  // anchor 100 ⇒ would expire at 1100
  pushSceneEntities(session, topic_id, /*store_ts=*/100, batchWithEntities({first}));
  PJ::sdk::SceneEntityDeletion del;
  del.type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId;
  del.timestamp = 20;  // entity.ts 10 <= 20 ⇒ erased
  del.id = "blink";
  pushSceneEntities(session, topic_id, /*store_ts=*/200, batchWithDeletions({del}));
  PJ::sdk::SceneEntity again = makeEntity("blink", /*timestamp=*/30);
  again.lifetime_ns = 1'000;  // fresh anchor 5000 ⇒ expires at 6000
  pushSceneEntities(session, topic_id, /*store_ts=*/5'000, batchWithEntities({again}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  layer.setTrackerTime(PJ::fromRaw(5'500));  // alive only if the anchor was refreshed to 5000
  EXPECT_EQ(layer.currentEntities().count("blink"), 1u);

  layer.setTrackerTime(PJ::fromRaw(6'001));  // past 5000 + 1000
  EXPECT_TRUE(layer.currentEntities().empty());
}

// Forward playback folds only newly-appended batches (incremental) instead of
// re-parsing the whole history every frame. This must produce exactly the same
// entity state a full rebuild does — across replace-by-id, deletions, and new
// entities. Layer A steps forward (incremental); layer B overshoots then scrubs
// back (forcing a genuine full rebuild at the same playhead); the two must match.
TEST(SceneEntitiesLayerModelTest, IncrementalForwardReplayMatchesFullRebuild) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(
      session, topic_id, 10, batchWithEntities({makeEntity("car", 10, "old_frame"), makeEntity("ped", 10)}));
  pushSceneEntities(session, topic_id, 20, batchWithEntities({makeEntity("car", 20, "new_frame")}));
  PJ::sdk::SceneEntityDeletion deletion;
  deletion.type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId;
  deletion.timestamp = 30;
  deletion.id = "ped";
  pushSceneEntities(session, topic_id, 30, batchWithDeletions({deletion}));
  pushSceneEntities(session, topic_id, 40, batchWithEntities({makeEntity("truck", 40)}));

  const auto snapshot = [](const pj::scene3d::SceneEntitiesLayer& layer) {
    std::map<std::string, std::pair<PJ::Timestamp, std::string>> out;
    for (const auto& [id, entity] : layer.currentEntities()) {
      out[id] = {entity.timestamp, entity.frame_id};
    }
    return out;
  };

  // A: forward stepping → each step folds only the freshly-appended batch.
  pj::scene3d::SceneEntitiesLayer incremental_layer(topic_id, u"/scene_entities"_s);
  const auto ctx_a = makeContext(session);
  ASSERT_TRUE(incremental_layer.attach(ctx_a));
  for (int64_t t : {15, 25, 35, 45}) {
    incremental_layer.setTrackerTime(PJ::fromRaw(t));
  }
  const auto incremental = snapshot(incremental_layer);

  // B: overshoot to 60, then scrub back to 45 → backward jump forces a full rebuild.
  pj::scene3d::SceneEntitiesLayer full_layer(topic_id, u"/scene_entities"_s);
  const auto ctx_b = makeContext(session);
  ASSERT_TRUE(full_layer.attach(ctx_b));
  full_layer.setTrackerTime(PJ::fromRaw(60));
  full_layer.setTrackerTime(PJ::fromRaw(45));
  const auto full = snapshot(full_layer);

  EXPECT_EQ(incremental, full);
  // Non-tautological: the folded state is the expected one.
  EXPECT_EQ(incremental.count("ped"), 0u) << "matching-id deletion not applied incrementally";
  ASSERT_EQ(incremental.count("car"), 1u);
  EXPECT_EQ(incremental.at("car").second, "new_frame") << "replace-by-id not applied incrementally";
  EXPECT_EQ(incremental.count("truck"), 1u) << "later-appended entity missed incrementally";
}

// Perf guard: a backward jump must re-fold entities from the decoded-batch cache
// WITHOUT re-parsing the history (the re-parse of heavy embedded models is what
// made big backward scrubs hitch). After playing forward to the end (which caches
// every batch), jumping back must add ~no model parses — only renderAt's single
// marker decode for the newly-active batch.
TEST(SceneEntitiesLayerModelTest, BackwardJumpRefoldsFromCacheWithoutReparsing) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);

  constexpr int kN = 10;
  for (int i = 0; i < kN; ++i) {
    const PJ::Timestamp ts = static_cast<PJ::Timestamp>(10 * (i + 1));
    pushSceneEntities(session, topic_id, ts, batchWithEntities({makeEntity("e" + std::to_string(i), ts)}));
  }

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(10 * kN + 5));  // play forward to the end: caches every batch
  ASSERT_EQ(layer.currentEntities().size(), static_cast<std::size_t>(kN));

  g_parse_count.store(0, std::memory_order_relaxed);
  layer.setTrackerTime(PJ::fromRaw(55));  // big jump back → full rebuild, but all batches are cached

  // Mid-history state is correct (5 entities: e0..e4 at ts 10..50).
  EXPECT_EQ(layer.currentEntities().size(), 5u);
  const int parses = g_parse_count.load(std::memory_order_relaxed);
  // Cache hits ⇒ the rebuild re-parses nothing; only renderAt decodes the one
  // newly-active marker batch. Without the cache this would be ~5 model re-parses.
  EXPECT_LE(parses, 2) << "backward jump re-parsed the history instead of using the cache; got " << parses;
}

// Perf guard: forward playback must parse each stored batch ~once (incremental),
// not re-parse the whole history every step. With N entries stepped through one by
// one, the old full-rebuild-per-frame path did ~N*(N+1)/2 parses; the incremental
// path does ~N. We assert the total stays linear (<= 2N) — a regression to the
// quadratic behavior (which pegged the CPU on heavy embedded models) would blow well past it.
TEST(SceneEntitiesLayerModelTest, ForwardPlaybackParsesEachBatchAboutOnce) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);

  constexpr int kN = 20;
  for (int i = 0; i < kN; ++i) {
    const PJ::Timestamp ts = static_cast<PJ::Timestamp>(10 * (i + 1));
    pushSceneEntities(session, topic_id, ts, batchWithEntities({makeEntity("e" + std::to_string(i), ts)}));
  }

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  g_parse_count.store(0, std::memory_order_relaxed);
  for (int i = 0; i < kN; ++i) {
    layer.setTrackerTime(PJ::fromRaw(10 * (i + 1) + 5));  // step just past each batch
  }

  EXPECT_EQ(layer.currentEntities().size(), static_cast<std::size_t>(kN)) << "distinct entities should accumulate";
  const int parses = g_parse_count.load(std::memory_order_relaxed);
  EXPECT_LE(parses, 2 * kN) << "forward playback is re-parsing history (quadratic); got " << parses << " parses for "
                            << kN << " batches (old path: ~" << (kN * (kN + 1)) / 2 << ")";
}

TEST(SceneEntitiesLayerModelTest, ForwardReplayKeepsApplyingAfterRetentionRenumbersIndices) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  session.objectStore().setRetentionBudget(topic_id, PJ::RetentionBudget{.time_window_ns = 25, .max_memory_bytes = 0});
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("e10", 10)}));
  pushSceneEntities(session, topic_id, 20, batchWithEntities({makeEntity("e20", 20)}));
  pushSceneEntities(session, topic_id, 30, batchWithEntities({makeEntity("e30", 30)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(35));
  EXPECT_EQ(layer.currentEntities().count("e30"), 1u);

  // Retention evicts the first store entry, so current deque indices renumber:
  // uid 2 becomes index 0, uid 3 index 1, uid 4 index 2. The replay cursor must
  // still notice and apply uid 4 instead of comparing the old index 2 to the new
  // latest index 2 and skipping it.
  pushSceneEntities(session, topic_id, 40, batchWithEntities({makeEntity("e40", 40)}));
  ASSERT_EQ(session.objectStore().entryCount(topic_id), 3u);

  layer.setTrackerTime(PJ::fromRaw(45));

  EXPECT_EQ(layer.currentEntities().count("e40"), 1u) << "new retained entry was skipped after index renumbering";
}

TEST(SceneEntitiesLayerModelTest, SameTimestampAppendStillAdvancesBySequentialUid) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("first", 10)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(10));
  EXPECT_EQ(layer.currentEntities().count("first"), 1u);

  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("second", 10)}));
  layer.setTrackerTime(PJ::fromRaw(10));

  EXPECT_EQ(layer.currentEntities().count("second"), 1u)
      << "same-timestamp append must not be skipped by a time-only replay guard";
}

// Regression (out-of-order ingest): once the ObjectStore accepts a late snapshot
// whose timestamp lands BEFORE an already-stored newer one, UID(arrival) order
// diverges from timestamp order. applyEntriesAfter bounds its replay by
// `uid <= latestAt(t)->uid`, using the UID as a stand-in for "timestamp <= t" —
// the exact assumption out-of-order ingest breaks (the same class the occupancy
// layer fixed via maxUidAtOrBefore/rangeByTime). It has two symptoms, both pinned
// here: a FUTURE snapshot leaks in, and the out-of-order (older) snapshot is
// dropped once a newer entry becomes the cursor target.
TEST(SceneEntitiesLayerModelTest, OutOfOrderSnapshotReplayHonorsTimestampNotArrivalUid) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  // Arrival order (== UID order): a@100, b@300, then c@200 out of order (newest UID,
  // middle timestamp). The store sorts by timestamp: [a@100, c@200, b@300].
  pushSceneEntities(session, topic_id, 100, batchWithEntities({makeEntity("a", 100)}));
  pushSceneEntities(session, topic_id, 300, batchWithEntities({makeEntity("b", 300)}));
  pushSceneEntities(session, topic_id, 200, batchWithEntities({makeEntity("c", 200)}));
  ASSERT_EQ(session.objectStore().entryCount(topic_id), 3u);

  // At t=250 the snapshots whose timestamp is <= 250 are {a, c}; b@300 is in the
  // future and must NOT be shown. Pre-fix: latestAt(250) resolves to c (the OOO
  // entry, which holds the LARGEST UID), so the walk `uid <= c.uid` also folds b.
  {
    pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
    const auto ctx = makeContext(session);
    ASSERT_TRUE(layer.attach(ctx));
    layer.setTrackerTime(PJ::fromRaw(250));
    EXPECT_EQ(layer.currentEntities().count("a"), 1u);
    EXPECT_EQ(layer.currentEntities().count("c"), 1u);
    EXPECT_EQ(layer.currentEntities().count("b"), 0u) << "future snapshot @300 leaked into the state at t=250";
  }

  // At t=400 all three snapshots (timestamp <= 400) must be present. Pre-fix:
  // latestAt(400) resolves to b, whose UID is SMALLER than c's, so the walk
  // `uid <= b.uid` stops before c and the out-of-order @200 snapshot is dropped.
  {
    pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
    const auto ctx = makeContext(session);
    ASSERT_TRUE(layer.attach(ctx));
    layer.setTrackerTime(PJ::fromRaw(400));
    EXPECT_EQ(layer.currentEntities().count("a"), 1u);
    EXPECT_EQ(layer.currentEntities().count("b"), 1u);
    EXPECT_EQ(layer.currentEntities().count("c"), 1u) << "out-of-order snapshot @200 was dropped at t=400";
  }
}

// UID allocation is process-global, so another topic's pushes leave large gaps in
// the scene topic's UID sequence. The timestamp-window fold (rangeByTime) must be
// unaffected by those gaps, and the result identical to the dense-UID case in both
// directions.
TEST(SceneEntitiesLayerModelTest, ReplayStepsSparseUidsFromInterleavedTopics) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  auto other = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = 1, .topic_name = "/other", .metadata_json = "{}"});
  ASSERT_TRUE(other.has_value());

  constexpr int kInterleavedPerBatch = 50;
  for (int i = 0; i < 3; ++i) {
    const PJ::Timestamp ts = static_cast<PJ::Timestamp>(10 * (i + 1));
    for (int k = 0; k < kInterleavedPerBatch; ++k) {
      ASSERT_TRUE(session.objectStore().pushOwned(*other, ts, {0x00}).has_value());
    }
    pushSceneEntities(session, topic_id, ts, batchWithEntities({makeEntity("e" + std::to_string(i), ts)}));
  }

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  for (int64_t t : {15, 25, 35}) {
    layer.setTrackerTime(PJ::fromRaw(t));
  }
  EXPECT_EQ(layer.currentEntities().size(), 3u);

  layer.setTrackerTime(PJ::fromRaw(15));  // backward jump across the gaps

  EXPECT_EQ(layer.currentEntities().size(), 1u);
  EXPECT_EQ(layer.currentEntities().count("e0"), 1u);
}

// Scrubbing within one batch's time window keeps the same active marker batch,
// so renderAt's UID guard skips its per-tick repaint. A lifetime expiry changes
// the model state anyway and must request its own frame, or the dead entity
// lingers on screen until some other layer repaints.
TEST(SceneEntitiesLayerModelTest, LifetimeExpiryWithinSameBatchRequestsRepaint) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  PJ::sdk::SceneEntity entity = makeEntity("fleeting", 10);
  entity.lifetime_ns = 20;
  pushSceneEntities(session, topic_id, 10, batchWithEntities({entity}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));
  ASSERT_EQ(layer.currentEntities().count("fleeting"), 1u);

  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::SceneEntitiesLayer::repaintRequested, [&repaints] { ++repaints; });
  layer.setTrackerTime(PJ::fromRaw(45));  // same batch active; entity expires at 30

  EXPECT_TRUE(layer.currentEntities().empty());
  EXPECT_GT(repaints, 0) << "lifetime expiry did not request a repaint";
}

// A dataset reload (SessionManager::replaceDataset) swaps the store generation
// in place and re-attaches layers WITHOUT an intervening detach(). attach()
// must reset every prior-generation replay/bootstrap artifact: stale
// time-range / source-frame / replay-cursor state must neither leak out of the
// layer's accessors nor anchor/skip the new generation's replay. Regression:
// without the reset, a reload that left the topic empty (streaming reload
// before the first new message) kept reporting the OLD dataset's time range
// and source frame.
TEST(SceneEntitiesLayerModelTest, DetachlessReattachAfterDatasetReplaceResetsStaleState) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("old_car", 10, "old_frame")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));
  ASSERT_EQ(layer.currentEntities().count("old_car"), 1u);
  ASSERT_EQ(PJ::toRaw(layer.timeRange().min), 10);
  ASSERT_EQ(layer.sourceFrame(), u"old_frame"_s);

  // Reload: stage the same topic name EMPTY (the new generation's first message
  // has not arrived yet) and run the real in-place replace. The primary
  // ObjectTopicId stays stable; the old entries are gone.
  PJ::DataEngine staged_engine;
  PJ::ObjectStore staged_store;
  auto staged_topic = staged_store.registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = 2, .topic_name = "/scene_entities", .metadata_json = "{}"});
  ASSERT_TRUE(staged_topic.has_value());
  auto staged_parser = makeBoundHandle("scene_entities", []() noexcept -> void* {
    return new CountingObjectParser(
        "scene_entities", PJ::sdk::BuiltinObjectType::kSceneEntities, &g_parse_count, &emitSceneEntities);
  });
  std::vector<std::pair<PJ::ObjectTopicId, std::unique_ptr<PJ::MessageParserHandle>>> staged_parsers;
  staged_parsers.emplace_back(*staged_topic, std::move(staged_parser));
  session.replaceDataset(staged_engine, staged_store, /*staged_id=*/2, /*primary_id=*/1, std::move(staged_parsers));
  ASSERT_EQ(session.objectStore().entryCount(topic_id), 0u);

  // Re-attach without detach (the reload path). Nothing of the previous
  // generation may survive: empty time range, frames, and entity state.
  ASSERT_TRUE(layer.attach(ctx));
  const auto empty_range = layer.timeRange();
  EXPECT_GT(empty_range.min, empty_range.max) << "stale time range leaked across re-attach";
  EXPECT_TRUE(layer.sourceFrame().isEmpty()) << "stale source frame leaked across re-attach";
  EXPECT_TRUE(layer.fallbackFrames().isEmpty()) << "stale fallback frames leaked across re-attach";
  EXPECT_TRUE(layer.currentEntities().empty());

  // The new generation's first message arrives: the layer must rebuild from it
  // rather than trust any prior-generation cursor.
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("new_car", 10, "new_frame")}));
  layer.setTrackerTime(PJ::fromRaw(15));
  EXPECT_EQ(layer.currentEntities().count("new_car"), 1u) << "new-generation entry skipped after re-attach";
  EXPECT_EQ(layer.currentEntities().count("old_car"), 0u);
  EXPECT_EQ(layer.sourceFrame(), u"new_frame"_s);
}

// Regression (L.23): attach() must seed the initial decode when the topic's
// first sample sits at store timestamp 0 (ROS sim time commonly starts at t=0).
// The old `ts_first_ != 0` sentinel conflated "no data" with a legitimate t=0
// first sample, so the model state was never built at attach and the entity was
// invisible until the next scrub. Presence is now tracked via std::optional, so
// a t=0 first sample is seeded just like any other.
TEST(SceneEntitiesLayerModelTest, AttachSeedsFirstSampleAtTimestampZero) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  // Store timestamp 0 — the first (and only) sample lands exactly at the epoch.
  pushSceneEntities(session, topic_id, 0, batchWithEntities({makeEntity("sim_car", 0, "base_link")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  // No setTrackerTime() yet: attach() alone must have built the model state at
  // t=0, so the entity is present immediately.
  ASSERT_EQ(layer.currentEntities().count("sim_car"), 1u)
      << "attach() skipped the t=0 first sample (0 treated as a 'no data' sentinel)";
  EXPECT_EQ(layer.sourceFrame(), u"base_link"_s);
}

// Regression: a finished async ModelPrimitive mesh load must itself request the
// repaint that consumes it (QFutureWatcher -> pollMeshLoads). The app paints
// strictly on demand, so before the fix the result was only drained inside
// render() and the placeholder cube lingered until the user scrubbed. After
// attach() kicks the load, only the event loop is pumped here — no render() or
// setTrackerTime() — and repaintRequested must still fire.
TEST(SceneEntitiesLayerModelTest, MeshLoadCompletionRequestsRepaintWithoutRender) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("car", 10)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));  // kicks the async mesh load for "car"

  // Connect AFTER attach so synchronous emissions during attach can't count.
  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::SceneEntitiesLayer::repaintRequested, [&repaints] { ++repaints; });

  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "finished mesh load did not request a repaint (only render() would have consumed it)";
}

TEST(SceneEntitiesLayerModelTest, FrameCompositionAppliesTfPrimitivePoseAndScale) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);

  PJ::sdk::SceneEntity entity = makeEntity("car", 10, "base_link");
  auto& primitive = entity.models.front();
  primitive.pose.position = {.x = 1.0, .y = 2.0, .z = 3.0};
  const double half_angle = std::numbers::pi / 4.0;
  primitive.pose.orientation = {.x = 0.0, .y = 0.0, .z = std::sin(half_angle), .w = std::cos(half_angle)};
  primitive.scale = {.x = 2.0, .y = 3.0, .z = 4.0};
  primitive.override_color = true;
  primitive.color = {.r = 25, .g = 51, .b = 76, .a = 102};
  pushSceneEntities(session, topic_id, 10, batchWithEntities({entity}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(ctx.tf_buffer->setTransform(
      pj::scene3d::StampedTransform{
          .stamp = PJ::fromRaw(0),
          .parent_frame = "world",
          .child_frame = "base_link",
          .transform = pj::scene3d::Transform({10.0, 20.0, 30.0}, {1.0, 0.0, 0.0, 0.0}),
      }));
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(10));

  const std::string fixed_frame = "world";
  const pj::scene3d::FrameContext frame_ctx{*ctx.tf_buffer, fixed_frame, PJ::fromRaw(10)};
  const auto draws = layer.modelDrawCallsForFrame(frame_ctx);

  ASSERT_EQ(draws.size(), 1u);
  const glm::mat4 expected = glm::mat4(ctx.tf_buffer->lookupTransform("world", "base_link", PJ::fromRaw(10)).matrix()) *
                             poseToMat4(primitive.pose) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));
  expectMatrixNear(draws.front().model, expected);
  EXPECT_EQ(draws.front().kind, pj::scene3d::MeshRenderPass::GeometryKind::kMesh);
  EXPECT_FALSE(draws.front().use_vertex_color);
  EXPECT_NEAR(draws.front().color.r, 25.0f / 255.0f, 1e-6f);
  EXPECT_NEAR(draws.front().color.g, 51.0f / 255.0f, 1e-6f);
  EXPECT_NEAR(draws.front().color.b, 76.0f / 255.0f, 1e-6f);
  EXPECT_NEAR(draws.front().color.a, 102.0f / 255.0f, 1e-6f);
}

// H.8: a DATA-SUPPLIED http(s) model URL must not be fetched without the
// explicit opt-in (QSettings pj_scene3d/allow_remote_model_fetch, default off):
// opening a crafted dataset is an SSRF / beacon vector. The block must be
// recorded once (no per-tick re-check), surfaced through remoteFetchNotice(),
// and produce zero network egress — proven by a local listener that would see
// any connection attempt.
TEST(SceneEntitiesLayerModelTest, RemoteModelUrlIsBlockedWithoutOptIn) {
  isolateSettings();
  QSettings settings;
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, false);
  settings.sync();

  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  int connections = 0;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&connections]() { ++connections; });

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/model.glb";
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeUrlEntity("remote", 10, url, "model/gltf-binary")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  QString notice_from_signal;
  QObject::connect(
      &layer, &pj::scene3d::SceneEntitiesLayer::remoteFetchNoticeChanged,
      [&notice_from_signal](const QString& notice) { notice_from_signal = notice; });
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));

  EXPECT_TRUE(layer.remoteFetchNotice().contains("Remote model fetch is disabled"_L1))
      << layer.remoteFetchNotice().toStdString();
  EXPECT_TRUE(layer.remoteFetchNotice().contains("allow_remote_model_fetch"_L1));
  EXPECT_EQ(notice_from_signal, layer.remoteFetchNotice()) << "notice signal did not track the accessor";

  // Pump: even an asynchronous fetch would have to open a socket toward us.
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 300) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  EXPECT_EQ(connections, 0) << "blocked model URL still produced network egress";
}

// Counterpart of the consent gate: LOCAL model URLs (file:// or bare paths) are
// disk reads, not network egress, so they keep working with the opt-in off —
// now asynchronously (fetch + import resolve through the event loop and request
// their own repaint, never blocking attach()/render()).
TEST(SceneEntitiesLayerModelTest, LocalFileModelUrlLoadsWithoutOptIn) {
  isolateSettings();
  QSettings settings;
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, false);
  settings.sync();

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const QString fixture = QString(PJ_SCENE3D_FIXTURES_DIR) + u"/meshes/cube.stl"_s;
  pushSceneEntities(
      session, topic_id, 10,
      batchWithEntities(
          {makeUrlEntity("local", 10, QUrl::fromLocalFile(fixture).toString().toStdString(), "model/stl")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));  // kicks the async file fetch + import

  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::SceneEntitiesLayer::repaintRequested, [&repaints] { ++repaints; });

  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "async local-URL mesh load never landed";
  EXPECT_TRUE(layer.remoteFetchNotice().isEmpty())
      << "local file URL tripped the remote gate: " << layer.remoteFetchNotice().toStdString();
}

// A URL record exists BEFORE its bytes do (pending fetch, future default-
// invalid). pollMeshLoads — driven here by a sibling embedded-data record's
// completion watcher — must skip the pending record instead of calling
// result() on an invalid future (which would crash). The remote URL points at
// a never-responding local server, so the record is guaranteed still pending
// when the embedded record drains.
TEST(SceneEntitiesLayerModelTest, PendingUrlFetchRecordIsSkippedByPoll) {
  isolateSettings();
  QSettings settings;
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, true);
  settings.sync();

  QTcpServer server;  // accepts and never responds: the fetch stays in flight
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/slow.glb";
  pushSceneEntities(
      session, topic_id, 10,
      batchWithEntities({makeUrlEntity("pending", 10, url, "model/gltf-binary"), makeEntity("embedded", 10)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));  // URL record pending + embedded import kicked

  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::SceneEntitiesLayer::repaintRequested, [&repaints] { ++repaints; });

  // The embedded record's watcher fires pollMeshLoads while the URL record is
  // still future-less; surviving that poll (and emitting the repaint) is the test.
  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "embedded mesh record never drained";

  // Reset the opt-in so no other test inherits an enabled gate.
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, false);
  settings.sync();
}

// Default policy is ON: a data-supplied http(s) model URL is fetched without any
// opt-in (the consent gate is now opt-OUT). Proven by a local listener that sees
// the connection attempt, and by the notice NOT reporting a policy block.
TEST(SceneEntitiesLayerModelTest, RemoteModelUrlIsFetchedByDefault) {
  isolateSettings();
  QSettings settings;
  settings.remove(u"pj_scene3d/allow_remote_model_fetch"_s);  // exercise the built-in default
  settings.sync();

  QTcpServer server;  // accepts the connection (response irrelevant — we assert the attempt)
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  int connections = 0;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&connections]() { ++connections; });

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.serverPort()) + "/model.glb";
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeUrlEntity("remote", 10, url, "model/gltf-binary")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));

  EXPECT_FALSE(layer.remoteFetchNotice().contains("disabled"_L1))
      << "default must not block the fetch: " << layer.remoteFetchNotice().toStdString();

  QElapsedTimer timer;
  timer.start();
  while (connections == 0 && timer.elapsed() < 3000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  EXPECT_GT(connections, 0) << "default policy should attempt the remote model fetch";
}

// A failed remote fetch reports the offending URL and the underlying error in
// the notice (the config-widget status surface) — not a bare "see the log" count.
TEST(SceneEntitiesLayerModelTest, FailedRemoteFetchSurfacesUrlAndErrorInNotice) {
  isolateSettings();
  QSettings settings;
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, true);
  settings.sync();

  // Bind then release a port so the connection is refused immediately (fast,
  // deterministic — no 15 s transfer-timeout wait).
  quint16 port = 0;
  {
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    port = probe.serverPort();
  }

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/missing.glb";
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeUrlEntity("remote", 10, url, "model/gltf-binary")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  QString notice_from_signal;
  QObject::connect(
      &layer, &pj::scene3d::SceneEntitiesLayer::remoteFetchNoticeChanged,
      [&notice_from_signal](const QString& notice) { notice_from_signal = notice; });
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));

  QElapsedTimer timer;
  timer.start();
  while (layer.remoteFetchNotice().isEmpty() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  const QString notice = layer.remoteFetchNotice();
  EXPECT_TRUE(notice.contains("Failed to fetch"_L1)) << notice.toStdString();
  EXPECT_TRUE(notice.contains(QString::fromStdString(url)))
      << "the offending URL must appear: " << notice.toStdString();
  EXPECT_EQ(notice_from_signal, notice) << "notice signal did not track the accessor";

  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, false);  // quiet default for siblings
  settings.sync();
}

// A model whose bytes load but fail to PARSE (assimp rejects them) must also
// surface — today the notice ignores parse failures, leaving a silent grey cube.
// makeEntity embeds a 4-byte "glTF" stub that is not a valid GLB.
TEST(SceneEntitiesLayerModelTest, EmbeddedModelParseFailureSurfacesInNotice) {
  isolateSettings();
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("bad", 10)}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));

  QElapsedTimer timer;
  timer.start();
  while (layer.remoteFetchNotice().isEmpty() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_TRUE(layer.remoteFetchNotice().contains("Failed to load model"_L1)) << layer.remoteFetchNotice().toStdString();
}

// Deleting the entity that owns a failed model clears its load notice: the
// mesh-load record is pruned with the entity, so a stale "Failed to fetch" line
// (and the row warning it drives) does not linger after the marker is gone.
TEST(SceneEntitiesLayerModelTest, DeletingEntityClearsItsModelLoadNotice) {
  isolateSettings();
  QSettings settings;
  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, true);
  settings.sync();

  quint16 port = 0;
  {
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    port = probe.serverPort();  // released → connection refused → fast fetch failure
  }

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/car.glb";
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeUrlEntity("car", 10, url, "model/gltf-binary")}));
  PJ::sdk::SceneEntityDeletion del;
  del.type = PJ::sdk::SceneEntityDeletion::Type::kAll;
  del.timestamp = 20;
  pushSceneEntities(session, topic_id, 20, batchWithDeletions({del}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(10));  // kicks the fetch (which fails)

  QElapsedTimer timer;
  timer.start();
  while (layer.remoteFetchNotice().isEmpty() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  ASSERT_TRUE(layer.remoteFetchNotice().contains("Failed to fetch"_L1)) << layer.remoteFetchNotice().toStdString();

  layer.setTrackerTime(PJ::fromRaw(20));  // DELETEALL folds → entity (and its failed model) gone
  EXPECT_TRUE(layer.remoteFetchNotice().isEmpty())
      << "deleting the entity must clear its model-load notice: " << layer.remoteFetchNotice().toStdString();

  settings.setValue(u"pj_scene3d/allow_remote_model_fetch"_s, false);
  settings.sync();
}

// Contract pin (sibling of RobotModelLayerTest.ReloadSwapsParserWithoutTouchingStaleOne):
// SceneEntitiesLayer already resolves the parser binding per use (renderAt /
// rebuildModelStateAt fetch parserBindingForObjectTopic), so a same-file reload
// that re-registers the topic's parser slot must transparently rebind. This
// passes today and gates against a regression to a cached raw pointer.
TEST(SceneEntitiesLayerModelTest, ReloadSwapsParserWithoutTouchingStaleOne) {
  g_first_scene_parser_calls.store(0, std::memory_order_relaxed);
  g_second_scene_parser_calls.store(0, std::memory_order_relaxed);

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  session.registerObjectTopicParser(topic_id, makeBoundHandle("scene_entities", []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          "scene_entities", PJ::sdk::BuiltinObjectType::kSceneEntities,
                                          &g_first_scene_parser_calls, &emitSceneEntities);
                                    }));
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("car", 10, "old_frame")}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(15));
  ASSERT_EQ(layer.currentEntities().count("car"), 1u);
  EXPECT_GE(g_first_scene_parser_calls.load(std::memory_order_relaxed), 1);

  // Keep the first parser's memory readable so a stale-pointer regression would
  // be a deterministic wrong-parser hit rather than UB.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Reload: re-register the topic's parser under its stable id and push a new
  // sample. The next tracker tick must decode through the new parser.
  session.registerObjectTopicParser(topic_id, makeBoundHandle("scene_entities", []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          "scene_entities", PJ::sdk::BuiltinObjectType::kSceneEntities,
                                          &g_second_scene_parser_calls, &emitSceneEntities);
                                    }));
  pushSceneEntities(session, topic_id, 20, batchWithEntities({makeEntity("truck", 20, "new_frame")}));

  const int stale_calls_before = g_first_scene_parser_calls.load(std::memory_order_relaxed);
  layer.setTrackerTime(PJ::fromRaw(25));  // new sample active -> re-decode

  EXPECT_EQ(g_first_scene_parser_calls.load(std::memory_order_relaxed), stale_calls_before)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_scene_parser_calls.load(std::memory_order_relaxed), 1)
      << "layer did not rebind to the re-registered parser";
  EXPECT_EQ(layer.currentEntities().count("truck"), 1u);
}

// Regression (H.7): a single batch containing a kAll deletion PLUS replacement
// entities at the same timestamp must leave those entities present. The SDK
// contract says deletions remove PRIOR entities; Foxglove uses this pattern
// ("DELETEALL then ADD in one message") as the canonical scene republish.
// Before the fix, applySnapshot() upserted entities first so the kAll deletion
// (deletion.timestamp == entity.timestamp → `<=` matches) erased the just-added
// entities, leaving the model path with nothing to draw.
TEST(SceneEntitiesLayerModelTest, DeleteAllPlusEntitiesInSameBatchLeavesEntitiesPresent) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);

  // Batch 0: two prior-generation entities at t=10.
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("prior_a", 10), makeEntity("prior_b", 10)}));

  // Batch 1 at t=20: kAll deletion (ts=20) + two replacement entities (ts=20).
  // All timestamps are identical — this is the canonical DELETEALL+re-add
  // republish pattern that the `deletion.timestamp <= entity.timestamp` gate
  // was incorrectly matching after the old upsert-first order.
  PJ::sdk::SceneEntityDeletion kall;
  kall.type = PJ::sdk::SceneEntityDeletion::Type::kAll;
  kall.timestamp = 20;

  PJ::sdk::SceneEntities batch;
  batch.deletions = {kall};
  batch.entities = {makeEntity("new_a", 20), makeEntity("new_b", 20)};
  pushSceneEntities(session, topic_id, 20, batch);

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(25));

  // Prior entities must be gone; replacement entities must survive.
  EXPECT_EQ(layer.currentEntities().count("prior_a"), 0u) << "prior entity leaked past kAll deletion";
  EXPECT_EQ(layer.currentEntities().count("prior_b"), 0u) << "prior entity leaked past kAll deletion";
  EXPECT_EQ(layer.currentEntities().count("new_a"), 1u) << "same-batch replacement entity was deleted";
  EXPECT_EQ(layer.currentEntities().count("new_b"), 1u) << "same-batch replacement entity was deleted";
  EXPECT_EQ(layer.currentEntities().size(), 2u);
}

// Regression (H.7) second pin: a kAll deletion in batch N+1 must still erase
// batch N's entities (verifies the reorder did not break ordinary cross-batch
// deletion, which was already correct before the fix).
TEST(SceneEntitiesLayerModelTest, CrossBatchDeleteAllErasesOlderBatchEntities) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id);

  // Batch 0 at t=10: two entities.
  pushSceneEntities(session, topic_id, 10, batchWithEntities({makeEntity("e0", 10), makeEntity("e1", 10)}));

  // Batch 1 at t=20: kAll deletion only (no replacement entities).
  PJ::sdk::SceneEntityDeletion kall;
  kall.type = PJ::sdk::SceneEntityDeletion::Type::kAll;
  kall.timestamp = 20;
  pushSceneEntities(session, topic_id, 20, batchWithDeletions({kall}));

  pj::scene3d::SceneEntitiesLayer layer(topic_id, u"/scene_entities"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setTrackerTime(PJ::fromRaw(25));

  EXPECT_TRUE(layer.currentEntities().empty()) << "kAll deletion in a subsequent batch did not erase earlier entities";
}

// Custom main: QFutureWatcher/UrlFetcher tests need an event loop, and the
// QCoreApplication must die BEFORE exit handlers run — QtNetwork (loaded by the
// layer's UrlFetcher) registers global cleanup that a function-local-static app
// would outlive, crashing at exit (pj_marketplace's download_manager_test pattern).
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // The layer's UrlFetcher builds a QNetworkDiskCache; point it at a throwaway dir
  // (auto-removed at exit) so these tests never write into the real ~/.local/share.
  static QTemporaryDir model_cache_dir;
  qputenv("PJ_MODEL_CACHE_DIR", (model_cache_dir.path() + u"/models"_s).toUtf8());
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
