// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QMenu>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/log_codec.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_log/LogDockWidget.h"
#include "pj_log/LogTableModel.h"
#include "pj_log/LogView.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/XmlStateOptions.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;
constexpr auto kLogMetadata = R"({"builtin_object_type":"kLog"})";

class LogDockWidgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dataset_id_ = addDataset("robot.mcap");
    ASSERT_NE(dataset_id_, 0U);
    rosout_ = registerTopic("/rosout", kLogMetadata);
    diagnostics_ = registerTopic("/diagnostics_log", kLogMetadata);
    image_ = registerTopic("/camera", R"({"builtin_object_type":"kImage"})");
    for (int second = 1; second <= 3; ++second) {
      push(rosout_, second, "rosout " + std::to_string(second));
    }
    push(diagnostics_, 2, "diagnostics");
    catalog_ = std::make_unique<CatalogModel>(&session_);
    catalog_->rebuildFromDatastore();
  }

  ObjectTopicId registerTopic(const std::string& name, const char* metadata) {
    return registerTopicIn(dataset_id_, name, metadata);
  }

  ObjectTopicId registerTopicIn(DatasetId dataset, const std::string& name, const char* metadata) {
    auto topic = session_.objectStore().registerTopic(
        ObjectTopicDescriptor{.dataset_id = dataset, .topic_name = name, .metadata_json = metadata});
    EXPECT_TRUE(topic.has_value());
    return topic.has_value() ? *topic : ObjectTopicId{};
  }

  /// A second file, "/data/<name>", in its own time domain.
  DatasetId addDataset(const std::string& name) {
    auto domain = session_.dataEngine().createTimeDomain(name);
    if (!domain.has_value()) {
      ADD_FAILURE() << "time domain";
      return 0;
    }
    auto dataset =
        session_.dataEngine().createDataset(DatasetDescriptor{.source_name = name, .time_domain_id = *domain});
    if (!dataset.has_value()) {
      ADD_FAILURE() << "dataset";
      return 0;
    }
    session_.setDatasetSourcePath(*dataset, QString::fromStdString("/data/" + name));
    return *dataset;
  }

  void push(ObjectTopicId topic, int second, const std::string& message) {
    sdk::Log log;
    log.timestamp_ns = second * kNs + 7;
    log.level = sdk::Log::Level::kWarning;
    log.source = "/node";
    log.message = message;
    ASSERT_TRUE(session_.objectStore().pushOwned(topic, second * kNs, serializeLog(log)).has_value());
  }

  [[nodiscard]] std::unique_ptr<LogDockWidget> makeDock() {
    return std::make_unique<LogDockWidget>(&session_, catalog_.get(), &playback_);
  }

  [[nodiscard]] static int savedTopics(const LogDockWidget& dock) {
    QDomDocument doc;
    const QDomElement saved = dock.xmlSaveState(doc);
    int count = 0;
    for (QDomElement child = saved.firstChildElement(u"topic"_s); !child.isNull();
         child = child.nextSiblingElement(u"topic"_s)) {
      ++count;
    }
    return count;
  }

  [[nodiscard]] static std::vector<std::string> messages(const LogDockWidget& dock) {
    std::vector<std::string> out;
    const auto& store = dock.view()->store();
    for (size_t i = 0; i < store.size(); ++i) {
      out.push_back(store.row(i).message);
    }
    return out;
  }

  /// A layout `<topic>` of @p file, loaded from "/data/<file>" as dataset @p dataset.
  [[nodiscard]] static QDomElement topicElement(
      QDomDocument& doc, DatasetId dataset, const QString& file, const QString& name) {
    QDomElement topic = doc.createElement(u"topic"_s);
    topic.setAttribute(u"dataset_id"_s, QString::number(dataset));
    topic.setAttribute(u"dataset_source"_s, file);
    topic.setAttribute(u"dataset_path"_s, u"/data/"_s + file);
    topic.setAttribute(u"topic"_s, name);
    return topic;
  }

  /// Folds @p source into @p anchor as AppSession does (the store, then the catalog), then waits out the panel's
  /// rate-limit window.
  void mergeInto(DatasetId anchor, DatasetId source) {
    ASSERT_TRUE(session_.mergeDatasets(anchor, {DatasetMergeSource{.dataset_id = source}}).has_value());
    catalog_->removeDataset(source);
    catalog_->rebuildFromDatastore();
    QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  }

  /// Ingest landed: the panel follows at once, or at the end of its rate-limit window.
  void landed() {
    emit session_.dataPublished();
    QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  }

  SessionManager session_;
  PlaybackEngine playback_;
  std::unique_ptr<CatalogModel> catalog_;
  DatasetId dataset_id_ = 0;
  ObjectTopicId rosout_;
  ObjectTopicId diagnostics_;
  ObjectTopicId image_;
};

TEST_F(LogDockWidgetTest, AcceptsEachLogTopicOnceAndMergesThem) {
  auto dock = makeDock();
  QSignalSpy changed(dock.get(), &LogDockWidget::workspaceChanged);
  EXPECT_FALSE(dock->canAcceptObjectTopic(image_, sdk::BuiltinObjectType::kImage));
  EXPECT_FALSE(dock->tryAcceptObjectTopic(image_, sdk::BuiltinObjectType::kLog, {}));  // the store says it is not
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  EXPECT_FALSE(dock->canAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog));
  ASSERT_TRUE(dock->tryAcceptObjectTopic(diagnostics_, sdk::BuiltinObjectType::kLog, {}));
  EXPECT_EQ(changed.count(), 2);
  // Equal times order by topic: /rosout registered first.
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "diagnostics", "rosout 3"}));
  const auto& row = dock->view()->store().row(0);
  EXPECT_EQ(row.level, LogLevel::kWarning);
  EXPECT_EQ(dock->view()->store().text(row.source), "/node");
  EXPECT_EQ(dock->displayedTopics().size(), 2u);
}

TEST_F(LogDockWidgetTest, AppendsNewAndLateEntriesInTimeOrder) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  push(rosout_, 4, "rosout 4");
  landed();
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "rosout 3", "rosout 4"}));
  // Entries older than the newest land in time order; the row the store evicted leaves the panel with it.
  session_.objectStore().evictBefore(rosout_, 2 * kNs);
  push(rosout_, 0, "rosout 0");
  push(rosout_, 3, "rosout 3 again");
  landed();
  EXPECT_EQ(
      messages(*dock), (std::vector<std::string>{"rosout 0", "rosout 2", "rosout 3", "rosout 3 again", "rosout 4"}));
}

std::vector<QString> modelMessages(const LogDockWidget& dock) {
  const auto* model = dock.view()->model();
  std::vector<QString> out;
  for (int row = 0; row < model->rowCount(); ++row) {
    out.push_back(model->data(model->index(row, log_view::LogTableModel::kMessage)).toString());
  }
  return out;
}

TEST_F(LogDockWidgetTest, ALateEntryLeavesEveryIndexedRowReadable) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  push(rosout_, 4, "rosout 4");
  landed();
  ASSERT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "rosout 3", "rosout 4"}));
  push(rosout_, 2, "late");  // lands after "rosout 2" and moves the rows after it in the store
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "rosout 3", "rosout 4"}));
  landed();
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "late", "rosout 3", "rosout 4"}));
}

TEST_F(LogDockWidgetTest, ALateEntryEvictedByTheSamePushKeepsRowsWithTheirTimes) {
  const ObjectTopicId live = registerTopic("/live", kLogMetadata);
  session_.objectStore().setRetentionBudget(live, RetentionBudget{.max_entries = 4});
  for (int second = 1; second <= 4; ++second) {
    push(live, second, "m" + std::to_string(second));
  }
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(live, sdk::BuiltinObjectType::kLog, {}));
  push(live, 0, "too late");  // older than every row: inserted first, evicted at once
  push(live, 5, "m5");
  landed();
  ASSERT_EQ(messages(*dock), (std::vector<std::string>{"m2", "m3", "m4", "m5"}));
  const auto& store = dock->view()->store();
  for (size_t i = 1; i < store.size(); ++i) {
    EXPECT_NEAR(store.row(i).time - store.row(0).time, static_cast<double>(i), 1e-6) << i;  // one second apart
  }
}

TEST_F(LogDockWidgetTest, ManyLateEntriesBetweenUpdatesAllLand) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_TRUE(dock->tryAcceptObjectTopic(diagnostics_, sdk::BuiltinObjectType::kLog, {}));
  push(rosout_, 100, "newest");
  for (int i = 0; i < 70; ++i) {
    sdk::Log log;
    log.message = "late " + std::to_string(i);
    ASSERT_TRUE(session_.objectStore().pushOwned(rosout_, 1 * kNs + 1000 * (i + 1), serializeLog(log)).has_value());
  }
  push(diagnostics_, 3, "diagnostics 3");
  landed();
  std::vector<std::string> expected{"rosout 1"};
  for (int i = 0; i < 70; ++i) {
    expected.push_back("late " + std::to_string(i));
  }
  expected.insert(expected.end(), {"rosout 2", "diagnostics", "rosout 3", "diagnostics 3", "newest"});
  EXPECT_EQ(messages(*dock), expected);
  EXPECT_EQ(dock->view()->model()->rowCount(), static_cast<int>(expected.size()));
}

TEST_F(LogDockWidgetTest, ShowsFirstEntryOfALogLargerThanTheOldCapacity) {
  constexpr int kEntries = 1'000'010;
  const ObjectTopicId big = registerTopic("/big", kLogMetadata);
  sdk::Log log;
  log.level = sdk::Log::Level::kInfo;
  for (int i = 0; i < kEntries; ++i) {
    log.message = "entry " + std::to_string(i);
    ASSERT_TRUE(session_.objectStore().pushOwned(big, Timestamp{i} * 1000, serializeLog(log)).has_value());
  }
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(big, sdk::BuiltinObjectType::kLog, {}));
  const auto* model = dock->view()->model();
  ASSERT_EQ(model->rowCount(), kEntries);
  EXPECT_EQ(model->data(model->index(0, log_view::LogTableModel::kMessage)).toString(), u"entry 0"_s);
  EXPECT_EQ(
      model->data(model->index(kEntries - 1, log_view::LogTableModel::kMessage)).toString(),
      QString::fromStdString("entry " + std::to_string(kEntries - 1)));
}

TEST_F(LogDockWidgetTest, EvictionUnderTheViewDropsRowsWithoutStaleText) {
  const ObjectTopicId live = registerTopic("/live", kLogMetadata);
  session_.objectStore().setRetentionBudget(live, RetentionBudget{.max_entries = 100});
  const auto push_many = [&](int first, int count) {
    sdk::Log log;
    for (int i = first; i < first + count; ++i) {
      log.message = "m" + std::to_string(i);
      ASSERT_TRUE(session_.objectStore().pushOwned(live, Timestamp{i + 10} * kNs, serializeLog(log)).has_value());
    }
  };
  push_many(0, 100);
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(live, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_EQ(modelMessages(*dock).size(), 100u);
  EXPECT_EQ(modelMessages(*dock).front(), u"m0"_s);

  push_many(100, 50);  // evicts m0..m49 before the panel hears of it
  const auto evicted = [](const QString& text) {
    return text.startsWith(u'm') && text.mid(1).toInt() < 50 && text != u"m"_s;
  };
  for (const QString& text : modelMessages(*dock)) {
    EXPECT_FALSE(evicted(text)) << text.toStdString();
  }
  landed();
  const std::vector<QString> after = modelMessages(*dock);
  ASSERT_EQ(after.size(), 100u);
  EXPECT_EQ(after.front(), u"m50"_s);
  EXPECT_EQ(after.back(), u"m149"_s);
}

TEST_F(LogDockWidgetTest, ReloadReplacesEveryRow) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_EQ(messages(*dock).size(), 3u);
  ObjectStore staged;
  constexpr DatasetId kStaged = 77;
  auto reloaded = staged.registerTopic(
      ObjectTopicDescriptor{.dataset_id = kStaged, .topic_name = "/rosout", .metadata_json = kLogMetadata});
  ASSERT_TRUE(reloaded.has_value());
  sdk::Log log;
  for (const char* text : {"new a", "new b"}) {
    log.message = text;
    ASSERT_TRUE(staged.pushOwned(*reloaded, (log.message == "new a" ? 5 : 6) * kNs, serializeLog(log)).has_value());
  }
  ASSERT_TRUE(session_.objectStore().replaceDatasetFrom(staged, kStaged, dataset_id_).has_value());
  landed();
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"new a", "new b"}));
  EXPECT_EQ(modelMessages(*dock), (std::vector<QString>{u"new a"_s, u"new b"_s}));
}

TEST_F(LogDockWidgetTest, ADisplayOffsetChangeMovesTheRows) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  const auto expected_time = [this]() {
    return toAxisDouble(rawToDisplaySecondsAtOrAfter(1 * kNs, session_.displayOffset(dataset_id_)));
  };
  EXPECT_DOUBLE_EQ(dock->view()->store().row(0).time, expected_time());
  const double before = dock->view()->store().row(0).time;
  session_.setDisplayOffset(dataset_id_, DisplayOffset{Duration{kNs / 2}});
  QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  EXPECT_DOUBLE_EQ(dock->view()->store().row(0).time, expected_time());
  EXPECT_NEAR(dock->view()->store().row(0).time, before - 0.5, 1e-9);
}

TEST_F(LogDockWidgetTest, TheTimeColumnFollowsTheAppFormat) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  auto* model = dock->view()->model();
  const auto time_text = [model]() { return model->data(model->index(0, log_view::LogTableModel::kTime)).toString(); };
  const double seconds = dock->view()->store().row(0).time;
  EXPECT_EQ(time_text(), formatTimeLabel(seconds, TimeLabelFormat::kSeconds, 3, true));
  dock->setTimeLabelFormat(TimeLabelFormat::kUtcClock);
  EXPECT_EQ(time_text(), formatTimeLabel(seconds, TimeLabelFormat::kUtcClock, 3, true));
}

TEST_F(LogDockWidgetTest, XmlRoundTripKeepsTopicsFiltersAndUnresolvedTopics) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  LogViewState state = dock->view()->state();
  state.levels = {false, true, true, true, true};
  state.text = u"motor"_s;
  state.show_future = true;
  dock->view()->applyState(state);

  QDomDocument doc;
  QDomElement saved = dock->xmlSaveState(doc);
  EXPECT_EQ(saved.tagName(), u"log"_s);
  const QDomElement topic = saved.firstChildElement(u"topic"_s);
  EXPECT_EQ(topic.attribute(u"topic"_s), u"/rosout"_s);
  EXPECT_EQ(topic.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  EXPECT_EQ(topic.attribute(u"dataset_path"_s), catalog_->datasetSourcePath(dataset_id_));
  // A topic whose dataset is not loaded.
  saved.appendChild(topicElement(doc, 999, u"other.mcap"_s, u"/rosout"_s));

  auto restored = makeDock();
  QSignalSpy changed(restored.get(), &LogDockWidget::workspaceChanged);
  ASSERT_TRUE(restored->xmlLoadState(saved));
  restored->finishRestore();
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(restored->view()->state(), state);
  EXPECT_EQ(messages(*restored).size(), 3u);
  EXPECT_TRUE(restored->hasPendingTopic(u"/rosout"_s));

  QDomDocument again;
  const QDomElement resaved = restored->xmlSaveState(again);
  int topics = 0;
  bool kept_missing = false;
  for (QDomElement child = resaved.firstChildElement(u"topic"_s); !child.isNull();
       child = child.nextSiblingElement(u"topic"_s)) {
    ++topics;
    kept_missing = kept_missing || child.attribute(u"dataset_path"_s) == u"/data/other.mcap"_s;
  }
  EXPECT_EQ(topics, 2);
  EXPECT_TRUE(kept_missing);
}

TEST_F(LogDockWidgetTest, FutureRowsAreHiddenUnlessTheLayoutShowsThem) {
  auto dock = makeDock();
  EXPECT_FALSE(dock->view()->state().show_future);
  LogViewState shown = dock->view()->state();
  shown.show_future = true;
  dock->view()->applyState(shown);
  QDomDocument doc;
  ASSERT_TRUE(dock->xmlLoadState(doc.createElement(u"log"_s)));  // a layout without the choice
  EXPECT_FALSE(dock->view()->state().show_future);
}

TEST_F(LogDockWidgetTest, APendingTopicBindsWhenItAppears) {
  auto dock = makeDock();
  dock->addPendingTopic(dataset_id_, u"/late"_s);
  EXPECT_TRUE(dock->hasPendingTopic(u"/late"_s));
  const ObjectTopicId late = registerTopic("/late", kLogMetadata);
  push(late, 1, "late one");
  catalog_->rebuildFromDatastore();
  QApplication::processEvents();
  EXPECT_FALSE(dock->hasPendingTopic(u"/late"_s));
  EXPECT_EQ(messages(*dock), std::vector<std::string>{"late one"});
}

TEST_F(LogDockWidgetTest, APendingTopicThatIsAlreadyShownIsResolved) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  dock->addPendingTopic(dataset_id_, u"/rosout"_s);
  EXPECT_FALSE(dock->hasPendingTopic(u"/rosout"_s));
  EXPECT_EQ(savedTopics(*dock), 1);

  dock->addPendingTopic(dataset_id_, u"/late"_s);
  dock->addPendingTopic(dataset_id_, u"/late"_s);
  EXPECT_EQ(dock->pendingDemands().size(), 1u);
  const ObjectTopicId late = registerTopic("/late", kLogMetadata);
  push(late, 1, "late one");
  catalog_->rebuildFromDatastore();
  QApplication::processEvents();
  EXPECT_FALSE(dock->hasPendingTopic(u"/late"_s));
  EXPECT_TRUE(dock->pendingDemands().empty());
  EXPECT_EQ(savedTopics(*dock), 2);
}

TEST_F(LogDockWidgetTest, AVanishedTopicKeepsItsDatasetIdentityAndRebinds) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  session_.evictDatasetObjects(dataset_id_);
  session_.removeDataset(dataset_id_);
  catalog_->rebuildFromDatastore();
  QTest::qWait(LogDockWidget::kEntriesLandedIntervalMs + 50);
  ASSERT_TRUE(dock->hasPendingTopic(u"/rosout"_s));
  QDomDocument doc;
  const QDomElement topic = dock->xmlSaveState(doc).firstChildElement(u"topic"_s);
  EXPECT_EQ(topic.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  // Normalized like every stored source path: Windows prefixes the current drive.
  EXPECT_EQ(topic.attribute(u"dataset_path"_s), SessionManager::normalizedSourcePath(u"/data/robot.mcap"_s));

  // The same file loaded again: a new dataset id, the same source and path.
  auto domain = session_.dataEngine().createTimeDomain("robot.mcap");
  ASSERT_TRUE(domain.has_value());
  auto dataset =
      session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "robot.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());
  dataset_id_ = *dataset;
  session_.setDatasetSourcePath(dataset_id_, u"/data/robot.mcap"_s);
  push(registerTopic("/rosout", kLogMetadata), 5, "reloaded");
  catalog_->rebuildFromDatastore();
  QApplication::processEvents();
  EXPECT_FALSE(dock->hasPendingTopic(u"/rosout"_s));
  EXPECT_EQ(messages(*dock), std::vector<std::string>{"reloaded"});
}

TEST_F(LogDockWidgetTest, UnresolvedRestoresListThePendingTopicsOnly) {
  auto dock = makeDock();
  QDomDocument doc;
  QDomElement state = doc.createElement(u"log"_s);
  for (const QString& name : {u"/rosout"_s, u"/missing"_s}) {
    QDomElement topic = doc.createElement(u"topic"_s);
    topic.setAttribute(u"topic"_s, name);
    state.appendChild(topic);
  }
  ASSERT_TRUE(dock->xmlLoadState(state));
  EXPECT_EQ(dock->unresolvedRestores(), QStringList{u"/missing"_s});
}

// Two datasets' /rosout in one panel: the Topics menu tells them apart by dataset, and hiding one hides its rows only,
// also after a layout round trip.
TEST_F(LogDockWidgetTest, SameNameTopicsOfTwoDatasetsAreToldApartAndHideApart) {
  const DatasetId base = addDataset("base.mcap");
  const ObjectTopicId base_rosout = registerTopicIn(base, "/rosout", kLogMetadata);
  push(base_rosout, 2, "base 2");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_TRUE(dock->tryAcceptObjectTopic(base_rosout, sdk::BuiltinObjectType::kLog, {}));

  QMenu* menu = dock->view()->findChild<QToolButton*>(u"topicsButton"_s)->menu();
  ASSERT_EQ(menu->actions().size(), 2);
  // Listed by topic id: robot.mcap's first.
  EXPECT_EQ(menu->actions()[0]->text(), u"robot.mcap:/rosout"_s);
  EXPECT_EQ(menu->actions()[1]->text(), u"base.mcap:/rosout"_s);
  // A dataset renamed (a replacing reload's commit) relabels its topic.
  catalog_->setDatasetDisplayName(base, u"base (2).mcap"_s);
  EXPECT_EQ(menu->actions()[1]->text(), u"base (2).mcap:/rosout"_s);

  menu->actions()[1]->setChecked(false);
  const std::vector<QString> robot_rows{u"rosout 1"_s, u"rosout 2"_s, u"rosout 3"_s};
  EXPECT_EQ(modelMessages(*dock), robot_rows);

  QDomDocument doc;
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(dock->xmlSaveState(doc)));
  EXPECT_EQ(modelMessages(*restored), robot_rows);
}

TEST_F(LogDockWidgetTest, ValidationDescribesTheSavedStateAndAgreesWithLoad) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  ASSERT_TRUE(dock->tryAcceptObjectTopic(diagnostics_, sdk::BuiltinObjectType::kLog, {}));
  LogViewState state = dock->view()->state();
  state.levels = {false, true, true, true, true};
  state.source = u"/node"_s;
  state.category = u"motors"_s;
  state.text = u"stall"_s;
  state.show_future = true;
  dock->view()->applyState(state);
  dock->view()->setTopicHidden(diagnostics_.id, true);

  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  ASSERT_FALSE(saved.firstChildElement(u"topic"_s).isNull());
  ASSERT_EQ(saved.firstChildElement(u"topic"_s).nextSiblingElement(u"topic"_s).attribute(u"hidden"_s), u"true"_s);
  EXPECT_EQ(
      xmlSelfDescriptionProblems(
          [&](const QDomElement& e) { return dock->xmlValidateState(e); }, saved,
          {.check_bad_values = true,
           .tolerates_bad = {u"log.levels"_s, u"log.future_rows"_s, u"topic.dataset_id"_s, u"topic.hidden"_s},
           .load = [&](const QDomElement& e) { return dock->xmlLoadState(e); }}),
      QStringList());

  XmlOptionsRecorder recorder;
  EXPECT_FALSE(dock->xmlValidateState(doc.createElement(u"plot"_s)));
  EXPECT_EQ(recorder.firstRejection(), u"<plot>: expected a <log> element"_s);
}

TEST_F(LogDockWidgetTest, ValidationListsTheTopicOptionsOfAStateWithoutTopics) {
  auto dock = makeDock();
  QDomDocument doc;
  XmlOptionsRecorder recorder;
  ASSERT_TRUE(dock->xmlValidateState(dock->xmlSaveState(doc)));
  QStringList reported;
  for (const XmlOption& option : recorder.options()) {
    reported.append(option.element + u'.' + option.attribute);
  }
  EXPECT_TRUE(reported.contains(u"topic.dataset_path"_s));
  EXPECT_TRUE(reported.contains(u"topic.hidden"_s));
}

// A layout saved while hidden topics were kept by name only: every topic of that name is hidden.
TEST_F(LogDockWidgetTest, AHiddenTopicSavedByNameHidesEveryTopicOfThatName) {
  const DatasetId base = addDataset("base.mcap");
  push(registerTopicIn(base, "/rosout", kLogMetadata), 2, "base 2");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  QDomDocument doc;
  QDomElement state = doc.createElement(u"log"_s);
  state.appendChild(topicElement(doc, dataset_id_, u"robot.mcap"_s, u"/rosout"_s));
  state.appendChild(topicElement(doc, dataset_id_, u"robot.mcap"_s, u"/diagnostics_log"_s));
  state.appendChild(topicElement(doc, base, u"base.mcap"_s, u"/rosout"_s));
  QDomElement hidden = doc.createElement(u"hidden_topic"_s);
  hidden.setAttribute(u"topic"_s, u"/rosout"_s);
  state.appendChild(hidden);
  ASSERT_TRUE(dock->xmlLoadState(state));
  ASSERT_EQ(dock->displayedTopics().size(), 3U);
  EXPECT_EQ(modelMessages(*dock), std::vector<QString>{u"diagnostics"_s});
}

// A layout's dataset id is its saved session's: deleting this session's dataset of that id leaves a restored topic of
// another file waiting, and drops only the topics of the deleted dataset.
TEST_F(LogDockWidgetTest, DeletingADatasetKeepsARestoredTopicOfAnotherFile) {
  const DatasetId base = addDataset("base.mcap");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  QDomDocument doc;
  QDomElement state = doc.createElement(u"log"_s);
  state.appendChild(topicElement(doc, base, u"other.mcap"_s, u"/elsewhere"_s));
  ASSERT_TRUE(dock->xmlLoadState(state));
  dock->addPendingTopic(base, u"/later"_s);
  ASSERT_EQ(savedTopics(*dock), 2);

  dock->discardDataset(base);
  EXPECT_EQ(dock->unresolvedRestores(), QStringList{u"/elsewhere"_s});
}

// A pending topic of a dataset a merge consumed waits for the merged dataset: deleting that one drops it too.
TEST_F(LogDockWidgetTest, DeletingAMergedDatasetDropsItsSourcesPendingTopics) {
  const DatasetId base = addDataset("base.mcap");
  push(registerTopicIn(base, "/base_log", kLogMetadata), 2, "base 2");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  dock->addPendingTopic(base, u"/later"_s);
  mergeInto(dataset_id_, base);
  ASSERT_EQ(dock->unresolvedRestores(), QStringList{u"/later"_s});

  dock->discardDataset(dataset_id_);
  EXPECT_TRUE(dock->unresolvedRestores().isEmpty());
}

// A merge reparents a topic only the consumed dataset had: the panel keeps showing it, under the anchor, with the
// user's pick, and says so, so topic demand and the curve tree follow. A pending topic of the consumed dataset waits
// for the anchor now.
TEST_F(LogDockWidgetTest, AMergeMovesItsTopicsAndSaysSo) {
  const DatasetId base = addDataset("base.mcap");
  const ObjectTopicId base_log = registerTopicIn(base, "/base_log", kLogMetadata);
  push(base_log, 2, "base 2");
  push(base_log, 3, "base 3");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(base_log, sdk::BuiltinObjectType::kLog, {}));
  dock->addPendingTopic(base, u"/later"_s);
  auto* list = dock->view()->findChild<QTreeView*>(u"logView"_s);
  ASSERT_NE(list, nullptr);
  list->setCurrentIndex(list->model()->index(1, 0));
  QSignalSpy changed(dock.get(), &LogDockWidget::topicsChanged);

  mergeInto(dataset_id_, base);

  EXPECT_GE(changed.count(), 1);
  EXPECT_EQ(dock->displayedTopics(), (std::vector<std::pair<DatasetId, QString>>{{dataset_id_, u"/base_log"_s}}));
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"base 2", "base 3"}));
  const QModelIndexList selected = list->selectionModel()->selectedRows();
  ASSERT_EQ(selected.size(), 1);
  EXPECT_EQ(selected.front().row(), 1);
  EXPECT_EQ(dock->pendingDemands(), (std::vector<LogDockWidget::PendingDemand>{{u"/later"_s, dataset_id_}}));
}

// A topic restored from a layout names its file by the saved session's dataset id, which another file may hold in
// this session: a merge of that dataset leaves the restored topic waiting for its own file.
TEST_F(LogDockWidgetTest, AMergeLeavesARestoredTopicOfAnotherFileWaiting) {
  const DatasetId base = addDataset("base.mcap");
  push(registerTopicIn(base, "/base_log", kLogMetadata), 2, "base 2");
  catalog_->rebuildFromDatastore();
  auto dock = makeDock();
  QDomDocument doc;
  QDomElement state = doc.createElement(u"log"_s);
  state.appendChild(topicElement(doc, base, u"other.mcap"_s, u"/elsewhere"_s));
  ASSERT_TRUE(dock->xmlLoadState(state));
  ASSERT_EQ(dock->unresolvedRestores(), QStringList{u"/elsewhere"_s});

  mergeInto(dataset_id_, base);

  EXPECT_EQ(dock->unresolvedRestores(), QStringList{u"/elsewhere"_s});
  EXPECT_EQ(dock->pendingDemands(), (std::vector<LogDockWidget::PendingDemand>{{u"/elsewhere"_s, std::nullopt}}));
  QDomDocument out;
  const QDomElement saved = dock->xmlSaveState(out).firstChildElement(u"topic"_s);
  EXPECT_EQ(saved.attribute(u"dataset_id"_s), QString::number(base));
  EXPECT_EQ(saved.attribute(u"dataset_source"_s), u"other.mcap"_s);
}

}  // namespace
}  // namespace PJ
