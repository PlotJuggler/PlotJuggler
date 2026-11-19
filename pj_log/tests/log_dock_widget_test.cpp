// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QSignalSpy>
#include <QTest>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/log_codec.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_log/LogDockWidget.h"
#include "pj_log/LogView.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;
constexpr auto kLogMetadata = R"({"builtin_object_type":"kLog"})";

class LogDockWidgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto domain = session_.dataEngine().createTimeDomain("robot.mcap");
    ASSERT_TRUE(domain.has_value());
    auto dataset_or =
        session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "robot.mcap", .time_domain_id = *domain});
    ASSERT_TRUE(dataset_or.has_value());
    dataset_id_ = *dataset_or;
    session_.setDatasetSourcePath(dataset_id_, u"/data/robot.mcap"_s);
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
    auto topic = session_.objectStore().registerTopic(
        ObjectTopicDescriptor{.dataset_id = dataset_id_, .topic_name = name, .metadata_json = metadata});
    EXPECT_TRUE(topic.has_value());
    return topic.has_value() ? *topic : ObjectTopicId{};
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

  [[nodiscard]] static std::vector<std::string> messages(const LogDockWidget& dock) {
    std::vector<std::string> out;
    const auto& store = dock.view()->store();
    for (size_t i = 0; i < store.size(); ++i) {
      out.push_back(store.row(i).message);
    }
    return out;
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

TEST_F(LogDockWidgetTest, AppendsNewEntriesAndReloadsOnALateOne) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptObjectTopic(rosout_, sdk::BuiltinObjectType::kLog, {}));
  push(rosout_, 4, "rosout 4");
  landed();
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 1", "rosout 2", "rosout 3", "rosout 4"}));
  // An entry older than the newest rewrites the topic: it lands in time order.
  push(rosout_, 0, "rosout 0");
  landed();
  EXPECT_EQ(messages(*dock), (std::vector<std::string>{"rosout 0", "rosout 1", "rosout 2", "rosout 3", "rosout 4"}));
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
  state.show_future = false;
  dock->view()->applyState(state);

  QDomDocument doc;
  QDomElement saved = dock->xmlSaveState(doc);
  EXPECT_EQ(saved.tagName(), u"log"_s);
  const QDomElement topic = saved.firstChildElement(u"topic"_s);
  EXPECT_EQ(topic.attribute(u"topic"_s), u"/rosout"_s);
  EXPECT_EQ(topic.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  EXPECT_EQ(topic.attribute(u"dataset_path"_s), catalog_->datasetSourcePath(dataset_id_));
  // A topic whose dataset is not loaded.
  QDomElement missing = doc.createElement(u"topic"_s);
  missing.setAttribute(u"dataset_id"_s, u"999"_s);
  missing.setAttribute(u"dataset_source"_s, u"other.mcap"_s);
  missing.setAttribute(u"dataset_path"_s, u"/data/other.mcap"_s);
  missing.setAttribute(u"topic"_s, u"/rosout"_s);
  saved.appendChild(missing);

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

}  // namespace
}  // namespace PJ
