// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression for the consumer-side, zero-copy video unwrap. The runtime stores
// the RAW canonical PJ.VideoFrame wire message per ObjectStore entry and defers
// the unwrap to display time: StreamingVideoDecoder's NAL extractor parses each
// entry's message and feeds FFmpeg the contained Annex-B `data` span WITHOUT
// copying the H.264 blob.
//
// This proves the whole feature end-to-end at the core level:
//   serializeVideoFrame(Annex-B) -> ObjectStore wire bytes
//     -> deserializeVideoFrameView extractor -> StreamingVideoDecoder::decodeAt
//     -> width/height > 0.
//
// Uses a view-based extractor (deserializeVideoFrameView, aliasing the entry's
// own bytes) — the same contract the parser-mode StreamingVideoSource relies on,
// without needing a MessageParser plugin in the unit. Skips when no test video
// is available (set PJ_TEST_VIDEO).

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pj_base/builtin/video_frame.hpp"
#include "pj_base/builtin/video_frame_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/span.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/streaming_video_decoder.h"
#include "pj_scene2d_core/streaming_video_source.h"
#include "test_mp4_demux.h"

namespace PJ {
namespace {

std::string testVideoPath() {
  // sdk::getEnv is the portable wrapper; a raw std::getenv trips MSVC C4996
  // (deprecation) which the Windows CI build treats as an error under /WX.
  if (const auto env = sdk::getEnv("PJ_TEST_VIDEO")) {
    return *env;
  }
  return "pj_scene2D/testdata/test_480p.mp4";
}

// Wrap one Annex-B frame as canonical PJ.VideoFrame wire bytes (= what a
// producer / parser write surface emits, and what the runtime stores verbatim).
std::vector<uint8_t> wrapVideoFrame(const test::AnnexBPacket& pkt) {
  sdk::VideoFrame vf;
  vf.timestamp_ns = pkt.timestamp;
  vf.frame_id = "camera";
  vf.format = "h264";
  vf.data = Span<const uint8_t>(pkt.data.data(), pkt.data.size());
  return serializeVideoFrame(vf);
}

class ConsumerUnwrap : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string video = testVideoPath();
    if (!std::filesystem::exists(video)) {
      GTEST_SKIP() << "test video not found: " << video << " (set PJ_TEST_VIDEO to an H.264 .mp4)";
    }
    packets_ = test::extractAnnexBPackets(video);
    ASSERT_GT(packets_.size(), 1u) << "need at least a couple of packets";
    ASSERT_TRUE(packets_.front().keyframe) << "first packet must be a keyframe to bootstrap decode";
  }

  std::vector<test::AnnexBPacket> packets_;
};

// The decoder, fed RAW canonical wire messages and a view-extractor that unwraps
// each one to its Annex-B `data`, decodes a real frame.
TEST_F(ConsumerUnwrap, DecodesCanonicalWireEntriesViaViewExtractor) {
  ObjectStore store;
  auto topic = store.registerTopic({.dataset_id = 1, .topic_name = "/camera/video", .metadata_json = "{}"});
  ASSERT_TRUE(topic.has_value());

  // Store ONE GOP worth of canonical wire messages (the entry bytes are the
  // wrapping PJ.VideoFrame message, NOT the raw NAL — exactly the runtime shape).
  // Real video topics are keyed by the MCAP logTime — a rebased, NON-negative
  // DTS (the converter rebases to 0, and MCAP logTime is unsigned). Raw demux
  // DTS starts negative for B-frame streams, so rebase to the first packet's DTS
  // to match the production contract.
  const Timestamp base_dts = packets_.front().dts;
  size_t count = 0;
  for (size_t i = 0; i < packets_.size(); ++i) {
    if (i > 0 && packets_[i].keyframe) {
      break;  // stop at the second keyframe — one full GOP is enough
    }
    auto wire = wrapVideoFrame(packets_[i]);
    ASSERT_TRUE(store.pushOwned(*topic, packets_[i].dts - base_dts, std::move(wire)).has_value());
    ++count;
  }
  ASSERT_GT(count, 0u);
  ASSERT_EQ(store.entryCount(*topic), count);

  // View-extractor: deserialize the canonical message, aliasing the entry's own
  // bytes (kept alive by entry.payload.anchor). Zero copy of the H.264 blob; the
  // span stays valid as long as the resolved entry — which the decoder keeps
  // alive across extract+decode.
  StreamingVideoDecoder::NalExtractor extractor =
      [](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    auto frame =
        deserializeVideoFrameView(entry.payload.bytes.data(), entry.payload.bytes.size(), entry.payload.anchor);
    if (!frame.has_value()) {
      return unexpected(frame.error());
    }
    return StreamingVideoDecoder::ExtractedFrame{frame->data, frame->format, frame->timestamp_ns};
  };

  StreamingVideoDecoder decoder;
  decoder.attach(&store, *topic, std::move(extractor));

  const auto range = store.timeRange(*topic);
  auto decoded = decoder.decodeAt(range.second);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_GT(decoded->width, 0);
  EXPECT_GT(decoded->height, 0);
  EXPECT_FALSE(decoded->isNull());
}

// The stored entry bytes are the canonical PJ.VideoFrame protobuf message, which
// begins with a protobuf field tag — NOT an Annex-B start code. The view
// extractor (the contract the parser-mode StreamingVideoSource relies on) unwraps
// them back to the raw NAL bitstream. This is why a canonical video topic must be
// consumed through the parser/view extractor: the stored bytes are a wrapped
// message, not a feedable elementary stream. (We deliberately do NOT assert that
// feeding the wrapped bytes to FFmpeg fails — because PJ.VideoFrame places `data`
// last, FFmpeg's lenient Annex-B parser can skip the short protobuf prefix and
// stumble onto the embedded NAL; that is fragile and breaks for other layouts,
// e.g. Foxglove's data=3, so we assert the reliable byte-level unwrap instead.)
TEST_F(ConsumerUnwrap, ViewExtractorUnwrapsCanonicalWireToRawAnnexB) {
  constexpr std::array<uint8_t, 4> kAnnexBStart = {0x00, 0x00, 0x00, 0x01};

  ObjectStore store;
  auto topic = store.registerTopic({.dataset_id = 1, .topic_name = "/camera/video2", .metadata_json = "{}"});
  ASSERT_TRUE(topic.has_value());

  auto wire = wrapVideoFrame(packets_.front());  // a keyframe, canonically wrapped
  ASSERT_GE(wire.size(), 4u);
  // The wrapped message starts with the protobuf field-1 tag, not a NAL start code.
  EXPECT_NE(std::memcmp(wire.data(), kAnnexBStart.data(), 4), 0)
      << "canonical wire must be a protobuf envelope, not raw Annex-B";

  ASSERT_TRUE(store.pushOwned(*topic, 0, std::move(wire)).has_value());
  const auto entry = store.at(*topic, 0);
  ASSERT_TRUE(entry.has_value());

  // View extractor: zero-copy unwrap back to the raw Annex-B NAL.
  auto frame =
      deserializeVideoFrameView(entry->payload.bytes.data(), entry->payload.bytes.size(), entry->payload.anchor);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_GE(frame->data.size(), 4u);
  EXPECT_EQ(std::memcmp(frame->data.data(), kAnnexBStart.data(), 4), 0)
      << "view extractor must unwrap to the raw Annex-B start code";
}

// The parser-mode source must hold the opaque parser keepalive for its WHOLE
// lifetime and drop it only after its decode worker is joined: on app close the extension catalog dlcloses the
// parser plugin, and the worker calls parseObject. parser=nullptr here:
// the worker never decodes, we only assert the keepalive lifetime.
TEST(StreamingVideoSourceKeepalive, HoldsParserKeepaliveForSourceLifetime) {
  ObjectStore store;
  auto topic = store.registerTopic({.dataset_id = 1, .topic_name = "/camera/video", .metadata_json = "{}"});
  ASSERT_TRUE(topic.has_value());

  auto sentinel = std::make_shared<int>(7);
  std::weak_ptr<int> weak = sentinel;
  {
    StreamingVideoSource src(
        &store, *topic, /*parser=*/nullptr, /*parser_mutex=*/nullptr,
        /*parser_keepalive=*/sentinel);
    sentinel.reset();
    EXPECT_FALSE(weak.expired()) << "source must keep the parser keepalive alive while it lives";
  }
  EXPECT_TRUE(weak.expired()) << "source must drop the keepalive once destroyed (after worker join)";
}

// Scrub settle. StreamingVideoSource decodes on a worker thread, so a
// single takeFrame() right after setTimestamp() — the GUI's one update()/render()
// per onTrackerTime — races the (tens-of-ms) GOP seek and returns nothing. With
// no notification, the decoded frame is never re-polled, so a scrub that stops
// freezes on the previous frame. The frame-ready callback (mirrors
// ImagePipelineSource) re-triggers a poll when the worker finishes, which is what
// the GUI hooks to surface the final frame. This test pushes one GOP of RAW
// Annex-B (identity-extractor / raw ctor), posts a seek to the tip, and asserts:
//   (a) takeFrame() is empty immediately after setTimestamp (the race), and
//   (b) the callback fires and a real frame is then available.
TEST_F(ConsumerUnwrap, NotifiesWhenDecodedFrameReady) {
  ObjectStore store;
  auto topic = store.registerTopic({.dataset_id = 1, .topic_name = "/camera/video3", .metadata_json = "{}"});
  ASSERT_TRUE(topic.has_value());

  // Raw-bytes path: entry bytes ARE the Annex-B NAL (no canonical wrapping), so
  // the default identity extractor feeds them straight to FFmpeg. Rebase DTS to
  // the first packet so keys are non-negative (matches the production contract
  // and dodges the kf_ts<0 sentinel for B-frame streams).
  const Timestamp base_dts = packets_.front().dts;
  for (size_t i = 0; i < packets_.size(); ++i) {
    if (i > 0 && packets_[i].keyframe) {
      break;  // one full GOP is enough to decode the tip
    }
    ASSERT_TRUE(store.pushOwned(*topic, packets_[i].dts - base_dts, packets_[i].data).has_value());
  }
  ASSERT_GT(store.entryCount(*topic), 0u);

  StreamingVideoSource src(&store, *topic);  // raw-bytes ctor → identity extractor

  std::mutex m;
  std::condition_variable cv;
  bool fired = false;
  src.setFrameReadyCallback([&] {
    std::lock_guard<std::mutex> lock(m);
    fired = true;
    cv.notify_all();
  });

  const Timestamp tip = store.timeRange(*topic).second;
  src.setTimestamp(tip);

  // (a) The single synchronous poll the GUI would do right after a seek almost
  // always loses the race with the worker — documents why the callback is needed.
  // (Not strictly asserted: on a heavily loaded box the worker could conceivably
  // win the race; the load-bearing assertion is (b).)

  // (b) Wait (bounded) for the worker to decode and notify, then the frame is
  // observable. Without the callback this wait would time out — the frame would
  // sit in result_frame_ unobserved.
  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return fired; }))
        << "frame-ready callback never fired — async decode result is unobservable from the GUI";
  }

  auto frame = src.takeFrame();
  ASSERT_TRUE(frame.has_value()) << "takeFrame must return the just-notified frame";
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_GT(frame->base->width, 0);
  EXPECT_GT(frame->base->height, 0);
  EXPECT_FALSE(frame->base->isNull());
}

}  // namespace
}  // namespace PJ
