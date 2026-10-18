// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/InFlightBudget.h"

#include <QMetaObject>
#include <algorithm>

namespace PJ {

bool InFlightBudget::Reservation::grow(std::size_t bytes) {
  InFlightBudget& budget = *budget_;
  const bool alone = budget.reserved_ == bytes_;
  if (!alone && !budget.fits(bytes)) {
    return false;
  }
  budget.reserved_ += bytes;
  bytes_ += bytes;
  return true;
}

std::optional<InFlightBudget::Reservation> InFlightBudget::tryReserve(std::size_t bytes) {
  if (!waiters_.empty() || !fits(bytes)) {
    return std::nullopt;
  }
  return Reservation(*this, bytes);
}

InFlightBudget::Ticket InFlightBudget::whenRoom(
    std::size_t bytes, std::function<void(Reservation)> granted, Place place) {
  auto queued = std::make_shared<bool>(true);
  Waiter waiter{queued, bytes, std::move(granted)};
  if (place == Place::kFront) {
    waiters_.push_front(std::move(waiter));
  } else {
    waiters_.push_back(std::move(waiter));
  }
  postDrain();  // room may be free already: the grant still comes from a drain
  return Ticket(*this, std::move(queued));
}

void InFlightBudget::drain() {
  if (draining_) {
    return;  // the running pass sees what changed
  }
  draining_ = true;
  while (!waiters_.empty() && fits(waiters_.front().bytes)) {
    Waiter waiter = std::move(waiters_.front());
    waiters_.pop_front();
    *waiter.queued = false;  // its ticket's cleanup returns at once
    waiter.granted(Reservation(*this, waiter.bytes));
  }
  draining_ = false;
}

void InFlightBudget::release(std::size_t bytes) {
  reserved_ -= bytes;
  postDrain();
}

void InFlightBudget::leave(const bool* queued) {
  const auto found = std::find_if(
      waiters_.begin(), waiters_.end(), [queued](const Waiter& waiter) { return waiter.queued.get() == queued; });
  if (found != waiters_.end()) {
    waiters_.erase(found);
    postDrain();  // the waiter behind may fit now
  }
}

void InFlightBudget::postDrain() {
  if (draining_ || drain_posted_ || waiters_.empty() || !fits(waiters_.front().bytes)) {
    return;
  }
  drain_posted_ = true;
  QMetaObject::invokeMethod(
      &context_,
      [this] {
        drain_posted_ = false;
        drain();
      },
      Qt::QueuedConnection);
}

}  // namespace PJ
