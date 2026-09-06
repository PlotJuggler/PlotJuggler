// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataSourceRuntimeHost.h"

#include <fmt/format.h>

#include <QLoggingCategory>
#include <QString>
#include <algorithm>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/source/record_envelope.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ObjectIngestTap.h"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/detail/payload_anchor.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace detail {

sdk::BufferAnchor wrapPayloadAnchor(const PJ_payload_anchor_t& anchor, std::shared_ptr<void> library_keepalive) {
  if (anchor.release == nullptr) {
    return {};
  }
  // Host-side deleter (always mapped): calls the plugin's release while holding
  // `library_keepalive`, so the producing DSO stays mapped for the call and is
  // dlclosed only once every anchor copy is destroyed.
  auto release = anchor.release;
  return sdk::BufferAnchor{std::shared_ptr<void>(
      anchor.ctx, [release, keepalive = std::move(library_keepalive)](void* ctx) noexcept { release(ctx); })};
}

}  // namespace detail

// Per-binding constants for the lazy-fetch failure logs. Shared (via one
// shared_ptr per binding, created on first use) by every lazy closure that
// binding mints — a load creates one closure per object message, so
// per-message copies of these strings would be pure waste. Namespace-scope to
// match the forward declaration in DataSourceRuntimeHost.h.
struct LazyFetchTopicContext {
  DatasetId dataset_id = 0;
  ObjectTopicId object_topic_id{};
  std::string source_id;
  std::string topic_name;
};

namespace {
Q_LOGGING_CATEGORY(lcIngest, "pj.runtime.ingest")
// Off by default; enable with QT_LOGGING_RULES="pj.runtime.ingest.bind=true"
// to trace how each topic's parser binding was resolved.
Q_LOGGING_CATEGORY(lcIngestBind, "pj.runtime.ingest.bind", QtWarningMsg)

std::vector<uint8_t> copyPayloadBytes(const PJ_payload_t& payload) {
  std::vector<uint8_t> bytes;
  if (payload.size > 0 && payload.data != nullptr) {
    bytes.assign(payload.data, payload.data + payload.size);
  }
  return bytes;
}

// Idempotent resident closure (kEager only) that replays the same PayloadView
// on every read. `anchor` is the payload's already-wrapped upstream anchor:
// non-null, the closure inherits it so the buffer lives for the ObjectStore
// entry's lifetime (zero copy); null (transient buffer), the bytes are copied
// into a shared_ptr<vector> that serves as its own anchor.
std::function<sdk::PayloadView()> makeCapturedPayloadClosure(const PJ_payload_t& payload, sdk::BufferAnchor anchor) {
  if (anchor == nullptr) {
    auto bytes = std::make_shared<const std::vector<uint8_t>>(copyPayloadBytes(payload));
    return [bytes]() -> sdk::PayloadView {
      return sdk::PayloadView{
          Span<const uint8_t>{bytes->data(), bytes->size()},
          sdk::BufferAnchor{bytes},
      };
    };
  }
  const uint8_t* data = payload.data;
  uint64_t size = payload.size;
  return [anchor = std::move(anchor), data, size]() -> sdk::PayloadView {
    return sdk::PayloadView{
        Span<const uint8_t>{data, static_cast<size_t>(size)},
        anchor,
    };
  };
}

struct FetcherOwner {
  FetcherOwner(PJ_message_data_fetcher_t fetcher_in, std::shared_ptr<void> library_keepalive_in)
      : library_keepalive(std::move(library_keepalive_in)), fetcher(fetcher_in) {}

  FetcherOwner(const FetcherOwner&) = delete;
  FetcherOwner& operator=(const FetcherOwner&) = delete;

  ~FetcherOwner() {
    // fetcher.release is plugin-DSO code. The destructor BODY runs while
    // library_keepalive is still held (members are destroyed only after the
    // body completes), so the producing .so cannot be dlclosed underneath this
    // call — even when this owner holds the LAST DSO reference (post-evict /
    // app close). Hold the keepalive as a member here; do NOT rely on a
    // separate lambda capture, whose destruction order relative to this owner
    // is unspecified and would let the DSO unmap before fetcher.release runs.
    if (fetcher.release != nullptr) {
      fetcher.release(fetcher.ctx);
    }
  }

  // The producing plugin's DSO token, held for the owner's whole lifetime.
  std::shared_ptr<void> library_keepalive;
  PJ_message_data_fetcher_t fetcher;
};

QString errorMessage(const PJ_error_t& err) {
  if (err.message[0] == '\0') {
    return u"<none>"_s;
  }
  return QString::fromUtf8(err.message);
}

// Deferred lazy closure: re-invokes the fetcher on every read, wrapping each
// invocation's anchor in a per-call shared_ptr so a returned PayloadView can
// outlive the call without holding the fetcher. Keeps object bytes
// non-resident — used by kPureLazy, and by kLazyObjectsEagerScalars once the
// ingest-time scalar parse is done with them.
//
// Returns nullopt when the source cannot re-produce usable bytes, so the
// ObjectStore flags the entry fetch_failed instead of conflating failure with
// a legitimately empty payload. A zero-byte result IS a failure here: the SDK's
// pushMessage glue reports ok=true for a plugin-side fetch that produced an
// empty view (it only reports false on an exception), so "no bytes" is how a
// file-source cold-path failure actually arrives across the C ABI — and no
// builtin object decodes from zero bytes anyway. The cost of that bias is a
// possible false LOST attribution for a payload that is legitimately zero
// bytes (e.g. an empty FrameTransforms set serializes to 0 bytes); removing
// the ambiguity needs a real failure signal in the SDK pushMessage glue,
// which is an SDK change (tracked follow-up).
PJ::LazyCallback makeLazyFetchClosure(
    std::shared_ptr<FetcherOwner> owner, std::shared_ptr<std::mutex> fetch_mutex,
    std::shared_ptr<const LazyFetchTopicContext> context, int64_t timestamp_ns) {
  // The DSO keepalive lives inside `owner` (FetcherOwner), so it is the single
  // source of truth here too — reach it via owner->library_keepalive when
  // wrapping each fetched anchor, rather than a parallel capture.
  return [owner = std::move(owner), fetch_mutex = std::move(fetch_mutex), context = std::move(context),
          timestamp_ns]() -> std::optional<sdk::PayloadView> {
    PJ_payload_t payload{};
    PJ_error_t err{};
    bool ok = false;
    if (owner->fetcher.fetchMessageData != nullptr) {
      if (fetch_mutex != nullptr) {
        std::lock_guard lock(*fetch_mutex);
        ok = owner->fetcher.fetchMessageData(owner->fetcher.ctx, &payload, &err);
      } else {
        ok = owner->fetcher.fetchMessageData(owner->fetcher.ctx, &payload, &err);
      }
    }
    if (!ok) {
      qCWarning(lcIngest) << "[lazy-fetch] failed source=" << QString::fromStdString(context->source_id)
                          << "dataset=" << context->dataset_id
                          << "topic=" << QString::fromStdString(context->topic_name)
                          << "object_topic_id=" << context->object_topic_id.id << "timestamp_ns=" << timestamp_ns
                          << "error=" << errorMessage(err);
      return std::nullopt;
    }
    if (payload.data == nullptr && payload.size > 0) {
      qCWarning(lcIngest) << "[lazy-fetch] null data with nonzero size source="
                          << QString::fromStdString(context->source_id) << "dataset=" << context->dataset_id
                          << "topic=" << QString::fromStdString(context->topic_name)
                          << "object_topic_id=" << context->object_topic_id.id << "timestamp_ns=" << timestamp_ns
                          << "payload_size=" << payload.size;
      if (payload.anchor.release != nullptr) {
        payload.anchor.release(payload.anchor.ctx);
      }
      return std::nullopt;
    }
    if (payload.size == 0) {
      qCWarning(lcIngest) << "[lazy-fetch] empty payload source=" << QString::fromStdString(context->source_id)
                          << "dataset=" << context->dataset_id
                          << "topic=" << QString::fromStdString(context->topic_name)
                          << "object_topic_id=" << context->object_topic_id.id << "timestamp_ns=" << timestamp_ns;
      if (payload.anchor.release != nullptr) {
        payload.anchor.release(payload.anchor.ctx);
      }
      return std::nullopt;
    }
    auto anchor = detail::wrapPayloadAnchor(payload.anchor, owner->library_keepalive);
    if (anchor == nullptr) {
      // No ownership — must copy because the buffer dies with this call.
      auto bytes = std::make_shared<const std::vector<uint8_t>>(copyPayloadBytes(payload));
      return sdk::PayloadView{
          Span<const uint8_t>{bytes->data(), bytes->size()},
          sdk::BufferAnchor{bytes},
      };
    }
    return sdk::PayloadView{
        Span<const uint8_t>{payload.data, static_cast<size_t>(payload.size)},
        std::move(anchor),
    };
  };
}
}  // namespace

// ---------------------------------------------------------------------------
// ParserBinding — out-of-line definitions so the header only needs forward
// declarations of MessageParserHandle / DatastoreParserWriteHost /
// ServiceRegistryBuilder.
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::ParserBinding::ParserBinding() = default;

DataSourceRuntimeHost::ParserBinding::ParserBinding(
    std::unique_ptr<ServiceRegistryBuilder> b, std::unique_ptr<DatastoreParserWriteHost> w,
    std::unique_ptr<DatastoreParserObjectWriteHost> ow, std::unique_ptr<MessageParserHandle> p, std::string topic,
    Signature sig, sdk::BuiltinObjectType kind, std::optional<ObjectTopicId> object_topic)
    : registry_builder(std::move(b)),
      write_host(std::move(w)),
      object_write_host(std::move(ow)),
      parser(std::move(p)),
      topic_name(std::move(topic)),
      signature(std::move(sig)),
      object_kind(kind),
      object_topic_id(object_topic) {}

DataSourceRuntimeHost::ParserBinding::~ParserBinding() = default;

DataSourceRuntimeHost::ParserBinding::ParserBinding(ParserBinding&&) noexcept = default;

DataSourceRuntimeHost::ParserBinding& DataSourceRuntimeHost::ParserBinding::operator=(ParserBinding&&) noexcept =
    default;

// ---------------------------------------------------------------------------
// Vtable — single static instance shared by every session.
// ---------------------------------------------------------------------------

const PJ_data_source_runtime_host_vtable_t DataSourceRuntimeHost::kVtable = {
    .protocol_version = 1,
    .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
    .report_message = &DataSourceRuntimeHost::cbReportMessage,
    .progress_start = &DataSourceRuntimeHost::cbProgressStart,
    .progress_update = &DataSourceRuntimeHost::cbProgressUpdate,
    .progress_finish = &DataSourceRuntimeHost::cbProgressFinish,
    .is_stop_requested = &DataSourceRuntimeHost::cbIsStopRequested,
    .notify_state = &DataSourceRuntimeHost::cbNotifyState,
    .request_stop = &DataSourceRuntimeHost::cbRequestStop,
    .ensure_parser_binding = &DataSourceRuntimeHost::cbEnsureParserBinding,
    .show_message_box = &DataSourceRuntimeHost::cbShowMessageBox,
    .list_available_encodings = &DataSourceRuntimeHost::cbListAvailableEncodings,
    .push_message = &DataSourceRuntimeHost::cbPushMessage,
    .notify_available_topics = &DataSourceRuntimeHost::cbNotifyAvailableTopics,
    .attach_source_record = &DataSourceRuntimeHost::cbAttachSourceRecord,
    .complete_ingest = &DataSourceRuntimeHost::cbCompleteIngest,
    .set_dataset_metadata = &DataSourceRuntimeHost::cbSetDatasetMetadata,
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DataSourceRuntimeHost::DataSourceRuntimeHost(
    DataEngine& engine, ExtensionCatalogService& catalog, DatasetId dataset_id, PJ_data_source_handle_t source_handle,
    ObjectStore& object_store, std::string source_id, ObjectTopicParserRegistrar parser_registrar,
    ObjectStore* secondary_object_store, DataEngine* secondary_data_engine, std::shared_ptr<void> library_keepalive,
    std::shared_ptr<ObjectIngestTapRegistry> ingest_taps)
    : engine_(engine),
      catalog_(catalog),
      object_store_(object_store),
      secondary_object_store_(secondary_object_store),
      secondary_data_engine_(secondary_data_engine),
      source_id_(std::move(source_id)),
      object_topic_parser_registrar_(std::move(parser_registrar)),
      dataset_id_(dataset_id),
      source_write_host_(engine, source_handle),
      source_object_write_host_(object_store, dataset_id),
      lazy_fetch_mutex_(std::make_shared<std::mutex>()),
      library_keepalive_(std::move(library_keepalive)),
      ingest_taps_(std::move(ingest_taps)) {
  // Wire the source-level write host with the secondary engine for the
  // streaming pause/resume two-engine lockstep. Without this, a plugin that
  // caches TopicHandle/FieldHandle on start() (e.g. data_stream_dummy) sees
  // them go stale after the first pause — the secondary engine has no
  // matching ids — and the worker dies on the first post-pause write.
  // Mirroring happens inside DatastoreSourceWriteHost via DataEngine's new
  // createTopic(requested_id) + createTopicField(requested_id) primitives.
  source_write_host_.setSecondaryEngine(secondary_data_engine_);
}

DataSourceRuntimeHost::~DataSourceRuntimeHost() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Status DataSourceRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  if (auto status = registerRequiredService<sdk::SourceWriteHostService>(registry, source_write_host_.raw()); !status) {
    return status;
  }
  registerOptionalService<sdk::SourceObjectWriteHostService>(registry, source_object_write_host_.raw());
  if (auto status = registerRequiredService<sdk::DataSourceRuntimeHostService>(registry, hostHandle()); !status) {
    return status;
  }
  return {};
}

void DataSourceRuntimeHost::flushAll() {
  if (flushed_) {
    return;
  }
  flushed_ = true;
  source_write_host_.flushPending();
  // Each parser binding owns its own DatastoreParserWriteHost / DataWriter
  // pair — their open chunks are independent from the source's. Without this
  // their pending rows never reach the reader, so the catalog sees the
  // column descriptors but bounds()/samples() return nothing and curves
  // drop in with empty plots.
  for (auto& [binding_id, binding] : parser_bindings_) {
    if (binding.write_host != nullptr) {
      binding.write_host->flushPending();
    }
  }
}

void DataSourceRuntimeHost::flushPending() {
  source_write_host_.flushPending();
  for (auto& [binding_id, binding] : parser_bindings_) {
    if (binding.write_host != nullptr) {
      binding.write_host->flushPending();
    }
  }
}
void DataSourceRuntimeHost::requestStop(std::string_view reason) {
  {
    std::lock_guard lock(stop_failure_mu_);
    if (stop_failure_.empty()) {
      stop_failure_.assign(reason.data(), reason.size());
    }
  }
  stop_requested_.store(true);
  std::function<void()> stop_hook;
  {
    std::lock_guard<std::mutex> lock(hooks_mu_);
    stop_hook = on_stop_requested_;
  }
  if (stop_hook) {
    stop_hook();
  }
}

void DataSourceRuntimeHost::requestStop() {
  stop_requested_.store(true);
  std::function<void()> stop_hook;
  {
    std::lock_guard<std::mutex> lock(hooks_mu_);
    stop_hook = on_stop_requested_;
  }
  if (stop_hook) {
    stop_hook();
  }
}

std::string DataSourceRuntimeHost::stopFailure() const {
  std::lock_guard lock(stop_failure_mu_);
  return stop_failure_;
}

std::string DataSourceRuntimeHost::lastError() const {
  if (auto failure = stopFailure(); !failure.empty()) {
    return failure;
  }
  return last_error_;
}

void DataSourceRuntimeHost::setObjectRetentionBudget(int64_t time_window_ns, size_t max_memory_bytes) {
  // Budget the active store (B while paused) so the paused tail stays bounded;
  // the frozen store is untouched (eviction is push-triggered anyway).
  ObjectStore* target = object_store_target_.load();
  for (auto& [_id, binding] : parser_bindings_) {
    if (!binding.object_topic_id.has_value()) {
      continue;
    }
    target->setRetentionBudget(
        *binding.object_topic_id,
        RetentionBudget{.time_window_ns = time_window_ns, .max_memory_bytes = max_memory_bytes});
  }
}

void DataSourceRuntimeHost::setObjectStoreTarget(ObjectStore* target) {
  // Route cbPushMessage's lazy-object push through the swap (it pushed straight
  // to the primary before, evicting the paused scrub-back snapshot).
  object_store_target_.store(target);
  // TODO(stream-pause): deferred edge cases: an in-flight push can strand a
  // frame in B across the resume flush; resume's catch-up notifyIngest lists
  // only scalar TopicIds (object-only flush nudge).
  // Retarget the source-level object write host and every per-parser-binding
  // one (the streaming hot path). Each host's atomic swap lets in-flight
  // pushes finish on the old target while the next lands on the new one; the
  // manager guarantees `target` already has the topics (lockstep mirror).
  source_object_write_host_.setTarget(target);
  for (auto& [_id, binding] : parser_bindings_) {
    if (binding.object_write_host != nullptr) {
      binding.object_write_host->setTarget(target);
    }
  }
}

void DataSourceRuntimeHost::setDataEngineTarget(DataEngine* target) {
  // Mirrors setObjectStoreTarget but for scalar writes. Retargets the
  // source-level write host and every parser binding so all scalar pushes
  // land on the secondary engine during pause and on the primary on resume.
  // Record it so a binding created AFTER this swap (a mid-pause new topic)
  // initializes against the active target rather than the frozen primary.
  data_engine_target_.store(target);
  source_write_host_.setTarget(target);
  for (auto& [_id, binding] : parser_bindings_) {
    if (binding.write_host != nullptr) {
      binding.write_host->setTarget(target);
    }
  }
}

void DataSourceRuntimeHost::setSourceRecordAttachedHook(std::function<void(std::string_view, bool)> hook) {
  std::lock_guard<std::mutex> lock(hooks_mu_);
  on_source_record_attached_ = std::move(hook);
}

void DataSourceRuntimeHost::setStopRequestedHook(std::function<void()> hook) {
  std::lock_guard<std::mutex> lock(hooks_mu_);
  on_stop_requested_ = std::move(hook);
}

bool DataSourceRuntimeHost::rejectAttachment(PJ_error_t* out_error, const std::string& message) noexcept {
  last_error_ = message;
  sdk::fillError(out_error, 1, "pj.runtime.ingest", last_error_);
  return false;
}

bool DataSourceRuntimeHost::fail(PJ_error_t* out_error, const char* message) noexcept {
  ingest_callback_failures_.fetch_add(1, std::memory_order_relaxed);
  last_error_ = message;
  sdk::fillError(out_error, 1, "pj.runtime.ingest", last_error_);
  return false;
}

// ---------------------------------------------------------------------------
// C-ABI callbacks
// ---------------------------------------------------------------------------

void DataSourceRuntimeHost::cbReportMessage(
    void* /*ctx*/, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
  const std::string_view text(message.data, message.size);
  switch (level) {
    case PJ_DATA_SOURCE_MESSAGE_ERROR:
      qCWarning(lcIngest) << "[plugin error]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    case PJ_DATA_SOURCE_MESSAGE_WARNING:
      qCWarning(lcIngest) << "[plugin warn]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
    default:
      qCInfo(lcIngest) << "[plugin]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
  }
}

bool DataSourceRuntimeHost::cbProgressStart(
    void* ctx, PJ_string_view_t label, uint64_t total, bool cancellable, PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->on_progress_start) {
    self->on_progress_start(std::string_view(label.data, label.size), total, cancellable);
  }
  return true;
}

bool DataSourceRuntimeHost::cbProgressUpdate(void* ctx, uint64_t current) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->stop_requested_.load()) {
    return false;
  }
  if (self->on_progress_update) {
    return self->on_progress_update(current);
  }
  return true;
}

void DataSourceRuntimeHost::cbProgressFinish(void* ctx) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  if (self->on_progress_finish) {
    self->on_progress_finish();
  }
}

bool DataSourceRuntimeHost::cbIsStopRequested(void* ctx) noexcept {
  return static_cast<DataSourceRuntimeHost*>(ctx)->stop_requested_.load();
}

void DataSourceRuntimeHost::cbNotifyState(void* /*ctx*/, PJ_data_source_state_t /*state*/) noexcept {}

// [thread-safe] in the ABI: a plugin may raise this from its own network
// thread while its poll thread is inside fail(), so only the two requestStop
// overloads are touched here — the flag is atomic and the failure reason has
// its own lock — and last_error_ never is.
void DataSourceRuntimeHost::cbRequestStop(
    void* ctx, PJ_data_source_state_t terminal, PJ_string_view_t reason) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  const std::string_view text(reason.data, reason.size);
  // STOPPED is the plugin's orderly end (end of file, a peer that closed
  // cleanly): its reason is narrative for the log, not a verdict, and must not
  // become an error that marks a recording of this source lossy. Any other
  // terminal state is a failure and its reason is the stream's verdict.
  if (terminal == PJ_DATA_SOURCE_STATE_STOPPED) {
    qCInfo(lcIngest) << "source requested an orderly stop:"
                     << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    self->requestStop();
    return;
  }
  qCWarning(lcIngest) << "source requested a stop after a failure:"
                      << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
  self->requestStop(text);
}

std::optional<uint32_t> DataSourceRuntimeHost::findReusableBinding(
    std::string_view topic_name, const ParserBinding::Signature& signature) const {
  for (const auto& [binding_id, binding] : parser_bindings_) {
    if (binding.topic_name == topic_name && binding.signature == signature) {
      return binding_id;
    }
  }
  return std::nullopt;
}

void DataSourceRuntimeHost::setRecordTap(std::shared_ptr<RecordTap> tap) {
  std::shared_ptr<RecordTap> released;
  {
    std::lock_guard lock(record_tap_mu_);
    released = std::exchange(record_tap_, std::move(tap));
    record_tap_active_.store(record_tap_ != nullptr, std::memory_order_relaxed);
  }
  // `released` dies here, outside the lock: dropping the last reference to a
  // Recorder joins its writer thread, and push threads must never queue behind
  // that mutex while it does.
}

uint64_t DataSourceRuntimeHost::recordTapSkippedLazy() const noexcept {
  return record_tap_skipped_lazy_.load();
}

std::shared_ptr<RecordTap> DataSourceRuntimeHost::recordTapSnapshot() const {
  std::lock_guard lock(record_tap_mu_);
  return record_tap_;
}

void DataSourceRuntimeHost::dropRecordTap(const std::shared_ptr<RecordTap>& tap) {
  std::shared_ptr<RecordTap> released;
  {
    std::lock_guard lock(record_tap_mu_);
    if (record_tap_ == tap) {
      released = std::exchange(record_tap_, nullptr);
      record_tap_active_.store(false, std::memory_order_relaxed);
    }
  }
  // Released outside the lock, for the reason setRecordTap documents.
}

void DataSourceRuntimeHost::recordMessage(
    const std::shared_ptr<RecordTap>& tap, uint32_t binding_id, const ParserBinding& binding, int64_t timestamp_ns,
    const uint8_t* data, uint64_t size) {
  // Only the signature is the host's to report: the tap stamps the source
  // identity, which is the recording's view of this source.
  const RecordedBindingView view{
      .topic = binding.topic_name,
      .encoding = binding.signature.encoding,
      .type_name = binding.signature.type_name,
      .schema_bytes = binding.signature.schema_bytes,
  };
  // Pre-set to the throw verdict: an exception falls through to the drop.
  TapVerdict verdict = TapVerdict::kStopRecording;
  try {
    verdict = tap->onMessage(binding_id, view, timestamp_ns, Span<const uint8_t>(data, static_cast<size_t>(size)));
  } catch (const std::exception& error) {
    qCWarning(lcIngest) << "record tap threw in onMessage; recording detached:" << error.what();
  } catch (...) {
    qCWarning(lcIngest) << "record tap threw an unknown exception in onMessage; recording detached";
  }
  if (verdict == TapVerdict::kStopRecording) {
    dropRecordTap(tap);
  }
}

bool DataSourceRuntimeHost::cbEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    const std::string_view encoding(request->parser_encoding.data, request->parser_encoding.size);
    const std::string_view topic_name(request->topic_name.data, request->topic_name.size);
    const std::string_view type_name(request->type_name.data, request->type_name.size);

    std::string parser_config;
    if (request->parser_config_json.size > 0) {
      parser_config.assign(request->parser_config_json.data, request->parser_config_json.size);
    }
    // The reuse identity deliberately ignores catalog reloads: an identical
    // re-request revives the existing binding (and its lazy object decoder)
    // so history is never re-decoded by a replacement provider. Re-resolving
    // after reload needs per-entry decoder generation pinning first.
    ParserBinding::Signature signature{
        std::string(encoding), std::string(type_name),
        request->schema.size > 0
            ? std::string(reinterpret_cast<const char*>(request->schema.data), request->schema.size)
            : std::string{},
        parser_config};

    // A demand-driven plugin re-requests the binding on every re-subscribe
    // (its cache dies with the subscription). Hand back the existing binding
    // for an identical request instead of re-registering the topic — a second
    // createTopic mints a duplicate engine topic with the same name, doubling
    // every field in the catalog.
    if (auto existing = self->findReusableBinding(topic_name, signature); existing.has_value()) {
      *out = PJ_parser_binding_handle_t{*existing};
      qCInfo(lcIngestBind) << "[parser-bind] reuse topic="
                           << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                           << "binding=" << *existing;
      return true;
    }

    // Per-route parser selection: the scalar winner takes this binding, the
    // object winner (if any) decides the topic's object identity and becomes its
    // lazy decoder. Both arrive prepared (bindSchema + loadConfig applied) and
    // own their DSO keepalive, so they stay valid past a later catalog reload().
    // This runs on the plugin poll thread; selection is serialized against a
    // GUI-thread reload inside the catalog service.
    const Span<const uint8_t> schema_span(request->schema.data, request->schema.size);
    ParserRouteSelection selection =
        self->catalog_.resolveParserRoutes(encoding, type_name, schema_span, parser_config);
    if (!selection.scalar.has_value()) {
      // No encoding/type prefix: the plugin names the channel when it
      // re-reports this through its own 224-byte PJ_error_t message, and the
      // routing service already logged the full text.
      return self->fail(out_error, selection.scalar_failure.c_str());
    }
    auto parser = std::make_unique<MessageParserHandle>(std::move(selection.scalar->parser));

    // Reuse the dataset's existing same-named scalar topic before minting a new
    // one — the transactional-refill contract (SessionManager::beginRefill keeps
    // TopicIds REGISTERED precisely so a same-source reload / import promotion
    // writes back into the same ids and every curve key survives) depends on it.
    // The direct-write API (WriteCore::ensureTopic) and the object route below
    // (ObjectStore::findTopic) already reuse by name; this scalar parser-binding
    // path was the one asymmetric spot that always createTopic'd — which
    // renumbered every TopicId on a delegated-ingest reload (data_load_mcap et
    // al.) and silently dropped every bound curve at the replace boundary
    // (caught live by the layout-import E2E's promotion leg).
    // Scan + create + mirror run under the engine lock(s), held across the whole
    // sequence: getTopicStorage() does NOT lock (engine.hpp's threading
    // contract), so a returned pointer is only valid while the lock is held —
    // and this runs on the plugin poll thread while the GUI thread may be
    // mutating the engine (a dataset removal frees TopicStorage). lockEnginePair
    // is the SAME helper the direct-write sibling locks through
    // (WriteCore::ensureTopic via lockWriteEngines), so the two paths can never
    // invert against each other. The engine mutex is recursive, so createTopic
    // re-acquiring inside is fine.
    TopicId topic_id = 0;
    {
      const EngineLockPair engine_locks = lockEnginePair(self->engine_, self->secondary_data_engine_);

      auto existing_ids = self->engine_.listTopics(self->dataset_id_);
      std::sort(existing_ids.begin(), existing_ids.end());
      for (const TopicId tid : existing_ids) {
        const auto* storage = self->engine_.getTopicStorage(tid);
        if (storage != nullptr && storage->descriptor().name == topic_name) {
          topic_id = tid;
          break;
        }
      }
      if (topic_id == 0) {
        auto topic_or = self->engine_.createTopic(self->dataset_id_, TopicDescriptor{.name = std::string(topic_name)});
        if (!topic_or.has_value()) {
          return self->fail(
              out_error, ("failed to create topic '" + std::string(topic_name) + "': " + topic_or.error()).c_str());
        }
        topic_id = *topic_or;
      }

      // Lockstep-mirror into the secondary engine with the SAME TopicId. The two
      // engines' TopicId counters drift whenever the primary gets topics the
      // secondary doesn't (e.g. a file loaded between streams), so we force the
      // secondary topic id to match the primary's instead of relying on the
      // counters staying in step. A later push uses one id against whichever
      // engine is the active target, so the ids MUST match. (On the reuse path
      // the secondary may already hold the id; mirror only when absent — the
      // same idempotent retry the direct-write mirror applies. Like that
      // sibling, the skip is name-blind: an id already present is assumed
      // mirrored rather than re-checked by name.)
      if (self->secondary_data_engine_ != nullptr &&
          self->secondary_data_engine_->getTopicStorage(topic_id) == nullptr) {
        auto mirrored = self->secondary_data_engine_->createTopic(
            self->dataset_id_, TopicDescriptor{.name = std::string(topic_name)}, topic_id);
        if (!mirrored.has_value()) {
          return self->fail(
              out_error,
              ("failed to mirror topic '" + std::string(topic_name) + "' into secondary engine: " + mirrored.error())
                  .c_str());
        }
      }
    }
    const PJ_topic_handle_t topic_handle{static_cast<uint32_t>(topic_id)};

    auto write_host = std::make_unique<DatastoreParserWriteHost>(self->engine_, topic_handle);
    // Same lockstep wiring as the source-level write host (see the runtime
    // host constructor). Closes the latent FieldHandle-stale bug for parser
    // plugins that cache handles across messages (parser_protobuf et al.):
    // every ensureField inside parserAppendRecord/appendBoundRecord is now
    // mirrored to the secondary engine via DataEngine::createTopicField with
    // the same FieldId, so the cached handle resolves after a pause/resume
    // target swap.
    write_host->setSecondaryEngine(self->secondary_data_engine_);
    // If this binding is minted while paused, the source-level swap already
    // happened, so point it at the active target (secondary) now — otherwise
    // this mid-pause topic's samples would write into the frozen primary and
    // drag the global timeline. When live, this is the primary (a no-op).
    write_host->setTarget(self->data_engine_target_.load());

    // Build the service registry the parser binds against. The builder must
    // outlive bind() because the plugin may hold a view into it; we move it
    // into the ParserBinding so its lifetime matches the parser's.
    auto registry_builder = std::make_unique<ServiceRegistryBuilder>();
    // The parser's scalar sink: message_parser_plugin_base require<>()s it, so a
    // rejection must fail the binding here rather than surface later as an
    // unexplained parse failure.
    if (auto status = registerRequiredService<sdk::ParserWriteHostService>(*registry_builder, write_host->raw());
        !status) {
      return self->fail(out_error, ("failed to register the parser write host: " + status.error()).c_str());
    }

    const sdk::BuiltinObjectType object_kind =
        selection.object.has_value() ? selection.object->object_type : sdk::BuiltinObjectType::kNone;
    std::optional<ObjectTopicId> object_topic_id;
    std::unique_ptr<DatastoreParserObjectWriteHost> object_write_host;
    if (object_kind == sdk::BuiltinObjectType::kNone) {
      // The parser declined to classify this topic as a builtin object — it
      // will only produce scalar columns. Traced once per binding so an
      // operator chasing why an image-shaped topic is not showing in the
      // catalog as an ObjectTopic can see the classification verdict.
      qCInfo(lcIngestBind) << "[parser-bind] classifySchema=kNone topic="
                           << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                           << "type=" << QString::fromUtf8(type_name.data(), static_cast<int>(type_name.size()))
                           << "encoding=" << QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))
                           << "— scalar-only ingest";
    } else {
      const std::string& object_provider_id = selection.object->provider_id;
      if (auto existing = self->object_store_.findTopic(self->dataset_id_, topic_name); existing.has_value()) {
        const auto previous_provider = self->object_topic_decoder_providers_.find(existing->id);
        if (previous_provider != self->object_topic_decoder_providers_.end() &&
            previous_provider->second != object_provider_id) {
          qCWarning(lcIngest).noquote() << "[parser-bind] refusing object decoder replacement for topic"
                                        << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                                        << "old provider" << QString::fromStdString(previous_provider->second)
                                        << "new provider" << QString::fromStdString(object_provider_id)
                                        << "— binding remains scalar-only";
          // Fail closed for this binding: retain the existing topic and decoder,
          // but do not enqueue or directly write objects through the replacement.
        } else {
          // KNOWN LIMITATION: a same-provider retype to a DIFFERENT builtin
          // object type reuses the original metadata_json until stream restart.
          object_topic_id = existing;
        }
      } else {
        const std::string metadata_json = fmt::format(R"({{"builtin_object_type":"{}"}})", sdk::name(object_kind));
        const ObjectTopicDescriptor descriptor{
            .dataset_id = self->dataset_id_,
            .topic_name = std::string(topic_name),
            .metadata_json = metadata_json,
        };
        auto registered = self->object_store_.registerTopic(descriptor);
        if (!registered.has_value()) {
          return self->fail(
              out_error,
              ("failed to register object topic '" + std::string(topic_name) + "': " + registered.error()).c_str());
        }
        object_topic_id = *registered;
        // Lockstep-mirror into the secondary store with the SAME ObjectTopicId.
        // The two stores' id counters drift whenever the primary gets topics the
        // secondary doesn't (e.g. a file loaded between streams), so we force the
        // secondary id to match the primary's. A later push uses one id against
        // whichever store is the active target, so the ids MUST match.
        if (self->secondary_object_store_ != nullptr) {
          auto mirrored = self->secondary_object_store_->registerTopic(descriptor, *registered);
          if (!mirrored.has_value()) {
            return self->fail(
                out_error, ("failed to mirror object topic '" + std::string(topic_name) +
                            "' into secondary store: " + mirrored.error())
                               .c_str());
          }
        }
      }
      if (object_topic_id.has_value()) {
        self->object_topic_decoder_providers_.try_emplace(object_topic_id->id, object_provider_id);
        // When the scalar and object routes selected DIFFERENT providers, the
        // scalar parser must not write objects — the topic's object content
        // belongs to the object winner. Live entries still flow through the lazy
        // push in cbPushMessage and decode via the registrar instance below.
        if (object_provider_id == selection.scalar->provider_id) {
          object_write_host =
              std::make_unique<DatastoreParserObjectWriteHost>(self->object_store_, object_topic_id->id);
          // Same mid-pause rule as the scalar host above: a binding minted while
          // paused must push into the active (secondary) store, not the frozen
          // primary. When live, object_store_target_ is the primary (a no-op).
          object_write_host->setTarget(self->object_store_target_.load());
          // Required at THIS point despite being an optional service in general: the
          // host only reaches here after committing this topic to object ingest (the
          // object topic is registered and mirrored). Merely warning would let the
          // parser bind with no object sink and drop every object payload silently.
          if (auto status = registerRequiredService<sdk::ParserObjectWriteHostService>(
                  *registry_builder, object_write_host->raw());
              !status) {
            return self->fail(
                out_error, ("failed to register the parser object write host for '" + std::string(topic_name) +
                            "': " + status.error())
                               .c_str());
          }
        }

        if (self->object_topic_parser_registrar_) {
          // The registrar keeps one decoder slot per object topic; the provider
          // guard above keeps another provider from replacing it. A replacement
          // build with the same provider id still needs per-entry pinning.
          self->object_topic_parser_registrar_(
              *object_topic_id, std::make_unique<MessageParserHandle>(std::move(selection.object->parser)));
        }
      }
    }

    // A bind(registry) failure is terminal: the resolver is not re-entered for
    // the next candidate.
    if (auto status = parser->bind(registry_builder->view()); !status) {
      return self->fail(out_error, ("failed to bind parser services: " + status.error()).c_str());
    }

    const uint32_t binding_id = self->next_binding_id_++;
    self->parser_bindings_.emplace(
        binding_id, ParserBinding{
                        std::move(registry_builder),
                        std::move(write_host),
                        std::move(object_write_host),
                        std::move(parser),
                        std::string(topic_name),
                        std::move(signature),
                        object_kind,
                        object_topic_id,
                    });

    *out = PJ_parser_binding_handle_t{binding_id};
    qCInfo(lcIngestBind) << "[parser-bind] encoding="
                         << QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))
                         << "topic=" << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                         << "object_kind=" << static_cast<int>(object_kind);
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while binding parser");
  }
}

bool DataSourceRuntimeHost::cbPushMessage(
    void* ctx, PJ_parser_binding_handle_t handle, int64_t timestamp_ns, PJ_message_data_fetcher_t fetch_message_data,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  auto fetcher_owner = std::make_shared<FetcherOwner>(fetch_message_data, self->library_keepalive_);
  self->ingest_begun_.store(true, std::memory_order_relaxed);

  try {
    if (self->ingest_sealed_.load(std::memory_order_relaxed)) {
      return self->fail(out_error, "push after ingest completion: the context is sealed");
    }
    auto it = self->parser_bindings_.find(handle.id);
    if (it == self->parser_bindings_.end()) {
      return self->fail(out_error, "invalid parser binding handle");
    }
    auto& binding = it->second;
    if (fetcher_owner->fetcher.fetchMessageData == nullptr) {
      return self->fail(out_error, "message data fetcher is null");
    }

    // Object ingest policy only applies to parser bindings that actually
    // classify as builtin objects. Scalar-only topics must stay eager so a
    // broad default lazy policy cannot accidentally drop normal curves.
    const bool is_object_topic = binding.object_topic_id.has_value();
    const auto policy = is_object_topic
                            ? self->policy_resolver_.resolve(self->source_id_, binding.topic_name, binding.object_kind)
                            : sdk::ObjectIngestPolicy::kEager;

    // Get-or-create the binding's shared lazy-fetch log context — one
    // allocation per binding, not one per message. [worker thread, same single
    // caller as the rest of cbPushMessage; parser_bindings_ is unsynchronized.]
    auto lazy_context = [self, &binding]() -> std::shared_ptr<const LazyFetchTopicContext> {
      if (binding.lazy_fetch_context == nullptr) {
        binding.lazy_fetch_context = std::make_shared<const LazyFetchTopicContext>(LazyFetchTopicContext{
            .dataset_id = self->dataset_id_,
            .object_topic_id = *binding.object_topic_id,
            .source_id = self->source_id_,
            .topic_name = binding.topic_name,
        });
      }
      return binding.lazy_fetch_context;
    };

    auto push_lazy_object = [&]() -> bool {
      if (!is_object_topic) {
        return true;
      }
      auto closure = makeLazyFetchClosure(fetcher_owner, self->lazy_fetch_mutex_, lazy_context(), timestamp_ns);
      if (auto status =
              self->object_store_target_.load()->pushLazy(*binding.object_topic_id, timestamp_ns, std::move(closure));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushLazy failed: " + status.error()).c_str());
      }
      return true;
    };

    if (policy == sdk::ObjectIngestPolicy::kPureLazy) {
      // No bytes are fetched on this thread, so there is nothing to record;
      // count the gap instead of silently dropping it.
      if (self->hasRecordTap()) {
        self->record_tap_skipped_lazy_.fetch_add(1);
      }
      return push_lazy_object();
    }

    PJ_payload_t payload{};
    bool fetched = false;
    if (self->lazy_fetch_mutex_ != nullptr) {
      std::lock_guard lock(*self->lazy_fetch_mutex_);
      fetched = fetcher_owner->fetcher.fetchMessageData(fetcher_owner->fetcher.ctx, &payload, out_error);
    } else {
      fetched = fetcher_owner->fetcher.fetchMessageData(fetcher_owner->fetcher.ctx, &payload, out_error);
    }
    if (!fetched) {
      // The fetcher populated out_error itself; still latch the failure —
      // a capture whose bytes were never produced must not publish.
      self->ingest_callback_failures_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Wrap the payload anchor FIRST, so every exit below — including the
    // failure paths — releases the plugin's buffer exactly once. The store
    // paths may extend its lifetime: kEager's captured closure keeps it for the
    // entry's lifetime, the lazy-scalars seed until the pool evicts it.
    auto payload_anchor = detail::wrapPayloadAnchor(payload.anchor, self->library_keepalive_);

    if (payload.data == nullptr && payload.size > 0) {
      return self->fail(out_error, "message data fetcher returned null data");
    }

    self->tap_eligible_messages_.fetch_add(1, std::memory_order_relaxed);
    // The session recorder sees the plugin's raw bytes, before any decoding.
    // The binding travels with the message, so a recording started mid-stream
    // gets a channel for every topic that keeps producing — including ones
    // subscribed long before it started.
    if (self->hasRecordTap()) {
      if (const auto tap = self->recordTapSnapshot(); tap != nullptr) {
        self->recordMessage(tap, handle.id, binding, timestamp_ns, payload.data, payload.size);
      }
    }

    if (auto status = binding.parser->parse(timestamp_ns, Span<const uint8_t>(payload.data, payload.size)); !status) {
      return self->fail(out_error, status.error().c_str());
    }

    if (!is_object_topic) {
      return true;
    }
    const bool tapped = self->ingest_taps_ != nullptr && [&]() {
      try {
        return self->ingest_taps_->invoke(
            binding.object_kind, *binding.object_topic_id, self->dataset_id_, timestamp_ns,
            sdk::PayloadView{Span<const uint8_t>{payload.data, static_cast<size_t>(payload.size)}, payload_anchor});
      } catch (const std::exception& error) {
        self->ingest_tap_failures_.fetch_add(1);
        qCWarning(lcIngest) << "object ingest tap threw:" << error.what();
      } catch (...) {
        self->ingest_tap_failures_.fetch_add(1);
        qCWarning(lcIngest) << "object ingest tap threw an unknown exception";
      }
      return true;
    }();
    // File-backed tapped objects are decoded before storage and keep only their
    // re-readable catalog closure. Untapped objects retain the ingest-time seed
    // so live-edge consumers do not re-fetch bytes already in hand.
    if (policy == sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars) {
      auto fallback = makeLazyFetchClosure(fetcher_owner, self->lazy_fetch_mutex_, lazy_context(), timestamp_ns);
      if (tapped) {
        if (auto status = self->object_store_target_.load()->pushLazy(
                *binding.object_topic_id, timestamp_ns, std::move(fallback));
            !status) {
          return self->fail(out_error, ("ObjectStore.pushLazy failed: " + status.error()).c_str());
        }
        return true;
      }
      sdk::PayloadView seed;
      if (payload_anchor != nullptr) {
        seed = sdk::PayloadView{
            Span<const uint8_t>{payload.data, static_cast<size_t>(payload.size)}, std::move(payload_anchor)};
      } else if (payload.size > 0) {
        // Anchorless transient buffer: the bytes die with this call, so the
        // seed needs its own copy.
        seed = sdk::makePayloadView(std::vector<uint8_t>(payload.data, payload.data + payload.size));
      }
      if (auto status = self->object_store_target_.load()->pushLazyWithSeed(
              *binding.object_topic_id, timestamp_ns, std::move(seed), std::move(fallback));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushLazyWithSeed failed: " + status.error()).c_str());
      }
      return true;
    }
    // Streaming has no durable source to re-read. Its kEager entries therefore
    // keep the captured payload even when a tap also consumed it synchronously.
    if (auto status = self->object_store_target_.load()->pushLazy(
            *binding.object_topic_id, timestamp_ns, makeCapturedPayloadClosure(payload, std::move(payload_anchor)));
        !status) {
      return self->fail(out_error, ("ObjectStore.pushLazy failed: " + status.error()).c_str());
    }
    return true;
  } catch (...) {
    return self->fail(out_error, "exception while pushing message v2");
  }
}

int DataSourceRuntimeHost::cbShowMessageBox(
    void* ctx, PJ_message_box_type_t type, PJ_string_view_t title, PJ_string_view_t message, int buttons) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  const std::string_view sv_title(title.data, title.size);
  const std::string_view sv_message(message.data, message.size);

  if (self->message_box_handler_) {
    return self->message_box_handler_(static_cast<int>(type), sv_title, sv_message, buttons);
  }

  // Headless fallback: log and pick the positive button.
  qCInfo(lcIngest) << "[plugin msgbox]" << QString::fromUtf8(title.data, static_cast<int>(title.size)) << "—"
                   << QString::fromUtf8(message.data, static_cast<int>(message.size));
  if ((buttons & PJ_MSG_BTN_CONTINUE) != 0) {
    return PJ_MSG_BTN_CONTINUE;
  }
  if ((buttons & PJ_MSG_BTN_YES) != 0) {
    return PJ_MSG_BTN_YES;
  }
  if ((buttons & PJ_MSG_BTN_OK) != 0) {
    return PJ_MSG_BTN_OK;
  }
  return -1;
}

const char* DataSourceRuntimeHost::cbListAvailableEncodings(void* ctx) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    // Build a JSON array of unique encodings the catalog knows. Cached on
    // the session so the returned char* is valid until the next call (per
    // the protocol contract). parserEncodings() snapshots under the catalog's
    // shared lock (this may run off the GUI thread) — already sorted+unique.
    const std::vector<std::string> unique_encodings = self->catalog_.parserEncodings();
    std::string json = "[";
    bool first = true;
    for (const auto& enc : unique_encodings) {
      if (!first) {
        json += ",";
      }
      first = false;
      json += "\"" + enc + "\"";
    }
    json += "]";
    self->available_encodings_cache_ = std::move(json);
    return self->available_encodings_cache_.c_str();
  } catch (...) {
    return nullptr;
  }
}

// Runs on the plugin's poll/stream thread — deliberately the SAME thread and
// catalog/parser access pattern cbEnsureParserBinding has always used.
sdk::BuiltinObjectType DataSourceRuntimeHost::classifyAvailableTopic(const PJ_available_topic_t& topic) const noexcept {
  const std::string_view encoding(topic.parser_encoding.data, topic.parser_encoding.size);
  const std::string_view type_name(topic.type_name.data, topic.type_name.size);
  const Span<const uint8_t> schema_span(topic.schema.data, topic.schema.size);
  try {
    // The advertised type is an object exactly when some provider holds the
    // OBJECT route for it — the same resolution bind time performs, so the
    // probe work is shared through the resolver cache. No config exists here.
    if (const auto object_type = catalog_.classifyParserObjectRoute(encoding, type_name, schema_span);
        object_type.has_value()) {
      return *object_type;
    }
  } catch (...) {
    // Fall through to the name-matching fallback below.
  }
  // Fallback: match the type name against the two infra-tier schemas so TF/CameraInfo
  // stay classified for advertising even when this host has no parser for the encoding
  // (or the parser's classify_schema returned kNone). Compare the LEAF segment
  // (after the last '/' or '.') exactly, not a substring — a substring match would
  // misclassify e.g. `my_msgs/CameraInfoStatus` as CameraInfo infrastructure and
  // wrongly pin it always-subscribed.
  const std::size_t leaf_start = type_name.find_last_of("/.");
  const std::string_view leaf = leaf_start == std::string_view::npos ? type_name : type_name.substr(leaf_start + 1);
  if (leaf == "FrameTransforms") {
    return sdk::BuiltinObjectType::kFrameTransforms;
  }
  if (leaf == "CameraInfo") {
    return sdk::BuiltinObjectType::kCameraInfo;
  }
  return sdk::BuiltinObjectType::kNone;
}

bool DataSourceRuntimeHost::cbNotifyAvailableTopics(
    void* ctx, const PJ_available_topic_t* topics, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    std::vector<AdvertisedTopicInfo> classified;
    classified.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
      const PJ_available_topic_t& topic = topics[i];
      classified.push_back(
          AdvertisedTopicInfo{
              std::string(topic.topic_name.data, topic.topic_name.size),
              self->classifyAvailableTopic(topic),
          });
    }
    if (self->on_available_topics) {
      self->on_available_topics(std::move(classified));
    }
    return true;
  } catch (...) {
    // Advertising is best-effort informational traffic — never fail the plugin's
    // poll loop over it.
    return true;
  }
}

// ---------------------------------------------------------------------------
// Source-capture surface (attach_source_record / complete_ingest)
// ---------------------------------------------------------------------------

std::optional<std::string> DataSourceRuntimeHost::sourceRecordDescriptor() const {
  std::lock_guard<std::mutex> lock(capture_mu_);
  return source_record_;
}

std::optional<sdk::IngestCompletionRecord> DataSourceRuntimeHost::ingestCompletion() const {
  std::lock_guard<std::mutex> lock(capture_mu_);
  return completion_;
}

std::string DataSourceRuntimeHost::captureVetoReason() const {
  std::lock_guard<std::mutex> lock(capture_mu_);
  return capture_veto_;
}

void DataSourceRuntimeHost::stageLoaderMetadata(std::string json) {
  std::lock_guard<std::mutex> lock(capture_mu_);
  loader_metadata_ = std::move(json);
}

std::optional<std::string> DataSourceRuntimeHost::stagedLoaderMetadata() const {
  std::lock_guard<std::mutex> lock(capture_mu_);
  return loader_metadata_;
}
// Descriptive loader metadata (set_dataset_metadata, SDK 0.32): validate the
// document against the session bounds, then stage it on this ingest context —
// publication to the dataset happens at the owning load's commit seam. A
// rejection returns false + reason and never affects ingestion or capture.
bool DataSourceRuntimeHost::cbSetDatasetMetadata(
    void* ctx, PJ_string_view_t metadata_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    // ABI guard first: a null pointer with a nonzero size is a malformed call
    // (toStringView would carry the size over substitute storage).
    if (metadata_json.data == nullptr && metadata_json.size != 0) {
      return self->fail(out_error, "dataset metadata: null data with nonzero size");
    }
    const std::string_view bytes(metadata_json.data == nullptr ? "" : metadata_json.data, metadata_json.size);
    const QString document = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    if (auto valid = SessionManager::validateDatasetMetadata(document); !valid) {
      return self->fail(out_error, valid.error().c_str());
    }
    self->stageLoaderMetadata(document.toStdString());
    return true;
  } catch (const std::exception& e) {
    return self->fail(out_error, e.what());
  } catch (...) {
    return self->fail(out_error, "unknown error in set_dataset_metadata");
  }
}

bool DataSourceRuntimeHost::cbAttachSourceRecord(
    void* ctx, PJ_string_view_t descriptor_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
#ifdef __EMSCRIPTEN__
  // The browser build has no capture service (and no pj_source support
  // library): refuse without latching, same as any refused record.
  (void)descriptor_json;
  return self->rejectAttachment(out_error, "source capture is not available in this build");
#else
  try {
    // The ABI guard comes first: a null pointer with a nonzero size is a
    // malformed call, and toStringView() would preserve that size over
    // substitute storage — a view running off the end of it.
    if (descriptor_json.data == nullptr || descriptor_json.size == 0) {
      return self->rejectAttachment(out_error, "source record descriptor is empty");
    }
    const std::string_view bytes(descriptor_json.data, descriptor_json.size);
    // Validation failures REFUSE without latching or vetoing: a refused
    // record only means no caching, and the provider may correct and
    // re-attach before the first push.
    if (auto parsed = sdk::source::parseSourceRecordEnvelope(bytes); !parsed) {
      return self->rejectAttachment(out_error, parsed.error());
    }

    bool replaced = false;
    {
      std::lock_guard<std::mutex> lock(self->capture_mu_);
      if (self->completion_.has_value()) {
        self->capture_veto_ = "source record attached after ingest completion";
        return self->fail(out_error, "attach after completion: the context is sealed");
      }
      if (self->ingest_begun_.load(std::memory_order_relaxed)) {
        if (self->source_record_ == bytes) {
          return true;  // byte-identical repeat is idempotent, even late
        }
        self->capture_veto_ = "source record attached after ingest began";
        return self->fail(out_error, "attach after the first push is an error");
      }
      replaced = self->source_record_.has_value() && *self->source_record_ != bytes;
      if (self->source_record_ == bytes) {
        return true;  // idempotent
      }
      self->source_record_ = std::string(bytes);  // last attachment before ingest wins
    }
    // Outside the lock: user code (the capture service opening its cache
    // transaction) must never run under capture_mu_.
    std::function<void(std::string_view, bool)> attached_hook;
    {
      std::lock_guard<std::mutex> lock(self->hooks_mu_);
      attached_hook = self->on_source_record_attached_;
    }
    if (attached_hook) {
      attached_hook(bytes, replaced);
    }
    return true;
  } catch (const std::exception& e) {
    return self->fail(out_error, e.what());
  } catch (...) {
    return self->fail(out_error, "unknown error in attach_source_record");
  }
#endif  // __EMSCRIPTEN__
}

bool DataSourceRuntimeHost::cbCompleteIngest(
    void* ctx, const PJ_ingest_completion_t* completion, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    auto copied = sdk::copyIngestCompletion(completion);
    std::lock_guard<std::mutex> lock(self->capture_mu_);
    if (!copied) {
      // Malformed evidence permanently vetoes caching for this context; the
      // ingest itself is unaffected.
      self->capture_veto_ = "malformed ingest completion: " + copied.error();
      return self->fail(out_error, self->capture_veto_.c_str());
    }
    if (self->completion_.has_value()) {
      auto sorted = [](std::vector<std::string> topics) {
        std::sort(topics.begin(), topics.end());
        return topics;
      };
      const bool identical = self->completion_->outcome == copied->outcome &&
                             self->completion_->flags == copied->flags &&
                             sorted(self->completion_->requested_topics) == sorted(copied->requested_topics);
      if (identical) {
        return true;  // idempotent repeat before release
      }
      self->capture_veto_ = "conflicting ingest terminals reported";
      return self->fail(out_error, self->capture_veto_.c_str());
    }
    self->completion_ = std::move(*copied);
    self->ingest_sealed_.store(true, std::memory_order_relaxed);
    return true;
  } catch (const std::exception& e) {
    return self->fail(out_error, e.what());
  } catch (...) {
    return self->fail(out_error, "unknown error in complete_ingest");
  }
}

}  // namespace PJ
