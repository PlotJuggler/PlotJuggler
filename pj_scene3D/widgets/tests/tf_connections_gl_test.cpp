// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL integration test for the TF parent-connection lines. Renders an
// actual SceneViewWidget on an OpenGL context, feeds it one parent->child TF
// edge, and asserts that magenta line pixels appear only when the connections
// are enabled — and that they survive the dock-reparent context recreation (the
// pass owns GL objects released/rebuilt in releaseGlResources/initializeGL).
//
// Skips when there is no usable GL or the live context is below 4.5 (the scene's
// #version 450 shaders won't compile), exactly like hud_overlay_gl_test. CI runs
// it under xvfb-run with software GL (llvmpipe reports 4.5).

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPalette>
#include <QString>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>
#include <string>
#include <utility>

#include "gl_scene_test_support.h"  // haveGl, liveGlVersion
#include "pj_base/sdk/platform.hpp"
#include "pj_base/time.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/FrameworkTokens.h"

namespace {

using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::haveGl;
using pj::scene3d::test::liveGlVersion;

// Count magenta-ish pixels: red AND blue both clearly above green. Robust to
// any residual tonemap/AA tint (the lines bypass the tonemap, but edge pixels
// still blend with the white background). The scene's other elements never
// satisfy this: the gray grid has r==g==b; the RGB triads each push ONE channel
// (red arrow: r>>g,b but b not > g; blue arrow: b>>r,g but r not > g); the white
// background has r==g==b. So a positive count means parent-connection lines.
int magentaPixels(const QImage& img) {
  if (img.isNull()) {
    return 0;
  }
  int n = 0;
  for (int y = 0; y < img.height(); ++y) {
    for (int x = 0; x < img.width(); ++x) {
      const QColor c = img.pixelColor(x, y);
      if (c.red() > 100 && c.blue() > 100 && c.red() - c.green() > 50 && c.blue() - c.green() > 50) {
        ++n;
      }
    }
  }
  return n;
}

class TfConnectionsGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
      PJ_SKIP_WITHOUT_GPU("platform '" << platform.toStdString() << "' has no real-GL QOpenGLWidget support");
    }
    host_ = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(host_.get());
    layout->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    view_ = new SceneViewWidget();
    layout->addWidget(view_);
    host_->resize(480, 360);
    host_->show();
    QApplication::processEvents();
    if (!haveGl(*view_)) {
      PJ_SKIP_WITHOUT_GPU("No usable OpenGL context (headless without xvfb/GL).");
    }
    if (liveGlVersion(*view_) < std::pair<int, int>(4, 5)) {
      PJ_SKIP_WITHOUT_GPU("GL context below 4.5 cannot compile the scene's #version 450 shaders");
    }

    // Several children of the fixed frame 'base_link', spread around the origin
    // in the XY plane (radius 1 m). base_link resolves to the origin, framed at
    // the viewport centre by the default OrbitCamera, so the connection lines
    // radiate outward across the view — the same star pattern the feature draws
    // on a real TF tree, and enough on-screen line length for a stable magenta
    // count. Axes hidden to isolate the lines from the RGB triads.
    auto tf = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
    const glm::dvec3 offsets[] = {{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},  {0.0, 1.0, 0.0},  {0.0, -1.0, 0.0},
                                  {0.7, 0.7, 0.0}, {-0.7, -0.7, 0.0}, {0.7, -0.7, 0.0}, {-0.7, 0.7, 0.0}};
    int i = 0;
    for (const glm::dvec3& offset : offsets) {
      ASSERT_TRUE(tf->setTransform(
                        pj::scene3d::StampedTransform{
                            .stamp = PJ::fromRaw(0),
                            .parent_frame = "base_link",
                            .child_frame = "child_" + std::to_string(i++),
                            .transform = pj::scene3d::Transform(offset, glm::dquat{1.0, 0.0, 0.0, 0.0}),
                        })
                      .has_value());
    }
    view_->setTransformBuffer(tf);
    view_->setFixedFrame("base_link");
    view_->setTrackerTime(PJ::fromRaw(1'000'000));
    view_->setAxesVisible(false);
  }

  void TearDown() override {
    host2_.reset();
    host_.reset();
  }

  // Reparent into a fresh window — forces QOpenGLWidget to destroy/recreate its
  // GL context (the layout-restore path), exercising the pass's releaseGL +
  // initializeGL. Mirrors hud_overlay_gl_test::reparentAndRecreate.
  void reparentAndRecreate() {
    host2_ = std::make_unique<QWidget>();
    auto* layout2 = new QVBoxLayout(host2_.get());
    layout2->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    layout2->addWidget(view_);
    host2_->resize(480, 360);
    host2_->show();
    QApplication::processEvents();
    view_->grabFramebuffer();  // drive the rebuild + a paint on the new context
  }

  std::unique_ptr<QWidget> host_;
  std::unique_ptr<QWidget> host2_;
  SceneViewWidget* view_ = nullptr;
};

TEST_F(TfConnectionsGlTest, MagentaLinesAppearOnlyWhenVisible) {
  view_->setTfConnectionsVisible(false);
  const int off = magentaPixels(view_->grabFramebuffer());

  view_->setTfConnectionsVisible(true);
  const QImage on_img = view_->grabFramebuffer();
  const int on = magentaPixels(on_img);

  // Optional eyeball aid: dump the rendered frame for manual inspection.
  if (const auto path = PJ::sdk::getEnv("PJ_TF_LINES_GL_SAVE"); path.has_value() && !on_img.isNull()) {
    on_img.save(QString::fromStdString(*path));
  }

  EXPECT_EQ(off, 0) << "magenta pixels present with connections hidden";
  EXPECT_GT(on, 30) << "no parent-connection lines rendered when enabled";

  // Toggling back off removes them again — the visibility gate is honored.
  view_->setTfConnectionsVisible(false);
  EXPECT_EQ(magentaPixels(view_->grabFramebuffer()), 0) << "lines lingered after hide";
}

TEST_F(TfConnectionsGlTest, LinesSurviveContextRecreation) {
  view_->setTfConnectionsVisible(true);
  ASSERT_GT(magentaPixels(view_->grabFramebuffer()), 30) << "lines absent before reparent; test would be vacuous";

  reparentAndRecreate();
  if (!haveGl(*view_)) {
    PJ_SKIP_WITHOUT_GPU("context not usable after reparent on this backend");
  }

  EXPECT_GT(magentaPixels(view_->grabFramebuffer()), 30) << "parent-connection lines vanished after context recreation";
}

}  // namespace

int main(int argc, char** argv) {
  // Request a 4.5 core context so QOpenGLWidget negotiates the format
  // SceneViewWidget expects (harmless under software GL).
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  // renderScene picks its clear color from the palette's Window color; force
  // white so the empty-scene background is deterministic and the magenta lines
  // stand out against it.
  QPalette pal = app.palette();
  pal.setColor(QPalette::Window, QColor(255, 255, 255));
  app.setPalette(pal);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
