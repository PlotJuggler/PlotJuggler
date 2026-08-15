#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
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
#include "pj_base/expected.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_plugins/sdk/object_ingest_policy.hpp"

namespace PJ {

class DataEngine;
class DatastoreParserWriteHost;
class ExtensionCatalogService;
class MessageParserHandle;
class ObjectIngestTapRegistry;
class ServiceRegistryBuilder;
struct LazyFetchTopicContext;

// Implements the host side of the v4 DataSource SDK for one ingest pass.
//
// Owns:
//   - the SourceWriteHostService that writes rows attributed to the source
//     itself,
//   - the runtime-host vtable (progress, stop, parser-bind, push-raw-message,
//     message-box stub, encoding-list helper),
//   - the set of parser bindings created on the fly when the source
//     delegates decoding to a MessageParser plugin.
//
// Lifetime is per-ingest: instantiate one, register its services into the
// builder used to bind the DataSource handle, run the plugin's start() loop,
// then call flushAll() to make written rows visible to readers. Destruction
// tears parser bindings down in the order required by the SDK contract
// (parser before write_host).
//
// Not a QObject — does not need signals or main-thread affinity and stays
// usable from headless or test contexts.
class DataSourceRuntimeHost {
 public:
  using ObjectTopicParserRegistrar = std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)>;

  // Wires the source-side write host immediately; the plugin's bind() sees
  // SourceWriteHostService + DataSourceRuntimeHostService via registerServices().
  //
  // `secondary_object_store` / `secondary_data_engine`, when non-null, enable
  // streaming dual-store / dual-engine routing: every object topic (resp.
  // scalar topic) this host registers is mirrored into the secondary in
  // lockstep, so both share the same ObjectTopicId (resp. TopicId) per topic
  // name. For scalar topics the mirror extends to the field level — the source
  // and per-binding write hosts get setSecondaryEngine(secondary_data_engine),
  // so each ensureField also lands the same FieldId on both engines (via
  // DataEngine::createTopicField) and a plugin's cached FieldHandle stays valid
  // across the swap. The manager then flips ingest between primary and secondary
  // via setObjectStoreTarget / setDataEngineTarget on each pause/resume. Both
  // null (file-load callers) → single-store behaviour, identical to before.
  // `library_keepalive` is intentionally NON-defaulting (see the member doc below): a silent
  // `{}` reintroduces the lazy-anchor use-after-dlclose crash, so every caller must pass the
  // DSO token (`handle.libraryOwner()`). The trailing optional params also lost their defaults
  // as a consequence — C++ forbids a non-defaulted parameter after a defaulted one — and every
  // construction site already passes them explicitly.
  DataSourceRuntimeHost(
      DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle,
      ObjectStore& object_store, std::string source_id, ObjectTopicParserRegistrar parser_registrar,
      ObjectStore* secondary_object_store, DataEngine* secondary_data_engine, std::shared_ptr<void> library_keepalive,
      std::shared_ptr<ObjectIngestTapRegistry> ingest_taps = nullptr);

  ~DataSourceRuntimeHost();

  DataSourceRuntimeHost(const DataSourceRuntimeHost&) = delete;
  DataSourceRuntimeHost& operator=(const DataSourceRuntimeHost&) = delete;

  // Signature: (type, title, message, buttons) → clicked button.
  // type and buttons use the PJ_message_box_type_t / PJ_MSG_BTN_* constants.
  // If not set, the host picks the positive button (headless mode).
  // THREADING: the handler is invoked synchronously on the thread that calls
  // show_message_box, which (per data_source_protocol.h) may be a worker/stream
  // thread — importData() runs on a background QThread on the single-instance
  // load path. The handler MUST therefore marshal any QWidget construction/use
  // to the GUI thread: open the dialog asynchronously there and block only this
  // calling worker thread for the answer (e.g. a semaphore the GUI completion
  // releases).
  // Building a QMessageBox directly here is a Qt thread-affinity violation, and
  // blocking the GUI thread instead would deadlock hosts whose event loop must
  // never wait. Same requirement as the on_progress_* hooks below.
  using MessageBoxHandler = std::function<int(int type, std::string_view title, std::string_view message, int buttons)>;
  void setMessageBoxHandler(MessageBoxHandler handler) {
    message_box_handler_ = std::move(handler);
  }

  // Registers SourceWriteHostService + DataSourceRuntimeHostService into the
  // builder used to bind the DataSource plugin. Both are required by the
  // DataSource ABI, so a rejection fails the Status; the object-write service is
  // optional (plugins publishing only scalars never resolve it) and merely warns.
  // Do not bind the plugin on a failed Status.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  // Fat pointer to this runtime host for handing across the C ABI outside of
  // registerServices() — ToolboxRuntimeHost's parser-ingest slots return it.
  // Valid only while this object lives.
  [[nodiscard]] PJ_data_source_runtime_host_t hostHandle() noexcept {
    return PJ_data_source_runtime_host_t{.ctx = this, .vtable = &kVtable};
  }

  // Flushes the source write host and every parser binding's write host.
  // Must be called once after handle.start() returns successfully so pending
  // rows reach the DataReader — open chunks are invisible until sealed, and
  // every parser binding has its own independent writer.
  void flushAll();

  // Non-terminal flush: commits rows written since the previous flush, safe to
  // call repeatedly. Used by long-lived ingest (streaming) to keep
  // work-in-progress visible to readers; flushAll() remains the terminal call.
  void flushPending();

  // Stop signalling for the plugin's cooperative-cancellation callbacks. The
  // reason is also recorded as the last error. Call this only from the SAME thread
  // that runs ingest — the last_error_ write is unsynchronized against fail().
  void requestStop(std::string_view reason);

  // Flag-only cooperative stop: sets the stop flag WITHOUT recording a reason, so it
  // touches only the atomic and cannot race the worker's fail()/last_error_ write.
  // Use this when signalling stop from a different thread than the ingest worker
  // (e.g. the GUI cancelling/joining a worker-thread file load).
  void requestStop();

  // Whether progress/stop callbacks should report a pending cancellation.
  bool stopRequested() const noexcept {
    return stop_requested_.load();
  }

  // Optional progress hooks — set before calling the plugin's start().
  // Called from the import thread; implementations must be thread-safe
  // (e.g. post to the main thread via QMetaObject::invokeMethod).
  // onProgressStart: label, total steps (0 = indeterminate), cancellable.
  // onProgressUpdate: current step; return false to cancel.
  std::function<void(std::string_view label, uint64_t total, bool cancellable)> on_progress_start;
  std::function<bool(uint64_t current)> on_progress_update;
  std::function<void()> on_progress_finish;

  // One topic from a notify_available_topics call, already classified a priori
  // (see cbNotifyAvailableTopics). Owned strings so on_available_topics can use
  // them after the worker->GUI marshal a caller performs inside the callback.
  struct AdvertisedTopicInfo {
    std::string topic_name;
    sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;
  };

  // Optional hook fired by the notify_available_topics vtable slot: the source
  // advertised (a subset of) the topics it can stream but has not subscribed to.
  // [poll/stream thread] — per data_source_protocol.h, the plugin calls
  // notify_available_topics on its own poll/stream thread, so this callback runs
  // there too; an implementation that touches Qt objects (CatalogModel,
  // TopicDemandTracker) MUST marshal to the GUI thread itself (mirror the
  // samplesIngested hop in StreamingSourceManager), exactly like on_progress_*
  // above. Set before calling the plugin's start().
  std::function<void(std::vector<AdvertisedTopicInfo> topics)> on_available_topics;

  // Most recent error message captured by any callback. Empty if none.
  const std::string& lastError() const noexcept {
    return last_error_;
  }

  [[nodiscard]] sdk::ObjectIngestPolicyResolver& policyResolver() noexcept {
    return policy_resolver_;
  }

  [[nodiscard]] const sdk::ObjectIngestPolicyResolver& policyResolver() const noexcept {
    return policy_resolver_;
  }

  /// Number of tap callbacks that threw. The host contains these exceptions so
  /// scalar parsing and ObjectStore registration still complete.
  [[nodiscard]] uint64_t ingestTapFailures() const noexcept {
    return ingest_tap_failures_.load();
  }

  // Apply a (time_window, max_memory) budget to every bound object topic, on the
  // *active* store (A live / B paused, per object_store_target_): bounds the
  // paused tail B while leaving the frozen primary untouched.
  void setObjectRetentionBudget(int64_t time_window_ns, size_t max_memory_bytes);

  // Atomically retarget every object write host (source-level + per-parser-
  // binding) at `target`, swapping ingest between primary and secondary store
  // on pause/resume. Bound ObjectTopicIds stay valid because every
  // registerTopic is mirrored to the secondary at registration time.
  void setObjectStoreTarget(ObjectStore* target);

  // Scalar-write analogue of setObjectStoreTarget: retargets the source-level
  // host and every parser binding. Bound topic handles stay valid via the same
  // lockstep-mirror guarantee.
  void setDataEngineTarget(DataEngine* target);

  // Secondary ObjectStore wired at construction (nullptr for single-store
  // callers). The streaming manager uses it for the resume flush and to
  // confirm a real secondary was wired at startSession.
  [[nodiscard]] ObjectStore* secondaryObjectStore() noexcept {
    return secondary_object_store_;
  }

  // Secondary DataEngine wired at construction (nullptr for single-engine
  // callers). Mirrors secondaryObjectStore.
  [[nodiscard]] DataEngine* secondaryDataEngine() noexcept {
    return secondary_data_engine_;
  }

 private:
  // ----- C-ABI callbacks -----
  // Each casts ctx to DataSourceRuntimeHost* and accesses members directly.
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

  // A-priori classification for one advertised topic: binds the schema against a
  // throwaway parser instance for `topic.parser_encoding` and calls classifySchema
  // (no bind(registry) — classification needs no write-host service; cbEnsureParserBinding
  // already proves the ABI accepts bindSchema/classifySchema before bind()). Falls back to
  // matching `type_name` against the FrameTransforms/CameraInfo infra schemas when no parser
  // is registered for the encoding, or the parser's classify_schema returns kNone (e.g. an
  // older parser .so, or a genuinely unclassified type). Never throws.
  [[nodiscard]] sdk::BuiltinObjectType classifyAvailableTopic(const PJ_available_topic_t& topic) const noexcept;

  // The vtable referenced by every PJ_data_source_runtime_host_t handed to a
  // plugin. Definition in the cpp — single static instance shared by every
  // session.
  static const PJ_data_source_runtime_host_vtable_t kVtable;

  // Centralised failure path used by every callback that signals an error.
  // Stores `message` into last_error_ and fills `out_error` for the plugin.
  bool fail(PJ_error_t* out_error, const char* message) noexcept;

  // One parser binding owned by the host. Destruction order is load-bearing:
  // the parser may flush pending writes through `write_host` when destroyed,
  // so the parser must die BEFORE `write_host`. The registry builder only
  // supplies fat pointers at bind time — afterwards the plugin holds its own
  // copies, so the builder can die first.
  struct ParserBinding {
    // Identity of the ensure request that created this binding. A demand-driven
    // plugin drops its binding cache with the subscription and re-requests on
    // re-subscribe; an identical signature hands back the existing binding — a
    // second createTopic would register a duplicate engine topic with the same
    // name, doubling every field in the catalog. Any difference is a genuine
    // retype and mints a fresh binding.
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
    // Per-binding constants for lazy-fetch failure logs, shared by every lazy
    // closure this binding mints (one per object message) instead of copied
    // into each. Created on first use in cbPushMessage [worker thread].
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

  // The existing binding id for (topic_name, signature), or nullopt when none
  // matches (first bind, or a retype). [worker-thread] — same poll thread as
  // cbEnsureParserBinding/cbPushMessage; parser_bindings_ is unsynchronized.
  [[nodiscard]] std::optional<uint32_t> findReusableBinding(
      std::string_view topic_name, const ParserBinding::Signature& signature) const;

  DataEngine& engine_;
  ExtensionCatalogService& catalog_;
  ObjectStore& object_store_;
  // Active target for cbPushMessage's lazy-object push: primary (A) while live,
  // secondary (B) while paused (swapped by setObjectStoreTarget). Atomic: UI
  // thread writes, worker thread reads. Keeps A unwritten — thus unevicted —
  // while paused.
  std::atomic<ObjectStore*> object_store_target_{&object_store_};
  // Lockstep mirror of `object_store_` for the streaming dual-store flow (null
  // for single-store callers). See the constructor doc for the id-sharing
  // invariant that lets write hosts retarget between the two.
  ObjectStore* secondary_object_store_ = nullptr;
  // Scalar-write analogue of object_store_target_: the active DataEngine a new
  // parser binding must write into — primary (A) while live, secondary (B) while
  // paused (swapped by setDataEngineTarget). A binding CREATED while paused (a
  // topic first seen mid-session) must init here, not at the primary, or its
  // samples land on the frozen engine and drag the global timeline. Atomic: UI
  // thread swaps, worker thread reads at bind time.
  std::atomic<DataEngine*> data_engine_target_{&engine_};
  // Lockstep mirror of `engine_` for the streaming dual-engine flow (null for
  // single-engine callers); same id-sharing invariant as above.
  DataEngine* secondary_data_engine_ = nullptr;
  std::string source_id_;
  ObjectTopicParserRegistrar object_topic_parser_registrar_;
  sdk::ObjectIngestPolicyResolver policy_resolver_;
  DatasetId dataset_id_;
  DatastoreSourceWriteHost source_write_host_;
  DatastoreSourceObjectWriteHost source_object_write_host_;
  // Shared by lazy ObjectStore closures. Some source plugins wrap readers
  // whose deferred message fetch API is not safe to call concurrently.
  std::shared_ptr<std::mutex> lazy_fetch_mutex_;
  // Keeps the producing DataSource plugin's DSO mapped for as long as any lazy
  // ObjectStore payload anchor created by this host survives: a payload anchor's
  // release fn is plugin code, so a cached ResolvedObjectEntry outliving the
  // extension catalog would otherwise call a dangling pointer on teardown.
  std::shared_ptr<void> library_keepalive_;
  // Shared session registry. Its callbacks run synchronously while
  // cbPushMessage still owns the payload anchor.
  std::shared_ptr<ObjectIngestTapRegistry> ingest_taps_;
  std::atomic<uint64_t> ingest_tap_failures_{0};

  MessageBoxHandler message_box_handler_;
  std::string last_error_;
  std::atomic<bool> stop_requested_{false};
  uint32_t next_binding_id_ = 1;
  std::unordered_map<uint32_t, ParserBinding> parser_bindings_;
  // Backing for cbListAvailableEncodings — the protocol contract is that the
  // returned pointer is valid until the next call, so the buffer outlives the
  // function return.
  std::string available_encodings_cache_;
  bool flushed_ = false;
};

}  // namespace PJ
