#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// ObjectStore: timestamped opaque byte payloads stored beside the columnar
// DataEngine, selectable by time. See docs/OBJECT_STORE_DESIGN.md.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/ordered_entries.hpp"
#include "pj_datastore/sequential_uid.hpp"

namespace PJ {

class ResidentPayloadPool;  // pj_datastore/resident_payload_pool.hpp

struct ObjectTopicId {
  uint32_t id = 0;

  bool operator==(const ObjectTopicId& other) const {
    return id == other.id;
  }
  bool operator!=(const ObjectTopicId& other) const {
    return id != other.id;
  }
};

/// Identity for an object topic: dataset scope + name (unique per dataset) plus
/// opaque metadata_json retained verbatim for callers that interpret bytes.
struct ObjectTopicDescriptor {
  DatasetId dataset_id = 0;
  std::string topic_name;
  std::string metadata_json;
};

/// Outcome of ObjectStore::replaceDatasetFrom() — the object-side parallel of
/// DatasetReplaceResult. The remapped pairs let the caller re-register object
/// parsers under the stable primary ObjectTopicId after the swap.
struct ObjectDatasetReplaceResult {
  /// (staged ObjectTopicId, primary ObjectTopicId) for every adopted topic
  /// (both matched and newly added), so a parser keyed by the staged id can be
  /// re-registered under the primary id. A newly added topic is simply one whose
  /// primary id did not exist before — callers needing only the remap use this.
  std::vector<std::pair<ObjectTopicId, ObjectTopicId>> remapped;
  /// Primary object topics removed (no staged match).
  std::vector<ObjectTopicId> removed_topics;
};

/// Outcome of ObjectStore::mergeDatasets() — the object-side parallel of
/// DatasetMergeReport. Object merge is mechanical: no canonical-type validation.
struct ObjectDatasetMergeReport {
  /// (source ObjectTopicId, anchor ObjectTopicId) for every shared-name fold.
  std::vector<std::pair<ObjectTopicId, ObjectTopicId>> remapped;
  /// Source-only topics reparented under the anchor dataset (kept their ObjectTopicId).
  std::vector<ObjectTopicId> added_topics;
  /// Source datasets whose object topics were emptied. Datasets stay registered.
  std::vector<DatasetId> consumed_datasets;
};

// SharedBuffer / LazyCallback / ObjectEntry live in ordered_entries.hpp (the
// OrderedEntries value type owns the entry deque), included above.

struct ResolvedObjectEntry {
  Timestamp timestamp = 0;
  SequentialUID sequential_uid;
  // Carried through from ObjectEntry — see its doc-comment. A consumer that reads
  // timestamps embedded in `payload` adds this to reconcile a time-shifted merge.
  Timestamp payload_stamp_shift = 0;
  // Non-owning Span over the bytes plus an opaque anchor (any shared_ptr<T>).
  // Consumers read `payload.bytes`; retain `payload.anchor` to keep the bytes
  // alive past the resolve call. resolveEntry never casts the anchor.
  sdk::PayloadView payload;
  // True when this entry's lazy fetch FAILED (payload left empty): the source
  // could not re-produce the bytes (file truncated, replaced, or corrupt).
  // Distinct from a legitimately empty payload. Cursor-style consumers use this
  // to count and report data loss instead of silently skipping the entry; the
  // loss is permanent (re-reading a corrupt region cannot succeed), so cursors
  // still advance past the entry.
  bool fetch_failed = false;
};

struct RetentionBudget {
  int64_t time_window_ns = 0;
  size_t max_memory_bytes = 0;
  // Keep at most this many of the most recent entries (0 = unlimited). Lets a
  // producer that republishes one snapshot per topic (e.g. plot markers, pushed
  // at a sentinel timestamp) cap accumulation at 1 instead of growing per push.
  size_t max_entries = 0;
};

/// Read view over a topic's entry timestamps that holds the series read lock for
/// its lifetime; keep it short-lived to avoid blocking writers.
class EntryTimestampsView {
 public:
  EntryTimestampsView() = default;
  EntryTimestampsView(std::shared_lock<std::shared_mutex> lock, const std::vector<Timestamp>* timestamps)
      : lock_(std::move(lock)), timestamps_(timestamps) {}

  [[nodiscard]] bool empty() const {
    return timestamps_ == nullptr || timestamps_->empty();
  }
  [[nodiscard]] size_t size() const {
    return timestamps_ != nullptr ? timestamps_->size() : 0;
  }
  [[nodiscard]] Timestamp operator[](size_t i) const {
    return (*timestamps_)[i];
  }
  [[nodiscard]] const Timestamp* begin() const {
    return timestamps_ != nullptr ? timestamps_->data() : nullptr;
  }
  [[nodiscard]] const Timestamp* end() const {
    return timestamps_ != nullptr ? timestamps_->data() + timestamps_->size() : nullptr;
  }

 private:
  std::shared_lock<std::shared_mutex> lock_;
  const std::vector<Timestamp>* timestamps_ = nullptr;
};

/// Timestamped opaque-blob store living alongside DataEngine: payloads selected
/// by time but never expanded into scalar columns (images, point clouds,
/// annotations). Owned (`pushOwned`) or lazy (`pushLazy`) entries, per-topic
/// retention, at-or-before lookup. Thread-safe: one shared_mutex per series +
/// one global lock for registration. Does NOT decode payloads or own renderer/
/// UI policy. See docs/OBJECT_STORE_DESIGN.md.
class ObjectStore {
 public:
  ObjectStore() = default;
  ~ObjectStore() = default;

  ObjectStore(const ObjectStore&) = delete;
  ObjectStore& operator=(const ObjectStore&) = delete;
  ObjectStore(ObjectStore&&) = delete;
  ObjectStore& operator=(ObjectStore&&) = delete;

  // --- Registration ---

  // Set `requested_id` non-zero to register with that exact id; otherwise the
  // next id is auto-assigned.
  Expected<ObjectTopicId> registerTopic(const ObjectTopicDescriptor& descriptor, ObjectTopicId requested_id = {});

  /// Atomic find-or-register for the refill contract: an existing topic with
  /// the same (dataset, name) AND byte-identical metadata_json is reused (its
  /// id — and every curve bound to it — survives a replace-refill); a
  /// same-name topic with DIFFERENT metadata is rejected loudly, preserving
  /// the collision alarm registerTopic gives. One lock: two racing callers
  /// converge on one topic instead of the loser erroring.
  Expected<ObjectTopicId> findOrRegisterTopic(const ObjectTopicDescriptor& descriptor);

  // Resolve a topic id by (dataset_id, topic_name) without registering. Returns
  // nullopt if no topic with that key exists. Used by hosts that need to bind a
  // parser-side write surface to a topic the source already registered.
  std::optional<ObjectTopicId> findTopic(DatasetId dataset_id, std::string_view topic_name) const;

  // Returns a COPY (not a reference): the descriptor lives in lock-protected
  // series storage that removeTopic/clear/replaceDatasetFrom can destroy, so a
  // reference would dangle once the caller drops the (internal) lock.
  ObjectTopicDescriptor descriptor(ObjectTopicId id) const;

  std::vector<ObjectTopicId> listTopics() const;
  std::vector<ObjectTopicId> listTopics(DatasetId dataset_id) const;
  /// `dataset_id`'s topics whose name starts with `name_prefix`, in registration
  /// order — one lock, no descriptor copies, for callers that scan a name family
  /// on every paint.
  std::vector<ObjectTopicId> listTopics(DatasetId dataset_id, std::string_view name_prefix) const;

  // --- Write ---

  Status pushOwned(ObjectTopicId id, Timestamp timestamp, std::vector<uint8_t> payload);

  // Fetcher runs on every read; the store retains the anchor via PayloadView
  // and never copies. The closure can return a view over bytes the producer
  // already owns (chunk cache, mmap, hand-off between stores).
  Status pushLazy(ObjectTopicId id, Timestamp timestamp, LazyCallback fetch);

  // pushLazy plus an ingest-time seed: `seed` (bytes the pushing host already
  // holds — the producer's synchronous "hot path") is admitted to the store's
  // ResidentPayloadPool so reads serve it with NO fetch until the pool's byte
  // budget evicts it; after that the entry behaves exactly like pushLazy(fetch).
  // Degrades to plain pushLazy when no pool is configured, the seed is empty,
  // or the pool rejects it (zero capacity / larger than the whole budget).
  // `fetch` must re-produce bytes identical to `seed`.
  Status pushLazyWithSeed(ObjectTopicId id, Timestamp timestamp, sdk::PayloadView seed, LazyCallback fetch);

  // --- Read ---

  std::optional<ResolvedObjectEntry> latestAt(ObjectTopicId id, Timestamp timestamp) const;

  std::optional<ResolvedObjectEntry> at(ObjectTopicId id, size_t index) const;
  // Resolve by stable SequentialUID. Returns nullopt when the entry was evicted
  // or the UID is invalid / from another topic generation.
  std::optional<ResolvedObjectEntry> at(ObjectTopicId id, SequentialUID sequential_uid) const;

  std::optional<size_t> indexAt(ObjectTopicId id, Timestamp timestamp) const;
  // First retained entry UID for this topic, or invalid when the topic is empty
  // or unknown. Useful for detecting retention gaps in replay cursors.
  SequentialUID firstSequentialUID(ObjectTopicId id) const;
  // Streaming-ingest cursor: returns every entry with sequential_uid > `cursor`, in
  // ASCENDING UID (arrival) order with each payload resolved, then advances `cursor`
  // to the last one seen. This is the only arrival-order read primitive — for a
  // consumer that must ingest each new entry exactly once into an order-independent
  // sink (e.g. the TF buffer), catching late/out-of-order arrivals a time window
  // would miss. Eviction-safe: the batch is snapshotted under ONE brief pass of the
  // locks, so retention cannot drop an entry out from under the walk; the payloads
  // are then resolved with NO lock held, so a slow lazy fetch stalls neither this
  // series' writers nor readers of unrelated topics.
  // For "state at time t" use rangeByTime()/latestAt() — never a UID as a time bound.
  std::vector<ResolvedObjectEntry> drainNewSince(ObjectTopicId id, SequentialUID& cursor) const;

  // The largest sequential_uid among entries with timestamp <= t, or the invalid
  // UID when none exists. Unlike `latestAt(t)->sequential_uid` — the newest-
  // TIMESTAMP entry, whose UID an out-of-order insert (old ts, newest UID) can
  // leave below a retained entry's — this is the true high-water arrival UID of
  // everything at-or-before t. A time-window replay cursor advances to it so a
  // late out-of-order entry is not re-detected every step. O(count with ts <= t).
  SequentialUID maxUidAtOrBefore(ObjectTopicId id, Timestamp t) const;

  // One entry's stable identity plus its store timestamp, as returned by
  // rangeByTime() and latestEntryIdAt(). Decode-free — resolve the payload later
  // via at(uid). Owned by OrderedEntries; aliased here for the public API.
  using TimeRangeEntry = OrderedEntries::TimeRangeEntry;

  // Identity of the entry latestAt(id, timestamp) would resolve — its
  // {sequential_uid, timestamp} — WITHOUT resolving the payload. Metadata-only
  // (shared locks + binary search); never invokes a lazy fetch, so a sampling
  // consumer can compare it against a memoized sample identity before paying
  // latestAt()'s resolve (a cold refetch once a pool seed is evicted). Not the
  // same as maxUidAtOrBefore(): under out-of-order inserts the latest-by-time
  // entry's UID can differ from the max arrival UID at-or-before t. nullopt when
  // no entry exists at or before `timestamp`.
  //
  // NOT atomic with a later resolve: between this call and a subsequent
  // latestAt()/at(), a concurrent insert or eviction can change which entry is
  // latest at `timestamp`. A consumer that skips work when this identity matches
  // a memo MUST re-check the resolved entry's sequential_uid after the resolve,
  // or it may act on a different entry than the one it gated on.
  std::optional<TimeRangeEntry> latestEntryIdAt(ObjectTopicId id, Timestamp timestamp) const;

  // Snapshot of the entries with lo < timestamp <= hi, as (uid, timestamp) pairs
  // in ASCENDING timestamp order. This is the correct primitive for a time-window
  // consumer (e.g. incremental occupancy-grid updates): it must NOT walk the
  // arrival-order UID cursor, because an out-of-order entry (older timestamp,
  // newest UID) can sit at any UID position. rangeByTime itself resolves no
  // payload under the store lock; the caller resolves each entry afterwards with
  // at(uid) — one at a time under at()'s own brief lock — getting nullopt for any
  // evicted in between. Empty when hi <= lo, or the topic is unknown/empty, or the
  // window holds nothing. O(log n + window).
  std::vector<TimeRangeEntry> rangeByTime(ObjectTopicId id, Timestamp lo, Timestamp hi) const;

  size_t entryCount(ObjectTopicId id) const;

  std::pair<Timestamp, Timestamp> timeRange(ObjectTopicId id) const;

  EntryTimestampsView entryTimestamps(ObjectTopicId id) const;

  // --- Retention ---

  void setRetentionBudget(ObjectTopicId id, RetentionBudget budget);
  RetentionBudget retentionBudget(ObjectTopicId id) const;
  size_t memoryUsage(ObjectTopicId id) const;

  // Pool consulted by pushLazyWithSeed. Set once during wiring, BEFORE any
  // pushes (plain member, not synchronized against concurrent pushes). Share
  // one pool app-wide so every store's seeds compete for a single byte budget;
  // seeded entries moved between stores (flushTo / replaceDatasetFrom) keep
  // their slots regardless of the destination's pool. Null (default) disables
  // seeding. Seeded bytes are pool-accounted and NOT included in memoryUsage().
  void setResidentPayloadPool(std::shared_ptr<ResidentPayloadPool> pool);

  // --- Explicit eviction ---

  void evictBefore(ObjectTopicId id, Timestamp threshold);
  void evictAllBefore(Timestamp threshold);

  // --- Cross-store flush ---

  // Move every entry into `dst`, leaving this store empty (registrations kept).
  // Topics are matched by descriptor (dataset_id + topic_name); both stores
  // must share descriptors. Monotonicity is enforced per series: the earliest
  // moved timestamp must be >= the destination's last. Any validation failure
  // returns an error and mutates neither store.
  //
  // Zero-copy: each ObjectEntry is moved by value, so the variant's shared_ptr
  // or closure transfers as a pointer move — bytes are never copied. Lazy
  // entries keep their semantics; their closure re-runs only on a dst read.
  // Afterward, dst's retention budget is applied to each touched series.
  Status flushTo(ObjectStore& dst);

  // In-place REPLACE of object dataset `primary_id` with the topics ingested
  // into `staged` dataset `staged_id` — the object-store half of reload. Topics
  // are matched by name. Matched topics keep their primary ObjectTopicId and have
  // their entries replaced; staged-only topics are registered under `primary_id`;
  // primary-only topics are removed. Returns the staged->primary id map so the
  // caller re-registers parsers under the stable ids. Either fully applies or
  // (on a validation error) mutates neither store.
  [[nodiscard]] Expected<ObjectDatasetReplaceResult> replaceDatasetFrom(
      ObjectStore& staged, DatasetId staged_id, DatasetId primary_id);

  /// Destructively fold each source dataset's object topics into `anchor_id`.
  /// Topics match by name: shared names are time-shifted by `raw_shift_ns`,
  /// interleaved in ascending timestamp order, and re-UID'd under the anchor's
  /// ObjectTopicId; source-only topics are reparented to the anchor while keeping
  /// their ObjectTopicId. Purely mechanical: canonical-type conflict detection is
  /// handled by pj_runtime. Validates self/duplicate sources up front; a source
  /// with no object topics is a no-op. GUI-thread only.
  ///
  /// `exclude` (optional) opts a topic out of the generic fold entirely — it is
  /// neither folded nor reparented, left untouched on its dataset. The store stays
  /// domain-neutral: the caller supplies the policy. Used for single-entry,
  /// supersede-style topics (plot markers) whose owner merges them set-aware,
  /// where the generic interleave+retention fold would drop all but one entry.
  [[nodiscard]] Expected<ObjectDatasetMergeReport> mergeDatasets(
      DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources,
      const std::function<bool(const ObjectTopicDescriptor&)>& exclude = {});

  // --- Lifecycle ---

  void removeTopic(ObjectTopicId id);

  /// Remove all entries from every series under `dataset_id`, keeping each
  /// topic registered with the SAME ObjectTopicId (clear-in-place — never
  /// removeTopic+re-register, which would risk ObjectTopicId drift). Other
  /// datasets are untouched. Idempotent; a no-op for an unknown `dataset_id`.
  void clearDataset(DatasetId dataset_id);

  /// Side snapshot of one dataset's object series, produced by `detachDataset()`.
  /// Holds the MOVED-OUT entries + timestamps + retention budget + memory
  /// accounting (keyed by ObjectTopicId.id); series stay registered + empty.
  /// `prior_object_topic_ids` is the registered set at detach time so reattach
  /// can remove any topic a failed refill added. The uid_order side index is NOT
  /// snapshotted (it is derivable — reattach rebuilds it). The warm latestAt cache
  /// is NOT snapshotted (reset on detach, re-seeded lazily after reattach).
  /// `valid == false` means nothing was detached.
  struct ObjectDatasetSnapshot {
    struct SeriesSnapshot {
      std::deque<ObjectEntry> entries;
      std::vector<Timestamp> entry_timestamps;
      RetentionBudget budget;
      size_t memory_bytes = 0;
    };
    DatasetId dataset_id = 0;
    std::vector<ObjectTopicId> prior_object_topic_ids;
    std::unordered_map<uint32_t, SeriesSnapshot> series;  // keyed by ObjectTopicId.id
    bool valid = false;
  };

  /// Transactional variant of `clearDataset()`: instead of dropping each series'
  /// entries, MOVE them (plus timestamps/budget/memory) into the returned snapshot;
  /// series stay registered + empty. Same drain-then-mutate discipline as
  /// `clearDataset` (store_mutex_ exclusive, `drainSeriesReaders` before touching a
  /// series). GUI-thread only. Unknown dataset -> `valid == false`.
  [[nodiscard]] ObjectDatasetSnapshot detachDataset(DatasetId dataset_id);

  /// Inverse of `detachDataset()`. For each series currently registered under
  /// `dataset_id`: one NOT in `snapshot.prior_object_topic_ids` was registered by a
  /// failed refill and is removed; a prior series is cleared. Then each snapshot
  /// series' entries/budget/memory are moved back into its (still-registered) series
  /// and the warm cache reset. store_mutex_ exclusive; consumes the snapshot. No-op
  /// when `!snapshot.valid`. Tolerates an already-clean state (added topics already
  /// removed by the caller).
  void reattachDataset(DatasetId dataset_id, ObjectDatasetSnapshot&& snapshot);

  void clear();

 private:
  struct ObjectSeries {
    ObjectTopicDescriptor descriptor;
    // The ordered-entry triple (entries + ascending timestamps + uid_order),
    // encapsulated so its ordering invariants are enforced by construction and no
    // mutation site can desync the three arrays by hand.
    OrderedEntries ordered;
    RetentionBudget budget;
    size_t memory_bytes = 0;
    mutable std::shared_mutex mutex;

    // Warm cache for latestAt: the most-recently-resolved entry, keyed by its
    // sequential_uid. A ~60 Hz reader that keeps landing on the same sample
    // (a topic publishing slower than the render rate) is served from here
    // instead of re-invoking a lazy fetcher that may decompress/read a file.
    // Holds at most ONE materialized payload per topic — reset when that entry
    // is evicted or the series is replaced. `cache_mutex` is separate from
    // `mutex` (which latestAt holds only shared) and is never held across a
    // resolve, so a slow decode can't block other readers of this series.
    mutable std::mutex cache_mutex;
    mutable std::optional<ResolvedObjectEntry> cached_latest;
  };

  ObjectSeries* findSeries(ObjectTopicId id);
  const ObjectSeries* findSeries(ObjectTopicId id) const;

  // Shared body of pushLazy / pushLazyWithSeed once the payload variant is
  // decided (plain closure vs slot + fallback).
  Status pushLazyEntry(ObjectTopicId id, Timestamp timestamp, ObjectEntryPayload payload);

  // Erase a topic from topics_. Caller must already hold store_mutex_ (used by
  // removeTopic under its own lock and by replaceDatasetFrom under the dual lock).
  void eraseTopicLocked(ObjectTopicId id);

  // Wait out any in-flight shared readers (e.g. an EntryTimestampsView, which
  // holds only the series lock — not store_mutex_) before `series` storage is
  // destroyed or its timestamp vector reallocated. Caller MUST hold store_mutex_
  // exclusively, so no new reader can find the series; taking the series lock
  // exclusively then blocks until the outstanding readers release. Without this,
  // destroying a still-locked series mutex is UB and the view's pointer dangles.
  static void drainSeriesReaders(ObjectSeries& series);

  // Empty one series' entries/timestamps/uid_order (via OrderedEntries::clear) and
  // reset its memory accounting + warm cache. Caller MUST hold store_mutex_
  // exclusively AND must have already drained the series' readers
  // (drainSeriesReaders) so no EntryTimestampsView dangles into the timestamp
  // vector being cleared.
  static void clearEntriesLocked(ObjectSeries& series);

  // Resolve an entry SNAPSHOT (a cheap copy of the payload variant — refcount
  // bumps only) taken under the series lock; called with that lock RELEASED so
  // a slow lazy fetch never blocks same-series writers. `served_from_resident`
  // (optional) reports a seeded-slot hit, letting latestAt skip warm-cache
  // population — a resident hit is already cheap and caching it would create a
  // second anchor owner outside the pool's budget.
  static ResolvedObjectEntry resolveEntry(const ObjectEntry& entry, bool* served_from_resident = nullptr);

  // Drop the oldest entry: the warm cache (if it holds it) + owned-payload memory
  // accounting live here; the triple pop delegates to OrderedEntries::evictFront.
  void evictFront(ObjectSeries& series);
  void applyRetention(ObjectSeries& series, Timestamp newest_ts);

  // See setResidentPayloadPool. Read without synchronization on the push path;
  // wiring must complete before ingest starts.
  std::shared_ptr<ResidentPayloadPool> resident_pool_;

  mutable std::shared_mutex store_mutex_;
  // topics_ is the ordered source of truth: listTopics/replaceDatasetFrom/flushTo
  // iterate it in registration order. series_index_ is an acceleration map from
  // topic id to ObjectSeries*. ObjectSeries are heap-owned by unique_ptr so
  // their addresses are stable across topics_ reallocs. Must only be touched
  // under store_mutex_ held exclusively; kept in exact sync with topics_.
  std::vector<std::pair<ObjectTopicId, std::unique_ptr<ObjectSeries>>> topics_;
  std::unordered_map<uint32_t, ObjectSeries*> series_index_;
  uint32_t next_id_ = 1;
};

}  // namespace PJ
