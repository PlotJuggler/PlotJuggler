// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"

namespace PJ {
namespace {

ObjectTopicId registerTopic(
    ObjectStore& store, DatasetId ds, const std::string& name, const std::string& type = "kImage") {
  auto id = store.registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = ds,
          .topic_name = name,
          .metadata_json = R"({"builtin_object_type":")" + type + R"("})",
      });
  EXPECT_TRUE(id.has_value()) << (id.has_value() ? "" : id.error());
  return *id;
}

void push(ObjectStore& store, ObjectTopicId id, Timestamp ts, uint8_t fill) {
  auto status = store.pushOwned(id, ts, std::vector<uint8_t>(4, fill));
  EXPECT_TRUE(status.has_value()) << (status.has_value() ? "" : status.error());
}

std::vector<Timestamp> timestamps(const ObjectStore& store, ObjectTopicId id) {
  std::vector<Timestamp> out;
  const auto view = store.entryTimestamps(id);
  for (size_t i = 0; i < view.size(); ++i) {
    out.push_back(view[i]);
  }
  return out;
}

bool containsTopic(const ObjectStore& store, DatasetId dataset_id, ObjectTopicId id) {
  const auto topics = store.listTopics(dataset_id);
  return std::find(topics.begin(), topics.end(), id) != topics.end();
}

TEST(ObjectMergeTest, FusesSameNamedTopicTimeOrdered) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 0, 0xA0);
  push(store, a, 200, 0xA2);
  push(store, b, 0, 0xB0);   // shifted +100 -> 100
  push(store, b, 50, 0xB5);  // shifted +100 -> 150

  const auto report = store.mergeDatasets(/*anchor_id=*/1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 100}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  EXPECT_EQ(timestamps(store, a), (std::vector<Timestamp>{0, 100, 150, 200}));
  EXPECT_EQ(store.entryCount(b), 0u);
  ASSERT_EQ(report->remapped.size(), 1u);
  EXPECT_EQ(report->remapped[0].first.id, b.id);
  EXPECT_EQ(report->remapped[0].second.id, a.id);
  EXPECT_EQ(report->consumed_datasets, (std::vector<DatasetId>{2}));
}

TEST(ObjectMergeTest, ReassignsAscendingUIDs) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 20, 0xA0);  // anchor pushed first -> lower original UID
  push(store, b, 10, 0xB0);  // source shifts before anchor but originally had the higher UID

  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}}).has_value());

  SequentialUID prev{};
  for (size_t i = 0; i < store.entryCount(a); ++i) {
    const auto e = store.at(a, i);
    ASSERT_TRUE(e.has_value());
    if (i > 0) {
      EXPECT_TRUE(prev < e->sequential_uid);
    }
    prev = e->sequential_uid;
  }
  const auto first = store.at(a, size_t{0});
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->timestamp, 10);
  EXPECT_TRUE(store.at(a, first->sequential_uid).has_value());
}

// After the merge's reuidSeriesLocked, uid_order is the identity permutation, so a
// drainNewSince walk visits every merged entry once in ascending UID (and, since the
// merge time-sorts, ascending timestamp) order.
TEST(ObjectMergeTest, UidWalkAfterMergeVisitsEveryEntryAscending) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 20, 0xA0);
  push(store, a, 40, 0xA4);
  push(store, b, 10, 0xB0);  // interleaves below/between anchor stamps after merge
  push(store, b, 30, 0xB3);

  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}}).has_value());

  std::vector<Timestamp> walked;
  SequentialUID cursor{};
  SequentialUID prev{};
  for (const auto& e : store.drainNewSince(a, cursor)) {
    EXPECT_TRUE(prev < e.sequential_uid) << "drainNewSince must yield strictly ascending UIDs";
    prev = e.sequential_uid;
    walked.push_back(e.timestamp);
  }
  EXPECT_EQ(walked.size(), store.entryCount(a));
  EXPECT_EQ(walked, (std::vector<Timestamp>{10, 20, 30, 40}));  // ascending timestamp == ascending UID
}

TEST(ObjectMergeTest, ReparentsSourceOnlyTopic) {
  ObjectStore store;
  registerTopic(store, 1, "/img");
  const auto extra = registerTopic(store, 2, "/cloud", "kPointCloud");
  push(store, extra, 5, 0xCC);

  const auto report = store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  EXPECT_EQ(store.descriptor(extra).dataset_id, 1u);
  EXPECT_EQ(timestamps(store, extra), (std::vector<Timestamp>{1005}));
  ASSERT_EQ(report->added_topics.size(), 1u);
  EXPECT_EQ(report->added_topics[0].id, extra.id);
  EXPECT_TRUE(containsTopic(store, 1, extra));
  EXPECT_FALSE(containsTopic(store, 2, extra));
}

TEST(ObjectMergeTest, NegativeShiftStaysAscending) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 0, 0xA0);
  push(store, b, 1000, 0xB0);  // shift -1500 -> -500

  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = -1500}}).has_value());
  EXPECT_EQ(timestamps(store, a), (std::vector<Timestamp>{-500, 0}));
}

TEST(ObjectMergeTest, RejectsSelfAndDuplicateSourceAtomically) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 0, 0xA0);
  push(store, b, 0, 0xB0);

  EXPECT_FALSE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 1, .raw_shift_ns = 0}}).has_value());
  EXPECT_FALSE(store
                   .mergeDatasets(
                       1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0},
                           DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}})
                   .has_value());

  EXPECT_EQ(store.entryCount(a), 1u);
  EXPECT_EQ(store.entryCount(b), 1u);
}

TEST(ObjectMergeTest, LazyEntryShiftedAndResolvableAfterSourceRemoval) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 0, 0xA0);
  auto bytes = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{7, 7, 7});
  int fetch_count = 0;
  ASSERT_TRUE(store
                  .pushLazy(
                      b, 500,
                      [bytes, &fetch_count]() -> sdk::PayloadView {
                        ++fetch_count;
                        return sdk::PayloadView{
                            Span<const uint8_t>{bytes->data(), bytes->size()},
                            sdk::BufferAnchor{bytes},
                        };
                      })
                  .has_value());

  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}}).has_value());
  EXPECT_EQ(fetch_count, 0);
  store.removeTopic(b);

  const auto resolved = store.latestAt(a, 500);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->timestamp, 500);
  ASSERT_EQ(resolved->payload.bytes.size(), 3u);
  EXPECT_EQ(resolved->payload.bytes[0], 7);
  EXPECT_EQ(fetch_count, 1);
}

TEST(ObjectMergeTest, DuplicateTimestampTieOrderIsAnchorThenSources) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  const auto c = registerTopic(store, 3, "/img");
  push(store, a, 100, 0xA0);
  push(store, b, 100, 0xB0);
  push(store, c, 100, 0xC0);

  ASSERT_TRUE(store
                  .mergeDatasets(
                      1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0},
                          DatasetMergeSource{.dataset_id = 3, .raw_shift_ns = 0}})
                  .has_value());

  ASSERT_EQ(store.entryCount(a), 3u);
  EXPECT_EQ(store.at(a, size_t{0})->payload.bytes[0], 0xA0);
  EXPECT_EQ(store.at(a, size_t{1})->payload.bytes[0], 0xB0);
  EXPECT_EQ(store.at(a, size_t{2})->payload.bytes[0], 0xC0);
}

TEST(ObjectMergeTest, RetentionBudgetAppliedAfterFuse) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  store.setRetentionBudget(a, RetentionBudget{.time_window_ns = 100, .max_memory_bytes = 0});
  push(store, a, 0, 0xA0);
  push(store, b, 150, 0xB0);
  push(store, b, 250, 0xB1);

  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}}).has_value());

  EXPECT_EQ(timestamps(store, a), (std::vector<Timestamp>{150, 250}));
}

TEST(ObjectMergeTest, SourceWithNoObjectTopicsIsNoOp) {
  ObjectStore store;
  const auto a = registerTopic(store, 1, "/img");
  push(store, a, 0, 0xA0);

  const auto report = store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 99, .raw_shift_ns = 1000}});
  ASSERT_TRUE(report.has_value()) << (report.has_value() ? "" : report.error());

  EXPECT_TRUE(report->remapped.empty());
  EXPECT_TRUE(report->added_topics.empty());
  EXPECT_TRUE(report->consumed_datasets.empty());
  EXPECT_EQ(timestamps(store, a), (std::vector<Timestamp>{0}));
}

// A merge shift that would push a timestamp past INT64_MAX saturates instead of
// overflowing (UB) and wrapping to the far past, which would break the order.
TEST(ObjectMergeTest, ShiftNearInt64MaxSaturates) {
  ObjectStore store;
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  const auto a = registerTopic(store, 1, "/img");
  const auto b = registerTopic(store, 2, "/img");
  push(store, a, 0, 0xA0);
  push(store, b, kMax - 10, 0xB0);
  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 100}}).has_value());
  EXPECT_EQ(timestamps(store, a), (std::vector<Timestamp>{0, kMax}));
}

}  // namespace
}  // namespace PJ
