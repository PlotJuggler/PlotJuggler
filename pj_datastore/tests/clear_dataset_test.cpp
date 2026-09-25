// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Tests for the in-place per-dataset clear primitives used by progressive
// reload: DataEngine::clearDatasetChunks and ObjectStore::clearDataset. Both
// must wipe a dataset's data while keeping its topics (and their ids)
// registered, so cached reader pointers / curve bindings survive a refill.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

// Build a dataset with two scalar topics ("alpha", "beta"), each carrying
// `rows` samples (ts = i, value = i). Returns the dataset id; appends the two
// topic ids to `topics_out`.
DatasetId makeScalarDataset(DataEngine& engine, const char* source, int rows, std::vector<TopicId>& topics_out) {
  auto ds = engine.createDataset(DatasetDescriptor{.source_name = source, .time_domain_id = 0});
  EXPECT_TRUE(ds.has_value());
  DataWriter writer = engine.createWriter();
  for (const char* name : {"alpha", "beta"}) {
    auto handle = writer.registerScalarSeries(*ds, name, NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    topics_out.push_back(handle->topic_id);
    for (int i = 0; i < rows; ++i) {
      writer.appendScalar(*handle, static_cast<Timestamp>(i), static_cast<double>(i));
    }
  }
  engine.commitChunks(writer.flushAll());
  return *ds;
}

// Count visible rows for a topic over the full open range. Returns 0 for a
// missing topic (rangeQuery yields no cursor) and for an existing empty one.
size_t countRows(DataEngine& engine, TopicId topic) {
  DataReader reader = engine.createReader();
  auto cursor = reader.rangeQuery(
      QueryRange{
          .topic_id = topic,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  if (!cursor.has_value()) {
    return 0;
  }
  size_t n = 0;
  cursor->forEach([&n](const SampleRow&) { ++n; });
  return n;
}

// Commit one extra scalar topic into an EXISTING dataset (ts = i, value = i),
// simulating a refill adding a topic the prior load did not have. Returns its id.
TopicId addScalarTopic(DataEngine& engine, DatasetId dataset_id, const char* name, int rows) {
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, name, NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value());
  for (int i = 0; i < rows; ++i) {
    writer.appendScalar(*handle, static_cast<Timestamp>(i), static_cast<double>(i));
  }
  engine.commitChunks(writer.flushAll());
  return handle->topic_id;
}

TEST(ClearDatasetChunksTest, TopicIdsStableRangeEmpty) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "to_clear", 100, topics);
  ASSERT_EQ(topics.size(), 2u);
  for (TopicId topic : topics) {
    ASSERT_EQ(countRows(engine, topic), 100u);
  }
  const std::vector<TopicId> ids_before = engine.listTopics(ds);

  engine.clearDatasetChunks(ds);

  for (TopicId topic : topics) {
    EXPECT_EQ(countRows(engine, topic), 0u) << "rows must be cleared";
  }
  // Ids stay registered and listed (unlike retireTopic, which hides them).
  EXPECT_EQ(engine.listTopics(ds), ids_before);
}

TEST(ClearDatasetChunksTest, OtherDatasetUntouched) {
  DataEngine engine;
  std::vector<TopicId> keep_topics;
  std::vector<TopicId> clear_topics;
  makeScalarDataset(engine, "keep", 50, keep_topics);
  const DatasetId clear_ds = makeScalarDataset(engine, "clear", 50, clear_topics);

  engine.clearDatasetChunks(clear_ds);

  for (TopicId topic : clear_topics) {
    EXPECT_EQ(countRows(engine, topic), 0u);
  }
  for (TopicId topic : keep_topics) {
    EXPECT_EQ(countRows(engine, topic), 50u) << "other dataset must survive";
  }
}

TEST(ClearDatasetChunksTest, IdempotentAndUnknownIdNoop) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "idem", 30, topics);

  engine.clearDatasetChunks(ds);
  engine.clearDatasetChunks(ds);  // second call is safe
  for (TopicId topic : topics) {
    EXPECT_EQ(countRows(engine, topic), 0u);
  }
  EXPECT_NO_THROW(engine.clearDatasetChunks(999999));  // unknown id: no-op, no throw
}

// --- ObjectStore side ---

ObjectTopicId registerObjectTopic(ObjectStore& store, DatasetId dataset_id, const std::string& name) {
  auto id = store.registerTopic({.dataset_id = dataset_id, .topic_name = name, .metadata_json = "{}"});
  EXPECT_TRUE(id.has_value());
  return *id;
}

// Underlying numeric ids, for order-sensitive comparison + readable failures.
std::vector<uint32_t> rawIds(const std::vector<ObjectTopicId>& ids) {
  std::vector<uint32_t> out;
  out.reserve(ids.size());
  for (const ObjectTopicId id : ids) {
    out.push_back(id.id);
  }
  return out;
}

TEST(ClearObjectDatasetTest, IdsStableEntriesEmpty) {
  ObjectStore store;
  const DatasetId ds = 1;
  const ObjectTopicId a = registerObjectTopic(store, ds, "cam/a");
  const ObjectTopicId b = registerObjectTopic(store, ds, "cam/b");
  for (const ObjectTopicId id : {a, b}) {
    for (int i = 0; i < 10; ++i) {
      ASSERT_TRUE(store.pushOwned(id, static_cast<Timestamp>(i), std::vector<uint8_t>(8, 0xAB)).has_value());
    }
  }
  ASSERT_EQ(store.entryCount(a), 10u);
  ASSERT_EQ(store.entryCount(b), 10u);
  const std::vector<uint32_t> ids_before = rawIds(store.listTopics(ds));

  store.clearDataset(ds);

  EXPECT_EQ(store.entryCount(a), 0u);
  EXPECT_EQ(store.entryCount(b), 0u);
  // Same ObjectTopicIds, same registration order (NOT removeTopic+re-register).
  EXPECT_EQ(rawIds(store.listTopics(ds)), ids_before);
}

TEST(ClearObjectDatasetTest, OtherDatasetUntouchedAndIdempotentNoop) {
  ObjectStore store;
  const ObjectTopicId keep = registerObjectTopic(store, 1, "keep/a");
  const ObjectTopicId clear = registerObjectTopic(store, 2, "clear/a");
  for (const ObjectTopicId id : {keep, clear}) {
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(store.pushOwned(id, static_cast<Timestamp>(i), std::vector<uint8_t>(4, 1)).has_value());
    }
  }

  store.clearDataset(2);
  EXPECT_EQ(store.entryCount(clear), 0u);
  EXPECT_EQ(store.entryCount(keep), 5u) << "other dataset must survive";

  store.clearDataset(2);                        // idempotent
  EXPECT_NO_THROW(store.clearDataset(999999));  // unknown id: no-op, no throw
  EXPECT_EQ(store.entryCount(keep), 5u);
}

// --- DataEngine detach/reattach: the transactional variant of clearDatasetChunks
//     used by RefillGuard. detach MOVES the prior data aside (O(1)) instead of
//     freeing it; reattach restores the EXACT prior state (data, floor, ratchet
//     stats) and retires any topic a failed refill added. ---

TEST(DatasetChunkSnapshotTest, DetachLeavesTopicsEmptyButListed) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "detach", 100, topics);
  const std::vector<TopicId> ids_before = engine.listTopics(ds);

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);

  EXPECT_TRUE(snap.valid);
  EXPECT_EQ(snap.dataset_id, ds);
  EXPECT_EQ(snap.prior_topic_ids, ids_before);
  for (TopicId topic : topics) {
    EXPECT_EQ(countRows(engine, topic), 0u) << "detach must leave the topic empty";
  }
  EXPECT_EQ(engine.listTopics(ds), ids_before) << "ids stay registered (unlike retireTopic)";
}

TEST(DatasetChunkSnapshotTest, ReattachRestoresExactData) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "roundtrip", 100, topics);

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);
  for (TopicId topic : topics) {
    ASSERT_EQ(countRows(engine, topic), 0u);
  }

  engine.reattachDatasetChunks(ds, std::move(snap));

  for (TopicId topic : topics) {
    EXPECT_EQ(countRows(engine, topic), 100u) << "rows restored";
    auto lock = engine.lockEngine();
    const TopicStorage* storage = engine.getTopicStorage(topic);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->timeMin(), 0);
    EXPECT_EQ(storage->timeMax(), 99);
  }
  EXPECT_FALSE(snap.valid) << "reattach consumes the snapshot";
}

TEST(DatasetChunkSnapshotTest, ReattachRestoresRetentionFloor) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "floor", 100, topics);  // ts 0..99
  engine.enforceRetention(/*retention_window_ns=*/50, ds);               // raises the floor
  Timestamp floor_before = kNoRetentionFloor;
  {
    auto lock = engine.lockEngine();
    floor_before = engine.getTopicStorage(topics[0])->retentionFloor();
  }
  ASSERT_GT(floor_before, kNoRetentionFloor) << "enforceRetention must raise the floor for this test";

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);
  {
    auto lock = engine.lockEngine();
    EXPECT_EQ(engine.getTopicStorage(topics[0])->retentionFloor(), kNoRetentionFloor)
        << "detach normalizes the empty topic's floor (matches clearDatasetChunks)";
  }

  engine.reattachDatasetChunks(ds, std::move(snap));
  {
    auto lock = engine.lockEngine();
    EXPECT_EQ(engine.getTopicStorage(topics[0])->retentionFloor(), floor_before) << "floor restored exactly";
  }
}

TEST(DatasetChunkSnapshotTest, ReattachRestoresRatchetStatsOverRefillIncrease) {
  // The refill ratchets max_observed_array_length / array-expansion counts UP via
  // increase-only accessors; rollback must restore the SMALLER prior value, which
  // is only possible with direct member assignment (not the public ratchet API).
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "stats", 10, topics);
  const TopicId topic = topics[0];
  {
    auto lock = engine.lockEngine();
    TopicStorage* storage = engine.getTopicStorage(topic);
    storage->updateMaxObservedArrayLength(5);
    storage->setArrayExpansionCount("field", 3);
    storage->incrementTruncatedSampleCount();  // prior truncated count -> 2
    storage->incrementTruncatedSampleCount();
  }

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);
  {
    auto lock = engine.lockEngine();  // simulate a refill ratcheting the stats up
    TopicStorage* storage = engine.getTopicStorage(topic);
    storage->updateMaxObservedArrayLength(9);
    storage->setArrayExpansionCount("field", 7);
    storage->incrementTruncatedSampleCount();  // refill drives truncated count -> 5
    storage->incrementTruncatedSampleCount();
    storage->incrementTruncatedSampleCount();
  }

  engine.reattachDatasetChunks(ds, std::move(snap));
  {
    auto lock = engine.lockEngine();
    const TopicStorage* storage = engine.getTopicStorage(topic);
    EXPECT_EQ(storage->maxObservedArrayLength(), 5u) << "restored, not the ratcheted-up 9";
    EXPECT_EQ(storage->arrayExpansionCount("field"), 3u) << "restored, not the refill's 7";
    // truncated_sample_count is the third increase-only ratchet field restored by
    // direct member write; pin it too (it would still read 5 if reattach dropped it).
    EXPECT_EQ(storage->truncatedSampleCount(), 2u) << "restored, not the refill's 5";
  }
}

TEST(DatasetChunkSnapshotTest, ReattachRestoresColumnDescriptors) {
  // The inline column layout (column_descriptors_) is snapshotted on detach and
  // moved back on reattach; clearChunks() does NOT touch it, so reattach's restore
  // is the only thing that undoes a layout the partial refill reshaped.
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "cols", 10, topics);
  const TopicId topic = topics[0];
  std::vector<ColumnDescriptor> original;
  {
    auto lock = engine.lockEngine();
    original = engine.getTopicStorage(topic)->columnDescriptors();
  }
  ASSERT_FALSE(original.empty()) << "scalar topic must carry an inline column layout for this test";

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);
  {
    // Simulate a refill that reshaped the column layout (a mid-stream new field
    // reseals chunks with a different column set).
    auto lock = engine.lockEngine();
    engine.getTopicStorage(topic)->setColumnDescriptors({});
  }

  engine.reattachDatasetChunks(ds, std::move(snap));
  {
    auto lock = engine.lockEngine();
    const std::vector<ColumnDescriptor> restored = engine.getTopicStorage(topic)->columnDescriptors();
    ASSERT_EQ(restored.size(), original.size()) << "column layout restored on reattach";
    for (size_t i = 0; i < restored.size(); ++i) {
      EXPECT_EQ(restored[i].field_path, original[i].field_path);
    }
  }
}

TEST(DatasetChunkSnapshotTest, ReattachRetiresTopicsAddedAfterDetach) {
  DataEngine engine;
  std::vector<TopicId> topics;
  const DatasetId ds = makeScalarDataset(engine, "reconcile", 50, topics);
  const std::vector<TopicId> ids_before = engine.listTopics(ds);

  DataEngine::DatasetChunkSnapshot snap = engine.detachDatasetChunks(ds);
  const TopicId added = addScalarTopic(engine, ds, "gamma", 50);  // the failed refill adds a topic
  {
    const auto listed = engine.listTopics(ds);
    ASSERT_NE(std::find(listed.begin(), listed.end(), added), listed.end());
  }

  engine.reattachDatasetChunks(ds, std::move(snap));

  const std::vector<TopicId> after = engine.listTopics(ds);
  EXPECT_EQ(after, ids_before) << "only the prior topics remain";
  EXPECT_EQ(std::find(after.begin(), after.end(), added), after.end()) << "added topic retired";
  for (TopicId topic : topics) {
    EXPECT_EQ(countRows(engine, topic), 50u) << "originals restored";
  }
}

TEST(DatasetChunkSnapshotTest, DetachUnknownDatasetIsInvalidNoThrow) {
  DataEngine engine;
  DataEngine::DatasetChunkSnapshot snap;
  EXPECT_NO_THROW({ snap = engine.detachDatasetChunks(999999); });
  EXPECT_FALSE(snap.valid);
  EXPECT_NO_THROW(engine.reattachDatasetChunks(999999, DataEngine::DatasetChunkSnapshot{}));
}

// --- ObjectStore detach/reattach: the object-store half of the transaction.
//     Same clear-in-place discipline as clearDataset (store_mutex_ exclusive,
//     drain readers, clear in place keeping the ObjectTopicId) but MOVES the
//     entries aside for restore. ---

TEST(ObjectDatasetSnapshotTest, DetachLeavesSeriesEmptyRegistered) {
  ObjectStore store;
  const DatasetId ds = 7;
  const ObjectTopicId a = registerObjectTopic(store, ds, "cam/a");
  const ObjectTopicId b = registerObjectTopic(store, ds, "cam/b");
  for (const ObjectTopicId id : {a, b}) {
    for (int i = 0; i < 10; ++i) {
      ASSERT_TRUE(store.pushOwned(id, static_cast<Timestamp>(i), std::vector<uint8_t>(8, 0xAB)).has_value());
    }
  }
  const std::vector<uint32_t> ids_before = rawIds(store.listTopics(ds));

  ObjectStore::ObjectDatasetSnapshot snap = store.detachDataset(ds);

  EXPECT_TRUE(snap.valid);
  EXPECT_EQ(snap.dataset_id, ds);
  EXPECT_EQ(rawIds(snap.prior_object_topic_ids), ids_before);
  EXPECT_EQ(store.entryCount(a), 0u);
  EXPECT_EQ(store.entryCount(b), 0u);
  EXPECT_EQ(rawIds(store.listTopics(ds)), ids_before) << "ids stay registered";
}

TEST(ObjectDatasetSnapshotTest, ReattachAfterClearNeverRestoresIntoAnotherDatasetsTopic) {
  // clear() while a dataset is detached restarts id allocation, so an unrelated
  // dataset's new topic can take the detached topic's raw id. Rollback must not
  // pour the snapshot's entries into that stranger.
  ObjectStore store;
  const DatasetId ds_a = 7;
  const DatasetId ds_b = 8;
  const ObjectTopicId a = registerObjectTopic(store, ds_a, "cam/a");
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(store.pushOwned(a, static_cast<Timestamp>(i), std::vector<uint8_t>(8, 0xAB)).has_value());
  }
  ObjectStore::ObjectDatasetSnapshot snap = store.detachDataset(ds_a);
  store.clear();
  const ObjectTopicId b = registerObjectTopic(store, ds_b, "other");
  ASSERT_EQ(b.id, a.id) << "precondition: the raw id was reused";
  ASSERT_TRUE(store.pushOwned(b, 100, std::vector<uint8_t>(4, 0xCD)).has_value());

  store.reattachDataset(ds_a, std::move(snap));

  EXPECT_EQ(store.entryCount(b), 1u);
  EXPECT_EQ(store.memoryUsage(b), 4u);
  EXPECT_EQ(store.descriptor(b).dataset_id, ds_b);
}

TEST(ObjectDatasetSnapshotTest, ReattachRestoresEntriesAndLatestAt) {
  ObjectStore store;
  const DatasetId ds = 7;
  const ObjectTopicId a = registerObjectTopic(store, ds, "cam/a");
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(store.pushOwned(a, static_cast<Timestamp>(i), std::vector<uint8_t>(8, 0xAB)).has_value());
  }

  ObjectStore::ObjectDatasetSnapshot snap = store.detachDataset(ds);
  ASSERT_EQ(store.entryCount(a), 0u);

  store.reattachDataset(ds, std::move(snap));

  EXPECT_EQ(store.entryCount(a), 10u);
  const auto range = store.timeRange(a);
  EXPECT_EQ(range.first, 0);
  EXPECT_EQ(range.second, 9);
  const auto latest = store.latestAt(a, 100);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->timestamp, 9);
  EXPECT_FALSE(snap.valid) << "reattach consumes the snapshot";
}

// A detach/reattach round-trip must reconstruct the uid_order side index from the
// preserved (out-of-order) entries via rebuildUidOrderLocked, so the post-reattach
// UID cursor matches the pre-detach one exactly.
TEST(ObjectDatasetSnapshotTest, ReattachPreservesUidCursorAfterOutOfOrder) {
  ObjectStore store;
  const DatasetId ds = 7;
  const ObjectTopicId a = registerObjectTopic(store, ds, "cam/a");
  for (const Timestamp ts : {Timestamp{100}, Timestamp{200}, Timestamp{300}, Timestamp{150}}) {  // 150 is OOO
    ASSERT_TRUE(store.pushOwned(a, ts, std::vector<uint8_t>(4, 0xAB)).has_value());
  }

  // Capture the pre-detach walk (arrival order, via drainNewSince).
  auto walk = [&store, a]() {
    std::vector<Timestamp> out;
    SequentialUID cursor{};
    for (const auto& e : store.drainNewSince(a, cursor)) {
      out.push_back(e.timestamp);
    }
    return out;
  };
  const std::vector<Timestamp> before = walk();
  const SequentialUID first_before = store.firstSequentialUID(a);
  ASSERT_EQ(before, (std::vector<Timestamp>{100, 200, 300, 150}));

  ObjectStore::ObjectDatasetSnapshot snap = store.detachDataset(ds);
  ASSERT_EQ(store.entryCount(a), 0u);
  store.reattachDataset(ds, std::move(snap));

  EXPECT_EQ(store.firstSequentialUID(a), first_before);
  EXPECT_EQ(store.at(a, first_before)->timestamp, 100);
  EXPECT_EQ(walk(), before) << "rebuildUidOrderLocked must reconstruct the preserved inversion";
}

TEST(ObjectDatasetSnapshotTest, ReattachRemovesTopicsRegisteredAfterDetach) {
  ObjectStore store;
  const DatasetId ds = 7;
  const ObjectTopicId a = registerObjectTopic(store, ds, "cam/a");
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(store.pushOwned(a, static_cast<Timestamp>(i), std::vector<uint8_t>(4, 1)).has_value());
  }
  const std::vector<uint32_t> ids_before = rawIds(store.listTopics(ds));

  ObjectStore::ObjectDatasetSnapshot snap = store.detachDataset(ds);
  const ObjectTopicId added = registerObjectTopic(store, ds, "cam/new");  // the failed refill registers a topic
  ASSERT_TRUE(store.pushOwned(added, 0, std::vector<uint8_t>(4, 2)).has_value());

  store.reattachDataset(ds, std::move(snap));

  EXPECT_EQ(rawIds(store.listTopics(ds)), ids_before) << "added topic removed, original kept";
  EXPECT_EQ(store.entryCount(a), 5u) << "original restored";
}

TEST(ObjectDatasetSnapshotTest, DetachUnknownDatasetInvalidNoThrow) {
  ObjectStore store;
  ObjectStore::ObjectDatasetSnapshot snap;
  EXPECT_NO_THROW({ snap = store.detachDataset(999999); });
  EXPECT_FALSE(snap.valid);
  EXPECT_NO_THROW(store.reattachDataset(999999, ObjectStore::ObjectDatasetSnapshot{}));
}

}  // namespace
}  // namespace PJ
