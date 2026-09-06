// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ParserRoutingService.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <algorithm>
#include <chrono>
#include <memory>
#include <span>
#include <utility>

#include "pj_base/parser_route_claims_protocol.h"

namespace PJ {

namespace {

constexpr uint16_t kKnownRouteFlags = PJ_PARSER_ROUTE_FLAG_SCALAR_V1 | PJ_PARSER_ROUTE_FLAG_OBJECT_V1;

// A route failure travels to the plugin through PJ_error_t's 224-byte message
// buffer, behind the data source's own prefix. Anything longer is cut there
// anyway; cutting here keeps the truncation visible and the full text goes to
// the diagnostic sink.
constexpr size_t kFailureBudget = 200;

// Rejects raw tags outside the SDK enum (name() maps unknown tags to "kNone").
bool isKnownObjectTag(uint16_t raw_type) {
  const auto type = static_cast<sdk::BuiltinObjectType>(raw_type);
  return type == sdk::BuiltinObjectType::kNone || sdk::parseBuiltinObjectType(sdk::name(type)) == type;
}

// Legacy classify_schema through the vtable, with failure kept distinct from a
// genuine kNone (MessageParserHandle::classifySchema folds the two together,
// which would memoize a transient failure as a permanent decline). Returns the
// validated tag; kNone when the plugin has no classify_schema slot.
Expected<sdk::BuiltinObjectType> rawClassifySchema(
    const MessageParserHandle& handle, std::string_view type_name, Span<const uint8_t> schema) {
  const PJ_message_parser_vtable_t* vtable = handle.vtable();
  if (!PJ_HAS_TAIL_SLOT(PJ_message_parser_vtable_t, vtable, classify_schema)) {
    return sdk::BuiltinObjectType::kNone;
  }
  const PJ_string_view_t tn{type_name.data(), type_name.size()};
  const PJ_bytes_view_t sc{schema.data(), schema.size()};
  PJ_schema_classification_t classification{};
  PJ_error_t error{};
  if (!vtable->classify_schema(handle.context(), tn, sc, &classification, &error)) {
    return unexpected(sdk::errorToString(error));
  }
  if (!isKnownObjectTag(classification.object_type)) {
    return unexpected("classify_schema reported an unknown object_type tag");
  }
  return static_cast<sdk::BuiltinObjectType>(classification.object_type);
}

}  // namespace

ParserRoutingService::ParserRoutingService(CreateHandleCallback create_handle, DiagnosticSink sink)
    : create_handle_(std::move(create_handle)),
      sink_(std::move(sink)),
      claim_catalog_(sink_, "ParserRoutingService"),
      route_resolver_(sink_, "ParserRoutingService") {}

std::string ParserRoutingService::sha256Digest(const void* data, size_t size) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayView(static_cast<const char*>(data), static_cast<qsizetype>(size)));
  return "sha256:" + hash.result().toHex().toStdString();
}

ParserRoutingService::RouteClassification ParserRoutingService::classifyPreparedInstance(
    const MessageParserHandle& handle, std::string_view type_name, Span<const uint8_t> schema) {
  RouteClassification result;
  const auto* extension =
      static_cast<const PJ_parser_route_claims_v1_t*>(handle.getPluginExtension(PJ_PARSER_ROUTE_CLAIMS_EXTENSION_V1));
  if (extension == nullptr) {
    // Extension absent: legacy classify_schema covers the OBJECT route only;
    // the scalar route is the universal wildcard.
    auto kind = rawClassifySchema(handle, type_name, schema);
    if (!kind) {
      result.error = std::move(kind.error());
      return result;
    }
    result.valid = true;
    result.object_type = *kind;
    result.object_claimed = result.object_type != sdk::BuiltinObjectType::kNone;
    return result;
  }

  // A present extension is authoritative. A malformed table is a provider
  // failure, never permission to fall back to legacy classify_schema.
  if (extension->struct_size < PJ_PARSER_ROUTE_CLAIMS_V1_MIN_SIZE) {
    result.error = "route-claims extension struct_size is too small";
    return result;
  }
  if (extension->classify_routes == nullptr) {
    result.error = "route-claims extension classify_routes is null";
    return result;
  }

  const PJ_string_view_t tn{type_name.data(), type_name.size()};
  const PJ_bytes_view_t sc{schema.data(), schema.size()};
  PJ_route_classification_v1_t classification{};
  PJ_error_t error{};
  if (!extension->classify_routes(handle.context(), tn, sc, &classification, &error)) {
    result.error = sdk::errorToString(error);
    return result;
  }

  if (classification.match != PJ_PARSER_ROUTE_MATCH_EXACT_V1) {
    result.error = "route classification reported a non-exact match";
    return result;
  }
  if (classification.status != PJ_PARSER_ROUTE_STATUS_CLAIMED_V1 &&
      classification.status != PJ_PARSER_ROUTE_STATUS_DECLINED_V1) {
    result.error = "route classification reported an invalid status";
    return result;
  }
  if ((classification.route_flags & ~kKnownRouteFlags) != 0) {
    result.error = "route classification reported unknown route flag bits";
    return result;
  }
  if (classification.status == PJ_PARSER_ROUTE_STATUS_DECLINED_V1) {
    if (classification.route_flags != 0 || classification.object_type != PJ_BUILTIN_OBJECT_TYPE_NONE) {
      result.error = "declined route classification contains claimed route data";
      return result;
    }
    result.valid = true;
    return result;
  }

  const bool object_claimed = (classification.route_flags & PJ_PARSER_ROUTE_FLAG_OBJECT_V1) != 0;
  if (object_claimed && classification.object_type == PJ_BUILTIN_OBJECT_TYPE_NONE) {
    result.error = "object route classification has object_type NONE";
    return result;
  }
  if (!object_claimed && classification.object_type != PJ_BUILTIN_OBJECT_TYPE_NONE) {
    result.error = "route classification has object_type without an object route";
    return result;
  }
  if (!isKnownObjectTag(classification.object_type)) {
    result.error = "route classification reported an unknown object_type tag";
    return result;
  }

  result.valid = true;
  result.scalar_exact = (classification.route_flags & PJ_PARSER_ROUTE_FLAG_SCALAR_V1) != 0;
  result.object_claimed = object_claimed;
  result.object_type = static_cast<sdk::BuiltinObjectType>(classification.object_type);
  return result;
}

std::vector<std::string> ParserRoutingService::registeredEncodings(const ParserProviderInfo& provider) {
  std::vector<std::string> registered;
  for (const auto& encoding : provider.encodings) {
    if (isRegisteredParserEncoding(encoding)) {
      registered.push_back(encoding);
    }
  }
  return registered;
}

void ParserRoutingService::rebuild(std::vector<ParserProviderInfo> providers) {
  claim_catalog_.clear();
  discovery_memo_.clear();
  discovered_claims_.clear();
  route_resolver_.invalidateCatalog();
  providers_ = std::move(providers);
  const uint64_t generation = provider_generation_.fetch_add(1, std::memory_order_release) + 1;

  for (const auto& provider : providers_) {
    const std::vector<std::string> registered = registeredEncodings(provider);
    if (registered.empty()) {
      continue;
    }
    if (auto status = claim_catalog_.admitParserPlugin(provider.id, registered, {}, provider.provenance, generation);
        !status) {
      report(
          DiagnosticLevel::kWarning, provider.id,
          "Parser \"" + provider.id + "\" was not admitted into the route claim catalog: " + status.error());
    }
  }
}

std::optional<ParserRoutingService::RouteContext> ParserRoutingService::makeContext(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
    std::string_view parser_config_json, PreparedPool& pool, bool instantiate_winner) {
  if (!isRegisteredParserEncoding(encoding)) {
    return std::nullopt;
  }
  auto normalized_type = normalizeParserTypeName(encoding, type_name);
  if (!normalized_type) {
    return std::nullopt;
  }
  return RouteContext{
      .encoding = std::string(encoding),
      .normalized_type_name = std::move(*normalized_type),
      .schema = schema,
      .schema_digest = sha256Digest(schema.data(), schema.size()),
      .parser_config_json = parser_config_json,
      .config_digest = parser_config_json.empty() ? std::string()
                                                  : sha256Digest(parser_config_json.data(), parser_config_json.size()),
      .prepared_pool = &pool,
      .instantiate_winner = instantiate_winner,
  };
}

const ParserProviderInfo* ParserRoutingService::firstProviderForEncoding(std::string_view encoding) const {
  for (const auto& provider : providers_) {
    if (std::find(provider.encodings.begin(), provider.encodings.end(), encoding) != provider.encodings.end()) {
      return &provider;
    }
  }
  return nullptr;
}

ParserRoutingService::PreparedInstance ParserRoutingService::prepareBoundInstance(
    std::string_view provider_id, std::string_view type_name, Span<const uint8_t> schema,
    std::string_view parser_config_json) const {
  PreparedInstance prepared;
  prepared.handle = create_handle_(provider_id);
  if (!prepared.handle.valid()) {
    prepared.diagnostic = "provider is not loaded";
    return prepared;
  }
  if (auto status = prepared.handle.bindSchema(type_name, schema); !status) {
    prepared.failure = ParserProbeOutcome::kDecline;
    prepared.diagnostic = "bind_schema: " + status.error();
    return prepared;
  }
  if (!parser_config_json.empty()) {
    if (auto status = prepared.handle.loadConfig(parser_config_json); !status) {
      prepared.diagnostic = "load_config: " + status.error();
    }
  }
  return prepared;
}

ParserRoutingService::PreparedInstance ParserRoutingService::prepareInstance(
    const RouteContext& context, std::string_view provider_id, bool consume_pool) const {
  if (consume_pool) {
    const auto pooled = context.prepared_pool->find(std::string(provider_id));
    if (pooled != context.prepared_pool->end()) {
      PreparedInstance prepared;
      prepared.handle = std::move(pooled->second);
      context.prepared_pool->erase(pooled);
      return prepared;
    }
  }
  PreparedInstance prepared =
      prepareBoundInstance(provider_id, context.normalized_type_name, context.schema, context.parser_config_json);
  if (!prepared.ok() || context.parser_config_json.empty()) {
    return prepared;
  }
  // bindSchema captured the handle's expected object type BEFORE the config
  // existed; a config-dependent classifier (parser_ros' RobotDescription rule)
  // may answer differently now, and parseObject validates emitted objects
  // against that expectation. A failed refresh must not leave the stale
  // expectation in place, so on this route-dispatch path it rejects the
  // instance (the legacy path keeps its failure-as-kNone contract).
  auto kind = rawClassifySchema(prepared.handle, context.normalized_type_name, context.schema);
  if (!kind) {
    prepared.diagnostic = "classify_schema after load_config: " + kind.error();
    return prepared;
  }
  // Only the handle's own call updates its expectation; a kNone answer needs
  // no update because nothing reads the expectation on a scalar-only topic.
  if (*kind != sdk::BuiltinObjectType::kNone) {
    static_cast<void>(prepared.handle.classifySchema(context.normalized_type_name, context.schema));
  }
  return prepared;
}

ParserRouteSelection ParserRoutingService::resolveLegacy(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
    std::string_view parser_config_json) const {
  ParserRouteSelection selection;
  const ParserProviderInfo* provider = firstProviderForEncoding(encoding);
  if (provider == nullptr) {
    selection.scalar_failure = "no parser found for encoding '" + std::string(encoding) + "'";
    return selection;
  }
  PreparedInstance scalar = prepareBoundInstance(provider->id, type_name, schema, parser_config_json);
  if (!scalar.ok()) {
    selection.scalar_failure = std::move(scalar.diagnostic);
    return selection;
  }
  // The pre-routing host asked the one selected parser; classifySchema's
  // failure-as-kNone fold is that path's contract (and it refreshes the
  // handle's expectation after the config load).
  const sdk::BuiltinObjectType kind = scalar.handle.classifySchema(type_name, schema);
  if (kind == sdk::BuiltinObjectType::kNone) {
    selection.scalar = ParserRouteWinner{.provider_id = provider->id, .parser = std::move(scalar.handle)};
    return selection;
  }
  // The object decoder is a second instance of the same provider. Its failure
  // fails the whole binding, as it always has: a topic committed to object
  // ingest must never silently degrade to scalar-only.
  PreparedInstance object = prepareBoundInstance(provider->id, type_name, schema, parser_config_json);
  if (!object.ok()) {
    selection.scalar_failure = "object decoder: " + object.diagnostic;
    return selection;
  }
  selection.scalar = ParserRouteWinner{.provider_id = provider->id, .parser = std::move(scalar.handle)};
  selection.object =
      ParserRouteWinner{.provider_id = provider->id, .parser = std::move(object.handle), .object_type = kind};
  return selection;
}

void ParserRoutingService::discoverParserExactClaims(const RouteContext& context) const {
  const std::string memo_key = context.encoding + "\n" + context.normalized_type_name + "\n" + context.schema_digest +
                               "\n" + context.config_digest;
  for (const auto& provider : providers_) {
    if (std::find(provider.encodings.begin(), provider.encodings.end(), context.encoding) == provider.encodings.end()) {
      continue;
    }
    auto& memo = discovery_memo_[provider.id];
    if (memo.contains(memo_key)) {
      continue;
    }

    PreparedInstance prepared = prepareInstance(context, provider.id, false);
    if (!prepared.ok()) {
      // bind_schema rejection is deterministic for this schema. Creation and
      // config failures may be transient and are retried next time.
      if (prepared.failure == ParserProbeOutcome::kDecline) {
        memo.insert(memo_key);
      }
      continue;
    }

    const RouteClassification classification =
        classifyPreparedInstance(prepared.handle, context.normalized_type_name, context.schema);
    context.prepared_pool->insert_or_assign(provider.id, std::move(prepared.handle));

    const auto report_discovery = [&](DiagnosticLevel level, std::string_view prefix, std::string_view detail) {
      report(
          level, provider.id,
          std::string(prefix) + " for parser \"" + provider.id + "\" type \"" + context.normalized_type_name +
              "\": " + std::string(detail));
    };
    if (!classification.valid) {
      report_discovery(DiagnosticLevel::kWarning, "Route classification failed", classification.error);
      continue;
    }
    memo.insert(memo_key);
    if (!classification.scalar_exact && !classification.object_claimed) {
      continue;
    }

    const PJ_route_classification_v1_t abi_classification{
        .route_flags = static_cast<uint16_t>(
            (classification.scalar_exact ? PJ_PARSER_ROUTE_FLAG_SCALAR_V1 : 0) |
            (classification.object_claimed ? PJ_PARSER_ROUTE_FLAG_OBJECT_V1 : 0)),
        .match = PJ_PARSER_ROUTE_MATCH_EXACT_V1,
        .status = PJ_PARSER_ROUTE_STATUS_CLAIMED_V1,
        .object_type = static_cast<uint16_t>(
            classification.object_claimed ? classification.object_type : sdk::BuiltinObjectType::kNone),
    };
    const ParserPluginExactClaim exact{context.encoding, context.normalized_type_name, abi_classification, {}};
    auto claims = synthesizeParserPluginClaims(
        provider.id, std::span(&context.encoding, 1), std::span(&exact, 1), provider.provenance);
    if (!claims) {
      report_discovery(DiagnosticLevel::kWarning, "Discovered claim is invalid", claims.error());
      continue;
    }
    // The SDK has no exact-only synthesis entry point: one encoding + one exact
    // claim yields the wildcard plus the one exact claim. Drop the wildcard.
    std::erase_if(*claims, [](const ParserClaim& claim) { return claim.type_name == "*"; });
    if (claims->size() != 1) {
      report_discovery(DiagnosticLevel::kWarning, "Discovered claim is invalid", "expected exactly one exact claim");
      continue;
    }
    ParserClaim& fresh = claims->front();

    // Claims are coarse candidacy records: route flags UNION across the
    // (schema, config) observations of one type, so a scalar-only first sight
    // never freezes the object route out of candidacy. Probes reclassify
    // against the real schema/config anyway.
    auto& stored = discovered_claims_[provider.id];
    auto [it, inserted] = stored.try_emplace(fresh.claim_id, fresh);
    bool changed = inserted;
    if (!inserted) {
      ParserClaim& accumulated = it->second;
      const uint16_t union_flags = accumulated.route_flags | fresh.route_flags;
      if (union_flags != accumulated.route_flags) {
        accumulated.route_flags = union_flags;
        changed = true;
      }
      if ((fresh.route_flags & PJ_PARSER_ROUTE_FLAG_OBJECT_V1) != 0 && accumulated.object_type != fresh.object_type) {
        if (accumulated.object_type.has_value()) {
          report_discovery(
              DiagnosticLevel::kWarning, "Discovered object type changed",
              "replacing the previously accumulated object type");
        }
        accumulated.object_type = fresh.object_type;
        changed = true;
      }
    }
    if (!changed) {
      continue;
    }

    // The catalog has no per-claim merge: re-admit the provider's whole set.
    auto batch = synthesizeParserPluginClaims(provider.id, registeredEncodings(provider), {}, provider.provenance);
    if (!batch) {
      report_discovery(DiagnosticLevel::kWarning, "Could not rebuild provider claims", batch.error());
      continue;
    }
    for (const auto& [claim_id, claim] : stored) {
      static_cast<void>(claim_id);
      batch->push_back(claim);
    }
    static_cast<void>(claim_catalog_.removeProvider(provider.id));
    if (auto status = claim_catalog_.admitClaims(
            std::move(*batch), provider.provenance, provider_generation_.load(std::memory_order_relaxed));
        !status) {
      report_discovery(DiagnosticLevel::kWarning, "Could not rebuild provider claims", status.error());
    }
  }
}

void ParserRoutingService::invalidateProvider(std::string_view provider_id) const {
  route_resolver_.invalidateProviderConfig(provider_id);
  discovery_memo_.erase(std::string(provider_id));
}

ParserRoutingService::RouteOutcome ParserRoutingService::resolveOneRoute(
    const RouteContext& context, ParserRoute route) const {
  RouteOutcome outcome;
  const ParserProviderConfigLookup provider_config = [&](std::string_view) {
    return ParserProviderConfig{std::string(context.parser_config_json), context.config_digest};
  };

  // A probed instance goes back into the pool: if its provider wins, the
  // winner step takes it instead of preparing a second one.
  const ParserProbeCallback probe = [&](const ParserProbeRequest& request) -> ParserProbeDecision {
    const std::string& provider_id = request.claim.provider_id;
    PreparedInstance prepared = prepareInstance(context, provider_id, true);
    if (!prepared.ok()) {
      return {prepared.failure, std::move(prepared.diagnostic), {}};
    }
    const bool wildcard_scalar = route == ParserRoute::kScalar && request.claim.type_name == "*";
    if (wildcard_scalar) {
      context.prepared_pool->insert_or_assign(provider_id, std::move(prepared.handle));
      return {ParserProbeOutcome::kAccept, {}, {}};
    }

    const RouteClassification classification =
        classifyPreparedInstance(prepared.handle, context.normalized_type_name, context.schema);
    context.prepared_pool->insert_or_assign(provider_id, std::move(prepared.handle));
    if (!classification.valid) {
      return {ParserProbeOutcome::kError, "route classification failed: " + classification.error, {}};
    }
    const bool claimed = route == ParserRoute::kScalar ? classification.scalar_exact : classification.object_claimed;
    if (!claimed) {
      return {ParserProbeOutcome::kDecline, "provider does not claim this route for the bound schema/config", {}};
    }
    return {ParserProbeOutcome::kAccept, {}, std::make_shared<RouteClassification>(classification)};
  };

  const ParserRouteRequest request{
      .encoding = context.encoding,
      .type_name = context.normalized_type_name,
      .schema_digest = context.schema_digest,
      .route = route,
  };
  auto resolution = route_resolver_.resolve(request, claim_catalog_, {}, provider_config, probe);
  if (!resolution) {
    outcome.failure = resolution.error();
    return outcome;
  }

  // Cached probe errors would freeze a transient failure until reload. Evict
  // every erroring provider on both selected and unselected outcomes.
  for (const auto& entry : resolution->trace) {
    if (entry.kind == ParserSelectionTraceKind::kProbeError) {
      invalidateProvider(entry.claim.provider_id);
    }
  }

  if (resolution->status != ParserRouteResolutionStatus::kSelected) {
    outcome.no_candidates = resolution->status == ParserRouteResolutionStatus::kNoCandidates;
    if (outcome.no_candidates) {
      outcome.failure = "no parser claims this route";
      return outcome;
    }
    // Report the probe verdicts themselves and nothing else: this text travels
    // to the plugin through PJ_error_t's fixed message buffer, so every byte of
    // candidate-trace noise pushes the plugin's own reason off the end.
    for (const auto& entry : resolution->trace) {
      if (entry.kind != ParserSelectionTraceKind::kProbeDecline &&
          entry.kind != ParserSelectionTraceKind::kProbeError) {
        continue;
      }
      outcome.failure += (outcome.failure.empty() ? "" : "; ") + entry.claim.provider_id + ": " + entry.detail;
    }
    if (outcome.failure.empty()) {
      outcome.failure = "no claimant accepted the route";
    } else if (outcome.failure.size() > kFailureBudget) {
      outcome.failure.resize(kFailureBudget - 3);
      outcome.failure += "...";
    }
    return outcome;
  }

  const std::string& winner_id = resolution->winner->provider_id;
  if (!context.instantiate_winner) {
    const auto* classification = static_cast<const RouteClassification*>(resolution->retained_instance.get());
    if (classification == nullptr || !classification->valid || !classification->object_claimed ||
        classification->object_type == sdk::BuiltinObjectType::kNone) {
      outcome.failure = "winning provider \"" + winner_id + "\" has no cached object-route classification";
      invalidateProvider(winner_id);
      return outcome;
    }
    outcome.winner = ParserRouteWinner{
        .provider_id = winner_id,
        .claim_id = resolution->winner->claim_id,
        .parser = MessageParserHandle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)},
        .object_type = classification->object_type,
    };
    return outcome;
  }

  PreparedInstance winner = prepareInstance(context, winner_id, true);
  if (!winner.ok()) {
    outcome.failure = "winning provider \"" + winner_id + "\" failed instantiation: " + winner.diagnostic;
    invalidateProvider(winner_id);
    return outcome;
  }

  sdk::BuiltinObjectType winner_object_type = sdk::BuiltinObjectType::kNone;
  if (route == ParserRoute::kObject) {
    // The resolver may answer from its probe cache (a record from another
    // topic's probe); the instance that will decode this topic is authoritative.
    const RouteClassification classification =
        classifyPreparedInstance(winner.handle, context.normalized_type_name, context.schema);
    if (!classification.valid || !classification.object_claimed ||
        classification.object_type == sdk::BuiltinObjectType::kNone) {
      outcome.failure = "winning provider \"" + winner_id + "\" no longer claims the object route on instantiation";
      invalidateProvider(winner_id);
      return outcome;
    }
    winner_object_type = classification.object_type;
  }

  outcome.winner = ParserRouteWinner{
      .provider_id = winner_id,
      .claim_id = resolution->winner->claim_id,
      .parser = std::move(winner.handle),
      .object_type = winner_object_type,
  };
  return outcome;
}

ParserRouteSelection ParserRoutingService::resolveParserRoutes(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
    std::string_view parser_config_json) const {
  PreparedPool prepared_pool;
  auto context = makeContext(encoding, type_name, schema, parser_config_json, prepared_pool, true);
  if (!context) {
    return resolveLegacy(encoding, type_name, schema, parser_config_json);
  }

  // THREADING: this runs on the source poll thread — the same thread class the
  // legacy ensure-binding path has always used for create/bindSchema/loadConfig
  // during live ingest; route dispatch adds volume, not a new thread class.
  ParserRouteSelection selection;
  selection.route_dispatch = true;
  discoverParserExactClaims(*context);

  RouteOutcome scalar = resolveOneRoute(*context, ParserRoute::kScalar);
  selection.scalar = std::move(scalar.winner);
  selection.scalar_failure = std::move(scalar.failure);
  if (!selection.scalar) {
    report(
        DiagnosticLevel::kWarning, {},
        "No parser accepted type \"" + context->normalized_type_name + "\" (" + context->encoding +
            "): " + selection.scalar_failure);
  }

  RouteOutcome object = resolveOneRoute(*context, ParserRoute::kObject);
  selection.object = std::move(object.winner);
  if (!selection.object && !object.no_candidates) {
    report(
        DiagnosticLevel::kInfo, {},
        "Object route unavailable for type \"" + context->normalized_type_name + "\" (" + context->encoding +
            "): " + object.failure);
  }
  return selection;
}

std::optional<sdk::BuiltinObjectType> ParserRoutingService::classifyParserObjectRoute(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema) const {
  PreparedPool prepared_pool;
  auto context = makeContext(encoding, type_name, schema, {}, prepared_pool, false);
  if (!context) {
    const ParserProviderInfo* provider = firstProviderForEncoding(encoding);
    if (provider == nullptr) {
      return std::nullopt;
    }
    PreparedInstance instance = prepareBoundInstance(provider->id, type_name, schema, {});
    if (!instance.ok()) {
      return std::nullopt;
    }
    const sdk::BuiltinObjectType kind = instance.handle.classifySchema(type_name, schema);
    return kind == sdk::BuiltinObjectType::kNone ? std::nullopt : std::optional(kind);
  }
  discoverParserExactClaims(*context);
  RouteOutcome outcome = resolveOneRoute(*context, ParserRoute::kObject);
  return outcome.winner.has_value() ? std::optional(outcome.winner->object_type) : std::nullopt;
}

void ParserRoutingService::report(DiagnosticLevel level, std::string_view id, std::string message) const {
  if (!sink_) {
    return;
  }
  sink_(
      Diagnostic{
          .level = level,
          .source = "ParserRoutingService",
          .id = std::string(id),
          .message = std::move(message),
          .timestamp = std::chrono::system_clock::now(),
      });
}

}  // namespace PJ
