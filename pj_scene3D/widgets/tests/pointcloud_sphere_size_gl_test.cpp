// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL evidence that the sphere-imposter point size is correct under an
// ORTHOGRAPHIC projection. The sphere-imposter vertex shader scales gl_PointSize
// by 1/depth to fake perspective foreshortening — correct for a perspective
// camera, WRONG for an orthographic one, where world->pixel scale is constant and
// does not depend on the camera-axis distance.
//
// These render a single sphere-shaped point onto an offscreen FBO and read back
// pixel coverage:
//   1. Under ortho, the sphere covers the SAME number of pixels regardless of how
//      far the camera sits along its view axis (the bug shrinks distant spheres to
//      ~1 px), and that coverage matches the analytic disc area.
//   2. Under perspective, a closer sphere is larger than a farther one — the
//      guard that the ortho fix must not flatten perspective foreshortening.
//
// Skips cleanly when no usable GL context is available, or below GL 4.5 (the
// scene's #version 450 shaders won't compile) — mirroring voxel_grid_render_pass_gl_test.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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

constexpr int kW = 128;
constexpr int kH = 128;

int litPixels(const QImage& img) {
  int n = 0;
  for (int y = 0; y < img.height(); ++y) {
    for (int x = 0; x < img.width(); ++x) {
      const QColor c = img.pixelColor(x, y);
      if (c.red() + c.green() + c.blue() > 60) {  // anything well above the black clear
        ++n;
      }
    }
  }
  return n;
}

// A single sphere-shaped white point at the world origin, in frame "map".
std::shared_ptr<const DecodedPointCloud> originPoint() {
  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions = {glm::vec3(0.0f, 0.0f, 0.0f)};
  return cloud;
}

class PointcloudSphereSizeGlTest : public ::testing::Test {
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

  // ViewParams with the given view/proj, sized to the FBO.
  static ViewParams viewParams(const glm::mat4& view, const glm::mat4& proj) {
    ViewParams vp;
    vp.view = view;
    vp.proj = proj;
    vp.viewport_width_px = kW;
    vp.viewport_height_px = kH;
    vp.device_width_px = kW;
    vp.device_height_px = kH;
    return vp;
  }

  // Render one sphere-shaped point at the origin from the given camera and return
  // its lit-pixel coverage.
  int renderSphereCoverage(const glm::mat4& view, const glm::mat4& proj) {
    ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    PointcloudRenderPass pass;
    pass.setShape(PointcloudRenderPass::Shape::kSphere);
    pass.setColorType(PointcloudRenderPass::ColorType::kSolid);
    pass.setSolidColor(glm::vec3(1.0f, 1.0f, 1.0f));
    pass.setSizeMeters(0.5f);  // radius 0.25 m
    pass.initializeGL();
    pass.setActiveCloud(originPoint());

    const TransformBuffer tf(TransformBuffer::kKeepAll);
    const std::string fixed = "map";  // same as cloud frame → identity model
    const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
    pass.render(viewParams(view, proj), fc);
    ctx_->functions()->glFlush();

    const QImage img = fbo_->toImage();
    EXPECT_FALSE(img.isNull());
    return litPixels(img);
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

// The crux: under an orthographic projection the sphere's screen size is fixed by
// the world radius and the view extent — NOT by the camera's distance along its
// view axis. A camera 80 m up and a camera 5 m up that frame the same world extent
// must draw the same sphere. The buggy 1/depth scaling collapses the distant one to
// ~1 px.
TEST_F(PointcloudSphereSizeGlTest, SphereSizeIndependentOfDepthUnderOrtho) {
  // glm::ortho(-2,2,-2,2): proj[1][1] = 0.5; 4 world units span 128 px (32 px/unit).
  // radius 0.25 m -> diameter 16 px -> disc area ~= pi*8^2 ~= 201 px.
  const glm::mat4 proj = glm::ortho(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 300.0f);
  const glm::mat4 view_near = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 view_far = glm::lookAt(glm::vec3(0.0f, 0.0f, 80.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

  const int near_cov = renderSphereCoverage(view_near, proj);
  const int far_cov = renderSphereCoverage(view_far, proj);

  // Both render the full disc (~201 px); allow rasterization slack.
  EXPECT_GT(near_cov, 150) << "near sphere under-covered";
  EXPECT_GT(far_cov, 150) << "far ortho sphere shrank — 1/depth scaling leaked into ortho";
  // And they are the SAME size: distance along the view axis must not matter.
  EXPECT_NEAR(static_cast<double>(near_cov), static_cast<double>(far_cov), 0.15 * near_cov)
      << "ortho sphere size varied with camera distance";
}

// Guard: the ortho fix must not flatten perspective foreshortening. Under a
// perspective projection a closer sphere is strictly larger than a farther one.
TEST_F(PointcloudSphereSizeGlTest, SphereSizeShrinksWithDepthUnderPerspective) {
  const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 300.0f);
  const glm::mat4 view_near = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 view_far = glm::lookAt(glm::vec3(0.0f, 0.0f, 12.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

  const int near_cov = renderSphereCoverage(view_near, proj);
  const int far_cov = renderSphereCoverage(view_far, proj);

  EXPECT_GT(near_cov, 0) << "near perspective sphere produced no fragments";
  EXPECT_GT(near_cov, far_cov) << "perspective foreshortening lost — near sphere not larger than far";
}

}  // namespace
