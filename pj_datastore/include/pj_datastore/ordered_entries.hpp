#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// OrderedEntries: the invariant-enforcing owner of one ObjectStore series' ordered
// entries. It encapsulates the three lockstep arrays that ObjectStore used to
// maintain by hand at ~14 sites — the entry deque, the ascending index-aligned
// timestamp vector, and the UID-order side index — behind methods that keep them
// consistent BY CONSTRUCTION. See docs/OBJECT_STORE_DESIGN.md.
//
// Qt-free, lock-free, cache-free: OrderedEntries owns ONLY the ordered-entry
// triple and its invariant. Locking (the per-series shared_mutex), the warm
// latestAt cache, retention/memory accounting, and payload resolution all stay in
// ObjectStore/ObjectSeries. The caller holds the series lock across every call.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/sequential_uid.hpp"

namespace PJ {

/// Eager payload: store-owned bytes, counted against the retention budget.
using SharedBuffer = std::shared_ptr<const std::vector<uint8_t>>;

/// Lazy payload: idempotent, thread-safe fetcher returning bytes + anchor.
/// Invoked on every read; bytes are not counted against the retention budget.
/// nullopt means the fetch FAILED — the source could not re-produce the bytes
/// (unreadable / truncated / corrupt). Distinct from an engaged view with zero
/// bytes, which is a payload that is legitimately empty. resolveEntry surfaces
/// failure as ResolvedObjectEntry::fetch_failed so consumers can report data
/// loss instead of silently skipping the entry.
using LazyCallback = std::function<std::optional<sdk::PayloadView>()>;

class ResidentSlot;  // pj_datastore/resident_payload_pool.hpp

/// Lazy payload: the fetcher, optionally seeded (`seed != nullptr`) with an
/// evictable resident copy of its ingest-time bytes. Seeded reads serve from
/// the slot while the ResidentPayloadPool keeps it resident (no fetch), then
/// degrade permanently to `fetch` — which must re-produce identical bytes —
/// once the pool's byte budget rolls over. Resident bytes are pool-accounted,
/// never counted against the series' retention budget (which governs whole
/// entries, not their residency).
struct LazyPayload {
  LazyCallback fetch;
  std::shared_ptr<ResidentSlot> seed;
};

/// Eager owned bytes, or a (possibly seeded) lazy resolver; resolveEntry
/// discriminates via std::get_if.
using ObjectEntryPayload = std::variant<SharedBuffer, LazyPayload>;

struct ObjectEntry {
  Timestamp timestamp = 0;
  SequentialUID sequential_uid;
  // Nanoseconds this entry's STORE timestamp has been slid relative to the
  // timestamps embedded in its payload bytes. Nonzero only after a time-shifted
  // dataset merge: the merge moves `timestamp` by the per-source delta but cannot
  // rewrite the payload, so a consumer that keys off timestamps embedded INSIDE
  // the payload (e.g. the TF buffer, indexed by each transform's own stamp) must
  // add this delta to land them on the shifted clock. Consumers that key off the
  // store `timestamp` ignore it. Accumulates across chained merges.
  Timestamp payload_stamp_shift = 0;
  ObjectEntryPayload payload;
};

/// Invariant-enforcing owner of a series' ordered entries. Holds three arrays
/// privately, kept in lockstep by every mutating method:
///   - `entries`: the payload entries, in ascending STORE-timestamp order.
///   - `entry_timestamps`: ascending, index-aligned with `entries` (the public
///     sorted-vector contract that EntryTimestampsView / timeRange expose).
///   - `uid_order`: a permutation of [0, size) with entries[uid_order[k]]
///     .sequential_uid STRICTLY ASCENDING in k — the bridge from the UID-cursor
///     read APIs back to timestamp-ordered storage (an out-of-order insert leaves
///     `entries` timestamp-sorted but NOT UID-sorted).
/// A #ifndef NDEBUG checker verifies the uid_order invariant from the two O(n)
/// mutating paths plus an O(1) tail check on the in-order append.
class OrderedEntries {
 public:
  /// One entry's stable identity plus its store timestamp, as returned by
  /// rangeByTime(). Decode-free — resolve the payload later via ObjectStore::at(uid).
  struct TimeRangeEntry {
    SequentialUID uid;
    Timestamp timestamp;
  };

  /// Which branch push() took, so the caller can invalidate the warm cache only
  /// when a mid-stream insert could change an at-or-before lookup.
  enum class PushOrder { kInOrderAppend, kOutOfOrderInsert };

  OrderedEntries() = default;

  // --- Size / basics ---

  [[nodiscard]] bool empty() const noexcept {
    return entries_.empty();
  }
  [[nodiscard]] size_t size() const noexcept {
    return entries_.size();
  }

  // --- Timestamp accessors (front/back precondition: !empty()) ---

  [[nodiscard]] Timestamp frontTimestamp() const {
    return entry_timestamps_.front();
  }
  [[nodiscard]] Timestamp backTimestamp() const {
    return entry_timestamps_.back();
  }
  // The ascending, index-aligned timestamp vector. Stable address for the lifetime
  // of this OrderedEntries; the caller wraps it in an EntryTimestampsView under the
  // series read lock. Its contents are the byte-for-byte public sorted contract.
  [[nodiscard]] const std::vector<Timestamp>& timestamps() const noexcept {
    return entry_timestamps_;
  }

  // --- Entry access by position ---

  // Unchecked; caller guarantees index < size().
  [[nodiscard]] const ObjectEntry& entryAt(size_t index) const {
    return entries_[index];
  }
  // Bounds-checked; nullptr when index >= size().
  [[nodiscard]] const ObjectEntry* atIndex(size_t index) const;
  // The oldest retained entry (precondition: !empty()). ObjectStore reads its
  // uid/payload for cache + memory accounting just before evictFront().
  [[nodiscard]] const ObjectEntry& frontEntry() const {
    return entries_.front();
  }

  // --- Time lookups ---

  // Index of the newest entry with timestamp <= t, or nullopt when none (empty or
  // all entries later than t). The latestAt()/indexAt() primitive.
  [[nodiscard]] std::optional<size_t> indexAtOrBefore(Timestamp t) const;
  // (uid, timestamp) of every entry with lo < timestamp <= hi, ascending. Empty
  // when hi <= lo or the window holds nothing. O(log n + window).
  [[nodiscard]] std::vector<TimeRangeEntry> rangeByTime(Timestamp lo, Timestamp hi) const;

  // --- UID-cursor lookups (binary-search uid_order, never `entries` directly) ---

  // The entry with this exact UID, or nullptr when absent (evicted / never here).
  [[nodiscard]] const ObjectEntry* atUid(SequentialUID uid) const;
  // Smallest RETAINED UID, or the invalid UID when empty.
  [[nodiscard]] SequentialUID firstUid() const;
  // Smallest retained UID strictly greater than `after`, or invalid when none.
  [[nodiscard]] SequentialUID nextUidAfter(SequentialUID after) const;
  // Largest UID among entries with timestamp <= t, or invalid when none. Unlike
  // indexAtOrBefore()->sequential_uid, this is the true high-water arrival UID of
  // the at-or-before prefix (an out-of-order insert can leave the newest-TIMESTAMP
  // entry's UID below a retained entry's). O(count with timestamp <= t).
  [[nodiscard]] SequentialUID maxUidAtOrBefore(Timestamp t) const;

  // --- Mutation ---

  // Append (in-order fast path, amortized O(1)) or sorted-insert (out-of-order,
  // O(n) shift) so `entry_timestamps` stays ascending for at-or-before lookup.
  // Reads entry.timestamp for placement. Precondition: entry.sequential_uid is the
  // series max (the ObjectStore push paths mint it via SequentialUID::getNext()
  // immediately before) — the incremental uid_order maintenance relies on it. A
  // caller inserting a non-max UID must rebuildUidOrder() instead.
  PushOrder push(ObjectEntry&& entry);
  // Drop the oldest entry (front position). No-op when empty. Triple only — the
  // caller drops the warm cache and adjusts memory accounting around it.
  void evictFront();
  // Empty the triple. Memory/cache stay the caller's (see clearEntriesLocked).
  void clear();
  // Slide every store timestamp by `delta` (also entry_timestamps and each entry's
  // payload_stamp_shift); uid_order is unchanged (a uniform shift reorders nothing).
  // Returns true iff it mutated (delta != 0 and non-empty), so the caller can drop
  // the warm cache only when needed.
  bool shift(Timestamp delta);
  // Reassign fresh ascending UIDs in current array order; uid_order becomes the
  // identity permutation. For merge/replace destinations whose entries are already
  // timestamp-sorted (so UID order == array order).
  void reuidAll();
  // Rebuild uid_order as the permutation of [0, size) sorted by sequential_uid
  // ascending. O(n log n), correct from ANY entries state — the primitive for
  // wholesale rebuilds (flushTo destination / reattach).
  void rebuildUidOrder();

  // --- Bulk cross-series ops (move entries between instances) ---

  // flushTo destination: reassign each entry of `src` a fresh ascending UID (in
  // src array order), append it here, extend timestamps, rebuild uid_order over the
  // combined entries, and leave `src` empty. `src`'s pre-existing UIDs are NOT
  // preserved (the destination generation re-UIDs); this instance's ARE.
  void appendReuidFrom(OrderedEntries& src);
  // replaceDatasetFrom target: adopt `src`'s entries + timestamps wholesale,
  // reassign fresh ascending identity UIDs, and leave `src` empty.
  void adoptReuidFrom(OrderedEntries& src);
  // mergeDatasets: move every entry out (appended to `out`) and empty this triple.
  // The caller sorts the pooled entries and reseats a destination via
  // assignSortedReuid(); source contributors stay emptied.
  void moveEntriesInto(std::vector<ObjectEntry>& out);
  // mergeDatasets destination: reseat from a timestamp-sorted entry pool (rebuilds
  // timestamps from each entry) and assign fresh ascending identity UIDs.
  void assignSortedReuid(std::vector<ObjectEntry>&& sorted);

  // --- Detach/reattach snapshot transfer (ObjectStore::ObjectDatasetSnapshot) ---

  // Move the entries + timestamps out into the snapshot's raw arrays and empty
  // this triple (uid_order is derivable, so it is not snapshotted).
  void detachInto(std::deque<ObjectEntry>& entries_out, std::vector<Timestamp>& stamps_out);
  // Move snapshotted entries + timestamps back and rebuild uid_order from them
  // (restored entries may carry preserved out-of-order UID inversions).
  void restoreRebuild(std::deque<ObjectEntry>&& entries_in, std::vector<Timestamp>&& stamps_in);

 private:
#ifndef NDEBUG
  // Assert uid_order is a size-matching permutation of entries with strictly
  // ascending projected UIDs. Debug tripwire for the two hand-rolled hot paths.
  void checkInvariant() const;
#endif

  std::deque<ObjectEntry> entries_;
  std::vector<Timestamp> entry_timestamps_;
  std::vector<uint32_t> uid_order_;
};

}  // namespace PJ
