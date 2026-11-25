#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {

using PJ::DatasetId;
using PJ::SchemaId;
using PJ::Timestamp;
using PJ::TopicId;

struct TopicDescriptor {
  std::string name;
  /// Schema of newly written chunks; 0 for inline columns (scalar series).
  SchemaId schema_id = 0;
  DatasetId dataset_id = 0;
  uint32_t max_chunk_rows = 1024;
  /// Element columns expandArray() creates at most per variable-length array field.
  uint32_t array_expansion_limit = 64;
};

struct TopicMetadata {
  TopicId topic_id = 0;
  std::string name;
  SchemaId current_schema = 0;
  DatasetId dataset_id = 0;
  /// Clamped to the retention floor.
  Timestamp time_range_min = 0;
  Timestamp time_range_max = 0;
  /// Physical rows, including the sub-floor rows of a chunk straddling the retention floor: a storage metric, not the
  /// visible sample count.
  uint64_t total_row_count = 0;
  uint64_t total_byte_size = 0;  // approximate
  /// Largest array length ever passed to expandArray() for this topic.
  uint32_t max_observed_array_length = 0;
  /// Times expandArray() clamped to array_expansion_limit.
  uint32_t truncated_sample_count = 0;
};

class DataEngine;

/// One topic's committed chunks, in commit order. Out-of-order ingest makes chunk time ranges overlap, so readers
/// merge across chunks. Only the last chunk changes, in place (appendRows), so nothing read from a chunk outlives the
/// engine lock it was read under. Also holds the inline column layout of schema-less topics and the array-expansion
/// counts. Guarded by the engine lock.
class TopicStorage {
 public:
  TopicStorage(TopicId topic_id, TopicDescriptor descriptor);

  /// Never rejects a chunk, overlapping ones included: rejecting would drop late data. Gives the chunk a fresh id when
  /// its id is not above this topic's newest, so ids ascend in commit order.
  [[nodiscard]] PJ::Status appendSealedChunk(TopicChunk chunk);

  /// Commits a writer's rows without sealing them into a chunk of their own: they extend the topic's last chunk while
  /// it has room, a layout theirs fits (TopicChunkBuilder::fitsLayoutOf) and no row newer than theirs, and start a
  /// new chunk otherwise. A chunk is re-encoded once, when it reaches the topic's max_chunk_rows, so a topic's chunks
  /// are cut by its rows and not by how often its writer commits. A chunk that grows keeps its id and its rows their
  /// positions (see TopicChunk), and gains the columns the topic gained. `batch` is left for clearRows(): its rows are
  /// sorted and may have been moved out.
  void appendRows(TopicChunkBuilder& batch);

  /// Re-encodes the last chunk if appendRows() left it in growable form: call when a source ends, so a chunk that will
  /// not fill does not stay stored raw.
  void compactLastChunk();

  /// Drops the chunks that end before `t_keep_min` and raises the retention floor to it (rows of a chunk straddling
  /// the floor stay in memory but are hidden).
  void evictBefore(Timestamp t_keep_min);

  /// Also resets the retention floor and evictedThroughChunkId(). Retired columns stay retired: a refill that empties
  /// the topic must not bring a column a reload lost back, and a value landing un-retires it anyway.
  void clearChunks() noexcept;

  /// What detachChunks() takes out: the chunks plus every field clearChunks() leaves alone, so restoreChunks() puts
  /// the prior state back exactly even if a writer changed those fields meanwhile.
  struct DetachedState {
    std::deque<TopicChunk> chunks;
    std::vector<ColumnDescriptor> column_descriptors;
    Timestamp retention_floor = kNoRetentionFloor;  // not 0: see kNoRetentionFloor
    ChunkId evicted_through_chunk_id = 0;
    SchemaId schema_id = 0;
    uint32_t max_observed_array_length = 0;
    uint32_t truncated_sample_count = 0;
    std::unordered_map<std::string, uint32_t> array_expansion_counts;
    std::unordered_set<FieldId> retired_columns;
  };

  /// Takes the chunks and metadata out, leaving the storage as clearChunks() would, so restoreChunks() can undo a
  /// failed write. Strong guarantee: the allocating copies run before the chunks move, so a throw changes nothing.
  [[nodiscard]] DetachedState detachChunks();

  /// Puts `detached` back over whatever is retained now. An rvalue reference so that even the argument binding cannot
  /// allocate: the whole call is noexcept, safe in a rollback path or a destructor.
  void restoreChunks(DetachedState&& detached) noexcept;

  /// A chunk straddling the retention floor still holds rows below retentionFloor(): skip them when reading values
  /// here, or read through DataReader, which does.
  [[nodiscard]] const std::deque<TopicChunk>& sealedChunks() const noexcept;

  /// The layout of a schema-less topic, stored at writer registration so the incremental engine and new writers
  /// resolve it before any chunk is committed.
  void setColumnDescriptors(std::vector<ColumnDescriptor> descs) noexcept;
  /// Appends one column without copying the layout, which is quadratic for a topic that grows column by column.
  void addColumnDescriptor(ColumnDescriptor desc);

  [[nodiscard]] const std::vector<ColumnDescriptor>& columnDescriptors() const noexcept;

  /// Retires every stored column no retained chunk holds a value of: a refill that no longer writes a column leaves it
  /// all-null. The column stays in columnDescriptors() (field ids are dense positions, and curve keys and writers
  /// address columns by them), so listings skip it through isColumnRetired(). A chunk holding a value of a retired
  /// column un-retires it.
  void retireValuelessColumns();

  [[nodiscard]] bool isColumnRetired(FieldId field_id) const noexcept {
    return retired_columns_.count(field_id) != 0;
  }

  [[nodiscard]] const std::unordered_set<FieldId>& retiredColumns() const noexcept {
    return retired_columns_;
  }

  /// The position of the first stored column not retired: the column a whole-topic name stands for. 0 when no
  /// column is stored or every one is retired.
  [[nodiscard]] std::size_t firstLiveColumn() const noexcept;

  /// O(1) on append and rebuilt from per-chunk stats after other changes, never from column data: progressive loads
  /// call it per topic on every ingest tick.
  [[nodiscard]] TopicMetadata metadata() const;

  [[nodiscard]] const TopicDescriptor& descriptor() const noexcept;

  [[nodiscard]] TopicId topicId() const noexcept;

  [[nodiscard]] bool empty() const noexcept;

  /// 0 if empty.
  [[nodiscard]] Timestamp timeMin() const noexcept;

  /// 0 if empty.
  [[nodiscard]] Timestamp timeMax() const noexcept;

  /// The last `t_keep_min` given to evictBefore() (absolute ns; kNoRetentionFloor when none). No read path may show a
  /// row below it, though a straddling chunk still holds such rows.
  [[nodiscard]] Timestamp retentionFloor() const noexcept;

  /// Highest chunk id that eviction dropped or cut rows from; 0 if none. Chunk ids ascend in commit order (see
  /// appendSealedChunk()), so a consumer whose last chunk id is below this missed evicted rows.
  [[nodiscard]] ChunkId evictedThroughChunkId() const noexcept;

  /// Bumped by every change a reader cannot follow incrementally: everything except a strictly in-order append and a
  /// retention prefix eviction (seen through retentionFloor()). Sample caches keyed on it rebuild.
  [[nodiscard]] uint64_t seriesGeneration() const noexcept {
    return series_generation_;
  }
  /// For a float field, whether every retained non-null value is a whole number; nullopt when no retained chunk has
  /// it as a float column. Lets consumers treat floats that only ever held integers (JSON numbers) as discrete.
  [[nodiscard]] std::optional<bool> wholeNumbersOnly(FieldId field_id) const;
  /// Changes whenever a wholeNumbersOnly() answer changes, so the catalog fingerprint notices without scanning.
  [[nodiscard]] uint64_t wholeNumbersSignature() const;

  void updateSchema(SchemaId new_schema);

  /// Called by DataWriter::expandArray().
  void updateMaxObservedArrayLength(uint32_t observed_length);

  /// Called when DataWriter::expandArray() clamps.
  void incrementTruncatedSampleCount();

  [[nodiscard]] uint32_t maxObservedArrayLength() const noexcept;

  [[nodiscard]] uint32_t truncatedSampleCount() const noexcept;

  /// 0 if the field was never expanded.
  [[nodiscard]] uint32_t arrayExpansionCount(const std::string& field_path) const noexcept;

  void setArrayExpansionCount(const std::string& field_path, uint32_t count);

 private:
  // DataEngine moves sealed_chunks_ between engines without copying. Every place that touches sealed_chunks_
  // directly must call invalidateChunkAggregate() on the storages it changed.
  friend class DataEngine;

  void invalidateChunkAggregate() const noexcept {
    chunk_agg_valid_ = false;
    whole_agg_valid_ = false;
  }

  // From per-chunk stats only (encoded sizes are memoized at append).
  void ensureChunkAggregate() const noexcept;
  // Separate because its map can allocate, which the noexcept rebuild above cannot.
  void ensureWholeNumberAggregate() const;
  // direction: +1 adds the chunk's float columns, -1 removes them.
  void foldWholeNumbers(const TopicChunk& chunk, int direction) const;
  // `evidence` is a WholeEvidence (topic_storage.cpp) of one chunk about `field_id`.
  void foldWholeEvidence(FieldId field_id, uint8_t evidence, int direction) const;
  // Runs `change` on `chunk`, already stored, and moves the whole-number aggregate by what it changed.
  template <typename Change>
  void changeStoredChunk(TopicChunk& chunk, Change&& change);
  // Grows `chunk`, the last one, by `count` rows of `batch` and keeps the cached aggregates in step.
  void extendLastChunk(TopicChunk& chunk, const TopicChunkBuilder& batch, std::size_t first, std::size_t count);
  // compact() on the last chunk, keeping the byte-size aggregate in step.
  void compactChunk(TopicChunk& chunk);
  void unretireColumnsWithValues(const TopicChunk& chunk);

  TopicId topic_id_;
  TopicDescriptor descriptor_;
  std::deque<TopicChunk> sealed_chunks_;

  // Raw extrema: the retention floor is applied at read time, so a rising floor never invalidates them.
  mutable bool chunk_agg_valid_ = false;
  mutable Timestamp chunk_agg_t_min_ = 0;
  mutable Timestamp chunk_agg_t_max_ = 0;
  mutable uint64_t chunk_agg_row_count_ = 0;
  mutable uint64_t chunk_agg_byte_size_ = 0;
  // Counts rather than a plain AND, so a prefix eviction subtracts instead of rebuilding.
  struct WholeNumberCounts {
    uint32_t chunks = 0;
    uint32_t fractional_chunks = 0;
  };
  mutable bool whole_agg_valid_ = false;
  mutable std::unordered_map<FieldId, WholeNumberCounts> whole_agg_;
  mutable uint64_t whole_agg_signature_ = 0;

  std::vector<ColumnDescriptor> column_descriptors_;  // schema-less topics only
  std::unordered_set<FieldId> retired_columns_;
  uint32_t max_observed_array_length_ = 0;
  uint32_t truncated_sample_count_ = 0;
  uint64_t series_generation_ = 0;
  Timestamp retention_floor_ = kNoRetentionFloor;
  ChunkId evicted_through_chunk_id_ = 0;
  // Shared by every DataWriter of this topic.
  std::unordered_map<std::string, uint32_t> array_expansion_counts_;
};

}  // namespace PJ
