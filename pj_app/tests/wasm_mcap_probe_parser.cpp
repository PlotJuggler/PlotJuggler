// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Acceptance-only parser for the MCAP container path. It recognizes just the tiny std_msgs/Float64 fixture and
// classifies it as an image so FileLoader's production pure-lazy policy exercises MCAP's cold byte-store path after
// import. Compiled only with PJ_WASM_ENABLE_INGRESS_PROBE.

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace {

class WasmMcapProbeParser : public PJ::MessageParserPluginBase {
 public:
  WasmMcapProbeParser() {
    const auto make_handler = []() {
      PJ::sdk::SchemaHandler handler;
      handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
      handler.parse_scalars = [](PJ::Timestamp, PJ::Span<const uint8_t>) -> PJ::Expected<PJ::sdk::ScalarRecord> {
        return PJ::sdk::ScalarRecord{};
      };
      handler.parse_object = [](PJ::Timestamp timestamp,
                                PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
        PJ::sdk::Image image;
        image.encoding = "wasm-mcap-probe";
        image.data = payload.bytes;
        image.anchor = payload.anchor;
        image.timestamp_ns = timestamp;
        return PJ::sdk::ObjectRecord{.ts = std::nullopt, .object = PJ::sdk::BuiltinObject{std::move(image)}};
      };
      return handler;
    };
    constexpr const char* kAcceptanceSchemas[] = {
        "std_msgs/Float64",
        "nav_msgs/msg/OccupancyGrid",
        "std_msgs/msg/String",
        "tf2_msgs/msg/TFMessage",
        "geometry_msgs/msg/PoseArray",
        "geometry_msgs/msg/PoseWithCovarianceStamped",
        "sensor_msgs/msg/LaserScan",
        "visualization_msgs/msg/MarkerArray",
    };
    for (const char* schema : kAcceptanceSchemas) {
      registerSchemaHandler(schema, make_handler());
    }
  }
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(
    WasmMcapProbeParser,
    R"({"id":"wasm-mcap-probe-parser","name":"WASM MCAP Probe Parser","version":"1.0.0","encoding":["ros2msg"]})")
