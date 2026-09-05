// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/streaming_video_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "pj_datastore/object_store.hpp"
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

}  // namespace
}  // namespace PJ
