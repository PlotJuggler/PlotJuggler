// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for HeadlessDescriptorProviderSession — the dialog-free per-batch
// owner of ONE descriptor-import provider toolbox instance (T5b). The
// provider is a FAKE toolbox registered statically through the plugin
// catalog (registerStaticToolbox — same precedent as
// plugin_runtime_catalog_test.cpp), whose start_import spawns a real worker
// thread driving on_dataset/on_terminal through the C ABI, and whose
// PJ_joinable_job_t records cancel/join/destroy into a global event log so
// the QUIESCENCE ordering (cancel -> join -> job destroy -> plugin destroy)
// is assertable, not assumed. Modeled on source_promotion_host_test.cpp's
// scaffolding (AppSession + FileLoader over a temp extensions dir).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "FileLoader.h"
#include "HeadlessDescriptorProviderSession.h"
#include "dataset_test_helpers.h"
#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "support/fake_import_provider.h"
#include "support/loader_test_support.h"
using namespace Qt::StringLiterals;

namespace {

using pj_app_test::flushQueuedEvents;
using pj_app_test::pumpUntil;

constexpr uint32_t kFakeDatasetId = 7;
constexpr const char* kProviderId = "fake-import-provider";
constexpr const char* kNoExtensionId = "fake-import-no-extension";
constexpr const char* kUndersizedId = "fake-import-undersized";
constexpr const char* kBindFailsId = "fake-import-bind-fails";

// ---------------------------------------------------------------------------
// Global fake-side instrumentation. Tests run sequentially in one process;
// the mutex exists because the fake's worker threads and the job vtable
// calls append concurrently with the GUI thread.
// ---------------------------------------------------------------------------

struct FakeEventLog {
  std::mutex mu;
  std::vector<std::string> events;

  void add(std::string event) {
    const std::lock_guard<std::mutex> lock(mu);
    events.push_back(std::move(event));
  }
  [[nodiscard]] std::vector<std::string> snapshot() {
    const std::lock_guard<std::mutex> lock(mu);
    return events;
  }
  void clear() {
    const std::lock_guard<std::mutex> lock(mu);
    events.clear();
  }
};

FakeEventLog g_events;
std::atomic<int> g_created{0};
std::atomic<int> g_destroyed{0};
std::atomic<bool> g_promotion_service_found{false};
// The pj.data_processors.v1 service the bound instance resolved (captured at bind,
// GUI thread).
PJ_service_t g_data_processors_service{};

// Promotion-blocking instrumentation (the shutdown-before-join regression
// test). `g_worker_promotes` switches the fake worker into a provider that
// calls promote_to_file_source through the REGISTERED service and then
// blocks its terminal on the promotion's exactly-once result callback —
// cancelled or not, because the ABI has no way to cancel a promotion.
std::atomic<bool> g_worker_promotes{false};
PJ_service_t g_promotion_service{};  // captured at bind (GUI thread, before any worker starts)
std::atomic<bool> g_promotion_accepted{false};
std::atomic<bool> g_promotion_result_ok{true};
std::binary_semaphore g_promotion_result_gate{0};

void promotionResult(void* /*callback_ctx*/, bool ok, PJ_string_view_t /*message*/) noexcept {
  g_promotion_result_ok.store(ok);
  g_events.add(ok ? "promotion.result.ok" : "promotion.result.failed");
  g_promotion_result_gate.release();
}

[[nodiscard]] int firstIndexOf(const std::vector<std::string>& log, const std::string& event) {
  const auto it = std::find(log.begin(), log.end(), event);
  return it == log.end() ? -1 : static_cast<int>(it - log.begin());
}

[[nodiscard]] int countOf(const std::vector<std::string>& log, const std::string& event) {
  return static_cast<int>(std::count(log.begin(), log.end(), event));
}

// ---------------------------------------------------------------------------
// The fake provider toolbox (static vtables, no DSO). The job half is the
// SHARED pj_fake_import::FakeJob/kFakeJobVtable (its worker blocks on a
// start gate as its FIRST action, released by the test, by cancel, or by
// destroy); this test instruments it through the job hooks so the
// QUIESCENCE ordering stays assertable in g_events.
// ---------------------------------------------------------------------------

using FakeJob = pj_fake_import::FakeJob;
using pj_fake_import::kFakeJobVtable;
using pj_fake_import::sv;

enum class ExtensionMode { kFull, kAbsent, kUndersized };

struct FakeProviderInstance {
  ExtensionMode mode = ExtensionMode::kFull;
  PJ_descriptor_import_provider_v1_t ext{};
  bool fail_bind = false;
  // Covers last_job lookup+use AND the destroy-side unlink+delete, so a
  // releaseStart can never race the pointee's deletion.
  std::mutex job_mu;
  FakeJob* last_job = nullptr;  // non-owning; owned by the returned out_job->ctx

  void releaseStart() {
    const std::lock_guard<std::mutex> lock(job_mu);
    if (last_job != nullptr) {
      last_job->releaseStartOnce();
    }
  }
};

// Most recently created instance — how the test reaches releaseStart().
// Only touched on the GUI thread (create/destroy are [main-thread]).
FakeProviderInstance* g_instance = nullptr;

bool fakeQueryDescriptor(
    void* /*plugin_ctx*/, PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out,
    PJ_error_t* /*out_error*/) noexcept {
  // The only caller is the session's DescriptorImportProviderView, which
  // always passes a full-size v1 struct — no growth-contract gymnastics here
  // (the SDK's own extension test pins that contract).
  static const std::string identity = "fake:v1:sha256/aa";
  static const std::string path = "/tmp/fake-artifact.mcap";
  out->trust = descriptor_json.size > 0 ? PJ_DESCRIPTOR_TRUST_TRUSTED : PJ_DESCRIPTOR_TRUST_REFUSED;
  out->is_materialized = 0;
  out->source_identity = PJ_string_view_t{identity.data(), identity.size()};
  out->local_path_utf8 = PJ_string_view_t{path.data(), path.size()};
  out->estimated_bytes = 777;
  return true;
}

bool fakeStartImport(
    void* plugin_ctx, const PJ_descriptor_import_start_request_v1_t* request,
    const PJ_descriptor_import_callbacks_v1_t* callbacks, void* callback_ctx, PJ_joinable_job_t* out_job,
    PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(plugin_ctx);
  if (request == nullptr || callbacks == nullptr || callbacks->on_terminal == nullptr) {
    return false;
  }
  auto on_dataset = callbacks->on_dataset;
  auto on_terminal = callbacks->on_terminal;
  auto* job = new FakeJob();
  // The shared vtable's instrumentation hooks: the session-side call events
  // land in this test's g_events log, and destroy unlinks the owner's
  // last_job registry entry before deletion.
  job->on_event = [](const char* event) { g_events.add(event); };
  job->unlink = [self](FakeJob* dying) {
    const std::lock_guard<std::mutex> lock(self->job_mu);
    if (self->last_job == dying) {
      self->last_job = nullptr;
    }
  };
  // Fill out_job BEFORE spawning the worker: no job callback may occur
  // before start_import returns, and the worker must not race the caller's
  // read of *out_job.
  out_job->ctx = job;
  out_job->vtable = &kFakeJobVtable;
  {
    const std::lock_guard<std::mutex> lock(self->job_mu);
    self->last_job = job;
  }
  job->worker = std::thread([job, on_dataset, on_terminal, callback_ctx] {
    job->start_gate.acquire();
    if (g_worker_promotes.load()) {
      // Models a provider whose worker CANNOT abandon an accepted promotion:
      // the terminal blocks on the promotion's result callback, cancel or no
      // cancel. The promotion is accepted through the real registered
      // service; the test never settles it — only the host can.
      const auto* vtable = static_cast<const PJ_source_promotion_host_vtable_t*>(g_promotion_service.vtable);
      static const std::string identity = "fake:v1:sha256/aa";
      static const std::string path = "/tmp/fake-artifact.mcap";
      PJ_source_promotion_request_v1_t promotion_request{};
      promotion_request.struct_size = sizeof(promotion_request);
      promotion_request.dataset = PJ_data_source_handle_t{kFakeDatasetId};
      promotion_request.source_identity = PJ_string_view_t{identity.data(), identity.size()};
      promotion_request.local_path_utf8 = PJ_string_view_t{path.data(), path.size()};
      promotion_request.loader_plugin_id = sv("fake-loader");
      promotion_request.loader_config_json = sv("{}");
      promotion_request.descriptor_json = sv(R"({"v":1})");
      PJ_error_t error{};
      if (vtable == nullptr || !vtable->promote_to_file_source(
                                   g_promotion_service.ctx, &promotion_request, &promotionResult, nullptr, &error)) {
        g_events.add("worker.promotion.rejected");
        on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("promotion rejected"));
        return;
      }
      g_events.add("worker.promotion.accepted");
      g_promotion_accepted.store(true);
      // THE BLOCKING EDGE UNDER TEST: an accepted promotion promises
      // result_cb exactly once — wait for it before delivering the terminal.
      g_promotion_result_gate.acquire();
      g_events.add("worker.terminal.after-promotion");
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("promotion settled at teardown"));
      return;
    }
    if (job->cancelled.load()) {
      g_events.add("worker.terminal.cancelled");
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
      return;
    }
    if (on_dataset != nullptr) {
      on_dataset(callback_ctx, PJ_data_source_handle_t{kFakeDatasetId});
    }
    g_events.add("worker.terminal.done");
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY, sv("done"));
  });
  return true;
}

FakeProviderInstance* makeInstance(ExtensionMode mode, bool fail_bind = false) {
  auto* instance = new FakeProviderInstance();
  instance->mode = mode;
  instance->fail_bind = fail_bind;
  instance->ext.struct_size = mode == ExtensionMode::kUndersized
                                  ? static_cast<uint32_t>(offsetof(PJ_descriptor_import_provider_v1_t, start_import))
                                  : static_cast<uint32_t>(sizeof(PJ_descriptor_import_provider_v1_t));
  instance->ext.query_descriptor = &fakeQueryDescriptor;
  instance->ext.start_import = &fakeStartImport;
  g_created.fetch_add(1);
  g_instance = instance;
  return instance;
}

void* fakeCreateFull() noexcept {
  return makeInstance(ExtensionMode::kFull);
}
void* fakeCreateNoExtension() noexcept {
  return makeInstance(ExtensionMode::kAbsent);
}
void* fakeCreateUndersized() noexcept {
  return makeInstance(ExtensionMode::kUndersized);
}
void* fakeCreateBindFails() noexcept {
  return makeInstance(ExtensionMode::kFull, /*fail_bind=*/true);
}

void fakeDestroyToolbox(void* ctx) noexcept {
  g_events.add("plugin.destroy");
  g_destroyed.fetch_add(1);
  if (g_instance == ctx) {
    g_instance = nullptr;
  }
  delete static_cast<FakeProviderInstance*>(ctx);
}

uint64_t fakeCapabilities(void* /*ctx*/) noexcept {
  return 0;
}

bool fakeBind(void* ctx, PJ_service_registry_t registry, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(ctx);
  if (self->fail_bind) {
    if (out_error != nullptr) {
      out_error->code = 1;
      std::snprintf(out_error->message, sizeof(out_error->message), "fake bind rejection");
    }
    return false;
  }
  // The plugin-side probe test 1 relies on: can the BOUND instance resolve
  // the per-instance promotion service from the registry it was handed?
  PJ_service_t service{};
  PJ_error_t error{};
  const bool found =
      registry.vtable != nullptr &&
      registry.vtable->get_service(registry.ctx, sv(PJ_SOURCE_PROMOTION_HOST_SERVICE_V1), 1, &service, &error);
  g_promotion_service_found.store(found && service.ctx != nullptr && service.vtable != nullptr);
  if (g_promotion_service_found.load()) {
    // Kept for the promote-and-block worker; bind runs on the GUI thread
    // strictly before any worker exists, so no synchronization is needed
    // beyond the thread-creation happens-before.
    g_promotion_service = service;
  }
  PJ_service_t data_processors{};
  if (registry.vtable != nullptr &&
      registry.vtable->get_service(
          registry.ctx, sv(PJ::sdk::DataProcessorsHostService::kName), 1, &data_processors, &error)) {
    g_data_processors_service = data_processors;
  }
  g_events.add("plugin.bind");
  return true;
}

bool fakeSaveConfig(void* /*ctx*/, PJ_string_view_t* out_json, PJ_error_t* /*out_error*/) noexcept {
  static constexpr const char* kJson = "{}";
  if (out_json != nullptr) {
    out_json->data = kJson;
    out_json->size = 2;
  }
  return true;
}

bool fakeLoadConfig(void* /*ctx*/, PJ_string_view_t /*config_json*/, PJ_error_t* /*out_error*/) noexcept {
  return true;
}

PJ_borrowed_dialog_t fakeDialog(void* /*ctx*/) noexcept {
  return PJ_borrowed_dialog_t{nullptr, nullptr};
}

void fakeOnDataChanged(void* /*ctx*/) noexcept {}

const void* fakeGetExtension(void* ctx, PJ_string_view_t id) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(ctx);
  if (self->mode == ExtensionMode::kAbsent) {
    return nullptr;
  }
  const std::string requested(id.data == nullptr ? "" : id.data, id.size);
  return requested == PJ_DESCRIPTOR_IMPORT_EXTENSION_V1 ? &self->ext : nullptr;
}

[[nodiscard]] constexpr PJ_toolbox_vtable_t makeFakeToolboxVtable(
    const char* manifest_json, void* (*create_fn)() noexcept) {
  return PJ_toolbox_vtable_t{
      PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
      sizeof(PJ_toolbox_vtable_t),
      create_fn,
      &fakeDestroyToolbox,
      manifest_json,
      &fakeCapabilities,
      &fakeBind,
      &fakeSaveConfig,
      &fakeLoadConfig,
      &fakeDialog,
      &fakeOnDataChanged,
      &fakeGetExtension,
  };
}

// registerStaticToolbox requires static storage duration for the vtables.
const PJ_toolbox_vtable_t kFullVtable = makeFakeToolboxVtable(
    R"({"id":"fake-import-provider","name":"Fake Import Provider","version":"1.0.0"})", &fakeCreateFull);
const PJ_toolbox_vtable_t kNoExtensionVtable = makeFakeToolboxVtable(
    R"({"id":"fake-import-no-extension","name":"Fake No Extension","version":"1.0.0"})", &fakeCreateNoExtension);
const PJ_toolbox_vtable_t kUndersizedVtable = makeFakeToolboxVtable(
    R"({"id":"fake-import-undersized","name":"Fake Undersized","version":"1.0.0"})", &fakeCreateUndersized);
const PJ_toolbox_vtable_t kBindFailsVtable = makeFakeToolboxVtable(
    R"({"id":"fake-import-bind-fails","name":"Fake Bind Fails","version":"1.0.0"})", &fakeCreateBindFails);

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class HeadlessDescriptorProviderSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    auto& catalog = app_session_->extensionCatalog().pluginCatalog();
    ASSERT_TRUE(catalog.registerStaticToolbox(&kFullVtable));
    ASSERT_TRUE(catalog.registerStaticToolbox(&kNoExtensionVtable));
    ASSERT_TRUE(catalog.registerStaticToolbox(&kUndersizedVtable));
    ASSERT_TRUE(catalog.registerStaticToolbox(&kBindFailsVtable));

    loader_ = std::make_unique<PJ::FileLoader>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel());

    // Reset the fake-side instrumentation AFTER registration: the catalog's
    // registration path creates+destroys a probe instance per toolbox (to
    // read capabilities), and those probe events are not under test.
    g_events.clear();
    g_created.store(0);
    g_destroyed.store(0);
    g_promotion_service_found.store(false);
    g_worker_promotes.store(false);
    g_promotion_service = PJ_service_t{};
    g_data_processors_service = PJ_service_t{};
    g_promotion_accepted.store(false);
    g_promotion_result_ok.store(true);
    g_instance = nullptr;
  }

  [[nodiscard]] PJ::Expected<PJ::HeadlessDescriptorProviderSession::Ptr> makeSession(const char* manifest_id) {
    return PJ::HeadlessDescriptorProviderSession::create(
        app_session_->sessionManager(), app_session_->extensionCatalog(), *loader_, app_session_->catalogModel(),
        QString::fromUtf8(manifest_id),
        [this](const PJ::Diagnostic& diagnostic) { diagnostics_.push_back(diagnostic); });
  }

  QTemporaryDir extensions_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::FileLoader> loader_;
  std::vector<PJ::Diagnostic> diagnostics_;
};

// Scenario 4 first (the RED test for the absorbed PR-A follow-up): the
// session destructor is the layer that makes ToolboxRuntimeHost's
// "workers quiesced before host destruction" precondition TRUE. Start an
// import whose fake worker keeps running (blocked on its start gate — the
// test never releases it); destroy the session; the fake must observe, in
// order: job cancel -> worker terminal returned -> job join returned -> job
// destroy -> plugin instance destroy. And NO user callback may fire after
// destruction (the queued-but-undelivered terminal is purged by ~QObject —
// legal precisely because the job was cancelled+joined first).
TEST_F(HeadlessDescriptorProviderSessionTest, DestructionQuiescesJobsBeforeHostsAndHandle) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();

  std::atomic<int> dataset_calls{0};
  std::atomic<int> terminal_calls{0};
  PJ::DescriptorImportStartRequest request;
  request.descriptor_json = R"({"v":1})";
  const auto status = (*session)->startImport(
      request, [&](PJ::DatasetId) { dataset_calls.fetch_add(1); },
      [&](PJ::DescriptorImportOutcome, std::string) { terminal_calls.fetch_add(1); });
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ((*session)->activeJobCount(), 1u);

  // Worker still blocked on the start gate: destroy the session mid-import.
  session->reset();

  const auto log = g_events.snapshot();
  const int idx_cancel = firstIndexOf(log, "job.cancel");
  const int idx_worker_terminal = firstIndexOf(log, "worker.terminal.cancelled");
  const int idx_join = firstIndexOf(log, "job.join.returned");
  const int idx_job_destroy = firstIndexOf(log, "job.destroy");
  const int idx_plugin_destroy = firstIndexOf(log, "plugin.destroy");
  ASSERT_GE(idx_cancel, 0) << "the destructor must cancel the live job";
  ASSERT_GE(idx_worker_terminal, 0) << "the cancelled worker must have delivered its terminal";
  ASSERT_GE(idx_join, 0) << "the destructor must join the live job";
  ASSERT_GE(idx_job_destroy, 0) << "the destructor must destroy the live job";
  ASSERT_GE(idx_plugin_destroy, 0) << "releasing the handle must destroy the plugin instance";
  EXPECT_LT(idx_cancel, idx_join) << "cancel precedes join";
  EXPECT_LT(idx_worker_terminal, idx_join) << "join returns only after the worker's terminal returned";
  EXPECT_LT(idx_join, idx_job_destroy) << "the job is joined before it is destroyed";
  EXPECT_LT(idx_job_destroy, idx_plugin_destroy)
      << "the worker must be fully quiesced BEFORE the handle/runtime hosts die";
  EXPECT_EQ(g_destroyed.load(), 1);

  // No callback after destruction: the queued terminal metacall died with
  // the session QObject.
  flushQueuedEvents();
  EXPECT_EQ(dataset_calls.load(), 0);
  EXPECT_EQ(terminal_calls.load(), 0);
}

// Scenario 4b (the PROMOTION-SHUTDOWN DEADLOCK regression, found
// independently by the stage-4 design consult and the stage-4 adversarial
// review): a provider worker may legally block its terminal on an ACCEPTED
// promotion's exactly-once result callback — the ABI has no way to cancel a
// promotion. The destructor must therefore fail the promotion intake
// (SourcePromotionHost::shutdown()) BEFORE joining the jobs; with the old
// order (join first, shutdown after) teardown deadlocks: joinAll waits on a
// terminal that waits on a callback only shutdown() can deliver. The
// destructor runs on a helper thread with a 5 s bound so the pre-fix
// deadlock FAILS the test instead of hanging the suite.
TEST_F(HeadlessDescriptorProviderSessionTest, DestructionFailsAcceptedPromotionBeforeJoiningJobs) {
  g_worker_promotes.store(true);
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();
  ASSERT_NE(g_promotion_service.ctx, nullptr);
  ASSERT_NE(g_promotion_service.vtable, nullptr);

  std::atomic<int> terminal_calls{0};
  PJ::DescriptorImportStartRequest request;
  request.descriptor_json = R"({"v":1})";
  const auto status = (*session)->startImport(
      request, nullptr, [&](PJ::DescriptorImportOutcome, std::string) { terminal_calls.fetch_add(1); });
  ASSERT_TRUE(status) << status.error();

  ASSERT_NE(g_instance, nullptr);
  g_instance->releaseStart();
  // Wait for ACCEPTANCE without pumping the event loop: the queued
  // processPromotion metacall must stay undelivered so the promotion is
  // still pending when the destructor runs — only shutdown() can settle it.
  const auto acceptance_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!g_promotion_accepted.load() && std::chrono::steady_clock::now() < acceptance_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(g_promotion_accepted.load()) << "the promotion must be ACCEPTED through the registered service";

  // Destroy the session on a helper thread and BOUND the teardown.
  std::atomic<bool> dtor_done{false};
  std::thread dtor([&] {
    session->reset();
    dtor_done.store(true);
  });
  const auto bound = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!dtor_done.load() && std::chrono::steady_clock::now() < bound) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(dtor_done.load())
      << "teardown DEADLOCKED: the destructor must shutdown() the promotion intake BEFORE joinAll";
  if (!dtor_done.load()) {
    // Red-run rescue so the failing run reports instead of hanging the
    // suite: deliver the queued processPromotion (GUI thread) exactly once —
    // the ownership gate fails the promotion (ok=false), the worker
    // unblocks, the stuck join returns. Pump only ONCE: the worker's
    // re-posted terminal metacall must die with the session QObject, not
    // race its destruction on this thread.
    QCoreApplication::processEvents();
    while (!dtor_done.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  dtor.join();

  const auto log = g_events.snapshot();
  const int idx_cancel = firstIndexOf(log, "job.cancel");
  const int idx_result = firstIndexOf(log, "promotion.result.failed");
  const int idx_terminal = firstIndexOf(log, "worker.terminal.after-promotion");
  const int idx_join = firstIndexOf(log, "job.join.returned");
  const int idx_job_destroy = firstIndexOf(log, "job.destroy");
  const int idx_plugin_destroy = firstIndexOf(log, "plugin.destroy");
  ASSERT_GE(idx_cancel, 0) << "the destructor must cancel the live job";
  ASSERT_GE(idx_result, 0) << "shutdown() must FAIL (ok=false) the accepted-but-unfinished promotion";
  ASSERT_GE(idx_terminal, 0) << "the failed promotion must unblock the worker's terminal";
  ASSERT_GE(idx_join, 0);
  ASSERT_GE(idx_job_destroy, 0);
  ASSERT_GE(idx_plugin_destroy, 0);
  EXPECT_FALSE(g_promotion_result_ok.load()) << "a teardown-settled promotion reports ok=false";
  EXPECT_EQ(countOf(log, "promotion.result.failed") + countOf(log, "promotion.result.ok"), 1)
      << "the result callback runs exactly once";
  EXPECT_LT(idx_cancel, idx_result) << "cancel precedes the promotion-intake shutdown";
  EXPECT_LT(idx_result, idx_join) << "THE FIX: the promotion fails BEFORE join, never after";
  EXPECT_LT(idx_terminal, idx_join) << "join returns only after the unblocked terminal returned";
  // The existing QUIESCENCE pin must survive the reorder: every job event
  // still precedes the plugin instance's destruction.
  EXPECT_LT(idx_join, idx_job_destroy) << "the job is joined before it is destroyed";
  EXPECT_LT(idx_job_destroy, idx_plugin_destroy)
      << "the worker must be fully quiesced BEFORE the handle/runtime hosts die";
  EXPECT_EQ(g_destroyed.load(), 1);

  // No user callback after destruction: the queued terminal metacall died
  // with the session QObject.
  flushQueuedEvents();
  EXPECT_EQ(terminal_calls.load(), 0) << "no callback may fire after the destructor";
}

// Scenario 1: creation success — the session resolves the fake by STABLE
// MANIFEST ID, binds it, and the bound instance can reach the per-instance
// "pj.source_promotion.v1" service from the registry it was handed. The
// queryDescriptor passthrough round-trips the fake's values.
TEST_F(HeadlessDescriptorProviderSessionTest, CreationResolvesBindsAndRegistersPromotionService) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();
  EXPECT_EQ((*session)->providerId(), QString::fromUtf8(kProviderId));
  EXPECT_EQ(g_created.load(), 1);
  EXPECT_GE(firstIndexOf(g_events.snapshot(), "plugin.bind"), 0);
  EXPECT_TRUE(g_promotion_service_found.load()) << "the bound instance must be able to resolve pj.source_promotion.v1";

  const auto query = (*session)->queryDescriptor(R"({"v":1})");
  ASSERT_TRUE(query.has_value()) << query.error();
  EXPECT_EQ(query->trust, PJ::DescriptorTrust::kTrusted);
  EXPECT_EQ(query->source_identity, "fake:v1:sha256/aa");
  EXPECT_EQ(query->local_path_utf8, "/tmp/fake-artifact.mcap");
  EXPECT_EQ(query->estimated_bytes, 777u);

  session->reset();
  EXPECT_EQ(g_destroyed.load(), 1);
}

// The provider sees the derived surface an interactive toolbox sees: the one
// pj.data_processors.v1 service creates a kind=markers rule as well as a transform.
TEST_F(HeadlessDescriptorProviderSessionTest, ProviderCreatesAMarkerRule) {
  const PJ::DatasetId dataset = pj_test::createDataset(*app_session_, "headless");
  ASSERT_NE(pj_test::addScalarTopic(*app_session_, dataset, "/sensor"), 0U);
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();
  const auto* vtable = static_cast<const PJ_data_processors_host_vtable_t*>(g_data_processors_service.vtable);
  ASSERT_NE(vtable, nullptr) << "the bound instance must be able to resolve pj.data_processors.v1";

  const PJ_string_view_t input = sv("/sensor/value");
  const PJ_string_view_t output = sv("__global__");
  PJ_error_t error{};
  const bool created = vtable->create_data_processor(
      g_data_processors_service.ctx, sv("rule"), sv("markers"), sv("luau"), &input, 1, &output, 1,
      sv("local s = series(\"/sensor/value\")\ncreateMarker(s:at(0).t)\n"), sv("{}"), 0, nullptr, 0, nullptr, &error);
  EXPECT_TRUE(created) << error.message;
  EXPECT_NE(app_session_->sessionManager().batchEngine().firstBinding("fake-import-provider/rule"), nullptr);
}

// ABI-41: the routed service validates an on-demand script in a headless session, and
// rejects one that does not compile.
TEST_F(HeadlessDescriptorProviderSessionTest, DataProcessorsServiceIsRoutedByKind) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();
  const auto* vtable = static_cast<const PJ_data_processors_host_vtable_t*>(g_data_processors_service.vtable);
  ASSERT_NE(vtable, nullptr);
  PJ_error_t error{};
  EXPECT_TRUE(vtable->validate_data_processor_script(
      g_data_processors_service.ctx, sv("on_demand"), sv("luau"), sv("return {}"), sv("{}"), &error))
      << error.message;
  EXPECT_FALSE(vtable->validate_data_processor_script(
      g_data_processors_service.ctx, sv("on_demand"), sv("luau"), sv("return {"), sv("{}"), &error));
  EXPECT_FALSE(vtable->validate_data_processor_script(
      g_data_processors_service.ctx, sv("unknown"), sv("luau"), sv("return {}"), sv("{}"), &error));
}

// A preview belongs to the session that made it: ending the session ends the
// previews its provider created and keeps the rules it created persistent.
TEST_F(HeadlessDescriptorProviderSessionTest, TeardownEndsTheProvidersEphemeralPreviews) {
  const PJ::DatasetId dataset = pj_test::createDataset(*app_session_, "headless");
  ASSERT_NE(pj_test::addScalarTopic(*app_session_, dataset, "/sensor"), 0U);
  const PJ::BatchEngine& markers = app_session_->sessionManager().batchEngine();
  {
    auto session = makeSession(kProviderId);
    ASSERT_TRUE(session.has_value()) << session.error();
    const auto* vtable = static_cast<const PJ_data_processors_host_vtable_t*>(g_data_processors_service.vtable);
    ASSERT_NE(vtable, nullptr);
    const PJ_string_view_t input = sv("/sensor/value");
    const PJ_string_view_t output = sv("__global__");
    const std::string script = "local s = series(\"/sensor/value\")\ncreateMarker(s:at(0).t)\n";
    PJ_error_t error{};
    ASSERT_TRUE(vtable->create_data_processor(
        g_data_processors_service.ctx, sv("rule"), sv("markers"), sv("luau"), &input, 1, &output, 1, sv(script.c_str()),
        sv("{}"), 0, nullptr, 0, nullptr, &error))
        << error.message;
    ASSERT_TRUE(vtable->create_data_processor(
        g_data_processors_service.ctx, sv("preview"), sv("markers"), sv("luau"), &input, 1, nullptr, 0,
        sv(script.c_str()), sv("{}"), PJ_DATA_PROCESSOR_FLAG_EPHEMERAL, nullptr, 0, nullptr, &error))
        << error.message;
    ASSERT_NE(markers.firstBinding("fake-import-provider/preview"), nullptr);
  }
  EXPECT_EQ(markers.firstBinding("fake-import-provider/preview"), nullptr);
  EXPECT_NE(markers.firstBinding("fake-import-provider/rule"), nullptr);
}

// Scenario 2a: unknown manifest id — a distinct, precise failure, and the
// plugin is never even instantiated.
TEST_F(HeadlessDescriptorProviderSessionTest, CreationFailsForUnknownManifestId) {
  auto session = makeSession("no-such-provider");
  ASSERT_FALSE(session.has_value());
  EXPECT_NE(session.error().find("no-such-provider"), std::string::npos) << session.error();
  EXPECT_NE(session.error().find("not found"), std::string::npos) << session.error();
  EXPECT_EQ(g_created.load(), 0);
}

// Scenario 2b: the plugin exposes no pj.descriptor_import.v1 extension —
// distinct message, and NO leaked handle (create/destroy balanced).
TEST_F(HeadlessDescriptorProviderSessionTest, CreationFailsWhenExtensionAbsent) {
  auto session = makeSession(kNoExtensionId);
  ASSERT_FALSE(session.has_value());
  EXPECT_NE(session.error().find(PJ_DESCRIPTOR_IMPORT_EXTENSION_V1), std::string::npos) << session.error();
  EXPECT_NE(session.error().find("does not expose"), std::string::npos) << session.error();
  EXPECT_EQ(g_created.load(), 1);
  EXPECT_EQ(g_destroyed.load(), 1) << "the failed factory must not leak the plugin handle";
}

// Scenario 2c: the extension is present but its struct_size does not cover
// the v1 slots — the view is invalid; distinct message, no leaked handle.
TEST_F(HeadlessDescriptorProviderSessionTest, CreationFailsWhenExtensionUndersized) {
  auto session = makeSession(kUndersizedId);
  ASSERT_FALSE(session.has_value());
  EXPECT_NE(session.error().find("struct_size"), std::string::npos) << session.error();
  EXPECT_EQ(g_created.load(), 1);
  EXPECT_EQ(g_destroyed.load(), 1) << "the failed factory must not leak the plugin handle";
}

// Scenario 2d: bind() itself fails — the plugin's own rejection reason is
// surfaced verbatim, no leaked handle.
TEST_F(HeadlessDescriptorProviderSessionTest, CreationFailsWhenBindFails) {
  auto session = makeSession(kBindFailsId);
  ASSERT_FALSE(session.has_value());
  EXPECT_NE(session.error().find("bind"), std::string::npos) << session.error();
  EXPECT_NE(session.error().find("fake bind rejection"), std::string::npos) << session.error();
  EXPECT_EQ(g_created.load(), 1);
  EXPECT_EQ(g_destroyed.load(), 1) << "the failed factory must not leak the plugin handle";
}

// Scenario 3: the import round trip. The fake pushes one dataset + terminal
// from its worker thread; both callbacks arrive ON THE GUI THREAD, queued-
// only (nothing delivers without the event loop, even after the provider
// side fully finished), in order, exactly once; the job registry empties
// after the terminal.
TEST_F(HeadlessDescriptorProviderSessionTest, ImportRoundTripMarshalsCallbacksQueuedOnlyToGuiThread) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();

  std::vector<std::string> order;  // GUI-thread only
  int dataset_calls = 0;
  int terminal_calls = 0;
  PJ::DescriptorImportOutcome outcome = PJ::DescriptorImportOutcome::kFailed;
  QThread* dataset_thread = nullptr;
  QThread* terminal_thread = nullptr;

  PJ::DescriptorImportStartRequest request;
  request.descriptor_json = R"({"v":1})";
  const auto status = (*session)->startImport(
      request,
      [&](PJ::DatasetId id) {
        ++dataset_calls;
        dataset_thread = QThread::currentThread();
        order.push_back("dataset:" + std::to_string(id));
      },
      [&](PJ::DescriptorImportOutcome o, std::string) {
        ++terminal_calls;
        terminal_thread = QThread::currentThread();
        outcome = o;
        order.push_back("terminal");
      });
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ((*session)->activeJobCount(), 1u);

  ASSERT_NE(g_instance, nullptr);
  g_instance->releaseStart();
  // joinAll returns only after the worker's on_terminal returned — i.e. the
  // provider side is completely done. QUEUED-ONLY pin: still, nothing may
  // have been delivered, because delivery needs the GUI event loop.
  (*session)->joinAll();
  EXPECT_EQ(dataset_calls, 0) << "provider-thread callbacks must be queued, never delivered inline";
  EXPECT_EQ(terminal_calls, 0) << "provider-thread callbacks must be queued, never delivered inline";

  ASSERT_TRUE(pumpUntil([&]() { return terminal_calls > 0; }));
  flushQueuedEvents();

  EXPECT_EQ(dataset_calls, 1);
  EXPECT_EQ(terminal_calls, 1);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "dataset:" + std::to_string(kFakeDatasetId));
  EXPECT_EQ(order[1], "terminal");
  EXPECT_EQ(outcome, PJ::DescriptorImportOutcome::kSucceededEagerOnly);
  EXPECT_EQ(dataset_thread, QCoreApplication::instance()->thread());
  EXPECT_EQ(terminal_thread, QCoreApplication::instance()->thread());
  EXPECT_EQ((*session)->activeJobCount(), 0u) << "the registry must empty after the terminal delivered";
  EXPECT_GE(firstIndexOf(g_events.snapshot(), "job.destroy"), 0) << "the concluded job must have been destroyed";
}

// Scenario 5: queued-terminal purge legality. cancelAll + joinAll conclude
// the job (its terminal is now queued on the GUI loop, undelivered); then
// destroy the session with the metacall still pending -> no crash, and no
// callback ever fires after the destructor.
TEST_F(HeadlessDescriptorProviderSessionTest, QueuedTerminalIsPurgedAfterCancelJoinTeardown) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();

  std::atomic<int> dataset_calls{0};
  std::atomic<int> terminal_calls{0};
  PJ::DescriptorImportStartRequest request;
  request.descriptor_json = R"({"v":1})";
  const auto status = (*session)->startImport(
      request, [&](PJ::DatasetId) { dataset_calls.fetch_add(1); },
      [&](PJ::DescriptorImportOutcome, std::string) { terminal_calls.fetch_add(1); });
  ASSERT_TRUE(status) << status.error();

  (*session)->cancelAll();
  (*session)->joinAll();  // terminal posted (queued) by the cancelled worker
  EXPECT_EQ(terminal_calls.load(), 0);

  session->reset();  // the queued metacall dies with the session QObject
  flushQueuedEvents();
  EXPECT_EQ(dataset_calls.load(), 0);
  EXPECT_EQ(terminal_calls.load(), 0) << "no callback may fire after the destructor";
}

// Scenario 7 (T7 per-job cancel): startImport returns a DISTINCT JobId per
// job, and cancelJob(id) cancels ONLY that job — its kCancelled terminal
// still arrives through the normal queued marshal while the other job stays
// live — returning without blocking (no join). An unknown or an
// already-concluded id is a safe no-op.
TEST_F(HeadlessDescriptorProviderSessionTest, StartImportReturnsDistinctJobIdsAndCancelJobTargetsOnlyThatJob) {
  auto session = makeSession(kProviderId);
  ASSERT_TRUE(session.has_value()) << session.error();

  int first_terminals = 0;
  int second_terminals = 0;
  auto first_outcome = PJ::DescriptorImportOutcome::kFailed;
  auto second_outcome = PJ::DescriptorImportOutcome::kFailed;
  PJ::DescriptorImportStartRequest request;
  request.descriptor_json = R"({"v":1})";
  const auto first = (*session)->startImport(request, nullptr, [&](PJ::DescriptorImportOutcome outcome, std::string) {
    first_outcome = outcome;
    ++first_terminals;
  });
  ASSERT_TRUE(first) << first.error();
  const auto second = (*session)->startImport(request, nullptr, [&](PJ::DescriptorImportOutcome outcome, std::string) {
    second_outcome = outcome;
    ++second_terminals;
  });
  ASSERT_TRUE(second) << second.error();
  EXPECT_NE(*first, *second) << "every started job must get a distinct JobId";
  EXPECT_EQ((*session)->activeJobCount(), 2u);

  // Cancel ONLY the first job (both workers still blocked on their start
  // gates). Nonblocking: no join may happen on this thread, and nothing may
  // deliver inline (queued-only marshal).
  (*session)->cancelJob(*first);
  const auto log = g_events.snapshot();
  EXPECT_EQ(countOf(log, "job.cancel"), 1);
  EXPECT_EQ(countOf(log, "job.join.returned"), 0) << "cancelJob must not join";
  EXPECT_EQ(first_terminals, 0) << "provider-thread callbacks must be queued, never delivered inline";

  ASSERT_TRUE(pumpUntil([&]() { return first_terminals > 0; }));
  EXPECT_EQ(first_outcome, PJ::DescriptorImportOutcome::kCancelled);
  EXPECT_EQ(second_terminals, 0) << "the untargeted job must be unaffected";
  EXPECT_EQ((*session)->activeJobCount(), 1u);

  // No-op safety: the concluded id (its registry entry is gone) and a
  // never-issued id both do nothing.
  (*session)->cancelJob(*first);
  (*session)->cancelJob(*first + *second + 1000);
  flushQueuedEvents();
  EXPECT_EQ(countOf(g_events.snapshot(), "job.cancel"), 1);
  EXPECT_EQ(second_terminals, 0);
  EXPECT_EQ((*session)->activeJobCount(), 1u);

  // The surviving job drains normally.
  ASSERT_NE(g_instance, nullptr);
  g_instance->releaseStart();  // last_job == the second job
  ASSERT_TRUE(pumpUntil([&]() { return second_terminals > 0; }));
  EXPECT_EQ(second_outcome, PJ::DescriptorImportOutcome::kSucceededEagerOnly);
  EXPECT_EQ((*session)->activeJobCount(), 0u);
  session->reset();
}

// Scenario 6: DSO-pin proxy. With a static vtable there is no dlopen to pin,
// so this can only assert HANDLE-LIFETIME ordering: the plugin instance is
// destroyed exactly once, and only after every job event — i.e. the handle
// (whose library_owner_ is what pins a real DSO) outlived all import work.
TEST_F(HeadlessDescriptorProviderSessionTest, HandleDestroyedExactlyOnceAndAfterAllJobWork) {
  {
    auto session = makeSession(kProviderId);
    ASSERT_TRUE(session.has_value()) << session.error();
    PJ::DescriptorImportStartRequest request;
    request.descriptor_json = R"({"v":1})";
    const auto status = (*session)->startImport(request, nullptr, nullptr);
    ASSERT_TRUE(status) << status.error();
    ASSERT_NE(g_instance, nullptr);
    g_instance->releaseStart();
    ASSERT_TRUE(pumpUntil([&]() { return (*session)->activeJobCount() == 0; }));
  }  // ~unique_ptr -> session destructor
  const auto log = g_events.snapshot();
  EXPECT_EQ(countOf(log, "plugin.destroy"), 1);
  EXPECT_EQ(g_destroyed.load(), 1);
  const int idx_plugin_destroy = firstIndexOf(log, "plugin.destroy");
  for (std::size_t i = 0; i < log.size(); ++i) {
    if (log[i].rfind("job.", 0) == 0 || log[i].rfind("worker.", 0) == 0) {
      EXPECT_LT(static_cast<int>(i), idx_plugin_destroy)
          << "every job/worker event must precede the plugin instance's destruction";
    }
  }
}

}  // namespace

PJ_APP_TEST_MAIN("headless_descriptor_provider_session_test")
