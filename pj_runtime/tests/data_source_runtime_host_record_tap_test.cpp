// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// The record tap sees every pushed message's raw bytes BEFORE parsing, on the
// push thread, each carrying a view of its binding; kStopRecording detaches it.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pj_runtime/RecordTap.h"
#include "runtime_host_test_fixture.h"

namespace {

// Records what the host handed the tap. The views die with the call, so every
// field is copied. The mutex is what the documented contract demands (the host
// calls the tap from the push thread) and what lets the threaded test below
// read the count from the test thread.
struct CountingTap : PJ::RecordTap {
  struct Seen {
    uint32_t binding_id = 0;
    std::string topic;
    std::string encoding;
    std::string type_name;
    int64_t log_time_ns = 0;
    std::vector<uint8_t> bytes;
  };

  std::mutex mutex;
  std::vector<Seen> messages;
  PJ::TapVerdict verdict = PJ::TapVerdict::kContinue;

  PJ::TapVerdict onMessage(
      uint32_t binding_id, const PJ::RecordedBindingView& binding, int64_t log_time_ns,
      PJ::Span<const uint8_t> bytes) override {
    std::lock_guard lock(mutex);
    messages.push_back(
        Seen{
            .binding_id = binding_id,
            .topic = std::string(binding.topic),
            .encoding = std::string(binding.encoding),
            .type_name = std::string(binding.type_name),
            .log_time_ns = log_time_ns,
            .bytes = std::vector<uint8_t>(bytes.begin(), bytes.end()),
        });
    return verdict;
  }
};

class DataSourceRuntimeHostRecordTapTest : public PJ::test::RuntimeHostFixture {
 protected:
  DataSourceRuntimeHostRecordTapTest() : RuntimeHostFixture("record_tap_test_source") {}
};

TEST_F(DataSourceRuntimeHostRecordTapTest, OrderlyRequestStopDoesNotSetLastError) {
  auto runtime_host = runtime();
  runtime_host.requestStop(PJ::DataSourceState::kStopped, "end of stream");

  EXPECT_TRUE(runtime_host.isStopRequested());
  EXPECT_TRUE(host->lastError().empty()) << "an orderly STOPPED end must not be reported as an error (Q8-A3)";
}

TEST_F(DataSourceRuntimeHostRecordTapTest, FailedRequestStopSetsLastError) {
  auto runtime_host = runtime();
  runtime_host.requestStop(PJ::DataSourceState::kFailed, "boom");

  EXPECT_TRUE(runtime_host.isStopRequested());
  EXPECT_EQ(host->lastError(), "boom");
}

TEST_F(DataSourceRuntimeHostRecordTapTest, SeesNewBindingsAndRawMessages) {
  auto tap = std::make_shared<CountingTap>();
  host->setRecordTap(tap);

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  auto same = bind("/camera/image");  // identical signature: the same binding, so the same id
  ASSERT_TRUE(same.has_value()) << same.error();
  push(*binding, 123, {0x10, 0x20});
  push(*binding, 124, {0x30});

  std::lock_guard lock(tap->mutex);
  ASSERT_EQ(tap->messages.size(), 2U);
  for (const auto& seen : tap->messages) {
    EXPECT_EQ(seen.binding_id, tap->messages.front().binding_id);
    EXPECT_EQ(seen.topic, "/camera/image");
    EXPECT_EQ(seen.encoding, "runtime_host_object");
    EXPECT_EQ(seen.type_name, "mock/image");
  }
  EXPECT_EQ(tap->messages[0].log_time_ns, 123);
  EXPECT_EQ(tap->messages[0].bytes, (std::vector<uint8_t>{0x10, 0x20}));
  EXPECT_EQ(tap->messages[1].log_time_ns, 124);
  EXPECT_EQ(tap->messages[1].bytes, (std::vector<uint8_t>{0x30}));
}

TEST_F(DataSourceRuntimeHostRecordTapTest, StopVerdictDetachesTheTapAndIngestContinues) {
  auto tap = std::make_shared<CountingTap>();
  tap->verdict = PJ::TapVerdict::kStopRecording;
  host->setRecordTap(tap);

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  push(*binding, 1, {1});
  push(*binding, 2, {2});  // must still ingest, must NOT reach the detached tap

  EXPECT_EQ(objectEntryCount("/camera/image"), 2U);
  std::lock_guard lock(tap->mutex);
  EXPECT_EQ(tap->messages.size(), 1U);
}

TEST_F(DataSourceRuntimeHostRecordTapTest, ClearingTheTapStopsCalls) {
  auto tap = std::make_shared<CountingTap>();
  host->setRecordTap(tap);

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  host->setRecordTap(nullptr);
  push(*binding, 1, {1});

  std::lock_guard lock(tap->mutex);
  EXPECT_TRUE(tap->messages.empty());
}

TEST_F(DataSourceRuntimeHostRecordTapTest, ThrowingTapIsDetachedAndIngestContinues) {
  struct ThrowingTap : CountingTap {
    std::atomic<int> calls{0};
    PJ::TapVerdict onMessage(uint32_t, const PJ::RecordedBindingView&, int64_t, PJ::Span<const uint8_t>) override {
      calls.fetch_add(1);
      throw std::runtime_error("boom");
    }
  };
  auto tap = std::make_shared<ThrowingTap>();
  host->setRecordTap(tap);

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  push(*binding, 1, {1});
  push(*binding, 2, {2});

  EXPECT_EQ(tap->calls.load(), 1) << "the throwing tap must be detached, not called again";
  EXPECT_EQ(objectEntryCount("/camera/image"), 2U);
}

// What makes mid-stream recording work at all: a binding minted before the tap
// was attached still reaches it, carrying the same view as any other.
TEST_F(DataSourceRuntimeHostRecordTapTest, PreExistingBindingIsRecordedWithItsView) {
  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();

  auto tap = std::make_shared<CountingTap>();
  host->setRecordTap(tap);
  push(*binding, 1, {1});
  push(*binding, 2, {2});

  std::lock_guard lock(tap->mutex);
  ASSERT_EQ(tap->messages.size(), 2U);
  for (const auto& seen : tap->messages) {
    EXPECT_EQ(seen.topic, "/camera/image");
    EXPECT_EQ(seen.encoding, "runtime_host_object");
    EXPECT_EQ(seen.type_name, "mock/image");
  }
}

// The tap slot is the one thing two threads touch at once in production: the
// GUI installs and removes taps while the source's push thread streams. Every
// push here comes from ONE worker, as for a real source (a parser binding is
// single-threaded by design); only setRecordTap runs on the test thread. Flips
// are paced by the worker's progress rather than by sleeps, so each install
// window spans real pushes, and every wait is bounded by the worker's fixed
// push count, so the test cannot hang.
TEST_F(DataSourceRuntimeHostRecordTapTest, TapCanBeInstalledAndRemovedWhileAnotherThreadPushes) {
  constexpr int kFlips = 200;  // even: the last flip removes the tap
  static_assert(kFlips % 2 == 0);
  constexpr int kPushesPerFlip = 20;
  constexpr int kPushes = (kFlips + 1) * kPushesPerFlip;  // one window's worth left after the last flip

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  // Taken on the test thread: runtime() carries a gtest check, and the worker
  // must run none.
  const auto runtime_host = runtime();
  auto tap = std::make_shared<CountingTap>();
  const auto seen = [&tap]() -> size_t {
    std::lock_guard lock(tap->mutex);
    return tap->messages.size();
  };

  std::atomic<int> pushed{0};  // completed pushes, tap call included
  std::atomic<int> failed{0};
  // Returns once the worker has completed `count` pushes; `count` never
  // exceeds kPushes, so the worker always gets there.
  const auto waitForPushed = [&pushed](int count) {
    while (pushed.load() < count) {
      std::this_thread::yield();
    }
  };

  // Installed before the worker exists, so the first window is certain to be
  // recorded: thread start orders the store before the first push.
  host->setRecordTap(tap);
  std::thread worker([&runtime_host, &binding, &pushed, &failed] {
    for (int i = 0; i < kPushes; ++i) {
      const std::vector<uint8_t> payload{static_cast<uint8_t>(i & 0xFF)};
      auto status = runtime_host.pushMessage(*binding, i + 1, [payload]() -> std::vector<uint8_t> { return payload; });
      if (!status.has_value()) {
        failed.fetch_add(1);
      }
      pushed.fetch_add(1);
    }
  });

  for (int flip = 1; flip < kFlips; ++flip) {
    waitForPushed(flip * kPushesPerFlip);
    if (flip % 2 == 0) {
      host->setRecordTap(tap);
    } else {
      host->setRecordTap(nullptr);
    }
  }

  // After the final removal, at most the ONE push already holding a snapshot
  // may still reach the tap. Once two more pushes have completed, the second
  // of them started after the removal, so from there on nothing may arrive.
  const int pushed_at_removal = pushed.load();
  const size_t seen_at_removal = seen();
  waitForPushed(std::min(pushed_at_removal + 2, kPushes));
  const size_t seen_after_two_more = seen();
  worker.join();

  EXPECT_EQ(failed.load(), 0) << "ingest must never fail because of the tap slot";
  EXPECT_EQ(pushed.load(), kPushes);
  EXPECT_EQ(objectEntryCount("/camera/image"), static_cast<uint64_t>(kPushes)) << "every push must still ingest";
  EXPECT_LE(seen_after_two_more - seen_at_removal, 1U);
  EXPECT_EQ(seen(), seen_after_two_more) << "no call may reach a tap after its removal";
  EXPECT_GE(seen(), static_cast<size_t>(kPushesPerFlip)) << "the first window ran with the tap installed";
  EXPECT_LE(seen(), static_cast<size_t>(kPushes));
  std::lock_guard lock(tap->mutex);
  for (const auto& message : tap->messages) {
    EXPECT_EQ(message.topic, "/camera/image");
    EXPECT_EQ(message.bytes.size(), 1U);
  }
}

TEST_F(DataSourceRuntimeHostRecordTapTest, PureLazyPushesAreCountedNotRecorded) {
  host->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kPureLazy);
  auto tap = std::make_shared<CountingTap>();
  host->setRecordTap(tap);

  auto binding = bind("/camera/image");
  ASSERT_TRUE(binding.has_value()) << binding.error();
  push(*binding, 1, {1});

  EXPECT_EQ(host->recordTapSkippedLazy(), 1U);
  std::lock_guard lock(tap->mutex);
  EXPECT_TRUE(tap->messages.empty());
}

}  // namespace
