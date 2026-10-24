// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/object_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/resident_payload_pool.hpp"

namespace PJ {
namespace {

ObjectTopicId registerTestTopic(ObjectStore& store, const std::string& name = "test/topic") {
  auto id_or = store.registerTopic({.dataset_id = 1, .topic_name = name, .metadata_json = "{}"});
  EXPECT_TRUE(id_or.has_value());
  return *id_or;
}

std::vector<uint8_t> makePayload(size_t size, uint8_t fill = 0xAB) {
  return std::vector<uint8_t>(size, fill);
}

// Drains every arrival in order via drainNewSince; returns arrival-order timestamps
// and asserts the UIDs come back strictly ascending.
std::vector<Timestamp> walkUids(const ObjectStore& store, ObjectTopicId id) {
  std::vector<Timestamp> out;
  SequentialUID cursor{};
  SequentialUID prev{};
  for (const auto& e : store.drainNewSince(id, cursor)) {
    EXPECT_TRUE(prev < e.sequential_uid) << "drainNewSince must yield strictly ascending UIDs";
    prev = e.sequential_uid;
    out.push_back(e.timestamp);
  }
  return out;
}

// =========================================================================
// Registration
// =========================================================================

TEST(ObjectStoreTest, RegisterAndDescriptor) {
  ObjectStore store;
  auto id = registerTestTopic(store, "cam/image");
  auto desc = store.descriptor(id);
  EXPECT_EQ(desc.topic_name, "cam/image");
  EXPECT_EQ(desc.dataset_id, 1u);
}

TEST(ObjectStoreTest, DuplicateRegistrationFails) {
  ObjectStore store;
  registerTestTopic(store, "cam/image");
  auto dup = store.registerTopic({.dataset_id = 1, .topic_name = "cam/image", .metadata_json = "{}"});
  EXPECT_FALSE(dup.has_value());
}

TEST(ObjectStoreTest, SameNameDifferentDatasetOk) {
  ObjectStore store;
  auto id1 = registerTestTopic(store, "cam/image");
  auto id2_or = store.registerTopic({.dataset_id = 2, .topic_name = "cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(id2_or.has_value());
  EXPECT_NE(id1.id, id2_or->id);
}

TEST(ObjectStoreTest, FindTopicReturnsRegisteredId) {
  ObjectStore store;
  auto id = registerTestTopic(store, "cam/image");
  auto found = store.findTopic(1, "cam/image");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->id, id.id);
}

TEST(ObjectStoreTest, FindTopicMissingReturnsNullopt) {
  ObjectStore store;
  registerTestTopic(store, "cam/image");
  EXPECT_FALSE(store.findTopic(1, "other/topic").has_value());
  EXPECT_FALSE(store.findTopic(99, "cam/image").has_value());
}

TEST(ObjectStoreTest, RegisterTopicCanMirrorExplicitIdAfterCounterDrift) {
  ObjectStore primary;
  ObjectStore secondary;

  auto primary_only =
      primary.registerTopic({.dataset_id = 1, .topic_name = "file/image", .metadata_json = R"({"source":"file"})"});
  ASSERT_TRUE(primary_only.has_value()) << primary_only.error();
  EXPECT_EQ(primary_only->id, 1u);

  auto stream_topic =
      primary.registerTopic({.dataset_id = 2, .topic_name = "stream/image", .metadata_json = R"({"source":"stream"})"});
  ASSERT_TRUE(stream_topic.has_value()) << stream_topic.error();
  EXPECT_EQ(stream_topic->id, 2u);

  auto mirrored = secondary.registerTopic(
      {.dataset_id = 2, .topic_name = "stream/image", .metadata_json = R"({"source":"stream"})"}, *stream_topic);
  ASSERT_TRUE(mirrored.has_value()) << mirrored.error();
  EXPECT_EQ(mirrored->id, stream_topic->id);
  ASSERT_TRUE(secondary.findTopic(2, "stream/image").has_value());
  EXPECT_EQ(secondary.descriptor(*stream_topic).metadata_json, R"({"source":"stream"})");

  auto next_secondary = secondary.registerTopic({.dataset_id = 2, .topic_name = "stream/depth", .metadata_json = "{}"});
  ASSERT_TRUE(next_secondary.has_value()) << next_secondary.error();
  EXPECT_EQ(next_secondary->id, stream_topic->id + 1);
}

TEST(ObjectStoreTest, DuplicateExplicitObjectTopicIdIsRejected) {
  ObjectStore store;
  auto existing = store.registerTopic({.dataset_id = 1, .topic_name = "cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(existing.has_value()) << existing.error();

  auto duplicate =
      store.registerTopic({.dataset_id = 2, .topic_name = "other/image", .metadata_json = "{}"}, *existing);
  EXPECT_FALSE(duplicate.has_value());
}

TEST(ObjectStoreTest, ListTopics) {
  ObjectStore store;
  auto id1 = registerTestTopic(store, "topic_a");
  auto id2 = registerTestTopic(store, "topic_b");
  auto all = store.listTopics();
  EXPECT_EQ(all.size(), 2u);
}

TEST(ObjectStoreTest, ListTopicsByDataset) {
  ObjectStore store;
  store.registerTopic({.dataset_id = 1, .topic_name = "a", .metadata_json = "{}"});
  store.registerTopic({.dataset_id = 2, .topic_name = "b", .metadata_json = "{}"});
  store.registerTopic({.dataset_id = 1, .topic_name = "c", .metadata_json = "{}"});
  auto ds1 = store.listTopics(1);
  auto ds2 = store.listTopics(2);
  EXPECT_EQ(ds1.size(), 2u);
  EXPECT_EQ(ds2.size(), 1u);
}

// forEachTopic visits one dataset's topics in registration order, on the descriptors
// in place, and skips every other dataset.
TEST(ObjectStoreTest, ForEachTopicVisitsOneDatasetInRegistrationOrder) {
  ObjectStore store;
  const auto first = store.registerTopic({.dataset_id = 1, .topic_name = "b", .metadata_json = R"({"k":1})"});
  store.registerTopic({.dataset_id = 2, .topic_name = "x", .metadata_json = "{}"});
  const auto second = store.registerTopic({.dataset_id = 1, .topic_name = "a", .metadata_json = R"({"k":2})"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  std::vector<ObjectTopicId> ids;
  std::vector<std::string> metadata;
  store.forEachTopic(1, [&](ObjectTopicId id, const ObjectTopicDescriptor& descriptor) {
    ids.push_back(id);
    metadata.push_back(descriptor.metadata_json);
  });
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0].id, first->id);
  EXPECT_EQ(ids[1].id, second->id);
  EXPECT_EQ(metadata, (std::vector<std::string>{R"({"k":1})", R"({"k":2})"}));

  bool visited = false;
  store.forEachTopic(3, [&](ObjectTopicId, const ObjectTopicDescriptor&) { visited = true; });
  EXPECT_FALSE(visited);
}

// =========================================================================
// Push + basic queries
// =========================================================================

TEST(ObjectStoreTest, PushOwnedAndEntryCount) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  constexpr size_t kCount = 100;
  for (size_t i = 0; i < kCount; ++i) {
    auto ts = static_cast<Timestamp>(i) * 1000;
    auto result = store.pushOwned(id, ts, makePayload(64));
    ASSERT_TRUE(result.has_value()) << result.error();
  }
  EXPECT_EQ(store.entryCount(id), kCount);
}

TEST(ObjectStoreTest, TimeRange) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(8));
  store.pushOwned(id, 500, makePayload(8));
  store.pushOwned(id, 900, makePayload(8));
  auto [t_min, t_max] = store.timeRange(id);
  EXPECT_EQ(t_min, 100);
  EXPECT_EQ(t_max, 900);
}

TEST(ObjectStoreTest, EmptyTopicQueries) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_EQ(store.entryCount(id), 0u);
  auto [t_min, t_max] = store.timeRange(id);
  EXPECT_EQ(t_min, 0);
  EXPECT_EQ(t_max, 0);
  EXPECT_FALSE(store.latestAt(id, 1000).has_value());
  EXPECT_FALSE(store.at(id, 0).has_value());
  EXPECT_FALSE(store.indexAt(id, 1000).has_value());
}

TEST(ObjectStoreTest, UnknownTopicQueries) {
  ObjectStore store;
  ObjectTopicId bogus{999};
  EXPECT_EQ(store.entryCount(bogus), 0u);
  EXPECT_FALSE(store.latestAt(bogus, 0).has_value());
  EXPECT_FALSE(store.at(bogus, 0).has_value());
  auto result = store.pushOwned(bogus, 0, makePayload(8));
  EXPECT_FALSE(result.has_value());
}

// =========================================================================
// latestAt semantics
// =========================================================================

TEST(ObjectStoreTest, LatestAtExact) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x01));
  store.pushOwned(id, 200, makePayload(4, 0x02));
  store.pushOwned(id, 300, makePayload(4, 0x03));

  auto r = store.latestAt(id, 200);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->timestamp, 200);
  EXPECT_EQ(r->payload.bytes[0], 0x02);
}

TEST(ObjectStoreTest, LatestAtBetween) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x01));
  store.pushOwned(id, 300, makePayload(4, 0x03));

  auto r = store.latestAt(id, 200);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->timestamp, 100);
}

TEST(ObjectStoreTest, LatestAtBeforeFirst) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  EXPECT_FALSE(store.latestAt(id, 50).has_value());
}

TEST(ObjectStoreTest, LatestAtAfterLast) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x01));
  store.pushOwned(id, 200, makePayload(4, 0x02));

  auto r = store.latestAt(id, 999);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->timestamp, 200);
}

// =========================================================================
// latestAt warm cache (per-topic memo)
// =========================================================================

// A ~60 Hz renderer calls latestAt every frame, but object topics publish far
// slower, so consecutive calls usually land on the SAME sample. The warm cache
// must serve those repeats WITHOUT re-invoking the (possibly decompressing) lazy
// fetcher — that redundant per-frame fetch is what the profiler flagged.
TEST(ObjectStoreTest, LatestAtMemoizesRepeatedSample) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  store.pushLazy(id, 100, [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return sdk::makePayloadView(makePayload(1000, 0x07));
  });

  auto a = store.latestAt(id, 150);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(fetch_count, 1);

  auto b = store.latestAt(id, 150);  // same sample
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(fetch_count, 1) << "repeated latestAt on the same sample must not re-fetch";
  EXPECT_EQ(b->timestamp, 100);
  ASSERT_FALSE(b->payload.bytes.empty());
  EXPECT_EQ(b->payload.bytes[0], 0x07);
}

// The cache holds ONE entry: a genuinely different sample must re-resolve, and
// returning to a cached sample must hit again.
TEST(ObjectStoreTest, LatestAtRefetchesOnSampleChange) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  auto fetcher = [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return sdk::makePayloadView(makePayload(8));
  };
  store.pushLazy(id, 100, fetcher);
  store.pushLazy(id, 200, fetcher);

  ASSERT_TRUE(store.latestAt(id, 100).has_value());  // sample @100
  EXPECT_EQ(fetch_count, 1);
  ASSERT_TRUE(store.latestAt(id, 100).has_value());  // same → cached
  EXPECT_EQ(fetch_count, 1);
  ASSERT_TRUE(store.latestAt(id, 250).has_value());  // sample @200 → refetch
  EXPECT_EQ(fetch_count, 2);
  ASSERT_TRUE(store.latestAt(id, 250).has_value());  // same → cached
  EXPECT_EQ(fetch_count, 2);
}

// A failed/empty resolve must NOT be cached, so a transient failure is retried
// on the next read rather than latched. (An engaged-but-empty PayloadView is a
// legitimately empty payload — success — while nullopt is a fetch failure; both
// leave the payload empty and neither may be memoized.)
TEST(ObjectStoreTest, LatestAtDoesNotCacheEmptyResolve) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  store.pushLazy(id, 100, [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return {};  // legitimately empty payload (engaged, zero bytes)
  });

  auto r1 = store.latestAt(id, 150);
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(r1->payload.bytes.empty());
  EXPECT_FALSE(r1->fetch_failed) << "an engaged empty view is success, not failure";
  EXPECT_EQ(fetch_count, 1);

  auto r2 = store.latestAt(id, 150);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(fetch_count, 2) << "an empty resolve must not be memoized (retry next read)";
}

// A fetcher that returns nullopt reports FAILURE: the source could not
// re-produce the bytes. The entry still resolves (metadata intact) but carries
// fetch_failed, so consumers can count and report the loss instead of
// conflating it with a legitimately empty payload. Failures are never
// memoized: a later read invokes the fetcher again.
TEST(ObjectStoreTest, FailedLazyFetchIsFlaggedAndNotMemoized) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  store.pushLazy(id, 100, [&fetch_count]() -> std::optional<sdk::PayloadView> {
    ++fetch_count;
    return std::nullopt;  // fetch failure: no usable bytes
  });

  auto r1 = store.latestAt(id, 150);
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(r1->fetch_failed);
  EXPECT_TRUE(r1->payload.bytes.empty());
  EXPECT_EQ(fetch_count, 1);

  auto r2 = store.at(id, r1->sequential_uid);
  ASSERT_TRUE(r2.has_value());
  EXPECT_TRUE(r2->fetch_failed);
  EXPECT_EQ(fetch_count, 2) << "a failed fetch must not be memoized";
}

// drainNewSince forwards fetch_failed to cursor consumers: the failed entry
// appears in the drained batch (present, empty payload, flagged) and the
// cursor advances past it — the loss is permanent, so it must be REPORTED by
// the consumer, never revisited. Healthy neighbours are unaffected.
TEST(ObjectStoreTest, DrainNewSinceForwardsFetchFailure) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushLazy(id, 100, []() -> sdk::PayloadView { return sdk::makePayloadView(makePayload(8, 0x01)); });
  store.pushLazy(id, 200, []() -> std::optional<sdk::PayloadView> { return std::nullopt; });
  store.pushLazy(id, 300, []() -> sdk::PayloadView { return sdk::makePayloadView(makePayload(8, 0x03)); });

  SequentialUID cursor;
  auto drained = store.drainNewSince(id, cursor);
  ASSERT_EQ(drained.size(), 3u);
  EXPECT_FALSE(drained[0].fetch_failed);
  EXPECT_TRUE(drained[1].fetch_failed);
  EXPECT_TRUE(drained[1].payload.bytes.empty());
  EXPECT_FALSE(drained[2].fetch_failed);
  EXPECT_EQ(drained[2].payload.bytes[0], 0x03);

  // Cursor advanced past the failed entry: nothing is re-delivered.
  EXPECT_TRUE(store.drainNewSince(id, cursor).empty());
}

// The warm cache keeps the most-recent entry resident across reads, but drops it
// when that entry is evicted (so it never pins bytes past the entry's lifetime).
TEST(ObjectStoreTest, LatestAtCacheReleasesEvictedEntry) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushLazy(id, 100, []() -> sdk::PayloadView { return sdk::makePayloadView(makePayload(4096)); });
  store.pushLazy(id, 200, []() -> sdk::PayloadView { return sdk::makePayloadView(makePayload(8)); });

  std::weak_ptr<const void> probe;
  {
    auto a = store.latestAt(id, 150);  // caches @100
    ASSERT_TRUE(a.has_value());
    probe = a->payload.anchor;
  }
  // Caller dropped its copy, but the warm cache still pins @100.
  EXPECT_FALSE(probe.expired());

  store.evictBefore(id, 150);  // evicts @100 — the cached entry
  EXPECT_TRUE(probe.expired()) << "evicting the cached entry must release its warm copy";
}

// =========================================================================
// latestEntryIdAt (metadata-only identity of the active latest sample)
// =========================================================================

// latestEntryIdAt names exactly the entry latestAt would resolve, without ever
// invoking the lazy fetcher — the identity a sampling layer compares against
// its memo BEFORE paying latestAt's resolve.
TEST(ObjectStoreTest, LatestEntryIdAtMatchesLatestAtWithoutResolving) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  auto fetcher = [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return sdk::makePayloadView(makePayload(8));
  };
  store.pushLazy(id, 100, fetcher);
  store.pushLazy(id, 200, fetcher);

  const auto exact = store.latestEntryIdAt(id, 200);
  const auto between = store.latestEntryIdAt(id, 150);
  const auto after = store.latestEntryIdAt(id, 999);
  ASSERT_TRUE(exact.has_value());
  ASSERT_TRUE(between.has_value());
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(exact->timestamp, 200);
  EXPECT_EQ(between->timestamp, 100);
  EXPECT_EQ(after->timestamp, 200);
  EXPECT_EQ(fetch_count, 0) << "latestEntryIdAt must never invoke a lazy fetch";

  const auto resolved = store.latestAt(id, 150);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->sequential_uid, between->uid) << "identity must name the entry latestAt resolves";
  EXPECT_EQ(fetch_count, 1);
}

// Under an out-of-order insert the latest-BY-TIME entry's UID differs from the
// max arrival UID at-or-before t (maxUidAtOrBefore): the identity must follow
// indexAtOrBefore's pick, never the arrival high-water.
TEST(ObjectStoreTest, LatestEntryIdAtFollowsLatestByTimeUnderOutOfOrderInsert) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  auto fetcher = [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return sdk::makePayloadView(makePayload(8));
  };
  store.pushLazy(id, 100, fetcher);
  store.pushLazy(id, 300, fetcher);
  store.pushLazy(id, 200, fetcher);  // out-of-order: oldest-but-one ts, newest UID

  // Decode-free ground truth for each entry's UID.
  const auto window = store.rangeByTime(id, std::numeric_limits<Timestamp>::min(), 400);
  ASSERT_EQ(window.size(), 3u);
  const auto uid_at_200 = window[1].uid;
  const auto uid_at_300 = window[2].uid;
  ASSERT_LT(uid_at_300, uid_at_200) << "precondition: the out-of-order entry must carry the newest UID";

  const auto mid = store.latestEntryIdAt(id, 250);
  ASSERT_TRUE(mid.has_value());
  EXPECT_EQ(mid->timestamp, 200);
  EXPECT_EQ(mid->uid, uid_at_200);

  const auto top = store.latestEntryIdAt(id, 400);
  ASSERT_TRUE(top.has_value());
  EXPECT_EQ(top->timestamp, 300);
  EXPECT_EQ(top->uid, uid_at_300);
  EXPECT_NE(top->uid, store.maxUidAtOrBefore(id, 400)) << "identity must not be the arrival high-water UID";
  EXPECT_EQ(fetch_count, 0) << "latestEntryIdAt must never invoke a lazy fetch";
}

TEST(ObjectStoreTest, LatestEntryIdAtEmptyCases) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_FALSE(store.latestEntryIdAt(id, 100).has_value()) << "empty topic";
  EXPECT_FALSE(store.latestEntryIdAt(ObjectTopicId{999}, 100).has_value()) << "unknown topic";
  store.pushOwned(id, 100, makePayload(4));
  EXPECT_FALSE(store.latestEntryIdAt(id, 50).has_value()) << "before the first entry";
}

// =========================================================================
// at(index)
// =========================================================================

TEST(ObjectStoreTest, AtValidIndex) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x01));
  store.pushOwned(id, 200, makePayload(4, 0x02));

  auto r0 = store.at(id, 0);
  ASSERT_TRUE(r0.has_value());
  EXPECT_EQ(r0->timestamp, 100);

  auto r1 = store.at(id, 1);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->timestamp, 200);
}

TEST(ObjectStoreTest, AtOutOfRange) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  EXPECT_FALSE(store.at(id, 1).has_value());
  EXPECT_FALSE(store.at(id, 999).has_value());
}

TEST(ObjectStoreTest, SequentialUIDSurvivesEviction) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x01));
  store.pushOwned(id, 200, makePayload(4, 0x02));
  store.pushOwned(id, 300, makePayload(4, 0x03));

  auto first = store.latestAt(id, 100);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->sequential_uid.valid());
  auto second = store.latestAt(id, 200);
  ASSERT_TRUE(second.has_value());
  auto third = store.latestAt(id, 300);
  ASSERT_TRUE(third.has_value());
  EXPECT_LT(first->sequential_uid, second->sequential_uid);
  EXPECT_LT(second->sequential_uid, third->sequential_uid);
  EXPECT_EQ(store.firstSequentialUID(id), first->sequential_uid);

  store.evictBefore(id, 250);
  EXPECT_EQ(store.entryCount(id), 1u);
  EXPECT_EQ(store.firstSequentialUID(id), third->sequential_uid);
  EXPECT_FALSE(store.at(id, first->sequential_uid).has_value());

  auto retained = store.at(id, third->sequential_uid);
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(retained->timestamp, 300);
  EXPECT_EQ(retained->payload.bytes[0], 0x03);

  store.pushOwned(id, 400, makePayload(4, 0x04));
  auto latest = store.latestAt(id, 999);
  ASSERT_TRUE(latest.has_value());
  EXPECT_LT(third->sequential_uid, latest->sequential_uid);
  EXPECT_EQ(store.at(id, latest->sequential_uid)->timestamp, 400);
}

TEST(ObjectStoreTest, SequentialUIDLookupSurvivesInterleavedTopics) {
  ObjectStore store;
  auto first_topic = registerTestTopic(store, "first");
  auto second_topic = registerTestTopic(store, "second");

  ASSERT_TRUE(store.pushOwned(first_topic, 100, makePayload(4, 0x11)).has_value());
  ASSERT_TRUE(store.pushOwned(second_topic, 150, makePayload(4, 0x22)).has_value());
  ASSERT_TRUE(store.pushOwned(first_topic, 200, makePayload(4, 0x33)).has_value());

  auto first_entry = store.latestAt(first_topic, 100);
  ASSERT_TRUE(first_entry.has_value());
  auto second_entry = store.latestAt(first_topic, 200);
  ASSERT_TRUE(second_entry.has_value());
  EXPECT_LT(first_entry->sequential_uid, second_entry->sequential_uid);

  auto resolved = store.at(first_topic, second_entry->sequential_uid);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->timestamp, 200);
  EXPECT_EQ(resolved->payload.bytes[0], 0x33);
}

TEST(ObjectStoreTest, SequentialUIDOfOtherTopicResolvesNullopt) {
  ObjectStore store;
  auto first_topic = registerTestTopic(store, "first");
  auto second_topic = registerTestTopic(store, "second");

  ASSERT_TRUE(store.pushOwned(first_topic, 100, makePayload(4, 0x11)).has_value());
  ASSERT_TRUE(store.pushOwned(second_topic, 150, makePayload(4, 0x22)).has_value());
  ASSERT_TRUE(store.pushOwned(first_topic, 200, makePayload(4, 0x33)).has_value());

  // The foreign UID falls inside first_topic's [first, last] UID range; the
  // lookup must miss, not resolve a neighboring entry.
  const auto foreign = store.latestAt(second_topic, 150);
  ASSERT_TRUE(foreign.has_value());
  EXPECT_FALSE(store.at(first_topic, foreign->sequential_uid).has_value());
}

TEST(ObjectStoreTest, ArrivalCursorStepsSparseTopicSequence) {
  ObjectStore store;
  auto walked = registerTestTopic(store, "walked");
  auto noise = registerTestTopic(store, "noise");

  // Interleave pushes so walked's UIDs are sparse in the global sequence.
  ASSERT_TRUE(store.pushOwned(walked, 100, makePayload(4, 0x01)).has_value());
  ASSERT_TRUE(store.pushOwned(noise, 110, makePayload(4, 0xEE)).has_value());
  ASSERT_TRUE(store.pushOwned(noise, 120, makePayload(4, 0xEE)).has_value());
  ASSERT_TRUE(store.pushOwned(walked, 200, makePayload(4, 0x02)).has_value());
  ASSERT_TRUE(store.pushOwned(noise, 210, makePayload(4, 0xEE)).has_value());
  ASSERT_TRUE(store.pushOwned(walked, 300, makePayload(4, 0x03)).has_value());

  // An invalid `cursor` starts from the first retained entry; the drain returns this
  // topic's entries in arrival order, skipping the interleaved foreign UIDs.
  SequentialUID cursor{};
  const auto drained = store.drainNewSince(walked, cursor);
  ASSERT_EQ(drained.size(), 3u);
  EXPECT_EQ(drained[0].sequential_uid, store.firstSequentialUID(walked));
  EXPECT_EQ(drained[0].timestamp, 100);
  EXPECT_EQ(drained[1].timestamp, 200);
  EXPECT_EQ(drained[2].timestamp, 300);
  EXPECT_GT(drained[1].sequential_uid.value, drained[0].sequential_uid.value + 1)
      << "test setup should leave a UID gap";

  // The cursor is now advanced past the last entry: a re-drain yields nothing.
  EXPECT_TRUE(store.drainNewSince(walked, cursor).empty());

  // Resuming from a mid-stream cursor yields only the entries after it.
  SequentialUID mid = drained[0].sequential_uid;
  const auto rest = store.drainNewSince(walked, mid);
  ASSERT_EQ(rest.size(), 2u);
  EXPECT_EQ(rest[0].timestamp, 200);

  // Unknown topic: empty.
  SequentialUID unknown{};
  EXPECT_TRUE(store.drainNewSince(ObjectTopicId{9999}, unknown).empty());

  // Eviction moves the start of the drain to the new front.
  store.evictBefore(walked, 250);
  SequentialUID after_evict{};
  const auto post = store.drainNewSince(walked, after_evict);
  ASSERT_EQ(post.size(), 1u);
  EXPECT_EQ(post[0].timestamp, 300);
}

// Regression for the out-of-order-push fix: keeping entries timestamp-sorted must
// NOT break the SequentialUID cursor. at(uid) / drainNewSince are the streaming
// ingest path used by TransformService; they require every retained entry to stay
// reachable by UID and drained exactly once, regardless of arrival order.
TEST(ObjectStoreTest, OutOfOrderPushRemainsReachableByUidCursor) {
  ObjectStore store;
  auto id = registerTestTopic(store);

  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0x01)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4, 0x02)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4, 0x03)).has_value());
  // Out-of-order arrival: the store timestamp regresses to 150 but the entry
  // still gets the newest UID (real case: multi-publisher /tf interleaving).
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4, 0x04)).has_value());

  const auto e150 = store.latestAt(id, 150);
  ASSERT_TRUE(e150.has_value());
  EXPECT_EQ(e150->timestamp, 150);

  // (a) The out-of-order entry now holds the largest UID; it must be reachable.
  const auto by_uid = store.at(id, e150->sequential_uid);
  ASSERT_TRUE(by_uid.has_value()) << "at(uid) lost the out-of-order entry";
  EXPECT_EQ(by_uid->timestamp, 150);

  // (b) A drainNewSince walk must visit every retained entry exactly once.
  std::vector<Timestamp> walked_ts = walkUids(store, id);
  std::sort(walked_ts.begin(), walked_ts.end());
  EXPECT_EQ(walked_ts, (std::vector<Timestamp>{100, 150, 200, 300}));
}

// UID order == arrival order: the out-of-order entry carries the newest UID and
// is therefore visited LAST by the walk, regardless of its earlier timestamp.
TEST(ObjectStoreTest, ArrivalCursorVisitsOutOfOrderEntryLast) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4)).has_value());  // OOO: newest UID

  EXPECT_EQ(walkUids(store, id), (std::vector<Timestamp>{100, 200, 300, 150}));

  // The last-drained entry is the out-of-order one (it holds the newest arrival UID).
  SequentialUID cursor{};
  const auto drained = store.drainNewSince(id, cursor);
  ASSERT_FALSE(drained.empty());
  EXPECT_EQ(drained.back().timestamp, 150);
}

// =========================================================================
// rangeByTime — half-open (lo, hi] time-window snapshot
// =========================================================================

// A time window must include an out-of-order entry regardless of its UID
// position — the exact case a UID-cursor walk would skip.
TEST(ObjectStoreTest, RangeByTimeIncludesOutOfOrderEntryInWindow) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0x01)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4, 0x02)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4, 0x03)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4, 0x04)).has_value());  // OOO: newest UID, mid-window

  const auto window = store.rangeByTime(id, 120, 180);
  ASSERT_EQ(window.size(), 1u) << "the out-of-order ts=150 entry must be in (120,180]";
  EXPECT_EQ(window[0].timestamp, 150);
  const auto resolved = store.at(id, window[0].uid);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->payload.bytes[0], 0x04);
}

// (lo, hi] is half-open (lo exclusive, hi inclusive), ascending by timestamp;
// hi <= lo and unknown/empty topics yield an empty window.
TEST(ObjectStoreTest, RangeByTimeHalfOpenBoundariesAndEmptyCases) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_TRUE(store.rangeByTime(id, 0, 1000).empty()) << "empty topic";
  EXPECT_TRUE(store.rangeByTime(ObjectTopicId{9999}, 0, 1000).empty()) << "unknown topic";

  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());

  auto stamps = [&](Timestamp lo, Timestamp hi) {
    std::vector<Timestamp> ts;
    for (const auto& ref : store.rangeByTime(id, lo, hi)) {
      ts.push_back(ref.timestamp);
    }
    return ts;
  };
  EXPECT_EQ(stamps(99, 300), (std::vector<Timestamp>{100, 200, 300}));
  EXPECT_EQ(stamps(100, 300), (std::vector<Timestamp>{200, 300})) << "lo is exclusive";
  EXPECT_EQ(stamps(100, 200), (std::vector<Timestamp>{200})) << "hi is inclusive";
  EXPECT_TRUE(stamps(300, 400).empty()) << "lo exclusive at the tail";
  EXPECT_TRUE(stamps(200, 100).empty()) << "reversed window (hi < lo)";
  EXPECT_TRUE(stamps(200, 200).empty()) << "empty window (hi == lo)";
}

// The window is a snapshot of stable UIDs; an entry evicted before the caller
// resolves it comes back nullopt while the rest still resolve.
TEST(ObjectStoreTest, RangeByTimeSnapshotIsEvictionSafe) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());

  const auto window = store.rangeByTime(id, 50, 350);
  ASSERT_EQ(window.size(), 3u);
  store.evictBefore(id, 250);  // drops ts=100 and ts=200 after the snapshot was taken
  int resolved = 0;
  for (const auto& ref : window) {
    if (store.at(id, ref.uid).has_value()) {
      ++resolved;
    }
  }
  EXPECT_EQ(resolved, 1) << "only the surviving ts=300 entry resolves; evicted uids -> nullopt";
}

// Equal timestamps keep arrival (UID) order within the window.
TEST(ObjectStoreTest, RangeByTimeEqualTimestampsKeepArrivalOrder) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0xA1)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0xB2)).has_value());

  const auto window = store.rangeByTime(id, 50, 100);
  ASSERT_EQ(window.size(), 2u);
  EXPECT_LT(window[0].uid, window[1].uid) << "ascending by UID for equal timestamps";
  EXPECT_EQ(store.at(id, window[0].uid)->payload.bytes[0], 0xA1);
  EXPECT_EQ(store.at(id, window[1].uid)->payload.bytes[0], 0xB2);
}

// maxUidAtOrBefore is the high-water arrival UID of everything at-or-before t —
// NOT latestAt(t)'s UID (the newest-TIMESTAMP entry), which an out-of-order insert
// can leave below a retained entry's UID.
TEST(ObjectStoreTest, MaxUidAtOrBeforeIsHighWaterNotLatestTimestamp) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_FALSE(store.maxUidAtOrBefore(id, 1000).valid()) << "empty topic";
  EXPECT_FALSE(store.maxUidAtOrBefore(ObjectTopicId{9999}, 1000).valid()) << "unknown topic";

  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());  // out-of-order: newest UID, middle ts

  const auto u_100 = store.latestAt(id, 100)->sequential_uid;
  const auto u_300 = store.latestAt(id, 300)->sequential_uid;  // newest TIMESTAMP
  const auto u_200 = store.latestAt(id, 200)->sequential_uid;  // newest UID (pushed last)
  ASSERT_LT(u_100, u_300);
  ASSERT_LT(u_300, u_200);

  EXPECT_FALSE(store.maxUidAtOrBefore(id, 50).valid()) << "nothing at-or-before 50";
  EXPECT_EQ(store.maxUidAtOrBefore(id, 100), u_100);
  EXPECT_EQ(store.maxUidAtOrBefore(id, 250), u_200) << "ts<=250 covers 100 and 200; u_200 is the max UID";
  // The crux: at-or-before 1000 covers all three; the max UID is u_200 (the OOO
  // entry), NOT latestAt(1000)'s u_300 (the newest-timestamp entry).
  EXPECT_EQ(store.maxUidAtOrBefore(id, 1000), u_200);
  EXPECT_EQ(store.latestAt(id, 1000)->sequential_uid, u_300);
}

// oldestArrivalAfter: the oldest timestamp among the entries that arrived after a cursor UID, so a consumer that
// evaluated everything up to some time detects an out-of-order or same-stamp arrival at or before it.
TEST(ObjectStoreTest, OldestArrivalAfterSeesLateAndSameStampEntries) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_FALSE(store.oldestArrivalAfter(id, SequentialUID{}).has_value()) << "empty topic";
  EXPECT_FALSE(store.oldestArrivalAfter(ObjectTopicId{9999}, SequentialUID{}).has_value()) << "unknown topic";

  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  const SequentialUID cursor = store.latestAt(id, 200)->sequential_uid;
  EXPECT_EQ(store.oldestArrivalAfter(id, SequentialUID{}), 100) << "everything arrived after the invalid UID";
  EXPECT_FALSE(store.oldestArrivalAfter(id, cursor).has_value()) << "nothing arrived since";

  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());
  EXPECT_EQ(store.oldestArrivalAfter(id, cursor), 300) << "an in-order append";
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4)).has_value());  // out of order
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());  // same stamp
  EXPECT_EQ(store.oldestArrivalAfter(id, cursor), 150);
  const SequentialUID same_stamp = store.latestAt(id, 200)->sequential_uid;
  EXPECT_FALSE(store.oldestArrivalAfter(id, same_stamp).has_value()) << "the same-stamp entry arrived last";
}

// Multiple out-of-order inserts, the second retargeting an already-shifted index.
TEST(ObjectStoreTest, ArrivalCursorAfterMultipleOutOfOrderInserts) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 400, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 500, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4)).has_value());  // OOO
  ASSERT_TRUE(store.pushOwned(id, 350, makePayload(4)).has_value());  // OOO, retargets shifted index

  auto walked = walkUids(store, id);
  EXPECT_EQ(walked.size(), store.entryCount(id));
  std::sort(walked.begin(), walked.end());
  EXPECT_EQ(walked, (std::vector<Timestamp>{100, 150, 200, 300, 350, 400, 500}));
}

// Evicting an out-of-order entry (a non-min UID sitting at the array front) must
// leave the UID cursor consistent for the survivors.
TEST(ObjectStoreTest, EvictionOfOutOfOrderEntryKeepsUidCursorConsistent) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 50, makePayload(4)).has_value());  // OOO: oldest ts, newest UID, lands at front
  const auto oldest = store.latestAt(id, 50);
  ASSERT_TRUE(oldest.has_value());
  const SequentialUID evicted = oldest->sequential_uid;
  store.evictBefore(id, 100);  // evicts the ts=50 entry (entries.front(), a non-min UID)
  EXPECT_FALSE(store.at(id, evicted).has_value());
  EXPECT_EQ(walkUids(store, id), (std::vector<Timestamp>{200, 300}));
}

// firstSequentialUID returns the smallest RETAINED uid, which after a partial
// eviction need not be entries.front().
TEST(ObjectStoreTest, FirstUidNotFrontEntryAfterPartialEvict) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4)).has_value());  // u1
  ASSERT_TRUE(store.pushOwned(id, 200, makePayload(4)).has_value());  // u2
  ASSERT_TRUE(store.pushOwned(id, 300, makePayload(4)).has_value());  // u3
  ASSERT_TRUE(store.pushOwned(id, 150, makePayload(4)).has_value());  // u4 (OOO, largest UID, sits at slot 1)

  const SequentialUID u1 = store.latestAt(id, 100)->sequential_uid;
  const SequentialUID u2 = store.latestAt(id, 200)->sequential_uid;
  const SequentialUID u4 = store.latestAt(id, 150)->sequential_uid;

  store.evictBefore(id, 125);  // drops only ts=100/u1
  EXPECT_EQ(store.firstSequentialUID(id), u2) << "smallest retained UID, NOT entries.front() (ts=150)";
  EXPECT_EQ(store.at(id, u4)->timestamp, 150);
  EXPECT_FALSE(store.at(id, u1).has_value());
  EXPECT_EQ(walkUids(store, id), (std::vector<Timestamp>{200, 300, 150}));
}

// Equal timestamps take the in-order append (100 >= back()), so arrival order ==
// UID order and the walk never reorders them.
TEST(ObjectStoreTest, EqualTimestampArrivalOrderPreservedByUidCursor) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0xA1)).has_value());
  ASSERT_TRUE(store.pushOwned(id, 100, makePayload(4, 0xB2)).has_value());

  SequentialUID cursor{};
  const auto drained = store.drainNewSince(id, cursor);
  ASSERT_EQ(drained.size(), 2u);
  EXPECT_EQ(drained[0].payload.bytes[0], 0xA1);
  EXPECT_EQ(drained[1].payload.bytes[0], 0xB2);
}

// =========================================================================
// indexAt
// =========================================================================

TEST(ObjectStoreTest, IndexAtExact) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  store.pushOwned(id, 200, makePayload(4));
  store.pushOwned(id, 300, makePayload(4));

  auto idx = store.indexAt(id, 200);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 1u);
}

TEST(ObjectStoreTest, IndexAtBetween) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  store.pushOwned(id, 300, makePayload(4));

  auto idx = store.indexAt(id, 250);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(*idx, 0u);
}

TEST(ObjectStoreTest, IndexAtBeforeFirst) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  EXPECT_FALSE(store.indexAt(id, 50).has_value());
}

// =========================================================================
// EntryTimestampsView
// =========================================================================

TEST(ObjectStoreTest, EntryTimestampsView) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  store.pushOwned(id, 200, makePayload(4));
  store.pushOwned(id, 300, makePayload(4));

  auto view = store.entryTimestamps(id);
  EXPECT_EQ(view.size(), 3u);
  EXPECT_EQ(view[0], 100);
  EXPECT_EQ(view[1], 200);
  EXPECT_EQ(view[2], 300);

  size_t count = 0;
  for (auto it = view.begin(); it != view.end(); ++it) {
    ++count;
  }
  EXPECT_EQ(count, 3u);
}

TEST(ObjectStoreTest, EntryTimestampsViewEmpty) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  auto view = store.entryTimestamps(id);
  EXPECT_TRUE(view.empty());
  EXPECT_EQ(view.size(), 0u);
}

TEST(ObjectStoreTest, RemoveTopicWaitsForOutstandingTimestampsView) {
  ObjectStore store;
  auto id = registerTestTopic(store, "cam/video");
  for (int i = 0; i < 5; ++i) {
    store.pushOwned(id, i * 1'000'000, makePayload(8));
  }

  // An EntryTimestampsView holds the series read lock plus a raw pointer into the
  // timestamp vector. removeTopic destroys the series (mutex + vector); if it does
  // not first drain outstanding readers it frees memory the view still references
  // (use-after-free) and destroys a still-locked mutex (UB). The observable
  // contract of the fix: removeTopic must not complete while a view is alive.
  std::atomic<bool> remove_done{false};
  std::thread remover;
  {
    auto view = store.entryTimestamps(id);
    ASSERT_EQ(view.size(), 5u);

    remover = std::thread([&] {
      store.removeTopic(id);
      remove_done.store(true, std::memory_order_release);
    });

    // The remover must block until the view is released. A too-early completion
    // (the bug) happens in microseconds, so a short wait reliably catches it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(remove_done.load(std::memory_order_acquire))
        << "removeTopic completed while an EntryTimestampsView was still alive (UAF window)";
    EXPECT_EQ(view.size(), 5u);  // view data stays valid for its whole lifetime
  }

  remover.join();
  EXPECT_TRUE(remove_done.load(std::memory_order_acquire));
  EXPECT_EQ(store.entryCount(id), 0u);
}

// =========================================================================
// pushLazy
// =========================================================================

TEST(ObjectStoreTest, PushLazyResolves) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int call_count = 0;
  store.pushLazy(id, 100, [&call_count]() -> sdk::PayloadView {
    ++call_count;
    return sdk::makePayloadView({0xDE, 0xAD});
  });

  EXPECT_EQ(call_count, 0);
  auto r = store.latestAt(id, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(r->payload.bytes.size(), 2u);
  EXPECT_EQ(r->payload.bytes[0], 0xDE);
}

// Regression: anchor type-erasure must survive resolveEntry. The anchor here
// is a shared_ptr<TestBuffer> (not vector); a prior static_pointer_cast to
// vector would UB.
TEST(ObjectStoreTest, PushLazyPreservesAnchorType) {
  struct TestBuffer {
    std::array<uint8_t, 4> bytes{0x11, 0x22, 0x33, 0x44};
  };
  ObjectStore store;
  auto id = registerTestTopic(store);

  auto buffer = std::make_shared<TestBuffer>();
  std::weak_ptr<TestBuffer> weak_buffer = buffer;

  store.pushLazy(id, 100, [buffer]() -> sdk::PayloadView {
    return sdk::PayloadView{
        Span<const uint8_t>{buffer->bytes.data(), buffer->bytes.size()},
        sdk::BufferAnchor{buffer},
    };
  });

  auto r = store.latestAt(id, 100);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->payload.bytes.size(), 4u);
  EXPECT_EQ(r->payload.bytes[0], 0x11);
  EXPECT_EQ(r->payload.bytes[3], 0x44);
  EXPECT_FALSE(weak_buffer.expired());  // anchor still holds the buffer alive
}

// Regression: the producer's Span is a sub-range of the anchor's storage.
// resolveEntry must propagate it verbatim — not the anchor's full extent.
TEST(ObjectStoreTest, PushLazyHonorsSpanSubview) {
  ObjectStore store;
  auto id = registerTestTopic(store);

  auto chunk = std::make_shared<std::vector<uint8_t>>(100);
  for (size_t i = 0; i < chunk->size(); ++i) {
    (*chunk)[i] = static_cast<uint8_t>(i);
  }

  store.pushLazy(id, 100, [chunk]() -> sdk::PayloadView {
    return sdk::PayloadView{
        Span<const uint8_t>{chunk->data() + 20, 10},  // bytes [20, 30)
        sdk::BufferAnchor{chunk},
    };
  });

  auto r = store.latestAt(id, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->payload.bytes.size(), 10u);
  EXPECT_EQ(r->payload.bytes.data(), chunk->data() + 20);
  EXPECT_EQ(r->payload.bytes[0], 20);
  EXPECT_EQ(r->payload.bytes[9], 29);
}

// =========================================================================
// Out-of-order ingest is lossless
// =========================================================================
// Real object streams legitimately regress in time — notably multi-publisher
// /tf, whose per-message publish timestamps interleave. Like the scalar engine
// (whose appends accept timestamp regressions), ObjectStore must keep, not drop,
// an out-of-order push. It inserts it at the sorted position so entry_timestamps
// stays ordered for at-or-before lookup.

TEST(ObjectStoreTest, OutOfOrderPushAcceptedLossless) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  EXPECT_TRUE(store.pushOwned(id, 200, makePayload(4, 0xBB)).has_value());
  auto result = store.pushOwned(id, 100, makePayload(4, 0xAA));  // regression
  EXPECT_TRUE(result.has_value());                               // accepted, not dropped
  EXPECT_EQ(store.entryCount(id), 2u);                           // both retained
}

TEST(ObjectStoreTest, OutOfOrderPushSortsForLookup) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  // Interleaved, out-of-order arrival: 300, 100, 200.
  store.pushOwned(id, 300, makePayload(1, 0x33));
  store.pushOwned(id, 100, makePayload(1, 0x11));
  store.pushOwned(id, 200, makePayload(1, 0x22));
  ASSERT_EQ(store.entryCount(id), 3u);

  // latestAt resolves to the at-or-before entry regardless of arrival order.
  auto a = store.latestAt(id, 150);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->payload.bytes[0], 0x11);
  auto b = store.latestAt(id, 250);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->payload.bytes[0], 0x22);
  auto c = store.latestAt(id, 999);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->payload.bytes[0], 0x33);
  EXPECT_FALSE(store.latestAt(id, 50).has_value());

  // entry_timestamps is exposed in sorted order.
  auto stamps = store.entryTimestamps(id);
  ASSERT_EQ(stamps.size(), 3u);
  EXPECT_EQ(stamps[0], 100);
  EXPECT_EQ(stamps[1], 200);
  EXPECT_EQ(stamps[2], 300);
}

TEST(ObjectStoreTest, EqualTimestampAllowed) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4));
  auto result = store.pushOwned(id, 100, makePayload(4));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(store.entryCount(id), 2u);
}

// At a duplicated timestamp, both the newest-timestamp lookup (latestAt) and the
// high-water arrival cursor (maxUidAtOrBefore) must resolve the LAST arrival, not
// the first. A regression to first-of-group (lower_bound / min-UID) survives
// EqualTimestampAllowed above, which only counts entries.
TEST(ObjectStoreTest, DuplicateTimestampResolvesLastArrival) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0x11));  // first arrival at ts=100
  store.pushOwned(id, 100, makePayload(4, 0x22));  // second arrival, same ts

  auto latest = store.latestAt(id, 100);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->payload.bytes[0], 0x22) << "latestAt must pick the last arrival at a duplicated timestamp";

  const SequentialUID high = store.maxUidAtOrBefore(id, 100);
  ASSERT_TRUE(high.valid());
  auto by_uid = store.at(id, high);
  ASSERT_TRUE(by_uid.has_value());
  EXPECT_EQ(by_uid->payload.bytes[0], 0x22) << "maxUidAtOrBefore must resolve the last-arrival (max) UID";
}

// =========================================================================
// Owning handle survives eviction
// =========================================================================

TEST(ObjectStoreTest, HandleSurvivesEviction) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.pushOwned(id, 100, makePayload(4, 0xAA));
  store.pushOwned(id, 200, makePayload(4, 0xBB));

  auto handle = store.latestAt(id, 100);
  ASSERT_TRUE(handle.has_value());
  EXPECT_EQ(handle->payload.bytes[0], 0xAA);

  store.evictBefore(id, 150);
  EXPECT_EQ(store.entryCount(id), 1u);

  EXPECT_EQ(handle->payload.bytes.size(), 4u);
  EXPECT_EQ(handle->payload.bytes[0], 0xAA);
}

// =========================================================================
// evictBefore
// =========================================================================

TEST(ObjectStoreTest, EvictBefore) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  for (int i = 0; i < 10; ++i) {
    store.pushOwned(id, static_cast<Timestamp>(i) * 100, makePayload(8));
  }
  EXPECT_EQ(store.entryCount(id), 10u);

  store.evictBefore(id, 500);
  EXPECT_EQ(store.entryCount(id), 5u);
  auto [t_min, t_max] = store.timeRange(id);
  EXPECT_EQ(t_min, 500);
  EXPECT_EQ(t_max, 900);
}

// =========================================================================
// removeTopic / clear
// =========================================================================

TEST(ObjectStoreTest, RemoveTopic) {
  ObjectStore store;
  auto id = registerTestTopic(store, "to_remove");
  store.pushOwned(id, 100, makePayload(4));
  store.removeTopic(id);
  EXPECT_EQ(store.entryCount(id), 0u);
  EXPECT_TRUE(store.listTopics().empty());
}

TEST(ObjectStoreTest, Clear) {
  ObjectStore store;
  registerTestTopic(store, "a");
  registerTestTopic(store, "b");
  EXPECT_EQ(store.listTopics().size(), 2u);
  store.clear();
  EXPECT_TRUE(store.listTopics().empty());
}

// =========================================================================
// Retention budget
// =========================================================================

TEST(ObjectStoreTest, TimeWindowRetention) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.time_window_ns = 2000, .max_memory_bytes = 0});

  for (int i = 0; i < 100; ++i) {
    store.pushOwned(id, static_cast<Timestamp>(i) * 100, makePayload(8));
  }

  auto [t_min, t_max] = store.timeRange(id);
  EXPECT_GE(t_min, t_max - 2000);
}

TEST(ObjectStoreTest, MemoryRetention) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.time_window_ns = 0, .max_memory_bytes = 500});

  for (int i = 0; i < 100; ++i) {
    store.pushOwned(id, static_cast<Timestamp>(i) * 100, makePayload(100));
  }

  EXPECT_LE(store.memoryUsage(id), 500u);
}

TEST(ObjectStoreTest, DefaultBudgetNoEviction) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  for (int i = 0; i < 100; ++i) {
    store.pushOwned(id, static_cast<Timestamp>(i) * 100, makePayload(100));
  }
  EXPECT_EQ(store.entryCount(id), 100u);
}

TEST(ObjectStoreTest, MaxEntriesRetentionKeepsOnlyLatest) {
  // A producer that republishes one snapshot per topic at a sentinel timestamp
  // (e.g. plot markers, always ts=0) would otherwise append a blob per push.
  // A keep-last-1 budget evicts superseded snapshots so the topic holds exactly 1.
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.max_entries = 1});

  for (int i = 0; i < 5; ++i) {
    store.pushOwned(id, 0, makePayload(4, static_cast<uint8_t>(i)));
  }

  EXPECT_EQ(store.entryCount(id), 1u);
  auto latest = store.latestAt(id, 1);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->payload.bytes[0], 4);  // the newest survives
}

// Retention runs INSIDE pushOwned: an out-of-order push whose timestamp lands below
// the time-window floor is sorted-inserted and then immediately front-evicted on the
// same call. That insert-then-evict of the OOO slot must leave uid_order consistent,
// so the surviving entries stay cursor-reachable in ascending-UID order.
TEST(ObjectStoreTest, RetentionEvictsOutOfOrderPushBelowFloorKeepsCursorConsistent) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.time_window_ns = 1500, .max_memory_bytes = 0});
  store.pushOwned(id, 1000, makePayload(4));
  store.pushOwned(id, 2000, makePayload(4));
  store.pushOwned(id, 3000, makePayload(4));  // floor is now 1500 → 1000 evicted
  ASSERT_EQ(store.entryCount(id), 2u);

  // Out-of-order and far below the floor: inserted, then self-evicted on this push.
  ASSERT_TRUE(store.pushOwned(id, 500, makePayload(4, 0x55)).has_value());
  EXPECT_EQ(store.entryCount(id), 2u) << "OOO entry below the retention floor should self-evict";

  auto walked = walkUids(store, id);
  EXPECT_EQ(walked.size(), store.entryCount(id));
  EXPECT_EQ(walked, (std::vector<Timestamp>{2000, 3000})) << "insert-then-evict corrupted uid_order";
  EXPECT_FALSE(store.latestAt(id, 500).has_value()) << "self-evicted OOO entry must not resolve";
}

// A retention jump or an explicit eviction that expires many entries at once keeps exactly what evicting the oldest
// one at a time kept: the same survivors at the same positions, the memory accounting, and a warm cache that names an
// evicted entry released. The model below is the one-at-a-time loop.
TEST(ObjectStoreTest, RetentionJumpKeepsWhatOneAtATimeEvictionKept) {
  struct Kept {
    SequentialUID uid;
    Timestamp timestamp = 0;
    size_t bytes = 0;
  };
  std::mt19937 random(11);
  const auto draw = [&random](int64_t lo, int64_t hi) {
    return std::uniform_int_distribution<int64_t>(lo, hi)(random);
  };
  for (int round = 0; round < 250; ++round) {
    ObjectStore store;
    const auto id = registerTestTopic(store);
    Timestamp newest = 0;
    const int size = static_cast<int>(draw(1, 80));
    for (int index = 0; index < size; ++index) {
      newest += draw(0, 3);
      const Timestamp stamp = draw(0, 3) == 0 ? draw(0, newest) : newest;  // some out of order, some equal
      ASSERT_TRUE(store.pushOwned(id, stamp, makePayload(static_cast<size_t>(draw(0, 64)))).has_value());
    }
    std::vector<Kept> model;
    for (const auto& entry : store.rangeByTime(id, -1, std::numeric_limits<Timestamp>::max())) {
      model.push_back({entry.uid, entry.timestamp, store.at(id, entry.uid)->payload.bytes.size()});
    }
    ASSERT_EQ(model.size(), static_cast<size_t>(size));
    // Warm the cache on an entry the jump may expire.
    const Timestamp probe = model[static_cast<size_t>(draw(0, size - 1))].timestamp;
    ASSERT_TRUE(store.latestAt(id, probe).has_value());

    const auto evict_while = [&model](const auto& expired) {
      while (!model.empty() && expired()) {
        model.erase(model.begin());
      }
    };
    const auto model_bytes = [&model] {
      size_t total = 0;
      for (const Kept& kept : model) {
        total += kept.bytes;
      }
      return total;
    };
    if (round % 5 == 4) {
      const Timestamp threshold = draw(0, newest + 1);
      store.evictBefore(id, threshold);
      evict_while([&] { return model.front().timestamp < threshold; });
    } else {
      RetentionBudget budget;
      const int kind = round % 5;
      const auto bytes = static_cast<size_t>(draw(0, 64));
      if (kind == 0 || kind == 3) {
        budget.time_window_ns = draw(1, newest + 10);
      }
      if (kind == 1 || kind == 3) {
        // At least the new entry's size, so the new entry itself stays and names its UID below.
        budget.max_memory_bytes = static_cast<size_t>(
            draw(std::max<int64_t>(1, static_cast<int64_t>(bytes)), static_cast<int64_t>(model_bytes()) + 64));
      }
      if (kind == 2 || kind == 3) {
        budget.max_entries = static_cast<size_t>(draw(1, size + 1));
      }
      store.setRetentionBudget(id, budget);
      const Timestamp jump = newest + draw(1, 10);
      ASSERT_TRUE(store.pushOwned(id, jump, makePayload(bytes)).has_value());
      model.push_back({store.rangeByTime(id, jump - 1, jump).back().uid, jump, bytes});
      if (budget.time_window_ns > 0) {
        evict_while([&] { return model.front().timestamp < jump - budget.time_window_ns; });
      }
      if (budget.max_memory_bytes > 0) {
        evict_while([&] { return model_bytes() > budget.max_memory_bytes; });
      }
      if (budget.max_entries > 0) {
        evict_while([&] { return model.size() > budget.max_entries; });
      }
    }

    const auto kept = store.rangeByTime(id, -1, std::numeric_limits<Timestamp>::max());
    ASSERT_EQ(kept.size(), model.size()) << "round " << round;
    for (size_t index = 0; index < kept.size(); ++index) {
      EXPECT_EQ(kept[index].uid, model[index].uid) << "round " << round << " position " << index;
      EXPECT_EQ(store.indexOf(id, model[index].uid), index);
    }
    EXPECT_EQ(store.memoryUsage(id), model_bytes()) << "round " << round;
    EXPECT_EQ(
        store.firstSequentialUID(id),
        model.empty() ? SequentialUID{}
                      : std::min_element(model.begin(), model.end(), [](const Kept& lhs, const Kept& rhs) {
                          return lhs.uid < rhs.uid;
                        })->uid);
    std::optional<SequentialUID> expected_latest;
    for (const Kept& entry : model) {
      if (entry.timestamp <= probe) {
        expected_latest = entry.uid;
      }
    }
    const auto latest = store.latestAt(id, probe);
    ASSERT_EQ(latest.has_value(), expected_latest.has_value()) << "round " << round;
    if (latest) {
      EXPECT_EQ(latest->sequential_uid, *expected_latest);
    }
  }
}

TEST(ObjectStoreTest, LazyEntriesZeroMemory) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  for (int i = 0; i < 10; ++i) {
    store.pushLazy(id, static_cast<Timestamp>(i) * 100, []() -> sdk::PayloadView {
      return sdk::makePayloadView(makePayload(1000));
    });
  }
  EXPECT_EQ(store.memoryUsage(id), 0u);
}

// A pure-lazy entry (the policy file-backed PJ.VideoFrame topics use) must keep
// its bytes NON-resident: the store holds only the fetcher closure, never the
// payload. Each read re-invokes the fetcher, and once the caller drops the
// resolved entry the anchor is the sole owner so the bytes are freed — which is
// exactly what lets a locator-only fetcher read one access unit from a file on
// demand without the whole video ever landing on the heap.
TEST(ObjectStoreTest, LazyFetcherNotPinnedAcrossReads) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  int fetch_count = 0;
  store.pushLazy(id, 0, [&fetch_count]() -> sdk::PayloadView {
    ++fetch_count;
    return sdk::makePayloadView(makePayload(64 * 1024));
  });

  EXPECT_EQ(store.memoryUsage(id), 0u);  // closure only — payload not counted

  std::weak_ptr<const void> anchor_probe;
  {
    auto resolved = store.at(id, 0);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->payload.bytes.size(), 64u * 1024u);
    anchor_probe = resolved->payload.anchor;
  }
  EXPECT_TRUE(anchor_probe.expired()) << "ObjectStore pinned the lazy payload past the read";

  // A second read fetches fresh rather than returning a cached buffer.
  auto second = store.at(id, 0);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(fetch_count, 2);
}

// =========================================================================
// Concurrency (basic smoke test — M2 will add thorough tests)
// =========================================================================

TEST(ObjectStoreTest, ConcurrentReadWriteSmoke) {
  ObjectStore store;
  auto id = registerTestTopic(store);

  constexpr int kPushCount = 1000;
  std::thread writer([&]() {
    for (int i = 0; i < kPushCount; ++i) {
      store.pushOwned(id, static_cast<Timestamp>(i) * 100, makePayload(16));
    }
  });

  std::thread reader([&]() {
    for (int i = 0; i < kPushCount; ++i) {
      store.latestAt(id, static_cast<Timestamp>(i) * 100);
      store.entryCount(id);
    }
  });

  writer.join();
  reader.join();
  EXPECT_EQ(store.entryCount(id), static_cast<size_t>(kPushCount));
}

// =========================================================================
// Cross-store flush (flushTo)
// =========================================================================

namespace {

ObjectTopicId registerSameDescriptor(
    ObjectStore& store, DatasetId dataset_id = 1, const std::string& name = "test/topic") {
  auto id_or = store.registerTopic({.dataset_id = dataset_id, .topic_name = name, .metadata_json = "{}"});
  EXPECT_TRUE(id_or.has_value());
  return *id_or;
}

}  // namespace

TEST(ObjectStoreFlushTest, BasicMoveOwnedEntries) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  src.pushOwned(src_id, 100, makePayload(8, 0xAA));
  src.pushOwned(src_id, 200, makePayload(8, 0xBB));
  src.pushOwned(src_id, 300, makePayload(8, 0xCC));

  ASSERT_EQ(src.entryCount(src_id), 3u);
  ASSERT_EQ(dst.entryCount(dst_id), 0u);

  auto result = src.flushTo(dst);
  ASSERT_TRUE(result.has_value()) << result.error();

  EXPECT_EQ(src.entryCount(src_id), 0u);
  EXPECT_EQ(dst.entryCount(dst_id), 3u);

  auto e0 = dst.at(dst_id, 0);
  ASSERT_TRUE(e0.has_value());
  EXPECT_EQ(e0->timestamp, 100);
  EXPECT_EQ(e0->payload.bytes.size(), 8u);
  EXPECT_EQ(e0->payload.bytes[0], 0xAA);

  auto e2 = dst.at(dst_id, 2);
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->timestamp, 300);
  EXPECT_EQ(e2->payload.bytes[0], 0xCC);
}

TEST(ObjectStoreFlushTest, PreservesTopicRegistrationOnSrc) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  registerSameDescriptor(dst);

  src.pushOwned(src_id, 100, makePayload(4));

  ASSERT_TRUE(src.flushTo(dst).has_value());

  // src is empty but the topic is still registered — the same id can accept new pushes.
  EXPECT_EQ(src.entryCount(src_id), 0u);
  auto post_push = src.pushOwned(src_id, 400, makePayload(4));
  EXPECT_TRUE(post_push.has_value());
  EXPECT_EQ(src.entryCount(src_id), 1u);
}

TEST(ObjectStoreFlushTest, AppendsToExistingDstEntries) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  dst.pushOwned(dst_id, 50, makePayload(4, 0x11));
  dst.pushOwned(dst_id, 100, makePayload(4, 0x22));
  src.pushOwned(src_id, 200, makePayload(4, 0x33));
  src.pushOwned(src_id, 300, makePayload(4, 0x44));

  ASSERT_TRUE(src.flushTo(dst).has_value());

  EXPECT_EQ(dst.entryCount(dst_id), 4u);
  EXPECT_EQ(dst.at(dst_id, 0)->timestamp, 50);
  EXPECT_EQ(dst.at(dst_id, 1)->timestamp, 100);
  EXPECT_EQ(dst.at(dst_id, 2)->timestamp, 200);
  EXPECT_EQ(dst.at(dst_id, 3)->timestamp, 300);
  const auto uid0 = dst.at(dst_id, 0)->sequential_uid;
  const auto uid1 = dst.at(dst_id, 1)->sequential_uid;
  const auto uid2 = dst.at(dst_id, 2)->sequential_uid;
  const auto uid3 = dst.at(dst_id, 3)->sequential_uid;
  EXPECT_LT(uid0, uid1);
  EXPECT_LT(uid1, uid2);
  EXPECT_LT(uid2, uid3);
  EXPECT_EQ(dst.at(dst_id, uid2)->timestamp, 200);
  EXPECT_EQ(dst.at(dst_id, uid3)->timestamp, 300);
}

TEST(ObjectStoreFlushTest, RejectsMonotonicityViolation) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  dst.pushOwned(dst_id, 200, makePayload(4));
  src.pushOwned(src_id, 100, makePayload(4));  // earlier than dst's last

  auto result = src.flushTo(dst);
  EXPECT_FALSE(result.has_value());

  // Neither store is mutated on failure.
  EXPECT_EQ(src.entryCount(src_id), 1u);
  EXPECT_EQ(dst.entryCount(dst_id), 1u);
}

TEST(ObjectStoreFlushTest, RejectsUnknownTopicInDst) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src, 1, "missing/topic");
  // dst has a different topic.
  registerSameDescriptor(dst, 1, "other/topic");

  src.pushOwned(src_id, 100, makePayload(4));

  auto result = src.flushTo(dst);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(src.entryCount(src_id), 1u);
}

TEST(ObjectStoreFlushTest, EmptySourceSeriesSkipped) {
  ObjectStore src, dst;
  auto src_id_with_data = registerSameDescriptor(src, 1, "with/data");
  registerSameDescriptor(src, 1, "empty/series");  // registered but no pushes

  auto dst_id_with_data = registerSameDescriptor(dst, 1, "with/data");
  // dst does NOT register "empty/series" — flush should still succeed since src
  // has no entries for it.

  src.pushOwned(src_id_with_data, 100, makePayload(4));

  auto result = src.flushTo(dst);
  EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
  EXPECT_EQ(dst.entryCount(dst_id_with_data), 1u);
}

TEST(ObjectStoreFlushTest, RejectsSameStore) {
  ObjectStore store;
  auto id = registerSameDescriptor(store);
  store.pushOwned(id, 100, makePayload(4));

  auto result = store.flushTo(store);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(store.entryCount(id), 1u);
}

TEST(ObjectStoreFlushTest, MultipleTopicsFlushed) {
  ObjectStore src, dst;
  auto src_a = registerSameDescriptor(src, 1, "topic/a");
  auto src_b = registerSameDescriptor(src, 1, "topic/b");
  auto dst_a = registerSameDescriptor(dst, 1, "topic/a");
  auto dst_b = registerSameDescriptor(dst, 1, "topic/b");

  src.pushOwned(src_a, 100, makePayload(4, 0xAA));
  src.pushOwned(src_b, 100, makePayload(4, 0xBB));
  src.pushOwned(src_a, 200, makePayload(4, 0xCC));
  src.pushOwned(src_b, 200, makePayload(4, 0xDD));

  ASSERT_TRUE(src.flushTo(dst).has_value());

  EXPECT_EQ(dst.entryCount(dst_a), 2u);
  EXPECT_EQ(dst.entryCount(dst_b), 2u);
  EXPECT_EQ(dst.at(dst_a, 0)->payload.bytes[0], 0xAA);
  EXPECT_EQ(dst.at(dst_b, 1)->payload.bytes[0], 0xDD);
}

TEST(ObjectStoreFlushTest, LazyEntriesPreserveSemantics) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  int src_invocations = 0;
  src.pushLazy(src_id, 100, [&src_invocations]() -> sdk::PayloadView {
    ++src_invocations;
    return sdk::makePayloadView({0xDE, 0xAD, 0xBE, 0xEF});
  });

  // Flush itself must NOT invoke the closure — it just moves the std::function.
  EXPECT_EQ(src_invocations, 0);
  ASSERT_TRUE(src.flushTo(dst).has_value());
  EXPECT_EQ(src_invocations, 0);

  EXPECT_EQ(src.entryCount(src_id), 0u);
  EXPECT_EQ(dst.entryCount(dst_id), 1u);

  // Reading from dst invokes the closure once, exactly like in src.
  auto resolved = dst.latestAt(dst_id, 100);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(src_invocations, 1);
  ASSERT_EQ(resolved->payload.bytes.size(), 4u);
  EXPECT_EQ(resolved->payload.bytes[0], 0xDE);
}

TEST(ObjectStoreFlushTest, RetentionBudgetAppliedAfterFlush) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  // Time-window retention of 100 ns on dst: anything older than newest - 100 evicts.
  dst.setRetentionBudget(dst_id, RetentionBudget{.time_window_ns = 100, .max_memory_bytes = 0});

  src.pushOwned(src_id, 100, makePayload(4));
  src.pushOwned(src_id, 150, makePayload(4));
  src.pushOwned(src_id, 250, makePayload(4));  // newest after flush; evict anything < 150.

  ASSERT_TRUE(src.flushTo(dst).has_value());

  // After flush: newest is 250, threshold = 150, so the entry at 100 evicts.
  EXPECT_EQ(dst.entryCount(dst_id), 2u);
  EXPECT_EQ(dst.at(dst_id, 0)->timestamp, 150);
  EXPECT_EQ(dst.at(dst_id, 1)->timestamp, 250);
}

TEST(ObjectStoreFlushTest, ZeroCopyOwnershipChainSurvives) {
  // A shared_ptr handed in via pushOwned is shared between the entry's payload
  // and any consumer of `at()`. After flushTo, the same shared_ptr instance
  // must be the one observed in the destination — no copy, just refcount
  // transfer through the move of the underlying ObjectEntry.
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);

  src.pushOwned(src_id, 100, makePayload(16, 0x77));
  auto pre_handle = src.at(src_id, 0);
  ASSERT_TRUE(pre_handle.has_value());
  const auto* pre_ptr = pre_handle->payload.anchor.get();

  ASSERT_TRUE(src.flushTo(dst).has_value());

  auto post_handle = dst.at(dst_id, 0);
  ASSERT_TRUE(post_handle.has_value());
  EXPECT_EQ(post_handle->payload.anchor.get(), pre_ptr) << "shared_ptr identity must survive the flush";
}

// A flush drains src; its uid_order must be cleared too, else the next push into
// the emptied src appends a stale position and corrupts the cursor.
TEST(ObjectStoreFlushTest, SrcUidOrderClearedAfterFlushAllowsSubsequentPushes) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  registerSameDescriptor(dst);
  src.pushOwned(src_id, 100, makePayload(4));
  src.pushOwned(src_id, 200, makePayload(4));
  ASSERT_TRUE(src.flushTo(dst).has_value());
  // Push MORE THAN ONE entry into the drained src. A single survivor resolves to
  // entries_[0] even if flushTo left a stale uid_order slot, so a one-entry check
  // passes vacuously; only a full walk whose length matches entryCount catches an
  // uncleared uid_order (the debug invariant assert that would otherwise trip is
  // compiled out under NDEBUG / RelWithDebInfo).
  ASSERT_TRUE(src.pushOwned(src_id, 400, makePayload(4, 0x77)).has_value());
  ASSERT_TRUE(src.pushOwned(src_id, 500, makePayload(4, 0x88)).has_value());
  auto walked = walkUids(src, src_id);
  EXPECT_EQ(walked.size(), src.entryCount(src_id)) << "stale uid_order from flushTo corrupted src's cursor";
  EXPECT_EQ(walked, (std::vector<Timestamp>{400, 500}));
}

// The flush-dst rebuild must cope with a non-identity dst prefix (an earlier
// out-of-order insert): every entry stays visited once in ascending UID order.
TEST(ObjectStoreFlushTest, UidWalkAfterAppendingFlushVisitsAllEntriesInOrder) {
  ObjectStore src, dst;
  auto src_id = registerSameDescriptor(src);
  auto dst_id = registerSameDescriptor(dst);
  // Seed dst with an out-of-order entry so its uid_order prefix is non-identity.
  dst.pushOwned(dst_id, 10, makePayload(4));
  dst.pushOwned(dst_id, 30, makePayload(4));
  dst.pushOwned(dst_id, 20, makePayload(4));  // OOO
  // Capture the pre-existing dst UIDs BEFORE the flush. flushTo must APPEND the
  // moved src entries (rebuild uid_order) and leave dst's own UIDs untouched — a
  // consumer cursor holds them. A destination-wide re-UID (reuidSeriesLocked
  // instead of rebuildUidOrderLocked) would renumber these and pass the walk
  // check below, so pin identity here.
  const SequentialUID uid10 = dst.latestAt(dst_id, 10)->sequential_uid;
  const SequentialUID uid20 = dst.latestAt(dst_id, 20)->sequential_uid;
  const SequentialUID uid30 = dst.latestAt(dst_id, 30)->sequential_uid;
  // src timestamps must respect monotonicity (>= dst.back() == 30).
  src.pushOwned(src_id, 40, makePayload(4));
  src.pushOwned(src_id, 50, makePayload(4));
  ASSERT_TRUE(src.flushTo(dst).has_value());

  // Pre-existing dst UIDs still resolve the same entries (stability).
  ASSERT_TRUE(dst.at(dst_id, uid10).has_value()) << "dst UID renumbered by flush";
  EXPECT_EQ(dst.at(dst_id, uid10)->timestamp, 10);
  EXPECT_EQ(dst.at(dst_id, uid20)->timestamp, 20);
  EXPECT_EQ(dst.at(dst_id, uid30)->timestamp, 30);

  auto walked = walkUids(dst, dst_id);
  EXPECT_EQ(walked.size(), dst.entryCount(dst_id));
  std::sort(walked.begin(), walked.end());
  EXPECT_EQ(walked, (std::vector<Timestamp>{10, 20, 30, 40, 50}));
}

// =========================================================================
// replaceDatasetFrom: in-place object-data replace keeping ObjectTopicId stable
// (the object-store half of reload).
// =========================================================================

TEST(ObjectStoreReplaceTest, EntryMoveAndIdPreservation) {
  ObjectStore primary;
  ObjectStore staged;
  auto primary_id = primary.registerTopic({.dataset_id = 1, .topic_name = "cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(primary_id.has_value());
  ASSERT_TRUE(primary.pushOwned(*primary_id, 10, makePayload(4, 0x11)).has_value());
  const auto old_primary_uid = primary.latestAt(*primary_id, 10)->sequential_uid;

  auto staged_id = staged.registerTopic({.dataset_id = 7, .topic_name = "cam/image", .metadata_json = R"({"k":1})"});
  ASSERT_TRUE(staged_id.has_value());
  for (Timestamp t : {Timestamp{100}, Timestamp{200}, Timestamp{300}}) {
    ASSERT_TRUE(staged.pushOwned(*staged_id, t, makePayload(4, 0x22)).has_value());
  }

  auto res = primary.replaceDatasetFrom(staged, /*staged_id=*/7, /*primary_id=*/1);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(primary.entryCount(*primary_id), 3u) << "entries replaced with staged's";
  EXPECT_EQ(staged.entryCount(*staged_id), 0u) << "staged drained";
  EXPECT_EQ(primary.descriptor(*primary_id).metadata_json, R"({"k":1})") << "reloaded metadata adopted";
  ASSERT_EQ(res->remapped.size(), 1u);
  EXPECT_EQ(res->remapped[0].first.id, staged_id->id);
  EXPECT_EQ(res->remapped[0].second.id, primary_id->id) << "primary ObjectTopicId preserved";
  EXPECT_TRUE(res->removed_topics.empty());
  EXPECT_FALSE(primary.at(*primary_id, old_primary_uid).has_value()) << "replaced entries get fresh primary UIDs";
  const auto uid0 = primary.at(*primary_id, 0)->sequential_uid;
  const auto uid1 = primary.at(*primary_id, 1)->sequential_uid;
  const auto uid2 = primary.at(*primary_id, 2)->sequential_uid;
  EXPECT_EQ(primary.firstSequentialUID(*primary_id), uid0);
  EXPECT_LT(uid0, uid1);
  EXPECT_LT(uid1, uid2);
  EXPECT_EQ(primary.at(*primary_id, uid0)->timestamp, 100);
  EXPECT_EQ(primary.at(*primary_id, uid2)->timestamp, 300);
}

TEST(ObjectStoreReplaceTest, RemovedTopic) {
  ObjectStore primary;
  ObjectStore staged;
  auto keep = primary.registerTopic({.dataset_id = 1, .topic_name = "cam/image", .metadata_json = "{}"});
  auto drop = primary.registerTopic({.dataset_id = 1, .topic_name = "cam/depth", .metadata_json = "{}"});
  ASSERT_TRUE(keep.has_value());
  ASSERT_TRUE(drop.has_value());
  ASSERT_TRUE(primary.pushOwned(*keep, 10, makePayload(4)).has_value());
  ASSERT_TRUE(primary.pushOwned(*drop, 10, makePayload(4)).has_value());

  auto staged_keep = staged.registerTopic({.dataset_id = 5, .topic_name = "cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(staged_keep.has_value());
  ASSERT_TRUE(staged.pushOwned(*staged_keep, 100, makePayload(4)).has_value());

  auto res = primary.replaceDatasetFrom(staged, 5, 1);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->removed_topics.size(), 1u);
  EXPECT_EQ(res->removed_topics[0].id, drop->id);
  EXPECT_TRUE(primary.descriptor(*drop).topic_name.empty()) << "removed topic gone from store";
  EXPECT_EQ(primary.descriptor(*keep).topic_name, "cam/image");
  EXPECT_EQ(primary.entryCount(*keep), 1u);
}

TEST(ObjectStoreReplaceTest, AddedTopic) {
  ObjectStore primary;
  ObjectStore staged;
  auto img = primary.registerTopic({.dataset_id = 1, .topic_name = "cam/image", .metadata_json = "{}"});
  ASSERT_TRUE(img.has_value());
  ASSERT_TRUE(primary.pushOwned(*img, 10, makePayload(4)).has_value());

  auto s_img = staged.registerTopic({.dataset_id = 3, .topic_name = "cam/image", .metadata_json = "{}"});
  auto s_lidar = staged.registerTopic({.dataset_id = 3, .topic_name = "cam/lidar", .metadata_json = "{}"});
  ASSERT_TRUE(s_img.has_value());
  ASSERT_TRUE(s_lidar.has_value());
  ASSERT_TRUE(staged.pushOwned(*s_img, 100, makePayload(4)).has_value());
  ASSERT_TRUE(staged.pushOwned(*s_lidar, 100, makePayload(4)).has_value());

  auto res = primary.replaceDatasetFrom(staged, 3, 1);
  ASSERT_TRUE(res.has_value());
  // The new lidar topic is registered under the primary dataset and reported in
  // the remap as (staged lidar id -> its fresh primary id).
  auto lidar_in_primary = primary.findTopic(1, "cam/lidar");
  ASSERT_TRUE(lidar_in_primary.has_value());
  EXPECT_EQ(primary.entryCount(*lidar_in_primary), 1u);
  bool lidar_remapped = false;
  for (const auto& [staged_id, primary_topic_id] : res->remapped) {
    if (staged_id.id == s_lidar->id) {
      EXPECT_EQ(primary_topic_id.id, lidar_in_primary->id);
      lidar_remapped = true;
    }
  }
  EXPECT_TRUE(lidar_remapped) << "added topic must appear in the remap";
  EXPECT_EQ(primary.findTopic(1, "cam/image")->id, img->id) << "existing topic id preserved";
}

// =========================================================================
// series_index_ invariants (M.45 regression)
// =========================================================================

// Register 3 topics, push entries, remove the middle topic, re-register a
// topic with the same name as the removed one, and verify that all surviving
// topics resolve correctly and the removed id returns empty/nullopt.
TEST(ObjectStoreIndexTest, RemoveMiddleThenReregisterSameName) {
  ObjectStore store;
  auto id_a = store.registerTopic({.dataset_id = 1, .topic_name = "topic/a", .metadata_json = "{}"});
  auto id_b = store.registerTopic({.dataset_id = 1, .topic_name = "topic/b", .metadata_json = "{}"});
  auto id_c = store.registerTopic({.dataset_id = 1, .topic_name = "topic/c", .metadata_json = "{}"});
  ASSERT_TRUE(id_a.has_value());
  ASSERT_TRUE(id_b.has_value());
  ASSERT_TRUE(id_c.has_value());

  ASSERT_TRUE(store.pushOwned(*id_a, 100, makePayload(4, 0xAA)).has_value());
  ASSERT_TRUE(store.pushOwned(*id_b, 100, makePayload(4, 0xBB)).has_value());
  ASSERT_TRUE(store.pushOwned(*id_c, 100, makePayload(4, 0xCC)).has_value());

  // Remove the middle topic; id_b must become unreachable.
  store.removeTopic(*id_b);
  EXPECT_EQ(store.entryCount(*id_b), 0u);
  EXPECT_FALSE(store.latestAt(*id_b, 200).has_value());
  EXPECT_FALSE(store.at(*id_b, 0).has_value());
  EXPECT_FALSE(store.findTopic(1, "topic/b").has_value());
  EXPECT_EQ(store.descriptor(*id_b).topic_name, "");

  // Survivors must still resolve correctly.
  EXPECT_EQ(store.entryCount(*id_a), 1u);
  EXPECT_EQ(store.entryCount(*id_c), 1u);
  EXPECT_EQ(store.latestAt(*id_a, 200)->payload.bytes[0], 0xAA);
  EXPECT_EQ(store.latestAt(*id_c, 200)->payload.bytes[0], 0xCC);
  EXPECT_EQ(store.descriptor(*id_a).topic_name, "topic/a");
  EXPECT_EQ(store.descriptor(*id_c).topic_name, "topic/c");

  // Re-register a topic with the same name as the removed one; it gets a fresh id.
  auto id_b2 = store.registerTopic({.dataset_id = 1, .topic_name = "topic/b", .metadata_json = R"({"v":2})"});
  ASSERT_TRUE(id_b2.has_value());
  EXPECT_NE(id_b2->id, id_b->id);
  ASSERT_TRUE(store.pushOwned(*id_b2, 200, makePayload(4, 0xDD)).has_value());
  EXPECT_EQ(store.entryCount(*id_b2), 1u);
  EXPECT_EQ(store.latestAt(*id_b2, 300)->payload.bytes[0], 0xDD);
  EXPECT_EQ(store.findTopic(1, "topic/b")->id, id_b2->id);
  EXPECT_EQ(store.descriptor(*id_b2).metadata_json, R"({"v":2})");

  // Old removed id still resolves nothing.
  EXPECT_EQ(store.entryCount(*id_b), 0u);
  EXPECT_FALSE(store.latestAt(*id_b, 300).has_value());

  // listTopics sees exactly 3 topics (a, c, b2).
  EXPECT_EQ(store.listTopics(1).size(), 3u);
}

// Exercise clear() then re-register: all previously valid ids become unreachable
// and new registrations work correctly after the wipe.
TEST(ObjectStoreIndexTest, ClearThenReregister) {
  ObjectStore store;
  auto id_x = store.registerTopic({.dataset_id = 1, .topic_name = "topic/x", .metadata_json = "{}"});
  auto id_y = store.registerTopic({.dataset_id = 1, .topic_name = "topic/y", .metadata_json = "{}"});
  ASSERT_TRUE(id_x.has_value());
  ASSERT_TRUE(id_y.has_value());
  ASSERT_TRUE(store.pushOwned(*id_x, 100, makePayload(4, 0x11)).has_value());
  ASSERT_TRUE(store.pushOwned(*id_y, 100, makePayload(4, 0x22)).has_value());

  store.clear();

  // Both old ids are gone.
  EXPECT_EQ(store.entryCount(*id_x), 0u);
  EXPECT_EQ(store.entryCount(*id_y), 0u);
  EXPECT_FALSE(store.latestAt(*id_x, 200).has_value());
  EXPECT_TRUE(store.listTopics().empty());

  // Re-registration succeeds and produces working topics.
  auto id_new = store.registerTopic({.dataset_id = 1, .topic_name = "topic/x", .metadata_json = "{}"});
  ASSERT_TRUE(id_new.has_value());
  ASSERT_TRUE(store.pushOwned(*id_new, 300, makePayload(4, 0x33)).has_value());
  EXPECT_EQ(store.entryCount(*id_new), 1u);
  EXPECT_EQ(store.latestAt(*id_new, 400)->payload.bytes[0], 0x33);
  EXPECT_EQ(store.findTopic(1, "topic/x")->id, id_new->id);
}

// Regression: a slow lazy resolve must NOT hold the series lock — a concurrent
// push to the same series has to complete while the resolve is in flight.
// Pre-fix this deadlocks: the resolve held the shared series lock, the push
// blocked on the exclusive lock, and the resolve waited for the push forever
// (bounded here by wait_for so a regression fails instead of hanging the suite).
TEST(ObjectStoreLockScopeTest, SlowResolveDoesNotBlockSameSeriesPush) {
  ObjectStore store;
  const auto id = registerTestTopic(store);

  std::mutex sync_mutex;
  std::condition_variable cv;
  bool push_done = false;

  LazyCallback blocking_fetch = [&]() -> sdk::PayloadView {
    std::unique_lock lock(sync_mutex);
    cv.wait_for(lock, std::chrono::seconds(5), [&] { return push_done; });
    EXPECT_TRUE(push_done) << "push did not complete while resolve was in flight";
    return sdk::makePayloadView(makePayload(8));
  };
  ASSERT_TRUE(store.pushLazy(id, 10, std::move(blocking_fetch)));

  std::thread resolver([&] { (void)store.latestAt(id, 10); });
  std::thread pusher([&] {
    ASSERT_TRUE(store.pushLazy(id, 20, []() -> sdk::PayloadView { return sdk::makePayloadView(makePayload(8)); }));
    {
      std::lock_guard lock(sync_mutex);
      push_done = true;
    }
    cv.notify_all();
  });
  pusher.join();
  resolver.join();
  EXPECT_EQ(store.entryCount(id), 2U);
}

// =========================================================================
// Review fixes (objectstore correctness review)
// =========================================================================

TEST(ObjectStoreReviewTest, LazyResidentBytesCountAgainstTheMemoryBudget) {
  // Streaming's kEager entries are lazy closures that pin their payload; they must
  // count against max_memory_bytes like owned payloads, or the cap never fires.
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.time_window_ns = 0, .max_memory_bytes = 250});
  for (int i = 0; i < 5; ++i) {
    auto bytes = std::make_shared<const std::vector<uint8_t>>(100, 0x11);
    auto fetch = [bytes]() -> std::optional<sdk::PayloadView> {
      return sdk::PayloadView{Span<const uint8_t>{bytes->data(), bytes->size()}, sdk::BufferAnchor{bytes}};
    };
    ASSERT_TRUE(store.pushLazy(id, i, std::move(fetch), bytes->size()).has_value());
  }
  EXPECT_EQ(store.entryCount(id), 2U);
  EXPECT_EQ(store.memoryUsage(id), 200U);
  store.evictBefore(id, 100);
  EXPECT_EQ(store.memoryUsage(id), 0U);
}

TEST(ObjectStoreReviewTest, RetentionWindowNearInt64MinDoesNotWrap) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  store.setRetentionBudget(id, {.time_window_ns = 10, .max_memory_bytes = 0});
  const Timestamp near_min = std::numeric_limits<Timestamp>::min() + 5;
  ASSERT_TRUE(store.pushOwned(id, near_min, makePayload(4)).has_value());
  // newest - window lies below INT64_MIN: the floor saturates, nothing is evicted.
  EXPECT_EQ(store.entryCount(id), 1U);

  // "Infinite" window with negative timestamps.
  auto id2 = registerTestTopic(store, "second");
  store.setRetentionBudget(id2, {.time_window_ns = std::numeric_limits<int64_t>::max(), .max_memory_bytes = 0});
  ASSERT_TRUE(store.pushOwned(id2, -5, makePayload(4)).has_value());
  ASSERT_TRUE(store.pushOwned(id2, -2, makePayload(4)).has_value());
  EXPECT_EQ(store.entryCount(id2), 2U);
}

TEST(ObjectStoreReviewTest, LazyEntryWithoutFetcherResolvesAsFetchFailed) {
  ObjectStore store;
  auto id = registerTestTopic(store);
  ASSERT_TRUE(store.pushLazy(id, 1, LazyCallback{}).has_value());
  const auto entry = store.at(id, size_t{0});
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(entry->fetch_failed);
  EXPECT_TRUE(entry->payload.bytes.empty());
}

TEST(ObjectStoreReviewTest, SeededPushToUnknownTopicDoesNotTouchThePool) {
  ObjectStore store;
  auto pool = std::make_shared<ResidentPayloadPool>(1024);
  store.setResidentPayloadPool(pool);
  auto id = registerTestTopic(store);
  auto fetch = []() -> std::optional<sdk::PayloadView> { return sdk::makePayloadView(std::vector<uint8_t>(600)); };
  ASSERT_TRUE(store.pushLazyWithSeed(id, 1, sdk::makePayloadView(std::vector<uint8_t>(600)), fetch).has_value());
  const auto before = pool->stats();

  EXPECT_FALSE(store.pushLazyWithSeed(ObjectTopicId{9999}, 1, sdk::makePayloadView(std::vector<uint8_t>(600)), fetch)
                   .has_value());
  const auto after = pool->stats();
  EXPECT_EQ(after.admitted, before.admitted);
  EXPECT_EQ(after.resident_bytes, before.resident_bytes) << "the live seed must not be evicted";
}

// A payload whose destruction calls back into the store, like a plugin
// fetch_ctx_destroy that queries its host. Dropping it under a store or series
// lock would self-deadlock (or be UB on the shared_mutex).
struct ReentrantOnDestroy {
  // make_shared cannot aggregate-initialize on Xcode 15's AppleClang (no C++20
  // parenthesized aggregate init), and a temporary to copy from would run the
  // destructor this test counts.
  ReentrantOnDestroy(ObjectStore* store_in, ObjectTopicId probe_in, std::atomic<int>* destroyed_in)
      : store(store_in), probe(probe_in), destroyed(destroyed_in) {}
  ReentrantOnDestroy(const ReentrantOnDestroy&) = delete;
  ReentrantOnDestroy& operator=(const ReentrantOnDestroy&) = delete;
  ReentrantOnDestroy(ReentrantOnDestroy&&) = delete;
  ReentrantOnDestroy& operator=(ReentrantOnDestroy&&) = delete;

  ObjectStore* store;
  ObjectTopicId probe;
  std::atomic<int>* destroyed;
  ~ReentrantOnDestroy() {
    (void)store->entryCount(probe);
    (void)store->listTopics();
    destroyed->fetch_add(1);
  }
};

LazyCallback reentrantFetch(ObjectStore& store, ObjectTopicId probe, std::atomic<int>& destroyed) {
  auto guard = std::make_shared<ReentrantOnDestroy>(&store, probe, &destroyed);
  return [guard]() -> std::optional<sdk::PayloadView> { return sdk::makePayloadView(std::vector<uint8_t>{1}); };
}

TEST(ObjectStoreReviewTest, EntryDestructionRunsAfterTheLocksRelease) {
  ObjectStore store;
  std::atomic<int> destroyed{0};
  auto id = registerTestTopic(store);

  // Retention eviction on push (series lock held exclusively inside the push).
  store.setRetentionBudget(id, {.time_window_ns = 0, .max_memory_bytes = 0, .max_entries = 1});
  ASSERT_TRUE(store.pushLazy(id, 1, reentrantFetch(store, id, destroyed)).has_value());
  ASSERT_TRUE(store.pushLazy(id, 2, reentrantFetch(store, id, destroyed)).has_value());
  EXPECT_EQ(destroyed.load(), 1);

  // Explicit eviction.
  store.evictBefore(id, 100);
  EXPECT_EQ(destroyed.load(), 2);

  // Dataset clear, topic removal and full clear (store lock held exclusively).
  ASSERT_TRUE(store.pushLazy(id, 3, reentrantFetch(store, id, destroyed)).has_value());
  store.clearDataset(1);
  EXPECT_EQ(destroyed.load(), 3);
  ASSERT_TRUE(store.pushLazy(id, 4, reentrantFetch(store, id, destroyed)).has_value());
  store.removeTopic(id);
  EXPECT_EQ(destroyed.load(), 4);
  auto id2 = registerTestTopic(store, "again");
  ASSERT_TRUE(store.pushLazy(id2, 5, reentrantFetch(store, id2, destroyed)).has_value());
  store.clear();
  EXPECT_EQ(destroyed.load(), 5);
}

TEST(ObjectStoreReviewTest, PushOwnedBundleDestroysRetiredEntriesAfterTheLocksRelease) {
  ObjectStore store;
  std::atomic<int> destroyed{0};
  auto id = registerTestTopic(store);

  ASSERT_TRUE(store.pushLazy(id, 1, reentrantFetch(store, id, destroyed)).has_value());
  ASSERT_TRUE(store.pushLazy(id, 2, reentrantFetch(store, id, destroyed)).has_value());
  ASSERT_TRUE(store.latestAt(id, 2));  // warm the cache on ts 2
  EXPECT_EQ(destroyed.load(), 0);

  OwnedObjectWrite write{id, 3, {9, 9, 9}};
  write.drop = [](Timestamp ts, SequentialUID) { return ts == 1; };
  ASSERT_TRUE(store.pushOwnedBundle({write}).has_value());
  // The dropped ts 1 dies after the store/series/cache locks release: a hang here
  // (ctest timeout) is a reentrant destructor deadlocking under one of them. The
  // warm-cache value (ts 2) and its entry survive.
  EXPECT_EQ(destroyed.load(), 1);
  EXPECT_EQ(store.entryCount(id), 2u);  // ts 2 and ts 3 survive

  // The reentrant payloads call back into the store: empty the topic while it is alive.
  store.evictBefore(id, 1000);
  EXPECT_EQ(store.entryCount(id), 0u);
}

// A lazy payload whose destructor asks whether another thread can take the store lock
// exclusively, which it cannot while the destroying thread holds that lock. The helper
// thread is the caller's to join.
struct StoreLockProbe {
  StoreLockProbe(ObjectStore* store_in, std::atomic<bool>* held_in, std::thread* helper_in)
      : store(store_in), held(held_in), helper(helper_in) {}
  StoreLockProbe(const StoreLockProbe&) = delete;
  StoreLockProbe& operator=(const StoreLockProbe&) = delete;
  StoreLockProbe(StoreLockProbe&&) = delete;
  StoreLockProbe& operator=(StoreLockProbe&&) = delete;

  ObjectStore* store;
  std::atomic<bool>* held;
  std::thread* helper;
  ~StoreLockProbe() {
    auto done = std::make_shared<std::atomic<bool>>(false);
    *helper = std::thread([target = store, done] {
      (void)target->registerTopic({.dataset_id = 2, .topic_name = "probe", .metadata_json = "{}"});
      done->store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!done->load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    held->store(!done->load());
  }
};

// The fetch blocks on `gate` the first time it runs, after announcing itself on `started`.
LazyCallback gatedFetch(
    std::shared_ptr<void> keep_alive, std::shared_ptr<std::promise<void>> started, std::shared_future<void> gate) {
  auto announced = std::make_shared<std::atomic<bool>>(false);
  return [keep_alive, started, gate, announced]() -> std::optional<sdk::PayloadView> {
    if (!announced->exchange(true)) {
      started->set_value();
    }
    gate.wait();
    return sdk::makePayloadView(std::vector<uint8_t>(8, 0xCD));
  };
}

// The snapshot a latestAt resolves can hold the last reference to a lazy entry's fetch
// context (the entry was evicted while it resolved). Its cleanup is foreign code that
// may call the store, so it must run after the store lock is released.
TEST(ObjectStoreReviewTest, LatestAtDestroysItsSnapshotAfterTheLocksRelease) {
  ObjectStore store;
  const auto id = registerTestTopic(store);
  std::atomic<bool> destroyed_under_lock{false};
  std::thread helper;
  auto started = std::make_shared<std::promise<void>>();
  std::promise<void> resume;
  ASSERT_TRUE(store
                  .pushLazy(
                      id, 100,
                      gatedFetch(
                          std::make_shared<StoreLockProbe>(&store, &destroyed_under_lock, &helper), started,
                          resume.get_future().share()))
                  .has_value());

  auto pending = std::async(std::launch::async, [&] { return store.latestAt(id, 100); });
  started->get_future().wait();
  store.evictBefore(id, 1000);  // the series lets go; the in-flight snapshot is the last holder
  resume.set_value();
  ASSERT_TRUE(pending.get().has_value());
  if (helper.joinable()) {
    helper.join();
  }
  EXPECT_FALSE(destroyed_under_lock.load());
}

TEST(ObjectStoreReviewTest, AnEntryEvictedWhileItResolvesIsNotMemoized) {
  ObjectStore store;
  const auto id = registerTestTopic(store);
  auto started = std::make_shared<std::promise<void>>();
  std::promise<void> resume;
  ASSERT_TRUE(store.pushLazy(id, 100, gatedFetch({}, started, resume.get_future().share())).has_value());

  auto pending = std::async(std::launch::async, [&] { return store.latestAt(id, 100); });
  started->get_future().wait();
  store.evictBefore(id, 1000);
  resume.set_value();
  auto resolved = pending.get();
  ASSERT_TRUE(resolved.has_value());
  std::weak_ptr<const void> probe = resolved->payload.anchor;
  resolved.reset();
  EXPECT_TRUE(probe.expired()) << "the warm cache pins the evicted payload";
  EXPECT_FALSE(store.latestAt(id, 100).has_value());
}

// A merge can move an entry's timestamp while a read of it is resolving (the UID stays):
// memoizing the read would later answer a lookup with a stale timestamp.
TEST(ObjectStoreReviewTest, AnEntryShiftedWhileItResolvesIsNotMemoized) {
  ObjectStore store;
  const auto id = store.registerTopic({.dataset_id = 2, .topic_name = "shifted", .metadata_json = "{}"});
  ASSERT_TRUE(id.has_value());
  auto started = std::make_shared<std::promise<void>>();
  std::promise<void> resume;
  ASSERT_TRUE(store.pushLazy(*id, 100, gatedFetch({}, started, resume.get_future().share())).has_value());

  auto pending = std::async(std::launch::async, [&] { return store.latestAt(*id, 100); });
  started->get_future().wait();
  ASSERT_TRUE(store.mergeDatasets(1, {DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 50}}).has_value());
  resume.set_value();
  ASSERT_TRUE(pending.get().has_value());

  const auto after = store.latestAt(*id, 200);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->timestamp, 150);
}

// =========================================================================
// Bundle publication
// =========================================================================

uint8_t firstByteAt(const ObjectStore& store, ObjectTopicId id, Timestamp timestamp) {
  const auto resolved = store.latestAt(id, timestamp);
  EXPECT_TRUE(resolved.has_value());
  return resolved ? resolved->payload.bytes.front() : uint8_t{0};
}

TEST(ObjectStoreBundleTest, EveryTopicLands) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");

  ASSERT_TRUE(store.pushOwnedBundle({{a, 10, {1, 1, 1}}, {b, 12, {2}}}).has_value());
  EXPECT_EQ(store.entryCount(a), 1u);
  EXPECT_EQ(store.entryCount(b), 1u);
  EXPECT_EQ(store.memoryUsage(a), 3u);
  EXPECT_EQ(store.memoryUsage(b), 1u);
  EXPECT_EQ(firstByteAt(store, a, 100), 1);
  EXPECT_EQ(firstByteAt(store, b, 100), 2);
  EXPECT_EQ(store.timeRange(b).first, 12);

  ASSERT_TRUE(store.pushOwnedBundle({{a, 20, {3}}, {b, 22, {4}}}).has_value());
  EXPECT_EQ(store.entryCount(a), 2u) << "a bundle without a drop predicate only appends";
}

// A ring keeps a ledger of what it published: the UID of each new entry, in write order
// (invalid for a write that only prunes).
TEST(ObjectStoreBundleTest, ReturnsTheUidOfEveryEntryItCreatedInWriteOrder) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  const auto c = registerTestTopic(store, "c");
  OwnedObjectWrite prune_c{c};
  prune_c.append = false;

  const auto uids = store.pushOwnedBundle({{b, 10, {2}}, {a, 10, {1}}, prune_c});
  ASSERT_TRUE(uids.has_value());
  ASSERT_EQ(uids->size(), 3u);
  EXPECT_EQ((*uids)[0], store.latestAt(b, 10)->sequential_uid);
  EXPECT_EQ((*uids)[1], store.latestAt(a, 10)->sequential_uid);
  EXPECT_NE((*uids)[0], (*uids)[1]);
  EXPECT_FALSE((*uids)[2].valid());
  EXPECT_TRUE(store.at(a, (*uids)[1]).has_value()) << "at(id, uid) is the ledger's exact check";
}

TEST(ObjectStoreBundleTest, TheTopicRetentionBudgetApplies) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  store.setRetentionBudget(a, {.time_window_ns = 0, .max_memory_bytes = 0, .max_entries = 1});
  ASSERT_TRUE(store.pushOwnedBundle({{a, 10, {1}}, {b, 10, {1}}}).has_value());
  ASSERT_TRUE(store.pushOwnedBundle({{a, 20, {2}}, {b, 20, {2}}}).has_value());
  EXPECT_EQ(store.entryCount(a), 1u);
  EXPECT_EQ(store.entryCount(b), 2u);
  EXPECT_EQ(store.memoryUsage(a), 1u);
}

// MAT-05 / STO-03: a rejected bundle leaves every previous entry and byte count exactly
// as it was, whichever write fails and whatever the writes before it selected.
TEST(ObjectStoreBundleTest, AFailedBundleLeavesEveryPriorEntryUntouched) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  ASSERT_TRUE(store.pushOwnedBundle({{a, 10, {1}}, {b, 10, {2}}}).has_value());

  EXPECT_FALSE(store.pushOwnedBundle({}).has_value());
  EXPECT_FALSE(store.pushOwnedBundle({{a, 20, {3}}, {ObjectTopicId{99999}, 20, {4}}}).has_value());
  EXPECT_FALSE(store.pushOwnedBundle({{a, 20, {3}}, {a, 20, {4}}}).has_value());

  // The first write's drop selects every entry of `a` before the second one throws.
  OwnedObjectWrite write_a{a, 20, {3}};
  OwnedObjectWrite write_b{b, 20, {4}};
  write_a.drop = [](Timestamp, SequentialUID) { return true; };
  write_b.drop = [](Timestamp, SequentialUID) -> bool { throw std::bad_alloc(); };
  const auto failed = store.pushOwnedBundle({write_a, write_b});
  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("object bundle rejected"), std::string::npos) << failed.error();

  for (const auto topic : {a, b}) {
    EXPECT_EQ(store.entryCount(topic), 1u);
    EXPECT_EQ(store.memoryUsage(topic), 1u);
    EXPECT_EQ(store.latestAt(topic, 100)->timestamp, 10);
  }
  EXPECT_EQ(firstByteAt(store, a, 100), 1);
  EXPECT_EQ(firstByteAt(store, b, 100), 2);

  // The same bundle with a well-behaved predicate then goes through.
  write_b.drop = write_a.drop;
  ASSERT_TRUE(store.pushOwnedBundle({write_a, write_b}).has_value());
  EXPECT_EQ(store.entryCount(a), 1u);
  EXPECT_EQ(firstByteAt(store, a, 100), 3);
  EXPECT_EQ(firstByteAt(store, b, 100), 4);
}

// The warm cache must not pin a payload a bundle removed.
TEST(ObjectStoreBundleTest, RemovingTheCachedEntryReleasesItsPayload) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  ASSERT_TRUE(store.pushOwnedBundle({{a, 10, {1}}}).has_value());
  ASSERT_TRUE(store.pushOwnedBundle({{a, 20, {2}}}).has_value());
  auto resolved = store.latestAt(a, 10);  // warms the cache on ts 10
  ASSERT_TRUE(resolved.has_value());
  std::weak_ptr<const void> probe = resolved->payload.anchor;
  resolved.reset();

  OwnedObjectWrite evict{a};
  evict.append = false;
  evict.drop = [](Timestamp ts, SequentialUID) { return ts == 10; };
  ASSERT_TRUE(store.pushOwnedBundle({evict}).has_value());
  EXPECT_TRUE(probe.expired()) << "the warm cache pins the removed payload";
  EXPECT_EQ(store.memoryUsage(a), 1u);
  EXPECT_EQ(firstByteAt(store, a, 100), 2);

  // The same through a write that drops and appends, the ring's publish path.
  resolved = store.latestAt(a, 20);
  ASSERT_TRUE(resolved.has_value());
  probe = resolved->payload.anchor;
  resolved.reset();
  OwnedObjectWrite replace{a, 30, {3}};
  replace.drop = [](Timestamp ts, SequentialUID) { return ts == 20; };
  ASSERT_TRUE(store.pushOwnedBundle({replace}).has_value());
  EXPECT_TRUE(probe.expired()) << "the warm cache pins the payload a drop-and-append removed";
  EXPECT_EQ(store.memoryUsage(a), 1u);
  EXPECT_EQ(firstByteAt(store, a, 100), 3);
}

TEST(ObjectStoreBundleTest, PruneOnlyKeepsTheRetainedEntryIdentityAndCanEmptyTopics) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  ASSERT_TRUE(store.pushOwnedBundle({{a, 10, {1}}, {b, 10, {2}}}).has_value());
  ASSERT_TRUE(store.pushOwnedBundle({{a, 20, {3}}, {b, 20, {4}}}).has_value());
  const auto uid = store.latestAt(a, 20)->sequential_uid;

  OwnedObjectWrite prune_a{a};
  OwnedObjectWrite prune_b{b};
  prune_a.append = prune_b.append = false;
  prune_a.drop = prune_b.drop = [](Timestamp t, SequentialUID) { return t < 20; };
  ASSERT_TRUE(store.pushOwnedBundle({prune_a, prune_b}).has_value());
  EXPECT_EQ(store.entryCount(a), 1u);
  EXPECT_EQ(store.entryCount(b), 1u);
  EXPECT_EQ(store.latestAt(a, 20)->sequential_uid, uid) << "pruning does not replace the surviving entry";

  prune_a.drop = prune_b.drop = [](Timestamp, SequentialUID) { return true; };
  ASSERT_TRUE(store.pushOwnedBundle({prune_a, prune_b}).has_value());
  EXPECT_EQ(store.entryCount(a), 0u);
  EXPECT_EQ(store.entryCount(b), 0u);
}

// The ring evicts every output of a node at one stamp in a single bundle (append=false
// plus a drop predicate): the outputs never disagree, and a failure evicts none.
TEST(ObjectStoreBundleTest, OneNodesOutputsAtAStampAreEvictedTogether) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  for (const Timestamp stamp : {10, 20, 30}) {
    const auto byte = static_cast<uint8_t>(stamp);
    ASSERT_TRUE(store.pushOwnedBundle({{a, stamp, {byte}}, {b, stamp, {byte}}}).has_value());
  }
  const auto survivor_a = store.latestAt(a, 30)->sequential_uid;
  const auto survivor_b = store.latestAt(b, 30)->sequential_uid;

  OwnedObjectWrite evict_a{a};
  OwnedObjectWrite evict_b{b};
  evict_a.append = evict_b.append = false;
  evict_a.drop = [](Timestamp stamp, SequentialUID) { return stamp == 30; };
  evict_b.drop = [](Timestamp, SequentialUID) -> bool { throw std::bad_alloc(); };
  EXPECT_FALSE(store.pushOwnedBundle({evict_a, evict_b}).has_value());
  EXPECT_EQ(store.entryCount(a), 3u) << "a failed eviction evicts from no topic";
  EXPECT_EQ(store.entryCount(b), 3u);

  evict_a.drop = evict_b.drop = [](Timestamp stamp, SequentialUID) { return stamp == 20; };
  ASSERT_TRUE(store.pushOwnedBundle({evict_a, evict_b}).has_value());
  for (const auto topic : {a, b}) {
    EXPECT_EQ(store.entryCount(topic), 2u);
    EXPECT_EQ(store.memoryUsage(topic), 2u);
    EXPECT_EQ(store.rangeByTime(topic, 19, 20).size(), 0u);
    EXPECT_EQ(store.rangeByTime(topic, 9, 10).size(), 1u);
  }
  EXPECT_EQ(store.latestAt(a, 30)->sequential_uid, survivor_a) << "survivors keep their UIDs";
  EXPECT_EQ(store.latestAt(b, 30)->sequential_uid, survivor_b);
}

// Stress, no forced interleaving: the eviction is one commit point, so a reader that
// finds a stamp gone from `a` and then looks in `b` finds it gone there too.
TEST(ObjectStoreBundleTest, ConcurrentReadersNeverSeeOneTopicEvictedAndTheOtherNot) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  constexpr Timestamp kStamps = 200;
  for (Timestamp stamp = 1; stamp <= kStamps; ++stamp) {
    ASSERT_TRUE(store.pushOwnedBundle({{a, stamp, {1}}, {b, stamp, {1}}}).has_value());
  }

  std::atomic<bool> done{false};
  std::atomic<int> failed_evictions{0};
  std::thread writer([&] {
    for (Timestamp stamp = 1; stamp <= kStamps; ++stamp) {
      OwnedObjectWrite evict_a{a};
      OwnedObjectWrite evict_b{b};
      evict_a.append = evict_b.append = false;
      evict_a.drop = evict_b.drop = [stamp](Timestamp t, SequentialUID) { return t == stamp; };
      if (!store.pushOwnedBundle({evict_a, evict_b}).has_value()) {
        failed_evictions.fetch_add(1);
      }
    }
    done.store(true, std::memory_order_release);
  });

  const auto holds = [&](ObjectTopicId topic, Timestamp stamp) {
    return !store.rangeByTime(topic, stamp - 1, stamp).empty();
  };
  int torn = 0;
  bool last_pass = false;
  while (!last_pass) {
    last_pass = done.load(std::memory_order_acquire);
    for (Timestamp stamp = 1; stamp <= kStamps; ++stamp) {
      const bool in_a = holds(a, stamp);
      const bool in_b = holds(b, stamp);
      torn += (!in_a && in_b) ? 1 : 0;
    }
  }
  writer.join();
  EXPECT_EQ(failed_evictions.load(), 0);
  EXPECT_EQ(torn, 0);
}

// Stress, no forced interleaving: a reader that finds the newest stamp of `b` and then
// reads `a` finds a stamp at least as new.
TEST(ObjectStoreBundleTest, ConcurrentReadersNeverSeeAnAppendedBundleHalfApplied) {
  ObjectStore store;
  const auto a = registerTestTopic(store, "a");
  const auto b = registerTestTopic(store, "b");
  ASSERT_TRUE(store.pushOwnedBundle({{a, 0, {0}}, {b, 0, {0}}}).has_value());

  constexpr Timestamp kBundles = 200;
  std::atomic<bool> done{false};
  std::atomic<int> failed_publications{0};
  std::thread writer([&] {
    for (Timestamp stamp = 1; stamp <= kBundles; ++stamp) {
      if (!store.pushOwnedBundle({{a, stamp, {1}}, {b, stamp, {1}}}).has_value()) {
        failed_publications.fetch_add(1);
      }
    }
    done.store(true, std::memory_order_release);
  });

  int torn = 0;
  bool last_pass = false;
  while (!last_pass) {
    last_pass = done.load(std::memory_order_acquire);
    const Timestamp newest_b = store.timeRange(b).second;
    const Timestamp newest_a = store.timeRange(a).second;
    torn += newest_a < newest_b ? 1 : 0;
  }
  writer.join();
  EXPECT_EQ(failed_publications.load(), 0);
  EXPECT_EQ(torn, 0);
  EXPECT_EQ(store.timeRange(a).second, kBundles);
}

// ---- entry identity for a cursor that rescans nothing: indexOf, contentIdentity (MED-25, STO-02) ---------------

// Equal stamps never alias: an entry's position is found by its UID, wherever retention or a late insert moved it.
TEST(ObjectStoreTest, IndexOfResolvesDuplicateStamps) {
  ObjectStore store;
  const auto topic = registerTestTopic(store);
  ASSERT_TRUE(store.pushOwned(topic, 100, {1}).has_value());
  ASSERT_TRUE(store.pushOwned(topic, 100, {2}).has_value());
  ASSERT_TRUE(store.pushOwned(topic, 50, {3}).has_value());  // late: lands first
  SequentialUID cursor;
  const auto arrivals = store.drainNewSince(topic, cursor);
  ASSERT_EQ(arrivals.size(), 3u);
  EXPECT_EQ(store.indexOf(topic, arrivals[2].sequential_uid), 0u);
  EXPECT_EQ(store.indexOf(topic, arrivals[0].sequential_uid), 1u);
  EXPECT_EQ(store.indexOf(topic, arrivals[1].sequential_uid), 2u);
  EXPECT_FALSE(store.indexOf(topic, SequentialUID{}).has_value());
  EXPECT_FALSE(store.indexOf(ObjectTopicId{999}, arrivals[0].sequential_uid).has_value());

  store.evictBefore(topic, 60);
  EXPECT_FALSE(store.indexOf(topic, arrivals[2].sequential_uid).has_value());
  EXPECT_EQ(store.indexOf(topic, arrivals[0].sequential_uid), 0u);
}

// `content` moves on every change; `rewrite` only when a UID cursor and a timestamp floor could not tell the change
// from an append plus a front eviction. Revisions are process-unique, so a topic id reused after a clear never repeats
// one.
TEST(ObjectStoreTest, ContentIdentityMovesOnReplacementAndEviction) {
  ObjectStore store;
  const auto topic = registerTestTopic(store);
  EXPECT_EQ(store.contentIdentity(ObjectTopicId{999}), OrderedEntries::ContentIdentity{});
  ASSERT_TRUE(store.pushOwned(topic, 100, {1}).has_value());
  const auto first = store.contentIdentity(topic);

  ASSERT_TRUE(store.pushOwned(topic, 200, {2}).has_value());
  const auto appended = store.contentIdentity(topic);
  EXPECT_NE(appended.content, first.content);
  EXPECT_EQ(appended.rewrite, first.rewrite) << "an append rewrites nothing";

  ASSERT_TRUE(store.pushOwned(topic, 200, {3}).has_value());
  const auto equal_stamp = store.contentIdentity(topic);
  EXPECT_NE(equal_stamp.content, appended.content);
  EXPECT_EQ(equal_stamp.rewrite, appended.rewrite) << "an equal stamp lands after the newest entry: an append";

  ASSERT_TRUE(store.pushOwned(topic, 150, {4}).has_value());
  const auto late = store.contentIdentity(topic);
  EXPECT_NE(late.rewrite, equal_stamp.rewrite);

  store.evictBefore(topic, 120);
  const auto evicted = store.contentIdentity(topic);
  EXPECT_NE(evicted.content, late.content);
  EXPECT_EQ(evicted.rewrite, late.rewrite) << "a front eviction rewrites nothing";

  OwnedObjectWrite prune;
  prune.topic = topic;
  prune.append = false;
  prune.drop = [](Timestamp stamp, SequentialUID) { return stamp == 150; };
  ASSERT_TRUE(store.pushOwnedBundle({prune}).has_value());
  const auto removed = store.contentIdentity(topic);
  EXPECT_NE(removed.rewrite, evicted.rewrite) << "a removal anywhere rewrites";
  const auto unchanged = store.contentIdentity(topic);
  EXPECT_EQ(unchanged, removed) << "a read changes nothing";

  store.clear();
  const auto reused = registerTestTopic(store);
  ASSERT_EQ(reused, topic);
  ASSERT_TRUE(store.pushOwned(reused, 100, {1}).has_value());
  const auto again = store.contentIdentity(reused);
  EXPECT_NE(again.content, first.content);
  EXPECT_NE(again.rewrite, first.rewrite);
}

// PJ_BENCH=1 prints what one bundle publication of three topics costs, each topic holding
// `window` retained entries and the drop removing one per call (a sliding window). The
// 900 and 3,600 windows make the first payload 64 B too, so the bench stays within memory:
// the cost under the lock is per entry, not per byte.
TEST(ObjectStoreBundleBench, PublishCost) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kCalls = 200;
  struct Case {
    int window;
    size_t first_payload;
  };
  for (const Case bench : {Case{0, 1'500'000}, Case{100, 1'500'000}, Case{900, 64}, Case{3600, 64}}) {
    const int window = bench.window;
    ObjectStore store;
    const auto cloud = registerTestTopic(store, "cloud");
    const auto small_a = registerTestTopic(store, "small_a");
    const auto small_b = registerTestTopic(store, "small_b");
    // A ring trims by predicate: the window keeps the newest `window` stamps.
    Timestamp stamp = 0;
    const auto publish = [&] {
      ++stamp;
      const Timestamp floor = stamp - window;
      const auto trim = [floor](Timestamp t, SequentialUID) { return t <= floor; };
      OwnedObjectWrite write_cloud{cloud, stamp, makePayload(bench.first_payload)};
      OwnedObjectWrite write_a{small_a, stamp, makePayload(64)};
      OwnedObjectWrite write_b{small_b, stamp, makePayload(64)};
      write_cloud.drop = write_a.drop = write_b.drop = trim;
      std::vector<OwnedObjectWrite> writes;
      writes.push_back(std::move(write_cloud));
      writes.push_back(std::move(write_a));
      writes.push_back(std::move(write_b));
      const auto begin = std::chrono::steady_clock::now();
      EXPECT_TRUE(store.pushOwnedBundle(std::move(writes)).has_value());
      return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    };
    for (int warmup = 0; warmup < window + 5; ++warmup) {
      publish();
    }
    std::vector<double> micros;
    for (int call = 0; call < kCalls; ++call) {
      micros.push_back(publish());
    }
    std::sort(micros.begin(), micros.end());
    std::printf(
        "pushOwnedBundle %7zu B + 2 x 64 B, window %4d entries: median %.1f us, p95 %.1f us\n", bench.first_payload,
        window, micros[micros.size() / 2], micros[micros.size() * 95 / 100]);
  }
}

// PJ_BENCH=1 prints the push that carries a retention jump (a shrunk time window, or a resume far past a paused tail):
// one push expiring N of N retained entries, and N of 2N, 64 B each, median of 3 (one for the 100k cases). Eviction
// runs under the series' write lock, on the pushing (ingest) thread.
TEST(ObjectStoreBundleBench, RetentionJump) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  for (const size_t expired : {size_t{1'000}, size_t{10'000}, size_t{100'000}}) {
    for (const size_t retained : {expired, 2 * expired}) {
      std::vector<double> millis;
      for (int sample = 0; sample < (expired >= 100'000 ? 1 : 3); ++sample) {
        ObjectStore store;
        const auto topic = registerTestTopic(store);
        for (size_t index = 0; index < retained; ++index) {
          ASSERT_TRUE(store.pushOwned(topic, static_cast<Timestamp>(index), makePayload(64)).has_value());
        }
        // The jump keeps the entries at or after `expired` (and the new one).
        const Timestamp newest = static_cast<Timestamp>(retained) + 1'000'000;
        store.setRetentionBudget(topic, RetentionBudget{.time_window_ns = newest - static_cast<Timestamp>(expired)});
        const auto begin = std::chrono::steady_clock::now();
        ASSERT_TRUE(store.pushOwned(topic, newest, makePayload(64)).has_value());
        millis.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        ASSERT_EQ(store.entryCount(topic), retained - expired + 1);
      }
      std::sort(millis.begin(), millis.end());
      std::printf(
          "[ObjectStoreBundleBench] retention jump expiring %6zu of %6zu entries: %9.2f ms (%.1f ns per expired "
          "entry)\n",
          expired, retained, millis[millis.size() / 2], millis[millis.size() / 2] * 1e6 / static_cast<double>(expired));
    }
  }
}

}  // namespace
}  // namespace PJ
