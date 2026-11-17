// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/async_frame_worker.h"

#include <cassert>
#include <exception>
#include <utility>

namespace PJ {

AsyncFrameWorker::~AsyncFrameWorker() {
  stop();
}

void AsyncFrameWorker::start(DecodeFn decode, OnDecodeError on_error) {
  start(std::move(decode), std::move(on_error), Options{});
}

void AsyncFrameWorker::start(DecodeFn decode, OnDecodeError on_error, Options options) {
  assert(!worker_.joinable() && "AsyncFrameWorker::start must be called exactly once");
  assert(decode && on_error);
  decode_ = std::move(decode);
  on_error_ = std::move(on_error);
  options_ = options;
  running_.store(true);
  worker_ = std::thread([gate = start_gate_, this] {
    {
      std::lock_guard lock(gate->mutex);
      if (gate->abandoned) {
        return;
      }
      gate->started = true;
    }
    workerLoop();
  });
}

void AsyncFrameWorker::stop() {
  // Under request_mutex_: see the lost-wakeup note in the class comment.
  {
    std::lock_guard lock(request_mutex_);
    running_.store(false);
    if (cancel_token_) {
      cancel_token_->cancel();
    }
  }
  request_cv_.notify_one();
  if (!worker_.joinable()) {
    return;
  }
  bool started = false;
  {
    std::lock_guard lock(start_gate_->mutex);
    started = start_gate_->started;
    start_gate_->abandoned = !started;
  }
  if (started) {
    worker_.join();
  } else {
    worker_.detach();
  }
}

void AsyncFrameWorker::requestDecode(Timestamp ts_ns) {
  if (ts_ns == last_requested_ts_) {
    return;
  }
  last_requested_ts_ = ts_ns;

  {
    std::lock_guard lock(request_mutex_);
    requested_ts_ = ts_ns;
    has_request_ = true;
    if (cancel_token_ && (!options_.preempt_predicate || options_.preempt_predicate(in_flight_ts_, ts_ns))) {
      cancel_token_->cancel();
    }
  }
  request_cv_.notify_one();
}

void AsyncFrameWorker::invalidate() {
  last_requested_ts_ = INT64_MIN;
  // The source clears its own same-entry dedup too, or it resolves the same entry and delivers nothing.
  std::lock_guard lock(request_mutex_);
  force_redecode_ = true;
}

void AsyncFrameWorker::deposit(DecodedFrame frame) {
  {
    std::lock_guard lock(result_mutex_);
    result_frame_ = std::move(frame);
  }
  // Notify outside result_mutex_ so the callback's take() doesn't contend.
  std::function<void()> cb_copy;
  {
    std::lock_guard lock(callback_mutex_);
    cb_copy = on_frame_ready_;
  }
  if (cb_copy) {
    cb_copy();
  }
}

std::optional<DecodedFrame> AsyncFrameWorker::take() {
  std::lock_guard lock(result_mutex_);
  if (!result_frame_.has_value()) {
    return std::nullopt;
  }
  std::optional<DecodedFrame> out = std::move(result_frame_);
  result_frame_.reset();
  return out;
}

void AsyncFrameWorker::setFrameReadyCallback(std::function<void()> cb) {
  std::lock_guard lock(callback_mutex_);
  on_frame_ready_ = std::move(cb);
}

void AsyncFrameWorker::workerLoop() {
  while (running_.load()) {
    Request request;
    {
      std::unique_lock lock(request_mutex_);
      request_cv_.wait(lock, [this] { return has_request_ || !running_.load(); });
      if (!running_.load()) {
        break;
      }
      request.target_ns = requested_ts_;
      request.force_redecode = force_redecode_;
      has_request_ = false;
      force_redecode_ = false;
      if (options_.use_cancel_token) {
        cancel_token_ = makeCancelToken();
        in_flight_ts_ = request.target_ns;
        request.cancel = cancel_token_;
      }
    }

    // An exception leaving the thread callable would std::terminate (a throwing parser plugin, a bad_alloc).
    try {
      decode_(request, *this);
    } catch (const std::exception& ex) {
      on_error_(ex.what());
    } catch (...) {
      on_error_("");
    }

    {
      std::lock_guard lock(request_mutex_);
      if (has_request_) {
        continue;
      }
    }
  }
}

}  // namespace PJ
