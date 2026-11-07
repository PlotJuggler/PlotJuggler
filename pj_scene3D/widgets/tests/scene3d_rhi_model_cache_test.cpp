// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The QRhi model pass's caches (ModelRhiPass), rendered through a real view on
// every backend:
//
//  - GPU meshes survive a modelRevision() bump when their MeshData pointer is
//    unchanged: a model arriving one mesh at a time builds each mesh once, a
//    pose-only SceneEntities batch builds nothing, a same-key reload rebuilds
//    only that mesh;
//  - decoded textures are shared per view (content key + colour space);
//  - wireframe edge lists are built on first use;
//  - the instances are re-uploaded only when what they encode changes: orbit and
//    zoom skip, a pose change or a TF-only link move (no revision bump) does not;
//  - the caches change no pixel (A/B against setCachesEnabledForTest(false)).
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>

#include <QBuffer>
#include <QFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <array>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../rhi/passes/marker_rhi_pass.h"
#include "../rhi/passes/model_rhi_pass.h"
#include "../rhi/scene_view_widget_rhi_quality_p.h"
#include "fake_model_layer.h"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_tf/tf_buffer.h"
#include "pj_widgets/GraphicsApi.h"
#include "scene3d_render_test_support.h"
#include "scene_view_test_access.h"

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::MeshData;
using pj::scene3d::ModelRhiPass;
using pj::scene3d::SceneViewTestAccess;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::FakeModelLayer;
using pj::scene3d::test::grab;
using pj::scene3d::test::serializeTfEdges;
using BT = PJ::sdk::BuiltinObjectType;

constexpr QSize kViewSize{240, 180};
constexpr char kWorld[] = "world";
constexpr char kLink[] = "link";

// A `size`-metre cube with flat normals and per-face UVs, one submesh of `material`.
MeshData cube(float size, std::shared_ptr<const pj::scene3d::Material> material) {
  MeshData mesh;
  const float h = size * 0.5F;
  for (const glm::vec3 n :
       {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),
        glm::vec3(0, 0, -1)}) {
    const glm::vec3 u = std::abs(n.z) > 0.5F ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
    const glm::vec3 v = glm::cross(n, u);
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const glm::vec2 c : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
      pj::scene3d::Vertex vertex;
      vertex.position = h * (n + c.x * u + c.y * v);
      vertex.normal = n;
      vertex.color = glm::vec4(1.0F);
      vertex.uv = (c + 1.0F) * 0.5F;
      mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  mesh.submeshes.push_back({0, mesh.indices.size(), std::move(material)});
  mesh.ok = true;
  return mesh;
}

std::shared_ptr<const pj::scene3d::Material> solid(glm::vec4 color) {
  auto material = std::make_shared<pj::scene3d::Material>();
  material->base_color_factor = color;
  return material;
}

// A material whose base colour is a 2x2 PNG checker with content key `key`
// (as glTF embedded images are keyed); every call carries its own copy.
std::shared_ptr<pj::scene3d::Material> textured(const std::string& key) {
  QImage image(2, 2, QImage::Format_RGBA8888);
  image.setPixelColor(0, 0, Qt::yellow);
  image.setPixelColor(1, 1, Qt::yellow);
  image.setPixelColor(1, 0, Qt::blue);
  image.setPixelColor(0, 1, Qt::blue);
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  auto material = std::make_shared<pj::scene3d::Material>();
  material->base_color_factor = glm::vec4(1.0F);
  material->base_color.bytes.assign(png.cbegin(), png.cend());
  material->base_color.key = key;
  return material;
}

void fixCamera(SceneViewWidget& view, glm::vec3 focal, float azimuth_deg, float radius) {
  pj::scene3d::CameraState state;
  state.focal = focal;
  state.radius = radius;
  state.azimuth = glm::radians(azimuth_deg);
  state.elevation = glm::radians(25.0F);
  view.camera().adoptState(state);
}

// Filled grid, no axes, and a warm-up frame before a test fixes the camera.
void settle(SceneViewWidget& view) {
  view.setGridStyle(SceneViewWidget::GridStyle::kFilledCells);
  view.setGridExtentMetres(10.0F);
  view.setAxesVisible(false);
  (void)grab(view);
}

// A standalone view in kWorld whose TF buffer puts kLink at x = 0 at t = 100 and
// at x = 1.5 at t = 200 (tracker at 100), with shadows on (the default).
class ModelView {
 public:
  ModelView() : tf_(std::make_shared<pj::tf::TransformBuffer>(pj::tf::TransformBuffer::kKeepAll)) {
    auto* layout = new QVBoxLayout(&window_);
    layout->setContentsMargins(0, 0, 0, 0);
    view_ = new SceneViewWidget(&window_);
    layout->addWidget(view_);
    for (const auto& [t, x] : {std::pair{100, 0.0}, std::pair{200, 1.5}}) {
      const pj::tf::Transform link({x, 0.0, 0.0}, glm::dquat(1.0, 0.0, 0.0, 0.0));
      EXPECT_TRUE(tf_->setTransform({PJ::fromRaw(t), kWorld, kLink, link}).has_value());
    }
    view_->setTransformBuffer(tf_);
    view_->setFixedFrame(kWorld);
    view_->setTrackerTime(PJ::fromRaw(100));
  }

  // False when the window never got exposed.
  bool show(const std::vector<pj::scene3d::Scene3DLayer*>& layers) {
    view_->setLayers(layers);
    window_.resize(kViewSize);
    window_.show();
    if (!QTest::qWaitForWindowExposed(&window_)) {
      return false;
    }
    settle(*view_);
    setCamera(-90.0F, 8.0F);
    return true;
  }
  void setCamera(float azimuth_deg, float radius) {
    fixCamera(*view_, {0.5F, 0.0F, 0.5F}, azimuth_deg, radius);
  }
  void setShadows(bool on) {
    auto params = view_->meshShadingParams();
    params.shadows_enabled = on;
    view_->setMeshShadingParams(params);
  }

  SceneViewWidget& view() {
    return *view_;
  }
  [[nodiscard]] const ModelRhiPass::CacheCounters& counters() const {
    return SceneViewTestAccess::modelPass(*view_).cacheCountersForTest();
  }

 private:
  QWidget window_;
  SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::tf::TransformBuffer> tf_;
};

// Shows the fixture's dock in base_link with a fixed camera; false when it never got exposed.
bool showDock(pj::scene3d::test::Scene3DRenderFixture& fixture) {
  fixture.window().resize(kViewSize);
  fixture.window().show();
  if (!QTest::qWaitForWindowExposed(&fixture.window()) || fixture.dock()->sceneView() == nullptr) {
    return false;
  }
  fixture.dock()->setFixedFrame(u"base_link"_s);
  settle(*fixture.dock()->sceneView());
  fixCamera(*fixture.dock()->sceneView(), {0.0F, 0.0F, 0.5F}, -90.0F, 8.0F);
  return true;
}

class Scene3dRhiModelCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

#define SKIP_UNLESS(shown)                                                                               \
  if (!(shown)) {                                                                                        \
    PJ_SKIP_WITHOUT_GPU(                                                                                 \
        "the view never got exposed on " << pjRhiBackendName(PJ::preferredGraphicsApi()).toStdString()); \
  }

#define GRAB_OR_SKIP(image, view)                              \
  const QImage image = grab(view);                             \
  if (image.isNull()) {                                        \
    PJ_SKIP_WITHOUT_GPU("no presented frame on this backend"); \
  }

// 50 meshes arriving one per frame, each arrival a revision bump, build 50 GPU
// meshes, not 1 + 2 + ... + 50.
TEST_F(Scene3dRhiModelCacheTest, MeshesArrivingOneByOneBuildOnce) {
  FakeModelLayer layer{kLink};
  ModelView scene;
  SKIP_UNLESS(scene.show({&layer}));
  const int before = scene.counters().mesh_builds;
  for (int i = 0; i < 50; ++i) {
    layer.put(
        "mesh_" + std::to_string(i), cube(0.2F, solid({0.8F, 0.3F, 0.2F, 1.0F})),
        {0.3 * (i % 10) - 1.5, 0.3 * (i / 10), 0.2});
    GRAB_OR_SKIP(image, scene.view());
  }
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).lastDrawCount(), 50);
  EXPECT_EQ(scene.counters().mesh_builds - before, 50);
  ++layer.revision;  // nothing changed: nothing rebuilt
  GRAB_OR_SKIP(same, scene.view());
  EXPECT_EQ(scene.counters().mesh_builds - before, 50);
}

// A same-key reload with a new MeshData rebuilds that mesh only, and draws it;
// a removed key drops its draw.
TEST_F(Scene3dRhiModelCacheTest, SameKeyReloadRebuildsOnlyThatMesh) {
  FakeModelLayer layer{kLink};
  layer.put("left", cube(0.8F, solid({0.9F, 0.2F, 0.2F, 1.0F})), {-1.2, 0.0, 0.5});
  layer.put("right", cube(0.8F, solid({0.2F, 0.9F, 0.2F, 1.0F})), {1.2, 0.0, 0.5});
  ModelView scene;
  SKIP_UNLESS(scene.show({&layer}));
  GRAB_OR_SKIP(first, scene.view());
  const int builds = scene.counters().mesh_builds;
  layer.put("right", cube(0.8F, solid({0.2F, 0.2F, 0.9F, 1.0F})));
  GRAB_OR_SKIP(reloaded, scene.view());
  EXPECT_EQ(scene.counters().mesh_builds, builds + 1);
  EXPECT_NE(first, reloaded) << "the reloaded mesh kept its old colour";
  layer.erase("left");
  GRAB_OR_SKIP(erased, scene.view());
  EXPECT_EQ(scene.counters().mesh_builds, builds + 1);
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).lastDrawCount(), 1);
  EXPECT_NE(reloaded, erased);
}

// One decode and one GPU texture per content key and colour space, shared by
// meshes and layers of the view, and freed with the last mesh using it.
TEST_F(Scene3dRhiModelCacheTest, TexturesAreSharedAcrossMeshesAndLayers) {
  FakeModelLayer first{kLink};
  FakeModelLayer second{kLink};
  first.put("a", cube(0.6F, textured("checker")), {-1.0, 0.0, 0.5});
  first.put("b", cube(0.6F, textured("checker")), {0.0, 0.0, 0.5});
  second.put("c", cube(0.6F, textured("checker")), {1.0, 0.0, 0.5});
  auto linear = textured("checker");
  linear->occlusion = linear->base_color;  // the same image sampled linear: a second texture
  second.put("d", cube(0.6F, linear), {2.0, 0.0, 0.5});
  ModelView scene;
  SKIP_UNLESS(scene.show({&first, &second}));
  GRAB_OR_SKIP(image, scene.view());
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).lastTextureSlotCounts()[0], 4);
  EXPECT_EQ(scene.counters().texture_decodes, 2);
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).cachedTextureCountForTest(), 2);
  // Replace every textured mesh (a layer without draws keeps its GPU state).
  first.erase("a");
  first.erase("b");
  first.put("e", cube(0.6F, solid({0.5F, 0.5F, 0.5F, 1.0F})));
  second.erase("c");
  second.erase("d");
  second.put("f", cube(0.6F, solid({0.5F, 0.5F, 0.5F, 1.0F})), {1.0, 0.0, 0.5});
  GRAB_OR_SKIP(untextured, scene.view());
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).cachedTextureCountForTest(), 0);
}

// Edge lists are built on the first wireframe draw, once per mesh; toggling back
// and forth reproduces both looks exactly.
TEST_F(Scene3dRhiModelCacheTest, WireframeEdgesBuildOnFirstUse) {
  FakeModelLayer layer{kLink};
  layer.put("box", cube(1.0F, solid({0.9F, 0.6F, 0.1F, 1.0F})), {0.0, 0.0, 0.8});
  ModelView scene;
  SKIP_UNLESS(scene.show({&layer}));
  GRAB_OR_SKIP(solid_image, scene.view());
  EXPECT_EQ(scene.counters().edge_builds, 0);
  layer.wireframe_on = true;
  GRAB_OR_SKIP(wire_image, scene.view());
  EXPECT_EQ(scene.counters().edge_builds, 1);
  EXPECT_NE(solid_image, wire_image);
  layer.wireframe_on = false;
  GRAB_OR_SKIP(solid_again, scene.view());
  EXPECT_EQ(solid_image, solid_again);
  layer.wireframe_on = true;
  GRAB_OR_SKIP(wire_again, scene.view());
  EXPECT_EQ(wire_image, wire_again);
  EXPECT_EQ(scene.counters().edge_builds, 1);
}

// Orbit and zoom keep the render origin and every placement: no instance upload.
// A pose change is one.
TEST_F(Scene3dRhiModelCacheTest, OrbitAndZoomSkipInstanceUploads) {
  FakeModelLayer layer{kLink};
  layer.put("box", cube(0.8F, solid({0.9F, 0.6F, 0.1F, 1.0F})), {0.0, 0.0, 1.0});
  ModelView scene;
  SKIP_UNLESS(scene.show({&layer}));
  GRAB_OR_SKIP(settled, scene.view());
  ASSERT_TRUE(SceneViewTestAccess::stage(scene.view(), SceneViewTestAccess::Stage::kShadow).last_active)
      << SceneViewTestAccess::stage(scene.view(), SceneViewTestAccess::Stage::kShadow).capability_error.toStdString();
  const ModelRhiPass::CacheCounters before = scene.counters();
  scene.setCamera(-60.0F, 6.0F);
  GRAB_OR_SKIP(orbited, scene.view());
  EXPECT_NE(settled, orbited);
  EXPECT_EQ(scene.counters().instance_uploads, before.instance_uploads);
  EXPECT_EQ(scene.counters().shadow_instance_uploads, before.shadow_instance_uploads);
  layer.put("box2", cube(0.3F, solid({0.2F, 0.2F, 0.9F, 1.0F})), {1.0, 0.0, 0.3});
  GRAB_OR_SKIP(posed, scene.view());
  EXPECT_EQ(scene.counters().instance_uploads, before.instance_uploads + 1);
  EXPECT_EQ(scene.counters().shadow_instance_uploads, before.shadow_instance_uploads + 1);
}

// Shadows off and on again rebind the kept meshes' materials to the new shadow
// map: no mesh rebuilt, and the shadowed frame comes back exactly.
TEST_F(Scene3dRhiModelCacheTest, ShadowToggleRebindsKeptMeshes) {
  FakeModelLayer layer{kLink};
  layer.put("box", cube(0.8F, textured("checker")), {0.0, 0.0, 1.2});
  ModelView scene;
  SKIP_UNLESS(scene.show({&layer}));
  GRAB_OR_SKIP(shadowed, scene.view());
  ASSERT_TRUE(SceneViewTestAccess::stage(scene.view(), SceneViewTestAccess::Stage::kShadow).last_active)
      << SceneViewTestAccess::stage(scene.view(), SceneViewTestAccess::Stage::kShadow).capability_error.toStdString();
  const int builds = scene.counters().mesh_builds;
  scene.setShadows(false);
  GRAB_OR_SKIP(unshadowed, scene.view());
  EXPECT_NE(shadowed, unshadowed);
  scene.setShadows(true);
  GRAB_OR_SKIP(shadowed_again, scene.view());
  EXPECT_TRUE(SceneViewTestAccess::stage(scene.view(), SceneViewTestAccess::Stage::kShadow).last_active);
  EXPECT_EQ(shadowed, shadowed_again);
  layer.put("small", cube(0.3F, solid({0.2F, 0.2F, 0.9F, 1.0F})), {1.2, 0.0, 0.3});  // a bump that keeps "box"
  GRAB_OR_SKIP(grown, scene.view());
  EXPECT_EQ(scene.counters().mesh_builds, builds + 1);
  EXPECT_EQ(SceneViewTestAccess::modelPass(scene.view()).lastDrawCount(), 2);
}

constexpr auto kMoverUrdf = R"(<robot name="mover">
  <link name="base_link"/>
  <link name="arm"><visual><origin xyz="0 0 1.2"/><geometry><box size="0.8 0.8 0.8"/></geometry></visual></link>
  <joint name="swing" type="continuous"><parent link="base_link"/><child link="arm"/><axis xyz="0 0 1"/></joint>
</robot>)";

// A RobotModelLayer link moved only by TF keeps modelRevision() and every GPU
// mesh, but its colour and shadow instances are re-uploaded: its pixels and the
// shadow caster bounds follow the link.
TEST_F(Scene3dRhiModelCacheTest, TfOnlyLinkMoveRefreshesInstances) {
  pj::scene3d::test::Scene3DRenderFixture fixture;
  fixture.dock()->setTransformService(&fixture.transforms());
  const auto tf =
      fixture.add("/tf", BT::kFrameTransforms, serializeTfEdges("base_link", {{"arm", {-1.0, 0.0, 0.0}}}, 100));
  fixture.push(tf, 200, serializeTfEdges("base_link", {{"arm", {1.0, 0.0, 0.0}}}, 200));
  fixture.transforms().ingestFrameTransformsForDataset(fixture.dataset());
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile urdf(dir.filePath(u"mover.urdf"_s));
  ASSERT_TRUE(urdf.open(QIODevice::WriteOnly));
  urdf.write(kMoverUrdf);
  urdf.close();
  const PJ::ObjectTopicId robot_id = fixture.dock()->addRobotModelLayer(urdf.fileName());
  ASSERT_NE(robot_id.id, 0U);
  const auto* robot = dynamic_cast<const pj::scene3d::ModelRenderable*>(fixture.dock()->layerFor(robot_id));
  ASSERT_NE(robot, nullptr);
  SKIP_UNLESS(showDock(fixture));
  SceneViewWidget& view = *fixture.dock()->sceneView();
  fixture.dock()->onTrackerTime(100e-9);
  GRAB_OR_SKIP(at_100, view);
  ASSERT_EQ(SceneViewTestAccess::modelPass(view).lastDrawCount(), 1);
  ASSERT_TRUE(SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kShadow).last_active)
      << SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kShadow).capability_error.toStdString();
  const std::uint64_t revision = robot->modelRevision();
  const ModelRhiPass::CacheCounters before = SceneViewTestAccess::modelPass(view).cacheCountersForTest();
  const pj::scene3d::AABB bounds_100 = SceneViewTestAccess::lastShadowBounds(view);

  fixture.dock()->onTrackerTime(200e-9);
  GRAB_OR_SKIP(at_200, view);
  const ModelRhiPass::CacheCounters& after = SceneViewTestAccess::modelPass(view).cacheCountersForTest();
  EXPECT_EQ(robot->modelRevision(), revision) << "the premise: a TF-only move bumps no revision";
  EXPECT_EQ(after.mesh_builds, before.mesh_builds);
  EXPECT_EQ(after.instance_uploads, before.instance_uploads + 1);
  EXPECT_EQ(after.shadow_instance_uploads, before.shadow_instance_uploads + 1);
  EXPECT_GT(pj::scene3d::test::differingPixels(at_100, at_200, 60), 200) << "the moved link kept its old pixels";
  const pj::scene3d::AABB bounds_200 = SceneViewTestAccess::lastShadowBounds(view);
  ASSERT_TRUE(bounds_100.valid && bounds_200.valid);
  EXPECT_GT(bounds_200.max.x, bounds_100.max.x + 1.0F) << "the shadow caster bounds stayed with the old pose";
}

// A pose-only batch of a textured GLB SceneEntities model re-places the model
// without rebuilding its mesh or re-decoding its texture.
TEST_F(Scene3dRhiModelCacheTest, PoseOnlyGlbBatchBuildsNothing) {
  QFile glb(QStringLiteral(PJ_SCENE3D_FIXTURES_DIR "/meshes/embedded_base.glb"));
  ASSERT_TRUE(glb.open(QIODevice::ReadOnly)) << glb.fileName().toStdString();
  const QByteArray bytes = glb.readAll();
  const auto batch = [&](double x) {
    PJ::sdk::ModelPrimitive model;
    model.pose.position = {x, 0.0, 0.8};
    model.pose.orientation.w = 1.0;
    model.scale = {1.0, 1.0, 1.0};
    model.media_type = "model/gltf-binary";
    model.data.assign(bytes.cbegin(), bytes.cend());
    PJ::sdk::SceneEntity entity;
    entity.id = "model";
    entity.frame_id = "base_link";
    entity.models.push_back(std::move(model));
    PJ::sdk::SceneEntities scene;
    scene.entities.push_back(std::move(entity));
    return PJ::serializeSceneEntities(scene);
  };
  pj::scene3d::test::Scene3DRenderFixture fixture;
  const auto topic = fixture.add("/entities", BT::kSceneEntities, batch(-1.0));
  fixture.push(topic, 200, batch(1.0));
  SKIP_UNLESS(showDock(fixture));
  SceneViewWidget& view = *fixture.dock()->sceneView();
  fixture.dock()->onTrackerTime(100e-9);
  for (int attempt = 0; attempt < 200 && SceneViewTestAccess::modelPass(view).lastTextureSlotCounts()[0] == 0;
       ++attempt) {
    QTest::qWait(20);  // the model imports off the GUI thread
    GRAB_OR_SKIP(loading, view);
  }
  ASSERT_GT(SceneViewTestAccess::modelPass(view).lastTextureSlotCounts()[0], 0) << "the textured GLB never loaded";
  GRAB_OR_SKIP(at_100, view);
  const ModelRhiPass::CacheCounters before = SceneViewTestAccess::modelPass(view).cacheCountersForTest();
  fixture.dock()->onTrackerTime(200e-9);
  GRAB_OR_SKIP(at_200, view);
  const ModelRhiPass::CacheCounters& after = SceneViewTestAccess::modelPass(view).cacheCountersForTest();
  EXPECT_EQ(after.mesh_builds, before.mesh_builds);
  EXPECT_EQ(after.texture_decodes, before.texture_decodes);
  EXPECT_GT(after.instance_uploads, before.instance_uploads);
  EXPECT_NE(at_100, at_200);
}

// One SceneEntities layer registers two families (markers and models): both
// draw under its one logical slot, beside another family's layer, and its
// removal takes both away while the other layer keeps drawing.
TEST_F(Scene3dRhiModelCacheTest, SceneEntitiesContributeMarkersAndModelsInOneSlot) {
  QFile glb(QStringLiteral(PJ_SCENE3D_FIXTURES_DIR "/meshes/embedded_base.glb"));
  ASSERT_TRUE(glb.open(QIODevice::ReadOnly)) << glb.fileName().toStdString();
  const QByteArray bytes = glb.readAll();
  PJ::sdk::ModelPrimitive model;
  model.pose.position = {1.0, 0.0, 0.8};
  model.pose.orientation.w = 1.0;
  model.scale = {1.0, 1.0, 1.0};
  model.media_type = "model/gltf-binary";
  model.data.assign(bytes.cbegin(), bytes.cend());
  PJ::sdk::CubePrimitive cube;
  cube.pose.position = {-1.0, 0.0, 0.5};
  cube.pose.orientation.w = 1.0;
  cube.size = {0.5, 0.5, 0.5};
  cube.color = {200, 60, 60, 255};
  PJ::sdk::SceneEntity entity;
  entity.id = "both";
  entity.frame_id = "base_link";
  entity.models.push_back(std::move(model));
  entity.cubes.push_back(cube);
  PJ::sdk::SceneEntities scene;
  scene.entities.push_back(std::move(entity));

  pj::scene3d::test::Scene3DRenderFixture fixture;
  const auto points = fixture.add(
      "/points", BT::kPointCloud,
      pj::scene3d::test::serializeCloud({{0.0F, -1.0F, 0.2F}, {0.0F, 1.0F, 0.2F}}, "base_link"));
  const auto entities = fixture.add("/entities", BT::kSceneEntities, PJ::serializeSceneEntities(scene));
  SKIP_UNLESS(showDock(fixture));
  SceneViewWidget& view = *fixture.dock()->sceneView();
  fixture.dock()->onTrackerTime(100e-9);
  for (int attempt = 0; attempt < 200 && SceneViewTestAccess::modelPass(view).lastDrawCount() == 0; ++attempt) {
    QTest::qWait(20);  // the model imports off the GUI thread
    GRAB_OR_SKIP(loading, view);
  }
  GRAB_OR_SKIP(both, view);
  EXPECT_EQ(SceneViewTestAccess::modelPass(view).lastDrawCount(), 1);
  EXPECT_EQ(SceneViewTestAccess::markerPass(view).lastInstanceCount(), 2) << "the cube's fill and edges";
  EXPECT_EQ(SceneViewTestAccess::lastSubmittedLayerIds(view), (std::vector<std::uint32_t>{points.id, entities.id}));

  fixture.dock()->removeTopic(entities);
  GRAB_OR_SKIP(points_only, view);
  EXPECT_EQ(SceneViewTestAccess::modelPass(view).lastDrawCount(), 0);
  EXPECT_EQ(SceneViewTestAccess::markerPass(view).lastInstanceCount(), 0);
  EXPECT_EQ(SceneViewTestAccess::lastSubmittedLayerIds(view), (std::vector<std::uint32_t>{points.id}));
}

// The frames of a scene with meshes, a texture, translucency, TF motion,
// wireframe and shadow changes, with or without the caches.
std::vector<QImage> renderSteps(bool caches) {
  std::vector<QImage> images;
  FakeModelLayer plain{kLink};
  FakeModelLayer textured_layer{kLink};
  plain.put("a", cube(0.7F, solid({0.9F, 0.3F, 0.2F, 1.0F})), {-1.0, 0.0, 0.8});
  plain.put("b", cube(0.5F, solid({0.2F, 0.8F, 0.3F, 0.6F})), {0.0, 0.5, 0.4});
  textured_layer.put("t", cube(0.6F, textured("checker")), {1.0, -0.3, 0.6});
  ModelView scene;
  SceneViewTestAccess::modelPass(scene.view()).setCachesEnabledForTest(caches);
  if (!scene.show({&plain, &textured_layer})) {
    return images;
  }
  const std::vector<std::function<void()>> steps = {
      [] {},
      [&] { scene.setCamera(-70.0F, 8.0F); },
      [&] { scene.view().setTrackerTime(PJ::fromRaw(200)); },
      [&] { plain.put("c", cube(0.4F, solid({0.3F, 0.3F, 0.9F, 1.0F})), {0.5, -1.0, 0.3}); },
      [&] { textured_layer.wireframe_on = true; },
      [&] { plain.put("a", cube(0.9F, solid({0.9F, 0.9F, 0.2F, 1.0F}))); },
      [&] { scene.setShadows(false); },
      [&] {
        scene.setShadows(true);
        scene.setCamera(-70.0F, 6.0F);
      },
      [] {},
  };
  for (const auto& step : steps) {
    step();
    images.push_back(grab(scene.view()));
  }
  return images;
}

TEST_F(Scene3dRhiModelCacheTest, CachedFramesMatchUncachedPixelForPixel) {
  const std::vector<QImage> cached = renderSteps(true);
  const std::vector<QImage> uncached = renderSteps(false);
  if (cached.empty() || uncached.empty() || cached.front().isNull()) {
    PJ_SKIP_WITHOUT_GPU("no presented frame on this backend");
  }
  ASSERT_EQ(cached.size(), uncached.size());
  // Apple's software OpenGL (the macOS fallback) differs on 10 pixels once
  // shadows go off (step 6); Mesa and Metal match exactly (TECHNICAL_NOTES,
  // "Known gaps").
  const bool apple_opengl =
      QSysInfo::productType() == QLatin1String("macos") && PJ::preferredGraphicsApi() == QRhiWidget::Api::OpenGL;
  const int allowed = apple_opengl ? 16 : 0;
  for (std::size_t i = 0; i < cached.size(); ++i) {
    ASSERT_FALSE(cached[i].isNull() || uncached[i].isNull()) << "step " << i;
    if (allowed == 0) {
      EXPECT_EQ(cached[i], uncached[i]) << "step " << i << " differs with the caches on ("
                                        << pj::scene3d::test::differingPixels(cached[i], uncached[i], 60) << " pixels)";
    } else {
      EXPECT_LE(pj::scene3d::test::differingPixels(cached[i], uncached[i], 60), allowed)
          << "step " << i << " differs with the caches on";
    }
  }
  EXPECT_GT(pj::scene3d::test::differingPixels(cached[1], cached[2], 60), 200) << "the TF step moved nothing";
}

}  // namespace
