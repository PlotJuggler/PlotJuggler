// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Headless render harness for the QRhi Scene3D renderer (PJ_SCENE3D_RHI).
//
// QRhiWidget::grabFramebuffer() renders a frame synchronously on a QPA with
// RHI-based rendering (xcb, wayland); the `offscreen` QPA has none, so this
// runner never defaults to it. CI runs it under Xvfb with Mesa llvmpipe
// (OpenGL) and lavapipe (Vulkan); PJ_GRAPHICS_API picks the backend and
// PJ_REQUIRE_GPU_TESTS=1 turns a missing one into a failure.
//
// Scenes are grabbed from a shown top-level through grabPresentedFrame(); a
// never-shown widget is good for one grab only (Qt 6.11.1, see that method).
//
// Checks are image DIFFS (same scene with one element toggled) compared against
// the camera's own projection, so they hold regardless of theme colours and
// tonemapping but catch Y/X flips, broken depth conventions and blend ordering.
// PJ_RHI_TEST_DUMP_DIR=<dir> saves every grabbed image as a PNG.

#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <cmath>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
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

// Backend the run asked for, as QRhi::backendName() spells it.
QString requestedBackend() {
  return qEnvironmentVariable("PJ_GRAPHICS_API").compare(u"vulkan"_s, Qt::CaseInsensitive) == 0 ? u"Vulkan"_s
                                                                                                : u"OpenGL"_s;
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

// test::grab() plus the optional PNG dump. Null when no frame was presented.
QImage grab(SceneViewWidget* view, const char* name) {
  QImage image = pj::scene3d::test::grab(*view);
  dump(image, name);
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
    // Through the persisted state: the layer's setter is private to its UI.
    auto* layer = dynamic_cast<pj::scene3d::SceneEntitiesLayer*>(dock()->layerFor(id));
    EXPECT_NE(layer, nullptr);
    if (layer != nullptr) {
      QDomDocument doc;
      QDomElement state = layer->xmlSaveState(doc);
      state.setAttribute(u"opacity"_s, u"1"_s);
      EXPECT_TRUE(layer->xmlLoadState(state));
    }
    return id;
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
// over the opaque cube behind it. Layers draw in layer order with no
// opaque-first sort (as in the GL renderer), so the glass layer comes last.
TEST_F(Scene3dRhiRenderTest, OcclusionAndTranslucency) {
  RhiScene scene;
  const glm::vec3 near_red{-1.5F, -1.0F, 0.5F};
  const glm::vec3 far_blue{-1.5F, 2.5F, 1.0F};
  const glm::vec3 glass{2.0F, -1.0F, 0.6F};
  const glm::vec3 behind_glass{2.0F, 1.5F, 0.9F};
  scene.addEntities(
      {
          cubeEntity("b_near_red", near_red, 1.0F, {255, 30, 30, 255}),
          cubeEntity("c_behind_glass", behind_glass, 1.6F, {255, 30, 30, 255}),
          cubeEntity("z_far_blue", far_blue, 2.5F, {30, 30, 255, 255}),
      },
      "/opaque");
  // Any translucent cube turns its whole layer's cubes translucent, so the glass
  // is its own layer.
  scene.addEntities({cubeEntity("glass", glass, 1.0F, {40, 255, 40, 90})}, "/glass");
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

}  // namespace
