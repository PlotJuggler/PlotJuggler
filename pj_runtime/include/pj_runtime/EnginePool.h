#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace PJ {

/// The worker pool the derived-data engines run their scripts on
/// (docs/derived_objects/ARCHITECTURE.md §11). It runs READY tasks: whatever is
/// submitted starts as soon as a thread of its lane is free, and its result is
/// published back on the GUI thread.
///
/// Keeps no per-node state: the consumer owns each node's one running run and one
/// pending request and submits only when nothing runs
/// (docs/derived_objects/ARCHITECTURE.md §11).
///
/// Known limits:
///  - The half-core bound covers this pool only, not other users of
///    `QThreadPool::globalInstance()`.
///  - Workers request `QThread::LowPriority`, which Qt ignores on Linux (SCHED_OTHER
///    has a single priority level), so there the bound alone keeps the GUI thread
///    responsive.
///  - Destruction waits for every running `compute`, so one that never polls its
///    token delays it.
///
/// THREADING: construct, `submit`, `waitIdle` and destroy on one thread (the GUI
/// thread). `compute` runs on a worker and touches only what it captured plus its
/// token. `publish` runs on the constructing thread, in drain passes of at most
/// `kMaxPublishesPerDrain` publishes each, so a burst of results never freezes the
/// event loop. `CancelToken` is safe on any thread.
class EnginePool : public QObject {
  Q_OBJECT
 public:
  /// Publishes per GUI-thread drain pass; the rest wait for the next event-loop turn.
  static constexpr std::size_t kMaxPublishesPerDrain = 32;

  /// Cooperative cancellation of one run, shared by the consumer (which cancels)
  /// and the run's `compute` (which polls). Copies share the state. A
  /// default-constructed token is never cancelled (a consumer running inline).
  class CancelToken {
   public:
    /// True once the run was cancelled or its pool began destruction. Poll it in
    /// the compute's inner loop (per record, not per top-level call).
    [[nodiscard]] bool cancelled() const noexcept;
    /// Cancels the run: `compute` sees it at its next poll, and `publish` gets
    /// `Outcome::Status::kCancelled` even when `compute` had already finished. Idempotent.
    void cancel() const noexcept;
    /// The flags `cancelled` reads (any set: cancelled), for a poller on another thread that must not call into the
    /// run (the Python watchdog). Empty for a default-constructed token.
    [[nodiscard]] std::vector<std::shared_ptr<const std::atomic<bool>>> flags() const;

   private:
    friend class EnginePool;
    std::shared_ptr<std::atomic<bool>> run_cancelled_;
    std::shared_ptr<const std::atomic<bool>> pool_stopping_;
  };

  /// How a run ended, as `publish` receives it.
  struct Outcome {
    enum class Status {
      kCompleted,  ///< `compute` returned (or the run had no `compute`)
      kCancelled,  ///< the consumer cancelled the run (even if `compute` then threw): discard its result
      kFailed,     ///< `compute` threw: `error` says why, the result is unusable
    };
    Status status = Status::kCompleted;
    /// `what()` of the exception that escaped `compute` ("unknown exception" for a
    /// non-`std::exception`); empty unless `status` is `kFailed`.
    std::string error;
  };

  /// One run. Both closures are destroyed on the GUI thread.
  struct Task {
    /// Worker thread. Skipped when the run is cancelled before it starts. An
    /// exception escaping it fails the run (`Outcome::Status::kFailed`).
    std::function<void(const CancelToken&)> compute;
    /// GUI thread, exactly once per submitted task, also after a cancel or a throw,
    /// unless the pool is destroyed first. May run long after `submit`, so it must
    /// not capture a raw pointer to an object that may be gone by then (use a
    /// `QPointer` or a weak guard). May call `submit`, `waitIdle`, or destroy the pool.
    std::function<void(const Outcome&)> publish;
  };

  enum class Lane {
    kShared,  ///< the bounded pool: max(1, idealThreadCount()/2) threads
    kPython,  ///< one dedicated thread: the GIL serializes Python anyway
  };

  explicit EnginePool(QObject* parent = nullptr);
  /// Cancels every token, waits for the running computes to return, and destroys
  /// every pending `publish` without calling it. Safe from inside a `publish`.
  ~EnginePool() override;

  EnginePool(const EnginePool&) = delete;
  EnginePool& operator=(const EnginePool&) = delete;

  /// Starts `task` on `lane` and returns its token, which the consumer keeps to
  /// cancel the run on a meaning change. No queueing per node, no coalescing.
  CancelToken submit(Task task, Lane lane = Lane::kShared);

  /// Test seam: runs whose compute is done and whose `publish` waits for a drain pass.
  [[nodiscard]] std::size_t pendingPublishes() const;

  /// The shared lane's threads: max(1, idealThreadCount()/2).
  [[nodiscard]] int sharedLaneThreads() const {
    return shared_lane_.maxThreadCount();
  }

  /// Runs a nested event loop until every submitted task was published, those submitted
  /// meanwhile included. May be called from inside a `publish` (the run being
  /// published no longer counts). Needs a QCoreApplication. Never call it holding a
  /// lock that a `publish` takes, and do not destroy the pool while it waits. Returns
  /// early, with work outstanding, once the application is exiting.
  void waitIdle();

 signals:
  /// Emitted after each drain pass that published at least one result.
  void drained();

 private:
  struct Run;
  class Job;

  /// Worker side of a run: hands the finished run to the GUI-thread drain.
  void finish(std::unique_ptr<Run> run);
  /// Queues a drain unless one is already queued. Requires `mutex_`.
  void postDrainLocked();
  /// One GUI-thread pass: publishes up to kMaxPublishesPerDrain finished runs, one at
  /// a time from `finished_`, so a publish that pumps events sees the rest as pending.
  void drain();

  mutable std::mutex mutex_;
  std::deque<std::unique_ptr<Run>> finished_;  ///< guarded by mutex_
  bool drain_posted_ = false;                  ///< a drain event is queued; guarded by mutex_
  std::size_t outstanding_ = 0;                ///< GUI thread only: submitted, not yet published
  std::shared_ptr<std::atomic<bool>> stopping_ = std::make_shared<std::atomic<bool>>(false);
  QThreadPool shared_lane_;
  QThreadPool python_lane_;
};

}  // namespace PJ
