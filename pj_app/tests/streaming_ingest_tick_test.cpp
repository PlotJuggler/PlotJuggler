// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// One streaming poll tick (StreamingSourceManager::onIngestTick), driven through
// ingestTickForTests over a bare DataSourceRuntimeHost: no plugin, no worker
// thread. Eager processors advance over every topic of the dataset, including
// one committed straight into the engine as a toolbox does, and the ingest
// announcement names every topic.

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "StreamingSourceManager.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "support/gui_test_env.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// Org/app names sandbox QSettings reads (the manager reads its retention
// window) away from the user's real preferences, without affecting other suites.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("StreamingIngestTickTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PJ4StreamingIngestTickTest"_s);
    QCoreApplication::setApplicationName(u"PJ4StreamingIngestTickTest"_s);
  }
};
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

constexpr const char* kNegateSource = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

[[nodiscard]] uint64_t rowCount(DataEngine& engine, TopicId topic_id) {
  DataReader reader(engine);
  const auto meta = reader.getMetadata(topic_id);
  return meta.has_value() ? meta->total_row_count : 0;
}

// Topic "a" is written through the runtime host; topic "b" is committed straight
// into the engine, which no write host sees. Each has an eager negate transform.
class StreamingIngestTickTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DataEngine& primary = session_.sessionManager().dataEngine();
    DataEngine& secondary = manager_.secondaryEngineForTests();

    // Lockstep dataset on both engines with the SAME id, as startSession sets up.
    const auto time_domain = primary.createTimeDomain("[stream] tick");
    ASSERT_TRUE(time_domain.has_value()) << time_domain.error();
    ASSERT_TRUE(secondary.createTimeDomain("[stream] tick", *time_domain).has_value());
    const auto dataset =
        primary.createDataset(DatasetDescriptor{.source_name = "[stream] tick", .time_domain_id = *time_domain});
    ASSERT_TRUE(dataset.has_value()) << dataset.error();
    dataset_id_ = static_cast<DatasetId>(*dataset);
    ASSERT_TRUE(secondary
                    .createDataset(
                        DatasetDescriptor{.source_name = "[stream] tick", .time_domain_id = *time_domain}, dataset_id_)
                    .has_value());

    host_ = std::make_unique<DataSourceRuntimeHost>(
        primary, extensions_, dataset_id_, PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id_)},
        session_.sessionManager().objectStore(), "ingest_tick_source", /*parser_registrar=*/nullptr,
        &manager_.secondaryStoreForTests(), &secondary, /*library_keepalive=*/nullptr);
    ASSERT_TRUE(host_->registerServices(registry_).has_value());
    sdk::ServiceRegistry services(registry_.view());
    auto writer_or = services.require<sdk::SourceWriteHostService>();
    ASSERT_TRUE(writer_or.has_value()) << writer_or.error();
    writer_ = *writer_or;

    topic_a_ = *writer_.ensureTopic("a");
    field_a_ = *writer_.ensureField(topic_a_, "v", PrimitiveType::kFloat64);
    engine_writer_ = std::make_unique<DataWriter>(primary.createWriter());
    series_b_ = *engine_writer_->registerScalarSeries(dataset_id_, "b", NumericType::kFloat64);

    writeA(1, 1.0);
    commitB(1, 1.0);
    DataProcessorService& processors = session_.sessionManager().dataProcessorService();
    const auto negate_a = processors.upsertTransform("pluginA", "negate", {"a"}, {"a_neg"}, kNegateSource, "{}");
    ASSERT_TRUE(negate_a.has_value()) << negate_a.error();
    const auto negate_b = processors.upsertTransform("pluginB", "negate", {"b"}, {"b_neg"}, kNegateSource, "{}");
    ASSERT_TRUE(negate_b.has_value()) << negate_b.error();
    a_neg_ = negate_a->output_topic_ids.at(0);
    b_neg_ = negate_b->output_topic_ids.at(0);
    ASSERT_EQ(rowCount(primary, a_neg_), 1U);
    ASSERT_EQ(rowCount(primary, b_neg_), 1U);
  }

  void writeA(int64_t timestamp_ns, double value) {
    ASSERT_TRUE(writer_.appendBoundRecord(topic_a_, timestamp_ns, {{.field = field_a_, .value = value}}).has_value());
    host_->flushPending();
  }

  void commitB(int64_t timestamp_ns, double value) {
    engine_writer_->appendScalar(series_b_, timestamp_ns, value);
    session_.sessionManager().dataEngine().commitChunks(engine_writer_->flushAll());
  }

  [[nodiscard]] uint64_t primaryRows(TopicId topic_id) {
    return rowCount(session_.sessionManager().dataEngine(), topic_id);
  }

  AppSession session_;
  ExtensionCatalogService extensions_{QString{}};
  StreamingSourceManager manager_{
      session_.sessionManager(), extensions_, session_.catalogModel(), session_.topicDemandTracker(),
      /*dialog_parent=*/nullptr};
  DatasetId dataset_id_{0};
  std::unique_ptr<DataSourceRuntimeHost> host_;
  ServiceRegistryBuilder registry_;
  sdk::SourceWriteHostView writer_;
  sdk::TopicHandle topic_a_;
  sdk::FieldHandle field_a_;
  std::unique_ptr<DataWriter> engine_writer_;
  ScalarSeriesHandle series_b_{};
  TopicId a_neg_{0};
  TopicId b_neg_{0};
};

// Every topic of the dataset advances, wherever its rows came from.
TEST_F(StreamingIngestTickTest, ATickAdvancesProcessorsOverEveryTopicOfTheDataset) {
  writeA(2, 2.0);
  commitB(2, 2.0);

  manager_.ingestTickForTests(dataset_id_);

  EXPECT_EQ(primaryRows(a_neg_), 2U);
  EXPECT_EQ(primaryRows(b_neg_), 2U) << "a row committed outside the write hosts must still reach its processor";
}

// A tick that finds nothing new leaves every output as it was.
TEST_F(StreamingIngestTickTest, ATickWithNothingNewLeavesTheOutputsAlone) {
  writeA(2, 2.0);
  manager_.ingestTickForTests(dataset_id_);
  ASSERT_EQ(primaryRows(a_neg_), 2U);

  manager_.ingestTickForTests(dataset_id_);
  manager_.ingestTickForTests(dataset_id_);

  EXPECT_EQ(primaryRows(a_neg_), 2U);
  EXPECT_EQ(primaryRows(b_neg_), 1U);
}

// The announcement names every topic of the dataset, quiet ones and the
// processors' own outputs included: nothing else tells a plot of a filtered
// curve, or a topic a retention trim moved, to refresh.
TEST_F(StreamingIngestTickTest, TheIngestAnnouncementNamesEveryTopicOfTheDataset) {
  std::optional<QVector<TopicId>> announced;
  std::optional<bool> announced_live;
  QObject::connect(
      &session_.sessionManager(), &SessionManager::samplesIngested, &manager_,
      [&](const QVector<TopicId>& ids, bool live) {
        announced = ids;
        announced_live = live;
      });
  writeA(2, 2.0);

  manager_.ingestTickForTests(dataset_id_);

  ASSERT_TRUE(announced.has_value());
  for (const TopicId topic_id : {topic_a_.id, series_b_.topic_id, a_neg_, b_neg_}) {
    EXPECT_TRUE(announced->contains(topic_id)) << "topic " << topic_id << " missing from the announcement";
  }
  EXPECT_EQ(announced_live, std::optional<bool>{true});
}

// While paused the writes land in the secondary engine: the tick advances no
// processor and announces the ingest as not live.
TEST_F(StreamingIngestTickTest, APausedTickAdvancesNoProcessor) {
  manager_.onPauseToggled(true);
  manager_.applyWriteTargetsForCurrentPauseState(*host_);
  std::optional<bool> announced_live;
  QObject::connect(
      &session_.sessionManager(), &SessionManager::samplesIngested, &manager_,
      [&](const QVector<TopicId>&, bool live) { announced_live = live; });
  writeA(2, 2.0);
  commitB(2, 2.0);

  manager_.ingestTickForTests(dataset_id_);

  EXPECT_EQ(primaryRows(a_neg_), 1U);
  EXPECT_EQ(primaryRows(b_neg_), 1U);
  EXPECT_EQ(announced_live, std::optional<bool>{false});
}

}  // namespace
}  // namespace PJ
