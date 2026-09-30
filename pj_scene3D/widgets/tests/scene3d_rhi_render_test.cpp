// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Headless render harness for the QRhi Scene3D renderer (PJ_SCENE3D_RHI).
//
// QRhiWidget::grabFramebuffer() renders a frame synchronously on a QPA with
// RHI-based rendering (xcb, wayland); the `offscreen` QPA has none, so this
// runner never defaults to it. CI runs it under Xvfb with Mesa llvmpipe
// (OpenGL) and lavapipe (Vulkan), and on Windows with the `windows` QPA on D3D11
// WARP; PJ_GRAPHICS_API picks the backend and PJ_REQUIRE_GPU_TESTS=1 turns a
// missing one into a failure.
//
// Scenes are grabbed from a shown top-level through grabPresentedFrame(); a
// never-shown widget is good for one grab only (Qt 6.11.1, see that method).
//
// Checks are image DIFFS (same scene with one element toggled) compared against
// the camera's own projection, so they hold regardless of theme colours and
// tonemapping but catch Y/X flips, broken depth conventions and blend ordering.
// PJ_RHI_TEST_DUMP_DIR=<dir> saves every grabbed image as a PNG and appends the
// view's per-frame counters to <dir>/counters.txt (for before/after audits).

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLoggingCategory>
#include <QMenu>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/grid_map_codec.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/builtin/voxel_grid_codec.hpp"
#include "pj_base/time.hpp"
#include "pj_scene3d_widgets/layers/grid_map_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_widgets/GraphicsApi.h"
#include "scene3d_render_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using BT = PJ::sdk::BuiltinObjectType;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::channelDeltaSum;

// Logs the backend + driver per view (runs once the shared main's QApplication exists).
struct GraphicsLog : ::testing::Environment {
  void SetUp() override {
    QLoggingCategory::setFilterRules(u"pj.graphics.info=true"_s);
  }
};
[[maybe_unused]] const auto* const kGraphicsLog = ::testing::AddGlobalTestEnvironment(new GraphicsLog);

constexpr char kFrame[] = "base_link";
constexpr QSize kViewSize{480, 360};

// Backend the run asked for (PJ_GRAPHICS_API, else the platform default), as
// QRhi::backendName() spells it.
QString requestedBackend() {
  return pjRhiBackendName(PJ::preferredGraphicsApi());
}

void dump(const QImage& image, const char* name) {
  const QString dir = qEnvironmentVariable("PJ_RHI_TEST_DUMP_DIR");
  if (dir.isEmpty() || image.isNull()) {
    return;
  }
  QDir().mkpath(dir);
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  image.save(u"%1/%2_%3_%4.png"_s.arg(dir, requestedBackend(), QString::fromLatin1(test->name()), QLatin1String(name)));
}

// One line of the view's last-frame counters, appended under PJ_RHI_TEST_DUMP_DIR.
void dumpCounters(const SceneViewWidget& v, const char* name) {
  const QString dir = qEnvironmentVariable("PJ_RHI_TEST_DUMP_DIR");
  if (dir.isEmpty()) {
    return;
  }
  QDir().mkpath(dir);
  QFile file(dir + u"/counters.txt"_s);
  if (!file.open(QIODevice::Append | QIODevice::Text)) {
    return;
  }
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto texture_slots = v.lastModelTextureSlotCountsForTest();
  QStringList ids;
  for (const std::uint32_t id : v.lastSubmittedLayerIdsForTest()) {
    ids.push_back(QString::number(id));
  }
  const QString line =
      u"%1 %2 %3 lines=%4 tris=%5 frames=%6 pts=%7/%8 cubes=%9/%10 fits=%11 depth=%12/%13 poses=%14/%15 "
      u"occ=%16/%17/%18/%19 vox=%20/%21/%22 markers=%23/%24/%25 models=%26/%27/%28 slots=%29,%30,%31,%32,%33 "
      u"shadow=%34/%35/%36/%37 hdr=%38 ssao=%39 edl=%40 submitted=%41\n"_s
          .arg(requestedBackend(), QString::fromLatin1(test->name()), QLatin1String(name))
          .arg(v.lastLineVertexCountForTest())
          .arg(v.lastTriangleVertexCountForTest())
          .arg(v.lastResolvedFrameCountForTest())
          .arg(v.lastPointLayerCountForTest())
          .arg(v.lastPointVertexCountForTest())
          .arg(v.lastCubeLayerCountForTest())
          .arg(v.lastCubeInstanceCountForTest())
          .arg(v.cameraFitCountForTest())
          .arg(v.lastDepthLayerCountForTest())
          .arg(v.lastDepthVertexCountForTest())
          .arg(v.lastPoseLayerCountForTest())
          .arg(v.lastPoseArmCountForTest())
          .arg(v.lastOccupancyLayerCountForTest())
          .arg(v.lastOccupancyCellCountForTest())
          .arg(v.occupancyFullUploadCountForTest())
          .arg(v.occupancyPartialUploadCountForTest())
          .arg(v.lastVoxelLayerCountForTest())
          .arg(v.lastVoxelCountForTest())
          .arg(v.voxelFullUploadCountForTest())
          .arg(v.lastMarkerLayerCountForTest())
          .arg(v.lastMarkerInstanceCountForTest())
          .arg(v.lastMarkerStreamVertexCountForTest())
          .arg(v.lastModelLayerCountForTest())
          .arg(v.lastModelDrawCountForTest())
          .arg(v.lastModelTriangleCountForTest())
          .arg(texture_slots[0])
          .arg(texture_slots[1])
          .arg(texture_slots[2])
          .arg(texture_slots[3])
          .arg(texture_slots[4])
          .arg(v.lastShadowActiveForTest() ? 1 : 0)
          .arg(v.lastShadowFitValidForTest() ? 1 : 0)
          .arg(v.lastShadowDrawCountForTest())
          .arg(v.lastShadowTriangleCountForTest())
          .arg(v.lastHdrActiveForTest() ? 1 : 0)
          .arg(v.lastSsaoActiveForTest() ? 1 : 0)
          .arg(v.lastEdlActiveForTest() ? 1 : 0)
          .arg(ids.join(u','));
  file.write(line.toUtf8());
}

// test::grab() plus the optional PNG + counter dump. Null when no frame was presented.
QImage grab(SceneViewWidget* view, const char* name) {
  QImage image = pj::scene3d::test::grab(*view);
  dump(image, name);
  dumpCounters(*view, name);
  return image;
}

// Pixels where two frames differ noticeably: count, centroid, and bounding box.
struct DiffMask {
  int count = 0;
  QPointF centroid;
  int count_in(const QRect& rect) const {
    int n = 0;
    for (const QPoint& p : pixels) {
      n += rect.contains(p) ? 1 : 0;
    }
    return n;
  }
  std::vector<QPoint> pixels;
};

DiffMask diffMask(const QImage& a, const QImage& b, int threshold = 60) {
  DiffMask mask;
  if (a.size() != b.size()) {
    return mask;
  }
  double sx = 0.0;
  double sy = 0.0;
  for (int y = 0; y < a.height(); ++y) {
    const auto* row_a = reinterpret_cast<const QRgb*>(a.constScanLine(y));
    const auto* row_b = reinterpret_cast<const QRgb*>(b.constScanLine(y));
    for (int x = 0; x < a.width(); ++x) {
      if (channelDeltaSum(row_a[x], row_b[x]) > threshold) {
        mask.pixels.emplace_back(x, y);
        sx += x;
        sy += y;
      }
    }
  }
  mask.count = static_cast<int>(mask.pixels.size());
  if (mask.count > 0) {
    mask.centroid = {sx / mask.count, sy / mask.count};
  }
  return mask;
}

// Pixels that got darker by more than `threshold` luminance levels from `lit` to `shaded`.
int darkenedPixels(const QImage& shaded, const QImage& lit, int threshold = 12) {
  int count = 0;
  for (int y = 0; y < shaded.height(); ++y) {
    for (int x = 0; x < shaded.width(); ++x) {
      count += qGray(lit.pixel(x, y)) - qGray(shaded.pixel(x, y)) > threshold ? 1 : 0;
    }
  }
  return count;
}

// World point -> image pixel through the view's own camera (GL clip space,
// image Y down): what the renderer must reproduce on every backend.
QPointF project(const SceneViewWidget& view, const glm::vec3& world, const QSize& image) {
  const float aspect = static_cast<float>(image.width()) / static_cast<float>(image.height());
  const glm::vec4 clip = view.camera().projMatrix(aspect) * view.camera().viewMatrix() * glm::vec4(world, 1.0F);
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return {(ndc.x * 0.5 + 0.5) * image.width(), (0.5 - ndc.y * 0.5) * image.height()};
}

// Mean colour of a small square around `centre`.
QColor meanColor(const QImage& image, QPointF centre, int half = 3) {
  int r = 0;
  int g = 0;
  int b = 0;
  int n = 0;
  for (int y = int(centre.y()) - half; y <= int(centre.y()) + half; ++y) {
    for (int x = int(centre.x()) - half; x <= int(centre.x()) + half; ++x) {
      if (image.valid(x, y)) {
        const QRgb px = image.pixel(x, y);
        r += qRed(px);
        g += qGreen(px);
        b += qBlue(px);
        ++n;
      }
    }
  }
  return n == 0 ? QColor() : QColor(r / n, g / n, b / n);
}

// A dense cube of points, `n`^3 of them, centred on `centre` with side `side`.
std::vector<glm::vec3> pointBlock(glm::vec3 centre, float side, int n) {
  std::vector<glm::vec3> points;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      for (int k = 0; k < n; ++k) {
        points.push_back(centre + side * (glm::vec3(i, j, k) / float(n - 1) - 0.5F));
      }
    }
  }
  return points;
}

PJ::sdk::SceneEntity cubeEntity(std::string id, glm::vec3 centre, float side, PJ::sdk::ColorRGBA color) {
  PJ::sdk::SceneEntity entity;
  entity.frame_id = kFrame;
  entity.id = std::move(id);
  PJ::sdk::CubePrimitive cube;
  cube.pose.position = {centre.x, centre.y, centre.z};
  cube.size = {side, side, side};
  cube.color = color;
  entity.cubes.push_back(cube);
  return entity;
}

// Shows `window` and waits until it is exposed: its backing store then owns
// the QRhi every grab renders through.
bool showExposed(QWidget& window) {
  window.show();
  return QTest::qWaitForWindowExposed(&window);
}

// The shared render fixture, all in one frame so no TF is required.
class RhiScene {
 public:
  PJ::ObjectTopicId addPoints(const std::vector<glm::vec3>& points) {
    return fixture_.add("/points", BT::kPointCloud, pj::scene3d::test::serializeCloud(points, kFrame));
  }
  // Layer opacity forced to 1 (the layer default is translucent), so each
  // cube's own alpha decides opaque vs translucent.
  PJ::ObjectTopicId addEntities(std::vector<PJ::sdk::SceneEntity> entities, const std::string& topic = "/entities") {
    PJ::sdk::SceneEntities scene;
    scene.entities = std::move(entities);
    const auto id = fixture_.add(topic, BT::kSceneEntities, PJ::serializeSceneEntities(scene));
    setLayerAttribute(id, u"opacity"_s, u"1"_s);
    return id;
  }
  // One scalar uint8 voxel per value along +X, `cell` metres each, the grid's
  // lower corner at `origin`.
  PJ::ObjectTopicId addVoxelRow(const std::vector<std::uint8_t>& values, glm::vec3 origin, float cell) {
    PJ::sdk::VoxelGrid grid;
    grid.frame_id = kFrame;
    grid.origin.position = {origin.x, origin.y, origin.z};
    grid.origin.orientation.w = 1.0;
    grid.cell_size = {cell, cell, cell};
    grid.column_count = static_cast<std::uint32_t>(values.size());
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
  // One identity-oriented pose at `position`, its triad arms `size` metres long.
  PJ::ObjectTopicId addPose(glm::vec3 position, float size) {
    PJ::sdk::PosesInFrame poses;
    poses.frame_id = kFrame;
    PJ::sdk::Pose pose;
    pose.position = {position.x, position.y, position.z};
    pose.orientation.w = 1.0;
    poses.poses.push_back(pose);
    const auto id = fixture_.add("/poses", BT::kPosesInFrame, PJ::serializePosesInFrame(poses));
    setLayerAttribute(id, u"gizmo_size"_s, QString::number(size));
    return id;
  }
  // A one-edge TF tree: `child` at `offset` from kFrame.
  void addTf(const std::string& child, glm::vec3 offset) {
    PJ::sdk::FrameTransforms transforms;
    PJ::sdk::FrameTransform edge;
    edge.timestamp = 100;
    edge.parent_frame_id = kFrame;
    edge.child_frame_id = child;
    edge.translation = {offset.x, offset.y, offset.z};
    edge.rotation.w = 1.0;
    transforms.transforms.push_back(edge);
    dock()->setTransformService(&fixture_.transforms());
    fixture_.add("/tf", BT::kFrameTransforms, PJ::serializeFrameTransforms(transforms));
    fixture_.transforms().ingestFrameTransformsForDataset(fixture_.dataset());
  }
  // Through the persisted state: layer setters are private to their UI.
  void setLayerAttribute(PJ::ObjectTopicId id, const QString& name, const QString& value) {
    auto* layer = dock()->layerFor(id);
    ASSERT_NE(layer, nullptr);
    QDomDocument doc;
    QDomElement state = layer->xmlSaveState(doc);
    state.setAttribute(name, value);
    EXPECT_TRUE(layer->xmlLoadState(state));
  }
  // A local URDF file robot: its visual is the renderer's shadow caster, and
  // the local TF buffer it brings is what enables shadows.
  void addRobot(const QByteArray& urdf) {
    ASSERT_TRUE(urdf_dir_.isValid());
    const QString path = urdf_dir_.filePath(u"caster.urdf"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(urdf), urdf.size());
    file.close();
    EXPECT_NE(dock()->addRobotModelLayer(path).id, 0U);
  }

  // A single-field (elevation) GridMap of `n` x `n` cells in the shared frame.
  PJ::ObjectTopicId addGridMap(glm::vec2 corner, float cell, int n, float elevation) {
    std::vector<uint8_t> bytes(static_cast<std::size_t>(n) * n * sizeof(float));
    for (int i = 0; i < n * n; ++i) {
      const float value = elevation + 0.02F * static_cast<float>(i % n);  // a gentle slope: lit, not flat
      std::memcpy(bytes.data() + static_cast<std::size_t>(i) * sizeof(float), &value, sizeof(float));
    }
    PJ::sdk::GridMap grid;
    grid.timestamp_ns = 100;
    grid.frame_id = kFrame;
    grid.origin.position = {corner.x, corner.y, 0.0};
    grid.cell_size = {cell, cell};
    grid.column_count = static_cast<uint32_t>(n);
    grid.row_count = static_cast<uint32_t>(n);
    grid.cell_stride = sizeof(float);
    grid.row_stride = static_cast<uint32_t>(n * sizeof(float));
    grid.fields = {{.name = "elevation", .offset = 0, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1}};
    grid.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
    return fixture_.add("/grid_map", BT::kGridMap, PJ::serializeGridMap(grid));
  }
  // A pose topic whose first pose visits `positions`, one sample per 100 ns
  // from t = 100. Binds the fixture's TF service first: trails need a TF buffer.
  PJ::ObjectTopicId addPoseTrack(const std::vector<glm::vec3>& positions) {
    dock()->setTransformService(&fixture_.transforms());
    const auto sample = [](const glm::vec3& position, PJ::Timestamp t) {
      PJ::sdk::PosesInFrame poses;
      poses.frame_id = kFrame;
      poses.timestamp_ns = t;
      PJ::sdk::Pose pose;
      pose.position = {position.x, position.y, position.z};
      pose.orientation.w = 1.0;
      poses.poses.push_back(pose);
      return PJ::serializePosesInFrame(poses);
    };
    const auto id = fixture_.add("/poses", BT::kPosesInFrame, sample(positions.front(), 100));
    for (std::size_t i = 1; i < positions.size(); ++i) {
      const auto t = static_cast<PJ::Timestamp>(100 * (i + 1));
      fixture_.push(id, t, sample(positions[i], t));
    }
    return id;
  }

  SceneViewWidget* view() {
    return dock()->sceneView();
  }
  // Fixes frame, grid and a deterministic front-above camera. Call after the
  // layers exist (the view is created lazily). The warm-up frame consumes the
  // view's one-shot auto-fit to the first layer bounds. With `show` false the
  // view stays a never-shown widget (one valid grab only, see the top).
  void frame(bool show = true) {
    SceneViewWidget* v = view();
    ASSERT_NE(v, nullptr);
    if (show) {
      fixture_.window().resize(kViewSize);
      ASSERT_TRUE(showExposed(fixture_.window())) << "top-level never exposed";
    } else {
      v->resize(kViewSize);
    }
    dock()->setFixedFrame(QString::fromLatin1(kFrame));
    v->setGridStyle(SceneViewWidget::GridStyle::kFilledCells);
    v->setGridExtentMetres(10.0F);
    v->setAxesVisible(false);
    if (show) {
      grab(v, "warmup");
    }
    pj::scene3d::CameraState state;
    state.focal = {0.0F, 0.0F, 0.5F};
    state.radius = 8.0F;
    state.azimuth = glm::radians(-90.0F);  // eye on -Y looking +Y: +X is screen-right
    state.elevation = glm::radians(15.0F);
    v->camera().adoptState(state);
  }

  PJ::Scene3DDockWidget* dock() {
    return fixture_.dock();
  }
  // Binds the (empty) session TF service; topics added afterwards bind its
  // buffer. The view renders shadows only with a transform buffer, which a
  // scene without robots does not otherwise get.
  void bindTransforms() {
    dock()->setTransformService(&fixture_.transforms());
  }

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  QTemporaryDir urdf_dir_;
};

class Scene3dRhiRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

#define GRAB_OR_SKIP(image, view, name)                                                                   \
  const QImage image = grab(view, name);                                                                  \
  if (image.isNull()) {                                                                                   \
    PJ_SKIP_WITHOUT_GPU("QRhiWidget::grabFramebuffer() failed for " << requestedBackend().toStdString()); \
  }

// Also the one grab a never-shown widget supports: the dedicated-QRhi path.
TEST_F(Scene3dRhiRenderTest, BackendMatchesRequestAndImageSize) {
  RhiScene scene;
  scene.addPoints(pointBlock({0.0F, 0.0F, 0.5F}, 1.0F, 4));
  scene.frame(/*show=*/false);
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(image, view, "frame");

  EXPECT_EQ(view->rhiBackendNameForTest(), requestedBackend());
  const qreal dpr = view->devicePixelRatio();
  EXPECT_EQ(image.size(), QSize(qRound(view->width() * dpr), qRound(view->height() * dpr)));
  EXPECT_EQ(view->size(), kViewSize);
#ifdef Q_OS_MACOS
  // Qt creates this dedicated QRhi with a default-constructed QSurfaceFormat, not
  // QSurfaceFormat::defaultFormat(): on macOS that is a legacy 2.1 context without
  // GLSL 330, so the view reports it cannot render. A shown view gets 4.1 core.
  if (PJ::preferredGraphicsApi() == QRhiWidget::Api::OpenGL) {
    return;
  }
#endif
  EXPECT_TRUE(view->renderingWarningForTest().isEmpty()) << view->renderingWarningForTest().toStdString();
}

// A Y flip puts the ground grid in the upper half; the grid diff must sit below
// the horizon line the camera projects, and never in the top band (sky).
TEST_F(Scene3dRhiRenderTest, GroundGridFillsLowerHalf) {
  RhiScene scene;
  scene.addPoints(pointBlock({0.0F, 0.0F, 0.5F}, 0.2F, 2));
  scene.frame();
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(with_grid, view, "grid");
  view->setGridVisible(false);
  GRAB_OR_SKIP(without_grid, view, "no_grid");

  const DiffMask grid = diffMask(with_grid, without_grid, 20);
  const int w = with_grid.width();
  const int h = with_grid.height();
  const QRect top_band(0, 0, w, h / 5);
  const QRect bottom_band(0, h * 3 / 4, w, h - h * 3 / 4);
  EXPECT_EQ(grid.count_in(top_band), 0) << "grid drawn in the sky band: Y flipped?";
  EXPECT_GT(grid.count_in(bottom_band), bottom_band.width() * bottom_band.height() / 2)
      << "grid missing from the lower band: Y flipped?";
  // The far grid edge (y = +5 m) projects to a horizon line; no grid above it.
  const double horizon = project(*view, {0.0F, 5.0F, 0.0F}, with_grid.size()).y();
  EXPECT_EQ(grid.count_in(QRect(0, 0, w, std::max(0, int(horizon) - 3))), 0) << "horizon at y=" << horizon;
}

// A point block up and to the right of the focal must render in the upper-right
// quadrant, exactly where the camera projects it (catches X and Y flips).
TEST_F(Scene3dRhiRenderTest, PointCloudLandsWhereProjected) {
  RhiScene scene;
  const glm::vec3 centre{2.5F, 0.0F, 2.0F};
  const auto points = pointBlock(centre, 0.8F, 8);
  const PJ::ObjectTopicId topic = scene.addPoints(points);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  GRAB_OR_SKIP(with_points, view, "points");
  EXPECT_EQ(view->lastPointVertexCountForTest(), static_cast<int>(points.size()));
  scene.dock()->setLayerVisible(topic, false);
  GRAB_OR_SKIP(without_points, view, "no_points");

  const DiffMask mask = diffMask(with_points, without_points);
  const QPointF expected = project(*view, centre, with_points.size());
  ASSERT_GT(mask.count, 200) << "point block did not colour any pixels";
  EXPECT_GT(expected.x(), with_points.width() / 2.0);
  EXPECT_LT(expected.y(), with_points.height() / 2.0);
  EXPECT_NEAR(mask.centroid.x(), expected.x(), with_points.width() * 0.05)
      << "centroid " << mask.centroid.x() << "," << mask.centroid.y();
  EXPECT_NEAR(mask.centroid.y(), expected.y(), with_points.height() * 0.05)
      << "centroid " << mask.centroid.x() << "," << mask.centroid.y();
}

// Opaque cubes must depth-test against each other regardless of draw order
// (the occluded blue cube is drawn last), and a translucent cube must blend
// over the opaque cube behind it wherever its layer sits in the dock order: the
// renderer draws every opaque draw before any translucent one.
void checkOcclusionAndTranslucency(bool glass_layer_first) {
  RhiScene scene;
  const glm::vec3 near_red{-1.5F, -1.0F, 0.5F};
  const glm::vec3 far_blue{-1.5F, 2.5F, 1.0F};
  const glm::vec3 glass{2.0F, -1.0F, 0.6F};
  const glm::vec3 behind_glass{2.0F, 1.5F, 0.9F};
  // Any translucent cube turns its whole layer's cubes translucent, so the glass
  // is its own layer.
  const auto add_glass = [&] { scene.addEntities({cubeEntity("glass", glass, 1.0F, {40, 255, 40, 90})}, "/glass"); };
  if (glass_layer_first) {
    add_glass();
  }
  scene.addEntities(
      {
          cubeEntity("b_near_red", near_red, 1.0F, {255, 30, 30, 255}),
          cubeEntity("c_behind_glass", behind_glass, 1.6F, {255, 30, 30, 255}),
          cubeEntity("z_far_blue", far_blue, 2.5F, {30, 30, 255, 255}),
      },
      "/opaque");
  if (!glass_layer_first) {
    add_glass();
  }
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  GRAB_OR_SKIP(image, view, "cubes");
  EXPECT_EQ(view->lastMarkerInstanceCountForTest(), 8) << "4 cube fills + 4 edge overlays";

  const QColor red = meanColor(image, project(*view, near_red, image.size()));
  EXPECT_GT(red.red(), red.blue() + 60) << "near red cube hidden by the far blue one: depth broken? rgb=" << red.red()
                                        << "," << red.green() << "," << red.blue();
  const QColor blue = meanColor(image, project(*view, far_blue + glm::vec3(0.0F, 0.0F, 1.0F), image.size()));
  EXPECT_GT(blue.blue(), blue.red() + 60)
      << "far blue cube top not visible; rgb=" << blue.red() << "," << blue.green() << "," << blue.blue();
  const QColor mixed = meanColor(image, project(*view, glass, image.size()));
  EXPECT_GT(mixed.red(), 60) << "opaque red not visible through the glass; rgb=" << mixed.red() << "," << mixed.green()
                             << "," << mixed.blue();
  EXPECT_GT(mixed.green(), mixed.blue() + 20)
      << "glass tint missing; rgb=" << mixed.red() << "," << mixed.green() << "," << mixed.blue();
  // Both layers drew, reported in dock order even though the glass drew last.
  const auto layers = scene.dock()->layers();
  ASSERT_EQ(layers.size(), 2U);
  EXPECT_EQ(
      view->lastSubmittedLayerIdsForTest(), (std::vector<std::uint32_t>{layers[0].topic_id.id, layers[1].topic_id.id}));
}

TEST_F(Scene3dRhiRenderTest, OcclusionAndTranslucency) {
  checkOcclusionAndTranslucency(/*glass_layer_first=*/false);
}

// The glass layer first in the dock: without the opaque-first submission the
// red cube behind it would overwrite it (the glass writes no depth).
TEST_F(Scene3dRhiRenderTest, TranslucentLayerFirstStillBlendsOverOpaque) {
  checkOcclusionAndTranslucency(/*glass_layer_first=*/true);
}

constexpr auto kCasterUrdf = R"(<robot name="caster">
  <link name="base_link">
    <visual>
      <origin xyz="0 0 1.2"/>
      <geometry><box size="1 1 1"/></geometry>
    </visual>
  </link>
</robot>)";

// The full quality chain runs on a mesh + points + markers scene: HDR, SSAO,
// EDL and shadows all active (no silent fallback), and the mesh actually casts
// a shadow on the grid (turning shadows off brightens ground pixels).
TEST_F(Scene3dRhiRenderTest, QualityStagesActiveAndMeshCastsShadow) {
  RhiScene scene;
  scene.addRobot(kCasterUrdf);
  scene.addPoints(pointBlock({2.5F, 0.0F, 0.5F}, 0.8F, 6));
  scene.addEntities({cubeEntity("cube", {-2.5F, 0.0F, 0.5F}, 1.0F, {200, 200, 60, 255})});
  scene.frame();
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(shadowed, view, "shadows");

  EXPECT_TRUE(view->lastHdrActiveForTest()) << view->hdrCapabilityErrorForTest().toStdString();
  EXPECT_TRUE(view->lastSsaoActiveForTest()) << view->ssaoCapabilityErrorForTest().toStdString();
  EXPECT_TRUE(view->lastEdlActiveForTest()) << view->edlCapabilityErrorForTest().toStdString();
  EXPECT_TRUE(view->lastShadowActiveForTest()) << view->shadowCapabilityErrorForTest().toStdString();
  EXPECT_GT(view->lastShadowDrawCountForTest(), 0);
  EXPECT_GT(view->lastModelDrawCountForTest(), 0);
  EXPECT_GT(view->lastPointVertexCountForTest(), 0);
  EXPECT_GT(view->lastMarkerInstanceCountForTest(), 0);
  EXPECT_TRUE(view->renderingWarningForTest().isEmpty()) << view->renderingWarningForTest().toStdString();

  auto params = view->meshShadingParams();
  params.shadows_enabled = false;
  view->setMeshShadingParams(params);
  GRAB_OR_SKIP(unshadowed, view, "no_shadows");
  EXPECT_FALSE(view->lastShadowActiveForTest());
  EXPECT_GT(darkenedPixels(shadowed, unshadowed), 300) << "enabling shadows darkened no ground pixels";
}

// A SceneEntities cube lifted off the ground is a shadow caster on its own (no
// model in the scene): enabling shadows darkens the ground under it.
TEST_F(Scene3dRhiRenderTest, SceneEntitiesCubeCastsShadow) {
  RhiScene scene;
  scene.bindTransforms();  // before the topic: adding it binds the dataset's TF buffer
  scene.addEntities({cubeEntity("cube", {0.0F, 0.0F, 1.2F}, 1.0F, {200, 200, 60, 255})});
  scene.frame();
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(shadowed, view, "shadows");
  EXPECT_TRUE(view->lastShadowActiveForTest()) << view->shadowCapabilityErrorForTest().toStdString();
  EXPECT_EQ(view->lastModelDrawCountForTest(), 0);
  EXPECT_EQ(view->lastShadowDrawCountForTest(), 1) << "one instanced cube caster range";
  EXPECT_EQ(view->lastShadowTriangleCountForTest(), 12);

  auto params = view->meshShadingParams();
  params.shadows_enabled = false;
  view->setMeshShadingParams(params);
  GRAB_OR_SKIP(unshadowed, view, "no_shadows");
  EXPECT_FALSE(view->lastShadowActiveForTest());
  EXPECT_GT(darkenedPixels(shadowed, unshadowed), 300) << "the cube cast no shadow on the grid";
  // Markers do not receive shadows: the ground darkened, the cube's front face
  // (facing the camera) did not.
  const glm::vec3 front_face{0.0F, -0.5F, 1.2F};
  EXPECT_EQ(
      meanColor(shadowed, project(*view, front_face, shadowed.size())),
      meanColor(unshadowed, project(*view, front_face, unshadowed.size())));
}

// Pixels of `mask` in each image quadrant: {top-left, top-right, bottom-left, bottom-right}.
std::array<int, 4> quadrantCounts(const DiffMask& mask, const QSize& size) {
  const int w = size.width() / 2;
  const int h = size.height() / 2;
  return {
      mask.count_in(QRect(0, 0, w, h)), mask.count_in(QRect(w, 0, size.width() - w, h)),
      mask.count_in(QRect(0, h, w, size.height() - h)),
      mask.count_in(QRect(w, h, size.width() - w, size.height() - h))};
}

// A unit quad in the XY plane textured with marked.png: UV (0,0) at (-x,-y), (1,1)
// at (+x,+y) (tests/fixtures/meshes/textured_quad.dae with its own texture).
constexpr char kMarkedQuadDae[] = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Z_UP</up_axis></asset>
  <library_images><image id="img"><init_from>marked.png</init_from></image></library_images>
  <library_effects><effect id="fx"><profile_COMMON>
    <newparam sid="surf"><surface type="2D"><init_from>img</init_from></surface></newparam>
    <newparam sid="samp"><sampler2D><source>surf</source></sampler2D></newparam>
    <technique sid="common"><lambert><diffuse><texture texture="samp" texcoord="UV0"/></diffuse></lambert></technique>
  </profile_COMMON></effect></library_effects>
  <library_materials><material id="mat"><instance_effect url="#fx"/></material></library_materials>
  <library_geometries><geometry id="quad"><mesh>
    <source id="pos"><float_array id="pos-a" count="12">-0.5 -0.5 0 0.5 -0.5 0 0.5 0.5 0 -0.5 0.5 0</float_array>
      <technique_common><accessor source="#pos-a" count="4" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <source id="uv"><float_array id="uv-a" count="8">0 0 1 0 1 1 0 1</float_array>
      <technique_common><accessor source="#uv-a" count="4" stride="2">
        <param name="S" type="float"/><param name="T" type="float"/>
      </accessor></technique_common></source>
    <vertices id="verts"><input semantic="POSITION" source="#pos"/></vertices>
    <triangles material="sym" count="2">
      <input semantic="VERTEX" source="#verts" offset="0"/>
      <input semantic="TEXCOORD" source="#uv" offset="1" set="0"/>
      <p>0 0 1 1 2 2 0 0 2 2 3 3</p>
    </triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node id="node"><instance_geometry url="#quad">
    <bind_material><technique_common><instance_material symbol="sym" target="#mat">
      <bind_vertex_input semantic="UV0" input_semantic="TEXCOORD" input_set="0"/>
    </instance_material></technique_common></bind_material>
  </instance_geometry></node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
)";

// A textured model whose texture marks one corner: its top-left quadrant is green,
// the rest red. Standing upright facing the camera (image top = +Z, image left =
// -X), the green quadrant must land top-left on screen on every backend: a
// mirrored or Y-flipped texture sample (or upload) moves it to another quadrant.
TEST_F(Scene3dRhiRenderTest, OrientationTextureMarkedCornerStaysTopLeft) {
  QTemporaryDir mesh_dir;
  ASSERT_TRUE(mesh_dir.isValid());
  QFile dae(mesh_dir.filePath(u"quad.dae"_s));
  ASSERT_TRUE(dae.open(QIODevice::WriteOnly));
  ASSERT_GT(dae.write(kMarkedQuadDae), 0);
  dae.close();
  QImage texture(64, 64, QImage::Format_RGB32);
  texture.fill(QColor(255, 0, 0));
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      texture.setPixelColor(x, y, QColor(0, 255, 0));
    }
  }
  ASSERT_TRUE(texture.save(mesh_dir.filePath(u"marked.png"_s)));

  RhiScene scene;
  const glm::vec3 centre{0.0F, 0.0F, 1.5F};
  scene.addRobot(
      u"<robot name=\"quad\"><link name=\"base_link\"><visual>"
      "<origin xyz=\"0 0 1.5\" rpy=\"1.5707963 0 0\"/>"
      "<geometry><mesh filename=\"%1\" scale=\"2 2 2\"/></geometry>"
      "</visual></link></robot>"_s.arg(mesh_dir.filePath(u"quad.dae"_s))
          .toUtf8());
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  QImage image;
  for (int attempt = 0; attempt < 100 && view->lastModelTextureSlotCountsForTest()[0] == 0; ++attempt) {
    QTest::qWait(20);  // the mesh imports off the GUI thread
    image = grab(view, "quad");
    if (image.isNull()) {
      PJ_SKIP_WITHOUT_GPU("QRhiWidget::grabFramebuffer() failed for " << requestedBackend().toStdString());
    }
  }
  ASSERT_GT(view->lastModelTextureSlotCountsForTest()[0], 0) << "the textured quad never loaded";
  GRAB_OR_SKIP(textured, view, "textured");

  const auto sample = [&](float x, float z) {
    return meanColor(textured, project(*view, centre + glm::vec3(x, 0.0F, z), textured.size()), 2);
  };
  const auto is_green = [](const QColor& c) { return c.green() > c.red() + 40; };
  const auto is_red = [](const QColor& c) { return c.red() > c.green() + 40; };
  const QColor top_left = sample(-0.5F, 0.5F);
  const QColor top_right = sample(0.5F, 0.5F);
  const QColor bottom_left = sample(-0.5F, -0.5F);
  const QColor bottom_right = sample(0.5F, -0.5F);
  EXPECT_TRUE(is_green(top_left)) << "top-left rgb=" << top_left.red() << "," << top_left.green() << ","
                                  << top_left.blue();
  EXPECT_TRUE(is_red(top_right)) << "mirrored in X? top-right rgb=" << top_right.red() << "," << top_right.green();
  EXPECT_TRUE(is_red(bottom_left)) << "flipped in Y? bottom-left rgb=" << bottom_left.red() << ","
                                   << bottom_left.green();
  EXPECT_TRUE(is_red(bottom_right)) << "bottom-right rgb=" << bottom_right.red() << "," << bottom_right.green();
}

// A cube lifted off the ground under a key light from the left and behind:
// its shadow must fall right of and in front of the cube, where the light ray
// through the cube's centre meets the ground. A Y-flipped shadow-map lookup (or
// a flipped depth comparison) moves or loses it.
TEST_F(Scene3dRhiRenderTest, OrientationShadowFallsAwayFromTheLight) {
  RhiScene scene;
  scene.bindTransforms();
  const glm::vec3 cube{-1.0F, 0.5F, 1.2F};
  scene.addEntities({cubeEntity("cube", cube, 0.8F, {200, 200, 60, 255})});
  scene.frame();
  SceneViewWidget* view = scene.view();
  auto params = view->meshShadingParams();
  params.key_light_dir = glm::normalize(glm::vec3(-1.0F, 1.0F, 1.5F));  // to the light: left, behind, above
  view->setMeshShadingParams(params);
  GRAB_OR_SKIP(shadowed, view, "shadows");
  ASSERT_TRUE(view->lastShadowActiveForTest()) << view->shadowCapabilityErrorForTest().toStdString();
  params.shadows_enabled = false;
  view->setMeshShadingParams(params);
  GRAB_OR_SKIP(unshadowed, view, "no_shadows");

  // Pixels the shadow darkened on the ground (the cube itself receives none).
  DiffMask shadow;
  double sx = 0.0;
  double sy = 0.0;
  for (int y = 0; y < shadowed.height(); ++y) {
    for (int x = 0; x < shadowed.width(); ++x) {
      if (qGray(unshadowed.pixel(x, y)) - qGray(shadowed.pixel(x, y)) > 12) {
        shadow.pixels.emplace_back(x, y);
        sx += x;
        sy += y;
      }
    }
  }
  shadow.count = static_cast<int>(shadow.pixels.size());
  ASSERT_GT(shadow.count, 300) << "the cube cast no shadow";
  const QPointF centroid(sx / shadow.count, sy / shadow.count);
  const glm::vec3 to_light = params.key_light_dir;
  const glm::vec3 ground_hit = cube - to_light * (cube.z / to_light.z);
  const QPointF expected = project(*view, ground_hit, shadowed.size());
  const QPointF caster = project(*view, cube, shadowed.size());
  EXPECT_GT(centroid.x(), caster.x()) << "shadow on the light's side: mirrored in X?";
  EXPECT_GT(centroid.y(), caster.y()) << "shadow above the caster: flipped in Y?";
  // The penumbra and the cube's own silhouette keep the centroid a few pixels off;
  // a v-mirrored shadow-map lookup moves it ~20 px at this size.
  EXPECT_NEAR(centroid.x(), expected.x(), shadowed.width() * 0.025) << "expected x=" << expected.x();
  EXPECT_NEAR(centroid.y(), expected.y(), shadowed.height() * 0.025) << "expected y=" << expected.y();
}

// Content only in the top-left quadrant: SSAO (a concave crease between two
// cubes) and EDL (a box model's depth edges) must darken only there. The
// present pass samples the coverage, AO and depth textures; any of them read
// flipped in Y or mirrored in X moves the darkening into another quadrant.
TEST_F(Scene3dRhiRenderTest, OrientationSsaoAndEdlStayWithTheirContent) {
  RhiScene scene;
  // A wall facing the camera with a small cube against it: a concave crease.
  scene.addEntities({
      cubeEntity("wall", {-2.6F, 1.0F, 2.6F}, 2.0F, {180, 180, 180, 255}),
      cubeEntity("bump", {-2.6F, -0.3F, 2.6F}, 0.6F, {180, 180, 180, 255}),
  });
  // EDL shades model meshes (the replayed mesh mask): a box model, also top-left.
  scene.addRobot(R"(<robot name="box"><link name="base_link"><visual>
      <origin xyz="-1 -1 2.4"/><geometry><box size="0.6 0.6 0.6"/></geometry>
    </visual></link></robot>)");
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  GRAB_OR_SKIP(both, view, "ssao_edl");
  ASSERT_TRUE(view->lastSsaoActiveForTest()) << view->ssaoCapabilityErrorForTest().toStdString();
  ASSERT_TRUE(view->lastEdlActiveForTest()) << view->edlCapabilityErrorForTest().toStdString();
  view->compositeParams().ssao_enabled = false;
  GRAB_OR_SKIP(edl_only, view, "edl_only");
  view->compositeParams().edl_enabled = false;
  GRAB_OR_SKIP(neither, view, "neither");

  const DiffMask ssao = diffMask(both, edl_only, 6);
  const DiffMask edl = diffMask(edl_only, neither, 6);
  const auto ssao_quadrants = quadrantCounts(ssao, both.size());
  const auto edl_quadrants = quadrantCounts(edl, both.size());
  ASSERT_GT(ssao.count, 30) << "SSAO darkened nothing";
  // AO hugs the crease: normals facing away from the camera (a flipped v sign)
  // occlude every flat face instead.
  int covered = 0;
  const QRgb background = neither.pixel(neither.width() - 1, 0);
  for (int y = 0; y < neither.height(); ++y) {
    for (int x = 0; x < neither.width(); ++x) {
      covered += channelDeltaSum(neither.pixel(x, y), background) > 6 ? 1 : 0;
    }
  }
  EXPECT_LT(ssao.count * 8, covered) << "SSAO darkened flat faces: normals flipped?";
  ASSERT_GT(edl.count, 30) << "EDL darkened nothing";
  for (std::size_t quadrant = 1; quadrant < 4; ++quadrant) {
    EXPECT_LT(ssao_quadrants[quadrant] * 10, ssao_quadrants[0]) << "SSAO in quadrant " << quadrant;
    EXPECT_LT(edl_quadrants[quadrant] * 10, edl_quadrants[0]) << "EDL in quadrant " << quadrant;
  }
}

// A draw that writes no depth (here a translucent cube) leaves no coverage in
// the depth replay, like the GL composite that finds the background by depth:
// where it tints only the background the present pass keeps that pixel
// ungraded, so the tonemap cannot change it. Over an opaque cube the same glass
// is graded.
TEST_F(Scene3dRhiRenderTest, NoDepthWriteDrawLeavesBackgroundUngraded) {
  RhiScene scene;
  const glm::vec3 glass{0.0F, 0.0F, 1.0F};
  const glm::vec3 behind{0.9F, 1.5F, 1.0F};  // covers the glass's right half only
  scene.addEntities({cubeEntity("opaque", behind, 1.0F, {60, 60, 255, 255})}, "/opaque");
  scene.addEntities({cubeEntity("glass", glass, 1.6F, {40, 255, 40, 120})}, "/glass");
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  view->compositeParams().tonemap_mode = 0;
  GRAB_OR_SKIP(plain, view, "tonemap_none");
  ASSERT_TRUE(view->lastHdrActiveForTest()) << view->hdrCapabilityErrorForTest().toStdString();
  view->compositeParams().tonemap_mode = 1;
  GRAB_OR_SKIP(aces, view, "tonemap_aces");

  const QPointF over_background = project(*view, glass + glm::vec3(-0.5F, -0.8F, 0.2F), plain.size());
  const QPointF over_opaque = project(*view, glass + glm::vec3(0.6F, -0.8F, 0.2F), plain.size());
  const QColor background_plain = meanColor(plain, over_background, 2);
  const QColor background_aces = meanColor(aces, over_background, 2);
  EXPECT_GT(background_plain.green(), background_plain.blue() + 20) << "the glass is not over this pixel";
  EXPECT_LE(channelDeltaSum(background_plain.rgb(), background_aces.rgb()), 3)
      << "glass over the background was graded: it left coverage in the depth replay";
  EXPECT_GT(channelDeltaSum(meanColor(plain, over_opaque, 2).rgb(), meanColor(aces, over_opaque, 2).rgb()), 10)
      << "glass over the opaque cube was not graded";
}

// Real top-levels: show, resize, reparent the dock to a second window, destroy
// a sibling view there, and the view must keep rendering its content.
TEST_F(Scene3dRhiRenderTest, SurvivesShowResizeReparentAndSiblingDestruction) {
  RhiScene scene;
  const glm::vec3 centre{0.0F, 0.0F, 1.0F};
  const auto points = pointBlock(centre, 1.0F, 6);
  scene.addPoints(points);
  scene.frame(/*show=*/false);
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(hidden, view, "hidden");

  QWidget first;
  auto* first_layout = new QVBoxLayout(&first);
  first_layout->addWidget(scene.dock());
  first.resize(kViewSize);
  ASSERT_TRUE(showExposed(first));
  GRAB_OR_SKIP(shown, view, "shown");
  const qreal dpr = view->devicePixelRatio();
  EXPECT_EQ(view->lastPointVertexCountForTest(), static_cast<int>(points.size()));

  // Shrink the dock inside the window: a compositor may veto a top-level resize.
  scene.dock()->setFixedSize(kViewSize - QSize(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&]() { return view->size() * dpr != shown.size(); }, 2000));
  GRAB_OR_SKIP(resized, view, "resized");
  EXPECT_EQ(resized.size(), QSize(qRound(view->width() * dpr), qRound(view->height() * dpr)));
  EXPECT_NE(resized.size(), shown.size());

  QWidget second;
  auto* second_layout = new QHBoxLayout(&second);
  auto* sibling = new SceneViewWidget();
  second_layout->addWidget(sibling);
  second_layout->addWidget(scene.dock());
  second.resize(kViewSize * 2);
  ASSERT_TRUE(showExposed(second));
  GRAB_OR_SKIP(sibling_image, sibling, "sibling");
  GRAB_OR_SKIP(reparented, view, "reparented");
  delete sibling;
  GRAB_OR_SKIP(after_sibling, view, "after_sibling");

  EXPECT_EQ(view->lastPointVertexCountForTest(), static_cast<int>(points.size()));
  view->setGridVisible(false);
  GRAB_OR_SKIP(no_grid, view, "after_sibling_no_grid");
  const auto layer = scene.dock()->layers().front().topic_id;
  scene.dock()->setLayerVisible(layer, false);
  GRAB_OR_SKIP(no_points, view, "after_sibling_no_points");
  const DiffMask mask = diffMask(no_grid, no_points);
  ASSERT_GT(mask.count, 200) << "points no longer render after reparent + sibling destruction";
  const QPointF expected = project(*view, centre, no_grid.size());
  EXPECT_NEAR(mask.centroid.x(), expected.x(), no_grid.width() * 0.05);
  EXPECT_NEAR(mask.centroid.y(), expected.y(), no_grid.height() * 0.05);
  EXPECT_EQ(view->rhiBackendNameForTest(), requestedBackend());
}

// ---- Presentation: corner gizmo, overlays, picking, render scale, tonemaps ----
// These also run at QT_SCALE_FACTOR=2 (the .opengl_dpr2. ctest pass): every
// check goes through devicePixelRatio() or logical widget coordinates.

// A robot whose body sits away from the origin, so the TF triad of its "tool"
// frame at the origin is free to see and hover (a single-link robot publishes
// no transform).
constexpr auto kOffsetRobotUrdf = R"(<robot name="offset">
  <link name="base_link">
    <visual>
      <origin xyz="3 0 1.2"/>
      <geometry><box size="1 1 1"/></geometry>
    </visual>
  </link>
  <link name="tool"/>
  <joint name="tool_joint" type="fixed">
    <parent link="base_link"/>
    <child link="tool"/>
    <origin xyz="0 0 0"/>
  </joint>
</robot>)";

// The corner gizmo's output rectangle: 80 logical pixels, bottom-right.
QRect gizmoRect(const SceneViewWidget& view, const QImage& image) {
  const int size = static_cast<int>(80.0 * image.height() / view.height());
  return {image.width() - size, image.height() - size, size, size};
}

void sendMouseMove(SceneViewWidget* view, QPointF logical) {
  QMouseEvent move(QEvent::MouseMove, logical, view->mapToGlobal(logical), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view, &move);
}

// The world axis gizmo sits in the bottom-right corner, follows the camera
// rotation and never draws outside its 80-logical-pixel square.
TEST_F(Scene3dRhiRenderTest, PresentationCornerGizmoFollowsCamera) {
  RhiScene scene;
  const PJ::ObjectTopicId topic = scene.addPoints(pointBlock({0.0F, 0.0F, 0.5F}, 0.2F, 2));
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  scene.dock()->setLayerVisible(topic, false);
  GRAB_OR_SKIP(front, view, "front");
  auto state = view->camera().state();
  state.azimuth += glm::radians(90.0F);
  view->camera().adoptState(state);
  GRAB_OR_SKIP(side, view, "side");

  const QRect rect = gizmoRect(*view, front);
  const DiffMask mask = diffMask(front, side);
  ASSERT_GT(mask.count, 50) << "the gizmo did not follow the camera";
  EXPECT_EQ(mask.count_in(rect), mask.count) << "overlay drawn outside the gizmo corner";
  // Eye on -Y: X (red) points right, Z (blue) up, both clearly visible.
  int red = 0;
  int blue = 0;
  for (int y = rect.top(); y <= rect.bottom(); ++y) {
    for (int x = rect.left(); x <= rect.right(); ++x) {
      const QRgb px = front.pixel(x, y);
      red += qRed(px) > qGreen(px) + 80 && qRed(px) > qBlue(px) + 80 ? 1 : 0;
      blue += qBlue(px) > qRed(px) + 60 && qBlue(px) > qGreen(px) + 30 ? 1 : 0;
    }
  }
  EXPECT_GT(red, 20);
  EXPECT_GT(blue, 20);
  const QColor red_arm = meanColor(front, rect.center() + QPoint(rect.width() / 4, 0), 1);
  EXPECT_GT(red_arm.red(), red_arm.green() + 80) << "red arm not to the right of the gizmo centre";
}

// The hover label (and its ring) and the perf HUD are texture quads over the
// presented image, positioned in logical pixels; frameAt() is the one picking
// primitive behind the hover.
TEST_F(Scene3dRhiRenderTest, PresentationHoverLabelAndPerfHud) {
  RhiScene scene;
  scene.addRobot(kOffsetRobotUrdf);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  view->setAxesVisible(true);
  view->setGizmoSize(1.0F);
  GRAB_OR_SKIP(plain, view, "plain");
  const qreal dpr = view->devicePixelRatio();

  const QPointF origin = project(*view, {0.0F, 0.0F, 0.0F}, view->size());
  // base_link and tool share the origin; the first listed frame wins the tie.
  const std::optional<std::string> picked = view->frameAt(origin + QPointF(6.0, -6.0));
  ASSERT_TRUE(picked == "base_link" || picked == "tool") << picked.value_or("<none>");
  EXPECT_FALSE(view->frameAt(origin + QPointF(60.0, 0.0)).has_value());

  QStringList hovered;
  QObject::connect(
      view, &SceneViewWidget::hoveredFrameChanged, view, [&hovered](const QString& frame) { hovered << frame; });
  sendMouseMove(view, origin);
  EXPECT_EQ(view->hoveredFrame(), picked);
  EXPECT_EQ(hovered, QStringList{QString::fromStdString(*picked)});
  GRAB_OR_SKIP(labelled, view, "labelled");
  const QRect label = view->lastOverlayRectsForTest()[1];
  const QRect ring = view->lastOverlayRectsForTest()[2];
  ASSERT_TRUE(label.isValid() && ring.isValid());
  // Above-right of the origin, in output pixels.
  EXPECT_GT(label.left(), origin.x() * dpr);
  EXPECT_LT(label.bottom(), origin.y() * dpr);
  EXPECT_TRUE(ring.contains((origin * dpr).toPoint()));
  const DiffMask label_mask = diffMask(plain, labelled, 30);
  EXPECT_GT(label_mask.count_in(label), label.width() * label.height() / 10) << "label not drawn";
  // Outside the overlay rectangles only the hovered frame's triad changes: it
  // brightens, as the GL AxisRenderPass highlight does.
  const QRgb background = plain.pixel(4, 4);
  int stray = 0;
  int brightened = 0;
  for (const QPoint& p : label_mask.pixels) {
    if (label.united(ring).contains(p)) {
      continue;
    }
    if (channelDeltaSum(plain.pixel(p), background) <= 30) {
      ++stray;
    } else if (qGray(labelled.pixel(p)) > qGray(plain.pixel(p))) {
      ++brightened;
    }
  }
  EXPECT_EQ(stray, 0) << "overlay drawn outside its rectangles";
  EXPECT_GT(brightened, 20) << "hovered triad not highlighted";

  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(view, &leave);
  EXPECT_FALSE(view->hoveredFrame().has_value());
  EXPECT_EQ(hovered, (QStringList{QString::fromStdString(*picked), QString()}));
  GRAB_OR_SKIP(left, view, "left");
  EXPECT_FALSE(view->lastOverlayRectsForTest()[1].isValid());
  EXPECT_EQ(diffMask(plain, left, 0).count, 0) << "hover label left behind";

  view->setShowPerfHud(true);
  GRAB_OR_SKIP(hud, view, "hud");
  const QRect panel = view->lastOverlayRectsForTest()[0];
  ASSERT_TRUE(panel.isValid());
  EXPECT_EQ(panel.left(), qRound(8 * dpr));
  EXPECT_NEAR(panel.bottom() + 1, hud.height() - 8 * dpr, 1.0);
  const DiffMask hud_mask = diffMask(plain, hud, 30);
  EXPECT_GT(hud_mask.count_in(panel), panel.width() * panel.height() / 20) << "perf HUD not drawn";
  EXPECT_EQ(hud_mask.count_in(panel), hud_mask.count) << "perf HUD drawn outside its panel";
}

// render_scale renders the HDR chain at scale x output and presents at output
// size; the fallback and its recovery keep the scaled size, and 1.0 restores
// the exact image.
TEST_F(Scene3dRhiRenderTest, PresentationRenderScaleSupersamples) {
  RhiScene scene;
  const glm::vec3 centre{1.5F, 0.0F, 1.0F};
  scene.addRobot(kOffsetRobotUrdf);
  scene.addPoints(pointBlock(centre, 0.8F, 8));
  scene.frame();
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(scale1, view, "scale1");
  const QSize output = scale1.size();
  EXPECT_EQ(view->hdrRenderSizeForTest(), output);

  view->setRenderScale(2.0F);
  EXPECT_EQ(view->renderScale(), 2.0F);
  GRAB_OR_SKIP(scale2, view, "scale2");
  EXPECT_EQ(scale2.size(), output);
  EXPECT_EQ(view->hdrRenderSizeForTest(), output * 2);
  EXPECT_TRUE(view->lastHdrActiveForTest() && view->lastSsaoActiveForTest() && view->lastEdlActiveForTest());
  // The same picture, only anti-aliased: few pixels move noticeably.
  const DiffMask moved = diffMask(scale1, scale2, 90);
  EXPECT_LT(moved.count, output.width() * output.height() / 50) << "supersampled image differs beyond edges";

  view->setForceHdrFallbackForTest(true);
  GRAB_OR_SKIP(fallback, view, "fallback");
  EXPECT_FALSE(view->lastHdrActiveForTest());
  EXPECT_EQ(fallback.size(), output);
  const QColor block = meanColor(fallback, project(*view, centre, output));
  const QColor sky = meanColor(fallback, QPointF(4.0, 4.0));
  EXPECT_GT(
      std::abs(block.red() - sky.red()) + std::abs(block.green() - sky.green()) + std::abs(block.blue() - sky.blue()),
      60)
      << "direct fallback lost the points";
  view->setForceHdrFallbackForTest(false);
  GRAB_OR_SKIP(recovered, view, "recovered");
  EXPECT_TRUE(view->lastHdrActiveForTest());
  EXPECT_EQ(view->hdrRenderSizeForTest(), output * 2);

  view->setRenderScale(1.0F);
  GRAB_OR_SKIP(back, view, "back");
  EXPECT_EQ(view->hdrRenderSizeForTest(), output);
  EXPECT_LT(diffMask(scale1, back, 3).count, output.width() * output.height() / 1000) << "scale 1 not restored";

  // A resize at scale 2 follows the new output.
  view->setRenderScale(2.0F);
  scene.dock()->setFixedSize(kViewSize - QSize(100, 80));
  const qreal dpr = view->devicePixelRatio();
  ASSERT_TRUE(QTest::qWaitFor([&]() { return view->size() * dpr != output; }, 2000));
  GRAB_OR_SKIP(resized, view, "resized");
  EXPECT_EQ(view->hdrRenderSizeForTest(), resized.size() * 2);

  view->setRenderScale(10.0F);
  EXPECT_EQ(view->renderScale(), 4.0F);
  view->setRenderScale(0.1F);
  EXPECT_EQ(view->renderScale(), 1.0F);
}

// Each tonemap grades data pixels differently, while the background and the
// annotation (TF triad) pixels bypass the grade in every mode.
TEST_F(Scene3dRhiRenderTest, PresentationTonemapModes) {
  RhiScene scene;
  scene.addRobot(kOffsetRobotUrdf);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  view->setAxesVisible(true);
  view->setGizmoSize(1.0F);
  auto params = view->meshShadingParams();
  params.shadows_enabled = false;
  view->setMeshShadingParams(params);

  std::vector<QImage> images;
  for (int mode = 0; mode < 4; ++mode) {
    view->compositeParams().tonemap_mode = mode;
    view->update();
    const std::string name = "mode" + std::to_string(mode);
    GRAB_OR_SKIP(image, view, name.c_str());
    EXPECT_TRUE(view->lastHdrActiveForTest());
    images.push_back(image);
  }
  const QSize size = images.front().size();
  const QPointF box = project(*view, {3.0F, -0.5F, 1.2F}, size);
  const QPointF arm = project(*view, {0.6F, 0.0F, 0.0F}, size);
  const auto distance = [](QColor a, QColor b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
  };
  for (std::size_t mode = 1; mode < images.size(); ++mode) {
    EXPECT_EQ(images[mode].pixel(4, 4), images[0].pixel(4, 4)) << "background graded in mode " << mode;
    EXPECT_LE(distance(meanColor(images[mode], arm, 1), meanColor(images[0], arm, 1)), 3)
        << "TF triad graded in mode " << mode;
    EXPECT_GT(distance(meanColor(images[mode], box), meanColor(images[mode - 1], box)), 6)
        << "modes " << mode - 1 << " and " << mode << " grade the mesh alike";
  }
  const QColor arm_colour = meanColor(images[0], arm, 1);
  EXPECT_GT(arm_colour.red(), arm_colour.green() + 80) << "the X arm is not where it was projected";
}

// Re-expresses voxel_grid_render_pass_gl_test on the QRhi renderer: in one
// instanced draw, a voxel whose value passes the (default non-zero) predicate
// rasterizes where the camera projects it, and a culled voxel between two drawn
// ones rasterizes nothing.
TEST_F(Scene3dRhiRenderTest, VoxelPredicateDrawsAndCulls) {
  RhiScene scene;
  const PJ::ObjectTopicId topic = scene.addVoxelRow({255, 0, 255}, {-1.5F, -0.5F, 0.0F}, 1.0F);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  GRAB_OR_SKIP(with_voxels, view, "voxels");
  EXPECT_EQ(view->lastVoxelLayerCountForTest(), 1);
  EXPECT_EQ(view->lastVoxelCountForTest(), 3) << "one instance per lattice cell, culled ones included";
  scene.dock()->setLayerVisible(topic, false);
  GRAB_OR_SKIP(without_voxels, view, "no_voxels");

  const DiffMask mask = diffMask(with_voxels, without_voxels);
  const auto around = [&](glm::vec3 world) {
    const QPointF centre = project(*view, world, with_voxels.size());
    return QRect(int(centre.x()) - 3, int(centre.y()) - 3, 7, 7);
  };
  EXPECT_EQ(mask.count_in(around({-1.0F, 0.0F, 0.5F})), 49) << "left voxel (value 255) not drawn";
  EXPECT_EQ(mask.count_in(around({1.0F, 0.0F, 0.5F})), 49) << "right voxel (value 255) not drawn";
  EXPECT_EQ(mask.count_in(around({0.0F, 0.0F, 0.5F})), 0) << "predicate-culled voxel still rasterized";
}

// A PosesInFrame triad renders where the camera projects the pose: every arm
// pixel lies within one arm length of the pose origin and the +X arm reaches
// its tip.
TEST_F(Scene3dRhiRenderTest, PoseTriadLandsWhereProjected) {
  RhiScene scene;
  const glm::vec3 origin{1.5F, 0.0F, 1.0F};
  constexpr float kArm = 1.0F;
  const PJ::ObjectTopicId topic = scene.addPose(origin, kArm);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  GRAB_OR_SKIP(with_pose, view, "pose");
  EXPECT_EQ(view->lastPoseLayerCountForTest(), 1);
  EXPECT_EQ(view->lastPoseArmCountForTest(), 3);
  scene.dock()->setLayerVisible(topic, false);
  GRAB_OR_SKIP(without_pose, view, "no_pose");

  const DiffMask mask = diffMask(with_pose, without_pose);
  ASSERT_GT(mask.count, 100) << "the pose triad coloured no pixels";
  const QPointF centre = project(*view, origin, with_pose.size());
  const QPointF x_tip = project(*view, origin + glm::vec3(0.9F * kArm, 0.0F, 0.0F), with_pose.size());
  const double reach = 1.3 * std::hypot(x_tip.x() - centre.x(), x_tip.y() - centre.y());
  int outside = 0;
  for (const QPoint& p : mask.pixels) {
    outside += std::hypot(p.x() - centre.x(), p.y() - centre.y()) > reach ? 1 : 0;
  }
  EXPECT_EQ(outside, 0) << "triad pixels far from the projected pose origin";
  EXPECT_GT(mask.count_in(QRect(int(x_tip.x()) - 4, int(x_tip.y()) - 4, 9, 9)), 0) << "+X arm tip not drawn";
}

// The TF frame triads share the pose pipeline and arrow mesh: with a TF tree
// present, turning the axes on draws the fixed frame's triad at its origin.
TEST_F(Scene3dRhiRenderTest, TfFrameTriadDrawsAtFrameOrigin) {
  RhiScene scene;
  scene.addTf("sensor", {2.0F, 0.0F, 1.0F});
  scene.addPoints(pointBlock({-2.5F, 2.0F, 0.5F}, 0.2F, 2));
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  constexpr float kArm = 1.0F;
  view->setGizmoSize(kArm);
  GRAB_OR_SKIP(without_axes, view, "no_axes");
  view->setAxesVisible(true);
  GRAB_OR_SKIP(with_axes, view, "axes");
  EXPECT_GE(view->lastResolvedFrameCountForTest(), 1);

  const DiffMask mask = diffMask(with_axes, without_axes);
  ASSERT_GT(mask.count, 100) << "the TF triad coloured no pixels";
  const QPointF x_tip = project(*view, {0.9F * kArm, 0.0F, 0.0F}, with_axes.size());
  EXPECT_GT(mask.count_in(QRect(int(x_tip.x()) - 4, int(x_tip.y()) - 4, 9, 9)), 0) << "+X arm tip not drawn";
}

// ---- GridMap, trails, frame context menu ------------------------------------

// A lit heightfield lands where its centre projects and draws the GL pass's
// procedural vertex count (6 per quad).
TEST_F(Scene3dRhiRenderTest, GridMapDrawsWhereProjected) {
  RhiScene scene;
  constexpr int kCells = 8;
  const PJ::ObjectTopicId topic = scene.addGridMap({-1.0F, -1.0F}, 0.25F, kCells, 0.3F);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  auto* layer = dynamic_cast<pj::scene3d::GridMapLayer*>(scene.dock()->layerFor(topic));
  ASSERT_NE(layer, nullptr);
  layer->setTrackerTime(PJ::fromRaw(100));  // what the dock's tracker fan-out does
  GRAB_OR_SKIP(with_map, view, "grid_map");
  EXPECT_EQ(view->lastGridMapLayerCountForTest(), 1);
  EXPECT_EQ(view->lastGridMapVertexCountForTest(), 6 * (kCells - 1) * (kCells - 1));
  EXPECT_GE(view->gridMapUploadCountForTest(), 1);
  EXPECT_TRUE(layer->statusWarning().isEmpty()) << layer->statusWarning().toStdString();
  scene.dock()->setLayerVisible(topic, false);
  GRAB_OR_SKIP(without_map, view, "no_grid_map");
  EXPECT_EQ(view->lastGridMapLayerCountForTest(), 0);

  const DiffMask mask = diffMask(with_map, without_map);
  ASSERT_GT(mask.count, 1000) << "grid map drew nothing";
  // Cell centres span [-0.875, 0.875]; the elevation rises slightly along +x.
  const QPointF expected = project(*view, {0.0F, 0.0F, 0.37F}, with_map.size());
  EXPECT_NEAR(mask.centroid.x(), expected.x(), with_map.width() * 0.05);
  EXPECT_NEAR(mask.centroid.y(), expected.y(), with_map.height() * 0.05);
  const QPointF far_corner = project(*view, {0.875F, 0.875F, 0.44F}, with_map.size());
  EXPECT_GT(mask.count_in(QRect(far_corner.toPoint() - QPoint(12, -2), QSize(10, 10))), 0)
      << "the far-right corner is missing";
}

// Five samples on a straight line at z = 1: the ribbon spans their projection,
// and moving the tracker splits it into past + future halves over one buffer.
void checkTrailDraws(bool embedded) {
  RhiScene scene;
  std::vector<glm::vec3> positions;
  for (int i = 0; i < 5; ++i) {
    positions.emplace_back(-2.0F + static_cast<float>(i), 0.0F, 1.0F);
  }
  const PJ::ObjectTopicId poses = scene.addPoseTrack(positions);
  auto* pose_layer = dynamic_cast<pj::scene3d::PosesInFrameLayer*>(scene.dock()->layerFor(poses));
  ASSERT_NE(pose_layer, nullptr);
  std::optional<PJ::ObjectTopicId> standalone;
  if (embedded) {
    pose_layer->setTrailEnabled(true);
    ASSERT_NE(pose_layer->trail(), nullptr);
  } else {
    scene.dock()->setLayerVisible(poses, false);
    standalone = scene.dock()->addTrailLayer(pj::scene3d::TrailSource::poseTopic(poses));
    ASSERT_NE(standalone->id, 0U);
  }
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  // The first frame adopts the fixed frame and rebuilds; the next one draws.
  GRAB_OR_SKIP(adopt, view, "adopt");
  GRAB_OR_SKIP(future_only, view, "future_only");
  EXPECT_EQ(view->lastTrailCountForTest(), 1);
  EXPECT_EQ(view->lastTrailVertexCountForTest(), 2 * static_cast<int>(positions.size()))
      << "before the first sample the whole ribbon is the future half";

  view->setTrackerTime(PJ::fromRaw(300));
  GRAB_OR_SKIP(split, view, "split");
  // Past: points 0..2; future: 2..4 (the shared pair keeps the ribbon continuous).
  EXPECT_EQ(view->lastTrailVertexCountForTest(), 2 * 3 + 2 * 3);
  EXPECT_GT(diffMask(future_only, split, 20).count, 50) << "the past half kept the future colour";

  if (embedded) {
    pose_layer->setTrailEnabled(false);
  } else {
    scene.dock()->setLayerVisible(*standalone, false);
  }
  GRAB_OR_SKIP(without, view, "no_trail");
  EXPECT_EQ(view->lastTrailCountForTest(), 0);
  const DiffMask mask = diffMask(split, without, 20);
  ASSERT_GT(mask.count, 300) << "trail drew nothing";
  const QSize size = split.size();
  const QPointF left = project(*view, positions.front(), size);
  const QPointF right = project(*view, positions.back(), size);
  EXPECT_NEAR(mask.centroid.x(), (left.x() + right.x()) / 2.0, size.width() * 0.05);
  EXPECT_NEAR(mask.centroid.y(), (left.y() + right.y()) / 2.0, size.height() * 0.05);
  for (const QPointF end : {left, right}) {
    EXPECT_GT(mask.count_in(QRect(end.toPoint() - QPoint(6, 6), QSize(12, 12))), 0)
        << "ribbon end missing at " << end.x() << "," << end.y();
  }
}

TEST_F(Scene3dRhiRenderTest, StandaloneTrailDrawsAndSplits) {
  checkTrailDraws(/*embedded=*/false);
}

TEST_F(Scene3dRhiRenderTest, PoseEmbeddedTrailDrawsAndSplits) {
  checkTrailDraws(/*embedded=*/true);
}

// setSampleCount() / setColorBufferFormat() after the first frame recreate the
// widget's render target on the same QRhi: every pipeline drawing into it must be
// rebuilt for the new one, and the scene must still render.
TEST_F(Scene3dRhiRenderTest, OutputTargetChangeRebuildsPipelines) {
  RhiScene scene;
  const PJ::ObjectTopicId topic = scene.addPoints(pointBlock({0.0F, 0.0F, 0.5F}, 0.8F, 8));
  scene.frame();
  SceneViewWidget* view = scene.view();
  GRAB_OR_SKIP(first, view, "first");
  EXPECT_TRUE(view->pipelinesBuiltForTargetForTest());

  const auto check_still_renders = [&](const char* name) {
    GRAB_OR_SKIP(with_points, view, name);
    EXPECT_TRUE(view->pipelinesBuiltForTargetForTest()) << name << ": a pipeline still targets the old render pass";
    scene.dock()->setLayerVisible(topic, false);
    GRAB_OR_SKIP(without_points, view, name);
    scene.dock()->setLayerVisible(topic, true);
    EXPECT_GT(diffMask(with_points, without_points).count, 200) << name << ": the point block vanished";
  };
  view->setSampleCount(view->sampleCount() > 1 ? 1 : 4);
  check_still_renders("samples");
  view->setColorBufferFormat(QRhiWidget::TextureFormat::RGBA16F);
  check_still_renders("rgba16f");
}

// Right-click on a TF frame: the view picks it and emits the request, the dock
// answers with a non-blocking popup; elsewhere the event is left to the dock.
TEST_F(Scene3dRhiRenderTest, FrameContextMenuPicksFrame) {
  RhiScene scene;
  scene.addRobot(kOffsetRobotUrdf);
  scene.frame();
  SceneViewWidget* view = scene.view();
  view->setGridVisible(false);
  view->setAxesVisible(true);
  view->setGizmoSize(1.0F);
  GRAB_OR_SKIP(plain, view, "plain");

  QStringList requested;
  QObject::connect(
      view, &SceneViewWidget::frameContextMenuRequested, view,
      [&requested](const QString& frame, const QPoint&) { requested << frame; });
  const QPointF origin = project(*view, {0.0F, 0.0F, 0.0F}, view->size());
  const auto right_click = [view](QPointF logical) {
    QContextMenuEvent event(QContextMenuEvent::Mouse, logical.toPoint(), view->mapToGlobal(logical.toPoint()));
    // QObject::event, not sendEvent: an ignored event must not reach the host
    // dock's standard (modal) menu in this test.
    static_cast<QObject*>(view)->event(&event);
    return event.isAccepted();
  };

  EXPECT_FALSE(right_click(origin + QPointF(60.0, 0.0))) << "empty space must fall through";
  EXPECT_TRUE(requested.isEmpty());

  ASSERT_TRUE(right_click(origin));
  ASSERT_EQ(requested.size(), 1);
  EXPECT_TRUE(requested.front() == u"base_link"_s || requested.front() == u"tool"_s) << requested.front().toStdString();
  // The call above already returned, so the menu cannot be modal.
  auto* menu = scene.dock()->findChild<QMenu*>(u"scene3dFrameMenu"_s);
  ASSERT_NE(menu, nullptr);
  EXPECT_TRUE(menu->isVisible());
  ASSERT_EQ(menu->actions().size(), 3);
  EXPECT_TRUE(menu->actions().front()->text().contains(requested.front()));
  menu->close();
}

}  // namespace
