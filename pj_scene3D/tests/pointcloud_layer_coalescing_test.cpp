// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Coverage for PointCloudLayer's latest-wins compressed-decode coalescing state
// machine (M.66). The async decode (requestDecode/startDecode/onDecodeFinished)
// runs on the Qt thread pool with several subtle invariants verified by nothing
// before this test:
//   (a) the single-slot pending_ latest-wins queue collapses a superseded sample
//       (request A while inflight, then B, then C -> only C is decoded after A);
//   (b) wanted_ staleness: a late decode that finished after the tracker scrubbed
//       away (even back onto a cached frame) is cached but NOT painted over the
//       live frame;
//   (c) failed_id_ memoization: an undecodable sample is recorded and a second
//       renderAt at that time issues NO new decode (asserted via the startDecode
//       counter).
//
// Headless-testable because QtConcurrent + QFutureWatcher run under the
// QCoreApplication event loop in main(); processEvents() drives the worker's
// queued finished() slot. Valid clouds are built with the REAL Cloudini encoder
// (the same fixture path pointcloud_codecs_test uses) so the decode succeeds.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cloudini_lib/cloudini.hpp"
#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
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

constexpr std::string_view kSchema = "mock/compressed_point_cloud";
// Samples stamped at/after this produce a CompressedPointCloud whose format the
// decoder does not recognize (decode always fails); earlier stamps produce a
// valid lossless-Cloudini blob.
constexpr PJ::Timestamp kGarbageStampThreshold = 100000;

std::atomic<int> g_parser_calls{0};

// A 4-float point: x, y, z, intensity (point_step = 16).
struct Pt {
  float x, y, z, intensity;
};

// Cloudini-encode a small cloud with the lossless (NONE first-stage + ZSTD) path
// so the decode round-trips exactly. Mirrors pointcloud_codecs_test::encodeCloudini.
std::shared_ptr<std::vector<uint8_t>> encodeValidCloudini() {
  std::vector<Pt> pts;
  for (int i = 0; i < 8; ++i) {
    pts.push_back(
        {static_cast<float>(i) * 0.1f, static_cast<float>(i) * 0.2f, static_cast<float>(i) * 0.3f,
         static_cast<float>(i)});
  }
  Cloudini::EncodingInfo info;
  info.width = static_cast<uint32_t>(pts.size());
  info.height = 1;
  info.point_step = sizeof(Pt);
  info.encoding_opt = Cloudini::EncodingOptions::NONE;
  info.compression_opt = Cloudini::CompressionOption::ZSTD;
  info.fields = {
      {"x", 0, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"y", 4, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"z", 8, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"intensity", 12, Cloudini::FieldType::FLOAT32, std::nullopt},
  };
  Cloudini::PointcloudEncoder encoder(info);
  Cloudini::ConstBufferView input(reinterpret_cast<const uint8_t*>(pts.data()), pts.size() * sizeof(Pt));
  auto out = std::make_shared<std::vector<uint8_t>>();
  encoder.encode(input, *out);
  return out;
}

// Pre-encoded blobs shared by all parser instances. Set up once before any
// test runs; the parser's create function is non-capturing (a plain fn ptr for
// vtableWithCreate), so it reaches these through file scope.
std::shared_ptr<std::vector<uint8_t>> g_valid_blob;

PJ::sdk::CompressedPointCloud wrapValid(PJ::Timestamp ts) {
  PJ::sdk::CompressedPointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.format = "cloudini";
  cloud.data = PJ::Span<const uint8_t>(g_valid_blob->data(), g_valid_blob->size());
  cloud.anchor = g_valid_blob;  // keep the bytes alive on the decode worker
  return cloud;
}

PJ::sdk::CompressedPointCloud wrapGarbage(PJ::Timestamp ts) {
  static const uint8_t k_blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  PJ::sdk::CompressedPointCloud cloud;
  cloud.timestamp_ns = ts;
  cloud.frame_id = "lidar";
  cloud.format = "bogus";  // decodeCompressedPointCloud rejects the unknown format
  cloud.data = PJ::Span<const uint8_t>(k_blob, sizeof(k_blob));
  return cloud;
}

// Object construction for the mock parser: produces a valid (ts < threshold) or
// undecodable (ts >= threshold) CompressedPointCloud. Wrapped by
// test::CountingObjectParser, which bumps g_parser_calls per parse.
PJ::Expected<PJ::sdk::ObjectRecord> emitCloud(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  if (ts >= kGarbageStampThreshold) {
    return PJ::sdk::ObjectRecord{.ts = ts, .object = wrapGarbage(ts)};
  }
  return PJ::sdk::ObjectRecord{.ts = ts, .object = wrapValid(ts)};
}

// A fresh session + store + parser-registered topic per test, with one sample
// pushed per timestamp in `stamps`. The store payload bytes are placeholders —
// the parser ignores them and synthesizes the cloud from the timestamp.
struct Harness {
  PJ::SessionManager session;
  PJ::ObjectTopicId topic_id;

  explicit Harness(const std::vector<PJ::Timestamp>& stamps) {
    PJ::ObjectStore& store = session.objectStore();
    topic_id = registerObjectTopic(session, "/cloud");
    for (PJ::Timestamp ts : stamps) {
      EXPECT_TRUE(store.pushOwned(topic_id, ts, std::vector<uint8_t>{static_cast<uint8_t>(ts & 0xFF)}).has_value());
    }
    session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                        return new CountingObjectParser(
                                            kSchema, PJ::sdk::BuiltinObjectType::kCompressedPointCloud, &g_parser_calls,
                                            &emitCloud);
                                      }));
  }
};

// Pumps the event loop until the layer's async decode chain is idle (no inflight
// and no pending), or the deadline passes. onDecodeFinished is a queued slot, and
// draining pending_ can re-arm inflight_, so a single processEvents is not enough.
void drainDecode(pj::scene3d::PointCloudLayer& layer) {
  const bool settled = pumpUntil([&layer] { return !layer.decodeInFlightForTest(); }, std::chrono::seconds(10));
  ASSERT_TRUE(settled) << "compressed decode chain never settled";
}

// (a) Three rapid requests A,B,C while A is inflight collapse B: pending_ holds
// only the latest, so after A finishes the layer decodes C (not B) and paints C.
TEST(PointCloudLayerCoalescing, LatestWinsCollapsesSupersededPending) {
  g_parser_calls.store(0);
  // 10 is the bootstrap sample (decoded + cached at attach); A=100, B=200, C=300
  // are all distinct and UNCACHED so each is a fresh decode candidate.
  Harness harness({10, 100, 200, 300});

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &harness.session;
  pj::scene3d::PointCloudLayer layer(harness.topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kCompressedPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  // attach() bootstrap-decodes the first sample (10) asynchronously; let it settle
  // so the test starts from a clean (no inflight, no pending) state.
  drainDecode(layer);

  const int decodes_before = layer.startDecodeCountForTest();

  // Drive A, then B, then C WITHOUT pumping the loop: A's worker may finish on the
  // pool thread, but onDecodeFinished is a queued slot that cannot run until we
  // processEvents, so inflight_ stays A and B/C route into pending_ (B superseded).
  layer.renderAtForTest(100);
  ASSERT_TRUE(layer.decodeInFlightForTest()) << "first request did not start a decode";
  layer.renderAtForTest(200);
  layer.renderAtForTest(300);

  drainDecode(layer);

  // Exactly two decodes since the snapshot: A (100) and the collapsed-latest C
  // (300). B (200) must never have been dispatched.
  EXPECT_EQ(layer.startDecodeCountForTest(), decodes_before + 2)
      << "the superseded pending sample B was decoded instead of being collapsed";
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{300})
      << "the latest sample C was not the one painted";
}

// (b) A late decode that finishes after the tracker scrubbed back onto a cached
// frame is cached but NOT painted over the live (cached) frame.
TEST(PointCloudLayerCoalescing, LateDecodeDoesNotRepaintAfterScrubBack) {
  g_parser_calls.store(0);
  Harness harness({100, 500});

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &harness.session;
  pj::scene3d::PointCloudLayer layer(harness.topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kCompressedPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  drainDecode(layer);

  // Decode + cache + paint the earlier frame (100) fully.
  layer.renderAtForTest(100);
  drainDecode(layer);
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});

  // Tracker moves to the later sample (500): an async decode of 500 starts.
  layer.renderAtForTest(500);
  ASSERT_TRUE(layer.decodeInFlightForTest());

  // Before 500 lands, the tracker scrubs back to 100 — served instantly from the
  // decode cache, so 100 becomes the live painted frame again (wanted_ = 100).
  layer.renderAtForTest(100);
  ASSERT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100});

  // The late 500 decode finishes: it is stale (wanted_ is 100), so it is cached but
  // must NOT repaint over the live 100 frame.
  drainDecode(layer);
  EXPECT_EQ(layer.lastPushedStampForTest(), std::optional<int64_t>{100})
      << "a late, scrubbed-away decode repainted a stale frame";
}

// (c) An undecodable sample is memoized in failed_id_; a second renderAt at that
// time issues NO new decode (the startDecode counter stays put).
TEST(PointCloudLayerCoalescing, FailedSampleIsMemoizedAndNotRedecoded) {
  g_parser_calls.store(0);
  Harness harness({100, kGarbageStampThreshold});

  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &harness.session;
  pj::scene3d::PointCloudLayer layer(harness.topic_id, u"cloud"_s, PJ::sdk::BuiltinObjectType::kCompressedPointCloud);
  ASSERT_TRUE(layer.attach(ctx));
  drainDecode(layer);

  // First visit to the garbage sample: a decode is dispatched and fails.
  layer.renderAtForTest(kGarbageStampThreshold);
  drainDecode(layer);
  const int decodes_after_failure = layer.startDecodeCountForTest();
  EXPECT_GE(decodes_after_failure, 1) << "the garbage sample was never decoded";
  // The failure path clears the view rather than painting stale points.
  EXPECT_EQ(layer.lastPushedStampForTest(), std::nullopt) << "a failed decode left a sample painted";

  // Second visit to the SAME garbage sample: failed_id_ short-circuits renderAt
  // before any decode is requested, so the counter must not move.
  layer.renderAtForTest(kGarbageStampThreshold);
  EXPECT_FALSE(layer.decodeInFlightForTest()) << "a known-failed sample was re-requested";
  EXPECT_EQ(layer.startDecodeCountForTest(), decodes_after_failure)
      << "a known-failed sample triggered a redundant re-decode at tracker rate";
}

}  // namespace

namespace {

class PointcloudCoalescingEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    g_valid_blob = encodeValidCloudini();
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new PointcloudCoalescingEnvironment);

}  // namespace
