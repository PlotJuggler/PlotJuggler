// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL evidence for the two things the cube shape's hexagon-fan geometry can get
// wrong, neither of which any CPU-side test can see:
//
//  1. FACE CHOICE IS PER CUBE. The fan draws only the three faces pointing at the
//     camera, chosen from each instance's own centre. Getting that wrong for a whole
//     draw (e.g. by deriving the camera reference from a matrix that no longer
//     carries it) leaves cubes on one side of the view axis showing an interior face,
//     which reads as a missing sliver. Two cubes placed symmetrically about the axis
//     must therefore cover the SAME number of pixels.
//  2. PER-FACE SHADING SURVIVES. The face normal is recovered in the fragment shader
//     from the interpolated corner instead of an attribute, so a cube must still show
//     three distinctly lit faces rather than one flat silhouette.
//
// Skips cleanly when no usable GL context is available, or below GL 4.5 (the scene's
// #version 450 shaders won't compile) — mirroring pointcloud_sphere_size_gl_test.

#include <gtest/gtest.h>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using namespace pj::scene3d;

constexpr int kW = 256;
constexpr int kH = 256;
// Two 1 m cubes, one each side of the view axis, at the same depth so their
// projections are exact mirror images of each other.
constexpr float kCubeSide = 1.0f;
constexpr float kOffsetX = 2.0f;
constexpr float kCameraDistance = 10.0f;

// Both cubes in frame "map", straddling the camera axis.
std::shared_ptr<const DecodedPointCloud> twoCubes() {
  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions = {glm::vec3(-kOffsetX, 0.0f, 0.0f), glm::vec3(kOffsetX, 0.0f, 0.0f)};
  return cloud;
}

class PointcloudCubeFacesGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(fmt);
    surface_->create();
    if (!surface_->isValid()) {
      GTEST_SKIP() << "no usable offscreen surface (headless without GL)";
    }
    ctx_ = std::make_unique<QOpenGLContext>();
    ctx_->setFormat(fmt);
    if (!ctx_->create() || !ctx_->makeCurrent(surface_.get())) {
      GTEST_SKIP() << "could not create/make-current an OpenGL context";
    }
    const auto* version = reinterpret_cast<const char*>(ctx_->functions()->glGetString(GL_VERSION));
    const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
    if (std::pair<int, int>(parts.value(0).toInt(), parts.value(1).toInt()) < std::pair<int, int>(4, 5)) {
      GTEST_SKIP() << "GL " << (version != nullptr ? version : "?") << " below 4.5 — can't compile scene shaders";
    }

    QOpenGLFramebufferObjectFormat fbo_fmt;
    fbo_fmt.setAttachment(QOpenGLFramebufferObject::Depth);
    fbo_ = std::make_unique<QOpenGLFramebufferObject>(kW, kH, fbo_fmt);
    ASSERT_TRUE(fbo_->bind());

    auto* f = ctx_->functions();
    f->glViewport(0, 0, kW, kH);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glEnable(GL_DEPTH_TEST);
  }

  void TearDown() override {
    fbo_.reset();
    if (ctx_ != nullptr) {
      ctx_->doneCurrent();
    }
  }

  // Renders both cubes in solid white — so the only thing varying across the image is
  // the per-face Lambert term — and returns the frame.
  QImage renderCubes(const glm::mat4& proj, const glm::vec3& eye) {
    ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    PointcloudRenderPass pass;
    pass.setShape(PointcloudRenderPass::Shape::kCube);
    pass.setColorType(PointcloudRenderPass::ColorType::kSolid);
    pass.setSolidColor(glm::vec3(1.0f, 1.0f, 1.0f));
    pass.setSizeMeters(kCubeSide);
    pass.initializeGL();
    pass.setActiveCloud(twoCubes());

    // Aimed at the origin, midway between the cubes, so each lands in its own half of
    // the image. The eye is lifted off the axis so all three chosen faces have area.
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ViewParams vp;
    vp.view = view;
    vp.proj = proj;
    vp.viewport_width_px = kW;
    vp.viewport_height_px = kH;
    vp.device_width_px = kW;
    vp.device_height_px = kH;

    const TransformBuffer tf(TransformBuffer::kKeepAll);
    const std::string fixed = "map";  // same as the cloud frame → identity model
    const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
    pass.render(vp, fc);
    ctx_->functions()->glFlush();

    const QImage image = fbo_->toImage();
    EXPECT_FALSE(image.isNull());
    return image;
  }

  static glm::mat4 perspective() {
    return glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
  }
  static glm::mat4 orthographic() {
    return glm::ortho(-4.0f, 4.0f, -4.0f, 4.0f, 0.1f, 100.0f);
  }
  // Perspective: straight down the x = 0 plane, so the two cubes sit on OPPOSITE
  // sides of the eye in x and must pick different faces there.
  static glm::vec3 perspectiveEye() {
    return {0.0f, -kCameraDistance, 4.0f};
  }
  // Ortho: off-axis in x too. A parallel projection gives every cube the same three
  // faces however it is placed, so the check is that both still project identically —
  // and the skew keeps all three faces off edge-on, unlike an axis-aligned view.
  static glm::vec3 orthographicEye() {
    return {3.0f, -kCameraDistance, 4.0f};
  }

  // Lit pixels in the given horizontal half of the image.
  static int litPixels(const QImage& image, bool left_half) {
    int lit = 0;
    const int x_begin = left_half ? 0 : kW / 2;
    const int x_end = left_half ? kW / 2 : kW;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = x_begin; x < x_end; ++x) {
        if (image.pixelColor(x, y).red() > 8) {
          ++lit;
        }
      }
    }
    return lit;
  }

  // Distinct grey levels covering a meaningful area — one per visible face. Small
  // populations (antialiased silhouette, the darkened cube-edge band) are ignored.
  static int distinctShades(const QImage& image, bool left_half) {
    std::map<int, int> histogram;
    const int x_begin = left_half ? 0 : kW / 2;
    const int x_end = left_half ? kW / 2 : kW;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = x_begin; x < x_end; ++x) {
        const int red = image.pixelColor(x, y).red();
        if (red > 8) {
          ++histogram[red / 8];  // 8-level buckets: face shades differ by far more
        }
      }
    }
    int shades = 0;
    for (const auto& entry : histogram) {
      if (entry.second >= 20) {
        ++shades;
      }
    }
    return shades;
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

// The crux. The two cubes are mirror images about the view axis, so whatever faces
// each shows, they must cover the same area. Choosing the visible faces once for the
// whole draw instead of per instance breaks exactly this: the cube on one side loses
// a face and shrinks.
TEST_F(PointcloudCubeFacesGlTest, MirroredCubesCoverEqualAreaUnderPerspective) {
  const QImage image = renderCubes(perspective(), perspectiveEye());
  const int left = litPixels(image, /*left_half=*/true);
  const int right = litPixels(image, /*left_half=*/false);

  ASSERT_GT(left, 100) << "left cube did not render";
  ASSERT_GT(right, 100) << "right cube did not render";
  EXPECT_NEAR(static_cast<double>(left), static_cast<double>(right), 0.05 * left)
      << "mirrored cubes differ in size — the visible-face choice is not per instance";
}

// The same must hold with no perspective divide, where the visible faces depend on the
// view DIRECTION rather than on the eye position.
TEST_F(PointcloudCubeFacesGlTest, MirroredCubesCoverEqualAreaUnderOrtho) {
  const QImage image = renderCubes(orthographic(), orthographicEye());
  const int left = litPixels(image, /*left_half=*/true);
  const int right = litPixels(image, /*left_half=*/false);

  ASSERT_GT(left, 100) << "left cube did not render under ortho";
  ASSERT_GT(right, 100) << "right cube did not render under ortho";
  EXPECT_NEAR(static_cast<double>(left), static_cast<double>(right), 0.05 * left)
      << "mirrored cubes differ in size under ortho";
}

// Guard on the fragment-derived face normal: a cube must read as a solid, not a flat
// square. Three faces at three angles to the fixed key light give three grey levels.
TEST_F(PointcloudCubeFacesGlTest, EachCubeShowsThreeDistinctlyLitFaces) {
  const QImage image = renderCubes(perspective(), perspectiveEye());
  EXPECT_GE(distinctShades(image, /*left_half=*/true), 3) << "left cube looks flat — per-face shading lost";
  EXPECT_GE(distinctShades(image, /*left_half=*/false), 3) << "right cube looks flat — per-face shading lost";
}

}  // namespace
