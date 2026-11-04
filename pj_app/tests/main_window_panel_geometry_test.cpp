// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <string_view>

#include "MainWindow.h"
#include "ToolboxPresenter.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "support/gui_test_env.h"

using namespace Qt::StringLiterals;

namespace PJ {

// Reaches the private chart-area takeover seam, so the assertions run against
// the real path a toolbox takes rather than a stand-in that would not reproduce
// anything. The widgets themselves are found by objectName - the generated
// ui_MainWindow.h is internal to the shell library.
class MainWindowPanelGeometryTestPeer {
 public:
  static void reconnectDock(MainWindow& window, PlotDocker* docker) {
    window.onPlotTabAdded(docker);
  }
  [[nodiscard]] static bool presentPanel(MainWindow& window, QWidget* panel) {
    window.presentPanel(panel);
    return window.current_panel_ == panel;
  }

  static void restoreCentralArea(MainWindow& window) {
    window.restoreCentralArea();
  }

  // A bare (engine-less) toolbox presenter for `container`, not yet presented.
  static ToolboxPresenter& registerToolbox(MainWindow& window, const QString& plugin_id, QWidget* container) {
    return window.registerToolbox(
        plugin_id, ToolboxPresenter::Spec{
                       .container = container,
                       .engine = nullptr,
                       .session = {},
                       .host = nullptr,
                       .title = u"Toolbox"_s,
                       .save_config = []() { return u"{}"_s; },
                       .apply_chrome = {}});
  }
};

}  // namespace PJ

namespace {

// Counts the ParentChange events the watched widget receives.
class ParentChangeCounter : public QObject {
 public:
  ParentChangeCounter(int* count, QObject* parent) : QObject(parent), count_(count) {}
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::ParentChange) {
      ++*count_;
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  int* count_;
};

// The left column's width is the user's setting: they drag the separator to a
// width that suits their data, and only another drag may change it.
constexpr int kUserChosenLeftWidth = 380;

constexpr int kWindowWidth = 1600;
constexpr int kWindowHeight = 900;
constexpr int kWiderWindowWidth = 2000;

// Drains posted events and deferred layout passes. The splitter re-lays out on
// LayoutRequest, so an assertion made before the queue drains can pass against
// geometry that is about to change.
void settle() {
  for (int pass = 0; pass < 5; ++pass) {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
  }
}

[[nodiscard]] QSplitter& mainSplitter(PJ::MainWindow& window) {
  QSplitter* splitter = window.findChild<QSplitter*>(u"mainSplitter"_s);
  Q_ASSERT(splitter != nullptr);
  return *splitter;
}

[[nodiscard]] QWidget& rightPanel(PJ::MainWindow& window) {
  QWidget* panel = window.findChild<QWidget*>(u"localToolbarWidget"_s);
  Q_ASSERT(panel != nullptr);
  return *panel;
}

[[nodiscard]] int leftPanelWidth(PJ::MainWindow& window) {
  return mainSplitter(window).sizes().value(0);
}

// Puts the window on screen at a known size with the left column at a width the
// user is taken to have chosen, which is the precondition every case shares.
void openAtUserChosenWidth(PJ::MainWindow& window) {
  window.resize(kWindowWidth, kWindowHeight);
  window.show();
  settle();
  QSplitter& splitter = mainSplitter(window);
  splitter.setSizes({kUserChosenLeftWidth, splitter.width() - kUserChosenLeftWidth});
  settle();
  ASSERT_EQ(leftPanelWidth(window), kUserChosenLeftWidth);
}

// Growing the window must feed the chart area, not the sidebar: a splitter that
// shares new width proportionally walks the left panel out to its 600px cap.
TEST(MainWindowPanelGeometryTest, WideningTheWindowLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  window.resize(kWiderWindowWidth, kWindowHeight);
  settle();

  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth);
}

// Collapsing the right side panel hands its width back to the splitter row. All
// of it belongs to the chart area.
TEST(MainWindowPanelGeometryTest, HidingTheRightPanelLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  rightPanel(window).hide();
  settle();

  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth);
}

// A toolbox takes over the chart area by swapping itself into the chart's
// splitter slot. Whatever width the incoming panel prefers, the sidebar keeps
// the width the user gave it - on the way in and on the way back out.
TEST(MainWindowPanelGeometryTest, PresentingAndDismissingAPanelLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  auto* panel = new QLabel(u"toolbox"_s);
  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, panel));
  settle();
  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth) << "presenting a toolbox panel resized the left panel";

  PJ::MainWindowPanelGeometryTestPeer::restoreCentralArea(window);
  settle();
  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth) << "dismissing a toolbox panel resized the left panel";
}

TEST(MainWindowPanelGeometryTest, SettingsActionOpensAndClosesTheSidebar) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);
  auto* tabs = window.findChild<PJ::TabbedPlotWidget*>();
  ASSERT_NE(tabs, nullptr);
  auto* dock = tabs->dockerAt(0)->plotAt(0);
  ASSERT_NE(dock, nullptr);
  PJ::MainWindowPanelGeometryTestPeer::reconnectDock(window, tabs->dockerAt(0));
  PJ::MainWindowPanelGeometryTestPeer::reconnectDock(window, tabs->dockerAt(0));
  rightPanel(window).hide();
  dock->settingsPanelToggleRequested(dock);
  settle();
  EXPECT_FALSE(rightPanel(window).isHidden());
  dock->settingsPanelToggleRequested(dock);
  settle();
  EXPECT_TRUE(rightPanel(window).isHidden());
}

// Launching a toolbox while another one is open dismisses the first and
// presents the second. That round trip puts the chart back into the splitter
// and takes it out again, and the sidebar must not pick up a slice on the way.
TEST(MainWindowPanelGeometryTest, ReplacingAnOpenPanelLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, new QLabel(u"first toolbox"_s)));
  settle();
  ASSERT_EQ(leftPanelWidth(window), kUserChosenLeftWidth) << "presenting the first toolbox resized the left panel";

  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, new QLabel(u"second toolbox"_s)));
  settle();
  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth) << "replacing an open toolbox resized the left panel";

  PJ::MainWindowPanelGeometryTestPeer::restoreCentralArea(window);
  settle();
  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth) << "dismissing the replacement resized the left panel";
}

// Presenting and dismissing a panel switch pages of the chart slot instead of
// moving the chart in and out of the window: every reparent re-polishes a whole
// widget tree against the app stylesheet, which froze a browser page for
// hundreds of ms on each toolbox close. The dismissed panel is not reparented
// either, only deleted.
TEST(MainWindowPanelGeometryTest, PresentingAndDismissingAPanelNeverReparentsTheChartOrThePanel) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  QWidget* chart_parent = tabbed->parentWidget();
  ASSERT_NE(chart_parent, nullptr);

  QPointer<QLabel> panel = new QLabel(u"toolbox"_s);
  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, panel));
  settle();
  EXPECT_EQ(tabbed->parentWidget(), chart_parent);
  EXPECT_FALSE(tabbed->isVisible());
  QWidget* panel_parent = panel->parentWidget();
  int panel_parent_changes = 0;
  panel->installEventFilter(new ParentChangeCounter(&panel_parent_changes, panel));

  PJ::MainWindowPanelGeometryTestPeer::restoreCentralArea(window);
  EXPECT_EQ(tabbed->parentWidget(), chart_parent);
  ASSERT_FALSE(panel.isNull()) << "the dismissed panel is deleted later, not on the spot";
  EXPECT_EQ(panel->parentWidget(), panel_parent);
  EXPECT_EQ(panel_parent_changes, 0);
  settle();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(tabbed->isVisible());
  EXPECT_TRUE(panel.isNull());
}

// A toolbox moving between presentations is reparented once per move, at its
// destination (a release leaves its parent alone), and closing its tab deletes
// it where it is.
TEST(MainWindowPanelGeometryTest, ToolboxMovesReparentOnceAndItsTabCloseDeletesInPlace) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  QPointer<QLabel> panel = new QLabel(u"toolbox"_s);
  int parent_changes = 0;
  panel->installEventFilter(new ParentChangeCounter(&parent_changes, panel));
  PJ::ToolboxPresenter& presenter =
      PJ::MainWindowPanelGeometryTestPeer::registerToolbox(window, u"test.toolbox"_s, panel);

  using Presentation = PJ::ToolboxPresenter::Presentation;
  presenter.moveToDock();
  EXPECT_EQ(presenter.presentation(), Presentation::kTakeover);
  EXPECT_EQ(parent_changes, 1);
  presenter.moveToTab(/*transient=*/false);
  EXPECT_EQ(presenter.presentation(), Presentation::kTab);
  EXPECT_EQ(parent_changes, 2);
  presenter.moveToFloating();
  EXPECT_EQ(presenter.presentation(), Presentation::kFloating);
  EXPECT_EQ(parent_changes, 3);
  presenter.moveToDock();
  EXPECT_EQ(presenter.presentation(), Presentation::kTakeover);
  EXPECT_EQ(parent_changes, 4);
  presenter.moveToTab(/*transient=*/false);
  settle();  // the floating window's deferred delete runs; the panel had left it
  ASSERT_FALSE(panel.isNull());
  EXPECT_TRUE(panel->isVisible());
  EXPECT_EQ(parent_changes, 5);

  tabbed->closeWidgetTab(panel);
  EXPECT_EQ(parent_changes, 5);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(panel.isNull());
}

// Putting the chart back has to restore its width-absorbing role too, not just
// its slot. It does so implicitly - a stretch factor is stored on the pane
// widget's own size policy, so the chart still carries the one it was given at
// construction - and that implicitness is worth pinning down.
TEST(MainWindowPanelGeometryTest, WideningAfterDismissingAPanelLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, new QLabel(u"toolbox"_s)));
  settle();
  PJ::MainWindowPanelGeometryTestPeer::restoreCentralArea(window);
  settle();

  window.resize(kWiderWindowWidth, kWindowHeight);
  settle();

  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth);
}

// A splitter whose panes all sit at stretch 0 shares any width change between
// them, which is never what a sidebar-plus-content layout wants and is exactly
// how the left panel started creeping. Rather than mandate specific factors -
// two panes that should grow together legitimately share one - this asks only
// that every splitter has made a deliberate choice, so the next one added to
// the window cannot inherit the same bug by omission.
//
// Scope: splitters this app declares (plain QSplitter) with something to share.
// The docking framework's own CDockSplitter subclass sizes itself and is not
// ours to govern; a lone pane has no one to share with.
TEST(MainWindowPanelGeometryTest, EverySplitterDeclaresAStretchPreference) {
  PJ::MainWindow window;
  window.resize(kWindowWidth, kWindowHeight);
  window.show();
  settle();

  int checked = 0;
  for (const QSplitter* splitter : window.findChildren<QSplitter*>()) {
    if (splitter->metaObject()->className() != std::string_view{"QSplitter"} || splitter->count() < 2) {
      continue;
    }
    ++checked;
    int total_stretch = 0;
    for (int pane = 0; pane < splitter->count(); ++pane) {
      const QSizePolicy policy = splitter->widget(pane)->sizePolicy();
      total_stretch +=
          splitter->orientation() == Qt::Horizontal ? policy.horizontalStretch() : policy.verticalStretch();
    }
    EXPECT_GT(total_stretch, 0) << "splitter '" << splitter->objectName().toStdString()
                                << "' leaves every pane at stretch 0, so it shares width changes between them "
                                   "instead of giving them to one";
  }
  EXPECT_GT(checked, 0) << "found no splitters to check - the traversal, not the app, is what broke";
}

// The presented panel takes over the chart's slot, and with it the chart's job
// of absorbing width changes. A splitter pane is not born with a stretch factor,
// so an open toolbox is exactly when the sidebar is most likely to start
// creeping - which is how this reached a user in the first place.
TEST(MainWindowPanelGeometryTest, WideningTheWindowWithAPanelOpenLeavesTheLeftPanelWidthAlone) {
  PJ::MainWindow window;
  openAtUserChosenWidth(window);

  auto* panel = new QLabel(u"toolbox"_s);
  ASSERT_TRUE(PJ::MainWindowPanelGeometryTestPeer::presentPanel(window, panel));
  settle();

  window.resize(kWiderWindowWidth, kWindowHeight);
  settle();

  EXPECT_EQ(leftPanelWidth(window), kUserChosenLeftWidth);
}

}  // namespace

namespace {

// Preserve this suite's settings scope without changing other runner cases.
class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("MainWindowPanelGeometryTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"main_window_panel_geometry_test"_s);
    QSettings().clear();
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

}  // namespace
