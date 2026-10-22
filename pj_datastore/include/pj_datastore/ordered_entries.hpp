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

#include <algorithm>
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

/// Chooses entries by (store timestamp, UID); true selects the entry. Runs under the
/// caller's lock, once per entry: keep it cheap and never call back into the ObjectStore.
using EntryPredicate = std::function<bool(Timestamp, SequentialUID)>;

/// Room for `count` more elements, grown geometrically as push_back would (appends stay
/// amortized O(1) across calls), so the push_backs that follow cannot allocate: how the store
/// takes every allocation of a mutation before it unlinks anything.
template <typename T>
void reserveMore(std::vector<T>& values, size_t count) {
  if (values.capacity() - values.size() < count) {
    values.reserve(std::max({values.size() + count, values.capacity() < 4 ? size_t{8} : 2 * values.capacity()}));
  }
}

template <typename T>
void reserveOneMore(std::vector<T>& values) {
  reserveMore(values, 1);
}

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
  /// Bytes the closure ITSELF keeps alive (a captured payload, as streaming's
  /// kEager entries hold), charged to the series' memory budget like an owned
  /// payload. 0 for a closure that re-reads its bytes on demand.
  size_t resident_bytes = 0;
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
  /// Also the sample-identity memo type of the scene sampling layers (aliased as
  /// their SampleId): memberwise equality is sound because a uid names exactly one
  /// entry, and the value-initialized {invalid uid, 0} is their "none" sentinel.
  struct TimeRangeEntry {
    SequentialUID uid;
    Timestamp timestamp = 0;
    bool operator==(const TimeRangeEntry&) const = default;
  };

  /// Which branch push() took, so the caller can invalidate the warm cache only
  /// when a mid-stream insert could change an at-or-before lookup.
  enum class PushOrder { kInOrderAppend, kOutOfOrderInsert };

  /// Revisions of the entries, process-unique, so a series rebuilt after a clear never repeats one. `content` moves on
  /// every change; `rewrite` only when the change is not an append at or after the newest stamp or a front eviction,
  /// the two a reader can follow with a UID cursor and a timestamp floor without rescanning what it read.
  struct ContentIdentity {
    uint64_t content = 0;
    uint64_t rewrite = 0;
    bool operator==(const ContentIdentity&) const = default;
  };

  OrderedEntries();

  [[nodiscard]] ContentIdentity contentIdentity() const noexcept {
    return identity_;
  }

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
  // Its timestamp-order position, or nullopt when absent. O(log n), no payload touched.
  [[nodiscard]] std::optional<size_t> indexOfUid(SequentialUID uid) const;
  // Smallest RETAINED UID, or the invalid UID when empty.
  [[nodiscard]] SequentialUID firstUid() const;
  // Smallest retained UID strictly greater than `after`, or invalid when none.
  [[nodiscard]] SequentialUID nextUidAfter(SequentialUID after) const;
  // Largest UID among entries with timestamp <= t, or invalid when none. Unlike
  // indexAtOrBefore()->sequential_uid, this is the true high-water arrival UID of
  // the at-or-before prefix (an out-of-order insert can leave the newest-TIMESTAMP
  // entry's UID below a retained entry's). O(count with timestamp <= t).
  [[nodiscard]] SequentialUID maxUidAtOrBefore(Timestamp t) const;
  // The smallest timestamp among the entries whose UID is greater than `after`, or nullopt when none: the oldest of
  // what arrived since a cursor. O(log n + those entries).
  [[nodiscard]] std::optional<Timestamp> oldestTimestampAfterUid(SequentialUID after) const;

  // --- Mutation ---

  // Append (in-order fast path, amortized O(1)) or sorted-insert (out-of-order,
  // O(n) shift) so `entry_timestamps` stays ascending for at-or-before lookup.
  // Reads entry.timestamp for placement. Precondition: entry.sequential_uid is the
  // series max (the ObjectStore push paths mint it via SequentialUID::getNext()
  // immediately before) — the incremental uid_order maintenance relies on it. A
  // caller inserting a non-max UID must rebuildUidOrder() instead. An allocation
  // failure throws before anything changes and leaves `entry` with the caller.
  PushOrder push(ObjectEntry&& entry);
  // Move the oldest `count` entries (front positions) into `removed`, in array order, so the
  // caller decides where they are destroyed (ObjectStore drops them after releasing its locks).
  // O(size) whatever `count`: a retention jump expiring most of a series costs one pass, not one
  // pass per entry. The only allocation is room in `removed`, taken first, so a failure changes
  // nothing. Precondition: count <= size(). Triple only: the caller drops the warm cache and
  // adjusts memory accounting around it. Not a rewrite (see ContentIdentity).
  void evictFront(size_t count, std::vector<ObjectEntry>& removed);
  // A removal of entries at any position (not only the oldest prefix), decided before
  // anything changes so a transaction can validate every topic first.
  struct Removal {
    static constexpr uint32_t kRemoved = UINT32_MAX;
    // Where each entry lands once the removal is applied, or kRemoved.
    std::vector<uint32_t> new_position;
    size_t count = 0;
  };
  // Runs `drop` once per entry and changes nothing, so an exception from it leaves this
  // instance as it was.
  [[nodiscard]] Removal planRemoval(const EntryPredicate& drop) const;
  // Applies a plan made on this instance, unchanged since: the removed entries are moved
  // into `removed` in array order (the caller adjusts memory accounting and the warm
  // cache), the survivors close the gaps in place and keep their order and UIDs. O(n).
  // The only allocation is room in `removed` for `plan.count` more, taken first, so a
  // failure changes nothing.
  void remove(const Removal& plan, std::vector<ObjectEntry>& removed);
  // remove() then push() as one step that applies whole or not at all: the entry takes
  // a slot the removal freed, so after the room in `removed` nothing allocates (with
  // nothing to remove it is push()). Same preconditions as both.
  PushOrder removeAndPush(const Removal& plan, ObjectEntry&& entry, std::vector<ObjectEntry>& removed);
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
  // Indexes the entry at the back of `entries_`, which the two vectors do not cover yet,
  // and moves it to its sorted position. The vectors have room for one more, so nothing
  // here allocates.
  PushOrder indexBack();
  // The survivors of `plan` close its gaps at the front; the removed entries move into
  // `removed`, which has room for them. The vectors are final; the deque keeps its size,
  // with moved-from slots after the survivors. Returns the survivor count.
  size_t compact(const Removal& plan, std::vector<ObjectEntry>& removed);
  // Every mutator calls it once it changed something; `rewrite` as `ContentIdentity` defines it.
  void changed(bool rewrite) noexcept;

#ifndef NDEBUG
  // Assert uid_order is a size-matching permutation of entries with strictly
  // ascending projected UIDs. Debug tripwire for the two hand-rolled hot paths.
  void checkInvariant() const;
#endif

  std::deque<ObjectEntry> entries_;
  std::vector<Timestamp> entry_timestamps_;
  std::vector<uint32_t> uid_order_;
  ContentIdentity identity_;
};

}  // namespace PJ
