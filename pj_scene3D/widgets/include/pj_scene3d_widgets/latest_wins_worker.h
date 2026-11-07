// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QThread>
#include <QtConcurrentRun>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace pj::scene3d {

// Runs one job at a time on the Qt thread pool behind a depth-1 "latest wins"
// queue: while a job runs, only the newest request waits behind it. GUI-thread
// only; results come back through the owner's event loop.
//
// invalidate() marks everything requested so far as stale: the waiting request
// is dropped and the running job's result is discarded on arrival (a layer
// calls it when its data or settings change under the job). `job` runs off the
// GUI thread and must touch nothing but its Request; it should catch its own
// exceptions (an escaping one delivers a default-constructed Result).
//
// The start/completion counters are always compiled: the browser acceptance
// probes read them from a library built without PJ_SCENE3D_TEST_HOOKS.
template <typename Request, typename Result>
class LatestWinsWorker {
 public:
  using Job = std::function<Result(const Request&)>;
  using Deliver = std::function<void(Result)>;

  // `context` parents the watcher and scopes the `deliver` connection, which
  // receives only results that were not invalidated.
  LatestWinsWorker(QObject* context, Job job, Deliver deliver)
      : watcher_(new QFutureWatcher<Tagged>(context)), job_(std::move(job)), deliver_(std::move(deliver)) {
    QObject::connect(watcher_, &QFutureWatcher<Tagged>::finished, context, [this]() { onFinished(); });
  }
  ~LatestWinsWorker() {
    watcher_->disconnect();  // the watcher (a child of the owner) outlives this member
  }
  LatestWinsWorker(const LatestWinsWorker&) = delete;
  LatestWinsWorker& operator=(const LatestWinsWorker&) = delete;

  // Starts `request` now when idle, else makes it the one waiting request.
  void request(Request request) {
    if (in_flight_) {
      waiting_ = std::move(request);
      return;
    }
    waiting_.reset();
    start(std::move(request));
  }
  void invalidate() {
    ++generation_;
    waiting_.reset();
  }
  void cancelWaiting() {
    waiting_.reset();
  }
  // Replaces the job for requests started from now on (tests use it to hold a job).
  void setJob(Job job) {
    job_ = std::move(job);
  }
  // Checked when a waiting request is about to start; false drops it (the
  // owner no longer wants that sample).
  void setWaitingFilter(std::function<bool(const Request&)> filter) {
    waiting_filter_ = std::move(filter);
  }

  // True from start until the completion was processed (not merely until the
  // future finished: its finished() event may still be queued).
  [[nodiscard]] bool busy() const {
    return in_flight_;
  }
  // The running request while its result is still wanted, else nullptr.
  [[nodiscard]] const Request* running() const {
    return in_flight_ && running_generation_ == generation_ ? &*running_ : nullptr;
  }
  [[nodiscard]] std::uint64_t starts() const {
    return starts_;
  }
  [[nodiscard]] std::uint64_t completions() const {
    return completions_;
  }
  // Completions whose job ran on a thread other than the one that started it.
  [[nodiscard]] std::uint64_t offMainCompletions() const {
    return off_main_completions_;
  }

 private:
  struct Tagged {
    Result result;
    bool off_main = false;
  };

  void start(Request request) {
    in_flight_ = true;
    running_generation_ = generation_;
    ++starts_;
    running_ = request;
    QThread* const starting_thread = QThread::currentThread();
    watcher_->setFuture(QtConcurrent::run([job = job_, request = std::move(request), starting_thread]() {
      Tagged tagged;
      tagged.off_main = QThread::currentThread() != starting_thread;
      try {
        tagged.result = job(request);
      } catch (...) {
        tagged.result = Result{};  // never rethrown on the GUI thread
      }
      return tagged;
    }));
  }

  void onFinished() {
    Tagged tagged = watcher_->future().takeResult();
    in_flight_ = false;
    running_.reset();
    ++completions_;
    off_main_completions_ += tagged.off_main ? 1U : 0U;
    if (running_generation_ == generation_) {
      deliver_(std::move(tagged.result));  // may itself request() and restart the worker
    }
    if (!in_flight_ && waiting_.has_value()) {
      Request next = std::move(*waiting_);
      waiting_.reset();
      if (!waiting_filter_ || waiting_filter_(next)) {
        start(std::move(next));
      }
    }
  }

  QFutureWatcher<Tagged>* watcher_;
  Job job_;
  Deliver deliver_;
  std::function<bool(const Request&)> waiting_filter_;
  std::optional<Request> running_;
  std::optional<Request> waiting_;
  bool in_flight_ = false;
  std::uint64_t generation_ = 0;
  std::uint64_t running_generation_ = 0;
  std::uint64_t starts_ = 0;
  std::uint64_t completions_ = 0;
  std::uint64_t off_main_completions_ = 0;
};

}  // namespace pj::scene3d
