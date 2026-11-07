// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/EnginePool.h"

#include <QEventLoop>
#include <QPointer>
#include <QRunnable>
#include <QThread>
#include <algorithm>
#include <exception>
#include <utility>

#include "pj_runtime/TsanAnnotations.h"

namespace PJ {

struct EnginePool::Run {
  Task task;
  CancelToken token;
  Outcome outcome;  ///< written by the worker before `finish`, read by the drain after it
};

/// One run on a worker thread; Qt owns it from `start` and deletes it after `run`.
class EnginePool::Job final : public QRunnable {
 public:
  Job(EnginePool* pool, std::unique_ptr<Run> run) : pool_(pool), run_(std::move(run)) {}

  void run() override {
    // `this` arrives in a register, not through memory the submitter wrote, so it is
    // the one address this thread can name before the acquire that orders those writes.
    tsan::acquire(this);
    Run& run = *run_;
    if (run.task.compute && !run.token.cancelled()) {
      try {
        run.task.compute(run.token);
      } catch (const std::exception& error) {
        run.outcome = {.status = Outcome::Status::kFailed, .error = error.what()};
      } catch (...) {
        run.outcome = {.status = Outcome::Status::kFailed, .error = "unknown exception"};
      }
    }
    // The run goes back through finish(), so its closures die on the GUI thread.
    pool_->finish(std::move(run_));
  }

 private:
  EnginePool* pool_;
  std::unique_ptr<Run> run_;
};

bool EnginePool::CancelToken::cancelled() const noexcept {
  return (run_cancelled_ != nullptr && run_cancelled_->load(std::memory_order_acquire)) ||
         (pool_stopping_ != nullptr && pool_stopping_->load(std::memory_order_acquire));
}

void EnginePool::CancelToken::cancel() const noexcept {
  if (run_cancelled_ != nullptr) {
    run_cancelled_->store(true, std::memory_order_release);
  }
}

std::vector<std::shared_ptr<const std::atomic<bool>>> EnginePool::CancelToken::flags() const {
  std::vector<std::shared_ptr<const std::atomic<bool>>> flags;
  if (run_cancelled_ != nullptr) {
    flags.push_back(run_cancelled_);
  }
  if (pool_stopping_ != nullptr) {
    flags.push_back(pool_stopping_);
  }
  return flags;
}

EnginePool::EnginePool(QObject* parent) : QObject(parent) {
  shared_lane_.setMaxThreadCount(std::max(1, QThread::idealThreadCount() / 2));
  shared_lane_.setThreadPriority(QThread::LowPriority);
  python_lane_.setMaxThreadCount(1);
  // The one thread must outlive idle periods: it holds the Python interpreter's thread-local state.
  python_lane_.setExpiryTimeout(-1);
  python_lane_.setThreadPriority(QThread::LowPriority);
}

EnginePool::~EnginePool() {
  stopping_->store(true, std::memory_order_release);
  {
    // A worker finishing from here on queues its run without posting a drain.
    std::lock_guard<std::mutex> lock(mutex_);
    drain_posted_ = true;
  }
  // Runs not started yet are deleted unrun (on this thread); then wait for the rest.
  shared_lane_.clear();
  python_lane_.clear();
  shared_lane_.waitForDone();
  python_lane_.waitForDone();
  std::lock_guard<std::mutex> lock(mutex_);
  finished_.clear();  // destroys every pending publish uncalled
}

EnginePool::CancelToken EnginePool::submit(Task task, Lane lane) {
  auto run = std::make_unique<Run>();
  run->task = std::move(task);
  run->token.run_cancelled_ = std::make_shared<std::atomic<bool>>(false);
  run->token.pool_stopping_ = stopping_;
  CancelToken token = run->token;
  ++outstanding_;
  auto* job = new Job(this, std::move(run));
  // Qt's hand-off is invisible to TSan: release once the job is fully built, acquire first thing in run().
  tsan::release(job);
  (lane == Lane::kPython ? python_lane_ : shared_lane_).start(job);
  return token;
}

void EnginePool::finish(std::unique_ptr<Run> run) {
  std::lock_guard<std::mutex> lock(mutex_);
  finished_.push_back(std::move(run));
  postDrainLocked();
}

void EnginePool::postDrainLocked() {
  if (!drain_posted_) {
    drain_posted_ = true;
    QMetaObject::invokeMethod(this, &EnginePool::drain, Qt::QueuedConnection);
  }
}

void EnginePool::drain() {
  // A publish may destroy the pool: after each one, touch no member until this is checked.
  const QPointer<EnginePool> self(this);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    drain_posted_ = false;
  }
  std::size_t published = 0;
  while (published < kMaxPublishesPerDrain) {
    std::unique_ptr<Run> run;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (finished_.empty()) {
        break;
      }
      run = std::move(finished_.front());
      finished_.pop_front();
      // Results left over stay in finished_ with a drain queued before this publish
      // runs, so a nested event loop inside it (a dialog, waitIdle) publishes them.
      if (!finished_.empty()) {
        postDrainLocked();
      }
    }
    --outstanding_;
    if (run->task.publish) {
      Outcome outcome = std::move(run->outcome);
      if (run->token.cancelled()) {
        outcome = {.status = Outcome::Status::kCancelled};
      }
      run->task.publish(outcome);
    }
    if (self.isNull()) {
      return;
    }
    ++published;
  }
  if (published > 0) {
    emit drained();
  }
}

std::size_t EnginePool::pendingPublishes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_.size();
}

void EnginePool::waitIdle() {
  if (outstanding_ == 0) {
    return;
  }
  // Not a processEvents(WaitForMoreEvents) loop: on Windows one pass can run the last
  // drain from its posted-event delivery and then block with nothing left to wake it.
  // quit() interrupts the dispatcher, so the pass that ran that drain returns instead.
  QEventLoop loop;
  connect(this, &EnginePool::drained, &loop, [this, &loop] {
    if (outstanding_ == 0) {
      loop.quit();
    }
  });
  // Re-checked after every exit: work may have been submitted meanwhile, and a nested
  // waitIdle's final drain also quits this loop. exec() clears the exit request; -1 means
  // the application is exiting.
  while (outstanding_ > 0 && loop.exec() == 0) {}
}

}  // namespace PJ
