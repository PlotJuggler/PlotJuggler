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

// Memoized into ChunkStats::encoded_byte_size at append, so the metadata path never walks a chunk.
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

// What a chunk says about one float column: nothing (not a float column, or no value of it), whole numbers only, or
// some fraction.
constexpr uint8_t kNoWholeEvidence = 0;
constexpr uint8_t kWholeEvidence = 1;
constexpr uint8_t kFractionalEvidence = 2;

[[nodiscard]] uint8_t wholeEvidence(const PJ::TopicChunk& chunk, std::size_t c) {
  const auto& descriptor = chunk.columns[c].descriptor;
  if (descriptor == nullptr) {
    return kNoWholeEvidence;
  }
  const PJ::StorageKind kind = PJ::storageKindOf(descriptor->logical_type);
  if (!PJ::isFloatKind(kind)) {
    return kNoWholeEvidence;
  }
  if (c < chunk.stats.column_stats.size() && chunk.stats.column_stats[c].null_count >= chunk.stats.row_count) {
    return kNoWholeEvidence;  // an all-null chunk holds no value of the field: no evidence either way
  }
  const bool fractional = c >= chunk.stats.column_stats.size() || !chunk.stats.column_stats[c].all_integral;
  return fractional ? kFractionalEvidence : kWholeEvidence;
}

}  // namespace

namespace PJ {

TopicStorage::TopicStorage(TopicId topic_id, TopicDescriptor descriptor)
    : topic_id_(topic_id), descriptor_(std::move(descriptor)) {}

PJ::Status TopicStorage::appendSealedChunk(TopicChunk chunk) {
  // Commit order: after out-of-order ingest a chunk may overlap earlier ones. Nothing is rejected; queries merge
  // across overlapping chunks.
  if (chunk.stats.encoded_byte_size == 0) {
    chunk.stats.encoded_byte_size = encodedChunkByteSize(chunk);
  }
  // The chunk that stops being the last will not gain rows again: if appendRows() left it unencoded, it is encoded
  // now, or it would stay stored raw.
  compactLastChunk();
  // Derived nodes skip chunks whose id is at or below what they consumed, so ids must ascend in commit order. A chunk
  // sealed before one this topic held (another writer held it, or a flush moved it) gets a fresh id.
  if (chunk.id <= newest_chunk_id_ever_ || chunk.id <= evicted_through_chunk_id_ ||
      (!sealed_chunks_.empty() && chunk.id <= sealed_chunks_.back().id)) {
    chunk.id = TopicChunkBuilder::nextChunkId();
  }
  newest_chunk_id_ever_ = chunk.id;
  // New to this topic, whatever it replaced where it came from (another engine's topic, on a flush or a reload).
  chunk.replaced_id = 0;

  // Only a chunk strictly after every existing sample keeps the series generation: incremental consumers absorb it,
  // any overlap makes their caches rebuild.
  if (!sealed_chunks_.empty() && chunk.stats.t_min <= timeMax()) {
    ++series_generation_;
  }

  // An append only widens the extrema, so the cached aggregate absorbs it without a rescan.
  foldChunkAggregate(chunk.stats, 0, 0);
  if (whole_agg_valid_) {
    foldWholeNumbers(chunk, +1);
  }

  unretireColumnsWithValues(chunk);
  sealed_chunks_.push_back(std::move(chunk));
  return PJ::okStatus();
}

void TopicStorage::foldChunkAggregate(const ChunkStats& stats, uint32_t rows_before, uint64_t bytes_before) {
  if (chunk_agg_valid_) {
    chunk_agg_t_min_ = std::min(chunk_agg_t_min_, stats.t_min);
    chunk_agg_t_max_ = std::max(chunk_agg_t_max_, stats.t_max);
    chunk_agg_row_count_ += stats.row_count - rows_before;
    chunk_agg_byte_size_ += stats.encoded_byte_size - bytes_before;
  }
}

template <typename Change>
void TopicStorage::changeStoredChunk(TopicChunk& chunk, Change&& change) {
  // The whole-number aggregate counts chunks per field: a column whose evidence the change moves is taken out as it
  // was and put back as it is.
  std::vector<uint8_t> evidence_before;
  if (whole_agg_valid_) {
    evidence_before.reserve(chunk.columns.size());
    for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
      evidence_before.push_back(wholeEvidence(chunk, c));
    }
  }
  const uint64_t bytes_before = chunk.stats.encoded_byte_size;
  const uint32_t rows_before = chunk.stats.row_count;

  std::forward<Change>(change)();
  chunk.stats.encoded_byte_size = encodedChunkByteSize(chunk);

  foldChunkAggregate(chunk.stats, rows_before, bytes_before);
  if (whole_agg_valid_) {
    for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
      const uint8_t evidence = wholeEvidence(chunk, c);
      if (evidence != evidence_before[c]) {
        const FieldId field_id = chunk.columns[c].descriptor->field_id;
        foldWholeEvidence(field_id, evidence_before[c], -1);
        foldWholeEvidence(field_id, evidence, +1);
      }
    }
  }
}

void TopicStorage::extendLastChunk(
    TopicChunk& chunk, const TopicChunkBuilder& batch, std::size_t first, std::size_t count) {
  // As for a new chunk: only rows strictly after every existing sample keep the series generation.
  if (batch.timestampAt(first) <= timeMax()) {
    ++series_generation_;
  }
  changeStoredChunk(chunk, [&] {
    chunk.makeGrowable();
    // A chunk put back by a restore after newer ones were read: under its old id its new rows would not be seen.
    if (chunk.id < newest_chunk_id_ever_) {
      chunk.replaced_id = chunk.firstId();
      chunk.id = TopicChunkBuilder::nextChunkId();
    }
    newest_chunk_id_ever_ = chunk.id;
    batch.appendRowsTo(chunk, first, count);
  });
  unretireColumnsWithValues(chunk);
}

void TopicStorage::compactChunk(TopicChunk& chunk) {
  changeStoredChunk(chunk, [&chunk] { chunk.compact(); });
}

void TopicStorage::mergeIntoLastChunk(TopicChunk& chunk, const TopicChunkBuilder& batch) {
  ++series_generation_;  // rows land behind rows readers hold
  changeStoredChunk(chunk, [&] {
    chunk.makeGrowable();
    const ChunkId first_id = chunk.firstId();
    chunk = batch.mergedWith(chunk);  // a new id: rows moved
    chunk.replaced_id = first_id;
    newest_chunk_id_ever_ = chunk.id;
  });
  unretireColumnsWithValues(chunk);
}

void TopicStorage::appendRows(TopicChunkBuilder& batch) {
  // ponytail: a late merge rewrites the whole last chunk, so it is bounded: past this many cells the late rows start a
  // chunk of their own, as they always did. Merge only the rows behind the first late one if wide topics with late
  // rows show up.
  constexpr std::size_t kLateMergeCells = 4'000'000;
  batch.sortRows();
  const std::size_t total = batch.rowCount();
  const std::size_t max_rows = std::max<uint32_t>(descriptor_.max_chunk_rows, 1);
  const auto compact_if_full = [&](TopicChunk& chunk) {
    if (chunk.stats.row_count >= max_rows) {
      compactChunk(chunk);
    }
  };
  std::size_t first = 0;
  while (first < total) {
    TopicChunk* last = sealed_chunks_.empty() ? nullptr : &sealed_chunks_.back();
    // Rows of another layout never join a chunk: a reader tells a column a chunk lacks from a null in it.
    const bool has_room = last != nullptr && last->stats.row_count < max_rows && batch.hasLayoutOf(*last);
    if (has_room && batch.timestampAt(first) >= last->stats.t_max) {
      const std::size_t count = std::min(total - first, max_rows - last->stats.row_count);
      extendLastChunk(*last, batch, first, count);
      compact_if_full(*last);
      first += count;
      continue;
    }
    // Rows older than the last chunk's newest would leave it unsorted: merge them in under a new id (`replaced_id`
    // keeps the old). Too large or another layout: they start a chunk that overlaps it.
    if (has_room && first == 0 && last->stats.row_count + total <= max_rows &&
        (last->stats.row_count + total) * batch.columnCount() <= kLateMergeCells) {
      mergeIntoLastChunk(*last, batch);
      compact_if_full(*last);
      return;
    }
    const std::size_t count = std::min(total - first, max_rows);
    if (first == 0 && count == total && count >= max_rows) {
      (void)appendSealedChunk(batch.seal());  // a whole chunk already: encoded straight from the builder
      return;
    }
    TopicChunk chunk = batch.makeGrowableChunk();
    batch.appendRowsTo(chunk, first, count);
    if (count >= max_rows) {
      chunk.compact();
    }
    (void)appendSealedChunk(std::move(chunk));
    first += count;
  }
}

void TopicStorage::compactLastChunk() {
  if (sealed_chunks_.empty()) {
    return;
  }
  TopicChunk& last = sealed_chunks_.back();
  if (last.stats.row_count < descriptor_.max_chunk_rows && last.isGrowable()) {
    compactChunk(last);
  }
}

void TopicStorage::evictBefore(Timestamp t_keep_min) {
  // Chunks are in commit order, not time order: only the leading run entirely older than t_keep_min goes.
  size_t end_to_remove = 0;
  while (end_to_remove < sealed_chunks_.size() && sealed_chunks_[end_to_remove].stats.t_max < t_keep_min) {
    evicted_through_chunk_id_ = std::max(evicted_through_chunk_id_, sealed_chunks_[end_to_remove].id);
    ++end_to_remove;
  }

  if (end_to_remove > 0) {
    // Retention runs often while streaming: subtract instead of rebuilding.
    if (whole_agg_valid_) {
      for (std::size_t k = 0; k < end_to_remove; ++k) {
        foldWholeNumbers(sealed_chunks_[k], -1);
      }
    }
    sealed_chunks_.erase(sealed_chunks_.begin(), sealed_chunks_.begin() + static_cast<std::ptrdiff_t>(end_to_remove));
    chunk_agg_valid_ = false;
  }

  // A front chunk straddling the cutoff counts as evicted: its sub-cutoff rows are lost to a consumer that has not
  // read it.
  if (!sealed_chunks_.empty() && sealed_chunks_.front().stats.t_min < t_keep_min) {
    evicted_through_chunk_id_ = std::max(evicted_through_chunk_id_, sealed_chunks_.front().id);
  }

  // Read paths clamp their lower bound to the floor, which hides a straddling chunk's sub-cutoff rows; it only rises.
  // The aggregate keeps raw extrema and is clamped at read time, so a floor change needs no invalidation.
  retention_floor_ = std::max(retention_floor_, t_keep_min);
}

void TopicStorage::clearChunks() noexcept {
  sealed_chunks_.clear();
  invalidateChunkAggregate();
  ++series_generation_;
  // A stale floor would hide refilled rows below it.
  retention_floor_ = kNoRetentionFloor;
  evicted_through_chunk_id_ = 0;
}

TopicStorage::DetachedState TopicStorage::detachChunks() {
  DetachedState detached;
  // Every copy that can throw comes before the deque moves out, so a throw loses no chunks.
  detached.column_descriptors = column_descriptors_;
  detached.array_expansion_counts = array_expansion_counts_;
  detached.retired_columns = retired_columns_;
  detached.retention_floor = retention_floor_;
  detached.evicted_through_chunk_id = evicted_through_chunk_id_;
  detached.schema_id = descriptor_.schema_id;
  detached.max_observed_array_length = max_observed_array_length_;
  detached.truncated_sample_count = truncated_sample_count_;

  detached.chunks = std::move(sealed_chunks_);
  // A moved-from deque is unspecified; clearChunks() also resets the aggregate and the floor.
  clearChunks();
  return detached;
}

void TopicStorage::restoreChunks(DetachedState&& detached) noexcept {
  // noexcept rests on move-assignment only; the asserts pin it for containers whose move-assign is conditionally
  // noexcept.
  static_assert(std::is_nothrow_move_assignable_v<std::deque<TopicChunk>>);
  static_assert(std::is_nothrow_move_assignable_v<std::vector<ColumnDescriptor>>);
  static_assert(std::is_nothrow_move_assignable_v<std::unordered_map<std::string, uint32_t>>);
  static_assert(std::is_nothrow_move_assignable_v<std::unordered_set<FieldId>>);

  sealed_chunks_ = std::move(detached.chunks);
  invalidateChunkAggregate();
  ++series_generation_;
  column_descriptors_ = std::move(detached.column_descriptors);
  retention_floor_ = detached.retention_floor;
  evicted_through_chunk_id_ = detached.evicted_through_chunk_id;
  descriptor_.schema_id = detached.schema_id;
  max_observed_array_length_ = detached.max_observed_array_length;
  truncated_sample_count_ = detached.truncated_sample_count;
  array_expansion_counts_ = std::move(detached.array_expansion_counts);
  retired_columns_ = std::move(detached.retired_columns);
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

namespace {
[[nodiscard]] uint64_t wholeNumberEntryHash(FieldId field_id, bool whole) noexcept {
  uint64_t x = (static_cast<uint64_t>(field_id) << 1 | (whole ? 1U : 0U)) + 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}
}  // namespace

void TopicStorage::foldWholeEvidence(FieldId field_id, uint8_t evidence, int direction) const {
  if (evidence == kNoWholeEvidence) {
    return;
  }
  auto& counts = whole_agg_[field_id];
  if (counts.chunks > 0) {
    whole_agg_signature_ -= wholeNumberEntryHash(field_id, counts.fractional_chunks == 0);
  }
  counts.chunks += static_cast<uint32_t>(direction);
  counts.fractional_chunks += evidence == kFractionalEvidence ? static_cast<uint32_t>(direction) : 0U;
  if (counts.chunks > 0) {
    whole_agg_signature_ += wholeNumberEntryHash(field_id, counts.fractional_chunks == 0);
  } else {
    whole_agg_.erase(field_id);
  }
}

void TopicStorage::foldWholeNumbers(const TopicChunk& chunk, int direction) const {
  for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
    if (const uint8_t evidence = wholeEvidence(chunk, c); evidence != kNoWholeEvidence) {
      foldWholeEvidence(chunk.columns[c].descriptor->field_id, evidence, direction);
    }
  }
}

void TopicStorage::ensureWholeNumberAggregate() const {
  if (whole_agg_valid_) {
    return;
  }
  whole_agg_.clear();
  whole_agg_signature_ = 0;
  for (const auto& chunk : sealed_chunks_) {
    foldWholeNumbers(chunk, +1);
  }
  whole_agg_valid_ = true;
}

std::optional<bool> TopicStorage::wholeNumbersOnly(FieldId field_id) const {
  ensureWholeNumberAggregate();
  const auto it = whole_agg_.find(field_id);
  return it == whole_agg_.end() ? std::nullopt : std::optional<bool>(it->second.fractional_chunks == 0);
}

uint64_t TopicStorage::wholeNumbersSignature() const {
  ensureWholeNumberAggregate();
  return whole_agg_signature_;
}

void TopicStorage::setColumnDescriptors(std::vector<ColumnDescriptor> descs) noexcept {
  column_descriptors_ = std::move(descs);
}

void TopicStorage::addColumnDescriptor(ColumnDescriptor desc) {
  column_descriptors_.push_back(std::move(desc));
}

const std::vector<ColumnDescriptor>& TopicStorage::columnDescriptors() const noexcept {
  return column_descriptors_;
}

void TopicStorage::retireValuelessColumns() {
  retired_columns_.clear();
  for (const ColumnDescriptor& column : column_descriptors_) {
    retired_columns_.insert(column.field_id);
  }
  for (const TopicChunk& chunk : sealed_chunks_) {
    unretireColumnsWithValues(chunk);
  }
}

std::size_t TopicStorage::firstLiveColumn() const noexcept {
  for (std::size_t position = 0; position < column_descriptors_.size(); ++position) {
    if (!isColumnRetired(column_descriptors_[position].field_id)) {
      return position;
    }
  }
  return 0;
}

void TopicStorage::unretireColumnsWithValues(const TopicChunk& chunk) {
  for (std::size_t c = 0; c < chunk.columns.size() && !retired_columns_.empty(); ++c) {
    if (chunk.columns[c].descriptor != nullptr && c < chunk.stats.column_stats.size() &&
        chunk.stats.column_stats[c].null_count < chunk.stats.row_count) {
      retired_columns_.erase(chunk.columns[c].descriptor->field_id);
    }
  }
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

ChunkId TopicStorage::evictedThroughChunkId() const noexcept {
  return evicted_through_chunk_id_;
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

void TopicStorage::setFieldMetadata(FieldId field_id, std::string metadata_json) {
  if (metadata_json.empty()) {
    field_metadata_.erase(field_id);
    return;
  }
  field_metadata_[field_id] = std::move(metadata_json);
}

std::string_view TopicStorage::fieldMetadata(FieldId field_id) const noexcept {
  const auto it = field_metadata_.find(field_id);
  return it != field_metadata_.end() ? std::string_view(it->second) : std::string_view();
}

void TopicStorage::setArrayExpansionCount(const std::string& field_path, uint32_t count) {
  array_expansion_counts_[field_path] = count;
}

}  // namespace PJ
