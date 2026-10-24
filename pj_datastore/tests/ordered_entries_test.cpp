// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Focused unit tests for the OrderedEntries value type: they exercise the
// ordering invariants (timestamp-ascending storage + a uid_order side index that
// is a strictly-ascending-by-UID permutation) DIRECTLY through the encapsulated
// API, across in-order and out-of-order pushes, eviction, re-UID, rebuild, shift,
// and the cross-series bulk ops. The ObjectStore black-box tests cover the same
// behavior end to end; these pin the invariant at the value-type boundary.

#include "pj_datastore/ordered_entries.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

namespace PJ {
namespace {

// Build an entry at `ts` with a freshly minted (globally increasing) UID, exactly
// as ObjectStore's push paths do. The payload is left default (irrelevant here).
ObjectEntry makeEntry(Timestamp ts, SequentialUID* out_uid = nullptr) {
  ObjectEntry entry;
  entry.timestamp = ts;
  entry.sequential_uid = SequentialUID::getNext();
  if (out_uid != nullptr) {
    *out_uid = entry.sequential_uid;
  }
  return entry;
}

// Walk the UID cursor from the front (firstUid + nextUidAfter), returning the
// UIDs in ascending-UID (== arrival) order.
std::vector<SequentialUID> uidWalk(const OrderedEntries& oe) {
  std::vector<SequentialUID> out;
  for (SequentialUID u = oe.firstUid(); u.valid(); u = oe.nextUidAfter(u)) {
    out.push_back(u);
  }
  return out;
}

// The stored timestamps, in array order (== ascending).
std::vector<Timestamp> stamps(const OrderedEntries& oe) {
  return {oe.timestamps().begin(), oe.timestamps().end()};
}

TEST(OrderedEntriesTest, InOrderPushKeepsTimestampsAndUidCursor) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(30, &c));

  EXPECT_EQ(oe.size(), 3u);
  EXPECT_FALSE(oe.empty());
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_EQ(oe.frontTimestamp(), 10);
  EXPECT_EQ(oe.backTimestamp(), 30);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c}));
  EXPECT_EQ(oe.firstUid(), a);

  ASSERT_NE(oe.atUid(b), nullptr);
  EXPECT_EQ(oe.atUid(b)->timestamp, 20);
  ASSERT_NE(oe.atIndex(2), nullptr);
  EXPECT_EQ(oe.atIndex(2)->timestamp, 30);
  EXPECT_EQ(oe.atIndex(3), nullptr);  // out of range
}

TEST(OrderedEntriesTest, IndexAtOrBeforeMatchesAtOrBeforeSemantics) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(20));
  oe.push(makeEntry(30));

  EXPECT_FALSE(oe.indexAtOrBefore(5).has_value());  // before first
  EXPECT_EQ(oe.indexAtOrBefore(10), std::optional<size_t>(0));
  EXPECT_EQ(oe.indexAtOrBefore(25), std::optional<size_t>(1));
  EXPECT_EQ(oe.indexAtOrBefore(30), std::optional<size_t>(2));
  EXPECT_EQ(oe.indexAtOrBefore(999), std::optional<size_t>(2));
  EXPECT_FALSE(OrderedEntries{}.indexAtOrBefore(0).has_value());  // empty
}

TEST(OrderedEntriesTest, OutOfOrderPushSortsTimestampsButKeepsArrivalUidOrder) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));  // in order
  oe.push(makeEntry(30, &b));  // in order
  oe.push(makeEntry(20, &c));  // OUT of order: sorts between a and b by timestamp

  // Timestamps ascending; the middle slot is the out-of-order entry (uid c).
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_EQ(oe.entryAt(1).sequential_uid, c);
  EXPECT_EQ(oe.entryAt(1).timestamp, 20);

  // UID cursor is arrival order (a < b < c), independent of timestamp order.
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c}));
  ASSERT_NE(oe.atUid(c), nullptr);
  EXPECT_EQ(oe.atUid(c)->timestamp, 20);
  ASSERT_NE(oe.atUid(b), nullptr);
  EXPECT_EQ(oe.atUid(b)->timestamp, 30);
}

TEST(OrderedEntriesTest, RangeByTimeIsHalfOpenAscendingAndDecodeFree) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  oe.push(makeEntry(20, &c));  // out of order

  const auto window = oe.rangeByTime(15, 30);  // (15, 30] -> ts 20, 30
  ASSERT_EQ(window.size(), 2u);
  EXPECT_EQ(window[0].uid, c);  // ascending timestamp order
  EXPECT_EQ(window[0].timestamp, 20);
  EXPECT_EQ(window[1].uid, b);
  EXPECT_EQ(window[1].timestamp, 30);

  EXPECT_TRUE(oe.rangeByTime(30, 30).empty());    // hi == lo -> empty
  EXPECT_TRUE(oe.rangeByTime(40, 5).empty());     // reversed -> empty
  EXPECT_TRUE(oe.rangeByTime(100, 200).empty());  // nothing in window
}

TEST(OrderedEntriesTest, MaxUidAtOrBeforeIsHighWaterNotLatestTimestamp) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(15, &c));  // OUT of order: newest UID, but middle timestamp

  // Entries by timestamp: 10(a), 15(c), 20(b). At-or-before 20 covers all three,
  // so the high-water UID is c (the newest arrival), NOT b (the latest timestamp).
  EXPECT_EQ(oe.maxUidAtOrBefore(20), c);
  ASSERT_TRUE(oe.indexAtOrBefore(20).has_value());
  EXPECT_EQ(oe.entryAt(*oe.indexAtOrBefore(20)).sequential_uid, b);  // latest-timestamp differs
  EXPECT_EQ(oe.maxUidAtOrBefore(10), a);
  EXPECT_FALSE(oe.maxUidAtOrBefore(5).valid());  // nothing at or before
}

TEST(OrderedEntriesTest, EvictFrontKeepsCursorConsistentAcrossInversion) {
  OrderedEntries oe;
  SequentialUID a, b, c;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  oe.push(makeEntry(20, &c));  // out of order -> array: 10(a),20(c),30(b)

  std::vector<ObjectEntry> removed;
  oe.evictFront(1, removed);  // drops the ts-10 entry (uid a)
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(removed[0].sequential_uid, a);
  EXPECT_EQ(oe.size(), 2u);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 30}));
  EXPECT_EQ(oe.firstUid(), b);  // smallest RETAINED uid is b (c arrived later)
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{b, c}));
  EXPECT_EQ(oe.atUid(a), nullptr);  // evicted
  ASSERT_NE(oe.atUid(c), nullptr);
  EXPECT_EQ(oe.atUid(c)->timestamp, 20);
}

TEST(OrderedEntriesTest, ReuidAllMakesUidOrderIdentity) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(30));
  oe.push(makeEntry(20));  // out of order -> uid_order non-identity

  oe.reuidAll();
  // Fresh UIDs in array (timestamp) order: uid strictly ascends with index, and
  // the cursor walk equals the array order.
  ASSERT_EQ(oe.size(), 3u);
  EXPECT_LT(oe.entryAt(0).sequential_uid, oe.entryAt(1).sequential_uid);
  EXPECT_LT(oe.entryAt(1).sequential_uid, oe.entryAt(2).sequential_uid);
  EXPECT_EQ(
      uidWalk(oe), (std::vector<SequentialUID>{
                       oe.entryAt(0).sequential_uid, oe.entryAt(1).sequential_uid, oe.entryAt(2).sequential_uid}));
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 20, 30}));  // timestamps untouched
}

TEST(OrderedEntriesTest, ShiftSlidesTimestampsAndPayloadStampShiftLeavingUidOrder) {
  OrderedEntries oe;
  SequentialUID a, b;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));

  EXPECT_FALSE(oe.shift(0));                // no-op
  EXPECT_FALSE(OrderedEntries{}.shift(5));  // empty no-op

  EXPECT_TRUE(oe.shift(5));
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{15, 25}));
  EXPECT_EQ(oe.entryAt(0).payload_stamp_shift, 5);
  EXPECT_EQ(oe.entryAt(1).payload_stamp_shift, 5);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b}));  // uid order unchanged

  EXPECT_TRUE(oe.shift(5));  // accumulates
  EXPECT_EQ(oe.entryAt(0).payload_stamp_shift, 10);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 30}));
}

TEST(OrderedEntriesTest, AppendReuidFromConcatenatesAndClearsSource) {
  OrderedEntries dst;
  dst.push(makeEntry(10));
  dst.push(makeEntry(30));
  dst.push(makeEntry(20));  // dst prefix has an inversion

  OrderedEntries src;
  src.push(makeEntry(100));
  src.push(makeEntry(110));

  dst.appendReuidFrom(src);
  EXPECT_TRUE(src.empty());
  EXPECT_EQ(dst.size(), 5u);
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30, 100, 110}));
  // The combined uid_order is strictly ascending -> the cursor visits all 5.
  EXPECT_EQ(uidWalk(dst).size(), 5u);
  // atUid resolves the last appended (max-timestamp) entry.
  EXPECT_EQ(dst.entryAt(4).timestamp, 110);
}

TEST(OrderedEntriesTest, AdoptReuidFromReplacesTargetWithIdentityOrder) {
  OrderedEntries dst;
  dst.push(makeEntry(5));  // will be discarded

  OrderedEntries src;
  src.push(makeEntry(10));
  src.push(makeEntry(20));
  src.push(makeEntry(30));

  dst.adoptReuidFrom(src);
  EXPECT_TRUE(src.empty());
  EXPECT_EQ(dst.size(), 3u);
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_LT(dst.entryAt(0).sequential_uid, dst.entryAt(1).sequential_uid);
  EXPECT_EQ(uidWalk(dst).size(), 3u);
}

TEST(OrderedEntriesTest, MoveEntriesIntoThenAssignSortedReuidRebuildsDestination) {
  OrderedEntries dst;
  dst.push(makeEntry(10));
  dst.push(makeEntry(40));

  OrderedEntries srcA;
  srcA.push(makeEntry(20));
  srcA.push(makeEntry(30));

  std::vector<ObjectEntry> pool;
  pool.reserve(dst.size() + srcA.size());
  dst.moveEntriesInto(pool);   // dst now empty
  srcA.moveEntriesInto(pool);  // srcA now empty
  EXPECT_TRUE(dst.empty());
  EXPECT_TRUE(srcA.empty());
  ASSERT_EQ(pool.size(), 4u);

  std::stable_sort(
      pool.begin(), pool.end(), [](const ObjectEntry& l, const ObjectEntry& r) { return l.timestamp < r.timestamp; });
  dst.assignSortedReuid(std::move(pool));
  EXPECT_EQ(stamps(dst), (std::vector<Timestamp>{10, 20, 30, 40}));
  EXPECT_LT(dst.entryAt(0).sequential_uid, dst.entryAt(3).sequential_uid);  // identity uids
  EXPECT_EQ(uidWalk(dst).size(), 4u);
}

TEST(OrderedEntriesTest, DetachRestoreRoundTripPreservesWalkAndInversion) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(30));
  oe.push(makeEntry(20));  // inversion: uid_order not identity
  const std::vector<SequentialUID> before = uidWalk(oe);
  const std::vector<Timestamp> before_ts = stamps(oe);

  std::deque<ObjectEntry> entries_out;
  std::vector<Timestamp> stamps_out;
  oe.detachInto(entries_out, stamps_out);
  EXPECT_TRUE(oe.empty());
  ASSERT_EQ(entries_out.size(), 3u);

  oe.restoreRebuild(std::move(entries_out), std::move(stamps_out));
  EXPECT_EQ(stamps(oe), before_ts);
  EXPECT_EQ(uidWalk(oe), before) << "restoreRebuild must reconstruct the preserved UID inversion";
}

TEST(OrderedEntriesTest, ClearEmptiesEverything) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(20));
  oe.clear();
  EXPECT_TRUE(oe.empty());
  EXPECT_EQ(oe.size(), 0u);
  EXPECT_FALSE(oe.firstUid().valid());
  EXPECT_FALSE(oe.indexAtOrBefore(20).has_value());
  EXPECT_TRUE(oe.timestamps().empty());
}

// push() reports in-order vs out-of-order — the signal ObjectStore uses to decide
// whether to reset its warm latestAt cache.
TEST(OrderedEntriesTest, PushReportsInOrderVsOutOfOrder) {
  OrderedEntries oe;
  EXPECT_EQ(oe.push(makeEntry(10)), OrderedEntries::PushOrder::kInOrderAppend);
  EXPECT_EQ(oe.push(makeEntry(30)), OrderedEntries::PushOrder::kInOrderAppend);
  EXPECT_EQ(oe.push(makeEntry(30)), OrderedEntries::PushOrder::kInOrderAppend) << "equal-to-back stays in-order";
  EXPECT_EQ(oe.push(makeEntry(20)), OrderedEntries::PushOrder::kOutOfOrderInsert) << "ts below back is out-of-order";
}

// Equal timestamps inserted out-of-order keep arrival (UID) order at the value-type
// boundary (upper_bound places a new equal-ts entry after existing equals).
TEST(OrderedEntriesTest, EqualTimestampOutOfOrderInsertKeepsArrivalOrder) {
  OrderedEntries oe;
  SequentialUID a, b, c, d;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(30, &b));
  ASSERT_EQ(oe.push(makeEntry(20, &c)), OrderedEntries::PushOrder::kOutOfOrderInsert);
  ASSERT_EQ(oe.push(makeEntry(20, &d)), OrderedEntries::PushOrder::kOutOfOrderInsert);  // equal to c, later arrival

  const auto window = oe.rangeByTime(15, 25);
  ASSERT_EQ(window.size(), 2u);
  EXPECT_EQ(window[0].uid, c) << "earlier arrival first among equal timestamps";
  EXPECT_EQ(window[1].uid, d);
  EXPECT_EQ(window[0].timestamp, 20);
  EXPECT_EQ(window[1].timestamp, 20);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b, c, d}));
}

// appendReuidFrom re-UIDs only the MOVED source entries; the destination's
// pre-existing UIDs must stay stable and resolvable (consumer cursors hold them).
TEST(OrderedEntriesTest, AppendReuidFromKeepsDestinationUidsResolvable) {
  OrderedEntries dst;
  SequentialUID d0, d1;
  dst.push(makeEntry(10, &d0));
  dst.push(makeEntry(20, &d1));

  OrderedEntries src;
  src.push(makeEntry(30));
  src.push(makeEntry(40));

  dst.appendReuidFrom(src);

  ASSERT_NE(dst.atUid(d0), nullptr) << "pre-existing destination UID must survive appendReuidFrom";
  EXPECT_EQ(dst.atUid(d0)->timestamp, 10);
  ASSERT_NE(dst.atUid(d1), nullptr);
  EXPECT_EQ(dst.atUid(d1)->timestamp, 20);
  EXPECT_EQ(dst.size(), 4u);
  EXPECT_TRUE(src.empty());
}

// A removal anywhere in the series: the survivors close the gaps in array order, keep
// their UIDs, and the UID cursor walks them in the same order as before.
TEST(OrderedEntriesTest, RemoveKeepsTheSurvivorsOrderAndUids) {
  OrderedEntries oe;
  SequentialUID a, b, c, d, e;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(30, &c));
  oe.push(makeEntry(40, &d));
  oe.push(makeEntry(50, &e));

  const auto plan = oe.planRemoval([](Timestamp ts, SequentialUID) { return ts == 20 || ts == 40; });
  EXPECT_EQ(oe.size(), 5u) << "planning changes nothing";
  EXPECT_EQ(plan.count, 2u);
  EXPECT_EQ(
      plan.new_position,
      (std::vector<uint32_t>{0, OrderedEntries::Removal::kRemoved, 1, OrderedEntries::Removal::kRemoved, 2}));

  std::vector<ObjectEntry> removed;
  oe.remove(plan, removed);
  ASSERT_EQ(removed.size(), 2u);
  EXPECT_EQ(removed[0].sequential_uid, b) << "removed entries come out in array order";
  EXPECT_EQ(removed[1].sequential_uid, d);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 30, 50}));
  EXPECT_EQ(oe.entryAt(1).sequential_uid, c);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, c, e}));
  EXPECT_EQ(oe.atUid(b), nullptr);
  EXPECT_EQ(oe.atUid(d), nullptr);
  ASSERT_NE(oe.atUid(e), nullptr);
  EXPECT_EQ(oe.atUid(e)->timestamp, 50);
}

// After an out-of-order insert the UID order is not the array order: the remapped
// cursor must still find each survivor at its new position.
TEST(OrderedEntriesTest, RemoveRemapsTheUidOrderAcrossAnInversion) {
  OrderedEntries oe;
  SequentialUID a, b, c, d;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(30, &c));
  oe.push(makeEntry(15, &d));  // out of order -> array: 10(a), 15(d), 20(b), 30(c)

  std::vector<ObjectEntry> removed;
  oe.remove(oe.planRemoval([b](Timestamp, SequentialUID uid) { return uid == b; }), removed);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{10, 15, 30}));
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, c, d}));
  ASSERT_NE(oe.atUid(d), nullptr);
  EXPECT_EQ(oe.atUid(d)->timestamp, 15);
  ASSERT_NE(oe.atUid(c), nullptr);
  EXPECT_EQ(oe.atUid(c)->timestamp, 30);
  EXPECT_EQ(oe.firstUid(), a);
  EXPECT_EQ(oe.nextUidAfter(c), d);

  // The front goes too: every survivor shifts, the inversion stays.
  oe.remove(oe.planRemoval([](Timestamp ts, SequentialUID) { return ts == 10; }), removed);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{15, 30}));
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{c, d}));
  ASSERT_NE(oe.atUid(d), nullptr);
  EXPECT_EQ(oe.atUid(d)->timestamp, 15);
  EXPECT_EQ(removed.size(), 2u) << "remove appends to what the caller already holds";
}

// One step: the new entry takes a slot the removal freed and lands at its sorted
// position, in order or not, with the UID cursor intact.
TEST(OrderedEntriesTest, RemoveAndPushPlacesTheEntryInOneStep) {
  OrderedEntries oe;
  SequentialUID a, b, c, d, e;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  oe.push(makeEntry(30, &c));
  std::vector<ObjectEntry> removed;

  EXPECT_EQ(
      oe.removeAndPush(
          oe.planRemoval([](Timestamp ts, SequentialUID) { return ts == 10; }), makeEntry(40, &d), removed),
      OrderedEntries::PushOrder::kInOrderAppend);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 30, 40}));
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{b, c, d}));

  EXPECT_EQ(
      oe.removeAndPush(
          oe.planRemoval([](Timestamp ts, SequentialUID) { return ts == 30; }), makeEntry(25, &e), removed),
      OrderedEntries::PushOrder::kOutOfOrderInsert);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 25, 40}));
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{b, d, e}));
  ASSERT_NE(oe.atUid(e), nullptr);
  EXPECT_EQ(oe.atUid(e)->timestamp, 25);
  EXPECT_EQ(oe.entryAt(1).sequential_uid, e);
  ASSERT_EQ(removed.size(), 2u);
  EXPECT_EQ(removed[0].sequential_uid, a);
  EXPECT_EQ(removed[1].sequential_uid, c);

  // Nothing to remove: a plain push.
  oe.removeAndPush(oe.planRemoval([](Timestamp, SequentialUID) { return false; }), makeEntry(50), removed);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{20, 25, 40, 50}));
  EXPECT_EQ(removed.size(), 2u);
}

TEST(OrderedEntriesTest, RemoveNothingOrEverything) {
  OrderedEntries oe;
  SequentialUID a, b;
  oe.push(makeEntry(10, &a));
  oe.push(makeEntry(20, &b));
  std::vector<ObjectEntry> removed;

  oe.remove(oe.planRemoval([](Timestamp, SequentialUID) { return false; }), removed);
  EXPECT_TRUE(removed.empty());
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b}));

  EXPECT_THROW(
      (void)oe.planRemoval([](Timestamp ts, SequentialUID) -> bool {
        if (ts == 20) {
          throw std::runtime_error("drop failed");
        }
        return true;
      }),
      std::runtime_error);
  EXPECT_EQ(uidWalk(oe), (std::vector<SequentialUID>{a, b})) << "a throwing predicate changes nothing";

  oe.remove(oe.planRemoval([](Timestamp, SequentialUID) { return true; }), removed);
  EXPECT_TRUE(oe.empty());
  EXPECT_FALSE(oe.firstUid().valid());
  EXPECT_EQ(removed.size(), 2u);
}

// The bulk front eviction a retention jump takes equals the same entries evicted one at a time and the same prefix
// removed through planRemoval/remove: same survivors, timestamps, UID cursor, positions and removed entries.
TEST(OrderedEntriesTest, BulkEvictFrontMatchesOneByOneAndRemove) {
  std::mt19937 random(42);
  for (int round = 0; round < 200; ++round) {
    OrderedEntries bulk;
    const int size = std::uniform_int_distribution<int>(0, 60)(random);
    Timestamp newest = 0;
    for (int index = 0; index < size; ++index) {
      // One in four lands out of order, some on an equal stamp.
      const bool late = std::uniform_int_distribution<int>(0, 3)(random) == 0;
      newest += std::uniform_int_distribution<Timestamp>(0, 3)(random);
      bulk.push(makeEntry(late ? std::uniform_int_distribution<Timestamp>(0, newest)(random) : newest));
    }
    OrderedEntries one_by_one = bulk;
    OrderedEntries reference = bulk;
    const auto count = static_cast<size_t>(std::uniform_int_distribution<int>(0, size)(random));

    std::vector<ObjectEntry> bulk_removed;
    bulk.evictFront(count, bulk_removed);
    std::vector<ObjectEntry> one_removed;
    for (size_t index = 0; index < count; ++index) {
      one_by_one.evictFront(1, one_removed);
    }
    std::set<SequentialUID> front;
    for (size_t index = 0; index < count; ++index) {
      front.insert(reference.entryAt(index).sequential_uid);
    }
    std::vector<ObjectEntry> reference_removed;
    reference.remove(
        reference.planRemoval([&front](Timestamp, SequentialUID uid) { return front.count(uid) != 0; }),
        reference_removed);

    for (const OrderedEntries* other : {&one_by_one, &reference}) {
      ASSERT_EQ(bulk.size(), other->size()) << "round " << round;
      EXPECT_EQ(stamps(bulk), stamps(*other));
      EXPECT_EQ(uidWalk(bulk), uidWalk(*other));
      for (size_t index = 0; index < bulk.size(); ++index) {
        EXPECT_EQ(bulk.entryAt(index).sequential_uid, other->entryAt(index).sequential_uid);
        EXPECT_EQ(bulk.indexOfUid(bulk.entryAt(index).sequential_uid), index);
      }
    }
    for (const std::vector<ObjectEntry>* other : {&one_removed, &reference_removed}) {
      ASSERT_EQ(bulk_removed.size(), other->size());
      for (size_t index = 0; index < bulk_removed.size(); ++index) {
        EXPECT_EQ(bulk_removed[index].sequential_uid, (*other)[index].sequential_uid);
        EXPECT_EQ(bulk_removed[index].timestamp, (*other)[index].timestamp);
      }
    }
  }
}

// A front eviction is not a rewrite: a reader following the UID cursor and a timestamp floor keeps its place.
TEST(OrderedEntriesTest, BulkEvictFrontMovesOnlyTheContentRevision) {
  OrderedEntries oe;
  oe.push(makeEntry(10));
  oe.push(makeEntry(20));
  oe.push(makeEntry(30));
  std::vector<ObjectEntry> removed;
  const auto before = oe.contentIdentity();
  oe.evictFront(0, removed);
  EXPECT_EQ(oe.contentIdentity(), before) << "evicting nothing changes nothing";
  oe.evictFront(2, removed);
  EXPECT_NE(oe.contentIdentity().content, before.content);
  EXPECT_EQ(oe.contentIdentity().rewrite, before.rewrite);
  EXPECT_EQ(stamps(oe), (std::vector<Timestamp>{30}));
}

}  // namespace
}  // namespace PJ
