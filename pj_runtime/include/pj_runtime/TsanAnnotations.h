// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#if defined(__SANITIZE_THREAD__)
#define PJ_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PJ_TSAN_ENABLED 1
#endif
#endif
#ifdef PJ_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

namespace PJ::tsan {

/// Tells ThreadSanitizer about a hand-off it cannot see (a futex inside prebuilt Qt:
/// QThread::wait(), QThreadPool::start()). The handing-over thread calls `release`
/// on a token after its last shared access; the receiving thread calls `acquire`
/// on the same token before its first, which makes the ordering the primitive
/// already guarantees visible to TSan. An acquire whose release never ran merges
/// only an older clock. No-ops outside a TSan build.
inline void release(const void* token) {
#ifdef PJ_TSAN_ENABLED
  __tsan_release(const_cast<void*>(token));
#else
  static_cast<void>(token);
#endif
}

/// Receiving side of `release`; see there.
inline void acquire(const void* token) {
#ifdef PJ_TSAN_ENABLED
  __tsan_acquire(const_cast<void*>(token));
#else
  static_cast<void>(token);
#endif
}

}  // namespace PJ::tsan
