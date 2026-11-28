#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

/// Owns datasets, topics, schemas and committed chunks. Every access holds lockEngine() (see there); the methods
/// that lock say so in their bodies, the raw accessors do not lock.
class DataEngine {
 public:
  DataEngine();

  ~DataEngine();

  DataEngine(DataEngine&&) noexcept;

  DataEngine& operator=(DataEngine&&) noexcept;

  DataEngine(const DataEngine&) = delete;

  DataEngine& operator=(const DataEngine&) = delete;

  // Dataset management
  /// A non-zero `requested_id` forces that id (keeps two engines' ids in lockstep); otherwise it is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::DatasetId> createDataset(
      PJ::DatasetDescriptor descriptor, PJ::DatasetId requested_id = 0);

  /// nullptr if missing.
  [[nodiscard]] const PJ::DatasetInfo* getDataset(PJ::DatasetId id) const;

  /// The one name of a dataset every surface shows and accepts: the catalog tree, the plugin catalog snapshot
  /// (`PJ_data_source_info_t.name`) and every "label:topic/field" qualifier the host resolves. It is the base ('/'
  /// read as '_', "Dataset" when empty), or the base plus the lowest free " (N)", N >= 2, when another dataset holds
  /// the base, so it is unique in this engine. Assigned at createDataset() and held, merged away or not, until
  /// removeDataset(): another dataset coming or going never renames it, and a removed dataset's label later names a
  /// new dataset, never a survivor. Empty for an unknown id. It is a session name, not a persistent identity: layouts
  /// keep the dataset id and the raw `source_name`. Locks.
  [[nodiscard]] std::string datasetLabel(PJ::DatasetId id) const;

  /// The dataset holding the data `label` names: the dataset so labelled or, for a source a mergeDatasets() consumed,
  /// the anchor now holding its data (through later merges). nullopt for an empty or unknown label, or when that
  /// anchor was removed. Locks.
  [[nodiscard]] std::optional<PJ::DatasetId> datasetByLabel(std::string_view label) const;

  /// Every dataset's label: the vocabulary a "label:topic/field" qualifier is split against. Locks.
  [[nodiscard]] std::vector<std::string> datasetLabels() const;

  /// Relabels the dataset from `base` (empty: its source name) by the createDataset() rule, its own label not counting
  /// as taken. A display name (a plugin's, a merge's) is such a relabel. A base equal, once normalized, to the one the
  /// label was made from keeps the label as it is (a reload naming the dataset after its file again renames nothing).
  /// Returns the label, empty for an unknown id. Locks.
  std::string setDatasetLabel(PJ::DatasetId id, std::string_view base);

  /// True for a source a mergeDatasets() consumed: registered with no topics, still holding its label (so no other
  /// dataset is renamed and no later one takes it; datasetByLabel() maps it to the anchor), listed by no surface (the
  /// plugin catalog snapshot skips it, the app's tree drops it), and never a load's target: opening its file again
  /// loads a new dataset. Locks.
  [[nodiscard]] bool isMergedAway(PJ::DatasetId id) const;

  /// The dataset holding @p id's data: @p id, or for a source a mergeDatasets() consumed, the anchor now holding it
  /// (through later merges). nullopt when that dataset is not registered (removed, or never was). Locks.
  [[nodiscard]] std::optional<PJ::DatasetId> mergeDestination(PJ::DatasetId id) const;

  /// Erases the dataset, its label and all its topics (unlike retireTopic(), which keeps the storage). Auto-assigned
  /// ids are never reused, so a later load gets fresh ids. Time domains stay (they may be shared). Invalidate every
  /// reader bound to the dataset first: a cached TopicChunk* would dangle. Idempotent.
  void removeDataset(PJ::DatasetId id);

  // Topic management (called by DataWriter)
  /// A non-zero `requested_id` forces that id; otherwise it is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::TopicId> createTopic(
      PJ::DatasetId dataset_id, TopicDescriptor descriptor, PJ::TopicId requested_id = 0);

  /// One column of `createColumnGroupTopic`.
  struct GroupColumn {
    std::string name;
    PJ::PrimitiveType type = PJ::PrimitiveType::kFloat64;
  };

  /// One topic whose schema `schema_name` is a struct with one primitive field per column (field k is column k), for
  /// an on-demand series. Reuses a schema already registered under that name. Locks.
  [[nodiscard]] PJ::Expected<PJ::TopicId> createColumnGroupTopic(
      PJ::DatasetId dataset_id, const std::string& topic_name, const std::string& schema_name,
      const std::vector<GroupColumn>& columns);

  /// Adds a column to an existing topic, or returns the id of an existing column with the same name and type. FieldIds
  /// are dense from 0; a forced `requested_id` must be the next dense id. Forcing keeps the live and staging engines
  /// in lockstep, so a plugin's cached FieldHandle resolves on both sides of the pause/resume swap. Fails for an
  /// unknown topic, a same-name column of another type, or a forced id that does not fit.
  [[nodiscard]] PJ::Expected<PJ::FieldId> createTopicField(
      PJ::TopicId topic_id, std::string_view field_name, PJ::PrimitiveType type,
      std::optional<PJ::FieldId> requested_id = std::nullopt);

  /// Does not lock: hold lockEngine() for as long as the pointer is used. nullptr if missing.
  [[nodiscard]] TopicStorage* getTopicStorage(PJ::TopicId id);

  /// Does not lock: hold lockEngine() for as long as the pointer is used. nullptr if missing.
  [[nodiscard]] const TopicStorage* getTopicStorage(PJ::TopicId id) const;

  /// TypeRegistry has no lock of its own: hold lockEngine() across every use, and do not keep a TypeTreeNode* from
  /// lookup() past it.
  [[nodiscard]] TypeRegistry& typeRegistry();

  /// See the non-const overload.
  [[nodiscard]] const TypeRegistry& typeRegistry() const;

  // Time domains
  /// A non-zero `requested_id` forces that id; otherwise it is auto-assigned.
  [[nodiscard]] PJ::Expected<PJ::TimeDomainId> createTimeDomain(std::string name, PJ::TimeDomainId requested_id = 0);

  /// nullptr if missing.
  [[nodiscard]] const PJ::TimeDomain* getTimeDomain(PJ::TimeDomainId id) const;

  void setDisplayOffset(PJ::TimeDomainId id, PJ::Timestamp offset);

  // Commit cycle: commit sealed chunks, enforce retention
  /// Returns the sorted, deduplicated ids of the topics that received a chunk, ready for
  /// IncrementalEngine::onSourceCommitted(): `derived.onSourceCommitted(engine.commitChunks(writer.flushAll()))`.
  std::vector<PJ::TopicId> commitChunks(std::vector<std::pair<PJ::TopicId, TopicChunk>> chunks);

  /// Commits a builder's rows without sealing them into a chunk of their own (TopicStorage::appendRows); false when
  /// the topic is gone. For DataWriter::commitRows(); the caller holds lockEngine() wherever another thread can see
  /// the engine.
  bool commitRows(PJ::TopicId topic_id, TopicChunkBuilder& rows);

  /// Evicts, in every dataset, what is older than each topic's newest row minus the window.
  void enforceRetention(PJ::Timestamp retention_window_ns);

  /// Same, for one dataset: a streaming source trims its own history, never a file loaded into the same engine.
  void enforceRetention(PJ::Timestamp retention_window_ns, PJ::DatasetId dataset_id);

  /// Hides every row the topic holds now by raising its retention floor past timeMax(); the topic, its id and its
  /// columns stay listed, and newer writes show. Drops a field-discovery preview before the requested data flows. The
  /// floor is data-relative because stream timestamps are recording time: a wall-clock floor could hide future rows.
  /// Locks.
  void evictTopicHistory(PJ::TopicId topic_id);

  /// Frees the topic's chunks and resets its floor, for a replay that rewrites the same timestamps
  /// (evictTopicHistory()'s floor would hide them). The topic stays listed with its columns. Same reader
  /// precondition as retireTopic(). Locks.
  void clearTopicHistory(PJ::TopicId topic_id);

  /// Frees the topic's chunks and hides it from listTopics(), keeping the TopicStorage object so a cached reader
  /// sees an empty deque, not freed memory. Removes a derived output whose producer is gone: a re-applied filter
  /// gets a new id, so the old chunks would otherwise stay resident. A later commit to the topic lists it again.
  /// Idempotent.
  void retireTopic(PJ::TopicId topic_id);

  /// Frees the chunks of every topic of the dataset but keeps the topics listed (unlike retireTopic()), so a refill
  /// writes back into the same ids. No reader may hold a TopicChunk* into the dataset (callers drain them on
  /// `datasetAboutToBeReplaced`). Idempotent.
  void clearDatasetChunks(PJ::DatasetId dataset_id);

  /// The chunks detachDatasetChunks() moved out, plus the per-topic metadata a refill can change. The metadata is
  /// stored because the public ratchet setters only increase, so they cannot restore a smaller prior value.
  /// `prior_topic_ids` lets reattach retire topics a failed refill added. `valid == false`: nothing was detached.
  struct DatasetChunkSnapshot {
    struct TopicSnapshot {
      std::deque<TopicChunk> chunks;
      std::vector<ColumnDescriptor> column_descriptors;
      PJ::Timestamp retention_floor = kNoRetentionFloor;  // not 0: see kNoRetentionFloor
      PJ::ChunkId evicted_through_chunk_id = 0;
      uint32_t max_observed_array_length = 0;
      uint32_t truncated_sample_count = 0;
      std::unordered_map<std::string, uint32_t> array_expansion_counts;
      std::unordered_set<PJ::FieldId> retired_columns;
    };
    PJ::DatasetId dataset_id = 0;
    std::vector<PJ::TopicId> prior_topic_ids;
    std::unordered_map<PJ::TopicId, TopicSnapshot> topics;
    bool valid = false;
  };

  /// clearDatasetChunks() that moves the chunks into the returned snapshot instead of freeing them. Same reader
  /// precondition; GUI thread only. Locks. Unknown or empty dataset: `valid == false`.
  [[nodiscard]] DatasetChunkSnapshot detachDatasetChunks(PJ::DatasetId dataset_id);

  /// Undoes detachDatasetChunks(): retires topics added since, empties the prior ones and moves the snapshot back
  /// (consuming it). Same reader precondition; locks. No-op for an invalid snapshot.
  void reattachDatasetChunks(PJ::DatasetId dataset_id, DatasetChunkSnapshot&& snapshot);

  /// Moves every chunk into `dst` (registrations stay), matching topics by (dataset, name); dst must have them, with
  /// compatible schemas (the staging engine is kept in lockstep), and a failure changes neither engine. Zero-copy; a
  /// source row older than dst's newest is kept, as an overlapping chunk. The source's retention floor and
  /// evicted-through chunk id carry over, so trimmed rows stay hidden and a consumer that never saw them records the
  /// gap.
  PJ::Status flushTo(DataEngine& dst);

  /// Replaces dataset `primary_id`'s data with what was ingested into `staged`'s `staged_id` (the reload path),
  /// matching topics by name, so the primary dataset and topic ids, and the curve keys built on them, stay. Matched
  /// topics take the staged chunks and column layout; staged-only topics are created; primary-only topics are
  /// retired. The staged engine is drained. Invalidate every reader bound to `primary_id` first and run no event loop
  /// in between: a surviving TopicChunk* would dangle.
  [[nodiscard]] PJ::Expected<DatasetReplaceResult> replaceDatasetFrom(
      DataEngine& staged, PJ::DatasetId staged_id, PJ::DatasetId primary_id);

  /// Folds `sources` into `anchor_id`, a union of their scalar topics (the ObjectStore has its own merge). Each source
  /// is shifted by its `raw_shift_ns`; every sample survives, equal timestamps included. Topics match by name and
  /// columns by field path; the anchor keeps its ids and column indices, so curve keys stay valid. A source whose
  /// field type conflicts with the anchor's is skipped for that topic and reported. Sources end up empty but
  /// registered.
  ///
  /// The rebuilt chunks are ascending and non-overlapping, as the random-access SeriesReader path requires.
  /// Invalidate every reader bound to the anchor and the sources first and run no event loop until this returns.
  /// Only the up-front checks (unknown, self or repeated source) are guaranteed to leave everything unchanged.
  [[nodiscard]] PJ::Expected<DatasetMergeReport> mergeDatasets(
      PJ::DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources);

  // Writer/Reader factories
  [[nodiscard]] DataWriter createWriter();

  [[nodiscard]] DataReader createReader() const;

  // Topic listing by dataset
  [[nodiscard]] std::vector<PJ::DatasetId> listDatasets() const;

  /// Excludes retired topics.
  [[nodiscard]] std::vector<PJ::TopicId> listTopics(PJ::DatasetId dataset_id) const;

  // ---- Threading ----
  /// The one engine mutex: the ingest worker and the GUI thread serialize through it. The guard must outlive every
  /// cursor and TopicStorage* obtained under it; DataReader moves it into the RangeCursor/SeriesReader it returns,
  /// because they walk the chunks lazily. Recursive, so a thread may re-lock (a held cursor reading again, one
  /// mutator calling another), but mutating from inside a forEach callback is still unsafe.
  [[nodiscard]] std::unique_lock<std::recursive_mutex> lockEngine() const;

  /// lockEngine() returned unlocked, for locking two engines with std::lock() (see lockEnginePair()). Holding one
  /// engine while plain-locking the other can deadlock against flushTo(), which std::lock()s both.
  [[nodiscard]] std::unique_lock<std::recursive_mutex> lockEngineDeferred() const;

  /// commitChunks() for a caller that already holds lockEngine(); it skips the re-lock.
  std::vector<PJ::TopicId> commitChunksLocked(std::vector<std::pair<PJ::TopicId, TopicChunk>> chunks);

  /// Runs before a whole-series snapshot read (the toolbox `read_series_arrow` bridge used by exporters and analysis
  /// toolboxes). The derived-series owner installs one that finishes a time-sliced replay still growing the topic, so
  /// a snapshot is never silently truncated; plots read without it. Set on the GUI thread. awaitCompleteSeries() may
  /// run on any thread, but only a GUI-thread caller gets the completed series: others read the rows present now.
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
  // Appends src's chunks to dst under dst's topic id and carries src's retention floor and evicted-through id over.
  static void adoptChunksFrom(TopicStorage& dst, TopicStorage& src);

  // For a caller that already holds the engine mutex; they skip the re-lock.
  [[nodiscard]] PJ::Expected<PJ::TopicId> createTopicLocked(
      PJ::DatasetId dataset_id, TopicDescriptor descriptor, PJ::TopicId requested_id = 0);
  [[nodiscard]] std::vector<PJ::TopicId> listTopicsLocked(PJ::DatasetId dataset_id) const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  CompleteReadBarrier complete_read_barrier_;
};

/// `secondary` is empty when lockEnginePair() got no secondary engine.
struct EngineLockPair {
  std::unique_lock<std::recursive_mutex> primary;
  std::unique_lock<std::recursive_mutex> secondary;
};

/// Locks the live engine and, when given, its staging mirror, with std::lock() so the order can never invert against
/// another two-engine locker such as flushTo(). Every ingest path that creates or mirrors topics (the direct-write
/// host and parser binding) locks through here.
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
