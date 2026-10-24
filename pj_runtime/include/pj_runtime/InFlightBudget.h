// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pj_runtime/EnginePool.h"
#include "pj_scripting/object_script.h"
#include "pj_scripting/sandbox.h"

namespace PJ {

/// The memory the script runs of the pool's consumers (`NodeScheduler`, `BatchEngine`) may hold at once
/// (docs/derived_objects/ARCHITECTURE.md §11). A run holds a `Reservation` of the most it can hold from its
/// submission to its publication: an on-demand run its VM cap plus its native budget (`kRunBytes`; its inputs are
/// views on the store's bytes, so they are not charged), a marker run its VM cap plus its output ceiling
/// (`BatchEngine::kMarkerRunBytes`) and its staged series copy. Runs that do not fit wait in ONE line for every
/// consumer, first in, first out: the line is served from its head while the head fits, so a small newcomer never
/// passes an earlier large waiter and mixed sizes cannot starve. One larger than the whole budget runs alone, once
/// nothing else is reserved, rather than never.
///
/// GUI thread only. A release never serves the line inside the call that released: it posts a drain to the event
/// loop, which a consumer runs earlier at a point of its own (`drain`). The budget must outlive every reservation,
/// hence the pool (a run still on the pool holds its reservation). A ticket may outlive it: the budget empties its
/// line when destroyed, so a waiter is never granted and its ticket's cleanup touches nothing.
class InFlightBudget {
 public:
  /// What one on-demand run on the default limits may hold.
  static constexpr std::size_t kRunBytes = scripting::BudgetLimits{}.mem_bytes + scripting::kDefaultNativeBudgetBytes;

  /// Room taken from the budget, returned when destroyed.
  class Reservation {
   public:
    Reservation(Reservation&& other) noexcept : budget_(std::exchange(other.budget_, nullptr)), bytes_(other.bytes_) {}
    /// Returns this room, then takes `other`'s.
    Reservation& operator=(Reservation&& other) noexcept {
      if (this != &other) {
        returnRoom();
        budget_ = std::exchange(other.budget_, nullptr);
        bytes_ = other.bytes_;
      }
      return *this;
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() {
      returnRoom();
    }

    /// Adds `bytes` when they fit beside everything reserved, or when this is the only reservation (the run-alone
    /// rule). It passes the waiters: the run it holds for was admitted before them.
    [[nodiscard]] bool grow(std::size_t bytes);
    [[nodiscard]] std::size_t bytes() const {
      return bytes_;
    }

   private:
    friend class InFlightBudget;
    Reservation(InFlightBudget& budget, std::size_t bytes) : budget_(&budget), bytes_(bytes) {
      budget.reserved_ += bytes;
    }
    void returnRoom() {
      if (budget_ != nullptr) {
        std::exchange(budget_, nullptr)->release(bytes_);
      }
    }
    InFlightBudget* budget_;
    std::size_t bytes_;
  };

  /// A place in the line (`whenRoom`). Destroying it, or assigning over it, leaves the line; after the grant it
  /// does nothing, without searching the line.
  class Ticket {
   public:
    Ticket() = default;
    Ticket(Ticket&& other) noexcept = default;
    Ticket& operator=(Ticket&& other) noexcept {
      if (this != &other) {
        leave();
        budget_ = other.budget_;
        queued_ = std::move(other.queued_);
      }
      return *this;
    }
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    ~Ticket() {
      leave();
    }

   private:
    friend class InFlightBudget;
    Ticket(InFlightBudget& budget, std::shared_ptr<bool> queued) : budget_(&budget), queued_(std::move(queued)) {}
    void leave() {
      if (queued_ != nullptr && *queued_) {
        budget_->leave(queued_.get());
      }
      queued_.reset();
    }
    InFlightBudget* budget_ = nullptr;
    std::shared_ptr<bool> queued_;  ///< shared with its waiter: true while it is in the line
  };

  /// Where `whenRoom` puts a waiter.
  enum class Place {
    kBack,   ///< behind every waiter
    kFront,  ///< ahead of them: a run admitted before them that turned out to need more room than it holds
  };

  /// Room for one default on-demand run per thread of the pool's shared lane.
  explicit InFlightBudget(const EnginePool& pool)
      : InFlightBudget(kRunBytes * static_cast<std::size_t>(std::max(1, pool.sharedLaneThreads()))) {}
  explicit InFlightBudget(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

  InFlightBudget(const InFlightBudget&) = delete;
  InFlightBudget& operator=(const InFlightBudget&) = delete;
  ~InFlightBudget() {
    for (Waiter& waiter : waiters_) {
      *waiter.queued = false;
    }
  }

  /// A reservation of `bytes` now: nobody waits, and they fit beside what is reserved (or nothing is).
  [[nodiscard]] std::optional<Reservation> tryReserve(std::size_t bytes);
  /// Waits in line for a reservation of `bytes`: `granted` is called with it from a later drain, once every waiter
  /// ahead was served and the bytes fit, never inside this call, and never once the ticket is gone (so it may
  /// capture the ticket's owner). `tryReserve` first starts at once when it can.
  [[nodiscard]] Ticket whenRoom(
      std::size_t bytes, std::function<void(Reservation)> granted, Place place = Place::kBack);
  /// Serves the line now, from its head while the head fits. A `granted` that releases, reserves or waits is seen by
  /// the same pass. For a consumer that just returned room where its waiters may run (its publication).
  void drain();

  [[nodiscard]] std::size_t reserved() const {
    return reserved_;
  }
  [[nodiscard]] std::size_t capacity() const {
    return capacity_;
  }

 private:
  struct Waiter {
    std::shared_ptr<bool> queued;  ///< its ticket's flag, and its identity in the line
    std::size_t bytes = 0;
    std::function<void(Reservation)> granted;
  };

  /// `bytes` fit beside what is reserved, or nothing is.
  [[nodiscard]] bool fits(std::size_t bytes) const {
    return reserved_ == 0 || (reserved_ <= capacity_ && bytes <= capacity_ - reserved_);
  }

  void release(std::size_t bytes);
  void leave(const bool* queued);
  /// Posts one drain when the head of the line fits and no drain is running or posted.
  void postDrain();

  std::size_t capacity_;
  std::size_t reserved_ = 0;
  std::deque<Waiter> waiters_;
  bool draining_ = false;
  bool drain_posted_ = false;
  /// The receiver of the posted drains, declared last: destroyed first, it drops a drain still posted.
  QObject context_;
};

}  // namespace PJ
