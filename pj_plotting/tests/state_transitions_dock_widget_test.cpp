// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QSignalSpy>
#include <memory>
#include <string_view>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;

class StateTransitionsDockWidgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "robot.mcap"});
    ASSERT_TRUE(dataset_or.has_value());
    dataset_id_ = *dataset_or;
    session_.setDatasetSourcePath(dataset_id_, u"/data/robot.mcap"_s);

    auto writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema("state_sample", makePrimitive("state", PrimitiveType::kString));
    ASSERT_TRUE(schema_or.has_value());
    TopicDescriptor descriptor;
    descriptor.name = "/robot/state";
    descriptor.schema_id = *schema_or;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value());
    topic_id_ = *topic_or;
    ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());
    for (int second = 0; second < 4; ++second) {
      ASSERT_TRUE(writer.beginRow(topic_id_, second * kNs).has_value());
      writer.set(topic_id_, 0, std::string_view{second < 2 ? "IDLE" : "RUNNING"});
      ASSERT_TRUE(writer.finishRow(topic_id_).has_value());
    }
    // A second string topic so restore ORDER is observable.
    TopicDescriptor estop_descriptor;
    estop_descriptor.name = "/robot/estop";
    estop_descriptor.schema_id = *schema_or;
    auto estop_or = writer.registerTopic(dataset_id_, estop_descriptor);
    ASSERT_TRUE(estop_or.has_value());
    estop_topic_id_ = *estop_or;
    ASSERT_TRUE(writer.bindTopicWriter(estop_topic_id_).has_value());
    ASSERT_TRUE(writer.beginRow(estop_topic_id_, 0).has_value());
    writer.set(estop_topic_id_, 0, std::string_view{"RELEASED"});
    ASSERT_TRUE(writer.finishRow(estop_topic_id_).has_value());
    // An integer topic: discrete series are not only strings.
    auto mode_schema_or = writer.registerSchema("mode_sample", makePrimitive("mode", PrimitiveType::kInt64));
    ASSERT_TRUE(mode_schema_or.has_value());
    TopicDescriptor mode_descriptor;
    mode_descriptor.name = "/robot/mode";
    mode_descriptor.schema_id = *mode_schema_or;
    auto mode_or = writer.registerTopic(dataset_id_, mode_descriptor);
    ASSERT_TRUE(mode_or.has_value());
    ASSERT_TRUE(writer.bindTopicWriter(*mode_or).has_value());
    ASSERT_TRUE(writer.beginRow(*mode_or, 0).has_value());
    writer.set(*mode_or, 0, static_cast<int64_t>(4));
    ASSERT_TRUE(writer.finishRow(*mode_or).has_value());
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());

    catalog_ = std::make_unique<CatalogModel>(&session_);
    catalog_->rebuildFromDatastore();
    for (const CatalogItem& item : catalog_->items()) {
      if (!catalog_->isDiscreteKey(item.key)) {
        continue;
      }
      if (item.topic_name == u"/robot/state"_s) {
        string_key_ = item.key;
      } else if (item.topic_name == u"/robot/estop"_s) {
        estop_key_ = item.key;
      } else if (item.topic_name == u"/robot/mode"_s) {
        int_key_ = item.key;
      }
    }
    ASSERT_FALSE(string_key_.isEmpty());
    ASSERT_FALSE(estop_key_.isEmpty());
    ASSERT_FALSE(int_key_.isEmpty());
  }

  [[nodiscard]] std::unique_ptr<StateTransitionsDockWidget> makeDock() {
    return std::make_unique<StateTransitionsDockWidget>(&session_, catalog_.get(), &playback_);
  }

  SessionManager session_;
  PlaybackEngine playback_;
  std::unique_ptr<CatalogModel> catalog_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  TopicId estop_topic_id_ = 0;
  QString string_key_;
  QString estop_key_;
  QString int_key_;
};

TEST_F(StateTransitionsDockWidgetTest, AcceptSeriesKeysFiltersAndSignalsWorkspaceChange) {
  auto dock = makeDock();
  QSignalSpy changed(dock.get(), &StateTransitionsDockWidget::workspaceChanged);
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({u"bogus"_s}));
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({u"bogus"_s, string_key_}));
  EXPECT_EQ(dock->controller()->rowCount(), 1);
  EXPECT_EQ(changed.count(), 1);
  // Integer series route through the same hook.
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({int_key_}));
  EXPECT_EQ(dock->controller()->rowCount(), 2);
  EXPECT_EQ(changed.count(), 2);
}

TEST_F(StateTransitionsDockWidgetTest, XmlRoundTripRebindsSeries) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptSeriesKeys({string_key_}));
  ASSERT_TRUE(dock->tryAcceptSeriesKeys({estop_key_}));
  ASSERT_TRUE(dock->tryAcceptSeriesKeys({int_key_}));
  dock->view()->setZoom(4e-8);

  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  ASSERT_FALSE(saved.isNull());
  EXPECT_EQ(saved.tagName(), u"state_transitions"_s);
  const QDomElement series = saved.firstChildElement(u"series"_s);
  ASSERT_FALSE(series.isNull());
  EXPECT_EQ(series.attribute(u"topic"_s), u"/robot/state"_s);
  EXPECT_EQ(series.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  // The saved path is the session's NORMALIZED registration (drive-qualified on
  // Windows), so compare against the same source xmlSaveState reads.
  const QString normalized_path = catalog_->datasetSourcePath(dataset_id_);
  EXPECT_FALSE(normalized_path.isEmpty());
  EXPECT_EQ(series.attribute(u"dataset_path"_s), normalized_path);

  // A fresh dock (fresh session objects untouched) rebinds via resolveCurveKey.
  // No type attribute is saved: the kDiscrete capability re-finds string AND
  // integer rows by (dataset, topic, field) alone.
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(saved));
  EXPECT_EQ(restored->controller()->rowCount(), 3);
  // Document order IS row order — a swapped restore would silently shuffle the
  // strip.
  const auto restored_series = restored->controller()->currentSeries();
  ASSERT_EQ(restored_series.size(), 3U);
  EXPECT_EQ(restored_series[0].topic_name, u"/robot/state"_s);
  EXPECT_EQ(restored_series[1].topic_name, u"/robot/estop"_s);
  EXPECT_EQ(restored_series[2].topic_name, u"/robot/mode"_s);
  EXPECT_DOUBLE_EQ(restored->view()->zoom(), 4e-8);
}

TEST_F(StateTransitionsDockWidgetTest, UnresolvedSeriesSurvivesSaveLoadCycles) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptSeriesKeys({string_key_}));

  QDomDocument doc;
  QDomElement saved = dock->xmlSaveState(doc);
  // Point the saved identity at a dataset that is NOT loaded.
  QDomElement series = saved.firstChildElement(u"series"_s);
  series.setAttribute(u"dataset_id"_s, u"999"_s);
  series.setAttribute(u"dataset_source"_s, u"missing.mcap"_s);
  series.setAttribute(u"dataset_path"_s, u"/data/missing.mcap"_s);
  series.setAttribute(u"topic"_s, u"/other/state"_s);

  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(saved));
  EXPECT_EQ(restored->controller()->rowCount(), 0);  // not rendered...

  // ...but re-serialized verbatim, so the round-trip loses nothing.
  QDomDocument doc2;
  const QDomElement resaved = restored->xmlSaveState(doc2);
  const QDomElement kept = resaved.firstChildElement(u"series"_s);
  ASSERT_FALSE(kept.isNull());
  EXPECT_EQ(kept.attribute(u"dataset_source"_s), u"missing.mcap"_s);
  EXPECT_EQ(kept.attribute(u"topic"_s), u"/other/state"_s);
}

TEST_F(StateTransitionsDockWidgetTest, PendingSeriesResolvesWhenDatasetLoadsLater) {
  // Restore a layout whose dataset is NOT loaded: the row stays pending...
  auto dock = makeDock();
  ASSERT_TRUE(dock->tryAcceptSeriesKeys({string_key_}));
  QDomDocument doc;
  QDomElement saved = dock->xmlSaveState(doc);
  QDomElement series = saved.firstChildElement(u"series"_s);
  series.setAttribute(u"dataset_id"_s, u"999"_s);
  series.setAttribute(u"dataset_source"_s, u"late.mcap"_s);
  series.setAttribute(u"dataset_path"_s, u"/data/late.mcap"_s);
  series.setAttribute(u"topic"_s, u"/late/state"_s);

  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(saved));
  ASSERT_EQ(restored->controller()->rowCount(), 0);

  // ...until the dataset arrives: the catalog rebuild (itemsAdded) must bind
  // the pending row without a layout re-load — the strip's PendingDisplayBinder
  // analog. The saved id (999) is dead, so resolution goes through the path leg.
  auto late_or = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "late.mcap"});
  ASSERT_TRUE(late_or.has_value());
  session_.setDatasetSourcePath(*late_or, u"/data/late.mcap"_s);
  auto writer = session_.dataEngine().createWriter();
  auto schema_or = writer.registerSchema("late_state", makePrimitive("state", PrimitiveType::kString));
  ASSERT_TRUE(schema_or.has_value());
  TopicDescriptor descriptor;
  descriptor.name = "/late/state";
  descriptor.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(*late_or, descriptor);
  ASSERT_TRUE(topic_or.has_value());
  ASSERT_TRUE(writer.bindTopicWriter(*topic_or).has_value());
  ASSERT_TRUE(writer.beginRow(*topic_or, 0).has_value());
  writer.set(*topic_or, 0, std::string_view{"LATE"});
  ASSERT_TRUE(writer.finishRow(*topic_or).has_value());
  EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  catalog_->rebuildFromDatastore();

  EXPECT_EQ(restored->controller()->rowCount(), 1);
  const auto restored_series = restored->controller()->currentSeries();
  ASSERT_EQ(restored_series.size(), 1U);
  EXPECT_EQ(restored_series[0].topic_name, u"/late/state"_s);
  // The pending entry drained: a resave carries the LIVE identity, not two.
  QDomDocument doc2;
  const QDomElement resaved = restored->xmlSaveState(doc2);
  const QDomElement kept = resaved.firstChildElement(u"series"_s);
  ASSERT_FALSE(kept.isNull());
  EXPECT_EQ(kept.attribute(u"dataset_source"_s), u"late.mcap"_s);
  EXPECT_TRUE(kept.nextSiblingElement(u"series"_s).isNull());
}

TEST_F(StateTransitionsDockWidgetTest, TrackerTimeDrivesPlayhead) {
  auto dock = makeDock();
  dock->onTrackerTime(2.5);
  EXPECT_EQ(dock->view()->playheadNsForTest(), 2'500'000'000LL);
}

}  // namespace
}  // namespace PJ
