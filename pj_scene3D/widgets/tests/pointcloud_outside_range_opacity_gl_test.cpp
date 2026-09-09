// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL evidence for the "Outside range" opacity + eye controls. Points whose
// colour scalar falls outside the manual [range_min, range_max] are, by default,
// clamped to the colormap end and drawn OPAQUE. The opacity scrubber fades them
// (alpha blend); at opacity 0 — or with the eye toggled off — they are culled
// entirely. Points inside the range are unaffected either way.
//
// Renders a single kField point at the origin onto an offscreen FBO and reads
// back both its lit-pixel coverage and its centre brightness. A grayscale
// colormap makes an in-range / clamped point render bright white, so a faded
// point's centre brightness scales with the applied opacity.
//
// Skips cleanly when no usable GL context is available, or below GL 4.5 (the
// scene's #version 450 shaders won't compile) — mirroring pointcloud_sphere_size_gl_test.

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

// Brightness (sum of channels) at the framebuffer centre, where the origin point
// projects — a proxy for the point's applied opacity over the black clear.
int centreBrightness(const QImage& img) {
  const QColor c = img.pixelColor(img.width() / 2, img.height() / 2);
  return c.red() + c.green() + c.blue();
}

struct Readback {
  int lit = 0;
  int centre = 0;
};

// A single flat point at the world origin (frame "map") carrying colour scalar `s`.
std::shared_ptr<const DecodedPointCloud> scalarPoint(float s) {
  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions = {glm::vec3(0.0f, 0.0f, 0.0f)};
  cloud->scalar = {s};
  return cloud;
}

// A near OUTSIDE-range point drawn BEFORE a far IN-range point (VBO order). Both
// project to the screen centre; the camera sits at +5z so z=0 is nearer than z=-1.
// This is the order that trips the depth-write occlusion bug if faded points write
// depth: the near faded point would clip the crisp point behind it.
std::shared_ptr<const DecodedPointCloud> nearFadedOverFarInRange() {
  auto cloud = std::make_shared<DecodedPointCloud>();
  cloud->frame_id = "map";
  cloud->positions = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
  cloud->scalar = {5.0f, 0.6f};  // near = outside [0,1]; far = in-range mid-grey
  return cloud;
}

class PointcloudOutsideRangeOpacityGlTest : public ::testing::Test {
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

  // Render a grayscale field-coloured cloud with the given manual colormap range and
  // effective outside-range alpha (0 hides / 1 opaque); read back coverage + centre.
  Readback renderCloud(
      const std::shared_ptr<const DecodedPointCloud>& cloud, float range_min, float range_max, float outside_alpha) {
    ctx_->functions()->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    PointcloudRenderPass pass;
    pass.setShape(PointcloudRenderPass::Shape::kPoint);  // flat + unlit -> pure colormap colour
    pass.setSizePixels(12.0f);
    pass.setColorType(PointcloudRenderPass::ColorType::kField);
    pass.setColormap(PointcloudRenderPass::Colormap::kGrayscale);  // t=1 -> white, unambiguous
    pass.setColormapRange(range_min, range_max);
    pass.setOutsideRangeAlpha(outside_alpha);
    pass.initializeGL();
    pass.setActiveCloud(cloud);

    // Ortho camera at (0,0,5) framing the origin: origin-plane points project to centre.
    const glm::mat4 proj = glm::ortho(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 300.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    const TransformBuffer tf(TransformBuffer::kKeepAll);
    const std::string fixed = "map";  // same as cloud frame -> identity model
    const FrameContext fc{tf, fixed, PJ::fromRaw(0)};
    pass.render(viewParams(view, proj), fc);
    ctx_->functions()->glFlush();

    const QImage img = fbo_->toImage();
    EXPECT_FALSE(img.isNull());
    return {litPixels(img), centreBrightness(img)};
  }

  // Single point at the origin with the given scalar; folds the eye + opacity into
  // the effective alpha the same way the layer does.
  Readback render(float scalar, float range_min, float range_max, float outside_opacity, bool outside_visible) {
    return renderCloud(scalarPoint(scalar), range_min, range_max, outside_visible ? outside_opacity : 0.0f);
  }

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> ctx_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

// An out-of-range point (scalar 5, range [0,1]) at opacity 1 draws opaque; at
// opacity 0 it is culled; at opacity 0.5 it renders at roughly half brightness.
TEST_F(PointcloudOutsideRangeOpacityGlTest, OpacityFadesThenCullsOutOfRangePoints) {
  const Readback opaque = render(5.0f, 0.0f, 1.0f, /*opacity=*/1.0f, /*visible=*/true);
  const Readback culled = render(5.0f, 0.0f, 1.0f, /*opacity=*/0.0f, /*visible=*/true);
  const Readback faded = render(5.0f, 0.0f, 1.0f, /*opacity=*/0.5f, /*visible=*/true);

  EXPECT_GT(opaque.lit, 0) << "opacity 1: out-of-range point must render opaque";
  EXPECT_EQ(culled.lit, 0) << "opacity 0: out-of-range point must be culled";
  EXPECT_GT(faded.lit, 0) << "opacity 0.5: out-of-range point must still render (blended)";
  // Half opacity over a black clear -> roughly half the opaque centre brightness.
  EXPECT_LT(faded.centre, opaque.centre * 3 / 4) << "opacity 0.5 not visibly dimmer than opaque";
  EXPECT_GT(faded.centre, opaque.centre / 4) << "opacity 0.5 unexpectedly near-invisible";
}

// The eye toggle (visible=false) hides out-of-range points regardless of the
// opacity value.
TEST_F(PointcloudOutsideRangeOpacityGlTest, EyeOffHidesOutOfRangePoints) {
  const Readback hidden = render(5.0f, 0.0f, 1.0f, /*opacity=*/1.0f, /*visible=*/false);
  EXPECT_EQ(hidden.lit, 0) << "eye off: out-of-range point must be hidden even at opacity 1";
}

// Two-pass depth split: a faded outside-range point in FRONT must not occlude the
// crisp in-range point behind it — the in-range point shows through the fade, so the
// blended centre is brighter than the faded point alone. Regression guard for the
// depth-write bug (faded points must draw with depth writes off, in a second pass).
TEST_F(PointcloudOutsideRangeOpacityGlTest, FadedOutsidePointDoesNotOccludeInRangeBehind) {
  const Readback both = renderCloud(nearFadedOverFarInRange(), 0.0f, 1.0f, /*outside_alpha=*/0.5f);
  const Readback only_faded = renderCloud(scalarPoint(5.0f), 0.0f, 1.0f, /*outside_alpha=*/0.5f);

  EXPECT_GT(both.centre, only_faded.centre + 20)
      << "faded outside point occluded the in-range point behind it (depth-write bug)";
}

// A point inside the range is unaffected: outside-range opacity 0 / eye off must
// not touch it.
TEST_F(PointcloudOutsideRangeOpacityGlTest, InRangePointUnaffected) {
  const Readback opacity_zero = render(0.9f, 0.0f, 1.0f, /*opacity=*/0.0f, /*visible=*/true);
  const Readback eye_off = render(0.9f, 0.0f, 1.0f, /*opacity=*/1.0f, /*visible=*/false);
  EXPECT_GT(opacity_zero.lit, 0) << "in-range point must render regardless of outside-range opacity";
  EXPECT_GT(eye_off.lit, 0) << "in-range point must render regardless of the outside-range eye";
}

}  // namespace
