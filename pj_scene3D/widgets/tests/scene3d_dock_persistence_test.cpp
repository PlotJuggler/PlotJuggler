// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression tests for Scene3DDockWidget layout persistence (WP6):
//
//  - M.18: a FrameTransforms config topic (consumed as config, zero render
//    layers) round-trips through xmlSaveState/xmlLoadState as a <config_topic>
//    element and re-binds the dock's TF buffer on restore.
//  - M.55: layer/config identity is re-resolved by stable dataset source name,
//    not the load-order DatasetId, so a layout saved when the data file was
//    dataset id N restores in a session where the same file is dataset id M.
//  - M.19: xmlLoadState forces the lazily-created view so a saved explicit fixed
//    frame survives even when zero layers restore (view_ otherwise null).
//
// These exercise the dock through its public surface against a real
// SessionManager/DataEngine/ObjectStore + TransformService. The GL view is a
// QOpenGLWidget; its ctor defers all GL to initializeGL(), so constructing it
// headless (offscreen platform, never shown) is safe and touches no context.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QMouseEvent>
#include <QSet>
#include <QString>
#include <QWheelEvent>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

namespace {

// Creates an engine dataset with the given source name (re-resolution key) and
// registers a TF object topic under its id, with one owned payload so the
// descriptor is non-empty (topic_name set) and entryCount() > 0. Returns the
// engine-assigned DatasetId and the topic id.
struct DatasetTopic {
  PJ::DatasetId dataset_id = 0;
  PJ::ObjectTopicId topic_id;
};

DatasetTopic registerTfDataset(
    PJ::SessionManager& session, const std::string& source_name, const std::string& topic_name) {
  auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source_name, .time_domain_id = 0});
  EXPECT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = topic_name;
  desc.metadata_json = R"({"builtin_object_type":"kFrameTransforms"})";
  const auto topic_or = session.objectStore().registerTopic(desc);
  EXPECT_TRUE(topic_or.has_value());
  EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x00}).has_value());
  return {dataset_id, *topic_or};
}

using namespace pj::scene3d::test;

constexpr std::string_view kImageSchema = "mock/image";
constexpr std::string_view kRobotSchema = "mock/robot_description";
const std::array<float, 4> kDepthPixels = {1.0f, 2.0f, 3.0f, 4.0f};
constexpr auto kRobotUrdf = R"(
<robot name="restored">
  <link name="base_link"/>
</robot>
)";

// A mock depth parser: ignores the payload and always yields a 32FC1 (depth) Image,
// so firstSampleIsDepthEncoded sees "depth" whenever a sample exists.
PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "32FC1";
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kDepthPixels.data()), kDepthPixels.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}

PJ::Expected<PJ::sdk::ObjectRecord> emitRobotDescription(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return PJ::sdk::ObjectRecord{
      .ts = ts,
      .object =
          PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kRobotUrdf,
          },
  };
}

PJ::ObjectTopicId registerDepthTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, const std::string& topic_name, bool push_sample) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = topic_name;
  desc.metadata_json = R"({"builtin_object_type":"kImage"})";
  const auto topic_or = session.objectStore().registerTopic(desc);
  EXPECT_TRUE(topic_or.has_value());
  if (!topic_or.has_value()) {
    return {};
  }
  if (push_sample) {
    EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x01}).has_value());
  }
  session.registerObjectTopicParser(*topic_or, makeBoundHandle(kImageSchema, []() noexcept -> void* {
    return new CountingObjectParser(kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
  }));
  return *topic_or;
}

// Like registerTfDataset, but for a depth-encoded kImage topic: registers a depth
// parser and, only when push_sample is true, pushes one sample. With
// push_sample=false the topic resolves (findTopic) but the ObjectStore holds no
// entry to peek — the "layout restored before the first frame arrives" case.
DatasetTopic registerDepthDataset(
    PJ::SessionManager& session, const std::string& source_name, const std::string& topic_name, bool push_sample) {
  auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source_name, .time_domain_id = 0});
  EXPECT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  return {dataset_id, registerDepthTopic(session, dataset_id, topic_name, push_sample)};
}

PJ::ObjectTopicId registerRobotTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, const std::string& topic_name) {
  const auto topic_or = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = topic_name,
          .metadata_json = R"({"builtin_object_type":"kRobotDescription"})",
      });
  EXPECT_TRUE(topic_or.has_value());
  if (!topic_or.has_value()) {
    return {};
  }
  session.registerObjectTopicParser(*topic_or, makeBoundHandle(kRobotSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kRobotSchema, PJ::sdk::BuiltinObjectType::kRobotDescription, nullptr, &emitRobotDescription);
  }));
  EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x01}).has_value());
  return *topic_or;
}

QDomElement appendLocalRobotTopicLayer(
    QDomDocument& doc, QDomElement& scene, PJ::DatasetId source_dataset_id, const QString& source_dataset,
    const QString& source_topic) {
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"local"_s, u"true"_s);
  layer.setAttribute(u"object_type"_s, u"kRobotDescription"_s);
  layer.setAttribute(u"display_name"_s, u"Robot model"_s);
  layer.setAttribute(u"visible"_s, u"true"_s);
  layer.setAttribute(u"order"_s, u"0"_s);
  QDomElement payload = doc.createElement(u"robot_model"_s);
  payload.setAttribute(u"source_type"_s, u"topic"_s);
  payload.setAttribute(u"source_topic_name"_s, source_topic);
  payload.setAttribute(u"source_dataset_id"_s, QString::number(source_dataset_id));
  payload.setAttribute(u"source_dataset_source"_s, source_dataset);
  layer.appendChild(payload);
  scene.appendChild(layer);
  return layer;
}

QDomElement appendUnqualifiedLayer(
    QDomDocument& doc, QDomElement& scene, const QString& topic_name, const QString& object_type) {
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"topic_name"_s, topic_name);
  layer.setAttribute(u"object_type"_s, object_type);
  layer.setAttribute(u"display_name"_s, topic_name);
  scene.appendChild(layer);
  return layer;
}

QDomElement appendUnqualifiedConfig(
    QDomDocument& doc, QDomElement& scene, const QString& topic_name, const QString& object_type) {
  QDomElement config = doc.createElement(u"config_topic"_s);
  config.setAttribute(u"topic_name"_s, topic_name);
  config.setAttribute(u"object_type"_s, object_type);
  scene.appendChild(config);
  return config;
}

QString savedSceneXml(const PJ::Scene3DDockWidget& dock) {
  QDomDocument doc;
  doc.appendChild(dock.xmlSaveState(doc));
  return doc.toString(-1);
}

class Scene3DDockStrictXmlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    topic_ = registerTfDataset(session_, "strict.dat", "/tf");
    dock_.setSessionManager(&session_);
    dock_.setTransformService(&transform_service_);
    ASSERT_TRUE(dock_.addTopic(topic_.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));

    // A normal save/replay realizes the lazy view and gives the rejection checks
    // a non-default, fully populated presentation snapshot to protect.
    QDomDocument seed_doc;
    const QDomElement seed = dock_.xmlSaveState(seed_doc);
    ASSERT_TRUE(dock_.xmlLoadState(seed));
    ASSERT_NE(dock_.sceneView(), nullptr);

    dock_.setFixedFrame(u"map"_s);
    dock_.setFollowFrame(u"base_link"_s);
    auto* view = dock_.sceneView();
    view->setCameraModel(pj::scene3d::SceneViewWidget::CameraModel::kFly);
    view->setGridVisible(false);
    view->setGridStyle(pj::scene3d::GridRenderPass::Style::kFilledCells);
    view->setGridExtentMetres(37.0f);
    view->setGridDivisions(19);
    view->setAxesVisible(false);
    view->setGizmoSize(0.42f);
    view->setGizmoOpacity(0.63f);
    view->setTfConnectionsVisible(false);
    auto shading = view->meshShadingParams();
    shading.meshes_visible = false;
    shading.mesh_opacity = 0.72f;
    shading.collisions_visible = false;
    shading.collision_opacity = 0.31f;
    view->setMeshShadingParams(shading);
  }

  template <typename Mutator>
  void expectRejectedWithoutMutation(Mutator&& mutate) {
    const QString before = savedSceneXml(dock_);

    QDomDocument invalid_doc;
    QDomDocument snapshot_doc;
    const QDomElement snapshot = dock_.xmlSaveState(snapshot_doc);
    QDomElement invalid = invalid_doc.importNode(snapshot, /*deep=*/true).toElement();
    invalid_doc.appendChild(invalid);
    mutate(invalid_doc, invalid);

    int presentation_changes = 0;
    int fixed_frame_changes = 0;
    int follow_frame_changes = 0;
    int workspace_changes = 0;
    const QMetaObject::Connection presentation_connection = QObject::connect(
        dock_.sceneView(), &pj::scene3d::SceneViewWidget::presentationChanged,
        [&presentation_changes]() { ++presentation_changes; });
    const QMetaObject::Connection fixed_connection = QObject::connect(
        &dock_, &PJ::Scene3DDockWidget::currentFixedFrameChanged, [&fixed_frame_changes]() { ++fixed_frame_changes; });
    const QMetaObject::Connection follow_connection = QObject::connect(
        &dock_, &PJ::Scene3DDockWidget::followFrameChanged, [&follow_frame_changes]() { ++follow_frame_changes; });
    const QMetaObject::Connection workspace_connection = QObject::connect(
        &dock_, &PJ::SceneDockWidget::workspaceChanged, [&workspace_changes]() { ++workspace_changes; });

    EXPECT_FALSE(dock_.xmlLoadState(invalid));

    QObject::disconnect(presentation_connection);
    QObject::disconnect(fixed_connection);
    QObject::disconnect(follow_connection);
    QObject::disconnect(workspace_connection);
    EXPECT_EQ(presentation_changes, 0) << "validation must finish before any view setter runs";
    EXPECT_EQ(fixed_frame_changes, 0) << "validation must finish before fixed-frame replay starts";
    EXPECT_EQ(follow_frame_changes, 0) << "validation must finish before follow-frame replay starts";
    EXPECT_EQ(workspace_changes, 0);
    EXPECT_EQ(savedSceneXml(dock_), before) << "a rejected document must preserve the complete live snapshot";
    EXPECT_TRUE(dock_.hasTransformBufferForTest());
    EXPECT_EQ(dock_.boundDatasetIdForTest(), topic_.dataset_id);
    EXPECT_FALSE(dock_.isAutoRootMode());
    EXPECT_EQ(dock_.currentFixedFrame(), u"map"_s);
    EXPECT_EQ(dock_.currentFollowFrame(), u"base_link"_s);
  }

  PJ::SessionManager session_;
  pj::scene3d::TransformService transform_service_{session_};
  PJ::Scene3DDockWidget dock_;
  DatasetTopic topic_;
};

TEST_F(Scene3DDockStrictXmlTest, RejectsUnknownOrDuplicateDirectChildrenBeforeMutation) {
  expectRejectedWithoutMutation(
      [](QDomDocument& doc, QDomElement& state) { state.appendChild(doc.createElement(u"unknown_scene_payload"_s)); });
  expectRejectedWithoutMutation(
      [](QDomDocument& doc, QDomElement& state) { state.appendChild(doc.createElement(u"scene_controls"_s)); });
  expectRejectedWithoutMutation([](QDomDocument& doc, QDomElement& state) {
    QDomElement controls = state.firstChildElement(u"scene_controls"_s);
    controls.appendChild(doc.createElement(u"unexpected_nested_payload"_s));
  });
}

TEST_F(Scene3DDockStrictXmlTest, RejectsInvalidFixedFrameAndCameraMetadataBeforeMutation) {
  expectRejectedWithoutMutation(
      [](QDomDocument&, QDomElement& state) { state.setAttribute(u"fixed_frame_mode"_s, u"automatic"_s); });
  expectRejectedWithoutMutation(
      [](QDomDocument&, QDomElement& state) { state.setAttribute(u"camera_model"_s, u"trackball"_s); });

  const QStringList invalid_camera_states = {
      u"{"_s,
      u"[]"_s,
      uR"({"radius":"near"})"_s,
      uR"({"radius":0})"_s,
      uR"({"elevation":2})"_s,
      uR"({"fov_y":4})"_s,
      uR"({"focal":[0,1,"far"]})"_s,
      uR"({"perspective":1})"_s,
      uR"({"azimuth":1e999})"_s,
  };
  for (const QString& encoded : invalid_camera_states) {
    expectRejectedWithoutMutation(
        [&encoded](QDomDocument&, QDomElement& state) { state.setAttribute(u"camera_state"_s, encoded); });
  }
}

TEST_F(Scene3DDockStrictXmlTest, RejectsMalformedOrOutOfRangeSceneControlsBeforeMutation) {
  const std::vector<std::pair<QString, QString>> invalid_attributes = {
      {u"grid_visible"_s, u"yes"_s},        {u"grid_style"_s, u"2"_s},           {u"grid_extent_m"_s, u"nan"_s},
      {u"grid_extent_m"_s, u"0"_s},         {u"grid_extent_m"_s, u"1.0e8"_s},    {u"grid_divisions"_s, u"1.5"_s},
      {u"grid_divisions"_s, u"0"_s},        {u"grid_divisions"_s, u"201"_s},     {u"axes_visible"_s, u"TRUE"_s},
      {u"gizmo_size_m"_s, u"0"_s},          {u"gizmo_size_m"_s, u"5.01"_s},      {u"gizmo_opacity"_s, u"1.01"_s},
      {u"tf_parent_lines"_s, u"false "_s},  {u"meshes_visible"_s, u"0"_s},       {u"mesh_opacity"_s, u"-0.01"_s},
      {u"collisions_visible"_s, u"TRUE"_s}, {u"collision_opacity"_s, u"1.01"_s},
  };
  for (const auto& [name, value] : invalid_attributes) {
    expectRejectedWithoutMutation([&name, &value](QDomDocument&, QDomElement& state) {
      state.firstChildElement(u"scene_controls"_s).setAttribute(name, value);
    });
  }
}

// The scene_controls grammar is one cross-platform contract: a browser-saved
// layout may carry sensor-scale values (e.g. a 5 km grid) and must still
// restore on the desktop instead of dropping the whole dock.
TEST_F(Scene3DDockStrictXmlTest, AcceptsSensorScaleSceneControlsSharedWithBrowser) {
  QDomDocument document;
  QDomElement state = dock_.xmlSaveState(document);
  document.appendChild(state);
  QDomElement controls = state.firstChildElement(u"scene_controls"_s);
  ASSERT_FALSE(controls.isNull());
  controls.setAttribute(u"grid_extent_m"_s, u"5000"_s);
  controls.setAttribute(u"gizmo_size_m"_s, u"5"_s);
  EXPECT_TRUE(dock_.xmlLoadState(state));
  EXPECT_EQ(dock_.sceneView()->gridExtentMetres(), 5000.0f);
  EXPECT_EQ(dock_.sceneView()->gizmoSize(), 5.0f);
}

// Generic layouts intentionally omit dataset qualifiers. Restore is portable
// only when (topic name, builtin type) identifies one live ObjectStore topic;
// a same-name topic of another type must not interfere.
TEST(Scene3DDockPersistence, GenericLayerResolvesUniqueTopicAndType) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic decoy_tf = registerTfDataset(session, "tf.dat", "/shared");
  const DatasetTopic depth = registerDepthDataset(session, "depth.dat", "/shared", /*push_sample=*/false);

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendUnqualifiedLayer(doc, state, u"/shared"_s, u"kImage"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  ASSERT_EQ(dock.layers().size(), 1U);
  EXPECT_EQ(dock.layers().front().topic_id.id, depth.topic_id.id);
  EXPECT_NE(dock.layers().front().topic_id.id, decoy_tf.topic_id.id);
  EXPECT_EQ(dock.boundDatasetIdForTest(), depth.dataset_id);
}

TEST(Scene3DDockPersistence, GenericLayerRejectsAmbiguousTopicAndType) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  registerDepthDataset(session, "left.dat", "/shared", /*push_sample=*/false);
  registerDepthDataset(session, "right.dat", "/shared", /*push_sample=*/false);

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendUnqualifiedLayer(doc, state, u"/shared"_s, u"kImage"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  EXPECT_FALSE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_FALSE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 0U);
}

TEST(Scene3DDockPersistence, GenericConfigResolvesUniqueTopicAndType) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic decoy_depth = registerDepthDataset(session, "depth.dat", "/shared_tf", /*push_sample=*/false);
  const DatasetTopic tf = registerTfDataset(session, "tf.dat", "/shared_tf");

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendUnqualifiedConfig(doc, state, u"/shared_tf"_s, u"kFrameTransforms"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), tf.dataset_id);
  EXPECT_NE(dock.boundDatasetIdForTest(), decoy_depth.dataset_id);
}

TEST(Scene3DDockPersistence, GenericConfigRejectsAmbiguousTopicAndType) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  registerTfDataset(session, "left.dat", "/shared_tf");
  registerTfDataset(session, "right.dat", "/shared_tf");

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendUnqualifiedConfig(doc, state, u"/shared_tf"_s, u"kFrameTransforms"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  EXPECT_FALSE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_FALSE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 0U);
}

// M.18: a TF-only dock persists its config topic and re-binds its TF buffer on
// restore. M.55: restore resolves the topic by dataset source name even when the
// DatasetId differs from the saved one.
TEST(Scene3DDockPersistence, ConfigTopicRoundTripsAndResolvesBySource) {
  // --- Save session: the file is dataset id 1.
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  const DatasetTopic saved = registerTfDataset(save_session, "drive.dat", "/tf");

  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  int workspace_changes = 0;
  QObject::connect(&save_dock, &PJ::SceneDockWidget::workspaceChanged, [&workspace_changes]() { ++workspace_changes; });
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  EXPECT_EQ(workspace_changes, 1) << "a config-only topic is part of Scene3D workspace XML";
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  EXPECT_EQ(workspace_changes, 1) << "re-adding the same config topic is a no-op";
  ASSERT_TRUE(save_dock.layers().empty()) << "FrameTransforms must not create a render layer";

  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);
  ASSERT_FALSE(state.isNull());
  const QDomElement config_el = state.firstChildElement(u"config_topic"_s);
  ASSERT_FALSE(config_el.isNull()) << "config topic must be persisted (M.18)";
  EXPECT_EQ(config_el.attribute(u"topic_name"_s), u"/tf"_s);
  EXPECT_EQ(config_el.attribute(u"dataset_source"_s), u"drive.dat"_s);

  // --- Restore session: a decoy file loads FIRST, so the SAME file is now
  // dataset id 2 — the saved id 1 must NOT be trusted blindly.
  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  registerTfDataset(load_session, "decoy.dat", "/other");                             // dataset id 1
  const DatasetTopic reloaded = registerTfDataset(load_session, "drive.dat", "/tf");  // dataset id 2
  ASSERT_NE(reloaded.dataset_id, saved.dataset_id) << "test premise: load order changed the DatasetId";

  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));

  // The config topic resolved by source name and re-bound the TF buffer to the
  // NEW dataset id, not the stale saved one.
  EXPECT_TRUE(load_dock.hasTransformBufferForTest());
  EXPECT_EQ(load_dock.boundDatasetIdForTest(), reloaded.dataset_id);
}

// M.55: when the saved dataset is absent entirely, restore drops the config
// topic gracefully (no binding) instead of resolving against a wrong dataset.
TEST(Scene3DDockPersistence, ConfigTopicUnresolvedWhenDatasetMissing) {
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  const DatasetTopic saved = registerTfDataset(save_session, "drive.dat", "/tf");

  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);

  // Restore into a session that loaded a DIFFERENT file: nothing to resolve.
  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  registerTfDataset(load_session, "unrelated.dat", "/other");

  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));
  EXPECT_FALSE(load_dock.hasTransformBufferForTest()) << "no TF binding when the saved dataset is absent";
}

// M.19: a saved explicit fixed frame is applied on restore even when zero layers
// restore, because xmlLoadState forces the lazily-created view first.
TEST(Scene3DDockPersistence, ExplicitFixedFrameSurvivesZeroLayerRestore) {
  // Hand-build a scene3d state element with an explicit fixed frame and no
  // layers / config topics (the "saved before the data file" case).
  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  state.setAttribute(u"version"_s, u"1"_s);
  state.setAttribute(u"fixed_frame_mode"_s, u"explicit"_s);
  state.setAttribute(u"fixed_frame"_s, u"map"_s);
  doc.appendChild(state);

  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));

  // The view was forced into existence and the explicit frame applied despite no
  // layers — previously view_ stayed null and the frame was silently dropped.
  EXPECT_FALSE(dock.isAutoRootMode()) << "explicit fixed-frame mode must survive restore (M.19)";
  ASSERT_NE(dock.sceneView(), nullptr) << "restore must force the lazily-created view";
  EXPECT_EQ(dock.currentFixedFrame(), u"map"_s);
}

// C1 regression: a saved DepthCloud (kImage) layer must restore even when its
// topic has no stored sample yet — the layout was applied before the first frame
// arrived (streaming, or a still-loading file). The interactive add path gates
// kImage on the first sample's encoding (firstSampleIsDepthEncoded), but on restore
// there may be no sample to peek; the layer was already validated as depth when the
// user created it, so restore must trust the saved type, not silently drop it (the
// drop was also invisible: unresolved_topics counts only dataset/topic-id
// resolution failures, which both succeeded here).
TEST(Scene3DDockPersistence, DepthCloudLayerRestoresBeforeFirstSample) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  // Topic resolves (dataset + topic registered, parser bound) but no sample yet.
  const DatasetTopic topic = registerDepthDataset(session, "cam.dat", "/cam/depth/image", /*push_sample=*/false);

  // Hand-built saved state: one kImage (DepthCloud) layer, resolvable by source.
  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  state.setAttribute(u"version"_s, u"1"_s);
  QDomElement layer_el = doc.createElement(u"layer"_s);
  layer_el.setAttribute(u"object_type"_s, u"kImage"_s);
  layer_el.setAttribute(u"display_name"_s, u"depth"_s);
  layer_el.setAttribute(u"dataset_id"_s, QString::number(topic.dataset_id));
  layer_el.setAttribute(u"dataset_source"_s, u"cam.dat"_s);
  layer_el.setAttribute(u"topic_name"_s, u"/cam/depth/image"_s);
  state.appendChild(layer_el);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));

  EXPECT_EQ(dock.layers().size(), 1U) << "a saved DepthCloud layer must restore even before its first sample arrives "
                                         "(the encoding gate belongs to the interactive add path, not restore)";
}

// Same-family widget paste calls xmlLoadState on an EXISTING Scene3D dock. The
// old TF binding is not a layer and therefore must be reset explicitly before
// the restored topics choose their dataset.
TEST(Scene3DDockPersistence, RepeatedLoadRebindsTransformDatasetAndEmptyStateClearsIt) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic a = registerTfDataset(session, "a.dat", "/tf_a");
  const DatasetTopic b = registerTfDataset(session, "b.dat", "/tf_b");

  PJ::Scene3DDockWidget source_b;
  source_b.setSessionManager(&session);
  source_b.setTransformService(&transform_service);
  ASSERT_TRUE(source_b.addTopic(b.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf_b"_s));
  QDomDocument b_doc;
  const QDomElement b_state = source_b.xmlSaveState(b_doc);

  PJ::Scene3DDockWidget target;
  target.setSessionManager(&session);
  target.setTransformService(&transform_service);
  ASSERT_TRUE(target.addTopic(a.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf_a"_s));
  ASSERT_EQ(target.boundDatasetIdForTest(), a.dataset_id);

  ASSERT_TRUE(target.xmlLoadState(b_state));
  EXPECT_TRUE(target.hasTransformBufferForTest());
  EXPECT_EQ(target.boundDatasetIdForTest(), b.dataset_id)
      << "repeated xmlLoadState must not retain the prior widget's TF buffer";

  target.setFixedFrame(u"map"_s);
  ASSERT_EQ(target.currentFixedFrame(), u"map"_s);

  QDomDocument empty_doc;
  QDomElement empty_state = empty_doc.createElement(u"scene3d"_s);
  empty_state.setAttribute(u"version"_s, u"1"_s);
  empty_doc.appendChild(empty_state);
  ASSERT_TRUE(target.xmlLoadState(empty_state));
  EXPECT_FALSE(target.hasTransformBufferForTest());
  EXPECT_EQ(target.boundDatasetIdForTest(), 0U);
  EXPECT_TRUE(target.availableFrames().isEmpty());
  EXPECT_TRUE(target.currentFixedFrame().isEmpty()) << "empty XML must not retain the prior scene's fixed frame";
}

TEST(Scene3DDockPersistence, QualifiedConfigRejectsKnownLiveObjectTypeMismatch) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic image = registerDepthDataset(session, "cam.dat", "/same_name", /*push_sample=*/false);

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  QDomElement config = doc.createElement(u"config_topic"_s);
  config.setAttribute(u"dataset_id"_s, QString::number(image.dataset_id));
  config.setAttribute(u"dataset_source"_s, u"cam.dat"_s);
  config.setAttribute(u"topic_name"_s, u"/same_name"_s);
  config.setAttribute(u"object_type"_s, u"kFrameTransforms"_s);
  state.appendChild(config);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  EXPECT_FALSE(dock.xmlLoadState(state));
  EXPECT_FALSE(dock.hasTransformBufferForTest());
}

TEST(Scene3DDockPersistence, QualifiedLayerRejectsKnownLiveObjectTypeMismatch) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic tf = registerTfDataset(session, "drive.dat", "/same_name");

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, QString::number(tf.dataset_id));
  layer.setAttribute(u"dataset_source"_s, u"drive.dat"_s);
  layer.setAttribute(u"topic_name"_s, u"/same_name"_s);
  layer.setAttribute(u"object_type"_s, u"kImage"_s);
  state.appendChild(layer);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  EXPECT_FALSE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_FALSE(dock.hasTransformBufferForTest());
}

// A Scene3D dock owns one TF/time domain. Accepting a second dataset currently
// leaves the first TF buffer bound and renders the second dataset against it,
// so the add must be rejected until per-layer domains exist.
TEST(Scene3DDockPersistence, RejectsTopicFromSecondDataset) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic a = registerTfDataset(session, "a.dat", "/tf_a");
  const DatasetTopic b = registerTfDataset(session, "b.dat", "/tf_b");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(a.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf_a"_s));
  ASSERT_EQ(dock.boundDatasetIdForTest(), a.dataset_id);

  EXPECT_FALSE(dock.addTopic(b.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf_b"_s));
  EXPECT_EQ(dock.boundDatasetIdForTest(), a.dataset_id);

  QDomDocument state_doc;
  const QDomElement state = dock.xmlSaveState(state_doc);
  int config_count = 0;
  for (QDomElement config = state.firstChildElement(u"config_topic"_s); !config.isNull();
       config = config.nextSiblingElement(u"config_topic"_s)) {
    ++config_count;
    EXPECT_EQ(config.attribute(u"dataset_id"_s).toUInt(), a.dataset_id);
  }
  EXPECT_EQ(config_count, 1);
}

TEST(Scene3DDockPersistence, MalformedLayerPayloadFailsAndPreservesLiveScene) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic topic = registerDepthDataset(session, "cam.dat", "/depth", /*push_sample=*/true);

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, QString::number(topic.dataset_id));
  layer.setAttribute(u"dataset_source"_s, u"cam.dat"_s);
  layer.setAttribute(u"topic_name"_s, u"/depth"_s);
  layer.setAttribute(u"object_type"_s, u"kImage"_s);
  layer.setAttribute(u"visible"_s, u"true"_s);
  layer.appendChild(doc.createElement(u"not_a_depthcloud_payload"_s));
  state.appendChild(layer);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(topic.topic_id, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));
  EXPECT_FALSE(dock.xmlLoadState(state));
  ASSERT_EQ(dock.layers().size(), 1u);
  EXPECT_EQ(dock.layers().front().topic_id, topic.topic_id);
  EXPECT_TRUE(dock.hasTransformBufferForTest());
}

TEST(Scene3DDockPersistence, MalformedConfigPayloadFailsAndPreservesExistingBinding) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic topic = registerTfDataset(session, "drive.dat", "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(topic.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  ASSERT_TRUE(dock.hasTransformBufferForTest());

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  QDomElement config = doc.createElement(u"config_topic"_s);
  config.setAttribute(u"dataset_id"_s, QString::number(topic.dataset_id));
  config.setAttribute(u"dataset_source"_s, u"drive.dat"_s);
  config.setAttribute(u"topic_name"_s, u"/tf"_s);
  config.setAttribute(u"object_type"_s, u"kFrameTransforms"_s);
  config.appendChild(doc.createElement(u"unexpected_payload"_s));
  state.appendChild(config);
  doc.appendChild(state);

  EXPECT_FALSE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), topic.dataset_id);
}

TEST(Scene3DDockPersistence, DeferredLayerReturnsToItsSavedDrawOrder) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "same.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;
  const PJ::ObjectTopicId ready = registerDepthTopic(session, dataset_id, "/ready", /*push_sample=*/false);

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  const auto append_layer = [&](const QString& topic_name, const QString& display_name) {
    QDomElement layer = doc.createElement(u"layer"_s);
    layer.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
    layer.setAttribute(u"dataset_source"_s, u"same.dat"_s);
    layer.setAttribute(u"topic_name"_s, topic_name);
    layer.setAttribute(u"object_type"_s, u"kImage"_s);
    layer.setAttribute(u"display_name"_s, display_name);
    state.appendChild(layer);
  };
  append_layer(u"/late"_s, u"late"_s);
  append_layer(u"/ready"_s, u"ready"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  ASSERT_EQ(dock.layers().size(), 1U);
  EXPECT_EQ(dock.layers().front().topic_id.id, ready.id);

  QDomDocument pending_snapshot_doc;
  const QDomElement pending_snapshot = dock.xmlSaveState(pending_snapshot_doc);
  int saved_layers = 0;
  bool saved_late_layer = false;
  for (QDomElement saved = pending_snapshot.firstChildElement(u"layer"_s); !saved.isNull();
       saved = saved.nextSiblingElement(u"layer"_s)) {
    ++saved_layers;
    saved_late_layer |= saved.attribute(u"topic_name"_s) == u"/late"_s;
  }
  EXPECT_EQ(saved_layers, 2) << "history XML must retain both live and deferred layers";
  EXPECT_TRUE(saved_late_layer) << "a deferred layer is still part of the restored workspace intent";

  int workspace_changes = 0;
  QObject::connect(
      &dock, &PJ::SceneDockWidget::workspaceChanged, &dock, [&workspace_changes]() { ++workspace_changes; });

  const PJ::ObjectTopicId late = registerDepthTopic(session, dataset_id, "/late", /*push_sample=*/false);
  EXPECT_EQ(dock.retryPendingRestores(QSet<QString>{u"/late"_s}), 1);
  EXPECT_EQ(workspace_changes, 0) << "materializing an already-restored pending layer is not a new user edit";
  const auto restored = dock.layers();
  ASSERT_EQ(restored.size(), 2U);
  EXPECT_EQ(restored[0].topic_id.id, late.id);
  EXPECT_EQ(restored[1].topic_id.id, ready.id);
}

TEST(Scene3DDockPersistence, DeferredMalformedLayerIsRemovedAndConsumedWhenTopicArrives) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "late.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  QDomElement layer = doc.createElement(u"layer"_s);
  layer.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  layer.setAttribute(u"dataset_source"_s, u"late.dat"_s);
  layer.setAttribute(u"topic_name"_s, u"/late_bad"_s);
  layer.setAttribute(u"object_type"_s, u"kImage"_s);
  layer.setAttribute(u"display_name"_s, u"late bad"_s);
  layer.appendChild(doc.createElement(u"not_a_depthcloud_payload"_s));
  state.appendChild(layer);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late_bad"_s});
  EXPECT_TRUE(dock.layers().empty());

  int workspace_changes = 0;
  QObject::connect(
      &dock, &PJ::SceneDockWidget::workspaceChanged, &dock, [&workspace_changes]() { ++workspace_changes; });
  registerDepthTopic(session, dataset_id, "/late_bad", /*push_sample=*/false);
  EXPECT_EQ(dock.retryPendingRestores(QSet<QString>{u"/late_bad"_s}), 0)
      << "an invalid pending payload is consumed but is not reported as restored";
  EXPECT_TRUE(dock.workspaceRestoreFailed())
      << "the progressive-layout owner must be able to roll back a permanently rejected pending element";
  EXPECT_TRUE(dock.unresolvedPendingRestores().isEmpty()) << "invalid XML must not retry forever";
  EXPECT_TRUE(dock.layers().empty()) << "the layer created before payload validation must be removed";
  EXPECT_FALSE(dock.hasTransformBufferForTest()) << "invalid pending restore must also release its dataset binding";
  EXPECT_EQ(dock.windowTitle(), u"3D View"_s);
  EXPECT_EQ(workspace_changes, 0) << "automatic pending cleanup is not a new user edit";
}

TEST(Scene3DDockPersistence, DeferredNestedRobotTopicMaterializesWithoutDuplicateLocalLayer) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robots.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendLocalRobotTopicLayer(doc, state, dataset_id, u"robots.dat"_s, u"/late_robot"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  EXPECT_TRUE(dock.layers().empty()) << "a layer with an unresolved nested source must not remain half-restored";
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late_robot"_s});
  EXPECT_FALSE(dock.workspaceRestoreFailed());

  // A rejected same-widget paste performs an internal live-state rollback. The
  // nested retry key is queue metadata (not an outer-layer XML attribute), so it
  // must survive that rollback together with the pending document.
  QDomDocument invalid_doc;
  QDomElement invalid_state = invalid_doc.createElement(u"scene3d"_s);
  QDomElement invalid_config = invalid_doc.createElement(u"config_topic"_s);
  invalid_config.setAttribute(u"topic_name"_s, u"/bad"_s);
  invalid_config.setAttribute(u"object_type"_s, u"kPointCloud"_s);
  invalid_state.appendChild(invalid_config);
  invalid_doc.appendChild(invalid_state);
  EXPECT_FALSE(dock.xmlLoadState(invalid_state));
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late_robot"_s});
  EXPECT_FALSE(dock.workspaceRestoreFailed()) << "rollback restores the prior pending status epoch";

  int workspace_changes = 0;
  QObject::connect(
      &dock, &PJ::SceneDockWidget::workspaceChanged, &dock, [&workspace_changes]() { ++workspace_changes; });
  const PJ::ObjectTopicId robot_topic = registerRobotTopic(session, dataset_id, "/late_robot");
  ASSERT_NE(robot_topic.id, 0U);
  EXPECT_EQ(dock.retryPendingRestores(QSet<QString>{u"/late_robot"_s}), 1);
  EXPECT_TRUE(dock.unresolvedPendingRestores().isEmpty());
  EXPECT_FALSE(dock.workspaceRestoreFailed());
  EXPECT_EQ(workspace_changes, 0) << "progressive materialization belongs to the original restore transaction";

  const auto layers = dock.layers();
  ASSERT_EQ(layers.size(), 1U) << "retry must replace the provisional local layer, not append a duplicate";
  auto* robot = dynamic_cast<pj::scene3d::RobotModelLayer*>(dock.layerFor(layers.front().topic_id));
  ASSERT_NE(robot, nullptr);
  ASSERT_NE(robot->robotModel(), nullptr);
  EXPECT_EQ(robot->robotModel()->root_link, "base_link");
  EXPECT_EQ(dock.retryPendingRestores({}), 0);
  EXPECT_EQ(dock.layers().size(), 1U);
}

TEST(Scene3DDockPersistence, DeferredNestedRobotWrongTypeIsPermanentRestoreFailure) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robots.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  appendLocalRobotTopicLayer(doc, state, dataset_id, u"robots.dat"_s, u"/late_robot"_s);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));
  ASSERT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late_robot"_s});

  const auto wrong_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = "/late_robot",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(wrong_topic.has_value()) << wrong_topic.error();
  EXPECT_EQ(dock.retryPendingRestores(QSet<QString>{u"/late_robot"_s}), 0);
  EXPECT_TRUE(dock.workspaceRestoreFailed())
      << "a live identity with the wrong type must make the progressive transaction roll back";
  EXPECT_TRUE(dock.unresolvedPendingRestores().isEmpty()) << "permanent failures must be consumed exactly once";
  EXPECT_TRUE(dock.layers().empty());
}

TEST(Scene3DDockPersistence, RejectedClipboardReplayPreservesPriorPendingQueue) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "pending.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  QDomDocument pending_doc;
  QDomElement pending_state = pending_doc.createElement(u"scene3d"_s);
  QDomElement pending_layer = pending_doc.createElement(u"layer"_s);
  pending_layer.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  pending_layer.setAttribute(u"dataset_source"_s, u"pending.dat"_s);
  pending_layer.setAttribute(u"topic_name"_s, u"/late"_s);
  pending_layer.setAttribute(u"object_type"_s, u"kImage"_s);
  pending_state.appendChild(pending_layer);
  pending_doc.appendChild(pending_state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(pending_state));
  ASSERT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late"_s});

  const PJ::ObjectTopicId ready = registerDepthTopic(session, dataset_id, "/ready", /*push_sample=*/false);
  QDomDocument invalid_doc;
  QDomElement invalid_state = invalid_doc.createElement(u"scene3d"_s);
  QDomElement invalid_layer = invalid_doc.createElement(u"layer"_s);
  invalid_layer.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
  invalid_layer.setAttribute(u"dataset_source"_s, u"pending.dat"_s);
  invalid_layer.setAttribute(u"topic_name"_s, u"/ready"_s);
  invalid_layer.setAttribute(u"object_type"_s, u"kImage"_s);
  invalid_layer.appendChild(invalid_doc.createElement(u"not_a_depthcloud_payload"_s));
  invalid_state.appendChild(invalid_layer);
  invalid_doc.appendChild(invalid_state);

  EXPECT_FALSE(dock.xmlLoadState(invalid_state));
  EXPECT_TRUE(dock.layers().empty());
  EXPECT_EQ(dock.unresolvedPendingRestores(), QStringList{u"/late"_s});
  EXPECT_FALSE(dock.workspaceRestoreFailed()) << "rollback restores the prior transaction's status epoch";
  EXPECT_NE(ready.id, 0U);
}

TEST(Scene3DDockPersistence, WorkspaceSignalTracksPresentationAndLayerConfigButNotXmlRestore) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic topic = registerDepthDataset(session, "cam.dat", "/depth", /*push_sample=*/true);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(topic.topic_id, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));

  // Round-trip once to force lazy view creation and exercise the restoration
  // guard before observing user-facing mutations.
  QDomDocument initial_doc;
  const QDomElement initial_state = dock.xmlSaveState(initial_doc);
  ASSERT_TRUE(dock.xmlLoadState(initial_state));
  ASSERT_NE(dock.sceneView(), nullptr);

  int workspace_changes = 0;
  QObject::connect(
      &dock, &PJ::SceneDockWidget::workspaceChanged, &dock, [&workspace_changes]() { ++workspace_changes; });

  auto* depth = dynamic_cast<pj::scene3d::DepthCloudLayer*>(dock.layerFor(topic.topic_id));
  ASSERT_NE(depth, nullptr);
  const float old_size = depth->pointSizePixels();
  depth->setPointSizePixels(old_size);
  EXPECT_EQ(workspace_changes, 0);
  depth->setPointSizePixels(old_size + 1.0f);
  EXPECT_EQ(workspace_changes, 1);

  auto* view = dock.sceneView();
  view->setGridVisible(!view->gridVisible());
  EXPECT_EQ(workspace_changes, 2);
  view->setGridVisible(view->gridVisible());
  EXPECT_EQ(workspace_changes, 2);
  auto shading = view->meshShadingParams();
  shading.meshes_visible = !shading.meshes_visible;
  view->setMeshShadingParams(shading);
  EXPECT_EQ(workspace_changes, 3);
  view->setMeshShadingParams(shading);
  EXPECT_EQ(workspace_changes, 3);
  view->setCameraModel(pj::scene3d::SceneViewWidget::CameraModel::kFly);
  EXPECT_EQ(workspace_changes, 4);
  view->setCameraModel(pj::scene3d::SceneViewWidget::CameraModel::kFly);
  EXPECT_EQ(workspace_changes, 4);

  dock.setFollowFrame(u"cam"_s);
  EXPECT_EQ(workspace_changes, 5);
  dock.setFollowFrame(u"cam"_s);
  EXPECT_EQ(workspace_changes, 5);
  dock.setFixedFrame(u"map"_s);
  EXPECT_EQ(workspace_changes, 6);

  const QPointF start(20.0, 20.0);
  const QPointF finish(45.0, 30.0);
  QMouseEvent press(QEvent::MouseButtonPress, start, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(view, &press);
  QMouseEvent move(QEvent::MouseMove, finish, finish, finish, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(view, &move);
  EXPECT_EQ(workspace_changes, 6) << "a camera drag must not snapshot every mouse-move event";
  QMouseEvent release(QEvent::MouseButtonRelease, finish, finish, finish, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view, &release);
  EXPECT_EQ(workspace_changes, 7) << "the camera drag commits exactly once on release";

  const QPointF wheel_pos(40.0, 40.0);
  QWheelEvent wheel(
      wheel_pos, wheel_pos, QPoint{}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
      /*inverted=*/false);
  QApplication::sendEvent(view, &wheel);
  EXPECT_EQ(workspace_changes, 8);
  view->resetCamera();
  EXPECT_EQ(workspace_changes, 9);
  view->resetCamera();
  EXPECT_EQ(workspace_changes, 9) << "Home on an already-default camera is not a workspace mutation";

  QDomDocument restored_doc;
  const QDomElement restored_state = dock.xmlSaveState(restored_doc);
  workspace_changes = 0;
  ASSERT_TRUE(dock.xmlLoadState(restored_state));
  EXPECT_EQ(workspace_changes, 0) << "undo/layout replay must not recursively create a new history entry";
}

// ---- Trail layers (role="trail", synthetic local ids) -----------------------

constexpr std::string_view kPosesSchema = "mock/poses_in_frame";

PJ::Expected<PJ::sdk::ObjectRecord> emitPoses(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  PJ::sdk::PosesInFrame poses;
  poses.timestamp_ns = ts;
  poses.frame_id = "odom";
  PJ::sdk::Pose pose;
  pose.position.x = 1.0;
  pose.position.y = 2.0;
  poses.poses.push_back(pose);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = poses};
}

PJ::ObjectTopicId registerPoseTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, const std::string& topic_name) {
  const auto topic_or = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = topic_name,
          .metadata_json = R"({"builtin_object_type":"kPosesInFrame"})",
      });
  EXPECT_TRUE(topic_or.has_value());
  if (!topic_or.has_value()) {
    return {};
  }
  session.registerObjectTopicParser(*topic_or, makeBoundHandle(kPosesSchema, []() noexcept -> void* {
    return new CountingObjectParser(kPosesSchema, PJ::sdk::BuiltinObjectType::kPosesInFrame, nullptr, &emitPoses);
  }));
  EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x01}).has_value());
  return *topic_or;
}

TEST(Scene3DDockPersistence, TfTrailRoundTripsThroughXml) {
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  const DatasetTopic saved = registerTfDataset(save_session, "trail.dat", "/tf");
  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));

  const PJ::ObjectTopicId trail_id = save_dock.addTrailLayer(pj::scene3d::TrailSource::tfFrame(u"base_link"_s));
  ASSERT_NE(trail_id.id, 0U);
  auto* trail = dynamic_cast<pj::scene3d::TrailLayer*>(save_dock.layerFor(trail_id));
  ASSERT_NE(trail, nullptr);
  trail->setPastColor(QColor(u"#112233"_s));
  trail->setFutureColor(QColor(u"#445566"_s));
  trail->setThickness(4.0F);

  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);
  const QDomElement layer_el = state.firstChildElement(u"layer"_s);
  ASSERT_FALSE(layer_el.isNull());
  EXPECT_EQ(layer_el.attribute(u"role"_s), u"trail"_s);
  EXPECT_EQ(layer_el.attribute(u"local"_s), u"true"_s);
  const QDomElement payload = layer_el.firstChildElement();
  EXPECT_EQ(payload.tagName(), u"trail"_s);
  EXPECT_EQ(payload.attribute(u"source_kind"_s), u"tf_frame"_s);
  EXPECT_EQ(payload.attribute(u"frame"_s), u"base_link"_s);

  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  registerTfDataset(load_session, "trail.dat", "/tf");
  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));

  ASSERT_EQ(load_dock.layers().size(), 1U);
  auto* restored = dynamic_cast<pj::scene3d::TrailLayer*>(load_dock.layerFor(load_dock.layers().front().topic_id));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->source().kind, pj::scene3d::TrailSource::Kind::kTfFrame);
  EXPECT_EQ(restored->source().frame, u"base_link"_s);
  EXPECT_EQ(restored->pastColor().name(), u"#112233"_s);
  EXPECT_EQ(restored->futureColor().name(), u"#445566"_s);
  EXPECT_FLOAT_EQ(restored->thickness(), 4.0F);
  // Layers replay BEFORE the config topic that binds TF; the dock's fan-out
  // must have delivered the buffer once the binding landed.
  EXPECT_NE(restored->statusWarning(), u"waiting for TF"_s);
}

TEST(Scene3DDockPersistence, PoseTrailDefersUntilTopicLoadsThenMaterializes) {
  // Hand-built saved element for a pose-source trail whose topic is not loaded.
  QDomDocument doc;
  QDomElement scene = doc.createElement(u"scene3d"_s);
  scene.setAttribute(u"version"_s, u"1"_s);
  doc.appendChild(scene);
  QDomElement layer_el = doc.createElement(u"layer"_s);
  layer_el.setAttribute(u"role"_s, u"trail"_s);
  layer_el.setAttribute(u"local"_s, u"true"_s);
  layer_el.setAttribute(u"object_type"_s, u"kNone"_s);
  layer_el.setAttribute(u"display_name"_s, u"Trail: /odom"_s);
  layer_el.setAttribute(u"visible"_s, u"true"_s);
  layer_el.setAttribute(u"order"_s, u"0"_s);
  QDomElement payload = doc.createElement(u"trail"_s);
  payload.setAttribute(u"source_kind"_s, u"pose_topic"_s);
  payload.setAttribute(u"source_topic_name"_s, u"/odom"_s);
  payload.setAttribute(u"source_dataset_source"_s, u"poses.dat"_s);
  payload.setAttribute(u"past_color"_s, u"#112233"_s);
  payload.setAttribute(u"future_color"_s, u"#445566"_s);
  layer_el.appendChild(payload);
  scene.appendChild(layer_el);

  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  ASSERT_TRUE(dock.xmlLoadState(scene));
  EXPECT_TRUE(dock.layers().empty()) << "the pose topic is absent: the trail must defer, not restore";
  EXPECT_TRUE(dock.unresolvedPendingRestores().contains(u"/odom"_s));

  // The dataset arrives; the pending retry materializes the trail.
  auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "poses.dat", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value());
  const PJ::ObjectTopicId pose_topic = registerPoseTopic(session, *dataset_or, "/odom");
  ASSERT_NE(pose_topic.id, 0U);
  EXPECT_EQ(dock.retryPendingRestores(QSet<QString>{u"/odom"_s}), 1);

  ASSERT_EQ(dock.layers().size(), 1U);
  auto* restored = dynamic_cast<pj::scene3d::TrailLayer*>(dock.layerFor(dock.layers().front().topic_id));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->source().kind, pj::scene3d::TrailSource::Kind::kPoseTopic);
  EXPECT_EQ(restored->source().topic, pose_topic);
  EXPECT_EQ(restored->pastColor().name(), u"#112233"_s);
}

TEST(Scene3DDockPersistence, PoseTrailBuildsPolylineFromDecodedMessages) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic tf = registerTfDataset(session, "posebuild.dat", "/tf");
  const PJ::ObjectTopicId pose_topic = registerPoseTopic(session, tf.dataset_id, "/odom");
  // A second message so the trail has two points (registerPoseTopic pushed ts=100).
  ASSERT_TRUE(session.objectStore().pushOwned(pose_topic, 200, std::vector<uint8_t>{0x02}).has_value());

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  const PJ::ObjectTopicId trail_id = dock.addTrailLayer(pj::scene3d::TrailSource::poseTopic(pose_topic));
  ASSERT_NE(trail_id.id, 0U);
  auto* trail = dynamic_cast<pj::scene3d::TrailLayer*>(dock.layerFor(trail_id));
  ASSERT_NE(trail, nullptr);

  // The mock parser emits frame_id "odom"; with the fixed frame equal to it the
  // pose lands identity-transformed (no TF needed) — full decode path headless.
  trail->rebuildNowForTest(u"odom"_s);
  EXPECT_EQ(trail->pointCountForTest(), 2U);
  EXPECT_TRUE(trail->statusWarning().isEmpty());
  EXPECT_EQ(PJ::toRaw(trail->timeRange().min), 100);
  EXPECT_EQ(PJ::toRaw(trail->timeRange().max), 200);
}

TEST(Scene3DDockPersistence, PoseTrailPrunesWithItsSourceTopicButTfTrailSurvives) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic tf = registerTfDataset(session, "prune.dat", "/tf");
  const PJ::ObjectTopicId pose_topic = registerPoseTopic(session, tf.dataset_id, "/odom");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  const PJ::ObjectTopicId pose_trail = dock.addTrailLayer(pj::scene3d::TrailSource::poseTopic(pose_topic));
  const PJ::ObjectTopicId tf_trail = dock.addTrailLayer(pj::scene3d::TrailSource::tfFrame(u"base_link"_s));
  ASSERT_NE(pose_trail.id, 0U);
  ASSERT_NE(tf_trail.id, 0U);
  ASSERT_EQ(dock.layers().size(), 2U);
  EXPECT_TRUE(dock.revalidateObjects());

  // Evict the pose SOURCE topic: its trail must be pruned (a synthetic-id layer
  // the standard descriptor sweep would skip), while the TF trail orphans and
  // survives.
  session.objectStore().removeTopic(pose_topic);
  EXPECT_TRUE(dock.revalidateObjects());
  ASSERT_EQ(dock.layers().size(), 1U);
  EXPECT_EQ(dock.layers().front().topic_id, tf_trail);
}

TEST(Scene3DDockPersistence, TfTrailRemovedWhenItsDatasetUnloads) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic tf = registerTfDataset(session, "unload.dat", "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  const PJ::ObjectTopicId tf_trail = dock.addTrailLayer(pj::scene3d::TrailSource::tfFrame(u"base_link"_s));
  ASSERT_NE(tf_trail.id, 0U);
  ASSERT_EQ(dock.layers().size(), 1U);

  // Removing the WHOLE dataset (its last tracked topic) resets the TF binding
  // and must take the TF trail with it — a gone dataset deletes the trail,
  // unlike a merely-missing frame, which only orphans it.
  session.objectStore().removeTopic(tf.topic_id);
  EXPECT_FALSE(dock.revalidateObjects()) << "no live content should remain";
  EXPECT_TRUE(dock.layers().empty()) << "the TF trail must be removed with its dataset";
}

TEST(Scene3DDockPersistence, PoseLayerOwnsEmbeddedTrailInsteadOfAddingALayer) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const DatasetTopic tf = registerTfDataset(session, "signal.dat", "/tf");
  const PJ::ObjectTopicId pose_topic = registerPoseTopic(session, tf.dataset_id, "/odom");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  ASSERT_TRUE(dock.addTopic(pose_topic, PJ::sdk::BuiltinObjectType::kPosesInFrame, u"poses"_s));
  ASSERT_EQ(dock.layers().size(), 1U);

  auto* poses = dynamic_cast<pj::scene3d::PosesInFrameLayer*>(dock.layerFor(pose_topic));
  ASSERT_NE(poses, nullptr);
  EXPECT_FALSE(poses->trailEnabled()) << "the trail is opt-in";

  poses->setTrailEnabled(true);
  ASSERT_TRUE(poses->trailEnabled());
  ASSERT_NE(poses->trail(), nullptr);
  // The whole point of the embedded shape: no second Topics row, no separate
  // settings panel — the trail is part of the pose layer.
  EXPECT_EQ(dock.layers().size(), 1U) << "an owned trail must not register as a dock layer";
  EXPECT_EQ(poses->trail()->source().kind, pj::scene3d::TrailSource::Kind::kPoseTopic);
  EXPECT_EQ(poses->trail()->source().topic, pose_topic);

  poses->trail()->setThickness(7.0F);
  poses->trail()->setPastColor(QColor(u"#123456"_s));
  poses->trail()->setFutureVisible(false);

  QDomDocument doc;
  const QDomElement state = dock.xmlSaveState(doc);
  // No layout element may carry the standalone trail role any more.
  for (QDomElement layer_el = state.firstChildElement(u"layer"_s); !layer_el.isNull();
       layer_el = layer_el.nextSiblingElement(u"layer"_s)) {
    EXPECT_NE(layer_el.attribute(u"role"_s), u"trail"_s) << "a pose trail must persist nested, not as its own layer";
  }

  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&session);
  load_dock.setTransformService(&transform_service);
  ASSERT_TRUE(load_dock.xmlLoadState(state));
  auto* restored = dynamic_cast<pj::scene3d::PosesInFrameLayer*>(load_dock.layerFor(pose_topic));
  ASSERT_NE(restored, nullptr);
  ASSERT_TRUE(restored->trailEnabled()) << "the nested <trail> payload is the enabled flag";
  EXPECT_EQ(load_dock.layers().size(), 1U);
  EXPECT_FLOAT_EQ(restored->trail()->thickness(), 7.0F);
  EXPECT_EQ(restored->trail()->pastColor().name(QColor::HexRgb), u"#123456"_s);
  EXPECT_FALSE(restored->trail()->futureVisible());

  // Disabling drops the trail entirely (and with it the nested payload).
  restored->setTrailEnabled(false);
  EXPECT_EQ(restored->trail(), nullptr);
  QDomDocument off_doc;
  const QDomElement off_state = restored->xmlSaveState(off_doc);
  EXPECT_TRUE(off_state.firstChildElement(u"trail"_s).isNull());
}

TEST(Scene3DDockPersistence, TrailRestoreRejectsMalformedElements) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);

  const auto try_restore = [&session, &transform_service](
                               const QString& source_kind, const QString& frame, const QString& order) {
    QDomDocument doc;
    QDomElement scene = doc.createElement(u"scene3d"_s);
    scene.setAttribute(u"version"_s, u"1"_s);
    doc.appendChild(scene);
    QDomElement layer_el = doc.createElement(u"layer"_s);
    layer_el.setAttribute(u"role"_s, u"trail"_s);
    layer_el.setAttribute(u"local"_s, u"true"_s);
    layer_el.setAttribute(u"object_type"_s, u"kNone"_s);
    layer_el.setAttribute(u"display_name"_s, u"Trail"_s);
    layer_el.setAttribute(u"visible"_s, u"true"_s);
    layer_el.setAttribute(u"order"_s, order);
    QDomElement payload = doc.createElement(u"trail"_s);
    payload.setAttribute(u"source_kind"_s, source_kind);
    if (!frame.isEmpty()) {
      payload.setAttribute(u"frame"_s, frame);
    }
    layer_el.appendChild(payload);
    scene.appendChild(layer_el);

    PJ::Scene3DDockWidget dock;
    dock.setSessionManager(&session);
    dock.setTransformService(&transform_service);
    // kInvalid aborts the whole replay: xmlLoadState rejects BEFORE mutation.
    const bool rejected = !dock.xmlLoadState(scene);
    return rejected && dock.layers().empty();
  };

  EXPECT_TRUE(try_restore(u"warp_drive"_s, u"base"_s, u"0"_s)) << "unknown source_kind must reject";
  EXPECT_TRUE(try_restore(u"tf_frame"_s, QString(), u"0"_s)) << "tf trail without a frame must reject";
  EXPECT_TRUE(try_restore(u"tf_frame"_s, u"base"_s, u"banana"_s)) << "malformed order must reject";
}

}  // namespace
