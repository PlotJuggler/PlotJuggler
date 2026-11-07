#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/replace_result.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/type_registry.hpp"

namespace PJ {

class DataWriter;
class DataReader;

/// Central owner of datasets, topics, schemas, and committed chunks.
class DataEngine {
 public:
  /// Construct an empty engine instance.
  DataEngine();

  /// Destructor (defined in .cpp for pimpl).
  ~DataEngine();

  /// Move constructor.
  DataEngine(DataEngine&&) noexcept;

  /// Move assignment.
  DataEngine& operator=(DataEngine&&) noexcept;

  /// Deleted copy constructor.
  DataEngine(const DataEngine&) = delete;

  /// Deleted copy assignment.
  DataEngine& operator=(const DataEngine&) = delete;

  // Dataset management
  /// Create and register a dataset. Set `requested_id` non-zero to create it
  /// with that exact DatasetId; otherwise the next id is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::DatasetId> createDataset(
      PJ::DatasetDescriptor descriptor, PJ::DatasetId requested_id = 0);

  /// Lookup dataset by id (nullptr if missing).
  [[nodiscard]] const PJ::DatasetInfo* getDataset(PJ::DatasetId id) const;

  /// REAL delete of a dataset: erase its `DatasetInfo` and every one of its topics
  /// (freeing all chunks/columns) so `listDatasets`/`getDataset`/`getTopicStorage`/
  /// `listTopics` all drop them — distinct from `retireTopic`'s hide-but-keep. Ids
  /// strictly increment, so the erased id is never reused; a later `createDataset`
  /// mints a fresh one (a reload after removal is a clean fresh load, not a reattach
  /// to an emptied shell). Time domains are left intact (a domain may be shared).
  ///
  /// PRECONDITION (same as `replaceDatasetFrom`): the caller MUST have invalidated
  /// every reader/adapter bound to this dataset BEFORE calling — erasing the
  /// `TopicStorage` frees the memory a cached reader/`TopicChunk*` would dereference.
  /// Idempotent; a no-op for an unknown id.
  void removeDataset(PJ::DatasetId id);

  // Topic management (called by DataWriter)
  /// Create a topic under a dataset. Set `requested_id` non-zero to create it
  /// with that exact TopicId; otherwise the next id is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::TopicId> createTopic(
      PJ::DatasetId dataset_id, TopicDescriptor descriptor, PJ::TopicId requested_id = 0);

  /// One column of `createColumnGroupTopic`.
  struct GroupColumn {
    std::string name;
    PJ::PrimitiveType type = PJ::PrimitiveType::kFloat64;
  };

  /// Creates ONE topic `topic_name` whose schema `schema_name` is a struct with one primitive field per entry of
  /// `columns` (field k is column k): an on-demand series' topic. Registers the schema, or reuses the one registered
  /// under that name. [thread-safe]
  [[nodiscard]] PJ::Expected<PJ::TopicId> createColumnGroupTopic(
      PJ::DatasetId dataset_id, const std::string& topic_name, const std::string& schema_name,
      const std::vector<GroupColumn>& columns);

  /// Symmetric counterpart to `createTopic(requested_id)` at the field/column
  /// level. Adds a column to an existing topic; pass `requested_id` non-empty
  /// to force a specific `FieldId` (used by the streaming pause/resume
  /// two-engine lockstep — both engines must assign matching FieldIds for
  /// the same (topic, field_name) pair so a plugin's cached `FieldHandle`
  /// resolves on either side of the pause/resume target swap). Default
  /// `std::nullopt` means auto-assign the next dense id.
  ///
  /// `FieldId` is dense from 0, so a sentinel like `0` would be ambiguous
  /// with a legitimate forced-id-zero — `std::optional` makes the intent
  /// explicit and unambiguous.
  ///
  /// Returns the assigned `FieldId`. Fails if:
  /// - the topic does not exist
  /// - a column with the same name already exists with a different type
  /// - `requested_id` is set and clashes with an existing different
  ///   (name, type) pair, or would create a non-dense column id
  ///
  /// If a column with the same (name, type) already exists, returns its
  /// existing `FieldId` (idempotent re-mirror).
  [[nodiscard]] PJ::Expected<PJ::FieldId> createTopicField(
      PJ::TopicId topic_id, std::string_view field_name, PJ::PrimitiveType type,
      std::optional<PJ::FieldId> requested_id = std::nullopt);

  /// Mutable topic storage lookup (nullptr if missing).
  [[nodiscard]] TopicStorage* getTopicStorage(PJ::TopicId id);

  /// Const topic storage lookup (nullptr if missing).
  [[nodiscard]] const TopicStorage* getTopicStorage(PJ::TopicId id) const;

  // Schema registry access. The returned reference is UNSYNCHRONIZED — the
  // TypeRegistry has no internal lock, so the caller must hold lockEngine() for
  // the lifetime of any access (lookup/registerOrGet/registerSchema), and must
  // not cache a raw TypeTreeNode* from lookup() past the lock.
  /// Mutable schema registry access (see threading note above).
  [[nodiscard]] TypeRegistry& typeRegistry();

  /// Const schema registry access (see threading note above).
  [[nodiscard]] const TypeRegistry& typeRegistry() const;

  // Time domains
  /// Create a new time domain. Set `requested_id` non-zero to create it with
  /// that exact TimeDomainId; otherwise the next id is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::TimeDomainId> createTimeDomain(std::string name, PJ::TimeDomainId requested_id = 0);

  /// Lookup time domain by id (nullptr if missing).
  [[nodiscard]] const PJ::TimeDomain* getTimeDomain(PJ::TimeDomainId id) const;

  /// Update display offset for one time domain.
  void setDisplayOffset(PJ::TimeDomainId id, PJ::Timestamp offset);

  // Commit cycle: commit sealed chunks, enforce retention
  /// Commit flushed chunks into topic storage.
  /// Returns the deduplicated set of topic IDs that received at least one new chunk.
  /// Pass the return value directly to IncrementalEngine::onSourceCommitted():
  ///   derived.onSourceCommitted(engine.commitChunks(writer.flushAll()));
  std::vector<PJ::TopicId> commitChunks(std::vector<std::pair<PJ::TopicId, TopicChunk>> chunks);

  /// Evict old chunks outside the retention window across ALL datasets.
  void enforceRetention(PJ::Timestamp retention_window_ns);

  /// Evict old chunks outside the retention window for a SINGLE dataset, leaving
  /// every other dataset untouched. A live streaming source must scope its rolling
  /// retention to its own dataset so it does not silently trim the history of a
  /// file the user loaded into the same engine.
  void enforceRetention(PJ::Timestamp retention_window_ns, PJ::DatasetId dataset_id);

  /// Disown one topic's entire CURRENT history: raise its virtual retention
  /// floor past timeMax() so every stored row disappears from all read paths,
  /// while the topic, its TopicId, and its column layout stay registered (the
  /// catalog keeps listing the fields). Writes with newer timestamps are
  /// unaffected. Used to drop fake-interest samples — a demand-subscription
  /// field-discovery preview — before really-requested data flows; the cutoff
  /// is data-relative on purpose (stream timestamps are recording time, not
  /// wall time, so a wall-clock floor could hide future real samples).
  /// No-op for an unknown or empty topic. [thread-safe]
  void evictTopicHistory(PJ::TopicId topic_id);

  /// Empties one topic's history for a REPLAY that rewrites the same timestamps: its chunks are freed and its
  /// retention floor reset (unlike `evictTopicHistory`, whose floor hides every write at or below the old `timeMax()`),
  /// while the topic stays registered, listed and un-retired, with its column layout. Same reader precondition as
  /// `retireTopic`. No-op for an unknown id. [thread-safe]
  void clearTopicHistory(PJ::TopicId topic_id);

  /// Retire a single topic: clear its chunks (reclaiming the materialized series) and
  /// exclude its id from `listTopics` (so the catalog drops it on the next rebuild),
  /// while keeping the `TopicStorage` object alive — so any cached reader pointer sees
  /// an empty deque rather than freed memory. The chunk reclaim matters for derived/
  /// filter outputs: a re-applied filter mints a fresh id, so a retired output's storage
  /// can never be reused and would otherwise leak across undo/redo + layout-load cycles.
  /// Same hide-but-keep mechanism `replaceDatasetFrom` uses for primary-only topics (it
  /// clears chunks too). Used when a filter's producing node is removed (e.g. undo of a
  /// filter), since there is no scalar topic-teardown API. Idempotent; no-op for an
  /// unknown id.
  void retireTopic(PJ::TopicId topic_id);

  /// Clear every committed chunk for all topics under `dataset_id`, keeping the
  /// topics, schemas, inline-column layout, and TopicIds registered — so cached
  /// reader pointers see an empty deque (NOT freed memory) and the catalog keeps
  /// the topics visible. Unlike `retireTopic`, the ids STAY in `listTopics` so a
  /// subsequent refill writes back into them. Other datasets are untouched.
  /// Idempotent; a no-op for an unknown `dataset_id` (no throw).
  ///
  /// PRECONDITION: no reader/adapter may hold a raw `TopicChunk*` into this
  /// dataset — the caller drains those (via `datasetAboutToBeReplaced`) before
  /// calling, the same precondition `replaceDatasetFrom` relies on.
  void clearDatasetChunks(PJ::DatasetId dataset_id);

  /// Side snapshot of one dataset's scalar storage, produced by
  /// `detachDatasetChunks()`. Holds the MOVED-OUT chunk deques (O(1), no deep
  /// copy) plus the per-topic metadata a refill can mutate — eviction floor,
  /// inline column layout, and the array-expansion ratchet stats — so `reattach`
  /// can restore the EXACT prior values (the public ratchet setters only ever
  /// increase, so restoring a smaller value needs the stored copy). `prior_topic_ids`
  /// is the topic set live at detach time, letting reattach retire any topic a
  /// failed refill added. `valid == false` means nothing was detached.
  struct DatasetChunkSnapshot {
    struct TopicSnapshot {
      std::deque<TopicChunk> chunks;
      std::vector<ColumnDescriptor> column_descriptors;
      PJ::Timestamp retention_floor = kNoRetentionFloor;  // NOT 0 — see chunk.hpp
      PJ::ChunkId evicted_through_chunk_id = 0;
      uint32_t max_observed_array_length = 0;
      uint32_t truncated_sample_count = 0;
      std::unordered_map<std::string, uint32_t> array_expansion_counts;
    };
    PJ::DatasetId dataset_id = 0;
    std::vector<PJ::TopicId> prior_topic_ids;
    std::unordered_map<PJ::TopicId, TopicSnapshot> topics;
    bool valid = false;
  };

  /// Transactional variant of `clearDatasetChunks()`: instead of FREEING each
  /// live topic's chunks, MOVE them (and the refill-mutable metadata above) into
  /// the returned snapshot. Externally-visible state afterward is identical to
  /// `clearDatasetChunks` (topics stay listed + empty, floor reset). Same
  /// PRECONDITION + thread rules (drop cached `TopicChunk*` first; GUI-thread only).
  /// Takes `lockEngine()` internally. Unknown / empty dataset -> `valid == false`.
  [[nodiscard]] DatasetChunkSnapshot detachDatasetChunks(PJ::DatasetId dataset_id);

  /// Inverse of `detachDatasetChunks()`. For each topic currently listed under
  /// `dataset_id`: one NOT in `snapshot.prior_topic_ids` was added by a failed
  /// refill and is `retireTopic`'d; a prior topic has its partial-refill chunks
  /// cleared. Then each snapshot topic's chunks + metadata are moved back. Same
  /// PRECONDITION + thread rules; takes `lockEngine()` internally. Consumes the
  /// snapshot (leaves `valid == false`). No-op when `!snapshot.valid`.
  void reattachDatasetChunks(PJ::DatasetId dataset_id, DatasetChunkSnapshot&& snapshot);

  /// Move every committed chunk into `dst`, leaving this engine's storages
  /// empty (datasets, topics, schemas, time domains stay registered). Topics
  /// are matched by descriptor (`dataset_id` + `name`); both engines must have
  /// them registered. Monotonicity is enforced per topic: the source's
  /// earliest chunk timestamp must be >= the destination's `time_max()`. Any
  /// failure mutates neither engine.
  ///
  /// Zero-copy: dst's `std::deque<TopicChunk>` receives the chunks via
  /// `std::move` (column buffers/value arrays are pointer moves). Schema
  /// compatibility is the caller's responsibility — typically dst is kept in
  /// lockstep with the source via parallel registration at startup.
  ///
  /// A source that was retention-trimmed hands its trim over: each dst topic's
  /// retention floor rises to the source's, and its evicted-through chunk id
  /// (TopicStorage::evictedThroughChunkId) covers the source's, so rows trimmed
  /// before the flush stay hidden and an incremental consumer that never saw them
  /// records the gap. The source topics are left with no floor.
  PJ::Status flushTo(DataEngine& dst);

  /// In-place REPLACE of dataset `primary_id`'s data with the data ingested into
  /// dataset `staged_id` of a throwaway `staged` engine — used for reload so the
  /// primary DatasetId/TopicIds (and therefore all curve keys) stay stable.
  /// Topics are matched by name. Matched primary topics have their chunks cleared
  /// and replaced with the staged topic's chunks (each chunk's `topic_id` rewritten
  /// to the primary id); their inline column layout is copied across. Staged-only
  /// topics are created under `primary_id`. Primary-only topics are retired (chunks
  /// cleared, id excluded from `listTopics`, storage kept so cached reader pointers
  /// see an empty deque rather than dangling).
  ///
  /// Caller MUST invalidate every reader/adapter bound to `primary_id` BEFORE this
  /// call (no live `TopicChunk*` may survive into the chunk clear) and run no event
  /// loop between that invalidation and this call. The staged engine is drained.
  [[nodiscard]] PJ::Expected<DatasetReplaceResult> replaceDatasetFrom(
      DataEngine& staged, PJ::DatasetId staged_id, PJ::DatasetId primary_id);

  /// Destructively fold `sources` into dataset `anchor_id` — the union ("OR") of
  /// their scalar topics. Each source's samples are shifted by its `raw_shift_ns`
  /// (the caller bakes display alignment into this; the engine stays
  /// offset-agnostic), then merged with the anchor's by ascending shifted
  /// timestamp — ALL samples survive, duplicate timestamps coexist. Topics are
  /// matched by name: a shared name unions columns by `field_path` (additive —
  /// older rows read null for a newly-introduced column) and the anchor's column
  /// order/indices are preserved (so its curve keys stay valid); a name only the
  /// sources have becomes a NEW topic under the anchor. The anchor keeps its
  /// `DatasetId`/`TopicId`s. Each source dataset is emptied (its topics retired);
  /// the datasets stay registered. A shared topic whose contributors
  /// disagree on a field's storage type leaves the anchor's version intact and
  /// skips the conflicting source's contribution (reported in `skipped_topics`).
  /// Scalar (DataEngine) topics only — ObjectStore is untouched.
  ///
  /// Result chunks are written in ascending time order (non-overlapping), so the
  /// random-access SeriesReader path (plots) stays correct. Caller MUST invalidate
  /// every reader/adapter bound to the anchor and the sources BEFORE this call (no
  /// live `TopicChunk*` may survive the rebuild) and run no event loop until it
  /// returns. Errors (mutating nothing) if the anchor or any source is unknown, or
  /// a source equals the anchor.
  [[nodiscard]] PJ::Expected<DatasetMergeReport> mergeDatasets(
      PJ::DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources);

  // Writer/Reader factories
  /// Create a writer bound to this engine.
  [[nodiscard]] DataWriter createWriter();

  /// Create a reader bound to this engine.
  [[nodiscard]] DataReader createReader() const;

  // Topic listing by dataset
  /// List all dataset ids.
  [[nodiscard]] std::vector<PJ::DatasetId> listDatasets() const;

  /// List topic ids for a dataset.
  [[nodiscard]] std::vector<PJ::TopicId> listTopics(PJ::DatasetId dataset_id) const;

  // ---- Threading ----
  /// Acquire the engine's mutex; the ingest worker thread and the GUI thread
  /// serialize through it. The guard MUST outlive any cursor built from the data it
  /// protects — `DataReader` moves it into the returned `RangeCursor`/`SeriesReader`,
  /// which iterate the chunk deques lazily after the call returns; `getTopicStorage()`
  /// does NOT lock, so the caller holds this for the returned pointer's lifetime.
  ///
  /// Recursive (a shared_mutex would buy nothing — the GUI is the sole reader thread),
  /// so the same thread may re-acquire it: a held cursor doing another read, or a
  /// compound mutator calling another. It does NOT make calling a mutator from inside
  /// a `forEach` callback safe (mutating mid-iteration) — don't. mutable so a const
  /// reader can lock.
  [[nodiscard]] std::unique_lock<std::recursive_mutex> lockEngine() const;

  /// Like lockEngine() but returns the guard UNLOCKED (std::defer_lock), to lock TWO
  /// engines together deadlock-free: a deferred guard on each, then std::lock(). The
  /// write host holds the live engine while replaying to the staging engine this way,
  /// so it can't invert lock order against flushTo() (which also std::lock()s both).
  /// For a SINGLE engine use lockEngine().
  [[nodiscard]] std::unique_lock<std::recursive_mutex> lockEngineDeferred() const;

  /// Like commitChunks(), but assumes the caller ALREADY holds lockEngine().
  /// For compound writers that batch reads + commits under one lock; calling
  /// commitChunks() while holding lockEngine() would self-deadlock.
  std::vector<PJ::TopicId> commitChunksLocked(std::vector<std::pair<PJ::TopicId, TopicChunk>> chunks);

  /// Runs before a whole-series snapshot read of a topic (the toolbox
  /// `read_series_arrow` bridge that exporters and analysis toolboxes use). The
  /// derived-series owner installs it to finish a time-sliced replay still growing
  /// that topic, so a snapshot is never a silent truncation; live views (plots)
  /// read without it and watch the series grow. An empty barrier (the default)
  /// does nothing. Set on the GUI thread. awaitCompleteSeries() may be called from
  /// any thread, but the installed owner only completes the series for a caller on
  /// its own (GUI) thread: another thread gets the current, possibly partial, rows.
  using CompleteReadBarrier = std::function<void(PJ::TopicId)>;
  void setCompleteReadBarrier(CompleteReadBarrier barrier) {
    complete_read_barrier_ = std::move(barrier);
  }
  void awaitCompleteSeries(PJ::TopicId topic_id) const {
    if (complete_read_barrier_) {
      complete_read_barrier_(topic_id);
    }
  }

 private:
  // Move src's sealed chunks, retention floor and evicted-through id into dst,
  // re-stamping each chunk's topic_id to dst's id. Shared by flushTo (append
  // between mirrored topics) and replaceDatasetFrom (after dst is cleared). Needs
  // friend access to TopicStorage's private state.
  static void adoptChunksFrom(TopicStorage& dst, TopicStorage& src);

  // Lock-assuming variants: the caller already holds impl_->mutex_ (unique for
  // createTopicLocked, shared-or-unique for listTopicsLocked). The public
  // same-named methods take the lock and delegate. The split exists so a compound
  // mutator (e.g. replaceDatasetFrom, which already holds unique) can call them
  // WITHOUT re-acquiring the non-recursive mutex — which would self-deadlock.
  [[nodiscard]] PJ::Expected<PJ::TopicId> createTopicLocked(
      PJ::DatasetId dataset_id, TopicDescriptor descriptor, PJ::TopicId requested_id = 0);
  [[nodiscard]] std::vector<PJ::TopicId> listTopicsLocked(PJ::DatasetId dataset_id) const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  CompleteReadBarrier complete_read_barrier_;
};

/// Guard pair returned by lockEnginePair(). `primary` is always engaged;
/// `secondary` stays empty when no secondary engine was supplied.
struct EngineLockPair {
  std::unique_lock<std::recursive_mutex> primary;
  std::unique_lock<std::recursive_mutex> secondary;
};

/// Lock one or two engines together, deadlock-free, per lockEngineDeferred()'s
/// contract: a single recursive lockEngine() when `secondary` is null, otherwise
/// a deferred guard on each locked with std::lock(), so this can never invert
/// order against another two-engine locker (flushTo, the write-host replay).
/// Both ingest routes that mint/mirror topics go through here — the direct-write
/// one (plugin_data_host's lockWriteEngines, wrapping this) and the parser
/// binding one (DataSourceRuntimeHost::cbEnsureParserBinding) — so the two paths
/// cannot drift out of lock order. The mutex is recursive, so the engine methods
/// called underneath may re-acquire it.
[[nodiscard]] inline EngineLockPair lockEnginePair(DataEngine& primary, DataEngine* secondary) {
  if (secondary == nullptr) {
    return {primary.lockEngine(), {}};
  }
  auto primary_lock = primary.lockEngineDeferred();
  auto secondary_lock = secondary->lockEngineDeferred();
  std::lock(primary_lock, secondary_lock);
  return {std::move(primary_lock), std::move(secondary_lock)};
}

}  // namespace PJ
