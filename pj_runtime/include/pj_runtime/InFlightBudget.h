// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include "pj_runtime/EnginePool.h"
#include "pj_scripting/object_script.h"
#include "pj_scripting/sandbox.h"

namespace PJ {

/// The memory the script runs of the pool's consumers may hold at once (docs/derived_objects/ARCHITECTURE.md
/// §11). A run holds a `Reservation` of the most it can hold, its VM cap plus its native budget, from its
/// submission to its publication. Its inputs are views on the store's bytes, so they are not charged. A run that
/// does not fit waits for room; one larger than the whole budget runs alone, once nothing else is reserved, rather
/// than never. GUI thread only. A reservation can outlive its consumer (a run still on the pool), so the budget
/// must outlive the pool.
class InFlightBudget {
 public:
  /// What one run on the default limits may hold.
  static constexpr std::size_t kRunBytes = scripting::BudgetLimits{}.mem_bytes + scripting::kDefaultNativeBudgetBytes;

  /// Room taken from the budget, returned when destroyed.
  class Reservation {
   public:
    Reservation(Reservation&& other) noexcept : budget_(std::exchange(other.budget_, nullptr)), bytes_(other.bytes_) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation& operator=(Reservation&&) = delete;
    ~Reservation() {
      if (budget_ != nullptr) {
        budget_->reserved_ -= bytes_;
      }
    }

   private:
    friend class InFlightBudget;
    Reservation(InFlightBudget& budget, std::size_t bytes) : budget_(&budget), bytes_(bytes) {
      budget.reserved_ += bytes;
    }
    InFlightBudget* budget_;
    std::size_t bytes_;
  };

  /// Room for one default run per thread of the pool's shared lane.
  explicit InFlightBudget(const EnginePool& pool)
      : InFlightBudget(kRunBytes * static_cast<std::size_t>(std::max(1, pool.sharedLaneThreads()))) {}
  explicit InFlightBudget(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

  /// `bytes` fit beside what is reserved, or nothing is.
  [[nodiscard]] bool fits(std::size_t bytes) const {
    return reserved_ == 0 || (reserved_ <= capacity_ && bytes <= capacity_ - reserved_);
  }
  /// A reservation of `bytes` when they fit.
  [[nodiscard]] std::optional<Reservation> tryReserve(std::size_t bytes) {
    if (!fits(bytes)) {
      return std::nullopt;
    }
    return Reservation(*this, bytes);
  }

  [[nodiscard]] std::size_t reserved() const {
    return reserved_;
  }
  [[nodiscard]] std::size_t capacity() const {
    return capacity_;
  }

 private:
  std::size_t capacity_;
  std::size_t reserved_ = 0;
};

}  // namespace PJ
