#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"

namespace PJ {

// Import base types into engine namespace
using PJ::DatasetId;
using PJ::SchemaId;
using PJ::Timestamp;
using PJ::TopicId;

struct TopicDescriptor {
  /// Topic display/name key.
  std::string name;
  /// Active schema for newly written chunks.
  SchemaId schema_id = 0;
  /// Owning dataset.
  DatasetId dataset_id = 0;
  /// Target maximum rows per chunk for writers.
  uint32_t max_chunk_rows = 1024;  // Default chunk size
  /// Maximum number of element columns to expand per variable-length array field.
  /// Prevents column explosion. expandArray() clamps to this limit.
  uint32_t array_expansion_limit = 64;
};

/// Aggregated metadata snapshot for one topic.
struct TopicMetadata {
  /// Topic identifier.
  TopicId topic_id = 0;
  /// Topic display/name key.
  std::string name;
  /// Current schema id.
  SchemaId current_schema = 0;
  /// Owning dataset id.
  DatasetId dataset_id = 0;
  /// Minimum timestamp across retained chunks.
  Timestamp time_range_min = 0;
  /// Maximum timestamp across retained chunks.
  Timestamp time_range_max = 0;
  /// Total PHYSICAL rows across retained chunks. Unlike time_range_min (clamped
  /// to the retention floor), this counts a straddling chunk's sub-floor rows
  /// too — it is a storage/memory metric, not the logical visible-sample count.
  uint64_t total_row_count = 0;
  /// Approximate total memory footprint across retained chunks.
  uint64_t total_byte_size = 0;  // approximate
  /// Largest array length ever passed to expandArray() for any field in this topic.
  uint32_t max_observed_array_length = 0;
  /// Number of times expandArray() clamped due to array_expansion_limit.
  uint32_t truncated_sample_count = 0;
};

class DataEngine;

/// Per-topic container of committed chunks (commit-ordered deque).
/// appendSealedChunk() appends in commit order without rejecting any chunk —
/// out-of-order ingest means chunk time ranges may overlap, so queries merge
/// across them. evictBefore() drops the contiguous older prefix and raises a
/// per-topic retention floor. Also holds the column layout for schemaless
/// (schema_id==0) topics and per-field array-expansion counts.
class TopicStorage {
 public:
  /// Create storage for one topic descriptor.
  TopicStorage(TopicId topic_id, TopicDescriptor descriptor);

  /// Append a sealed chunk. Never rejects: chunks are kept in commit order and
  /// may overlap in time (queries merge across them); rejecting would silently
  /// drop late data.
  [[nodiscard]] PJ::Status appendSealedChunk(TopicChunk chunk);

  /// Remove chunks whose max time is strictly before `t_keep_min`.
  void evictBefore(Timestamp t_keep_min);

  /// Unconditionally remove all retained sealed chunks.
  void clearChunks() noexcept;

  /// State pulled out of a TopicStorage by detachChunks(): the chunk deque plus every
  /// mutable field clearChunks() leaves untouched (column layout, active schema id,
  /// array-expansion ratchets), so restoreChunks() can put a prior state back byte-for-byte
  /// even if a writer mutated those fields in between.
  struct DetachedState {
    std::deque<TopicChunk> chunks;
    std::vector<ColumnDescriptor> column_descriptors;
    Timestamp retention_floor = kNoRetentionFloor;  // NOT 0 -- see kNoRetentionFloor
    SchemaId schema_id = 0;                         // descriptor().schema_id, mutable via updateSchema()
    uint32_t max_observed_array_length = 0;
    uint32_t truncated_sample_count = 0;
    std::unordered_map<std::string, uint32_t> array_expansion_counts;
  };

  /// Move every retained chunk out (O(1), like clearChunks()) and snapshot the mutable
  /// metadata clearChunks() itself leaves alone, so a caller can later undo whatever a
  /// failed write did via restoreChunks(). Leaves this storage exactly as clearChunks()
  /// would.
  /// Strong guarantee, hence NOT noexcept: every allocating copy runs before the chunk
  /// deque is moved out, so a throw leaves this storage untouched and loses no data.
  [[nodiscard]] DetachedState detachChunks();

  /// Inverse of detachChunks(): move `detached`'s chunks and metadata back into this
  /// storage, overwriting whatever is currently retained (typically the empty state
  /// detachChunks() left, plus any partial data a failed write produced afterward).
  /// Consumes `detached`. Takes an rvalue REFERENCE rather than a value so that not even
  /// the argument binding can allocate: every step is a move-assignment, making the whole
  /// call non-throwing and therefore safe from a rollback path or a destructor.
  void restoreChunks(DetachedState&& detached) noexcept;

  /// Access retained sealed chunks in commit order.
  /// CAVEAT: a chunk straddling the retention floor still physically holds rows
  /// with timestamp < retentionFloor(). Those rows are logically evicted; callers
  /// that read values here must skip rows below retentionFloor() (or read through
  /// DataReader, which enforces the floor for you).
  [[nodiscard]] const std::deque<TopicChunk>& sealedChunks() const noexcept;

  /// Store column layout for schema_id==0 topics (populated at writer registration time).
  /// Allows derived engine and fresh writers to resolve the layout without a committed chunk.
  void setColumnDescriptors(std::vector<ColumnDescriptor> descs) noexcept;

  /// Inline column layout (non-empty for schema_id==0 topics after the first writer is created).
  [[nodiscard]] const std::vector<ColumnDescriptor>& columnDescriptors() const noexcept;

  /// Aggregated metadata for current retained chunks. O(1) on the hot path: a
  /// cached aggregate is merged incrementally on append and rebuilt lazily
  /// (from per-chunk stats only — column buffers are never re-walked) after a
  /// destructive mutation. Progressive loads call this per topic on every
  /// ingest tick, so it must not scan chunk contents.
  [[nodiscard]] TopicMetadata metadata() const;

  /// Access topic descriptor.
  [[nodiscard]] const TopicDescriptor& descriptor() const noexcept;

  /// Topic identifier.
  [[nodiscard]] TopicId topicId() const noexcept;

  /// True if no chunks are retained.
  [[nodiscard]] bool empty() const noexcept;

  /// Minimum timestamp of retained chunks (0 if empty).
  [[nodiscard]] Timestamp timeMin() const noexcept;

  /// Maximum timestamp of retained chunks (0 if empty).
  [[nodiscard]] Timestamp timeMax() const noexcept;

  /// Logical retention floor: the most recent `t_keep_min` ever passed to
  /// evictBefore(). Data with timestamp < this is logically evicted and must
  /// never be observed through any read path — even though the straddling chunk
  /// that physically holds it survives whole-chunk eviction (the chunk lingers as
  /// lazy GC state). Absolute ns; the "no floor" value is kNoRetentionFloor.
  /// Raised monotonically by evictBefore(); reset by clearChunks(). See the
  /// sealedChunks() caveat.
  [[nodiscard]] Timestamp retentionFloor() const noexcept;

  /// Update descriptor schema id for future writes.
  void updateSchema(SchemaId new_schema);

  /// Track the largest observed array length (called by DataWriter::expand_array).
  void updateMaxObservedArrayLength(uint32_t observed_length);

  /// Increment the truncation counter (called when expand_array clamps due to limit).
  void incrementTruncatedSampleCount();

  /// Largest array length ever passed to expandArray() for any field in this topic.
  [[nodiscard]] uint32_t maxObservedArrayLength() const noexcept;

  /// Number of times expandArray() clamped due to array_expansion_limit.
  [[nodiscard]] uint32_t truncatedSampleCount() const noexcept;

  /// Return the current expansion count for a variable-length array field.
  /// Returns 0 if the field has not been expanded yet.
  [[nodiscard]] uint32_t arrayExpansionCount(const std::string& field_path) const noexcept;

  /// Update the expansion count for a variable-length array field.
  void setArrayExpansionCount(const std::string& field_path, uint32_t count);

 private:
  // DataEngine::flushTo needs to move sealed_chunks_ between TopicStorage
  // instances of different engines without copying. Friending it lets the
  // transfer happen entirely inside DataEngine without exposing the move
  // primitive on the public TopicStorage API. Every friend site that touches
  // sealed_chunks_ directly MUST call invalidateChunkAggregate() on the
  // storages it mutated.
  friend class DataEngine;

  /// Drop the cached chunk aggregate; the next metadata()/timeMin()/timeMax()
  /// rebuilds it from per-chunk stats. For mutations that bypass
  /// appendSealedChunk (friend moves, eviction, clear).
  void invalidateChunkAggregate() const noexcept {
    chunk_agg_valid_ = false;
  }

  /// Rebuild the cached aggregate from per-chunk stats when invalid. Stats-only
  /// scan — never walks column buffers (their encoded size is memoized in
  /// ChunkStats::encoded_byte_size at append time).
  void ensureChunkAggregate() const noexcept;

  TopicId topic_id_;
  TopicDescriptor descriptor_;
  std::deque<TopicChunk> sealed_chunks_;

  // Cached aggregate over sealed_chunks_ (raw extrema — the retention-floor
  // clamp is applied at read time, so a rising floor never invalidates it).
  // Guarded by the engine lock like everything else here; mutable because the
  // metadata read paths are const.
  mutable bool chunk_agg_valid_ = false;
  mutable Timestamp chunk_agg_t_min_ = 0;
  mutable Timestamp chunk_agg_t_max_ = 0;
  mutable uint64_t chunk_agg_row_count_ = 0;
  mutable uint64_t chunk_agg_byte_size_ = 0;

  std::vector<ColumnDescriptor> column_descriptors_;  // for schema_id==0 topics
  uint32_t max_observed_array_length_ = 0;
  uint32_t truncated_sample_count_ = 0;
  // Logical retention floor (absolute ns). evictBefore() raises this to the
  // requested cutoff; read accessors clamp to it so rows below it stay invisible
  // even while their straddling chunk remains physically retained (lazy GC).
  Timestamp retention_floor_ = kNoRetentionFloor;
  // Authoritative expansion count per variable-length array field path.
  // Shared across all DataWriter instances writing to this topic.
  std::unordered_map<std::string, uint32_t> array_expansion_counts_;
};

}  // namespace PJ
