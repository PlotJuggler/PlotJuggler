// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// DepthCloudLayer integration: a depth-encoded sdk::Image topic + a CameraInfo
// topic (matched by frame_id) drive a real SessionManager/ObjectStore through the
// layer; attach() back-projects the first sample into points handed to the render
// pass. Also pins the encoding gate (isDepthEncoding) and the color-image opt-out.

#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kDepthSchema = "mock/depth_image";
constexpr std::string_view kInfoSchema = "mock/camera_info";

// 2x2 32FC1 depth (metres), row-major: all four pixels valid (> 0).
const std::array<float, 4> kDepthsM = {1.0f, 2.0f, 3.0f, 4.0f};

PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "32FC1";
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kDepthsM.data()), kDepthsM.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}

PJ::Expected<PJ::sdk::ObjectRecord> emitColorImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  static const std::array<uint8_t, 12> kRgb = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "rgb8";
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(kRgb.data(), kRgb.size());
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}

PJ::Expected<PJ::sdk::ObjectRecord> emitCameraInfo(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  PJ::sdk::CameraInfo ci;
  ci.frame_id = "cam";
  ci.width = 2;
  ci.height = 2;
  ci.timestamp_ns = ts;
  ci.K = {100.0, 0.0, 0.5, 0.0, 100.0, 0.5, 0.0, 0.0, 1.0};  // fx=fy=100, cx=cy=0.5
  return PJ::sdk::ObjectRecord{.ts = ts, .object = ci};
}

// A 2x2 16-bit-grayscale PNG carrying kDepthsM in millimetres, STRIPPED of its
// 8-byte signature + 4-byte IHDR length so it begins at the "IHDR" chunk type —
// the headerless shape RealSense compressedDepth leaves after the parser. Built
// once; the returned bytes back a long-lived Span.
const std::vector<uint8_t>& barePngDepthBytes() {
  static const std::vector<uint8_t> bytes = [] {
    QImage img(2, 2, QImage::Format_Grayscale16);
    for (int y = 0; y < 2; ++y) {
      auto* line = reinterpret_cast<uint16_t*>(img.scanLine(y));
      for (int x = 0; x < 2; ++x) {
        line[x] = static_cast<uint16_t>(kDepthsM[y * 2 + x] * 1000.0f);  // metres -> mm
      }
    }
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    buf.close();
    const QByteArray bare = png.mid(12);  // drop 8-byte signature + 4-byte IHDR length
    return std::vector<uint8_t>(bare.begin(), bare.end());
  }();
  return bytes;
}

PJ::Expected<PJ::sdk::ObjectRecord> emitCompressedDepthImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  const std::vector<uint8_t>& png = barePngDepthBytes();
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "compressedDepth";
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(png.data(), png.size());
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}

// A depth image in a chosen frame (function pointers can't capture, so the frame
// is baked per-emitter). Geometry is identical to emitDepthImage (kDepthsM).
PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImageInFrame(PJ::Timestamp ts, std::string_view frame) {
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "32FC1";
  img.frame_id = std::string(frame);
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kDepthsM.data()), kDepthsM.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}
PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImageCamB(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return emitDepthImageInFrame(ts, "camB");
}
PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImageNoMatch(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return emitDepthImageInFrame(ts, "camX");  // matches none of the registered cameras
}

// CameraInfo for a named frame with focal length fx=fy=`focal` (cx=cy=0.5, 2x2).
PJ::Expected<PJ::sdk::ObjectRecord> makeCameraInfo(PJ::Timestamp ts, std::string_view frame, double focal) {
  PJ::sdk::CameraInfo ci;
  ci.frame_id = std::string(frame);
  ci.width = 2;
  ci.height = 2;
  ci.timestamp_ns = ts;
  ci.K = {focal, 0.0, 0.5, 0.0, focal, 0.5, 0.0, 0.0, 1.0};
  return PJ::sdk::ObjectRecord{.ts = ts, .object = ci};
}
PJ::Expected<PJ::sdk::ObjectRecord> emitCameraInfoCamA(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return makeCameraInfo(ts, "camA", 100.0);  // fx=fy=100
}
PJ::Expected<PJ::sdk::ObjectRecord> emitCameraInfoCamB(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return makeCameraInfo(ts, "camB", 200.0);  // fx=fy=200 (half the X/Y spread of camA)
}

// Bumped once per CameraInfo parse, so a test can assert intrinsics are resolved
// from a latched CameraInfo once and cached, not re-parsed on every depth frame.
std::atomic<int> g_camera_info_parse_count{0};

PJ::ObjectTopicId registerTypedTopic(
    PJ::ObjectStore& store, const std::string& name, const std::string& builtin_type_name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = name;
  desc.metadata_json = R"({"builtin_object_type":")" + builtin_type_name + R"("})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

TEST(DepthCloudLayer, BackProjects32FC1UsingCameraInfoIntrinsics) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info = registerTypedTopic(store, "/cam/depth/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(info, 100, std::vector<uint8_t>{0x02}).has_value());

  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
                                    }));
  session.registerObjectTopicParser(
      info, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
        return new CountingObjectParser(kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr, &emitCameraInfo);
      }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  // attach() renders the first sample: 4 valid pixels -> 4 back-projected points.
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});
  EXPECT_EQ(layer.lastPointCountForTest(), 4U);
  EXPECT_EQ(layer.sourceFrameForTest(), u"cam"_s);
}

// RealSense compressedDepth arrives as a BARE PNG (no signature). toDepthView must
// restore the signature before decoding, or QImage rejects it and the layer reports
// "Not a depth image". Regression for that fringe — without the repair this yields 0
// points.
TEST(DepthCloudLayer, BackProjectsBarePngCompressedDepth) {
  // The fixture really is headerless (begins at the IHDR chunk type, no signature).
  const std::vector<uint8_t>& png = barePngDepthBytes();
  ASSERT_GE(png.size(), 4U);
  EXPECT_EQ(png[0], 'I');
  EXPECT_EQ(png[1], 'H');
  EXPECT_EQ(png[2], 'D');
  EXPECT_EQ(png[3], 'R');

  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info = registerTypedTopic(store, "/cam/depth/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(info, 100, std::vector<uint8_t>{0x02}).has_value());

  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr,
                                          &emitCompressedDepthImage);
                                    }));
  session.registerObjectTopicParser(
      info, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
        return new CountingObjectParser(kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr, &emitCameraInfo);
      }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  // The bare PNG decodes (16UC1 mm) and back-projects to 4 points (== the 32FC1 case).
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});
  EXPECT_EQ(layer.lastPointCountForTest(), 4U);
  EXPECT_EQ(layer.sourceFrameForTest(), u"cam"_s);
}

// Resolve-before-memo regression: when the sample on the GPU is unchanged,
// renderAt must skip on metadata alone WITHOUT resolving payload bytes — a
// resolve is a cold refetch (file read + whole-chunk decompress on the GUI
// thread) once the ResidentPayloadPool has evicted the entry's seed. The store's
// warm latestAt cache would mask a redundant resolve, so it is deliberately
// reset between renders via an out-of-order push.
TEST(DepthCloudLayer, UnchangedSampleDoesNotResolvePayload) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info = registerTypedTopic(store, "/cam/depth/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(info, 50, std::vector<uint8_t>{0x02}).has_value());
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, depth, 100, std::vector<uint8_t>{0x01}, fetch_count);

  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
                                    }));
  session.registerObjectTopicParser(
      info, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
        return new CountingObjectParser(kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr, &emitCameraInfo);
      }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));  // renders the first frame (t=100)
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});
  const int fetches_after_attach = *fetch_count;
  ASSERT_GT(fetches_after_attach, 0);

  // Bust the warm latestAt cache (an out-of-order insert resets it): any resolve
  // issued by the next render now HAS to re-invoke the lazy fetcher.
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(store, depth, 60, std::vector<uint8_t>{0x01}, old_fetch_count);

  layer.renderAtForTest(100);  // same sample already on the GPU -> metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_attach) << "unchanged sample resolved payload bytes on a later tick";
  EXPECT_EQ(*old_fetch_count, 0) << "the out-of-order older sample must not be resolved at all";
}

// A CameraInfo-only update — a new sample on the camera topic while the depth
// sample is unchanged — must re-resolve intrinsics and re-project. The
// depth-sample memo alone must not swallow the tick, or the view keeps stale
// calibration until the next depth frame.
TEST(DepthCloudLayer, CameraInfoOnlyUpdateReprojects) {
  g_camera_info_parse_count = 0;
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info = registerTypedTopic(store, "/cam/depth/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(info, 50, std::vector<uint8_t>{0x02}).has_value());
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());

  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
                                    }));
  session.registerObjectTopicParser(info, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo,
                                          &g_camera_info_parse_count, &emitCameraInfo);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));  // renders t=100 with intrinsics from info@50
  ASSERT_EQ(layer.lastPointCountForTest(), 4U);
  ASSERT_EQ(g_camera_info_parse_count.load(), 1);

  // CameraInfo-only update at/before the playhead: a new camera-topic entry.
  ASSERT_TRUE(store.pushOwned(info, 60, std::vector<uint8_t>{0x02}).has_value());
  layer.renderAtForTest(100);  // depth sample unchanged
  EXPECT_EQ(g_camera_info_parse_count.load(), 2) << "new CameraInfo was not re-resolved (stale calibration)";
  EXPECT_EQ(layer.lastPointCountForTest(), 4U) << "re-projection did not run";
}

TEST(DepthCloudLayer, CachesIntrinsicsAcrossFramesNotReparsedPerFrame) {
  g_camera_info_parse_count = 0;
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info = registerTypedTopic(store, "/cam/depth/camera_info", "kCameraInfo");
  // CameraInfo latched once before the depth stream; three depth frames follow.
  ASSERT_TRUE(store.pushOwned(info, 50, std::vector<uint8_t>{0x02}).has_value());
  for (PJ::Timestamp ts : {100, 200, 300}) {
    ASSERT_TRUE(store.pushOwned(depth, ts, std::vector<uint8_t>{0x01}).has_value());
  }

  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
                                    }));
  session.registerObjectTopicParser(info, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo,
                                          &g_camera_info_parse_count, &emitCameraInfo);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));  // renders the first frame (t=100)
  layer.renderAtForTest(200);
  layer.renderAtForTest(300);

  // All three frames share the same latched CameraInfo: it is parsed once and the
  // intrinsics cached, NOT re-parsed (and the whole topic list re-scanned) per frame.
  EXPECT_EQ(g_camera_info_parse_count.load(), 1);
  EXPECT_EQ(layer.lastPointCountForTest(), 4U) << "intrinsics still applied on every frame";
}

TEST(DepthCloudLayer, PrefersCameraInfoMatchingFrameIdAmongMany) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info_a = registerTypedTopic(store, "/camA/camera_info", "kCameraInfo");
  const PJ::ObjectTopicId info_b = registerTypedTopic(store, "/camB/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(info_a, 100, std::vector<uint8_t>{0x02}).has_value());
  ASSERT_TRUE(store.pushOwned(info_b, 100, std::vector<uint8_t>{0x03}).has_value());
  session.registerObjectTopicParser(
      depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
        return new CountingObjectParser(kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImageCamB);
      }));
  session.registerObjectTopicParser(info_a, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr,
                                          &emitCameraInfoCamA);
                                    }));
  session.registerObjectTopicParser(info_b, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr,
                                          &emitCameraInfoCamB);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  // frame_id="camB" must select camB's intrinsics (fx=200), never camA's (fx=100).
  EXPECT_EQ(layer.lastPointCountForTest(), 4U);
  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  // max X = (1-0.5)*z_max/fx = 0.5*4/200 = 0.01 for camB; camA (fx=100) would give
  // 0.02, so this value uniquely identifies which camera's intrinsics were used.
  EXPECT_NEAR(bounds->max.x, 0.01f, 2e-3f);
}

TEST(DepthCloudLayer, RefusesIntrinsicsAmongMultipleCamerasWithNoFrameMatch) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info_a = registerTypedTopic(store, "/camA/camera_info", "kCameraInfo");
  const PJ::ObjectTopicId info_b = registerTypedTopic(store, "/camB/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(info_a, 100, std::vector<uint8_t>{0x02}).has_value());
  ASSERT_TRUE(store.pushOwned(info_b, 100, std::vector<uint8_t>{0x03}).has_value());
  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr,
                                          &emitDepthImageNoMatch);
                                    }));
  session.registerObjectTopicParser(info_a, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr,
                                          &emitCameraInfoCamA);
                                    }));
  session.registerObjectTopicParser(info_b, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr,
                                          &emitCameraInfoCamB);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  // Two cameras, neither frame matches "camX": refuse rather than pair the wrong one.
  EXPECT_EQ(layer.lastPointCountForTest(), 0U);
  EXPECT_EQ(layer.lastPushedStampForTest(), std::nullopt);
}

TEST(DepthCloudLayer, FallsBackToLoneCameraOnFrameMismatch) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId depth = registerTypedTopic(store, "/cam/depth/image", "kImage");
  const PJ::ObjectTopicId info_a = registerTypedTopic(store, "/camA/camera_info", "kCameraInfo");
  ASSERT_TRUE(store.pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
  ASSERT_TRUE(store.pushOwned(info_a, 100, std::vector<uint8_t>{0x02}).has_value());
  // Depth frame is "cam" (emitDepthImage); the only camera is "camA" -> mismatch,
  // but unambiguous, so the lone CameraInfo is used.
  session.registerObjectTopicParser(depth, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
                                    }));
  session.registerObjectTopicParser(info_a, makeBoundHandle(kInfoSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kInfoSchema, PJ::sdk::BuiltinObjectType::kCameraInfo, nullptr,
                                          &emitCameraInfoCamA);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(depth, u"depth"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  EXPECT_EQ(layer.lastPointCountForTest(), 4U);
}

TEST(DepthCloudLayer, ColorImageProducesNoPoints) {
  PJ::SessionManager session;
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId color = registerTypedTopic(store, "/cam/color/image", "kImage");
  ASSERT_TRUE(store.pushOwned(color, 100, std::vector<uint8_t>{0x01}).has_value());
  session.registerObjectTopicParser(color, makeBoundHandle(kDepthSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kDepthSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitColorImage);
                                    }));

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  pj::scene3d::DepthCloudLayer layer(color, u"color"_s, PJ::sdk::BuiltinObjectType::kImage);
  ASSERT_TRUE(layer.attach(ctx));

  // A non-depth encoding produces no geometry and no pushed sample.
  EXPECT_EQ(layer.lastPointCountForTest(), 0U);
  EXPECT_EQ(layer.lastPushedStampForTest(), std::nullopt);
}

TEST(IsDepthEncoding, OnlyDepthCarriers) {
  EXPECT_TRUE(pj::scene3d::isDepthEncoding("16UC1"));
  EXPECT_TRUE(pj::scene3d::isDepthEncoding("32FC1"));
  EXPECT_TRUE(pj::scene3d::isDepthEncoding("compressedDepth"));
  EXPECT_FALSE(pj::scene3d::isDepthEncoding("rgb8"));
  EXPECT_FALSE(pj::scene3d::isDepthEncoding("mono16"));  // grayscale camera, not depth
  EXPECT_FALSE(pj::scene3d::isDepthEncoding("jpeg"));
  EXPECT_FALSE(pj::scene3d::isDepthEncoding(""));
}

}  // namespace
