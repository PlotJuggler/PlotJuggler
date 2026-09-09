// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// CoalescingTrigger caps a repeatedly-requested action at ~1000/interval Hz:
// one leading run immediately, intermediate requests dropped, one trailing run
// when the window closes. This is the single "cap the rate once at the origin"
// primitive used to throttle the playhead fan-out (playback + scrubbing + seek)
// to ~30 Hz.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include "pj_widgets/CoalescingTrigger.h"

namespace {

// Run the event loop for `ms` so the throttle timer can fire.
void spin(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

constexpr int kIntervalMs = 30;

}  // namespace

TEST(CoalescingTrigger, BurstRunsExactlyOneLeadingAction) {
  int calls = 0;
  PJ::CoalescingTrigger trigger(kIntervalMs, [&calls]() { ++calls; });

  for (int i = 0; i < 10; ++i) {
    trigger.request();
  }
  // Synchronous burst within one window → one leading run, the rest coalesced.
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(trigger.fireCountForTesting(), 1);
}

TEST(CoalescingTrigger, CoalescedRequestsRunOnceOnTheTrailingEdge) {
  int calls = 0;
  PJ::CoalescingTrigger trigger(kIntervalMs, [&calls]() { ++calls; });

  for (int i = 0; i < 10; ++i) {
    trigger.request();
  }
  EXPECT_EQ(calls, 1);  // leading only, so far

  spin(kIntervalMs * 3);

  // The coalesced burst fires exactly one trailing action (leading + trailing).
  EXPECT_EQ(calls, 2);
}

TEST(CoalescingTrigger, QuietWindowFiresNoTrailingAction) {
  int calls = 0;
  PJ::CoalescingTrigger trigger(kIntervalMs, [&calls]() { ++calls; });

  trigger.request();  // leading
  EXPECT_EQ(calls, 1);
  spin(kIntervalMs * 3);  // window passes with no further requests
  EXPECT_EQ(calls, 1);    // no spurious trailing run

  trigger.request();  // window is closed → leading again
  EXPECT_EQ(calls, 2);
}

TEST(CoalescingTrigger, ReentrantRequestFromActionCoalescesInsteadOfRecursing) {
  // An action that re-enters request() (e.g. a tracker broadcast that indirectly
  // causes another cursor update) must coalesce into the trailing edge, not
  // recurse synchronously. The window is opened before the action runs, so the
  // nested request() sees the timer active and only marks pending.
  int calls = 0;
  bool reenter_once = true;
  PJ::CoalescingTrigger* self = nullptr;
  PJ::CoalescingTrigger trigger(kIntervalMs, [&]() {
    ++calls;
    if (reenter_once) {
      reenter_once = false;
      self->request();  // re-enter from inside the action
    }
  });
  self = &trigger;

  trigger.request();
  // The re-entrant request during the leading action coalesced — no synchronous
  // recursion, so exactly one run so far.
  EXPECT_EQ(calls, 1);

  spin(kIntervalMs * 3);
  // The coalesced re-entrant request fires exactly once on the trailing edge.
  EXPECT_EQ(calls, 2);
}
