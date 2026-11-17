// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A parser resolves both the scalar and the object write host from the service registry and writes to
// each from a single parse() call.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace PJ {
namespace {

using sdk::ObjectBytes;
using sdk::ObjectTopicHandle;
using sdk::ParserObjectWriteHostService;
using sdk::ParserObjectWriteHostView;
using sdk::ParserWriteHostService;

/// A mock parser that expects both hosts. parse() peels a trivial
/// "seq:<u64>;bytes:<raw>" envelope and writes seq to the scalar host
/// and the raw bytes to the object host.
class MediaParser : public MessageParserPluginBase {
 public:
  Status parse(Timestamp timestamp_ns, Span<const uint8_t> payload) override {
    // Envelope: first 8 bytes little-endian seq; rest = bytes.
    if (payload.size() < sizeof(uint64_t)) {
      return unexpected("payload too small");
    }
    uint64_t seq = 0;
    std::memcpy(&seq, payload.data(), sizeof(uint64_t));
    Span<const uint8_t> body(payload.data() + sizeof(uint64_t), payload.size() - sizeof(uint64_t));

    const std::vector<sdk::NamedFieldValue> fields = {{.name = "seq", .value = static_cast<uint64_t>(seq)}};
    if (auto s = writeHost().appendRecord(timestamp_ns, fields); !s) {
      return s;
    }

    // Object side: only if the host registered it.
    if (auto* obj = objectWriteHost()) {
      if (auto s = obj->pushOwned(timestamp_ns, body); !s) {
        return s;
      }
    }
    return okStatus();
  }
};

// Minimal PJ_service_registry_vtable_t for tests: a map of service name -> PJ_service_t.
struct MockRegistryState {
  std::unordered_map<std::string, PJ_service_t> services;
};

bool mockGetService(
    void* ctx, PJ_string_view_t name, uint32_t /*min_version*/, PJ_service_t* out_service,
    PJ_error_t* out_error) noexcept {
  auto* state = static_cast<MockRegistryState*>(ctx);
  try {
    std::string key(name.data, name.size);
    auto it = state->services.find(key);
    if (it == state->services.end()) {
      if (out_error != nullptr) {
        sdk::fillError(out_error, 1, "registry", "service not found");
      }
      return false;
    }
    *out_service = it->second;
    return true;
  } catch (...) {
    if (out_error != nullptr) {
      sdk::fillError(out_error, 1, "registry", "exception in lookup");
    }
    return false;
  }
}

TEST(ParserObjectWriteHostTest, ParserWritesToBothHostsFromOneParse) {
  DataEngine engine;
  auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
  PJ_data_source_handle_t source_handle{static_cast<uint32_t>(*dataset_or)};

  DatastoreSourceWriteHost scalar_impl(engine, source_handle);
  auto scalar_view = sdk::SourceWriteHostView{scalar_impl.raw()};
  const auto topic = *scalar_view.ensureTopic("media_topic");
  DatastoreParserWriteHost parser_write_impl(engine, topic);

  ObjectStore store;
  DatastoreSourceObjectWriteHost obj_source(store, *dataset_or);
  const auto obj_topic =
      *sdk::SourceObjectWriteHostView{obj_source.raw()}.registerTopic("media_topic", R"({"media_class":"image"})");
  DatastoreParserObjectWriteHost parser_obj_impl(store, obj_topic.id);

  MockRegistryState registry_state;
  const auto scalar_raw = parser_write_impl.raw();
  const auto obj_raw = parser_obj_impl.raw();
  registry_state.services[ParserWriteHostService::kName] = PJ_service_t{scalar_raw.ctx, scalar_raw.vtable};
  registry_state.services[ParserObjectWriteHostService::kName] = PJ_service_t{obj_raw.ctx, obj_raw.vtable};

  static const PJ_service_registry_vtable_t registry_vtable = {
      PJ_PLUGIN_DATA_API_VERSION,
      sizeof(PJ_service_registry_vtable_t),
      mockGetService,
  };
  const PJ_service_registry_t registry_raw{&registry_state, &registry_vtable};

  MediaParser parser;
  ASSERT_TRUE(parser.bind(sdk::ServiceRegistry{registry_raw}).has_value());

  // parse() one message: seq=7, payload=[0xAA 0xBB 0xCC].
  std::vector<uint8_t> payload(sizeof(uint64_t) + 3);
  uint64_t seq = 7;
  std::memcpy(payload.data(), &seq, sizeof(uint64_t));
  payload[sizeof(uint64_t) + 0] = 0xAA;
  payload[sizeof(uint64_t) + 1] = 0xBB;
  payload[sizeof(uint64_t) + 2] = 0xCC;

  ASSERT_TRUE(parser.parse(100, Span<const uint8_t>(payload.data(), payload.size())).has_value());

  auto resolved = store.latestAt(ObjectTopicId{obj_topic.id}, 100);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_NE(resolved->payload.anchor, nullptr);
  const std::vector<uint8_t> expected{0xAA, 0xBB, 0xCC};
  EXPECT_TRUE(
      std::equal(resolved->payload.bytes.begin(), resolved->payload.bytes.end(), expected.begin(), expected.end()));

  // The scalar side is not read back here: the test proves both hosts were resolved and invoked.
}

TEST(ParserObjectWriteHostTest, ParserFallsBackToScalarOnlyWhenObjectServiceAbsent) {
  DataEngine engine;
  auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
  PJ_data_source_handle_t source_handle{static_cast<uint32_t>(*dataset_or)};

  DatastoreSourceWriteHost scalar_impl(engine, source_handle);
  auto scalar_view = sdk::SourceWriteHostView{scalar_impl.raw()};
  const auto topic = *scalar_view.ensureTopic("scalar_only");
  DatastoreParserWriteHost parser_write_impl(engine, topic);

  MockRegistryState registry_state;
  const auto scalar_raw = parser_write_impl.raw();
  registry_state.services[ParserWriteHostService::kName] = PJ_service_t{scalar_raw.ctx, scalar_raw.vtable};

  static const PJ_service_registry_vtable_t registry_vtable = {
      PJ_PLUGIN_DATA_API_VERSION,
      sizeof(PJ_service_registry_vtable_t),
      mockGetService,
  };
  const PJ_service_registry_t registry_raw{&registry_state, &registry_vtable};

  MediaParser parser;
  ASSERT_TRUE(parser.bind(sdk::ServiceRegistry{registry_raw}).has_value());

  // The parser's object host view is empty: parse() takes the non-media branch and still succeeds.
  std::vector<uint8_t> payload(sizeof(uint64_t));
  uint64_t seq = 1;
  std::memcpy(payload.data(), &seq, sizeof(uint64_t));
  ASSERT_TRUE(parser.parse(1, Span<const uint8_t>(payload.data(), payload.size())).has_value());
}

TEST(ParserObjectWriteHostTest, ObjectHostViewPushLazyThroughSdk) {
  // Exercises the SDK pushLazy(Fetch&&) path for parsers — proves the
  // heap-allocated LazyBox box is wired through the parser vtable.
  ObjectStore store;
  DatastoreSourceObjectWriteHost src(store, DatasetId{1});
  const auto topic = *sdk::SourceObjectWriteHostView{src.raw()}.registerTopic("lazy", "{}");

  DatastoreParserObjectWriteHost parser_obj(store, topic.id);
  ParserObjectWriteHostView view{parser_obj.raw()};

  int fetch_calls = 0;
  auto fetch = [&fetch_calls]() -> std::vector<uint8_t> {
    ++fetch_calls;
    return {0xAA, 0xBB};
  };
  ASSERT_TRUE(view.pushLazy(10, fetch).has_value());

  auto resolved = store.latestAt(ObjectTopicId{topic.id}, 10);
  ASSERT_TRUE(resolved.has_value());
  const std::vector<uint8_t> expected{0xAA, 0xBB};
  EXPECT_TRUE(
      std::equal(resolved->payload.bytes.begin(), resolved->payload.bytes.end(), expected.begin(), expected.end()));
  EXPECT_GE(fetch_calls, 1);
}

TEST(ParserObjectWriteHostTest, UnboundViewReturnsError) {
  ParserObjectWriteHostView empty;
  EXPECT_FALSE(empty.valid());
  auto status = empty.pushOwned(0, {});
  EXPECT_FALSE(status.has_value());
}

}  // namespace
}  // namespace PJ
