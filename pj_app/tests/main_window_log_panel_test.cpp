// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The Log panel through the shell: the placeholder's Log button, a log topic dropped on an empty area, the app-wide
// time format, and a layout round trip.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <memory>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/log_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_log/LogDockWidget.h"
#include "pj_log/LogView.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowLogPanelTestPeer {
 public:
  static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static QDomDocument save(const MainWindow& window) {
    return window.xmlSaveState();
  }
  static bool load(MainWindow& window, const QDomDocument& doc) {
    return window.xmlLoadState(doc);
  }
  static void applyTimeLabelFormat(MainWindow& window) {
    window.applyTimeLabelFormat();
  }
  static void trash(MainWindow& window, const QString& key) {
    window.onCatalogTrashRequested(QStringList{key}, /*covers_all=*/false);
  }
  static void removeDataset(MainWindow& window, DatasetId dataset_id) {
    window.removeDatasets({dataset_id});
  }
};

namespace {

using Peer = MainWindowLogPanelTestPeer;

class MainWindowLogPanelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    window_ = std::make_unique<MainWindow>(extensions_dir_.path());
    AppSession& app = Peer::session(*window_);
    dataset_ = pj_test::createDataset(app, "robot.mcap");
    ASSERT_NE(dataset_, 0U);
    auto topic = app.sessionManager().objectStore().registerTopic(
        ObjectTopicDescriptor{
            .dataset_id = dataset_, .topic_name = "/rosout", .metadata_json = R"({"builtin_object_type":"kLog"})"});
    ASSERT_TRUE(topic.has_value()) << topic.error();
    for (int second = 1; second <= 2; ++second) {
      sdk::Log log;
      log.level = sdk::Log::Level::kInfo;
      log.message = "message " + std::to_string(second);
      ASSERT_TRUE(app.sessionManager()
                      .objectStore()
                      .pushOwned(*topic, second * 1'000'000'000LL, serializeLog(log))
                      .has_value());
    }
    app.catalogModel().rebuildFromDatastore();
    for (const CatalogItem& item : app.catalogModel().items()) {
      if (item.topic_name == u"/rosout"_s) {
        key_ = item.key;
      }
    }
    ASSERT_FALSE(key_.isEmpty());
    tabs_ = window_->findChild<TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    ASSERT_NE(tabs_, nullptr);
    dock_ = tabs_->currentTab()->plotAt(0);
    ASSERT_NE(dock_, nullptr);
  }

  [[nodiscard]] static LogDockWidget* logOf(DockWidget* dock) {
    IDataWidget* widget = dock != nullptr ? dock->objectWidget() : nullptr;
    return widget != nullptr ? qobject_cast<LogDockWidget*>(widget->widget()) : nullptr;
  }

  [[nodiscard]] bool drop() const {
    return QMetaObject::invokeMethod(
        dock_, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key_}));
  }

  QTemporaryDir extensions_dir_;
  std::unique_ptr<MainWindow> window_;
  TabbedPlotWidget* tabs_ = nullptr;
  DockWidget* dock_ = nullptr;
  DatasetId dataset_ = 0;
  QString key_;
};

TEST_F(MainWindowLogPanelTest, TheLogButtonCreatesAnEmptyLogPanel) {
  auto* placeholder = dock_->findChild<VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* button = placeholder->findChild<QToolButton*>(u"buttonVizLog"_s);
  ASSERT_NE(button, nullptr);
  EXPECT_EQ(button->toolTip(), u"Log"_s);
  button->click();
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  EXPECT_EQ(log->view()->store().size(), 0U);

  // The panel then takes a dropped log topic.
  ASSERT_TRUE(drop());
  EXPECT_EQ(logOf(dock_), log);
  EXPECT_EQ(log->view()->store().size(), 2U);
}

TEST_F(MainWindowLogPanelTest, ALogTopicDroppedOnAnEmptyAreaOpensALogPanel) {
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr) << "a log drop on an empty area builds a Log panel";
  ASSERT_EQ(log->view()->store().size(), 2U);
  EXPECT_EQ(log->view()->store().row(0).message, "message 1");
}

TEST_F(MainWindowLogPanelTest, ALogPanelDroppedOnAnEmptyAreaStartsAtThePlayhead) {
  AppSession& app = Peer::session(*window_);
  const DisplayOffset offset = app.sessionManager().displayOffset(dataset_);
  const auto display = [offset](Timestamp raw) { return rawToDisplaySecondsAtOrAfter(raw, offset); };
  app.playbackEngine().setRange(DisplayRange{display(0), display(3'000'000'000LL)});
  app.playbackEngine().setCurrentTime(display(1'500'000'000LL));
  ASSERT_DOUBLE_EQ(toAxisDouble(app.playbackEngine().currentTime()), toAxisDouble(display(1'500'000'000LL)));
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  ASSERT_EQ(log->view()->store().size(), 2U);
  EXPECT_EQ(log->view()->model()->revealedRows(), 1) << "the row at 1 s is at the playhead, the one at 2 s after it";
}

// Removed from the Datasets panel, a shown topic leaves the panel instead of waiting for its return.
TEST_F(MainWindowLogPanelTest, RemovingAShownTopicLeavesNoPendingDemand) {
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  Peer::trash(*window_, key_);
  QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  EXPECT_TRUE(log->displayedTopics().empty());
  EXPECT_FALSE(log->hasPendingTopic(u"/rosout"_s));
  EXPECT_TRUE(log->pendingDemands().empty());
}

TEST_F(MainWindowLogPanelTest, RemovingTheDatasetOfAShownTopicLeavesNoPendingDemand) {
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  Peer::removeDataset(*window_, dataset_);
  QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  EXPECT_TRUE(log->displayedTopics().empty());
  EXPECT_FALSE(log->hasPendingTopic(u"/rosout"_s));
  EXPECT_TRUE(log->pendingDemands().empty());
}

// The other dataset starts earlier, so it is the merge's anchor: the shown /rosout folds into its /rosout, and the
// shown dataset's /robot_log moves under it. The panel follows both topics to the merged dataset.
TEST_F(MainWindowLogPanelTest, MergingTheShownDatasetIntoAnotherKeepsItsLogs) {
  AppSession& app = Peer::session(*window_);
  ObjectStore& objects = app.sessionManager().objectStore();
  const auto log_topic = [&objects](DatasetId dataset, const std::string& name) {
    auto topic = objects.registerTopic(
        ObjectTopicDescriptor{
            .dataset_id = dataset, .topic_name = name, .metadata_json = R"({"builtin_object_type":"kLog"})"});
    EXPECT_TRUE(topic.has_value()) << topic.error();
    return topic.has_value() ? *topic : ObjectTopicId{};
  };
  const auto push = [&objects](ObjectTopicId topic, Timestamp stamp, const std::string& message) {
    sdk::Log log;
    log.message = message;
    ASSERT_TRUE(objects.pushOwned(topic, stamp, serializeLog(log)).has_value());
  };
  const ObjectTopicId robot_log = log_topic(dataset_, "/robot_log");
  push(robot_log, 1'500'000'000LL, "robot 1.5");
  const DatasetId other = pj_test::createDataset(app, "other.mcap");
  push(log_topic(other, "/rosout"), 500'000'000LL, "other 0.5");
  app.catalogModel().rebuildFromDatastore();
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  ASSERT_TRUE(log->tryAcceptObjectTopic(robot_log, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_EQ(log->view()->store().size(), 3U);

  ASSERT_EQ(app.mergeDatasets({dataset_, other}), other);
  QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  std::vector<std::string> messages;
  for (size_t i = 0; i < log->view()->store().size(); ++i) {
    messages.push_back(log->view()->store().row(i).message);
  }
  EXPECT_EQ(messages, (std::vector<std::string>{"other 0.5", "message 1", "robot 1.5", "message 2"}));
  for (const auto& [dataset, topic] : log->displayedTopics()) {
    EXPECT_EQ(dataset, other) << topic.toStdString();
  }
  EXPECT_TRUE(log->unresolvedRestores().isEmpty());
}

TEST_F(MainWindowLogPanelTest, TheTimeColumnFollowsTheDateTimeToggle) {
  ASSERT_TRUE(drop());
  LogDockWidget* log = logOf(dock_);
  ASSERT_NE(log, nullptr);
  // The Time column's text of the first entry, which sits after the playhead (hidden) right after the drop.
  const auto time_text = [log]() { return log->view()->model()->timeText(log->view()->store().row(0)); };
  const double seconds = log->view()->store().row(0).time;
  EXPECT_EQ(time_text(), formatTimeLabel(seconds, TimeLabelFormat::kSeconds, 3, true));

  QSettings settings;
  settings.setValue(MainWindow::kDateTimeDisplayKey, true);
  settings.setValue(MainWindow::kClockUtcKey, true);
  Peer::applyTimeLabelFormat(*window_);
  EXPECT_EQ(time_text(), formatTimeLabel(seconds, TimeLabelFormat::kUtcClock, 3, true));
}

TEST_F(MainWindowLogPanelTest, ALayoutRoundTripRestoresThePanelAndItsTopic) {
  ASSERT_TRUE(drop());
  ASSERT_NE(logOf(dock_), nullptr);
  const QDomDocument saved = Peer::save(*window_);
  EXPECT_FALSE(saved.elementsByTagName(u"log"_s).isEmpty());

  ASSERT_TRUE(Peer::load(*window_, saved));
  LogDockWidget* restored = nullptr;
  tabs_->forEachDocker([&restored](PlotDocker* docker) {
    for (int i = 0; i < docker->plotCount(); ++i) {
      if (LogDockWidget* log = logOf(docker->plotAt(i)); log != nullptr) {
        restored = log;
      }
    }
  });
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->view()->store().size(), 2U);
  EXPECT_FALSE(restored->hasPendingTopic(u"/rosout"_s));
}

}  // namespace
}  // namespace PJ
