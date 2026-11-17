#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared descriptor-import fake for the layout-import suites, in two layers:
//   * FakeJob + kFakeJobVtable: a PJ_joinable_job_t whose worker blocks on a start gate as its first action (so no
//     callback can precede start_import's return, the ABI contract), released by the test, by cancel, or by destroy.
//     Optional per-job hooks (`on_event` for event logs, `unlink` for owner detach) let the scripted provider below
//     and the headless-session test's event-ordering fake share the same job mechanics.
//   * A descriptor-scripted provider toolbox (registerStaticToolbox, no DSO), driven by the descriptor JSON:
//       {"name":"s1","trust":"trusted|confirm|refused","path":"<effective>",
//        "estimated":123,"import":"<mode>",
//        "materialized":bool,   // optional: overrides the cache verdict
//        "identity":"...",      // optional: overrides the source identity
//        "topic":"...",         // optional: published topic of the progressive and capture modes
//        "loader":"..."}        // optional: progressive-promoted's stock loader
//     query_descriptor answers trust/path/identity/estimate and the is_materialized verdict (defaults: identity =
//     "fake:"+path, materialized = whether the file exists; override both to script verdicts independent of the
//     filesystem). start_import runs the scripted outcome on a real worker thread through the C ABI. Import modes:
//       eager|promoted|fail   run to their terminal once released (every mode but "block" is released by a queued
//                             0-timer after start_import returned).
//       block                 holds the job before any callback.
//       announce-then-block   announces its dataset, then holds on the resume gate: a deterministic mid-import state.
//       progressive[-promoted], capture   drive the host services cached at fakeBind from the job worker (below).
//     The start-return gate invariant holds for every mode: the worker blocks on the start gate first.

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_base/toolbox_protocol.h"

namespace pj_fake_import {

inline constexpr const char* kProviderId = "fake-import-provider";

[[nodiscard]] inline PJ_string_view_t sv(const char* text) {
  return PJ_string_view_t{text, std::strlen(text)};
}

// ---- The job half ----

struct FakeJob {
  std::thread worker;
  std::atomic<bool> cancelled{false};
  std::binary_semaphore start_gate{0};
  std::atomic<bool> start_released{false};
  // Second blocking point (announce-then-block and the progressive modes): the worker re-blocks here after
  // announcing its dataset. Released like the start gate (test, cancel, or destroy).
  std::binary_semaphore resume_gate{0};
  std::atomic<bool> resume_released{false};
  // Optional instrumentation: `on_event` receives the vtable-side events
  // ("job.cancel", "job.join.returned", "job.destroy"); `unlink` detaches
  // the job from its owner's registry inside destroy, before deletion.
  std::function<void(const char*)> on_event;
  std::function<void(FakeJob*)> unlink;

  // At-most-once release: binary_semaphore::release() on an already-released
  // semaphore is UB, and the gate can be released from the test, cancel, or
  // destroy in any combination.
  void releaseStartOnce() {
    bool expected = false;
    if (start_released.compare_exchange_strong(expected, true)) {
      start_gate.release();
    }
  }

  void releaseResumeOnce() {
    bool expected = false;
    if (resume_released.compare_exchange_strong(expected, true)) {
      resume_gate.release();
    }
  }

  void event(const char* name) const {
    if (on_event) {
      on_event(name);
    }
  }
};

inline void fakeJobCancel(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  job->event("job.cancel");
  job->cancelled.store(true);
  job->releaseStartOnce();  // a blocked worker must observe the cancel
  job->releaseResumeOnce();
}

inline void fakeJobJoin(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  if (job->worker.joinable()) {
    job->worker.join();
  }
  job->event("job.join.returned");
}

inline void fakeJobDestroy(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  // Destroy cancels and joins when necessary (the ABI contract) — without
  // re-emitting cancel/join events, so an event log keeps one entry per
  // caller-side call.
  job->cancelled.store(true);
  job->releaseStartOnce();
  job->releaseResumeOnce();
  if (job->worker.joinable()) {
    job->worker.join();
  }
  job->event("job.destroy");
  if (job->unlink) {
    job->unlink(job);
  }
  delete job;
}

inline constexpr PJ_joinable_job_vtable_t kFakeJobVtable{
    sizeof(PJ_joinable_job_vtable_t), 0, &fakeJobCancel, &fakeJobJoin, &fakeJobDestroy};

// ---- The scripted provider half ----

struct FakeLog {
  std::mutex mu;
  std::vector<std::string> started;            // import start order, by "name"
  std::vector<std::uint64_t> start_max_bytes;  // request->max_transfer_bytes per start
  void addStart(std::string name, std::uint64_t max_bytes) {
    const std::lock_guard<std::mutex> lock(mu);
    started.push_back(std::move(name));
    start_max_bytes.push_back(max_bytes);
  }
  [[nodiscard]] std::vector<std::string> snapshot() {
    const std::lock_guard<std::mutex> lock(mu);
    return started;
  }
  [[nodiscard]] std::vector<std::uint64_t> maxBytes() {
    const std::lock_guard<std::mutex> lock(mu);
    return start_max_bytes;
  }
  void clear() {
    const std::lock_guard<std::mutex> lock(mu);
    started.clear();
    start_max_bytes.clear();
  }
};

inline FakeLog g_log;
inline std::atomic<std::uint32_t> g_next_dataset_id{101};

struct FakeProviderInstance;
inline FakeProviderInstance* g_instance = nullptr;

struct FakeProviderInstance {
  PJ_descriptor_import_provider_v1_t ext{};
  // Host services cached at fakeBind: registry views stay valid for the whole plugin session and the session joins
  // every job before the instance dies, so job workers use copies for their whole run. Default (invalid) views mean
  // the host did not register the service: progressive and capture jobs then fail loudly.
  PJ::sdk::ToolboxHostView toolbox_host;
  PJ::ToolboxRuntimeHostView runtime_host;
  PJ::SourcePromotionHostView promotion_host;
  // Borrowed-view backing storage for query_descriptor results (valid until
  // the next query on this instance — the ABI's lifetime contract).
  std::string query_identity;
  std::string query_path;
  // Covers last_job lookup+use AND the destroy-side unlink, so a
  // releaseStart can never race the pointee's deletion.
  std::mutex job_mu;
  FakeJob* last_job = nullptr;  // non-owning; owned by the returned out_job->ctx

  void releaseStart() {
    const std::lock_guard<std::mutex> lock(job_mu);
    if (last_job != nullptr) {
      last_job->releaseStartOnce();
    }
  }

  void releaseResume() {
    const std::lock_guard<std::mutex> lock(job_mu);
    if (last_job != nullptr) {
      last_job->releaseResumeOnce();
    }
  }
};

[[nodiscard]] inline QJsonObject parseDescriptor(PJ_string_view_t descriptor_json) {
  const QByteArray bytes(descriptor_json.data, static_cast<qsizetype>(descriptor_json.size));
  QJsonObject scripted = QJsonDocument::fromJson(bytes).object();
  // Capture-eligible descriptors must keep the host's attach envelope (only
  // kind/request/v at top level), so their script rides inside "request".
  if (!scripted.contains(QStringLiteral("name")) && scripted.value(QStringLiteral("request")).isObject()) {
    scripted = scripted.value(QStringLiteral("request")).toObject();
  }
  return scripted;
}

[[nodiscard]] inline QString makeDescriptor(
    const QString& name, const QString& trust, const QString& path, std::uint64_t estimated = 0,
    const QString& import_mode = QStringLiteral("eager")) {
  QJsonObject obj;
  obj.insert(QStringLiteral("name"), name);
  obj.insert(QStringLiteral("trust"), trust);
  obj.insert(QStringLiteral("path"), path);
  obj.insert(QStringLiteral("estimated"), static_cast<double>(estimated));
  obj.insert(QStringLiteral("import"), import_mode);
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

inline bool fakeQueryDescriptor(
    void* plugin_ctx, PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out,
    PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(plugin_ctx);
  const QJsonObject scripted = parseDescriptor(descriptor_json);
  const QString trust = scripted.value(QStringLiteral("trust")).toString(QStringLiteral("trusted"));
  if (trust == QStringLiteral("refused")) {
    out->trust = PJ_DESCRIPTOR_TRUST_REFUSED;
  } else if (trust == QStringLiteral("confirm")) {
    out->trust = PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION;
  } else {
    out->trust = PJ_DESCRIPTOR_TRUST_TRUSTED;
  }
  self->query_path = scripted.value(QStringLiteral("path")).toString().toStdString();
  self->query_identity = scripted.contains(QStringLiteral("identity"))
                             ? scripted.value(QStringLiteral("identity")).toString().toStdString()
                             : "fake:" + self->query_path;
  const QJsonValue materialized = scripted.value(QStringLiteral("materialized"));
  out->is_materialized = materialized.isBool() ? (materialized.toBool() ? 1 : 0)
                                               : (QFileInfo::exists(QString::fromStdString(self->query_path)) ? 1 : 0);
  out->source_identity = PJ_string_view_t{self->query_identity.data(), self->query_identity.size()};
  out->local_path_utf8 = PJ_string_view_t{self->query_path.data(), self->query_path.size()};
  out->estimated_bytes = static_cast<uint64_t>(scripted.value(QStringLiteral("estimated")).toDouble(0));
  return true;
}

// ---- The progressive and progressive-promoted modes ----
// Everything the worker needs is captured by value: the views are borrowed fat pointers valid for the plugin session,
// and the session joins every job before teardown, so no capture can dangle.

struct ProgressiveScript {
  PJ::sdk::ToolboxHostView toolbox;
  PJ::ToolboxRuntimeHostView runtime;
  PJ::SourcePromotionHostView promotion;
  std::string name;        // dataset name AND the progressStart label
  std::string topic;       // published through the real write host
  std::string path;        // the artifact (promoted mode's promotion target)
  std::string identity;    // source identity for the promotion request
  std::string descriptor;  // verbatim descriptor json for the promotion request
  std::string loader_id;   // promoted mode's stock loader manifest id
  bool promoted = false;
};

using ImportTerminalFn = void (*)(void*, PJ_descriptor_import_outcome_t, PJ_string_view_t) PJ_NOEXCEPT;
using ImportDatasetFn = void (*)(void*, PJ_data_source_handle_t) PJ_NOEXCEPT;

inline void progressiveWorker(
    FakeJob* job, const ProgressiveScript& script, ImportDatasetFn on_dataset, ImportTerminalFn on_terminal,
    void* callback_ctx) {
  // Block first, before any host call or callback: none may precede start_import's return (the ABI contract).
  job->start_gate.acquire();
  if (job->cancelled.load()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
    return;
  }
  if (!script.toolbox.valid() || !script.runtime.valid()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("progressive fake: host services were not bound"));
    return;
  }

  // A real dataset from the bound write host, announced via on_dataset strictly before progress_start (same worker,
  // so the two GUI marshals land in that order).
  const auto source = script.toolbox.createDataSource(script.name);
  if (!source.has_value()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("progressive fake: createDataSource failed"));
    return;
  }
  if (on_dataset != nullptr) {
    on_dataset(callback_ctx, PJ_data_source_handle_t{source->id});
  }

  const auto ingest = script.runtime.createDatasetIngest(source->id);
  if (!ingest.has_value()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("progressive fake: createDatasetIngest failed"));
    return;
  }
  // From here on the ingest context exists: every exit runs the lifecycle
  // epilogue (progressFinish + release) so the host's exactly-once terminal
  // bookkeeping stays consistent on cancel/failure paths too.
  const auto conclude = [&](PJ_descriptor_import_outcome_t outcome, const char* message) {
    ingest->progressFinish();
    static_cast<void>(script.runtime.releaseDatasetIngest(source->id));
    on_terminal(callback_ctx, outcome, sv(message));
  };
  static_cast<void>(ingest->progressStart(script.name, /*total_steps=*/2, /*cancellable=*/true));

  const auto topic = script.toolbox.ensureTopic(*source, script.topic);
  if (!topic.has_value()) {
    conclude(PJ_DESCRIPTOR_IMPORT_FAILED, "progressive fake: ensureTopic failed");
    return;
  }
  if (const auto field = script.toolbox.ensureField(*topic, "value", PJ::PrimitiveType::kFloat64); !field.has_value()) {
    conclude(PJ_DESCRIPTOR_IMPORT_FAILED, "progressive fake: ensureField failed");
    return;
  }
  if (const auto appended = script.toolbox.appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
      !appended) {
    conclude(PJ_DESCRIPTOR_IMPORT_FAILED, "progressive fake: appendRecord failed");
    return;
  }

  // One deterministic tick: immediately throttle-eligible, it flushes the write host so the record is
  // reader-visible, then the host marshals on_ingest_progress to the GUI thread (SessionManager::updateIngest
  // publishes samplesIngested). Then park until released.
  static_cast<void>(ingest->progressUpdate(1));
  job->resume_gate.acquire();
  if (job->cancelled.load()) {
    conclude(PJ_DESCRIPTOR_IMPORT_CANCELLED, "cancelled");
    return;
  }

  // Finish + release the ingest, then (promoted mode) the real promotion transaction: the artifact was already
  // "downloaded" (the descriptor's path), and PROMOTED is reported only once the async result says the replace
  // transaction succeeded.
  ingest->progressFinish();
  static_cast<void>(script.runtime.releaseDatasetIngest(source->id));
  PJ_descriptor_import_outcome_t outcome = PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY;
  if (script.promoted && script.promotion.valid()) {
    PJ::SourcePromotionRequest request;
    request.dataset = source->id;
    request.source_identity = script.identity;
    request.local_path_utf8 = script.path;
    request.loader_plugin_id = script.loader_id;
    request.loader_config_json = "{}";
    request.descriptor_json = script.descriptor;
    auto promotion_ok = std::make_shared<std::atomic<bool>>(false);
    auto promotion_done = std::make_shared<std::binary_semaphore>(0);
    const auto accepted =
        script.promotion.promoteToFileSource(request, [promotion_ok, promotion_done](bool ok, std::string /*msg*/) {
          promotion_ok->store(ok);
          promotion_done->release();
        });
    if (accepted) {
      // Exactly-once result, delivered even at teardown (the session fails
      // the promotion intake before joining), so this wait cannot deadlock.
      promotion_done->acquire();
      if (promotion_ok->load()) {
        outcome = PJ_DESCRIPTOR_IMPORT_SUCCEEDED_PROMOTED;
      }
    }
  }
  // The toolbox-instance runtime host publishes the ingested dataset (catalog rebuild + time-reference refresh in
  // the headless session's on_data_changed).
  script.runtime.notifyDataChanged();
  on_terminal(callback_ctx, outcome, sv("done"));
}

// ---- The capture mode ----
// A delegated download in the shape the source-capture publication gate requires: descriptor attached before the first
// push, raw bytes through a real parser binding (encoding "runtime_host_object": the suite stages that parser
// plugin), an explicit COMPLETED terminal, then the ingest release (the capture commit point). A host wired with
// SourceCaptureService therefore publishes a real cache artifact for the request.

struct CaptureScript {
  PJ::sdk::ToolboxHostView toolbox;
  PJ::ToolboxRuntimeHostView runtime;
  std::string name;        // dataset name
  std::string topic;       // the one recorded (and declared) topic
  std::string descriptor;  // verbatim request bytes — attached as the source record
};

inline void captureWorker(
    FakeJob* job, const CaptureScript& script, ImportDatasetFn on_dataset, ImportTerminalFn on_terminal,
    void* callback_ctx) {
  job->start_gate.acquire();
  if (job->cancelled.load()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
    return;
  }
  if (!script.toolbox.valid() || !script.runtime.valid()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("capture fake: host services were not bound"));
    return;
  }
  const auto source = script.toolbox.createDataSource(script.name);
  if (!source.has_value()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("capture fake: createDataSource failed"));
    return;
  }
  if (on_dataset != nullptr) {
    on_dataset(callback_ctx, PJ_data_source_handle_t{source->id});
  }
  const auto ingest = script.runtime.createDatasetIngest(source->id);
  if (!ingest.has_value()) {
    on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("capture fake: createDatasetIngest failed"));
    return;
  }
  bool ok = ingest->attachSourceRecord(script.descriptor).has_value();
  if (ok) {
    const auto binding = ingest->ensureParserBinding(
        PJ::ParserBindingRequest{
            .topic_name = script.topic,
            .parser_encoding = "runtime_host_object",
            .type_name = "mock/image",
            .schema = PJ::Span<const uint8_t>{},
            .parser_config_json = R"({"k":1})",
        });
    ok = binding.has_value() &&
         ingest->pushMessage(*binding, PJ::Timestamp{100}, []() -> std::vector<uint8_t> { return {1, 2, 3}; })
             .has_value();
    if (ok) {
      const std::string_view requested[] = {std::string_view(script.topic)};
      ok = ingest->completeIngest(PJ::sdk::IngestOutcome::kCompleted, {requested, 1}).has_value();
    }
  }
  // The release IS the capture commit: a wired host evaluates the publication
  // gate here, on this worker thread, before the terminal is reported.
  static_cast<void>(script.runtime.releaseDatasetIngest(source->id));
  script.runtime.notifyDataChanged();
  on_terminal(
      callback_ctx, ok ? PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY : PJ_DESCRIPTOR_IMPORT_FAILED,
      sv(ok ? "done" : "capture fake: delegated ingest failed"));
}

inline bool fakeStartImport(
    void* plugin_ctx, const PJ_descriptor_import_start_request_v1_t* request,
    const PJ_descriptor_import_callbacks_v1_t* callbacks, void* callback_ctx, PJ_joinable_job_t* out_job,
    PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(plugin_ctx);
  if (request == nullptr || callbacks == nullptr || callbacks->on_terminal == nullptr) {
    return false;
  }
  const QJsonObject scripted = parseDescriptor(request->descriptor_json);
  const std::string name = scripted.value(QStringLiteral("name")).toString().toStdString();
  const QString mode = scripted.value(QStringLiteral("import")).toString(QStringLiteral("eager"));
  g_log.addStart(name, request->max_transfer_bytes);

  auto on_dataset = callbacks->on_dataset;
  auto on_terminal = callbacks->on_terminal;
  auto* job = new FakeJob();
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
  if (mode == QStringLiteral("capture")) {
    CaptureScript script;
    script.toolbox = self->toolbox_host;
    script.runtime = self->runtime_host;
    script.name = name;
    script.topic = scripted.value(QStringLiteral("topic"))
                       .toString(QStringLiteral("/") + QString::fromStdString(name))
                       .toStdString();
    script.descriptor.assign(request->descriptor_json.data, request->descriptor_json.size);
    job->worker = std::thread([job, script = std::move(script), on_dataset, on_terminal, callback_ctx] {
      captureWorker(job, script, on_dataset, on_terminal, callback_ctx);
    });
    QTimer::singleShot(0, [self]() {
      if (g_instance == self) {
        self->releaseStart();
      }
    });
    return true;
  }
  if (mode == QStringLiteral("progressive") || mode == QStringLiteral("progressive-promoted")) {
    ProgressiveScript script;
    script.toolbox = self->toolbox_host;
    script.runtime = self->runtime_host;
    script.promotion = self->promotion_host;
    script.name = name;
    const QString path = scripted.value(QStringLiteral("path")).toString();
    script.path = path.toStdString();
    script.topic = scripted.value(QStringLiteral("topic"))
                       .toString(QStringLiteral("/") + QString::fromStdString(name))
                       .toStdString();
    script.identity = scripted.contains(QStringLiteral("identity"))
                          ? scripted.value(QStringLiteral("identity")).toString().toStdString()
                          : "fake:" + script.path;
    script.descriptor.assign(request->descriptor_json.data, request->descriptor_json.size);
    script.loader_id =
        scripted.value(QStringLiteral("loader")).toString(QStringLiteral("mock-file-source")).toStdString();
    script.promoted = mode == QStringLiteral("progressive-promoted");
    job->worker = std::thread([job, script = std::move(script), on_dataset, on_terminal, callback_ctx] {
      progressiveWorker(job, script, on_dataset, on_terminal, callback_ctx);
    });
    QTimer::singleShot(0, [self]() {
      if (g_instance == self) {
        self->releaseStart();
      }
    });
    return true;
  }

  const bool fail = mode == QStringLiteral("fail");
  const bool promoted = mode == QStringLiteral("promoted");
  const bool announce_then_block = mode == QStringLiteral("announce-then-block");
  job->worker = std::thread([job, on_dataset, on_terminal, callback_ctx, fail, promoted, announce_then_block] {
    // Every worker blocks on its start gate first, so no job callback can precede start_import's return (the ABI
    // contract). Non-"block" modes are auto-released by the queued 0-timer below.
    job->start_gate.acquire();
    if (job->cancelled.load()) {
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
      return;
    }
    if (fail) {
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("scripted import failure"));
      return;
    }
    if (on_dataset != nullptr) {
      on_dataset(callback_ctx, PJ_data_source_handle_t{g_next_dataset_id.fetch_add(1)});
    }
    if (announce_then_block) {
      // Hold the job open between its dataset announcement and its terminal (a deterministic mid-import window)
      // until the test releases the resume gate, or a cancel/destroy does.
      job->resume_gate.acquire();
      if (job->cancelled.load()) {
        on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
        return;
      }
    }
    on_terminal(
        callback_ctx, promoted ? PJ_DESCRIPTOR_IMPORT_SUCCEEDED_PROMOTED : PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY,
        sv("done"));
  });
  if (mode != QStringLiteral("block")) {
    // start_import runs on the GUI thread: a queued 0-timer releases the gate strictly after this call returned to
    // the event loop, keeping the "no callback before start_import returns" contract. `self` may be destroyed before
    // the timer fires; the g_instance guard makes the release a no-op then.
    QTimer::singleShot(0, [self]() {
      if (g_instance == self) {
        self->releaseStart();
      }
    });
  }
  return true;
}

inline void* fakeCreate() noexcept {
  auto* instance = new FakeProviderInstance();
  instance->ext.struct_size = sizeof(PJ_descriptor_import_provider_v1_t);
  instance->ext.query_descriptor = &fakeQueryDescriptor;
  instance->ext.start_import = &fakeStartImport;
  g_instance = instance;
  return instance;
}

inline void fakeDestroyToolbox(void* ctx) noexcept {
  if (g_instance == ctx) {
    g_instance = nullptr;
  }
  delete static_cast<FakeProviderInstance*>(ctx);
}

inline uint64_t fakeCapabilities(void* /*ctx*/) noexcept {
  return 0;
}

inline bool fakeBind(void* ctx, PJ_service_registry_t registry, PJ_error_t* /*out_error*/) noexcept {
  // Cache the bound host services for the progressive and capture modes. Optional lookups: suites that bind this
  // fake through hosts without one of these services keep working; only those modes require them.
  auto* self = static_cast<FakeProviderInstance*>(ctx);
  const PJ::sdk::ServiceRegistry services(registry);
  self->toolbox_host = services.get<PJ::sdk::ToolboxHostService>().value_or(PJ::sdk::ToolboxHostView{});
  self->runtime_host = services.get<PJ::sdk::ToolboxRuntimeHostService>().value_or(PJ::ToolboxRuntimeHostView{});
  self->promotion_host = services.get<PJ::sdk::SourcePromotionHostService>().value_or(PJ::SourcePromotionHostView{});
  return true;
}

inline bool fakeSaveConfig(void* /*ctx*/, PJ_string_view_t* out_json, PJ_error_t* /*out_error*/) noexcept {
  if (out_json != nullptr) {
    out_json->data = "{}";
    out_json->size = 2;
  }
  return true;
}

inline bool fakeLoadConfig(void* /*ctx*/, PJ_string_view_t /*config_json*/, PJ_error_t* /*out_error*/) noexcept {
  return true;
}

inline PJ_borrowed_dialog_t fakeDialog(void* /*ctx*/) noexcept {
  return PJ_borrowed_dialog_t{nullptr, nullptr};
}

inline void fakeOnDataChanged(void* /*ctx*/) noexcept {}

inline const void* fakeGetExtension(void* ctx, PJ_string_view_t id) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(ctx);
  const std::string requested(id.data == nullptr ? "" : id.data, id.size);
  return requested == PJ_DESCRIPTOR_IMPORT_EXTENSION_V1 ? &self->ext : nullptr;
}

inline const PJ_toolbox_vtable_t kFakeVtable{
    PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
    sizeof(PJ_toolbox_vtable_t),
    &fakeCreate,
    &fakeDestroyToolbox,
    R"({"id":"fake-import-provider","name":"Fake Import Provider","version":"1.0.0"})",
    &fakeCapabilities,
    &fakeBind,
    &fakeSaveConfig,
    &fakeLoadConfig,
    &fakeDialog,
    &fakeOnDataChanged,
    &fakeGetExtension,
};

}  // namespace pj_fake_import
