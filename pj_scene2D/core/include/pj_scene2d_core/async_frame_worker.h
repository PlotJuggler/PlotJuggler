#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "pj_base/types.hpp"
#include "pj_scene2d_core/cancel_token.h"
#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// The latest-wins decode worker behind the asynchronous MediaSources. One thread serves requests; a request posted
/// while a decode runs overwrites the pending target (queue depth one). Results go through a single-slot mailbox
/// (`deposit()` / `take()`), and an optional frame-ready callback fires after every deposit so the consumer re-polls.
///
/// Threading contract:
///  - `start()`, `requestDecode()`, `invalidate()`, `take()`, and
///    `setFrameReadyCallback()` are main-thread-only.
///  - The DecodeFn runs on the internal worker thread. It may call
///    `deposit()` any number of times per request (e.g. an instant scrub
///    preview followed by the settled full-res frame).
///  - The frame-ready callback is invoked FROM the worker thread, outside the
///    result lock; implementers must hop to their own thread.
///
/// A DecodeFn that throws goes to OnDecodeError and the loop keeps serving (an exception leaving the std::thread
/// callable would std::terminate).
///
/// Teardown flips the running flag UNDER the request mutex before notifying: an unlocked store can land between the
/// worker's predicate check and its block, the notify wakes nobody and the join hangs. With cancellation enabled the
/// in-flight token is cancelled too, so a slow GOP decode does not block teardown.
class AsyncFrameWorker {
 public:
  /// One coalesced unit of work handed to the DecodeFn.
  struct Request {
    /// Target timestamp (nanoseconds), the latest value posted via requestDecode().
    Timestamp target_ns = 0;
    /// True when invalidate() was called since the previous dispatch: the source
    /// must clear any same-entry dedup and produce a frame even at an unchanged
    /// timestamp.
    bool force_redecode = false;
    /// Cancellation token for this decode — non-null only when the worker was
    /// started with Options::use_cancel_token. A newer requestDecode() (or
    /// teardown) cancels it; long decodes should poll it between units.
    CancelTokenPtr cancel;
  };

  /// Source-specific decode logic, run on the worker thread. Deposit results
  /// via `worker.deposit()`; returning without depositing is fine (no frame).
  using DecodeFn = std::function<void(const Request&, AsyncFrameWorker& worker)>;

  /// Receives what() of any exception escaping the DecodeFn (worker thread).
  /// The empty string signals a non-std exception.
  using OnDecodeError = std::function<void(const char* what)>;

  struct Options {
    /// Allocate a fresh CancelToken per request and cancel it when a newer
    /// request (or teardown) arrives. Off for sources whose decodes are cheap
    /// enough that latest-wins coalescing alone bounds latency.
    bool use_cancel_token = false;
    /// Whether a new target cancels the in-flight decode (only with use_cancel_token; teardown always cancels).
    /// Null = always preempt. When a cancelled decode is expensive to resume, return false for contiguous playback
    /// steps: with ticks faster than one decode, preempting every time cancels every decode and no frame is shown.
    /// Called from requestDecode() with the request mutex held: keep it trivial and non-blocking.
    std::function<bool(Timestamp in_flight_target, Timestamp new_target)> preempt_predicate;
  };

  /// Inert until start(); a never-started worker destructs safely.
  AsyncFrameWorker() = default;
  ~AsyncFrameWorker();

  AsyncFrameWorker(const AsyncFrameWorker&) = delete;
  AsyncFrameWorker& operator=(const AsyncFrameWorker&) = delete;
  AsyncFrameWorker(AsyncFrameWorker&&) = delete;
  AsyncFrameWorker& operator=(AsyncFrameWorker&&) = delete;

  /// Call exactly once, at the END of the owning source's constructor: `decode` captures the owner, so every member
  /// it touches must already be initialized. `on_error` must be valid. (Two overloads because GCC rejects an
  /// `Options = {}` default for a nested struct with member initializers.)
  void start(DecodeFn decode, OnDecodeError on_error);
  void start(DecodeFn decode, OnDecodeError on_error, Options options);

  /// Idempotent; also run by the destructor. Afterwards no DecodeFn runs or will start. Declare the worker LAST in
  /// the owning source, so its stop joins before any member the DecodeFn touches is destroyed. A thread that has not
  /// begun running is detached, not joined: under WebAssembly a thread created with the pthread pool exhausted starts
  /// only once the main thread yields, so the join would wait forever. The detach runs under the start gate, which
  /// the thread must pass before it can exit.
  void stop();

  /// Latest-wins; equal consecutive targets are deduplicated.
  void requestDecode(Timestamp ts_ns);

  /// The next dispatch carries force_redecode and skips the equal-target dedup.
  void invalidate();

  /// Overwrites the mailbox and fires the frame-ready callback outside the lock.
  void deposit(DecodedFrame frame);

  /// The latest deposit once, then empty until the next one.
  [[nodiscard]] std::optional<DecodedFrame> take();

  /// nullptr clears it. The callback runs on the worker thread.
  void setFrameReadyCallback(std::function<void()> cb);

 private:
  void workerLoop();

  DecodeFn decode_;
  OnDecodeError on_error_;
  Options options_;

  std::mutex request_mutex_;
  std::condition_variable request_cv_;
  Timestamp requested_ts_ = INT64_MIN;
  Timestamp last_requested_ts_ = INT64_MIN;  // main-thread-only equal-target dedup
  bool has_request_ = false;
  bool force_redecode_ = false;
  // In-flight decode token and its dispatch target (only with
  // Options::use_cancel_token). Guarded by request_mutex_; the worker installs
  // a fresh token + target per dispatch. The target feeds preempt_predicate.
  CancelTokenPtr cancel_token_;
  Timestamp in_flight_ts_ = INT64_MIN;
  // Atomic for the worker's unlocked outer-loop check; stop() still flips it
  // UNDER request_mutex_ so the flip can't slip between the wait predicate and
  // the block (the lost-wakeup hazard described in the class comment).
  std::atomic<bool> running_{false};

  std::mutex result_mutex_;
  std::optional<DecodedFrame> result_frame_;

  // Frame-ready notification; the mutex makes replacement safe mid-run.
  std::mutex callback_mutex_;
  std::function<void()> on_frame_ready_;

  /// The handshake between stop() and a thread that may not have started yet.
  /// Shared with the thread, which reads it before touching `this`: a thread
  /// stop() abandoned returns at once, so the worker may already be destroyed.
  struct StartGate {
    std::mutex mutex;
    bool started = false;
    bool abandoned = false;
  };
  std::shared_ptr<StartGate> start_gate_ = std::make_shared<StartGate>();

  std::thread worker_;
};

}  // namespace PJ
