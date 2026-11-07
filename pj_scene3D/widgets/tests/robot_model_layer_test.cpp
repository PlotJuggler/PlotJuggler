// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/robot_model_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mesh_import.h"  // maxConcurrentMeshLoads
#include "mock_parser_support.h"
#include "pj_base/time.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/layout_resources.h"
#include "pj_tf/tf_buffer.h"
#include "pj_tf/transform.h"
#include "urdf_package_resolver.h"
using namespace Qt::StringLiterals;

namespace {

constexpr const char* kDecodedUrdf = R"(
<robot name="decoded">
  <link name="base_link">
    <visual>
      <geometry><box size="1 2 3"/></geometry>
    </visual>
  </link>
  <link name="tool0"/>
</robot>
)";

// A two-link robot whose child link is attached ONLY by a FIXED joint. Mirrors
// the DROID gripper case: the data publishes the parent frame but never the child,
// so the child renders only if the URDF's fixed joint is bridged into TF.
constexpr const char* kBridgeUrdf = R"(
<robot name="bridge">
  <link name="base_link"/>
  <joint name="mount" type="fixed">
    <parent link="base_link"/>
    <child link="gripper_base"/>
    <origin xyz="0 0 0.1"/>
  </joint>
  <link name="gripper_base"/>
</robot>
)";

// A mixed model: a link with a <visual> plus a collision-ONLY sublink attached by
// a fixed joint (mirrors panda_link*_sc self-collision capsules on the DROID arm).
constexpr const char* kMixedCollisionUrdf = R"(
<robot name="mixed">
  <link name="body">
    <visual><geometry><box size="1 1 1"/></geometry></visual>
  </link>
  <joint name="sc" type="fixed">
    <parent link="body"/><child link="body_sc"/><origin xyz="0 0 0"/>
  </joint>
  <link name="body_sc">
    <collision><geometry><sphere radius="0.5"/></geometry></collision>
  </link>
</robot>
)";

// An entirely-collision model (no <visual> anywhere): kAuto must promote it to the
// visuals group so it renders solid.
constexpr const char* kAllCollisionUrdf = R"(
<robot name="allcol">
  <link name="solid">
    <collision><geometry><sphere radius="0.5"/></geometry></collision>
  </link>
</robot>
)";

constexpr const char* kUnresolvedMeshUrdf = R"(
<robot name="mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="package://missing_pkg/meshes/body.dae"/></geometry>
    </visual>
  </link>
</robot>
)";

// URDF whose single visual mesh resolves immediately (file:// fixture path), so
// attach() actually kicks an async MeshLoader load.
const std::string& resolvedMeshUrdf() {
  static const std::string text = std::string(R"(
<robot name="resolved_mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="file://)") +
                                  PJ_SCENE3D_FIXTURES_DIR + R"(/meshes/cube.stl"/></geometry>
    </visual>
  </link>
</robot>
)";
  return text;
}

// URDF whose single visual mesh is a resolved .dae (file:// fixture path). The
// COLLADA up-axis override only affects .dae/.collada, so this exercises the
// setIgnoreColladaUpAxis reload path.
const std::string& resolvedColladaMeshUrdf() {
  static const std::string text = std::string(R"(
<robot name="collada_mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="file://)") +
                                  PJ_SCENE3D_FIXTURES_DIR + R"(/meshes/cube.dae"/></geometry>
    </visual>
  </link>
</robot>
)";
  return text;
}

class UrdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  UrdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kDecodedUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class SdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  SdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "sdf",
              .text = "<sdf/>",
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class BridgeUrdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  BridgeUrdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kBridgeUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class MixedCollisionRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  MixedCollisionRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts, .topic = "/robot_description", .format = "urdf", .text = kMixedCollisionUrdf}},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class AllCollisionRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  AllCollisionRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts, .topic = "/robot_description", .format = "urdf", .text = kAllCollisionUrdf}},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class UnresolvedMeshRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  UnresolvedMeshRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kUnresolvedMeshUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class ResolvedMeshRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  ResolvedMeshRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = resolvedMeshUrdf(),
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// A topic URDF naming one mesh through a package and one through a direct
// http URL: an explicit package root may authorize the first, never the second.
constexpr const char* kPackageUrlUrdf = R"(
<robot name="url_roots">
  <link name="base_link">
    <visual><geometry><mesh filename="package://pj_url_pkg/meshes/cube.stl"/></geometry></visual>
  </link>
  <link name="tool0">
    <visual><geometry><mesh filename="https://blocked.invalid/tool.stl"/></geometry></visual>
  </link>
  <joint name="mount" type="fixed"><parent link="base_link"/><child link="tool0"/></joint>
</robot>
)";

class PackageUrlRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  PackageUrlRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kPackageUrlUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// Rebind-test parse counters. vtableWithCreate() requires a non-capturing create
// function, so the counting parsers reach these via file scope rather than a
// lambda capture.
std::atomic<int> g_first_urdf_parser_calls{0};
std::atomic<int> g_second_urdf_parser_calls{0};
// Counts decodes of the resolved-collada URDF so the up-axis toggle test can
// prove setIgnoreColladaUpAxis re-decoded the source.
std::atomic<int> g_collada_urdf_parser_calls{0};

// Emits the resolved-collada URDF and counts each decode, for the up-axis toggle
// reload test.
class CountingColladaUrdfParser final : public PJ::MessageParserPluginBase {
 public:
  CountingColladaUrdfParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      g_collada_urdf_parser_calls.fetch_add(1);
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = resolvedColladaMeshUrdf(),
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// Counts parseObject invocations so a rebind test can prove which parser a
// re-decode reached. Emits a fixed minimal URDF so the layer still parses a
// valid model.
class CountingUrdfParser final : public PJ::MessageParserPluginBase {
 public:
  explicit CountingUrdfParser(std::atomic<int>* counter) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object =
        [counter](PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      counter->fetch_add(1);
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kDecodedUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// vtableWithCreate() holds one static vtable per CreateFn instantiation, so each
// counting parser needs its own distinct lambda type — a shared function-pointer
// type would latch the first counter for both handles.
template <typename CreateFn>
const PJ_message_parser_vtable_t* countingUrdfVtable(CreateFn create_fn) {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      create_fn,
      R"({"id":"robot-counting-urdf-test","name":"Robot Counting URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* urdfParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new UrdfRobotDescriptionParser(); },
      R"({"id":"robot-urdf-test","name":"Robot URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* sdfParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new SdfRobotDescriptionParser(); },
      R"({"id":"robot-sdf-test","name":"Robot SDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* bridgeParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new BridgeUrdfRobotDescriptionParser(); },
      R"({"id":"robot-bridge-test","name":"Robot Bridge URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* mixedCollisionParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new MixedCollisionRobotDescriptionParser(); },
      R"({"id":"robot-mixed-col-test","name":"Robot Mixed Collision Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* allCollisionParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new AllCollisionRobotDescriptionParser(); },
      R"({"id":"robot-all-col-test","name":"Robot All Collision Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* unresolvedMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new UnresolvedMeshRobotDescriptionParser(); },
      R"({"id":"robot-mesh-test","name":"Robot Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* resolvedMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new ResolvedMeshRobotDescriptionParser(); },
      R"({"id":"robot-resolved-mesh-test","name":"Robot Resolved Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* colladaMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new CountingColladaUrdfParser(); },
      R"({"id":"robot-collada-mesh-test","name":"Robot Collada Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* packageUrlParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new PackageUrlRobotDescriptionParser(); },
      R"({"id":"robot-package-url-test","name":"Robot Package URL Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

PJ::ObjectTopicId registerTopic(PJ::SessionManager& session, std::string topic_name = "/robot_description") {
  auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = 1, .topic_name = std::move(topic_name), .metadata_json = "{}"});
  EXPECT_TRUE(topic.has_value());
  return *topic;
}

PJ::DatasetId createDataset(PJ::SessionManager& session, std::string source_name) {
  const auto dataset = session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = std::move(source_name), .time_domain_id = 0});
  EXPECT_TRUE(dataset.has_value());
  return dataset.has_value() ? *dataset : 0;
}

PJ::ObjectTopicId registerTopicForDataset(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string topic_name = "/robot_description") {
  auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = std::move(topic_name),
          .metadata_json = R"({"builtin_object_type":"kRobotDescription"})"});
  EXPECT_TRUE(topic.has_value());
  return topic.has_value() ? *topic : PJ::ObjectTopicId{};
}

void registerParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id, const PJ_message_parser_vtable_t* vtable) {
  auto parser = std::make_unique<PJ::MessageParserHandle>(vtable);
  ASSERT_TRUE(parser->bindSchema("robot_description", PJ::Span<const uint8_t>{}));
  session.registerObjectTopicParser(topic_id, std::move(parser));
}

pj::scene3d::Scene3DLayerContext makeContext(PJ::SessionManager& session) {
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = std::make_shared<pj::tf::TransformBuffer>();
  return ctx;
}

void pushWireBytes(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  ASSERT_TRUE(session.objectStore().pushOwned(topic_id, 123, std::vector<uint8_t>{0x00, 0x04, 0xFF, 0x10}));
}

QString serializedLayerState(const pj::scene3d::RobotModelLayer& layer) {
  QDomDocument doc;
  doc.appendChild(layer.xmlSaveState(doc));
  return doc.toString(-1);
}

}  // namespace

TEST(RobotModelLayerTest, TopicSourceDecodesThroughParserAndAppliesFramePrefix) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  layer.setFramePrefix(u"robot1/"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.robotModel()->root_link, "base_link");
  EXPECT_EQ(layer.sourceFrame(), u"robot1/base_link"_s);
  EXPECT_EQ(layer.linkFrameName("tool0"), u"robot1/tool0"_s);
  EXPECT_TRUE(layer.fallbackFrames().contains("robot1/base_link"_L1));
}

TEST(RobotModelLayerTest, TimeRangeIsInvertedEvenWhenLatchIsMissing) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  const auto range = layer.timeRange();
  EXPECT_EQ(range.min, PJ::Timepoint::max());
  EXPECT_EQ(range.max, PJ::Timepoint::min());
  EXPECT_GT(range.min, range.max);
  EXPECT_TRUE(layer.statusText().contains("Waiting for /robot_description"_L1));
}

TEST(RobotModelLayerTest, FormatOtherThanUrdfIsRejectedWithStatus) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, sdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(layer.robotModel(), nullptr);
  EXPECT_TRUE(layer.statusText().contains("Format 'sdf' is not supported"_L1));
}

TEST(RobotModelLayerTest, UnresolvedPackageMeshIsCountedForPlaceholderRendering) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, unresolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.totalMeshCount(), 1);
  EXPECT_EQ(layer.unresolvedMeshCount(), 1);
  EXPECT_TRUE(layer.statusText().contains("packages unresolved"_L1));
}

// Regression: a finished async URDF mesh load must itself request the repaint
// that consumes it (QFutureWatcher -> pollMeshLoads). The app paints strictly on
// demand, so before the fix the result was only drained inside render() and the
// magenta placeholder lingered until the user scrubbed. After attach() kicks the
// load, only the event loop is pumped here — no render() or setTrackerTime() —
// and repaintRequested must still fire.
TEST(RobotModelLayerTest, MeshLoadCompletionRequestsRepaintWithoutRender) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, resolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));  // parses the URDF and kicks the fixture mesh load
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.totalMeshCount(), 1);
  ASSERT_EQ(layer.unresolvedMeshCount(), 0) << "fixture mesh must resolve or no load is kicked";

  // Connect AFTER attach so synchronous emissions during attach can't count.
  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::RobotModelLayer::repaintRequested, [&repaints] { ++repaints; });

  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "finished mesh load did not request a repaint (only render() would have consumed it)";
}

// M.28 regression: the "Ignore COLLADA up_axis" toggle must actually reload the
// model (the flip is baked into the loaded MeshData, so a bare repaint would do
// nothing). Toggling setIgnoreColladaUpAxis on a .dae-mesh URDF re-decodes the
// source and re-kicks the mesh load; an unchanged value is a no-op.
TEST(RobotModelLayerTest, ColladaUpAxisToggleReloadsModel) {
  g_collada_urdf_parser_calls.store(0);

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, colladaMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.totalMeshCount(), 1);
  ASSERT_EQ(layer.unresolvedMeshCount(), 0) << "the .dae fixture mesh must resolve";
  const int decodes_after_attach = g_collada_urdf_parser_calls.load();
  ASSERT_GE(decodes_after_attach, 1);

  // Toggling the flag must re-decode the source (loadFromCurrentSource), so the
  // .dae re-imports under the new effective flip.
  layer.setIgnoreColladaUpAxis(true);
  EXPECT_GT(g_collada_urdf_parser_calls.load(), decodes_after_attach)
      << "setIgnoreColladaUpAxis did not reload the model";
  EXPECT_NE(layer.robotModel(), nullptr) << "model dropped after the up-axis toggle";
  EXPECT_EQ(layer.totalMeshCount(), 1);

  // Setting the same value again is a no-op (no extra decode).
  const int decodes_after_toggle = g_collada_urdf_parser_calls.load();
  layer.setIgnoreColladaUpAxis(true);
  EXPECT_EQ(g_collada_urdf_parser_calls.load(), decodes_after_toggle)
      << "an unchanged setIgnoreColladaUpAxis must not reload";

  // Drain the kicked mesh load so its watcher fires (and is severed) before
  // teardown, then assert the reloaded mesh actually loaded: the watcher-driven
  // pollMeshLoads recomputes loaded_mesh_count_ to total via updateMeshCounters.
  int repaints = 0;
  QObject::connect(
      &layer, &pj::scene3d::RobotModelLayer::meshLoadStatusChanged,
      [&repaints](int, int, const QStringList&) { ++repaints; });
  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "reloaded .dae mesh load never completed (no meshLoadStatusChanged)";
}

// Regression for the file-reload UAF (PR #175 pattern): SessionManager::
// replaceDataset() re-registers each surviving topic's parser slot, destroying
// the previous MessageParserHandle. A layer that cached the raw parser pointer
// at attach() then decodes through freed memory on the next re-decode.
//
// The robot layer's risk is the latch-retry path: attach() on an empty topic
// arms latch_pending_, and every setTrackerTime() while pending re-invokes
// tryLoadTopicDescription(). With a cached parser_ that decode reaches the freed
// handle after a reload; resolving the binding per use rebinds it to the new
// parser. This test drives exactly that path (attach empty -> reload -> push ->
// tick past the 500ms retry interval) so it FAILS on the cached-pointer code and
// passes once the binding is resolved per use.
TEST(RobotModelLayerTest, ReloadSwapsParserWithoutTouchingStaleOne) {
  g_first_urdf_parser_calls.store(0);
  g_second_urdf_parser_calls.store(0);

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, countingUrdfVtable([]() noexcept -> void* {
                   return new CountingUrdfParser(&g_first_urdf_parser_calls);
                 }));
  // NOTE: no payload yet — attach must latch and wait for the first sample.

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_EQ(layer.robotModel(), nullptr);  // nothing to decode yet -> latched
  EXPECT_EQ(g_first_urdf_parser_calls.load(), 0);

  // Keep the first parser's memory readable from the test so the buggy
  // stale-pointer call below is a deterministic wrong-parser hit rather than UB.
  // Production holds no such guard — there the same call is a use-after-free.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: re-register the surviving topic's parser under
  // its stable id, overwriting the slot and dropping the old handle. Then the new
  // generation's first sample arrives.
  registerParser(session, topic_id, countingUrdfVtable([]() noexcept -> void* {
                   return new CountingUrdfParser(&g_second_urdf_parser_calls);
                 }));
  pushWireBytes(session, topic_id);

  // The latch retry is rate-limited to 500ms; wait it out so the next tick fires
  // tryLoadTopicDescription() (and pump the event loop in case anything posted).
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 600) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  layer.setTrackerTime(PJ::fromRaw(123));  // pending + interval elapsed -> retry decode

  EXPECT_EQ(g_first_urdf_parser_calls.load(), 0)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_urdf_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
  EXPECT_NE(layer.robotModel(), nullptr) << "latch retry did not decode the new sample";
}

// A dataset replacement swaps the robot_description bytes under the stable topic
// id: the previous dataset's model must go at once and the description be decoded
// again from the replacement (after the swap, which runs no event loop).
TEST(RobotModelLayerTest, DatasetReplacementReloadsTopicDescription) {
  g_first_urdf_parser_calls.store(0);
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, countingUrdfVtable([]() noexcept -> void* {
                   return new CountingUrdfParser(&g_first_urdf_parser_calls);
                 }));
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  const int parses_before = g_first_urdf_parser_calls.load();

  emit session.datasetAboutToBeReplaced(session.objectStore().descriptor(topic_id).dataset_id);
  EXPECT_EQ(layer.robotModel(), nullptr) << "the replaced dataset's model is still shown";

  QElapsedTimer timer;
  timer.start();
  while (layer.robotModel() == nullptr && timer.elapsed() < 2000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  EXPECT_GT(g_first_urdf_parser_calls.load(), parses_before) << "the description was not decoded again";
  EXPECT_NE(layer.robotModel(), nullptr) << "no model loaded from the replacement";
}

// M.49/H.14 regression: the kUrl source must not block the caller. setSourceUrl
// (and therefore attach()/xmlLoadState restores and undo/redo) only kicks the
// fetch off — nothing is applied synchronously — and the model lands later
// through the event loop. The old code spun a nested QEventLoop here, which
// both froze the UI for up to 15s and allowed re-entrant layer destruction.
TEST(RobotModelLayerTest, UrlSourceLoadsAsynchronouslyFromFileUrl) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString urdf_path = dir.filePath(u"decoded.urdf"_s);
  {
    QFile file(urdf_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(kDecodedUrdf);
  }

  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{.id = 1}, u"robot"_s);
  layer.setSourceUrl(QUrl::fromLocalFile(urdf_path).toString());

  // Asynchronous kickoff: nothing may have been applied before returning.
  EXPECT_EQ(layer.robotModel(), nullptr) << "kUrl load applied synchronously (blocking-fetch regression)";
  EXPECT_TRUE(layer.statusText().contains("Fetching"_L1)) << layer.statusText().toStdString();

  QElapsedTimer timer;
  timer.start();
  while (layer.robotModel() == nullptr && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  ASSERT_NE(layer.robotModel(), nullptr) << "async URL fetch never applied the model; status: "
                                         << layer.statusText().toStdString();
  EXPECT_EQ(layer.robotModel()->root_link, "base_link");
  EXPECT_FALSE(layer.statusText().contains("Fetching"_L1));
}

// M.27: a kTopic robot layer must persist enough identity (dataset + topic name)
// that xmlLoadState re-resolves the same topic and re-decodes its model. The
// round trip must survive a fresh layer wired to a different default topic id.
TEST(RobotModelLayerTest, TopicSourceIdentitySurvivesSaveLoadRoundTrip) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset_id = createDataset(session, "same-session.dat");
  const PJ::ObjectTopicId topic_id = registerTopicForDataset(session, dataset_id, "/robot_description");
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer source_layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(source_layer.attach(ctx));
  ASSERT_NE(source_layer.robotModel(), nullptr);

  QDomDocument doc;
  QDomElement saved = source_layer.xmlSaveState(doc);
  EXPECT_EQ(saved.attribute(u"source_dataset_source"_s), u"same-session.dat"_s);

  // A second topic with a different id stands in for "the constructor default no
  // longer points at /robot_description" — the restore must re-resolve by name.
  // It has its own (SDF) parser, so a layer left on this default would decode the
  // WRONG, unsupported model; only re-resolution by name yields the URDF.
  const PJ::ObjectTopicId other_id = registerTopicForDataset(session, dataset_id, "/other_description");
  ASSERT_NE(other_id.id, topic_id.id);
  registerParser(session, other_id, sdfParserVtable());
  pushWireBytes(session, other_id);

  pj::scene3d::RobotModelLayer restored_layer(other_id, u"/other_description"_s);
  ASSERT_TRUE(restored_layer.attach(ctx));
  int configuration_changes = 0;
  QObject::connect(
      &restored_layer, &PJ::ISceneLayer::configurationChanged, &restored_layer,
      [&configuration_changes]() { ++configuration_changes; });
  ASSERT_TRUE(restored_layer.xmlLoadState(saved));
  EXPECT_EQ(configuration_changes, 1) << "topic-source XML paste must produce one committed configuration change";
  ASSERT_NE(restored_layer.robotModel(), nullptr) << "restored layer did not re-resolve the persisted source topic";
  EXPECT_EQ(restored_layer.robotModel()->root_link, "base_link");
  configuration_changes = 0;
  ASSERT_TRUE(restored_layer.xmlLoadState(saved));
  EXPECT_EQ(configuration_changes, 0) << "re-applying identical robot parameters is not a workspace mutation";

  // Pre-source-attribute layouts retain their safe same-session raw-id lookup.
  saved.removeAttribute(u"source_dataset_source"_s);
  pj::scene3d::RobotModelLayer legacy_layer(other_id, u"/other_description"_s);
  ASSERT_TRUE(legacy_layer.attach(ctx));
  ASSERT_TRUE(legacy_layer.xmlLoadState(saved));
  ASSERT_NE(legacy_layer.robotModel(), nullptr);
  EXPECT_EQ(legacy_layer.robotModel()->root_link, "base_link");

  // Generic layouts remove both qualifiers. A globally unique topic+type may
  // still bind to the loaded recording.
  saved.removeAttribute(u"source_dataset_id"_s);
  pj::scene3d::RobotModelLayer generic_layer(other_id, u"/other_description"_s);
  ASSERT_TRUE(generic_layer.attach(ctx));
  ASSERT_TRUE(generic_layer.xmlLoadState(saved));
  ASSERT_NE(generic_layer.robotModel(), nullptr);
  EXPECT_EQ(generic_layer.robotModel()->root_link, "base_link");
}

TEST(RobotModelLayerTest, TopicSourceResolvesRemintedDatasetIdByUniqueRawSource) {
  PJ::SessionManager save_session;
  const PJ::DatasetId saved_dataset = createDataset(save_session, "robot-source.dat");
  const PJ::ObjectTopicId saved_topic = registerTopicForDataset(save_session, saved_dataset, "/robot_description");
  registerParser(save_session, saved_topic, urdfParserVtable());
  pushWireBytes(save_session, saved_topic);
  pj::scene3d::RobotModelLayer source(saved_topic, u"/robot_description"_s);
  ASSERT_TRUE(source.attach(makeContext(save_session)));

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);
  ASSERT_EQ(saved.attribute(u"source_dataset_id"_s).toUInt(), saved_dataset);
  ASSERT_EQ(saved.attribute(u"source_dataset_source"_s), u"robot-source.dat"_s);

  PJ::SessionManager load_session;
  const PJ::DatasetId decoy_dataset = createDataset(load_session, "decoy.dat");
  const PJ::ObjectTopicId decoy_topic = registerTopicForDataset(load_session, decoy_dataset, "/robot_description");
  registerParser(load_session, decoy_topic, sdfParserVtable());
  pushWireBytes(load_session, decoy_topic);
  const PJ::DatasetId reminted_dataset = createDataset(load_session, "robot-source.dat");
  ASSERT_NE(reminted_dataset, saved_dataset);
  const PJ::ObjectTopicId reminted_topic =
      registerTopicForDataset(load_session, reminted_dataset, "/robot_description");
  registerParser(load_session, reminted_topic, urdfParserVtable());
  pushWireBytes(load_session, reminted_topic);

  pj::scene3d::RobotModelLayer restored(decoy_topic, u"/robot_description"_s);
  ASSERT_TRUE(restored.attach(makeContext(load_session)));
  ASSERT_TRUE(restored.xmlLoadState(saved));
  ASSERT_NE(restored.robotModel(), nullptr) << restored.statusText().toStdString();
  EXPECT_EQ(restored.robotModel()->root_link, "base_link");

  QDomDocument re_saved_doc;
  const QDomElement re_saved = restored.xmlSaveState(re_saved_doc);
  EXPECT_EQ(re_saved.attribute(u"source_dataset_id"_s).toUInt(), reminted_dataset);
  EXPECT_EQ(re_saved.attribute(u"source_dataset_source"_s), u"robot-source.dat"_s);
}

TEST(RobotModelLayerTest, TopicSourceFullPathRejectsSameBasenameRemintedIdCollision) {
  PJ::SessionManager save_session;
  const PJ::DatasetId saved_dataset = createDataset(save_session, "robot-source.dat");
  const PJ::ObjectTopicId saved_topic = registerTopicForDataset(save_session, saved_dataset, "/robot_description");
  registerParser(save_session, saved_topic, urdfParserVtable());
  pushWireBytes(save_session, saved_topic);
  pj::scene3d::RobotModelLayer source(saved_topic, u"/robot_description"_s);
  ASSERT_TRUE(source.attach(makeContext(save_session)));

  QDomDocument doc;
  QDomElement saved = source.xmlSaveState(doc);
  const QString intended_path = u"wanted/robot-source.dat"_s;
  saved.setAttribute(u"source_dataset_path"_s, intended_path);

  PJ::SessionManager load_session;
  // First id collides numerically with the save session and has the same raw
  // basename, but comes from a different file and decodes to the wrong model.
  const PJ::DatasetId collision_dataset = createDataset(load_session, "robot-source.dat");
  saved.setAttribute(u"source_dataset_id"_s, QString::number(collision_dataset));
  const PJ::ObjectTopicId collision_topic =
      registerTopicForDataset(load_session, collision_dataset, "/robot_description");
  registerParser(load_session, collision_topic, sdfParserVtable());
  pushWireBytes(load_session, collision_topic);
  load_session.setDatasetSourcePath(collision_dataset, u"other/robot-source.dat"_s);

  const PJ::DatasetId intended_dataset = createDataset(load_session, "robot-source.dat");
  const PJ::ObjectTopicId intended_topic =
      registerTopicForDataset(load_session, intended_dataset, "/robot_description");
  registerParser(load_session, intended_topic, urdfParserVtable());
  pushWireBytes(load_session, intended_topic);
  load_session.setDatasetSourcePath(intended_dataset, intended_path);

  pj::scene3d::RobotModelLayer restored(collision_topic, u"/robot_description"_s);
  ASSERT_TRUE(restored.attach(makeContext(load_session)));
  ASSERT_TRUE(restored.xmlLoadState(saved)) << restored.statusText().toStdString();
  ASSERT_NE(restored.robotModel(), nullptr);
  EXPECT_EQ(restored.robotModel()->root_link, "base_link")
      << "the saved full path must override the coincidentally reminted same-basename id";
}

TEST(RobotModelLayerTest, QualifiedTopicSourceTypeMismatchLeavesLiveLayerUntouched) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = createDataset(session, "robot-source.dat");
  const PJ::ObjectTopicId robot_topic = registerTopicForDataset(session, dataset, "/robot_description");
  registerParser(session, robot_topic, urdfParserVtable());
  pushWireBytes(session, robot_topic);
  const auto wrong_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/same_name_but_cloud",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(wrong_topic.has_value()) << wrong_topic.error();

  pj::scene3d::RobotModelLayer layer(robot_topic, u"/robot_description"_s);
  ASSERT_TRUE(layer.attach(makeContext(session)));
  ASSERT_NE(layer.robotModel(), nullptr);
  const QString state_before = serializedLayerState(layer);
  const QString status_before = layer.statusText();
  const std::string root_before = layer.robotModel()->root_link;
  int configuration_changes = 0;
  int repaints = 0;
  int status_changes = 0;
  QObject::connect(
      &layer, &PJ::ISceneLayer::configurationChanged, &layer, [&configuration_changes] { ++configuration_changes; });
  QObject::connect(&layer, &PJ::ISceneLayer::repaintRequested, &layer, [&repaints] { ++repaints; });
  QObject::connect(&layer, &pj::scene3d::RobotModelLayer::statusTextChanged, &layer, [&status_changes](const QString&) {
    ++status_changes;
  });

  QDomDocument doc;
  QDomElement saved = doc.createElement(u"robot_model"_s);
  saved.setAttribute(u"source_type"_s, u"topic"_s);
  saved.setAttribute(u"source_topic_name"_s, u"/same_name_but_cloud"_s);
  saved.setAttribute(u"source_dataset_id"_s, QString::number(dataset));
  saved.setAttribute(u"source_dataset_source"_s, u"robot-source.dat"_s);

  EXPECT_FALSE(layer.xmlLoadState(saved));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.robotModel()->root_link, root_before);
  EXPECT_EQ(serializedLayerState(layer), state_before);
  EXPECT_EQ(layer.statusText(), status_before);
  EXPECT_EQ(configuration_changes, 0);
  EXPECT_EQ(repaints, 0);
  EXPECT_EQ(status_changes, 0);
}

TEST(RobotModelLayerTest, DeferredTopicIdentityLeavesLiveLayerAndSignalsUntouched) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset = createDataset(session, "current.dat");
  const PJ::ObjectTopicId current_topic = registerTopicForDataset(session, dataset, "/robot_description");
  registerParser(session, current_topic, urdfParserVtable());
  pushWireBytes(session, current_topic);

  pj::scene3d::RobotModelLayer layer(current_topic, u"/robot_description"_s);
  ASSERT_TRUE(layer.attach(makeContext(session)));
  ASSERT_NE(layer.robotModel(), nullptr);
  const QString state_before = serializedLayerState(layer);
  const QString status_before = layer.statusText();
  const std::string root_before = layer.robotModel()->root_link;
  int configuration_changes = 0;
  int repaints = 0;
  int info_changes = 0;
  int visibility_changes = 0;
  int status_changes = 0;
  QObject::connect(
      &layer, &PJ::ISceneLayer::configurationChanged, &layer, [&configuration_changes] { ++configuration_changes; });
  QObject::connect(&layer, &PJ::ISceneLayer::repaintRequested, &layer, [&repaints] { ++repaints; });
  QObject::connect(&layer, &PJ::ISceneLayer::infoChanged, &layer, [&info_changes] { ++info_changes; });
  QObject::connect(
      &layer, &PJ::ISceneLayer::visibilityChanged, &layer, [&visibility_changes](bool) { ++visibility_changes; });
  QObject::connect(&layer, &pj::scene3d::RobotModelLayer::statusTextChanged, &layer, [&status_changes](const QString&) {
    ++status_changes;
  });

  QDomDocument doc;
  QDomElement pending = doc.createElement(u"robot_model"_s);
  pending.setAttribute(u"source_type"_s, u"topic"_s);
  pending.setAttribute(u"source_value"_s, u"/late_robot"_s);
  pending.setAttribute(u"source_topic_name"_s, u"/late_robot"_s);
  pending.setAttribute(u"source_dataset_id"_s, QString::number(dataset));
  pending.setAttribute(u"source_dataset_source"_s, u"current.dat"_s);
  pending.setAttribute(u"frame_prefix"_s, u"would_have_mutated/"_s);
  pending.setAttribute(u"display_mode"_s, u"collision"_s);
  pending.setAttribute(u"visible"_s, u"false"_s);
  pending.setAttribute(u"color"_s, u"#123456"_s);
  pending.setAttribute(u"ignore_collada_up_axis"_s, u"true"_s);
  doc.appendChild(pending);

  EXPECT_EQ(layer.xmlLoadStateResult(pending), pj::scene3d::RobotModelLayer::XmlLoadResult::kDeferred);
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.robotModel()->root_link, root_before);
  EXPECT_EQ(serializedLayerState(layer), state_before);
  EXPECT_EQ(layer.statusText(), status_before);
  EXPECT_EQ(configuration_changes, 0);
  EXPECT_EQ(repaints, 0);
  EXPECT_EQ(info_changes, 0);
  EXPECT_EQ(visibility_changes, 0);
  EXPECT_EQ(status_changes, 0);
}

TEST(RobotModelLayerTest, DuplicateRawSourceDoesNotSelectFirstDatasetAfterIdRemint) {
  PJ::SessionManager save_session;
  const PJ::DatasetId saved_dataset = createDataset(save_session, "duplicate.dat");
  const PJ::ObjectTopicId saved_topic = registerTopicForDataset(save_session, saved_dataset, "/robot_description");
  registerParser(save_session, saved_topic, urdfParserVtable());
  pushWireBytes(save_session, saved_topic);
  pj::scene3d::RobotModelLayer source(saved_topic, u"/robot_description"_s);
  ASSERT_TRUE(source.attach(makeContext(save_session)));
  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  PJ::SessionManager load_session;
  const PJ::DatasetId decoy_dataset = createDataset(load_session, "decoy.dat");
  const PJ::ObjectTopicId decoy_topic = registerTopicForDataset(load_session, decoy_dataset, "/robot_description");
  registerParser(load_session, decoy_topic, urdfParserVtable());
  pushWireBytes(load_session, decoy_topic);
  const PJ::DatasetId duplicate_a = createDataset(load_session, "duplicate.dat");
  const PJ::DatasetId duplicate_b = createDataset(load_session, "duplicate.dat");
  ASSERT_NE(duplicate_a, duplicate_b);
  const PJ::ObjectTopicId topic_a = registerTopicForDataset(load_session, duplicate_a, "/robot_description");
  const PJ::ObjectTopicId topic_b = registerTopicForDataset(load_session, duplicate_b, "/robot_description");
  registerParser(load_session, topic_a, urdfParserVtable());
  registerParser(load_session, topic_b, bridgeParserVtable());
  pushWireBytes(load_session, topic_a);
  pushWireBytes(load_session, topic_b);

  pj::scene3d::RobotModelLayer restored(decoy_topic, u"/robot_description"_s);
  ASSERT_TRUE(restored.attach(makeContext(load_session)));
  ASSERT_NE(restored.robotModel(), nullptr) << "test premise: constructor default loaded the decoy robot";
  const QString restored_state_before = serializedLayerState(restored);
  const QString restored_status_before = restored.statusText();
  const std::string restored_root_before = restored.robotModel()->root_link;
  ASSERT_FALSE(restored.xmlLoadState(saved));
  ASSERT_NE(restored.robotModel(), nullptr);
  EXPECT_EQ(restored.robotModel()->root_link, restored_root_before);
  EXPECT_EQ(serializedLayerState(restored), restored_state_before);
  EXPECT_EQ(restored.statusText(), restored_status_before);

  QDomElement generic = saved.cloneNode(/*deep=*/true).toElement();
  generic.removeAttribute(u"source_dataset_id"_s);
  generic.removeAttribute(u"source_dataset_source"_s);
  pj::scene3d::RobotModelLayer generic_restored(decoy_topic, u"/robot_description"_s);
  ASSERT_TRUE(generic_restored.attach(makeContext(load_session)));
  ASSERT_NE(generic_restored.robotModel(), nullptr);
  const QString generic_state_before = serializedLayerState(generic_restored);
  const QString generic_status_before = generic_restored.statusText();
  const std::string generic_root_before = generic_restored.robotModel()->root_link;
  ASSERT_FALSE(generic_restored.xmlLoadState(generic));
  ASSERT_NE(generic_restored.robotModel(), nullptr);
  EXPECT_EQ(generic_restored.robotModel()->root_link, generic_root_before);
  EXPECT_EQ(serializedLayerState(generic_restored), generic_state_before);
  EXPECT_EQ(generic_restored.statusText(), generic_status_before);
}

// Seed "scene -> child" (identity) into a buffer.
void seedSceneChild(pj::tf::TransformBuffer& buf, const std::string& child) {
  pj::tf::StampedTransform tf;
  tf.stamp = PJ::fromRaw(1000);
  tf.parent_frame = "scene";
  tf.child_frame = child;
  tf.transform = pj::tf::Transform{glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
  ASSERT_TRUE(buf.setTransform(tf).has_value());
}

// The DROID gripper bug: the layer must bridge the URDF's fixed joints into the
// buffer it actually RENDERS against (the dock's live buffer), not the ctx_
// snapshot captured at attach (which can be null/stale). Bridge a live buffer
// distinct from the attach-time context to prove the render buffer is the one
// bridged.
TEST(RobotModelLayerTest, FixedJointBridgeReconnectsChildFrameInRenderBuffer) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, bridgeParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);  // attach-time context (its buffer is NOT the one we render against)
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.robotModel()->joints.size(), 1u);

  // The live render buffer the dock renders against — a distinct object that
  // publishes base_link under scene but never the fixed-joint child gripper_base.
  pj::tf::TransformBuffer live;
  seedSceneChild(live, "base_link");
  EXPECT_FALSE(live.tryLookupTransform("scene", "gripper_base", PJ::fromRaw(2000), pj::tf::Clock::kHeader).has_value());

  layer.ensureStaticBridgesForTest(live);

  const auto tf = live.tryLookupTransform("scene", "gripper_base", PJ::fromRaw(2000), pj::tf::Clock::kHeader);
  ASSERT_TRUE(tf.has_value()) << "fixed-joint mount must be bridged into the LIVE render buffer";
  EXPECT_NEAR(tf->t.z, 0.1, 1e-9);
}

// The bridge frames must carry the layer's frame prefix, or they would not match
// the prefixed names the link poses are looked up under.
TEST(RobotModelLayerTest, FixedJointBridgeAppliesFramePrefix) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, bridgeParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  layer.setFramePrefix(u"robot1/"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::tf::TransformBuffer live;
  seedSceneChild(live, "robot1/base_link");

  layer.ensureStaticBridgesForTest(live);

  const auto tf = live.tryLookupTransform("scene", "robot1/gripper_base", PJ::fromRaw(2000), pj::tf::Clock::kHeader);
  ASSERT_TRUE(tf.has_value()) << "prefixed bridge frame should resolve in the render buffer";
  EXPECT_NEAR(tf->t.z, 0.1, 1e-9);
}

// kAuto in a MIXED model: a collision-only sublink (the self-collision capsules)
// must land in the COLLISION draw group so the Collision toggle/opacity hides it —
// NOT be promoted into the visuals group where only the Meshes toggle reaches it.
TEST(RobotModelLayerTest, AutoModeCollisionOnlyLinkRendersInCollisionGroupWhenModelHasVisuals) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, mixedCollisionParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);  // kAuto default
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::tf::TransformBuffer live;
  seedSceneChild(live, "body");  // the fixed joint body->body_sc is bridged

  layer.ensureStaticBridgesForTest(live);

  EXPECT_EQ(layer.visualDrawCountForTest(), 1) << "the body's <visual> box";
  EXPECT_EQ(layer.collisionDrawCountForTest(), 1) << "body_sc collision sphere must be in the collision group";

  // Collision geometry with no <material> gets an orange tint (RViz convention).
  const glm::vec4 color = layer.firstCollisionDrawColorForTest();
  EXPECT_NEAR(color.r, 1.0f, 1e-3);
  EXPECT_NEAR(color.g, 0.5f, 1e-3);
  EXPECT_NEAR(color.b, 0.1f, 1e-3);
}

// kAuto in an ENTIRELY-collision model: promote to the visuals group so it renders
// solid instead of ghosting at the collision opacity.
TEST(RobotModelLayerTest, AutoModeAllCollisionModelPromotesToVisuals) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, allCollisionParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);  // kAuto default
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::tf::TransformBuffer live;
  seedSceneChild(live, "solid");

  layer.ensureStaticBridgesForTest(live);

  EXPECT_EQ(layer.visualDrawCountForTest(), 1) << "collision-only model promoted to visuals";
  EXPECT_EQ(layer.collisionDrawCountForTest(), 0);
}

// Coordinate contract: the layer's model is link-local (prefixed link frame +
// <origin> × scale) and TF is applied exactly once: the view receives the
// untransformed ModelDrawCall and applies TF itself.
TEST(RobotModelLayerTest, DrawsKeepLinkLocalGeometryAndApplyTfOnce) {
  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{}, u"robot"_s);
  layer.setFramePrefix(u"robot1/"_s);
  layer.setSourceFileContent(u"offset.urdf"_s, QByteArray(R"(
<robot name="offset">
  <link name="base_link">
    <visual>
      <origin xyz="1 0 0"/>
      <geometry><box size="2 3 4"/></geometry>
    </visual>
  </link>
</robot>
)"));
  ASSERT_NE(layer.robotModel(), nullptr) << layer.statusText().toStdString();
  EXPECT_EQ(layer.sourceFrame(), u"robot1/base_link"_s);

  pj::tf::TransformBuffer live;
  pj::tf::StampedTransform tf;
  tf.stamp = PJ::fromRaw(1000);
  tf.parent_frame = "scene";
  tf.child_frame = "robot1/base_link";
  tf.transform = pj::tf::Transform{glm::dvec3(5, 0, 0), glm::dquat(1, 0, 0, 0)};
  ASSERT_TRUE(live.setTransform(tf).has_value());
  layer.ensureStaticBridgesForTest(live);
  EXPECT_EQ(layer.visualDrawCountForTest(), 1);

  const auto& draws = layer.modelDrawCalls();
  ASSERT_EQ(draws.size(), 1u);
  EXPECT_EQ(draws.front().frame_id, "robot1/base_link");
  EXPECT_EQ(draws.front().mesh_key, "robot:primitive:box");
  EXPECT_EQ(draws.front().group, pj::scene3d::ModelDrawGroup::kVisual);
  EXPECT_NEAR(draws.front().model[3].x, 1.0, 1e-12) << "TF must not be baked into the link-local draw";
  EXPECT_NEAR(draws.front().model[0].x, 2.0, 1e-12);
  EXPECT_NEAR(draws.front().model[2].z, 4.0, 1e-12);
}

// A source switch while mesh imports run must not stack imports: superseded
// imports keep their slots until their workers finish, so the imports still
// running never exceed maxConcurrentMeshLoads(); their results are dropped and
// the new source's meshes all load.
TEST(RobotModelLayerTest, SourceSwitchKeepsSupersededImportsWithinTheBound) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::size_t limit = pj::scene3d::maxConcurrentMeshLoads();
  const int mesh_count = static_cast<int>(limit) + 2;  // more than one wave
  const std::vector<QString> paths = pj::scene3d::test::cubeCopies(dir, mesh_count);
  QString links;
  for (int index = 0; index < mesh_count; ++index) {
    links +=
        uR"(<link name="link_%1"><visual><geometry><mesh filename="%2"/></geometry></visual></link>)"_s.arg(index).arg(
            paths[static_cast<std::size_t>(index)]);
  }
  const auto urdf = [&](const QString& name) { return u"<robot name=\"%1\">%2</robot>"_s.arg(name, links).toUtf8(); };

  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{}, u"robot"_s);
  layer.setSourceFileContent(u"first.urdf"_s, urdf(u"first"_s));
  ASSERT_NE(layer.robotModel(), nullptr) << layer.statusText().toStdString();
  ASSERT_EQ(layer.totalMeshCount(), mesh_count);
  ASSERT_EQ(layer.meshImportsOutstandingForTest(), limit);

  // No event processing in between: every first-source import is still in flight.
  layer.setSourceFileContent(u"second.urdf"_s, urdf(u"second"_s));
  ASSERT_NE(layer.robotModel(), nullptr) << layer.statusText().toStdString();
  EXPECT_LE(layer.meshImportsOutstandingForTest(), limit) << "superseded imports lost their concurrency slots";

  QElapsedTimer timer;
  timer.start();
  while ((layer.meshImportsOutstandingForTest() != 0 || layer.loadedMeshCount() != mesh_count) &&
         timer.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_LE(layer.meshImportsOutstandingForTest(), limit);
  }
  EXPECT_EQ(layer.meshImportsOutstandingForTest(), 0U) << "superseded imports never settled";
  EXPECT_EQ(layer.loadedMeshCount(), mesh_count) << "the new source's meshes did not all load";
}

// A topic URDF has no URL of its own, so a package fetch needs an explicit
// root. The root is model-local: the dock-shared resolver stays clean for a
// sibling robot, the topic's direct http refs stay blocked, and the mapping
// persists with the layer so a restored layer needs no layout context.
TEST(RobotModelLayerTest, TopicUrdfUsesExplicitUrlRoots) {
  const pj::scene3d::test::FileHttpServer assets(
      {{u"/assets/pj_url_pkg/meshes/cube.stl"_s, pj::scene3d::test::readFixtureBytes("cube.stl")}});
  PJ::SessionManager session;
  const PJ::DatasetId dataset_id = createDataset(session, "url-roots.mcap");
  const PJ::ObjectTopicId topic_id = registerTopicForDataset(session, dataset_id);
  registerParser(session, topic_id, packageUrlParserVtable());
  pushWireBytes(session, topic_id);
  const auto ctx = makeContext(session);
  pj::scene3d::UrdfPackageResolver shared;
  // A topic robot as a layout saves it: its roots, and the web-hosted layout's network-only policy.
  const auto element = [&assets](bool web_hosted) {
    QDomDocument doc;
    QDomElement robot = doc.createElement(u"robot_model"_s);
    robot.setAttribute(u"source_type"_s, u"topic"_s);
    robot.setAttribute(u"source_value"_s, u"/robot_description"_s);
    robot.setAttribute(u"source_topic_name"_s, u"/robot_description"_s);
    robot.setAttribute(
        u"package_url_roots"_s,
        pj::scene3d::serializePackageUrlRoots({{u"pj_url_pkg"_s, QUrl(assets.url(u"/assets/pj_url_pkg/"_s))}}));
    if (web_hosted) {
      robot.setAttribute(u"remote_resources_only"_s, u"true"_s);
    }
    return robot;
  };

  // From a layout file on the desktop, the network root is skipped and nothing is fetched.
  pj::scene3d::RobotModelLayer desktop(topic_id, u"/robot_description"_s);
  desktop.setPackageResolver(&shared);
  ASSERT_TRUE(desktop.attach(ctx));
  ASSERT_TRUE(desktop.xmlLoadState(element(/*web_hosted=*/false)));
  ASSERT_NE(desktop.robotModel(), nullptr) << desktop.statusText().toStdString();
  EXPECT_EQ(desktop.unresolvedMeshCount(), 2);
  EXPECT_TRUE(assets.paths().isEmpty()) << assets.paths().join(u',').toStdString();

  pj::scene3d::RobotModelLayer mapped(topic_id, u"/robot_description"_s);
  mapped.setPackageResolver(&shared);
  ASSERT_TRUE(mapped.attach(ctx));
  ASSERT_TRUE(mapped.xmlLoadState(element(/*web_hosted=*/true)));
  ASSERT_NE(mapped.robotModel(), nullptr) << mapped.statusText().toStdString();
  EXPECT_EQ(mapped.totalMeshCount(), 2);
  EXPECT_EQ(mapped.unresolvedMeshCount(), 1) << "a package root must not authorize the topic's direct http ref";
  ASSERT_TRUE(
      pj::scene3d::test::pumpUntil([&mapped]() { return mapped.loadedMeshCount() == 1; }, std::chrono::seconds(10)))
      << mapped.statusText().toStdString();
  EXPECT_EQ(assets.paths().join(u',').toStdString(), "/assets/pj_url_pkg/meshes/cube.stl");

  pj::scene3d::RobotModelLayer unmapped(topic_id, u"/robot_description"_s);
  unmapped.setPackageResolver(&shared);
  ASSERT_TRUE(unmapped.attach(ctx));
  ASSERT_NE(unmapped.robotModel(), nullptr);
  EXPECT_EQ(unmapped.unresolvedMeshCount(), 2) << "one robot's mapping leaked into the shared resolver";

  QDomDocument doc;
  const QDomElement saved = mapped.xmlSaveState(doc);
  EXPECT_TRUE(saved.hasAttribute(u"package_url_roots"_s));
  pj::scene3d::RobotModelLayer restored(topic_id, u"/robot_description"_s);
  restored.setPackageResolver(&shared);
  ASSERT_TRUE(restored.attach(ctx));
  ASSERT_TRUE(restored.xmlLoadState(saved));
  ASSERT_NE(restored.robotModel(), nullptr);
  EXPECT_EQ(restored.unresolvedMeshCount(), 1);
}

// A robot restored from a web-hosted layout keeps the network-only policy for its URDF's own mesh
// references (here a file:// mesh that exists locally), and saves it so undo and re-saves keep it.
TEST(RobotModelLayerTest, RemoteResourcesOnlyBlocksLocalMeshesAndPersists) {
  PJ::SessionManager session;
  const PJ::DatasetId dataset_id = createDataset(session, "remote-only.mcap");
  const PJ::ObjectTopicId topic_id = registerTopicForDataset(session, dataset_id);
  registerParser(session, topic_id, resolvedMeshParserVtable());
  pushWireBytes(session, topic_id);
  const auto ctx = makeContext(session);
  const auto restored = [&](const QString& policy) {
    QDomDocument doc;
    QDomElement element = doc.createElement(u"robot_model"_s);
    element.setAttribute(u"source_type"_s, u"topic"_s);
    element.setAttribute(u"source_value"_s, u"/robot_description"_s);
    element.setAttribute(u"source_topic_name"_s, u"/robot_description"_s);
    if (!policy.isEmpty()) {
      element.setAttribute(u"remote_resources_only"_s, policy);
    }
    auto layer = std::make_unique<pj::scene3d::RobotModelLayer>(topic_id, u"/robot_description"_s);
    EXPECT_TRUE(layer->attach(ctx));
    EXPECT_TRUE(layer->xmlLoadState(element)) << policy.toStdString();
    return layer;
  };

  const auto desktop = restored({});
  ASSERT_NE(desktop->robotModel(), nullptr);
  EXPECT_EQ(desktop->unresolvedMeshCount(), 0);
  QDomDocument desktop_doc;
  EXPECT_FALSE(desktop->xmlSaveState(desktop_doc).hasAttribute(u"remote_resources_only"_s));

  const auto web = restored(u"true"_s);
  ASSERT_NE(web->robotModel(), nullptr);
  EXPECT_EQ(web->totalMeshCount(), 1);
  EXPECT_EQ(web->unresolvedMeshCount(), 1) << "a web layout's URDF reached a local mesh";
  QDomDocument web_doc;
  EXPECT_EQ(web->xmlSaveState(web_doc).attribute(u"remote_resources_only"_s), u"true"_s);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  QDomDocument invalid_doc;
  QDomElement invalid = invalid_doc.createElement(u"robot_model"_s);
  invalid.setAttribute(u"remote_resources_only"_s, u"maybe"_s);
  EXPECT_FALSE(layer.xmlLoadState(invalid));
}
