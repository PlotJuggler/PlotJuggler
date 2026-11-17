#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QTimer>
#include <functional>
#include <utility>

namespace PJ {

/// Leading and trailing rate limiter for a repeatedly requested action.
///
/// `request()` runs the action at once if the throttle window is open (leading edge) and opens a window of
/// `interval_ms`; further requests within the window coalesce into one action fired when it closes (trailing edge). A
/// burst of N requests is one leading plus at most one trailing run, capping the action at about `1000/interval_ms` Hz
/// however fast requests arrive.
///
/// The action takes no argument: the caller stores the latest payload (the current tracker time) and the trailing edge
/// runs against whatever was stored last, i.e. "newest value wins, intermediate ones dropped", as a moving playhead
/// needs.
///
/// The one "cap the rate at the origin" primitive: put one on the fan-out that every driver (playback ticks, scrubbing,
/// seeks) flows through, not one per downstream consumer.
///
/// Not a QObject, and non-copyable and non-movable: it owns a QTimer whose connection captures `this`, and the timer is
/// destroyed with the trigger, so the connection cannot outlive it.
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

  /// Runs now (leading edge) if the window is open, otherwise marks a trailing run.
  void request() {
    if (timer_.isActive()) {
      pending_ = true;  // within the window — the trailing edge will run it
      return;
    }
    // The window opens before the action runs: the rate is measured from the action's start, so a slow action (a
    // multi-ms replot) does not stretch the window and drag the effective rate below target, and a re-entrant request()
    // from inside the action coalesces (the timer is already active) instead of recursing.
    timer_.start();
    fire();
  }

  /// Number of times the action has run (leading and trailing).
  [[nodiscard]] int fireCountForTesting() const noexcept {
    return fire_count_;
  }
  /// No window is open, so no trailing run is pending.
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
