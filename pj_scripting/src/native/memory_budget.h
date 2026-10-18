// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/expected.hpp"

namespace PJ::scripting::native {

/// One evaluation's byte limit for native allocations (crops, transforms, scene builders), which live
/// outside the VM's own memory cap. Atomic because an output retained past its evaluation may be
/// destroyed, and so release its bytes, on another thread.
class MemoryBudget {
 public:
  explicit MemoryBudget(std::size_t limit) : limit_(limit) {}

  /// Adds `bytes` to the ledger unless that would pass the limit; on false nothing changes.
  [[nodiscard]] bool tryAcquire(std::size_t bytes) noexcept {
    std::size_t used = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > limit_ - used) {
        return false;
      }
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
  }
  void release(std::size_t bytes) noexcept {
    used_.fetch_sub(bytes, std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t used() const noexcept {
    return used_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t limit() const noexcept {
    return limit_;
  }

 private:
  const std::size_t limit_;
  std::atomic<std::size_t> used_{0};
};

/// Bytes held against a MemoryBudget until the reservation is destroyed. Store it beside the allocation
/// it pays for (inside a buffer anchor, or next to a vector-based value) so a retained output keeps
/// counting. A null budget reserves nothing and never fails.
class MemoryReservation {
 public:
  explicit MemoryReservation(std::shared_ptr<MemoryBudget> budget) : budget_(std::move(budget)) {}
  MemoryReservation(MemoryReservation&& other) noexcept
      : budget_(std::move(other.budget_)), bytes_(std::exchange(other.bytes_, 0)) {}
  ~MemoryReservation() {
    if (budget_) {
      budget_->release(bytes_);
    }
  }

  /// Reserves `bytes` more, BEFORE the caller allocates them. On failure nothing changes and the error
  /// names the native limit, a ledger separate from the VM's own memory cap.
  [[nodiscard]] Status grow(std::size_t bytes) {
    if (budget_ && !budget_->tryAcquire(bytes)) {
      const std::size_t limit = budget_->limit();
      const std::string text =
          limit % (1u << 20) == 0 ? std::to_string(limit >> 20) + " MB" : std::to_string(limit) + " bytes";
      return unexpected("script native memory limit (" + text + ") exceeded");
    }
    bytes_ += bytes;
    return okStatus();
  }

 private:
  std::shared_ptr<MemoryBudget> budget_;
  std::size_t bytes_ = 0;
};

}  // namespace PJ::scripting::native
