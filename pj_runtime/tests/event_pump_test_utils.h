// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Event-loop pumps for tests that wait on a real QTimer (BatchEngine's brake) or a
// real EnginePool's queued publish, without a per-test event-loop harness.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <functional>

namespace PJ::test {

/// Pumps this thread's event loop until `done()` or `timeout_ms` elapses.
inline void pumpUntil(const std::function<bool()>& done, int timeout_ms = 3000) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
}

/// Pumps this thread's event loop for `duration_ms`.
inline void pumpFor(int duration_ms) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < duration_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

}  // namespace PJ::test
