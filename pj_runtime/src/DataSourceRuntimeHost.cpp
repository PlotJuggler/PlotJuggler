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
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/source/record_envelope.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ObjectIngestTap.h"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/UsageCounters.h"
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

// Per-binding constants for lazy-fetch failure logs, shared by every lazy closure of the binding (one closure per
// object message). Namespace scope to match the header's forward declaration.
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

// Idempotent resident closure (kEager only) replaying one PayloadView. A non-null `anchor` keeps the buffer for the
// entry's lifetime (zero copy); with a null one (transient buffer) the bytes are copied into their own anchor.
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
    // fetcher.release is plugin code. The destructor body runs while library_keepalive (a member) is still held, so
    // the DSO cannot be dlclosed underneath this call even when this owner holds its last reference. A separate
    // lambda capture would not do: its destruction order relative to this owner is unspecified.
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

// Deferred lazy closure: re-invokes the fetcher on every read, wrapping each call's anchor in a per-call shared_ptr so
// a returned PayloadView outlives the call. Keeps object bytes non-resident (kPureLazy, and kLazyObjectsEagerScalars
// after the ingest-time scalar parse).
//
// nullopt when the source cannot re-produce usable bytes, so the ObjectStore flags the entry fetch_failed. Zero bytes
// count as a failure: the SDK's pushMessage glue reports ok=true for a fetch that produced an empty view (false only on
// an exception), so "no bytes" is how a file-source failure arrives, and no builtin object decodes from zero bytes.
// The cost is a false loss for a legitimately empty payload (an empty FrameTransforms set); removing the ambiguity
// needs a failure signal in the SDK glue.
PJ::LazyCallback makeLazyFetchClosure(
    std::shared_ptr<FetcherOwner> owner, std::shared_ptr<std::mutex> fetch_mutex,
    std::shared_ptr<const LazyFetchTopicContext> context, int64_t timestamp_ns) {
  // The DSO keepalive is owner->library_keepalive, the one source of truth for each wrapped anchor.
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

// ParserBinding: out of line, so the header needs only forward declarations.

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
  // The two-engine lockstep of streaming pause/resume: a plugin caching TopicHandle/FieldHandle at start() would find
  // no matching ids in the secondary after the first pause and its worker would die on the next write.
  source_write_host_.setSecondaryEngine(secondary_data_engine_);
  // Values the datastore stores as null (don't fit their column's type) are
  // reported here. Process-wide and idempotent: every host installs the same sink.
  setIngestWarningHandler([](std::string_view message) {
    qCWarning(lcIngest) << QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
  });
}

DataSourceRuntimeHost::~DataSourceRuntimeHost() = default;

Status DataSourceRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  if (auto status = registerRequiredService<sdk::SourceWriteHostService>(registry, source_write_host_.raw()); !status) {
    return status;
  }
  registerOptionalService<sdk::SourceObjectWriteHostService>(registry, source_object_write_host_.raw());
  if (auto status = registerRequiredService<sdk::DataSourceRuntimeHostService>(registry, hostHandle()); !status) {
    return status;
  }
  if (settings_host_.has_value()) {
    registerOptionalService<sdk::SettingsStoreService>(registry, settings_host_->view());
  }
  return {};
}

Status DataSourceRuntimeHost::bind(DataSourceHandle& handle, std::unique_ptr<sdk::SettingsBackend> settings) {
  if (registry_ != nullptr) {
    return unexpected(std::string("bind failed: this host already bound a plugin"));
  }
  if (settings != nullptr) {
    settings_ = std::move(settings);
    settings_host_.emplace(*settings_);
  }
  registry_ = std::make_unique<ServiceRegistryBuilder>();
  if (auto status = registerServices(*registry_); !status) {
    return unexpected("service registration failed: " + status.error());
  }
  if (auto status = handle.bind(registry_->view()); !status) {
    return unexpected("bind failed: " + status.error());
  }
  return {};
}

void DataSourceRuntimeHost::flushAll() {
  if (flushed_) {
    return;
  }
  flushed_ = true;
  source_write_host_.finish();
  // Each parser binding has its own writer with independent open chunks: unflushed, the catalog sees the columns but
  // every curve is empty.
  for (auto& [binding_id, binding] : parser_bindings_) {
    if (binding.write_host != nullptr) {
      binding.write_host->finish();
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

std::string DataSourceRuntimeHost::lastReportedProblem() const {
  std::lock_guard lock(stop_failure_mu_);
  return last_reported_problem_;
}

void DataSourceRuntimeHost::forgetReportedProblem() {
  std::lock_guard lock(stop_failure_mu_);
  last_reported_problem_.clear();
}

std::string DataSourceRuntimeHost::lastError() const {
  if (auto failure = stopFailure(); !failure.empty()) {
    return failure;
  }
  return last_error_;
}

void DataSourceRuntimeHost::setObjectRetentionBudget(int64_t time_window_ns, size_t max_memory_bytes) {
  // The active store (the secondary while paused), so the paused tail stays bounded; the frozen store is untouched.
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
  object_store_target_.store(target);
  // TODO(stream-pause): an in-flight push can strand a frame in the secondary across the resume flush; resume's
  // catch-up notifyIngest lists only scalar TopicIds, so an object-only flush gets no nudge.
  // Each host's atomic swap lets in-flight pushes finish on the old target; the manager guarantees `target` already
  // has the topics (lockstep mirror).
  source_object_write_host_.setTarget(target);
  for (auto& [_id, binding] : parser_bindings_) {
    if (binding.object_write_host != nullptr) {
      binding.object_write_host->setTarget(target);
    }
  }
}

void DataSourceRuntimeHost::setDataEngineTarget(DataEngine* target) {
  // A binding created after this swap (a topic first seen mid-pause) initializes against the active target, not the
  // frozen primary.
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

void DataSourceRuntimeHost::cbReportMessage(
    void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  const std::string_view text(message.data, message.size);
  std::optional<DiagnosticLevel> forward;
  switch (level) {
    case PJ_DATA_SOURCE_MESSAGE_ERROR:
      qCWarning(lcIngest) << "[plugin error]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      forward = DiagnosticLevel::kError;
      break;
    case PJ_DATA_SOURCE_MESSAGE_WARNING:
      qCWarning(lcIngest) << "[plugin warn]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      forward = DiagnosticLevel::kWarning;
      break;
    default:
      qCInfo(lcIngest) << "[plugin]" << QString::fromUtf8(text.data(), static_cast<int>(text.size()));
      break;
  }
  if (!forward) {
    return;
  }
  try {
    std::lock_guard lock(self->stop_failure_mu_);
    self->last_reported_problem_.assign(text);
  } catch (...) {
    // noexcept C-ABI slot: losing the text costs only a load-failure reason.
  }
  if (!self->diagnostic_sink_) {
    return;
  }
  const auto n = self->forwarded_messages_.fetch_add(1, std::memory_order_relaxed);
  if (n > kMaxForwardedMessages) {
    return;  // Console log only past the cap (one notice already sent).
  }
  try {
    if (n == kMaxForwardedMessages) {
      emitDiagnosticTo(
          self->diagnostic_sink_, DiagnosticLevel::kWarning, self->diagnostic_source_, "data-source-message-limit",
          self->diagnostic_subject_ + ": further plugin messages are in the log only");
      return;
    }
    emitDiagnosticTo(
        self->diagnostic_sink_, *forward, self->diagnostic_source_, "data-source-message",
        self->diagnostic_subject_ + ": " + std::string(text));
  } catch (...) {
    // noexcept C-ABI slot: a failing sink costs the bell entry, never the import.
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

// Thread-safe in the ABI: a plugin may call it from its own thread while its poll thread is inside fail(), so only the
// requestStop overloads are touched (atomic flag, locked reason), never last_error_.
void DataSourceRuntimeHost::cbRequestStop(
    void* ctx, PJ_data_source_state_t terminal, PJ_string_view_t reason) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  const std::string_view text(reason.data, reason.size);
  // STOPPED is an orderly end (end of file, a peer closing cleanly): its reason is narrative and must not mark a
  // recording of this source lossy. Any other terminal state is a failure.
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
  // `released` dies here, outside the lock: dropping the last reference to a Recorder joins its writer thread, and push
  // threads must not queue behind the mutex meanwhile.
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
  // Only the signature is the host's to report; the tap stamps the source identity.
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
    // The reuse identity ignores catalog reloads: an identical re-request revives the existing binding and its lazy
    // decoder, so history is never re-decoded by a replacement provider (re-resolving would need per-entry decoder
    // generation pinning).
    ParserBinding::Signature signature{
        std::string(encoding), std::string(type_name),
        request->schema.size > 0
            ? std::string(reinterpret_cast<const char*>(request->schema.data), request->schema.size)
            : std::string{},
        parser_config};

    // A demand-driven plugin re-requests the binding on every re-subscribe: hand back the existing one, since a second
    // createTopic duplicates every field in the catalog.
    if (auto existing = self->findReusableBinding(topic_name, signature); existing.has_value()) {
      *out = PJ_parser_binding_handle_t{*existing};
      qCInfo(lcIngestBind) << "[parser-bind] reuse topic="
                           << QString::fromUtf8(topic_name.data(), static_cast<int>(topic_name.size()))
                           << "binding=" << *existing;
      return true;
    }

    // The scalar winner takes this binding; the object winner (if any) decides the topic's object identity and becomes
    // its lazy decoder. Both arrive prepared and own their DSO keepalive, so they survive a catalog reload(). Runs on
    // the plugin poll thread; the catalog serializes selection against a GUI-thread reload.
    const Span<const uint8_t> schema_span(request->schema.data, request->schema.size);
    ParserRouteSelection selection =
        self->catalog_.resolveParserRoutes(encoding, type_name, schema_span, parser_config);
    if (!selection.scalar.has_value()) {
      // No prefix: the plugin re-reports this through its 224-byte PJ_error_t, and the routing service logged it all.
      return self->fail(out_error, selection.scalar_failure.c_str());
    }
    // One decode per bound topic for each plugin that decodes it; a reused binding returned above.
    usageCounters().countPlugin(PluginUse::kParserBind, selection.scalar->provider_id);
    if (selection.object.has_value() && selection.object->provider_id != selection.scalar->provider_id) {
      usageCounters().countPlugin(PluginUse::kParserBind, selection.object->provider_id);
    }
    auto parser = std::make_unique<MessageParserHandle>(std::move(selection.scalar->parser));

    // Reuse the dataset's same-named scalar topic: the transactional refill (SessionManager::beginRefill) keeps
    // TopicIds registered so a reload or import promotion writes back into them and every curve key survives; minting a
    // new topic here would renumber every TopicId of a delegated-ingest reload and drop every bound curve. Scan, create
    // and mirror under the engine lock(s) for the whole sequence: getTopicStorage() does not lock, and the GUI thread
    // may free a TopicStorage meanwhile (dataset removal). lockEnginePair is the helper the direct-write path
    // (WriteCore::ensureTopic) locks through, so the two never invert. The mutex is recursive (createTopic re-locks).
    // The engine name as the direct-write path stores it; the binding keeps the source's name.
    const std::string engine_topic_name = normalizeTopicName(topic_name);
    TopicId topic_id = 0;
    {
      const EngineLockPair engine_locks = lockEnginePair(self->engine_, self->secondary_data_engine_);

      auto existing_ids = self->engine_.listTopics(self->dataset_id_);
      std::sort(existing_ids.begin(), existing_ids.end());
      for (const TopicId tid : existing_ids) {
        const auto* storage = self->engine_.getTopicStorage(tid);
        if (storage != nullptr && storage->descriptor().name == engine_topic_name) {
          topic_id = tid;
          break;
        }
      }
      if (topic_id == 0) {
        auto topic_or = self->engine_.createTopic(self->dataset_id_, TopicDescriptor{.name = engine_topic_name});
        if (!topic_or.has_value()) {
          return self->fail(
              out_error, ("failed to create topic '" + engine_topic_name + "': " + topic_or.error()).c_str());
        }
        topic_id = *topic_or;
      }

      // Mirror into the secondary with the same TopicId: the engines' counters drift whenever the primary gets topics
      // the secondary does not (a file loaded between streams), and a push uses one id against whichever engine is
      // active. On reuse the secondary may already hold the id: mirror only when absent (name-blind, as the
      // direct-write mirror).
      if (self->secondary_data_engine_ != nullptr &&
          self->secondary_data_engine_->getTopicStorage(topic_id) == nullptr) {
        auto mirrored = self->secondary_data_engine_->createTopic(
            self->dataset_id_, TopicDescriptor{.name = engine_topic_name}, topic_id);
        if (!mirrored.has_value()) {
          return self->fail(
              out_error,
              ("failed to mirror topic '" + engine_topic_name + "' into secondary engine: " + mirrored.error())
                  .c_str());
        }
      }
    }
    const PJ_topic_handle_t topic_handle{static_cast<uint32_t>(topic_id)};

    auto write_host = std::make_unique<DatastoreParserWriteHost>(self->engine_, topic_handle);
    // Lockstep for parser plugins caching handles across messages: every ensureField is mirrored to the secondary with
    // the same FieldId, so a cached handle resolves after a pause/resume swap.
    write_host->setSecondaryEngine(self->secondary_data_engine_);
    // A binding minted while paused writes to the active (secondary) engine, or its samples land on the frozen
    // primary and stretch the timeline.
    write_host->setTarget(self->data_engine_target_.load());

    // The plugin may keep a view into the builder: it lives in the ParserBinding, as long as the parser.
    auto registry_builder = std::make_unique<ServiceRegistryBuilder>();
    // message_parser_plugin_base require<>()s it: fail the binding here, not later as an unexplained parse failure.
    if (auto status = registerRequiredService<sdk::ParserWriteHostService>(*registry_builder, write_host->raw());
        !status) {
      return self->fail(out_error, ("failed to register the parser write host: " + status.error()).c_str());
    }

    const sdk::BuiltinObjectType object_kind =
        selection.object.has_value() ? selection.object->object_type : sdk::BuiltinObjectType::kNone;
    std::optional<ObjectTopicId> object_topic_id;
    std::unique_ptr<DatastoreParserObjectWriteHost> object_write_host;
    if (object_kind == sdk::BuiltinObjectType::kNone) {
      // Scalar columns only; traced once per binding for anyone chasing why a topic is not an object topic.
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
          // Fail closed: keep the existing topic and decoder, write no object through the replacement.
        } else {
          // Known limitation: a same-provider retype to another builtin type keeps the original metadata_json until the
          // stream restarts.
          object_topic_id = existing;
        }
      } else {
        // The wire format travels with the topic: a dataset merge checks it before fusing same-named topics
        // (objectPayloadFormatFromMetadata).
        const std::string metadata_json =
            nlohmann::json{
                {"builtin_object_type", sdk::name(object_kind)},
                {"payload_encoding", std::string(encoding)},
                {"payload_schema", std::string(type_name)},
            }
                .dump();
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
        // Mirror into the secondary with the same ObjectTopicId (the stores' counters drift, as the engines' do).
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
        // When the routes chose different providers, the scalar parser writes no object: the object winner owns the
        // topic's objects, which flow through the lazy push and decode through the registrar instance below.
        if (object_provider_id == selection.scalar->provider_id) {
          object_write_host =
              std::make_unique<DatastoreParserObjectWriteHost>(self->object_store_, object_topic_id->id);
          // Mid-pause rule, as for the scalar host above.
          object_write_host->setTarget(self->object_store_target_.load());
          // Required here, though optional in general: the topic is committed to object ingest, and a parser bound
          // without an object sink would drop every payload silently.
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
          // One decoder slot per object topic; the provider guard above keeps another provider out. A replacement build
          // with the same provider id would still need per-entry pinning.
          self->object_topic_parser_registrar_(
              *object_topic_id, std::make_unique<MessageParserHandle>(std::move(selection.object->parser)));
        }
      }
    }

    // Terminal: the resolver is not re-entered for another candidate.
    if (auto status = parser->bind(registry_builder->view()); !status) {
      return self->fail(out_error, ("failed to bind parser services: " + status.error()).c_str());
    }

    const uint32_t binding_id = self->next_binding_id_++;
    const auto inserted = self->parser_bindings_.emplace(
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
    inserted.first->second.object_only = object_topic_id.has_value() && selection.scalar_winner_object_only;

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

    // Only builtin-object bindings follow the object ingest policy: scalar topics stay eager, so a broad lazy default
    // cannot drop normal curves.
    const bool is_object_topic = binding.object_topic_id.has_value();
    const auto policy = is_object_topic
                            ? self->policy_resolver_.resolve(self->source_id_, binding.topic_name, binding.object_kind)
                            : sdk::ObjectIngestPolicy::kEager;

    // One log context per binding (worker thread; parser_bindings_ is unsynchronized).
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
      // No bytes on this thread, nothing to record: count the gap.
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
      // out_error is already filled; latch anyway: a capture whose bytes were never produced must not publish.
      self->ingest_callback_failures_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    // Wrap the anchor first, so every exit releases the plugin's buffer exactly once. kEager's closure keeps it for the
    // entry's lifetime, the lazy-scalars seed until the pool evicts it.
    auto payload_anchor = detail::wrapPayloadAnchor(payload.anchor, self->library_keepalive_);

    if (payload.data == nullptr && payload.size > 0) {
      return self->fail(out_error, "message data fetcher returned null data");
    }

    self->tap_eligible_messages_.fetch_add(1, std::memory_order_relaxed);
    // Raw bytes, before decoding; the binding travels with the message (see setRecordTap).
    if (self->hasRecordTap()) {
      if (const auto tap = self->recordTapSnapshot(); tap != nullptr) {
        self->recordMessage(tap, handle.id, binding, timestamp_ns, payload.data, payload.size);
      }
    }

    if (auto status = binding.parser->parse(timestamp_ns, Span<const uint8_t>(payload.data, payload.size)); !status) {
      // An object-only schema has no scalar route, so parse() reports an error on every message; that is not a failed
      // message. parse() still runs: a parser may validate or diagnose in it.
      if (!binding.object_only) {
        return self->fail(out_error, status.error().c_str());
      }
      if (!binding.object_only_logged) {
        binding.object_only_logged = true;
        qCInfo(lcIngestBind).noquote() << "[parser-bind] object-only schema on topic"
                                       << QString::fromStdString(binding.topic_name)
                                       << "— scalar parse skipped over:" << QString::fromStdString(status.error());
      }
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
    // File-backed tapped objects are decoded before storage and keep only their re-readable closure; untapped ones keep
    // the ingest-time seed so live-edge reads do not re-fetch.
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
        // A transient buffer dies with this call: the seed needs its own copy.
        seed = sdk::makePayloadView(std::vector<uint8_t>(payload.data, payload.data + payload.size));
      }
      if (auto status = self->object_store_target_.load()->pushLazyWithSeed(
              *binding.object_topic_id, timestamp_ns, std::move(seed), std::move(fallback));
          !status) {
        return self->fail(out_error, ("ObjectStore.pushLazyWithSeed failed: " + status.error()).c_str());
      }
      return true;
    }
    // Streaming has no source to re-read: kEager entries keep the payload even when a tap consumed it, charged its size
    // so the per-topic memory budget can evict them.
    if (auto status = self->object_store_target_.load()->pushLazy(
            *binding.object_topic_id, timestamp_ns, makeCapturedPayloadClosure(payload, std::move(payload_anchor)),
            static_cast<size_t>(payload.size));
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
    // Cached because the returned char* must stay valid until the next call (protocol contract). parserEncodings()
    // snapshots under the catalog's shared lock (this may run off the GUI thread), sorted and unique.
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

// The plugin's poll/stream thread, as cbEnsureParserBinding.
sdk::BuiltinObjectType DataSourceRuntimeHost::classifyAvailableTopic(const PJ_available_topic_t& topic) const noexcept {
  const std::string_view encoding(topic.parser_encoding.data, topic.parser_encoding.size);
  const std::string_view type_name(topic.type_name.data, topic.type_name.size);
  const Span<const uint8_t> schema_span(topic.schema.data, topic.schema.size);
  try {
    // An object exactly when some provider holds the object route: the bind-time resolution, sharing its cache.
    if (const auto object_type = catalog_.classifyParserObjectRoute(encoding, type_name, schema_span);
        object_type.has_value()) {
      return *object_type;
    }
  } catch (...) {}
  // Fallback: the type name's leaf segment (after the last '/' or '.') compared exactly with the two infra schemas, so
  // TF and CameraInfo stay classified without a parser. A substring match would pin `my_msgs/CameraInfoStatus`
  // always-subscribed.
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
    // Best-effort: never fail the plugin's poll loop over advertising.
    return true;
  }
}

// Source capture (attach_source_record / complete_ingest)

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
// set_dataset_metadata: validated against the session bounds and staged; published at the load's commit seam. A
// rejection returns false and a reason and never affects ingestion or capture.
bool DataSourceRuntimeHost::cbSetDatasetMetadata(
    void* ctx, PJ_string_view_t metadata_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    // A null pointer with a nonzero size is malformed (toStringView would keep the size over substitute storage).
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
  // No capture service in the browser: refuse without latching.
  (void)descriptor_json;
  return self->rejectAttachment(out_error, "source capture is not available in this build");
#else
  try {
    // A null pointer or empty size is malformed (toStringView would keep the size over substitute storage).
    if (descriptor_json.data == nullptr || descriptor_json.size == 0) {
      return self->rejectAttachment(out_error, "source record descriptor is empty");
    }
    const std::string_view bytes(descriptor_json.data, descriptor_json.size);
    // Refuse without latching or vetoing: the provider may correct and re-attach before the first push.
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
    // Outside the lock: the capture service's hook must not run under capture_mu_.
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
