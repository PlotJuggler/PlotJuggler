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

/// The shared latest-wins decode-worker engine behind the asynchronous
/// MediaSource implementations (ImagePipelineSource, StreamingVideoSource).
///
/// One dedicated thread serves decode requests with latest-target-wins
/// coalescing: requests posted while a decode is in flight overwrite the
/// pending target, keeping the queue at depth one regardless of slider drag
/// rate. Results flow through a single-slot mailbox (`deposit()` / `take()`),
/// and an optional frame-ready callback fires after every deposit so pull-only
/// consumers know to re-poll.
///
/// Threading contract (mirrors the MediaSource contract it serves):
///  - `start()`, `requestDecode()`, `invalidate()`, `take()`, and
///    `setFrameReadyCallback()` are main-thread-only.
///  - The DecodeFn runs on the internal worker thread. It may call
///    `deposit()` any number of times per request (e.g. an instant scrub
///    preview followed by the settled full-res frame).
///  - The frame-ready callback is invoked FROM the worker thread, outside the
///    result lock; implementers must hop to their own thread.
///
/// Exception barrier: a DecodeFn that throws does not kill the worker (and
/// cannot escape the std::thread callable, which would std::terminate). The
/// exception is routed to the OnDecodeError hook and the loop keeps serving —
/// see ARCHITECTURE.md §10.5.
///
/// Teardown: the destructor (or an explicit `stop()`) flips the running flag
/// UNDER the request mutex before notifying — an unlocked store can land
/// between the worker's predicate check and its block, so the notify wakes
/// nobody and the join hangs (the historical scene2d_dock_widget_test
/// deadlock, previously fixed separately in each source). When cancellation
/// is enabled the in-flight token is cancelled too, so a slow (4K GOP) decode
/// is preempted rather than blocking teardown.
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
    /// Decides whether a new target should cancel the in-flight decode (only
    /// consulted with use_cancel_token; teardown always cancels). Null =
    /// always preempt. A source whose cancelled decode is expensive to resume
    /// (StreamingVideoSource: any cancel forces a full GOP re-seek) must
    /// return false for contiguous-playback steps — when ticks arrive faster
    /// than one decode completes, unconditional preemption cancels EVERY
    /// decode before it can deliver and playback collapses to zero frames.
    /// Return true only for genuine scrubs (backward, or a forward jump too
    /// large to ride the codec forward). Called from requestDecode() with the
    /// request mutex held — keep it trivial and non-blocking.
    std::function<bool(Timestamp in_flight_target, Timestamp new_target)> preempt_predicate;
  };

  /// Inert until start(). Members are placed so a default-constructed,
  /// never-started worker destructs safely.
  AsyncFrameWorker() = default;
  ~AsyncFrameWorker();

  AsyncFrameWorker(const AsyncFrameWorker&) = delete;
  AsyncFrameWorker& operator=(const AsyncFrameWorker&) = delete;
  AsyncFrameWorker(AsyncFrameWorker&&) = delete;
  AsyncFrameWorker& operator=(AsyncFrameWorker&&) = delete;

  /// Launch the worker thread. Call exactly once, at the END of the owning
  /// source's constructor: `decode` typically captures the owner, so every
  /// member it touches must already be initialized. `on_error` must be valid.
  /// (Two overloads instead of an `Options = {}` default argument: GCC rejects
  /// brace-init defaults of a nested NSDMI struct inside the enclosing class.)
  void start(DecodeFn decode, OnDecodeError on_error);
  void start(DecodeFn decode, OnDecodeError on_error, Options options);

  /// Stop and join the worker (idempotent; also run by the destructor). After
  /// stop() no DecodeFn is running and none will start. Declare the worker
  /// LAST in the owning source so its implicit stop joins the thread before
  /// any member the DecodeFn touches is destroyed. A thread that has not begun
  /// running is detached instead of joined: under WebAssembly a thread created
  /// with the pthread pool exhausted only starts once the main thread yields,
  /// so joining it from the main thread would wait forever.
  void stop();

  /// Post a decode target (latest-wins). Equal consecutive targets are
  /// deduplicated; cancellation (when enabled and the preempt predicate
  /// agrees) preempts the in-flight decode.
  void requestDecode(Timestamp ts_ns);

  /// Make the next dispatch carry force_redecode = true and drop the
  /// equal-target dedup, so a decode re-runs even at an unchanged timestamp
  /// (a composite rebuild consumed the last frame and needs a fresh one).
  void invalidate();

  /// Publish a frame to the single-slot mailbox (latest deposit wins) and fire
  /// the frame-ready callback outside the lock. Normally called by the
  /// DecodeFn on the worker thread.
  void deposit(DecodedFrame frame);

  /// Drain the mailbox: returns the latest deposited frame once, then empty
  /// until the next deposit. Main thread.
  [[nodiscard]] std::optional<DecodedFrame> take();

  /// Install/replace/clear (nullptr) the after-deposit notification. Invoked
  /// from the worker thread — see the class comment. Main thread.
  void setFrameReadyCallback(std::function<void()> cb);

 private:
  void workerLoop();

  DecodeFn decode_;
  OnDecodeError on_error_;
  Options options_;

  // Request channel (main → worker).
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

  // Result channel (worker → main): single-slot latest-wins mailbox.
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
