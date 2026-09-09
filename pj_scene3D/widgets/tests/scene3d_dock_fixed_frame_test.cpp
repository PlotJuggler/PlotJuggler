// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A newly-created 3D dock defaults its fixed frame to the last one the user
// manually picked for the same dataset's TransformBuffer (feat:
// scene3d-remember-fixed-frame). Recording happens on the explicit-selection
// path (setFixedFrame); seeding happens in the auto-root path
// (onAvailableFrames -> resolveAutoFixedFrame), preferring the remembered frame
// over the map/world/odom heuristic when it is present in the live TF tree.
//
// Driven headless through the dock's public surface + a mock FrameTransforms
// parser, like scene3d_dock_persistence_test: the GL view defers all GL to
// initializeGL(), so an offscreen, never-shown dock touches no context.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kTfSchema = "mock/frame_transforms";

// A static three-frame TF tree map -> odom -> base_link. The map/world/odom/...
// heuristic (pickFixedFrame) resolves this to "map", so a remembered "odom" or
// "base_link" is observably different from the default.
PJ::Expected<PJ::sdk::ObjectRecord> emitTfTree(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  PJ::sdk::FrameTransforms ft;
  const auto add = [&](const char* parent, const char* child) {
    PJ::sdk::FrameTransform edge;
    edge.timestamp = ts;
    edge.parent_frame_id = parent;
    edge.child_frame_id = child;
    edge.translation = {0.0, 0.0, 0.0};
    edge.rotation = {0.0, 0.0, 0.0, 1.0};  // identity (w=1)
    ft.transforms.push_back(std::move(edge));
  };
  add("map", "odom");
  add("odom", "base_link");
  return PJ::sdk::ObjectRecord{.ts = ts, .object = ft};
}

struct DatasetTopic {
  PJ::DatasetId dataset_id = 0;
  PJ::ObjectTopicId topic_id;
};

// Create an engine dataset (source_name keys the cross-session memory), register a
// FrameTransforms topic under it bound to the emitTfTree parser, push one sample,
// and bulk-ingest its TF so the shared frame tree is populated.
DatasetTopic makeTfDataset(
    PJ::SessionManager& session, pj::scene3d::TransformService& tf, const std::string& source,
    const std::string& topic_name) {
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source, .time_domain_id = 0});
  EXPECT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = dataset_or.has_value() ? *dataset_or : 0;

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = topic_name;
  const auto topic_or = session.objectStore().registerTopic(desc);
  EXPECT_TRUE(topic_or.has_value());
  const PJ::ObjectTopicId topic_id = topic_or.has_value() ? *topic_or : PJ::ObjectTopicId{};
  EXPECT_TRUE(session.objectStore().pushOwned(topic_id, 100, std::vector<uint8_t>{0x00}).has_value());
  session.registerObjectTopicParser(
      topic_id, makeBoundHandle(kTfSchema, []() noexcept -> void* {
        return new CountingObjectParser(kTfSchema, PJ::sdk::BuiltinObjectType::kFrameTransforms, nullptr, &emitTfTree);
      }));
  tf.ingestFrameTransformsForDataset(dataset_id);
  return {dataset_id, topic_id};
}

// Bring up a dock bound to session/tf, add the TF topic, and let its lazily
// created GL view come into existence (the base schedules it via singleShot(0)).
void attachTfTopic(
    PJ::Scene3DDockWidget& dock, PJ::SessionManager& session, pj::scene3d::TransformService& tf,
    PJ::ObjectTopicId topic_id) {
  dock.setSessionManager(&session);
  dock.setTransformService(&tf);
  ASSERT_TRUE(dock.addTopic(topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  ASSERT_TRUE(pumpUntil([&] { return dock.sceneView() != nullptr; })) << "the lazily-created GL view must come up";
}

// A new dock prefers the dataset's remembered manual frame over the heuristic root.
TEST(Scene3DDockFixedFrame, NewDockSeedsRememberedFrameWhenPresent) {
  PJ::SessionManager session;
  pj::scene3d::TransformService tf(session);
  const DatasetTopic ds = makeTfDataset(session, tf, "seed.dat", "/tf");
  tf.rememberFixedFrame(ds.dataset_id, u"odom"_s);  // not the heuristic root "map"

  PJ::Scene3DDockWidget dock;
  attachTfTopic(dock, session, tf, ds.topic_id);
  dock.sceneView()->refreshAvailableFrames();  // -> framesChanged -> onAvailableFrames -> auto-seed

  EXPECT_EQ(dock.currentFixedFrame(), u"odom"_s)
      << "a new dock must default to the dataset's remembered frame, not the map/world/odom heuristic";
}

// When the remembered frame is absent from the dock's TF tree, fall back to the
// heuristic root (guards the seeding change against breaking the default path).
TEST(Scene3DDockFixedFrame, NewDockFallsBackToHeuristicWhenRememberedAbsent) {
  PJ::SessionManager session;
  pj::scene3d::TransformService tf(session);
  const DatasetTopic ds = makeTfDataset(session, tf, "fallback.dat", "/tf");
  tf.rememberFixedFrame(ds.dataset_id, u"lidar"_s);  // not in map/odom/base_link

  PJ::Scene3DDockWidget dock;
  attachTfTopic(dock, session, tf, ds.topic_id);
  dock.sceneView()->refreshAvailableFrames();

  EXPECT_EQ(dock.currentFixedFrame(), u"map"_s)
      << "falls back to the heuristic root when the remembered frame is absent from the tree";
}

// End-to-end: a manual pick in one dock becomes the default for a second dock
// created afterward on the same dataset / TransformBuffer.
TEST(Scene3DDockFixedFrame, SecondDockInheritsFirstDocksManualChoice) {
  PJ::SessionManager session;
  pj::scene3d::TransformService tf(session);
  const DatasetTopic ds = makeTfDataset(session, tf, "inherit.dat", "/tf");

  PJ::Scene3DDockWidget dock_a;
  attachTfTopic(dock_a, session, tf, ds.topic_id);
  dock_a.sceneView()->refreshAvailableFrames();
  dock_a.setFixedFrame(u"base_link"_s);  // a manual selection
  ASSERT_EQ(dock_a.currentFixedFrame(), u"base_link"_s);

  PJ::Scene3DDockWidget dock_b;  // created AFTER the manual pick
  attachTfTopic(dock_b, session, tf, ds.topic_id);
  dock_b.sceneView()->refreshAvailableFrames();

  EXPECT_EQ(dock_b.currentFixedFrame(), u"base_link"_s)
      << "a second dock on the same TransformBuffer must inherit the first dock's manual fixed frame";
}

// Undo/layout restore must reproduce the resolved frame recorded in the XML,
// even for an auto-root dock. The remembered-frame preference is mutable
// user-global state and may have changed after the snapshot was captured.
TEST(Scene3DDockFixedFrame, AutoRootRestoreUsesSavedResolvedFrame) {
  PJ::SessionManager session;
  pj::scene3d::TransformService tf(session);
  const DatasetTopic ds = makeTfDataset(session, tf, "auto_restore.dat", "/tf");

  PJ::Scene3DDockWidget dock;
  attachTfTopic(dock, session, tf, ds.topic_id);
  dock.sceneView()->refreshAvailableFrames();
  ASSERT_TRUE(dock.isAutoRootMode());
  ASSERT_EQ(dock.currentFixedFrame(), u"map"_s);

  QDomDocument doc;
  const QDomElement saved = dock.xmlSaveState(doc);
  ASSERT_EQ(saved.attribute(u"fixed_frame_mode"_s), u"auto_root"_s);
  ASSERT_EQ(saved.attribute(u"fixed_frame"_s), u"map"_s);

  dock.setFixedFrame(u"odom"_s);
  ASSERT_EQ(tf.rememberedFixedFrame(ds.dataset_id), u"odom"_s);

  ASSERT_TRUE(dock.xmlLoadState(saved));
  EXPECT_TRUE(dock.isAutoRootMode());
  EXPECT_EQ(dock.currentFixedFrame(), u"map"_s)
      << "restore must use the snapshot's resolved frame, not today's remembered preference";
  EXPECT_EQ(tf.rememberedFixedFrame(ds.dataset_id), u"odom"_s)
      << "restoring an auto snapshot must not rewrite the user's remembered preference";
}

// Applying an explicit fixed frame from XML is restoration, not a manual pick.
// It must not change the preference that seeds subsequently-created docks.
TEST(Scene3DDockFixedFrame, ExplicitRestoreDoesNotRewriteRememberedFrame) {
  PJ::SessionManager session;
  pj::scene3d::TransformService tf(session);
  const DatasetTopic ds = makeTfDataset(session, tf, "explicit_restore.dat", "/tf");

  PJ::Scene3DDockWidget save_dock;
  attachTfTopic(save_dock, session, tf, ds.topic_id);
  save_dock.sceneView()->refreshAvailableFrames();
  save_dock.setFixedFrame(u"map"_s);
  QDomDocument doc;
  const QDomElement saved = save_dock.xmlSaveState(doc);
  ASSERT_EQ(saved.attribute(u"fixed_frame_mode"_s), u"explicit"_s);

  tf.rememberFixedFrame(ds.dataset_id, u"odom"_s);
  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&session);
  load_dock.setTransformService(&tf);
  ASSERT_TRUE(load_dock.xmlLoadState(saved));

  EXPECT_FALSE(load_dock.isAutoRootMode());
  EXPECT_EQ(load_dock.currentFixedFrame(), u"map"_s);
  EXPECT_EQ(tf.rememberedFixedFrame(ds.dataset_id), u"odom"_s) << "XML restore must not masquerade as a user selection";
}

}  // namespace

namespace {

class FixedFrameEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    QCoreApplication::setApplicationName(u"scene3d_dock_fixed_frame_test"_s);
    // Redirect QSettings to a throwaway test location so recording never touches the
    // developer's real PlotJuggler config.
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new FixedFrameEnvironment);

}  // namespace
