// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL evidence for the procedural heightfield: the attributeless
// gl_VertexID draw rasterizes a flat 2x2 grid, and the `flat` validity drops
// exactly the triangles that touch a NaN corner (a shared corner kills both
// triangles of the quad, a private one only its own).
//
// Skips cleanly without a usable context or below GL 4.5 (the #version 450
// shaders), like voxel_grid_render_pass_gl_test.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gl_scene_test_support.h"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/passes/grid_map_render_pass.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using namespace pj::scene3d;

constexpr int kW = 128;
constexpr int kH = 128;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

int litPixels(const QImage& img) {
  int n = 0;
  for (int y = 0; y < img.height(); ++y) {
    for (int x = 0; x < img.width(); ++x) {
      const QColor c = img.pixelColor(x, y);
      if (c.red() + c.green() + c.blue() > 60) {
        ++n;
      }
    }
  }
  return n;
}

class GridMapRenderPassGlTest : public ::testing::Test {
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
    if (test::parseGlVersion(version) < std::pair<int, int>(4, 5)) {
      GTEST_SKIP() << "GL " << (version != nullptr ? version : "?") << " below 4.5 - can't compile scene shaders";
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

  // Top-down ortho camera over the 2x2 grid whose cell centres span [0.5, 1.5]^2.
  static ViewParams cameraOverGrid() {
    ViewParams vp;
    vp.view = glm::lookAt(glm::vec3(1.0f, 1.0f, 5.0f), glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    vp.proj = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 20.0f);
    vp.viewport_width_px = kW;
    vp.viewport_height_px = kH;
    vp.device_width_px = kW;
    vp.device_height_px = kH;
    return vp;
  }

  static GridMapUpload twoByTwo(std::vector<float> elevation) {
    GridMapUpload up;
    up.frame_id = "map";
    up.cell_size = glm::vec2(1.0f);
    up.column_count = 2;
    up.row_count = 2;
    up.elevation = std::move(elevation);
    up.color = {1.0f, 1.0f, 1.0f, 1.0f};
    up.color_range = FiniteRange{0.0f, 1.0f, true};
    return up;
  }

  struct Draw {
    GridMapUpload upload;
    float opacity = 1.0f;
  };

  // One clear, then each grid drawn by its own pass in order, unlit grayscale.
  int renderAndCount(std::vector<Draw> draws) {
    ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const TransformBuffer tf(TransformBuffer::kKeepAll);
    const std::string fixed = "map";
    const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
    for (Draw& draw : draws) {
      GridMapRenderPass pass;
      pass.setColormap(PJ::Colormap::kGrayscale);
      pass.setLighting(false);
      pass.setOpacity(draw.opacity);
      pass.initializeGL();
      pass.setGrid(std::move(draw.upload));
      pass.render(cameraOverGrid(), fc);
    }
    ctx_->functions()->glFlush();
    const QImage img = fbo_->toImage();
    EXPECT_FALSE(img.isNull());
    return litPixels(img);
  }
  int renderAndCount(GridMapUpload upload) {
    std::vector<Draw> draws;
    draws.push_back(Draw{std::move(upload)});
    return renderAndCount(std::move(draws));
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

TEST_F(GridMapRenderPassGlTest, FlatGridRasterizesAndNaNCornersDropTriangles) {
  // The quad covers a quarter of the 128x128 view: about 4096 pixels.
  const int full = renderAndCount(twoByTwo({}));
  EXPECT_GT(full, 3000) << "a flat grid produced no fragments";

  const int lifted = renderAndCount(twoByTwo({0.0f, 0.5f, 0.0f, 0.5f}));
  EXPECT_GT(lifted, 3000) << "an elevated grid produced no fragments";

  // Corner (1,1) belongs to both triangles: nothing survives.
  EXPECT_LT(renderAndCount(twoByTwo({0.0f, 0.0f, 0.0f, kNaN})), 10) << "a NaN shared corner still rasterized";

  // Corner (1,0) belongs only to the first triangle: roughly half survives.
  const int half = renderAndCount(twoByTwo({0.0f, kNaN, 0.0f, 0.0f}));
  EXPECT_GT(half, full / 4) << "a NaN private corner dropped the whole quad";
  EXPECT_LT(half, full * 3 / 4) << "a NaN private corner dropped nothing";
}

// A translucent grid must not write depth (the OccupancyGrid pass contract): a
// dark half-transparent grid at z=1 drawn first must not hide the opaque white
// plane at z=0 drawn after it. The depth mask is restored for later passes.
TEST_F(GridMapRenderPassGlTest, TranslucentGridDoesNotOccludeAndRestoresDepthMask) {
  GridMapUpload dark = twoByTwo({1.0f, 1.0f, 1.0f, 1.0f});
  dark.color = {0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<Draw> draws;
  draws.push_back(Draw{dark, 0.5f});
  draws.push_back(Draw{twoByTwo({}), 1.0f});
  EXPECT_GT(renderAndCount(std::move(draws)), 3000) << "a translucent grid occluded the surface behind it";
  GLboolean depth_mask = GL_FALSE;
  ctx_->functions()->glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
  EXPECT_EQ(depth_mask, GL_TRUE);
}

// A grid wider than GL_MAX_TEXTURE_SIZE is refused with a reason the layer can
// show, but its staged payload survives so a recreated context retries it.
TEST_F(GridMapRenderPassGlTest, OverMaxTextureSizeIsRejectedButKeptForRetry) {
  GLint max_size = 0;
  ctx_->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
  ASSERT_GT(max_size, 0);
  GridMapUpload up;
  up.frame_id = "map";
  up.column_count = static_cast<uint32_t>(max_size) + 1;
  up.row_count = 2;
  up.elevation.assign(static_cast<size_t>(up.column_count) * up.row_count, 0.0f);
  up.color_is_elevation = true;
  up.color_range = FiniteRange{0.0f, 1.0f, true};

  GridMapRenderPass pass;
  pass.initializeGL();
  pass.setGrid(std::move(up));
  EXPECT_FALSE(pass.rejection().has_value()) << "rejection is a GL-limit verdict, known only at upload";
  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "map";
  const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
  pass.render(cameraOverGrid(), fc);
  ASSERT_TRUE(pass.rejection().has_value());
  EXPECT_FALSE(pass.rejection()->isEmpty());

  pass.releaseGL();
  EXPECT_FALSE(pass.rejection().has_value()) << "a new context gets to retry";
  pass.initializeGL();
  pass.render(cameraOverGrid(), fc);
  EXPECT_TRUE(pass.rejection().has_value()) << "the staged payload must survive the rejection";
}

// Colour rule order: a non-finite sample maps to the LUT low end even when the
// range is degenerate (the midpoint rule is for finite values only), and a
// finite value above 3e38 is still finite.
TEST_F(GridMapRenderPassGlTest, ColorRuleTestsFinitenessFirstAndAcceptsLargeFinite) {
  GridMapUpload nan_color = twoByTwo({});
  nan_color.color = {kNaN, kNaN, kNaN, kNaN};
  nan_color.color_range = FiniteRange{};  // no finite sample: lo == hi
  EXPECT_LT(renderAndCount(nan_color), 10) << "NaN colour painted the LUT midpoint";

  GridMapUpload degenerate = twoByTwo({});
  degenerate.color_range = FiniteRange{1.0f, 1.0f, true};
  EXPECT_GT(renderAndCount(degenerate), 3000) << "a finite value in a degenerate range paints the midpoint";

  // A narrow range around the value: a span near FLT_MAX has a denormal
  // reciprocal, which Mesa's division flushes to zero (a driver limit, not ours).
  GridMapUpload large = twoByTwo({});
  large.color = {3.3e38f, 3.3e38f, 3.3e38f, 3.3e38f};
  large.color_range = FiniteRange{3.0e38f, 3.4e38f, true};
  EXPECT_GT(renderAndCount(large), 3000) << "a finite value above 3e38 was treated as a hole";
}

}  // namespace

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
