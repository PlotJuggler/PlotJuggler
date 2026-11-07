// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// QRhiWidgets in Qt-Advanced-Docking docks: float/redock, split, sibling
// destruction, closing a floating window and a device-pixel-ratio change must
// leave every widget rendering, with its QRhi resources released before each
// QRhi switch. Stand-in widgets clear to a known colour; the real Scene2D media
// viewer (when built) also checks image orientation and overlay survival.
// Registered per backend (PJ_GRAPHICS_API); needs a real display (skips on
// the offscreen QPA).

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QOpenGLWidget>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRhiWidget>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTest>
#include <QWindow>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_widgets/GraphicsApi.h"
#include "support/rhi_dock_test_support.h"

#ifdef PJ_WITH_SCENE2D
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#endif

using namespace Qt::StringLiterals;

namespace {

using namespace pj_rhi_dock_test;

// Outlives its widget, so a destroyed widget's balance stays checkable.
struct Lifecycle {
  int inits = 0;     ///< initialize() calls that met a new QRhi
  int releases = 0;  ///< releaseResources() calls that freed something
  int stale = 0;     ///< a new QRhi arrived while old resources were still held
  int frames = 0;    ///< render() calls
  int live = 0;      ///< resources currently held
  std::vector<QString> backends;
};

// Clears to one colour and holds one QRhiBuffer per QRhi, like a real GPU widget.
class StandInRhiWidget final : public QRhiWidget {
 public:
  StandInRhiWidget(QColor colour, Lifecycle& lifecycle, std::optional<Api> api = std::nullopt)
      : colour_(colour), lifecycle_(lifecycle) {
    setApi(api.value_or(PJ::preferredGraphicsApi()));
    setMinimumSize(120, 90);
  }
  ~StandInRhiWidget() override {
    // Qt does not call releaseResources() on destruction.
    releaseResources();
  }
  StandInRhiWidget(const StandInRhiWidget&) = delete;
  StandInRhiWidget& operator=(const StandInRhiWidget&) = delete;

 protected:
  void initialize(QRhiCommandBuffer* /*cb*/) override {
    if (rhi() == seen_rhi_ && buffer_ != nullptr) {
      return;
    }
    if (buffer_ != nullptr) {
      ++lifecycle_.stale;
      releaseResources();
    }
    seen_rhi_ = rhi();
    buffer_ = rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64);
    buffer_->create();
    ++lifecycle_.inits;
    ++lifecycle_.live;
    lifecycle_.backends.emplace_back(QString::fromLatin1(rhi()->backendName()));
  }

  void render(QRhiCommandBuffer* cb) override {
    cb->beginPass(renderTarget(), colour_, {1.0f, 0});
    cb->endPass();
    ++lifecycle_.frames;
  }

  void releaseResources() override {
    if (buffer_ != nullptr) {
      delete buffer_;
      buffer_ = nullptr;
      ++lifecycle_.releases;
      --lifecycle_.live;
    }
  }

 private:
  QColor colour_;
  Lifecycle& lifecycle_;
  QRhi* seen_rhi_ = nullptr;
  QRhiBuffer* buffer_ = nullptr;
};

[[nodiscard]] ::testing::AssertionResult rendersColour(QRhiWidget* widget, QColor colour) {
  const QImage image = presentAndGrab(widget);
  if (image.isNull()) {
    return ::testing::AssertionFailure() << "no frame presented";
  }
  const QRgb centre = image.pixel(image.width() / 2, image.height() / 2);
  if (!colourNear(centre, colour)) {
    return ::testing::AssertionFailure() << "centre is " << QColor(centre).name().toStdString() << ", expected "
                                         << colour.name().toStdString();
  }
  return ::testing::AssertionSuccess();
}

// One main window with a dock manager and two stand-in docks side by side.
class RhiDockFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
    window_ = std::make_unique<QMainWindow>();
    manager_ = new ads::CDockManager(window_.get());
    // On Wayland, ADS's stays-on-top emulation calls setWindowFlag() on every
    // visible floating container when the main window's activation changes, which
    // hides it (pj_scene3D/docs/TECHNICAL_NOTES.md, ADS quirks); a floated dock can then vanish
    // mid-test. Not what these tests are about.
    window_->removeEventFilter(manager_);
    red_dock_ = addStandIn(u"red"_s, Qt::red, red_, ads::LeftDockWidgetArea);
    green_dock_ = addStandIn(u"green"_s, Qt::green, green_, ads::RightDockWidgetArea);
    window_->resize(640, 400);
    window_->show();
    if (!QTest::qWaitForWindowExposed(window_.get())) {
      PJ_SKIP_WITHOUT_GPU("the main window was never exposed");
    }
    ASSERT_TRUE(rendersColour(red(), Qt::red));
    ASSERT_TRUE(rendersColour(green(), Qt::green));
  }

  void TearDown() override {
    window_.reset();
    QApplication::processEvents();
    EXPECT_TRUE(warnings_.rhiProblems().isEmpty()) << warnings_.rhiProblems().join(u'\n').toStdString();
  }

  ads::CDockWidget* addStandIn(
      const QString& name, QColor colour, Lifecycle& lifecycle, ads::DockWidgetArea area,
      ads::CDockAreaWidget* target = nullptr) {
    auto* dock = new ads::CDockWidget(manager_, name);
    dock->setWidget(new StandInRhiWidget(colour, lifecycle), ads::CDockWidget::ForceNoScrollArea);
    manager_->addDockWidget(area, dock, target);
    return dock;
  }

  [[nodiscard]] static QRhiWidget* rhiWidgetOf(ads::CDockWidget* dock) {
    return qobject_cast<QRhiWidget*>(dock->widget());
  }
  [[nodiscard]] QRhiWidget* red() const {
    return rhiWidgetOf(red_dock_);
  }
  [[nodiscard]] QRhiWidget* green() const {
    return rhiWidgetOf(green_dock_);
  }

  // Members, not test-body locals: the docked widgets live until TearDown.
  WarningCollector warnings_;
  Lifecycle red_;
  Lifecycle green_;
  Lifecycle blue_;
#ifdef PJ_WITH_SCENE2D
  std::unique_ptr<PJ::MediaSource> media_source_;
#endif
  std::unique_ptr<QMainWindow> window_;
  ads::CDockManager* manager_ = nullptr;
  ads::CDockWidget* red_dock_ = nullptr;
  ads::CDockWidget* green_dock_ = nullptr;
};

TEST_F(RhiDockFixture, FloatAndRedockKeepsRenderingAndBalancesResources) {
  ASSERT_NO_FATAL_FAILURE(floatDock(red_dock_));
  EXPECT_NE(red()->window(), window_.get());
  EXPECT_TRUE(rendersColour(red(), Qt::red));
  EXPECT_EQ(red_.inits, 2);
  EXPECT_EQ(red_.releases, 1);

  manager_->addDockWidget(ads::LeftDockWidgetArea, red_dock_);
  ASSERT_EQ(red()->window(), window_.get());
  EXPECT_TRUE(rendersColour(red(), Qt::red));
  EXPECT_EQ(red_.inits, 3);
  EXPECT_EQ(red_.releases, 2);
  EXPECT_EQ(red_.stale, 0);

  // The sibling never changed top-level, so it kept its one QRhi.
  EXPECT_TRUE(rendersColour(green(), Qt::green));
  EXPECT_EQ(green_.inits, 1);
  EXPECT_EQ(green_.releases, 0);
  const QString expected = red_.backends.front();
  for (const QString& backend : red_.backends) {
    EXPECT_EQ(backend, expected);
  }
}

TEST_F(RhiDockFixture, SplittingAndMovingWithinTheWindowKeepRendering) {
  auto* blue_dock = addStandIn(u"blue"_s, Qt::blue, blue_, ads::BottomDockWidgetArea, green_dock_->dockAreaWidget());
  EXPECT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));

  manager_->addDockWidget(ads::TopDockWidgetArea, red_dock_, green_dock_->dockAreaWidget());
  ASSERT_EQ(red()->window(), window_.get());
  EXPECT_TRUE(rendersColour(red(), Qt::red));
  EXPECT_TRUE(rendersColour(green(), Qt::green));
  EXPECT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));
  // ADS parks a moving dock outside the window, so even a move within one window
  // costs one release/initialize cycle.
  EXPECT_EQ(red_.inits, red_.releases + 1);
  EXPECT_EQ(red_.stale, 0);
}

// A dock-area tab switch hides the previous tab but keeps it parented (our ADS
// patch, pj_scene3D/docs/TECHNICAL_NOTES.md §5): a hidden tab stays in the
// window with its resources and renders again without a new initialize().
TEST_F(RhiDockFixture, HiddenTabsKeepTheirResources) {
  auto* blue_dock = new ads::CDockWidget(manager_, u"blue"_s);
  blue_dock->setWidget(new StandInRhiWidget(Qt::blue, blue_), ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidgetTabToArea(blue_dock, green_dock_->dockAreaWidget());
  blue_dock->setAsCurrentTab();
  EXPECT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));
  EXPECT_FALSE(green()->isVisible());
  EXPECT_EQ(green()->window(), window_.get());
  EXPECT_EQ(green_.live, 1);

  for (int round = 0; round < 3; ++round) {
    green_dock_->setAsCurrentTab();
    EXPECT_TRUE(rendersColour(green(), Qt::green));
    blue_dock->setAsCurrentTab();
    EXPECT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));
  }
  EXPECT_EQ(green_.inits, 1);
  EXPECT_EQ(green_.releases, 0);
  EXPECT_EQ(blue_.inits, 1);
  EXPECT_EQ(blue_.releases, 0);
  EXPECT_EQ(green_.stale + blue_.stale, 0);
}

// The ADS operations around kept (hidden, parented) tabs: reordering, closing
// and reopening a hidden and the current tab, a state save/restore, floating the
// whole area, moving a hidden tab to another window and destroying a hidden tab. Every tab that is shown renders its
// colour, none ever meets a new QRhi holding the old one's resources, and none holds more than one.
TEST_F(RhiDockFixture, OperationsOnKeptTabsKeepRendering) {
  Lifecycle cyan;
  auto* blue_dock = new ads::CDockWidget(manager_, u"blue"_s);
  blue_dock->setWidget(new StandInRhiWidget(Qt::blue, blue_), ads::CDockWidget::ForceNoScrollArea);
  auto* cyan_dock = new ads::CDockWidget(manager_, u"cyan"_s);
  cyan_dock->setWidget(new StandInRhiWidget(Qt::cyan, cyan), ads::CDockWidget::ForceNoScrollArea);
  ads::CDockAreaWidget* area = green_dock_->dockAreaWidget();
  manager_->addDockWidgetTabToArea(blue_dock, area);
  manager_->addDockWidgetTabToArea(cyan_dock, area);
  const std::vector<std::pair<ads::CDockWidget*, QColor>> tabs{
      {green_dock_, Qt::green}, {blue_dock, Qt::blue}, {cyan_dock, Qt::cyan}};
  const auto current_renders = [&](const char* where) {
    for (const auto& [dock, colour] : tabs) {
      if (dock->isCurrentTab() && !dock->isClosed()) {
        EXPECT_TRUE(rendersColour(rhiWidgetOf(dock), colour)) << where << ": " << dock->objectName().toStdString();
      }
    }
    for (const Lifecycle* lifecycle : {&green_, &blue_, &cyan}) {
      EXPECT_EQ(lifecycle->stale, 0) << where;
      EXPECT_LE(lifecycle->live, 1) << where;
    }
  };
  for (const auto& [dock, colour] : tabs) {
    dock->setAsCurrentTab();
    current_renders("first visit");
  }

  // Dragging the current tab to the front of the bar.
  ASSERT_TRUE(QMetaObject::invokeMethod(area, "reorderDockWidget", Q_ARG(int, 2), Q_ARG(int, 0)));
  EXPECT_TRUE(cyan_dock->isCurrentTab());
  current_renders("reordered");

  blue_dock->toggleView(false);  // a hidden tab
  current_renders("hidden tab closed");
  blue_dock->toggleView(true);
  EXPECT_TRUE(blue_dock->isCurrentTab());
  current_renders("hidden tab reopened");
  blue_dock->toggleView(false);  // the current tab
  current_renders("current tab closed");
  blue_dock->toggleView(true);
  current_renders("current tab reopened");
  EXPECT_EQ(green_.inits + blue_.inits + cyan.inits, 3) << "a tab operation within the window re-initialized";

  // Before any floating window exists: restoring one hides and re-shows it at
  // once, which Vulkan on Wayland does not survive (TECHNICAL_NOTES §5).
  const QByteArray state = manager_->saveState();
  ASSERT_TRUE(manager_->restoreState(state));
  QApplication::processEvents();
  current_renders("state restored");
  EXPECT_TRUE(rendersColour(red(), Qt::red));

  area = green_dock_->dockAreaWidget();  // the restore replaced every area
  area->setFloating();                   // every tab, kept ones included, moves to the new window
  ASSERT_TRUE(QTest::qWaitForWindowExposed(blue_dock->window()));
  EXPECT_NE(green()->window(), window_.get());
  for (const auto& [dock, colour] : tabs) {
    dock->setAsCurrentTab();
    current_renders("area floating");
  }

  manager_->addDockWidget(ads::RightDockWidgetArea, green_dock_, red_dock_->dockAreaWidget());  // a hidden tab
  ASSERT_EQ(green()->window(), window_.get());
  current_renders("hidden tab moved back to the main window");
  EXPECT_TRUE(rendersColour(green(), Qt::green));

  cyan_dock->setAsCurrentTab();
  ASSERT_TRUE(rendersColour(rhiWidgetOf(cyan_dock), Qt::cyan));
  manager_->removeDockWidget(blue_dock);  // hidden behind cyan
  delete blue_dock;
  QApplication::processEvents();
  EXPECT_EQ(blue_.live, 0);
  EXPECT_EQ(blue_.inits, blue_.releases);
  EXPECT_TRUE(rendersColour(rhiWidgetOf(cyan_dock), Qt::cyan));
  manager_->removeDockWidget(cyan_dock);
  delete cyan_dock;  // before `cyan` goes out of scope
  QApplication::processEvents();
  EXPECT_EQ(cyan.live, 0);
}

TEST_F(RhiDockFixture, DestroyingASiblingLeavesTheOthersRendering) {
  manager_->removeDockWidget(green_dock_);
  delete green_dock_;
  green_dock_ = nullptr;
  QApplication::processEvents();
  EXPECT_EQ(green_.live, 0);
  EXPECT_EQ(green_.inits, green_.releases);
  EXPECT_TRUE(rendersColour(red(), Qt::red));

  // And the same for a sibling destroyed inside a floating window.
  auto* blue_dock = addStandIn(u"blue"_s, Qt::blue, blue_, ads::RightDockWidgetArea);
  ASSERT_NO_FATAL_FAILURE(floatDock(red_dock_));
  manager_->addDockWidget(ads::RightDockWidgetArea, blue_dock, red_dock_->dockAreaWidget());
  ASSERT_EQ(rhiWidgetOf(blue_dock)->window(), red()->window());
  EXPECT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));
  manager_->removeDockWidget(blue_dock);
  delete blue_dock;
  QApplication::processEvents();
  EXPECT_EQ(blue_.live, 0);
  EXPECT_TRUE(rendersColour(red(), Qt::red));
  EXPECT_EQ(red_.stale, 0);
}

TEST_F(RhiDockFixture, ClosingAFloatingWindowAndReopeningTheDockRendersAgain) {
  ASSERT_NO_FATAL_FAILURE(floatDock(red_dock_));
  QPointer<ads::CFloatingDockContainer> floating = red_dock_->floatingDockContainer();
  ASSERT_NE(floating, nullptr);
  floating->close();
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return floating.isNull() || floating->windowHandle() == nullptr || !floating->windowHandle()->isExposed();
  }));
  // Reopen only after the compositor has applied the unmap. Within a frame of a
  // Vulkan present (Mesa's FIFO queues it) the new mapping is a Wayland protocol
  // error that drops the connection; see pj_scene3D/docs/TECHNICAL_NOTES.md, ADS quirks.
  QTest::qWait(100);
  EXPECT_TRUE(red_dock_->isClosed());

  red_dock_->toggleView(true);
  ASSERT_TRUE(QTest::qWaitForWindowExposed(red()->window()));
  EXPECT_TRUE(rendersColour(red(), Qt::red));
  EXPECT_EQ(red_.stale, 0);
  EXPECT_LE(red_.live, 1);

  // Destroying the dock while it floats releases what it holds.
  manager_->removeDockWidget(red_dock_);
  delete red_dock_;
  red_dock_ = nullptr;
  QApplication::processEvents();
  EXPECT_EQ(red_.live, 0);
  EXPECT_TRUE(rendersColour(green(), Qt::green));
}

// A floating dock moved to a screen with another device-pixel ratio. Needs two
// screens with different ratios on xcb: a headless mutter's Xwayland with two
// virtual monitors plus QT_SCREEN_SCALE_FACTORS="1;2" (linux-ci's mixed-DPR
// step). Skips without them (fails with PJ_REQUIRE_MIXED_DPR=1); a move that does
// not change the ratio fails. The whole-suite dpr2 run covers a uniform ratio.
TEST_F(RhiDockFixture, FloatingDockMovedToAScreenWithAnotherPixelRatio) {
  const QList<QScreen*> screens = QGuiApplication::screens();
  QScreen* other = nullptr;
  for (QScreen* screen : screens) {
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
  ASSERT_NO_FATAL_FAILURE(floatDock(red_dock_));
  QWindow* floating = red()->window()->windowHandle();
  ASSERT_NE(floating, nullptr);
  const qreal before = red()->devicePixelRatio();
  floating->setScreen(other);
  ASSERT_TRUE(QTest::qWaitFor([&] { return red()->devicePixelRatio() != before; }, 3000))
      << "the window system kept the window on its screen (" << QGuiApplication::platformName().toStdString() << ")";
  const QImage image = presentAndGrab(red());
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.size(), (red()->size() * red()->devicePixelRatio()));
  EXPECT_TRUE(colourNear(image.pixel(image.width() / 2, image.height() / 2), Qt::red));
  EXPECT_EQ(red_.stale, 0);
}

#ifdef PJ_WITH_SCENE2D
// Image top on top (Vulkan's clip space is y-down) and the overlay present, in
// the main window and again after a float, a redock and a tab switch.
TEST_F(RhiDockFixture, MediaViewerKeepsOrientationAndOverlaysAcrossReparenting) {
  media_source_ = std::make_unique<OneShotMediaSource>(orientationFrame());
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setMediaSource(media_source_.get());
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);

  expectOrientationFrame(viewer, "docked");
  ASSERT_NO_FATAL_FAILURE(floatDock(dock));
  expectOrientationFrame(viewer, "floating");
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);
  expectOrientationFrame(viewer, "redocked");

  // A tab switch reparents at an unchanged size, so nothing else forces the
  // overlay geometry to be rebuilt.
  auto* blue_dock = new ads::CDockWidget(manager_, u"blue"_s);
  blue_dock->setWidget(new StandInRhiWidget(Qt::blue, blue_), ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidgetTabToArea(blue_dock, dock->dockAreaWidget());
  blue_dock->setAsCurrentTab();
  ASSERT_TRUE(rendersColour(rhiWidgetOf(blue_dock), Qt::blue));
  dock->setAsCurrentTab();
  expectOrientationFrame(viewer, "tab shown again");
}

// An overlay pipeline that cannot be created drops that overlay while the image
// draws; once the fault is gone the next initialize() (a resize, same QRhi)
// rebuilds it. Needs the viewer's test hooks (PJ_RHI_TEST_HOOKS).
TEST_F(RhiDockFixture, DroppedMediaOverlayReturnsOnTheNextInitialize) {
  media_source_ = std::make_unique<OneShotMediaSource>(orientationFrame());
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setProperty("pjTestFailPipeline", u"points"_s);  // the circle's fill
  viewer->setMediaSource(media_source_.get());
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);

  QImage image;
  ASSERT_TRUE(
      QTest::qWaitFor(
          [&] {
            image = presentAndGrab(viewer);
            return !image.isNull() && colourNear(image.pixel(image.width() / 2, image.height() / 5), Qt::red);
          },
          5000))
      << "the image did not draw without its overlay";
  EXPECT_FALSE(colourNear(image.pixel(image.width() / 2, image.height() / 2), Qt::green)) << "the failed overlay drew";

  viewer->setProperty("pjTestFailPipeline", QVariant());
  const int adoptions = viewer->property("pjTestRhiAdoptions").toInt();
  const QSize before = viewer->size();
  window_->resize(window_->size() + QSize(40, 30));
  ASSERT_TRUE(QTest::qWaitFor([&] { return viewer->size() != before; }, 5000));
  expectOrientationFrame(viewer, "overlay rebuilt");
  EXPECT_EQ(viewer->property("pjTestRhiAdoptions").toInt(), adoptions) << "the rebuild needed a new QRhi";
}

// 32x32 metric depth, 2 m everywhere, colormapped over 0.5-4 m.
[[nodiscard]] PJ::MediaFrame depthFrame() {
  constexpr int kSize = 32;
  PJ::DecodedFrame image;
  image.width = kSize;
  image.height = kSize;
  image.format = PJ::PixelFormat::kDepthR32F;
  std::vector<float> depth(static_cast<size_t>(kSize * kSize), 2.0F);
  image.pixels = std::make_shared<std::vector<uint8_t>>(
      reinterpret_cast<const uint8_t*>(depth.data()), reinterpret_cast<const uint8_t*>(depth.data() + depth.size()));
  image.depth = PJ::DepthColorParams{.near_m = 0.5F, .far_m = 4.0F, .active = true};
  PJ::MediaFrame frame;
  frame.base = image;
  return frame;
}

// A frame texture whose bindings cannot be rebuilt drops that frame. A depth frame
// queues its colormap upload before the rebuild, so the dropped textures are still
// referenced by a pending upload: it must be discarded with them, never submitted.
// The failed frame draws nothing, and the retained one draws on the next QRhi.
TEST_F(RhiDockFixture, FrameWhoseBindingsFailIsDroppedWithItsUploads) {
  media_source_ = std::make_unique<OneShotMediaSource>(depthFrame());
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setProperty("pjTestFailPipeline", u"layer srb"_s);
  viewer->setMediaSource(media_source_.get());
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);

  const QImage failed = presentAndGrab(viewer);
  ASSERT_FALSE(failed.isNull());
  EXPECT_TRUE(colourNear(failed.pixel(failed.width() / 2, failed.height() / 2), viewer->clearColor()))
      << "the dropped frame drew";

  viewer->setProperty("pjTestFailPipeline", QVariant());
  ASSERT_NO_FATAL_FAILURE(floatDock(dock));
  QImage image;
  EXPECT_TRUE(
      QTest::qWaitFor(
          [&] {
            image = presentAndGrab(viewer);
            return !image.isNull() &&
                   !colourNear(image.pixel(image.width() / 2, image.height() / 2), viewer->clearColor());
          },
          5000))
      << "the retained frame did not draw on the new QRhi";
}
// Hands out queued frames, one per takeFrame().
class QueuedMediaSource final : public PJ::MediaSource {
 public:
  void push(PJ::MediaFrame frame) {
    frames_.push_back(std::move(frame));
  }
  void setTimestamp(int64_t /*ts_ns*/) override {}
  std::optional<PJ::MediaFrame> takeFrame() override {
    if (frames_.empty()) {
      return std::nullopt;
    }
    PJ::MediaFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
  }

 private:
  std::deque<PJ::MediaFrame> frames_;
};

// A frame above the GPU's texture limit (lowered for the test: the real one
// needs an image of gigabytes) is not drawn; one within it draws.
TEST_F(RhiDockFixture, MediaFrameAboveTheTextureLimitIsNotDrawn) {
  auto source = std::make_unique<QueuedMediaSource>();
  QueuedMediaSource& queue = *source;
  media_source_ = std::move(source);
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setMaxTextureSizeForTest(32);  // orientationFrame() is 64 x 64
  queue.push(orientationFrame());
  viewer->setMediaSource(&queue);
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);

  const QImage rejected = presentAndGrab(viewer);
  ASSERT_FALSE(rejected.isNull());
  EXPECT_TRUE(colourNear(rejected.pixel(rejected.width() / 2, rejected.height() / 5), viewer->clearColor()))
      << "the oversize frame drew";

  viewer->setMaxTextureSizeForTest(64);
  queue.push(orientationFrame());
  viewer->update();
  expectOrientationFrame(viewer, "within the limit");
}

// A composite whose one layer changed re-uploads that layer only: the other
// brings the very pixel buffer its textures already hold.
TEST_F(RhiDockFixture, MediaCompositeUploadsOnlyTheChangedLayer) {
  auto source = std::make_unique<QueuedMediaSource>();
  QueuedMediaSource& queue = *source;
  media_source_ = std::move(source);
  const PJ::DecodedFrame image = *orientationFrame().base;
  const auto composite = [&image](const PJ::DecodedFrame& top) {
    PJ::MediaFrame frame;
    frame.pixel_layers = {PJ::PixelLayer{.frame = image}, PJ::PixelLayer{.frame = top, .opacity = 0.5F}};
    return frame;
  };
  PJ::DecodedFrame top = image;
  top.pixels = std::make_shared<std::vector<uint8_t>>(*image.pixels);
  queue.push(composite(top));
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setMediaSource(&queue);
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);
  ASSERT_FALSE(presentAndGrab(viewer).isNull());
  const std::uint64_t first = viewer->frameUploadCountForTest();
  EXPECT_EQ(first, 2U);

  PJ::DecodedFrame next_top = image;
  next_top.pixels = std::make_shared<std::vector<uint8_t>>(*image.pixels);
  queue.push(composite(next_top));
  viewer->update();
  ASSERT_FALSE(presentAndGrab(viewer).isNull());
  EXPECT_EQ(viewer->frameUploadCountForTest(), first + 1U);
}

// More distinct labels than the glyph cache holds (512): the same labels in the
// next frame are drawn without rasterizing any of them again, and a frame of
// other labels evicts the unused ones, leaving only what it draws.
TEST_F(RhiDockFixture, MediaLabelsBeyondTheCacheBoundAreNotRasterizedAgain) {
  constexpr int kLabels = 600;
  const auto labelled = [](int first) {
    PJ::MediaFrame frame = orientationFrame();
    std::vector<PJ::TextAnnotation>& texts = frame.overlays.front().annotations.front().texts;
    for (int i = 0; i < kLabels; ++i) {
      texts.push_back(PJ::TextAnnotation{.position = {2.0, 2.0}, .font_size = 8.0, .text = std::to_string(first + i)});
    }
    return frame;
  };
  auto source = std::make_unique<QueuedMediaSource>();
  QueuedMediaSource& queue = *source;
  media_source_ = std::move(source);
  queue.push(labelled(0));
  auto* viewer = new PJ::MediaViewerWidget();
  viewer->setMediaSource(&queue);
  auto* dock = new ads::CDockWidget(manager_, u"media"_s);
  dock->setWidget(viewer, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(ads::BottomDockWidgetArea, dock);
  ASSERT_FALSE(presentAndGrab(viewer).isNull());
  const std::uint64_t first = viewer->labelRasterizationsForTest();
  ASSERT_EQ(first, std::uint64_t{kLabels});

  queue.push(labelled(0));
  viewer->update();
  ASSERT_FALSE(presentAndGrab(viewer).isNull());
  EXPECT_EQ(viewer->labelRasterizationsForTest(), first) << "unchanged labels were rasterized again";

  queue.push(labelled(kLabels));
  viewer->update();
  ASSERT_FALSE(presentAndGrab(viewer).isNull());
  EXPECT_EQ(viewer->labelRasterizationsForTest(), first + kLabels);
  EXPECT_EQ(viewer->labelCacheSizeForTest(), std::size_t{kLabels}) << "unused labels kept past the bound";
}

// Records the rectification path the viewer selects for its source.
class RectifyProbeSource final : public PJ::MediaSource {
 public:
  explicit RectifyProbeSource(PJ::MediaFrame frame) : frame_(std::move(frame)) {}
  void setTimestamp(int64_t /*ts_ns*/) override {}
  std::optional<PJ::MediaFrame> takeFrame() override {
    return std::exchange(frame_, std::nullopt);
  }
  void setGpuRectificationAvailable(bool available) override {
    gpu_rectify = available;
  }
  std::optional<bool> gpu_rectify;

 private:
  std::optional<PJ::MediaFrame> frame_;
};

class ProbingMediaViewer final : public PJ::MediaViewerWidget {
 public:
  using PJ::MediaViewerWidget::rhi;
};

// WebGL2 is OpenGL ES 3.0, where Qt's GLES backend reports no 32-bit integer
// texture format (it lists them from ES 3.1 on) though the rectification LUT's
// RG32UI is core ES 3.0. Mesa serves such a context (an ES context capped at
// 3.0), on which the viewer must still select GPU rectification. The case
// re-runs itself in a child process, since the GL integration is chosen when
// the application starts.
TEST(MediaViewerGlesTest, SelectsGpuRectificationOnOpenGlEs30) {
  constexpr char kChild[] = "PJ_TEST_GLES30_CHILD";
  if (!qEnvironmentVariableIsSet(kChild)) {
    PJ_SKIP_ON_HEADLESS_QPA();
    if (QGuiApplication::platformName() != u"xcb"_s || PJ::preferredGraphicsApi() != QRhiWidget::Api::OpenGL) {
      GTEST_SKIP() << "needs Mesa OpenGL ES through xcb";
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kChild), u"1"_s);
    env.insert(u"QT_XCB_GL_INTEGRATION"_s, u"xcb_egl"_s);
    env.insert(u"MESA_GLES_VERSION_OVERRIDE"_s, u"3.0"_s);
    QProcess child;
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {u"--gtest_filter=MediaViewerGlesTest.*"_s});
    ASSERT_TRUE(child.waitForFinished(60000));
    const QString output = QString::fromLocal8Bit(child.readAll());
    if (output.contains(u"[  SKIPPED ]"_s)) {
      GTEST_SKIP() << output.toStdString();
    }
    EXPECT_EQ(child.exitCode(), 0) << output.toStdString();
    EXPECT_TRUE(output.contains(u"[  PASSED  ] 1 test"_s));
    return;
  }
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setRenderableType(QSurfaceFormat::OpenGLES);
  format.setVersion(3, 0);
  QSurfaceFormat::setDefaultFormat(format);
  RectifyProbeSource source(orientationFrame());
  QWidget window;
  window.resize(200, 200);
  auto* layout = new QHBoxLayout(&window);
  auto* viewer = new ProbingMediaViewer();
  viewer->setMediaSource(&source);
  layout->addWidget(viewer);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  ASSERT_FALSE(presentAndGrab(viewer).isNull()) << "no frame on the OpenGL ES context";
  QRhi* rhi = viewer->rhi();
  ASSERT_NE(rhi, nullptr);
  if (rhi->isTextureFormatSupported(QRhiTexture::RG32UI)) {
    GTEST_SKIP() << "the context is not capped at OpenGL ES 3.0";
  }
  ASSERT_TRUE(source.gpu_rectify.has_value()) << "the viewer selected no rectification path";
  EXPECT_TRUE(*source.gpu_rectify) << "the viewer rectifies on the CPU";
}

#endif

// One top-level can composite with only one API. A QOpenGLWidget always asks for
// OpenGL, so a non-GL QRhiWidget beside it (Vulkan where an instance can be made,
// else Qt's Null backend) cannot render: Qt logs the mismatch and the widget
// reports renderFailed(); nothing crashes and the GL widget still paints.
[[nodiscard]] QRhiWidget::Api nonGlApi() {
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
  QVulkanInstance probe;
  if (probe.create()) {
    return QRhiWidget::Api::Vulkan;
  }
#endif
  return QRhiWidget::Api::Null;
}

class ClearingGlWidget final : public QOpenGLWidget {
 public:
  int paints = 0;

 protected:
  void paintGL() override {
    ++paints;
  }
};

TEST(MixedApiWindowTest, NonGlRhiWidgetBesideAnOpenGlWidgetFailsWithoutCrashing) {
  PJ_SKIP_ON_HEADLESS_QPA();
  WarningCollector warnings;
  Lifecycle lifecycle;
  QWidget window;
  window.resize(400, 200);
  auto* layout = new QHBoxLayout(&window);
  auto* gl = new ClearingGlWidget();  // first: the window's API is decided by the first GPU widget found
  layout->addWidget(gl);
  auto* rhi_widget = new StandInRhiWidget(Qt::red, lifecycle, nonGlApi());
  layout->addWidget(rhi_widget);
  int failed = 0;
  QObject::connect(rhi_widget, &QRhiWidget::renderFailed, [&failed] { ++failed; });
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  rhi_widget->update();
  (void)QTest::qWaitFor([&] { return failed > 0 && gl->paints > 0; }, 3000);

  EXPECT_GT(gl->paints, 0);
  EXPECT_GT(failed, 0);
  EXPECT_EQ(lifecycle.frames, 0);
  EXPECT_EQ(lifecycle.inits, 0);
  const QStringList problems = warnings.rhiProblems();
  EXPECT_TRUE(std::any_of(problems.cbegin(), problems.cend(), [](const QString& message) {
    return message.contains(u"already using another graphics API");
  })) << problems.join(u'\n').toStdString();
}

#ifdef PJ_WITH_SCENE2D
// The real media dock joins a window an OpenGL widget pinned to OpenGL, its
// viewer asking for another API. The viewer never gets a QRhi, so initialize()
// never runs: Qt's renderFailed() alone must show the dock's rendering warning.
TEST(MixedApiWindowTest, MediaDockWithoutAQRhiShowsItsWarning) {
  PJ_SKIP_ON_HEADLESS_QPA();
  WarningCollector warnings;
  PJ::SessionManager session;
  (void)session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "camera", .time_domain_id = 0}, 1);
  const auto topic = session.objectStore().registerTopic(PJ::ObjectTopicDescriptor{1, "/camera/image", {}});
  ASSERT_TRUE(topic.has_value());
  QWidget window;
  window.resize(400, 300);
  auto* layout = new QHBoxLayout(&window);
  layout->addWidget(new ClearingGlWidget());
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

  auto* dock = new PJ::Scene2DDockWidget();
  dock->setSessionManager(&session);
  ASSERT_TRUE(dock->addTopic(*topic, PJ::sdk::BuiltinObjectType::kImage, u"image"_s));  // fronts the viewer
  auto* viewer = dock->findChild<PJ::MediaViewerWidget*>(u"scene2dMediaViewer"_s);
  auto* label = dock->findChild<QLabel*>(u"scene2dRenderingWarning"_s);
  ASSERT_NE(viewer, nullptr);
  ASSERT_NE(label, nullptr);
  viewer->setApi(nonGlApi());
  layout->addWidget(dock);
  EXPECT_TRUE(QTest::qWaitFor([label] { return label->isVisible(); }, 3000)) << "no rendering warning shown";
  EXPECT_FALSE(label->text().isEmpty());
}
#endif

}  // namespace
