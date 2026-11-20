#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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

/// Owned payload, counted against the retention budget.
using SharedBuffer = std::shared_ptr<const std::vector<uint8_t>>;

/// True selects the entry. Runs once per entry under the store's locks: keep it cheap; calling the ObjectStore from it
/// deadlocks.
using EntryPredicate = std::function<bool(Timestamp, SequentialUID)>;

/// Room for `count` more, grown geometrically (appends stay amortized O(1)), so the push_backs that follow cannot
/// allocate: the store takes every allocation of a mutation before it unlinks anything.
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

/// Idempotent and thread-safe; it runs on every read that misses the caches. nullopt means the source could not
/// reproduce the bytes (it surfaces as ResolvedObjectEntry::fetch_failed); an empty view is a legitimately empty
/// payload.
using LazyCallback = std::function<std::optional<sdk::PayloadView>()>;

class ResidentSlot;  // pj_datastore/resident_payload_pool.hpp

/// A non-null `seed` serves reads without a fetch while the ResidentPayloadPool keeps it; once evicted, reads use
/// `fetch` for good, which must reproduce the same bytes. Seeded bytes count against the pool, not the series budget.
struct LazyPayload {
  LazyCallback fetch;
  std::shared_ptr<ResidentSlot> seed;
  /// Bytes the closure itself keeps alive (a captured payload), charged to the series' memory budget like an owned
  /// payload; 0 when it re-reads on demand.
  size_t resident_bytes = 0;
};

using ObjectEntryPayload = std::variant<SharedBuffer, LazyPayload>;

struct ObjectEntry {
  Timestamp timestamp = 0;
  SequentialUID sequential_uid;
  // How far a time-shifted merge moved `timestamp` (ns, accumulated over chained merges). The payload bytes are never
  // rewritten, so a consumer that reads timestamps embedded in them (the TF buffer) adds this to land on the store
  // clock; consumers of `timestamp` ignore it.
  Timestamp payload_stamp_shift = 0;
  ObjectEntryPayload payload;
};

/// One ObjectStore series' entries. Three private arrays stay in lockstep through every mutator: the entries in
/// ascending store-timestamp order, their timestamps (the sorted vector EntryTimestampsView exposes), and
/// `uid_order`, the permutation that lists them by strictly ascending UID (an out-of-order insert leaves the entries
/// sorted by time but not by UID). Debug builds check the permutation. No locking or accounting here: the caller
/// holds the series lock and owns memory accounting and the warm cache.
class OrderedEntries {
 public:
  /// The scene sampling layers use it as their sample identity (SampleId): a UID names exactly one entry, and the
  /// value-initialized {invalid, 0} means "none".
  struct TimeRangeEntry {
    SequentialUID uid;
    Timestamp timestamp = 0;
    bool operator==(const TimeRangeEntry&) const = default;
  };

  /// Tells the caller whether an at-or-before lookup may have changed (so the warm cache must go).
  enum class PushOrder { kInOrderAppend, kOutOfOrderInsert };

  /// Process-unique revisions, so a series rebuilt after a clear never repeats one. `content` moves on every change;
  /// `rewrite` on every change except an append at or after the newest stamp and a front eviction, the two a reader
  /// can follow with a UID cursor and a timestamp floor without rescanning. `removal` moves only when an entry a reader
  /// may hold is gone or changed (removed, replaced, cleared, shifted, re-UIDed), never on an insert or a front
  /// eviction: an arrival-order reader (drainNewSince()) that orders entries itself rescans when it moves.
  /// A `removal` move implies a `rewrite` move.
  struct ContentIdentity {
    uint64_t content = 0;
    uint64_t rewrite = 0;
    uint64_t removal = 0;
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

  // --- Timestamp accessors (front/back: not empty) ---

  [[nodiscard]] Timestamp frontTimestamp() const {
    return entry_timestamps_.front();
  }
  [[nodiscard]] Timestamp backTimestamp() const {
    return entry_timestamps_.back();
  }
  // Index-aligned with the entries; its address is stable for this object's lifetime (EntryTimestampsView holds it).
  [[nodiscard]] const std::vector<Timestamp>& timestamps() const noexcept {
    return entry_timestamps_;
  }

  // --- Entry access by position ---

  // Unchecked.
  [[nodiscard]] const ObjectEntry& entryAt(size_t index) const {
    return entries_[index];
  }
  // nullptr when out of range.
  [[nodiscard]] const ObjectEntry* atIndex(size_t index) const;

  // --- Time lookups ---

  // The newest entry with timestamp <= t.
  [[nodiscard]] std::optional<size_t> indexAtOrBefore(Timestamp t) const;
  // Entries with lo < timestamp <= hi, ascending. O(log n + window).
  [[nodiscard]] std::vector<TimeRangeEntry> rangeByTime(Timestamp lo, Timestamp hi) const;

  // --- UID-cursor lookups (binary search over uid_order) ---

  // nullptr when absent.
  [[nodiscard]] const ObjectEntry* atUid(SequentialUID uid) const;
  // Timestamp-order position. O(log n).
  [[nodiscard]] std::optional<size_t> indexOfUid(SequentialUID uid) const;
  // Smallest retained UID; invalid when empty.
  [[nodiscard]] SequentialUID firstUid() const;
  // Smallest retained UID above `after`; invalid when none.
  [[nodiscard]] SequentialUID nextUidAfter(SequentialUID after) const;
  // See ObjectStore::maxUidAtOrBefore(). O(entries with timestamp <= t).
  [[nodiscard]] SequentialUID maxUidAtOrBefore(Timestamp t) const;
  // Oldest timestamp among entries with UID > `after`. O(log n + those entries).
  [[nodiscard]] std::optional<Timestamp> oldestTimestampAfterUid(SequentialUID after) const;

  // --- Mutation ---

  // Appends in amortized O(1), or inserts out of order with an O(n) shift. `entry`'s UID must be the series maximum
  // (the store mints it right before), which the incremental uid_order update relies on; otherwise use
  // rebuildUidOrder(). An allocation failure throws before anything changes and leaves `entry` with the caller.
  PushOrder push(ObjectEntry&& entry);
  // Moves the oldest `count` (<= size()) into `removed`, which the store destroys after releasing its locks. One
  // O(size) pass whatever `count`. Only `removed` grows, first, so a failure changes nothing.
  void evictFront(size_t count, std::vector<ObjectEntry>& removed);
  // Removes entries at any position, planned before anything changes so a transaction can validate every topic
  // first.
  struct Removal {
    static constexpr uint32_t kRemoved = UINT32_MAX;
    // Each entry's position after the removal, or kRemoved.
    std::vector<uint32_t> new_position;
    size_t count = 0;
  };
  // Runs `drop` once per entry and changes nothing, so an exception from it leaves this object as it was.
  [[nodiscard]] Removal planRemoval(const EntryPredicate& drop) const;
  // Applies a plan made on this object, unchanged since: removed entries move into `removed`, survivors keep their
  // order and UIDs. O(n). Only `removed` grows, first, so a failure changes nothing.
  void remove(const Removal& plan, std::vector<ObjectEntry>& removed);
  // remove() then push(), whole or not at all: the entry takes a freed slot, so nothing allocates after `removed`
  // grows. Preconditions of both.
  PushOrder removeAndPush(const Removal& plan, ObjectEntry&& entry, std::vector<ObjectEntry>& removed);
  void clear();
  // Moves every timestamp and payload_stamp_shift by `delta`. Returns whether anything changed.
  bool shift(Timestamp delta);
  // Fresh ascending UIDs in array order, for destinations already sorted by time.
  void reuidAll();
  // O(n log n); correct from any state.
  void rebuildUidOrder();

  // --- Bulk cross-series ops (move entries between instances) ---

  // flushTo(): adds `src` with fresh UIDs in timestamp order (a moved entry lands after equal stamps, as push() does;
  // this object's UIDs stay) and empties it.
  void appendReuidFrom(OrderedEntries& src);
  // replaceDatasetFrom(): takes `src`'s entries with fresh UIDs and empties it.
  void adoptReuidFrom(OrderedEntries& src);
  // mergeDatasets(): moves every entry into `out`; the caller sorts the pool and reseats a destination with
  // assignSortedReuid().
  void moveEntriesInto(std::vector<ObjectEntry>& out);
  // `sorted` must be in timestamp order; assigns fresh UIDs.
  void assignSortedReuid(std::vector<ObjectEntry>&& sorted);

  // --- Detach/reattach snapshot transfer (ObjectStore::ObjectDatasetSnapshot) ---

  void detachInto(std::deque<ObjectEntry>& entries_out, std::vector<Timestamp>& stamps_out);
  // Rebuilds uid_order: restored entries may keep out-of-order UIDs.
  void restoreRebuild(std::deque<ObjectEntry>&& entries_in, std::vector<Timestamp>&& stamps_in);

 private:
  // Sorts the entry just pushed to the back of entries_ into place; the vectors already have room, so nothing
  // allocates.
  PushOrder indexBack();
  // Survivors close the gaps at the front, removed entries move into `removed` (which has room); the deque keeps
  // its size, moved-from slots at the back. Returns the survivor count.
  size_t compact(const Removal& plan, std::vector<ObjectEntry>& removed);
  // Which ContentIdentity revisions a change moves, each level including the ones before it.
  enum class Change { kFollowable, kInsert, kRemoval };
  // Every mutator calls it on a change.
  void changed(Change change) noexcept;

#ifndef NDEBUG
  void checkInvariant() const;
#endif

  std::deque<ObjectEntry> entries_;
  std::vector<Timestamp> entry_timestamps_;
  std::vector<uint32_t> uid_order_;
  ContentIdentity identity_;
};

}  // namespace PJ
