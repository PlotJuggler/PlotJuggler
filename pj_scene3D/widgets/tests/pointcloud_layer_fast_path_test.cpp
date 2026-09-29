// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PointCloudLayer conversion and retained-array integration tests:
//  - world_bounds_ must refresh on EVERY new sample even when no colormap consumes the scan
//    (solid / RGB / auto-off). It feeds the camera scene-fit (Scene3DDockWidget::
//    updateSceneBounds) every frame; gating the bounds scan on colour mode silently froze it
//    (a regression caught in review). This test pushes two differently-placed clouds in solid
//    mode and asserts worldBounds() tracks the second, not the first.
//  - an RGB-direct (kRgb) cloud carrying a packed rgba uint32 field keeps its wire colour
//    verbatim in kXyzRgba records.
//  - the retained array's reuse, eviction and re-derivation.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <memory>
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
#include "pj_widgets/GraphicsApi.h"  // paddedUploadSize
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

// The size of a layer's retained array: its records, padded for upload.
qsizetype paddedBytes(const pj::scene3d::PointCloudLayer& layer) {
  return PJ::paddedUploadSize(
      static_cast<qsizetype>(layer.pointCount() * pj::scene3d::pointVertexStride(layer.vertexFormat())));
}

std::atomic<int> g_anchored_calls{0};
// Anchored copies of the two-extent clouds, so a test can watch who holds them.
std::shared_ptr<std::vector<uint8_t>>& anchoredBytes(PJ::Timestamp ts) {
  static std::shared_ptr<std::vector<uint8_t>> near_bytes;
  static std::shared_ptr<std::vector<uint8_t>> far_bytes;
  return ts >= 200 ? far_bytes : near_bytes;
}

PJ::Expected<PJ::sdk::ObjectRecord> emitAnchoredCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  const float point[3] = {static_cast<float>(ts), 0.0f, 0.0f};
  auto& bytes = anchoredBytes(ts);
  bytes = std::make_shared<std::vector<uint8_t>>(sizeof(point));
  std::memcpy(bytes->data(), point, sizeof(point));
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
  cloud.data = PJ::Span<const uint8_t>(bytes->data(), bytes->size());
  cloud.anchor = bytes;
  return PJ::sdk::ObjectRecord{.ts = ts, .object = cloud};
}

std::atomic<int> g_intensity_calls{0};
// Two points with a float32 intensity of 3 and 7.
PJ::Expected<PJ::sdk::ObjectRecord> emitIntensityCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const float k_points[8] = {1.0f, 2.0f, 3.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  PJ::sdk::PointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.width = 2;
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.row_step = 32;
  cloud.fields = {
      PJ::sdk::PointField{"x", 0, DT::kFloat32, 1}, PJ::sdk::PointField{"y", 4, DT::kFloat32, 1},
      PJ::sdk::PointField{"z", 8, DT::kFloat32, 1}, PJ::sdk::PointField{"intensity", 12, DT::kFloat32, 1}};
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(k_points), sizeof(k_points));
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
  layer.setColorType(pj::scene3d::PointColorType::kSolid);
  layer.renderAtForTest(200);

  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value()) << "world_bounds_ was never set on the solid fast path";
  EXPECT_FLOAT_EQ(bounds->min.x, 5.0f);  // tracks the FAR cloud, not the stale near one
  EXPECT_FLOAT_EQ(bounds->min.y, 6.0f);
  EXPECT_FLOAT_EQ(bounds->min.z, 7.0f);
  EXPECT_FLOAT_EQ(bounds->max.x, 5.0f);
}

// A packed-rgba RGB-direct cloud is retained as kXyzRgba records, carrying the wire colour as-is.
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
  layer.setColorType(pj::scene3d::PointColorType::kRgb);
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.pointCount(), 1U);
  ASSERT_EQ(layer.vertexFormat(), pj::scene3d::PointVertexFormat::kXyzRgba);
  pj::scene3d::PointVertex vertex;
  std::memcpy(&vertex, layer.vertexBytes().constData(), sizeof(vertex));
  EXPECT_EQ(vertex.attribute, 0xff302010U) << "packed-rgba colour was not carried verbatim";
}

// The retained array is rewritten in place for the next sample only while nothing
// shares it: an array the view's upload still holds keeps the old cloud.
TEST(PointCloudLayerFastPath, RetainedVerticesReusedOnlyWhenUnshared) {
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
  ASSERT_TRUE(layer.attach(ctx));  // the near cloud (stamp 100)
  const auto first_x = [](const QByteArray& bytes) {
    float x = 0.0f;
    std::memcpy(&x, bytes.constData(), sizeof(x));
    return x;
  };

  QByteArray held = layer.vertexBytes();  // what an upload batch in flight holds
  const std::uint64_t revision = layer.geometryRevision();
  layer.renderAtForTest(200);
  EXPECT_EQ(first_x(held), 1.0f) << "the shared array was written";
  EXPECT_EQ(first_x(layer.vertexBytes()), 5.0f);
  EXPECT_NE(held.constData(), layer.vertexBytes().constData());
  EXPECT_GT(layer.geometryRevision(), revision);

  held = QByteArray();
  const char* unshared = layer.vertexBytes().constData();
  layer.renderAtForTest(100);
  EXPECT_EQ(first_x(layer.vertexBytes()), 1.0f);
  EXPECT_EQ(layer.vertexBytes().constData(), unshared) << "an unshared array was not reused";
}
// The layer keeps no reference to a converted sample's bytes: the retained
// vertices are its own, and the store owns the payload.
TEST(PointCloudLayerFastPath, ConvertedSamplesAreNotHeld) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_anchored_calls,
                                          &emitAnchoredCloud);
                                    }));
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));  // the near cloud (stamp 100)
  EXPECT_EQ(anchoredBytes(100).use_count(), 1) << "the converted sample was held";
  EXPECT_GT(layer.pointCount(), 0U);

  layer.renderAtForTest(200);
  EXPECT_EQ(anchoredBytes(200).use_count(), 1) << "the converted sample was held";
  EXPECT_GT(layer.pointCount(), 0U);
}
// The retained vertices carry the scalar only while a colormapped non-spatial field
// reads it, and a colour-type or field change that needs another layout re-packs.
TEST(PointCloudLayerFastPath, VertexFormatFollowsWhatTheStyleReads) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_intensity_calls,
                                          &emitIntensityCloud);
                                    }));
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  using pj::scene3d::PointVertexFormat;
  const auto first_attribute = [&] {
    pj::scene3d::PointVertex vertex;
    std::memcpy(&vertex, layer.vertexBytes().constData(), sizeof(vertex));
    return vertex.attribute;
  };

  layer.setColorField(u"intensity"_s);
  layer.setColorType(pj::scene3d::PointColorType::kField);
  ASSERT_EQ(layer.vertexFormat(), PointVertexFormat::kXyzScalar);
  EXPECT_EQ(layer.pointCount(), 2U);
  EXPECT_EQ(layer.vertexBytes().size(), paddedBytes(layer)) << "not padded for upload";
  EXPECT_EQ(first_attribute(), std::bit_cast<uint32_t>(3.0f));
  EXPECT_EQ(layer.scalarRange(glm::mat4(1.0f)), (std::pair<float, float>{3.0f, 7.0f}));

  std::uint64_t revision = layer.geometryRevision();
  layer.setColorType(pj::scene3d::PointColorType::kSolid);
  EXPECT_EQ(layer.vertexFormat(), PointVertexFormat::kXyz) << "solid colour kept the unread scalar";
  EXPECT_EQ(layer.pointCount(), 2U);
  EXPECT_EQ(layer.vertexBytes().size(), paddedBytes(layer)) << "not padded for upload";
  EXPECT_GT(layer.geometryRevision(), revision);
  EXPECT_EQ(layer.scalarRange(glm::mat4(1.0f)), (std::pair<float, float>{3.0f, 7.0f}));

  revision = layer.geometryRevision();
  layer.setColorType(pj::scene3d::PointColorType::kField);
  EXPECT_EQ(layer.vertexFormat(), PointVertexFormat::kXyzScalar) << "the colormap lost its scalar";
  EXPECT_GT(layer.geometryRevision(), revision);
  EXPECT_EQ(first_attribute(), std::bit_cast<uint32_t>(3.0f));

  layer.setColorField(u"x"_s);  // coloured by the fixed-frame axis: the shader derives it
  EXPECT_EQ(layer.vertexFormat(), PointVertexFormat::kXyz);
}

// A static cloud drops its array once the view uploaded it and keeps what the
// view reads without it; a new QRhi's upload rebuilds the same bytes under the
// same revision, and a playhead tick on the same sample does not.
TEST(PointCloudLayerFastPath, StaticCloudEvictsOnceUploadedAndRederivesTheSameBytes) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_intensity_calls,
                                          &emitIntensityCloud);
                                    }));
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setColorField(u"intensity"_s);
  layer.setColorType(pj::scene3d::PointColorType::kField);
  const QByteArray uploaded = layer.vertexBytes();  // what the upload batch holds
  const std::uint64_t revision = layer.geometryRevision();
  const auto bounds = layer.sourceBounds();
  const auto range = layer.scalarRange(glm::mat4(1.0f));

  layer.onVerticesUploaded(revision - 1);  // a stale upload drops nothing
  EXPECT_FALSE(layer.verticesEvictedForTest());
  layer.onVerticesUploaded(revision);
  EXPECT_TRUE(layer.verticesEvictedForTest());
  EXPECT_TRUE(layer.vertexBytes().isEmpty());
  EXPECT_TRUE(uploaded.isDetached()) << "the layer still holds the uploaded array";
  EXPECT_EQ(layer.pointCount(), 2U);
  EXPECT_EQ(layer.vertexFormat(), pj::scene3d::PointVertexFormat::kXyzScalar);
  EXPECT_EQ(layer.geometryRevision(), revision);
  EXPECT_EQ(layer.sourceBounds(), bounds);
  EXPECT_EQ(layer.scalarRange(glm::mat4(1.0f)), range);

  const int decodes = g_intensity_calls.load();
  layer.renderAtForTest(100);
  EXPECT_TRUE(layer.verticesEvictedForTest()) << "a tick on the same sample re-derived it";
  EXPECT_EQ(g_intensity_calls.load(), decodes);

  ASSERT_TRUE(layer.ensureVertexBytes());
  EXPECT_FALSE(layer.verticesEvictedForTest());
  EXPECT_EQ(layer.vertexBytes(), uploaded) << "the re-derived bytes differ";
  EXPECT_EQ(layer.geometryRevision(), revision);
  EXPECT_EQ(layer.sourceBounds(), bounds);
  EXPECT_EQ(layer.scalarRange(glm::mat4(1.0f)), range);
  EXPECT_TRUE(layer.ensureVertexBytes());  // nothing evicted: no work
}

// While evicted, a style change that needs another layout re-packs the same
// sample at once (a new revision, evicted again once uploaded); one that only
// flips a uniform leaves the cloud evicted.
TEST(PointCloudLayerFastPath, StyleChangeWhileEvictedRepacksOnlyWhenTheLayoutChanges) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId topic_id = registerCloudTopic(store);
  ASSERT_TRUE(store.pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kPointCloud, &g_intensity_calls,
                                          &emitIntensityCloud);
                                    }));
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::PointCloudLayer layer(topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  layer.setColorField(u"intensity"_s);
  layer.setColorType(pj::scene3d::PointColorType::kField);
  layer.onVerticesUploaded(layer.geometryRevision());
  ASSERT_TRUE(layer.verticesEvictedForTest());

  layer.setColormap(PJ::Colormap::kViridis);
  EXPECT_TRUE(layer.verticesEvictedForTest()) << "a uniform-only change re-packed";

  const std::uint64_t revision = layer.geometryRevision();
  layer.setColorType(pj::scene3d::PointColorType::kSolid);
  EXPECT_FALSE(layer.verticesEvictedForTest());
  EXPECT_EQ(layer.vertexFormat(), pj::scene3d::PointVertexFormat::kXyz);
  EXPECT_EQ(layer.pointCount(), 2U);
  EXPECT_GT(layer.geometryRevision(), revision);
  layer.onVerticesUploaded(layer.geometryRevision());
  EXPECT_TRUE(layer.verticesEvictedForTest()) << "a re-pack of the same sample is not a stream";

  ASSERT_TRUE(layer.ensureVertexBytes());
  EXPECT_EQ(layer.vertexFormat(), pj::scene3d::PointVertexFormat::kXyz);
  EXPECT_EQ(layer.vertexBytes().size(), paddedBytes(layer));
}

// While evicted, a new sample converts as usual; one following the previous
// sample this closely is a stream, whose array stays for the next rewrite.
TEST(PointCloudLayerFastPath, SampleChangeWhileEvictedConvertsAndAStreamKeepsItsArray) {
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
  ASSERT_TRUE(layer.attach(ctx));  // the near cloud (stamp 100)
  layer.onVerticesUploaded(layer.geometryRevision());
  ASSERT_TRUE(layer.verticesEvictedForTest());

  const std::uint64_t revision = layer.geometryRevision();
  layer.renderAtForTest(200);
  EXPECT_FALSE(layer.verticesEvictedForTest());
  EXPECT_GT(layer.geometryRevision(), revision);
  float x = 0.0f;
  std::memcpy(&x, layer.vertexBytes().constData(), sizeof(x));
  EXPECT_EQ(x, 5.0f);
  ASSERT_TRUE(layer.sourceBounds().has_value());
  EXPECT_FLOAT_EQ(layer.sourceBounds()->min.x, 5.0f);

  layer.onVerticesUploaded(layer.geometryRevision());
  EXPECT_FALSE(layer.verticesEvictedForTest()) << "a stream dropped the array it rewrites";
  EXPECT_FALSE(layer.vertexBytes().isEmpty());
}

}  // namespace
