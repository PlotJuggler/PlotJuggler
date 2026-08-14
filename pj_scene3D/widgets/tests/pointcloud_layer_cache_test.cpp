// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PointCloudLayer sample-identity cache tests:
//  - a dataset reload (SessionManager::replaceDataset) swaps the store bytes
//    under a stable ObjectTopicId, so every sample-identity cache (decoded
//    cloud, failure memo, pushed sample) must reset — the new bytes must be
//    decoded, never served from pre-reload state;
//  - timestamp 0 is a valid stamp (sim-time datasets), not a "no data" sentinel:
//    attach() must render a first sample at t=0 and refreshNow() must not no-op;
//  - on a mixed raw/compressed topic, a stale in-flight compressed decode must
//    not clobber a newer raw sample once the tracker moved on (wanted_ gating).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/point_cloud";
// Samples stamped at/after this produce a CompressedPointCloud with a bogus
// codec id (decode always fails); earlier stamps produce a raw 1-point cloud.
constexpr PJ::Timestamp kCompressedStampThreshold = 1000;

std::atomic<int> g_primary_parser_calls{0};
std::atomic<int> g_staged_parser_calls{0};
std::atomic<int> g_zero_stamp_parser_calls{0};
std::atomic<int> g_mixed_parser_calls{0};
std::atomic<int> g_metadata_gate_parser_calls{0};

// Minimal dual-mode emit. The raw cloud is a single float32 xyz point over a
// static buffer (no anchor needed); at/after the threshold a CompressedPointCloud
// with a bogus codec id (decode always fails). Per-parse counting is handled by
// test::CountingObjectParser at the registration call site.
PJ::Expected<PJ::sdk::ObjectRecord> emitMixedCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  if (ts >= kCompressedStampThreshold) {
    static const uint8_t k_blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    PJ::sdk::CompressedPointCloud cloud;
    cloud.timestamp_ns = ts;
    cloud.frame_id = "lidar";
    cloud.format = "bogus";  // decodeCompressedPointCloud must fail on it
    cloud.data = PJ::Span<const uint8_t>(k_blob, sizeof(k_blob));
    return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
  }
  static const float k_point[3] = {1.0f, 2.0f, 3.0f};
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 12;
  cloud.row_step = 12;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"y", 4, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, PJ::sdk::PointField::Datatype::kFloat32, 1}};
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(k_point), sizeof(k_point));
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

PJ::ObjectTopicId registerCloudTopic(PJ::ObjectStore& store, PJ::DatasetId dataset_id) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = "/cloud";
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  return *topic_id;
}

// L.18: a reload that lands a same-timestamp, same-size sample under the stable
// topic id must not be served from the pre-reload caches.
TEST(PointCloudLayerReload, DatasetReplaceClearsSampleIdentityCaches) {
  g_primary_parser_calls.store(0);
  g_staged_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_primary_parser_calls,
                                          &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100}) << "attach did not render the first sample";

  // Baseline: the same sample id is skipped at tracker rate.
  const int calls_after_attach = g_primary_parser_calls.load();
  layer.renderAtForTest(100);
  EXPECT_EQ(g_primary_parser_calls.load(), calls_after_attach);

  // In-place reload: same topic name, same stamp, same byte SIZE — the maximal
  // lookalike of the already-pushed sample (only its SequentialUID differs).
  PJ::DataEngine staged_engine;
  PJ::ObjectStore staged_store;
  const PJ::ObjectTopicId staged_topic = registerCloudTopic(staged_store, /*dataset_id=*/2);
  ASSERT_TRUE(staged_store.pushOwned(staged_topic, 100, std::vector<uint8_t>{0x02}).has_value());
  std::vector<std::pair<PJ::ObjectTopicId, std::unique_ptr<PJ::MessageParserHandle>>> staged_parsers;
  staged_parsers.emplace_back(staged_topic, makeBoundHandle(kSchema, []() noexcept -> void* {
                                return new CountingObjectParser(
                                    kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_staged_parser_calls,
                                    &emitMixedCloud);
                              }));
  session.replaceDataset(staged_engine, staged_store, /*staged_id=*/2, /*primary_id=*/1, std::move(staged_parsers));

  // Without the datasetAboutToBeReplaced reset the colliding id hits the
  // "already pushed" skip and the new bytes are never decoded.
  layer.renderAtForTest(100);
  EXPECT_GE(g_staged_parser_calls.load(), 1) << "layer served stale cached content after the dataset replace";
}

// The dock's per-tick repaint gate keys on PointCloudLayer::renderKey(): it must be
// STABLE while the same sample is active (so a 60 Hz playhead over a <10 Hz cloud
// coalesces) and CHANGE when a new sample becomes active (so the repaint resumes).
// With no TF buffer the key reduces to the active sample stamp, which is what this
// pins; the transform term is covered by the live scene path.
TEST(PointCloudLayerRenderKey, TracksActiveSampleStamp) {
  g_primary_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_primary_parser_calls,
                                          &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;  // no tf_buffer: renderKey reduces to the sample stamp
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));

  const uint64_t k_150 = layer.renderKey(PJ::fromRaw(150));  // sample @100 active
  const uint64_t k_180 = layer.renderKey(PJ::fromRaw(180));  // still @100 → coalesce
  const uint64_t k_250 = layer.renderKey(PJ::fromRaw(250));  // sample @200 active → repaint
  EXPECT_EQ(k_150, k_180) << "same active sample must give a stable key";
  EXPECT_NE(k_150, k_250) << "a new active sample must change the key";
}

// A same-stamp, same-size replacement entry (the store permits duplicate
// timestamps; indexAtOrBefore picks the last pushed) is a DIFFERENT sample and
// must CHANGE the key — a stamp-keyed renderKey left the dock's repaint gate
// closed, so the replacement never even reached renderAt's UID memo.
TEST(PointCloudLayerRenderKey, SameStampReplacementChangesKey) {
  g_primary_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_primary_parser_calls,
                                          &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;  // no tf_buffer: renderKey reduces to the sample identity
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));

  const uint64_t k_before = layer.renderKey(PJ::fromRaw(150));
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x02}).has_value());  // same stamp + size
  const uint64_t k_after = layer.renderKey(PJ::fromRaw(150));
  EXPECT_NE(k_before, k_after) << "same-stamp replacement entry did not change the render key";
}

// The renderKey transform-fold branch — untested by TracksActiveSampleStamp, which
// uses no tf_buffer. With the SAME active cloud sample, the key must CHANGE when the
// fixed←source transform changes (sensor moved) and stay STABLE when it does not, so
// a moving cloud reopens the repaint gate while a static pose coalesces.
TEST(PointCloudLayerRenderKey, FoldsFixedFromSourceTransform) {
  g_primary_parser_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  // One sample at stamp 0 → it is the active sample for every t ≥ 0, so the key's
  // ONLY moving part across the times below is the transform term.
  ASSERT_TRUE(store.pushOwned(topic_id, 0, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_primary_parser_calls,
                                          &emitMixedCloud);
                                    }));

  // world←lidar at two stamps with DIFFERENT translations (same orientation).
  auto tf = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  ASSERT_TRUE(tf->setTransform(
                    pj::scene3d::StampedTransform{
                        .stamp = PJ::fromRaw(100),
                        .parent_frame = "world",
                        .child_frame = "lidar",
                        .transform = pj::scene3d::Transform(glm::dvec3{1.0, 0.0, 0.0}, glm::dquat{1, 0, 0, 0})})
                  .has_value());
  ASSERT_TRUE(tf->setTransform(
                    pj::scene3d::StampedTransform{
                        .stamp = PJ::fromRaw(200),
                        .parent_frame = "world",
                        .child_frame = "lidar",
                        .transform = pj::scene3d::Transform(glm::dvec3{9.0, 0.0, 0.0}, glm::dquat{1, 0, 0, 0})})
                  .has_value());

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = tf;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));  // source_frame_ = "lidar" (the cloud's frame_id)
  layer.setFixedFrame(u"world"_s);

  const uint64_t k_t150 = layer.renderKey(PJ::fromRaw(150));  // TF@100 (x=1)
  const uint64_t k_t180 = layer.renderKey(PJ::fromRaw(180));  // still TF@100 → stable
  const uint64_t k_t250 = layer.renderKey(PJ::fromRaw(250));  // TF@200 (x=9) → changed
  EXPECT_EQ(k_t150, k_t180) << "same sample + same transform must coalesce";
  EXPECT_NE(k_t150, k_t250) << "a transform change at the same sample must reopen the gate";
}

// L.20: stamp 0 is data, not a sentinel — attach renders it and refreshNow()
// (here driven through a color-field change) re-decodes at it.
TEST(PointCloudLayerTimestampZero, AttachAndRefreshRenderFirstSampleAtStampZero) {
  g_zero_stamp_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  ASSERT_TRUE(store.pushOwned(topic_id, 0, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_zero_stamp_parser_calls,
                                          &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{0}) << "attach treated stamp 0 as 'no data'";

  const int calls_before = g_zero_stamp_parser_calls.load();
  layer.setColorField(u"y"_s);  // triggers refreshNow() before any tracker tick
  EXPECT_GT(g_zero_stamp_parser_calls.load(), calls_before) << "refreshNow() treated stamp 0 as 'no data'";
}

// L.19: the raw push path (and the already-pushed skip) must update wanted_, so a
// stale in-flight compressed decode finishing late is discarded instead of
// clearing/overwriting the newer sample the tracker moved to.
TEST(PointCloudLayerMixedMode, StaleCompressedDecodeDoesNotClobberNewerSample) {
  g_mixed_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());   // raw
  ASSERT_TRUE(store.pushOwned(topic_id, 2000, std::vector<uint8_t>{0x02}).has_value());  // compressed (bogus codec)
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_mixed_parser_calls,
                                          &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});

  // Tracker reaches the compressed sample: an async decode of (2000) starts.
  layer.renderAtForTest(2000);
  ASSERT_TRUE(layer.decodeInFlightForTest());

  // Before the decode lands the tracker scrubs back to the raw sample.
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});

  // The decode fails (bogus codec). It is stale — the tracker wants (100) — so it
  // must NOT clear the view / pushed identity the failure path applies to a
  // current sample.
  ASSERT_TRUE(pumpUntil([&] { return !layer.decodeInFlightForTest(); }, std::chrono::seconds(10)))
      << "compressed decode never completed";
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100})
      << "stale compressed decode result clobbered the newer sample";
}

// Resolve-before-memo regression: when the active sample is unchanged, renderAt
// must skip on metadata alone WITHOUT resolving payload bytes — a resolve is a
// cold refetch (file read + whole-chunk decompress on the GUI thread) once the
// ResidentPayloadPool has evicted the entry's seed. The store's warm latestAt
// cache would mask a redundant resolve, so it is deliberately reset between
// renders via an out-of-order push — exactly the streaming condition that made
// the redundant resolve go cold in production.
TEST(PointCloudLayerMetadataGate, UnchangedSampleDoesNotResolvePayload) {
  g_metadata_gate_parser_calls.store(0);

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store, /*dataset_id=*/1);
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, topic_id, 100, std::vector<uint8_t>{0x01}, fetch_count);
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud,
                                          &g_metadata_gate_parser_calls, &emitMixedCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));  // bootstrap + first render resolve the sample
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});
  const int fetches_after_attach = *fetch_count;
  const int parses_after_attach = g_metadata_gate_parser_calls.load();
  ASSERT_GT(fetches_after_attach, 0);

  // Bust the warm latestAt cache (an out-of-order insert resets it): any resolve
  // issued by the next render now HAS to re-invoke the lazy fetcher.
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, topic_id, 50, std::vector<uint8_t>{0x02}, old_fetch_count);

  layer.renderAtForTest(100);  // same active sample -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_attach) << "unchanged sample resolved payload bytes on a later tick";
  EXPECT_EQ(g_metadata_gate_parser_calls.load(), parses_after_attach);
  EXPECT_EQ(*old_fetch_count, 0) << "the out-of-order older sample must not be resolved at all";
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
