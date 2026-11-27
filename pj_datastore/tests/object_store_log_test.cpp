// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// ObjectStore over kLog topics: a canonical kLog topic is stored as LogEntries and still serves every generic read
// with canonical bytes.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/log_entries.hpp"
#include "pj_datastore/object_store.hpp"

namespace PJ {
namespace {

constexpr auto kLogMetadata = R"({"builtin_object_type":"kLog"})";

sdk::Log makeLog(std::string message, Timestamp own_stamp = 0, std::string source = "node") {
  sdk::Log log;
  log.timestamp_ns = own_stamp;
  log.level = sdk::Log::Level::kInfo;
  log.message = std::move(message);
  log.source = std::move(source);
  return log;
}

std::vector<uint8_t> bytesOf(const sdk::Log& log) {
  auto bytes = serializeBuiltinObject(sdk::BuiltinObject(log));
  EXPECT_TRUE(bytes.has_value());
  return bytes ? *bytes : std::vector<uint8_t>{};
}

sdk::Log decode(const ResolvedObjectEntry& entry) {
  auto object =
      deserializeBuiltinObject(sdk::BuiltinObjectType::kLog, entry.payload.bytes.data(), entry.payload.bytes.size());
  EXPECT_TRUE(object.has_value());
  const sdk::Log* log = object ? object->get<sdk::Log>() : nullptr;
  return log != nullptr ? *log : sdk::Log{};
}

ObjectTopicId registerLog(ObjectStore& store, DatasetId dataset = 1, const std::string& name = "/rosout") {
  auto id = store.registerTopic({.dataset_id = dataset, .topic_name = name, .metadata_json = kLogMetadata});
  EXPECT_TRUE(id.has_value());
  return *id;
}

TEST(ObjectStoreLog, CanonicalLogTopicUsesLogStorage) {
  ObjectStore store;
  const ObjectTopicId log_topic = registerLog(store);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(store.pushOwned(log_topic, 100 * (i + 1), bytesOf(makeLog("m" + std::to_string(i)))));
  }
  auto view = store.logView(log_topic);
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ((*view)->size(), 3u);
  EXPECT_EQ((*view)->recordAt(2).log.message, "m2");
  view.reset();
  EXPECT_EQ(store.entryCount(log_topic), 3u);
  EXPECT_GT(store.memoryUsage(log_topic), 0u);

  auto wire = store.registerTopic(
      {.dataset_id = 1,
       .topic_name = "/rosout_raw",
       .metadata_json =
           R"({"builtin_object_type":"kLog","payload_encoding":"cdr","payload_schema":"rcl_interfaces/msg/Log"})"});
  ASSERT_TRUE(wire.has_value());
  EXPECT_FALSE(store.logView(*wire).has_value());
}

TEST(ObjectStoreLog, GenericReadsReturnCanonicalBytes) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  const std::vector<sdk::Log> logs = {makeLog("a", 95), makeLog("b"), makeLog("c", 301, "other")};
  for (size_t i = 0; i < logs.size(); ++i) {
    ASSERT_TRUE(store.pushOwned(topic, Timestamp(100 * (i + 1)), bytesOf(logs[i])));
  }
  auto by_index = store.at(topic, size_t{1});
  ASSERT_TRUE(by_index.has_value());
  EXPECT_EQ(decode(*by_index), logs[1]);
  EXPECT_EQ(by_index->payload_stamp_shift, 0);

  auto by_uid = store.at(topic, by_index->sequential_uid);
  ASSERT_TRUE(by_uid.has_value());
  EXPECT_EQ(decode(*by_uid), logs[1]);

  for (int pass = 0; pass < 2; ++pass) {  // the second pass hits the warm cache
    auto latest = store.latestAt(topic, 250);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->timestamp, 200);
    EXPECT_EQ(decode(*latest), logs[1]);
  }
  auto id_at = store.latestEntryIdAt(topic, 250);
  ASSERT_TRUE(id_at.has_value());
  EXPECT_EQ(id_at->uid, by_index->sequential_uid);

  SequentialUID cursor;
  const std::vector<ResolvedObjectEntry> drained = store.drainNewSince(topic, cursor);
  ASSERT_EQ(drained.size(), logs.size());
  for (size_t i = 0; i < logs.size(); ++i) {
    EXPECT_EQ(decode(drained[i]), logs[i]) << i;
  }
  EXPECT_EQ(cursor, drained.back().sequential_uid);
  EXPECT_TRUE(store.drainNewSince(topic, cursor).empty());

  const auto [first, last] = store.timeRange(topic);
  EXPECT_EQ(first, 100);
  EXPECT_EQ(last, 300);
  EXPECT_EQ(store.entryTimestamps(topic).size(), 3u);
}

TEST(ObjectStoreLog, MalformedPayloadIsRejected) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  ASSERT_TRUE(store.pushOwned(topic, 10, bytesOf(makeLog("ok"))));
  EXPECT_FALSE(store.pushOwned(topic, 20, std::vector<uint8_t>{1, 2, 3}).has_value());
  EXPECT_EQ(store.entryCount(topic), 1u);

  const ObjectTopicId other = registerLog(store, 1, "/other");
  std::vector<OwnedObjectWrite> writes;
  writes.push_back(OwnedObjectWrite{.topic = other, .timestamp = 5, .payload = bytesOf(makeLog("fine"))});
  writes.push_back(OwnedObjectWrite{.topic = topic, .timestamp = 30, .payload = {9, 9}});
  EXPECT_FALSE(store.pushOwnedBundle(std::move(writes)).has_value());
  EXPECT_EQ(store.entryCount(topic), 1u);
  EXPECT_EQ(store.entryCount(other), 0u);
}

TEST(ObjectStoreLog, BundleWritesDecodeAndDrop) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  ASSERT_TRUE(store.pushOwned(topic, 10, bytesOf(makeLog("old"))));
  std::vector<OwnedObjectWrite> writes;
  writes.push_back(
      OwnedObjectWrite{
          .topic = topic,
          .timestamp = 20,
          .payload = bytesOf(makeLog("new")),
          .drop = [](Timestamp t, SequentialUID) { return t == 10; },
      });
  auto created = store.pushOwnedBundle(std::move(writes));
  ASSERT_TRUE(created.has_value());
  ASSERT_EQ(store.entryCount(topic), 1u);
  EXPECT_EQ(decode(*store.at(topic, size_t{0})).message, "new");
}

TEST(ObjectStoreLog, LazyPushOnLogTopicIsRejected) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  const auto status = store.pushLazy(topic, 10, [] { return std::optional<sdk::PayloadView>{}; });
  EXPECT_FALSE(status.has_value());
  EXPECT_EQ(store.entryCount(topic), 0u);
}

TEST(ObjectStoreLog, RetentionByCountAndTimeOnLogSeries) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  store.setRetentionBudget(topic, RetentionBudget{.max_entries = 3});
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(store.pushOwned(topic, 10 * (i + 1), bytesOf(makeLog("m" + std::to_string(i)))));
  }
  ASSERT_EQ(store.entryCount(topic), 3u);
  EXPECT_EQ(decode(*store.at(topic, size_t{0})).message, "m2");
  EXPECT_EQ((*store.logView(topic))->evicted(), 2u);

  store.setRetentionBudget(topic, RetentionBudget{.time_window_ns = 15});
  ASSERT_TRUE(store.pushOwned(topic, 60, bytesOf(makeLog("m5"))));
  EXPECT_EQ(store.timeRange(topic).first, 50);
}

}  // namespace
}  // namespace PJ
