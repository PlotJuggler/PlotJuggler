// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pj_base/expected.hpp"

namespace PJ::scripting::native {

/// How every error of a native allocation over its budget starts; a binder matches it to retry after a
/// garbage collection.
inline constexpr std::string_view kNativeLimitError = "script native memory limit";

/// A byte limit as an error message reads it: "N MB" for a whole number of MiB, else "N bytes".
inline std::string describeBytes(std::size_t bytes) {
  return bytes % (std::size_t{1} << 20) == 0 ? std::to_string(bytes >> 20) + " MB" : std::to_string(bytes) + " bytes";
}

/// One evaluation's byte limit for native allocations (crops, transforms, scene builders), which live
/// outside the VM's own memory cap. Atomic because an output retained past its evaluation may be
/// destroyed, and so release its bytes, on another thread.
class MemoryBudget {
 public:
  explicit MemoryBudget(std::size_t limit) : limit_(limit) {}

  /// Adds `bytes` to the ledger unless that would pass the limit (zero bytes always fit); on false nothing changes.
  [[nodiscard]] bool tryAcquire(std::size_t bytes) noexcept {
    std::size_t used = used_.load(std::memory_order_relaxed);
    do {
      if (bytes != 0 && (used > limit_ || bytes > limit_ - used)) {  // past the limit through acquireAnyway
        return false;
      }
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
  }
  /// Adds `bytes` to the ledger whatever the limit; returns the bytes used after it.
  std::size_t acquireAnyway(std::size_t bytes) noexcept {
    return used_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  }
  /// Acquire-release, as is `used()`'s acquire: a thread that reads zero and destroys the budget is ordered after the
  /// last release, whichever thread made it, so that thread's final access to the budget is not a data race with the
  /// destruction.
  void release(std::size_t bytes) noexcept {
    used_.fetch_sub(bytes, std::memory_order_acq_rel);
  }
  [[nodiscard]] std::size_t used() const noexcept {
    return used_.load(std::memory_order_acquire);
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
      return unexpected(std::string(kNativeLimitError) + " (" + describeBytes(budget_->limit()) + ") exceeded");
    }
    bytes_ += bytes;
    return okStatus();
  }

  /// Gives back `bytes` of what this reservation holds (at most all of it), e.g. a buffer's old storage once a
  /// reallocation freed it.
  void shrink(std::size_t bytes) noexcept {
    bytes = std::min(bytes, bytes_);
    bytes_ -= bytes;
    if (budget_) {
      budget_->release(bytes);
    }
  }

 private:
  std::shared_ptr<MemoryBudget> budget_;
  std::size_t bytes_ = 0;
};

/// A native value and the reservation that pays for it: keep them together, so a retained output keeps
/// counting against the evaluation's budget.
template <class Value>
struct Charged {
  Value value;
  MemoryReservation charge;
};

}  // namespace PJ::scripting::native
