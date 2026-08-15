// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
using namespace Qt::StringLiterals;

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace {

// Collect the qWarning output `body` produces. The Qt handler is process-global,
// so the previous one is restored before returning and the sink is only live for
// the duration of the call.
std::vector<std::string> captureWarnings(const std::function<void()>& body) {
  static std::vector<std::string>* sink = nullptr;
  std::vector<std::string> captured;
  sink = &captured;
  QtMessageHandler previous =
      qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& text) {
        if (type == QtWarningMsg && sink != nullptr) {
          sink->push_back(text.toStdString());
        }
      });
  body();
  qInstallMessageHandler(previous);
  sink = nullptr;
  return captured;
}

class ToolboxRuntimeHostTest : public ::testing::Test {
 protected:
  PJ::sdk::ServiceRegistry registered() {
    EXPECT_TRUE(host_->registerServices(builder_).has_value());
    return PJ::sdk::ServiceRegistry(builder_.view());
  }

  // Rows become visible only after a flush; createDataSource's handle id is the
  // backing DatasetId (see DatastoreToolboxHost), so we read back through it.
  [[nodiscard]] uint64_t totalRowCount(uint32_t source_id) const {
    PJ::DataReader reader(engine_);
    const auto topics = reader.listTopics(static_cast<PJ::DatasetId>(source_id));
    if (topics.empty()) {
      return 0;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? metadata->total_row_count : 0;
  }

  // Bring-up state the helpers below fill in; every helper takes it as an
  // out-param and returns void so gtest ASSERTs abort the helper itself —
  // callers wrap each call in ASSERT_NO_FATAL_FAILURE. It owns the catalog the
  // host's ingest contexts point at, so it must outlive host_.
  struct StartedIngest {
    std::unique_ptr<PJ::test::HermeticCatalog> catalog;
    std::optional<PJ::sdk::ToolboxHostView> toolbox;
    std::optional<PJ::ToolboxRuntimeHostView> runtime;
    std::optional<PJ::DataSourceRuntimeHostView> view;
    uint32_t source_id = 0;
  };
  // Host bring-up only: hermetic catalog deps + a host wired with `callbacks`,
  // with NO ingest context yet, so a caller can observe the pre-ingest state.
  void buildHermeticHost(PJ::ToolboxRuntimeHost::Callbacks callbacks, StartedIngest& out) {
    QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
    out.catalog = std::make_unique<PJ::test::HermeticCatalog>(plugin_file.absolutePath());
    PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
    deps.catalog = &out.catalog->service;
    host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
        engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
    auto services = registered();
    auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
    ASSERT_TRUE(toolbox_or.has_value());
    out.toolbox.emplace(*toolbox_or);
    auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
    ASSERT_TRUE(runtime_or.has_value());
    out.runtime.emplace(*runtime_or);
  }

  // One dataset + its parser-ingest context on a host from buildHermeticHost.
  void createIngestContext(StartedIngest& out) {
    ASSERT_TRUE(out.toolbox.has_value());
    const auto source = *out.toolbox->createDataSource("cloud download");
    out.source_id = source.id;
    PJ_data_source_runtime_host_t ingest_raw{};
    PJ_error_t error{};
    ASSERT_TRUE(
        out.runtime->raw().vtable->create_parser_ingest(out.runtime->raw().ctx, source.id, &ingest_raw, &error));
    out.view.emplace(ingest_raw);
  }

  // Bring-up shared by the teardown-terminal tests: hermetic catalog deps, a
  // host wired with `callbacks`, and one parser-ingest context — with its
  // progress sequence STARTED unless start_progress is false (a test that
  // needs the started callback QUEUED starts it from a worker thread via
  // `view`). Out-param + void return so gtest ASSERTs abort the helper;
  // callers wrap the call in ASSERT_NO_FATAL_FAILURE.
  void startHermeticIngest(
      PJ::ToolboxRuntimeHost::Callbacks callbacks, StartedIngest& out, bool start_progress = true) {
    ASSERT_NO_FATAL_FAILURE(buildHermeticHost(std::move(callbacks), out));
    ASSERT_NO_FATAL_FAILURE(createIngestContext(out));
    if (start_progress) {
      ASSERT_TRUE(out.view->progressStart("import", 10, true).has_value());  // on-thread: started delivered directly
    }
  }

  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::sdk::InMemorySettingsBackend settings_;
  PJ::ServiceRegistryBuilder builder_;
  std::unique_ptr<PJ::ToolboxRuntimeHost> host_;
};

TEST_F(ToolboxRuntimeHostTest, RegistersWriteRuntimeAndSettingsServices) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  auto services = registered();

  EXPECT_TRUE(services.get<PJ::sdk::ToolboxHostService>().has_value());
  EXPECT_TRUE(services.get<PJ::sdk::ToolboxRuntimeHostService>().has_value());
  EXPECT_TRUE(services.get<PJ::sdk::SettingsStoreService>().has_value());
}

// A REQUIRED service the host could not register must fail registration at the
// boundary where the cause is still known. A duplicate name is a host wiring
// defect: continuing would bind the plugin against whichever instance got there
// first — the wrong service surface, indistinguishable from the right one.
TEST_F(ToolboxRuntimeHostTest, DuplicateRequiredServiceFailsRegistration) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  ASSERT_TRUE(host_->registerServices(builder_).has_value());
  ASSERT_EQ(builder_.size(), 4U);

  const PJ::Status status = host_->registerServices(builder_);

  ASSERT_FALSE(status.has_value());
  EXPECT_NE(status.error().find(PJ::sdk::ToolboxHostService::kName), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("duplicate name"), std::string::npos) << status.error();
  // Fail-fast: nothing new landed, and the later services were not attempted.
  EXPECT_EQ(builder_.size(), 4U);
}

// An OPTIONAL service is one the SDK plugin bases reach through get<>(), so its
// absence is a supported degraded mode — it must warn and let the host succeed
// rather than failing the whole bind.
TEST_F(ToolboxRuntimeHostTest, DuplicateOptionalServiceWarnsButStillSucceeds) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  // Pre-claim ONLY the optional settings service; every required name stays free.
  int decoy = 0;
  ASSERT_TRUE(
      builder_.tryRegisterService(PJ::sdk::SettingsStoreService::kName, 1, PJ_service_t{&decoy, &decoy}).has_value());

  PJ::Status status{};
  const auto warnings = captureWarnings([&]() { status = host_->registerServices(builder_); });

  EXPECT_TRUE(status.has_value()) << status.error();
  ASSERT_EQ(warnings.size(), 1U);
  EXPECT_NE(warnings[0].find(PJ::sdk::SettingsStoreService::kName), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("duplicate name"), std::string::npos) << warnings[0];
  // The three required services still landed alongside the pre-claimed decoy.
  EXPECT_EQ(builder_.size(), 4U);
}

TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedFiresOnDataChangedCallback) {
  int calls = 0;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&calls](std::vector<PJ::DatasetId>) { ++calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  runtime->notifyDataChanged();
  EXPECT_EQ(calls, 1);
}

TEST_F(ToolboxRuntimeHostTest, ReportMessageRoutesLevelAndTextToOnMessage) {
  std::vector<std::pair<PJ_toolbox_message_level_t, std::string>> received;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_message = [&received](PJ_toolbox_message_level_t level, std::string text) {
    received.emplace_back(level, std::move(text));
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  runtime->reportMessage(PJ::ToolboxMessageLevel::kError, "fetch failed");
  ASSERT_EQ(received.size(), 1U);
  EXPECT_EQ(received[0].first, PJ_TOOLBOX_MESSAGE_ERROR);
  EXPECT_EQ(received[0].second, "fetch failed");
}

// The runtime-host vtable advertises notify_data_changed as [thread-safe]: a
// plugin worker thread may call it. The host must not run the (Qt-touching)
// callback on that worker thread — it marshals it to the constructing/GUI
// thread. Lock the contract: fire from a std::thread, prove the callback did
// NOT run synchronously on the worker, then runs on the host thread once its
// event loop is pumped.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedMarshalsCallbackToConstructingThread) {
  std::atomic<bool> fired{false};
  std::thread::id callback_thread;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&](std::vector<PJ::DatasetId>) {
    callback_thread = std::this_thread::get_id();
    fired.store(true);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto host_thread = std::this_thread::get_id();
  std::thread worker([&]() { runtime->notifyDataChanged(); });
  worker.join();

  // Cross-thread => queued: the callback must not have run on the worker.
  EXPECT_FALSE(fired.load());

  QCoreApplication::processEvents();

  EXPECT_TRUE(fired.load());
  EXPECT_EQ(callback_thread, host_thread);
}

TEST_F(ToolboxRuntimeHostTest, ReportMessageMarshalsCallbackToConstructingThread) {
  std::atomic<bool> fired{false};
  std::thread::id callback_thread;
  std::string received_text;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_message = [&](PJ_toolbox_message_level_t, std::string text) {
    callback_thread = std::this_thread::get_id();
    received_text = std::move(text);
    fired.store(true);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto host_thread = std::this_thread::get_id();
  std::thread worker([&]() { runtime->reportMessage(PJ::ToolboxMessageLevel::kWarning, "from worker"); });
  worker.join();

  EXPECT_FALSE(fired.load());

  QCoreApplication::processEvents();

  EXPECT_TRUE(fired.load());
  EXPECT_EQ(callback_thread, host_thread);
  EXPECT_EQ(received_text, "from worker");
}

// End-to-end write -> notify -> read: a plugin writes through ToolboxHostService
// (buffered), and notifyDataChanged must seal those writes (flushPending) before
// firing on_data_changed, so the freshly written rows are visible to a reader by
// the time the host rebuilds its catalog.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedFlushesBufferedWritesBeforeCatalogRebuild) {
  std::atomic<int> data_changed_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&](std::vector<PJ::DatasetId>) { ++data_changed_calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto toolbox = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox.has_value()) << toolbox.error();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto source = *toolbox->createDataSource("mosaico");
  const auto topic = *toolbox->ensureTopic(source, "imu");
  ASSERT_TRUE(toolbox->ensureField(topic, "ax", PJ::PrimitiveType::kFloat64).has_value());
  const std::vector<PJ::sdk::NamedFieldValue> row = {{.name = "ax", .value = 1.5}};
  ASSERT_TRUE(toolbox->appendRecord(topic, 1, row).has_value());

  // Buffered but not flushed: no rows visible yet.
  EXPECT_EQ(totalRowCount(source.id), 0U);

  runtime->notifyDataChanged();

  EXPECT_EQ(data_changed_calls.load(), 1);
  EXPECT_EQ(totalRowCount(source.id), 1U);
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestDelegatesToCatalogParserAndRegistersObjectParser) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  ASSERT_NE(catalog.findParserByEncoding(u"runtime_host_object"_s), nullptr);

  std::vector<PJ::ObjectTopicId> registered_object_parsers;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  deps.register_object_parser = [&registered_object_parsers](
                                    PJ::ObjectTopicId id, std::unique_ptr<PJ::MessageParserHandle> parser) {
    EXPECT_NE(parser, nullptr);
    registered_object_parsers.push_back(id);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();

  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  auto ds = (*toolbox_or).createDataSource("cloud download");
  ASSERT_TRUE(ds.has_value()) << ds.error();

  auto ingest_or = (*runtime_or).createParserIngest(ds->id);
  ASSERT_TRUE(ingest_or.has_value()) << ingest_or.error();
  auto ingest = *ingest_or;

  auto binding = ingest.ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/camera/image",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/image",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding.has_value()) << binding.error();

  auto push = ingest.pushMessage(*binding, PJ::Timestamp{100}, []() -> std::vector<uint8_t> { return {1, 2, 3, 4}; });
  ASSERT_TRUE(push.has_value()) << push.error();

  // Release flushes: the stub parser's scalar row (byte_count) becomes readable.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  EXPECT_EQ(totalRowCount(ds->id), 1u);

  // mock/image classifies kImage: one object topic, one stored entry, and the
  // registrar received exactly one render-time parser instance.
  const auto object_topics = object_store_.listTopics(static_cast<PJ::DatasetId>(ds->id));
  ASSERT_EQ(object_topics.size(), 1u);
  EXPECT_EQ(object_store_.entryCount(object_topics.front()), 1u);
  EXPECT_EQ(registered_object_parsers.size(), 1u);

  // Idempotent release; recreate-after-release works.
  EXPECT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  EXPECT_TRUE((*runtime_or).createParserIngest(ds->id).has_value());

  // The recreated context references test-body locals (catalog, registrar):
  // release it and destroy the host while those locals are still alive.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  host_.reset();
}

// notify_data_changed reports WHICH datasets received a parser-ingest context
// since the previous notify, and drains the set: the host uses this to focus
// playback on a bulk import (a 10s cloud snippet must present a 10s timeline),
// while plain write-API notifies keep reporting an empty list.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedReportsAndDrainsIngestedDatasets) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;

  std::vector<std::vector<PJ::DatasetId>> reported;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&reported](std::vector<PJ::DatasetId> ingested) {
    reported.push_back(std::move(ingested));
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  // Write-API only: the notify reports no ingested datasets.
  auto plain = (*toolbox_or).createDataSource("write-api toolbox");
  ASSERT_TRUE(plain.has_value());
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_TRUE(reported[0].empty());

  // A parser-ingest context marks its dataset as a bulk import.
  auto ds = (*toolbox_or).createDataSource("cloud download");
  ASSERT_TRUE(ds.has_value());
  ASSERT_TRUE((*runtime_or).createParserIngest(ds->id).has_value());
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 2u);
  ASSERT_EQ(reported[1].size(), 1u);
  EXPECT_EQ(reported[1][0], static_cast<PJ::DatasetId>(ds->id));

  // Drained: the next notify is back to empty.
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 3u);
  EXPECT_TRUE(reported[2].empty());
}

// The progress slots on the parser-ingest fat pointer drive the shell's
// progressive-import surface: progress_start/finish bracket the import, and
// each unthrottled progress_update seals pending toolbox writes BEFORE
// on_ingest_progress fires, so the handler observes reader-visible rows.
TEST_F(ToolboxRuntimeHostTest, ProgressHooksFlushWritesAndDriveIngestCallbacks) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog_box.service;

  struct StartedEvent {
    PJ::DatasetId dataset;
    std::string label;
    uint64_t total;
  };
  struct ProgressEvent {
    PJ::DatasetId dataset;
    uint64_t current;
    uint64_t total;
    uint64_t visible_rows;
  };
  std::vector<StartedEvent> started;
  std::vector<ProgressEvent> progressed;
  std::vector<PJ::DatasetId> finished;
  uint32_t source_id = 0;  // assigned after createDataSource; read at callback time

  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&started](PJ::DatasetId dataset, std::string label, uint64_t total, bool) {
    started.push_back({dataset, std::move(label), total});
  };
  callbacks.on_ingest_progress = [&, this](PJ::DatasetId dataset, uint64_t current, uint64_t total) {
    progressed.push_back({dataset, current, total, totalRowCount(source_id)});
  };
  callbacks.on_ingest_finished = [&finished](PJ::DatasetId dataset) { finished.push_back(dataset); };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  host_->setFlushThrottleMs(0);
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  const auto source = *(*toolbox_or).createDataSource("cloud download");
  source_id = source.id;
  const auto topic = *(*toolbox_or).ensureTopic(source, "imu");
  ASSERT_TRUE((*toolbox_or).ensureField(topic, "ax", PJ::PrimitiveType::kFloat64).has_value());

  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(
      (*runtime_or).raw().vtable->create_parser_ingest((*runtime_or).raw().ctx, source.id, &ingest_raw, &error));
  const PJ::DataSourceRuntimeHostView progress(ingest_raw);

  ASSERT_TRUE(progress.progressStart("mosaico download", 10, /*cancellable=*/true).has_value());
  ASSERT_EQ(started.size(), 1u);
  EXPECT_EQ(started[0].dataset, static_cast<PJ::DatasetId>(source.id));
  EXPECT_EQ(started[0].label, "mosaico download");
  EXPECT_EQ(started[0].total, 10u);

  // Buffered toolbox write: invisible until the progress tick flushes it.
  const std::vector<PJ::sdk::NamedFieldValue> row = {{.name = "ax", .value = 1.5}};
  ASSERT_TRUE((*toolbox_or).appendRecord(topic, 1, row).has_value());
  EXPECT_EQ(totalRowCount(source.id), 0u);

  EXPECT_TRUE(progress.progressUpdate(3));
  ASSERT_EQ(progressed.size(), 1u);
  EXPECT_EQ(progressed[0].dataset, static_cast<PJ::DatasetId>(source.id));
  EXPECT_EQ(progressed[0].current, 3u);
  EXPECT_EQ(progressed[0].total, 10u);
  EXPECT_EQ(progressed[0].visible_rows, 1u);  // the flush preceded the callback

  progress.progressFinish();
  QCoreApplication::processEvents();  // terminals always queue, so they can never overtake a queued begin
  ASSERT_EQ(finished.size(), 1u);
  EXPECT_EQ(finished[0], static_cast<PJ::DatasetId>(source.id));

  ASSERT_TRUE((*runtime_or).releaseParserIngest(source.id).has_value());
  host_.reset();  // context references test-body locals (catalog)
}

// A mid-import dataset (between progress_start and progress_finish) re-reports
// on EVERY notify — the shell's playback focus must follow the growing import —
// and a released context re-reports once so the terminal notify runs the
// shell's focus/reconcile pass over the finished dataset.
TEST_F(ToolboxRuntimeHostTest, NotifyReportsMidImportDatasetOnEveryNotify) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog_box.service;

  std::vector<std::vector<PJ::DatasetId>> reported;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&reported](std::vector<PJ::DatasetId> ingested) {
    reported.push_back(std::move(ingested));
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  const auto source = *(*toolbox_or).createDataSource("cloud download");
  const auto ds_id = static_cast<PJ::DatasetId>(source.id);
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(
      (*runtime_or).raw().vtable->create_parser_ingest((*runtime_or).raw().ctx, source.id, &ingest_raw, &error));
  const PJ::DataSourceRuntimeHostView progress(ingest_raw);

  ASSERT_TRUE(progress.progressStart("import", 0, true).has_value());
  (*runtime_or).notifyDataChanged();
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 2u);
  EXPECT_EQ(reported[0], std::vector<PJ::DatasetId>{ds_id});  // create-time pending + active, deduped
  EXPECT_EQ(reported[1], std::vector<PJ::DatasetId>{ds_id});  // still mid-import

  progress.progressFinish();
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 3u);
  EXPECT_TRUE(reported[2].empty());  // import ended, context idle

  ASSERT_TRUE((*runtime_or).releaseParserIngest(source.id).has_value());
  (*runtime_or).notifyDataChanged();
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 5u);
  EXPECT_EQ(reported[3], std::vector<PJ::DatasetId>{ds_id});  // release re-reports once
  EXPECT_TRUE(reported[4].empty());
}

// The shell's "stop this import" routes through requestStopActiveIngests: a
// flag-only cooperative stop every live context observes via is_stop_requested
// and progress_update returning false.
TEST_F(ToolboxRuntimeHostTest, RequestStopActiveIngestsSignalsCooperativeStop) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog_box.service;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  const auto source = *(*toolbox_or).createDataSource("cloud download");
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(
      (*runtime_or).raw().vtable->create_parser_ingest((*runtime_or).raw().ctx, source.id, &ingest_raw, &error));
  const PJ::DataSourceRuntimeHostView progress(ingest_raw);

  EXPECT_FALSE(progress.isStopRequested());
  host_->requestStopActiveIngests();
  EXPECT_TRUE(progress.isStopRequested());
  EXPECT_FALSE(progress.progressUpdate(1));  // cooperative-cancel signal

  ASSERT_TRUE((*runtime_or).releaseParserIngest(source.id).has_value());
  host_.reset();
}

// The source-promotion service's ownership authority: hasIngestForDataset
// answers "did THIS host's plugin create a parser-ingest context for the
// dataset" — and must keep answering true after terminal delivery AND after
// release, because promotion happens once the download has finished (the
// progress bookkeeping is retained until host teardown by design).
TEST_F(ToolboxRuntimeHostTest, HasIngestForDatasetCoversLiveAndCompletedIngests) {
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(PJ::ToolboxRuntimeHost::Callbacks{}, ingest));
  const auto ds_id = static_cast<PJ::DatasetId>(ingest.source_id);
  EXPECT_TRUE(host_->hasIngestForDataset(ds_id)) << "live ingest context";
  EXPECT_FALSE(host_->hasIngestForDataset(ds_id + 1)) << "dataset without an ingest";

  ingest.view->progressFinish();
  QCoreApplication::processEvents();  // deliver the queued terminal
  EXPECT_TRUE(host_->hasIngestForDataset(ds_id)) << "after terminal delivery";

  ASSERT_TRUE(ingest.runtime->releaseParserIngest(ingest.source_id).has_value());
  QCoreApplication::processEvents();
  EXPECT_TRUE(host_->hasIngestForDataset(ds_id)) << "after release — promotion runs post-download";
  host_.reset();  // context references test-body locals (catalog)
}

// The fold predicate must be true only while an ingest is LIVE: unlike
// hasIngestForDataset (which stays true after release so source promotion can
// ask later), a released ingest must read idle or a panel would be busy forever
// after its first import.
TEST_F(ToolboxRuntimeHostTest, HasActiveIngestsTracksLiveContextsOnly) {
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(buildHermeticHost(PJ::ToolboxRuntimeHost::Callbacks{}, ingest));
  EXPECT_FALSE(host_->hasActiveIngests()) << "no context created yet";

  ASSERT_NO_FATAL_FAILURE(createIngestContext(ingest));
  EXPECT_TRUE(host_->hasActiveIngests()) << "live context";

  ASSERT_TRUE(ingest.runtime->releaseParserIngest(ingest.source_id).has_value());
  QCoreApplication::processEvents();
  EXPECT_FALSE(host_->hasActiveIngests()) << "a released context is no longer work in flight";
  EXPECT_TRUE(host_->hasIngestForDataset(static_cast<PJ::DatasetId>(ingest.source_id)))
      << "retained per-dataset bookkeeping intact";
  host_.reset();  // context references test-body locals (catalog)
}

// progressFinish without a started sequence is the SDK finite-import pattern's
// safe no-op — it must not emit an unpaired on_ingest_finished (which would
// disturb the shell's started/finished bookkeeping for other imports). And the
// [stream-thread] progress hooks must marshal their callbacks to the
// constructing thread, exactly like report_message/notify_data_changed.
TEST_F(ToolboxRuntimeHostTest, ProgressCallbacksArePairedAndMarshalled) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog_box.service;

  std::atomic<int> started_calls{0};
  std::atomic<int> finished_calls{0};
  std::thread::id callback_thread;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) {
    callback_thread = std::this_thread::get_id();
    ++started_calls;
  };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  const auto source = *(*toolbox_or).createDataSource("cloud download");
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(
      (*runtime_or).raw().vtable->create_parser_ingest((*runtime_or).raw().ctx, source.id, &ingest_raw, &error));
  const PJ::DataSourceRuntimeHostView progress(ingest_raw);

  // Finish with no active sequence: swallowed, no unpaired callback.
  progress.progressFinish();
  QCoreApplication::processEvents();
  EXPECT_EQ(finished_calls.load(), 0);

  // Cross-thread start: queued, delivered on the constructing thread only.
  const auto host_thread = std::this_thread::get_id();
  // (Plain call: gtest ASSERTs are not usable off the test thread; the
  // started-count checks below prove the call succeeded.)
  std::thread worker([&]() { (void)progress.progressStart("import", 0, true); });
  worker.join();
  EXPECT_EQ(started_calls.load(), 0);
  QCoreApplication::processEvents();
  EXPECT_EQ(started_calls.load(), 1);
  EXPECT_EQ(callback_thread, host_thread);

  progress.progressFinish();
  QCoreApplication::processEvents();
  EXPECT_EQ(finished_calls.load(), 1);  // paired now that a sequence ran

  ASSERT_TRUE((*runtime_or).releaseParserIngest(source.id).has_value());
  QCoreApplication::processEvents();
  EXPECT_EQ(finished_calls.load(), 1);  // release after a clean finish adds nothing
  host_.reset();
}

// An OFF-THREAD release queues on_ingest_finished to the constructing thread;
// destroying the host before that metacall is pumped PURGES it (queued calls
// die with marshaller_). The teardown sweep must still deliver the terminal —
// exactly once — or the shell's hoisted lifecycle bookkeeping
// (SessionManager::hasActiveIngests) wedges true forever and the residual
// stop-routing entry holds an expired owner.
TEST_F(ToolboxRuntimeHostTest, OffThreadReleaseThenImmediateTeardownStillFiresFinishedOnce) {
  std::atomic<int> finished_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest));

  // [thread-safe] release from a worker: finished is QUEUED, not delivered.
  std::thread worker([&]() { (void)ingest.runtime->releaseParserIngest(ingest.source_id); });
  worker.join();
  EXPECT_EQ(finished_calls.load(), 0);

  host_.reset();  // no pump before destruction: the queued metacall is purged
  EXPECT_EQ(finished_calls.load(), 1) << "teardown must deliver the queued-but-undelivered finished";
  QCoreApplication::processEvents();  // nothing residual may fire afterwards
  EXPECT_EQ(finished_calls.load(), 1);
}

// The complementary exactly-once half: a finished that WAS delivered before
// teardown must not be re-fired by the destructor's sweep.
TEST_F(ToolboxRuntimeHostTest, DeliveredFinishedIsNotRefiredByTeardown) {
  std::atomic<int> finished_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest));

  std::thread worker([&]() { (void)ingest.runtime->releaseParserIngest(ingest.source_id); });
  worker.join();
  QCoreApplication::processEvents();  // normal delivery
  EXPECT_EQ(finished_calls.load(), 1);

  host_.reset();
  EXPECT_EQ(finished_calls.load(), 1) << "the sweep must skip a DELIVERED terminal";
}

// The guarantee MainWindow's hoisted bookkeeping now leans on: destroying a
// host with an ACTIVE progress sequence (progress_start, no finish and no
// release) still pairs the started callback with a finished.
TEST_F(ToolboxRuntimeHostTest, TeardownWithActiveProgressFiresFinished) {
  std::atomic<int> started_calls{0};
  std::atomic<int> finished_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) { ++started_calls; };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest));
  ASSERT_EQ(started_calls.load(), 1);
  EXPECT_EQ(finished_calls.load(), 0);

  host_.reset();  // mid-import teardown: the sweep closes the pairing
  EXPECT_EQ(finished_calls.load(), 1);
}

// Collapse semantics pin: a released import whose finished is still queued,
// SUPERSEDED by a re-created-and-started context for the same dataset, must
// NOT fire that stale finish after the re-arm. SessionManager's bookkeeping
// is dataset-keyed (beginIngest RESTARTS the entry, endIngest erases it, a
// double end is a silent no-op): a superseded finish landing mid-import
// would erase the re-armed entry — progress UI hidden early, the real finish
// downgraded to a no-op double-end. Exactly one finish per dataset, at the
// LAST sequence's real terminal.
TEST_F(ToolboxRuntimeHostTest, RearmedImportSwallowsSupersededQueuedFinish) {
  std::atomic<int> started_calls{0};
  std::atomic<int> finished_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) { ++started_calls; };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest));

  // Off-thread release queues A's finished without delivering it.
  std::thread worker([&]() { (void)ingest.runtime->releaseParserIngest(ingest.source_id); });
  worker.join();
  EXPECT_EQ(finished_calls.load(), 0);

  // Re-create AND start the same dataset's import BEFORE the queued call pumps.
  PJ_data_source_runtime_host_t second_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(ingest.runtime->raw().vtable->create_parser_ingest(
      ingest.runtime->raw().ctx, ingest.source_id, &second_raw, &error));
  const PJ::DataSourceRuntimeHostView second(second_raw);
  ASSERT_TRUE(second.progressStart("import-b", 5, true).has_value());
  ASSERT_EQ(started_calls.load(), 2);

  QCoreApplication::processEvents();  // A's stale delivery must lose to the re-arm
  EXPECT_EQ(finished_calls.load(), 0) << "a superseded finish must not drain the re-armed import";

  host_.reset();  // the re-armed sequence's terminal closes the pairing
  EXPECT_EQ(finished_calls.load(), 1) << "exactly one finish for the dataset";
}

// The erase-after-delivery regression (Codex S2): re-create the context but
// do NOT start it, then let the PRIOR sequence's queued finish deliver. The
// live re-created context must stay tracked — its later start/release must
// pair started/finished exactly — and teardown must add nothing.
TEST_F(ToolboxRuntimeHostTest, RecreatedContextStaysTrackedAfterPriorTerminalDelivers) {
  std::atomic<int> started_calls{0};
  std::atomic<int> finished_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) { ++started_calls; };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) { ++finished_calls; };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest));

  std::thread worker([&]() { (void)ingest.runtime->releaseParserIngest(ingest.source_id); });
  worker.join();

  // Re-create the context for the same dataset WITHOUT starting it, then let
  // the prior sequence's queued finish deliver normally.
  PJ_data_source_runtime_host_t second_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(ingest.runtime->raw().vtable->create_parser_ingest(
      ingest.runtime->raw().ctx, ingest.source_id, &second_raw, &error));
  const PJ::DataSourceRuntimeHostView second(second_raw);
  QCoreApplication::processEvents();
  EXPECT_EQ(finished_calls.load(), 1) << "the prior sequence's finish delivers normally";

  // The re-created context must still be tracked: its own sequence pairs.
  ASSERT_TRUE(second.progressStart("import-b", 5, true).has_value());
  ASSERT_EQ(started_calls.load(), 2);
  std::thread worker2([&]() { (void)ingest.runtime->releaseParserIngest(ingest.source_id); });
  worker2.join();
  QCoreApplication::processEvents();
  EXPECT_EQ(finished_calls.load(), 2) << "the re-created context's release must still pair its finish";

  host_.reset();
  EXPECT_EQ(finished_calls.load(), 2) << "teardown adds nothing for delivered terminals";
}

// Final Codex round: a queued finish must carry its own sequence's identity.
// Two complete off-thread sequences (start + release each, nothing pumped in
// between) share the retained progress entry; the pump must deliver
// begin A, begin B, then exactly ONE finish — B's, AFTER begin B — so a
// dataset-keyed consumer (begin restarts the entry, finish erases it, double
// end is a no-op) ends DRAINED. Pre-fix, A's stale delivery consumed B's
// kFinishQueued between the two begins and B's real finish was discarded:
// order begin, finish, begin — the key wedged active forever.
TEST_F(ToolboxRuntimeHostTest, StaleQueuedFinishCannotStealNewerSequencesTerminal) {
  std::vector<std::string> order;  // host-thread callback order
  int keyed_active = 0;            // dataset-keyed mock: begin -> restart entry, finish -> erase
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) {
    order.emplace_back("begin");
    keyed_active = 1;
  };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) {
    order.emplace_back("finish");
    keyed_active = 0;
  };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest, /*start_progress=*/false));

  // Sequence A entirely off-thread: its begin AND finish are both queued.
  std::thread first_seq([&]() {
    (void)ingest.view->progressStart("import-a", 10, true);
    (void)ingest.runtime->releaseParserIngest(ingest.source_id);
  });
  first_seq.join();

  // Sequence B: re-create (a control call, no queued callbacks), then start +
  // release off-thread — its begin and finish queue BEHIND A's pair.
  PJ_data_source_runtime_host_t second_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(ingest.runtime->raw().vtable->create_parser_ingest(
      ingest.runtime->raw().ctx, ingest.source_id, &second_raw, &error));
  const PJ::DataSourceRuntimeHostView second(second_raw);
  std::thread second_seq([&]() {
    (void)second.progressStart("import-b", 5, true);
    (void)ingest.runtime->releaseParserIngest(ingest.source_id);
  });
  second_seq.join();
  ASSERT_TRUE(order.empty()) << "nothing may deliver before the pump";

  QCoreApplication::processEvents();
  const std::vector<std::string> expected{"begin", "begin", "finish"};
  EXPECT_EQ(order, expected) << "the single finish must be B's, delivered AFTER begin B";
  EXPECT_EQ(keyed_active, 0) << "the dataset-keyed tracking must drain";

  host_.reset();
  EXPECT_EQ(order.size(), 3u) << "teardown adds nothing — B's terminal was already delivered";
}

// Closing Codex round: release is a [thread-safe] slot, so it may legally run
// ON the marshaller thread while the sequence's begin — queued by an
// off-thread progress_start — is still in flight. The finish must ENQUEUE
// behind that begin, never deliver directly: finish-before-begin lands the
// end on an absent key (silent no-op) and the late begin then re-inserts an
// entry nothing will ever erase.
TEST_F(ToolboxRuntimeHostTest, GuiThreadReleaseFinishCannotOvertakeQueuedBegin) {
  std::vector<std::string> order;
  int keyed_active = 0;  // dataset-keyed mock: begin -> restart entry, finish -> erase
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) {
    order.emplace_back("begin");
    keyed_active = 1;
  };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) {
    order.emplace_back("finish");
    keyed_active = 0;
  };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest, /*start_progress=*/false));

  std::thread starter([&]() { (void)ingest.view->progressStart("import", 10, true); });
  starter.join();
  ASSERT_TRUE(order.empty()) << "the off-thread begin must still be queued";

  // GUI-thread release while the begin is still queued.
  ASSERT_TRUE(ingest.runtime->releaseParserIngest(ingest.source_id).has_value());

  QCoreApplication::processEvents();
  const std::vector<std::string> expected{"begin", "finish"};
  EXPECT_EQ(order, expected) << "the finish must enqueue BEHIND its own sequence's queued begin";
  EXPECT_EQ(keyed_active, 0) << "the dataset-keyed tracking must drain";
}

// Two-arm variant of the same hazard: sequence A fully off-thread (begin A
// queued, its finish queued then superseded), sequence B started off-thread
// (begin B queued) and released ON the GUI thread. Delivery must read
// begin A, begin B, finish B — pre-fix the direct finish landed first.
TEST_F(ToolboxRuntimeHostTest, GuiThreadReleaseTwoArmVariantKeepsBeginBeforeFinish) {
  std::vector<std::string> order;
  int keyed_active = 0;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_started = [&](PJ::DatasetId, std::string, uint64_t, bool) {
    order.emplace_back("begin");
    keyed_active = 1;
  };
  callbacks.on_ingest_finished = [&](PJ::DatasetId) {
    order.emplace_back("finish");
    keyed_active = 0;
  };
  StartedIngest ingest;
  ASSERT_NO_FATAL_FAILURE(startHermeticIngest(std::move(callbacks), ingest, /*start_progress=*/false));

  // Sequence A entirely off-thread: begin A and finish A both queue.
  std::thread first_seq([&]() {
    (void)ingest.view->progressStart("import-a", 10, true);
    (void)ingest.runtime->releaseParserIngest(ingest.source_id);
  });
  first_seq.join();

  // Sequence B: re-create, start off-thread (begin B queues; the re-arm
  // supersedes A's queued finish), then release ON the GUI thread.
  PJ_data_source_runtime_host_t second_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(ingest.runtime->raw().vtable->create_parser_ingest(
      ingest.runtime->raw().ctx, ingest.source_id, &second_raw, &error));
  const PJ::DataSourceRuntimeHostView second(second_raw);
  std::thread second_start([&]() { (void)second.progressStart("import-b", 5, true); });
  second_start.join();
  ASSERT_TRUE(order.empty()) << "both begins must still be queued";
  ASSERT_TRUE(ingest.runtime->releaseParserIngest(ingest.source_id).has_value());

  QCoreApplication::processEvents();
  const std::vector<std::string> expected{"begin", "begin", "finish"};
  EXPECT_EQ(order, expected) << "begin A, begin B, then B's finish — nothing may overtake a queued begin";
  EXPECT_EQ(keyed_active, 0) << "the dataset-keyed tracking must drain";

  host_.reset();
  EXPECT_EQ(order.size(), 3u) << "teardown adds nothing — B's terminal was already delivered";
}

// Rapid progress_update calls inside the throttle window must not stack
// flush+callback ticks — only the first (epoch-aged) tick fires.
TEST_F(ToolboxRuntimeHostTest, ProgressUpdateThrottleSuppressesRapidTicks) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog_box.service;

  int progress_calls = 0;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_ingest_progress = [&progress_calls](PJ::DatasetId, uint64_t, uint64_t) { ++progress_calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  host_->setFlushThrottleMs(60000);
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  const auto source = *(*toolbox_or).createDataSource("cloud download");
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  ASSERT_TRUE(
      (*runtime_or).raw().vtable->create_parser_ingest((*runtime_or).raw().ctx, source.id, &ingest_raw, &error));
  const PJ::DataSourceRuntimeHostView progress(ingest_raw);

  ASSERT_TRUE(progress.progressStart("import", 100, true).has_value());
  EXPECT_TRUE(progress.progressUpdate(1));  // epoch-aged last_flush: fires
  EXPECT_TRUE(progress.progressUpdate(2));  // inside the window: suppressed
  EXPECT_TRUE(progress.progressUpdate(3));
  EXPECT_EQ(progress_calls, 1);

  ASSERT_TRUE((*runtime_or).releaseParserIngest(source.id).has_value());
  host_.reset();
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestWithoutDepsFailsCleanly) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds = (*toolbox_or).createDataSource("x");
  ASSERT_TRUE(ds.has_value());
  auto ingest = (*runtime_or).createParserIngest(ds->id);
  ASSERT_FALSE(ingest.has_value());
  EXPECT_NE(ingest.error().find("not configured"), std::string::npos);
}

// A context the toolbox never released must still be flushed on host teardown
// so its rows aren't lost.
TEST_F(ToolboxRuntimeHostTest, ParserIngestUnreleasedContextFlushedOnTeardown) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds = (*toolbox_or).createDataSource("teardown flush");
  ASSERT_TRUE(ds.has_value());
  auto ingest_or = (*runtime_or).createParserIngest(ds->id);
  ASSERT_TRUE(ingest_or.has_value());
  auto binding = (*ingest_or)
                     .ensureParserBinding(
                         PJ::ParserBindingRequest{
                             .topic_name = "/scalar",
                             .parser_encoding = "runtime_host_object",
                             .type_name = "mock/scalar",
                             .schema = PJ::Span<const uint8_t>{},
                             .parser_config_json = "{}",
                         });
  ASSERT_TRUE(binding.has_value()) << binding.error();
  ASSERT_TRUE(
      (*ingest_or).pushMessage(*binding, PJ::Timestamp{5}, []() -> std::vector<uint8_t> { return {9}; }).has_value());
  const auto id = ds->id;
  host_.reset();  // NO release — teardown must flush
  EXPECT_EQ(totalRowCount(id), 1u);
}

// Contexts are per-dataset and independent: releasing one leaves the other
// usable, and each dataset ends up with its own rows.
TEST_F(ToolboxRuntimeHostTest, ParserIngestContextsArePerDataset) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds_a = (*toolbox_or).createDataSource("a");
  auto ds_b = (*toolbox_or).createDataSource("b");
  ASSERT_TRUE(ds_a.has_value());
  ASSERT_TRUE(ds_b.has_value());
  ASSERT_NE(ds_a->id, ds_b->id);
  auto ingest_a = (*runtime_or).createParserIngest(ds_a->id);
  auto ingest_b = (*runtime_or).createParserIngest(ds_b->id);
  ASSERT_TRUE(ingest_a.has_value());
  ASSERT_TRUE(ingest_b.has_value());
  auto bind_a = (*ingest_a).ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/a",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(bind_a.has_value());
  ASSERT_TRUE(
      (*ingest_a).pushMessage(*bind_a, PJ::Timestamp{1}, []() -> std::vector<uint8_t> { return {1}; }).has_value());
  // Releasing A leaves B's context usable.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds_a->id).has_value());
  auto bind_b = (*ingest_b).ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/b",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(bind_b.has_value()) << bind_b.error();
  ASSERT_TRUE(
      (*ingest_b).pushMessage(*bind_b, PJ::Timestamp{2}, []() -> std::vector<uint8_t> { return {2}; }).has_value());
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds_b->id).has_value());
  EXPECT_EQ(totalRowCount(ds_a->id), 1u);
  EXPECT_EQ(totalRowCount(ds_b->id), 1u);
  host_.reset();
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestUnknownDataSourceFails) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::test::HermeticCatalog catalog_box(plugin_file.absolutePath());
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ingest = (*runtime_or).createParserIngest(99999);
  ASSERT_FALSE(ingest.has_value());
  EXPECT_NE(ingest.error().find("not found"), std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  QCoreApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
