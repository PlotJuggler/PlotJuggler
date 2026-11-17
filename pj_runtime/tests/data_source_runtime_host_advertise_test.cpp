// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// DataSourceRuntimeHost::notify_available_topics, the two load-bearing contracts of the advertise path:
//   (a) the vtable slot can be invoked from a non-GUI thread with no crash, and on_available_topics fires on that same
//       (calling) thread: the host does no marshaling itself (per data_source_protocol.h the plugin calls this on its
//       own poll/stream thread), so the callback must marshal (as on_progress_* do); the test pins "runs on the
//       caller's thread" as the contract a StreamingSourceManager-style consumer must marshal away from.
//   (b) a-priori classification: with no parser registered for the advertised encoding (an empty
//       ExtensionCatalogService), classifyAvailableTopic falls back to matching the type name against the
//       FrameTransforms/CameraInfo infra schemas, and kNone otherwise.
//
// Classification via a real MessageParser plugin's classify_schema is not covered here (cbEnsureParserBinding's tests
// exercise it through the same bindSchema-then-classify sequence).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace {

class DataSourceRuntimeHostAdvertiseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*dataset_or);
    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        engine_, catalog_, dataset_id_, source_handle_, object_store_, "advertise_test_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
        /*library_keepalive=*/nullptr);
    EXPECT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  // catalog_ has no plugins loaded (empty extensions dir), so every
  // findParserByEncoding lookup misses and classification exercises the
  // schema-name fallback exclusively.
  PJ::ExtensionCatalogService catalog_;
  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

TEST_F(DataSourceRuntimeHostAdvertiseTest, FiresCallbackOnCallingThreadWithFallbackClassification) {
  std::atomic<bool> callback_ran{false};
  std::thread::id observed_thread{};
  std::vector<PJ::DataSourceRuntimeHost::AdvertisedTopicInfo> observed;

  host_->on_available_topics = [&](std::vector<PJ::DataSourceRuntimeHost::AdvertisedTopicInfo> topics) {
    observed_thread = std::this_thread::get_id();
    observed = std::move(topics);
    callback_ran.store(true);
  };

  std::thread::id worker_thread_id{};
  std::thread worker([&]() {
    worker_thread_id = std::this_thread::get_id();
    const std::vector<PJ::AvailableTopic> topics{
        {.topic_name = "/tf",
         .parser_encoding = "no_such_encoding",
         .type_name = "foxglove.FrameTransforms",
         .schema = {}},
        {.topic_name = "/camera/info",
         .parser_encoding = "no_such_encoding",
         .type_name = "sensor_msgs/CameraInfo",
         .schema = {}},
        {.topic_name = "/scan",
         .parser_encoding = "no_such_encoding",
         .type_name = "sensor_msgs/LaserScan",
         .schema = {}},
    };
    auto status = runtime().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
    EXPECT_TRUE(status.has_value()) << status.error();
  });
  worker.join();

  ASSERT_TRUE(callback_ran.load());
  EXPECT_EQ(observed_thread, worker_thread_id);
  EXPECT_NE(observed_thread, std::this_thread::get_id());

  ASSERT_EQ(observed.size(), 3u);
  EXPECT_EQ(observed[0].topic_name, "/tf");
  EXPECT_EQ(observed[0].classification, PJ::sdk::BuiltinObjectType::kFrameTransforms);
  EXPECT_EQ(observed[1].topic_name, "/camera/info");
  EXPECT_EQ(observed[1].classification, PJ::sdk::BuiltinObjectType::kCameraInfo);
  EXPECT_EQ(observed[2].topic_name, "/scan");
  EXPECT_EQ(observed[2].classification, PJ::sdk::BuiltinObjectType::kNone);
}

// The infra fallback matches the type-name LEAF segment exactly, not a substring:
// a type whose leaf merely CONTAINS "CameraInfo"/"FrameTransforms" must NOT be
// misclassified as infrastructure (which would wrongly pin it always-subscribed).
TEST_F(DataSourceRuntimeHostAdvertiseTest, InfraFallbackDoesNotSubstringMatch) {
  std::vector<PJ::DataSourceRuntimeHost::AdvertisedTopicInfo> observed;
  host_->on_available_topics = [&](std::vector<PJ::DataSourceRuntimeHost::AdvertisedTopicInfo> topics) {
    observed = std::move(topics);
  };

  const std::vector<PJ::AvailableTopic> topics{
      {.topic_name = "/cam_status",
       .parser_encoding = "no_such_encoding",
       .type_name = "my_msgs/CameraInfoStatus",
       .schema = {}},
      {.topic_name = "/tf_meta",
       .parser_encoding = "no_such_encoding",
       .type_name = "pkg/msg/FrameTransformsExtra",
       .schema = {}},
  };
  auto status = runtime().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  EXPECT_TRUE(status.has_value()) << status.error();

  ASSERT_EQ(observed.size(), 2u);
  EXPECT_EQ(observed[0].classification, PJ::sdk::BuiltinObjectType::kNone);
  EXPECT_EQ(observed[1].classification, PJ::sdk::BuiltinObjectType::kNone);
}

TEST_F(DataSourceRuntimeHostAdvertiseTest, NoCallbackInstalledIsSafeNoOp) {
  const std::vector<PJ::AvailableTopic> topics{
      {.topic_name = "/tf",
       .parser_encoding = "no_such_encoding",
       .type_name = "foxglove.FrameTransforms",
       .schema = {}},
  };
  auto status = runtime().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  EXPECT_TRUE(status.has_value()) << status.error();
}

}  // namespace
