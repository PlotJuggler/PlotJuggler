// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Route-aware parser selection (SDK 0.22): claim admission, lazy exact-claim
// discovery, per-route winners, and the legacy fallback for encodings outside
// the SDK claim registry. Uses statically registered in-process mock parsers —
// no DSOs — so each test composes exactly the overlap it exercises.

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hermetic_catalog.h"
#include "mock_message_parser_vtable.h"
#include "pj_base/builtin_object_abi.h"
#include "pj_base/parser_route_claims_protocol.h"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace {

using namespace PJ;

constexpr std::string_view kExactType = "test/PointCloud";
constexpr std::string_view kLegacyType = "legacy/Cloud";
const std::vector<uint8_t> kSchema = {'s', 'c', 'h', 'e', 'm', 'a'};

Span<const uint8_t> schemaSpan() {
  return {kSchema.data(), kSchema.size()};
}

PJ_route_classification_v1_t exactClaim(bool claimed, uint16_t flags) {
  return PJ_route_classification_v1_t{
      .route_flags = static_cast<uint16_t>(claimed ? flags : 0),
      .match = PJ_PARSER_ROUTE_MATCH_EXACT_V1,
      .status = static_cast<uint16_t>(claimed ? PJ_PARSER_ROUTE_STATUS_CLAIMED_V1 : PJ_PARSER_ROUTE_STATUS_DECLINED_V1),
      .object_type = static_cast<uint16_t>(
          claimed && (flags & PJ_PARSER_ROUTE_FLAG_OBJECT_V1) != 0 ? PJ_BUILTIN_OBJECT_TYPE_POINTCLOUD
                                                                   : PJ_BUILTIN_OBJECT_TYPE_NONE),
  };
}

template <const PJ_parser_route_claims_v1_t* Claims>
const void* routeClaimsExtension(void*, PJ_string_view_t id) noexcept {
  return std::string_view(id.data, id.size) == PJ_PARSER_ROUTE_CLAIMS_EXTENSION_V1 ? Claims : nullptr;
}

// Route-claims extension claiming BOTH routes exactly for kExactType.
bool classifyBothRoutes(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  const std::string_view tn(type_name.data, type_name.size);
  *out = exactClaim(
      tn == kExactType, static_cast<uint16_t>(PJ_PARSER_ROUTE_FLAG_SCALAR_V1 | PJ_PARSER_ROUTE_FLAG_OBJECT_V1));
  return true;
}
const PJ_parser_route_claims_v1_t kBothRoutesClaims{sizeof(PJ_parser_route_claims_v1_t), classifyBothRoutes};

// Route-claims extension claiming ONLY the object route for kExactType.
bool classifyObjectOnly(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  const std::string_view tn(type_name.data, type_name.size);
  *out = exactClaim(tn == kExactType, PJ_PARSER_ROUTE_FLAG_OBJECT_V1);
  return true;
}
const PJ_parser_route_claims_v1_t kObjectOnlyClaims{sizeof(PJ_parser_route_claims_v1_t), classifyObjectOnly};

// Legacy classify_schema (no extension): objects for kLegacyType only.
bool legacyClassifySchema(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_schema_classification_t* out, PJ_error_t*) noexcept {
  const std::string_view tn(type_name.data, type_name.size);
  out->object_type = tn == kLegacyType ? PJ_BUILTIN_OBJECT_TYPE_POINTCLOUD : PJ_BUILTIN_OBJECT_TYPE_NONE;
  out->reserved = 0;
  return true;
}

// Extension-free parser whose legacy classify_schema FAILS while
// g_legacy_classify_failing is set and claims kLegacyType afterwards — models
// a provider recovering from a transient fault.
bool g_legacy_classify_failing = false;
bool flakyLegacyClassifySchema(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_schema_classification_t* out, PJ_error_t*) noexcept {
  if (g_legacy_classify_failing) {
    return false;
  }
  const std::string_view tn(type_name.data, type_name.size);
  out->object_type = tn == kLegacyType ? PJ_BUILTIN_OBJECT_TYPE_POINTCLOUD : PJ_BUILTIN_OBJECT_TYPE_NONE;
  out->reserved = 0;
  return true;
}
PJ_message_parser_vtable_t makeFlakyLegacyParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"ddd-flaky","name":"Flaky Legacy Parser","version":"1.0.0","encoding":["json"]})");
  vt.classify_schema = flakyLegacyClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kFlakyLegacyParser = makeFlakyLegacyParser();

// Route-claims extension that classifies kExactType VALIDLY on its first call
// (discovery admits a real claim) and then reports a tag outside the SDK enum
// on every later call (probe / winner) — so only end-to-end tag validation,
// not SDK claim admission, can reject it.
int g_unknown_tag_calls = 0;
bool classifyUnknownTag(
    void*, PJ_string_view_t, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  const bool first = g_unknown_tag_calls++ == 0;
  *out = PJ_route_classification_v1_t{
      .route_flags = PJ_PARSER_ROUTE_FLAG_OBJECT_V1,
      .match = PJ_PARSER_ROUTE_MATCH_EXACT_V1,
      .status = PJ_PARSER_ROUTE_STATUS_CLAIMED_V1,
      .object_type = static_cast<uint16_t>(first ? PJ_BUILTIN_OBJECT_TYPE_POINTCLOUD : 65535),
  };
  return true;
}

// Extension-free parser whose legacy classify_schema reports an unknown tag.
bool legacyUnknownTagClassifySchema(
    void*, PJ_string_view_t, PJ_bytes_view_t, PJ_schema_classification_t* out, PJ_error_t*) noexcept {
  out->object_type = 65535;
  out->reserved = 0;
  return true;
}
PJ_message_parser_vtable_t makeLegacyUnknownTagParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"zzz-legacy-unknown","name":"Legacy Unknown Tag Parser","version":"1.0.0","encoding":["json"]})");
  vt.classify_schema = legacyUnknownTagClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kLegacyUnknownTagParser = makeLegacyUnknownTagParser();
const PJ_parser_route_claims_v1_t kUnknownTagClaims{sizeof(PJ_parser_route_claims_v1_t), classifyUnknownTag};
PJ_message_parser_vtable_t makeUnknownTagParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"zzz-unknown-tag","name":"Unknown Tag Parser","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kUnknownTagClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kUnknownTagParser = makeUnknownTagParser();

// Plain wildcard-only parser on the registered "json" encoding.
const PJ_message_parser_vtable_t kWildcardParser = pj_mock::makeMockMessageParserVtable(
    R"({"id":"aaa-wildcard","name":"Wildcard Parser","version":"1.0.0","encoding":["json"]})");

// Route-aware parser holding exact scalar+object claims for kExactType.
PJ_message_parser_vtable_t makeBothRoutesParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"mmm-exact","name":"Exact Parser","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kBothRoutesClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kBothRoutesParser = makeBothRoutesParser();

// Route-aware parser holding ONLY an exact object claim for kExactType. Named
// after every other mock so the scalar wildcard tie-break never selects it.
PJ_message_parser_vtable_t makeObjectOnlyParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"zzz-object","name":"Object Parser","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kObjectOnlyClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kObjectOnlyParser = makeObjectOnlyParser();

// Extension-free parser whose legacy classify_schema claims kLegacyType.
PJ_message_parser_vtable_t makeLegacyObjectParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"bbb-legacy","name":"Legacy Parser","version":"1.0.0","encoding":["json"]})");
  vt.classify_schema = legacyClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kLegacyObjectParser = makeLegacyObjectParser();

// Parser on an encoding OUTSIDE the SDK claim registry.
const PJ_message_parser_vtable_t kCustomEncodingParser = pj_mock::makeMockMessageParserVtable(
    R"({"id":"ccc-custom","name":"Custom Encoding Parser","version":"1.0.0","encoding":["custom_enc"]})");

// Instances carry real state (the stored config) for config-gated mocks.
struct ConfigCtx {
  std::string config;
};
void* configCreate() noexcept {
  return new ConfigCtx();
}
void configDestroy(void* ctx) noexcept {
  delete static_cast<ConfigCtx*>(ctx);
}
bool configLoadConfig(void* ctx, PJ_string_view_t json, PJ_error_t*) noexcept {
  static_cast<ConfigCtx*>(ctx)->config.assign(json.data, json.size);
  return true;
}

// Second parser on the unregistered encoding, registered AFTER kCustomEncodingParser.
const PJ_message_parser_vtable_t kCustomEncodingParser2 = pj_mock::makeMockMessageParserVtable(
    R"({"id":"ddd-custom2","name":"Custom Encoding Parser 2","version":"1.0.0","encoding":["custom_enc"]})");

// Unregistered-encoding parser that claims an object type but whose SECOND
// instance (the object decoder) fails bind_schema — the legacy path must fail
// the binding rather than degrade it to scalar-only.
int g_flaky_decoder_creates = 0;
void* flakyDecoderCreate() noexcept {
  ++g_flaky_decoder_creates;
  return new int(g_flaky_decoder_creates);
}
void flakyDecoderDestroy(void* ctx) noexcept {
  delete static_cast<int*>(ctx);
}
bool flakyDecoderBindSchema(void* ctx, PJ_string_view_t, PJ_bytes_view_t, PJ_error_t*) noexcept {
  return *static_cast<int*>(ctx) != 2;
}
bool pointCloudClassifySchema(
    void*, PJ_string_view_t, PJ_bytes_view_t, PJ_schema_classification_t* out, PJ_error_t*) noexcept {
  out->object_type = PJ_BUILTIN_OBJECT_TYPE_POINTCLOUD;
  out->reserved = 0;
  return true;
}
PJ_message_parser_vtable_t makeFlakyDecoderParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"eee-flaky-decoder","name":"Flaky Decoder","version":"1.0.0","encoding":["custom_enc"]})");
  vt.create = flakyDecoderCreate;
  vt.destroy = flakyDecoderDestroy;
  vt.bind_schema = flakyDecoderBindSchema;
  vt.classify_schema = pointCloudClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kFlakyDecoderParser = makeFlakyDecoderParser();

// Unregistered-encoding parser whose classify_schema always fails: the legacy
// contract folds that to kNone and still binds scalars, config or not.
bool failingClassifySchema(
    void*, PJ_string_view_t, PJ_bytes_view_t, PJ_schema_classification_t*, PJ_error_t*) noexcept {
  return false;
}
PJ_message_parser_vtable_t makeLegacyFailingClassifyParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"fff-legacy-failing","name":"Legacy Failing Classify","version":"1.0.0","encoding":["custom_enc"]})");
  vt.classify_schema = failingClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kLegacyFailingClassifyParser = makeLegacyFailingClassifyParser();

// Config-carrying legacy parser whose classify_schema FAILS once a config
// containing "break_classify" is loaded — models a classifier that cannot
// answer for the configured topic, so the post-config refresh must reject the
// instance instead of leaving bindSchema's pre-config expectation in place.
bool configBreakingClassifySchema(
    void* ctx, PJ_string_view_t, PJ_bytes_view_t, PJ_schema_classification_t* out, PJ_error_t*) noexcept {
  if (static_cast<ConfigCtx*>(ctx)->config.find("break_classify") != std::string::npos) {
    return false;
  }
  out->object_type = PJ_BUILTIN_OBJECT_TYPE_NONE;
  out->reserved = 0;
  return true;
}
PJ_message_parser_vtable_t makeConfigBreakingParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"hhh-config-break","name":"Config Breaking Parser","version":"1.0.0","encoding":["json"]})");
  vt.create = configCreate;
  vt.destroy = configDestroy;
  vt.load_config = configLoadConfig;
  vt.classify_schema = configBreakingClassifySchema;
  return vt;
}
const PJ_message_parser_vtable_t kConfigBreakingParser = makeConfigBreakingParser();

// parser_ros' real config-dependent shape: the exact handler is scalar when
// ungated and object-only when robot_description config is present.
bool unionConfigClassifyRoutes(
    void* ctx, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  const bool matching = std::string_view(type_name.data, type_name.size) == "std/String";
  const bool gated = matching && static_cast<ConfigCtx*>(ctx)->config.find("robot_description") != std::string::npos;
  *out = exactClaim(
      matching, static_cast<uint16_t>(gated ? PJ_PARSER_ROUTE_FLAG_OBJECT_V1 : PJ_PARSER_ROUTE_FLAG_SCALAR_V1));
  return true;
}
const PJ_parser_route_claims_v1_t kUnionConfigClaims{sizeof(PJ_parser_route_claims_v1_t), unionConfigClassifyRoutes};
PJ_message_parser_vtable_t makeUnionConfigParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"ggg-union-config","name":"Union Config Parser","version":"1.0.0","encoding":["json"]})");
  vt.create = configCreate;
  vt.destroy = configDestroy;
  vt.load_config = configLoadConfig;
  vt.get_plugin_extension = routeClaimsExtension<&kUnionConfigClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kUnionConfigParser = makeUnionConfigParser();

// The same scalar-then-object-only transition keyed by schema revision.
bool unionSchemaClassifyRoutes(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t schema, PJ_route_classification_v1_t* out,
    PJ_error_t*) noexcept {
  const bool matching = std::string_view(type_name.data, type_name.size) == kExactType;
  const std::string_view bytes(reinterpret_cast<const char*>(schema.data), schema.size);
  const bool gated = matching && bytes == std::string_view("schema", 6);
  *out = exactClaim(
      matching, static_cast<uint16_t>(gated ? PJ_PARSER_ROUTE_FLAG_OBJECT_V1 : PJ_PARSER_ROUTE_FLAG_SCALAR_V1));
  return true;
}
const PJ_parser_route_claims_v1_t kUnionSchemaClaims{sizeof(PJ_parser_route_claims_v1_t), unionSchemaClassifyRoutes};
PJ_message_parser_vtable_t makeUnionSchemaParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"hhh-union-schema","name":"Union Schema Parser","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kUnionSchemaClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kUnionSchemaParser = makeUnionSchemaParser();

// First classification is valid so discovery admits an exact claim; probing
// the pooled prepared handle then returns a malformed object claim.
struct MalformedCtx {
  int classifications = 0;
};
void* malformedCreate() noexcept {
  return new MalformedCtx();
}
void malformedDestroy(void* ctx) noexcept {
  delete static_cast<MalformedCtx*>(ctx);
}
bool malformedClassifyRoutes(
    void* ctx, PJ_string_view_t, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  auto* state = static_cast<MalformedCtx*>(ctx);
  if (state->classifications++ == 0) {
    *out = exactClaim(true, PJ_PARSER_ROUTE_FLAG_SCALAR_V1);
  } else {
    *out = PJ_route_classification_v1_t{
        .route_flags = PJ_PARSER_ROUTE_FLAG_OBJECT_V1,
        .match = PJ_PARSER_ROUTE_MATCH_EXACT_V1,
        .status = PJ_PARSER_ROUTE_STATUS_CLAIMED_V1,
        .object_type = PJ_BUILTIN_OBJECT_TYPE_NONE,
    };
  }
  return true;
}
const PJ_parser_route_claims_v1_t kMalformedClaims{sizeof(PJ_parser_route_claims_v1_t), malformedClassifyRoutes};
PJ_message_parser_vtable_t makeMalformedParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"zzz-malformed","name":"Malformed Parser","version":"1.0.0","encoding":["json"]})");
  vt.create = malformedCreate;
  vt.destroy = malformedDestroy;
  vt.get_plugin_extension = routeClaimsExtension<&kMalformedClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kMalformedParser = makeMalformedParser();

int g_classification_creates = 0;
void* classificationCountingCreate() noexcept {
  ++g_classification_creates;
  return new int(0);
}
void classificationCountingDestroy(void* ctx) noexcept {
  delete static_cast<int*>(ctx);
}
bool classificationCountingRoutes(
    void*, PJ_string_view_t, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  *out = exactClaim(true, PJ_PARSER_ROUTE_FLAG_OBJECT_V1);
  return true;
}
const PJ_parser_route_claims_v1_t kClassificationCountingClaims{
    sizeof(PJ_parser_route_claims_v1_t), classificationCountingRoutes};
PJ_message_parser_vtable_t makeClassificationCountingParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"iii-classify-count","name":"Classification Counting Parser","version":"1.0.0","encoding":["json"]})");
  vt.create = classificationCountingCreate;
  vt.destroy = classificationCountingDestroy;
  vt.get_plugin_extension = routeClaimsExtension<&kClassificationCountingClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kClassificationCountingParser = makeClassificationCountingParser();

constexpr std::string_view kProviderXType = "test/ObjectA";
constexpr std::string_view kProviderYType = "test/ObjectB";
bool providerXClassifyRoutes(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  *out = exactClaim(std::string_view(type_name.data, type_name.size) == kProviderXType, PJ_PARSER_ROUTE_FLAG_OBJECT_V1);
  return true;
}
bool providerYClassifyRoutes(
    void*, PJ_string_view_t type_name, PJ_bytes_view_t, PJ_route_classification_v1_t* out, PJ_error_t*) noexcept {
  *out = exactClaim(std::string_view(type_name.data, type_name.size) == kProviderYType, PJ_PARSER_ROUTE_FLAG_OBJECT_V1);
  return true;
}
const PJ_parser_route_claims_v1_t kProviderXClaims{sizeof(PJ_parser_route_claims_v1_t), providerXClassifyRoutes};
const PJ_parser_route_claims_v1_t kProviderYClaims{sizeof(PJ_parser_route_claims_v1_t), providerYClassifyRoutes};
PJ_message_parser_vtable_t makeProviderXParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"provider-x","name":"Provider X","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kProviderXClaims>;
  return vt;
}
PJ_message_parser_vtable_t makeProviderYParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"provider-y","name":"Provider Y","version":"1.0.0","encoding":["json"]})");
  vt.get_plugin_extension = routeClaimsExtension<&kProviderYClaims>;
  return vt;
}
const PJ_message_parser_vtable_t kProviderXParser = makeProviderXParser();
const PJ_message_parser_vtable_t kProviderYParser = makeProviderYParser();

// Wildcard parser whose instances carry a REAL distinct context, so tests can
// observe that repeated resolutions never share one parser instance.
void* countingCreate() noexcept {
  return new int(0);
}
void countingDestroy(void* ctx) noexcept {
  delete static_cast<int*>(ctx);
}
PJ_message_parser_vtable_t makeCountingParser() noexcept {
  auto vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"ddd-counting","name":"Counting Parser","version":"1.0.0","encoding":["json"]})");
  vt.create = countingCreate;
  vt.destroy = countingDestroy;
  return vt;
}
const PJ_message_parser_vtable_t kCountingParser = makeCountingParser();

StaticPluginSet staticParsers(std::initializer_list<const PJ_message_parser_vtable_t*> parsers) {
  StaticPluginSet static_plugins;
  for (const auto* parser : parsers) {
    static_plugins.message_parsers.emplace_back(parser);
  }
  return static_plugins;
}

TEST(ExtensionCatalogParserRoutes, WildcardClaimServesEveryTypeOnRegisteredEncoding) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kWildcardParser}));

  const auto selection = catalog.service.resolveParserRoutes("json", "any/Type", schemaSpan(), {});
  ASSERT_TRUE(selection.route_dispatch);
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "aaa-wildcard");
  EXPECT_EQ(selection.scalar->claim_id, "wildcard:json");
  EXPECT_TRUE(selection.scalar->parser.valid());
  EXPECT_FALSE(selection.object.has_value());
}

TEST(ExtensionCatalogParserRoutes, UnregisteredEncodingStaysOnLegacyPath) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kCustomEncodingParser}));

  const auto selection = catalog.service.resolveParserRoutes("custom_enc", "any/Type", schemaSpan(), {});
  EXPECT_FALSE(selection.route_dispatch);
  // The first provider registered for the encoding serves the binding, as the
  // pre-routing host selected it.
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "ccc-custom");
  EXPECT_TRUE(selection.scalar->claim_id.empty());
  EXPECT_TRUE(selection.scalar->parser.valid());
  EXPECT_FALSE(selection.object.has_value());
  EXPECT_FALSE(catalog.service.classifyParserObjectRoute("custom_enc", "any/Type", schemaSpan()).has_value());
}

TEST(ExtensionCatalogParserRoutes, EmptyTypeNameStaysOnLegacyPath) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kWildcardParser}));

  const auto selection = catalog.service.resolveParserRoutes("json", "", schemaSpan(), {});
  EXPECT_FALSE(selection.route_dispatch);
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "aaa-wildcard");
}

TEST(ExtensionCatalogParserRoutes, DiscoveredExactClaimOutranksWildcard) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kWildcardParser, &kBothRoutesParser}));

  const auto exact = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(exact.route_dispatch);
  ASSERT_TRUE(exact.scalar.has_value());
  EXPECT_EQ(exact.scalar->provider_id, "mmm-exact");
  EXPECT_EQ(exact.scalar->claim_id, std::string("handler:json:") + std::string(kExactType));
  ASSERT_TRUE(exact.object.has_value());
  EXPECT_EQ(exact.object->provider_id, "mmm-exact");
  EXPECT_EQ(exact.object->object_type, sdk::BuiltinObjectType::kPointCloud);

  // A type the extension declines keeps the wildcard scalar route and gains no
  // object route.
  const auto declined = catalog.service.resolveParserRoutes("json", "other/Type", schemaSpan(), {});
  ASSERT_TRUE(declined.route_dispatch);
  ASSERT_TRUE(declined.scalar.has_value());
  EXPECT_TRUE(declined.scalar->claim_id.starts_with("wildcard:"));
  EXPECT_FALSE(declined.object.has_value());
}

TEST(ExtensionCatalogParserRoutes, LegacyClassifySchemaDiscoversObjectRoute) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kLegacyObjectParser}));

  const auto selection = catalog.service.resolveParserRoutes("json", kLegacyType, schemaSpan(), {});
  ASSERT_TRUE(selection.route_dispatch);
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->claim_id, "wildcard:json");
  ASSERT_TRUE(selection.object.has_value());
  EXPECT_EQ(selection.object->provider_id, "bbb-legacy");
  EXPECT_EQ(selection.object->object_type, sdk::BuiltinObjectType::kPointCloud);

  const auto other = catalog.service.resolveParserRoutes("json", "other/Type", schemaSpan(), {});
  ASSERT_TRUE(other.route_dispatch);
  EXPECT_FALSE(other.object.has_value());
}

TEST(ExtensionCatalogParserRoutes, DiscoveredClaimsUnionConfigDependentRouteFlags) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kUnionConfigParser}));

  const auto plain = catalog.service.resolveParserRoutes("json", "std/String", schemaSpan(), {});
  ASSERT_TRUE(plain.scalar.has_value());
  EXPECT_EQ(plain.scalar->claim_id, "handler:json:std/String");
  EXPECT_FALSE(plain.object.has_value());

  const auto gated =
      catalog.service.resolveParserRoutes("json", "std/String", schemaSpan(), R"({"topic_name":"/robot_description"})");
  ASSERT_TRUE(gated.object.has_value());
  EXPECT_EQ(gated.object->provider_id, "ggg-union-config");
  EXPECT_EQ(gated.object->object_type, sdk::BuiltinObjectType::kPointCloud);

  const auto plain_again = catalog.service.resolveParserRoutes("json", "std/String", schemaSpan(), {});
  ASSERT_TRUE(plain_again.scalar.has_value());
  EXPECT_EQ(plain_again.scalar->claim_id, "handler:json:std/String");
  EXPECT_FALSE(plain_again.object.has_value());
}

TEST(ExtensionCatalogParserRoutes, DiscoveredClaimsUnionSchemaDependentRouteFlags) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kUnionSchemaParser}));
  const std::vector<uint8_t> other_schema = {'o', 't', 'h', 'e', 'r'};

  const auto plain =
      catalog.service.resolveParserRoutes("json", kExactType, {other_schema.data(), other_schema.size()}, {});
  ASSERT_TRUE(plain.scalar.has_value());
  EXPECT_EQ(plain.scalar->claim_id, std::string("handler:json:") + std::string(kExactType));
  EXPECT_FALSE(plain.object.has_value());

  const auto gated = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(gated.object.has_value());
  EXPECT_EQ(gated.object->provider_id, "hhh-union-schema");
  EXPECT_EQ(gated.object->object_type, sdk::BuiltinObjectType::kPointCloud);

  const auto plain_again =
      catalog.service.resolveParserRoutes("json", kExactType, {other_schema.data(), other_schema.size()}, {});
  ASSERT_TRUE(plain_again.scalar.has_value());
  EXPECT_EQ(plain_again.scalar->claim_id, std::string("handler:json:") + std::string(kExactType));
  EXPECT_FALSE(plain_again.object.has_value());
}

TEST(ExtensionCatalogParserRoutes, AdvertiseClassificationCachesRecordsWithoutFreshWinners) {
  g_classification_creates = 0;
  PJ::test::HermeticCatalog catalog(staticParsers({&kClassificationCountingParser}));

  const auto first = catalog.service.classifyParserObjectRoute("json", kExactType, schemaSpan());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, sdk::BuiltinObjectType::kPointCloud);
  EXPECT_EQ(g_classification_creates, 1);

  const auto cached = catalog.service.classifyParserObjectRoute("json", kExactType, schemaSpan());
  ASSERT_TRUE(cached.has_value());
  EXPECT_EQ(*cached, sdk::BuiltinObjectType::kPointCloud);
  EXPECT_EQ(g_classification_creates, 1);
}

TEST(ExtensionCatalogParserRoutes, MalformedClassificationSkipsProviderWithProbeError) {
  std::vector<Diagnostic> diagnostics;
  PJ::test::HermeticCatalog catalog(
      staticParsers({&kWildcardParser, &kMalformedParser}),
      [&diagnostics](const Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto selection = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "aaa-wildcard");
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.level == DiagnosticLevel::kError && diagnostic.id == "zzz-malformed" &&
           diagnostic.message.find("object_type NONE") != std::string::npos;
  }));
}

TEST(ExtensionCatalogParserRoutes, LegacyClassifyFailureIsRetriedNotMemoized) {
  g_legacy_classify_failing = true;
  PJ::test::HermeticCatalog catalog(staticParsers({&kFlakyLegacyParser}));

  const auto failing = catalog.service.resolveParserRoutes("json", kLegacyType, schemaSpan(), {});
  ASSERT_TRUE(failing.route_dispatch);
  ASSERT_TRUE(failing.scalar.has_value());
  EXPECT_FALSE(failing.object.has_value());

  // The provider recovers: the same (type, schema) must rediscover its object
  // route without a catalog rebuild.
  g_legacy_classify_failing = false;
  const auto recovered = catalog.service.resolveParserRoutes("json", kLegacyType, schemaSpan(), {});
  ASSERT_TRUE(recovered.object.has_value());
  EXPECT_EQ(recovered.object->provider_id, "ddd-flaky");
  EXPECT_EQ(recovered.object->object_type, sdk::BuiltinObjectType::kPointCloud);
}

TEST(ExtensionCatalogParserRoutes, UnknownObjectTypeTagIsRejectedAfterValidDiscovery) {
  g_unknown_tag_calls = 0;
  std::vector<Diagnostic> diagnostics;
  PJ::test::HermeticCatalog catalog(
      staticParsers({&kWildcardParser, &kUnknownTagParser}),
      [&diagnostics](const Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto selection = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "aaa-wildcard");
  EXPECT_FALSE(selection.object.has_value());
  EXPECT_GT(g_unknown_tag_calls, 1);
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.id == "zzz-unknown-tag" &&
           diagnostic.message.find("unknown object_type tag") != std::string::npos;
  }));
}

TEST(ExtensionCatalogParserRoutes, LegacyUnknownObjectTypeTagIsRejected) {
  std::vector<Diagnostic> diagnostics;
  PJ::test::HermeticCatalog catalog(
      staticParsers({&kLegacyUnknownTagParser}),
      [&diagnostics](const Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto selection = catalog.service.resolveParserRoutes("json", kLegacyType, schemaSpan(), {});
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_FALSE(selection.object.has_value());
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.id == "zzz-legacy-unknown" &&
           diagnostic.message.find("unknown object_type tag") != std::string::npos;
  }));
}

TEST(ExtensionCatalogParserRoutes, ClassifyFailureAfterConfigRejectsPreparedInstance) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kConfigBreakingParser}));

  const auto healthy = catalog.service.resolveParserRoutes("json", "any/Type", schemaSpan(), R"({"x":1})");
  ASSERT_TRUE(healthy.route_dispatch);
  EXPECT_TRUE(healthy.scalar.has_value());

  const auto broken =
      catalog.service.resolveParserRoutes("json", "any/Type", schemaSpan(), R"({"break_classify":true})");
  ASSERT_TRUE(broken.route_dispatch);
  EXPECT_FALSE(broken.scalar.has_value());
  EXPECT_NE(broken.scalar_failure.find("classify_schema after load_config"), std::string::npos)
      << broken.scalar_failure;
}

TEST(ExtensionCatalogParserRoutes, LegacyPathSelectsFirstProviderForEncoding) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kCustomEncodingParser, &kCustomEncodingParser2}));

  const auto selection = catalog.service.resolveParserRoutes("custom_enc", "any/Type", schemaSpan(), R"({"k":1})");
  ASSERT_TRUE(selection.scalar.has_value());
  EXPECT_EQ(selection.scalar->provider_id, "ccc-custom");
}

TEST(ExtensionCatalogParserRoutes, LegacyObjectDecoderFailureFailsBinding) {
  g_flaky_decoder_creates = 0;
  PJ::test::HermeticCatalog catalog(staticParsers({&kFlakyDecoderParser}));

  const auto selection = catalog.service.resolveParserRoutes("custom_enc", "any/Type", schemaSpan(), {});
  EXPECT_FALSE(selection.scalar.has_value());
  EXPECT_FALSE(selection.object.has_value());
  EXPECT_NE(selection.scalar_failure.find("object decoder"), std::string::npos) << selection.scalar_failure;
}

TEST(ExtensionCatalogParserRoutes, LegacyClassifyFailureStillBindsScalars) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kLegacyFailingClassifyParser}));

  const auto with_config = catalog.service.resolveParserRoutes("custom_enc", "any/Type", schemaSpan(), "{}");
  ASSERT_TRUE(with_config.scalar.has_value());
  EXPECT_FALSE(with_config.object.has_value());
  EXPECT_FALSE(catalog.service.classifyParserObjectRoute("custom_enc", "any/Type", schemaSpan()).has_value());
}

TEST(ExtensionCatalogParserRoutes, RepeatedResolutionReturnsFreshInstances) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kCountingParser}));

  auto first = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  auto second = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(first.scalar.has_value());
  ASSERT_TRUE(second.scalar.has_value());
  // Two topics of the same type must never share one stateful parser instance:
  // the prepared-instance pool is scoped to ONE resolution, so a second
  // resolution gets its own winner even when the resolver answers from cache.
  EXPECT_NE(first.scalar->parser.context(), nullptr);
  EXPECT_NE(second.scalar->parser.context(), nullptr);
  EXPECT_NE(first.scalar->parser.context(), second.scalar->parser.context());
}

// ----- Runtime-host binding through route dispatch -----

class ParserRouteBindingTest : public ::testing::Test {
 protected:
  void startWithCatalog(PJ::test::HermeticCatalog& catalog) {
    auto dataset_or = engine_.createDataset(DatasetDescriptor{.source_name = "route_test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = static_cast<DatasetId>(*dataset_or);

    host_ = std::make_unique<DataSourceRuntimeHost>(
        engine_, catalog.service, dataset_id_, PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)},
        object_store_, "route_test_source",
        [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
          registrar_calls_.emplace_back(id, std::move(parser));
        },
        /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr, /*library_keepalive=*/nullptr);
    ASSERT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  void bindType(std::string type_name) {
    auto binding_or = runtime().ensureParserBinding(
        ParserBindingRequest{
            .topic_name = "/route_topic",
            .parser_encoding = "json",
            .type_name = std::move(type_name),
            .schema = schemaSpan(),
            .parser_config_json = "{}",
        });
    ASSERT_TRUE(binding_or.has_value()) << binding_or.error();
    binding_ = *binding_or;
  }

  void bindWithCatalog(PJ::test::HermeticCatalog& catalog, std::string type_name) {
    startWithCatalog(catalog);
    bindType(std::move(type_name));
  }

  [[nodiscard]] DataSourceRuntimeHostView runtime() {
    sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value());
    return runtime_or.has_value() ? *runtime_or : DataSourceRuntimeHostView{};
  }

  [[nodiscard]] Status pushPayload(Timestamp timestamp, std::vector<uint8_t> payload) {
    return runtime().pushMessage(
        binding_, timestamp, [payload = std::move(payload)]() -> std::vector<uint8_t> { return payload; });
  }

  DataEngine engine_;
  ObjectStore object_store_;
  ServiceRegistryBuilder registry_builder_;
  DatasetId dataset_id_ = 0;
  std::unique_ptr<DataSourceRuntimeHost> host_;
  ParserBindingHandle binding_{};
  std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> registrar_calls_;
};

TEST_F(ParserRouteBindingTest, SameProviderWinsBothRoutes) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kBothRoutesParser}));
  bindWithCatalog(catalog, std::string(kExactType));

  ASSERT_EQ(registrar_calls_.size(), 1U);
  EXPECT_TRUE(registrar_calls_[0].second->valid());
  EXPECT_TRUE(object_store_.findTopic(dataset_id_, "/route_topic").has_value());
}

TEST_F(ParserRouteBindingTest, ObjectDecoderReplacementFailsClosed) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kProviderXParser, &kProviderYParser}));
  startWithCatalog(catalog);

  bindType(std::string(kProviderXType));
  ASSERT_EQ(registrar_calls_.size(), 1U);
  EXPECT_EQ(registrar_calls_[0].second->vtable(), &kProviderXParser);

  // Retyping the same topic selects provider Y for the object route. The
  // scalar binding still succeeds, but the original decoder remains the only
  // registrar entry for this ObjectTopicId.
  bindType(std::string(kProviderYType));
  ASSERT_EQ(registrar_calls_.size(), 1U);
  EXPECT_EQ(registrar_calls_[0].second->vtable(), &kProviderXParser);
}

TEST_F(ParserRouteBindingTest, DivergentWinnersBindScalarAndObjectIndependently) {
  // aaa-wildcard wins the scalar route (wildcard tie-break by provider id);
  // zzz-object holds the only object claim for the type.
  PJ::test::HermeticCatalog catalog(staticParsers({&kWildcardParser, &kObjectOnlyParser}));

  const auto selection = catalog.service.resolveParserRoutes("json", kExactType, schemaSpan(), {});
  ASSERT_TRUE(selection.scalar.has_value());
  ASSERT_TRUE(selection.object.has_value());
  ASSERT_EQ(selection.scalar->provider_id, "aaa-wildcard");
  ASSERT_EQ(selection.object->provider_id, "zzz-object");

  bindWithCatalog(catalog, std::string(kExactType));

  // The object topic is registered with the object winner's type and the lazy
  // re-decode instance is an instance OF the object winner — observable by its
  // vtable identity.
  ASSERT_EQ(registrar_calls_.size(), 1U);
  EXPECT_TRUE(registrar_calls_[0].second->valid());
  EXPECT_EQ(registrar_calls_[0].second->vtable(), &kObjectOnlyParser);
  EXPECT_TRUE(object_store_.findTopic(dataset_id_, "/route_topic").has_value());

  // Live pushes flow through the divergent binding: the scalar winner parses
  // and the object entry is enqueued lazily for the registrar instance.
  const auto pushed = pushPayload(1'000, {0x01, 0x02, 0x03});
  EXPECT_TRUE(pushed.has_value()) << pushed.error();
}

TEST_F(ParserRouteBindingTest, ScalarOnlyTypeBindsWithoutObjectTopic) {
  PJ::test::HermeticCatalog catalog(staticParsers({&kWildcardParser}));
  bindWithCatalog(catalog, "plain/Type");

  EXPECT_TRUE(registrar_calls_.empty());
  EXPECT_FALSE(object_store_.findTopic(dataset_id_, "/route_topic").has_value());
}

}  // namespace
