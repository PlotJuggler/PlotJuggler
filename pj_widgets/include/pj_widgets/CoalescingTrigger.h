#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QTimer>
#include <functional>
#include <utility>

namespace PJ {

/// Leading + trailing rate limiter for a repeatedly-requested action.
///
/// `request()` runs the action immediately if the throttle window is open
/// (leading edge) and opens a window of `interval_ms`; further `request()`s
/// within the window are coalesced into a single action fired when the window
/// closes (trailing edge). So a burst of N requests collapses to one leading +
/// at most one trailing run, capping the action at ~`1000/interval_ms` Hz no
/// matter how fast requests arrive.
///
/// The action takes no argument — the caller stores the latest payload (e.g. the
/// current tracker time) and the trailing edge runs the action against whatever
/// the caller last stored, which is exactly the "use the newest value, drop the
/// intermediate ones" semantics wanted for a moving playhead.
///
/// This is the single "cap the rate once, at the origin" primitive: put one on
/// the fan-out that every driver (playback ticks, scrubbing, seeks) flows
/// through, rather than throttling each downstream consumer separately.
///
/// Not a QObject and intentionally non-copyable/non-movable (it owns a QTimer):
/// the timer connection captures `this`, and the timer member is destroyed with
/// the trigger, so the connection can never outlive it.
class CoalescingTrigger {
 public:
  CoalescingTrigger(int interval_ms, std::function<void()> action) : action_(std::move(action)) {
    timer_.setSingleShot(true);
    timer_.setInterval(interval_ms);
    QObject::connect(&timer_, &QTimer::timeout, [this]() {
      if (pending_) {
        pending_ = false;
        timer_.start();  // re-open the window BEFORE running the action (below)
        fire();
      }
      // else: a quiet window — let the single-shot timer stay stopped.
    });
  }

  CoalescingTrigger(const CoalescingTrigger&) = delete;
  CoalescingTrigger& operator=(const CoalescingTrigger&) = delete;
  CoalescingTrigger(CoalescingTrigger&&) = delete;
  CoalescingTrigger& operator=(CoalescingTrigger&&) = delete;

  /// Run now (leading edge) if the window is open, otherwise mark a trailing run.
  void request() {
    if (timer_.isActive()) {
      pending_ = true;  // within the window — the trailing edge will run it
      return;
    }
    // Open the window BEFORE running the action: the rate is then measured from
    // the action's START, so a slow action (e.g. a multi-ms replot) does not leak
    // its own duration into the window and drag the effective rate below target.
    // It also makes a re-entrant request() from inside the action coalesce (the
    // timer is already active) instead of recursing.
    timer_.start();
    fire();
  }

  /// Total number of times the action has actually run (leading + trailing).
  [[nodiscard]] int fireCountForTesting() const noexcept {
    return fire_count_;
  }
  /// No window open, so no trailing run is still to come.
  [[nodiscard]] bool idleForTesting() const noexcept {
    return !timer_.isActive();
  }

 private:
  void fire() {
    ++fire_count_;
    action_();
  }

  std::function<void()> action_;
  QTimer timer_;
  bool pending_ = false;
  int fire_count_ = 0;
};

}  // namespace PJ
