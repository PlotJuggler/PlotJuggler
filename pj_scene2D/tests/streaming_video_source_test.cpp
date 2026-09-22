// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_source.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/builtin/video_frame.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "test_mp4_demux.h"

namespace PJ {
namespace {

const std::string kTestVideo = "pj_scene2D/testdata/test_480p.mp4";

/// Push all video packets from an MP4 file into ObjectStore as decoder-ready units.
size_t pushVideoPackets(const std::string& path, ObjectStore& store, ObjectTopicId topic) {
  const auto packets = test::extractAnnexBPackets(path);
  for (const auto& packet : packets) {
    store.pushOwned(topic, packet.dts, packet.data);
  }
  return packets.size();
}

class StreamingVideoSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kTestVideo)) {
      GTEST_SKIP() << "UNTRACKED FIXTURE test_480p.mp4 — not in the repository, so this suite does NOT run on a clean "
                      "clone. Generate it per pj_scene2D/testdata/README.md.";
    }
  }
};

TEST_F(StreamingVideoSourceTest, DecodeAtTimestamp) {
  ObjectStore store;
  auto topic_or = store.registerTopic({.dataset_id = 1, .topic_name = "video", .metadata_json = "{}"});
  ASSERT_TRUE(topic_or.has_value());
  auto topic = *topic_or;

  size_t packet_count = pushVideoPackets(kTestVideo, store, topic);
  ASSERT_GT(packet_count, 0u);

  auto [t_min, t_max] = store.timeRange(topic);

  StreamingVideoSource source(&store, topic);

  // Seek to the middle of the video
  int64_t mid_ts = (t_min + t_max) / 2;
  source.setTimestamp(mid_ts);

  // Poll for the decoded frame
  std::optional<MediaFrame> frame;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!frame.has_value() || !frame->base.has_value()) && std::chrono::steady_clock::now() < deadline) {
    frame = source.takeFrame();
    if (!frame.has_value() || !frame->base.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  ASSERT_TRUE(frame.has_value()) << "no frame decoded within 5 seconds";
  ASSERT_TRUE(frame->base.has_value());
  EXPECT_FALSE(frame->base->isNull());
  EXPECT_EQ(frame->base->width, 640);
  EXPECT_EQ(frame->base->height, 480);
  EXPECT_TRUE(frame->base->isValid());
}

TEST_F(StreamingVideoSourceTest, RapidTimestampChanges) {
  ObjectStore store;
  auto topic_or = store.registerTopic({.dataset_id = 1, .topic_name = "video", .metadata_json = "{}"});
  ASSERT_TRUE(topic_or.has_value());
  auto topic = *topic_or;

  pushVideoPackets(kTestVideo, store, topic);
  auto [t_min, t_max] = store.timeRange(topic);

  StreamingVideoSource source(&store, topic);

  // Rapid timestamp changes — should not crash
  for (int i = 0; i < 50; ++i) {
    int64_t ts = t_min + (t_max - t_min) * i / 50;
    source.setTimestamp(ts);
    source.takeFrame();  // may or may not have a result yet
  }

  // Wait for the last decode to complete
  std::optional<MediaFrame> frame;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!frame.has_value() || !frame->base.has_value()) && std::chrono::steady_clock::now() < deadline) {
    frame = source.takeFrame();
    if (!frame.has_value() || !frame->base.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  EXPECT_TRUE(frame.has_value()) << "should eventually produce a frame";
}

TEST_F(StreamingVideoSourceTest, IsInitializedAfterKeyframe) {
  ObjectStore store;
  auto topic_or = store.registerTopic({.dataset_id = 1, .topic_name = "video", .metadata_json = "{}"});
  ASSERT_TRUE(topic_or.has_value());
  auto topic = *topic_or;

  StreamingVideoSource source(&store, topic);
  EXPECT_FALSE(source.isInitialized());

  pushVideoPackets(kTestVideo, store, topic);
  auto [t_min, t_max] = store.timeRange(topic);
  source.setTimestamp(t_min);

  // Wait for initialization
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!source.isInitialized() && std::chrono::steady_clock::now() < deadline) {
    source.takeFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  EXPECT_TRUE(source.isInitialized());
}

// --- Parser-mode teardown ordering ------------------------------------------
// The decoder's NAL extractor caches the last parsed ObjectRecord in a keepalive
// slot. That record is a std::any whose manager function — and whose BufferAnchor
// control block — live in the parser plugin's DSO, which `parser_keepalive_` is
// the last thing keeping mapped once the session is gone. Destroying the record
// after the keepalive drops jumps into unmapped text.

/// Destruction-order log. Written from the decode worker (each parse replaces the
/// extractor's slot, destroying the previous record) and from the main thread at
/// teardown, hence the mutex.
class OrderLog {
 public:
  void add(std::string event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
  }

  std::vector<std::string> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

/// Parser returning a VideoFrame whose anchor logs its own destruction — the
/// stand-in for the DSO-owned control block the real teardown dereferenced.
class AnchorLoggingVideoParser final : public MessageParserPluginBase {
 public:
  explicit AnchorLoggingVideoParser(std::shared_ptr<OrderLog> log) {
    sdk::SchemaHandler handler;
    handler.object_type = sdk::BuiltinObjectType::kVideoFrame;
    handler.parse_object = [this, log](Timestamp ts, sdk::PayloadView payload) -> Expected<sdk::ObjectRecord> {
      std::shared_ptr<const void> anchor(new int(0), [log](const int* probe) {
        delete probe;
        log->add("record");
      });
      parse_count_.fetch_add(1, std::memory_order_release);
      return sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = sdk::BuiltinObject{sdk::VideoFrame{
              .timestamp_ns = ts,
              .frame_id = "",
              .format = "h264",
              .data = payload.bytes,
              .anchor = std::move(anchor),
          }}};
    };
    registerSchemaHandler("video", std::move(handler));
  }

  [[nodiscard]] int parseCount() const {
    return parse_count_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<int> parse_count_{0};
};

TEST(StreamingVideoSourceLifetime, CachedRecordDiesBeforeParserKeepalive) {
  auto log = std::make_shared<OrderLog>();
  AnchorLoggingVideoParser parser(log);
  ASSERT_TRUE(parser.bindSchema("video", Span<const uint8_t>{}));

  ObjectStore store;
  auto topic_or = store.registerTopic({.dataset_id = 1, .topic_name = "video", .metadata_json = "{}"});
  ASSERT_TRUE(topic_or.has_value());
  const auto topic = *topic_or;
  // Non-zero retention marks the topic streaming, which skips the thumbnail cache:
  // the decoder's extractor is then the ONLY holder of a parsed record.
  store.setRetentionBudget(topic, RetentionBudget{.time_window_ns = 60'000'000'000});
  store.pushOwned(topic, 0, std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x65});
  ASSERT_EQ(store.entryCount(topic), 1u);

  {
    std::shared_ptr<void> keepalive(new int(0), [log](const int* probe) {
      delete probe;
      log->add("keepalive");
    });
    StreamingVideoSource source(&store, topic, &parser, std::make_shared<std::mutex>(), std::move(keepalive));
    source.setTimestamp(0);

    // The keyframe scan runs the extractor on every entry, so one parse is enough
    // to fill the slot. Without it the ordering assertion below would be vacuous.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (parser.parseCount() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GT(parser.parseCount(), 0) << "extractor never ran — nothing was cached to order against";
  }

  const auto events = log->snapshot();
  EXPECT_NE(std::find(events.begin(), events.end(), "record"), events.end());
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), "keepalive")
      << "a parsed ObjectRecord outlived the parser keepalive; in the app that keepalive's drop "
         "dlcloses the plugin that owns the record's std::any manager and anchor control block";
}

}  // namespace
}  // namespace PJ
