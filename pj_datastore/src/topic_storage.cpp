// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/topic_storage.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include "pj_base/expected.hpp"

namespace {

// Approximate encoded bytes of one sealed chunk: timestamp buffer + every
// column's encoded payload + validity bitmaps. Walked ONCE per chunk (memoized
// into ChunkStats::encoded_byte_size at append) — never on the metadata path.
uint64_t encodedChunkByteSize(const PJ::TopicChunk& chunk) {
  uint64_t bytes = chunk.timestamps.size() * sizeof(PJ::Timestamp);
  for (const auto& col : chunk.columns) {
    std::visit(
        [&](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, PJ::RawBuffer>) {
            bytes += v.size();
          } else if constexpr (std::is_same_v<T, PJ::encoding::DictionaryEncoded>) {
            bytes += v.indices.size();
            for (const auto& s : v.dictionary) {
              bytes += s.size();
            }
          } else if constexpr (std::is_same_v<T, PJ::encoding::PackedBools>) {
            bytes += v.bits.size();
          } else if constexpr (std::is_same_v<T, PJ::encoding::ConstantEncoded>) {
            bytes += v.value_size;
          } else if constexpr (std::is_same_v<T, PJ::encoding::FrameOfReferenceEncoded>) {
            bytes += v.offsets.size();
          }
        },
        col.data);
    if (col.validity_bitmap) {
      bytes += col.validity_bitmap->sizeBytes();
    }
  }
  return bytes;
}

}  // namespace

namespace PJ {

TopicStorage::TopicStorage(TopicId topic_id, TopicDescriptor descriptor)
    : topic_id_(topic_id), descriptor_(std::move(descriptor)) {}

PJ::Status TopicStorage::appendSealedChunk(TopicChunk chunk) {
  // Chunks are stored in commit order. Each chunk is internally sorted, but
  // out-of-order ingest means a chunk's time range may overlap earlier ones —
  // queries merge across overlapping chunks instead of assuming disjoint
  // ranges, so nothing is rejected (rejecting silently lost late data).
  if (chunk.stats.encoded_byte_size == 0) {
    chunk.stats.encoded_byte_size = encodedChunkByteSize(chunk);
  }

  // The append fast path folds the new chunk into the cached aggregate in
  // O(1); extrema only widen, so no rescan is ever needed here.
  if (chunk_agg_valid_) {
    chunk_agg_t_min_ = std::min(chunk_agg_t_min_, chunk.stats.t_min);
    chunk_agg_t_max_ = std::max(chunk_agg_t_max_, chunk.stats.t_max);
    chunk_agg_row_count_ += chunk.stats.row_count;
    chunk_agg_byte_size_ += chunk.stats.encoded_byte_size;
  }

  sealed_chunks_.push_back(std::move(chunk));
  return PJ::okStatus();
}

void TopicStorage::evictBefore(Timestamp t_keep_min) {
  // Chunks are commit-ordered: evict the contiguous prefix whose chunks are
  // entirely older than t_keep_min.
  size_t end_to_remove = 0;
  while (end_to_remove < sealed_chunks_.size() && sealed_chunks_[end_to_remove].stats.t_max < t_keep_min) {
    ++end_to_remove;
  }

  if (end_to_remove > 0) {
    sealed_chunks_.erase(sealed_chunks_.begin(), sealed_chunks_.begin() + static_cast<std::ptrdiff_t>(end_to_remove));
    invalidateChunkAggregate();
  }

  // Raise the logical retention floor to the requested cutoff. Whole-chunk
  // eviction above can only drop chunks entirely older than t_keep_min; a chunk
  // straddling the cutoff stays, but the floor makes its sub-cutoff rows
  // invisible to every read path (timeMin/metadata clamp; readers clamp their
  // lower bound). The floor only rises, so a smaller later cutoff is a no-op.
  // The aggregate stores RAW extrema and the clamp happens at read time, so a
  // floor-only change needs no invalidation.
  retention_floor_ = std::max(retention_floor_, t_keep_min);
}

void TopicStorage::clearChunks() noexcept {
  sealed_chunks_.clear();
  invalidateChunkAggregate();
  // A full replace/reload retains no data, so it must carry no stale floor.
  retention_floor_ = kNoRetentionFloor;
}

TopicStorage::DetachedState TopicStorage::detachChunks() {
  DetachedState detached;
  // Every allocating copy happens FIRST: if one throws, the chunks are still owned by this
  // storage and nothing is lost. Only once the snapshot is complete does the deque move out.
  detached.column_descriptors = column_descriptors_;
  detached.array_expansion_counts = array_expansion_counts_;
  detached.retention_floor = retention_floor_;
  detached.schema_id = descriptor_.schema_id;
  detached.max_observed_array_length = max_observed_array_length_;
  detached.truncated_sample_count = truncated_sample_count_;

  detached.chunks = std::move(sealed_chunks_);
  // A moved-from deque is valid but unspecified: normalize through clearChunks(), which also
  // drops the stale aggregate and the retention floor exactly like a plain clear would.
  clearChunks();
  return detached;
}

void TopicStorage::restoreChunks(DetachedState&& detached) noexcept {
  // noexcept holds because every member below is move-assigned (no allocation) — the
  // asserts pin that for the container members, whose move-assign is only conditionally
  // noexcept.
  static_assert(std::is_nothrow_move_assignable_v<std::deque<TopicChunk>>);
  static_assert(std::is_nothrow_move_assignable_v<std::vector<ColumnDescriptor>>);
  static_assert(std::is_nothrow_move_assignable_v<std::unordered_map<std::string, uint32_t>>);

  sealed_chunks_ = std::move(detached.chunks);
  invalidateChunkAggregate();
  column_descriptors_ = std::move(detached.column_descriptors);
  retention_floor_ = detached.retention_floor;
  descriptor_.schema_id = detached.schema_id;
  max_observed_array_length_ = detached.max_observed_array_length;
  truncated_sample_count_ = detached.truncated_sample_count;
  array_expansion_counts_ = std::move(detached.array_expansion_counts);
}

void TopicStorage::ensureChunkAggregate() const noexcept {
  if (chunk_agg_valid_) {
    return;
  }
  chunk_agg_t_min_ = 0;
  chunk_agg_t_max_ = 0;
  chunk_agg_row_count_ = 0;
  chunk_agg_byte_size_ = 0;
  if (!sealed_chunks_.empty()) {
    chunk_agg_t_min_ = sealed_chunks_.front().stats.t_min;
    chunk_agg_t_max_ = sealed_chunks_.back().stats.t_max;
    for (const auto& chunk : sealed_chunks_) {
      chunk_agg_t_min_ = std::min(chunk_agg_t_min_, chunk.stats.t_min);
      chunk_agg_t_max_ = std::max(chunk_agg_t_max_, chunk.stats.t_max);
      chunk_agg_row_count_ += chunk.stats.row_count;
      chunk_agg_byte_size_ += chunk.stats.encoded_byte_size;
    }
  }
  chunk_agg_valid_ = true;
}

void TopicStorage::setColumnDescriptors(std::vector<ColumnDescriptor> descs) noexcept {
  column_descriptors_ = std::move(descs);
}

const std::vector<ColumnDescriptor>& TopicStorage::columnDescriptors() const noexcept {
  return column_descriptors_;
}

const std::deque<TopicChunk>& TopicStorage::sealedChunks() const noexcept {
  return sealed_chunks_;
}

TopicMetadata TopicStorage::metadata() const {
  TopicMetadata meta;
  meta.topic_id = topic_id_;
  meta.name = descriptor_.name;
  meta.current_schema = descriptor_.schema_id;
  meta.dataset_id = descriptor_.dataset_id;

  meta.max_observed_array_length = max_observed_array_length_;
  meta.truncated_sample_count = truncated_sample_count_;

  if (sealed_chunks_.empty()) {
    return meta;
  }

  ensureChunkAggregate();
  meta.time_range_min = chunk_agg_t_min_;
  meta.time_range_max = chunk_agg_t_max_;
  meta.total_row_count = chunk_agg_row_count_;
  meta.total_byte_size = chunk_agg_byte_size_;

  // Clamp the reported minimum up to the retention floor: a straddling chunk's
  // sub-floor rows are logically evicted, so the catalog/axis must not see them.
  meta.time_range_min = std::max(meta.time_range_min, retention_floor_);

  return meta;
}

const TopicDescriptor& TopicStorage::descriptor() const noexcept {
  return descriptor_;
}

TopicId TopicStorage::topicId() const noexcept {
  return topic_id_;
}

bool TopicStorage::empty() const noexcept {
  return sealed_chunks_.empty();
}

Timestamp TopicStorage::timeMin() const noexcept {
  if (sealed_chunks_.empty()) {
    return 0;
  }
  ensureChunkAggregate();
  // Clamp up to the retention floor: rows below it are logically evicted even
  // when a straddling chunk still physically holds them.
  return std::max(chunk_agg_t_min_, retention_floor_);
}

Timestamp TopicStorage::timeMax() const noexcept {
  if (sealed_chunks_.empty()) {
    return 0;
  }
  ensureChunkAggregate();
  return chunk_agg_t_max_;
}

Timestamp TopicStorage::retentionFloor() const noexcept {
  return retention_floor_;
}

void TopicStorage::updateSchema(SchemaId new_schema) {
  descriptor_.schema_id = new_schema;
}

void TopicStorage::updateMaxObservedArrayLength(uint32_t observed_length) {
  if (observed_length > max_observed_array_length_) {
    max_observed_array_length_ = observed_length;
  }
}

void TopicStorage::incrementTruncatedSampleCount() {
  ++truncated_sample_count_;
}

uint32_t TopicStorage::maxObservedArrayLength() const noexcept {
  return max_observed_array_length_;
}

uint32_t TopicStorage::truncatedSampleCount() const noexcept {
  return truncated_sample_count_;
}

uint32_t TopicStorage::arrayExpansionCount(const std::string& field_path) const noexcept {
  auto it = array_expansion_counts_.find(field_path);
  return it != array_expansion_counts_.end() ? it->second : 0;
}

void TopicStorage::setArrayExpansionCount(const std::string& field_path, uint32_t count) {
  array_expansion_counts_[field_path] = count;
}

}  // namespace PJ
