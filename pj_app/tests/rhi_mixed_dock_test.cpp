// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The real GPU widgets together in one Qt-Advanced-Docking manager: a PlotWidget
// (PlotRhiCanvas) with a curve, a Scene3D view showing a point cloud and the
// Scene2D media viewer showing an image. Float/redock, hidden tabs, sibling
// destruction, splits, closing a floating window and a device-pixel-ratio move
// must leave each one drawing its own content, captured at its pixel size, with
// no QRhi warning and no resource left on a QRhi that goes away
// (QT_RHI_LEAK_CHECK, which does not see OpenGL uniform buffers). The widgets'
// lifecycle counters (test hooks) also show each one re-initializing on every
// new top-level and never holding resources on two QRhis. Also the media
// viewer's failure state, beside working siblings. The widgets and their data
// come from mixed_layout_fixture.h. Registered per backend (PJ_GRAPHICS_API);
// needs a real display (skips on the offscreen QPA).

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QRhiWidget>
#include <QScreen>
#include <QTest>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "mixed_layout_fixture.h"
#include "passes/point_rhi_pass.h"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_plotting/PlotRhiCanvas.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "scene_view_test_access.h"
#include "support/rhi_dock_test_support.h"

using namespace Qt::StringLiterals;
using namespace pj_rhi_dock_test;

namespace {

using pj::scene3d::SceneViewTestAccess;

using pj_mixed_layout::kCurveColour;
constexpr int kCloudSide = 8;  // points per edge of the cube-shaped cloud
constexpr int kCloudPoints = kCloudSide * kCloudSide * kCloudSide;

// Pixels of `image` for which `match` holds.
int countPixels(const QImage& image, const std::function<bool(QRgb)>& match) {
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      count += match(image.pixel(x, y)) ? 1 : 0;
    }
  }
  return count;
}

// A real GPU widget's QRhi lifecycle, from the pjTestRhi* counters its test
// hooks keep: resources adopted on a QRhi, released, and a new QRhi met while
// the old one's resources were still held.
struct RhiHold {
  int adoptions = 0;
  int releases = 0;
  int stale = 0;
  [[nodiscard]] int held() const {
    return adoptions - releases;
  }
};

RhiHold rhiHold(const QObject* widget) {
  return {
      widget->property("pjTestRhiAdoptions").toInt(), widget->property("pjTestRhiReleases").toInt(),
      widget->property("pjTestRhiStale").toInt()};
}

// The media fixture's image, top on top (Vulkan's clip space is y-down): red top
// half, blue bottom half.
bool showsMediaImage(const QImage& image) {
  if (image.isNull()) {
    return false;
  }
  const int cx = image.width() / 2;
  const int offset = std::min(image.width(), image.height()) * 3 / 10;
  return colourNear(image.pixel(cx, image.height() / 2 - offset), Qt::red) &&
         colourNear(image.pixel(cx, image.height() / 2 + offset), Qt::blue);
}

::testing::AssertionResult capturedAtPixelSize(const QImage& image, const QWidget* widget) {
  if (image.isNull()) {
    return ::testing::AssertionFailure() << "no frame presented";
  }
  const QSize expected = widget->size() * widget->devicePixelRatio();
  if (image.size() != expected) {
    return ::testing::AssertionFailure() << "captured " << image.width() << "x" << image.height() << ", widget is "
                                         << expected.width() << "x" << expected.height() << " device pixels";
  }
  return ::testing::AssertionSuccess();
}

class RhiMixedDockFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
    layout_ = std::make_unique<pj_mixed_layout::MixedLayout>();
    manager_ = &layout_->manager();
    window_ = &layout_->window();
    if (late_gpu_docks_) {
      // A raster-only first show: no QRhiWidget exists when the window maps.
      layout_->addDock(u"raster"_s, new QLabel(u"raster"_s), ads::TopDockWidgetArea);
      ASSERT_NO_FATAL_FAILURE(showMainWindow());
      if (IsSkipped()) {
        return;
      }
    }
    plot_ = layout_->createPlot();
    scene_ = layout_->createScene3D(kCloudSide);
    media_ = layout_->createMedia();
    if (media_fail_pipeline_ != nullptr) {
      mediaViewer()->setProperty("pjTestFailPipeline", QString::fromLatin1(media_fail_pipeline_));
    }
    plot_dock_ = layout_->addDock(u"plot"_s, plot_, ads::LeftDockWidgetArea);
    scene_dock_ = layout_->addDock(u"scene3d"_s, scene_, ads::RightDockWidgetArea);
    media_dock_ = layout_->addDock(u"media"_s, media_, ads::BottomDockWidgetArea);
    const std::array<QRhiWidget*, 3> widgets{plotCanvas(), sceneView(), mediaViewer()};
    for (std::size_t i = 0; i < widgets.size(); ++i) {
      ASSERT_NE(widgets[i], nullptr) << "GPU widget " << i << " missing";
      QObject::connect(widgets[i], &QRhiWidget::renderFailed, window_, [this, i] { ++render_failures_[i]; });
    }

    ASSERT_NO_FATAL_FAILURE(showMainWindow());
    if (IsSkipped()) {
      return;
    }
    (void)pjWaitForGrab([this](auto done) { sceneView()->grabPresentedFrame(std::move(done)); });  // first frame
    if (expected_media_failures_ == 0) {
      ASSERT_NO_FATAL_FAILURE(expectAllRender("initial layout"));
    }
  }

  void TearDown() override {
    layout_.reset();
    QApplication::processEvents();
    if (IsSkipped()) {
      return;
    }
    EXPECT_TRUE(warnings_.rhiProblems().isEmpty()) << warnings_.rhiProblems().join(u'\n').toStdString();
    EXPECT_EQ(render_failures_[0], 0) << "the plot canvas failed";
    EXPECT_EQ(render_failures_[1], 0) << "the Scene3D view failed";
    EXPECT_EQ(render_failures_[2], expected_media_failures_) << "media viewer renderFailed() count";
  }

  // Shows the main window and waits for it to map; skips (fails under
  // PJ_REQUIRE_GPU_TESTS=1) when it never does.
  void showMainWindow() {
    window_->resize(1000, 760);
    window_->show();
    if (!QTest::qWaitForWindowExposed(window_)) {
      PJ_SKIP_WITHOUT_GPU("the main window was never exposed");
    }
  }

  [[nodiscard]] PJ::PlotRhiCanvas* plotCanvas() const {
    return qobject_cast<PJ::PlotRhiCanvas*>(plot_->canvasWidget());
  }
  [[nodiscard]] pj::scene3d::SceneViewWidget* sceneView() const {
    return scene_->sceneView();
  }
  [[nodiscard]] PJ::MediaViewerWidget* mediaViewer() const {
    return media_->findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
  }

  void expectPlotRenders(const char* where) {
    auto* canvas = plotCanvas();
    ASSERT_NE(canvas, nullptr) << where << ": the plot canvas is not a PlotRhiCanvas";
    const QImage image = pjWaitForGrab([canvas](auto done) { canvas->grabPresentedFrame(std::move(done)); });
    ASSERT_TRUE(capturedAtPixelSize(image, canvas)) << where << ": plot";
    EXPECT_GT(countPixels(image, [](QRgb p) { return colourNear(p, kCurveColour); }), 100)
        << where << ": no curve drawn";
  }

  void expectSceneRenders(const char* where) {
    auto* view = sceneView();
    const QImage image = pjWaitForGrab([view](auto done) { view->grabPresentedFrame(std::move(done)); });
    ASSERT_TRUE(capturedAtPixelSize(image, view)) << where << ": Scene3D";
    EXPECT_EQ(SceneViewTestAccess::pointPass(*view).lastPointVertexCount(), kCloudPoints) << where;
    const int green = countPixels(image, [](QRgb p) { return qGreen(p) > qRed(p) + 50 && qGreen(p) > qBlue(p) + 50; });
    EXPECT_GT(green, 200) << where << ": no point cloud drawn";
  }

  // Frames are decoded asynchronously, so this waits for the first one.
  void expectMediaRenders(const char* where) {
    QImage image;
    const auto shows_image = [&] {
      image = presentAndGrab(mediaViewer());
      return showsMediaImage(image);
    };
    EXPECT_TRUE(QTest::qWaitFor(shows_image, 5000)) << where << ": the media viewer does not show the image";
    EXPECT_TRUE(capturedAtPixelSize(image, mediaViewer())) << where << ": media";
  }

  // Every widget whose dock is open and the current tab draws its own content
  // and holds resources on exactly one QRhi; no widget ever holds two.
  void expectAllRender(const char* where) {
    const auto shown = [](const ads::CDockWidget* dock) {
      return dock != nullptr && !dock->isClosed() && dock->isCurrentTab();
    };
    if (shown(plot_dock_)) {
      ASSERT_NO_FATAL_FAILURE(expectPlotRenders(where));
    }
    if (shown(scene_dock_)) {
      ASSERT_NO_FATAL_FAILURE(expectSceneRenders(where));
    }
    if (shown(media_dock_)) {
      ASSERT_NO_FATAL_FAILURE(expectMediaRenders(where));
    }
    for (ads::CDockWidget* dock : docks()) {
      if (dock == nullptr) {
        continue;
      }
      const RhiHold hold = rhiHold(gpuWidget(dock));
      const std::string name = dock->objectName().toStdString();
      EXPECT_EQ(hold.stale, 0) << where << ", " << name << ": met a new QRhi still holding the old one's resources";
      if (shown(dock)) {
        EXPECT_EQ(hold.held(), 1) << where << ", " << name << ": holds resources on " << hold.held() << " QRhis";
      } else {
        EXPECT_LE(hold.held(), 1) << where << ", " << name << ": holds resources on " << hold.held() << " QRhis";
      }
    }
  }

  [[nodiscard]] QRhiWidget* gpuWidget(const ads::CDockWidget* dock) const {
    if (dock == plot_dock_) {
      return plotCanvas();
    }
    return dock == scene_dock_ ? static_cast<QRhiWidget*>(sceneView()) : mediaViewer();
  }

  // Resources adopted so far by the GPU widget in `dock`.
  [[nodiscard]] int adoptions(const ads::CDockWidget* dock) const {
    return rhiHold(gpuWidget(dock)).adoptions;
  }

  [[nodiscard]] std::array<ads::CDockWidget*, 3> docks() const {
    return {plot_dock_, scene_dock_, media_dock_};
  }

  // Deletes `dock` (and the widget in it) and clears the pointer the fixture holds.
  void destroyDock(ads::CDockWidget*& dock) {
    manager_->removeDockWidget(dock);
    delete dock;
    dock = nullptr;
    QApplication::processEvents();
  }

  // Floats each dock, then docks it back, checking every widget each time.
  void floatAndRedockEach() {
    for (ads::CDockWidget* dock : docks()) {
      SCOPED_TRACE(dock->objectName().toStdString());
      const int docked = adoptions(dock);
      ASSERT_NO_FATAL_FAILURE(floatDock(dock));
      EXPECT_NE(dock->window(), window_);
      ASSERT_NO_FATAL_FAILURE(expectAllRender("one dock floating"));
      EXPECT_GT(adoptions(dock), docked) << "no initialize() on the floating window's QRhi";
      const int floating = adoptions(dock);
      manager_->addDockWidget(ads::BottomDockWidgetArea, dock);
      ASSERT_EQ(dock->window(), window_);
      ASSERT_NO_FATAL_FAILURE(expectAllRender("redocked"));
      EXPECT_GT(adoptions(dock), floating) << "no initialize() on the main window's QRhi";
    }
  }

  // Set by a derived fixture before SetUp(): the media viewer pipeline to fail.
  const char* media_fail_pipeline_ = nullptr;
  // Set by a derived fixture before SetUp(): show the window before any GPU
  // widget exists, then dock them.
  bool late_gpu_docks_ = false;
  int expected_media_failures_ = 0;

  // Declaration order is destruction order reversed: the layout dies before the
  // collector that checks its warnings.
  WarningCollector warnings_;
  std::array<int, 3> render_failures_{};  // plot, Scene3D, media
  std::unique_ptr<pj_mixed_layout::MixedLayout> layout_;
  QMainWindow* window_ = nullptr;         // layout_'s
  ads::CDockManager* manager_ = nullptr;  // layout_'s
  QPointer<PJ::PlotWidget> plot_;
  QPointer<PJ::Scene3DDockWidget> scene_;
  QPointer<PJ::Scene2DDockWidget> media_;
  ads::CDockWidget* plot_dock_ = nullptr;
  ads::CDockWidget* scene_dock_ = nullptr;
  ads::CDockWidget* media_dock_ = nullptr;
};

TEST_F(RhiMixedDockFixture, FloatAndRedockEachKeepsEveryWidgetRendering) {
  floatAndRedockEach();
}

// GPU docks created after the window's first show had no QRhiWidget (the app
// without a GPU plot canvas, then a Scene3D or media dock): Qt switches the
// window to QRhi composition, and no zero-size bootstrap widget is needed.
class RhiMixedLateDockFixture : public RhiMixedDockFixture {
 protected:
  void SetUp() override {
    late_gpu_docks_ = true;
    RhiMixedDockFixture::SetUp();
  }
};

TEST_F(RhiMixedLateDockFixture, GpuDocksAddedAfterARasterOnlyFirstShowRender) {
  expectAllRender("GPU docks added after the first show");
}

// The app builds a Scene2D dock in place, its parent a dock already in the shown
// window, and here no other GPU widget switches that raster window to QRhi
// composition (see Scene2DDockWidget::createSceneView).
TEST(RhiScene2DInPlaceTest, MediaDockBuiltInsideAShownRasterWindowRenders) {
  PJ_SKIP_ON_HEADLESS_QPA();
  WarningCollector warnings;
  pj_mixed_layout::MixedLayout layout;
  auto* host = new QWidget();
  auto* host_layout = new QVBoxLayout(host);
  host_layout->setContentsMargins(0, 0, 0, 0);
  layout.addDock(u"host"_s, host, ads::TopDockWidgetArea);
  layout.window().resize(1000, 760);
  layout.window().show();
  if (!QTest::qWaitForWindowExposed(&layout.window())) {
    PJ_SKIP_WITHOUT_GPU("the main window was never exposed");
  }

  PJ::Scene2DDockWidget* media = layout.createMedia(host);
  host_layout->addWidget(media);
  auto* viewer = media->findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
  ASSERT_NE(viewer, nullptr);
  int render_failures = 0;
  QObject::connect(viewer, &QRhiWidget::renderFailed, viewer, [&render_failures] { ++render_failures; });

  EXPECT_TRUE(QTest::qWaitFor([viewer] { return showsMediaImage(presentAndGrab(viewer)); }, 5000))
      << "the media viewer does not show the image";
  EXPECT_EQ(render_failures, 0) << "the media viewer failed to render";
  EXPECT_TRUE(warnings.rhiProblems().isEmpty()) << warnings.rhiProblems().join(u'\n').toStdString();
}

TEST_F(RhiMixedDockFixture, HiddenTabsRenderAgainWhenShown) {
  manager_->addDockWidgetTabToArea(scene_dock_, plot_dock_->dockAreaWidget());
  manager_->addDockWidgetTabToArea(media_dock_, plot_dock_->dockAreaWidget());
  for (int round = 0; round < 2; ++round) {
    for (ads::CDockWidget* dock : docks()) {
      SCOPED_TRACE(dock->objectName().toStdString());
      dock->setAsCurrentTab();
      ASSERT_NO_FATAL_FAILURE(expectAllRender("tab shown"));
    }
  }
}

// A dock-area tab switch keeps the hidden tab parented (our ADS patch,
// pj_scene3D/docs/TECHNICAL_NOTES.md §5): after the first visit, switching back
// to a tab adopts no QRhi, rebuilds nothing and uploads nothing, and what
// changed while the Scene3D tab was hidden shows when it comes back.
TEST_F(RhiMixedDockFixture, TabSwitchesAfterTheFirstVisitRebuildNothing) {
  manager_->addDockWidgetTabToArea(scene_dock_, plot_dock_->dockAreaWidget());
  manager_->addDockWidgetTabToArea(media_dock_, plot_dock_->dockAreaWidget());
  for (ads::CDockWidget* dock : docks()) {
    dock->setAsCurrentTab();
    ASSERT_NO_FATAL_FAILURE(expectAllRender("first visit"));
  }
  std::array<int, 3> adopted{};
  for (std::size_t i = 0; i < adopted.size(); ++i) {
    adopted[i] = adoptions(docks()[i]);
  }
#ifdef PJ_RHI_TEST_HOOKS
  const auto& scene_work = SceneViewTestAccess::stageTimes(*sceneView());
  const int builds = scene_work.base_builds;
  const std::uint64_t uploads = scene_work.point_uploads;
#endif
  for (int round = 0; round < 3; ++round) {
    for (ads::CDockWidget* dock : docks()) {
      SCOPED_TRACE(dock->objectName().toStdString());
      dock->setAsCurrentTab();
      ASSERT_NO_FATAL_FAILURE(expectAllRender("switched back"));
    }
  }
  for (std::size_t i = 0; i < adopted.size(); ++i) {
    EXPECT_EQ(adoptions(docks()[i]), adopted[i]) << docks()[i]->objectName().toStdString() << " adopted a QRhi again";
  }
#ifdef PJ_RHI_TEST_HOOKS
  EXPECT_EQ(scene_work.base_builds, builds) << "the Scene3D view rebuilt its base resources";
  EXPECT_EQ(scene_work.point_uploads, uploads) << "the Scene3D view uploaded its cloud again";
#endif

  auto* view = sceneView();
  const auto green_pixels = [view] {
    const QImage image = pjWaitForGrab([view](auto done) { view->grabPresentedFrame(std::move(done)); });
    EXPECT_TRUE(capturedAtPixelSize(image, view));
    return countPixels(image, [](QRgb p) { return qGreen(p) > qRed(p) + 50 && qGreen(p) > qBlue(p) + 50; });
  };
  scene_dock_->setAsCurrentTab();
  const int with_cloud = green_pixels();
  plot_dock_->setAsCurrentTab();
  ASSERT_NO_FATAL_FAILURE(expectAllRender("plot in front"));
  scene_->setLayerVisible(scene_->layers().front().topic_id, false);
  scene_dock_->setAsCurrentTab();
  const int without_cloud = green_pixels();  // the corner gizmo's Y axis stays
  EXPECT_EQ(SceneViewTestAccess::pointPass(*view).lastPointVertexCount(), 0)
      << "the cloud hidden while its tab was hidden still drew";
  EXPECT_LT(without_cloud * 2, with_cloud);
}

TEST_F(RhiMixedDockFixture, SplittingAndMovingWithinTheWindowKeepRendering) {
  manager_->addDockWidget(ads::TopDockWidgetArea, scene_dock_, media_dock_->dockAreaWidget());
  ASSERT_NO_FATAL_FAILURE(expectAllRender("Scene3D split above the media viewer"));
  manager_->addDockWidget(ads::RightDockWidgetArea, plot_dock_, media_dock_->dockAreaWidget());
  ASSERT_NO_FATAL_FAILURE(expectAllRender("plot split beside the media viewer"));
}

TEST_F(RhiMixedDockFixture, DestroyingSiblingsLeavesTheOthersRendering) {
  destroyDock(plot_dock_);
  ASSERT_NO_FATAL_FAILURE(expectAllRender("plot destroyed"));

  // A sibling destroyed inside a floating window.
  ASSERT_NO_FATAL_FAILURE(floatDock(scene_dock_));
  manager_->addDockWidget(ads::RightDockWidgetArea, media_dock_, scene_dock_->dockAreaWidget());
  ASSERT_EQ(media_dock_->window(), scene_dock_->window());
  ASSERT_NO_FATAL_FAILURE(expectAllRender("media joined the floating Scene3D"));
  destroyDock(scene_dock_);
  ASSERT_NO_FATAL_FAILURE(expectAllRender("Scene3D destroyed while floating"));
}

TEST_F(RhiMixedDockFixture, ClosingEachFloatingWindowAndReopeningRendersAgain) {
  for (ads::CDockWidget* dock : docks()) {
    SCOPED_TRACE(dock->objectName().toStdString());
    const int docked = adoptions(dock);
    ASSERT_NO_FATAL_FAILURE(floatDock(dock));
    QPointer<ads::CFloatingDockContainer> floating = dock->floatingDockContainer();
    ASSERT_NE(floating, nullptr);
    ASSERT_NO_FATAL_FAILURE(expectAllRender("floating"));
    EXPECT_GT(adoptions(dock), docked) << "no initialize() on the floating window's QRhi";
    const int before_close = adoptions(dock);
    const QPointer<QWidget> floating_window = dock->window();
    floating->close();
    ASSERT_TRUE(QTest::qWaitFor([&] {
      return floating.isNull() || floating->windowHandle() == nullptr || !floating->windowHandle()->isExposed();
    }));
    // See rhi_dock_lifecycle_test: reopen only after the compositor applied the unmap.
    QTest::qWait(100);
    EXPECT_TRUE(dock->isClosed());
    ASSERT_NO_FATAL_FAILURE(expectAllRender("one floating window closed"));
    dock->toggleView(true);
    ASSERT_TRUE(QTest::qWaitForWindowExposed(dock->window()));
    ASSERT_NO_FATAL_FAILURE(expectAllRender("reopened"));
    // ADS reopens a closed floating dock in its hidden container, whose QRhi
    // lives on: the widget keeps its resources there, and re-initializes only
    // when it lands in another top-level.
    if (dock->window() == floating_window) {
      EXPECT_EQ(adoptions(dock), before_close) << "resources rebuilt for the same QRhi";
    } else {
      EXPECT_GT(adoptions(dock), before_close) << "no initialize() in the new top-level";
    }
  }
}

// Needs two screens with different device-pixel ratios on xcb: a headless
// mutter's Xwayland with two virtual monitors plus QT_SCREEN_SCALE_FACTORS="1;2"
// (linux-ci's mixed-DPR step). Skips without them (fails with
// PJ_REQUIRE_MIXED_DPR=1); a move that does not change the ratio fails.
TEST_F(RhiMixedDockFixture, FloatingDocksMovedToAScreenWithAnotherPixelRatio) {
  QScreen* other = nullptr;
  for (QScreen* screen : QGuiApplication::screens()) {
    if (screen->devicePixelRatio() != window_->devicePixelRatio()) {
      other = screen;
    }
  }
  if (other == nullptr) {
    if (qEnvironmentVariableIntValue("PJ_REQUIRE_MIXED_DPR") == 1) {
      FAIL() << "PJ_REQUIRE_MIXED_DPR=1 but no screen has another device-pixel ratio";
    }
    GTEST_SKIP() << "needs two screens with different device-pixel ratios (TECHNICAL_NOTES §8, mixed-DPR lane)";
  }
  if (QGuiApplication::platformName() == u"wayland") {
    GTEST_SKIP() << "the Wayland compositor places windows; run on xcb";
  }
  for (ads::CDockWidget* dock : docks()) {
    SCOPED_TRACE(dock->objectName().toStdString());
    ASSERT_NO_FATAL_FAILURE(floatDock(dock));
    QWindow* floating = dock->window()->windowHandle();
    ASSERT_NE(floating, nullptr);
    const qreal before = dock->widget()->devicePixelRatio();
    floating->setScreen(other);
    ASSERT_TRUE(QTest::qWaitFor([&] { return dock->widget()->devicePixelRatio() != before; }, 3000))
        << "the window system kept the window on its screen (" << QGuiApplication::platformName().toStdString() << ")";
    ASSERT_NO_FATAL_FAILURE(expectAllRender("moved to the other screen"));
  }
}

// The media viewer's base pipeline cannot be created: it clears to its
// background, shows the dock's warning and emits renderFailed() once, while the
// plot and the Scene3D view in the same window keep rendering; with the fault
// gone, the next initialize() (a new render-target size here) recovers.
class MediaViewerFailureTest : public RhiMixedDockFixture {
 protected:
  void SetUp() override {
    media_fail_pipeline_ = "base";
    expected_media_failures_ = 1;
    RhiMixedDockFixture::SetUp();
  }
};

TEST_F(MediaViewerFailureTest, BaseFailureShowsWarningAndRecovers) {
  PJ::MediaViewerWidget* viewer = mediaViewer();
  auto* warning = media_->findChild<QLabel*>(u"scene2dRenderingWarning"_s);
  ASSERT_NE(warning, nullptr);
  for (int frame = 0; frame < 3; ++frame) {
    const QImage image = presentAndGrab(viewer);
    ASSERT_FALSE(image.isNull()) << "the failed viewer presented nothing";
    EXPECT_TRUE(colourNear(image.pixel(image.width() / 2, image.height() / 2), viewer->clearColor()))
        << "the failed viewer drew something";
  }
  EXPECT_EQ(render_failures_[2], 1);
  EXPECT_TRUE(warning->isVisible());
  EXPECT_FALSE(warning->text().isEmpty());
  ASSERT_NO_FATAL_FAILURE(expectPlotRenders("beside the failed viewer"));
  ASSERT_NO_FATAL_FAILURE(expectSceneRenders("beside the failed viewer"));

  viewer->setProperty("pjTestFailPipeline", QVariant());
  window_->resize(window_->size() + QSize(40, 30));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !warning->isVisible(); }, 5000)) << "the viewer did not recover";
  ASSERT_NO_FATAL_FAILURE(expectAllRender("recovered"));
  EXPECT_EQ(render_failures_[2], 1);
}

// Without the optional blending pipeline, the image layer still draws (opaque).
class MediaViewerCompositeFailureTest : public RhiMixedDockFixture {
 protected:
  void SetUp() override {
    media_fail_pipeline_ = "composite";
    RhiMixedDockFixture::SetUp();
  }
};

TEST_F(MediaViewerCompositeFailureTest, ImageStillRendersWithoutBlending) {
  ASSERT_NO_FATAL_FAILURE(expectAllRender("composite pipeline failed"));
  EXPECT_FALSE(media_->findChild<QLabel*>(u"scene2dRenderingWarning"_s)->isVisible());
}

}  // namespace
