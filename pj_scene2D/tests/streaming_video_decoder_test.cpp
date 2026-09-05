// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_decoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/object_store.hpp"
#include "test_mp4_demux.h"

namespace PJ {
namespace {

using test::AnnexBPacket;

const std::string kTestVideo = "pj_scene2D/testdata/test_480p.mp4";

ObjectTopicId pushPackets(ObjectStore& store, const std::vector<AnnexBPacket>& packets, size_t count) {
  auto topic_result = store.registerTopic({0, "video/test", R"({"media_class":"video","encoding":"h264"})"});
  EXPECT_TRUE(topic_result.has_value());
  auto topic = topic_result.value();

  for (size_t i = 0; i < count && i < packets.size(); ++i) {
    auto result = store.pushOwned(topic, packets[i].timestamp, packets[i].data);
    EXPECT_TRUE(result.has_value()) << "push failed at i=" << i;
  }
  return topic;
}

class StreamingVideoDecoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kTestVideo)) {
      GTEST_SKIP() << "UNTRACKED FIXTURE test_480p.mp4 — not in the repository, so this suite does NOT run on a clean "
                      "clone. Generate it per pj_scene2D/testdata/README.md.";
    }
    all_packets_ = test::extractAnnexBPackets(kTestVideo);
    ASSERT_GT(all_packets_.size(), 30u) << "need at least 30 packets";

    // Count keyframes for reference
    for (const auto& pkt : all_packets_) {
      if (pkt.keyframe) {
        ++expected_keyframe_count_;
      }
    }
    ASSERT_GT(expected_keyframe_count_, 0);
  }

  std::vector<AnnexBPacket> all_packets_;
  int expected_keyframe_count_ = 0;
};

TEST_F(StreamingVideoDecoderTest, BasicDecode) {
  ObjectStore store;
  // Push enough packets for at least one GOP
  size_t first_gop_end = 0;
  int kf_count = 0;
  for (size_t i = 0; i < all_packets_.size(); ++i) {
    if (all_packets_[i].keyframe) {
      ++kf_count;
      if (kf_count > 1) {
        first_gop_end = i;
        break;
      }
    }
  }
  if (first_gop_end == 0) {
    first_gop_end = all_packets_.size();
  }

  auto topic = pushPackets(store, all_packets_, first_gop_end);

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Decode the last frame in the GOP
  Timestamp last_ts = all_packets_[first_gop_end - 1].timestamp;
  auto result = decoder.decodeAt(last_ts);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 640);
  EXPECT_EQ(result->height, 480);
  // SW decode -> planar YUV420P; HW decode (VAAPI) -> native NV12. Accept both.
  EXPECT_TRUE(result->format == PixelFormat::kYUV420P || result->format == PixelFormat::kNV12)
      << "format=" << static_cast<int>(result->format);
}

TEST_F(StreamingVideoDecoderTest, SequentialLive) {
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/live", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  int decoded_count = 0;
  size_t limit = std::min(all_packets_.size(), size_t{60});

  for (size_t i = 0; i < limit; ++i) {
    auto push_result = store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);
    ASSERT_TRUE(push_result.has_value());

    auto range = store.timeRange(topic);
    auto result = decoder.decodeAt(range.second);
    if (result.has_value() && !result->isNull()) {
      ++decoded_count;
    }
  }

  // First frame(s) before keyframe may fail
  EXPECT_GT(decoded_count, 0) << "should decode at least some frames";
}

TEST_F(StreamingVideoDecoderTest, JoinMidStream) {
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/join", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Push only P-frames (skip initial keyframe)
  size_t first_p = 0;
  for (size_t i = 0; i < all_packets_.size(); ++i) {
    if (!all_packets_[i].keyframe) {
      first_p = i;
      break;
    }
  }

  // Push a few P-frames
  for (size_t i = first_p; i < first_p + 5 && i < all_packets_.size(); ++i) {
    store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);
  }

  EXPECT_FALSE(decoder.isInitialized());
  auto range = store.timeRange(topic);
  auto result = decoder.decodeAt(range.second);
  EXPECT_FALSE(result.has_value()) << "should fail without keyframe";

  // Now push a keyframe (find the next one after our P-frames)
  for (size_t i = first_p + 5; i < all_packets_.size(); ++i) {
    store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);
    if (all_packets_[i].keyframe) {
      // Push a few more after the keyframe
      for (size_t j = i + 1; j < i + 5 && j < all_packets_.size(); ++j) {
        store.pushOwned(topic, all_packets_[j].timestamp, all_packets_[j].data);
      }
      break;
    }
  }

  range = store.timeRange(topic);
  result = decoder.decodeAt(range.second);
  EXPECT_TRUE(result.has_value()) << "should decode after keyframe arrives";
  EXPECT_TRUE(decoder.isInitialized());
}

TEST_F(StreamingVideoDecoderTest, ScrubToMiddle) {
  ObjectStore store;
  auto topic = pushPackets(store, all_packets_, all_packets_.size());

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Find a frame in the middle of the first GOP (not a keyframe)
  size_t mid = 0;
  for (size_t i = 1; i < all_packets_.size(); ++i) {
    if (!all_packets_[i].keyframe) {
      mid = i;
      if (i > 10) {
        break;  // Get a frame well into the GOP
      }
    }
  }

  Timestamp mid_ts = all_packets_[mid].timestamp;
  auto result = decoder.decodeAt(mid_ts);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 640);
}

TEST_F(StreamingVideoDecoderTest, ScrubBackward) {
  ObjectStore store;
  auto topic = pushPackets(store, all_packets_, all_packets_.size());

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Decode a later frame
  size_t later = std::min(size_t{50}, all_packets_.size() - 1);
  auto result1 = decoder.decodeAt(all_packets_[later].timestamp);
  ASSERT_TRUE(result1.has_value()) << result1.error();

  // Now scrub backward to an earlier frame
  size_t earlier = 5;
  auto result2 = decoder.decodeAt(all_packets_[earlier].timestamp);
  ASSERT_TRUE(result2.has_value()) << result2.error();
  EXPECT_FALSE(result2->isNull());
}

TEST_F(StreamingVideoDecoderTest, ScrubForwardWithinGop) {
  ObjectStore store;
  auto topic = pushPackets(store, all_packets_, all_packets_.size());

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Decode frame 5 (within first GOP)
  auto result1 = decoder.decodeAt(all_packets_[5].timestamp);
  ASSERT_TRUE(result1.has_value()) << result1.error();

  // Decode frame 8 (still within same GOP — should not flush)
  auto result2 = decoder.decodeAt(all_packets_[8].timestamp);
  ASSERT_TRUE(result2.has_value()) << result2.error();
  EXPECT_FALSE(result2->isNull());
}

TEST_F(StreamingVideoDecoderTest, RetentionEviction) {
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/retention", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  // Set a tight retention window
  store.setRetentionBudget(topic, {500'000'000, 0});  // 500ms

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Push all packets — retention will evict old ones
  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
  }

  // The earliest timestamps should have been evicted
  auto range = store.timeRange(topic);
  Timestamp first_pkt_ts = all_packets_.front().timestamp;

  if (range.first > first_pkt_ts) {
    // Early frames were evicted — trying to decode them should fail
    // or at least the keyframe index should not contain stale entries
    auto result = decoder.decodeAt(first_pkt_ts);
    EXPECT_FALSE(result.has_value()) << "evicted timestamp should be undecodable";
  }

  // But latest frames should still decode
  auto result = decoder.decodeAt(range.second);
  EXPECT_TRUE(result.has_value()) << result.error();
}

TEST_F(StreamingVideoDecoderTest, KeyframeCount) {
  ObjectStore store;
  auto topic = pushPackets(store, all_packets_, all_packets_.size());

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Trigger a decode to force keyframe index scan
  auto range = store.timeRange(topic);
  decoder.decodeAt(range.second);

  // Verify by decoding at each keyframe timestamp
  int successful_kf_decodes = 0;
  for (const auto& pkt : all_packets_) {
    if (pkt.keyframe) {
      auto result = decoder.decodeAt(pkt.timestamp);
      if (result.has_value() && !result->isNull()) {
        ++successful_kf_decodes;
      }
    }
  }
  // The last keyframe may not produce a frame (no subsequent packets
  // to fill the decoder's reorder buffer), so allow one miss.
  EXPECT_GE(successful_kf_decodes, expected_keyframe_count_ - 1);
}

TEST_F(StreamingVideoDecoderTest, LiveStreamWithRetention) {
  // Simulates the demo: push frames one-by-one with retention,
  // decode the latest on each tick. The decoder must continue
  // working after the initial keyframe is evicted.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/live_retention", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  // Retention window smaller than the full video — forces eviction
  // Use 2 seconds (60 frames at 30fps)
  store.setRetentionBudget(topic, {2'000'000'000, 0});

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  int decoded_count = 0;
  int failed_after_eviction = 0;
  bool eviction_started = false;

  for (size_t i = 0; i < all_packets_.size(); ++i) {
    auto push_result = store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);
    ASSERT_TRUE(push_result.has_value()) << "push failed at i=" << i;

    auto range = store.timeRange(topic);
    if (range.first > all_packets_.front().timestamp) {
      eviction_started = true;
    }

    auto result = decoder.decodeAt(range.second);
    if (result.has_value() && !result->isNull()) {
      ++decoded_count;
    } else if (eviction_started) {
      ++failed_after_eviction;
    }
  }

  EXPECT_TRUE(eviction_started) << "retention should have evicted some entries";
  // The key assertion: decoding must continue working after eviction.
  // Without the fix, failed_after_eviction would be high (all frames after ~150).
  EXPECT_LE(failed_after_eviction, 5) << "should not fail after keyframe eviction in live mode";
  EXPECT_GT(decoded_count, static_cast<int>(all_packets_.size()) / 2)
      << "should decode most frames even with retention";
}

TEST_F(StreamingVideoDecoderTest, LiveDecodeIsRealTime) {
  // Verify that decoding one frame in live mode (forward, sequential)
  // takes less than one frame interval. At 30fps, frame interval = 33ms.
  // A single decodeAt in forward mode should decode just 1 frame, not
  // seek back to a keyframe.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/realtime", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Push all packets first (like a pre-loaded buffer)
  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
  }

  // Warm up: decode the first few frames (VAAPI pipeline needs a few
  // packets before producing output — not a B-frame issue, just HW latency)
  for (size_t i = 0; i < 10 && i < all_packets_.size(); ++i) {
    decoder.decodeAt(all_packets_[i].timestamp);
  }

  // Measure: decode 20 sequential frames and check each takes < 33ms
  using Clock = std::chrono::steady_clock;
  int slow_frames = 0;
  constexpr size_t kTestFrames = 20;
  constexpr auto kMaxFrameTime = std::chrono::milliseconds(33);

  for (size_t i = 10; i < 10 + kTestFrames && i < all_packets_.size(); ++i) {
    auto start = Clock::now();
    auto result = decoder.decodeAt(all_packets_[i].timestamp);
    auto elapsed = Clock::now() - start;

    if (!result.has_value() || result->isNull()) {
      continue;  // EAGAIN from decoder pipeline — normal for first few
    }
    if (elapsed > kMaxFrameTime) {
      ++slow_frames;
    }
  }

  // Allow at most 2 slow frames (VAAPI init jitter, first few frames)
  EXPECT_LE(slow_frames, 2) << "too many frames exceeded 33ms — "
                            << "forward decode may be seeking unnecessarily";
}

TEST_F(StreamingVideoDecoderTest, LiveDecodeWithRetentionIsRealTime) {
  // Same as above but with retention eviction active, simulating
  // the actual streaming use case. Each call should decode exactly
  // one new frame, not re-decode from a keyframe.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/rt_retention", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();
  store.setRetentionBudget(topic, {2'000'000'000, 0});  // 2s

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  using Clock = std::chrono::steady_clock;
  int slow_frames = 0;
  constexpr auto kMaxFrameTime = std::chrono::milliseconds(33);

  for (size_t i = 0; i < all_packets_.size(); ++i) {
    store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);

    auto range = store.timeRange(topic);
    auto start = Clock::now();
    auto result = decoder.decodeAt(range.second);
    auto elapsed = Clock::now() - start;

    if (result.has_value() && !result->isNull()) {
      // Only count after the decoder is warmed up (past first keyframe)
      if (i > 5 && elapsed > kMaxFrameTime) {
        ++slow_frames;
      }
    }
  }

  // In live streaming, nearly all frames should be fast (single decode)
  EXPECT_LE(slow_frames, 5) << "too many slow frames during live streaming with retention";
}

TEST_F(StreamingVideoDecoderTest, ScrubAfterLiveStream) {
  // Simulates: user watches live stream, then pauses and scrubs
  // the retained buffer. Per REQUIREMENTS.md §4.3, live and scrub
  // are mutually exclusive — push stops during scrub.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/scrub_after", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();
  store.setRetentionBudget(topic, {3'000'000'000, 0});  // 3s

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Phase 1: live streaming — push all frames
  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
    auto range = store.timeRange(topic);
    decoder.decodeAt(range.second);
  }

  // Phase 2: pause — no more pushes, buffer is frozen
  size_t count = store.entryCount(topic);
  ASSERT_GT(count, 10u) << "need entries in buffer to scrub";

  // Scrub forward through entire retained buffer
  int forward_decoded = 0;
  for (size_t i = 0; i < count; ++i) {
    auto entry = store.at(topic, i);
    ASSERT_TRUE(entry.has_value());
    auto result = decoder.decodeAt(entry->timestamp);
    if (result.has_value() && !result->isNull()) {
      ++forward_decoded;
    }
  }
  EXPECT_GT(forward_decoded, static_cast<int>(count) / 2) << "should decode most frames during forward scrub";

  // Scrub backward through entire retained buffer
  int backward_decoded = 0;
  for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
    auto entry = store.at(topic, static_cast<size_t>(i));
    ASSERT_TRUE(entry.has_value());
    auto result = decoder.decodeAt(entry->timestamp);
    if (result.has_value() && !result->isNull()) {
      ++backward_decoded;
    }
  }
  EXPECT_GT(backward_decoded, static_cast<int>(count) / 2) << "should decode most frames during backward scrub";

  // Scrub to specific position (middle of buffer)
  auto mid_entry = store.at(topic, count / 2);
  ASSERT_TRUE(mid_entry.has_value());
  auto mid_result = decoder.decodeAt(mid_entry->timestamp);
  ASSERT_TRUE(mid_result.has_value()) << mid_result.error();
  EXPECT_FALSE(mid_result->isNull());
  EXPECT_EQ(mid_result->width, 640);
  EXPECT_EQ(mid_result->height, 480);
}

TEST_F(StreamingVideoDecoderTest, ScrubAfterLiveStreamToFirstEntry) {
  // Edge case: scrub to the very first entry in the retained buffer.
  // This requires the keyframe at or before that entry to still exist.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/scrub_first", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();
  store.setRetentionBudget(topic, {3'000'000'000, 0});

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Live phase
  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
    auto range = store.timeRange(topic);
    decoder.decodeAt(range.second);
  }

  // Scrub to first retained entry
  auto first_entry = store.at(topic, 0);
  ASSERT_TRUE(first_entry.has_value());

  auto result = decoder.decodeAt(first_entry->timestamp);
  // This may fail if the keyframe for this entry was evicted — that's OK
  // (it's a known limitation). But it must not crash.
  if (result.has_value()) {
    EXPECT_FALSE(result->isNull());
  }
}

TEST_F(StreamingVideoDecoderTest, ScrubAfterLiveStreamToLastEntry) {
  // Edge case: after pause, scrub to the very last entry.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/scrub_last", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();
  store.setRetentionBudget(topic, {3'000'000'000, 0});

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
    auto range = store.timeRange(topic);
    decoder.decodeAt(range.second);
  }

  // Scrub to last entry — should work since we just decoded it live
  auto range = store.timeRange(topic);
  auto result = decoder.decodeAt(range.second);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 640);
}

TEST_F(StreamingVideoDecoderTest, DecodeAtSameTimestampTwice) {
  // When display polls faster than push rate (60Hz display vs 30Hz push),
  // decodeAt is called with the same timestamp twice. This must not
  // trigger a full seek — it should return quickly.
  ObjectStore store;
  auto topic = pushPackets(store, all_packets_, all_packets_.size());

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Decode frame 10
  auto result1 = decoder.decodeAt(all_packets_[10].timestamp);
  ASSERT_TRUE(result1.has_value()) << result1.error();

  // Decode same frame again — must succeed and be fast
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  auto result2 = decoder.decodeAt(all_packets_[10].timestamp);
  auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

  ASSERT_TRUE(result2.has_value()) << result2.error();
  EXPECT_FALSE(result2->isNull());
  // Same-frame decode should be nearly instant (< 5ms), not a full seek
  EXPECT_LT(elapsed, 10.0) << "same-timestamp decode took " << elapsed << "ms — likely re-seeking";
}

TEST_F(StreamingVideoDecoderTest, ScrubAfterSteadyStateStreaming) {
  // Codex P1: after the retention window fills, entryCount() stays constant.
  // updateKeyframeIndex() must still discover new keyframes, or scrub
  // into the retained buffer fails with "no keyframe yet".
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/steady_state", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  // Small retention: 1 second (~30 frames at 30fps)
  store.setRetentionBudget(topic, {1'000'000'000, 0});

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // Phase 1: live stream all packets (retention will reach steady state)
  for (const auto& pkt : all_packets_) {
    store.pushOwned(topic, pkt.timestamp, pkt.data);
    auto range = store.timeRange(topic);
    decoder.decodeAt(range.second);
  }

  // Phase 2: pause and scrub. The retained buffer should contain ~30 frames.
  // Scrubbing to any entry must work — keyframe index must be up to date.
  size_t count = store.entryCount(topic);
  ASSERT_GT(count, 5u);

  // Scrub to middle of retained buffer
  auto mid_entry = store.at(topic, count / 2);
  ASSERT_TRUE(mid_entry.has_value());
  auto result = decoder.decodeAt(mid_entry->timestamp);
  EXPECT_TRUE(result.has_value()) << "scrub after steady-state should work: " << result.error();

  // Scrub to first entry
  auto first_entry = store.at(topic, 0);
  ASSERT_TRUE(first_entry.has_value());
  auto first_result = decoder.decodeAt(first_entry->timestamp);
  // May fail if keyframe was evicted, but must not fail with "no keyframe yet"
  if (!first_result.has_value()) {
    EXPECT_NE(first_result.error(), "no keyframe yet")
        << "keyframe index was not updated during steady-state streaming";
  }
}

TEST_F(StreamingVideoDecoderTest, BenchmarkLiveWith60HzDisplay) {
  // Simulates real demo: push at 30Hz, display polls at 60Hz.
  // Every other display call hits the same timestamp (no new frame).
  // This must NOT cause a re-seek — total time should be close to
  // the push-only benchmark.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/bench60", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  using Clock = std::chrono::steady_clock;
  int slow_calls = 0;
  int total_calls = 0;
  constexpr auto kMaxCallTime = std::chrono::milliseconds(33);
  size_t pkt_idx = 0;
  bool push_tick = true;  // Alternates: push+decode, then decode-only

  while (pkt_idx < all_packets_.size()) {
    if (push_tick && pkt_idx < all_packets_.size()) {
      store.pushOwned(topic, all_packets_[pkt_idx].timestamp, all_packets_[pkt_idx].data);
      ++pkt_idx;
    }
    push_tick = !push_tick;

    auto range = store.timeRange(topic);
    auto start = Clock::now();
    decoder.decodeAt(range.second);
    auto elapsed = Clock::now() - start;

    ++total_calls;
    if (elapsed > kMaxCallTime) {
      ++slow_calls;
    }
  }

  fprintf(stderr, "[Benchmark 60Hz] %d calls, %d slow (>33ms)\n", total_calls, slow_calls);
  // Very few should be slow — same-timestamp calls should be fast
  EXPECT_LE(slow_calls, 3) << "too many slow calls — same-timestamp may be re-seeking";
}

TEST_F(StreamingVideoDecoderTest, BenchmarkLiveDecodePerFrame) {
  // Benchmark: push frames one-by-one (simulating live), decode each.
  // Measures per-frame decode time to identify real-time bottlenecks.
  // At 30fps, each frame must take < 33ms for real-time playback.
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/bench", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  using Clock = std::chrono::steady_clock;
  std::vector<double> frame_times_ms;
  frame_times_ms.reserve(all_packets_.size());

  for (size_t i = 0; i < all_packets_.size(); ++i) {
    store.pushOwned(topic, all_packets_[i].timestamp, all_packets_[i].data);

    auto range = store.timeRange(topic);
    auto start = Clock::now();
    auto result = decoder.decodeAt(range.second);
    auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    if (result.has_value() && !result->isNull()) {
      frame_times_ms.push_back(elapsed);
    }
  }

  ASSERT_GT(frame_times_ms.size(), 10u);

  // Compute statistics
  std::sort(frame_times_ms.begin(), frame_times_ms.end());
  double min_ms = frame_times_ms.front();
  double max_ms = frame_times_ms.back();
  double sum = 0;
  for (double t : frame_times_ms) {
    sum += t;
  }
  double avg_ms = sum / static_cast<double>(frame_times_ms.size());
  double p95_ms = frame_times_ms[frame_times_ms.size() * 95 / 100];
  double p99_ms = frame_times_ms[frame_times_ms.size() * 99 / 100];

  int over_budget = 0;
  for (double t : frame_times_ms) {
    if (t > 33.0) {
      ++over_budget;
    }
  }

  fprintf(
      stderr, "[Benchmark] %zu frames: min=%.1fms avg=%.1fms p95=%.1fms p99=%.1fms max=%.1fms | %d over 33ms budget\n",
      frame_times_ms.size(), min_ms, avg_ms, p95_ms, p99_ms, max_ms, over_budget);

  // At most 5% of frames should exceed the 33ms budget (480p should be fast)
  EXPECT_LT(over_budget, static_cast<int>(frame_times_ms.size()) / 20)
      << "too many frames exceed 33ms real-time budget";
}

// ---------------------------------------------------------------------------
// B-frame tests: push using DTS (decode order), verify decode works
// ---------------------------------------------------------------------------

const std::string kBframeVideo = "pj_scene2D/testdata/test_1080p_bframes.mp4";

TEST(StreamingVideoDecoderBframeTest, PushWithDtsAndDecode) {
  if (!std::filesystem::exists(kBframeVideo)) {
    GTEST_SKIP() << "UNTRACKED FIXTURE test_1080p_bframes.mp4 — not in the repository, so this suite does NOT run on a "
                    "clean clone. Generate it per pj_scene2D/testdata/README.md.";
  }
  auto packets = test::extractAnnexBPackets(kBframeVideo);
  ASSERT_GT(packets.size(), 10u);

  // Verify this video actually has non-monotonic PTS (B-frames)
  bool has_non_monotonic_pts = false;
  for (size_t i = 1; i < packets.size(); ++i) {
    if (packets[i].timestamp < packets[i - 1].timestamp) {
      has_non_monotonic_pts = true;
      break;
    }
  }
  ASSERT_TRUE(has_non_monotonic_pts) << "test video should have B-frames with non-monotonic PTS";

  // Verify DTS IS monotonic
  for (size_t i = 1; i < packets.size(); ++i) {
    ASSERT_GE(packets[i].dts, packets[i - 1].dts) << "DTS should be monotonic at i=" << i;
  }

  // Push using DTS as ObjectStore timestamp
  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/bframes", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  for (const auto& pkt : packets) {
    auto result = store.pushOwned(topic, pkt.dts, pkt.data);
    ASSERT_TRUE(result.has_value()) << "push failed at dts=" << pkt.dts << ": " << result.error();
  }

  EXPECT_EQ(store.entryCount(topic), packets.size());

  // Decode via StreamingVideoDecoder
  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  auto range = store.timeRange(topic);
  auto result = decoder.decodeAt(range.second);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 1920);
  EXPECT_EQ(result->height, 1080);
}

TEST(StreamingVideoDecoderBframeTest, LiveStreamWithDts) {
  if (!std::filesystem::exists(kBframeVideo)) {
    GTEST_SKIP() << "UNTRACKED FIXTURE test_1080p_bframes.mp4 — not in the repository, so this suite does NOT run on a "
                    "clean clone. Generate it per pj_scene2D/testdata/README.md.";
  }
  auto packets = test::extractAnnexBPackets(kBframeVideo);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/bframes_live", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  int decoded_count = 0;
  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);
    auto range = store.timeRange(topic);
    auto result = decoder.decodeAt(range.second);
    if (result.has_value() && !result->isNull()) {
      ++decoded_count;
    }
  }

  EXPECT_GT(decoded_count, static_cast<int>(packets.size()) / 2) << "should decode most B-frame packets";
}

TEST(StreamingVideoDecoderBframeTest, BenchmarkLiveDecodeBframes) {
  if (!std::filesystem::exists(kBframeVideo)) {
    GTEST_SKIP() << "UNTRACKED FIXTURE test_1080p_bframes.mp4 — not in the repository, so this suite does NOT run on a "
                    "clean clone. Generate it per pj_scene2D/testdata/README.md.";
  }
  auto packets = test::extractAnnexBPackets(kBframeVideo);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic_result = store.registerTopic({0, "video/bframes_bench", R"({"media_class":"video","encoding":"h264"})"});
  auto topic = topic_result.value();

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  using Clock = std::chrono::steady_clock;
  std::vector<double> frame_times_ms;
  frame_times_ms.reserve(packets.size());

  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);

    auto range = store.timeRange(topic);
    auto start = Clock::now();
    auto result = decoder.decodeAt(range.second);
    auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    if (result.has_value() && !result->isNull()) {
      frame_times_ms.push_back(elapsed);
    }
  }

  ASSERT_GT(frame_times_ms.size(), 10u);

  std::sort(frame_times_ms.begin(), frame_times_ms.end());
  double sum = 0;
  for (double t : frame_times_ms) {
    sum += t;
  }
  double avg_ms = sum / static_cast<double>(frame_times_ms.size());
  double p95_ms = frame_times_ms[frame_times_ms.size() * 95 / 100];
  double p99_ms = frame_times_ms[frame_times_ms.size() * 99 / 100];
  double max_ms = frame_times_ms.back();

  int over_budget = 0;
  for (double t : frame_times_ms) {
    if (t > 33.0) {
      ++over_budget;
    }
  }

  fprintf(
      stderr, "[Benchmark B-frames 1080p] %zu frames: avg=%.1fms p95=%.1fms p99=%.1fms max=%.1fms | %d over 33ms\n",
      frame_times_ms.size(), avg_ms, p95_ms, p99_ms, max_ms, over_budget);

  EXPECT_LT(over_budget, static_cast<int>(frame_times_ms.size()) / 10)
      << "too many 1080p B-frame frames exceed 33ms budget";
}

TEST(StreamingVideoDecoderBframeTest, SimulateDemoDualTimer) {
  // Exactly simulates the demo: push at 30Hz, display polls at 60Hz.
  // Measures pushes until first displayed frame.
  if (!std::filesystem::exists(kBframeVideo)) {
    GTEST_SKIP() << "UNTRACKED FIXTURE test_1080p_bframes.mp4 — not in the repository, so this suite does NOT run on a "
                    "clean clone. Generate it per pj_scene2D/testdata/README.md.";
  }
  auto packets = test::extractAnnexBPackets(kBframeVideo);
  ASSERT_GT(packets.size(), 100u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/demo_sim", R"({"media_class":"video","encoding":"h264"})"});
  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  size_t push_idx = 0;
  int push_count = 0;
  int display_count = 0;
  bool first_frame_found = false;

  // Simulate interleaved timers: push every 33ms, display every 16ms
  // In real time: push at ticks 0,33,66,99,... display at 0,16,32,48,64,...
  // Simulate 200 ticks at 1ms resolution
  for (int tick_ms = 0; tick_ms < 6000 && !first_frame_found; ++tick_ms) {
    // Push tick: every 33ms
    if (tick_ms % 33 == 0 && push_idx < packets.size()) {
      store.pushOwned(topic, packets[push_idx].dts, packets[push_idx].data);
      ++push_idx;
      ++push_count;
    }

    // Display tick: every 16ms
    if (tick_ms % 16 == 0) {
      auto count = store.entryCount(topic);
      if (count == 0) {
        continue;
      }
      auto range = store.timeRange(topic);
      auto result = decoder.decodeAt(range.second);
      ++display_count;

      if (result.has_value() && !result->isNull()) {
        fprintf(
            stderr, "[DemoDualTimer] First frame at tick=%dms, push_count=%d, display_count=%d\n", tick_ms, push_count,
            display_count);
        first_frame_found = true;
      }
    }
  }

  ASSERT_TRUE(first_frame_found) << "no frame after 6 seconds of simulation";
  // With B-frame reorder depth ~30, expect first frame at ~30-40 pushes (~1s)
  EXPECT_LT(push_count, 100) << "first frame took too many pushes — possible O(n^2) regression";
}

// ---------------------------------------------------------------------------
// HEVC (H.265): end-to-end exercise of the codec-generic decode path on a real
// stream. Fixture is a tiny libx265 clip (pj_scene2D/testdata/test_hevc.mp4),
// regenerable with an ffmpeg that has libx265 (PJ4's FFmpeg is decoder-only):
//   ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=2 -c:v libx265
//   -x265-params keyint=15:min-keyint=15:no-scenecut=1 -pix_fmt yuv420p
//   -tag:v hvc1 -an pj_scene2D/testdata/test_hevc.mp4
// ---------------------------------------------------------------------------

const std::string kHevcVideo = "pj_scene2D/testdata/test_hevc.mp4";

// Raw-bytes entries carry no VideoFrame.format, so report the codec via a custom
// extractor (mirrors the parser-mode extractor, which reads sdk::VideoFrame::format).
StreamingVideoDecoder::NalExtractor formatExtractor(std::string format) {
  return [format =
              std::move(format)](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    // These fixtures are pushed by PTS-as-store-key (no B-frames), so PTS == the
    // entry's store key here.
    return StreamingVideoDecoder::ExtractedFrame{entry.payload.bytes, format, entry.timestamp};
  };
}

TEST(StreamingVideoDecoderHevcTest, DecodeAndKeyframeOracle) {
  if (!std::filesystem::exists(kHevcVideo)) {
    GTEST_SKIP() << "test_hevc.mp4 not found";
  }
  ASSERT_EQ(test::mp4VideoFormat(kHevcVideo), "h265") << "fixture must be HEVC";
  auto packets = test::extractAnnexBPackets(kHevcVideo);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/hevc", R"({"media_class":"video","encoding":"h265"})"});
  for (const auto& pkt : packets) {
    ASSERT_TRUE(store.pushOwned(topic, pkt.dts, pkt.data).has_value());
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("h265"));

  // The HEVC IRAP keyframe oracle must find the fixture's keyframes (keyint=15
  // over 60 frames -> ~4). A regressed oracle would find 0 and seeking would
  // fail with "no keyframe yet".
  EXPECT_GT(decoder.keyframeTimestamps().size(), 1u) << "HEVC keyframe oracle found no IRAP frames";

  auto range = store.timeRange(topic);
  auto result = decoder.decodeAt(range.second);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 320);
  EXPECT_EQ(result->height, 240);
  // SW decode -> planar YUV420P; HW decode (VAAPI) -> native NV12. Accept both.
  EXPECT_TRUE(result->format == PixelFormat::kYUV420P || result->format == PixelFormat::kNV12)
      << "format=" << static_cast<int>(result->format);
}

TEST(StreamingVideoDecoderHevcTest, ScrubBackwardSeeksToIrap) {
  if (!std::filesystem::exists(kHevcVideo)) {
    GTEST_SKIP() << "test_hevc.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kHevcVideo);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/hevc_scrub", R"({"media_class":"video","encoding":"h265"})"});
  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("h265"));

  // Decode a late frame, then scrub back — forces a seek to a preceding IRAP.
  // The fixture's first frames have negative DTS (encoder-delay convention), so
  // this also guards the negative-timestamp keyframe-seek path.
  auto later = decoder.decodeAt(packets[std::min<size_t>(50, packets.size() - 1)].dts);
  ASSERT_TRUE(later.has_value()) << later.error();
  auto earlier = decoder.decodeAt(packets[5].dts);
  ASSERT_TRUE(earlier.has_value()) << earlier.error();
  EXPECT_FALSE(earlier->isNull());
  EXPECT_EQ(earlier->width, 320);
}

TEST(StreamingVideoDecoderHevcTest, ScrubReturnsExactlyRequestedFrame) {
  if (!std::filesystem::exists(kHevcVideo)) {
    GTEST_SKIP() << "test_hevc.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kHevcVideo);
  ASSERT_GT(packets.size(), 40u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/hevc_pts", R"({"media_class":"video","encoding":"h265"})"});
  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("h265"));

  // Scrub onto mid-buffer targets: the decoded frame must be EXACTLY the requested
  // one. FFmpeg passes our packet pts straight through to frame->pts, so the frame
  // produced by the target entry carries pts == that entry's timestamp. A decoder
  // that returns "the first frame off the target packet" hands back an earlier
  // frame — FFmpeg's reorder + frame-threading delay buffers the target. This
  // exercises the seek path (20, then a backward jump to 25) and the forward
  // fast-path (35, 45). Mixed order on purpose.
  for (size_t idx : std::initializer_list<size_t>{20, 35, 45, 25}) {
    const Timestamp target = packets[idx].dts;
    auto result = decoder.decodeAt(target);
    ASSERT_TRUE(result.has_value()) << "idx=" << idx << ": " << result.error();
    ASSERT_FALSE(result->isNull()) << "idx=" << idx;
    EXPECT_EQ(result->pts, target) << "idx=" << idx << " requested ts=" << target
                                   << " but got a frame from pts=" << result->pts << " (decoder-delay slip)";
  }
}

TEST(StreamingVideoDecoderHevcTest, ForwardPlaybackReturnsEachExactFrame) {
  if (!std::filesystem::exists(kHevcVideo)) {
    GTEST_SKIP() << "test_hevc.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kHevcVideo);
  ASSERT_GT(packets.size(), 40u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/hevc_seq", R"({"media_class":"video","encoding":"h265"})"});
  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("h265"));

  // Sequential forward playback across a contiguous mid-buffer run: every frame
  // must be exactly the requested one (not a few frames behind), and the forward
  // continuation must keep working step after step without re-seeking.
  for (size_t idx = 10; idx <= 30; ++idx) {
    const Timestamp target = packets[idx].dts;
    auto result = decoder.decodeAt(target);
    ASSERT_TRUE(result.has_value()) << "idx=" << idx << ": " << result.error();
    ASSERT_FALSE(result->isNull()) << "idx=" << idx;
    EXPECT_EQ(result->pts, target) << "idx=" << idx;
  }
}

TEST(StreamingVideoDecoderHevcTest, UnsupportedCodecSurfacesError) {
  // VP9 has a decoder in this FFmpeg but no keyframe oracle, so it is not in the
  // supported set: it must fail with a clear error rather than decode without
  // seek support (or mis-decode as H.264). Fixture-free.
  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/badcodec", R"({"media_class":"video","encoding":"vp9"})"});
  const std::vector<uint8_t> dummy = {0x00, 0x00, 0x00, 0x01, 0x42, 0x01};
  for (int i = 0; i < 5; ++i) {
    store.pushOwned(topic, static_cast<Timestamp>(i) * 1'000'000, dummy);
  }
  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("vp9"));
  auto result = decoder.decodeAt(store.timeRange(topic).second);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("unsupported video codec"), std::string::npos) << "got: " << result.error();
}

// ---------------------------------------------------------------------------
// AV1: end-to-end exercise on a real stream (OBU temporal units, not Annex-B).
// Fixture is a tiny libaom-av1 clip (pj_scene2D/testdata/test_av1.mp4):
//   ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=2 -c:v libaom-av1
//   -usage realtime -cpu-used 8 -g 15 -keyint_min 15 -pix_fmt yuv420p
//   -tag:v av01 -an pj_scene2D/testdata/test_av1.mp4
// The test demuxer re-prepends the av1C sequence header to keyframes so each is
// a self-contained temporal unit (the LOBF wire form the decoder expects).
// ---------------------------------------------------------------------------

const std::string kAv1Video = "pj_scene2D/testdata/test_av1.mp4";

TEST(StreamingVideoDecoderAv1Test, DecodeAndKeyframeOracle) {
  if (!std::filesystem::exists(kAv1Video)) {
    GTEST_SKIP() << "test_av1.mp4 not found";
  }
  ASSERT_EQ(test::mp4VideoFormat(kAv1Video), "av1") << "fixture must be AV1";
  auto packets = test::extractAnnexBPackets(kAv1Video);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/av1", R"({"media_class":"video","encoding":"av1"})"});
  for (const auto& pkt : packets) {
    ASSERT_TRUE(store.pushOwned(topic, pkt.dts, pkt.data).has_value());
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("av1"));

  // The AV1 sequence-header-OBU oracle must find the fixture's keyframes.
  EXPECT_GT(decoder.keyframeTimestamps().size(), 1u) << "AV1 keyframe oracle found no sequence-header OBUs";

  auto range = store.timeRange(topic);
  auto result = decoder.decodeAt(range.second);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_FALSE(result->isNull());
  EXPECT_EQ(result->width, 320);
  EXPECT_EQ(result->height, 240);
  // SW decode -> planar YUV420P; HW decode (VAAPI) -> native NV12. Accept both.
  EXPECT_TRUE(result->format == PixelFormat::kYUV420P || result->format == PixelFormat::kNV12)
      << "format=" << static_cast<int>(result->format);
}

TEST(StreamingVideoDecoderAv1Test, ScrubBackwardSeeksToKeyframe) {
  if (!std::filesystem::exists(kAv1Video)) {
    GTEST_SKIP() << "test_av1.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kAv1Video);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/av1_scrub", R"({"media_class":"video","encoding":"av1"})"});
  for (const auto& pkt : packets) {
    store.pushOwned(topic, pkt.dts, pkt.data);
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("av1"));

  auto later = decoder.decodeAt(packets[std::min<size_t>(50, packets.size() - 1)].dts);
  ASSERT_TRUE(later.has_value()) << later.error();
  auto earlier = decoder.decodeAt(packets[5].dts);
  ASSERT_TRUE(earlier.has_value()) << earlier.error();
  EXPECT_FALSE(earlier->isNull());
  EXPECT_EQ(earlier->width, 320);
}

// Direct, fixture-free regression for the negative-DTS keyframe-seek fix. FFmpeg
// gives the first frames negative DTS (encoder-delay convention), so a keyframe
// can sit at a negative ObjectStore timestamp. findKeyframeBefore must return it
// (std::optional), not treat the negative value as the old "-1 = not found"
// sentinel. Synthetic Annex-B: an IDR NAL the H.264 oracle detects, at a negative
// timestamp; the bytes do not decode, but the keyframe lookup must still succeed.
TEST(StreamingVideoDecoderNegativeDtsTest, KeyframeAtNegativeTimestampIsFound) {
  const std::vector<uint8_t> idr = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00};      // NAL type 5 (IDR)
  const std::vector<uint8_t> non_idr = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x00, 0x00};  // NAL type 1

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/neg_dts", R"({"media_class":"video","encoding":"h264"})"});
  ASSERT_TRUE(store.pushOwned(topic, -2'000'000, idr).has_value());  // keyframe at NEGATIVE timestamp
  store.pushOwned(topic, -1'000'000, non_idr);
  store.pushOwned(topic, 0, non_idr);
  store.pushOwned(topic, 1'000'000, non_idr);

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic);

  // The keyframe at the negative timestamp must be indexed.
  auto kfs = decoder.keyframeTimestamps();
  ASSERT_FALSE(kfs.empty());
  EXPECT_LT(kfs.front(), 0) << "keyframe at negative DTS should be indexed";

  // Seeking to a later frame must FIND that negative-timestamp keyframe, not
  // reject it as "no keyframe before target" (the pre-fix -1-sentinel bug). The
  // decode itself fails (synthetic bytes are not a real stream), but the failure
  // must be a decode failure, not the keyframe lookup.
  auto result = decoder.decodeAt(0);
  if (!result.has_value()) {
    EXPECT_NE(result.error(), "no keyframe before target")
        << "negative-timestamp keyframe was wrongly rejected (the -1 sentinel bug)";
  }
}

// ---------------------------------------------------------------------------
// decodeSampled(): thumbnail-builder forward pass. Verifies the sampling cadence
// and that the optimization (materialize pixels only for surfaced frames) holds.
//
// Default fixture is kTestVideo (skips in CI when absent). Point PJ_SAMPLED_BENCH
// at a larger H.264 clip — ideally 4K — to get a representative speedup number:
//   PJ_SAMPLED_BENCH=clip_3840x2160.mp4 ./streaming_video_decoder_test --gtest_filter='*Sampled*'
// ---------------------------------------------------------------------------
std::string sampledBenchVideo() {
  // sdk::getEnv is the portable wrapper; a raw std::getenv trips MSVC C4996
  // (deprecation) which the Windows CI build treats as an error under /WX.
  if (const auto env = sdk::getEnv("PJ_SAMPLED_BENCH"); env && !env->empty()) {
    return *env;
  }
  return kTestVideo;
}

TEST(StreamingVideoSampledTest, MaterializesOnlySurfacedFrames) {
  const std::string path = sampledBenchVideo();
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << path << " not found (set PJ_SAMPLED_BENCH to a local H.264 clip)";
  }
  auto packets = test::extractAnnexBPackets(path);
  ASSERT_GT(packets.size(), 30u);

  // extractAnnexBPackets yields packets in decode (file) order. Push them in that
  // order under synthetic monotonic keys spaced by the clip's average frame
  // duration, so the store hands the decoder decode order even for B-frame clips
  // (real thumbnail builds key by DTS, which is decode order). Only the timestamp
  // labels are synthetic — decode work, pixels, and timings are unaffected, and
  // the ~1 s sampling interval still maps to roughly one frame per real second.
  const size_t cap = std::min<size_t>(packets.size(), 1200);
  int64_t min_pts = packets.front().timestamp;
  int64_t max_pts = packets.front().timestamp;
  for (size_t i = 0; i < cap; ++i) {
    min_pts = std::min(min_pts, packets[i].timestamp);
    max_pts = std::max(max_pts, packets[i].timestamp);
  }
  const int64_t step = std::max<int64_t>(1, (max_pts - min_pts) / static_cast<int64_t>(std::max<size_t>(1, cap - 1)));

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/test", R"({"media_class":"video","encoding":"h264"})"});
  size_t keyframes = 0;
  for (size_t i = 0; i < cap; ++i) {
    if (packets[i].keyframe) {
      ++keyframes;
    }
    auto bytes = packets[i].data;  // pushOwned takes ownership
    ASSERT_TRUE(store.pushOwned(topic, static_cast<Timestamp>(i) * step, std::move(bytes)).has_value());
  }

  // Run decodeSampled at a given interval; collect surfaced PTS + wall-clock.
  auto run = [&](Timestamp interval) {
    StreamingVideoDecoder decoder;
    decoder.attach(&store, topic);
    std::vector<int64_t> surfaced;
    const auto t0 = std::chrono::steady_clock::now();
    decoder.decodeSampled(interval, [&](const DecodedFrame& frame) -> bool {
      surfaced.push_back(frame.pts);
      return true;
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return std::make_pair(std::move(surfaced), ms);
  };

  // Which strategy does the 1/s cadence take on this clip? (keyframe-only when the
  // source keyframes are already >= 1/s, else the forward materialize-gated pass.)
  StreamingVideoDecoder probe;
  probe.attach(&store, topic);
  const bool keyframe_only = probe.sampledUsesKeyframeOnly(1'000'000'000);

  // interval = 1 ns surfaces (materializes) ~every frame — the OLD decodeSampled
  // cost. interval = 1 s is the optimized cadence.
  auto [pts_full, ms_full] = run(1);
  auto [pts_samp, ms_samp] = run(1'000'000'000);

  ASSERT_FALSE(pts_samp.empty());
  ASSERT_FALSE(pts_full.empty());
  // The full pass must surface ~every frame (proves decode actually succeeded in
  // decode order, not that we silently produced nothing).
  EXPECT_GT(pts_full.size(), cap / 2) << "full pass should decode most frames";
  EXPECT_LT(pts_samp.size(), pts_full.size()) << "sampled cadence must surface far fewer frames";
  // Decoder emits in display order, so surfaced PTS are ascending in both runs.
  for (size_t i = 1; i < pts_samp.size(); ++i) {
    EXPECT_GT(pts_samp[i], pts_samp[i - 1]) << "sampled PTS must be strictly ascending";
  }
  for (size_t i = 1; i < pts_full.size(); ++i) {
    EXPECT_GE(pts_full[i], pts_full[i - 1]) << "full-pass PTS must be non-descending";
  }

  std::fprintf(
      stderr,
      "[sampled-bench] %s  frames=%zu  keyframes=%zu  1/s strategy=%s\n"
      "  full-materialize : %zu surfaced, %8.1f ms (%.2f ms/frame)\n"
      "  sampled (1/s)    : %zu surfaced, %8.1f ms\n"
      "  -> wall-clock %.1fx faster\n",
      path.c_str(), pts_full.size(), keyframes, keyframe_only ? "keyframe-only" : "forward", pts_full.size(), ms_full,
      ms_full / static_cast<double>(std::max<size_t>(1, pts_full.size())), pts_samp.size(), ms_samp,
      ms_full / std::max(1e-6, ms_samp));
}

// Keyframe-dense source (the committed AV1 fixture is keyint=15 @ 30fps ≈ a
// keyframe every 0.5 s): a 1 s cadence is already covered by keyframes, so
// decodeSampled takes the keyframe-only fast path and still surfaces a valid,
// ascending ~1/interval set. Runs in CI (fixture committed).
TEST(StreamingVideoSampledTest, KeyframeDenseSourceUsesKeyframeOnly) {
  if (!std::filesystem::exists(kAv1Video)) {
    GTEST_SKIP() << "test_av1.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kAv1Video);
  ASSERT_GT(packets.size(), 30u);

  ObjectStore store;
  auto topic = *store.registerTopic({0, "video/av1", R"({"media_class":"video","encoding":"av1"})"});
  for (const auto& pkt : packets) {
    ASSERT_TRUE(store.pushOwned(topic, pkt.dts, pkt.data).has_value());
  }

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, formatExtractor("av1"));

  // Dense keyframes cover a 1 s cadence -> keyframe-only; a 1 ns cadence is far
  // finer than any keyframe spacing -> it cannot, so the forward pass is chosen.
  EXPECT_TRUE(decoder.sampledUsesKeyframeOnly(1'000'000'000));
  EXPECT_FALSE(decoder.sampledUsesKeyframeOnly(1));

  std::vector<int64_t> surfaced;
  decoder.decodeSampled(1'000'000'000, [&](const DecodedFrame& frame) -> bool {
    EXPECT_FALSE(frame.isNull());
    // SW decode -> YUV420P; HW decode (VAAPI) -> NV12. Both reach the thumbnail
    // encoder, which deinterleaves NV12; accept either here.
    EXPECT_TRUE(frame.format == PixelFormat::kYUV420P || frame.format == PixelFormat::kNV12);
    surfaced.push_back(frame.pts);
    return true;
  });
  ASSERT_FALSE(surfaced.empty()) << "keyframe-only path surfaced nothing";
  for (size_t i = 1; i < surfaced.size(); ++i) {
    EXPECT_GT(surfaced[i], surfaced[i - 1]) << "surfaced PTS must be strictly ascending";
  }
}

// ---------------------------------------------------------------------------
// B-frame presentation order. With B-frames decode order (DTS) != presentation
// order (PTS). The producer keys the store by DTS; the decoder must SERVE by PTS
// so playback is in presentation order. Regression for the "vibration/cracks +
// high CPU" bug, where frames played back in decode order.
// ---------------------------------------------------------------------------

const std::string kH264BframeVideo = "pj_scene2D/testdata/test_h264_bframes.mp4";

TEST(StreamingVideoDecoderBframeOrderTest, SequentialPlaybackIsPresentationOrdered) {
  if (!std::filesystem::exists(kH264BframeVideo)) {
    GTEST_SKIP() << "test_h264_bframes.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kH264BframeVideo);
  ASSERT_GT(packets.size(), 10u);

  // Precondition: the fixture really has B-frames (PTS non-monotonic in decode order).
  bool has_bframes = false;
  for (size_t i = 1; i < packets.size(); ++i) {
    if (packets[i].timestamp < packets[i - 1].timestamp) {
      has_bframes = true;
      break;
    }
  }
  ASSERT_TRUE(has_bframes) << "fixture must have B-frames (non-monotonic PTS)";

  // Push keyed by DTS (decode order) — exactly what the lazy-VideoFrame producer does.
  ObjectStore store;
  auto topic = store.registerTopic({0, "video/bframe_order", R"({"media_class":"video","encoding":"h264"})"}).value();
  auto dts_to_pts = std::make_shared<std::map<Timestamp, Timestamp>>();
  for (const auto& p : packets) {
    ASSERT_TRUE(store.pushOwned(topic, p.dts, p.data).has_value());
    (*dts_to_pts)[p.dts] = p.timestamp;
  }

  // Parser-like extractor: supplies the real PTS per entry (as production does via
  // VideoFrame.timestamp_ns), keyed off the entry's DTS store key.
  StreamingVideoDecoder::NalExtractor extractor =
      [dts_to_pts](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    auto it = dts_to_pts->find(entry.timestamp);
    Timestamp pts = (it != dts_to_pts->end()) ? it->second : entry.timestamp;
    return StreamingVideoDecoder::ExtractedFrame{entry.payload.bytes, "h264", pts};
  };

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, extractor);

  // Play forward through every presentation timestamp (PTS ascending) and assert
  // each request returns the EXACT frame for that presentation time, strictly
  // ascending — i.e. presentation order, not decode order. Before the fix the
  // decoder served by DTS, so frame->pts would not match the requested PTS.
  std::vector<Timestamp> pts_sorted;
  pts_sorted.reserve(packets.size());
  for (const auto& p : packets) {
    pts_sorted.push_back(p.timestamp);
  }
  std::sort(pts_sorted.begin(), pts_sorted.end());

  // Edge: a request at the timeline start (min DTS) lands before the first PTS with
  // B-frames (IDR encoder-delay). It must still return the first presentable frame.
  {
    auto first = decoder.decodeAt(store.timeRange(topic).first);
    ASSERT_TRUE(first.has_value()) << "decodeAt(stream start) failed: " << first.error();
    ASSERT_FALSE(first->isNull());
  }

  std::optional<int64_t> prev;
  int served = 0;
  for (Timestamp want_pts : pts_sorted) {
    auto frame = decoder.decodeAt(want_pts);
    ASSERT_TRUE(frame.has_value()) << "decodeAt(" << want_pts << ") failed: " << frame.error();
    ASSERT_FALSE(frame->isNull());
    EXPECT_EQ(frame->width, 128);
    EXPECT_EQ(frame->height, 128);
    EXPECT_EQ(frame->pts, want_pts) << "served wrong frame — decode-order leak";
    if (prev.has_value()) {
      EXPECT_GT(frame->pts, *prev) << "presentation PTS must strictly increase";
    }
    prev = frame->pts;
    ++served;
  }
  EXPECT_EQ(served, static_cast<int>(pts_sorted.size()));
}

// Deeper-reorder fixture (3 B-frames @60fps — the shape of real screencasts,
// where the lead-in clamp + forward continuation can back up the codec queue):
//   gst-launch-1.0 videotestsrc num-buffers=120 pattern=ball !
//     video/x-raw,width=128,height=128,framerate=60/1 !
//     x264enc bframes=3 b-adapt=false key-int-max=30 speed-preset=veryfast
//     bitrate=64 ! h264parse ! mp4mux !
//     filesink location=pj_scene2D/testdata/test_h264_bframes_deep.mp4
const std::string kH264BframeDeepVideo = "pj_scene2D/testdata/test_h264_bframes_deep.mp4";

TEST(StreamingVideoDecoderBframeOrderTest, TimelineSweepByStoreKeysLosesNoFrame) {
  if (!std::filesystem::exists(kH264BframeDeepVideo)) {
    GTEST_SKIP() << "test_h264_bframes_deep.mp4 not found";
  }
  auto packets = test::extractAnnexBPackets(kH264BframeDeepVideo);
  ASSERT_GT(packets.size(), 10u);

  ObjectStore store;
  auto topic = store.registerTopic({0, "video/bframe_sweep", R"({"media_class":"video","encoding":"h264"})"}).value();
  auto dts_to_pts = std::make_shared<std::map<Timestamp, Timestamp>>();
  for (const auto& p : packets) {
    ASSERT_TRUE(store.pushOwned(topic, p.dts, p.data).has_value());
    (*dts_to_pts)[p.dts] = p.timestamp;
  }
  StreamingVideoDecoder::NalExtractor extractor =
      [dts_to_pts](const ResolvedObjectEntry& entry) -> Expected<StreamingVideoDecoder::ExtractedFrame> {
    auto it = dts_to_pts->find(entry.timestamp);
    Timestamp pts = (it != dts_to_pts->end()) ? it->second : entry.timestamp;
    return StreamingVideoDecoder::ExtractedFrame{entry.payload.bytes, "h264", pts};
  };

  StreamingVideoDecoder decoder;
  decoder.attach(&store, topic, extractor);

  // Sweep the timeline the way the PLAYBACK CLOCK does: by STORE keys (DTS),
  // from the very first entry. The encoder-delay lead-in (several DTS keys all
  // clamping to the first presented frame) lets the codec's output queue back
  // up; the next send can then hit EAGAIN, whose legacy recovery silently
  // DISCARDS the queued frame — the very frame the following request needs.
  // Symptom: exactly one "forward decode produced no frame" hiccup early in
  // playback, then recovery. Every request must produce a frame.
  for (size_t i = 0; i < packets.size(); ++i) {
    const Timestamp request_ts = packets[i].dts;
    auto frame = decoder.decodeAt(request_ts);
    ASSERT_TRUE(frame.has_value()) << "i=" << i << " decodeAt(" << request_ts << ") failed: " << frame.error();
    ASSERT_FALSE(frame->isNull()) << "i=" << i;
  }
}

}  // namespace
}  // namespace PJ
