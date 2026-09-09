// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL evidence that the VoxelGrid GPU-instanced-cube path works: renders the
// pass onto an offscreen FBO with a real OpenGL context and reads back pixels.
// Proves two invariants on actual hardware/software GL:
//   1. A drawn voxel (predicate passes) rasterizes lit fragments.
//   2. A culled voxel (predicate fails) rasterizes NOTHING — the vertex-shader
//      degenerate-clip cull is what keeps a moderately-sparse grid cheap.
//
// Skips cleanly when no usable GL context is available, or when the context is
// below GL 4.5 (the scene's #version 450 shaders won't compile) — mirroring
// hud_overlay_gl_test. Linux CI under xvfb + llvmpipe reports 4.5 and runs.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/passes/voxel_grid_render_pass.h"
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

class VoxelGridRenderPassGlTest : public ::testing::Test {
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
    // The DRIVER version (not the requested format, which can lie) must be >= 4.5
    // for the #version 450 shaders to compile.
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

  // A top-down ortho camera framing the unit cube centred at (0.5, 0.5, 0.5).
  static ViewParams cameraFramingUnitCell() {
    ViewParams vp;
    vp.view = glm::lookAt(glm::vec3(0.5f, 0.5f, 5.0f), glm::vec3(0.5f, 0.5f, 0.5f), glm::vec3(0.0f, 1.0f, 0.0f));
    vp.proj = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 20.0f);
    vp.viewport_width_px = kW;
    vp.viewport_height_px = kH;
    vp.device_width_px = kW;
    vp.device_height_px = kH;
    return vp;
  }

  // A 1x1x1 grid (one scalar voxel) carrying `value`, in frame "map".
  static VoxelGridUpload oneVoxel(float value) {
    VoxelGridUpload up;
    up.frame_id = "map";
    up.cell_size = glm::vec3(1.0f);
    up.column_count = 1;
    up.row_count = 1;
    up.slice_count = 1;
    up.kind = VoxelValueKind::kScalar;
    up.scalar = {value};
    return up;
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

TEST_F(VoxelGridRenderPassGlTest, DrawnVoxelRasterizesFragments) {
  ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  VoxelGridRenderPass pass;
  pass.setColormap(PJ::Colormap::kGrayscale);
  pass.setDrawMode(VoxelDrawMode::kAll);
  pass.setAutoRange(false);
  pass.setManualRange(0.0f, 1.0f);
  pass.initializeGL();
  pass.setGrid(oneVoxel(1.0f));  // bright voxel

  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "map";  // same as grid frame → identity model
  const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
  pass.render(cameraFramingUnitCell(), fc);
  ctx_->functions()->glFlush();

  const QImage img = fbo_->toImage();
  ASSERT_FALSE(img.isNull());
  EXPECT_GT(litPixels(img), 200) << "a drawn voxel produced no fragments";
}

// The whole-grid frustum reject, new with the shared chooseCubeDrawMode(): this pass had
// no such gate and drew every lattice cell regardless of where the grid was.
//
// Only ONE direction of it is worth a render test. "An off-screen grid draws nothing"
// passes whether or not the reject exists — off-screen geometry rasterizes nothing either
// way — so that assertion cannot fail and is not written here; the skip itself is a
// performance property, covered by cube_draw_policy_test without a context.
//
// The direction that CAN go wrong is over-culling, which makes a grid silently vanish.
// A voxel whose CENTRE sits outside the view volume while its cell still overlaps must
// survive: the bounds handed to the policy describe centres, so this is the case that
// catches an extent computed half a cell wrong. Verified to fail when the inflation is
// removed.
TEST_F(VoxelGridRenderPassGlTest, GridOverhangingTheViewEdgeStillDraws) {
  ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  VoxelGridRenderPass pass;
  pass.setColormap(PJ::Colormap::kGrayscale);
  pass.setDrawMode(VoxelDrawMode::kAll);
  pass.setAutoRange(false);
  pass.setManualRange(0.0f, 1.0f);
  pass.initializeGL();
  pass.setGrid(oneVoxel(1.0f));

  // The ortho box spans +/-1 around the look-at point. Aim it 1.4 cells to the side so the
  // single voxel's centre (0.5, 0.5, 0.5) is outside, while the cell it occupies is not.
  ViewParams vp = cameraFramingUnitCell();
  vp.view = glm::lookAt(glm::vec3(1.9f, 0.5f, 5.0f), glm::vec3(1.9f, 0.5f, 0.5f), glm::vec3(0.0f, 1.0f, 0.0f));

  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "map";
  const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
  pass.render(vp, fc);
  ctx_->functions()->glFlush();

  EXPECT_GT(litPixels(fbo_->toImage()), 0)
      << "the voxel overhangs the view edge but nothing drew — the reject is testing voxel "
         "CENTRES instead of the cells they occupy";
}

TEST_F(VoxelGridRenderPassGlTest, CulledVoxelRasterizesNothing) {
  ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  VoxelGridRenderPass pass;
  pass.setColormap(PJ::Colormap::kGrayscale);
  pass.setDrawMode(VoxelDrawMode::kNonZero);  // value 0 → culled
  pass.setAutoRange(false);
  pass.setManualRange(0.0f, 1.0f);
  pass.initializeGL();
  pass.setGrid(oneVoxel(0.0f));  // zero → predicate fails → no draw

  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "map";
  const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
  pass.render(cameraFramingUnitCell(), fc);
  ctx_->functions()->glFlush();

  const QImage img = fbo_->toImage();
  ASSERT_FALSE(img.isNull());
  EXPECT_LT(litPixels(img), 10) << "a predicate-culled voxel still rasterized fragments";
}

}  // namespace
