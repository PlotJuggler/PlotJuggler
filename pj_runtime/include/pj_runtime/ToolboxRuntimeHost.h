#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// Host-side runner for a Toolbox plugin — the toolbox sibling of
// DataSourceRuntimeHost. Owns the host implementations of the services a
// toolbox plugin's bind() consumes and assembles them into a ServiceRegistry:
//
//   - ToolboxHostService        the write surface (DatastoreToolboxHost) into
//                               the session's DataEngine + ObjectStore.
//   - ToolboxObjectReadHostService  the read surface (DatastoreToolboxObjectReadHost)
//                               so a toolbox can read back object topics it (or
//                               another producer) published — e.g. existing markers.
//   - ToolboxRuntimeHostService diagnostics (report_message) + notify_data_changed.
//   - SettingsStoreService      persistence over an injected SettingsBackend.
//
// App-shell concerns (which catalog to rebuild, where messages surface) are
// injected as Callbacks, so this class stays Qt-Widgets-free and headless
// testable. Not movable: the runtime-host vtable stores `this`.
class ToolboxRuntimeHost {
 public:
  // Callbacks are always invoked on the thread that constructed this host (the
  // GUI thread) — see marshaller_. Safe to touch Qt models / session state.
  struct Callbacks {
    // Fired after notify_data_changed flushes buffered writes — the host
    // rebuilds its catalog / reseeds playback here. `ingested_datasets` lists
    // the toolbox-created datasets that mark a bulk import shaped like a file
    // load (the cloud connector's fetch): datasets whose parser-ingest context
    // was created or released since the PREVIOUS notify, plus those whose
    // context is MID-IMPORT (between progress_start and progress_finish) at
    // every notify while the import runs. Empty for plain write-API toolboxes.
    // Hosts use it to FOCUS playback on the imported data: a 10s cloud snippet
    // must present a 10s timeline, not be unioned into whatever range the
    // session already had — and a progressive import keeps that focus as it
    // grows instead of collapsing back to the union on each notify. Ordering
    // caveat: "no longer mid-import at the terminal notify" assumes the plugin
    // posts that notify AFTER progress_finish from the same thread (the
    // documented single-ingest-worker pattern); a notify raced in from a third
    // thread may still observe the import as active for one round.
    std::function<void(std::vector<DatasetId> ingested_datasets)> on_data_changed;
    // Fired for each plugin diagnostic — routed to the app's notification UI.
    std::function<void(PJ_toolbox_message_level_t, std::string)> on_message;
    // Progress surface of a delegated bulk import — fired when the toolbox
    // calls progress_start / progress_update / progress_finish on the runtime
    // host returned by create_parser_ingest. `total` is 0 for an indeterminate
    // import. on_ingest_progress fires only on the ticks that also flushed
    // pending writes (throttled, ~50 ms), so each invocation means "new rows
    // just became reader-visible" — a handler can publish incrementally
    // (plots/playback/TF) without any gating of its own. The heavyweight
    // catalog rebuild stays on on_data_changed.
    // `cancellable` is the PLUGIN's own declaration, forwarded verbatim. The
    // shell needs it to decide whether the row offers a stop affordance at
    // all; note that stopping this path always keeps (requestStopActiveIngests
    // has no rollback), so a shell should register the ingest as stop-only.
    std::function<void(DatasetId dataset, std::string label, uint64_t total, bool cancellable)> on_ingest_started;
    std::function<void(DatasetId dataset, uint64_t current, uint64_t total)> on_ingest_progress;
    std::function<void(DatasetId dataset)> on_ingest_finished;
  };

  // Optional dependencies enabling the parser-ingest tail slots. With a null
  // catalog the slots fail "not configured" — headless tests and embedders
  // that load no parser plugins keep working unchanged.
  struct ParserIngestDeps {
    ExtensionCatalogService* catalog = nullptr;
    std::shared_ptr<ObjectIngestTapRegistry> ingest_taps;
    // Receives the render-time parser instance for every object topic a
    // parser binding registers — same contract as FileLoader's registrar
    // (forward to SessionManager::registerObjectTopicParser). May be invoked
    // from a toolbox worker thread.
    std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)> register_object_parser;
  };

  // The 4-arg overload (no parser-ingest deps) delegates to the 5-arg one —
  // GCC rejects `ParserIngestDeps{}` as a default argument here because the
  // nested struct's member initializers aren't usable until the enclosing
  // class is complete.
  ToolboxRuntimeHost(
      DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks);
  ToolboxRuntimeHost(
      DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks,
      ParserIngestDeps parser_ingest);
  // Out-of-line: parser_ingests_ holds unique_ptr<DataSourceRuntimeHost>
  // (incomplete here); also flushes any context the toolbox never released.
  ~ToolboxRuntimeHost();

  ToolboxRuntimeHost(const ToolboxRuntimeHost&) = delete;
  ToolboxRuntimeHost& operator=(const ToolboxRuntimeHost&) = delete;

  // Registers ToolboxHostService + ToolboxRuntimeHostService + SettingsStoreService
  // into the builder used to bind the toolbox plugin. Fails (without binding) when
  // a service the toolbox ABI requires could not be registered; a rejected
  // SettingsStore / object-read service only warns, since a toolbox runs without
  // them. Do not bind the plugin on a failed Status: the surface is not the one
  // this host meant to publish.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  // True when this host has (ever) created a parser-ingest context for
  // `dataset_id` — deliberately including finished and released ingests: the
  // source-promotion service asks AFTER a download completed, and the
  // per-dataset progress bookkeeping is retained until host teardown (see
  // ingest_progress_). Thread-safe.
  [[nodiscard]] bool hasIngestForDataset(DatasetId dataset_id) const;

  // True while this host owns at least one parser-ingest context created and not
  // yet released — i.e. the panel driving this host has work in flight. Unlike
  // hasIngestForDataset(), which stays true after release, this goes false again,
  // so a panel does not read busy forever after its first import. Thread-safe.
  [[nodiscard]] bool hasActiveIngests() const;

  // Flag-only cooperative stop for every live parser-ingest context — the
  // shell's "stop this import" affordance. Thread-safe. The plugin observes it
  // through is_stop_requested / progress_update returning false and ends the
  // import with its own keep-partial semantics: the toolbox ABI has no
  // host-side rollback, so there is no keep/discard choice on this path.
  void requestStopActiveIngests();

  // Minimum wall-clock between the flush+on_ingest_progress ticks driven by a
  // plugin's progress_update calls (test seam; production default 50 ms).
  // Plain int read by the ingest thread: call only BEFORE any ingest begins.
  void setFlushThrottleMs(int ms) {
    flush_throttle_ms_ = ms;
  }

 private:
  static void onReportMessage(void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept;
  static void onNotifyDataChanged(void* ctx) noexcept;
  static bool onCreateParserIngest(
      void* ctx, uint32_t data_source_id, PJ_data_source_runtime_host_t* out_host, PJ_error_t* out_error) noexcept;
  static bool onReleaseParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept;

  DatastoreToolboxHost write_host_;
  DatastoreToolboxObjectReadHost read_host_;
  sdk::SettingsStoreHost settings_host_;
  Callbacks callbacks_;
  DataEngine& engine_;
  ObjectStore& object_store_;
  ParserIngestDeps parser_ingest_deps_;
  // Guards map consistency among concurrent create/release calls and keeps the
  // teardown flush from interleaving with an in-flight create/release. It does
  // NOT make destruction safe against a still-running plugin worker: as with
  // report_message/notify_data_changed, the plugin must be unbound and its
  // worker quiesced before this host is destroyed ("valid until ... host
  // teardown" in toolbox_protocol.h). Concurrent create+release of the SAME id
  // is protocol-violating plugin behavior and out of contract. Mutable so the
  // const hasIngestForDataset query can lock it.
  mutable std::mutex parser_ingest_mu_;
  // One delegated-ingest session per toolbox-created dataset, keyed by the
  // data-source handle id (== DatasetId). Reuses the exact machinery file
  // loads use — catalog lookup, classifySchema, ObjectStore registration,
  // render-parser registrar — on a toolbox-created dataset.
  std::unordered_map<uint32_t, std::unique_ptr<DataSourceRuntimeHost>> parser_ingests_;
  // Progress bookkeeping shared with the hooks installed on a context's
  // DataSourceRuntimeHost. `total` backs on_ingest_progress; `last_flush` is
  // touched only on the plugin's ingest thread (single progress caller per
  // context, per the data-source protocol). Map guarded by parser_ingest_mu_;
  // entries are RETAINED until host teardown — release copies instead of
  // erasing, and a re-created context REUSES its dataset's entry. Eager
  // cleanup is deliberately absent: an undelivered terminal must stay
  // visible to the destructor's sweep, and after delivery a live re-created
  // context may hold the same entry at idle phase — indistinguishable from a
  // dead slot, so erasing would untrack it and break the shell's keyed
  // started/finished pairing. The per-notify scan cost is O(datasets),
  // which is small.
  struct IngestProgress {
    // Packed terminal-delivery word: low 2 bits = phase, high bits = the ARM
    // GENERATION (bumped by every progress_start). One word, one CAS per
    // transition, so a STALE queued finish can never consume a NEWER
    // sequence's terminal — a delivery CASes exactly the word its claim
    // produced, and any re-arm changed the generation. Transition table:
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
    // Claims never retry across generations (single-shot CAS): the RELEASE
    // path claims under parser_ingest_mu_ — a re-arm needs the same lock via
    // create — so it can only take its own context's sequence; the finish
    // hook claims lock-free, where a lost CAS means already-terminated or
    // superseded, both correctly no-ops.
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
  // Claim this sequence's terminal: single CAS (gen, kActive) ->
  // (gen, kFinishQueued). Returns the claimed word, or 0 when there is
  // nothing to claim — no active sequence (progressFinish without a start
  // stays a documented safe no-op), already claimed, or superseded by a
  // re-arm.
  [[nodiscard]] static uint64_t claimIngestFinish(IngestProgress& progress);
  // Marshal on_ingest_finished for a successful claim. The delivery CASes
  // exactly `claimed_word` back to (gen, kIdle) and fires only if it wins, so
  // a stale delivery can never steal a newer sequence's terminal; an
  // undelivered claim stays kFinishQueued for the destructor's sweep.
  void postIngestFinished(DatasetId dataset_id, const std::shared_ptr<IngestProgress>& progress, uint64_t claimed_word);
  // See setFlushThrottleMs.
  int flush_throttle_ms_ = 50;
  // Datasets that received a parser-ingest context since the last
  // notify_data_changed — drained (on the GUI thread) into the
  // on_data_changed callback so the host can focus playback on them.
  // Guarded by parser_ingest_mu_.
  std::vector<DatasetId> pending_ingest_datasets_;
  PJ_toolbox_runtime_host_vtable_t runtime_vtable_;
  PJ_toolbox_runtime_host_t runtime_;
  // The toolbox runtime-host vtable advertises report_message/notify_data_changed
  // as [thread-safe], so a plugin may call them from a worker thread. The
  // callbacks above touch Qt UI/session state, so the trampolines marshal them to
  // this QObject's thread (= the constructing/GUI thread) via Qt::AutoConnection:
  // same-thread calls run synchronously, cross-thread calls are queued.
  QObject marshaller_;
};

}  // namespace PJ
