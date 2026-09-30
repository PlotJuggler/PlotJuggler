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
// viewer's failure state, beside working siblings. Registered per backend (PJ_GRAPHICS_API); needs a real display
// (skips on the offscreen QPA).

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>
#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDomDocument>
#include <QLabel>
#include <QMainWindow>
#include <QPen>
#include <QPointer>
#include <QRhiWidget>
#include <QScreen>
#include <QTest>
#include <algorithm>
#include <array>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_plotting/PlotRhiCanvas.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "scene3d_render_test_support.h"  // serializeCloud
#include "support/rhi_dock_test_support.h"

using namespace Qt::StringLiterals;
using namespace pj_rhi_dock_test;

namespace {

const QColor kCurveColour(230, 40, 40);
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
    PJ::PlotWidgetBase::setOpenGlDisabledOverride(false);
    window_ = std::make_unique<QMainWindow>();
    manager_ = new ads::CDockManager(window_.get());
    // As in rhi_dock_lifecycle_test: ADS's Wayland stays-on-top emulation can
    // hide a floated dock mid-test.
    window_->removeEventFilter(manager_);

    plot_dock_ = addDock(u"plot"_s, createPlot(), ads::LeftDockWidgetArea);
    scene_dock_ = addDock(u"scene3d"_s, createScene3D(), ads::RightDockWidgetArea);
    media_dock_ = addDock(u"media"_s, createMedia(), ads::BottomDockWidgetArea);
    const std::array<QRhiWidget*, 3> widgets{plotCanvas(), sceneView(), mediaViewer()};
    for (std::size_t i = 0; i < widgets.size(); ++i) {
      ASSERT_NE(widgets[i], nullptr) << "GPU widget " << i << " missing";
      QObject::connect(widgets[i], &QRhiWidget::renderFailed, window_.get(), [this, i] { ++render_failures_[i]; });
    }

    window_->resize(1000, 760);
    window_->show();
    if (!QTest::qWaitForWindowExposed(window_.get())) {
      PJ_SKIP_WITHOUT_GPU("the main window was never exposed");
    }
    (void)pjWaitForGrab([this](auto done) { sceneView()->grabPresentedFrame(std::move(done)); });  // one-shot auto-fit
    if (expected_media_failures_ == 0) {
      ASSERT_NO_FATAL_FAILURE(expectAllRender("initial layout"));
    }
  }

  void TearDown() override {
    window_.reset();
    QApplication::processEvents();
    if (IsSkipped()) {
      return;
    }
    EXPECT_TRUE(warnings_.rhiProblems().isEmpty()) << warnings_.rhiProblems().join(u'\n').toStdString();
    EXPECT_EQ(render_failures_[0], 0) << "the plot canvas failed";
    EXPECT_EQ(render_failures_[1], 0) << "the Scene3D view failed";
    EXPECT_EQ(render_failures_[2], expected_media_failures_) << "media viewer renderFailed() count";
  }

  ads::CDockWidget* addDock(const QString& name, QWidget* content, ads::DockWidgetArea area) {
    auto* dock = new ads::CDockWidget(manager_, name);
    dock->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
    manager_->addDockWidget(area, dock);
    return dock;
  }

  // A PlotWidget with one thick red curve from the datastore.
  QWidget* createPlot() {
    auto dataset = session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "plot.mcap"});
    EXPECT_TRUE(dataset.has_value());
    PJ::DataWriter writer = session_.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(*dataset, "/imu/x", PJ::NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    for (int i = 0; i < 20; ++i) {
      writer.appendScalar(*handle, 1'000'000'000 + i * 100'000'000LL, static_cast<double>(i % 5));
    }
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
    auto* plot = new PJ::PlotWidget(&session_, &catalog_);
    for (const auto& curve : catalog_.curves()) {
      auto* info = plot->addCurve(curve.name, kCurveColour);
      EXPECT_NE(info, nullptr);
      if (info != nullptr) {
        info->curve->setPen(QPen(kCurveColour, 3.0));
      }
    }
    plot->setGridVisible(false);
    plot->setLegendVisible(false);
    plot->zoomOut(/*emit_signal=*/false);
    plot_ = plot;
    return plot;
  }

  // A Scene3D dock with a solid-green cube-shaped point cloud, no grid or axes.
  QWidget* createScene3D() {
    auto* dock = new PJ::Scene3DDockWidget();
    dock->setSessionManager(&session_);
    const auto dataset =
        *session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "scene", .time_domain_id = 0});
    std::vector<glm::vec3> points;
    for (int i = 0; i < kCloudSide; ++i) {
      for (int j = 0; j < kCloudSide; ++j) {
        for (int k = 0; k < kCloudSide; ++k) {
          points.push_back(glm::vec3(i, j, k) / float(kCloudSide - 1) - 0.5F);
        }
      }
    }
    const auto topic = session_.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset,
            .topic_name = "/points",
            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"});
    EXPECT_TRUE(topic.has_value());
    EXPECT_TRUE(
        session_.objectStore().pushOwned(*topic, 100, pj::scene3d::test::serializeCloud(points, "map")).has_value());
    EXPECT_TRUE(dock->addTopic(*topic, PJ::sdk::BuiltinObjectType::kPointCloud, u"points"_s));
    // Through the persisted state: the layer's setters are private to its UI.
    if (auto* layer = dock->layerFor(*topic); layer != nullptr) {
      QDomDocument doc;
      QDomElement state = layer->xmlSaveState(doc);
      state.setAttribute(u"color_type"_s, u"solid"_s);
      state.setAttribute(u"color_choice_explicit"_s, u"true"_s);
      state.setAttribute(u"solid_color"_s, u"#00ff00"_s);
      state.setAttribute(u"size_meters"_s, u"0.1"_s);
      EXPECT_TRUE(layer->xmlLoadState(state));
    }
    dock->setFixedFrame(u"map"_s);
    EXPECT_NE(dock->sceneView(), nullptr);
    dock->sceneView()->setGridVisible(false);
    dock->sceneView()->setAxesVisible(false);
    scene_ = dock;
    return dock;
  }

  // A Scene2D dock showing one 64x64 rgba8 image: red top half, blue bottom half.
  QWidget* createMedia() {
    constexpr uint32_t kSize = 64;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSize * kSize * 4));
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        uint8_t* px = pixels.data() + static_cast<size_t>((y * kSize + x) * 4);
        px[0] = y < kSize / 2 ? 255 : 0;
        px[2] = y < kSize / 2 ? 0 : 255;
        px[3] = 255;
      }
    }
    PJ::sdk::Image image;
    image.timestamp_ns = 100;
    image.width = kSize;
    image.height = kSize;
    image.encoding = "rgba8";
    image.data = PJ::Span<const uint8_t>(pixels.data(), pixels.size());
    const auto dataset =
        *session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "media", .time_domain_id = 0});
    const auto topic = session_.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset, .topic_name = "/camera/image", .metadata_json = R"({"image_codec":"pj_image_v1"})"});
    EXPECT_TRUE(topic.has_value());
    EXPECT_TRUE(session_.objectStore().pushOwned(*topic, 100, PJ::serializeImage(image)).has_value());

    auto* dock = new PJ::Scene2DDockWidget();
    dock->setSessionManager(&session_);
    EXPECT_TRUE(dock->addTopic(*topic, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));
    auto* viewer = dock->findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
    EXPECT_NE(viewer, nullptr);
    if (media_fail_pipeline_ != nullptr) {
      viewer->setProperty("pjTestFailPipeline", QString::fromLatin1(media_fail_pipeline_));
    }
    dock->onTrackerTime(100.0 / 1'000'000'000.0);
    media_ = dock;
    return dock;
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
    EXPECT_EQ(view->lastPointVertexCountForTest(), kCloudPoints) << where;
    const int green = countPixels(image, [](QRgb p) { return qGreen(p) > qRed(p) + 50 && qGreen(p) > qBlue(p) + 50; });
    EXPECT_GT(green, 200) << where << ": no point cloud drawn";
  }

  // The image, top on top (Vulkan's clip space is y-down). Frames are decoded
  // asynchronously, so this waits for the first one.
  void expectMediaRenders(const char* where) {
    QImage image;
    const auto shows_image = [&] {
      image = presentAndGrab(mediaViewer());
      const int cx = image.width() / 2;
      const int offset = std::min(image.width(), image.height()) * 3 / 10;
      return !image.isNull() && colourNear(image.pixel(cx, image.height() / 2 - offset), Qt::red) &&
             colourNear(image.pixel(cx, image.height() / 2 + offset), Qt::blue);
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

  // Set by a derived fixture before SetUp(): the media viewer pipeline to fail.
  const char* media_fail_pipeline_ = nullptr;
  int expected_media_failures_ = 0;

  // Declaration order is destruction order reversed: the window (and the widgets
  // pointing at the session) dies first.
  WarningCollector warnings_;
  PJ::SessionManager session_;
  PJ::CatalogModel catalog_{&session_};
  std::array<int, 3> render_failures_{};  // plot, Scene3D, media
  std::unique_ptr<QMainWindow> window_;
  ads::CDockManager* manager_ = nullptr;
  QPointer<PJ::PlotWidget> plot_;
  QPointer<PJ::Scene3DDockWidget> scene_;
  QPointer<PJ::Scene2DDockWidget> media_;
  ads::CDockWidget* plot_dock_ = nullptr;
  ads::CDockWidget* scene_dock_ = nullptr;
  ads::CDockWidget* media_dock_ = nullptr;
};

TEST_F(RhiMixedDockFixture, FloatAndRedockEachKeepsEveryWidgetRendering) {
  for (ads::CDockWidget* dock : docks()) {
    SCOPED_TRACE(dock->objectName().toStdString());
    const int docked = adoptions(dock);
    ASSERT_NO_FATAL_FAILURE(floatDock(dock));
    EXPECT_NE(dock->window(), window_.get());
    ASSERT_NO_FATAL_FAILURE(expectAllRender("one dock floating"));
    EXPECT_GT(adoptions(dock), docked) << "no initialize() on the floating window's QRhi";
    const int floating = adoptions(dock);
    manager_->addDockWidget(ads::BottomDockWidgetArea, dock);
    ASSERT_EQ(dock->window(), window_.get());
    ASSERT_NO_FATAL_FAILURE(expectAllRender("redocked"));
    EXPECT_GT(adoptions(dock), floating) << "no initialize() on the main window's QRhi";
  }
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

// Needs two screens with different device-pixel ratios, e.g. two gnome-shell
// virtual monitors plus QT_SCREEN_SCALE_FACTORS="1;2"; skips otherwise.
TEST_F(RhiMixedDockFixture, FloatingDocksMovedToAScreenWithAnotherPixelRatio) {
  QScreen* other = nullptr;
  for (QScreen* screen : QGuiApplication::screens()) {
    if (screen->devicePixelRatio() != window_->devicePixelRatio()) {
      other = screen;
    }
  }
  if (other == nullptr) {
    GTEST_SKIP() << "needs two screens with different device-pixel ratios";
  }
  for (ads::CDockWidget* dock : docks()) {
    SCOPED_TRACE(dock->objectName().toStdString());
    ASSERT_NO_FATAL_FAILURE(floatDock(dock));
    QWindow* floating = dock->window()->windowHandle();
    ASSERT_NE(floating, nullptr);
    const qreal before = dock->widget()->devicePixelRatio();
    floating->setScreen(other);
    if (!QTest::qWaitFor([&] { return dock->widget()->devicePixelRatio() != before; }, 3000)) {
      GTEST_SKIP() << "the window system kept the window on its screen ("
                   << QGuiApplication::platformName().toStdString() << ")";
    }
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
