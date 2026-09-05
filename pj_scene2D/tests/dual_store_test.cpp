// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>

namespace PJ {
namespace {

const std::string kPotatoPath = "pj_scene2D/testdata/potato.mcap";

class DualStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kPotatoPath)) {
      GTEST_SKIP() << "UNTRACKED FIXTURE potato.mcap — not in the repository, so this suite does NOT run on a clean "
                      "clone. Generate it per pj_scene2D/testdata/README.md.";
    }
  }
};

struct TopicEntry {
  ObjectTopicId obj_id{};
  size_t count = 0;
};

struct DualStoreResult {
  std::unordered_map<std::string, TopicEntry> image_topics;
  std::vector<std::string> scalar_topic_names;
  size_t scalar_count = 0;
};

DualStoreResult loadPotato(ObjectStore& obj_store, DataEngine& engine) {
  DualStoreResult result;

  auto shared_reader = std::make_shared<mcap::McapReader>();
  if (!shared_reader->open(kPotatoPath).ok()) {
    return result;
  }
  if (!shared_reader->readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok()) {
    return result;
  }

  auto dataset_or = engine.createDataset({.source_name = kPotatoPath, .time_domain_id = 0});
  if (!dataset_or.has_value()) {
    return result;
  }
  auto dataset_id = *dataset_or;
  auto writer = engine.createWriter();

  // Classify channels
  std::unordered_map<uint16_t, ObjectTopicId> image_chan_map;
  std::unordered_map<uint16_t, ScalarSeriesHandle> scalar_chan_map;

  for (const auto& [chan_id, chan_ptr] : shared_reader->channels()) {
    if (chan_ptr == nullptr) {
      continue;
    }
    // schemas() returns by value — cache to avoid dangling iterator
    std::string schema_name;
    auto schemas = shared_reader->schemas();
    auto schema_it = schemas.find(chan_ptr->schemaId);
    if (schema_it != schemas.end() && schema_it->second != nullptr) {
      schema_name = schema_it->second->name;
    }

    bool is_image_topic = chan_ptr->topic.find("image") != std::string::npos;
    bool is_imu_topic =
        chan_ptr->topic.find("imu") != std::string::npos && chan_ptr->topic.find("sample") != std::string::npos;
    (void)schema_name;

    if (is_image_topic) {
      auto id_or =
          obj_store.registerTopic({.dataset_id = dataset_id, .topic_name = chan_ptr->topic, .metadata_json = "{}"});
      if (id_or.has_value()) {
        image_chan_map[chan_id] = *id_or;
        result.image_topics[chan_ptr->topic] = {*id_or, 0};
      }
    } else if (is_imu_topic) {
      auto handle_or = writer.registerScalarSeries(dataset_id, chan_ptr->topic, NumericType::kFloat64);
      if (handle_or.has_value()) {
        scalar_chan_map[chan_id] = *handle_or;
        result.scalar_topic_names.push_back(chan_ptr->topic);
      }
    }
  }

  // Route messages
  mcap::ReadMessageOptions opts;
  auto view = shared_reader->readMessages([](const mcap::Status&) {}, opts);

  for (auto it = view.begin(); it != view.end(); ++it) {
    auto ts = static_cast<Timestamp>(it->message.logTime);
    auto chan = it->message.channelId;

    if (auto img_it = image_chan_map.find(chan); img_it != image_chan_map.end()) {
      auto topic_id = img_it->second;
      const auto& local_reader = shared_reader;

      // optional return: nullopt models a genuine re-read FAILURE, matching the
      // production LazyCallback contract.
      obj_store.pushLazy(topic_id, ts, [local_reader, chan, ts]() -> std::optional<PJ::sdk::PayloadView> {
        mcap::ReadMessageOptions read_opts;
        read_opts.startTime = static_cast<mcap::Timestamp>(ts);
        read_opts.endTime = read_opts.startTime + 1;
        auto v = local_reader->readMessages([](const mcap::Status&) {}, read_opts);
        for (auto vit = v.begin(); vit != v.end(); ++vit) {
          if (vit->message.channelId == chan) {
            const auto* d = reinterpret_cast<const uint8_t*>(vit->message.data);
            return PJ::sdk::makePayloadView(std::vector<uint8_t>(d, d + vit->message.dataSize));
          }
        }
        return std::nullopt;  // message not found: a re-read failure, not an empty payload
      });

      // Find the topic name to update count
      for (auto& [name, entry] : result.image_topics) {
        if (entry.obj_id == topic_id) {
          ++entry.count;
          break;
        }
      }
    } else if (auto sc_it = scalar_chan_map.find(chan); sc_it != scalar_chan_map.end()) {
      writer.appendScalar(sc_it->second, ts, static_cast<double>(ts));
      ++result.scalar_count;
    }
  }

  auto flushed = writer.flushAll();
  engine.commitChunks(std::move(flushed));
  return result;
}

TEST_F(DualStoreTest, LoadBothStoresFromSameMcap) {
  ObjectStore obj_store;
  DataEngine engine;

  auto result = loadPotato(obj_store, engine);

  // Two image topics: color (8400) and depth (8400)
  EXPECT_EQ(result.image_topics.size(), 2u);
  for (const auto& [name, entry] : result.image_topics) {
    EXPECT_EQ(entry.count, 8400u) << "topic: " << name;
    EXPECT_EQ(obj_store.entryCount(entry.obj_id), 8400u) << "topic: " << name;
  }

  // Two IMU topics: accel + gyro
  EXPECT_EQ(result.scalar_topic_names.size(), 2u);
  EXPECT_GT(result.scalar_count, 0u);
}

TEST_F(DualStoreTest, BothStoresQueryAtSameTimestamp) {
  ObjectStore obj_store;
  DataEngine engine;

  auto result = loadPotato(obj_store, engine);
  ASSERT_FALSE(result.image_topics.empty());

  auto& [name, entry] = *result.image_topics.begin();
  auto [t_min, t_max] = obj_store.timeRange(entry.obj_id);
  auto mid_ts = t_min + (t_max - t_min) / 2;

  auto image_entry = obj_store.latestAt(entry.obj_id, mid_ts);
  ASSERT_TRUE(image_entry.has_value());
  EXPECT_GT(image_entry->payload.bytes.size(), 0u);
}

}  // namespace
}  // namespace PJ
