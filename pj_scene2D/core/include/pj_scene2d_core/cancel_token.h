#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace PJ {

/// Cooperative cancellation flag shared between a requester and a decoder.
///
/// The requester calls cancel(); the decoder polls isCancelled() between
/// decode units (NAL packets, JPEG scans, etc.) and returns early when set.
/// Shared via shared_ptr so both sides can hold it independently.
/// See ARCHITECTURE.md §3.3.
class CancelToken {
 public:
  CancelToken() = default;
  /// A token that is also cancelled once `poll` says so (a caller's own check, e.g. a script run's), asked on every
  /// `isCancelled` from the decoding thread.
  explicit CancelToken(std::function<bool()> poll) : poll_(std::move(poll)) {}

  [[nodiscard]] bool isCancelled() const {
    return flag_.load(std::memory_order_relaxed) || (poll_ && poll_());
  }

  void cancel() noexcept {
    flag_.store(true, std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> flag_{false};
  std::function<bool()> poll_;
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

[[nodiscard]] inline CancelTokenPtr makeCancelToken() {
  return std::make_shared<CancelToken>();
}

}  // namespace PJ
