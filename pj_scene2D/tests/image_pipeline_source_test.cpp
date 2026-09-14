// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/image_pipeline_source.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "test_png_io.h"

namespace {

class CountingStage final : public PJ::CodecStage {
 public:
  explicit CountingStage(int& calls) : calls_(calls) {}

  PJ::Expected<PJ::DecodedFrame> decode(const PJ::DecodedFrame& input) const override {
    ++calls_;
    PJ::DecodedFrame frame;
    frame.width = 1;
    frame.height = 1;
    frame.format = PJ::PixelFormat::kRGBA8888;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, input.pixels->empty() ? 0 : input.pixels->front());
    return frame;
  }

 private:
  int& calls_;
};

std::unique_ptr<PJ::CodecPipeline> makeCountingPipeline(int& calls) {
  auto pipeline = std::make_unique<PJ::CodecPipeline>();
  pipeline->addStage(std::make_unique<CountingStage>(calls));
  return pipeline;
}

// Parser returning a canonical sdk::Image with caller-chosen geometry/encoding,
// reusing the pushed payload bytes verbatim as Image::data. Lets a test drive
// the raw/bayer decode paths (incl. Mosaico's grayscale-PNG-wrapped buffers).
class CanonicalRawParser final : public PJ::MessageParserPluginBase {
 public:
  CanonicalRawParser(
      uint32_t width, uint32_t height, std::string encoding, uint32_t row_step,
      std::optional<float> compressed_depth_min = std::nullopt,
      std::optional<float> compressed_depth_max = std::nullopt, std::string schema_name = "image") {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [width, height, encoding, row_step, compressed_depth_min, compressed_depth_max](
                               PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
              .width = width,
              .height = height,
              .encoding = encoding,
              .row_step = row_step,
              .is_bigendian = false,
              .data = payload.bytes,
              .anchor = payload.anchor,
              .compressed_depth_min = compressed_depth_min,
              .compressed_depth_max = compressed_depth_max,
              .timestamp_ns = ts,
              .frame_id = "",
          }}};
    };
    registerSchemaHandler(schema_name, std::move(handler));
  }
};

// Mirrors the real-world failure mode where a MessageParser keeps internal
// scratch (fastcdr offset, dictionaries) and two ImagePipelineSource workers
// sharing the same parser pointer enter parseObject concurrently. The atomic
// `in_flight_` counter catches concurrent entry deterministically; a
// non-atomic scratch field gives TSan a second signal when the suite is
// rebuilt with PJ_ENABLE_TSAN=ON.
class RacyImageParser final : public PJ::MessageParserPluginBase {
 public:
  RacyImageParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [this](
                               PJ::Timestamp ts, PJ::sdk::PayloadView payload) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      const int prev = in_flight_.fetch_add(1, std::memory_order_acq_rel);
      if (prev > 0) {
        race_observed_.store(true, std::memory_order_release);
      }
      // Force a deterministic window: without the sleep, the two workers may
      // serialise by chance on a fast machine and hide the race.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      scratch_ = static_cast<int>(ts & 0xFF);  // intentionally non-atomic
      PJ::sdk::ObjectRecord out{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::Image{
              .width = 1,
              .height = 1,
              .encoding = "rgb8",
              .row_step = 3,
              .is_bigendian = false,
              .data = payload.bytes,
              .anchor = payload.anchor,
              .compressed_depth_min = std::nullopt,
              .compressed_depth_max = std::nullopt,
              .timestamp_ns = ts,
              .frame_id = "",
          }},
      };
      in_flight_.fetch_sub(1, std::memory_order_acq_rel);
      return out;
    };
    registerSchemaHandler("image", std::move(handler));
  }

  bool raceObserved() const {
    return race_observed_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<int> in_flight_{0};
  std::atomic<bool> race_observed_{false};
  int scratch_ = 0;
};

// A parser whose parseObject throws — models a misbehaving plugin (or an
// allocation failure on crafted geometry). The worker must catch at the thread
// boundary; an uncaught throw out of the std::thread callable calls std::terminate.
class ThrowingImageParser final : public PJ::MessageParserPluginBase {
 public:
  ThrowingImageParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kImage;
    handler.parse_object = [](PJ::Timestamp, PJ::sdk::PayloadView) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      throw std::runtime_error("parser blew up");
    };
    registerSchemaHandler("image", std::move(handler));
  }
};

// Bridges the source's frame-ready callback (fired from the worker thread)
// into a condition variable test code can wait on. Each waitReady() consumes
// at most one ready notification; back-to-back fires while no waiter is
// listening collapse to a single pending ready (matches the single-slot
// result_frame_ semantics of the source).
// Declare it BEFORE the source it is installed on: the source's worker thread
// calls into it until the source is destroyed.
struct FrameSync {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;

  void install(PJ::ImagePipelineSource& source) {
    source.setFrameReadyCallback([this]() {
      std::lock_guard lock(mutex);
      ready = true;
      cv.notify_all();
    });
  }

  bool waitReady(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::unique_lock lock(mutex);
    if (cv.wait_for(lock, timeout, [this] { return ready; })) {
      ready = false;
      return true;
    }
    return false;
  }
};

// Brief idle wait. Used to confirm a request that takes the dedup-skip path
// has been consumed by the worker (which never fires the frame-ready callback
// because no frame was produced). 100 ms is generous for an in-memory store.
constexpr std::chrono::milliseconds kIdleSettleTime{100};

// Distortion-free calibration whose only effect is to lift the source to a
// `out_w x out_h` native resolution — lets a rectification test assert the
// output size without depending on a specific lens model.
PJ::sdk::CameraInfo makeRescaleCalibration(const std::string& frame_id, uint32_t out_w, uint32_t out_h) {
  PJ::sdk::CameraInfo ci;
  ci.frame_id = frame_id;
  ci.width = out_w;
  ci.height = out_h;
  ci.distortion_model = "plumb_bob";
  ci.D = {0.0, 0.0, 0.0, 0.0, 0.0};
  ci.K = {4.0, 0.0, static_cast<double>(out_w) / 2.0, 0.0, 4.0, static_cast<double>(out_h) / 2.0, 0.0, 0.0, 1.0};
  ci.R = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  ci.P = {};  // zeroed -> computeUndistortMap falls back to K.
  return ci;
}

// Serialize a raw rgb8 sdk::Image (pj_image_v1) carrying `frame_id` so the
// CanonicalImageCodec source path can rectify it against a matching CameraInfo.
std::vector<uint8_t> makeRawRgbBlob(
    uint32_t w, uint32_t h, const std::string& frame_id, const std::vector<uint8_t>& rgb) {
  PJ::sdk::Image img;
  img.width = w;
  img.height = h;
  img.encoding = "rgb8";
  img.row_step = w * 3;
  img.frame_id = frame_id;
  img.data = PJ::Span<const uint8_t>(rgb.data(), rgb.size());
  return PJ::serializeImage(img);
}

}  // namespace

TEST(ImagePipelineSourceTest, DeduplicatesResolvedEntryTimestampBeforeResolvingLazyPayload) {
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());

  int fetch_calls = 0;
  ASSERT_TRUE(store.pushLazy(*topic, 1'000, [&fetch_calls]() -> PJ::sdk::PayloadView {
    ++fetch_calls;
    return PJ::sdk::makePayloadView({1});
  }));
  ASSERT_TRUE(store.pushLazy(*topic, 2'000, [&fetch_calls]() -> PJ::sdk::PayloadView {
    ++fetch_calls;
    return PJ::sdk::makePayloadView({2});
  }));

  int decode_calls = 0;
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, makeCountingPipeline(decode_calls));
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  EXPECT_EQ(fetch_calls, 1);
  EXPECT_EQ(decode_calls, 1);
  ASSERT_TRUE(source.takeFrame().has_value());

  // Both timestamps resolve to entry 0 (timestamp 1'000), which the worker
  // already decoded — dedup must short-circuit BEFORE invoking the lazy
  // fetcher or the pipeline stage. No callback fires for a dedup-skip.
  source.setTimestamp(1'200);
  source.setTimestamp(1'900);
  std::this_thread::sleep_for(kIdleSettleTime);
  EXPECT_EQ(fetch_calls, 1);
  EXPECT_EQ(decode_calls, 1);
  EXPECT_FALSE(source.takeFrame().has_value());

  source.setTimestamp(2'000);
  ASSERT_TRUE(sync.waitReady());
  EXPECT_EQ(fetch_calls, 2);
  EXPECT_EQ(decode_calls, 2);
  ASSERT_TRUE(source.takeFrame().has_value());
}

TEST(ImagePipelineSourceTest, ParserDrivenPathConsumesCanonicalImage) {
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{10, 20, 30}));

  CanonicalRawParser parser(1, 1, "rgb8", 3);
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 1);
  EXPECT_EQ(frame->base->height, 1);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(*frame->base->pixels, (std::vector<uint8_t>{10, 20, 30}));
}

TEST(ImagePipelineSourceTest, ThrowingParserDoesNotTerminateProcess) {
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{10, 20, 30}));
  ASSERT_TRUE(store.pushOwned(*topic, 2'000, std::vector<uint8_t>{40, 50, 60}));

  ThrowingImageParser parser;
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);

  // A parser that throws inside parseObject must NOT terminate the process: the
  // worker catches at the thread boundary (ARCHITECTURE §10.5) and produces no
  // frame. Without the barrier this aborts the entire test binary.
  source.setTimestamp(1'000);
  std::this_thread::sleep_for(kIdleSettleTime);
  EXPECT_FALSE(source.takeFrame().has_value());

  // The worker stays alive and responsive: a second (also-throwing) request is
  // handled without crashing, and clean destruction still joins the worker.
  source.setTimestamp(2'000);
  std::this_thread::sleep_for(kIdleSettleTime);
  EXPECT_FALSE(source.takeFrame().has_value());
}

TEST(ImagePipelineSourceTest, ParserDrivenCompressedDepthDecodesPngThenNormalizesMono16) {
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(2, 2, 16, std::vector<uint16_t>{0, 1000, 2000, 3000});
  ASSERT_FALSE(png.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/depth", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, png));

  CanonicalRawParser parser(0, 0, "compressedDepth", 0, 0.0f, 1.0f, "depth");
  ASSERT_TRUE(parser.bindSchema("depth", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(frame->base->pixels->size(), 12U);
  EXPECT_EQ((*frame->base->pixels)[0], 0);
  EXPECT_EQ((*frame->base->pixels)[3], 0);
  EXPECT_EQ((*frame->base->pixels)[6], 127);
  EXPECT_EQ((*frame->base->pixels)[9], 255);
}

TEST(ImagePipelineSourceTest, ParserDrivenCompressedDepthRepairsChunkStreamStartingAtIhdr) {
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(2, 2, 16, std::vector<uint16_t>{0, 1000, 2000, 3000});
  ASSERT_GT(png.size(), 12U);
  ASSERT_EQ(png[12], 'I');
  ASSERT_EQ(png[13], 'H');
  ASSERT_EQ(png[14], 'D');
  ASSERT_EQ(png[15], 'R');

  std::vector<uint8_t> chunk_stream(png.begin() + 12, png.end());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/depth", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, chunk_stream));

  CanonicalRawParser parser(0, 0, "compressedDepth", 0, 0.0f, 1.0f, "depth");
  ASSERT_TRUE(parser.bindSchema("depth", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(frame->base->pixels->size(), 12U);
  EXPECT_EQ((*frame->base->pixels)[0], 0);
  EXPECT_EQ((*frame->base->pixels)[3], 0);
  EXPECT_EQ((*frame->base->pixels)[6], 127);
  EXPECT_EQ((*frame->base->pixels)[9], 255);
}

TEST(ImagePipelineSourceTest, CoalescesBurstOfSetTimestampToSingleDecode) {
  // Three distinct entries; burst-fire 100 setTimestamp calls that resolve
  // across all three. Worker must skip intermediate values via the in-decode
  // re-check and only decode the LATEST target (plus possibly one earlier
  // target if it captured a request before the burst finished).
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{1, 2, 3}));
  ASSERT_TRUE(store.pushOwned(*topic, 2'000, std::vector<uint8_t>{4, 5, 6}));
  ASSERT_TRUE(store.pushOwned(*topic, 3'000, std::vector<uint8_t>{7, 8, 9}));

  CanonicalRawParser parser(1, 1, "rgb8", 3);
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  for (int i = 0; i < 100; ++i) {
    source.setTimestamp(1'000 + (i % 3) * 1'000);
  }
  // Explicit final target so the worker has a deterministic "latest" — the
  // burst-loop modulo arithmetic ends on i=99 → t=1'000, which would race
  // with whichever decode the worker happens to capture.
  source.setTimestamp(3'000);

  // Worker should converge on entry 2 (timestamp 3'000) and stop. Wait long
  // enough for any pending decodes to drain.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  // Latest target was 3'000 → entry 2 → pixel value 7.
  ASSERT_NE(frame->base->pixels, nullptr);
  EXPECT_EQ((*frame->base->pixels)[0], 7U);
}

TEST(ImagePipelineSourceTest, DestructorJoinsWorkerEvenWithPendingRequest) {
  // Stress destruction race: construct + destroy in a tight loop with a fresh
  // setTimestamp posted each iteration. Each destructor must cleanly stop the
  // worker (no use-after-free, no hang). Run under ASAN/TSAN this is the
  // signal that destruction ordering is correct.
  for (int i = 0; i < 50; ++i) {
    PJ::ObjectStore store;
    auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
    ASSERT_TRUE(topic.has_value());
    ASSERT_TRUE(store.pushOwned(*topic, 1'000, std::vector<uint8_t>{10, 20, 30}));

    CanonicalRawParser parser(1, 1, "rgb8", 3);
    ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));

    auto source = std::make_unique<PJ::ImagePipelineSource>(
        &store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
    source->setTimestamp(1'000);
    // Destroy immediately — may be mid-decode. Destructor must join cleanly.
    source.reset();
  }
}

TEST(ImagePipelineSourceTest, SharedParserMutexSerializesConcurrentSources) {
  // Two ImagePipelineSource instances sharing the same parser pointer (the
  // SessionManager singleton, in production) must never enter parseObject
  // concurrently — fastcdr et al. keep stateful scratch and corrupt under
  // a race, manifesting as bogus payload sizes and segfaults. Passing a
  // shared mutex collapses concurrent decodes onto a single critical section.
  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(store.pushOwned(*topic, 1'000 + i * 100, std::vector<uint8_t>{static_cast<uint8_t>(i), 20, 30}));
  }

  RacyImageParser parser;
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));

  auto parser_mutex = std::make_shared<std::mutex>();
  FrameSync sync_a;
  FrameSync sync_b;
  PJ::ImagePipelineSource source_a(&store, *topic, &parser, parser_mutex, /*parser_keepalive=*/nullptr);
  PJ::ImagePipelineSource source_b(&store, *topic, &parser, parser_mutex, /*parser_keepalive=*/nullptr);
  sync_a.install(source_a);
  sync_b.install(source_b);

  // Burst-fire both sources at distinct timestamps so they cannot dedup against
  // their own previous request. Worker threads will pile onto parseObject and,
  // without the shared mutex, race.
  for (int i = 0; i < 8; ++i) {
    source_a.setTimestamp(1'000 + i * 100);
    source_b.setTimestamp(1'000 + i * 100);
  }
  // Wait long enough for both workers to drain — 5 ms per parse × 8 frames
  // × 2 workers, serialized = ~80 ms; double it for safety.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_FALSE(parser.raceObserved()) << "parseObject was entered concurrently — shared parser_mutex did not "
                                         "serialise consumers (this is the bug the fix addresses).";
}

TEST(ImagePipelineSourceTest, ParserDrivenRawRgb8WrappedInGrayscalePngDecodesToColor) {
  // Mosaico transports a raw rgb8 frame losslessly by reshaping its flat byte
  // buffer (row_step * height) as an 8-bit grayscale PNG of width=row_step,
  // height=height, then advertising encoding="rgb8". The viewer must detect the
  // PNG container, recover the flat bytes, and reinterpret at the logical 2x2
  // geometry — NOT treat the PNG container bytes as raw pixels (renders black).
  const std::vector<uint8_t> rgb = {
      10, 20, 30, 40,  50,  60,   // row 0: two RGB pixels
      70, 80, 90, 100, 110, 120,  // row 1: two RGB pixels
  };
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(/*width=row_step*/ 6, /*height*/ 2, 8, rgb);
  ASSERT_FALSE(png.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, png));

  CanonicalRawParser parser(2, 2, "rgb8", 6);
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  EXPECT_EQ(*frame->base->pixels, rgb);
}

TEST(ImagePipelineSourceTest, ParserDrivenBayerRggb8WrappedInGrayscalePngDemosaicsToRgb) {
  // A bayer_rggb8 mosaic arrives the same way: a grayscale PNG (width=row_step=W)
  // wrapping the mono8 CFA samples. The viewer must recover the mosaic and
  // demosaic to RGB888 — not render the gray mosaic as-is. The CFA encodes a
  // uniform per-channel field (R=200, G=100, B=50) so every reconstructed pixel
  // must be exactly (200,100,50).
  constexpr int kW = 4;
  constexpr int kH = 4;
  std::vector<uint8_t> mosaic(static_cast<size_t>(kW) * kH);
  for (int r = 0; r < kH; ++r) {
    for (int c = 0; c < kW; ++c) {
      // RGGB top-left 2x2 tile: (even,even)=R, (odd,odd)=B, else G.
      uint8_t v = 100;  // G
      if (r % 2 == 0 && c % 2 == 0) {
        v = 200;  // R
      } else if (r % 2 == 1 && c % 2 == 1) {
        v = 50;  // B
      }
      mosaic[static_cast<size_t>(r) * kW + static_cast<size_t>(c)] = v;
    }
  }
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(kW, kH, 8, mosaic);
  ASSERT_FALSE(png.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic({PJ::DatasetId{1}, "/camera/image", "{}"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, png));

  CanonicalRawParser parser(kW, kH, "bayer_rggb8", kW);
  ASSERT_TRUE(parser.bindSchema("image", PJ::Span<const uint8_t>{}));
  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, &parser, /*parser_mutex=*/nullptr, /*parser_keepalive=*/nullptr);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, kW);
  EXPECT_EQ(frame->base->height, kH);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  ASSERT_EQ(frame->base->pixels->size(), static_cast<size_t>(kW) * kH * 3);
  // Interior pixel (1,1) has a full neighbourhood — the uniform field round-trips.
  const auto& px = *frame->base->pixels;
  const size_t i = (1 * static_cast<size_t>(kW) + 1) * 3;
  EXPECT_EQ(px[i + 0], 200);
  EXPECT_EQ(px[i + 1], 100);
  EXPECT_EQ(px[i + 2], 50);
}

TEST(ImagePipelineSourceTest, CanonicalCodecDecodesSerializedRawRgb8) {
  // Toolbox path: the producer serializes an sdk::Image (pj_base pj_image_v1
  // codec) and pushes the blob via pushOwnedObject; the topic advertises
  // image_codec=pj_image_v1 and gets NO MessageParser. The viewer's canonical
  // codec source must deserialize each blob, then decode it.
  const std::vector<uint8_t> rgb = {10, 20, 30, 40, 50, 60};  // 2x1 rgb8
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 1;
  img.encoding = "rgb8";
  img.row_step = 6;
  img.data = PJ::Span<const uint8_t>(rgb.data(), rgb.size());
  const std::vector<uint8_t> blob = PJ::serializeImage(img);
  ASSERT_FALSE(blob.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 1);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  EXPECT_EQ(*frame->base->pixels, rgb);
}

TEST(ImagePipelineSourceTest, CanonicalCodecDecodesSerializedPngWrappedRgb8) {
  // The real Mosaico shape: a serialized sdk::Image whose encoding is "rgb8" but
  // whose data is the flat buffer wrapped in a grayscale PNG (width=row_step).
  // Exercises both halves of the fix end to end: canonical deserialize, then
  // PNG-unwrap + reinterpret at the logical geometry.
  const std::vector<uint8_t> rgb = {
      10, 20, 30, 40,  50,  60,  // row 0
      70, 80, 90, 100, 110, 120  // row 1
  };
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(/*width=row_step*/ 6, /*height*/ 2, 8, rgb);
  ASSERT_FALSE(png.empty());

  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "rgb8";
  img.row_step = 6;
  img.data = PJ::Span<const uint8_t>(png.data(), png.size());
  const std::vector<uint8_t> blob = PJ::serializeImage(img);
  ASSERT_FALSE(blob.empty());

  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());

  EXPECT_EQ(frame->base->width, 2);
  EXPECT_EQ(frame->base->height, 2);
  EXPECT_EQ(frame->base->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(frame->base->pixels, nullptr);
  EXPECT_EQ(*frame->base->pixels, rgb);
}

TEST(ImagePipelineSourceTest, CpuFallbackRectifiesToNativeResolutionWithNoMap) {
  // Default mode (no GPU): the source rectifies on the worker and delivers a
  // display-ready frame at native resolution, with no rectify_map attached.
  const std::vector<uint8_t> rgb(static_cast<size_t>(4) * 3 * 3, 128);  // solid 4x3 rgb8
  const std::vector<uint8_t> blob = makeRawRgbBlob(4, 3, "cam", rgb);
  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  source.setCameraInfoMap({{"cam", makeRescaleCalibration("cam", 8, 6)}});
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 8);
  EXPECT_EQ(frame->base->height, 6);
  EXPECT_EQ(frame->base->rectify_map, nullptr);
}

TEST(ImagePipelineSourceTest, GpuModeKeepsRawFrameAndAttachesRectifyMap) {
  // GPU mode: the worker leaves the frame raw and attaches the (cached) map so
  // the widget rectifies at draw time. The logical (rectified) size lives on the
  // map; the uploaded pixels stay at the smaller source resolution.
  const std::vector<uint8_t> rgb(static_cast<size_t>(4) * 3 * 3, 128);
  const std::vector<uint8_t> blob = makeRawRgbBlob(4, 3, "cam", rgb);
  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  source.setCameraInfoMap({{"cam", makeRescaleCalibration("cam", 8, 6)}});
  source.setGpuRectificationAvailable(true);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 4);
  EXPECT_EQ(frame->base->height, 3);
  ASSERT_NE(frame->base->rectify_map, nullptr);
  EXPECT_EQ(frame->base->rectify_map->out_width, 8);
  EXPECT_EQ(frame->base->rectify_map->out_height, 6);
}

TEST(ImagePipelineSourceTest, RectifyDisabledPassesFrameThroughRawInCpuMode) {
  // setRectifyEnabled(false) is the user override for the always-on auto-decision
  // (TECHNICAL_NOTES §12): a matching CameraInfo exists, but the operator wants the
  // raw image (e.g. the stream is already rectified). The frame must pass through at
  // its SOURCE resolution with no rescale and no map — the safe passthrough contract.
  const std::vector<uint8_t> rgb(static_cast<size_t>(4) * 3 * 3, 128);  // solid 4x3 rgb8
  const std::vector<uint8_t> blob = makeRawRgbBlob(4, 3, "cam", rgb);
  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  source.setCameraInfoMap({{"cam", makeRescaleCalibration("cam", 8, 6)}});
  source.setRectifyEnabled(false);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 4);
  EXPECT_EQ(frame->base->height, 3);
  EXPECT_EQ(frame->base->rectify_map, nullptr);
}

TEST(ImagePipelineSourceTest, RectifyDisabledInGpuModeAttachesNoMap) {
  // The gate sits ahead of the GPU branch too: with rectification off, the worker
  // must not hand the widget a rectify_map, so the widget draws the raw frame
  // without the shader remap (no double-handling of the disable).
  const std::vector<uint8_t> rgb(static_cast<size_t>(4) * 3 * 3, 128);
  const std::vector<uint8_t> blob = makeRawRgbBlob(4, 3, "cam", rgb);
  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  source.setCameraInfoMap({{"cam", makeRescaleCalibration("cam", 8, 6)}});
  source.setGpuRectificationAvailable(true);
  source.setRectifyEnabled(false);
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  auto frame = source.takeFrame();
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_EQ(frame->base->width, 4);
  EXPECT_EQ(frame->base->height, 3);
  EXPECT_EQ(frame->base->rectify_map, nullptr);
}

TEST(ImagePipelineSourceTest, RectifyReEnabledRedecodesAndRectifies) {
  // End-to-end of the toggle. setRectifyEnabled() flips the gate and ARMS a
  // re-decode (invalidate()); re-applying the SAME timestamp is what actually
  // posts it — exactly the sequence ImageLayer::applyOptions() runs after the
  // setter (invalidate() alone does not wake the worker by design). Off → raw
  // 4x3; back on → rectified to native 8x6 again.
  const std::vector<uint8_t> rgb(static_cast<size_t>(4) * 3 * 3, 128);
  const std::vector<uint8_t> blob = makeRawRgbBlob(4, 3, "cam", rgb);
  PJ::ObjectStore store;
  auto topic = store.registerTopic(
      {PJ::DatasetId{1}, "/camera/image", R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(store.pushOwned(*topic, 1'000, blob));

  FrameSync sync;
  PJ::ImagePipelineSource source(&store, *topic, PJ::ImagePipelineSource::CanonicalImageCodec{});
  source.setCameraInfoMap({{"cam", makeRescaleCalibration("cam", 8, 6)}});
  sync.install(source);

  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  {
    auto frame = source.takeFrame();  // default ON -> rectified to native resolution.
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->base.has_value());
    EXPECT_EQ(frame->base->width, 8);
    EXPECT_EQ(frame->base->height, 6);
  }

  source.setRectifyEnabled(false);  // arm the re-decode...
  source.setTimestamp(1'000);       // ...and post it (mirrors applyOptions).
  ASSERT_TRUE(sync.waitReady());
  {
    auto frame = source.takeFrame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->base.has_value());
    EXPECT_EQ(frame->base->width, 4);
    EXPECT_EQ(frame->base->height, 3);
  }

  source.setRectifyEnabled(true);  // re-enable -> rectifies again.
  source.setTimestamp(1'000);
  ASSERT_TRUE(sync.waitReady());
  {
    auto frame = source.takeFrame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->base.has_value());
    EXPECT_EQ(frame->base->width, 8);
    EXPECT_EQ(frame->base->height, 6);
  }
}
