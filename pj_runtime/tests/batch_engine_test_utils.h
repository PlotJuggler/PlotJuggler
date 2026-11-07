// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// BatchEngine's process-wide test seams, scoped to one test.

#include <QThread>
#include <atomic>
#include <chrono>
#include <functional>
#include <utility>

#include "pj_runtime/BatchEngine.h"

namespace PJ::test {

/// Installs a compute probe for one test and removes it on scope exit, even when an
/// assertion returns early. Declare it BEFORE the pool, so the pool joins the
/// workers that may be inside the probe before the probe goes.
class ScopedComputeProbe {
 public:
  explicit ScopedComputeProbe(std::function<void(QThread*)> probe) {
    BatchEngine::setComputeThreadProbeForTest(std::move(probe));
  }
  ~ScopedComputeProbe() {
    BatchEngine::setComputeThreadProbeForTest({});
  }
  ScopedComputeProbe(const ScopedComputeProbe&) = delete;
  ScopedComputeProbe& operator=(const ScopedComputeProbe&) = delete;
};

/// A compute probe that holds every compute running off the calling thread until
/// `released`, at most 5 s so a failed assertion cannot hang the pool's destructor. A
/// compute on the calling thread itself (a synchronous run) passes straight through.
inline std::function<void(QThread*)> holdPoolComputesUntil(const std::atomic<bool>& released) {
  QThread* const caller = QThread::currentThread();
  return [&released, caller](QThread* thread) {
    for (int waited_ms = 0; thread != caller && !released.load() && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  };
}

/// Sets how long a `CreateWait::kBounded` create waits for its first run, for one test:
/// `kGenerousCreateBudget` when its assertions must not race the pool, or
/// `BatchEngine::kCreateBudget` to test the bounded wait itself.
class ScopedCreateBudget {
 public:
  explicit ScopedCreateBudget(std::chrono::milliseconds budget)
      : previous_(BatchEngine::setCreateBudgetForTest(budget)) {}
  ~ScopedCreateBudget() {
    BatchEngine::setCreateBudgetForTest(previous_);
  }
  ScopedCreateBudget(const ScopedCreateBudget&) = delete;
  ScopedCreateBudget& operator=(const ScopedCreateBudget&) = delete;

 private:
  std::chrono::milliseconds previous_;
};

/// A create budget long enough that a bounded create's first run always finishes
/// inside it, as a sanitizer build under `ctest -j` may need. The app GUI tests apply
/// it to every case (`tests/support/gui_test_env.cpp`), since a layout load is bounded.
inline constexpr std::chrono::milliseconds kGenerousCreateBudget{30'000};

}  // namespace PJ::test
