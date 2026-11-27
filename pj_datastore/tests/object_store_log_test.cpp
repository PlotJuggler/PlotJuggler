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

std::vector<std::string> messagesOf(const ObjectStore& store, ObjectTopicId topic) {
  std::vector<std::string> out;
  auto view = store.logView(topic);
  if (view) {
    (*view)->messages(0, (*view)->size(), [&out](size_t, std::string_view m) { out.emplace_back(m); });
  }
  return out;
}

TEST(ObjectStoreLog, ByteBudgetEvictsWholeChunksAndBumpsEvicted) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  const size_t rows = 4 * LogEntries::kChunkRows;
  for (size_t i = 0; i < rows; ++i) {
    ASSERT_TRUE(store.pushOwned(topic, Timestamp(i), bytesOf(makeLog("message number " + std::to_string(i)))));
  }
  const size_t full = store.memoryUsage(topic);
  store.setRetentionBudget(topic, RetentionBudget{.max_memory_bytes = full / 2});
  ASSERT_TRUE(store.pushOwned(topic, Timestamp(rows), bytesOf(makeLog("one more"))));
  EXPECT_LE(store.memoryUsage(topic), full / 2);
  auto view = store.logView(topic);
  ASSERT_TRUE(view.has_value());
  EXPECT_GT((*view)->evicted(), 0u);
  EXPECT_EQ((*view)->evicted() + (*view)->size(), rows + 1);
  EXPECT_EQ((*view)->recordAt(0).log.message, "message number " + std::to_string((*view)->evicted()));
}

TEST(ObjectStoreLog, ByteBudgetFreesAPartlyEvictedChunkWithoutEvictingMore) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  const size_t rows = 4 * LogEntries::kChunkRows;
  for (size_t i = 0; i < rows; ++i) {
    ASSERT_TRUE(store.pushOwned(topic, Timestamp(i), bytesOf(makeLog("message number " + std::to_string(i)))));
  }
  store.setRetentionBudget(topic, RetentionBudget{.max_entries = rows - 100});
  ASSERT_TRUE(store.pushOwned(topic, Timestamp(rows), bytesOf(makeLog("one more"))));
  ASSERT_EQ((*store.logView(topic))->evicted(), 101u);  // the first sealed chunk is partly evicted
  store.setRetentionBudget(topic, RetentionBudget{.max_memory_bytes = store.memoryUsage(topic) - 1});
  ASSERT_TRUE(store.pushOwned(topic, Timestamp(rows + 1), bytesOf(makeLog("and another"))));
  // Freeing the first chunk is enough: its rows go, not a whole chunk's count more.
  EXPECT_EQ((*store.logView(topic))->evicted(), LogEntries::kChunkRows);
}

TEST(ObjectStoreLog, BundleAppendWithoutDropOnAFilledLogTopic) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  ASSERT_TRUE(store.pushOwned(topic, 10, bytesOf(makeLog("old"))));
  std::vector<OwnedObjectWrite> writes;
  writes.push_back(OwnedObjectWrite{.topic = topic, .timestamp = 20, .payload = bytesOf(makeLog("new")), .drop = {}});
  ASSERT_TRUE(store.pushOwnedBundle(std::move(writes)).has_value());
  EXPECT_EQ(messagesOf(store, topic), (std::vector<std::string>{"old", "new"}));
}

TEST(ObjectStoreLog, FlushToAppendsLogRows) {
  ObjectStore primary;
  ObjectStore secondary;
  const ObjectTopicId to = registerLog(primary);
  const ObjectTopicId from = registerLog(secondary);
  ASSERT_TRUE(primary.pushOwned(to, 10, bytesOf(makeLog("p10"))));
  ASSERT_TRUE(primary.pushOwned(to, 30, bytesOf(makeLog("p30"))));
  ASSERT_TRUE(secondary.pushOwned(from, 20, bytesOf(makeLog("s20"))));
  ASSERT_TRUE(secondary.pushOwned(from, 40, bytesOf(makeLog("s40"))));
  ASSERT_TRUE(secondary.flushTo(primary));
  EXPECT_EQ(messagesOf(primary, to), (std::vector<std::string>{"p10", "s20", "p30", "s40"}));
  EXPECT_EQ(secondary.entryCount(from), 0u);
  const size_t view_bytes = (*primary.logView(to))->memoryBytes();  // the view ends here: never call the store under it
  EXPECT_EQ(primary.memoryUsage(to), view_bytes);
}

TEST(ObjectStoreLog, ReplaceDatasetFromSwapsRows) {
  ObjectStore primary;
  ObjectStore staged;
  const ObjectTopicId shown = registerLog(primary, 1);
  ASSERT_TRUE(primary.pushOwned(shown, 10, bytesOf(makeLog("old load"))));
  const ObjectTopicId reloaded = registerLog(staged, 7);
  ASSERT_TRUE(staged.pushOwned(reloaded, 5, bytesOf(makeLog("new a"))));
  ASSERT_TRUE(staged.pushOwned(reloaded, 6, bytesOf(makeLog("new b"))));
  auto result = primary.replaceDatasetFrom(staged, 7, 1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(messagesOf(primary, shown), (std::vector<std::string>{"new a", "new b"}));
  EXPECT_EQ(staged.entryCount(reloaded), 0u);
}

TEST(ObjectStoreLog, ReplaceThatChangesTheStorageKindSwapsIt) {
  ObjectStore primary;
  ObjectStore staged;
  auto shown = primary.registerTopic(
      {.dataset_id = 1,
       .topic_name = "/rosout",
       .metadata_json = R"({"builtin_object_type":"kLog","payload_encoding":"cdr"})"});
  ASSERT_TRUE(shown.has_value());
  ASSERT_TRUE(primary.pushOwned(*shown, 1, {1, 2, 3}));
  const ObjectTopicId reloaded = registerLog(staged, 7);
  ASSERT_TRUE(staged.pushOwned(reloaded, 5, bytesOf(makeLog("canonical now"))));
  ASSERT_TRUE(primary.replaceDatasetFrom(staged, 7, 1).has_value());
  EXPECT_EQ(messagesOf(primary, *shown), (std::vector<std::string>{"canonical now"}));
}

TEST(ObjectStoreLog, MergeDatasetsInterleavesShiftedLogs) {
  ObjectStore store;
  const ObjectTopicId anchor = registerLog(store, 1);
  const ObjectTopicId source = registerLog(store, 2);
  ASSERT_TRUE(store.pushOwned(anchor, 1000, bytesOf(makeLog("a1000", 1001))));
  ASSERT_TRUE(store.pushOwned(anchor, 3000, bytesOf(makeLog("a3000"))));
  ASSERT_TRUE(store.pushOwned(source, 500, bytesOf(makeLog("s500", 505))));
  ASSERT_TRUE(store.pushOwned(source, 2000, bytesOf(makeLog("s2000"))));
  auto report = store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}}, nullptr);
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(messagesOf(store, anchor), (std::vector<std::string>{"a1000", "s500", "a3000", "s2000"}));
  auto view = store.logView(anchor);
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ((*view)->timestamps(), (std::vector<Timestamp>{1000, 1500, 3000, 3000}));
  EXPECT_EQ((*view)->ownStamp(0), 1001);
  EXPECT_EQ((*view)->ownStamp(1), 1505);
  view.reset();
  EXPECT_EQ(store.entryCount(source), 0u);
}

TEST(ObjectStoreLog, MergeShiftMovesStoreStampAndKeepsZeroOwnStampUnshifted) {
  ObjectStore store;
  const ObjectTopicId anchor = registerLog(store, 1);
  auto other = store.registerTopic({.dataset_id = 1, .topic_name = "/other", .metadata_json = kLogMetadata});
  ASSERT_TRUE(other.has_value());
  const ObjectTopicId moved = registerLog(store, 2, "/only_in_source");
  ASSERT_TRUE(store.pushOwned(anchor, 1, bytesOf(makeLog("anchor"))));
  ASSERT_TRUE(store.pushOwned(moved, 100, bytesOf(makeLog("no own stamp", 0))));
  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 50}}, nullptr).has_value());
  auto view = store.logView(moved);
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ((*view)->timestamps()[0], 150);
  EXPECT_EQ((*view)->ownStamp(0), 0);
  EXPECT_EQ((*view)->recordAt(0).log.timestamp_ns, 0);
}

TEST(ObjectStoreLog, DetachReattachRestoresLogRows) {
  ObjectStore store;
  const ObjectTopicId topic = registerLog(store);
  ASSERT_TRUE(store.pushOwned(topic, 10, bytesOf(makeLog("kept a"))));
  ASSERT_TRUE(store.pushOwned(topic, 20, bytesOf(makeLog("kept b"))));
  auto snapshot = store.detachDataset(1);
  EXPECT_EQ(store.entryCount(topic), 0u);
  ASSERT_TRUE(store.pushOwned(topic, 30, bytesOf(makeLog("during the reload"))));
  store.reattachDataset(1, std::move(snapshot));
  EXPECT_EQ(messagesOf(store, topic), (std::vector<std::string>{"kept a", "kept b"}));
  EXPECT_GT(store.memoryUsage(topic), 0u);
}

}  // namespace
}  // namespace PJ
