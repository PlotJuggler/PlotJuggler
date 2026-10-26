// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/async_frame_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace PJ {
namespace {

DecodedFrame tinyFrame(int64_t pts) {
  DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(3, uint8_t{0});  // 1x1 RGB888
  frame.width = 1;
  frame.height = 1;
  frame.format = PixelFormat::kRGB888;
  frame.pts = pts;
  return frame;
}

void failOnError(const char* what) {
  FAIL() << "unexpected decode error: " << what;
}

// Wait until `pred` holds, far below the test timeout but robust to slow CI.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

TEST(AsyncFrameWorkerTest, DecodesAndDeliversThroughMailbox) {
  AsyncFrameWorker worker;
  worker.start(
      [](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) { w.deposit(tinyFrame(req.target_ns)); },
      failOnError);

  worker.requestDecode(42);
  std::optional<DecodedFrame> got;
  ASSERT_TRUE(waitFor([&] {
    got = worker.take();
    return got.has_value();
  }));
  EXPECT_EQ(got->pts, 42);
  // Mailbox drains: a second take with no new deposit is empty.
  EXPECT_FALSE(worker.take().has_value());
}

TEST(AsyncFrameWorkerTest, EqualTargetsAreDeduplicated) {
  std::atomic<int> decodes{0};
  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) {
        decodes.fetch_add(1);
        w.deposit(tinyFrame(req.target_ns));
      },
      failOnError);

  worker.requestDecode(7);
  ASSERT_TRUE(waitFor([&] { return decodes.load() == 1; }));
  worker.requestDecode(7);  // same target — must not dispatch again
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(decodes.load(), 1);
}

TEST(AsyncFrameWorkerTest, LatestTargetWinsUnderBurst) {
  // Block the first decode so a burst of requests lands while it is in flight;
  // the worker must then dispatch exactly once more, with the LAST target.
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::mutex seen_mutex;
  std::vector<int64_t> seen;

  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker&) {
        {
          std::lock_guard lock(seen_mutex);
          seen.push_back(req.target_ns);
        }
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&] { return gate_open; });
      },
      failOnError);

  worker.requestDecode(1);
  ASSERT_TRUE(waitFor([&] {
    std::lock_guard lock(seen_mutex);
    return seen.size() == 1;
  }));
  for (int64_t ts = 2; ts <= 9; ++ts) {
    worker.requestDecode(ts);  // all coalesce while decode #1 blocks
  }
  {
    std::lock_guard lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();

  ASSERT_TRUE(waitFor([&] {
    std::lock_guard lock(seen_mutex);
    return seen.size() == 2;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::lock_guard lock(seen_mutex);
  ASSERT_EQ(seen.size(), 2u);  // burst coalesced to one extra dispatch
  EXPECT_EQ(seen[0], 1);
  EXPECT_EQ(seen[1], 9);  // ...carrying the latest target
}

TEST(AsyncFrameWorkerTest, InvalidateForcesRedecodeAtSameTarget) {
  std::atomic<int> decodes{0};
  std::atomic<int> forced{0};
  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker&) {
        decodes.fetch_add(1);
        if (req.force_redecode) {
          forced.fetch_add(1);
        }
      },
      failOnError);

  worker.requestDecode(5);
  ASSERT_TRUE(waitFor([&] { return decodes.load() == 1; }));
  EXPECT_EQ(forced.load(), 0);

  worker.invalidate();
  worker.requestDecode(5);  // same target now goes through, flagged forced
  ASSERT_TRUE(waitFor([&] { return decodes.load() == 2; }));
  EXPECT_EQ(forced.load(), 1);
}

TEST(AsyncFrameWorkerTest, FrameReadyCallbackFiresPerDeposit) {
  std::atomic<int> notifications{0};
  AsyncFrameWorker worker;
  worker.start(
      [](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) {
        // Two deposits per request: preview + settled frame (the video shape).
        w.deposit(tinyFrame(req.target_ns - 1));
        w.deposit(tinyFrame(req.target_ns));
      },
      failOnError);
  worker.setFrameReadyCallback([&] { notifications.fetch_add(1); });

  worker.requestDecode(100);
  ASSERT_TRUE(waitFor([&] { return notifications.load() == 2; }));
  // Latest deposit wins in the single-slot mailbox.
  auto got = worker.take();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->pts, 100);
}

TEST(AsyncFrameWorkerTest, CancelTokenPreemptsInFlightDecode) {
  std::atomic<int> started{0};
  std::atomic<int> cancelled_observed{0};
  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker&) {
        ASSERT_NE(req.cancel, nullptr);
        started.fetch_add(1);
        // Emulate a long GOP decode polling its token between units.
        for (int i = 0; i < 2000; ++i) {
          if (req.cancel->isCancelled()) {
            cancelled_observed.fetch_add(1);
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      failOnError, AsyncFrameWorker::Options{.use_cancel_token = true, .preempt_predicate = {}});

  worker.requestDecode(1);
  ASSERT_TRUE(waitFor([&] { return started.load() == 1; }));
  worker.requestDecode(2);  // preempts decode #1 via its token
  ASSERT_TRUE(waitFor([&] { return cancelled_observed.load() >= 1; }));
}

TEST(AsyncFrameWorkerTest, PreemptPredicateGatesCancellation) {
  // Contiguous-playback contract: a small forward step must NOT cancel the
  // in-flight decode (a cancel costs the source a full GOP re-seek — see
  // Options::preempt_predicate), while a large jump must preempt it.
  std::mutex seen_mutex;
  std::vector<int64_t> seen;
  std::atomic<int> cancels_observed{0};
  std::atomic<bool> gate_open{false};

  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) {
        {
          std::lock_guard lock(seen_mutex);
          seen.push_back(req.target_ns);
        }
        while (true) {
          if (req.cancel->isCancelled()) {
            cancels_observed.fetch_add(1);
            return;
          }
          if (gate_open.load()) {
            w.deposit(tinyFrame(req.target_ns));
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      failOnError,
      AsyncFrameWorker::Options{
          .use_cancel_token = true,
          .preempt_predicate = [](Timestamp in_flight, Timestamp incoming) { return incoming - in_flight > 100; }});

  worker.requestDecode(1000);
  ASSERT_TRUE(waitFor([&] {
    std::lock_guard lock(seen_mutex);
    return seen.size() == 1;
  }));

  worker.requestDecode(1001);  // contiguous step: must NOT preempt
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(cancels_observed.load(), 0);

  worker.requestDecode(2000);  // large jump: must preempt the in-flight decode
  ASSERT_TRUE(waitFor([&] { return cancels_observed.load() == 1; }));

  // The worker then serves the latest coalesced target (2000; 1001 was
  // overwritten and never dispatched).
  gate_open.store(true);
  std::optional<DecodedFrame> got;
  ASSERT_TRUE(waitFor([&] {
    got = worker.take();
    return got.has_value();
  }));
  EXPECT_EQ(got->pts, 2000);
  std::lock_guard lock(seen_mutex);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], 1000);
  EXPECT_EQ(seen[1], 2000);
}

TEST(AsyncFrameWorkerTest, ExceptionBarrierKeepsWorkerServing) {
  std::atomic<int> errors{0};
  std::atomic<int> decodes{0};
  std::string last_error;
  std::mutex error_mutex;
  AsyncFrameWorker worker;
  worker.start(
      [&](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) {
        decodes.fetch_add(1);
        if (req.target_ns == 1) {
          throw std::runtime_error("boom");
        }
        w.deposit(tinyFrame(req.target_ns));
      },
      [&](const char* what) {
        std::lock_guard lock(error_mutex);
        last_error = what;
        errors.fetch_add(1);
      });

  worker.requestDecode(1);
  ASSERT_TRUE(waitFor([&] { return errors.load() == 1; }));
  {
    std::lock_guard lock(error_mutex);
    EXPECT_EQ(last_error, "boom");
  }
  // The worker survived and serves the next request.
  worker.requestDecode(2);
  std::optional<DecodedFrame> got;
  ASSERT_TRUE(waitFor([&] {
    got = worker.take();
    return got.has_value();
  }));
  EXPECT_EQ(got->pts, 2);
  EXPECT_EQ(decodes.load(), 2);
}

TEST(AsyncFrameWorkerTest, TeardownUnderLoadNeverHangs) {
  // Lost-wakeup regression (the historical scene2d_dock_widget_test deadlock):
  // hammer construct → request → destruct; a racy stop would hang the join.
  for (int i = 0; i < 200; ++i) {
    AsyncFrameWorker worker;
    worker.start(
        [](const AsyncFrameWorker::Request& req, AsyncFrameWorker& w) { w.deposit(tinyFrame(req.target_ns)); },
        failOnError);
    worker.requestDecode(i);
    if (i % 2 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    // worker destructs here, racing the in-flight decode/wait.
  }
  SUCCEED();
}

TEST(AsyncFrameWorkerTest, NoDecodeStartsAfterStopReturns) {
  // A worker destroyed right after start() races its own thread's start: a
  // thread that had not begun is detached rather than joined (a WebAssembly
  // thread created with the pool exhausted cannot start while the main thread
  // waits), and must then never run the DecodeFn of a destroyed owner.
  std::atomic<int> late_decodes{0};
  for (int i = 0; i < 1000; ++i) {
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    {
      AsyncFrameWorker worker;
      worker.start(
          [stopped, &late_decodes](const AsyncFrameWorker::Request&, AsyncFrameWorker&) {
            if (stopped->load()) {
              ++late_decodes;
            }
          },
          failOnError);
      worker.requestDecode(i);
      worker.stop();
      stopped->store(true);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(late_decodes.load(), 0);
}

TEST(AsyncFrameWorkerTest, NeverStartedWorkerDestructsSafely) {
  AsyncFrameWorker worker;
  EXPECT_FALSE(worker.take().has_value());
}

}  // namespace
}  // namespace PJ
