// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/resident_payload_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pj_datastore/object_store.hpp"

namespace {

using PJ::LazyCallback;
using PJ::ObjectStore;
using PJ::ObjectTopicDescriptor;
using PJ::ObjectTopicId;
using PJ::ResidentPayloadPool;
using PJ::Span;
using PJ::sdk::PayloadView;

PayloadView makePayload(size_t size, uint8_t fill) {
  return PJ::sdk::makePayloadView(std::vector<uint8_t>(size, fill));
}

/// Fallback closure that counts invocations and returns `size` bytes of `fill`.
LazyCallback countingFallback(std::shared_ptr<std::atomic<int>> calls, size_t size, uint8_t fill) {
  return [calls = std::move(calls), size, fill]() -> PayloadView {
    calls->fetch_add(1);
    return PJ::sdk::makePayloadView(std::vector<uint8_t>(size, fill));
  };
}

// --- Pool unit tests ---

TEST(ResidentPayloadPoolTest, AdmitServesAndAccountsBytes) {
  ResidentPayloadPool pool(1024);
  auto slot = pool.admit(makePayload(100, 0xAB));
  ASSERT_NE(slot, nullptr);

  auto resident = slot->load();
  ASSERT_TRUE(resident.has_value());
  EXPECT_EQ(resident->bytes.size(), 100U);
  EXPECT_EQ(resident->bytes[0], 0xAB);

  const auto stats = pool.stats();
  EXPECT_EQ(stats.resident_bytes, 100U);
  EXPECT_EQ(stats.admitted, 1U);
  EXPECT_EQ(stats.resident_hits, 1U);
}

// The reservation exists so a few huge payloads cannot evict a numerous tiny
// topic. MEASURED motivation: 598 point clouds filled a 256 MB budget and
// evicted 19,817 of 29,360 admissions, so a third of the GUI's TF reads missed
// the pool and paid a file re-read + chunk decompress each.
TEST(ResidentPayloadPoolTest, LargePayloadsCannotEvictTheSmallClass) {
  // 8 MiB pool -> 1 MiB reserved for payloads <= 64 KiB.
  ResidentPayloadPool pool(8 * 1024 * 1024);

  // A numerous small topic, comfortably inside the reservation.
  std::vector<std::shared_ptr<PJ::ResidentSlot>> small;
  small.reserve(64);
  for (int i = 0; i < 64; ++i) {
    small.push_back(pool.admit(makePayload(1024, static_cast<uint8_t>(i))));
    ASSERT_NE(small.back(), nullptr);
  }

  // Now churn far more than the whole budget through the large class.
  for (int i = 0; i < 32; ++i) {
    ASSERT_NE(pool.admit(makePayload(1024 * 1024, 0xAA)), nullptr);
  }

  // Every small payload must still be resident: large admissions may only evict
  // other large ones.
  for (std::size_t i = 0; i < small.size(); ++i) {
    EXPECT_TRUE(small[i]->load().has_value()) << "small payload " << i << " was evicted by large churn";
  }
}

// A payload bigger than the small class's entire capacity is NOT small: calling
// it small would make each admission evict the whole class and still not fit.
TEST(ResidentPayloadPoolTest, PayloadTooBigForTheReservationIsTreatedAsLarge) {
  ResidentPayloadPool pool(250);  // reservation is 31 bytes
  auto a = pool.admit(makePayload(100, 1));
  auto b = pool.admit(makePayload(100, 2));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  // Both live in the large class, so both fit under 250 - 31.
  EXPECT_TRUE(a->load().has_value());
  EXPECT_TRUE(b->load().has_value());
}

TEST(ResidentPayloadPoolTest, FifoEvictionByAdmissionOrder) {
  ResidentPayloadPool pool(250);
  auto first = pool.admit(makePayload(100, 1));
  auto second = pool.admit(makePayload(100, 2));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  // 100 + 100 + 100 > 250 -> the oldest (first) is evicted, second survives.
  auto third = pool.admit(makePayload(100, 3));
  ASSERT_NE(third, nullptr);

  EXPECT_EQ(first->load(), std::nullopt);
  EXPECT_TRUE(second->load().has_value());
  EXPECT_TRUE(third->load().has_value());

  const auto stats = pool.stats();
  EXPECT_EQ(stats.resident_bytes, 200U);
  EXPECT_EQ(stats.evicted, 1U);
  EXPECT_EQ(stats.high_water_bytes, 200U);
}

TEST(ResidentPayloadPoolTest, OversizePayloadRejectedWithoutEvictingResidents) {
  ResidentPayloadPool pool(200);
  auto resident = pool.admit(makePayload(150, 1));
  ASSERT_NE(resident, nullptr);

  EXPECT_EQ(pool.admit(makePayload(300, 2)), nullptr);

  // The oversize rejection must not have displaced the smaller resident.
  EXPECT_TRUE(resident->load().has_value());
  const auto stats = pool.stats();
  EXPECT_EQ(stats.rejected_oversize, 1U);
  EXPECT_EQ(stats.resident_bytes, 150U);
}

TEST(ResidentPayloadPoolTest, ZeroCapacityAndEmptyPayloadRejected) {
  ResidentPayloadPool disabled(0);
  EXPECT_EQ(disabled.admit(makePayload(10, 1)), nullptr);

  ResidentPayloadPool pool(100);
  EXPECT_EQ(pool.admit(PayloadView{}), nullptr);
}

TEST(ResidentPayloadPoolTest, SlotDestructionRetiresAccounting) {
  ResidentPayloadPool pool(1024);
  {
    auto slot = pool.admit(makePayload(400, 1));
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(pool.stats().resident_bytes, 400U);
  }
  // Slot died with its (hypothetical) entry -> bytes fully retired.
  EXPECT_EQ(pool.stats().resident_bytes, 0U);

  // The freed budget is available again.
  auto slot = pool.admit(makePayload(1024, 2));
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(pool.stats().evicted, 0U);
}

TEST(ResidentPayloadPoolTest, TrimEvictsEverything) {
  ResidentPayloadPool pool(1024);
  auto a = pool.admit(makePayload(100, 1));
  auto b = pool.admit(makePayload(100, 2));
  pool.trim();
  EXPECT_EQ(a->load(), std::nullopt);
  EXPECT_EQ(b->load(), std::nullopt);
  EXPECT_EQ(pool.stats().resident_bytes, 0U);
}

TEST(ResidentPayloadPoolTest, ConcurrentLoadVersusEvictionIsSafe) {
  ResidentPayloadPool pool(100);
  auto slot = pool.admit(makePayload(100, 7));
  ASSERT_NE(slot, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<int> valid_reads{0};
  std::thread reader([&] {
    while (!stop.load()) {
      if (auto payload = slot->load()) {
        // A racing eviction must never yield a torn/invalid view.
        if (payload->bytes.size() == 100 && payload->bytes[0] == 7) {
          valid_reads.fetch_add(1);
        } else {
          ADD_FAILURE() << "torn resident payload observed";
          break;
        }
      }
    }
  });
  // Evict by admitting a displacing payload, then stop the reader.
  auto displacing = pool.admit(makePayload(100, 8));
  stop.store(true);
  reader.join();
  EXPECT_EQ(slot->load(), std::nullopt);
}

// --- ObjectStore integration ---

class ObjectStoreSeedTest : public ::testing::Test {
 protected:
  ObjectTopicId registerTopic(const char* name = "/seeded") {
    auto id = store_.registerTopic(ObjectTopicDescriptor{.dataset_id = 1, .topic_name = name, .metadata_json = ""});
    EXPECT_TRUE(id.has_value());
    return *id;
  }

  ObjectStore store_;
  std::shared_ptr<ResidentPayloadPool> pool_ = std::make_shared<ResidentPayloadPool>(1024);
  std::shared_ptr<std::atomic<int>> fetch_calls_ = std::make_shared<std::atomic<int>>(0);
};

TEST_F(ObjectStoreSeedTest, SeededReadServesResidentBytesWithoutFallback) {
  store_.setResidentPayloadPool(pool_);
  const auto id = registerTopic();

  ASSERT_TRUE(store_.pushLazyWithSeed(id, 10, makePayload(64, 0xCD), countingFallback(fetch_calls_, 64, 0xCD)));

  auto entry = store_.latestAt(id, 10);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->payload.bytes.size(), 64U);
  EXPECT_EQ(entry->payload.bytes[0], 0xCD);
  EXPECT_EQ(fetch_calls_->load(), 0) << "resident seed must serve without invoking the fallback";

  // Repeated reads stay resident-served (and are not warm-cached: the slot is
  // the single owner of the bytes).
  (void)store_.latestAt(id, 10);
  EXPECT_EQ(fetch_calls_->load(), 0);
  EXPECT_EQ(store_.memoryUsage(id), 0U) << "seeded bytes are pool-accounted, not series-accounted";
}

TEST_F(ObjectStoreSeedTest, EvictedSeedFallsBackToFetchWithIdenticalBytes) {
  store_.setResidentPayloadPool(pool_);
  const auto id = registerTopic();

  ASSERT_TRUE(store_.pushLazyWithSeed(id, 10, makePayload(64, 0xEE), countingFallback(fetch_calls_, 64, 0xEE)));
  pool_->trim();

  auto entry = store_.latestAt(id, 10);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->payload.bytes.size(), 64U);
  EXPECT_EQ(entry->payload.bytes[0], 0xEE);
  EXPECT_EQ(fetch_calls_->load(), 1);

  // Post-eviction the entry behaves like plain pushLazy: the warm cache now
  // memoizes the fallback resolve, so a repeat hit does not re-fetch.
  (void)store_.latestAt(id, 10);
  EXPECT_EQ(fetch_calls_->load(), 1);
}

TEST_F(ObjectStoreSeedTest, NoPoolDegradesToPlainLazy) {
  const auto id = registerTopic();

  ASSERT_TRUE(store_.pushLazyWithSeed(id, 10, makePayload(64, 0x11), countingFallback(fetch_calls_, 64, 0x11)));

  auto entry = store_.latestAt(id, 10);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(fetch_calls_->load(), 1) << "without a pool the seed is dropped and the fallback used";
}

TEST_F(ObjectStoreSeedTest, EntryEvictionRetiresSeededBytesFromPool) {
  store_.setResidentPayloadPool(pool_);
  const auto id = registerTopic();
  store_.setRetentionBudget(id, PJ::RetentionBudget{.time_window_ns = 0, .max_memory_bytes = 0});

  ASSERT_TRUE(store_.pushLazyWithSeed(id, 10, makePayload(64, 1), countingFallback(fetch_calls_, 64, 1)));
  EXPECT_EQ(pool_->stats().resident_bytes, 64U);

  store_.removeTopic(id);
  EXPECT_EQ(pool_->stats().resident_bytes, 0U) << "destroyed entries must retire their pool charge";
}

TEST_F(ObjectStoreSeedTest, FlushToKeepsSeededEntriesServable) {
  store_.setResidentPayloadPool(pool_);
  const auto id = registerTopic();
  ASSERT_TRUE(store_.pushLazyWithSeed(id, 10, makePayload(64, 0x42), countingFallback(fetch_calls_, 64, 0x42)));

  // Destination shares descriptors but has NO pool: the moved entry must keep
  // serving from its (still-pool-owned) slot.
  ObjectStore dst;
  auto dst_id = dst.registerTopic(ObjectTopicDescriptor{.dataset_id = 1, .topic_name = "/seeded", .metadata_json = ""});
  ASSERT_TRUE(dst_id.has_value());
  ASSERT_TRUE(store_.flushTo(dst));

  auto entry = dst.latestAt(*dst_id, 10);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->payload.bytes[0], 0x42);
  EXPECT_EQ(fetch_calls_->load(), 0);
}

}  // namespace
