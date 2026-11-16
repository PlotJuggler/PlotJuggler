// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// TSan lane only. Proves PJ::tsan::invokeQueued synchronizes each queued call with exactly what
// the poster did before posting IT, nothing later. CTest passes only on a race report naming
// readWhatTheCallWasNotHandedOff: with one token shared by every post, call A's acquire would
// inherit call B's release and TSan would miss this race, as it would miss a real one.

#include <QCoreApplication>
#include <QObject>
#include <atomic>
#include <thread>

#include "pj_runtime/TsanAnnotations.h"

namespace {

/// Intentionally racy; never "fix" it: the read must be reported.
[[gnu::noinline]] int readWhatTheCallWasNotHandedOff(const volatile int* value) {
  return *value;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QObject receiver;
  volatile int written_after_post = 0;
  // Relaxed on both sides: TSan derives no ordering from it, so the only edges between the worker
  // and this thread are the ones invokeQueued creates.
  std::atomic<bool> both_posted{false};

  std::thread worker([&] {
    // Call A, posted BEFORE the write it reads: nothing orders that write before A.
    PJ::tsan::invokeQueued(&receiver, [&] { readWhatTheCallWasNotHandedOff(&written_after_post); });
    written_after_post = 1;
    // Call B, posted after the write. Its token must not reach A.
    PJ::tsan::invokeQueued(&receiver, [] {});
    both_posted.store(true, std::memory_order_relaxed);
  });

  while (!both_posted.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  // Runs A, then B, straight from the post queue; never polls the dispatcher's wakeup fd, whose
  // read TSan would count as an acquire of the worker's wakeup write.
  QCoreApplication::sendPostedEvents();
  worker.join();
  // A clean exit means TSan saw no race: CTest fails it (no matching report).
  return 0;
}
