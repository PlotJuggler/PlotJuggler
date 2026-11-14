// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

PJ::sdk::SchemaHandler makeByteCountHandler(PJ::sdk::BuiltinObjectType object_type) {
  return PJ::sdk::SchemaHandler{
      .object_type = object_type,
      .parse_scalars = [](PJ::Timestamp /*timestamp_ns*/,
                          PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        return PJ::sdk::ScalarRecord{
            .ts = std::nullopt,
            .fields = {{.name = "byte_count", .value = static_cast<uint64_t>(payload.size())}},
        };
      },
      .parse_object = {},
  };
}

// Mimics a parser honoring an embedded header stamp (ROS "use embedded
// timestamp"): the record's ts override is the payload's first 8 bytes
// (native int64). A multi-publisher topic can regress this stamp even though
// host receive times stay monotonic.
PJ::sdk::SchemaHandler makeEmbeddedTimestampHandler(PJ::sdk::BuiltinObjectType object_type) {
  return PJ::sdk::SchemaHandler{
      .object_type = object_type,
      .parse_scalars = [](PJ::Timestamp /*timestamp_ns*/,
                          PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        if (payload.size() < sizeof(int64_t)) {
          return PJ::unexpected(std::string("embedded_ts payload too short"));
        }
        int64_t embedded_ts = 0;
        std::memcpy(&embedded_ts, payload.data(), sizeof(embedded_ts));
        return PJ::sdk::ScalarRecord{
            .ts = embedded_ts,
            .fields = {{.name = "byte_count", .value = static_cast<uint64_t>(payload.size())}},
        };
      },
      .parse_object = {},
  };
}

// The object route these two handlers register is never invoked by the ingest
// tests: the host stores the raw payload and decodes on pull.
PJ::Expected<PJ::sdk::ObjectRecord> undecodedObject(PJ::Timestamp /*timestamp_ns*/, PJ::sdk::PayloadView /*payload*/) {
  return PJ::unexpected(std::string("object decoding is not exercised here"));
}

// Object-only, like parser_ros's robot_description: an object route and no
// scalar route. parse() has nothing to write, and the SDK base reports the
// absent route as "registered handler has no parse_scalars".
PJ::sdk::SchemaHandler makeObjectOnlyHandler() {
  return PJ::sdk::SchemaHandler{
      .object_type = PJ::sdk::BuiltinObjectType::kImage,
      .parse_scalars = {},
      .parse_object = undecodedObject,
  };
}

// Both routes, with a scalar route that fails on every message: a genuine
// per-message failure, unlike the absent route above.
PJ::sdk::SchemaHandler makeFailingScalarHandler() {
  return PJ::sdk::SchemaHandler{
      .object_type = PJ::sdk::BuiltinObjectType::kImage,
      .parse_scalars = [](PJ::Timestamp /*timestamp_ns*/, PJ::Span<const uint8_t> /*payload*/)
          -> PJ::Expected<PJ::sdk::ScalarRecord> { return PJ::unexpected(std::string("scalar decode failed")); },
      .parse_object = undecodedObject,
  };
}

class RuntimeHostObjectParser : public PJ::MessageParserPluginBase {
 public:
  RuntimeHostObjectParser() {
    registerSchemaHandler("mock/image", makeByteCountHandler(PJ::sdk::BuiltinObjectType::kImage));
    registerSchemaHandler("mock/scalar", makeByteCountHandler(PJ::sdk::BuiltinObjectType::kNone));
    registerSchemaHandler("mock/embedded_ts_image", makeEmbeddedTimestampHandler(PJ::sdk::BuiltinObjectType::kImage));
    registerSchemaHandler("mock/object_only_image", makeObjectOnlyHandler());
    registerSchemaHandler("mock/failing_scalar_image", makeFailingScalarHandler());
  }

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    if (auto status = MessageParserPluginBase::bindSchema(type_name, schema); !status) {
      return status;
    }
    if (type_name == "mock/bind_schema_image") {
      registerSchemaHandler(type_name, makeByteCountHandler(PJ::sdk::BuiltinObjectType::kImage));
    }
    return PJ::okStatus();
  }
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    RuntimeHostObjectParser,
    R"({"id":"runtime-host-object-parser","name":"Runtime Host Object Parser","version":"1.0.0","encoding":["runtime_host_object"]})")
