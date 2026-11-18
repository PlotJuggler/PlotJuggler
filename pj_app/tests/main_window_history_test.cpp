// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/UsageCounters.h"
#include "pj_runtime/XmlCdata.h"
#include "pj_widgets/Timeline.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
#include "support/gui_test_env.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#endif

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowHistoryTestPeer {
 public:
  [[nodiscard]] static QByteArray liveState(const MainWindow& window) {
    return window.xmlSaveState().toByteArray(2);
  }

  static void recordDiscreteState(MainWindow& window) {
    window.onUndoableChange(/*force_new_state=*/true);
  }

  static void resetHistory(MainWindow& window) {
    window.resetUndoHistory();
  }

  static void completeFileLoad(MainWindow& window, const QString& path) {
    window.onFileLoaded(path, {}, u"test-loader"_s, u"{}"_s, u"test-loader-id"_s);
  }

  static void setProgressiveRestoreInFlight(MainWindow& window, bool active) {
    window.progressive_layout_in_flight_ = active;
    window.updateUndoRedoActions();
  }

  static void beginProgressiveRestore(MainWindow& window, QDomDocument doc) {
    // kPrompt: the interactive-load policy.
    window.beginProgressiveLayoutRestore(
        std::move(doc), u"/tmp/progressive-test.pj4.xml"_s, MainWindow::MissingCurvePolicy::kPrompt);
  }

  static void drainProgressiveRestore(MainWindow& window) {
    window.onProgressiveLayoutDrained();
  }

  [[nodiscard]] static bool progressiveRestoreInFlight(const MainWindow& window) {
    return window.progressive_layout_in_flight_;
  }

  static void replaceUndoTarget(MainWindow& window, QByteArray state) {
    ASSERT_GE(window.undo_states_.size(), 2U);
    window.undo_states_[window.undo_states_.size() - 2].xml = std::move(state);
  }

  static void replaceUndoTargetDatasetId(MainWindow& window, DatasetId dataset_id) {
    ASSERT_GE(window.undo_states_.size(), 2U);
    auto& tracks = window.undo_states_[window.undo_states_.size() - 2].timeline.tracks;
    ASSERT_FALSE(tracks.empty());
    tracks.front().dataset_id = dataset_id;
  }

  static void replaceUndoTargetOffset(MainWindow& window, qint64 offset_ns) {
    ASSERT_GE(window.undo_states_.size(), 2U);
    auto& tracks = window.undo_states_[window.undo_states_.size() - 2].timeline.tracks;
    ASSERT_FALSE(tracks.empty());
    tracks.front().display_offset_ns = offset_ns;
  }

  static void undo(MainWindow& window) {
    window.onUndo();
  }

  static void redo(MainWindow& window) {
    window.onRedo();
  }

  [[nodiscard]] static std::size_t undoSize(const MainWindow& window) {
    return window.undo_states_.size();
  }

  [[nodiscard]] static std::size_t redoSize(const MainWindow& window) {
    return window.redo_states_.size();
  }

  [[nodiscard]] static bool undoEnabled(const MainWindow& window) {
    return window.undo_action_ != nullptr && window.undo_action_->isEnabled();
  }

  [[nodiscard]] static bool redoEnabled(const MainWindow& window) {
    return window.redo_action_ != nullptr && window.redo_action_->isEnabled();
  }

  [[nodiscard]] static AppSession& appSession(MainWindow& window) {
    return *window.session_;
  }

  static void setUseTimeOffset(MainWindow& window, bool enabled) {
    window.onUseTimeOffsetToggled(enabled);
  }

  [[nodiscard]] static Timeline& sourceTimeline(MainWindow& window) {
    EXPECT_NE(window.source_timeline_, nullptr);
    return *window.source_timeline_;
  }

  [[nodiscard]] static SourceTimelineController& sourceTimelineController(MainWindow& window) {
    EXPECT_NE(window.source_timeline_controller_, nullptr);
    return *window.source_timeline_controller_;
  }

  static void setTimelineSnap(MainWindow& window, bool enabled) {
    auto* button = window.findChild<QToolButton*>(u"buttonTimelineSnap"_s);
    ASSERT_NE(button, nullptr);
    const QSignalBlocker blocker(button);
    button->setChecked(enabled);
    window.source_timeline_->setSnapEnabled(enabled);
  }

  [[nodiscard]] static bool timelineSnap(const MainWindow& window) {
    auto* button = window.findChild<QToolButton*>(u"buttonTimelineSnap"_s);
    EXPECT_NE(button, nullptr);
    return button != nullptr && button->isChecked();
  }

  static void saveGenericLayout(MainWindow& window, const QString& path) {
    window.saveLayoutToPath(path, /*include_data_source=*/false);
  }

  [[nodiscard]] static bool restoreLayout(MainWindow& window, QDomDocument& doc) {
    return window.restoreWorkspaceState(doc, MainWindow::MissingCurvePolicy::kSilentDrop) ==
           MainWindow::RestoreResult::kApplied;
  }
};

}  // namespace PJ

namespace {

PJ::PlotWidget* ensureCurrentPlot(PJ::MainWindow& window) {
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  if (tabbed == nullptr || tabbed->currentTab() == nullptr || tabbed->currentTab()->plotCount() == 0) {
    return nullptr;
  }
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  if (dock == nullptr) {
    return nullptr;
  }
  if (dock->plotWidget() == nullptr) {
    dock->setPlotWidget(new PJ::PlotWidget(nullptr, nullptr, dock));
  }
  return dock->plotWidget();
}

PJ::DatasetId addScalarDataset(
    PJ::AppSession& app_session, std::string_view source, std::string_view topic, PJ::Timestamp first,
    PJ::Timestamp last) {
  const PJ::DatasetId dataset = pj_test::createDataset(app_session, source, /*own_time_domain=*/true);
  if (dataset == 0 || pj_test::addScalarTopic(app_session, dataset, topic, first, last) == 0) {
    return 0;
  }
  return dataset;
}

// Sorted t_start of the Global set all-datasets generator `owner` publishes.
std::vector<PJ::Timestamp> allDatasetsMarkerTimes(PJ::AppSession& app, std::string_view owner) {
  return pj_test::markerTimes(app, PJ::kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic, owner);
}

// For one test: the production create budget, and every marker run computed on the
// pool held until release() (a synchronous run on the GUI thread passes). Counts
// every compute, held or not. The destructor releases and waits for the held computes
// to return and their runs to publish before the probe goes, since a cancelled run's
// compute is held on the pool although no binding is running.
class HeldMarkerComputes {
 public:
  explicit HeldMarkerComputes(PJ::BatchEngine& engine)
      : engine_(engine), previous_budget_(PJ::BatchEngine::setCreateBudgetForTest(PJ::BatchEngine::kCreateBudget)) {
    QThread* const gui = QThread::currentThread();
    PJ::BatchEngine::setComputeThreadProbeForTest([this, gui](QThread* thread) {
      ++computes_;
      ++holding_;
      for (int waited_ms = 0; thread != gui && !released_.load() && waited_ms < 5000; ++waited_ms) {
        QThread::msleep(1);
      }
      --holding_;
    });
  }
  ~HeldMarkerComputes() {
    release();
    static_cast<void>(QTest::qWaitFor([this] { return idle() && engine_.runningIds().empty(); }, 5000));
    PJ::BatchEngine::setComputeThreadProbeForTest({});
    PJ::BatchEngine::setCreateBudgetForTest(previous_budget_);
  }
  HeldMarkerComputes(const HeldMarkerComputes&) = delete;
  HeldMarkerComputes& operator=(const HeldMarkerComputes&) = delete;

  void release() {
    released_ = true;
  }

  [[nodiscard]] int computes() const {
    return computes_.load();
  }

  // Whether no compute is inside the probe (held or about to be).
  [[nodiscard]] bool idle() const {
    return holding_.load() == 0;
  }

 private:
  PJ::BatchEngine& engine_;
  std::chrono::milliseconds previous_budget_;
  std::atomic<bool> released_{false};
  std::atomic<int> computes_{0};
  std::atomic<int> holding_{0};
};

QString catalogKeyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name);
        descriptor.has_value() && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

QDomElement addSavedAbsoluteProcessor(
    QDomDocument& doc, PJ::DatasetId dataset_id, const QString& source_name, const QString& topic_name) {
  QDomElement processors = doc.documentElement().firstChildElement(u"data_processors"_s);
  if (processors.isNull()) {
    processors = doc.createElement(u"data_processors"_s);
    doc.documentElement().appendChild(processors);
  }
  QDomElement processor = doc.createElement(u"processor"_s);
  processor.setAttribute(u"input_topic"_s, topic_name);
  processor.setAttribute(u"input_field"_s, u"value"_s);
  processor.setAttribute(u"input_dataset_id"_s, QString::number(dataset_id));
  processor.setAttribute(u"input_dataset_source"_s, source_name);
  processor.setAttribute(u"processor_id"_s, u"absolute"_s);
  processor.setAttribute(u"output_name"_s, topic_name + u"[Absolute]"_s);
  PJ::appendJsonAsCdata(doc, processor, u"{}"_s);
  processors.appendChild(processor);
  return processor;
}

void closeNextModalDialog() {
  QTimer::singleShot(0, []() {
    if (QWidget* modal = QApplication::activeModalWidget(); modal != nullptr) {
      modal->close();
    }
  });
}

class MainWindowHistoryFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
  }

  static void TearDownTestSuite() {
    window_.reset();
    extensions_dir_.reset();
  }

  [[nodiscard]] PJ::MainWindow& mainWindow() const {
    return *window_;
  }

 private:
  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(MainWindowHistoryFixture, FailedDestructiveUndoKeepsLiveStateAndCursorThenUndoRedoRemainInvertible) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);

  plot->setStateId(u"history-before-edit"_s);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  const QByteArray baseline_state = PJ::MainWindowHistoryTestPeer::liveState(window);

  plot->setStateId(u"history-after-edit"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const QByteArray edited_state = PJ::MainWindowHistoryTestPeer::liveState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  ASSERT_NE(edited_state, baseline_state);

  // Append a second tab whose container has no recognized layout root. The
  // first tab has already been rebuilt when TabbedPlotWidget rejects the second,
  // making this a destructive late failure rather than a parse-only rejection.
  QDomDocument rejected_target;
  ASSERT_TRUE(static_cast<bool>(rejected_target.setContent(baseline_state)));
  QDomElement tabbed = rejected_target.documentElement().firstChildElement(u"tabbed_widget"_s);
  ASSERT_FALSE(tabbed.isNull());
  QDomElement second_tab = tabbed.firstChildElement(u"Tab"_s).cloneNode(true).toElement();
  ASSERT_FALSE(second_tab.isNull());
  second_tab.setAttribute(u"id"_s, u"rejected-second-tab"_s);
  QDomElement second_container = second_tab.firstChildElement(u"Container"_s);
  ASSERT_FALSE(second_container.isNull());
  QDomElement second_root = second_container.firstChildElement();
  ASSERT_FALSE(second_root.isNull());
  second_root.setTagName(u"unknown_test_layout"_s);
  tabbed.insertBefore(second_tab, tabbed.firstChildElement(u"currentTabIndex"_s));
  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, rejected_target.toByteArray(2));

  PJ::MainWindowHistoryTestPeer::undo(window);

  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), edited_state);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);

  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, baseline_state);
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 1U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 1U);
  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), edited_state);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);
}

TEST_F(MainWindowHistoryFixture, PlaceholderChoiceUndoRedoRestoresChooserAndEmptyPlot) {
  PJ::MainWindow& window = mainWindow();
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  ASSERT_NE(tabbed->currentTab(), nullptr);
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  dock->setPlaceholderWidget();
  PJ::MainWindowHistoryTestPeer::resetHistory(window);

  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* plot_button = placeholder->findChild<QToolButton*>(u"buttonVizPlot"_s);
  ASSERT_NE(plot_button, nullptr);
  plot_button->click();
  ASSERT_NE(dock->plotWidget(), nullptr);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  PJ::MainWindowHistoryTestPeer::undo(window);
  dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_NE(dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);

  PJ::MainWindowHistoryTestPeer::redo(window);
  dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  EXPECT_NE(dock->plotWidget(), nullptr);
}

TEST_F(MainWindowHistoryFixture, AreaRenameIsItsOwnUndoStepAndLeavesTheEarlierEditAlone) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);

  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  const QByteArray baseline_state = PJ::MainWindowHistoryTestPeer::liveState(window);
  const QString original_title = dock->name();

  plot->setStateId(u"area-rename-earlier-edit"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const QByteArray edited_state = PJ::MainWindowHistoryTestPeer::liveState(window);
  ASSERT_NE(edited_state, baseline_state);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  // Rename the area the way the user does: double-click the title, type, Return.
  PJ::DockToolbar* toolbar = dock->toolBar();
  auto* edit = toolbar->findChild<QLineEdit*>(u"lineEditRename"_s);
  ASSERT_NE(edit, nullptr);
  QMouseEvent double_click(
      QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  ASSERT_TRUE(toolbar->eventFilter(toolbar->label(), &double_click));
  edit->setText(u"renamed area"_s);
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(edit, &enter);
  ASSERT_EQ(dock->name(), u"renamed area"_s);
  const QByteArray renamed_state = PJ::MainWindowHistoryTestPeer::liveState(window);
  ASSERT_NE(renamed_state, edited_state);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 3U) << "the rename must record exactly one step";

  // One undo reverts the rename only; the earlier edit stays.
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), edited_state);
  EXPECT_EQ(tabbed->currentTab()->plotAt(0)->name(), original_title);

  // The next undo reverts the earlier edit.
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), baseline_state);

  // Redo replays both, ending on the rename.
  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), edited_state);
  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), renamed_state);
  EXPECT_EQ(tabbed->currentTab()->plotAt(0)->name(), u"renamed area"_s);
}

TEST_F(MainWindowHistoryFixture, CtrlYAndCtrlShiftZRedoThroughTheKeyboard) {
  PJ::MainWindow& window = mainWindow();
  window.show();
  window.activateWindow();
  ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  dock->setPlaceholderWidget();
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* plot_button = placeholder->findChild<QToolButton*>(u"buttonVizPlot"_s);
  ASSERT_NE(plot_button, nullptr);
  plot_button->click();

  // Redo is keyboard-only: each of its keys must reach it, which a key bound twice to it does not (Qt calls the
  // overload ambiguous and fires nothing).
  for (const QKeyCombination redo_key : {Qt::CTRL | Qt::Key_Y, Qt::CTRL | Qt::SHIFT | Qt::Key_Z}) {
    PJ::MainWindowHistoryTestPeer::undo(window);
    ASSERT_EQ(tabbed->currentTab()->plotAt(0)->plotWidget(), nullptr);
    QTest::keyClick(&window, redo_key.key(), redo_key.keyboardModifiers());
    EXPECT_NE(tabbed->currentTab()->plotAt(0)->plotWidget(), nullptr)
        << QKeySequence(redo_key).toString().toStdString() << " did not redo";
  }
}

TEST_F(MainWindowHistoryFixture, AutomaticGlobalRebasePreservesAbsolutePlayheadAndReframesRange) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app_session.sessionManager();
  PJ::PlaybackEngine& playback = app_session.playbackEngine();
  PJ::MainWindowHistoryTestPeer::setUseTimeOffset(window, false);

  ASSERT_NE(addScalarDataset(app_session, "later.mcap", "/later", 100 * kSecond, 110 * kSecond), 0U);
  PJ::MainWindowHistoryTestPeer::setUseTimeOffset(window, true);
  ASSERT_EQ(session.globalTimeReference(), 100 * kSecond);
  ASSERT_DOUBLE_EQ(playback.rangeMin().value, 0.0);
  ASSERT_DOUBLE_EQ(playback.rangeMax().value, 10.0);

  playback.setCurrentTime(PJ::DisplaySeconds{5.0});
  ASSERT_DOUBLE_EQ(playback.currentTime().value, 5.0);

  // The shared origin moves from 100 s to 50 s. Preserve the absolute 105 s
  // playhead while reframing the complete range in the moved origin's relative domain.
  ASSERT_NE(addScalarDataset(app_session, "earlier.mcap", "/earlier", 50 * kSecond, 60 * kSecond), 0U);

  EXPECT_EQ(session.globalTimeReference(), 50 * kSecond);
  EXPECT_DOUBLE_EQ(playback.currentTime().value, 55.0);
  EXPECT_DOUBLE_EQ(
      playback.currentTime().value + static_cast<double>(session.globalTimeReference()) / PJ::kNanosecondsPerSecond,
      105.0);
  EXPECT_DOUBLE_EQ(playback.rangeMin().value, 0.0);
  EXPECT_DOUBLE_EQ(playback.rangeMax().value, 60.0);
}

TEST_F(MainWindowHistoryFixture, AdditiveAndTopologyPreservingLoadsKeepWorkspaceHistory) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);

  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"history-before-additive-load"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  const PJ::DatasetId additive =
      addScalarDataset(app_session, "additive-history.mcap", "/additive", 200'000'000'000LL, 201'000'000'000LL);
  ASSERT_NE(additive, 0U);
  PJ::MainWindowHistoryTestPeer::completeFileLoad(window, u"/tmp/additive-history.mcap"_s);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_TRUE(PJ::MainWindowHistoryTestPeer::undoEnabled(window));

  PJ::MainWindowHistoryTestPeer::completeFileLoad(window, u"/tmp/additive-history.mcap"_s);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  // The load hydrates the current tip with its non-undoable baseline. Therefore
  // the first timeline edit can still be undone back to offset zero.
  app_session.sessionManager().setDisplayOffset(additive, PJ::DisplayOffset{PJ::Duration{123'456}});
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 3U);
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(app_session.sessionManager().sourceDisplayOffset(additive).value.count(), 0);
  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(app_session.sessionManager().sourceDisplayOffset(additive).value.count(), 123'456);
}

TEST_F(MainWindowHistoryFixture, RemintedDatasetCompletionRebaselinesStaleHistory) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);

  const PJ::DatasetId original =
      addScalarDataset(app_session, "reminted-history.mcap", "/reminted", 300'000'000'000LL, 301'000'000'000LL);
  ASSERT_NE(original, 0U);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"history-before-remint"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  app_session.catalogModel().removeDataset(original, /*tombstone=*/false);
  app_session.sessionManager().evictDatasetObjects(original);
  app_session.sessionManager().removeDataset(original);
  const PJ::DatasetId replacement =
      addScalarDataset(app_session, "reminted-history.mcap", "/reminted", 400'000'000'000LL, 401'000'000'000LL);
  ASSERT_NE(replacement, 0U);
  ASSERT_NE(replacement, original);

  PJ::MainWindowHistoryTestPeer::completeFileLoad(window, u"/tmp/reminted-history.mcap"_s);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 1U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);
  EXPECT_FALSE(PJ::MainWindowHistoryTestPeer::undoEnabled(window));
}

TEST_F(MainWindowHistoryFixture, AdvertisedCurveIntentSurvivesUndoRedoAndMaterializesWithoutNewEdit) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->removeAllCurves();

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "history-pending-stream");
  ASSERT_NE(dataset, 0U);
  app_session.catalogModel().setAdvertisedTopics(
      dataset, {PJ::AdvertisedTopic{u"/history_future"_s, PJ::sdk::BuiltinObjectType::kNone}});
  PJ::MainWindowHistoryTestPeer::resetHistory(window);

  QDomDocument intent_doc;
  QDomElement intent = intent_doc.createElement(u"curve"_s);
  intent.setAttribute(u"pending_intent"_s, u"true"_s);
  intent.setAttribute(u"topic"_s, u"/history_future"_s);
  intent.setAttribute(u"field"_s, QString{});
  intent.setAttribute(u"dataset_id"_s, QString::number(dataset));
  intent.setAttribute(u"dataset_source"_s, u"history-pending-stream"_s);
  intent_doc.appendChild(intent);
  ASSERT_TRUE(plot->rememberPendingCurveIntent(intent, /*notify=*/true));
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  PJ::MainWindowHistoryTestPeer::undo(window);
  plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  EXPECT_EQ(plot->pendingCurveIntentCount(), 0U);
  PJ::MainWindowHistoryTestPeer::redo(window);
  plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  ASSERT_EQ(plot->pendingCurveIntentCount(), 1U);
  const std::size_t history_size_before_materialize = PJ::MainWindowHistoryTestPeer::undoSize(window);

  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/history_future"), 0U);
  EXPECT_EQ(plot->pendingCurveIntentCount(), 0U);
  EXPECT_FALSE(plot->curveList().empty());
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), history_size_before_materialize)
      << "fulfilling an existing intent is not a second user edit";
}

TEST_F(MainWindowHistoryFixture, ReplayedPendingCurveAutomaticallyRebuildsDemandBinding) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setDataServices(&app_session.sessionManager(), &app_session.catalogModel());
  plot->removeAllCurves();

  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "history-replayed-stream");
  ASSERT_NE(dataset, 0U);
  app_session.catalogModel().setAdvertisedTopics(
      dataset, {PJ::AdvertisedTopic{u"/history_replayed"_s, PJ::sdk::BuiltinObjectType::kNone}});

  QDomDocument doc;
  QDomElement state = doc.createElement(u"plot"_s);
  state.setAttribute(u"id"_s, plot->stateId());
  state.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement pending = doc.createElement(u"curve"_s);
  pending.setAttribute(u"pending_intent"_s, u"true"_s);
  pending.setAttribute(u"topic"_s, u"/history_replayed"_s);
  pending.setAttribute(u"field"_s, QString{});
  pending.setAttribute(u"dataset_id"_s, QString::number(dataset));
  pending.setAttribute(u"dataset_source"_s, u"history-replayed-stream"_s);
  state.appendChild(pending);
  doc.appendChild(state);

  ASSERT_TRUE(plot->xmlLoadState(state));
  QApplication::processEvents();
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/history_replayed"), 0U);
  EXPECT_EQ(plot->pendingCurveIntentCount(), 0U);
  EXPECT_EQ(plot->curveList().size(), 1U);
}

TEST_F(MainWindowHistoryFixture, FailedProcessorRebuildLeavesUndoTransactionUnchanged) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"processor-transaction-before"_s);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"processor-transaction-after"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const QByteArray live_before_undo = PJ::MainWindowHistoryTestPeer::liveState(window);

  QDomDocument rejected_target;
  ASSERT_TRUE(static_cast<bool>(rejected_target.setContent(live_before_undo)));
  addSavedAbsoluteProcessor(
      rejected_target, std::numeric_limits<PJ::DatasetId>::max(), u"missing-processor-source"_s,
      u"/missing_processor_input"_s);
  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, rejected_target.toByteArray(2));
  PJ::MainWindowHistoryTestPeer::undo(window);

  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), live_before_undo);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);
}

TEST_F(MainWindowHistoryFixture, ProgressiveProcessorWaitsUntilDrainForLateInput) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::DataProcessorService& processors = app_session.sessionManager().dataProcessorService();
  processors.clearAllFilters();
  const PJ::DatasetId dataset = pj_test::createDataset(app_session, "progressive-processor-late");
  ASSERT_NE(dataset, 0U);
  app_session.catalogModel().setAdvertisedTopics(
      dataset, {PJ::AdvertisedTopic{u"/late_processor_input"_s, PJ::sdk::BuiltinObjectType::kNone}});

  QDomDocument target;
  ASSERT_TRUE(static_cast<bool>(target.setContent(PJ::MainWindowHistoryTestPeer::liveState(window))));
  addSavedAbsoluteProcessor(target, dataset, u"progressive-processor-late"_s, u"/late_processor_input"_s);
  PJ::MainWindowHistoryTestPeer::beginProgressiveRestore(window, target);
  ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));
  EXPECT_TRUE(processors.recipes().empty()) << "begin must clear only while file data is still arriving";

  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/late_processor_input"), 0U);
  PJ::MainWindowHistoryTestPeer::drainProgressiveRestore(window);
  EXPECT_FALSE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));
  EXPECT_EQ(processors.recipes().size(), 1U);
  processors.clearAllFilters();
  app_session.catalogModel().rebuildFromDatastore();
}

TEST_F(MainWindowHistoryFixture, ProgressiveProcessorMissingAtDrainRollsWorkspaceBack) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::DataProcessorService& processors = app_session.sessionManager().dataProcessorService();
  processors.clearAllFilters();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"progressive-processor-rollback-before"_s);
  const QByteArray previous_workspace = PJ::MainWindowHistoryTestPeer::liveState(window);

  QDomDocument target;
  ASSERT_TRUE(static_cast<bool>(target.setContent(previous_workspace)));
  QDomElement target_plot = target.elementsByTagName(u"plot"_s).at(0).toElement();
  if (!target_plot.isNull()) {
    target_plot.setAttribute(u"id"_s, u"progressive-processor-rollback-target"_s);
  }
  addSavedAbsoluteProcessor(target, std::numeric_limits<PJ::DatasetId>::max(), u"never-arrives"_s, u"/never_arrives"_s);
  PJ::MainWindowHistoryTestPeer::beginProgressiveRestore(window, target);
  ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));

  closeNextModalDialog();
  PJ::MainWindowHistoryTestPeer::drainProgressiveRestore(window);
  EXPECT_FALSE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), previous_workspace);
  EXPECT_TRUE(processors.recipes().empty());
}

TEST_F(MainWindowHistoryFixture, InvalidSceneElementFailsExactUndoTransactionally) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"scene-transaction-before"_s);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"scene-transaction-after"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const QByteArray live_before_undo = PJ::MainWindowHistoryTestPeer::liveState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  // Scene docks are restore participants: a permanently invalid scene element
  // in the undo target must fail the whole exact transaction, leaving live
  // state and both stacks untouched.
  QDomDocument rejected_target;
  ASSERT_TRUE(static_cast<bool>(rejected_target.setContent(live_before_undo)));
  QDomElement tab = rejected_target.documentElement().firstChildElement(u"tabbed_widget"_s).firstChildElement(u"Tab"_s);
  ASSERT_FALSE(tab.isNull());
  QDomElement area =
      tab.firstChildElement(u"Container"_s).firstChildElement(u"DockSplitter"_s).isNull()
          ? tab.firstChildElement(u"Container"_s).firstChildElement(u"DockArea"_s)
          : tab.firstChildElement(u"Container"_s).firstChildElement(u"DockSplitter"_s).firstChildElement(u"DockArea"_s);
  ASSERT_FALSE(area.isNull());
  QDomElement scene = rejected_target.createElement(u"scene3d"_s);
  QDomElement bad_layer = rejected_target.createElement(u"layer"_s);
  bad_layer.setAttribute(u"object_type"_s, u"not_a_type"_s);
  scene.appendChild(bad_layer);
  area.removeChild(area.firstChildElement());
  area.appendChild(scene);
  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, rejected_target.toByteArray(2));

  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), live_before_undo);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);

  // Second arm: a WELL-FORMED scene layer whose dataset is not loaded defers at
  // the dock — an exact snapshot must treat that unresolved BLOCKING reference
  // as a failed transaction too (a partial scene must never become the tip).
  QDomDocument deferred_target;
  ASSERT_TRUE(static_cast<bool>(deferred_target.setContent(live_before_undo)));
  QDomElement deferred_tab =
      deferred_target.documentElement().firstChildElement(u"tabbed_widget"_s).firstChildElement(u"Tab"_s);
  QDomElement deferred_area =
      deferred_tab.firstChildElement(u"Container"_s).firstChildElement(u"DockSplitter"_s).isNull()
          ? deferred_tab.firstChildElement(u"Container"_s).firstChildElement(u"DockArea"_s)
          : deferred_tab.firstChildElement(u"Container"_s)
                .firstChildElement(u"DockSplitter"_s)
                .firstChildElement(u"DockArea"_s);
  ASSERT_FALSE(deferred_area.isNull());
  QDomElement deferred_scene = deferred_target.createElement(u"scene3d"_s);
  QDomElement ghost_layer = deferred_target.createElement(u"layer"_s);
  ghost_layer.setAttribute(u"dataset_id"_s, u"999"_s);
  ghost_layer.setAttribute(u"dataset_source"_s, u"ghost.dat"_s);
  ghost_layer.setAttribute(u"topic_name"_s, u"/ghost"_s);
  ghost_layer.setAttribute(u"object_type"_s, u"kPointCloud"_s);
  deferred_scene.appendChild(ghost_layer);
  deferred_area.removeChild(deferred_area.firstChildElement());
  deferred_area.appendChild(deferred_scene);
  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, deferred_target.toByteArray(2));

  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::liveState(window), live_before_undo);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);
}

// The usage report counts a panel once per session: undo and redo rebuild it, and every rebuild is the same use.
TEST_F(MainWindowHistoryFixture, UndoAndRedoCountAScenePanelOncePerSession) {
  PJ::MainWindow& window = mainWindow();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"usage-before"_s);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"usage-after"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const QByteArray live_before_undo = PJ::MainWindowHistoryTestPeer::liveState(window);

  QDomDocument target;
  ASSERT_TRUE(static_cast<bool>(target.setContent(live_before_undo)));
  QDomElement area = target.elementsByTagName(u"DockArea"_s).at(0).toElement();
  ASSERT_FALSE(area.isNull());
  area.removeChild(area.firstChildElement());
  area.appendChild(target.createElement(u"scene3d"_s));
  PJ::MainWindowHistoryTestPeer::replaceUndoTarget(window, target.toByteArray(2));

  PJ::UsageCounters& counters = PJ::usageCounters();
  counters.configure({}, u"test"_s);
  counters.setEnabled(true);
  for (int cycle = 0; cycle < 3; ++cycle) {
    PJ::MainWindowHistoryTestPeer::undo(window);
    ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::liveState(window).contains("<scene3d")) << "cycle " << cycle;
    PJ::MainWindowHistoryTestPeer::redo(window);
    ASSERT_FALSE(PJ::MainWindowHistoryTestPeer::liveState(window).contains("<scene3d")) << "cycle " << cycle;
  }
  EXPECT_EQ(counters.today().features.at("panel.scene3d"), 1);
  counters.setEnabled(false);
}

// An undo restores into the live plots whose ids match: a tab the edit did not touch keeps its plot widget (and with it
// the curves' sample caches), while the edited tab returns to its saved state.
TEST_F(MainWindowHistoryFixture, UndoKeepsThePlotOfAnUntouchedTab) {
  PJ::MainWindow& window = mainWindow();
  QDomDocument doc;
  ASSERT_TRUE(
      static_cast<bool>(doc.setContent(
          u"<root binding=\"generic\" format=\"PlotJuggler\" pj4_version=\"4\">"
          u"<tabbed_widget id=\"\" name=\"main\" parent=\"main_window\">"
          u"<Tab containers=\"1\" id=\"tab-edited\" tab_name=\"edited\"><Container><DockSplitter count=\"1\" "
          u"orientation=\"-\" sizes=\"1\"><DockArea id=\"area-edited\" name=\"...\"><plot id=\"plot-edited\" "
          u"mode=\"TimeSeries\" style=\"Lines\" tracker_enabled=\"true\"/></DockArea></DockSplitter></Container></Tab>"
          u"<Tab containers=\"1\" id=\"tab-kept\" tab_name=\"kept\"><Container><DockSplitter count=\"1\" "
          u"orientation=\"-\" sizes=\"1\"><DockArea id=\"area-kept\" name=\"...\"><plot id=\"plot-kept\" "
          u"mode=\"TimeSeries\" style=\"Lines\"/></DockArea></DockSplitter></Container></Tab>"
          u"<currentTabIndex index=\"0\"/></tabbed_widget></root>"_s)));
  ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::restoreLayout(window, doc));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  ASSERT_EQ(tabbed->dockerCount(), 2);
  const auto plot_in = [tabbed](int tab) -> PJ::PlotWidget* {
    PJ::PlotDocker* docker = tabbed->dockerAt(tab);
    return docker != nullptr && docker->plotCount() > 0 && docker->plotAt(0) != nullptr
               ? docker->plotAt(0)->plotWidget()
               : nullptr;
  };
  PJ::PlotWidget* edited = plot_in(0);
  const QPointer<PJ::PlotWidget> kept = plot_in(1);
  ASSERT_NE(edited, nullptr);
  ASSERT_NE(kept, nullptr);

  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  edited->setTrackerEnabled(false);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  PJ::MainWindowHistoryTestPeer::undo(window);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  ASSERT_NE(plot_in(0), nullptr);
  EXPECT_TRUE(plot_in(0)->trackerEnabled());
  EXPECT_FALSE(kept.isNull()) << "the untouched tab's plot was destroyed and rebuilt";
  EXPECT_EQ(plot_in(1), kept.data());

  // The restore re-wires the reused plot: its status message still reaches the diagnostics once.
  int reports = 0;
  const QMetaObject::Connection counter = QObject::connect(
      window.diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, &window,
      [&reports](int /*level*/, const QString& /*source*/, const QString& id, const QString& message) {
        reports += id == u"status"_s && message == u"reused"_s ? 1 : 0;
      });
  ASSERT_FALSE(kept.isNull());
  emit kept->statusMessageRequested(u"reused"_s);
  QCoreApplication::processEvents();
  QObject::disconnect(counter);
  EXPECT_EQ(reports, 1);
}

// A reused plot comes back exactly as saved: curve order, and the curve editor bound to it shows the restored rows.
TEST_F(MainWindowHistoryFixture, UndoOnAReusedPlotRestoresCurveOrderAndTheEditorRows) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "order");
  ASSERT_NE(dataset, 0U);
  QStringList keys;
  for (const char* topic : {"/a", "/b", "/c"}) {
    const PJ::TopicId id = pj_test::addScalarTopic(app, dataset, topic);
    ASSERT_NE(id, 0U);
    keys << catalogKeyForTopic(app.catalogModel(), id);
  }
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  for (const QString& key : keys) {
    ASSERT_NE(plot->addCurve(key), nullptr);
  }
  auto* editor = window.findChild<PJ::CurveEditor*>();
  ASSERT_NE(editor, nullptr);
  editor->setPlot(plot);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);

  plot->curveFromTitle(keys[1])->curve->setVisible(false);
  plot->removeCurve(keys[0]);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  PJ::MainWindowHistoryTestPeer::undo(window);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();

  PJ::PlotWidget* restored = ensureCurrentPlot(window);
  ASSERT_NE(restored, nullptr);
  QStringList order;
  for (const auto& info : restored->curveList()) {
    order << info.source_name;
  }
  EXPECT_EQ(order, keys);
  ASSERT_EQ(editor->plot(), restored);
  const auto toggles = editor->findChildren<QToolButton*>(u"curveVisibilityToggle"_s);
  ASSERT_EQ(toggles.size(), 3);
  for (const QToolButton* toggle : toggles) {
    EXPECT_TRUE(toggle->isChecked()) << "an editor row kept the undone visibility";
  }
}

// A dock the user focused is replaced by the restore: the curve editor must follow the plot, not the removed dock.
TEST_F(MainWindowHistoryFixture, UndoKeepsTheFocusedPlotBoundToTheCurveEditor) {
  PJ::MainWindow& window = mainWindow();
  window.show();
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  PJ::PlotDocker* docker = tabbed->currentTab();
  ASSERT_NE(docker, nullptr);
  docker->focusDock(docker->plotAt(0));
  QCoreApplication::processEvents();
  auto* editor = window.findChild<PJ::CurveEditor*>();
  ASSERT_NE(editor, nullptr);
  ASSERT_EQ(editor->plot(), plot);

  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setTrackerEnabled(!plot->trackerEnabled());
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  PJ::MainWindowHistoryTestPeer::undo(window);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();

  EXPECT_NE(editor->plot(), nullptr) << "the editor was bound to the removed dock";
  EXPECT_EQ(editor->plot(), ensureCurrentPlot(window));
}

// PJ_BENCH=1: how long one undo and one redo block the GUI thread on a heavy workspace. Every history restore
// re-applies every non-exempt dock from the saved XML, so the cost grows with the workspace. Reports the median of
// kRounds undo and redo calls for: one plot; 16 plots of 5 curves over 100k-sample series; the same plus two Scene3D
// docks each showing a 1M-point cloud. "settled" adds the event-loop turns the restore queues (deferred
// rebinds, layout passes; rendering only where the platform renders).
TEST_F(MainWindowHistoryFixture, UndoRedoLatencyBenchmark) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kSeries = 80;
  constexpr int kSamples = 100'000;
  constexpr int kRounds = 7;
  PJ::MainWindow& window = mainWindow();
  window.resize(1600, 1000);
  window.show();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);

  const PJ::DatasetId dataset = pj_test::createDataset(app, "bench.mcap");
  ASSERT_NE(dataset, 0U);
  {
    PJ::DataWriter writer = app.sessionManager().dataEngine().createWriter();
    for (int s = 0; s < kSeries; ++s) {
      auto handle = writer.registerScalarSeries(dataset, "sig" + std::to_string(s), PJ::NumericType::kFloat64);
      ASSERT_TRUE(handle.has_value());
      for (int i = 0; i < kSamples; ++i) {
        writer.appendScalar(*handle, PJ::Timestamp{i} * 1'000'000, std::sin(0.001 * i + s));
      }
    }
    ASSERT_FALSE(app.sessionManager().commitChunks(writer.flushAll()).empty());
  }
#ifdef PJ_WITH_SCENE3D
  {
    std::vector<float> xyz;
    constexpr int kSide = 100;  // 1M points
    xyz.reserve(std::size_t{kSide} * kSide * kSide * 3);
    for (int i = 0; i < kSide; ++i) {
      for (int j = 0; j < kSide; ++j) {
        for (int k = 0; k < kSide; ++k) {
          xyz.insert(
              xyz.end(), {static_cast<float>(i) * 0.01F, static_cast<float>(j) * 0.01F, static_cast<float>(k) * 0.01F});
        }
      }
    }
    using DT = PJ::sdk::PointField::Datatype;
    PJ::sdk::PointCloud cloud;
    cloud.frame_id = "map";
    cloud.fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
    cloud.point_step = 12;
    cloud.width = static_cast<uint32_t>(xyz.size() / 3);
    cloud.height = 1;
    cloud.row_step = cloud.point_step * cloud.width;
    cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(xyz.data()), xyz.size() * sizeof(float));
    cloud.timestamp_ns = 1'000'000;
    auto& store = app.sessionManager().objectStore();
    const auto points = store.registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset,
            .topic_name = "/points",
            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"});
    ASSERT_TRUE(points.has_value());
    ASSERT_TRUE(store.pushOwned(*points, 1'000'000, PJ::serializePointCloud(cloud)).has_value());
  }
#endif
  app.catalogModel().rebuildFromDatastore();

  // One tab per row of docks: `plots` plots of 5 curves each (4 per tab) and `scenes` Scene3D docks on a tab of
  // their own.
  const auto workspace = [](int plots, int scenes) {
    QString xml =
        u"<root binding=\"generic\" format=\"PlotJuggler\" pj4_version=\"4\">"
        u"<tabbed_widget id=\"\" name=\"main\" parent=\"main_window\">"_s;
    const auto tab = [&xml](const QString& name, const QStringList& areas) {
      QStringList sizes;
      for (int i = 0; i < areas.size(); ++i) {
        sizes << QString::number(1.0 / static_cast<double>(areas.size()), 'f', 8);
      }
      xml +=
          u"<Tab containers=\"1\" tab_name=\"%1\"><Container><DockSplitter count=\"%2\" orientation=\"|\" "
          u"sizes=\"%3\">"_s.arg(name)
              .arg(areas.size())
              .arg(sizes.join(u';'));
      for (const QString& area : areas) {
        xml += u"<DockArea name=\"...\">"_s + area + u"</DockArea>"_s;
      }
      xml += u"</DockSplitter></Container></Tab>"_s;
    };
    QStringList areas;
    for (int p = 0; p < plots; ++p) {
      QString plot = u"<plot mode=\"TimeSeries\" style=\"Lines\" tracker_enabled=\"true\">"_s;
      for (int c = 0; c < 5; ++c) {
        plot +=
            u"<curve color=\"#1f77b4\" field=\"value\" topic=\"sig%1\" visible=\"true\"/>"_s.arg((p * 5 + c) % kSeries);
      }
      areas << plot + u"</plot>"_s;
      if (areas.size() == 4 || p + 1 == plots) {
        tab(u"plots%1"_s.arg(p / 4), areas);
        areas.clear();
      }
    }
    for (int s = 0; s < scenes; ++s) {
      areas << u"<scene3d fixed_frame=\"map\" fixed_frame_mode=\"auto_root\" version=\"1\"><layer "
               u"display_name=\"points\" "
               u"object_type=\"kPointCloud\" order=\"0\" topic_name=\"/points\" visible=\"true\"/></scene3d>"_s;
    }
    if (!areas.isEmpty()) {
      tab(u"scenes"_s, areas);
    }
    return xml + u"<currentTabIndex index=\"0\"/></tabbed_widget></root>"_s;
  };

  const auto settle = []() {
    QElapsedTimer idle;
    idle.start();
    while (idle.elapsed() < 30) {  // until 30 ms pass in which no event-loop pass did work (> 0.5 ms)
      QElapsedTimer pass;
      pass.start();
      QCoreApplication::processEvents(QEventLoop::AllEvents);
      // A running event loop deletes the replaced docks; processEvents() from here never would.
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      if (pass.nsecsElapsed() > 500'000) {
        idle.restart();
      } else {
        QThread::msleep(1);
      }
    }
  };
  const auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };

  struct Case {
    const char* name;
    int plots;
    int scenes;
  };
  for (const Case& bench_case :
       {Case{"1 plot", 1, 0}, Case{"16 plots x 5 curves x 100k", 16, 0},
        Case{"16 plots + 2 Scene3D x 1M points", 16, 2}}) {
#ifndef PJ_WITH_SCENE3D
    if (bench_case.scenes > 0) {
      continue;
    }
#endif
    QDomDocument doc;
    ASSERT_TRUE(static_cast<bool>(doc.setContent(workspace(bench_case.plots, bench_case.scenes))));
    ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::restoreLayout(window, doc)) << bench_case.name;
    settle();
    PJ::PlotWidget* plot = ensureCurrentPlot(window);
    ASSERT_NE(plot, nullptr);
    plot->setStateId(u"bench-before"_s);
    PJ::MainWindowHistoryTestPeer::resetHistory(window);
    plot->setStateId(u"bench-after"_s);
    PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
    ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

    std::vector<double> undo_ms, redo_ms, undo_settled_ms, redo_settled_ms;
    QElapsedTimer timer;
    for (int round = 0; round < kRounds; ++round) {
      timer.start();
      PJ::MainWindowHistoryTestPeer::undo(window);
      undo_ms.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
      settle();
      undo_settled_ms.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6 - 30.0);
      ASSERT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 1U) << bench_case.name;
      timer.start();
      PJ::MainWindowHistoryTestPeer::redo(window);
      redo_ms.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
      settle();
      redo_settled_ms.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6 - 30.0);
    }
    int scene_views = 0;
#ifdef PJ_WITH_SCENE3D
    for (auto* scene : window.findChildren<PJ::Scene3DDockWidget*>()) {
      scene_views += scene->sceneView() != nullptr ? 1 : 0;
    }
#endif
    std::printf(
        "[PJ_BENCH] undo/redo, %s (platform %s, %d live scene views): undo %.1f ms (settled %.1f), "
        "redo %.1f ms (settled %.1f); median of %d\n",
        bench_case.name, qPrintable(QGuiApplication::platformName()), scene_views, median(undo_ms),
        median(undo_settled_ms), median(redo_ms), median(redo_settled_ms), kRounds);
  }
}

TEST_F(MainWindowHistoryFixture, ProgressiveFileCompletionLeavesHistoryTransactionOwnedByDrain) {
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);

  const PJ::DatasetId original =
      addScalarDataset(app_session, "progressive-history.mcap", "/progressive", 500'000'000'000LL, 501'000'000'000LL);
  ASSERT_NE(original, 0U);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"history-before-progressive-load"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);

  PJ::MainWindowHistoryTestPeer::setProgressiveRestoreInFlight(window, true);
  app_session.catalogModel().removeDataset(original, /*tombstone=*/false);
  app_session.sessionManager().evictDatasetObjects(original);
  app_session.sessionManager().removeDataset(original);
  ASSERT_NE(
      addScalarDataset(app_session, "progressive-history.mcap", "/progressive", 600'000'000'000LL, 601'000'000'000LL),
      0U);

  PJ::MainWindowHistoryTestPeer::completeFileLoad(window, u"/tmp/progressive-history.mcap"_s);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U) << "the queue drain owns the progressive baseline";

  PJ::MainWindowHistoryTestPeer::setProgressiveRestoreInFlight(window, false);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
}

TEST_F(MainWindowHistoryFixture, SourceTimelineUndoRedoIsExactTransactionalAndNotPersistedWithSessionIds) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  QTemporaryDir output_dir;
  ASSERT_TRUE(output_dir.isValid());

  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app_session = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app_session.sessionManager();
  const PJ::DatasetId left = addScalarDataset(app_session, "timeline-left", "/left", 0, 100 * kSecond);
  const PJ::DatasetId right = addScalarDataset(app_session, "timeline-right", "/right", 10 * kSecond, 110 * kSecond);
  ASSERT_NE(left, 0U);
  ASSERT_NE(right, 0U);
  app_session.recomputeRange();
  QApplication::processEvents();

  PJ::Timeline& timeline = PJ::MainWindowHistoryTestPeer::sourceTimeline(window);
  PJ::SourceTimelineController& controller = PJ::MainWindowHistoryTestPeer::sourceTimelineController(window);
  session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{2 * kSecond}});
  session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{-3 * kSecond}});
  controller.setDisplayOrder({left, right});
  timeline.setDatasetFilter(u"timeline-"_s);
  timeline.setZoom(1.0e-8);
  timeline.setViewportLeftDisplayNs(5 * kSecond);
  timeline.resizeNameColumn(180);
  PJ::MainWindowHistoryTestPeer::setTimelineSnap(window, false);

  const double initial_zoom = timeline.zoom();
  const qint64 initial_left_ns = timeline.viewportLeftDisplayNs();
  const int initial_width = timeline.nameColumnWidth();
  const std::vector<PJ::DatasetId> initial_order = controller.currentTrackOrder();
  ASSERT_GE(initial_order.size(), 2U);
  const QByteArray initial_xml = PJ::MainWindowHistoryTestPeer::liveState(window);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);

  QDomDocument initial_doc;
  ASSERT_TRUE(static_cast<bool>(initial_doc.setContent(initial_xml)));
  EXPECT_EQ(initial_doc.elementsByTagName(u"workspace_timeline"_s).size(), 0);
  EXPECT_EQ(initial_doc.elementsByTagName(u"source_timeline"_s).size(), 0);

  // Stage all edited values without creating intermediate history entries, then
  // commit through the real row-reorder gesture path.
  {
    const QSignalBlocker timeline_blocker(&timeline);
    session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{7 * kSecond}});
    session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{-9 * kSecond}});
    timeline.setZoom(2.0e-8);
    timeline.setViewportLeftDisplayNs(25 * kSecond);
    timeline.resizeNameColumn(240);
    PJ::MainWindowHistoryTestPeer::setTimelineSnap(window, true);
  }
  timeline.reorderForTest(/*from=*/0, /*drop_index=*/2);

  ASSERT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  const std::vector<PJ::DatasetId> edited_order = controller.currentTrackOrder();
  const double edited_zoom = timeline.zoom();
  const qint64 edited_left_ns = timeline.viewportLeftDisplayNs();
  const int edited_width = timeline.nameColumnWidth();

  const QString layout_path = output_dir.filePath(u"portable.pj4.xml"_s);
  PJ::MainWindowHistoryTestPeer::saveGenericLayout(window, layout_path);
  QFile layout_file(layout_path);
  ASSERT_TRUE(layout_file.open(QIODevice::ReadOnly));
  QDomDocument portable_doc;
  ASSERT_TRUE(static_cast<bool>(portable_doc.setContent(layout_file.readAll())));
  EXPECT_EQ(portable_doc.elementsByTagName(u"workspace_timeline"_s).size(), 0);
  EXPECT_EQ(portable_doc.elementsByTagName(u"source_timeline"_s).size(), 1);

  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 2 * kSecond);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -3 * kSecond);
  EXPECT_EQ(controller.currentTrackOrder(), initial_order);
  EXPECT_DOUBLE_EQ(timeline.zoom(), initial_zoom);
  EXPECT_NEAR(static_cast<double>(timeline.viewportLeftDisplayNs()), static_cast<double>(initial_left_ns), 1.0e8);
  EXPECT_EQ(timeline.nameColumnWidth(), initial_width);
  EXPECT_FALSE(PJ::MainWindowHistoryTestPeer::timelineSnap(window));

  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 7 * kSecond);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -9 * kSecond);
  EXPECT_EQ(controller.currentTrackOrder(), edited_order);
  EXPECT_DOUBLE_EQ(timeline.zoom(), edited_zoom);
  EXPECT_NEAR(static_cast<double>(timeline.viewportLeftDisplayNs()), static_cast<double>(edited_left_ns), 5.0e7);
  EXPECT_EQ(timeline.nameColumnWidth(), edited_width);
  EXPECT_TRUE(PJ::MainWindowHistoryTestPeer::timelineSnap(window));

  // An offset whose raw-offset subtraction would overflow is rejected before
  // any track or chrome mutation.
  PJ::MainWindowHistoryTestPeer::replaceUndoTargetOffset(window, std::numeric_limits<qint64>::min());
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 7 * kSecond);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -9 * kSecond);
  EXPECT_EQ(controller.currentTrackOrder(), edited_order);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);

  PJ::MainWindowHistoryTestPeer::replaceUndoTargetOffset(window, 2 * kSecond);
  PJ::MainWindowHistoryTestPeer::replaceUndoTargetDatasetId(window, std::numeric_limits<PJ::DatasetId>::max());
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 7 * kSecond);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -9 * kSecond);
  EXPECT_EQ(controller.currentTrackOrder(), edited_order);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::undoSize(window), 2U);
  EXPECT_EQ(PJ::MainWindowHistoryTestPeer::redoSize(window), 0U);
  timeline.setDatasetFilter(QString{});
}

TEST_F(MainWindowHistoryFixture, UndoOfASourceTimelineDragKeepsGlobalMarkersAtTheirDisplayInstant) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId a = addScalarDataset(app, "a", "/sensor", 100 * kSecond, 110 * kSecond);
  const PJ::DatasetId b = addScalarDataset(app, "b", "/sensor", 100 * kSecond, 110 * kSecond);
  ASSERT_NE(a, 0U);
  ASSERT_NE(b, 0U);
  constexpr std::string_view kRule = "test/global-rule";
  PJ::BatchEngine::GeneratorRecipe recipe;
  recipe.id = std::string(kRule);
  recipe.all_datasets = true;
  recipe.inputs = {"/sensor/value"};
  recipe.outputs = {std::string(PJ::kAllDatasetsMarkerTopic)};
  recipe.script = R"(
    local s = series("/sensor/value")
    for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
  )";
  const auto inserted = session.batchEngine().upsertGenerator(recipe);
  ASSERT_TRUE(inserted.has_value()) << (inserted.has_value() ? std::string{} : inserted.error());
  // Both datasets are aligned, so each contributes its two samples at their raw instants.
  const std::vector<PJ::Timestamp> baseline{100 * kSecond, 100 * kSecond, 110 * kSecond, 110 * kSecond};
  ASSERT_EQ(allDatasetsMarkerTimes(app, kRule), baseline);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);

  // A drag alone republishes nothing: the set stays where it was drawn.
  session.setDisplayOffset(a, PJ::DisplayOffset{PJ::Duration{20 * kSecond}});
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), baseline);

  // Undo replays the rule once the offset is back at 0, so nothing moves.
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), baseline);

  // Redo replays under the snapshot's offset: a's raw 100/110 sit at display 80/90.
  PJ::MainWindowHistoryTestPeer::redo(window);
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 20 * kSecond);
  const std::vector<PJ::Timestamp> redone{80 * kSecond, 90 * kSecond, 100 * kSecond, 110 * kSecond};
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), redone);

  // Undoing a drag of the other track restores its offset before the replay too.
  session.setDisplayOffset(b, PJ::DisplayOffset{PJ::Duration{5 * kSecond}});
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(session.sourceDisplayOffset(b).value.count(), 0);
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), redone);
}

// A run staged at a dragged offset is still computing when an undo restores the
// offset: the undo re-creates the all-datasets rule, which cancels that run, so the
// set is drawn in the restored frame when the undo returns and the cancelled run's
// result never lands.
TEST_F(MainWindowHistoryFixture, UndoOfADragDuringAGlobalRunDrawsTheRestoredFrame) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app.sessionManager();
  PJ::BatchEngine& engine = session.batchEngine();
  const PJ::DatasetId a = addScalarDataset(app, "a", "/sensor", 100 * kSecond, 110 * kSecond);
  const PJ::DatasetId b = addScalarDataset(app, "b", "/sensor", 100 * kSecond, 110 * kSecond);
  ASSERT_NE(a, 0U);
  ASSERT_NE(b, 0U);
  constexpr std::string_view kRule = "test/global-rule";
  PJ::BatchEngine::GeneratorRecipe recipe;
  recipe.id = std::string(kRule);
  recipe.all_datasets = true;
  recipe.inputs = {"/sensor/value"};
  recipe.outputs = {std::string(PJ::kAllDatasetsMarkerTopic)};
  recipe.script = R"(
    local s = series("/sensor/value")
    for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
  )";
  ASSERT_TRUE(engine.upsertGenerator(recipe).has_value());
  const std::vector<PJ::Timestamp> baseline{100 * kSecond, 100 * kSecond, 110 * kSecond, 110 * kSecond};
  ASSERT_EQ(allDatasetsMarkerTimes(app, kRule), baseline);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  session.setDisplayOffset(a, PJ::DisplayOffset{PJ::Duration{20 * kSecond}});
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);

  HeldMarkerComputes held(engine);
  engine.refresh(kRule);
  ASSERT_TRUE(engine.isRunning(kRule)) << "the refresh staged a's samples 20 s earlier";
  PJ::MainWindowHistoryTestPeer::undo(window);
  ASSERT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), baseline) << "drawn in the restored frame when the undo returns";

  held.release();
  EXPECT_TRUE(QTest::qWaitFor([&] { return held.idle() && !engine.isRunning(kRule); }, 5000));
  QTest::qWait(50);  // the cancelled run's publish would land here
  EXPECT_EQ(allDatasetsMarkerTimes(app, kRule), baseline);
}

// A marker rule bound to dataset `dataset` drawing one marker per /sensor sample, or,
// with `first_only`, at the first sample only.
PJ::BatchEngine::GeneratorRecipe sensorRule(PJ::DatasetId dataset, bool first_only = false) {
  PJ::BatchEngine::GeneratorRecipe recipe;
  recipe.id = "test/slow-rule";
  recipe.dataset_id = dataset;
  recipe.inputs = {"/sensor/value"};
  recipe.outputs = {std::string(PJ::sdk::kGlobalMarkerTopic)};
  recipe.script = first_only ? R"(
    local s = series("/sensor/value")
    createMarker(s:at(0).t)
  )"
                             : R"(
    local s = series("/sensor/value")
    for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
  )";
  return recipe;
}

// The sequential uid of the set `owner` last published for `output` on `dataset`:
// a republish or a tombstone mints another.
std::optional<std::uint64_t> publishUid(
    PJ::AppSession& app, PJ::DatasetId dataset, std::string_view output, std::string_view owner) {
  const PJ::ObjectStore& store = app.sessionManager().objectStore();
  const std::optional<PJ::ObjectTopicId> id = store.findTopic(dataset, PJ::markerOwnerTopicName(output, owner));
  const std::optional<PJ::ResolvedObjectEntry> entry =
      id.has_value() ? store.latestAt(*id, std::numeric_limits<PJ::Timestamp>::max()) : std::nullopt;
  return entry.has_value() ? std::optional<std::uint64_t>{entry->sequential_uid.value} : std::nullopt;
}

// An undo whose snapshot describes a live rule unchanged leaves it as it is drawn:
// however slow the rule, nothing runs, and its set is neither tombstoned nor
// republished.
TEST_F(MainWindowHistoryFixture, UndoOfAnotherStepLeavesASlowRuleDrawnAndRunsNothing) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId a = addScalarDataset(app, "a", "/sensor", 100 * kSecond, 110 * kSecond);
  ASSERT_NE(a, 0U);
  const PJ::BatchEngine::GeneratorRecipe recipe = sensorRule(a);
  const auto inserted = session.batchEngine().upsertGenerator(recipe);
  ASSERT_TRUE(inserted.has_value()) << (inserted.has_value() ? std::string{} : inserted.error());
  const std::string_view output = PJ::sdk::kGlobalMarkerTopic;
  const std::vector<PJ::Timestamp> baseline{100 * kSecond, 110 * kSecond};
  ASSERT_EQ(pj_test::markerTimes(app, a, output, recipe.id), baseline);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  plot->setStateId(u"slow-rule-before"_s);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  plot->setStateId(u"slow-rule-after"_s);
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);
  const std::optional<std::uint64_t> drawn = publishUid(app, a, output, recipe.id);
  ASSERT_TRUE(drawn.has_value());

  const HeldMarkerComputes held(session.batchEngine());
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(held.computes(), 0) << "the rule did not change, so it does not run";
  EXPECT_TRUE(session.batchEngine().runningIds().empty());
  EXPECT_EQ(publishUid(app, a, output, recipe.id), drawn) << "the drawn set is left as it is";
  EXPECT_EQ(pj_test::markerTimes(app, a, output, recipe.id), baseline);
}

// An undo that changes a rule runs it to completion before it returns, however slow
// the run: the user expects the previous markers back, not a blank set while it
// computes. The run is computed on the GUI thread, so the held pool computes never
// delay it.
TEST_F(MainWindowHistoryFixture, UndoThatChangesASlowRuleDrawsItsRestoredMarkersWhenItReturns) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app.sessionManager();
  PJ::BatchEngine& engine = session.batchEngine();
  const PJ::DatasetId a = addScalarDataset(app, "a", "/sensor", 100 * kSecond, 110 * kSecond);
  ASSERT_NE(a, 0U);
  const std::string_view output = PJ::sdk::kGlobalMarkerTopic;
  const PJ::BatchEngine::GeneratorRecipe recipe = sensorRule(a);
  ASSERT_TRUE(engine.upsertGenerator(recipe).has_value());
  const std::vector<PJ::Timestamp> baseline{100 * kSecond, 110 * kSecond};
  ASSERT_EQ(pj_test::markerTimes(app, a, output, recipe.id), baseline);
  PJ::MainWindowHistoryTestPeer::resetHistory(window);
  ASSERT_TRUE(engine.upsertGenerator(sensorRule(a, /*first_only=*/true)).has_value());
  ASSERT_EQ(pj_test::markerTimes(app, a, output, recipe.id), std::vector<PJ::Timestamp>{100 * kSecond});
  PJ::MainWindowHistoryTestPeer::recordDiscreteState(window);

  const HeldMarkerComputes held(engine);
  PJ::MainWindowHistoryTestPeer::undo(window);
  EXPECT_EQ(held.computes(), 1) << "the changed rule ran once";
  EXPECT_FALSE(engine.isRunning(recipe.id)) << "the undo waited for the run";
  ASSERT_NE(engine.firstBinding(recipe.id), nullptr);
  EXPECT_EQ(engine.firstBinding(recipe.id)->script, recipe.script) << "the snapshot's rule is installed";
  EXPECT_EQ(pj_test::markerTimes(app, a, output, recipe.id), baseline) << "its markers are drawn when the undo returns";
}

TEST_F(MainWindowHistoryFixture, ProgressiveDrainRestoresOffsetsGlobalMarkersAndHiddenScopeRows) {
  constexpr PJ::Timestamp kSecond = 1'000'000'000LL;
  QTemporaryDir source_dir;
  ASSERT_TRUE(source_dir.isValid());
  const QString source_a = source_dir.filePath(u"progressive-a.mcap"_s);
  const QString source_b = source_dir.filePath(u"progressive-b.mcap"_s);
  for (const QString& path : {source_a, source_b}) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  }
  PJ::MainWindow& window = mainWindow();
  PJ::AppSession& app = PJ::MainWindowHistoryTestPeer::appSession(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId a = pj_test::createDataset(app, "progressive-a", /*own_time_domain=*/true);
  const PJ::DatasetId b = pj_test::createDataset(app, "progressive-b", /*own_time_domain=*/true);
  ASSERT_NE(a, 0U);
  ASSERT_NE(b, 0U);
  const PJ::TopicId topic_a = pj_test::addScalarTopic(app, a, "/sensor", 100 * kSecond, 200 * kSecond);
  const PJ::TopicId topic_b = pj_test::addScalarTopic(app, b, "/other", 100 * kSecond, 200 * kSecond);
  ASSERT_NE(topic_a, 0U);
  ASSERT_NE(topic_b, 0U);
  PJ::PlotWidget* plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  const QString key_a = catalogKeyForTopic(app.catalogModel(), topic_a);
  const QString key_b = catalogKeyForTopic(app.catalogModel(), topic_b);
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());
  ASSERT_NE(plot->addCurve(key_a), nullptr);
  ASSERT_NE(plot->addCurve(key_b), nullptr);

  QDomDocument target;
  ASSERT_TRUE(static_cast<bool>(target.setContent(PJ::MainWindowHistoryTestPeer::liveState(window))));
  QDomElement root = target.documentElement();
  root.setAttribute(u"pj4_version"_s, u"4"_s);
  QDomElement sources = target.createElement(u"previouslyLoaded_Datafiles"_s);
  root.appendChild(sources);
  const std::vector<std::pair<QString, PJ::Timestamp>> source_offsets{{source_a, 20 * kSecond}, {source_b, 0}};
  for (const auto& [path, offset] : source_offsets) {
    QDomElement file = target.createElement(u"fileInfo"_s);
    file.setAttribute(u"filename"_s, path);
    file.setAttribute(u"display_offset_ns"_s, QString::number(offset));
    sources.appendChild(file);
  }
  QDomElement processors = root.firstChildElement(u"data_processors"_s);
  if (processors.isNull()) {
    processors = target.createElement(u"data_processors"_s);
    root.appendChild(processors);
  }
  QDomElement generator = target.createElement(u"generator"_s);
  generator.setAttribute(u"id"_s, u"test/progressive-global"_s);
  generator.setAttribute(u"language"_s, u"luau"_s);
  generator.setAttribute(u"all_datasets"_s, u"1"_s);
  processors.appendChild(generator);
  QDomElement input = target.createElement(u"input"_s);
  input.setAttribute(u"name"_s, u"/sensor/value"_s);
  generator.appendChild(input);
  QDomElement output = target.createElement(u"output"_s);
  output.setAttribute(u"name"_s, QString::fromStdString(std::string(PJ::kAllDatasetsMarkerTopic)));
  generator.appendChild(output);
  QDomElement script = target.createElement(u"script"_s);
  script.appendChild(
      target.createCDATASection(uR"(
    local s = series("/sensor/value")
    if s ~= nil then
      for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
    end
  )"_s));
  generator.appendChild(script);
  QDomElement saved_plot = target.elementsByTagName(u"plot"_s).at(0).toElement();
  ASSERT_FALSE(saved_plot.isNull());
  QDomElement scope = target.createElement(u"marker_scope"_s);
  scope.setAttribute(u"dataset_id"_s, QString::number(a));
  scope.setAttribute(u"dataset_source"_s, u"progressive-a"_s);
  scope.setAttribute(u"scope"_s, u"global"_s);
  scope.setAttribute(u"visible"_s, u"false"_s);
  saved_plot.appendChild(scope);

  PJ::MainWindowHistoryTestPeer::beginProgressiveRestore(window, target);
  ASSERT_TRUE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 0);
  session.setDatasetSourcePath(a, source_a);
  session.setDatasetSourcePath(b, source_b);
  // A layout load does not wait for a slow rule: it returns with the run in flight,
  // and the markers and their scope row arrive with the run's later publish.
  HeldMarkerComputes held(session.batchEngine());
  PJ::MainWindowHistoryTestPeer::drainProgressiveRestore(window);
  EXPECT_FALSE(PJ::MainWindowHistoryTestPeer::progressiveRestoreInFlight(window));
  EXPECT_EQ(session.sourceDisplayOffset(a).value.count(), 20 * kSecond);
  EXPECT_TRUE(allDatasetsMarkerTimes(app, "test/progressive-global").empty()) << "the run is still computing";
  plot = ensureCurrentPlot(window);
  ASSERT_NE(plot, nullptr);
  EXPECT_TRUE(plot->markerScopeRows().empty()) << "nothing is published yet";

  held.release();
  EXPECT_TRUE(QTest::qWaitFor([&plot] { return !plot->markerScopeRows().empty(); }, 5000))
      << "the late publish brings the scope row";
  EXPECT_FALSE(plot->datasetMarkerScopeVisible(a, PJ::MarkerScope::kAllDatasets)) << "the saved hidden scope survives";
  // Only a carries /sensor; replayed under its restored 20 s offset, raw 100/200 sit at 80/180.
  EXPECT_EQ(
      allDatasetsMarkerTimes(app, "test/progressive-global"),
      (std::vector<PJ::Timestamp>{80 * kSecond, 180 * kSecond}));
}

}  // namespace

namespace {

// Preserve this suite's settings scope without changing other runner cases.
class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("MainWindowHistoryFixture")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"main_window_history_test"_s);
    QSettings().clear();
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

}  // namespace
