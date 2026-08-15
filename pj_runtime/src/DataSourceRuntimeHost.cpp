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
#include "pj_datastore/engine.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ObjectIngestTap.h"
#include "pj_runtime/ServiceRegistration.h"
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
  last_error_.assign(reason.data(), reason.size());
  stop_requested_.store(true);
}

void DataSourceRuntimeHost::requestStop() {
  stop_requested_.store(true);
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

bool DataSourceRuntimeHost::fail(PJ_error_t* out_error, const char* message) noexcept {
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

void DataSourceRuntimeHost::cbRequestStop(
    void* ctx, PJ_data_source_state_t /*terminal*/, PJ_string_view_t reason) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  self->requestStop(std::string_view(reason.data, reason.size));
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

bool DataSourceRuntimeHost::cbEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataSourceRuntimeHost*>(ctx);
  try {
    const std::string_view encoding(request->parser_encoding.data, request->parser_encoding.size);
    const std::string_view topic_name(request->topic_name.data, request->topic_name.size);
    const std::string_view type_name(request->type_name.data, request->type_name.size);
    const QString encoding_str = QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()));

    std::string parser_config;
    if (request->parser_config_json.size > 0) {
      parser_config.assign(request->parser_config_json.data, request->parser_config_json.size);
    }
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

    // Resolve + instantiate the parser atomically under the catalog's shared
    // lock (this runs on the plugin poll thread; the GUI thread may reload the
    // catalog). The returned handle owns its DSO keepalive, so it stays valid
    // past the lock and past a later reload().
    auto parser = std::make_unique<MessageParserHandle>(self->catalog_.createParserHandleForEncoding(encoding_str));
    if (!parser->valid()) {
      return self->fail(out_error, ("no parser found for encoding '" + std::string(encoding) + "'").c_str());
    }

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

    const Span<const uint8_t> schema_span(request->schema.data, request->schema.size);
    if (auto status = parser->bindSchema(type_name, schema_span); !status) {
      return self->fail(
          out_error, ("failed to bind schema for " + std::string(type_name) + ": " + status.error()).c_str());
    }

    if (!parser_config.empty()) {
      if (auto status = parser->loadConfig(parser_config); !status) {
        return self->fail(out_error, ("failed to load parser config: " + status.error()).c_str());
      }
    }

    const sdk::BuiltinObjectType object_kind = parser->classifySchema(type_name, schema_span);
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
      if (auto existing = self->object_store_.findTopic(self->dataset_id_, topic_name); existing.has_value()) {
        // KNOWN LIMITATION: a topic retyped to a DIFFERENT builtin object type
        // mid-session reuses this id and its original metadata_json, so the new
        // payloads keep routing as the old type until the stream restarts.
        // Acceptable for now — a mid-session builtin-type change is not a flow
        // any supported source produces.
        object_topic_id = existing;
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
      object_write_host = std::make_unique<DatastoreParserObjectWriteHost>(self->object_store_, object_topic_id->id);
      // Same mid-pause rule as the scalar host above: a binding minted while
      // paused must push into the active (secondary) store, not the frozen
      // primary. When live, object_store_target_ is the primary (a no-op).
      object_write_host->setTarget(self->object_store_target_.load());
      // Required at THIS point despite being an optional service in general: the
      // host only reaches here after committing this topic to object ingest (the
      // object topic is registered and mirrored). Merely warning would let the
      // parser bind with no object sink and drop every object payload silently.
      if (auto status =
              registerRequiredService<sdk::ParserObjectWriteHostService>(*registry_builder, object_write_host->raw());
          !status) {
        return self->fail(
            out_error,
            ("failed to register the parser object write host for '" + std::string(topic_name) + "': " + status.error())
                .c_str());
      }

      if (self->object_topic_parser_registrar_) {
        auto object_parser = std::make_unique<MessageParserHandle>(self->catalog_.createParserHandleForEncoding(
            QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size()))));
        if (!object_parser->valid()) {
          return self->fail(
              out_error, ("failed to create object parser instance for '" + std::string(encoding) + "'").c_str());
        }
        if (auto status = object_parser->bindSchema(type_name, schema_span); !status) {
          return self->fail(
              out_error,
              ("failed to bind object parser schema for " + std::string(type_name) + ": " + status.error()).c_str());
        }
        if (!parser_config.empty()) {
          if (auto status = object_parser->loadConfig(parser_config); !status) {
            return self->fail(out_error, ("failed to load object parser config: " + status.error()).c_str());
          }
        }
        self->object_topic_parser_registrar_(*object_topic_id, std::move(object_parser));
      }
    }

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

  try {
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
// catalog/parser access pattern cbEnsureParserBinding has always used
// (findParserByEncoding + createHandle + bindSchema during live ingest), so
// advertise-time classification introduces no cross-thread access that binding
// didn't already perform.
sdk::BuiltinObjectType DataSourceRuntimeHost::classifyAvailableTopic(const PJ_available_topic_t& topic) const noexcept {
  const std::string_view encoding(topic.parser_encoding.data, topic.parser_encoding.size);
  const std::string_view type_name(topic.type_name.data, topic.type_name.size);
  const Span<const uint8_t> schema_span(topic.schema.data, topic.schema.size);
  try {
    // Resolve + instantiate under the catalog's shared lock (poll thread vs a
    // GUI-thread reload()); the handle owns its DSO keepalive, so bindSchema /
    // classifySchema below run safely after the lock is released.
    MessageParserHandle parser =
        catalog_.createParserHandleForEncoding(QString::fromUtf8(encoding.data(), static_cast<int>(encoding.size())));
    if (parser.valid() && parser.bindSchema(type_name, schema_span)) {
      const sdk::BuiltinObjectType classification = parser.classifySchema(type_name, schema_span);
      if (classification != sdk::BuiltinObjectType::kNone) {
        return classification;
      }
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

}  // namespace PJ
