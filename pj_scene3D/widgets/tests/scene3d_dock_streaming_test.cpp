// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression tests for Scene3DDockWidget streaming / live-edge correctness:
//
//  - H.4: a TF-only dock (a FrameTransforms topic consumed as config, zero render
//    layers) must survive revalidateObjects() so MainWindow::syncWidgetsToCatalog
//    does not wipe it to a placeholder on every catalog change — and must report
//    "empty" only once its config topic is actually evicted from the store.
//  - M.17: the dock's TF buffer / dataset binding is not one-shot. After the bound
//    dataset's last tracked topic is removed, the binding resets so a topic from a
//    second dataset can rebind to the correct TF tree.
//
// These exercise the dock through its public IObjectViewer surface against a real
// SessionManager/ObjectStore + TransformService. The GL view is created lazily
// (a queued singleShot in the base ctor) and never realized here, so no OpenGL
// context is required: addTopic of a config topic and revalidateObjects touch the
// view only behind a `view_ != nullptr` guard.

#include <gtest/gtest.h>

#include <QApplication>
#include <QList>
#include <QObject>
#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"  // pumpUntil
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

namespace {

// Registers a topic in the store and pushes one owned payload so the descriptor
// is non-empty (topic_name set) and entryCount() > 0.
PJ::ObjectTopicId registerTfTopic(PJ::ObjectStore& store, PJ::DatasetId dataset_id, const std::string& name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = name;
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  EXPECT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x00}).has_value());
  return *topic_id;
}

using pj::scene3d::test::CountingObjectParser;
using pj::scene3d::test::makeBoundHandle;

constexpr std::string_view kCloudSchema = "mock/point_cloud";
std::atomic<int> g_cloud_parser_calls{0};

// A single float32-xyz point over a static buffer (no anchor needed).
PJ::Expected<PJ::sdk::ObjectRecord> emitTinyCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
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

// Captures the layer pointers the base hands over on registration —
// syncViewLayers is the one (protected) seam of a headless, view-less dock that
// sees the concrete layers, since the public layers() returns metadata only.
class ProbeDock : public PJ::Scene3DDockWidget {
 public:
  std::vector<PJ::ISceneLayer*> synced_layers;

 protected:
  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers) override {
    synced_layers = ordered_layers;
    PJ::Scene3DDockWidget::syncViewLayers(ordered_layers);
  }
};

// End-to-end repaint-gate regression, driven through SceneDockWidget::onTrackerTime:
// a same-stamp, same-size replacement cloud (new SequentialUID — the store permits
// duplicate timestamps) must reopen the per-tick gate and reach the layer. With a
// stamp-keyed renderKey the combined key never changed, the tick was swallowed at
// the gate, and renderAt's UID memo never saw the new entry. The headless harness
// cannot run the GL paint that drains the deferred decode, so renderAtForTest()
// stands in for paintGL after each delivered tick.
TEST(Scene3DDockStreaming, SameStampReplacementCloudPassesRepaintGate) {
  g_cloud_parser_calls.store(0);
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/cloud";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kCloudSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kCloudSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_cloud_parser_calls, &emitTinyCloud);
  }));

  ProbeDock dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(*topic_id, PJ::sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
  ASSERT_EQ(dock.synced_layers.size(), 1u);
  auto* layer = dynamic_cast<pj::scene3d::PointCloudLayer*>(dock.synced_layers.front());
  ASSERT_NE(layer, nullptr);

  dock.onTrackerTime(0.0);      // clamps to the lone sample; the first tick always paints
  layer->renderAtForTest(100);  // stand-in for the gated paintGL
  const int parses_after_first = g_cloud_parser_calls.load();
  ASSERT_GE(parses_after_first, 1);

  // Same stamp, same byte size — only the SequentialUID differs.
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x02}).has_value());

  int repaint_requests = 0;
  const auto connection =
      QObject::connect(layer, &PJ::ISceneLayer::repaintRequested, [&repaint_requests] { ++repaint_requests; });
  dock.onTrackerTime(0.0);
  QObject::disconnect(connection);
  EXPECT_GT(repaint_requests, 0) << "repaint gate swallowed the same-stamp replacement cloud";
  layer->renderAtForTest(100);
  EXPECT_GT(g_cloud_parser_calls.load(), parses_after_first) << "replacement cloud was never decoded";
}

TEST(Scene3DDockStreaming, RevalidateWithoutSessionKeepsNeverPopulatedDock) {
  // No session set (e.g. mid teardown / session swap): a never-populated 3D dock
  // must still be kept, not reported empty and wiped to the placeholder.
  PJ::Scene3DDockWidget dock;
  EXPECT_TRUE(dock.revalidateObjects());
}

TEST(Scene3DDockStreaming, EmptyDockSurvivesRevalidateUntilItHasHeldContent) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // A click-created, never-populated 3D dock (no layers, no config topics) must
  // survive a catalog change (e.g. a dataset load), so syncWidgetsToCatalog does
  // not reset it to the placeholder.
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_TRUE(dock.revalidateObjects());

  // Once it has held a config topic that is then evicted, it reports empty so the
  // shell can reset it — same as the eviction path for a populated dock.
  const auto tf_topic = registerTfTopic(session.objectStore(), /*dataset_id=*/1, "/tf");
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  EXPECT_TRUE(dock.revalidateObjects());
  session.objectStore().removeTopic(tf_topic);
  EXPECT_FALSE(dock.revalidateObjects());
}

TEST(Scene3DDockStreaming, TfOnlyDockSurvivesRevalidateUntilTopicEvicted) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const auto tf_topic = registerTfTopic(store, /*dataset_id=*/1, "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // Drop a FrameTransforms topic: consumed as a config topic, creates NO layer.
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  EXPECT_TRUE(dock.layers().empty()) << "a FrameTransforms topic must not create a render layer";

  // H.4: with zero layers but a live config topic, the dock is still alive — a
  // catalog change (here, just re-running the check) must not wipe it.
  EXPECT_TRUE(dock.revalidateObjects());
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 1u);

  // Evicting the config topic from the store leaves the dock genuinely empty.
  store.removeTopic(tf_topic);
  EXPECT_FALSE(dock.revalidateObjects());
  // The binding reset because no tracked topic remained for the dataset (M.17).
  EXPECT_FALSE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 0u);
}

TEST(Scene3DDockStreaming, RebindsToSecondDatasetAfterFirstRemoved) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const auto tf_a = registerTfTopic(store, /*dataset_id=*/1, "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  ASSERT_TRUE(dock.addTopic(tf_a, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  ASSERT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 1u);

  // Remove dataset A's topic and revalidate: the binding must reset so the dock
  // is free to adopt a different dataset's TF tree.
  store.removeTopic(tf_a);
  EXPECT_FALSE(dock.revalidateObjects());
  ASSERT_FALSE(dock.hasTransformBufferForTest());

  // A topic from a second dataset now rebinds cleanly to dataset B's buffer
  // rather than silently resolving against A's dead tree.
  const auto tf_b = registerTfTopic(store, /*dataset_id=*/2, "/tf");
  ASSERT_TRUE(dock.addTopic(tf_b, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf2"_s));
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 2u);
}

// Regression (PR #212 "Use time offset"): a TF-only dock consumes /tf as a config
// topic and creates NO render layer, so the base onTrackerTime() layers_-only scan
// found no dataset, defaulted the display offset to zero, and treated display
// seconds as absolute. The TF buffer is keyed by ABSOLUTE ns, so the lookup found
// nothing — TF rendered only with the toggle OFF. With the toggle ON,
// onTrackerTime(0.0) must recover offset == kBaseNs and drive the view at kBaseNs.
TEST(Scene3DDockStreaming, TfOnlyDockTrackerTimeRecoversAbsoluteOffset) {
  static constexpr int64_t kBaseNs = 1'700'000'000'000'000'000LL;  // ~2023-11 in epoch ns

  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  // Object-only source, but with its own DataEngine dataset + TimeDomain (mirrors
  // StreamingSourceManager): "Use time offset" writes the align-starts shift to the
  // domain, so the dataset must exist there with a non-default domain. First
  // createDataset mints id 1, matching the ObjectStore topic's dataset_id below.
  const auto domain = session.dataEngine().createTimeDomain("tf_stream");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  const auto ds =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "tf_stream", .time_domain_id = *domain});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  ASSERT_EQ(*ds, 1u);

  // Register /tf and push one entry at kBaseNs so datasetMinTimestamp(1) == kBaseNs
  // (datasetRawBounds unions the ObjectStore).
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/tf";
  const auto topic_id_opt = store.registerTopic(desc);
  ASSERT_TRUE(topic_id_opt.has_value());
  const PJ::ObjectTopicId tf_topic = *topic_id_opt;
  ASSERT_TRUE(store.pushOwned(tf_topic, kBaseNs, std::vector<uint8_t>{0x00}).has_value());

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // Consume /tf as a config topic (handleSceneConfigTopic path) -> no render layer.
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"/tf"_s));
  ASSERT_TRUE(dock.layers().empty()) << "a FrameTransforms topic must not create a render layer";
  ASSERT_EQ(dock.boundDatasetIdForTest(), 1u) << "dataset_id_ must be set by the config path";

  // "Use time offset" ON (PJ3-parity default in the app; SessionManager default is OFF).
  session.setUseTimeOffset(true);

  // display 0.0 (start of the relative axis) -> absolute = 0 + offset = kBaseNs.
  // RED before the fix: lastTrackerNs() == 0 (empty layers_, offset stayed zero).
  dock.onTrackerTime(0.0);
  const auto ns = dock.lastTrackerNsForTest();
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kBaseNs);
}

// Guard: with the toggle OFF the absolute path is unchanged — displayOffset(1) == 0,
// so onTrackerTime(5.0) yields exactly 5e9 ns (integer ns, no double drift).
TEST(Scene3DDockStreaming, TfOnlyDockTrackerTimeToggleOffIsRawSeconds) {
  static constexpr int64_t kBaseNs = 1'700'000'000'000'000'000LL;

  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/tf";
  const auto topic_id_opt = store.registerTopic(desc);
  ASSERT_TRUE(topic_id_opt.has_value());
  const PJ::ObjectTopicId tf_topic = *topic_id_opt;
  ASSERT_TRUE(store.pushOwned(tf_topic, kBaseNs, std::vector<uint8_t>{0x00}).has_value());

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"/tf"_s));

  session.setUseTimeOffset(false);
  dock.onTrackerTime(5.0);
  const auto ns = dock.lastTrackerNsForTest();
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, static_cast<int64_t>(5LL * 1'000'000'000LL));
}

// Regression: the frame combos (fixed-frame + camera follow) are fed from the
// WHOLE TF buffer, not the frames resolvable at the current playhead. TF folded
// into the buffer while the tracker is PAUSED — a file load drives ingest via
// datasetTransformsReady at a fixed time — must refresh the available-frame list
// immediately. Previously the list was only re-polled inside setTrackerTime (which
// early-returns on an unchanged time), so the full tree appeared only after the
// user pressed play.
TEST(Scene3DDockStreaming, FrameListReflectsBufferGrowthWhilePaused) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto tf_topic = registerTfTopic(session.objectStore(), /*dataset_id=*/1, "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"/tf"_s));
  ASSERT_TRUE(pj::scene3d::test::pumpUntil([&] { return dock.sceneView() != nullptr; }));

  // Pin a fixed playhead and enumerate while the buffer is still empty.
  dock.onTrackerTime(2.0);
  EXPECT_TRUE(dock.availableFrames().isEmpty());

  // TF arrives into the shared buffer at the SAME playhead (no play / scrub).
  auto buf = transform_service.transformBuffer(/*dataset_id=*/1);
  ASSERT_NE(buf, nullptr);
  const auto add_edge = [&](const char* parent, const char* child) {
    pj::scene3d::StampedTransform st;
    st.stamp = pj::scene3d::TimePoint{std::chrono::nanoseconds(100)};
    st.parent_frame = parent;
    st.child_frame = child;
    st.transform.t = glm::dvec3{0.0};
    st.transform.q = glm::dquat{1.0, 0.0, 0.0, 0.0};
    EXPECT_TRUE(buf->setTransform(st).has_value());
  };
  add_edge("map", "odom");
  add_edge("odom", "base_link");

  // The file-load driver re-emits datasetTransformsReady after folding TF in; the
  // playhead does NOT move.
  transform_service.ingestFrameTransformsForDataset(/*dataset_id=*/1);

  const QList<pj::scene3d::FrameRow> frames = dock.availableFrames();
  const auto has = [&](const char* name) {
    return std::any_of(frames.begin(), frames.end(), [&](const pj::scene3d::FrameRow& r) { return r.name == name; });
  };
  EXPECT_TRUE(has("map")) << "the whole TF tree must list without pressing play";
  EXPECT_TRUE(has("odom"));
  EXPECT_TRUE(has("base_link"));
}

}  // namespace
