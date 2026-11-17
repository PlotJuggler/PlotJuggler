#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// ResidentPayloadPool: an app-global, byte-bounded window over ingest-time object payloads. A host whose
// ObjectIngestPolicy fetches bytes at ingest (kLazyObjectsEagerScalars) holds each payload once, during the producer's
// synchronous push. The pool keeps admitted payloads resident until the byte budget rolls over; then their slots
// empty and the entries fall back to their lazy re-fetch (for MCAP, re-decompressing a chunk). Residency stays
// within `capacity_bytes`.
//
// Payloads are split into small and large classes, each with its own FIFO and a reserved share for the small one. A
// payload evicts within its own class, except that a small one also reclaims space a large one borrowed from its share;
// so a few large payloads cannot empty a numerous small topic (docs/TECHNICAL_NOTES.md "ObjectStore costs"). Eviction
// runs on the admitting (ingest) thread, and load() takes no pool-wide lock.
//
// Accounting charges `payload.bytes.size()`. A zero-copy anchored payload can pin a larger upstream allocation (the
// producer's decompressed chunk) until eviction, so the budget bounds logical payload bytes, not anchor-backed RSS.
//
// Threading: `admit()` and slot destruction may run on any thread; consumer threads call `load()` concurrently.
// Payload anchors may run plugin release code, so they are released outside the pool mutex. Callers may hold
// ObjectStore locks when a slot retires (entry destruction); pool code never takes store locks.

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

#include "pj_base/buffer_anchor.hpp"

namespace PJ {

class ResidentPayloadPool;

namespace detail {
struct ResidentPoolState;
}  // namespace detail

/// One admitted payload's evictable residency. Owned (shared_ptr) by the ObjectStore entry it seeds; the pool tracks
/// it weakly. The payload goes resident -> empty once, on pool eviction or slot destruction, and is never re-seeded:
/// a reader copies the immutable payload or sees empty and uses the lazy fallback.
class ResidentSlot {
 public:
  ~ResidentSlot();

  ResidentSlot(const ResidentSlot&) = delete;
  ResidentSlot& operator=(const ResidentSlot&) = delete;
  ResidentSlot(ResidentSlot&&) = delete;
  ResidentSlot& operator=(ResidentSlot&&) = delete;

  /// A copy of the payload (the anchor is shared), or nullopt once evicted. Takes only the per-slot mutex.
  [[nodiscard]] std::optional<sdk::PayloadView> load() const;

 private:
  friend class ResidentPayloadPool;
  ResidentSlot(std::shared_ptr<detail::ResidentPoolState> state, sdk::PayloadView payload, size_t charged_bytes);

  /// Moves the payload out (nullopt if already taken). The caller subtracts the charge and releases the payload's
  /// anchor outside the pool mutex.
  std::optional<sdk::PayloadView> take();

  mutable std::mutex mutex_;
  std::optional<sdk::PayloadView> payload_;
  std::shared_ptr<detail::ResidentPoolState> state_;
  size_t charged_bytes_ = 0;
};

/// One per app/session, shared (shared_ptr) with every ObjectStore that ingest pushes into. Capacity 0 disables
/// admission: every push takes the plain lazy path.
class ResidentPayloadPool {
 public:
  /// Counters are cumulative; `resident_bytes` and `high_water_bytes` count resident payloads, not entries.
  struct Stats {
    size_t capacity_bytes = 0;
    size_t resident_bytes = 0;
    size_t high_water_bytes = 0;
    uint64_t admitted = 0;
    uint64_t rejected_oversize = 0;
    uint64_t evicted = 0;
    uint64_t resident_hits = 0;
  };

  explicit ResidentPayloadPool(size_t capacity_bytes);
  ~ResidentPayloadPool();

  ResidentPayloadPool(const ResidentPayloadPool&) = delete;
  ResidentPayloadPool& operator=(const ResidentPayloadPool&) = delete;
  ResidentPayloadPool(ResidentPayloadPool&&) = delete;
  ResidentPayloadPool& operator=(ResidentPayloadPool&&) = delete;

  /// nullptr when the pool is disabled, the payload is empty or exceeds the whole capacity, or a large payload cannot
  /// fit beside the resident small ones. Evicts older slots on this thread to make room; their anchors are released
  /// after the pool lock drops.
  [[nodiscard]] std::shared_ptr<ResidentSlot> admit(sdk::PayloadView payload);

  /// Evicts every resident slot; entries keep their lazy fallbacks.
  void trim();

  [[nodiscard]] Stats stats() const;

 private:
  std::shared_ptr<detail::ResidentPoolState> state_;
};

}  // namespace PJ
