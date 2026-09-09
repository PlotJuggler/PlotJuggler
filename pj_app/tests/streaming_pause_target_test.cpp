// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A streaming source started WHILE paused must write into the secondary tail
// buffer, never the frozen primary. startSession is UI-coupled, so this test
// drives applyWriteTargetsForCurrentPauseState on a bare host to pin the
// pause-target routing without the dialog. Removing the pause branch turns
// the first EXPECT red.

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>
#include <cstdint>

#include "StreamingSourceManager.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "support/gui_test_env.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

using sdk::SourceWriteHostView;

// Org/app names sandbox QSettings reads (the manager reads its retention
// window) away from the user's real preferences, without affecting other suites.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("StreamingPauseTargetTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PJ4StreamingPauseTargetTest"_s);
    QCoreApplication::setApplicationName(u"PJ4StreamingPauseTargetTest"_s);
  }
};
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

// Committed rows for `topic_id` on `engine`.
[[nodiscard]] uint64_t rowCount(DataEngine& engine, TopicId topic_id) {
  DataReader reader(engine);
  const auto meta = reader.getMetadata(topic_id);
  return meta.has_value() ? meta->total_row_count : 0;
}

}  // namespace

TEST(StreamingPauseTargetTest, SessionCreatedWhilePausedWritesToSecondary) {
  AppSession session;
  ExtensionCatalogService extensions{QString{}};
  StreamingSourceManager manager(
      session.sessionManager(), extensions, session.catalogModel(), session.topicDemandTracker(),
      /*dialog_parent=*/nullptr);

  DataEngine& primary = session.sessionManager().dataEngine();
  DataEngine& secondary = manager.secondaryEngineForTests();

  // Lockstep dataset on both engines with the SAME id — exactly what
  // startSession sets up before building the runtime host.
  const auto td = primary.createTimeDomain("[stream] test");
  ASSERT_TRUE(td.has_value()) << td.error();
  ASSERT_TRUE(secondary.createTimeDomain("[stream] test", *td).has_value());
  const auto ds = primary.createDataset(DatasetDescriptor{.source_name = "[stream] test", .time_domain_id = *td});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  const DatasetId dataset_id = static_cast<DatasetId>(*ds);
  ASSERT_TRUE(
      secondary.createDataset(DatasetDescriptor{.source_name = "[stream] test", .time_domain_id = *td}, dataset_id)
          .has_value());

  // Host wired exactly as startSession wires it: primary engine + primary store,
  // with the manager's own secondary store/engine as the pause tail buffer.
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(dataset_id)};
  DataSourceRuntimeHost host(
      primary, extensions, dataset_id, source_handle, session.sessionManager().objectStore(), "pause_target_source",
      /*parser_registrar=*/nullptr, &manager.secondaryStoreForTests(), &secondary, /*library_keepalive=*/nullptr);
  ServiceRegistryBuilder registry;
  ASSERT_TRUE(host.registerServices(registry).has_value());
  sdk::ServiceRegistry services(registry.view());
  auto writer_or = services.require<sdk::SourceWriteHostService>();
  ASSERT_TRUE(writer_or.has_value()) << writer_or.error();
  SourceWriteHostView writer = *writer_or;

  // The user is paused BEFORE this source starts. startSession applies the
  // pause targets right before start(); do the same here.
  manager.onPauseToggled(true);
  manager.applyWriteTargetsForCurrentPauseState(host);

  const auto topic = *writer.ensureTopic("late_imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, {{.field = field, .value = 1.0F}}).has_value());
  host.flushPending();

  EXPECT_EQ(rowCount(secondary, topic.id), 1U)
      << "a source added while paused must write into the secondary tail buffer";
  EXPECT_EQ(rowCount(primary, topic.id), 0U)
      << "the frozen primary gained a row during pause — the timeline would advance";
}

}  // namespace PJ
