#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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
#include "pj_datastore/log_entries.hpp"
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

/// `topic_name` is unique within its dataset. The store keeps `metadata_json` verbatim and never reads it, except
/// that registering a legacy `__markers__/<key>` name stamps marker metadata (`stampLegacyMarkerMetadata`).
struct ObjectTopicDescriptor {
  DatasetId dataset_id = 0;
  std::string topic_name;
  std::string metadata_json;
};

/// One topic's part of ObjectStore::pushOwnedBundle().
struct OwnedObjectWrite {
  ObjectTopicId topic;
  Timestamp timestamp = 0;
  std::vector<uint8_t> payload;
  /// Selects existing entries to remove before the append. Runs under the store lock, so calling the ObjectStore
  /// from it deadlocks; an exception rejects the bundle.
  EntryPredicate drop;
  /// False only prunes: `drop` runs, nothing is appended, and the surviving entries keep their UIDs.
  bool append = true;
};

/// Outcome of ObjectStore::replaceDatasetFrom().
struct ObjectDatasetReplaceResult {
  /// (staged id, primary id) for every adopted topic, matched or new: re-register a parser keyed by the staged id
  /// under the primary id.
  std::vector<std::pair<ObjectTopicId, ObjectTopicId>> remapped;
  /// Primary topics with no staged match, removed.
  std::vector<ObjectTopicId> removed_topics;
};

/// Outcome of ObjectStore::mergeDatasets().
struct ObjectDatasetMergeReport {
  /// (source ObjectTopicId, anchor ObjectTopicId) for every shared-name fold.
  std::vector<std::pair<ObjectTopicId, ObjectTopicId>> remapped;
  /// Source-only topics reparented under the anchor dataset (they keep their ObjectTopicId).
  std::vector<ObjectTopicId> added_topics;
  /// Source datasets whose object topics were emptied; the datasets stay registered.
  std::vector<DatasetId> consumed_datasets;
};

struct ResolvedObjectEntry {
  Timestamp timestamp = 0;
  SequentialUID sequential_uid;
  /// Add to timestamps embedded in `payload` to put them on the store clock after a time-shifted merge (see
  /// ObjectEntry).
  Timestamp payload_stamp_shift = 0;
  /// `bytes` stay valid while `anchor` is held, whatever the store does afterwards.
  sdk::PayloadView payload;
  /// The lazy fetch failed (source truncated, replaced or corrupt) and `payload` is empty; not the same as an empty
  /// payload. The loss is permanent, so cursors count it and move past the entry.
  bool fetch_failed = false;
};

/// Per-topic retention, applied after every insert. Each limit is off at 0.
struct RetentionBudget {
  int64_t time_window_ns = 0;
  size_t max_memory_bytes = 0;
  /// Keep at most this many newest entries.
  size_t max_entries = 0;
};

/// Holds the series read lock while alive: keep it short, writers wait. Calling the ObjectStore while holding one
/// can deadlock (store methods lock the store, then the series; the view inverts that order). For several topics,
/// use the list overload of ObjectStore::entryTimestamps().
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

/// A kLog series' columns, read in place (ObjectStore::logView()). Holds the series read lock while alive, with
/// EntryTimestampsView's rules: keep it short, and never call the ObjectStore while holding it (deadlock).
class LogSeriesView {
 public:
  LogSeriesView(std::shared_lock<std::shared_mutex> lock, const LogEntries* entries)
      : lock_(std::move(lock)), entries_(entries) {}

  const LogEntries& operator*() const {
    return *entries_;
  }
  const LogEntries* operator->() const {
    return entries_;
  }

 private:
  std::shared_lock<std::shared_mutex> lock_;
  const LogEntries* entries_ = nullptr;
};

/// Timestamped opaque payloads (images, point clouds, annotations) selected by time and never expanded into columns.
/// A canonical kLog topic (isCanonicalLogMetadata()) is stored as LogEntries instead: its entries are decoded on push
/// (owned pushes only; a lazy push is rejected) and generic reads return re-serialized canonical bytes with
/// payload_stamp_shift 0, the own stamp already on the store clock.
/// Thread-safe: a store-wide shared mutex, then one per topic. Locking and lifetime rules: docs/ARCHITECTURE.md,
/// "ObjectStore".
class ObjectStore {
 public:
  ObjectStore() = default;
  ~ObjectStore() = default;

  ObjectStore(const ObjectStore&) = delete;
  ObjectStore& operator=(const ObjectStore&) = delete;
  ObjectStore(ObjectStore&&) = delete;
  ObjectStore& operator=(ObjectStore&&) = delete;

  // --- Registration ---

  /// A non-zero `requested_id` forces that id (keeps two stores' ids in lockstep); otherwise it is auto-assigned.
  Expected<ObjectTopicId> registerTopic(ObjectTopicDescriptor descriptor, ObjectTopicId requested_id = {});

  /// Reuses an existing (dataset, name) topic whose metadata_json is byte-identical, so its id and the curves bound
  /// to it survive a refill; different metadata is an error, as in registerTopic(). Atomic: racing callers get the
  /// same topic.
  Expected<ObjectTopicId> findOrRegisterTopic(ObjectTopicDescriptor descriptor);

  std::optional<ObjectTopicId> findTopic(DatasetId dataset_id, std::string_view topic_name) const;
  /// O(1) check that a remembered id still names this (dataset, topic): false after removal, a merge's reparenting,
  /// or clear() reissuing the id.
  [[nodiscard]] bool isTopic(ObjectTopicId id, DatasetId dataset_id, std::string_view topic_name) const;

  /// By value: a reference would dangle once removeTopic/clear/replaceDatasetFrom destroys the series.
  ObjectTopicDescriptor descriptor(ObjectTopicId id) const;

  std::vector<ObjectTopicId> listTopics() const;
  std::vector<ObjectTopicId> listTopics(DatasetId dataset_id) const;
  /// Visits the topics of `dataset_id` in registration order under the store's shared lock: `visit` must be cheap
  /// and must not call the store (deadlock).
  void forEachTopic(
      DatasetId dataset_id, const std::function<void(ObjectTopicId, const ObjectTopicDescriptor&)>& visit) const;
  /// Entry count and [first, last] timestamp, read under one series lock (zeros when empty).
  struct RetainedRange {
    size_t entry_count = 0;
    Timestamp first = 0;
    Timestamp last = 0;
  };
  /// forEachTopic() over every dataset, with each topic's RetainedRange; same locking rules.
  void forEachTopicRetained(
      const std::function<void(ObjectTopicId, const ObjectTopicDescriptor&, const RetainedRange&)>& visit) const;

  // --- Write ---

  Status pushOwned(ObjectTopicId id, Timestamp timestamp, std::vector<uint8_t> payload);

  /// Publishes one entry to each of several distinct topics as one transaction, under the exclusive store lock.
  /// Phase one validates every write and runs every `drop`: an unknown or repeated topic, or an exception from a
  /// `drop` or an allocation, returns an error with nothing changed. Phase two applies each topic whole; an
  /// allocation failure there throws (as in pushOwned) and can leave some topics applied. Appends go through each
  /// topic's retention budget. Returns the UID each write created, in write order (invalid for a prune-only write).
  ///
  /// One read call never sees half a bundle; separate calls on several topics may straddle two publications.
  [[nodiscard]] Expected<std::vector<SequentialUID>> pushOwnedBundle(std::vector<OwnedObjectWrite> writes);

  /// `fetch` runs on every read except latestAt()'s warm-cache hits; its view is returned uncopied.
  /// `resident_bytes`: what the closure itself pins, charged to the topic's memory budget (0 when it re-reads on
  /// demand).
  Status pushLazy(ObjectTopicId id, Timestamp timestamp, LazyCallback fetch, size_t resident_bytes = 0);

  /// pushLazy() plus `seed`, bytes the host already holds: reads serve them without a fetch until the
  /// ResidentPayloadPool evicts them, then the entry behaves as pushLazy(). Plain pushLazy() when there is no pool,
  /// the seed is empty or the pool rejects it. `fetch` must reproduce `seed` byte for byte.
  Status pushLazyWithSeed(ObjectTopicId id, Timestamp timestamp, sdk::PayloadView seed, LazyCallback fetch);

  // --- Read ---

  std::optional<ResolvedObjectEntry> latestAt(ObjectTopicId id, Timestamp timestamp) const;

  std::optional<ResolvedObjectEntry> at(ObjectTopicId id, size_t index) const;
  /// nullopt once evicted, or for a UID of another topic or generation.
  std::optional<ResolvedObjectEntry> at(ObjectTopicId id, SequentialUID sequential_uid) const;

  std::optional<size_t> indexAt(ObjectTopicId id, Timestamp timestamp) const;
  /// Timestamp-order position of `uid`, unambiguous under equal stamps. Decode-free.
  [[nodiscard]] std::optional<size_t> indexOf(ObjectTopicId id, SequentialUID uid) const;
  /// See OrderedEntries::ContentIdentity; zero for an unknown topic. Decode-free.
  [[nodiscard]] OrderedEntries::ContentIdentity contentIdentity(ObjectTopicId id) const;
  /// Smallest retained UID (not necessarily the front entry's): a cursor below it missed evicted or replaced
  /// entries. Invalid when the topic is empty or unknown.
  SequentialUID firstSequentialUID(ObjectTopicId id) const;
  /// Every entry with UID > `cursor`, in arrival order with payloads resolved; advances `cursor`. The only
  /// arrival-order read: for sinks that must see each entry once, late arrivals included (the TF buffer). Snapshots
  /// under one brief lock pass and resolves with no lock held. For the state at a time use rangeByTime() or
  /// latestAt(); a UID is not a time bound. A sink that keeps what it drained reads contentIdentity() before draining
  /// (read after, a removal in between goes unseen) and rescans when its `removal` moved. A front eviction moves
  /// neither `removal` nor any other signal a sink can rely on (a later insert may lower timeRange().first again), so a
  /// sink that must drop what retention evicted checks its held entries with indexOf().
  std::vector<ResolvedObjectEntry> drainNewSince(ObjectTopicId id, SequentialUID& cursor) const;

  /// Largest UID among entries with timestamp <= t (invalid if none). Differs from latestAt(t)'s UID after an
  /// out-of-order insert; a time-window cursor advances to this one, or it re-detects a late entry every step.
  /// O(entries with timestamp <= t).
  SequentialUID maxUidAtOrBefore(ObjectTopicId id, Timestamp t) const;
  /// Oldest timestamp among entries with UID > `after` (nullopt if none): how a consumer that evaluated up to some
  /// time finds out-of-order or same-stamp arrivals behind it. Decode-free, O(log n + those entries).
  std::optional<Timestamp> oldestArrivalAfter(ObjectTopicId id, SequentialUID after) const;

  using TimeRangeEntry = OrderedEntries::TimeRangeEntry;

  /// Identity of the entry latestAt() would resolve, without fetching its payload. Not atomic with a later resolve:
  /// a consumer that skips work when this matches a memo must re-check the resolved entry's UID.
  std::optional<TimeRangeEntry> latestEntryIdAt(ObjectTopicId id, Timestamp timestamp) const;

  /// Entries with lo < timestamp <= hi, in timestamp order and unresolved; resolve each with at(uid), which returns
  /// nullopt for one evicted since. Use this, not the UID cursor, for a time window: an out-of-order entry can sit at
  /// any UID position. O(log n + window).
  std::vector<TimeRangeEntry> rangeByTime(ObjectTopicId id, Timestamp lo, Timestamp hi) const;

  size_t entryCount(ObjectTopicId id) const;

  std::pair<Timestamp, Timestamp> timeRange(ObjectTopicId id) const;

  EntryTimestampsView entryTimestamps(ObjectTopicId id) const;

  /// Mutually consistent views of several topics, locked in ascending id order (the order every caller locks series
  /// in); unknown topics are skipped.
  std::vector<EntryTimestampsView> entryTimestamps(std::vector<ObjectTopicId> ids) const;

  /// The columns of a kLog series; nullopt for any other topic.
  [[nodiscard]] std::optional<LogSeriesView> logView(ObjectTopicId id) const;

  // --- Retention ---

  void setRetentionBudget(ObjectTopicId id, RetentionBudget budget);
  RetentionBudget retentionBudget(ObjectTopicId id) const;
  size_t memoryUsage(ObjectTopicId id) const;

  /// Set once, before any push (unsynchronized). Null disables seeding. Seeded bytes count against the pool, not
  /// memoryUsage(); entries moved to another store keep their slots.
  void setResidentPayloadPool(std::shared_ptr<ResidentPayloadPool> pool);

  // --- Explicit eviction ---

  void evictBefore(ObjectTopicId id, Timestamp threshold);
  void evictAllBefore(Timestamp threshold);

  // --- Cross-store flush ---

  /// Moves every entry into `dst` (registrations stay), matching topics by (dataset, name); a validation failure
  /// changes neither store. Entries older than dst's newest merge into timestamp order, as an out-of-order push does;
  /// dst's entries keep their UIDs and the moved ones get fresh, newer ones. Zero-copy; dst's retention applies
  /// afterwards.
  Status flushTo(ObjectStore& dst);

  /// Replaces object dataset `primary_id` with what was ingested into `staged`'s `staged_id` (the reload path),
  /// matching topics by name: matched topics keep their primary id, staged-only ones are registered, primary-only ones
  /// removed. A validation error changes neither store.
  [[nodiscard]] Expected<ObjectDatasetReplaceResult> replaceDatasetFrom(
      ObjectStore& staged, DatasetId staged_id, DatasetId primary_id);

  /// Folds each source dataset into `anchor_id`: shared topic names are shifted by `raw_shift_ns`, interleaved by
  /// timestamp and re-UID'd under the anchor's id; source-only topics are reparented and keep their id. Mechanical:
  /// canonical-type conflicts are pj_runtime's job. Self or duplicate sources are rejected first. GUI thread only.
  ///
  /// `exclude` leaves a topic untouched on its dataset; the caller supplies the policy (single-entry topics such as
  /// plot markers, which their owner merges itself).
  [[nodiscard]] Expected<ObjectDatasetMergeReport> mergeDatasets(
      DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources,
      const std::function<bool(const ObjectTopicDescriptor&)>& exclude = {});

  // --- Lifecycle ---

  void removeTopic(ObjectTopicId id);

  /// Empties every topic of `dataset_id` in place, keeping their ids (re-registering would mint new ones).
  /// Idempotent.
  void clearDataset(DatasetId dataset_id);

  /// The entries detachDataset() moved out, per topic. `prior_object_topic_ids` lets reattachDataset() remove topics
  /// a failed refill added. `valid == false`: nothing was detached.
  struct ObjectDatasetSnapshot {
    struct SeriesSnapshot {
      std::deque<ObjectEntry> entries;
      std::vector<Timestamp> entry_timestamps;
      /// A log series' rows (then `entries` is empty).
      std::unique_ptr<LogEntries> log;
      RetentionBudget budget;
      size_t memory_bytes = 0;
    };
    DatasetId dataset_id = 0;
    std::vector<ObjectTopicId> prior_object_topic_ids;
    std::unordered_map<uint32_t, SeriesSnapshot> series;  // keyed by ObjectTopicId.id
    bool valid = false;
  };

  /// clearDataset() that moves the entries into the returned snapshot instead of dropping them. GUI thread only.
  [[nodiscard]] ObjectDatasetSnapshot detachDataset(DatasetId dataset_id);

  /// Undoes detachDataset(): removes topics registered since, empties the prior ones and moves the snapshot back.
  /// No-op for an invalid snapshot.
  void reattachDataset(DatasetId dataset_id, ObjectDatasetSnapshot&& snapshot);

  void clear();

 private:
  struct ObjectSeries {
    ObjectTopicDescriptor descriptor;
    OrderedEntries ordered;  ///< empty when `log` is set
    /// Set for a canonical kLog topic: its entries live here instead of `ordered`. Chosen from the metadata at
    /// registration (and again when a reload replaces the metadata).
    std::unique_ptr<LogEntries> log;
    RetentionBudget budget;
    size_t memory_bytes = 0;
    mutable std::shared_mutex mutex;

    // latestAt()'s one-entry warm cache. cache_mutex is never held across a resolve, so a slow fetch blocks no
    // reader of this series.
    mutable std::mutex cache_mutex;
    mutable std::optional<ResolvedObjectEntry> cached_latest;
  };

  // What a mutation unlinks under the locks. Destroying an entry can run plugin code (fetch_ctx_destroy, an anchor
  // release, a library unload) that calls back into the store, so every mutator declares one before its locks and
  // it is destroyed after they release.
  struct Graveyard {
    std::vector<ObjectEntry> entries;
    std::vector<ResolvedObjectEntry> cached;
    std::vector<std::unique_ptr<ObjectSeries>> series;
  };

  ObjectSeries* findSeries(ObjectTopicId id);
  const ObjectSeries* findSeries(ObjectTopicId id) const;

  Status pushLazyEntry(ObjectTopicId id, Timestamp timestamp, ObjectEntryPayload payload);

  // Caller holds store_mutex_ exclusively.
  void eraseTopicLocked(ObjectTopicId id, Graveyard& graveyard);

  // Waits out shared readers (an EntryTimestampsView holds only the series lock) before the series is destroyed or
  // its timestamps reallocated: destroying a locked mutex is UB and the view's pointer would dangle. Caller holds
  // store_mutex_ exclusively, so no new reader can find the series.
  static void drainSeriesReaders(ObjectSeries& series);

  // Caller holds store_mutex_ exclusively and has drained the series' readers.
  static void clearEntriesLocked(ObjectSeries& series, Graveyard& graveyard);

  static void dropCache(const ObjectSeries& series, Graveyard& graveyard);

  // Takes an entry copied under the series lock and runs with that lock released, so a slow fetch never blocks
  // writers. `served_from_resident` reports a pool hit, which latestAt does not cache: that would hold a second
  // anchor outside the pool's budget.
  static ResolvedObjectEntry resolveEntry(const ObjectEntry& entry, bool* served_from_resident = nullptr);

  // Mints the UID and inserts `entry` (applying `removal` first), charges its bytes and applies retention. The caller
  // holds the series exclusively: OrderedEntries::push needs the new UID to be the series maximum. If insertion
  // throws, `entry` is still the caller's, so declare it before the locks (a lazy entry's cleanup may call the store).
  SequentialUID insertLocked(
      ObjectSeries& series, ObjectEntry&& entry, Graveyard& graveyard,
      const OrderedEntries::Removal* removal = nullptr);
  // insertLocked() for a log series.
  SequentialUID insertLogLocked(
      ObjectSeries& series, Timestamp timestamp, const sdk::Log& log, Graveyard& graveyard,
      const OrderedEntries::Removal* removal = nullptr);
  // After a log series lost rows: recharges its bytes and drops the warm cache if it named one of @p removed.
  static void accountLogRemoved(ObjectSeries& series, const std::vector<SequentialUID>& removed, Graveyard& graveyard);
  static ResolvedObjectEntry resolveLog(const LogEntries& log, size_t index);
  static std::unique_ptr<ObjectSeries> makeSeries(ObjectTopicDescriptor descriptor);
  // Removes the bytes of graveyard.entries[first..] from the series total and drops the warm cache if it holds one
  // of them. Allocation-free when graveyard.cached has room for one more.
  static void accountRemoved(ObjectSeries& series, Graveyard& graveyard, size_t first);
  // Drops the oldest `count` entries in one pass. Reserves graveyard room first, so an allocation failure leaves the
  // series whole.
  void evictFront(ObjectSeries& series, size_t count, Graveyard& graveyard);
  void applyRetention(ObjectSeries& series, Timestamp newest_ts, Graveyard& graveyard);

  // Unsynchronized: see setResidentPayloadPool().
  std::shared_ptr<ResidentPayloadPool> resident_pool_;

  mutable std::shared_mutex store_mutex_;
  // topics_ holds registration order; series_index_ maps ids to the same series. Heap-owned series keep stable
  // addresses. Write both only under store_mutex_ held exclusively.
  std::vector<std::pair<ObjectTopicId, std::unique_ptr<ObjectSeries>>> topics_;
  std::unordered_map<uint32_t, ObjectSeries*> series_index_;
  uint32_t next_id_ = 1;
};

}  // namespace PJ
