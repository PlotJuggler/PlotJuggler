#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {

class DataEngine;
class DataSourceRuntimeHost;
class ExtensionCatalogService;
class MessageParserHandle;
class ObjectIngestTapRegistry;
class ObjectStore;
class ServiceRegistryBuilder;
class SourceCaptureService;

// Host-side runner for a Toolbox plugin (the toolbox sibling of DataSourceRuntimeHost): builds the ServiceRegistry its
// bind() consumes: ToolboxHostService (writes into the session's DataEngine and ObjectStore),
// ToolboxObjectReadHostService (reads object topics back, e.g. existing markers), ToolboxRuntimeHostService
// (report_message, notify_data_changed) and SettingsStoreService (over an injected SettingsBackend). App concerns
// arrive as Callbacks, so it stays Widgets-free and headless-testable. Not movable: the runtime-host vtable stores
// `this`.
class ToolboxRuntimeHost {
 public:
  // Always invoked on the constructing (GUI) thread, see marshaller_.
  struct Callbacks {
    // After notify_data_changed flushed buffered writes. `ingested_datasets`: the toolbox-created datasets of a bulk
    // import shaped like a file load, i.e. those whose parser-ingest context was created or released since the
    // previous notify, plus those mid-import (between progress_start and progress_finish) at every notify. The shell
    // focuses playback on them (a 10 s snippet presents a 10 s timeline, and keeps that focus while it grows). Empty
    // for plain write-API toolboxes. A plugin that posts its terminal notify from a third thread, not after
    // progress_finish on its ingest thread, may see the import reported active for one more round.
    std::function<void(std::vector<DatasetId> ingested_datasets)> on_data_changed;
    std::function<void(PJ_toolbox_message_level_t, std::string)> on_message;
    // Progress of a delegated bulk import (progress_start / _update / _finish on the runtime host create_parser_ingest
    // returned). `total` 0 = indeterminate. on_ingest_progress fires only on ticks that flushed (throttled), so each
    // call means new rows are reader-visible; the catalog rebuild stays on on_data_changed. `cancellable` is the
    // plugin's own flag; stopping this path always keeps what arrived (no rollback), so register the ingest stop-only.
    std::function<void(DatasetId dataset, std::string label, uint64_t total, bool cancellable)> on_ingest_started;
    std::function<void(DatasetId dataset, uint64_t current, uint64_t total)> on_ingest_progress;
    std::function<void(DatasetId dataset)> on_ingest_finished;
  };

  // Enables the parser-ingest slots; with a null catalog they fail "not configured".
  struct ParserIngestDeps {
    ExtensionCatalogService* catalog = nullptr;
    std::shared_ptr<ObjectIngestTapRegistry> ingest_taps;
    // The render-time parser of every object topic a binding registers (forward to
    // SessionManager::registerObjectTopicParser, as FileLoader does). May run on a toolbox worker thread.
    std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)> register_object_parser;

    // Source capture (desktop only; null disables it). With the plugin's manifest id as the unspoofable provider
    // identity, every context is armed at creation: release runs the publication gate, discard and teardown abort, a
    // host-side stop refuses. A capture never outlives its context, so a re-created context starts a fresh one.
    SourceCaptureService* capture_service = nullptr;
    std::string capture_provider_id;
    // On the release caller's thread (a plugin worker), only for a record-worthy release (record attached, COMPLETED,
    // no veto, no host stop). `source_identity` is sourceCacheIdentityDigest(provider, descriptor); `artifact_path`
    // (native encoding, empty when refused) lets the shell register the dataset's backing file. The callback marshals
    // to the GUI thread itself.
    std::function<void(
        DatasetId dataset, const std::string& provider_id, const std::string& descriptor_json,
        const std::string& source_identity, bool published, const std::string& refusal_reason,
        const std::filesystem::path& artifact_path)>
        on_capture_finalized;
  };

  // Two overloads, not a default argument: GCC rejects `ParserIngestDeps{}` as one, since the nested struct's member
  // initializers are unusable until the enclosing class is complete.
  ToolboxRuntimeHost(
      DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks);
  ToolboxRuntimeHost(
      DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks,
      ParserIngestDeps parser_ingest);
  // Out of line (DataSourceRuntimeHost is incomplete here); flushes any context the toolbox never released.
  ~ToolboxRuntimeHost();

  ToolboxRuntimeHost(const ToolboxRuntimeHost&) = delete;
  ToolboxRuntimeHost& operator=(const ToolboxRuntimeHost&) = delete;

  // Fails, without binding, when a service the toolbox ABI requires was rejected (do not bind the plugin then); a
  // rejected SettingsStore or object-read service only warns.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  // A context was ever created for `dataset_id`, released ones included: source promotion asks after the download
  // completed. Thread-safe.
  [[nodiscard]] bool hasIngestForDataset(DatasetId dataset_id) const;

  // A context created and not yet released (unlike hasIngestForDataset, goes false again). Thread-safe.
  [[nodiscard]] bool hasActiveIngests() const;

  // Flag-only cooperative stop of every live context; the plugin sees it through is_stop_requested / progress_update
  // returning false. Keeps what arrived: the toolbox ABI has no host-side rollback. Thread-safe.
  void requestStopActiveIngests();

  /// The same for one dataset's context, leaving the siblings of a multi-dataset import running. False when it has no
  /// live context.
  bool requestStopIngestForDataset(DatasetId dataset_id);

  // Test seam. A plain int read by the ingest thread: set it before any ingest begins.
  void setFlushThrottleMs(int ms) {
    flush_throttle_ms_ = ms;
  }

 private:
  static void onReportMessage(void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept;
  static void onNotifyDataChanged(void* ctx) noexcept;
  static bool onCreateParserIngest(
      void* ctx, uint32_t data_source_id, PJ_data_source_runtime_host_t* out_host, PJ_error_t* out_error) noexcept;
  static bool onReleaseParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept;
  // The rollback twin of release: destroys the context without flushing and reports no completed import, so a vetoed
  // download leaves neither rows nor a focus event. Dataset rollback stays with the owning transaction. Idempotent.
  static bool onDiscardParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept;

  DatastoreToolboxHost write_host_;
  DatastoreToolboxObjectReadHost read_host_;
  sdk::SettingsStoreHost settings_host_;
  Callbacks callbacks_;
  DataEngine& engine_;
  ObjectStore& object_store_;
  ParserIngestDeps parser_ingest_deps_;
  // Keeps the maps consistent across concurrent create/release and the teardown flush out of them. Not a guard against
  // a running plugin worker: unbind the plugin and quiesce its worker before destroying this host
  // (toolbox_protocol.h "valid until ... host teardown"). Concurrent create+release of one id is out of contract.
  mutable std::mutex parser_ingest_mu_;
  // Keyed by data-source handle id (== DatasetId): the file-load ingest machinery on a toolbox-created dataset.
  std::unordered_map<uint32_t, std::unique_ptr<DataSourceRuntimeHost>> parser_ingests_;
  // One armed capture per live context, same key and lock; torn down before its context on every path. Opaque so
  // the header builds where SourceCaptureService does not (wasm).
  struct CaptureSlot;
  std::unordered_map<uint32_t, std::unique_ptr<CaptureSlot>> captures_;
  // Defined in the cpp: commit on release, abort otherwise. Caller's thread, `host` alive, producers quiesced.
  void finalizeCapture(uint32_t data_source_id, std::unique_ptr<CaptureSlot> slot, DataSourceRuntimeHost& host);
  static void abortCapture(std::unique_ptr<CaptureSlot> slot);
  // Shared with the hooks on a context's host. `last_flush` is touched only on the plugin's ingest thread (one progress
  // caller per context). The map (under parser_ingest_mu_) keeps entries until teardown, and a re-created context
  // reuses its dataset's entry: an undelivered terminal must stay visible to the destructor's sweep, and a live
  // re-created context at idle phase is indistinguishable from a dead one, so erasing would break the shell's
  // started/finished pairing.
  struct IngestProgress {
    // Terminal-delivery word: low 2 bits phase, high bits the arm generation (bumped by every progress_start). One
    // CAS per transition, so a stale queued finish never consumes a newer sequence's terminal:
    //   progress_start   (g, *)             -> (g+1, kActive)     CAS-loop RMW
    //   claim            (g, kActive)       -> (g, kFinishQueued) single CAS;
    //     (finish hook /                       losers no-op: a prior claim of
    //      release)                            the same sequence, a re-arm
    //                                          (generation moved), the sweep
    //   delivery         (g, kFinishQueued) -> (g, kIdle)         single CAS
    //                                          on the CLAIMED word; fires iff
    //                                          it wins (a re-armed or swept
    //                                          word fails structurally)
    //   destructor sweep (any)              -> 0; fires iff the prior phase
    //                                          was not kIdle (generation-
    //                                          agnostic: live OR queued still
    //                                          owes its finish; queued
    //                                          metacalls die with marshaller_)
    // Claims never retry across generations: release claims under parser_ingest_mu_ (a re-arm needs it too, through
    // create), so it takes only its own sequence; the finish hook claims lock-free, where a lost CAS means already
    // terminated or superseded.
    static constexpr uint64_t kPhaseMask = 0x3;
    static constexpr uint64_t kPhaseIdle = 0x0;
    static constexpr uint64_t kPhaseActive = 0x1;
    static constexpr uint64_t kPhaseFinishQueued = 0x2;
    static constexpr uint64_t kGenerationStep = 0x4;
    std::atomic<uint64_t> state{0};
    std::atomic<uint64_t> total{0};
    std::chrono::steady_clock::time_point last_flush;
  };
  std::unordered_map<uint32_t, std::shared_ptr<IngestProgress>> ingest_progress_;
  // Single CAS (gen, kActive) -> (gen, kFinishQueued). 0 when there is nothing to claim: no active sequence (a finish
  // without a start is a safe no-op), already claimed, or re-armed.
  [[nodiscard]] static uint64_t claimIngestFinish(IngestProgress& progress);
  // Delivery CASes exactly `claimed_word` back to (gen, kIdle) and fires only if it wins; an undelivered claim stays
  // queued for the destructor's sweep.
  void postIngestFinished(DatasetId dataset_id, const std::shared_ptr<IngestProgress>& progress, uint64_t claimed_word);
  int flush_throttle_ms_ = 50;
  // Datasets given a context since the last notify_data_changed, drained into on_data_changed. Under
  // parser_ingest_mu_.
  std::vector<DatasetId> pending_ingest_datasets_;
  PJ_toolbox_runtime_host_vtable_t runtime_vtable_;
  PJ_toolbox_runtime_host_t runtime_;
  // report_message and notify_data_changed are [thread-safe] in the ABI, while the callbacks touch Qt state: the
  // trampolines marshal to this object's (GUI) thread with Qt::AutoConnection (same thread runs synchronously).
  QObject marshaller_;
};

}  // namespace PJ
