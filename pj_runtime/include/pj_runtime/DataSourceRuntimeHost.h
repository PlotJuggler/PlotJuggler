#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/sdk/ingest_completion.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_plugins/sdk/object_ingest_policy.hpp"
#include "pj_runtime/RecordTap.h"

namespace PJ {

class DataEngine;
class DataSourceHandle;
class DatastoreParserWriteHost;
class ExtensionCatalogService;
class MessageParserHandle;
class ObjectIngestTapRegistry;
class ServiceRegistryBuilder;
struct LazyFetchTopicContext;

// The host side of the DataSource SDK for one ingest pass: the source's own write host, the runtime-host vtable, and
// the parser bindings created when the source delegates decoding to a MessageParser plugin.
//
// One per ingest: bind() the DataSource handle, run the plugin, then flushAll() so the rows become reader-visible. Not
// a QObject: usable headless and in tests.
class DataSourceRuntimeHost {
 public:
  using ObjectTopicParserRegistrar = std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)>;

  // `secondary_object_store` / `secondary_data_engine`, when non-null, enable the streaming pause flow: every object
  // and scalar topic this host registers is mirrored into the secondary in lockstep, so both share the ObjectTopicId /
  // TopicId per name, and for scalars the FieldId too (DataEngine::createTopicField), so a plugin's cached FieldHandle
  // stays valid when setObjectStoreTarget / setDataEngineTarget swap ingest between them. Both null: one store.
  // `library_keepalive` (`handle.libraryOwner()`) has no default on purpose: an empty one brings back the lazy-anchor
  // use-after-dlclose (see library_keepalive_).
  DataSourceRuntimeHost(
      DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle,
      ObjectStore& object_store, std::string source_id, ObjectTopicParserRegistrar parser_registrar,
      ObjectStore* secondary_object_store, DataEngine* secondary_data_engine, std::shared_ptr<void> library_keepalive,
      std::shared_ptr<ObjectIngestTapRegistry> ingest_taps = nullptr);

  ~DataSourceRuntimeHost();

  DataSourceRuntimeHost(const DataSourceRuntimeHost&) = delete;
  DataSourceRuntimeHost& operator=(const DataSourceRuntimeHost&) = delete;

  // (type, title, message, buttons) -> clicked button, with the PJ_message_box_type_t / PJ_MSG_BTN_* constants.
  // Unset: the positive button (headless). Runs synchronously on the thread calling show_message_box, which may be a
  // worker or stream thread: the handler opens the dialog on the GUI thread and blocks only this caller for the
  // answer. A QMessageBox built here violates Qt thread affinity; blocking the GUI thread instead deadlocks.
  using MessageBoxHandler = std::function<int(int type, std::string_view title, std::string_view message, int buttons)>;
  void setMessageBoxHandler(MessageBoxHandler handler) {
    message_box_handler_ = std::move(handler);
  }

  static constexpr std::size_t kMaxForwardedMessages = 20;

  // Forwards the plugin's WARNING/ERROR report_message (never INFO) to `sink` as Diagnostic{source,
  // "data-source-message", "<subject>: <text>"}; every level still goes to the log. After kMaxForwardedMessages per
  // host, one "data-source-message-limit" warning, then nothing. The sink runs on the plugin's reporting thread, so it
  // must be thread-safe (QtDiagnosticBridge::sink is). Set before registerServices(); not synchronized.
  void setDiagnosticSink(DiagnosticSink sink, std::string source, std::string subject) {
    diagnostic_sink_ = std::move(sink);
    diagnostic_source_ = std::move(source);
    diagnostic_subject_ = std::move(subject);
  }

  // The recording hook: called inline on the push thread with every pushed message's raw bytes, before the parser
  // runs, plus a view of the message's binding, so a recording started mid-stream captures topics subscribed long
  // before. Any thread may set or clear it; each call runs on a shared_ptr snapshot, so a replaced tap finishes its
  // call. A tap returning kStopRecording (or throwing) is dropped. Pure-lazy object pushes fetch no bytes, so they are
  // counted (recordTapSkippedLazy()), not recorded: a source keeping a type pure-lazy records a hole.
  // Not ObjectIngestTapRegistry: that one routes (it decides pushLazy vs pushLazyWithSeed) and is object-only; this
  // observes every message and never changes what ingest does.
  void setRecordTap(std::shared_ptr<RecordTap> tap);

  // Detaches `tap` unless a newer one was installed: the session recorder and the capture service share the slot.
  void dropRecordTap(const std::shared_ptr<RecordTap>& tap);

  /// Messages a tap could not see because their payload was fetched lazily off the push thread.
  [[nodiscard]] uint64_t recordTapSkippedLazy() const noexcept;

  // ---- Source capture. This instance is the ingest generation: attach and completion state die with it, so a stale
  // terminal never blesses a replacement ingest on the same dataset. Read the accessors after the stream thread was
  // joined.

  /// The descriptor the source attached (attach_source_record), verbatim; the last attachment before the first push
  /// wins. nullopt: never attached, not cacheable.
  [[nodiscard]] std::optional<std::string> sourceRecordDescriptor() const;

  /// The validated complete_ingest terminal; nullopt (none arrived) means not cacheable.
  [[nodiscard]] std::optional<sdk::IngestCompletionRecord> ingestCompletion() const;

  /// Why this ingest can never publish a capture (empty: no veto): a malformed or conflicting completion, an attach
  /// after the first push or after sealing. Permanent.
  [[nodiscard]] std::string captureVetoReason() const;

  /// Stages the loader-metadata document (set_dataset_metadata), replacing the previous one; allowed any time during
  /// import. Thread-safe. Published into SessionManager at the dataset's commit seam, which enforces the bounds; a
  /// rejected document never affects the ingest.
  void stageLoaderMetadata(std::string json);

  [[nodiscard]] std::optional<std::string> stagedLoaderMetadata() const;

  /// fail() calls (rejected pushes, parser bind errors, failed fetches). Ingest may still have succeeded, but the
  /// recorded bytes then disagree with the dataset, so a capture requires zero.
  [[nodiscard]] uint64_t ingestCallbackFailures() const noexcept {
    return ingest_callback_failures_.load(std::memory_order_relaxed);
  }

  /// Messages whose bytes were offered to the record tap (eager pushes with a successful fetch), with or without a
  /// tap. A capture compares its ledger against it, so a tap dropped mid-stream shows as a mismatch, not a hole.
  [[nodiscard]] uint64_t tapEligibleMessages() const noexcept {
    return tap_eligible_messages_.load(std::memory_order_relaxed);
  }

  /// Fired on the stream thread, outside the capture lock, after a source record was stored: (descriptor, replaced an
  /// earlier one). The capture service opens its cache transaction here. Any thread may set or clear it; a call
  /// already snapshotted may run after clearing, so its captures must keep alive what they touch (weak_ptr).
  void setSourceRecordAttachedHook(std::function<void(std::string_view descriptor, bool replaced)> hook);

  /// Fired by both requestStop overloads, any thread, after the flag is set: a capture owner wakes producers blocked on
  /// a lossless recorder queue here, before anything joins them (or the join deadlocks). Rules as above.
  void setStopRequestedHook(std::function<void()> hook);

  // Registers SourceWriteHostService and DataSourceRuntimeHostService (required: a rejection fails the Status; do not
  // bind the plugin then) and the optional object-write and, once bind() was given a store, settings services (a
  // rejection only warns).
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  // Registers these services, with `settings` as pj.settings.v1 when given, in a registry this host owns and binds
  // `handle` to it; once per host. The SDK's DataSourcePluginBase keeps that registry (services()) and views into its
  // services after bind(), up to its destructor, so this host must outlive `handle`: an owner declares the host
  // before the handle. Errors read "service registration failed: ..." or "bind failed: ...".
  [[nodiscard]] Status bind(DataSourceHandle& handle, std::unique_ptr<sdk::SettingsBackend> settings = nullptr);

  // This host across the C ABI outside registerServices() (ToolboxRuntimeHost's parser-ingest slots). Valid while
  // this object lives.
  [[nodiscard]] PJ_data_source_runtime_host_t hostHandle() noexcept {
    return PJ_data_source_runtime_host_t{.ctx = this, .vtable = &kVtable};
  }

  // The terminal flush of the source and every parser binding's writer; call once after start() succeeds. Open chunks
  // are invisible until sealed.
  void flushAll();

  // Commits rows written since the previous flush; for long-lived ingest. flushAll() stays the terminal call.
  void flushPending();

  // Stop for a failure that ends ingest: `reason` becomes stopFailure(); the first failure wins (a later one is a
  // consequence). Any thread: the reason has its own lock and never touches last_error_.
  void requestStop(std::string_view reason);

  // Flag-only stop for an orderly end (GUI cancel, end of stream); stopFailure() stays empty. Any thread.
  void requestStop();

  bool stopRequested() const noexcept {
    return stop_requested_.load();
  }

  // Set before start(). Called from the import thread: implementations marshal to the GUI thread themselves.
  // on_progress_start: label, total (0 = indeterminate), cancellable. on_progress_update returns false to cancel.
  std::function<void(std::string_view label, uint64_t total, bool cancellable)> on_progress_start;
  std::function<bool(uint64_t current)> on_progress_update;
  std::function<void()> on_progress_finish;

  // One advertised topic, classified up front. Owned strings, so they survive the caller's worker-to-GUI marshal.
  struct AdvertisedTopicInfo {
    std::string topic_name;
    sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;
  };

  // The source advertised (some of) the topics it can stream but has not subscribed to. Runs on the plugin's poll or
  // stream thread: an implementation touching Qt objects (CatalogModel, TopicDemandTracker) marshals to the GUI thread
  // itself. Set before start().
  std::function<void(std::vector<AdvertisedTopicInfo> topics)> on_available_topics;

  // The reason ingest ended in failure: requestStop(reason) from a poll error or the plugin's FAILED request_stop.
  // Empty after an orderly end, whatever fail() recorded: a recovered callback failure must not mark a recording of
  // this source lossy. Any thread; meaningful once ingest has stopped.
  [[nodiscard]] std::string stopFailure() const;

  // stopFailure(), else the last callback failure (possibly recovered from). Only after ingest stopped: last_error_ is
  // written unsynchronized on the stream thread.
  [[nodiscard]] std::string lastError() const;

  // The last WARNING or ERROR text the plugin passed to report_message since forgetReportedProblem(), empty if none:
  // why an import that succeeded may still have produced nothing. Any thread.
  [[nodiscard]] std::string lastReportedProblem() const;

  // Call right before start(): a warning from loadConfig or the dialog is not a reason the import found nothing.
  void forgetReportedProblem();

  [[nodiscard]] sdk::ObjectIngestPolicyResolver& policyResolver() noexcept {
    return policy_resolver_;
  }

  [[nodiscard]] const sdk::ObjectIngestPolicyResolver& policyResolver() const noexcept {
    return policy_resolver_;
  }

  /// Tap callbacks that threw; the host contains them so parsing and registration still complete.
  [[nodiscard]] uint64_t ingestTapFailures() const noexcept {
    return ingest_tap_failures_.load();
  }

  // Applies to every bound object topic on the active store (the primary live, the secondary paused), so a pause
  // bounds the paused tail and leaves the frozen primary alone.
  void setObjectRetentionBudget(int64_t time_window_ns, size_t max_memory_bytes);

  // Atomically retargets every object write host (source and bindings) on pause/resume. Bound ids stay valid: every
  // registerTopic is mirrored at registration.
  void setObjectStoreTarget(ObjectStore* target);

  void setDataEngineTarget(DataEngine* target);

  // nullptr for single-store callers.
  [[nodiscard]] ObjectStore* secondaryObjectStore() noexcept {
    return secondary_object_store_;
  }

  [[nodiscard]] DataEngine* secondaryDataEngine() noexcept {
    return secondary_data_engine_;
  }

 private:
  static void cbReportMessage(void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept;
  static bool cbProgressStart(
      void* ctx, PJ_string_view_t label, uint64_t total, bool cancellable, PJ_error_t* out_error) noexcept;
  static bool cbProgressUpdate(void* ctx, uint64_t current) noexcept;
  static void cbProgressFinish(void* ctx) noexcept;
  static bool cbIsStopRequested(void* ctx) noexcept;
  static void cbNotifyState(void* ctx, PJ_data_source_state_t state) noexcept;
  static void cbRequestStop(void* ctx, PJ_data_source_state_t terminal, PJ_string_view_t reason) noexcept;
  static bool cbEnsureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out,
      PJ_error_t* out_error) noexcept;
  static bool cbPushMessage(
      void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_message_data_fetcher_t fetch_message_data,
      PJ_error_t* out_error) noexcept;
  static int cbShowMessageBox(
      void* ctx, PJ_message_box_type_t type, PJ_string_view_t title, PJ_string_view_t message, int buttons) noexcept;
  static const char* cbListAvailableEncodings(void* ctx) noexcept;
  static bool cbNotifyAvailableTopics(
      void* ctx, const PJ_available_topic_t* topics, uint64_t count, PJ_error_t* out_error) noexcept;
  static bool cbAttachSourceRecord(void* ctx, PJ_string_view_t descriptor_json, PJ_error_t* out_error) noexcept;
  static bool cbCompleteIngest(void* ctx, const PJ_ingest_completion_t* completion, PJ_error_t* out_error) noexcept;
  static bool cbSetDatasetMetadata(void* ctx, PJ_string_view_t metadata_json, PJ_error_t* out_error) noexcept;

  // The object route's type through ExtensionCatalogService::classifyParserObjectRoute (no bind needed), else
  // `type_name` against the FrameTransforms/CameraInfo schemas (no parser for the encoding, or it classifies kNone).
  [[nodiscard]] sdk::BuiltinObjectType classifyAvailableTopic(const PJ_available_topic_t& topic) const noexcept;

  // Shared by every handle given to a plugin.
  static const PJ_data_source_runtime_host_vtable_t kVtable;

  // Records `message` in last_error_ and fills `out_error`; the plugin decides whether it is fatal. A terminal failure
  // reaches stopFailure() through requestStop(reason) only.
  bool fail(PJ_error_t* out_error, const char* message) noexcept;

  // fail() without the failure latch: a refused attachment only means "no caching", and the provider may re-attach
  // before the first push.
  bool rejectAttachment(PJ_error_t* out_error, const std::string& message) noexcept;

  // Destruction order: the parser may flush through `write_host` when destroyed and keeps the registry bind() received,
  // so it is declared after both (destroyed first).
  struct ParserBinding {
    // The ensure request's identity. A demand-driven plugin re-requests its binding on every re-subscribe; an identical
    // signature gets the existing binding, since a second createTopic would duplicate every field in the catalog. Any
    // difference is a retype and mints a fresh binding.
    struct Signature {
      std::string encoding;
      std::string type_name;
      std::string schema_bytes;
      std::string parser_config;
      bool operator==(const Signature&) const = default;
    };

    std::unique_ptr<ServiceRegistryBuilder> registry_builder;
    std::unique_ptr<DatastoreParserWriteHost> write_host;
    std::unique_ptr<DatastoreParserObjectWriteHost> object_write_host;
    std::unique_ptr<MessageParserHandle> parser;
    std::string topic_name;
    Signature signature;
    sdk::BuiltinObjectType object_kind = sdk::BuiltinObjectType::kNone;
    std::optional<ObjectTopicId> object_topic_id;
    // An object route and no scalar route (ParserRouteSelection::scalar_winner_object_only): a failing scalar parse is
    // expected and keeps the object.
    bool object_only = false;
    bool object_only_logged = false;  // worker thread; one trace per binding
    // Shared by the lazy closures of this binding's messages for fetch-failure logs; created on first use (worker).
    std::shared_ptr<const LazyFetchTopicContext> lazy_fetch_context;

    ParserBinding();
    ParserBinding(
        std::unique_ptr<ServiceRegistryBuilder> b, std::unique_ptr<DatastoreParserWriteHost> w,
        std::unique_ptr<DatastoreParserObjectWriteHost> ow, std::unique_ptr<MessageParserHandle> p, std::string topic,
        Signature sig, sdk::BuiltinObjectType kind, std::optional<ObjectTopicId> object_topic);
    ~ParserBinding();

    ParserBinding(ParserBinding&&) noexcept;
    ParserBinding& operator=(ParserBinding&&) noexcept;
  };

  // nullopt on a first bind or a retype. Worker thread, as the push path: parser_bindings_ is unsynchronized.
  [[nodiscard]] std::optional<uint32_t> findReusableBinding(
      std::string_view topic_name, const ParserBinding::Signature& signature) const;

  DataEngine& engine_;
  ExtensionCatalogService& catalog_;
  ObjectStore& object_store_;
  // Primary while live, secondary while paused, so the primary is not written (nor evicted) during a pause. Written by
  // the GUI thread, read by the worker.
  std::atomic<ObjectStore*> object_store_target_{&object_store_};
  // Null for single-store callers.
  ObjectStore* secondary_object_store_ = nullptr;
  // Where a new parser binding writes: primary live, secondary paused. A binding created during a pause must init
  // here, or its samples land on the frozen engine and stretch the timeline. GUI writes, worker reads.
  std::atomic<DataEngine*> data_engine_target_{&engine_};
  DataEngine* secondary_data_engine_ = nullptr;
  std::string source_id_;
  ObjectTopicParserRegistrar object_topic_parser_registrar_;
  sdk::ObjectIngestPolicyResolver policy_resolver_;
  DatasetId dataset_id_;
  DatastoreSourceWriteHost source_write_host_;
  DatastoreSourceObjectWriteHost source_object_write_host_;
  // bind()'s store, before registry_ (destroyed after it); settings_host_ refers to settings_.
  std::unique_ptr<sdk::SettingsBackend> settings_;
  std::optional<sdk::SettingsStoreHost> settings_host_;
  std::unique_ptr<ServiceRegistryBuilder> registry_;  // bind()'s; heap, as the plugin holds its address
  // Shared by the lazy closures: some sources wrap readers whose deferred fetch is not safe concurrently.
  std::shared_ptr<std::mutex> lazy_fetch_mutex_;
  // Keeps the plugin DSO mapped while any lazy payload anchor of this host lives: an anchor's release function is
  // plugin code, and a cached ResolvedObjectEntry can outlive the extension catalog.
  std::shared_ptr<void> library_keepalive_;
  // Its callbacks run synchronously while cbPushMessage owns the payload anchor.
  std::shared_ptr<ObjectIngestTapRegistry> ingest_taps_;
  std::atomic<uint64_t> ingest_tap_failures_{0};

  // Guards only the slot: calls run on a snapshot taken outside it, and a replaced tap is released after unlocking,
  // since dropping the last reference to a Recorder joins its writer thread.
  mutable std::mutex record_tap_mu_;
  std::shared_ptr<RecordTap> record_tap_;
  // "record_tap_ != nullptr", written under record_tap_mu_ and read relaxed without it, so the push path costs nothing
  // while nothing records. A hint, stale either way harmlessly: false just after attaching misses a message pushed in
  // that window (as pressing Record a moment later); true just after detaching costs one mutex round-trip and may count
  // one pure-lazy skip, so record_tap_skipped_lazy_ is +-1 at attach/detach (read it before detaching). Nothing acts on
  // the flag alone; recordTapSnapshot() is authoritative.
  std::atomic<bool> record_tap_active_{false};
  std::atomic<uint64_t> record_tap_skipped_lazy_{0};

  [[nodiscard]] std::shared_ptr<RecordTap> recordTapSnapshot() const;
  // False: no tap, skip the mutex. True is a hint; take recordTapSnapshot().
  [[nodiscard]] bool hasRecordTap() const noexcept {
    return record_tap_active_.load(std::memory_order_relaxed);
  }

  // A tap that throws or returns kStopRecording is detached here.
  void recordMessage(
      const std::shared_ptr<RecordTap>& tap, uint32_t binding_id, const ParserBinding& binding, int64_t timestamp_ns,
      const uint8_t* data, uint64_t size);

  MessageBoxHandler message_box_handler_;
  DiagnosticSink diagnostic_sink_;
  std::atomic<std::size_t> forwarded_messages_{0};
  std::string diagnostic_source_;
  std::string diagnostic_subject_;
  // fail()'s rolling diagnostic: stream thread only, unsynchronized; read after joining. Never a verdict on ingest.
  std::string last_error_;
  // Its own lock: request_stop is thread-safe in the ABI. Off the push path.
  mutable std::mutex stop_failure_mu_;
  std::string stop_failure_;
  // Guarded by stop_failure_mu_ too: report_message is rare and may come from any plugin thread.
  std::string last_reported_problem_;
  std::atomic<bool> stop_requested_{false};
  uint32_t next_binding_id_ = 1;
  std::unordered_map<uint32_t, ParserBinding> parser_bindings_;
  // First decoder provider per object topic. A retype picking another provider stays scalar-only, so old lazy entries
  // are never re-decoded by another implementation.
  std::unordered_map<uint64_t, std::string> object_topic_decoder_providers_;
  // cbListAvailableEncodings' pointer is valid until the next call (protocol contract).
  std::string available_encodings_cache_;
  bool flushed_ = false;

  // ---- Capture state: one mutex, control plane only (never per message). ingest_begun_ ("a push happened") is
  // written relaxed on the push path and read only under capture_mu_.
  mutable std::mutex capture_mu_;
  std::optional<std::string> source_record_;
  std::optional<std::string> loader_metadata_;
  std::optional<sdk::IngestCompletionRecord> completion_;
  std::string capture_veto_;
  std::atomic<bool> ingest_begun_{false};
  /// Relaxed mirror of "completion_ has a value" for the push path. The ABI gives pushes one stream thread; the atomic
  /// keeps a misbehaving multi-threaded plugin defined rather than racy.
  std::atomic<bool> ingest_sealed_{false};
  std::atomic<uint64_t> ingest_callback_failures_{0};
  std::atomic<uint64_t> tap_eligible_messages_{0};
  /// Guards only the hook slots; hooks run on copies, never under it.
  mutable std::mutex hooks_mu_;
  std::function<void(std::string_view, bool)> on_source_record_attached_;
  std::function<void()> on_stop_requested_;
};

}  // namespace PJ
