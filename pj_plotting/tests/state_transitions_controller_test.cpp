// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QTest>
#include <memory>
#include <string_view>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/StateTransitionsController.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;

struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    static int argc = 0;
    app_ = new QApplication(argc, nullptr);
  }
  void TearDown() override {
    delete app_;
    app_ = nullptr;
  }
  QApplication* app_ = nullptr;
};

const auto* const kEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

// Session with one string topic + one numeric topic, a populated catalog, and
// the controller bound to a real view.
class StateTransitionsControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A real time domain so setDisplayOffset(dataset) has a shift to write.
    auto domain_or = session_.dataEngine().createTimeDomain("test");
    ASSERT_TRUE(domain_or.has_value());
    auto dataset_or = session_.dataEngine().createDataset(
        DatasetDescriptor{.source_name = "test.mcap", .time_domain_id = *domain_or});
    ASSERT_TRUE(dataset_or.has_value());
    dataset_id_ = *dataset_or;

    auto writer = session_.dataEngine().createWriter();
    auto string_schema = writer.registerSchema("state_sample", makePrimitive("state", PrimitiveType::kString));
    ASSERT_TRUE(string_schema.has_value());
    TopicDescriptor string_topic;
    string_topic.name = "/robot/state";
    string_topic.schema_id = *string_schema;
    auto string_topic_or = writer.registerTopic(dataset_id_, string_topic);
    ASSERT_TRUE(string_topic_or.has_value());
    string_topic_id_ = *string_topic_or;
    ASSERT_TRUE(writer.bindTopicWriter(string_topic_id_).has_value());

    auto numeric_schema = writer.registerSchema("num_sample", makePrimitive("value", PrimitiveType::kFloat64));
    ASSERT_TRUE(numeric_schema.has_value());
    TopicDescriptor numeric_topic;
    numeric_topic.name = "/robot/speed";
    numeric_topic.schema_id = *numeric_schema;
    auto numeric_topic_or = writer.registerTopic(dataset_id_, numeric_topic);
    ASSERT_TRUE(numeric_topic_or.has_value());
    numeric_topic_id_ = *numeric_topic_or;
    ASSERT_TRUE(writer.bindTopicWriter(numeric_topic_id_).has_value());

    auto int_schema = writer.registerSchema("mode_sample", makePrimitive("mode", PrimitiveType::kInt32));
    ASSERT_TRUE(int_schema.has_value());
    TopicDescriptor int_topic;
    int_topic.name = "/robot/mode";
    int_topic.schema_id = *int_schema;
    auto int_topic_or = writer.registerTopic(dataset_id_, int_topic);
    ASSERT_TRUE(int_topic_or.has_value());
    ASSERT_TRUE(writer.bindTopicWriter(*int_topic_or).has_value());

    auto bool_schema = writer.registerSchema("estop_sample", makePrimitive("engaged", PrimitiveType::kBool));
    ASSERT_TRUE(bool_schema.has_value());
    TopicDescriptor bool_topic;
    bool_topic.name = "/robot/estop";
    bool_topic.schema_id = *bool_schema;
    auto bool_topic_or = writer.registerTopic(dataset_id_, bool_topic);
    ASSERT_TRUE(bool_topic_or.has_value());
    ASSERT_TRUE(writer.bindTopicWriter(*bool_topic_or).has_value());

    appendState(writer, {{0, "IDLE"}, {5, "RUNNING"}});
    ASSERT_TRUE(writer.beginRow(numeric_topic_id_, 0).has_value());
    writer.set(numeric_topic_id_, 0, 1.0);
    ASSERT_TRUE(writer.finishRow(numeric_topic_id_).has_value());
    ASSERT_TRUE(writer.beginRow(*int_topic_or, 0).has_value());
    writer.set(*int_topic_or, 0, static_cast<int32_t>(2));
    ASSERT_TRUE(writer.finishRow(*int_topic_or).has_value());
    ASSERT_TRUE(writer.beginRow(*bool_topic_or, 0).has_value());
    writer.set(*bool_topic_or, 0, true);
    ASSERT_TRUE(writer.finishRow(*bool_topic_or).has_value());
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());

    catalog_ = std::make_unique<CatalogModel>(&session_);
    catalog_->rebuildFromDatastore();
    string_key_ = keyFor(u"state"_s);
    numeric_key_ = keyFor(u"value"_s);
    int_key_ = keyFor(u"mode"_s);
    bool_key_ = keyFor(u"engaged"_s);
    ASSERT_FALSE(string_key_.isEmpty());
    ASSERT_FALSE(numeric_key_.isEmpty());
    ASSERT_FALSE(int_key_.isEmpty());
    ASSERT_FALSE(bool_key_.isEmpty());

    playback_.setRange(DisplayRange{.min = DisplaySeconds{0.0}, .max = DisplaySeconds{20.0}});

    view_ = std::make_unique<StateTransitionsView>();
    controller_ = std::make_unique<StateTransitionsController>(view_.get(), &session_, catalog_.get(), &playback_);
  }

  void appendState(DataWriter& writer, std::initializer_list<std::pair<Timestamp, const char*>> samples) {
    for (const auto& [seconds, value] : samples) {
      ASSERT_TRUE(writer.beginRow(string_topic_id_, seconds * kNs).has_value());
      if (value == nullptr) {
        writer.setNull(string_topic_id_, 0);
      } else {
        writer.set(string_topic_id_, 0, std::string_view{value});
      }
      ASSERT_TRUE(writer.finishRow(string_topic_id_).has_value());
    }
  }

  /// Let the coalescing refresh's trailing edge (~100 ms window) fire. A bare
  /// 150 ms budget leaves 50 ms of headroom, which a loaded CI host overruns —
  /// it surfaces as a half-refreshed row rather than as a timeout.
  static void waitForCoalescedRefresh() {
    QTest::qWait(1000);
  }

  /// Same trailing edge, but returns as soon as `ready` observes the refreshed
  /// state. Prefer this wherever the expected result is known: it is faster than
  /// the fixed wait and independent of how heavily the host is loaded.
  template <typename Predicate>
  static void waitForCoalescedRefresh(Predicate ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
      QTest::qWait(10);
    }
  }

  void appendStateAndCommit(std::initializer_list<std::pair<Timestamp, const char*>> samples) {
    auto writer = session_.dataEngine().createWriter();
    ASSERT_TRUE(writer.bindTopicWriter(string_topic_id_).has_value());
    appendState(writer, samples);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  /// The catalog key of the scalar field named `field` (keys are opaque).
  [[nodiscard]] QString keyFor(const QString& field) const {
    for (const CatalogItem& item : catalog_->items()) {
      if (const ScalarFieldPayload* scalar = asScalarField(item); scalar != nullptr && scalar->field_name == field) {
        return item.key;
      }
    }
    return {};
  }

  SessionManager session_;
  PlaybackEngine playback_;
  std::unique_ptr<CatalogModel> catalog_;
  std::unique_ptr<StateTransitionsView> view_;
  std::unique_ptr<StateTransitionsController> controller_;
  DatasetId dataset_id_ = 0;
  TopicId string_topic_id_ = 0;
  TopicId numeric_topic_id_ = 0;
  QString string_key_;
  QString numeric_key_;
  QString int_key_;
  QString bool_key_;

  /// In-place reload of `/robot/state` whose staged recording declares the
  /// field with a DIFFERENT primitive — the schema-drift case rebindRowFromCatalog
  /// exists for. Follows the replaceDataset contract: stage into a throwaway
  /// engine/store, replace, rebuild the catalog with no event loop between.
  void replaceStateTopicWithType(PrimitiveType type) {
    DataEngine staged_engine;
    ObjectStore staged_store;
    auto staged_dataset_or = staged_engine.createDataset(DatasetDescriptor{.source_name = "test.mcap"});
    ASSERT_TRUE(staged_dataset_or.has_value());
    auto writer = staged_engine.createWriter();
    auto schema_or = writer.registerSchema("state_sample_v2", makePrimitive("state", type));
    ASSERT_TRUE(schema_or.has_value());
    TopicDescriptor descriptor;
    descriptor.name = "/robot/state";
    descriptor.schema_id = *schema_or;
    auto topic_or = writer.registerTopic(*staged_dataset_or, descriptor);
    ASSERT_TRUE(topic_or.has_value());
    ASSERT_TRUE(writer.bindTopicWriter(*topic_or).has_value());
    for (int second = 0; second < 2; ++second) {
      ASSERT_TRUE(writer.beginRow(*topic_or, second * kNs).has_value());
      if (type == PrimitiveType::kInt64) {
        writer.set(*topic_or, 0, static_cast<int64_t>(second + 1));
      } else {
        writer.set(*topic_or, 0, static_cast<double>(second));
      }
      ASSERT_TRUE(writer.finishRow(*topic_or).has_value());
    }
    EXPECT_FALSE(staged_engine.commitChunks(writer.flushAll()).empty());
    session_.replaceDataset(staged_engine, staged_store, *staged_dataset_or, dataset_id_, {});
    catalog_->rebuildFromDatastore();
  }
};

TEST_F(StateTransitionsControllerTest, AddSeriesGates) {
  QSignalSpy changed(controller_.get(), &StateTransitionsController::seriesListChanged);
  EXPECT_TRUE(controller_->addSeries(string_key_));
  EXPECT_EQ(controller_->rowCount(), 1);
  EXPECT_EQ(view_->rowCountForTest(), 1);
  // Duplicate and non-discrete (float) keys are refused.
  EXPECT_FALSE(controller_->addSeries(string_key_));
  EXPECT_FALSE(controller_->addSeries(numeric_key_));
  EXPECT_FALSE(controller_->addSeries(u"no/such/key"_s));
  EXPECT_EQ(controller_->rowCount(), 1);
  EXPECT_EQ(changed.count(), 1);
  // Integer and bool fields are discrete series too.
  EXPECT_TRUE(controller_->addSeries(int_key_));
  EXPECT_TRUE(controller_->addSeries(bool_key_));
  EXPECT_EQ(controller_->rowCount(), 3);
  const StateRow int_row = view_->rowForTest(1);
  ASSERT_FALSE(int_row.segments.empty());
  EXPECT_EQ(int_row.segments[0].value, u"2"_s);
  const StateRow bool_row = view_->rowForTest(2);
  ASSERT_FALSE(bool_row.segments.empty());
  EXPECT_EQ(bool_row.segments[0].value, u"true"_s);

  // The view got display-frame segments with the trailing run pinned to the
  // playback range max (20 s).
  const StateRow row = view_->rowForTest(0);
  ASSERT_EQ(row.segments.size(), 2U);
  EXPECT_EQ(row.segments[0].value, u"IDLE"_s);
  EXPECT_EQ(row.segments[1].value, u"RUNNING"_s);
  EXPECT_EQ(row.segments[1].t_end_ns, 20 * kNs);
}

TEST_F(StateTransitionsControllerTest, DropIntentFiltersToDiscreteKeys) {
  view_->dropKeysForTest({numeric_key_, string_key_, int_key_});
  EXPECT_EQ(controller_->rowCount(), 2);  // the float key is silently dropped
  EXPECT_EQ(controller_->currentSeriesKeys(), (QStringList{string_key_, int_key_}));
}

TEST_F(StateTransitionsControllerTest, RemoveAndVisibilityViaPanelSurface) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  QSignalSpy changed(controller_.get(), &StateTransitionsController::seriesListChanged);

  // Eye toggle: the row leaves the strip but keeps its panel entry + data.
  const auto entries = controller_->seriesEntries();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_TRUE(controller_->setSeriesVisible(entries.front().row_id, false));
  EXPECT_EQ(view_->rowCountForTest(), 0);
  EXPECT_EQ(controller_->rowCount(), 1);
  EXPECT_FALSE(controller_->seriesEntries().front().visible);
  EXPECT_TRUE(controller_->setSeriesVisible(entries.front().row_id, true));
  EXPECT_EQ(view_->rowCountForTest(), 1);

  // Bin: removes the row entirely (the panel's trash path).
  EXPECT_TRUE(controller_->removeSeries(entries.front().row_id));
  EXPECT_EQ(controller_->rowCount(), 0);
  EXPECT_EQ(view_->rowCountForTest(), 0);
  EXPECT_EQ(changed.count(), 3);
}

TEST_F(StateTransitionsControllerTest, IngestRefreshesAffectedRow) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  appendStateAndCommit({{8, "ERROR"}});
  // commitChunks emits samplesIngested synchronously; the refresh coalesces to
  // the trigger's trailing edge (~100 ms window).
  waitForCoalescedRefresh([this] { return view_->rowForTest(0).segments.size() == 3U; });
  const StateRow row = view_->rowForTest(0);
  ASSERT_EQ(row.segments.size(), 3U);
  EXPECT_EQ(row.segments[2].value, u"ERROR"_s);
}

TEST_F(StateTransitionsControllerTest, PerDatasetOffsetRemapsWithoutReread) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  session_.setDisplayOffset(dataset_id_, DisplayOffset{Duration{2 * kNs}});
  const StateRow row = view_->rowForTest(0);
  ASSERT_EQ(row.segments.size(), 2U);
  EXPECT_EQ(row.segments[0].t_start_ns, -2 * kNs);  // display = raw − offset
  EXPECT_EQ(row.segments[1].t_start_ns, 3 * kNs);
}

TEST_F(StateTransitionsControllerTest, DatasetReplaceClearsSynchronously) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  ASSERT_EQ(view_->rowForTest(0).segments.size(), 2U);
  {
    // RefillGuard's CONSTRUCTION emits datasetAboutToBeReplaced; the adapters
    // must drop their caches synchronously inside it. Destruction without
    // commit() rolls the dataset back to its prior data.
    RefillGuard guard(session_, dataset_id_);
  }
  waitForCoalescedRefresh();  // the coalesced refresh re-reads the restored data
  EXPECT_EQ(view_->rowForTest(0).segments.size(), 2U);
}

TEST_F(StateTransitionsControllerTest, ReorderSeriesFollowsLayerListDropArithmetic) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  ASSERT_TRUE(controller_->addSeries(int_key_));
  ASSERT_TRUE(controller_->addSeries(bool_key_));
  QSignalSpy changed(controller_.get(), &StateTransitionsController::seriesListChanged);

  // Drop row 0 past the end: [state, mode, estop] -> [mode, estop, state],
  // reflected in the strip's visible rows AND the persisted series order.
  EXPECT_TRUE(controller_->reorderSeries(0, 3));
  EXPECT_EQ(changed.count(), 1);
  auto series = controller_->currentSeries();
  ASSERT_EQ(series.size(), 3U);
  EXPECT_EQ(series[0].topic_name, u"/robot/mode"_s);
  EXPECT_EQ(series[1].topic_name, u"/robot/estop"_s);
  EXPECT_EQ(series[2].topic_name, u"/robot/state"_s);
  EXPECT_EQ(view_->rowForTest(2).name, u"robot/state/state"_s);  // full "topic/field" legend path

  // Dropping a row onto its own slot (or the gap just after it) is a no-op:
  // no reorder, no signal, no spurious undo snapshot.
  EXPECT_FALSE(controller_->reorderSeries(1, 1));
  EXPECT_FALSE(controller_->reorderSeries(1, 2));
  EXPECT_FALSE(controller_->reorderSeries(5, 0));  // out of range
  EXPECT_EQ(changed.count(), 1);
}

TEST_F(StateTransitionsControllerTest, ReplaceRetypeToIntRebindsRow) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  ASSERT_EQ(view_->rowForTest(0).segments.size(), 2U);

  // The reloaded recording declares /robot/state/state as int64: the key and
  // TopicId stay stable, so the row must follow the new type, not go blank.
  replaceStateTopicWithType(PrimitiveType::kInt64);
  waitForCoalescedRefresh();
  EXPECT_EQ(controller_->rowCount(), 1);
  const StateRow row = view_->rowForTest(0);
  ASSERT_EQ(row.segments.size(), 2U);
  EXPECT_EQ(row.segments[0].value, u"1"_s);
  EXPECT_EQ(row.segments[1].value, u"2"_s);
}

TEST_F(StateTransitionsControllerTest, ReplaceRetypeToFloatDropsRow) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  QSignalSpy changed(controller_.get(), &StateTransitionsController::seriesListChanged);

  // Retyped to a float: no longer a discrete field, so the row leaves the
  // strip — the same policy as a vanished topic.
  replaceStateTopicWithType(PrimitiveType::kFloat64);
  waitForCoalescedRefresh([this] { return controller_->rowCount() == 0; });
  EXPECT_EQ(controller_->rowCount(), 0);
  EXPECT_EQ(view_->rowCountForTest(), 0);
  EXPECT_GE(changed.count(), 1);
}

TEST_F(StateTransitionsControllerTest, CatalogRemovalPrunesRows) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  QSignalSpy changed(controller_.get(), &StateTransitionsController::seriesListChanged);
  catalog_->removeItems({string_key_});  // emits itemsRemoved
  EXPECT_EQ(controller_->rowCount(), 0);
  EXPECT_EQ(view_->rowCountForTest(), 0);
  EXPECT_EQ(changed.count(), 1);

  // A fresh rebuild resurfaces the catalog entry; adding again then clearing
  // everything prunes through the cleared() path too.
  catalog_->resetRemovalState();
  catalog_->rebuildFromDatastore();
  string_key_ = keyFor(u"state"_s);
  ASSERT_TRUE(controller_->addSeries(string_key_));
  catalog_->clearAll();  // emits cleared
  EXPECT_EQ(controller_->rowCount(), 0);
}

TEST_F(StateTransitionsControllerTest, RangeGrowthExtendsTrailingSegment) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  playback_.setRange(DisplayRange{.min = DisplaySeconds{0.0}, .max = DisplaySeconds{50.0}});
  waitForCoalescedRefresh([this] {
    const StateRow row = view_->rowForTest(0);
    return !row.segments.empty() && row.segments.back().t_end_ns == 50 * kNs;
  });
  EXPECT_EQ(view_->rowForTest(0).segments.back().t_end_ns, 50 * kNs);
}

TEST_F(StateTransitionsControllerTest, DemandSurfaceListsDisplayedTopics) {
  ASSERT_TRUE(controller_->addSeries(string_key_));
  const auto topics = controller_->displayedTopics();
  ASSERT_EQ(topics.size(), 1);
  EXPECT_EQ(topics.front().first, dataset_id_);
  EXPECT_EQ(topics.front().second, u"/robot/state"_s);
}

}  // namespace
}  // namespace PJ
