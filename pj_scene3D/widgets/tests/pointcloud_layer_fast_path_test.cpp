// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PointCloudLayer zero-copy fast-path integration tests:
//  - world_bounds_ must refresh on EVERY new sample even when no colormap consumes the scan
//    (solid / RGB / auto-off). It feeds the camera scene-fit (Scene3DDockWidget::
//    updateSceneBounds) every frame; gating the bounds scan on colour mode silently froze it
//    (a regression caught in review). This test pushes two differently-placed clouds in solid
//    mode and asserts worldBounds() tracks the second, not the first.
//  - an RGB-direct (kRgb) cloud carrying a packed rgba uint32 field must take the zero-copy
//    fast path (verbatim colour upload), not the CPU fallback.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/span.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using DT = PJ::sdk::PointField::Datatype;

constexpr std::string_view kSchema = "mock/point_cloud";

std::atomic<int> g_two_extent_calls{0};
std::atomic<int> g_rgba_calls{0};

// Two single-point float32-xyz clouds with different positions -> different AABBs. The sample at
// stamp >= 200 is the "far" cloud; everything earlier is the "near" cloud.
PJ::Expected<PJ::sdk::ObjectRecord> emitTwoExtentCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const float k_near[3] = {1.0f, 1.0f, 1.0f};
  static const float k_far[3] = {5.0f, 6.0f, 7.0f};
  const float* pts = (ts >= 200) ? k_far : k_near;
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 12;
  cloud.row_step = 12;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, DT::kFloat32, 1}, PJ::sdk::PointField{"y", 4, DT::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, DT::kFloat32, 1}};
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(pts), sizeof(k_near));
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

// One point: xyz float32 + a packed rgba uint32 (R=0x10,G=0x20,B=0x30,A=0xff) at offset 12.
PJ::Expected<PJ::sdk::ObjectRecord> emitRgbaCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const uint8_t k_pt[16] = {0x00, 0x00, 0x80, 0x3f,   // x = 1.0f
                                   0x00, 0x00, 0x00, 0x40,   // y = 2.0f
                                   0x00, 0x00, 0x40, 0x40,   // z = 3.0f
                                   0x10, 0x20, 0x30, 0xff};  // rgba (R,G,B,A in increasing address)
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.row_step = 16;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, DT::kFloat32, 1}, PJ::sdk::PointField{"y", 4, DT::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, DT::kFloat32, 1}, PJ::sdk::PointField{"rgba", 12, DT::kUint32, 1}};
  cloud.data = PJ::Span<const uint8_t>(k_pt, sizeof(k_pt));
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

PJ::ObjectTopicId registerCloudTopic(PJ::ObjectStore& store) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/cloud";
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  return *topic_id;
}

// REGRESSION: the bounds scan must refresh world_bounds_ on every sample even in solid mode
// (where no colormap consumes it). Pre-fix the scan was skipped for solid/RGB/auto-off, freezing
// world_bounds_ at the first sample and silently feeding the camera scene-fit a stale extent.
TEST(PointCloudLayerFastPath, WorldBoundsTracksLatestCloudInSolidMode) {
  g_two_extent_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_two_extent_calls,
                                          &emitTwoExtentCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));  // renders the near cloud (stamp 100)

  // Switch to solid colour (no colormap) and advance to the far cloud (stamp 200).
  layer.setColorType(pj::scene3d::PointcloudRenderPass::ColorType::kSolid);
  layer.renderAtForTest(200);

  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value()) << "world_bounds_ was never set on the solid fast path";
  EXPECT_FLOAT_EQ(bounds->min.x, 5.0f);  // tracks the FAR cloud, not the stale near one
  EXPECT_FLOAT_EQ(bounds->min.y, 6.0f);
  EXPECT_FLOAT_EQ(bounds->min.z, 7.0f);
  EXPECT_FLOAT_EQ(bounds->max.x, 5.0f);
}

// A packed-rgba RGB-direct cloud must take the zero-copy fast path (not the CPU fallback).
TEST(PointCloudLayerFastPath, RgbCloudTakesFastPath) {
  g_rgba_calls.store(0);
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_rgba_calls,
                                          &emitRgbaCloud);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));

  // RGB-direct is the smart default once a colour field is detected; assert it explicitly too.
  layer.setColorType(pj::scene3d::PointcloudRenderPass::ColorType::kRgb);
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.activeCloudIsFastForTest()) << "packed-rgba RGB cloud did not take the fast path";
}

}  // namespace
