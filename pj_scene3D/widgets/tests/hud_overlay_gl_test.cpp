// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL integration test for the on-screen HUD overlays (perf HUD + TF hover
// label). It renders a SceneViewWidget on an ACTUAL OpenGL context, then forces
// the GL-context recreation that ADS triggers when it reparents a dock during
// layout restore — the exact trigger that corrupted HUD text in PR #214 — and
// asserts the overlay still rasterizes correctly afterward.
//
// What this proves (deterministically): the overlays render through real GL and
// SURVIVE a context recreation, because their text is CPU-rasterized into a
// QImage and blitted (drawImage), not painted through QPainter's per-GL-context
// glyph atlas (which did not survive the recreation). The reparent really does
// recreate the context here — proven by latching the dying context's
// aboutToBeDestroyed — so the test genuinely exercises the failure path, it does
// not merely render twice.
//
// What it does NOT claim: to reproduce the exact doubled-glyph artifact. That
// artifact is driver/DPR/timing dependent (it would not reproduce on the first
// try even by hand), so a test asserting it would itself be flaky. This test
// instead locks in the fix's invariant: overlay text is present and stable
// across the recreation.
//
// Skips cleanly when no usable GL context is available (e.g. a headless dev box
// without xvfb) OR when the context is too old for the scene's `#version 450`
// shaders. Linux CI runs under `xvfb-run` + Mesa llvmpipe, which reports GL 4.5,
// so it executes there. Windows CI's software GL is only a GL 3.0 / GLSL 1.30
// context — it cannot compile the scene shaders, so this test skips there.

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <limits>
#include <memory>
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

int luma(const QColor& c) {
  return (c.red() * 2126 + c.green() * 7152 + c.blue() * 722) / 10000;
}

// Bounding box of pixels in `region` that `on` darkened relative to `off` by
// more than `drop` luma — i.e. the translucent HUD panel's footprint. Returns an
// invalid (null) QRect when nothing darkened.
QRect darkenedBBox(const QImage& off, const QImage& on, const QRect& region, int drop, int* count) {
  int min_x = std::numeric_limits<int>::max(), min_y = std::numeric_limits<int>::max();
  int max_x = -1, max_y = -1, n = 0;
  for (int y = region.top(); y <= region.bottom(); ++y) {
    for (int x = region.left(); x <= region.right(); ++x) {
      if (luma(off.pixelColor(x, y)) - luma(on.pixelColor(x, y)) > drop) {
        ++n;
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
      }
    }
  }
  if (count != nullptr) {
    *count = n;
  }
  if (max_x < 0) {
    return {};
  }
  return QRect(QPoint(min_x, min_y), QPoint(max_x, max_y));
}

// Count near-white pixels (text strokes) inside `box`. The panel base is always
// well below 200 luma whatever the scene background is, so near-white pixels in
// the panel are glyphs — a background-independent "text rendered" signal.
int brightPixels(const QImage& img, const QRect& box) {
  if (!box.isValid()) {
    return 0;
  }
  int n = 0;
  for (int y = box.top(); y <= box.bottom(); ++y) {
    for (int x = box.left(); x <= box.right(); ++x) {
      const QColor c = img.pixelColor(x, y);
      if (c.red() > 200 && c.green() > 200 && c.blue() > 200) {
        ++n;
      }
    }
  }
  return n;
}

// Stands up a shown SceneViewWidget on a real GL context and owns the reparent
// that recreates that context. Both HUD tests share this prelude; the fixture
// still creates and destroys a FRESH widget + context per test (the property the
// recreation path depends on).
class HudOverlayGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // QOpenGLWidget needs a real windowing platform with GL. The "offscreen" and
    // "minimal" plugins report a valid context but cannot back a QOpenGLWidget
    // FBO ("QOpenGLWidget is not supported on this platform"), so paintGL's GL is
    // skipped and only a raster fallback draws — a meaningless pass. Skip those;
    // CI runs the suite under xvfb-run (xcb + software GL).
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
    // A context can be valid yet too old to compile the scene's shaders: every
    // scene3D shader is `#version 450` (GL 4.5). Linux CI's llvmpipe reports GL
    // 4.5 and renders; Windows CI's software GL is a GL 3.0 / GLSL 1.30 context,
    // so GridRenderPass/ArrowGizmo fail to compile, nothing renders, and there
    // is no HUD/scene footprint to detect — the assertions would fail on a scene
    // that physically cannot draw. Skip when the live context is below 4.5,
    // exactly as we skip when there is no GL at all.
    const auto gl_version = liveGlVersion(*view_);
    if (gl_version < std::pair<int, int>(4, 5)) {
      PJ_SKIP_WITHOUT_GPU(
          "GL " << gl_version.first << "." << gl_version.second
                << " context cannot compile the scene's #version 450 shaders (need GL 4.5)");
    }
  }

  void TearDown() override {
    // view_ is owned by whichever host currently parents it; resetting host2_
    // first (it owns view_ after a reparent) destroys the view and its GL context
    // before host_ (then an empty layout) goes away.
    host2_.reset();
    host_.reset();
  }

  // Reparent view_ into a fresh top-level window — the layout-restore path that
  // makes QOpenGLWidget destroy its old GL context and build a new one. Proves
  // the destruction by latching the dying context's aboutToBeDestroyed (comparing
  // raw QOpenGLContext* before/after is unsound: the allocator can hand the new
  // context the just-freed address, and recreation can be deferred past the
  // reparent). aboutToBeDestroyed is the same signal the widget itself wires in
  // initializeGL, so this also proves the production teardown hook ran.
  [[nodiscard]] ::testing::AssertionResult reparentAndRecreate() {
    bool old_ctx_destroyed = false;
    QObject::connect(view_->context(), &QOpenGLContext::aboutToBeDestroyed, view_, [&old_ctx_destroyed] {
      old_ctx_destroyed = true;
    });
    host2_ = std::make_unique<QWidget>();
    auto* layout2 = new QVBoxLayout(host2_.get());
    layout2->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    layout2->addWidget(view_);  // reparents view_; Qt removes it from the old layout
    host2_->resize(480, 360);
    host2_->show();
    QApplication::processEvents();
    view_->grabFramebuffer();  // drive the rebuild + a paint on the new context
    if (!old_ctx_destroyed) {
      return ::testing::AssertionFailure() << "reparent did not destroy/recreate the GL context; test is vacuous";
    }
    return ::testing::AssertionSuccess();
  }

  std::unique_ptr<QWidget> host_;
  std::unique_ptr<QWidget> host2_;
  SceneViewWidget* view_ = nullptr;
};

}  // namespace

// The perf HUD renders on a real GL context and survives the context recreation
// that a dock reparent (layout restore) forces. It shares the exact
// renderHudPanel + drawImage path with the TF hover label.
TEST_F(HudOverlayGlTest, PerfHudRendersAndSurvivesContextRecreation) {
  // Baseline: HUD off.
  view_->setShowPerfHud(false);
  const QImage off = view_->grabFramebuffer();
  ASSERT_FALSE(off.isNull());
  // Bottom-left region where drawPerfHud anchors (device pixels).
  const QRect region(0, off.height() / 2, off.width() / 2, off.height() - off.height() / 2 - 1);

  // HUD on: the panel darkens a chunk of the region and text rasterizes into it.
  view_->setShowPerfHud(true);
  const QImage on = view_->grabFramebuffer();
  ASSERT_FALSE(on.isNull());
  int panel_n = 0;
  // The panel is source-over black at alpha 150, i.e. it keeps 1-150/255 ≈ 41% of
  // the background, a ≈59%·bg_luma darkening. main() forces a white background so
  // that drop is huge; drop=10 still leaves margin even on a dark default palette.
  const QRect panel = darkenedBBox(off, on, region, /*drop=*/10, &panel_n);
  const int text_on = brightPixels(on, panel);

  if (PJ::sdk::getEnv("PJ_HUD_GL_DEBUG").has_value()) {
    qWarning(
        "img=%dx%d dpr=%.2f panel_px=%d panel_bbox=(%d,%d %dx%d) text_on=%d", on.width(), on.height(),
        on.devicePixelRatio(), panel_n, panel.x(), panel.y(), panel.width(), panel.height(), text_on);
  }

  ASSERT_TRUE(panel.isValid()) << "perf HUD panel left no darkened footprint";
  EXPECT_GT(panel_n, 200) << "perf HUD panel footprint implausibly small";
  EXPECT_GT(text_on, 15) << "perf HUD text did not rasterize over the panel";

  ASSERT_TRUE(reparentAndRecreate());
  if (!haveGl(*view_)) {
    PJ_SKIP_WITHOUT_GPU("context not usable after reparent on this backend");
  }

  // The HUD must still render after recreation — the regression the fix targets.
  view_->setShowPerfHud(false);
  const QImage off2 = view_->grabFramebuffer();
  ASSERT_FALSE(off2.isNull());
  view_->setShowPerfHud(true);
  const QImage on3 = view_->grabFramebuffer();
  ASSERT_FALSE(on3.isNull());
  int panel_n2 = 0;
  const QRect panel2 = darkenedBBox(off2, on3, region, /*drop=*/10, &panel_n2);
  const int text_after = brightPixels(on3, panel2);

  if (PJ::sdk::getEnv("PJ_HUD_GL_DEBUG").has_value()) {
    qWarning("after recreation: panel_px=%d text_after=%d (was text_on=%d)", panel_n2, text_after, text_on);
  }

  ASSERT_TRUE(panel2.isValid()) << "perf HUD panel vanished after context recreation";
  EXPECT_GT(text_after, 15) << "perf HUD text vanished/garbled after context recreation";
}

// The TF hover label renders end to end on real GL and survives a context
// recreation. Unlike the perf HUD, this drives the FULL hover path
// (mouseMoveEvent -> updateHoverFrame -> drawHoverLabel: frame lookup, anchor
// re-projection, the axes-visible gate, and the rasterize-then-blit). The
// default OrbitCamera targets the world origin, so the fixed frame (at the
// origin) projects to the viewport centre and a centre cursor hovers it.
TEST_F(HudOverlayGlTest, HoverLabelRendersAndSurvivesContextRecreation) {
  // One TF edge: 'odom' as a child of the fixed frame 'base_link'. base_link
  // itself resolves to the origin, which the default camera frames at the centre.
  auto tf = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  ASSERT_TRUE(tf->setTransform(
                    pj::scene3d::StampedTransform{
                        .stamp = PJ::fromRaw(0),
                        .parent_frame = "base_link",
                        .child_frame = "odom",
                        .transform = pj::scene3d::Transform(glm::dvec3{1.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}),
                    })
                  .has_value());
  view_->setTransformBuffer(tf);
  view_->setFixedFrame("base_link");
  view_->setTrackerTime(PJ::fromRaw(1'000'000));
  view_->setAxesVisible(true);

  // Returns the hover label's panel bbox + its text-pixel count. Sequence: clear
  // any prior hover (a far-corner move misses every frame), grab a baseline, then
  // hover the centre and grab again. The centre move uses last_view_proj_ from
  // the baseline grab, so it projects correctly. The highlighted triad and the
  // connector ring BRIGHTEN pixels, so a darkened bbox isolates the label panel.
  auto measureHoverLabel = [](SceneViewWidget* v) -> std::pair<QRect, int> {
    auto sendMove = [v](QPointF pos) {
      // 7-arg ctor (local/scene/global): the 5-arg form is deprecated in Qt 6.11.
      // The handler only reads position() (local); scene/global mirror it.
      QMouseEvent move(QEvent::MouseMove, pos, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(v, &move);
    };
    sendMove(QPointF(2.0, 2.0));  // far corner: clears any prior hover
    const QImage baseline = v->grabFramebuffer();
    sendMove(QPointF(v->width() / 2.0, v->height() / 2.0));  // centre: hovers the origin frame
    const QImage hovered = v->grabFramebuffer();
    if (baseline.isNull() || hovered.isNull()) {
      return {QRect(), 0};
    }
    const QRect central(hovered.width() / 8, 0, hovered.width() * 3 / 4, hovered.height() * 3 / 4);
    int n = 0;
    const QRect label = darkenedBBox(baseline, hovered, central, /*drop=*/10, &n);
    return {label, brightPixels(hovered, label)};
  };

  const auto [label, text] = measureHoverLabel(view_);
  if (PJ::sdk::getEnv("PJ_HUD_GL_DEBUG").has_value()) {
    qWarning("hover: label_bbox=(%d,%d %dx%d) text=%d", label.x(), label.y(), label.width(), label.height(), text);
  }
  ASSERT_TRUE(label.isValid()) << "hover label did not appear (no darkened panel near centre)";
  EXPECT_GT(text, 10) << "hover label text did not rasterize";

  ASSERT_TRUE(reparentAndRecreate());
  if (!haveGl(*view_)) {
    PJ_SKIP_WITHOUT_GPU("context not usable after reparent on this backend");
  }

  const auto [label2, text2] = measureHoverLabel(view_);
  if (PJ::sdk::getEnv("PJ_HUD_GL_DEBUG").has_value()) {
    qWarning(
        "hover after recreation: label_bbox=(%d,%d %dx%d) text=%d", label2.x(), label2.y(), label2.width(),
        label2.height(), text2);
  }
  ASSERT_TRUE(label2.isValid()) << "hover label vanished after context recreation";
  EXPECT_GT(text2, 10) << "hover label text vanished/garbled after context recreation";
}

int main(int argc, char** argv) {
  // Match the demos: request a 4.5 core context so QOpenGLWidget negotiates the
  // format SceneViewWidget expects. Harmless under software GL.
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  // renderScene picks its clear color from the light/dark branch of the
  // application palette's Window color. Force white so the empty-scene
  // background is deterministic (light theme) regardless of the host/CI default
  // palette — this is what gives the HUD-panel darkening detection its margin.
  QPalette pal = app.palette();
  pal.setColor(QPalette::Window, QColor(255, 255, 255));
  app.setPalette(pal);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
