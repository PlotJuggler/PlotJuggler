// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QObject>
#include <memory>
#include <utility>

#if defined(__SANITIZE_THREAD__)
#define PJ_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PJ_TSAN_ENABLED 1
#endif
#endif
#ifdef PJ_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
// The TSan runtime's dynamic-annotation entry points (what Abseil's
// ABSL_ANNOTATE_IGNORE_READS_AND_WRITES_BEGIN calls); no public header declares them.
extern "C" {
void AnnotateIgnoreReadsBegin(const char* file, int line);
void AnnotateIgnoreReadsEnd(const char* file, int line);
void AnnotateIgnoreWritesBegin(const char* file, int line);
void AnnotateIgnoreWritesEnd(const char* file, int line);
}
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

/// Hides the calling thread's memory accesses from ThreadSanitizer while in scope; its
/// synchronization stays visible. For the part of a hand-off that happens inside prebuilt Qt
/// after the last point `release` can mark: a queued call allocates its functor on the posting
/// thread and frees it on the receiving one, so nothing the poster does can order that
/// allocation. Keep the scope to the hand-off alone: no access inside it is ever checked.
class IgnoreAccesses {
 public:
  IgnoreAccesses() {
#ifdef PJ_TSAN_ENABLED
    AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
    AnnotateIgnoreWritesBegin(__FILE__, __LINE__);
#endif
  }
  ~IgnoreAccesses() {
#ifdef PJ_TSAN_ENABLED
    AnnotateIgnoreWritesEnd(__FILE__, __LINE__);
    AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
#endif
  }
  IgnoreAccesses(const IgnoreAccesses&) = delete;
  IgnoreAccesses& operator=(const IgnoreAccesses&) = delete;
};

/// QMetaObject::invokeMethod(receiver, call, Qt::QueuedConnection) from another thread, with the
/// hand-off made visible to ThreadSanitizer. Qt orders the post before the call with a futex inside
/// prebuilt Qt that TSan cannot see, so TSan would report the receiver's first touch of anything
/// the posting thread wrote: the call's captures, the state it reads, and the functor Qt allocates
/// on the poster and frees on the receiver. The poster releases a token before posting and the
/// call acquires it before running; the post itself, where Qt allocates the functor after that
/// release, is hidden from TSan.
///
/// Each call gets its own token, freed with the functor. One token shared by every post would
/// over-synchronize: TSan accumulates releases on an address, so a call that acquires late would
/// inherit what the poster wrote after posting it (up to its next post), and TSan would miss a
/// real race between those writes and the call. Plain invokeMethod outside a TSan build.
template <typename F>
void invokeQueued(QObject* receiver, F&& call) {
#ifdef PJ_TSAN_ENABLED
  auto token = std::make_shared<char>();
  release(token.get());
  const IgnoreAccesses hand_off;
  QMetaObject::invokeMethod(
      receiver,
      [token = std::move(token), call = std::forward<F>(call)]() mutable {
        acquire(token.get());
        call();
      },
      Qt::QueuedConnection);
#else
  QMetaObject::invokeMethod(receiver, std::forward<F>(call), Qt::QueuedConnection);
#endif
}

}  // namespace PJ::tsan
