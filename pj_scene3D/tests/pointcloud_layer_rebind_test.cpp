// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression test for the file-reload crash on the pointcloud layer (M.65).
// SessionManager::replaceDataset() re-registers each surviving topic's parser
// slot, destroying the previous MessageParserHandle. A layer that cached the
// raw parser pointer at attach() then calls parseObject() on freed memory on
// the next tracker tick. PointCloudLayer::renderAt()/bootstrap() must instead
// resolve the parser binding through the session on every use, so a reload
// transparently rebinds them to the new parser. The raw (uncompressed) path
// still calls parseLocked on the UI thread per tick, so a reintroduced cached
// pointer here is the same use-after-free — this test pins the per-use binding.
//
// The sample is kept RAW (not compressed) so no async worker is involved and
// the assertion is fully synchronous.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kSchema = "mock/point_cloud";

std::atomic<int> g_first_parser_calls{0};
std::atomic<int> g_second_parser_calls{0};

// Emits a fixed single-point RAW cloud. The xyz floats live in a static buffer
// (no anchor needed), so the decode is synchronous and reaches the GPU push
// without a worker. Bound through CountingObjectParser, which handles the per-
// parse counter bump.
PJ::Expected<PJ::sdk::ObjectRecord> emitCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const float k_point[3] = {1.0f, 2.0f, 3.0f};
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 12;
  cloud.row_step = 12;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"y", 4, PJ::sdk::PointField::Datatype::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, PJ::sdk::PointField::Datatype::kFloat32, 1}};
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(k_point), sizeof(k_point));
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

TEST(PointCloudLayerRebind, ReloadSwapsParserWithoutTouchingStaleOne) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/cloud";
  const auto topic_id = store.registerTopic(desc);
  ASSERT_TRUE(topic_id.has_value());
  ASSERT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());

  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_first_parser_calls, &emitCloud);
  }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(*topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  // attach() decodes the bootstrap sample (field discovery) AND renders the first
  // sample, so the first parser ran at least once.
  EXPECT_GE(g_first_parser_calls.load(), 1) << "attach did not decode through the registered parser";
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100}) << "attach did not render the first sample";

  // Keep the first parser's memory alive from the test, so the buggy stale-pointer
  // call below would show up as a deterministic wrong-parser call instead of
  // undefined behavior. Production holds no such guard — there the same call is a
  // use-after-free crash.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(*topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: replaceDataset() re-registers the surviving
  // topic's parser under its stable id, overwriting the slot and dropping the old
  // handle.
  session.registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(
        kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_second_parser_calls, &emitCloud);
  }));

  // Force a re-decode at the same time: a swap re-registers the parser slot but
  // does NOT change the store bytes here, so the already-pushed sample identity
  // would make renderAt early-skip. Move the tracker to a fresh sample first so
  // the skip guard can't mask the rebind.
  ASSERT_TRUE(store.pushOwned(*topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  const int stale_calls_before = g_first_parser_calls.load();
  layer.setTrackerTime(PJ::fromRaw(200));
  layer.renderAtForTest(200);

  EXPECT_EQ(g_first_parser_calls.load(), stale_calls_before)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{200})
      << "layer did not render the new sample through the rebound parser";
}

}  // namespace
