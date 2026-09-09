// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL test for TrailRenderPass on a raw offscreen 4.5-core context (the
// pass is layer-owned, so no SceneViewWidget is needed). Exercises the split
// draw at both boundaries and the release/re-init context-lifecycle contract.
// Skips when the driver grants less than GL 4.5 (the #version 450 shader won't
// compile) — same rule as the SceneViewWidget-based GL tests.

#include <gtest/gtest.h>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <glm/glm.hpp>
#include <vector>

#include "pj_scene3d_widgets/passes/trail_render_pass.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using pj::scene3d::TrailRenderPass;
using pj::scene3d::ViewParams;

class TrailRenderPassGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_.setFormat(QSurfaceFormat::defaultFormat());
    if (!context_.create()) {
      GTEST_SKIP() << "no GL context available";
    }
    surface_.setFormat(context_.format());
    surface_.create();
    if (!surface_.isValid() || !context_.makeCurrent(&surface_)) {
      GTEST_SKIP() << "offscreen surface unusable";
    }
    const auto* version = reinterpret_cast<const char*>(context_.functions()->glGetString(GL_VERSION));
    const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
    if (parts.value(0).toInt() < 4 || (parts.value(0).toInt() == 4 && parts.value(1).toInt() < 5)) {
      GTEST_SKIP() << "driver GL " << (version != nullptr ? version : "?") << " < 4.5";
    }
  }

  void TearDown() override {
    if (context_.isValid()) {
      context_.doneCurrent();
    }
  }

  [[nodiscard]] GLenum glError() {
    return context_.functions()->glGetError();
  }

  QOpenGLContext context_;
  QOffscreenSurface surface_;
};

TEST_F(TrailRenderPassGlTest, SplitDrawAndContextRecreationSurvival) {
  // An offscreen surface has no drawable default framebuffer — draws against it
  // raise GL_INVALID_FRAMEBUFFER_OPERATION. Render into an explicit FBO instead.
  QOpenGLFramebufferObject fbo(QSize(64, 64));
  ASSERT_TRUE(fbo.bind());

  TrailRenderPass pass;
  pass.initializeGL();

  std::vector<glm::dvec3> points;
  points.reserve(10);
  for (int i = 0; i < 10; ++i) {
    points.push_back(glm::dvec3(static_cast<double>(i), 0.0, 0.0));
  }
  pass.setPoints(points);

  ViewParams view_params;
  view_params.view = glm::mat4(1.0F);
  view_params.proj = glm::mat4(1.0F);
  view_params.device_width_px = 64;  // the ribbon expansion needs a viewport size
  view_params.device_height_px = 64;
  const glm::vec3 past_color(0.0F, 0.0F, 1.0F);
  const glm::vec3 future_color(0.5F, 0.7F, 1.0F);

  // All-past, all-future, mid-split, and degenerate sizes must all be GL-clean.
  pass.render(view_params, glm::dvec3(0.0), 10, past_color, future_color, 2.0F, true, true);
  pass.render(view_params, glm::dvec3(0.0), 0, past_color, future_color, 2.0F, true, true);
  pass.render(view_params, glm::dvec3(0.0), 5, past_color, future_color, 2.0F, true, true);
  pass.render(view_params, glm::dvec3(0.0), 999, past_color, future_color, 2.0F, true, true);  // past_count clamped
  EXPECT_EQ(glError(), static_cast<GLenum>(GL_NO_ERROR));

  // Origin change re-uploads (camera-relative rule) without GL errors.
  pass.render(view_params, glm::dvec3(1.0e6, 0.0, 0.0), 5, past_color, future_color, 2.0F, true, true);
  EXPECT_EQ(glError(), static_cast<GLenum>(GL_NO_ERROR));

  // A 1-point polyline draws nothing (and must not crash).
  pass.setPoints({glm::dvec3(0.0)});
  pass.render(view_params, glm::dvec3(0.0), 1, past_color, future_color, 2.0F, true, true);
  EXPECT_EQ(glError(), static_cast<GLenum>(GL_NO_ERROR));

  // Context-lifecycle contract: release + re-init + render (fresh upload path).
  pass.setPoints(points);
  pass.releaseGL();
  pass.initializeGL();
  pass.render(view_params, glm::dvec3(2.0), 5, past_color, future_color, 2.0F, true, true);
  EXPECT_EQ(glError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(TrailRenderPassGlTest, SplitColorsLandInTheFramebuffer) {
  QOpenGLFramebufferObject fbo(QSize(64, 64));
  ASSERT_TRUE(fbo.bind());
  auto* functions = context_.functions();
  functions->glViewport(0, 0, 64, 64);
  functions->glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  functions->glClear(GL_COLOR_BUFFER_BIT);

  TrailRenderPass pass;
  pass.initializeGL();

  // A horizontal strip across NDC x in [-0.9, 0.9] at y = 0; identity MVP maps
  // it straight to the viewport. past_count = 5 -> left half past, right half
  // future, sharing the middle vertex.
  std::vector<glm::dvec3> points;
  points.reserve(10);
  for (int i = 0; i < 10; ++i) {
    points.push_back(glm::dvec3(-0.9 + 0.2 * static_cast<double>(i), 0.0, 0.0));
  }
  pass.setPoints(points);

  ViewParams view_params;
  view_params.view = glm::mat4(1.0F);
  view_params.proj = glm::mat4(1.0F);
  view_params.device_width_px = 64;
  view_params.device_height_px = 64;
  pass.render(
      view_params, glm::dvec3(0.0), 5, glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 1.0F, 0.0F), 2.0F, true, true);
  ASSERT_EQ(glError(), static_cast<GLenum>(GL_NO_ERROR));

  const QImage image = fbo.toImage();
  int past_pixels = 0;
  int future_pixels = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      if (color.blue() > 200 && color.green() < 50) {
        ++past_pixels;
      } else if (color.green() > 200 && color.blue() < 50) {
        ++future_pixels;
      }
    }
  }
  // ~26 px of blue strip left of center, ~26 px of green right of it (1 px
  // wide). Use loose floors — rasterization details vary across drivers.
  EXPECT_GT(past_pixels, 10) << "past-color segment missing from the framebuffer";
  EXPECT_GT(future_pixels, 10) << "future-color segment missing from the framebuffer";
}

}  // namespace
