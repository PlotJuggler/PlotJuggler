// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/sdk/object_bytes.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using sdk::ObjectBytes;
using sdk::ObjectTopicHandle;
using sdk::SourceObjectWriteHostView;
using sdk::ToolboxObjectReadHostView;

constexpr DatasetId kDatasetId = 99;

struct Fixture {
  ObjectStore store;
  DatastoreSourceObjectWriteHost write_impl{store, kDatasetId};
  DatastoreToolboxObjectReadHost read_impl{store};
  SourceObjectWriteHostView writer{write_impl.raw()};
  ToolboxObjectReadHostView reader{read_impl.raw()};
};

TEST(ToolboxObjectReadHostTest, ReadsBytesWrittenByWriteHost) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("markers", R"({"media_class":"scene"})");

  const std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04};
  ASSERT_TRUE(f.writer.pushOwned(topic, 1000, payload).has_value());

  auto bytes = f.reader.readLatestAt(topic, 1500);
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  ASSERT_TRUE(*bytes);
  const auto view = bytes->view();
  EXPECT_EQ(view.size(), payload.size());
  EXPECT_EQ(std::vector<uint8_t>(view.begin(), view.end()), payload);
}

TEST(ToolboxObjectReadHostTest, ObjectBytesDestructorReleasesExactlyOnce) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("images", "{}");
  const std::vector<uint8_t> payload{0xAA, 0xBB};
  ASSERT_TRUE(f.writer.pushOwned(topic, 1, payload).has_value());

  // Scope the ObjectBytes holder; its destructor releases the bytes.
  {
    auto bytes = f.reader.readLatestAt(topic, 1);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_FALSE(bytes->empty());
    // release_bytes runs exactly once here; ASan flags a double free or leak.
  }

  auto again = f.reader.readLatestAt(topic, 1);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->view().size(), payload.size());
}

TEST(ToolboxObjectReadHostTest, OwningHandleSurvivesStoreMutation) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("pointclouds", "{}");
  const std::vector<uint8_t> original{0x10, 0x20, 0x30};
  ASSERT_TRUE(f.writer.pushOwned(topic, 100, original).has_value());

  auto bytes = f.reader.readLatestAt(topic, 100);
  ASSERT_TRUE(bytes.has_value());

  const std::vector<uint8_t> replacement{0xFF};
  ASSERT_TRUE(f.writer.pushOwned(topic, 200, replacement).has_value());
  f.store.evictBefore(ObjectTopicId{topic.id}, 150);
  EXPECT_EQ(f.store.entryCount(ObjectTopicId{topic.id}), 1U);

  // The original handle still points at the original bytes — the
  // shared_ptr inside the handle kept them alive despite eviction.
  const auto view = bytes->view();
  EXPECT_EQ(std::vector<uint8_t>(view.begin(), view.end()), original);
}

TEST(ToolboxObjectReadHostTest, LookupTopicByName) {
  Fixture f;
  const auto registered = *f.writer.registerTopic("lidar/front", "{}");

  const auto found = f.reader.lookupTopic("lidar/front");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->id, registered.id);

  EXPECT_FALSE(f.reader.lookupTopic("no-such-topic").has_value());
}

TEST(ToolboxObjectReadHostTest, ListTopicsReturnsAllRegistered) {
  Fixture f;
  const auto a = *f.writer.registerTopic("a", "{}");
  const auto b = *f.writer.registerTopic("b", "{}");
  const auto c = *f.writer.registerTopic("c", "{}");

  auto topics = f.reader.listTopics();
  ASSERT_TRUE(topics.has_value()) << topics.error();
  ASSERT_EQ(topics->size(), 3U);
  // Order matches insertion in the ObjectStore.
  EXPECT_EQ((*topics)[0].id, a.id);
  EXPECT_EQ((*topics)[1].id, b.id);
  EXPECT_EQ((*topics)[2].id, c.id);
}

TEST(ToolboxObjectReadHostTest, TopicMetadataRoundTrip) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("camera", R"({"media_class":"image","encoding":"jpeg"})");
  EXPECT_EQ(f.reader.topicMetadata(topic), R"({"media_class":"image","encoding":"jpeg"})");
}

TEST(ToolboxObjectReadHostTest, EntryCountAndTimeRange) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("stream", "{}");
  const std::vector<uint8_t> one{0x01};
  const std::vector<uint8_t> two{0x02};
  const std::vector<uint8_t> three{0x03};
  ASSERT_TRUE(f.writer.pushOwned(topic, 10, one).has_value());
  ASSERT_TRUE(f.writer.pushOwned(topic, 20, two).has_value());
  ASSERT_TRUE(f.writer.pushOwned(topic, 30, three).has_value());

  EXPECT_EQ(f.reader.entryCount(topic), 3U);
  const auto range = f.reader.timeRange(topic);
  EXPECT_EQ(range.first, 10);
  EXPECT_EQ(range.second, 30);
}

TEST(ToolboxObjectReadHostTest, ReadLatestAtReturnsErrorOnMiss) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("empty", "{}");

  auto bytes = f.reader.readLatestAt(topic, 42);
  EXPECT_FALSE(bytes.has_value());
}

TEST(ToolboxObjectReadHostTest, HandleSurvivesAcrossThreads) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("threaded", "{}");
  const std::vector<uint8_t> payload(256, 0x7F);
  ASSERT_TRUE(f.writer.pushOwned(topic, 1, payload).has_value());

  auto bytes = f.reader.readLatestAt(topic, 1);
  ASSERT_TRUE(bytes.has_value());

  // Move the holder into a worker thread. Writer mutates the store
  // concurrently; the consumer's view remains valid until the worker
  // drops the holder.
  std::thread worker([b = std::move(*bytes), &payload]() {
    const auto view = b.view();
    ASSERT_EQ(view.size(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
      ASSERT_EQ(view[i], payload[i]);
    }
  });
  const std::vector<uint8_t> other{0x00};
  ASSERT_TRUE(f.writer.pushOwned(topic, 2, other).has_value());
  f.store.evictBefore(ObjectTopicId{topic.id}, 2);

  worker.join();
}

TEST(ToolboxObjectReadHostTest, ViewReportsInvalidWhenUnbound) {
  ToolboxObjectReadHostView empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_FALSE(empty.lookupTopic("x").has_value());
  EXPECT_FALSE(empty.listTopics().has_value());
  EXPECT_FALSE(empty.readLatestAt(ObjectTopicHandle{1}, 0).has_value());
  EXPECT_EQ(empty.entryCount(ObjectTopicHandle{1}), 0U);
  EXPECT_EQ(empty.timeRange(ObjectTopicHandle{1}).first, 0);
}

TEST(ToolboxObjectReadHostTest, MovedObjectBytesIsSafelyEmptied) {
  Fixture f;
  const auto topic = *f.writer.registerTopic("move", "{}");
  const std::vector<uint8_t> one_byte{0xAB};
  ASSERT_TRUE(f.writer.pushOwned(topic, 5, one_byte).has_value());

  auto a = f.reader.readLatestAt(topic, 5);
  ASSERT_TRUE(a.has_value());
  ObjectBytes moved = std::move(*a);
  EXPECT_FALSE(a->empty() && moved.empty()) << "both cannot be empty after move";
  EXPECT_TRUE(a->empty());  // moved-from holder releases nothing on destruction.
  EXPECT_FALSE(moved.empty());
}

TEST(ToolboxObjectReadHostTest, LookupTopicOnDatasetDisambiguatesByDataset) {
  // Object-topic identity is (dataset, name): the same series path can carry
  // markers on two loaded datasets. The dataset-scoped lookup must return the
  // topic owned by the requested dataset, not the first name match.
  ObjectStore store;
  DatastoreToolboxObjectReadHost read_impl{store};
  ToolboxObjectReadHostView reader{read_impl.raw()};

  const auto a = store.registerTopic({.dataset_id = 1, .topic_name = "__markers__//s/p", .metadata_json = "{}"});
  const auto b = store.registerTopic({.dataset_id = 2, .topic_name = "__markers__//s/p", .metadata_json = "{}"});
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_NE(a->id, b->id);

  const auto ha = reader.lookupTopicOnDataset(DatasetId{1}, "__markers__//s/p");
  const auto hb = reader.lookupTopicOnDataset(DatasetId{2}, "__markers__//s/p");
  ASSERT_TRUE(ha.has_value());
  ASSERT_TRUE(hb.has_value());
  EXPECT_EQ(ha->id, a->id);
  EXPECT_EQ(hb->id, b->id);
  EXPECT_NE(ha->id, hb->id);
  EXPECT_FALSE(reader.lookupTopicOnDataset(DatasetId{3}, "__markers__//s/p").has_value());
}

}  // namespace
}  // namespace PJ
