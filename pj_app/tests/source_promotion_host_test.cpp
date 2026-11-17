// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SourcePromotionHost, the host side of the SDK service "pj.source_promotion.v1". Driven through the C ABI
// (SourcePromotionHostView over host.raw(), or raw vtable calls where the view's fixed shape would mask the case)
// against the real FileLoader replace pipeline; the SDK's mock_file_source_plugin ingests through
// ExtensionCatalogService as in file_loader_test.cpp. The ownership predicate is a fixture stub (the production
// authority, ToolboxRuntimeHost::hasIngestForDataset, has its own pj_runtime test).

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "SourcePromotionHost.h"
#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "support/loader_test_support.h"
using namespace Qt::StringLiterals;

#ifndef PJ_MOCK_FILE_SOURCE_PLUGIN_PATH
#error "PJ_MOCK_FILE_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;

constexpr const char* kMockManifestId = "mock-file-source";

// One delivered result; `calls` pins exactly-once delivery.
struct PromotionResult {
  int calls = 0;
  bool ok = false;
  std::string message;
};

class SourcePromotionHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(data_dir_.isValid());

    // Stage a copy of the plugin instead of pointing at the build dir: the SDK
    // builds deliberately-broken sibling test plugins next to this one.
    const QString plugin_src = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    const QString plugin_dst = extensions_dir_.filePath(QFileInfo(plugin_src).fileName());
    ASSERT_TRUE(QFile::copy(plugin_src, plugin_dst)) << "could not stage " << plugin_src.toStdString();

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    ASSERT_FALSE(app_session_->extensionCatalog().findSourcesForExtension(u".mock"_s).empty())
        << "mock_file_source_plugin did not load from the staged extensions dir";

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());

    mock_path_ = makeMockFile(u"sensors.mock"_s);
  }

  [[nodiscard]] PJ::SessionManager& session() {
    return app_session_->sessionManager();
  }

  // The host under test, with the fixture-controlled ownership predicate.
  void makeHost(bool owns = true) {
    owns_result_ = owns;
    host_ = std::make_unique<PJ::SourcePromotionHost>(*loader_, session(), kProviderId, [this](PJ::DatasetId id) {
      owned_queries_.push_back(id);
      return owns_result_;
    });
  }

  [[nodiscard]] QString makeMockFile(const QString& name) {
    return pj_app_test::makeMockFile(data_dir_, name);
  }

  // Skip-dialog hints for the plain setup load.
  [[nodiscard]] PJ::LoadHints loadHints() {
    return pj_app_test::mockLoadHints();
  }

  [[nodiscard]] bool loadAndWait(const QString& path, const PJ::LoadHints& hints) {
    return pj_app_test::loadAndWait(*loader_, path, hints);
  }

  [[nodiscard]] bool load() {
    return loadAndWait(mock_path_, loadHints());
  }

  [[nodiscard]] PJ::DatasetId datasetNamed(const std::string& basename) {
    for (const PJ::DatasetId id : session().createReader().listDatasets()) {
      const PJ::DatasetInfo* info = session().dataEngine().getDataset(id);
      if (info != nullptr && info->source_name == basename) {
        return id;
      }
    }
    return 0;
  }

  [[nodiscard]] bool engineHasDataset(PJ::DatasetId dataset_id) {
    const auto ids = session().createReader().listDatasets();
    return std::find(ids.begin(), ids.end(), dataset_id) != ids.end();
  }

  [[nodiscard]] int64_t singleTopicRowCount(PJ::DatasetId dataset_id) {
    const PJ::DataReader reader = session().createReader();
    const auto topics = reader.listTopics(dataset_id);
    if (topics.size() != 1u) {
      return -1;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? static_cast<int64_t>(metadata->total_row_count) : -1;
  }

  // Loader fully idle + all queued terminals delivered — for the "no second
  // fire" assertions after a result was already observed.
  void settle() {
    (void)pumpUntil([this]() { return !loader_->isBusy(); });
    flushQueuedEvents();
  }

  [[nodiscard]] PJ::SourcePromotionRequest makeRequest(PJ::DatasetId dataset, const QString& artifact_path) {
    PJ::SourcePromotionRequest request;
    request.dataset = dataset;
    request.source_identity = "cloud:v1:sha256/aa";
    request.local_path_utf8 = artifact_path.toStdString();
    request.loader_plugin_id = kMockManifestId;
    request.loader_config_json = "{}";
    request.descriptor_json = R"({"v":1})";
    return request;
  }

  // view.promoteToFileSource with the result recorded into `result`.
  [[nodiscard]] PJ::Status promote(
      const PJ::SourcePromotionHostView& view, const PJ::SourcePromotionRequest& request, PromotionResult& result) {
    return view.promoteToFileSource(request, [&result](bool ok, std::string message) {
      ++result.calls;
      result.ok = ok;
      result.message = std::move(message);
    });
  }

  static constexpr auto kProviderId = "cloud-connector-provider";

  QTemporaryDir extensions_dir_;
  QTemporaryDir data_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
  std::unique_ptr<PJ::SourcePromotionHost> host_;
  QString mock_path_;
  bool owns_result_ = true;
  std::vector<PJ::DatasetId> owned_queries_;
};

// The full round trip: the promoted dataset is replaced through the stock loader, and the SourceRecord (provider_id
// host-derived from the binding, identity/descriptor from the request) is attached inside the pre-catalog commit
// seam, before catalog publication.
TEST_F(SourcePromotionHostTest, PromoteReplacesDatasetAndAttachesRecordAtCommitSeam) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  ASSERT_EQ(singleTopicRowCount(dataset_id), 3);
  makeHost();
  PJ::SourcePromotionHostView view(host_->raw());
  ASSERT_TRUE(view.valid());

  // Connected after the host (same-signal slots run in connection order), so it sees the host's attach at the seam,
  // before the catalog rebuild that publishes the load.
  bool record_present_at_seam = false;
  QString provider_at_seam;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::loadCommitting, loader_.get(),
      [&](quint64, const QVector<PJ::DatasetId>& produced, const QString&, const QString&) {
        if (!produced.contains(dataset_id)) {
          return;
        }
        const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
        record_present_at_seam = record != nullptr;
        if (record != nullptr) {
          provider_at_seam = record->provider_id;
        }
      });

  PromotionResult result;
  const auto status = promote(view, makeRequest(dataset_id, makeMockFile(u"artifact.mock"_s)), result);
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(result.calls, 0) << "accepted means queued — the transaction cannot have resolved in-call";
  ASSERT_TRUE(pumpUntil([&]() { return result.calls > 0; }));
  settle();

  EXPECT_EQ(result.calls, 1);
  EXPECT_TRUE(result.ok) << result.message;
  EXPECT_TRUE(record_present_at_seam) << "the record must exist already at the commit seam";
  EXPECT_EQ(provider_at_seam, kProviderId);
  const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, kProviderId) << "provider identity is host-derived, never from the request";
  EXPECT_EQ(record->source_identity, u"cloud:v1:sha256/aa"_s);
  EXPECT_EQ(record->descriptor_json, uR"({"v":1})"_s);
  EXPECT_TRUE(engineHasDataset(dataset_id)) << "strict replacement keeps the DatasetId";
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "the artifact's rows replaced the dataset in place";
  ASSERT_FALSE(owned_queries_.empty()) << "ownership must have been consulted";
  EXPECT_EQ(owned_queries_.front(), dataset_id);
}

// A loader manifest id that matches no installed plugin: accepted (the request is structurally fine), then the
// strict load fails: ok=false exactly once, no record, data untouched.
TEST_F(SourcePromotionHostTest, UnknownLoaderPluginFailsPromotionWithoutTouchingDataset) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost();
  PJ::SourcePromotionHostView view(host_->raw());

  auto request = makeRequest(dataset_id, makeMockFile(u"artifact.mock"_s));
  request.loader_plugin_id = "no-such-loader-plugin";
  PromotionResult result;
  const auto status = promote(view, request, result);
  ASSERT_TRUE(status) << status.error();
  ASSERT_TRUE(pumpUntil([&]() { return result.calls > 0; }));
  settle();

  EXPECT_EQ(result.calls, 1) << "exactly once";
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("no-such-loader-plugin"), std::string::npos)
      << "the loader-resolution failure must be surfaced, got: " << result.message;
  EXPECT_EQ(session().sourceRecord(dataset_id), nullptr) << "no record on a failed promotion";
  EXPECT_TRUE(engineHasDataset(dataset_id));
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3) << "the target's data must be untouched";
}

// Promoting a dataset that does not exist. Ownership is stubbed true: liveness is the strict-replacement check's
// job, and its diagnostic must reach the plugin through the result message.
TEST_F(SourcePromotionHostTest, VanishedDatasetFailsWithStrictReplacementDiagnostic) {
  makeHost(/*owns=*/true);
  PJ::SourcePromotionHostView view(host_->raw());

  PromotionResult result;
  const auto status = promote(view, makeRequest(9999, makeMockFile(u"artifact.mock"_s)), result);
  ASSERT_TRUE(status) << status.error();
  ASSERT_TRUE(pumpUntil([&]() { return result.calls > 0; }));
  settle();

  EXPECT_EQ(result.calls, 1);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("9999"), std::string::npos)
      << "the strict-replacement failure must name the vanished target, got: " << result.message;
  EXPECT_NE(result.message.find("no longer exists"), std::string::npos) << "got: " << result.message;
}

// The bound provider's ingest did not produce the dataset: accepted, then failed, and FileLoader is never engaged.
TEST_F(SourcePromotionHostTest, NotOwnedDatasetIsRejectedWithoutEngagingTheLoader) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost(/*owns=*/false);
  PJ::SourcePromotionHostView view(host_->raw());

  int started = 0;
  QObject::connect(
      loader_.get(), &PJ::FileLoader::requestStarted, loader_.get(), [&](quint64, std::uint64_t) { ++started; });

  PromotionResult result;
  const auto status = promote(view, makeRequest(dataset_id, makeMockFile(u"artifact.mock"_s)), result);
  ASSERT_TRUE(status) << status.error();
  ASSERT_TRUE(pumpUntil([&]() { return result.calls > 0; }));
  settle();

  EXPECT_EQ(result.calls, 1);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(started, 0) << "an unowned dataset must never reach the FileLoader";
  EXPECT_EQ(session().sourceRecord(dataset_id), nullptr);
  EXPECT_EQ(singleTopicRowCount(dataset_id), 3);
}

// Structurally invalid requests are synchronous rejects: promote_to_file_source returns false and result_cb never
// runs. Driven through the raw vtable (the C++ view cannot produce an undersized struct_size).
TEST_F(SourcePromotionHostTest, StructurallyInvalidRequestsRejectSynchronously) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost();
  const PJ_source_promotion_host_t raw = host_->raw();

  int calls = 0;
  const auto count_cb = +[](void* ctx, bool, PJ_string_view_t) noexcept { ++*static_cast<int*>(ctx); };
  const auto sv = [](const std::string& s) { return PJ_string_view_t{s.data(), s.size()}; };
  const std::string identity = "cloud:v1:sha256/aa";
  const std::string path = makeMockFile(u"artifact.mock"_s).toStdString();
  const std::string empty_path;
  const std::string loader_id = kMockManifestId;
  const std::string config = "{}";
  const std::string descriptor = R"({"v":1})";
  const auto make_raw = [&](PJ::DatasetId dataset, const std::string& p) {
    PJ_source_promotion_request_v1_t request{};
    request.struct_size = sizeof(request);
    request.dataset = PJ_data_source_handle_t{dataset};
    request.source_identity = sv(identity);
    request.local_path_utf8 = sv(p);
    request.loader_plugin_id = sv(loader_id);
    request.loader_config_json = sv(config);
    request.descriptor_json = sv(descriptor);
    return request;
  };

  PJ_error_t error{};
  // Null request.
  EXPECT_FALSE(raw.vtable->promote_to_file_source(raw.ctx, nullptr, count_cb, &calls, &error));
  // Null result_cb on a valid request: an accepted call promises exactly-once delivery, unfulfillable, so it must
  // reject synchronously.
  auto valid = make_raw(dataset_id, path);
  EXPECT_FALSE(raw.vtable->promote_to_file_source(raw.ctx, &valid, nullptr, &calls, &error));
  // Undersized struct_size (v1 fields not covered).
  auto undersized = make_raw(dataset_id, path);
  undersized.struct_size = sizeof(uint32_t) + sizeof(PJ_data_source_handle_t);
  EXPECT_FALSE(raw.vtable->promote_to_file_source(raw.ctx, &undersized, count_cb, &calls, &error));
  // Zero dataset handle (out_error may legally be NULL — tolerate it).
  auto no_dataset = make_raw(0, path);
  EXPECT_FALSE(raw.vtable->promote_to_file_source(raw.ctx, &no_dataset, count_cb, &calls, nullptr));
  // Empty local path.
  auto no_path = make_raw(dataset_id, empty_path);
  EXPECT_FALSE(raw.vtable->promote_to_file_source(raw.ctx, &no_path, count_cb, &calls, &error));

  settle();
  EXPECT_EQ(calls, 0) << "a synchronous reject must never run result_cb";
  EXPECT_EQ(session().sourceRecord(dataset_id), nullptr);
}

// result_cb may re-enter promote_to_file_source (the ABI allows it): a second valid promotion issued from inside
// the first result completes, with no deadlock and each result exactly once.
TEST_F(SourcePromotionHostTest, ReentrantPromotionFromResultCallbackCompletes) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost();
  PJ::SourcePromotionHostView view(host_->raw());

  const QString second_artifact = makeMockFile(u"artifact2.mock"_s);
  PromotionResult first;
  PromotionResult second;
  const auto status = view.promoteToFileSource(
      makeRequest(dataset_id, makeMockFile(u"artifact.mock"_s)), [&](bool ok, std::string message) {
        ++first.calls;
        first.ok = ok;
        first.message = std::move(message);
        auto second_request = makeRequest(dataset_id, second_artifact);
        second_request.source_identity = "cloud:v1:sha256/second";
        const auto second_status = promote(view, second_request, second);
        EXPECT_TRUE(second_status) << second_status.error();
      });
  ASSERT_TRUE(status) << status.error();
  ASSERT_TRUE(pumpUntil([&]() { return second.calls > 0; }));
  settle();

  EXPECT_EQ(first.calls, 1);
  EXPECT_TRUE(first.ok) << first.message;
  EXPECT_EQ(second.calls, 1);
  EXPECT_TRUE(second.ok) << second.message;
  const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->source_identity, u"cloud:v1:sha256/second"_s) << "the second promotion's record wins";
}

// promote_to_file_source is thread-safe: called from a plugin worker thread it copies the entire request before
// returning (the caller scrubs and frees its buffers right after), then completes on the GUI loop.
TEST_F(SourcePromotionHostTest, OffThreadPromotionCopiesTheRequestBeforeReturning) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost();
  const PJ_source_promotion_host_t raw = host_->raw();

  struct ResultCapture {
    std::atomic<int> calls{0};
    std::atomic<bool> ok{false};
  };
  ResultCapture capture;
  const auto result_cb = +[](void* ctx, bool ok, PJ_string_view_t) noexcept {
    auto* c = static_cast<ResultCapture*>(ctx);
    c->ok.store(ok);
    c->calls.fetch_add(1);
  };

  const QString artifact = makeMockFile(u"artifact.mock"_s);
  std::atomic<bool> accepted{false};
  std::thread caller([&]() {
    std::string identity = "cloud:v1:sha256/off-thread";
    std::string path = artifact.toStdString();
    std::string loader_id = kMockManifestId;
    std::string config = "{}";
    std::string descriptor = R"({"v":7})";
    PJ_source_promotion_request_v1_t request{};
    request.struct_size = sizeof(request);
    request.dataset = PJ_data_source_handle_t{dataset_id};
    request.source_identity = PJ_string_view_t{identity.data(), identity.size()};
    request.local_path_utf8 = PJ_string_view_t{path.data(), path.size()};
    request.loader_plugin_id = PJ_string_view_t{loader_id.data(), loader_id.size()};
    request.loader_config_json = PJ_string_view_t{config.data(), config.size()};
    request.descriptor_json = PJ_string_view_t{descriptor.data(), descriptor.size()};
    PJ_error_t error{};
    accepted.store(raw.vtable->promote_to_file_source(raw.ctx, &request, result_cb, &capture, &error));
    // Scrub every caller buffer: the host must already hold its own copy.
    identity.assign(identity.size(), 'X');
    path.assign(path.size(), 'X');
    loader_id.assign(loader_id.size(), 'X');
    config.assign(config.size(), 'X');
    descriptor.assign(descriptor.size(), 'X');
    request = PJ_source_promotion_request_v1_t{};
  });
  caller.join();
  ASSERT_TRUE(accepted.load());
  EXPECT_EQ(capture.calls.load(), 0) << "nothing can have completed before the GUI loop is pumped";

  ASSERT_TRUE(pumpUntil([&]() { return capture.calls.load() > 0; }));
  settle();

  EXPECT_EQ(capture.calls.load(), 1);
  EXPECT_TRUE(capture.ok.load());
  const PJ::SourceRecord* record = session().sourceRecord(dataset_id);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provider_id, kProviderId);
  EXPECT_EQ(record->source_identity, u"cloud:v1:sha256/off-thread"_s) << "attached from the host's own copy";
  EXPECT_EQ(record->descriptor_json, uR"({"v":7})"_s);
}

// An accepted promotion whose load is in flight is failed (ok=false) by the destructor, exactly once and while the
// loader is still alive; the underlying load completes as an ordinary replace with no record attach.
TEST_F(SourcePromotionHostTest, TeardownFailsAcceptedPromotionExactlyOnce) {
  ASSERT_TRUE(load());
  flushQueuedEvents();
  const PJ::DatasetId dataset_id = datasetNamed("sensors.mock");
  ASSERT_NE(dataset_id, 0u);
  makeHost();
  PJ::SourcePromotionHostView view(host_->raw());

  // Exit the loop the moment the promotion engages the loader: exec() returns after the current (queued
  // processPromotion) dispatch, so the load's terminal, always delivered through a later queued event, cannot have
  // arrived yet.
  int started = 0;
  QEventLoop until_started;
  QObject::connect(loader_.get(), &PJ::FileLoader::requestStarted, &until_started, [&](quint64, std::uint64_t) {
    ++started;
    until_started.quit();
  });

  PromotionResult result;
  const auto status = promote(view, makeRequest(dataset_id, makeMockFile(u"artifact.mock"_s)), result);
  ASSERT_TRUE(status) << status.error();
  QTimer::singleShot(10000, &until_started, &QEventLoop::quit);  // safety: fail, don't hang CI
  until_started.exec();
  ASSERT_GT(started, 0) << "the promotion never engaged the loader";
  ASSERT_EQ(result.calls, 0);

  host_.reset();
  EXPECT_EQ(result.calls, 1) << "teardown must fail the accepted promotion";
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("shutting down"), std::string::npos) << "got: " << result.message;

  settle();
  EXPECT_EQ(result.calls, 1) << "the orphaned load's terminal must not re-fire the result";
  EXPECT_EQ(session().sourceRecord(dataset_id), nullptr) << "no listener at the seam — no record attached";
  EXPECT_TRUE(engineHasDataset(dataset_id)) << "the orphaned load still completed as an ordinary replace";
}

// The two-phase teardown contract: after shutdown() the host stays safely callable (live mutex, closed intake):
// every promote_to_file_source either sync-rejects or, if accepted before the intake closed, fails its callback
// exactly once; never an ok result, a lost callback or a use-after-free. Interactive teardown relies on this
// window: shutdown() runs before the plugin handle is destroyed (which joins the provider's workers) and the
// host dies only afterwards, so straggler workers land here and not on a freed object. A worker thread
// hammering across the transition models them.
TEST_F(SourcePromotionHostTest, TwoPhaseShutdownSafelyRejectsConcurrentPromotes) {
  makeHost();
  const PJ::SourcePromotionHostView view(host_->raw());

  std::atomic<bool> stop{false};
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<int> ok_callbacks{0};
  std::atomic<int> failed_callbacks{0};
  std::thread hammer([&]() {
    while (!stop.load()) {
      PJ::SourcePromotionRequest request;
      request.dataset = 1;
      request.source_identity = "fake:v1:sha256/aa";
      request.local_path_utf8 = "/nonexistent/artifact.mock";
      request.loader_plugin_id = "Mock File Source";
      request.loader_config_json = "{}";
      request.descriptor_json = "{}";
      const auto status = view.promoteToFileSource(
          request, [&](bool ok, std::string) { (ok ? ok_callbacks : failed_callbacks).fetch_add(1); });
      (status.has_value() ? accepted : rejected).fetch_add(1);
    }
  });

  QThread::msleep(20);  // let a batch of accepts land (nothing pumps, so they stay pending)
  host_->shutdown();    // phase 1: intake closed + pending failed — the object stays alive
  QThread::msleep(20);  // stragglers keep hammering the live-but-shut host
  stop.store(true);
  hammer.join();
  host_->shutdown();  // idempotent

  // Queued processPromotion metacalls for swept promotions must be no-ops.
  flushQueuedEvents();
  host_.reset();

  EXPECT_GT(accepted.load(), 0) << "the pre-shutdown phase should accept";
  EXPECT_GT(rejected.load(), 0) << "the post-shutdown phase must sync-reject";
  EXPECT_EQ(ok_callbacks.load(), 0);
  EXPECT_EQ(failed_callbacks.load(), accepted.load())
      << "every accepted promotion must fail its callback exactly once across shutdown";
}

}  // namespace

PJ_APP_TEST_MAIN("source_promotion_host_test")
