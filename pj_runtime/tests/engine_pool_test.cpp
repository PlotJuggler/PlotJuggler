// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// EnginePool runs ready tasks: compute on a worker, publish back on the thread
// that built the pool, in bounded drain passes. Per-node scheduling (one running
// run, one pending request) is the consumer's job (D30), so nothing here queues,
// coalesces or supersedes. Every wait is a latch or a bounded spin, never a sleep;
// waits inside the pool itself (`waitIdle`) are guarded by a watchdog.

#include <gtest/gtest.h>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QObject>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pj_runtime/EnginePool.h"

namespace PJ {
namespace {

using Token = EnginePool::CancelToken;
using Outcome = EnginePool::Outcome;
using Status = Outcome::Status;

/// A one-shot latch: opened on one thread, awaited on another. `wait` gives up
/// after a deadline, so a broken pool fails the test instead of hanging it.
/// `open` notifies under the lock, so a waiter that returns may destroy the gate.
class Gate {
 public:
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    cv_.notify_all();
  }

  [[nodiscard]] bool wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

/// Aborts the process unless destroyed within a deadline. For waits the pool itself
/// owns (`waitIdle` has no deadline): a regression must fail the test, not hang it.
class Watchdog {
 public:
  explicit Watchdog(const char* waiting_for) {
    thread_ = std::thread([this, waiting_for] {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!cv_.wait_for(lock, std::chrono::seconds(5), [this] { return disarmed_; })) {
        std::fprintf(stderr, "WATCHDOG: still waiting for %s after 5 s\n", waiting_for);
        std::abort();
      }
    });
  }

  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      disarmed_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool disarmed_ = false;
  std::thread thread_;
};

/// Spins WITHOUT pumping the event loop (so nothing gets published meanwhile)
/// until `condition` holds; false after a 5 s deadline.
template <typename Condition>
[[nodiscard]] bool spinUntil(Condition condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!condition()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/// A compute that polls its token until cancelled, as a real script loop does;
/// gives up after a deadline so a token that never fires fails the test.
void computeUntilCancelled(const Token& token) {
  static_cast<void>(spinUntil([&] { return token.cancelled(); }));
}

TEST(EnginePoolTest, PublishRunsOnTheConstructingThreadAndComputeOffIt) {
  EnginePool pool;
  const std::thread::id owner = std::this_thread::get_id();
  std::thread::id compute_thread;
  std::thread::id publish_thread;
  int publishes = 0;
  Outcome delivered{.status = Status::kCancelled};

  pool.submit({
      .compute = [&](const Token&) { compute_thread = std::this_thread::get_id(); },
      .publish =
          [&](const Outcome& outcome) {
            publish_thread = std::this_thread::get_id();
            delivered = outcome;
            ++publishes;
          },
  });
  pool.waitIdle();

  EXPECT_EQ(publishes, 1);
  EXPECT_EQ(delivered.status, Status::kCompleted);
  EXPECT_TRUE(delivered.error.empty());
  EXPECT_EQ(publish_thread, owner);
  EXPECT_NE(compute_thread, std::thread::id{});
  EXPECT_NE(compute_thread, owner) << "a script must never run on the GUI thread";
}

TEST(EnginePoolTest, CancelledRunStopsEarlyAndPublishesCancelled) {
  EnginePool pool;
  Gate started;
  std::atomic<bool> compute_saw_cancel{false};
  int publishes = 0;
  Status delivered = Status::kCompleted;

  const Token token = pool.submit({
      .compute =
          [&](const Token& run_token) {
            started.open();
            computeUntilCancelled(run_token);
            compute_saw_cancel = run_token.cancelled();
          },
      .publish =
          [&](const Outcome& outcome) {
            delivered = outcome.status;
            ++publishes;
          },
  });
  ASSERT_TRUE(started.wait());
  token.cancel();
  pool.waitIdle();

  EXPECT_TRUE(compute_saw_cancel.load()) << "the running compute must see the cancel and return";
  EXPECT_EQ(publishes, 1) << "a cancelled run is still published, once";
  EXPECT_EQ(delivered, Status::kCancelled);
}

// The consumer cancels after the compute finished but before the GUI drain ran:
// the result must be delivered as cancelled, so it cannot overwrite newer state.
TEST(EnginePoolTest, CancelAfterComputeBeforeDrainPublishesCancelled) {
  EnginePool pool;
  int visible_state = 0;
  Status delivered = Status::kCompleted;

  const Token token = pool.submit({
      .compute = [](const Token&) {},
      .publish =
          [&](const Outcome& outcome) {
            delivered = outcome.status;
            if (outcome.status == Status::kCompleted) {
              visible_state = 1;
            }
          },
  });
  ASSERT_TRUE(spinUntil([&] { return pool.pendingPublishes() == 1; }));
  token.cancel();
  visible_state = 2;  // the newer state the consumer installed meanwhile
  pool.waitIdle();

  EXPECT_EQ(delivered, Status::kCancelled);
  EXPECT_EQ(visible_state, 2);
}

// The Python lane has one thread, so a run queued behind a busy one is
// guaranteed not to have started when it is cancelled: its compute is skipped.
TEST(EnginePoolTest, CancelledBeforeStartSkipsComputeAndLeavesOtherRunsAlone) {
  EnginePool pool;
  Gate blocker_started;
  Gate release_blocker;
  std::atomic<bool> skipped_compute_ran{false};
  Status blocker_status = Status::kCancelled;
  Status skipped_status = Status::kCompleted;

  pool.submit(
      {
          .compute =
              [&](const Token&) {
                blocker_started.open();
                EXPECT_TRUE(release_blocker.wait());
              },
          .publish = [&](const Outcome& outcome) { blocker_status = outcome.status; },
      },
      EnginePool::Lane::kPython);
  ASSERT_TRUE(blocker_started.wait());
  const Token queued = pool.submit(
      {
          .compute = [&](const Token&) { skipped_compute_ran = true; },
          .publish = [&](const Outcome& outcome) { skipped_status = outcome.status; },
      },
      EnginePool::Lane::kPython);
  queued.cancel();
  release_blocker.open();
  pool.waitIdle();

  EXPECT_FALSE(skipped_compute_ran.load());
  EXPECT_EQ(skipped_status, Status::kCancelled);
  EXPECT_EQ(blocker_status, Status::kCompleted) << "cancelling one run must not touch another";
}

TEST(EnginePoolTest, DrainPublishesAtMost32PerPass) {
  EnginePool pool;
  constexpr int kTasks = 100;
  int published_in_pass = 0;
  std::vector<int> passes;
  QObject::connect(&pool, &EnginePool::drained, [&] {
    passes.push_back(published_in_pass);
    published_in_pass = 0;
  });

  for (int task_index = 0; task_index < kTasks; ++task_index) {
    pool.submit({.compute = [](const Token&) {}, .publish = [&](const Outcome&) { ++published_in_pass; }});
  }
  // Every result is waiting before the first drain runs, so the passes are exact.
  ASSERT_TRUE(spinUntil([&] { return pool.pendingPublishes() == kTasks; }));
  pool.waitIdle();

  static_assert(EnginePool::kMaxPublishesPerDrain == 32);
  EXPECT_EQ(passes, (std::vector<int>{32, 32, 32, 4}));
}

// That the lane's thread outlives an idle period (setExpiryTimeout(-1)) is not
// asserted: Qt offers no way to expire it early, and waiting out the 30 s default
// is not a unit test.
TEST(EnginePoolTest, PythonLaneIsOneThreadAndDoesNotBlockTheSharedLane) {
  EnginePool pool;
  Gate first_started;
  Gate release_first;
  Gate shared_done;
  std::thread::id first_thread;
  std::thread::id second_thread;

  pool.submit(
      {.compute =
           [&](const Token&) {
             first_thread = std::this_thread::get_id();
             first_started.open();
             EXPECT_TRUE(release_first.wait());
           },
       .publish = [](const Outcome&) {}},
      EnginePool::Lane::kPython);
  ASSERT_TRUE(first_started.wait());
  pool.submit(
      {.compute = [&](const Token&) { second_thread = std::this_thread::get_id(); }, .publish = [](const Outcome&) {}},
      EnginePool::Lane::kPython);
  pool.submit({.compute = [&](const Token&) { shared_done.open(); }, .publish = [](const Outcome&) {}});

  EXPECT_TRUE(shared_done.wait()) << "a busy Python lane must not hold up the shared lane";
  release_first.open();
  pool.waitIdle();

  EXPECT_NE(first_thread, std::thread::id{});
  EXPECT_EQ(first_thread, second_thread);
}

// Under D30 the consumer submits its pending request from the publish of the run
// that just finished; waitIdle must cover the run submitted that way too.
TEST(EnginePoolTest, PublishMaySubmitTheNextRun) {
  EnginePool pool;
  std::vector<int> published;

  pool.submit({
      .compute = [](const Token&) {},
      .publish =
          [&](const Outcome&) {
            published.push_back(1);
            pool.submit({.compute = [](const Token&) {}, .publish = [&](const Outcome&) { published.push_back(2); }});
          },
  });
  pool.waitIdle();

  EXPECT_EQ(published, (std::vector<int>{1, 2}));
}

// The result is posted only once the GUI thread is about to block inside waitIdle, so the
// posted drain is the only wake-up the wait gets.
TEST(EnginePoolTest, WaitIdleReturnsWhenTheResultIsPostedWhileItBlocks) {
  std::atomic<bool> blocking{false};
  EnginePool pool;
  QObject::connect(
      QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, &pool, [&] { blocking = true; },
      Qt::DirectConnection);
  std::atomic<bool> saw_blocking{false};
  int publishes = 0;

  pool.submit({
      .compute = [&](const Token&) { saw_blocking = spinUntil([&] { return blocking.load(); }); },
      .publish = [&](const Outcome&) { ++publishes; },
  });
  const Watchdog watchdog("waitIdle, result posted while it blocks");
  pool.waitIdle();

  EXPECT_TRUE(saw_blocking) << "the GUI thread never blocked, so the posted drain was not the only wake-up";
  EXPECT_EQ(publishes, 1);
}

// A publish that waits for another task and then submits more work: the inner wait's final
// drain must not end the outer wait, which still has the new task outstanding.
TEST(EnginePoolTest, WaitIdleCoversWorkSubmittedAfterANestedWaitIdle) {
  std::atomic<bool> last_submitted{false};
  std::atomic<bool> blocking{false};
  std::atomic<bool> outer_returned{false};
  EnginePool pool;
  QObject::connect(
      QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, &pool,
      [&] {
        if (last_submitted) {
          blocking = true;
        }
      },
      Qt::DirectConnection);
  Gate release_inner;
  bool last_published = false;

  pool.submit({
      .compute = [&](const Token&) { EXPECT_TRUE(release_inner.wait()); },
      .publish = [](const Outcome&) {},
  });
  // The one-thread Python lane keeps this run clear of the shared lane's blocked one.
  pool.submit(
      {
          .compute = [](const Token&) {},
          .publish =
              [&](const Outcome&) {
                release_inner.open();
                pool.waitIdle();
                // Held until the outer wait blocks again, or returns early (the failure).
                pool.submit({
                    .compute =
                        [&](const Token&) {
                          static_cast<void>(spinUntil([&] { return blocking.load() || outer_returned.load(); }));
                        },
                    .publish = [&](const Outcome&) { last_published = true; },
                });
                last_submitted = true;
              },
      },
      EnginePool::Lane::kPython);
  {
    const Watchdog watchdog("waitIdle, nested wait followed by new work");
    pool.waitIdle();
  }
  const bool published_before_return = last_published;
  outer_returned = true;
  pool.waitIdle();  // failing case: lets the held run finish before the pool is destroyed

  EXPECT_TRUE(published_before_return) << "the outer waitIdle returned with the last task outstanding";
}

TEST(EnginePoolTest, WaitIdleReturnsWithNothingOutstanding) {
  EnginePool pool;
  pool.waitIdle();  // idle from the start: returns at once

  constexpr int kTasks = 10;
  int publishes = 0;
  for (int task_index = 0; task_index < kTasks; ++task_index) {
    pool.submit(
        {.compute = [](const Token&) {}, .publish = [&](const Outcome&) { ++publishes; }},
        task_index % 2 == 0 ? EnginePool::Lane::kShared : EnginePool::Lane::kPython);
  }
  pool.waitIdle();

  EXPECT_EQ(publishes, kTasks);
}

// A publish may destroy the pool (directly, or through a nested event loop and
// deleteLater): the drain it runs in must stop touching the pool and publish nothing more.
TEST(EnginePoolTest, PublishThatDestroysThePoolAbandonsTheRemainingResults) {
  auto pool = std::make_unique<EnginePool>();
  int first_publishes = 0;
  int second_publishes = 0;

  // The one-thread Python lane finishes the runs in submission order.
  pool->submit(
      {
          .compute = [](const Token&) {},
          .publish =
              [&](const Outcome&) {
                ++first_publishes;
                pool.reset();
              },
      },
      EnginePool::Lane::kPython);
  pool->submit(
      {.compute = [](const Token&) {}, .publish = [&](const Outcome&) { ++second_publishes; }},
      EnginePool::Lane::kPython);
  // Both results are waiting before the drain runs, so one pass would publish both.
  ASSERT_TRUE(spinUntil([&] { return pool->pendingPublishes() == 2; }));
  QCoreApplication::processEvents();

  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(first_publishes, 1);
  EXPECT_EQ(second_publishes, 0) << "the destroyed pool must not publish the rest";
}

TEST(EnginePoolTest, ThrowingComputePublishesFailedAndThePoolKeepsWorking) {
  EnginePool pool;
  Outcome standard;
  Outcome unknown;
  Outcome python_failed;
  Outcome shared_after;
  Outcome python_after;
  bool shared_after_ran = false;
  bool python_after_ran = false;

  pool.submit({
      .compute = [](const Token&) { throw std::runtime_error("boom"); },
      .publish = [&](const Outcome& outcome) { standard = outcome; },
  });
  pool.submit({
      .compute = [](const Token&) { throw 42; },
      .publish = [&](const Outcome& outcome) { unknown = outcome; },
  });
  pool.submit(
      {
          .compute = [](const Token&) { throw std::runtime_error("python boom"); },
          .publish = [&](const Outcome& outcome) { python_failed = outcome; },
      },
      EnginePool::Lane::kPython);
  pool.waitIdle();
  pool.submit({
      .compute = [&](const Token&) { shared_after_ran = true; },
      .publish = [&](const Outcome& outcome) { shared_after = outcome; },
  });
  pool.submit(
      {
          .compute = [&](const Token&) { python_after_ran = true; },
          .publish = [&](const Outcome& outcome) { python_after = outcome; },
      },
      EnginePool::Lane::kPython);
  pool.waitIdle();

  EXPECT_EQ(standard.status, Status::kFailed);
  EXPECT_EQ(standard.error, "boom");
  EXPECT_EQ(unknown.status, Status::kFailed);
  EXPECT_EQ(unknown.error, "unknown exception");
  EXPECT_EQ(python_failed.status, Status::kFailed);
  EXPECT_EQ(python_failed.error, "python boom");
  EXPECT_TRUE(shared_after_ran);
  EXPECT_EQ(shared_after.status, Status::kCompleted);
  EXPECT_TRUE(python_after_ran) << "the Python lane's one thread must survive a throwing compute";
  EXPECT_EQ(python_after.status, Status::kCompleted);
}

// A run cancelled by its consumer reports the cancel even when its compute threw
// because of it: the consumer discards the result either way.
TEST(EnginePoolTest, CancelWinsOverFailed) {
  EnginePool pool;
  Gate started;
  Status delivered = Status::kCompleted;

  const Token token = pool.submit({
      .compute =
          [&](const Token& run_token) {
            started.open();
            computeUntilCancelled(run_token);
            throw std::runtime_error("interrupted");
          },
      .publish = [&](const Outcome& outcome) { delivered = outcome.status; },
  });
  ASSERT_TRUE(started.wait());
  token.cancel();
  pool.waitIdle();

  EXPECT_EQ(delivered, Status::kCancelled);
}

// The Python lane is one thread so that a run submitted while another is pending
// finishes in submission order, which the nested-publish checks below rely on.
TEST(EnginePoolTest, WaitIdleInsideAPublishReturnsOnceTheOtherRunsArePublished) {
  EnginePool pool;
  std::vector<std::string> order;

  const auto record = [&](const char* name) { return [&order, name](const Outcome&) { order.emplace_back(name); }; };
  pool.submit(
      {
          .compute = [](const Token&) {},
          .publish =
              [&](const Outcome&) {
                order.emplace_back("first begins");
                const Watchdog watchdog("waitIdle inside a publish");
                pool.waitIdle();
                order.emplace_back("first ends");
              },
      },
      EnginePool::Lane::kPython);
  pool.submit({.compute = [](const Token&) {}, .publish = record("second")}, EnginePool::Lane::kPython);
  pool.submit({.compute = [](const Token&) {}, .publish = record("third")}, EnginePool::Lane::kPython);
  ASSERT_TRUE(spinUntil([&] { return pool.pendingPublishes() == 3; }));
  pool.waitIdle();

  EXPECT_EQ(order, (std::vector<std::string>{"first begins", "second", "third", "first ends"}));
}

/// Records the thread its last owner released it on.
struct DestroyedOn {
  std::thread::id* thread = nullptr;
  ~DestroyedOn() {
    if (thread != nullptr) {
      *thread = std::this_thread::get_id();
    }
  }
};

TEST(EnginePoolTest, DestructorCancelsAndWaitsForWorkersWithoutPublishing) {
  const std::thread::id owner = std::this_thread::get_id();
  std::atomic<bool> running_returned{false};
  std::atomic<bool> running_saw_cancel{false};
  int publishes = 0;
  std::thread::id closures_destroyed_on;
  {
    EnginePool pool;
    Gate started;
    auto probe = std::make_shared<DestroyedOn>();
    probe->thread = &closures_destroyed_on;
    pool.submit({
        .compute =
            [&](const Token& token) {
              started.open();
              computeUntilCancelled(token);
              running_saw_cancel = token.cancelled();
              running_returned = true;
            },
        .publish = [&, probe](const Outcome&) { ++publishes; },
    });
    probe.reset();
    // Python lane: the shared lane may have a single thread, which the blocked compute holds.
    pool.submit(
        {.compute = [](const Token&) {}, .publish = [&](const Outcome&) { ++publishes; }}, EnginePool::Lane::kPython);
    ASSERT_TRUE(started.wait());
    ASSERT_TRUE(spinUntil([&] { return pool.pendingPublishes() == 1; }));
    // Destroyed with one compute running and one result waiting for its drain.
  }
  QCoreApplication::processEvents();  // a drain posted to the dead pool must not run

  EXPECT_TRUE(running_returned.load()) << "the destructor must wait for the running compute";
  EXPECT_TRUE(running_saw_cancel.load()) << "the destructor must cancel the running compute";
  EXPECT_EQ(publishes, 0);
  EXPECT_EQ(closures_destroyed_on, owner) << "task closures must die on the GUI thread";
}

}  // namespace
}  // namespace PJ
