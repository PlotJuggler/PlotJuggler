// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PointCloudLayer RGB-direct colour mode (the foxglove red/green/blue/alpha case).
// A cloud whose fields describe a per-point colour must NOT expose the individual
// colour channels as separately colourable scalar fields. Instead the layer offers
// a single RGB mode (kRgb), defaults to it on a fresh attach, and renders the literal
// per-point colour. A plain XYZI cloud keeps the scalar/colormap behaviour, and an
// explicitly restored colour_type survives the smart default.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QString>
#include <cstdint>
#include <cstring>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using ColorType = pj::scene3d::PointcloudRenderPass::ColorType;
using DT = PJ::sdk::PointField::Datatype;

constexpr std::string_view kSchema = "mock/point_cloud";

// A one-point cloud in the user's foxglove layout: x,y,z,intensity (float32) then
// red,green,blue,alpha (uint8). The colour is olivedrab (107,142,35) — a semantic
// class colour, exactly what should be painted literally.
PJ::Expected<PJ::sdk::ObjectRecord> emitColoredCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const std::vector<uint8_t> buf = [] {
    std::vector<uint8_t> b;
    const auto put_f32 = [&](float f) {
      uint8_t tmp[4];
      std::memcpy(tmp, &f, 4);
      b.insert(b.end(), tmp, tmp + 4);
    };
    put_f32(1.0f);
    put_f32(2.0f);
    put_f32(3.0f);
    put_f32(0.0f);  // intensity
    b.insert(b.end(), {107, 142, 35, 255});
    return b;
  }();
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 20;
  cloud.row_step = 20;
  cloud.fields = {{"x", 0, DT::kFloat32, 1},          {"y", 4, DT::kFloat32, 1},   {"z", 8, DT::kFloat32, 1},
                  {"intensity", 12, DT::kFloat32, 1}, {"red", 16, DT::kUint8, 1},  {"green", 17, DT::kUint8, 1},
                  {"blue", 18, DT::kUint8, 1},        {"alpha", 19, DT::kUint8, 1}};
  cloud.data = PJ::Span<const uint8_t>(buf.data(), buf.size());
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

// A plain XYZI cloud — no colour channels.
PJ::Expected<PJ::sdk::ObjectRecord> emitPlainCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const std::vector<uint8_t> buf(16, 0);
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.row_step = 16;
  cloud.fields = {
      {"x", 0, DT::kFloat32, 1},
      {"y", 4, DT::kFloat32, 1},
      {"z", 8, DT::kFloat32, 1},
      {"intensity", 12, DT::kFloat32, 1}};
  cloud.data = PJ::Span<const uint8_t>(buf.data(), buf.size());
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

// Build + attach a layer whose topic emits `emit_fn`. Returns the live layer plus
// the owning session (kept alive by the caller).
struct AttachedLayer {
  std::unique_ptr<PJ::SessionManager> session;
  std::unique_ptr<pj::scene3d::PointCloudLayer> layer;
};

template <PJ::Expected<PJ::sdk::ObjectRecord> (*EmitFn)(PJ::Timestamp, PJ::sdk::PayloadView)>
AttachedLayer makeAttachedLayer() {
  auto session = std::make_unique<PJ::SessionManager>();
  PJ::ObjectStore& store = session->objectStore();
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "/cloud";
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  EXPECT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  static std::atomic<int> calls{0};
  session->registerObjectTopicParser(*topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
    return new CountingObjectParser(kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &calls, EmitFn);
  }));
  auto layer =
      std::make_unique<pj::scene3d::PointCloudLayer>(*topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  return AttachedLayer{std::move(session), std::move(layer)};
}

TEST(PointCloudLayerRgb, ColorChannelsCollapseToRgbAndDefaultToIt) {
  auto fixture = makeAttachedLayer<&emitColoredCloud>();
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = fixture.session.get();
  ASSERT_TRUE(fixture.layer->attach(ctx));

  // The individual colour channels must NOT be selectable as scalar fields.
  const QStringList fields = fixture.layer->availableColorFields();
  EXPECT_FALSE(fields.contains("red"_L1));
  EXPECT_FALSE(fields.contains("green"_L1));
  EXPECT_FALSE(fields.contains("blue"_L1));
  EXPECT_FALSE(fields.contains("alpha"_L1));
  // Genuine scalar fields are still offered.
  EXPECT_TRUE(fields.contains("intensity"_L1));
  EXPECT_TRUE(fields.contains("x"_L1));

  // A cloud carrying colour defaults to RGB-direct mode.
  EXPECT_TRUE(fixture.layer->hasColorField());
  EXPECT_EQ(fixture.layer->colorType(), ColorType::kRgb);
}

TEST(PointCloudLayerRgb, PlainCloudKeepsFieldColormapMode) {
  auto fixture = makeAttachedLayer<&emitPlainCloud>();
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = fixture.session.get();
  ASSERT_TRUE(fixture.layer->attach(ctx));

  EXPECT_FALSE(fixture.layer->hasColorField());
  EXPECT_EQ(fixture.layer->colorType(), ColorType::kField);
  EXPECT_TRUE(fixture.layer->availableColorFields().contains("intensity"_L1));
}

TEST(PointCloudLayerRgb, RestoredColorTypeSurvivesSmartDefault) {
  auto fixture = makeAttachedLayer<&emitColoredCloud>();

  // Restore an explicit "field" choice BEFORE attach (the layout-restore order).
  QDomDocument doc;
  QDomElement el = doc.createElement(u"pointcloud"_s);
  el.setAttribute(u"color_type"_s, u"field"_s);
  el.setAttribute(u"color_field"_s, u"intensity"_s);
  ASSERT_TRUE(fixture.layer->xmlLoadState(el));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = fixture.session.get();
  ASSERT_TRUE(fixture.layer->attach(ctx));

  // The explicit restore must NOT be overridden by the colour-present RGB default.
  EXPECT_EQ(fixture.layer->colorType(), ColorType::kField);
  EXPECT_EQ(fixture.layer->colorField(), u"intensity"_s);
}

TEST(PointCloudLayerRgb, RgbModeRoundTripsThroughXml) {
  auto fixture = makeAttachedLayer<&emitColoredCloud>();
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = fixture.session.get();
  ASSERT_TRUE(fixture.layer->attach(ctx));
  ASSERT_EQ(fixture.layer->colorType(), ColorType::kRgb);

  QDomDocument doc;
  const QDomElement saved = fixture.layer->xmlSaveState(doc);

  auto restored = makeAttachedLayer<&emitColoredCloud>();
  ASSERT_TRUE(restored.layer->xmlLoadState(saved));
  // A smart-defaulted (latent) RGB choice persists as latent, not as a baked
  // "rgb": the restored layer re-derives the colour-present default at its
  // first decode, so the same layout against a colourless cloud correctly
  // falls back to field colouring instead of forcing RGB.
  pj::scene3d::Scene3DLayerContext restored_ctx;
  restored_ctx.session = restored.session.get();
  ASSERT_TRUE(restored.layer->attach(restored_ctx));
  EXPECT_EQ(restored.layer->colorType(), ColorType::kRgb);
}

}  // namespace
