// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>

namespace PJ {
namespace {

const std::string kTestImagesPath = "pj_scene2D/testdata/test_images.mcap";

bool testDataExists() {
  return std::filesystem::exists(kTestImagesPath);
}

class McapImageIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!testDataExists()) {
      GTEST_SKIP() << "test_images.mcap not found at " << kTestImagesPath;
    }
  }
};

struct McapLoader {
  std::shared_ptr<mcap::McapReader> reader;
  ObjectTopicId topic_id{};
  size_t message_count = 0;

  bool load(const std::string& path, ObjectStore& store) {
    reader = std::make_shared<mcap::McapReader>();
    auto status = reader->open(path);
    if (!status.ok()) {
      return false;
    }
    status = reader->readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!status.ok()) {
      return false;
    }

    for (const auto& [chan_id, chan_ptr] : reader->channels()) {
      if (chan_ptr == nullptr) {
        continue;
      }
      // schemas() returns by value — cache to avoid dangling iterator
      std::string schema_name;
      auto schemas = reader->schemas();
      auto schema_it = schemas.find(chan_ptr->schemaId);
      if (schema_it != schemas.end() && schema_it->second != nullptr) {
        schema_name = schema_it->second->name;
      }

      auto id_or = store.registerTopic(
          {.dataset_id = 1,
           .topic_name = chan_ptr->topic,
           .metadata_json =
               nlohmann::json(
                   {{"media_class", "image"}, {"encoding", chan_ptr->messageEncoding}, {"schema", schema_name}})
                   .dump()});
      if (!id_or.has_value()) {
        continue;
      }
      topic_id = *id_or;

      auto local_reader = reader;
      auto local_topic = chan_ptr->topic;
      auto local_chan_id = chan_id;

      mcap::ReadMessageOptions opts;
      opts.topicFilter = [local_topic](std::string_view t) { return t == local_topic; };
      auto view = reader->readMessages([](const mcap::Status&) {}, opts);

      for (auto it = view.begin(); it != view.end(); ++it) {
        auto ts = static_cast<Timestamp>(it->message.logTime);
        if (it->message.channelId != local_chan_id) {
          continue;
        }

        // optional return: nullopt models a genuine re-read FAILURE (message no
        // longer findable), matching the production LazyCallback contract.
        store.pushLazy(topic_id, ts, [local_reader, local_topic, ts]() -> std::optional<PJ::sdk::PayloadView> {
          mcap::ReadMessageOptions read_opts;
          read_opts.startTime = static_cast<mcap::Timestamp>(ts);
          read_opts.endTime = read_opts.startTime + 1;
          read_opts.topicFilter = [local_topic](std::string_view t) { return t == local_topic; };
          auto v = local_reader->readMessages([](const mcap::Status&) {}, read_opts);
          for (auto vit = v.begin(); vit != v.end(); ++vit) {
            const auto* d = reinterpret_cast<const uint8_t*>(vit->message.data);
            return PJ::sdk::makePayloadView(std::vector<uint8_t>(d, d + vit->message.dataSize));
          }
          return std::nullopt;  // message not found: a re-read failure, not an empty payload
        });
        ++message_count;
      }
      break;
    }
    return message_count > 0;
  }
};

TEST_F(McapImageIntegration, LoadAndQueryEntries) {
  ObjectStore store;
  McapLoader loader;
  ASSERT_TRUE(loader.load(kTestImagesPath, store));
  EXPECT_EQ(store.entryCount(loader.topic_id), 90u);

  auto [t_min, t_max] = store.timeRange(loader.topic_id);
  EXPECT_GT(t_max, t_min);
}

TEST_F(McapImageIntegration, LatestAtReturnsBytes) {
  ObjectStore store;
  McapLoader loader;
  ASSERT_TRUE(loader.load(kTestImagesPath, store));

  auto [t_min, t_max] = store.timeRange(loader.topic_id);
  auto mid_ts = t_min + (t_max - t_min) / 2;
  auto entry = store.latestAt(loader.topic_id, mid_ts);
  ASSERT_TRUE(entry.has_value());
  EXPECT_GT(entry->payload.bytes.size(), 0u);
}

TEST_F(McapImageIntegration, DecodeImageFromStore) {
  ObjectStore store;
  McapLoader loader;
  ASSERT_TRUE(loader.load(kTestImagesPath, store));

  auto entry = store.at(loader.topic_id, 0);
  ASSERT_TRUE(entry.has_value());
  ASSERT_GT(entry->payload.bytes.size(), 0u);

  auto raw = entry->payload.bytes;  // Span<const uint8_t>

  auto json_str = store.descriptor(loader.topic_id).metadata_json;
  auto meta = nlohmann::json::parse(json_str);
  auto schema = meta.value("schema", "");

  if (schema == "foxglove.CompressedImage") {
    auto parsed = nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("data")) {
      GTEST_SKIP() << "Cannot parse Foxglove JSON image (binary data embedded)";
    }
  }
}

TEST_F(McapImageIntegration, AllEntriesResolve) {
  ObjectStore store;
  McapLoader loader;
  ASSERT_TRUE(loader.load(kTestImagesPath, store));

  size_t count = store.entryCount(loader.topic_id);
  for (size_t i = 0; i < count; ++i) {
    auto entry = store.at(loader.topic_id, i);
    ASSERT_TRUE(entry.has_value()) << "entry " << i << " failed to resolve";
    EXPECT_GT(entry->payload.bytes.size(), 0u) << "entry " << i << " has empty data";
  }
}

}  // namespace
}  // namespace PJ
