// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A scalar MessageParser that caches a FieldHandle on its first parse and reuses it on every later message via
// appendBoundRecord (the parser_protobuf pattern). A parser that writes scalars by name re-resolves the field through
// a fresh WriteCore after each setDataEngineTarget swap, so only a cached handle exercises a pause/resume swap.
//
// Payload: the first 4 bytes are a native-endian float32 written to field "value"; shorter payloads write 0.0.
// The bound type "streaming/cached_scalar" is a scalar topic (BuiltinObjectType::kNone), so the runtime host routes
// pushMessage straight to parse().

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

class StreamingCachingParser : public PJ::MessageParserPluginBase {
 public:
  StreamingCachingParser() {
    // Register the bound type so classifySchema()/bind() accept it; parse_scalars is never invoked (parse() is
    // overridden).
    registerSchemaHandler(
        "streaming/cached_scalar",
        PJ::sdk::SchemaHandler{
            .object_type = PJ::sdk::BuiltinObjectType::kNone,
            .parse_scalars = [](PJ::Timestamp, PJ::Span<const uint8_t>) -> PJ::Expected<PJ::sdk::ScalarRecord> {
              return PJ::sdk::ScalarRecord{};
            },
            .parse_object = {},
        });
  }

  // Resolve and cache the FieldHandle once, then write bound records; the handle must stay valid across a pause/resume
  // DataEngine swap.
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) {
      return PJ::unexpected(std::string("streaming caching parser: write host not bound"));
    }
    if (!cached_field_.has_value()) {
      auto field_or = writeHost().ensureField("value", PJ::PrimitiveType::kFloat32);
      if (!field_or.has_value()) {
        return PJ::unexpected(field_or.error());
      }
      cached_field_ = *field_or;
    }
    float value = 0.0F;
    if (payload.size() >= sizeof(float)) {
      std::memcpy(&value, payload.data(), sizeof(float));
    }
    return writeHost().appendBoundRecord(
        timestamp_ns, {PJ::sdk::BoundFieldValue{.field = *cached_field_, .value = value}});
  }

 private:
  std::optional<PJ::sdk::FieldHandle> cached_field_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    StreamingCachingParser,
    R"({"id":"streaming-caching-parser","name":"Streaming Caching Parser","version":"1.0.0","encoding":["streaming_caching"]})")
