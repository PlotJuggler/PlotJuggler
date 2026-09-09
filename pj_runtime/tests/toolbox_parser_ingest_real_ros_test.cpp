// Copyright 2026
// SPDX-License-Identifier: MPL-2.0
//
// Integration: ToolboxRuntimeHost parser ingest + the REAL parser_ros plugin.
// Pushes a handcrafted tf2_msgs/msg/TFMessage CDR payload through the toolbox
// parser-ingest path and asserts the 3D-scene contract end to end:
//   - the topic classifies kFrameTransforms (metadata_json drives drag routing,
//     CatalogModel.cpp objectTypeFromMetadata),
//   - the message lands as an ObjectStore entry (render source),
//   - the render-time parser registrar fires (SessionManager contract).
// Gated: SKIPs unless PJ_REAL_ROS_PARSER_DIR points at the directory holding
// the built parser_ros .so (pj-official-plugins/build/parser_ros/...).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
using namespace Qt::StringLiterals;

namespace {

// Minimal XCDR1 little-endian writer. Alignment is relative to the start of
// the body (after the 4-byte encapsulation header) — the rule rosx/nanocdr
// deserializers apply (nanocdr Decoder: origin_ = buffer.data() + 4).
struct CdrWriter {
  std::vector<uint8_t> buf{0x00, 0x01, 0x00, 0x00};  // {representation=CDR_LE, options=0}
  [[nodiscard]] size_t body() const {
    return buf.size() - 4;
  }
  void align(size_t n) {
    while (body() % n != 0) {
      buf.push_back(0);
    }
  }
  void u32(uint32_t v) {
    align(4);
    for (int i = 0; i < 4; ++i) {
      buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void i32(int32_t v) {
    u32(static_cast<uint32_t>(v));
  }
  void f64(double v) {
    align(8);
    uint64_t b = 0;
    std::memcpy(&b, &v, 8);
    for (int i = 0; i < 8; ++i) {
      buf.push_back(static_cast<uint8_t>(b >> (8 * i)));
    }
  }
  void str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size() + 1));
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
  }
};

// One TFMessage with one TransformStamped: map -> base_link, t=(1,2,3), q=identity.
std::vector<uint8_t> makeTfPayload() {
  CdrWriter w;
  w.u32(1);            // transforms sequence length
  w.i32(7);            // header.stamp.sec
  w.u32(500);          // header.stamp.nanosec
  w.str("map");        // header.frame_id
  w.str("base_link");  // child_frame_id
  w.f64(1.0);          // transform.translation.x
  w.f64(2.0);
  w.f64(3.0);
  w.f64(0.0);  // transform.rotation.x
  w.f64(0.0);
  w.f64(0.0);
  w.f64(1.0);  // w
  return w.buf;
}

// The concatenated ros2msg schema text exactly as a rosbag2 MCAP embeds it.
constexpr const char* kTfSchema = R"(geometry_msgs/TransformStamped[] transforms
================================================================================
MSG: geometry_msgs/TransformStamped
std_msgs/Header header
string child_frame_id
Transform transform
================================================================================
MSG: geometry_msgs/Transform
Vector3 translation
Quaternion rotation
================================================================================
MSG: geometry_msgs/Vector3
float64 x
float64 y
float64 z
================================================================================
MSG: geometry_msgs/Quaternion
float64 x 0
float64 y 0
float64 z 0
float64 w 1
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
)";

TEST(ToolboxParserIngestRealRos, TfMessageBecomesFrameTransformsObjectTopic) {
  const auto dir = PJ::sdk::getEnv("PJ_REAL_ROS_PARSER_DIR");
  if (!dir) {
    GTEST_SKIP() << "PJ_REAL_ROS_PARSER_DIR not set (directory containing the built parser_ros .so)";
  }
  PJ::test::HermeticCatalog catalog_box(QString::fromUtf8(dir->c_str()));
  PJ::ExtensionCatalogService& catalog = catalog_box.service;
  if (catalog.findParserByEncoding(u"ros2msg"_s) == nullptr) {
    GTEST_SKIP() << "no ros2msg parser found in " << *dir;
  }

  PJ::DataEngine engine;
  PJ::ObjectStore object_store;
  PJ::sdk::InMemorySettingsBackend settings;
  PJ::ServiceRegistryBuilder builder;

  std::vector<PJ::ObjectTopicId> registered;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  deps.register_object_parser = [&registered](PJ::ObjectTopicId id, std::unique_ptr<PJ::MessageParserHandle> parser) {
    EXPECT_NE(parser, nullptr);
    registered.push_back(id);
  };
  PJ::ToolboxRuntimeHost host(engine, object_store, settings, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  ASSERT_TRUE(host.registerServices(builder).has_value());
  PJ::sdk::ServiceRegistry services(builder.view());

  auto toolbox = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox.has_value());
  auto runtime = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  auto ds = (*toolbox).createDataSource("tf download");
  ASSERT_TRUE(ds.has_value()) << ds.error();
  auto ingest_or = (*runtime).createParserIngest(ds->id);
  ASSERT_TRUE(ingest_or.has_value()) << ingest_or.error();
  auto ingest = *ingest_or;

  // "{}" (non-empty) is LOAD-BEARING. DataSourceRuntimeHost skips
  // parser->loadConfig() entirely when parser_config_json is empty
  // (DataSourceRuntimeHost.cpp ~:452), and parser_ros selects its SPECIALIZED
  // handlers (the TF entry carrying object_type=kFrameTransforms) only inside
  // loadConfig -> compileBoundSchema(register_specialized_handler=true)
  // (ros_parser.cpp ~:430). bindSchema alone registers only the kDefault
  // generic handler -> classifySchema returns kNone -> no object topic.
  // Any non-empty JSON works ("{}" included); "" silently degrades to
  // scalar-only ingest. Empirically falsified: with "" this test fails at the
  // topics.size() assertion.
  const std::string_view schema{kTfSchema};
  auto binding = ingest.ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/tf",
          .parser_encoding = "ros2msg",
          .type_name = "tf2_msgs/msg/TFMessage",  // verbatim, as the wire/mcap carries it
          .schema = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema.data()), schema.size()),
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding.has_value()) << binding.error();

  const auto payload = makeTfPayload();
  auto push = ingest.pushMessage(
      *binding, PJ::Timestamp{1'000'000'000}, [payload]() -> std::vector<uint8_t> { return payload; });
  ASSERT_TRUE(push.has_value()) << push.error();
  ASSERT_TRUE((*runtime).releaseParserIngest(ds->id).has_value());

  // 3D contract: kFrameTransforms metadata (drag routing), one stored entry
  // (render source), render parser registered (SessionManager contract).
  // sdk::name(kFrameTransforms) == "kFrameTransforms" (builtin_object.hpp), so
  // metadata_json is {"builtin_object_type":"kFrameTransforms"}.
  const auto topics = object_store.listTopics(static_cast<PJ::DatasetId>(ds->id));
  ASSERT_EQ(topics.size(), 1u);
  const auto& desc = object_store.descriptor(topics.front());
  EXPECT_NE(desc.metadata_json.find("kFrameTransforms"), std::string::npos) << desc.metadata_json;
  EXPECT_EQ(object_store.entryCount(topics.front()), 1u);
  EXPECT_EQ(registered.size(), 1u);
}

}  // namespace
