#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/span.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/parser_claim_catalog.hpp"
#include "pj_plugins/host/parser_route_resolver.hpp"

namespace PJ {

// Immutable parser metadata captured by ExtensionCatalogService under its
// catalog lock and pushed into the routing service on each rebuild. Snapshot
// order is catalog order, which the legacy single-provider selection relies on.
struct ParserProviderInfo {
  std::string id;
  std::vector<std::string> encodings;
  ParserClaimProvenance provenance = ParserClaimProvenance::kMarketplace;
};

// The winning provider for one parser route of one topic, carrying a
// prepared plugin instance (created, bindSchema and loadConfig applied) that
// no other binding shares.
struct ParserRouteWinner {
  std::string provider_id;
  // The SDK claim that won; empty on the legacy single-provider selection.
  std::string claim_id;
  MessageParserHandle parser;
  // The object route's claimed builtin type; kNone on the scalar route.
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
};

// Per-route selection for one (encoding, type, schema, config) topic. The
// scalar winner takes the binding; the object winner (absent when nobody
// claims the object route) decides the topic's builtin object identity and
// serves as its lazy decoder.
struct ParserRouteSelection {
  // True when the SDK claim catalog resolved the routes. False when the
  // encoding is outside the SDK registry (or the type name cannot be
  // normalized): both winners then come from the first provider registered
  // for the encoding, which is the same parser the pre-routing host selected.
  bool route_dispatch = false;
  std::optional<ParserRouteWinner> scalar;
  std::optional<ParserRouteWinner> object;
  std::string scalar_failure;
};

// Route-aware parser selection over the SDK claim catalog + route resolver.
// The owning catalog supplies a provider snapshot on rebuild and a callback
// that creates handles by provider id.
//
// EXTERNALLY SYNCHRONIZED: this class has no mutex of its own. The owner
// serializes rebuild/resolve/classify under one lock and holds its catalog lock
// for the whole call (shared for resolve/classify, exclusive for rebuild), so
// one resolution never observes a catalog swap between its probes and its
// winner creation; create_handle_ runs with that catalog lock already held and
// must not lock it again.
class ParserRoutingService {
 public:
  using CreateHandleCallback = std::function<MessageParserHandle(std::string_view provider_id)>;

  explicit ParserRoutingService(CreateHandleCallback create_handle, DiagnosticSink sink = {});

  // Replaces the provider snapshot, clears claims/memo/discovered exact claims,
  // advances generation, and admits registered wildcard claims.
  void rebuild(std::vector<ParserProviderInfo> providers);

  [[nodiscard]] ParserRouteSelection resolveParserRoutes(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
      std::string_view parser_config_json) const;

  // Object identity of an advertised topic without a config: the object route's
  // claimed type, or nullopt when no provider claims it. Registry encodings
  // answer from cached classification records and never instantiate a winner;
  // legacy encodings classify on a throwaway instance.
  [[nodiscard]] std::optional<sdk::BuiltinObjectType> classifyParserObjectRoute(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema) const;

 private:
  struct RouteClassification {
    bool valid = false;
    bool scalar_exact = false;
    bool object_claimed = false;
    sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
    std::string error;
  };

  struct PreparedInstance {
    MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
    ParserProbeOutcome failure = ParserProbeOutcome::kError;
    std::string diagnostic;

    [[nodiscard]] bool ok() const {
      return diagnostic.empty();
    }
  };

  // Instances prepared during one resolution, keyed by provider id, so the
  // discovery / probe instance becomes the winner instead of being recreated.
  // Scoped to one call: no instance is ever shared between two bindings.
  using PreparedPool = std::unordered_map<std::string, MessageParserHandle>;

  struct RouteContext {
    std::string encoding;
    std::string normalized_type_name;
    Span<const uint8_t> schema;
    std::string schema_digest;
    std::string_view parser_config_json;
    std::string config_digest;
    PreparedPool* prepared_pool = nullptr;  // never null; one pool per resolution
    bool instantiate_winner = true;
  };

  struct RouteOutcome {
    std::optional<ParserRouteWinner> winner;
    std::string failure;
    bool no_candidates = false;
  };

  [[nodiscard]] static RouteClassification classifyPreparedInstance(
      const MessageParserHandle& handle, std::string_view type_name, Span<const uint8_t> schema);
  [[nodiscard]] static std::string sha256Digest(const void* data, size_t size);
  [[nodiscard]] static std::vector<std::string> registeredEncodings(const ParserProviderInfo& provider);

  // nullopt when the encoding is outside the SDK registry or the type name
  // cannot be normalized: the legacy single-provider selection applies.
  [[nodiscard]] static std::optional<RouteContext> makeContext(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
      std::string_view parser_config_json, PreparedPool& pool, bool instantiate_winner);

  [[nodiscard]] const ParserProviderInfo* firstProviderForEncoding(std::string_view encoding) const;
  [[nodiscard]] PreparedInstance prepareInstance(
      const RouteContext& context, std::string_view provider_id, bool consume_pool) const;
  // create + bindSchema + loadConfig, no classification.
  [[nodiscard]] PreparedInstance prepareBoundInstance(
      std::string_view provider_id, std::string_view type_name, Span<const uint8_t> schema,
      std::string_view parser_config_json) const;
  [[nodiscard]] ParserRouteSelection resolveLegacy(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
      std::string_view parser_config_json) const;
  void discoverParserExactClaims(const RouteContext& context) const;
  [[nodiscard]] RouteOutcome resolveOneRoute(const RouteContext& context, ParserRoute route) const;
  void invalidateProvider(std::string_view provider_id) const;
  void report(DiagnosticLevel level, std::string_view id, std::string message) const;

  CreateHandleCallback create_handle_;
  DiagnosticSink sink_;

  std::vector<ParserProviderInfo> providers_;
  mutable ParserClaimCatalog claim_catalog_;
  mutable ParserRouteResolver route_resolver_;
  // provider id -> discovered (encoding, type, schema digest, config digest)
  // keys. Rebuild clears it under the owner's lock, so no generation component.
  mutable std::unordered_map<std::string, std::unordered_set<std::string>> discovery_memo_;
  mutable std::unordered_map<std::string, std::map<std::string, ParserClaim>> discovered_claims_;
  std::atomic<uint64_t> provider_generation_{0};
};

}  // namespace PJ
