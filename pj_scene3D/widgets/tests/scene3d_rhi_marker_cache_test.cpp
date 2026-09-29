// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The scene prelude and marker caches of the QRhi view, rendered through the real
// Scene3DDockWidget on every backend:
//
//  - MarkerRhiPass keeps each layer's expansion in Static buffers keyed on its
//    geometry revision, style, the render origin and the resolved
//    TF placement of every frame it references: orbit and zoom rebuild nothing,
//    a pan rebuilds once, a live layer rebuilds once per new sample, a TF-only move
//    of a static layer's frame rebuilds it and moves its pixels, and the cached
//    frames match the uncached path (setMarkerCacheEnabledForTest(false)) pixel
//    for pixel; a partial buffer-allocation failure drops the layer for one frame;
//  - a layer re-uploaded within a few frames keeps its expansion, and an
//    origin-only change (pan, follow) then rewrites only its translations and
//    stream positions, pixel-identical to the uncached path near the origin and
//    1e6 m away, for small and large pans and across a TF move; any other layer
//    drops its expansion after the upload;
//  - GridRhiPass builds the grid once: panning and playback never rebuild it;
//    the TF connection lines follow the playhead and the render origin;
//  - VoxelRhiPass rejects a grid wholly outside the frustum and keeps one that
//    only overhangs it.
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>

#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../rhi/passes/grid_rhi_pass.h"
#include "../rhi/passes/marker_rhi_pass.h"
#include "../rhi/passes/model_rhi_pass.h"
#include "../rhi/passes/voxel_rhi_pass.h"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/builtin/voxel_grid_codec.hpp"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "scene3d_render_test_support.h"
#include "scene_view_test_access.h"

using namespace Qt::StringLiterals;

namespace {

using BT = PJ::sdk::BuiltinObjectType;
using pj::scene3d::SceneViewTestAccess;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::differingPixels;
using pj::scene3d::test::serializeTfEdges;

constexpr char kFrame[] = "map";
constexpr QSize kViewSize{400, 300};
constexpr PJ::Timestamp kT0 = 100;             // every first sample
constexpr PJ::Timestamp kStepNs = 10'000'000;  // later samples, 10 ms apart

double seconds(PJ::Timestamp t) {
  return static_cast<double>(t) / 1.0e9;
}

PJ::sdk::SceneEntity cubeEntity(const std::string& id, const std::string& frame, glm::dvec3 centre, float side) {
  PJ::sdk::SceneEntity entity;
  entity.frame_id = frame;
  entity.id = id;
  PJ::sdk::CubePrimitive cube;
  cube.pose.position = {centre.x, centre.y, centre.z};
  cube.size = {side, side, side};
  cube.color = {220, 90, 40, 255};
  entity.cubes.push_back(cube);
  return entity;
}

// Every marker family: cube (+ edge overlay), sphere, cylinder, arrow, a line
// strip and a triangle list, around `centre`.
std::vector<PJ::sdk::SceneEntity> mixedEntities(const std::string& frame, glm::dvec3 centre) {
  PJ::sdk::SceneEntity entity = cubeEntity("mixed", frame, centre + glm::dvec3(-1.0, 0.0, 0.3), 0.5F);
  PJ::sdk::SpherePrimitive sphere;
  sphere.pose.position = {centre.x, centre.y, centre.z + 0.3};
  sphere.size = {0.5, 0.5, 0.5};
  sphere.color = {60, 160, 220, 255};
  entity.spheres.push_back(sphere);
  PJ::sdk::CylinderPrimitive cylinder;
  cylinder.pose.position = {centre.x + 1.0, centre.y, centre.z + 0.3};
  cylinder.size = {0.4, 0.4, 0.6};
  cylinder.top_scale = 0.3;
  cylinder.color = {90, 200, 90, 255};
  entity.cylinders.push_back(cylinder);
  PJ::sdk::ArrowPrimitive arrow;
  arrow.pose.position = {centre.x - 1.0, centre.y - 1.0, centre.z + 0.8};
  arrow.shaft_length = 0.6;
  arrow.shaft_diameter = 0.08;
  arrow.head_length = 0.2;
  arrow.head_diameter = 0.16;
  arrow.color = {230, 220, 60, 255};
  entity.arrows.push_back(arrow);
  PJ::sdk::LinePrimitive line;
  line.thickness = 0.02;
  line.color = {250, 250, 250, 255};
  for (int i = 0; i <= 16; ++i) {
    const double angle = 0.4 * i;
    line.points.push_back({centre.x + 1.4 * std::cos(angle), centre.y + 1.4 * std::sin(angle), centre.z + 0.05 * i});
  }
  entity.lines.push_back(line);
  PJ::sdk::TrianglePrimitive triangles;
  triangles.color = {200, 60, 200, 200};
  triangles.points = {
      {centre.x, centre.y + 1.0, centre.z + 0.1},
      {centre.x + 0.6, centre.y + 1.0, centre.z + 0.1},
      {centre.x + 0.3, centre.y + 1.5, centre.z + 0.7}};
  entity.triangles.push_back(triangles);
  return {entity};
}

std::vector<uint8_t> serializeEntities(std::vector<PJ::sdk::SceneEntity> entities) {
  PJ::sdk::SceneEntities scene;
  scene.entities = std::move(entities);
  return PJ::serializeSceneEntities(scene);
}

// One dock bound to the fixture's TF service (so markers can sit in a TF child
// frame), with a pinned camera.
class CacheScene {
 public:
  CacheScene() {
    fixture_.dock()->setTransformService(&fixture_.transforms());
  }

  PJ::ObjectTopicId addEntities(const std::string& topic, std::vector<PJ::sdk::SceneEntity> entities) {
    const auto id = fixture_.add(topic, BT::kSceneEntities, serializeEntities(std::move(entities)));
    if (auto* layer = markerLayer(id); layer != nullptr) {
      layer->setOpacity(1.0F);
    }
    return id;
  }
  // `child` under kFrame at each (time, offset); the first time must be kT0.
  void addEdge(const std::string& child, const std::vector<std::pair<PJ::Timestamp, glm::dvec3>>& poses) {
    const auto id = fixture_.add(
        "/tf_" + child, BT::kFrameTransforms, serializeTfEdges(kFrame, {{child, poses.front().second}}, kT0));
    for (std::size_t i = 1; i < poses.size(); ++i) {
      fixture_.push(id, poses[i].first, serializeTfEdges(kFrame, {{child, poses[i].second}}, poses[i].first));
    }
    fixture_.transforms().ingestFrameTransformsForDataset(fixture_.dataset());
  }
  PJ::ObjectTopicId addVoxels(glm::vec3 origin, float cell, int count) {
    std::vector<std::uint8_t> values(static_cast<std::size_t>(count), 200U);
    PJ::sdk::VoxelGrid grid;
    grid.frame_id = kFrame;
    grid.origin.position = {origin.x, origin.y, origin.z};
    grid.origin.orientation.w = 1.0;
    grid.cell_size = {cell, cell, cell};
    grid.column_count = static_cast<std::uint32_t>(count);
    grid.row_count = 1;
    grid.slice_count = 1;
    grid.cell_stride = 1;
    grid.row_stride = grid.column_count;
    grid.slice_stride = grid.column_count;
    PJ::sdk::PointField field;
    field.name = "occupancy";
    field.datatype = PJ::sdk::PointField::Datatype::kUint8;
    field.count = 1;
    grid.fields = {field};
    grid.data = PJ::Span<const std::uint8_t>(values.data(), values.size());
    return fixture_.add("/voxels", BT::kVoxelGrid, PJ::serializeVoxelGrid(grid));
  }
  void push(PJ::ObjectTopicId id, PJ::Timestamp t, std::vector<uint8_t> bytes) {
    fixture_.push(id, t, std::move(bytes));
  }

  pj::scene3d::SceneEntitiesLayer* markerLayer(PJ::ObjectTopicId id) {
    return dynamic_cast<pj::scene3d::SceneEntitiesLayer*>(fixture_.dock()->layerFor(id));
  }
  PJ::Scene3DDockWidget& dock() {
    return *fixture_.dock();
  }
  SceneViewWidget* view() {
    return fixture_.dock()->sceneView();
  }
  pj::scene3d::MarkerRhiPass& markers() {
    return SceneViewTestAccess::markerPass(*view());
  }

  // Shows the window at kT0, presents a warm-up frame, then pins a
  // front-above camera. False when no frame could be presented.
  bool show(bool grid = false) {
    for (int i = 0; i < 20 && view() == nullptr; ++i) {
      QCoreApplication::processEvents();
    }
    if (view() == nullptr) {
      return false;
    }
    fixture_.window().resize(kViewSize);
    fixture_.window().show();
    if (!QTest::qWaitForWindowExposed(&fixture_.window())) {
      return false;
    }
    dock().setFixedFrame(QString::fromLatin1(kFrame));
    view()->setGridVisible(grid);
    view()->setAxesVisible(false);
    dock().onTrackerTime(seconds(kT0));
    if (!settle()) {
      return false;
    }
    camera_.focal = {0.0F, 0.0F, 0.3F};
    camera_.radius = 9.0F;
    camera_.azimuth = glm::radians(-90.0F);
    camera_.elevation = glm::radians(25.0F);
    camera_.fov_y = glm::radians(45.0F);
    view()->camera().adoptState(camera_);
    return !grab().isNull();
  }
  // Grabs until the marker expansion is on screen: async decodes land a frame late.
  bool settle() {
    for (int i = 0; i < 40; ++i) {
      if (grab().isNull()) {
        return false;
      }
      if (SceneViewTestAccess::markerPass(*view()).lastLayerCount() == expected_layers_) {
        break;
      }
      QTest::qWait(5);
    }
    return true;
  }
  void expectLayers(int layers) {
    expected_layers_ = layers;
  }
  QImage grab() {
    return pj::scene3d::test::grab(*view());
  }
  // The pinned camera, changed by `edit` and re-adopted.
  template <typename Edit>
  QImage moveCamera(Edit edit) {
    edit(camera_);
    view()->camera().adoptState(camera_);
    return grab();
  }
  // Moves the playhead and grabs until `layer`'s geometry revision moves (or
  // `expect_new_geometry` is false), then one more frame.
  QImage tick(PJ::Timestamp t, pj::scene3d::SceneEntitiesLayer* layer = nullptr, bool expect_new_geometry = true) {
    const std::uint64_t before = layer != nullptr ? layer->geometryRevision() : 0U;
    dock().onTrackerTime(seconds(t));
    QImage image = grab();
    for (int i = 0; layer != nullptr && expect_new_geometry && layer->geometryRevision() == before && i < 100; ++i) {
      QTest::qWait(5);
      image = grab();
    }
    return grab();
  }

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  pj::scene3d::CameraState camera_;
  int expected_layers_ = 1;
};

class Scene3dRhiMarkerCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

#define SHOW_OR_SKIP(scene, ...)                                     \
  if (!(scene).show(__VA_ARGS__)) {                                  \
    PJ_SKIP_WITHOUT_GPU("the Scene3D view never presented a frame"); \
  }

TEST_F(Scene3dRhiMarkerCacheTest, OrbitAndZoomRebuildNothing) {
  CacheScene scene;
  scene.addEntities("/markers", mixedEntities(kFrame, {0.0F, 0.0F, 0.0F}));
  SHOW_OR_SKIP(scene);
  ASSERT_GT(SceneViewTestAccess::markerPass(*scene.view()).lastInstanceCount(), 0);
  const std::uint64_t rebuilds = scene.markers().layerRebuildCountForTest();
  const QImage start = scene.grab();

  const QImage orbited = scene.moveCamera([](auto& camera) { camera.azimuth += 0.4F; });
  const QImage zoomed = scene.moveCamera([](auto& camera) { camera.radius *= 0.7F; });
  EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds) << "orbit or zoom rebuilt the markers";
  EXPECT_GT(differingPixels(start, orbited), 1000);
  EXPECT_GT(differingPixels(orbited, zoomed), 1000);

  // A pan moves the render origin the vertices are relative to: one rebuild.
  const QImage panned = scene.moveCamera([](auto& camera) { camera.focal += glm::vec3(0.8F, -0.5F, 0.2F); });
  EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds + 1);
  EXPECT_GT(differingPixels(zoomed, panned), 1000);
  scene.grab();
  EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds + 1);
}

TEST_F(Scene3dRhiMarkerCacheTest, OnlyTheLiveLayerRebuildsPerTick) {
  CacheScene scene;
  scene.addEntities("/static", {cubeEntity("s", kFrame, {-1.5F, 0.0F, 0.3F}, 0.6F)});
  const auto live = scene.addEntities("/live", {cubeEntity("l", kFrame, {1.5F, 0.0F, 0.3F}, 0.6F)});
  for (int k = 1; k <= 3; ++k) {
    scene.push(
        live, kT0 + k * kStepNs,
        serializeEntities({cubeEntity("l", kFrame, {1.5F, 0.2F * static_cast<float>(k), 0.3F}, 0.6F)}));
  }
  scene.expectLayers(2);
  SHOW_OR_SKIP(scene);
  ASSERT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 2);
  auto* live_layer = scene.markerLayer(live);
  ASSERT_NE(live_layer, nullptr);
  for (int k = 1; k <= 3; ++k) {
    const std::uint64_t rebuilds = scene.markers().layerRebuildCountForTest();
    scene.tick(kT0 + k * kStepNs, live_layer);
    EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds + 1) << "tick " << k;
  }
}

TEST_F(Scene3dRhiMarkerCacheTest, TfOnlyMoveOfAStaticLayerRebuildsAndMovesIt) {
  CacheScene scene;
  scene.addEdge("child", {{kT0, {0.0, 0.0, 0.0}}, {kT0 + kStepNs, {1.5, 0.0, 0.0}}});
  const auto id = scene.addEntities("/markers", {cubeEntity("c", "child", {-0.75F, 0.0F, 0.4F}, 0.8F)});
  SHOW_OR_SKIP(scene);
  auto* layer = scene.markerLayer(id);
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 1);
  const std::uint64_t revision = layer->geometryRevision();
  const std::uint64_t rebuilds = scene.markers().layerRebuildCountForTest();
  const QImage before = scene.grab();

  const QImage after = scene.tick(kT0 + kStepNs, layer, /*expect_new_geometry=*/false);
  EXPECT_EQ(layer->geometryRevision(), revision) << "the marker batch itself must not change";
  EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds + 1) << "the TF move did not rebuild the layer";
  EXPECT_GT(differingPixels(before, after, 60), 500) << "the cube did not move with its frame";
  // Same placement again: nothing to rebuild.
  scene.tick(kT0 + 2 * kStepNs, layer, false);
  EXPECT_EQ(scene.markers().layerRebuildCountForTest(), rebuilds + 1);
}

TEST_F(Scene3dRhiMarkerCacheTest, StyleAndVisibilityInvalidate) {
  CacheScene scene;
  const auto id = scene.addEntities("/markers", mixedEntities(kFrame, {0.0F, 0.0F, 0.0F}));
  SHOW_OR_SKIP(scene);
  auto* layer = scene.markerLayer(id);
  ASSERT_NE(layer, nullptr);
  auto& markers = scene.markers();
  const auto set_shadows = [&](bool enabled) {
    pj::scene3d::MeshShadingParams params = scene.view()->meshShadingParams();
    params.shadows_enabled = enabled;
    scene.view()->setMeshShadingParams(params);
  };
  const auto rebuilt_by = [&](auto change) {
    const std::uint64_t before = markers.layerRebuildCountForTest();
    change();
    scene.grab();
    return markers.layerRebuildCountForTest() - before;
  };
  EXPECT_EQ(rebuilt_by([] {}), 0U);
  EXPECT_EQ(rebuilt_by([&] { layer->setOpacity(0.5F); }), 1U);
  EXPECT_EQ(rebuilt_by([&] { layer->setOverrideColor(QColor(10, 200, 10)); }), 0U) << "override still disabled";
  EXPECT_EQ(rebuilt_by([&] { layer->setColorOverrideEnabled(true); }), 1U);
  EXPECT_EQ(rebuilt_by([&] { layer->setWireframe(true); }), 1U);
  EXPECT_EQ(rebuilt_by([&] { layer->setWireframe(false); }), 1U);
  // Markers cast no shadows: the shadow toggle is not theirs.
  EXPECT_EQ(rebuilt_by([&] { set_shadows(false); }), 0U);
  EXPECT_EQ(rebuilt_by([&] { set_shadows(true); }), 0U);
  EXPECT_EQ(SceneViewTestAccess::modelPass(*scene.view()).lastShadowDrawCount(), 0);
  // A hidden layer gives its buffers back; showing it again rebuilds.
  EXPECT_EQ(rebuilt_by([&] { layer->setVisible(false); }), 0U);
  EXPECT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 0);
  EXPECT_EQ(
      rebuilt_by([&] {
        layer->setVisible(true);
        scene.dock().onTrackerTime(seconds(kT0 + kStepNs));  // what the dock's un-hide re-delivers
        scene.settle();
      }),
      1U);
}

// The cached frames against the uncached path (every layer rebuilt every frame):
// identical at rest, after an orbit (served from the cache) and after a pan.
TEST_F(Scene3dRhiMarkerCacheTest, CachedFramesMatchTheUncachedPath) {
  CacheScene scene;
  scene.addEdge("child", {{kT0, {0.5, 0.5, 0.0}}});
  scene.addEntities("/markers", mixedEntities(kFrame, {0.0F, 0.0F, 0.0F}));
  scene.addEntities("/child", {cubeEntity("c", "child", {0.0F, -1.5F, 0.6F}, 0.4F)});
  scene.expectLayers(2);
  SHOW_OR_SKIP(scene, /*grid=*/true);
  auto& markers = scene.markers();
  const auto both = [&](const char* what, auto move) {
    markers.setMarkerCacheEnabledForTest(true);
    const QImage cached = scene.moveCamera(move);
    markers.setMarkerCacheEnabledForTest(false);
    const QImage uncached = scene.grab();
    markers.setMarkerCacheEnabledForTest(true);
    EXPECT_EQ(differingPixels(cached, uncached), 0) << what;
  };
  both("at rest", [](auto&) {});
  both("orbited", [](auto& camera) { camera.azimuth += 0.5F; });
  both("panned", [](auto& camera) { camera.focal += glm::vec3(0.37F, -0.21F, 0.13F); });
  // The orbit case really came from the cache.
  scene.grab();
  const std::uint64_t rebuilds = markers.layerRebuildCountForTest();
  scene.moveCamera([](auto& camera) { camera.azimuth += 0.3F; });
  EXPECT_EQ(markers.layerRebuildCountForTest(), rebuilds);
}

// Consecutive pans take the origin-only path. Each such frame (a solid layer and
// a wireframe one), and a TF move
// (a full rebuild) between them, matches the uncached path pixel for pixel, around
// the fixed-frame origin and 1e6 m away from it (where the float render origin
// only moves in 0.125 m steps, so the small pans there are a few ulps).
TEST_F(Scene3dRhiMarkerCacheTest, OriginOnlyUpdatesMatchTheUncachedPath) {
  for (const glm::vec3 base : {glm::vec3(0.0F), glm::vec3(1.5e6F, -2.25e6F, 35.0F)}) {
    SCOPED_TRACE(base.x);
    CacheScene scene;
    const glm::dvec3 child_offset = glm::dvec3(base) + glm::dvec3(0.5, 0.5, 0.0);
    scene.addEdge("child", {{kT0, child_offset}, {kT0 + kStepNs, child_offset + glm::dvec3(0.3, -0.2, 0.05)}});
    scene.addEntities("/markers", mixedEntities(kFrame, base));
    const auto child = scene.addEntities("/child", mixedEntities("child", {0.0F, -1.5F, 0.3F}));
    scene.expectLayers(2);
    SHOW_OR_SKIP(scene, /*grid=*/true);
    // Wireframe: its triangles become stream line edges.
    scene.markerLayer(child)->setWireframe(true);
    auto& markers = scene.markers();
    const glm::vec3 focal = base + glm::vec3(0.0F, 0.0F, 0.3F);
    const float ulp = std::nextafter(focal.x, std::numeric_limits<float>::infinity()) - focal.x;
    const float small_pan = std::max(0.01F, 3.0F * ulp);
    const auto pan = [&](glm::vec3 delta) { return [delta](auto& camera) { camera.focal += delta; }; };
    // Onto the scene, then one more pan: the layers keep re-uploading.
    scene.moveCamera([&](auto& camera) { camera.focal = focal; });
    scene.moveCamera(pan({small_pan, 0.0F, 0.0F}));
    const auto matches = [&](const char* what, auto move, bool origin_only) {
      const std::uint64_t updates = markers.originUpdateCountForTest();
      const QImage cached = scene.moveCamera(move);
      EXPECT_EQ(markers.originUpdateCountForTest() > updates, origin_only) << what;
      markers.setMarkerCacheEnabledForTest(false);
      const QImage uncached = scene.grab();
      markers.setMarkerCacheEnabledForTest(true);
      EXPECT_EQ(differingPixels(cached, uncached), 0) << what;
    };
    matches("small pan", pan({small_pan, -small_pan, small_pan}), true);
    matches("small pan back", pan({-small_pan, small_pan, -small_pan}), true);
    matches("large pan", pan({0.9F, -0.6F, 0.25F}), true);
    matches("TF move", [&](auto&) { scene.tick(kT0 + kStepNs); }, false);
    matches("pan after the TF move", pan({-small_pan, 0.0F, small_pan}), true);
    EXPECT_GT(markers.retainedExpansionCountForTest(), 0);
  }
}

// The same markers 1.5e6 m from the fixed-frame origin (at a fractional offset,
// so float coordinates there are off by up to 6 cm) and near it, each under the
// same camera-relative view: every vertex is placed in double before the float
// narrowing, so the two frames match. The cached/uncached A/B above cannot see a
// precision loss both paths share; this compares against the geometry itself.
TEST_F(Scene3dRhiMarkerCacheTest, FarFromTheOriginMatchesTheSameSceneNearIt) {
  const glm::dvec3 far_centre{1.5e6 + 0.0371, -2.25e6 + 0.0613, 0.0};
  const glm::vec3 far_focal = glm::vec3(far_centre + glm::dvec3(0.37, -0.21, 0.3));
  // A float-valued translation: the near scene's focal stays exactly representable.
  const glm::dvec3 shift{far_focal.x, far_focal.y, 0.0};
  const auto render = [](glm::dvec3 centre, glm::vec3 focal, bool wireframe) {
    CacheScene scene;
    const auto id = scene.addEntities("/markers", mixedEntities(kFrame, centre));
    scene.markerLayer(id)->setWireframe(wireframe);
    if (!scene.show()) {
      return QImage();
    }
    scene.moveCamera([&](auto& camera) { camera.focal = focal; });
    return scene.moveCamera([](auto& camera) { camera.focal.z += 0.01F; });  // a pan: the origin-only path
  };
  for (const bool wireframe : {false, true}) {
    // Not named "far"/"near": those collide with the legacy far/near-pointer
    // macros windows.h (transitively) defines, which MSVC silently expands away.
    const QImage far_image = render(far_centre, far_focal, wireframe);
    if (far_image.isNull()) {
      PJ_SKIP_WITHOUT_GPU("the Scene3D view never presented a frame");
    }
    const QImage near_image = render(far_centre - shift, far_focal - glm::vec3(shift), wireframe);
    // 0 on Mesa (OpenGL, Vulkan); the near/far planes come from the absolute scene
    // bounds, in float at this range, so a few depth-tie pixels may differ
    // elsewhere. Placing the stream vertices in float moves 43+ pixels here.
    EXPECT_LE(differingPixels(far_image, near_image, 60), 8) << (wireframe ? "wireframe" : "solid");
  }
}

// Only a layer that keeps re-uploading keeps its CPU expansion: a one-off
// rebuild gives it back right away, a recurring one a few frames later, or after
// a short spell without frames.
TEST_F(Scene3dRhiMarkerCacheTest, ExpansionIsKeptOnlyWhileReuploading) {
  CacheScene scene;
  scene.addEntities("/markers", mixedEntities(kFrame, {0.0F, 0.0F, 0.0F}));
  SHOW_OR_SKIP(scene);
  auto& markers = scene.markers();
  const auto settle = [&] {
    for (int i = 0; i < 8; ++i) {
      scene.grab();
    }
  };
  settle();
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 0);
  // The frame-window checks must not race the wall-clock release: a slow grab
  // (software GL, a loaded runner) can outlast its 250 ms.
  markers.setIdleReleasePausedForTest(true);
  const auto pan = [](auto& camera) { camera.focal.x += 0.2F; };
  scene.moveCamera(pan);  // one-off
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 0);
  scene.moveCamera(pan);  // recurring
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 1);
  const std::uint64_t updates = markers.originUpdateCountForTest();
  scene.moveCamera(pan);
  EXPECT_EQ(markers.originUpdateCountForTest(), updates + 1);
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 1);
  settle();
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 0);

  // A pan burst, then no frame at all (a settled view renders none): the
  // wall-clock release gives the expansion back.
  scene.moveCamera(pan);
  scene.moveCamera(pan);
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 1);
  QTest::qWait(400);
  EXPECT_EQ(markers.retainedExpansionCountForTest(), 1) << "released while the idle release was paused";
  markers.setIdleReleasePausedForTest(false);
  EXPECT_TRUE(QTest::qWaitFor([&] { return markers.retainedExpansionCountForTest() == 0; }, 5000))
      << "the idle release never gave the expansion back";
}

// A rebuild whose stream buffers cannot be allocated after its instance buffer
// was: the layer draws nothing that frame (none of its buffers stays queued for
// upload; ASan reports the use-after-free otherwise), and the next frame
// retries it and draws it as before.
TEST_F(Scene3dRhiMarkerCacheTest, PartialAllocationFailureDropsTheLayerAndRetries) {
  CacheScene scene;
  scene.addEntities("/markers", mixedEntities(kFrame, {0.0F, 0.0F, 0.0F}));
  SHOW_OR_SKIP(scene);
  auto& markers = scene.markers();
  const QImage drawn = scene.grab();
  ASSERT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 1);

  markers.setMarkerCacheEnabledForTest(false);  // every frame rebuilds
  markers.setFailStreamAllocationForTest(true);
  const QImage failed = scene.grab();
  EXPECT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 0);
  EXPECT_GT(differingPixels(drawn, failed), 1000) << "the failed layer still drew";
  scene.grab();

  markers.setFailStreamAllocationForTest(false);
  markers.setMarkerCacheEnabledForTest(true);
  const QImage retried = scene.grab();
  EXPECT_EQ(SceneViewTestAccess::markerPass(*scene.view()).lastLayerCount(), 1);
  EXPECT_EQ(differingPixels(drawn, retried), 0);
}

TEST_F(Scene3dRhiMarkerCacheTest, GridBuildsOnceUnderPanAndPlayback) {
  CacheScene scene;
  scene.addEdge("child", {{kT0, {1.0, 0.0, 0.0}}, {kT0 + 2 * kStepNs, {1.0, 0.5, 0.0}}});
  scene.addEntities("/markers", {cubeEntity("c", kFrame, {0.0F, 0.0F, 0.3F}, 0.4F)});
  SHOW_OR_SKIP(scene, /*grid=*/true);
  scene.view()->setTfConnectionsVisible(true);
  scene.grab();
  const auto& grid = SceneViewTestAccess::gridPass(*scene.view());
  const std::uint64_t grid_builds = grid.gridBuildCountForTest();
  const std::uint64_t connection_builds = grid.connectionBuildCountForTest();
  ASSERT_GT(
      SceneViewTestAccess::lastTriangleVertexCount(*scene.view()) +
          SceneViewTestAccess::lastLineVertexCount(*scene.view()),
      0);

  const QImage before = scene.grab();
  const QImage panned = scene.moveCamera([](auto& camera) { camera.focal += glm::vec3(0.6F, 0.4F, 0.0F); });
  scene.moveCamera([](auto& camera) { camera.focal += glm::vec3(0.6F, 0.4F, 0.0F); });
  EXPECT_GT(differingPixels(before, panned), 1000);
  EXPECT_EQ(grid.gridBuildCountForTest(), grid_builds) << "panning rebuilt the grid";
  EXPECT_EQ(grid.connectionBuildCountForTest(), connection_builds + 2) << "the connections follow the render origin";

  scene.tick(kT0 + kStepNs);
  scene.tick(kT0 + 2 * kStepNs);
  EXPECT_EQ(grid.gridBuildCountForTest(), grid_builds) << "playback rebuilt the grid";
  EXPECT_EQ(grid.connectionBuildCountForTest(), connection_builds + 4) << "the connections follow the playhead";
}

TEST_F(Scene3dRhiMarkerCacheTest, VoxelGridOutsideTheFrustumIsRejected) {
  CacheScene scene;
  // Five 0.4 m cells on +X from the origin; the camera looks at them from -Y.
  scene.addVoxels({-1.0F, 0.0F, 0.0F}, 0.4F, 5);
  scene.expectLayers(0);
  SHOW_OR_SKIP(scene);
  SceneViewWidget* view = scene.view();
  const QImage empty = scene.moveCamera([](auto& camera) { camera.focal = {0.0F, 0.0F, 60.0F}; });
  EXPECT_EQ(SceneViewTestAccess::voxelPass(*view).lastLayerCount(), 0) << "a grid behind the camera's view was drawn";
  EXPECT_TRUE(SceneViewTestAccess::voxelPass(*view).lastBounds().valid) << "the rejected grid still counts as admitted";

  scene.moveCamera([](auto& camera) {
    camera.focal = {0.0F, 0.0F, 0.2F};
    camera.radius = 3.0F;
  });
  ASSERT_EQ(SceneViewTestAccess::voxelPass(*view).lastLayerCount(), 1);
  const QImage visible = scene.grab();
  EXPECT_GT(differingPixels(empty, visible, 60), 500);
}

}  // namespace
