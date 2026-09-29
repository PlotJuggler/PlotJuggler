// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// QRhi point-pass behaviour the golden corpus does not pin, rendered through the
// real Scene3DDockWidget on every backend:
//
//  - the hexagon fan picks each cube's faces from its own centre (mirrored cubes
//    cover equal areas) and keeps three distinctly lit faces;
//  - the whole-cloud cube LOD: sub-pixel clouds become sprites in the cloud's own
//    colour, near clouds stay fans;
//  - sphere size follows 1/depth under perspective only;
//  - faded out-of-range points neither vanish nor occlude in-range points behind
//    them (the translucent half draws without depth writes), and opacity 0 culls;
//  - signed 8-bit extrema keep their sign;
//  - quads crossing the frustum edge keep their visible part, and a cube whose
//    centre is off screen still draws what pokes in.
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QDomDocument>
#include <QOffscreenSurface>
#include <QScopeGuard>
#include <QTest>
#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../rhi/passes/point_rhi_pass.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_widgets/GraphicsApi.h"
#include "scene3d_render_test_support.h"
// Qt declares the Vulkan QRhi types only where the Vulkan headers exist (not in a
// Windows build without the Vulkan SDK), whatever QT_CONFIG(vulkan) says.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define PJ_TEST_HAS_VULKAN 1
#include <QVulkanInstance>
#else
#define PJ_TEST_HAS_VULKAN 0
#endif
#include <limits>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::channelDeltaSum;
using pj::scene3d::test::ExtraField;
using Attrs = std::map<QString, QString>;
using DT = PJ::sdk::PointField::Datatype;

constexpr QSize kViewSize{320, 240};

// One dock with point-cloud layers configured through their persisted state.
class PointScene {
 public:
  PJ::ObjectTopicId add(
      const std::vector<glm::vec3>& points, const Attrs& attrs, const std::vector<ExtraField>& fields = {}) {
    const auto id = fixture_.add(
        "/cloud" + std::to_string(next_topic_++), PJ::sdk::BuiltinObjectType::kPointCloud,
        pj::scene3d::test::serializeCloud(points, "map", fields));
    PJ::ISceneLayer* layer = fixture_.dock()->layerFor(id);
    EXPECT_NE(layer, nullptr);
    if (layer != nullptr) {
      QDomDocument doc;
      QDomElement payload = doc.createElement(u"pointcloud"_s);
      for (const auto& [key, value] : attrs) {
        payload.setAttribute(key, value);
      }
      EXPECT_TRUE(layer->xmlLoadState(payload));
    }
    return id;
  }

  // Shows the window, presents a warm-up frame, then pins `camera`.
  SceneViewWidget* frame(const pj::scene3d::CameraState& camera, bool ortho = false) {
    PJ::Scene3DDockWidget* dock = fixture_.dock();
    for (int i = 0; i < 20 && dock->sceneView() == nullptr; ++i) {
      QCoreApplication::processEvents();
    }
    SceneViewWidget* view = dock->sceneView();
    EXPECT_NE(view, nullptr);
    if (view == nullptr) {
      return nullptr;
    }
    view->setFixedSize(kViewSize);
    fixture_.window().show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&fixture_.window()));
    view->setGridVisible(false);
    view->setAxesVisible(false);
    view->setTfConnectionsVisible(false);
    dock->setFixedFrame(u"map"_s);
    dock->onTrackerTime(0.0);
    for (int i = 0; i < 3; ++i) {
      pj::scene3d::test::grab(*view);
    }
    if (ortho) {
      view->setCameraModel(SceneViewWidget::CameraModel::kTopDownOrtho);
    }
    view->camera().adoptState(camera);
    return view;
  }

  PJ::Scene3DDockWidget& dock() {
    return *fixture_.dock();
  }

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  int next_topic_ = 0;
};

pj::scene3d::CameraState frontCamera(float radius, float elevation = 0.35F) {
  pj::scene3d::CameraState camera;
  camera.focal = {0.0F, 0.0F, 0.0F};
  camera.radius = radius;
  camera.azimuth = glm::radians(-90.0F);  // eye on -Y looking +Y: +X is screen-right
  camera.elevation = elevation;
  camera.fov_y = glm::radians(45.0F);
  return camera;
}

Attrs solid(const QString& shape, const QString& size_key, const QString& size) {
  return {
      {u"shape"_s, shape},
      {u"color_type"_s, u"solid"_s},
      {u"color_choice_explicit"_s, u"true"_s},
      {u"solid_color"_s, u"#2060d0"_s},
      {size_key, size}};
}

// A stable frame; PJ_RHI_TEST_DUMP_DIR=<dir> also saves it as <test>_<n>.png.
QImage grab(SceneViewWidget& view) {
  QImage image = pj::scene3d::test::grabStable(view);
  const QString dir = qEnvironmentVariable("PJ_RHI_TEST_DUMP_DIR");
  if (!dir.isEmpty() && !image.isNull()) {
    static int counter = 0;
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    image.save(u"%1/%2_%3_%4.png"_s.arg(dir, qEnvironmentVariable("PJ_GRAPHICS_API"), QString::fromLatin1(test->name()))
                   .arg(counter++));
  }
  return image;
}

// Pixels that differ from the empty scene.
std::vector<QPoint> covered(const QImage& image, const QImage& empty, int threshold = 30) {
  std::vector<QPoint> out;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (channelDeltaSum(image.pixel(x, y), empty.pixel(x, y)) > threshold) {
        out.emplace_back(x, y);
      }
    }
  }
  return out;
}

QColor meanOf(const QImage& image, const std::vector<QPoint>& pixels) {
  long r = 0;
  long g = 0;
  long b = 0;
  for (const QPoint& p : pixels) {
    const QRgb px = image.pixel(p);
    r += qRed(px);
    g += qGreen(px);
    b += qBlue(px);
  }
  const long n = std::max<long>(1, static_cast<long>(pixels.size()));
  return QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n));
}

// The scene with every layer hidden: the backdrop to diff against.
QImage emptyScene(PointScene& scene, SceneViewWidget& view) {
  for (const auto& info : scene.dock().layers()) {
    scene.dock().setLayerVisible(info.topic_id, false);
  }
  QImage empty = grab(view);
  for (const auto& info : scene.dock().layers()) {
    scene.dock().setLayerVisible(info.topic_id, true);
  }
  return empty;
}

class Scene3dRhiPointTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

#define VIEW_OR_SKIP(view, expression)                       \
  SceneViewWidget* view = (expression);                      \
  ASSERT_NE(view, nullptr);                                  \
  if (pj::scene3d::test::grab(*view).isNull()) {             \
    PJ_SKIP_WITHOUT_GPU("the QRhi view presented no frame"); \
  }

// Two cubes mirrored about the view axis must cover the same area (per-instance
// face choice), and each shows three distinctly lit faces (fragment-derived normal).
TEST_F(Scene3dRhiPointTest, FanPicksFacesPerCubeAndShadesThree) {
  PointScene scene;
  scene.add({{-2.2F, 0.0F, 0.0F}, {2.2F, 0.0F, 0.0F}}, solid(u"cube"_s, u"size_meters"_s, u"1"_s));
  VIEW_OR_SKIP(view, scene.frame(frontCamera(5.0F, 0.6F)));
  const QImage image = grab(*view);
  EXPECT_EQ(view->lastCubeSpriteLayerCountForTest(), 0) << "1 m cubes must draw as fans";
  const QImage empty = emptyScene(scene, *view);
  const auto pixels = covered(image, empty);
  const int mid = image.width() / 2;
  std::vector<QPoint> left;
  std::vector<QPoint> right;
  for (const QPoint& p : pixels) {
    (p.x() < mid ? left : right).push_back(p);
  }
  ASSERT_GT(left.size(), 500U);
  EXPECT_NEAR(
      static_cast<double>(left.size()), static_cast<double>(right.size()), 0.02 * static_cast<double>(left.size()))
      << "mirrored cubes cover different areas: faces chosen per draw, not per cube";
  // Face shades: every face is flat-lit, so three faces are three exact colours,
  // each covering a good share of the cube (edges and silhouettes are the rest).
  std::map<QRgb, int> shades;
  for (const QPoint& p : left) {
    ++shades[image.pixel(p)];
  }
  int distinct = 0;
  for (const auto& [shade, count] : shades) {
    distinct += count > static_cast<int>(left.size()) / 20 ? 1 : 0;
  }
  EXPECT_GE(distinct, 3) << "a cube reads as fewer than three lit faces";
}

// Sub-pixel cubes: the whole cloud becomes sprites, drawn in the cloud's colour
// (not dimmed towards black) and covering about one pixel per cube; a near cloud
// of the same cubes keeps its fans.
TEST_F(Scene3dRhiPointTest, SubPixelCubeCloudBecomesColouredSprites) {
  PointScene scene;
  std::vector<glm::vec3> lattice;
  for (int j = 0; j < 20; ++j) {
    for (int i = 0; i < 20; ++i) {
      lattice.emplace_back(0.25F * (static_cast<float>(i) - 9.5F), 0.0F, 0.25F * (static_cast<float>(j) - 9.5F));
    }
  }
  scene.add(lattice, solid(u"cube"_s, u"size_meters"_s, u"0.004"_s));
  VIEW_OR_SKIP(view, scene.frame(frontCamera(14.0F, 0.0F)));
  const QImage far_image = grab(*view);
  EXPECT_EQ(view->lastCubeSpriteLayerCountForTest(), 1) << "a sub-pixel cloud must draw as LOD sprites";
  // Small sprites use the point list where the backend has QRhi::VertexShaderPointSize
  // (every backend but Direct3D), and stay quads elsewhere.
  const auto api = PJ::preferredGraphicsApi();
  const bool point_size = api != QRhiWidget::Api::Direct3D11 && api != QRhiWidget::Api::Direct3D12;
  EXPECT_EQ(view->lastPointListLayerCountForTest(), point_size ? 1 : 0);
  const QImage empty = emptyScene(scene, *view);
  auto pixels = covered(far_image, empty);
  EXPECT_GT(pixels.size(), lattice.size() * 3 / 4) << "sprites lost: fewer lit pixels than cubes";
  EXPECT_LT(pixels.size(), lattice.size() * 3) << "sprites much larger than a pixel";
  // Sub-pixel sprites share their pixels with the backdrop after the MSAA resolve;
  // the most covered tenth still shows the cloud's blue, not a dimmed grey.
  std::sort(pixels.begin(), pixels.end(), [&](QPoint a, QPoint b) {
    return qGray(far_image.pixel(a)) < qGray(far_image.pixel(b));
  });
  pixels.resize(pixels.size() / 10);
  const QColor mean = meanOf(far_image, pixels);
  EXPECT_GT(mean.blue(), mean.red() + 60)
      << "sprites lost the cloud colour: rgb " << mean.red() << "," << mean.green() << "," << mean.blue();

  view->camera().adoptState(frontCamera(0.9F, 0.0F));
  grab(*view);
  EXPECT_EQ(view->lastCubeSpriteLayerCountForTest(), 0) << "a near cloud must keep its fans";
}

// A GPU that cannot build the point-list pipeline draws the same small sprites
// as quads (the point list is optional): no failure state, no missing frame.
TEST_F(Scene3dRhiPointTest, PointListFailureFallsBackToQuads) {
  qputenv("PJ_SCENE3D_TEST_FAIL_SHADER", "/point_list.frag.qsb");
  const auto clear_fault = qScopeGuard([] { qunsetenv("PJ_SCENE3D_TEST_FAIL_SHADER"); });
  PointScene scene;
  int failures = 0;
  std::vector<glm::vec3> lattice;
  for (int j = 0; j < 20; ++j) {
    for (int i = 0; i < 20; ++i) {
      lattice.emplace_back(0.25F * (static_cast<float>(i) - 9.5F), 0.0F, 0.25F * (static_cast<float>(j) - 9.5F));
    }
  }
  scene.add(lattice, solid(u"cube"_s, u"size_meters"_s, u"0.004"_s));
  VIEW_OR_SKIP(view, scene.frame(frontCamera(14.0F, 0.0F)));
  QObject::connect(view, &QRhiWidget::renderFailed, view, [&failures] { ++failures; });
  const QImage image = grab(*view);
  EXPECT_EQ(view->lastCubeSpriteLayerCountForTest(), 1);
  EXPECT_EQ(view->lastPointListLayerCountForTest(), 0) << "a failed point list still drew";
  EXPECT_GT(view->lastPointVertexCountForTest() + view->lastCubeInstanceCountForTest(), 0);
  const QImage empty = emptyScene(scene, *view);
  EXPECT_GT(covered(image, empty).size(), lattice.size() * 3 / 4) << "the quads did not draw the sprites";
  EXPECT_EQ(failures, 0);
  EXPECT_TRUE(view->renderingWarningForTest().isEmpty()) << view->renderingWarningForTest().toStdString();
}

// Perspective: a sphere twice as far covers less. Orthographic: spheres at any
// depth have the same size.
TEST_F(Scene3dRhiPointTest, SphereSizeFollowsDepthOnlyUnderPerspective) {
  {
    PointScene scene;
    scene.add({{-1.0F, 0.0F, 0.0F}, {1.0F, 4.0F, 0.0F}}, solid(u"sphere"_s, u"size_meters"_s, u"0.3"_s));
    VIEW_OR_SKIP(view, scene.frame(frontCamera(4.0F, 0.0F)));
    const QImage image = grab(*view);
    const auto pixels = covered(image, emptyScene(scene, *view));
    int near_pixels = 0;
    for (const QPoint& p : pixels) {
      near_pixels += p.x() < image.width() / 2 ? 1 : 0;
    }
    const int far_pixels = static_cast<int>(pixels.size()) - near_pixels;
    ASSERT_GT(far_pixels, 20);
    EXPECT_GT(near_pixels, 2 * far_pixels) << "perspective spheres do not shrink with depth";
  }
  {
    PointScene scene;
    scene.add({{-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, -3.0F}}, solid(u"sphere"_s, u"size_meters"_s, u"0.3"_s));
    pj::scene3d::CameraState top;
    top.focal = {0.0F, 0.0F, 0.0F};
    top.ortho_scale = 2.0F;
    top.perspective = false;
    VIEW_OR_SKIP(view, scene.frame(top, /*ortho=*/true));
    const QImage image = grab(*view);
    const auto pixels = covered(image, emptyScene(scene, *view));
    int left = 0;
    for (const QPoint& p : pixels) {
      left += p.x() < image.width() / 2 ? 1 : 0;
    }
    const int right = static_cast<int>(pixels.size()) - left;
    ASSERT_GT(left, 50);
    EXPECT_NEAR(left, right, 0.1 * left) << "orthographic sphere size depends on depth";
  }
}

// The faded half draws after every opaque draw and writes no depth: an in-range
// point behind a faded one shows through it, and a faded point behind another
// faded one still blends in (with depth writes it would be depth-rejected).
// Opacity 0 culls the out-of-range points.
TEST_F(Scene3dRhiPointTest, FadedOutOfRangePointsNeitherOccludeNorVanish) {
  const auto scalar = [](std::vector<float> values) {
    return ExtraField{"intensity", DT::kFloat32, [values](std::size_t i, uint8_t* dst) {
                        std::memcpy(dst, &values[i], sizeof(float));
                      }};
  };
  const Attrs faded = {{u"shape"_s, u"point"_s},           {u"size_pixels"_s, u"24"_s},
                       {u"color_type"_s, u"field"_s},      {u"color_choice_explicit"_s, u"true"_s},
                       {u"color_field"_s, u"intensity"_s}, {u"colormap"_s, u"grayscale"_s},
                       {u"auto_range"_s, u"false"_s},      {u"range_min"_s, u"0"_s},
                       {u"range_max"_s, u"1"_s},           {u"outside_range_opacity"_s, u"0.5"_s}};
  // Points on the view axis, nearest first in the buffer. Out-of-range values clamp
  // to the colormap ends: 5 -> white, -5 -> black; 0 is in range (black).
  const auto centre_gray = [&](const std::vector<glm::vec3>& points, std::vector<float> values,
                               const QString& opacity) {
    PointScene scene;
    Attrs attrs = faded;
    attrs[u"outside_range_opacity"_s] = opacity;
    scene.add(points, attrs, {scalar(std::move(values))});
    SceneViewWidget* view = scene.frame(frontCamera(5.0F, 0.0F));
    if (view == nullptr) {
      return -1;
    }
    const QImage image = grab(*view);
    return image.isNull() ? -1 : qGray(image.pixel(image.width() / 2, image.height() / 2));
  };
  const glm::vec3 near_point{0.0F, -1.0F, 0.0F};
  const glm::vec3 far_point{0.0F, 1.0F, 0.0F};
  const int near_white_alone = centre_gray({near_point}, {5.0F}, u"0.5"_s);
  if (near_white_alone < 0) {
    PJ_SKIP_WITHOUT_GPU("the QRhi view presented no frame");
  }
  const int over_in_range_black = centre_gray({near_point, far_point}, {5.0F, 0.0F}, u"0.5"_s);
  const int over_faded_black = centre_gray({near_point, far_point}, {5.0F, -5.0F}, u"0.5"_s);
  const int culled = centre_gray({near_point, far_point}, {5.0F, 0.0F}, u"0"_s);
  // On the light backdrop the faded white point alone is invisible (255).
  EXPECT_LT(over_in_range_black, near_white_alone - 20)
      << "the in-range black point does not show through the faded one";
  EXPECT_LT(over_faded_black, near_white_alone - 40)
      << "the far faded point was depth-rejected by the near one: faded draws wrote depth";
  EXPECT_LT(culled, 20) << "opacity 0 must cull the out-of-range point, leaving the black one";
}

// int8 extrema through the colormap: -128 is the darkest, 127 the brightest.
TEST_F(Scene3dRhiPointTest, SignedByteExtremaKeepTheirSign) {
  const std::vector<int8_t> values{-128, 127, 0};
  const ExtraField field{
      "intensity", DT::kInt8, [values](std::size_t i, uint8_t* dst) { std::memcpy(dst, &values[i], 1); }};
  const Attrs attrs = {{u"shape"_s, u"point"_s},           {u"size_pixels"_s, u"16"_s},
                       {u"color_type"_s, u"field"_s},      {u"color_choice_explicit"_s, u"true"_s},
                       {u"color_field"_s, u"intensity"_s}, {u"colormap"_s, u"grayscale"_s},
                       {u"auto_range"_s, u"true"_s}};
  PointScene scene;
  scene.add({{-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}}, attrs, {field});
  VIEW_OR_SKIP(view, scene.frame(frontCamera(5.0F, 0.0F)));
  const QImage image = grab(*view);
  const auto pixel_at = [&](float x) {
    const glm::mat4 clip_from_world =
        view->camera().projMatrix(float(image.width()) / float(image.height())) * view->camera().viewMatrix();
    const glm::vec4 clip = clip_from_world * glm::vec4(x, 0.0F, 0.0F, 1.0F);
    return qGray(image.pixel(
        int((clip.x / clip.w * 0.5F + 0.5F) * float(image.width())),
        int((0.5F - clip.y / clip.w * 0.5F) * float(image.height()))));
  };
  const int minimum = pixel_at(-1.0F);
  const int middle = pixel_at(0.0F);
  const int maximum = pixel_at(1.0F);
  EXPECT_LT(minimum, 30) << "-128 is not the range minimum";
  EXPECT_GT(maximum, 225) << "127 is not the range maximum";
  EXPECT_GT(middle, minimum + 60);
  EXPECT_LT(middle, maximum - 60);
}

// A point whose centre is just past the left edge still shows its visible half
// (quads clip per pixel; a point sprite would vanish with its centre), a point
// behind the camera draws nothing, and a 1 m cube centred off screen still draws
// the part inside it.
TEST_F(Scene3dRhiPointTest, GeometryCrossingTheFrustumEdgeKeepsItsVisiblePart) {
  PointScene scene;
  const auto camera = frontCamera(5.0F, 0.0F);
  // Horizontal half-extent of the view at the focal depth (5 m, 45 degree fovy).
  const float aspect = float(kViewSize.width()) / float(kViewSize.height());
  const float half_width = 5.0F * std::tan(glm::radians(22.5F)) * aspect;
  scene.add({{-half_width - 0.05F, 0.0F, 0.0F}, {0.0F, -6.0F, 0.0F}}, solid(u"point"_s, u"size_pixels"_s, u"30"_s));
  scene.add({{half_width + 0.3F, 0.0F, 0.0F}}, solid(u"cube"_s, u"size_meters"_s, u"1"_s));
  VIEW_OR_SKIP(view, scene.frame(camera));
  const QImage image = grab(*view);
  EXPECT_EQ(view->lastPointListLayerCountForTest(), 0) << "30 px points must draw as quads";
  const QImage empty = emptyScene(scene, *view);
  int left_edge = 0;
  int right_edge = 0;
  int elsewhere = 0;
  for (const QPoint& p : covered(image, empty)) {
    if (p.x() < 20) {
      ++left_edge;
    } else if (p.x() >= image.width() - 40) {
      ++right_edge;
    } else {
      ++elsewhere;
    }
  }
  EXPECT_GT(left_edge, 60) << "the point straddling the left edge vanished with its centre";
  EXPECT_GT(right_edge, 300) << "the cube straddling the right edge was culled";
  EXPECT_EQ(elsewhere, 0) << "the point behind the camera drew something";
}

// The same point alone: no point elsewhere widens the cloud's bounds into the
// view, so the whole-cloud frustum reject alone decides. It must allow for the
// sprite's screen size, not just the centre.
TEST_F(Scene3dRhiPointTest, PixelSizedPointJustOffScreenStillDraws) {
  PointScene scene;
  const float aspect = float(kViewSize.width()) / float(kViewSize.height());
  const float half_width = 5.0F * std::tan(glm::radians(22.5F)) * aspect;
  scene.add({{-half_width - 0.05F, 0.0F, 0.0F}}, solid(u"point"_s, u"size_pixels"_s, u"30"_s));
  VIEW_OR_SKIP(view, scene.frame(frontCamera(5.0F, 0.0F)));
  const QImage image = grab(*view);
  const QImage empty = emptyScene(scene, *view);
  int left_edge = 0;
  for (const QPoint& p : covered(image, empty)) {
    left_edge += p.x() < 20 ? 1 : 0;
  }
  EXPECT_GT(left_edge, 60) << "the cloud was rejected by its centre while its quad overlaps the view";
}

// A faded (translucent, no depth write) 1 m cube seen with the eye inside its X
// and Z slabs: the fan still picks the +X and +Z faces, which face away from the
// eye. Culled, only the -Y face shows and mirrored points of it match; blended in,
// the +X face darkens the right side of it.
TEST_F(Scene3dRhiPointTest, FadedCubeInsideAnAxisSlabShowsNoBackFaces) {
  const float value = -5.0F;  // below the range: black at the faded opacity
  const ExtraField field{
      "intensity", DT::kFloat32, [value](std::size_t, uint8_t* dst) { std::memcpy(dst, &value, sizeof(float)); }};
  const Attrs faded = {
      {u"shape"_s, u"cube"_s},
      {u"size_meters"_s, u"1"_s},
      {u"color_type"_s, u"field"_s},
      {u"color_choice_explicit"_s, u"true"_s},
      {u"color_field"_s, u"intensity"_s},
      {u"colormap"_s, u"grayscale"_s},
      {u"auto_range"_s, u"false"_s},
      {u"range_min"_s, u"0"_s},
      {u"range_max"_s, u"1"_s},
      {u"outside_range_opacity"_s, u"0.5"_s}};
  PointScene scene;
  scene.add({{0.0F, 0.0F, 0.0F}}, faded, {field});
  VIEW_OR_SKIP(view, scene.frame(frontCamera(3.0F, 0.0F)));
  const QImage image = grab(*view);
  // Points of the -Y face (y = -0.5) at x = +-0.42: the +X face projects over the
  // outer part of the right half only.
  const glm::mat4 clip_from_world =
      view->camera().projMatrix(float(image.width()) / float(image.height())) * view->camera().viewMatrix();
  const auto gray_at = [&](float x) {
    const glm::vec4 clip = clip_from_world * glm::vec4(x, -0.5F, 0.0F, 1.0F);
    return qGray(image.pixel(
        int((clip.x / clip.w * 0.5F + 0.5F) * float(image.width())),
        int((0.5F - clip.y / clip.w * 0.5F) * float(image.height()))));
  };
  const int left = gray_at(-0.42F);
  const int right = gray_at(0.42F);
  EXPECT_LT(left, 200) << "the faded cube is missing";
  EXPECT_NEAR(left, right, 3) << "a back face blended into the faded cube";
}

// The eye inside an opaque cube: every face is seen from behind, so nothing of
// the cube may draw (its interior is not a surface).
TEST_F(Scene3dRhiPointTest, CameraInsideACubeDrawsNoInterior) {
  PointScene scene;
  // A 4 m cube centred 1 m to the left: its +X and +Z faces are in view from
  // the eye at (0, -1.5, 0).
  scene.add({{-1.0F, 0.0F, 0.0F}}, solid(u"cube"_s, u"size_meters"_s, u"4"_s));
  VIEW_OR_SKIP(view, scene.frame(frontCamera(1.5F, 0.0F)));
  const QImage image = grab(*view);
  const QImage empty = emptyScene(scene, *view);
  EXPECT_EQ(covered(image, empty).size(), 0U) << "the cube's interior faces drew";
}

// ---- The pass against a PointRenderable of its own ----------------------------

// A minimal cloud layer: its bounds are the finite extent of its vertices, as a
// layer's conversion computes them.
class ScannedCloud : public pj::scene3d::PointRenderable {
 public:
  void set(const std::vector<pj::scene3d::PointVertex>& vertices) {
    bytes_ = QByteArray(
        reinterpret_cast<const char*>(vertices.data()),
        static_cast<qsizetype>(vertices.size() * sizeof(pj::scene3d::PointVertex)));
    bounds_ = {};
    for (const pj::scene3d::PointVertex& vertex : vertices) {
      if (std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z)) {
        pj::scene3d::expandAABB(bounds_, glm::vec3(vertex.x, vertex.y, vertex.z));
      }
    }
    ++revision_;
  }
  bool prepareForRender(std::uint64_t /*remaining*/) override {
    return true;
  }
  [[nodiscard]] const QByteArray& vertexBytes() const override {
    return bytes_;
  }
  [[nodiscard]] pj::scene3d::PointVertexFormat vertexFormat() const override {
    return pj::scene3d::PointVertexFormat::kXyzScalar;
  }
  [[nodiscard]] std::size_t pointCount() const override {
    return static_cast<std::size_t>(bytes_.size()) / sizeof(pj::scene3d::PointVertex);  // unpadded
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return frame_;
  }
  [[nodiscard]] std::optional<pj::scene3d::AABB> sourceBounds() const override {
    return bounds_.valid ? std::optional(bounds_) : std::nullopt;
  }
  [[nodiscard]] bool visible() const override {
    return true;
  }
  [[nodiscard]] pj::scene3d::PointShape shape() const override {
    return pj::scene3d::PointShape::kPoint;
  }
  [[nodiscard]] pj::scene3d::PointColorType colorType() const override {
    return pj::scene3d::PointColorType::kSolid;
  }
  [[nodiscard]] float sizeMeters() const override {
    return 0.01F;
  }
  [[nodiscard]] float sizePixels() const override {
    return 2.0F;
  }
  [[nodiscard]] QColor solidColor() const override {
    return Qt::white;
  }
  [[nodiscard]] PJ::Colormap colormap() const override {
    return PJ::Colormap::kTurbo;
  }
  [[nodiscard]] bool invertLut() const override {
    return false;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return 1.0F;
  }
  [[nodiscard]] int scalarAxis() const override {
    return -1;
  }
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& /*fixed_from_source*/) const override {
    return {0.0F, 1.0F};
  }
  void noteRenderedScalarRange(float /*minimum*/, float /*maximum*/) override {}
  [[nodiscard]] bool isDepthCloud() const override {
    return false;
  }

 private:
  QByteArray bytes_;
  pj::scene3d::AABB bounds_;
  std::uint64_t revision_ = 0;
  std::string frame_ = "map";
};

// An offscreen QRhi on the backend the widgets use (PJ_GRAPHICS_API, else the
// platform default), with a small target the point pass can build its pipelines
// against.
class OffscreenRhi {
 public:
  OffscreenRhi() {
    // The same adapter preference the widgets' backing store applies (Qt Quick's variable).
    const QRhi::Flags flags = qEnvironmentVariableIntValue("QSG_RHI_PREFER_SOFTWARE_RENDERER") != 0
                                  ? QRhi::Flags(QRhi::PreferSoftwareRenderer)
                                  : QRhi::Flags();
    switch (PJ::preferredGraphicsApi()) {
      case QRhiWidget::Api::Vulkan: {
#if PJ_TEST_HAS_VULKAN
        vulkan_.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (vulkan_.create()) {
          QRhiVulkanInitParams params;
          params.inst = &vulkan_;
          rhi_.reset(QRhi::create(QRhi::Vulkan, &params, flags));
        }
#endif
        break;
      }
      case QRhiWidget::Api::Direct3D11: {
#ifdef Q_OS_WIN
        QRhiD3D11InitParams params;
        rhi_.reset(QRhi::create(QRhi::D3D11, &params, flags));
#endif
        break;
      }
      case QRhiWidget::Api::Metal: {
#if QT_CONFIG(metal)
        QRhiMetalInitParams params;
        rhi_.reset(QRhi::create(QRhi::Metal, &params, flags));
#endif
        break;
      }
      case QRhiWidget::Api::OpenGL: {
        surface_.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = surface_.get();
        rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params, flags));
        break;
      }
      default:
        break;  // no QRhi: the test skips
    }
    if (rhi_ == nullptr) {
      return;
    }
    color_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(64, 64), 1, QRhiTexture::RenderTarget));
    depth_.reset(rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(64, 64)));
    render_mode_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    color_->create();
    depth_->create();
    render_mode_->create();
    QRhiTextureRenderTargetDescription description{QRhiColorAttachment(color_.get())};
    description.setDepthStencilBuffer(depth_.get());
    target_.reset(rhi_->newTextureRenderTarget(description));
    pass_descriptor_.reset(target_->newCompatibleRenderPassDescriptor());
    target_->setRenderPassDescriptor(pass_descriptor_.get());
    target_->create();
  }
  QRhi* rhi() {
    return rhi_.get();
  }
  pj::scene3d::RhiPassSetup setup() {
    return {rhi_.get(), pass_descriptor_.get(), 1, render_mode_.get()};
  }
  // One frame of the view's contract: admit, prepare, then an empty pass that
  // consumes the uploads.
  void frame(pj::scene3d::PointRhiPass& pass) {
    QRhiCommandBuffer* command_buffer = nullptr;
    ASSERT_EQ(rhi_->beginOffscreenFrame(&command_buffer), QRhi::FrameOpSuccess);
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    pass.admit([](const std::string&) { return std::optional<pj::scene3d::Transform>(pj::scene3d::Transform{}); });
    pass.prepare({rhi_.get(), updates, glm::dvec3(0.0), QMatrix4x4()}, {glm::mat4(1.0F), QSize(64, 64), 1.0F});
    command_buffer->beginPass(target_.get(), Qt::black, {1.0F, 0}, updates);
    command_buffer->endPass();
    rhi_->endOffscreenFrame();
  }

 private:
  // Declared before the QRhi, which uses them until it is destroyed.
  std::unique_ptr<QOffscreenSurface> surface_;
#if PJ_TEST_HAS_VULKAN
  QVulkanInstance vulkan_;
#endif
  std::unique_ptr<QRhi> rhi_;
  std::unique_ptr<QRhiTexture> color_;
  std::unique_ptr<QRhiRenderBuffer> depth_;
  std::unique_ptr<QRhiBuffer> render_mode_;
  std::unique_ptr<QRhiTextureRenderTarget> target_;
  std::unique_ptr<QRhiRenderPassDescriptor> pass_descriptor_;
};

std::vector<pj::scene3d::PointVertex> box(glm::vec3 minimum, glm::vec3 maximum) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  return {
      {minimum.x, maximum.y, maximum.z, 0U},
      {maximum.x, minimum.y, minimum.z, 0U},
      {nan, 100.0F, 100.0F, 0U},  // non-finite: ignored
      {0.5F * (minimum.x + maximum.x), 0.5F * (minimum.y + maximum.y), 0.5F * (minimum.z + maximum.z), 0U}};
}

// The pass reports a layer's bounds in the frame its sample arrives, and a new
// sample replaces them at once. A released pass (QRhi teardown, e.g. a dock
// reparent) uploads the retained cloud again.
TEST_F(Scene3dRhiPointTest, CloudBoundsFollowEachSampleAndReuploadAfterRelease) {
  OffscreenRhi offscreen;
  if (offscreen.rhi() == nullptr) {
    PJ_SKIP_WITHOUT_GPU("no QRhi for " << qPrintable(qEnvironmentVariable("PJ_GRAPHICS_API")));
  }
  ScannedCloud cloud;
  pj::scene3d::PointRhiPass pass;
  ASSERT_TRUE(pass.initialize(offscreen.setup()));
  pass.setLayers({&cloud});

  using Box = std::pair<glm::vec3, glm::vec3>;
  const auto expect_frame = [&](const Box& box, const char* what) {
    offscreen.frame(pass);
    const pj::scene3d::AABB bounds = pass.lastPointBounds();
    EXPECT_TRUE(bounds.valid && bounds.min == box.first && bounds.max == box.second)
        << what << ": the bounds are not the layer's";
    // Counted once uploaded, before the frustum reject (this identity view culls the cloud).
    EXPECT_EQ(pass.lastPointVertexCount(), 4) << what << ": the retained cloud was not uploaded";
  };
  const Box first{{-1.0F, 3.0F, 5.0F}, {2.0F, 4.0F, 6.0F}};
  const Box second{{10.0F, 11.0F, 12.0F}, {13.0F, 14.0F, 15.0F}};
  cloud.set(box(first.first, first.second));
  expect_frame(first, "first sample");
  cloud.set(box(second.first, second.second));
  expect_frame(second, "second sample");

  pass.release();
  ASSERT_TRUE(pass.initialize(offscreen.setup()));
  pass.setLayers({&cloud});
  expect_frame(second, "after release()");
  pass.release();
}

}  // namespace
