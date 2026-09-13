// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QPointer>
#include <QTemporaryDir>
#include <memory>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace PJ {

/// Exercise the shell callbacks that enforce plugin identity and dataset liveness.
class MainWindowPluginViewsTestPeer {
 public:
  static void checkViewportIsolation(MainWindow& window) {
    auto& app = *window.session_;
    app.sessionManager().setUseTimeOffset(false);
    const auto dataset = pj_test::createDataset(app, "viewport-isolation", true);
    ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/isolation", 0, 10'000'000'000), 0U);
    const auto key = app.catalogModel().resolveCurveKey(0, u"viewport-isolation"_s, {}, u"/isolation"_s, u"value"_s);
    ASSERT_TRUE(key);
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    auto* user = tabs->addTab(u"User viewport"_s);
    ASSERT_TRUE(window.createOwnedPlotTab(u"zoom-a"_s, u"view"_s, u"A"_s));
    ASSERT_TRUE(window.createOwnedPlotTab(u"zoom-b"_s, u"view"_s, u"B"_s));
    auto* a = window.ownedPlotTab(u"zoom-a"_s, u"view"_s);
    auto* b = window.ownedPlotTab(u"zoom-b"_s, u"view"_s);
    const QRectF original(1.0, 8.0, 8.0, -10.0);
    for (auto* docker : {user, a, b}) {
      auto* dock = docker->plotAt(0);
      ASSERT_NE(dock, nullptr);
      auto* plot = dock->ensurePlotWidget();
      ASSERT_NE(plot, nullptr);
      ASSERT_NE(plot->addCurve(*key), nullptr);
      plot->setZoomRectangle(original, false);
    }
    auto* a_plot = a->plotAt(0)->plotWidget();
    const auto before = a_plot->currentBoundingRect();
    const auto user_before = user->plotAt(0)->plotWidget()->currentBoundingRect();
    const auto b_before = b->plotAt(0)->plotWidget()->currentBoundingRect();
    ASSERT_TRUE(window.createOwnedPlotTab(u"zoom-a"_s, u"empty"_s, u"Empty"_s));
    ASSERT_TRUE(window.createOwnedPlotTab(u"zoom-a"_s, u"xy"_s, u"XY"_s));
    auto* empty = window.ownedPlotTab(u"zoom-a"_s, u"empty"_s)->plotAt(0)->ensurePlotWidget();
    auto* xy = window.ownedPlotTab(u"zoom-a"_s, u"xy"_s)->plotAt(0)->ensurePlotWidget();
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(xy, nullptr);
    xy->setModeXY(true);
    ASSERT_NE(xy->addCurveXY(*key, *key, u"XY"_s), nullptr);
    ASSERT_TRUE(xy->isXYPlot());
    const auto empty_before = empty->currentBoundingRect();
    const auto xy_before = xy->currentBoundingRect();

    ASSERT_TRUE(window.zoomOwnedPlotsToTimeRange(u"zoom-a"_s, 2.0, 4.0));
    const auto zoomed = a_plot->currentBoundingRect();
    EXPECT_DOUBLE_EQ(zoomed.left(), 2.0);
    EXPECT_DOUBLE_EQ(zoomed.right(), 4.0);
    EXPECT_DOUBLE_EQ(zoomed.top(), before.top());
    EXPECT_DOUBLE_EQ(zoomed.bottom(), before.bottom());
    EXPECT_EQ(user->plotAt(0)->plotWidget()->currentBoundingRect(), user_before);
    EXPECT_EQ(b->plotAt(0)->plotWidget()->currentBoundingRect(), b_before);
    EXPECT_EQ(empty->currentBoundingRect(), empty_before);
    EXPECT_EQ(xy->currentBoundingRect(), xy_before);
    ASSERT_TRUE(window.zoomOwnedPlotsOut(u"zoom-a"_s));
    EXPECT_NE(a_plot->currentBoundingRect(), zoomed);
    EXPECT_EQ(user->plotAt(0)->plotWidget()->currentBoundingRect(), user_before);
    EXPECT_EQ(b->plotAt(0)->plotWidget()->currentBoundingRect(), b_before);
  }

  static void checkCurveOwnershipAndResolution(MainWindow& window) {
    auto& app = *window.session_;
    const auto first = pj_test::createDataset(app, "curve-a", true);
    const auto second = pj_test::createDataset(app, "curve-b", true);
    ASSERT_NE(pj_test::addScalarTopic(app, first, "/shared"), 0U);
    ASSERT_NE(pj_test::addScalarTopic(app, second, "/shared"), 0U);
    ASSERT_TRUE(window.createOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"Original"_s));
    auto* tab = window.ownedPlotTab(u"curve-owner"_s, u"view"_s);
    ASSERT_FALSE(window.addCurveToOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, {}));
    ASSERT_TRUE(window.addCurveToOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    auto* plot = tab->plotAt(0)->plotWidget();
    ASSERT_NE(plot, nullptr);
    ASSERT_EQ(plot->curveList().size(), 1U);
    ASSERT_TRUE(window.addCurveToOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_EQ(plot->curveList().size(), 1U);
    EXPECT_FALSE(window.clearOwnedPlotTab(u"intruder"_s, tab->stateId()));
    EXPECT_FALSE(
        window.removeCurveFromOwnedPlotTab(u"intruder"_s, tab->stateId(), u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_FALSE(window.addCurveToOwnedPlotTab(u"intruder"_s, tab->stateId(), u"/shared"_s, u"value"_s, u"curve-b"_s));
    EXPECT_FALSE(window.ownedPlotTabConfig(u"intruder"_s, tab->stateId()));
    EXPECT_EQ(plot->curveList().size(), 1U);
    const auto config = window.ownedPlotTabConfig(u"curve-owner"_s, u"view"_s);
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("curve-a"), std::string::npos);
    EXPECT_EQ(config->find("curve-b"), std::string::npos);
    ASSERT_TRUE(
        window.removeCurveFromOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    EXPECT_TRUE(plot->isEmpty());
    EXPECT_FALSE(
        window.removeCurveFromOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-a"_s));
    ASSERT_TRUE(window.addCurveToOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"/shared"_s, u"value"_s, u"curve-b"_s));
    ASSERT_TRUE(window.createOwnedPlotTab(u"curve-owner"_s, u"view"_s, u"Replacement"_s));
    EXPECT_EQ(window.ownedPlotTab(u"curve-owner"_s, u"view"_s), tab);
    EXPECT_EQ(tab->name(), u"Replacement"_s);
    EXPECT_TRUE(plot->isEmpty());
  }

  static void check(MainWindow& window) {
    const QString owner = u"plugin-a"_s;
    const QString other = u"plugin-b"_s;
    auto* tabs = window.findChild<TabbedPlotWidget*>();
    ASSERT_NE(tabs, nullptr);
    PlotDocker* user_tab = tabs->addTab(u"Temperature"_s);
    const QString user_id = user_tab->stateId();

    ASSERT_TRUE(window.createOwnedPlotTab(owner, u"run-a"_s, u"Temperature"_s));
    ASSERT_TRUE(window.createOwnedPlotTab(owner, u"run-b"_s, u"Temperature"_s));
    ASSERT_TRUE(window.createOwnedPlotTab(other, u"run-a"_s, u"Temperature"_s));
    PlotDocker* first = window.ownedPlotTab(owner, u"run-a"_s);
    PlotDocker* second = window.ownedPlotTab(owner, u"run-b"_s);
    PlotDocker* foreign = window.ownedPlotTab(other, u"run-a"_s);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(foreign, nullptr);
    EXPECT_NE(first, foreign);
    EXPECT_EQ(first->name(), second->name());
    const QString second_id = second->stateId();
    second->setName(u"Renamed"_s);
    EXPECT_EQ(window.ownedPlotTab(owner, u"run-b"_s), second);
    EXPECT_EQ(second->stateId(), second_id);

    EXPECT_FALSE(window.closeOwnedPlotTab(owner, user_id));
    EXPECT_FALSE(window.closeOwnedPlotTab(owner, foreign->stateId()));
    ASSERT_TRUE(window.closeOwnedPlotTab(owner, u"run-a"_s));
    EXPECT_EQ(window.ownedPlotTab(owner, u"run-b"_s), second);
    EXPECT_EQ(window.ownedPlotTab(other, u"run-a"_s), foreign);
    EXPECT_EQ(user_tab->stateId(), user_id);
    const auto ids = window.listOwnedPlotTabs(owner);
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::string>{"run-b"}));
    const auto config = window.ownedPlotTabConfig(owner, u"run-b"_s);
    ASSERT_TRUE(config);
    EXPECT_NE(config->find("Renamed"), std::string::npos);

    DockWidget* second_dock = second->plotAt(0);
    ASSERT_NE(second_dock, nullptr);
    if (second_dock->plotWidget() == nullptr) {
      second_dock->setPlotWidget(new PlotWidget(nullptr, nullptr, second_dock));
    }
    second_dock->plotWidget()->setStateId(u"persisted-owned-content"_s);

    QPointer<PlotDocker> before_restore(second);
    const QDomDocument saved = window.xmlSaveState(MainWindow::SnapshotScope::kFull);
    const QString saved_xml = QString::fromUtf8(saved.toByteArray());
    EXPECT_TRUE(saved_xml.contains(u"owner_plugin=\"plugin-a\""_s));
    EXPECT_TRUE(saved_xml.contains(u"tab_id=\"run-b\""_s));
    ASSERT_TRUE(window.xmlLoadState(saved, MainWindow::RestoreIntent::kReplace));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(before_restore.isNull());

    second = window.ownedPlotTab(owner, u"run-b"_s);
    foreign = window.ownedPlotTab(other, u"run-a"_s);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(foreign, nullptr);
    EXPECT_TRUE(second->isHistoryExempt());
    EXPECT_EQ(second->ownerPlugin(), owner);
    EXPECT_EQ(second->ownerTabId(), u"run-b"_s);
    EXPECT_FALSE(second->isOwnerPluginAvailable());
    EXPECT_TRUE(second->toolTip().contains(u"Plugin unavailable"_s));
    ASSERT_NE(second->plotAt(0), nullptr);
    ASSERT_NE(second->plotAt(0)->plotWidget(), nullptr);
    EXPECT_EQ(second->plotAt(0)->plotWidget()->stateId(), u"persisted-owned-content"_s);

    PlotDocker* restored = second;
    ASSERT_TRUE(window.createOwnedPlotTab(owner, u"run-b"_s, u"Updated"_s));
    EXPECT_EQ(window.ownedPlotTab(owner, u"run-b"_s), restored);
    EXPECT_EQ(restored->name(), u"Updated"_s);

    AppSession& app = *window.session_;
    SessionManager& session = app.sessionManager();
    session.setUseTimeOffset(false);
    const auto a = pj_test::createDataset(app, "same.mcap", /*own_time_domain=*/true);
    const auto b = pj_test::createDataset(app, "same.mcap", /*own_time_domain=*/true);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    ASSERT_NE(pj_test::addScalarTopic(app, a, "/speed"), 0u);
    ASSERT_NE(pj_test::addScalarTopic(app, b, "/speed"), 0u);
    session.setDisplayOffset(a, DisplayOffset{Duration{1'000'000'000}});
    session.setDisplayOffset(b, DisplayOffset{Duration{2'000'000'000}});
    const PJ_data_source_handle_t source_a{static_cast<uint32_t>(a)};
    const PJ_data_source_handle_t source_b{static_cast<uint32_t>(b)};
    const auto display_a = window.displayTimeForSource(source_a, 3'500'000'000);
    const auto display_b = window.displayTimeForSource(source_b, 3'500'000'000);
    ASSERT_TRUE(display_a);
    ASSERT_TRUE(display_b);
    EXPECT_DOUBLE_EQ(*display_a, 2.5);
    EXPECT_DOUBLE_EQ(*display_b, 1.5);
    session.setDisplayOffset(b, DisplayOffset{Duration{3'000'000'000}});
    const auto shifted = window.displayTimeForSource(source_b, 3'500'000'000);
    ASSERT_TRUE(shifted);
    EXPECT_DOUBLE_EQ(*shifted, 0.5);
    EXPECT_FALSE(window.displayTimeForSource({0}, 0));
    EXPECT_FALSE(window.displayTimeForSource({999999}, 0));
    app.catalogModel().removeDataset(b, /*tombstone=*/false);
    session.dataEngine().removeDataset(b);
    EXPECT_FALSE(window.displayTimeForSource(source_b, 3'500'000'000));
  }
};

}  // namespace PJ

class MainWindowPluginViewsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_->path());
  }
  static void TearDownTestSuite() {
    window_.reset();
    extensions_.reset();
  }
  inline static std::unique_ptr<QTemporaryDir> extensions_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(MainWindowPluginViewsTest, StableTabIdsAndExplicitSourceOffsets) {
  PJ::MainWindowPluginViewsTestPeer::check(*window_);
}

// Claim: time-range zoom preserves Y and both zoom operations leave user and other-plugin plots untouched.
TEST_F(MainWindowPluginViewsTest, ViewportCommandsOnlyChangeCallingPluginsPlots) {
  PJ::MainWindowPluginViewsTestPeer::checkViewportIsolation(*window_);
}

// Claim: curve operations resolve one dataset, enforce ownership, and replacing a tab clears its contents.
TEST_F(MainWindowPluginViewsTest, CurveCommandsEnforceOwnershipAndRejectAmbiguousDatasets) {
  PJ::MainWindowPluginViewsTestPeer::checkCurveOwnershipAndResolution(*window_);
}
