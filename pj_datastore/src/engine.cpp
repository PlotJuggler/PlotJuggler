// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/engine.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {

struct DataEngine::Impl {
  TypeRegistry type_registry;
  PJ::DatasetId next_dataset_id = 1;
  PJ::TopicId next_topic_id = 1;
  PJ::TimeDomainId next_time_domain_id = 1;
  tsl::robin_map<PJ::DatasetId, PJ::DatasetInfo> datasets;
  // unique_ptr keeps each TopicStorage at a stable address across rehashes; with topics erased only by
  // removeDataset(), that is what lets a worker append under the engine lock without invalidating a GUI-cached
  // TopicChunk*.
  tsl::robin_map<PJ::TopicId, std::unique_ptr<TopicStorage>> topics;
  tsl::robin_map<PJ::TimeDomainId, PJ::TimeDomain> time_domains;
  // Hidden from listTopics(); their storage stays so a cached reader sees an empty deque, not freed memory.
  std::unordered_set<PJ::TopicId> retired_topic_ids;
  // DatasetInfo is an SDK struct, so the labels live beside it; see DataEngine::datasetLabel().
  struct Label {
    std::string base;  // normalized; setDatasetLabel() from the same base keeps `text`
    std::string text;
  };
  tsl::robin_map<PJ::DatasetId, Label> labels;
  // Source a mergeDatasets() consumed -> the anchor it was folded into; see DataEngine::isMergedAway().
  tsl::robin_map<PJ::DatasetId, PJ::DatasetId> merged_into;

  // Guards every field above; contract on DataEngine::lockEngine().
  mutable std::recursive_mutex mutex_;
};

DataEngine::DataEngine() : impl_(std::make_unique<Impl>()) {}

DataEngine::~DataEngine() = default;

DataEngine::DataEngine(DataEngine&&) noexcept = default;

DataEngine& DataEngine::operator=(DataEngine&&) noexcept = default;

std::unique_lock<std::recursive_mutex> DataEngine::lockEngine() const {
  return std::unique_lock<std::recursive_mutex>(impl_->mutex_);
}

std::unique_lock<std::recursive_mutex> DataEngine::lockEngineDeferred() const {
  return std::unique_lock<std::recursive_mutex>(impl_->mutex_, std::defer_lock);
}

// ---------------------------------------------------------------------------
// Dataset management
// ---------------------------------------------------------------------------

namespace {

// The base of a label: `base`, or the source name when it is empty, '/' read as '_', "Dataset" when still empty.
std::string normalizedLabelBase(std::string_view base, std::string_view source_name) {
  std::string label(base.empty() ? source_name : base);
  std::replace(label.begin(), label.end(), '/', '_');
  if (label.empty()) {
    label = "Dataset";
  }
  return label;
}

// The datasetLabel() rule over a normalized base; `self` is the dataset being labelled, whose current label does not
// count as taken.
template <typename Labels>
std::string uniqueLabel(const Labels& labels, const std::string& base, DatasetId self) {
  const auto taken = [&](const std::string& candidate) {
    return std::any_of(labels.begin(), labels.end(), [&](const auto& entry) {
      return entry.first != self && entry.second.text == candidate;
    });
  };
  // ponytail: linear scans over the loaded datasets (a handful); index by label if sessions reach thousands.
  std::string candidate = base;
  for (int ordinal = 2; taken(candidate); ++ordinal) {
    candidate = fmt::format("{} ({})", base, ordinal);
  }
  return candidate;
}

}  // namespace

Expected<DatasetId> DataEngine::createDataset(DatasetDescriptor descriptor, DatasetId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  DatasetId id = requested_id != 0 ? requested_id : impl_->next_dataset_id;
  if (requested_id != 0) {
    if (impl_->datasets.find(requested_id) != impl_->datasets.end()) {
      return PJ::unexpected(fmt::format("Dataset {} already exists", requested_id));
    }
  }

  if (descriptor.time_domain_id != 0) {
    auto it = impl_->time_domains.find(descriptor.time_domain_id);
    if (it == impl_->time_domains.end()) {
      return PJ::unexpected(fmt::format("Time domain {} not found", descriptor.time_domain_id));
    }
  }
  if (requested_id != 0) {
    if (impl_->next_dataset_id <= id) {
      impl_->next_dataset_id = id + 1;
    }
  } else {
    ++impl_->next_dataset_id;
  }

  DatasetInfo info;
  info.id = id;
  info.source_name = std::move(descriptor.source_name);
  if (descriptor.time_domain_id != 0) {
    info.time_domain = impl_->time_domains.at(descriptor.time_domain_id);
  }
  std::string base = normalizedLabelBase({}, info.source_name);
  std::string text = uniqueLabel(impl_->labels, base, id);
  impl_->labels.insert_or_assign(id, Impl::Label{.base = std::move(base), .text = std::move(text)});
  impl_->datasets.emplace(id, std::move(info));
  return id;
}

std::string DataEngine::datasetLabel(DatasetId id) const {
  const auto lock = lockEngine();
  const auto it = impl_->labels.find(id);
  return it != impl_->labels.end() ? it->second.text : std::string{};
}

std::optional<DatasetId> DataEngine::datasetByLabel(std::string_view label) const {
  const auto lock = lockEngine();
  const auto held = std::find_if(impl_->labels.begin(), impl_->labels.end(), [label](const auto& entry) {
    return !label.empty() && entry.second.text == label;
  });
  if (held == impl_->labels.end()) {
    return std::nullopt;
  }
  DatasetId id = held->first;
  for (auto merged = impl_->merged_into.find(id); merged != impl_->merged_into.end();
       merged = impl_->merged_into.find(id)) {
    id = merged->second;
  }
  return impl_->datasets.contains(id) ? std::optional{id} : std::nullopt;
}

std::vector<std::string> DataEngine::datasetLabels() const {
  const auto lock = lockEngine();
  std::vector<std::string> labels;
  labels.reserve(impl_->labels.size());
  for (const auto& [id, label] : impl_->labels) {
    labels.push_back(label.text);
  }
  return labels;
}

std::string DataEngine::setDatasetLabel(DatasetId id, std::string_view base) {
  const auto lock = lockEngine();
  const auto dataset = impl_->datasets.find(id);
  if (dataset == impl_->datasets.end()) {
    return {};
  }
  Impl::Label& label = impl_->labels[id];
  std::string normalized = normalizedLabelBase(base, dataset->second.source_name);
  if (normalized != label.base) {
    label.text = uniqueLabel(impl_->labels, normalized, id);
    label.base = std::move(normalized);
  }
  return label.text;
}

bool DataEngine::isMergedAway(DatasetId id) const {
  const auto lock = lockEngine();
  return impl_->merged_into.contains(id);
}

const DatasetInfo* DataEngine::getDataset(DatasetId id) const {
  auto it = impl_->datasets.find(id);
  if (it == impl_->datasets.end()) {
    return nullptr;
  }
  return &it->second;
}

// ---------------------------------------------------------------------------
// Topic management
// ---------------------------------------------------------------------------

Expected<TopicId> DataEngine::createTopic(DatasetId dataset_id, TopicDescriptor descriptor, TopicId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return createTopicLocked(dataset_id, std::move(descriptor), requested_id);
}

Expected<TopicId> DataEngine::createTopicLocked(
    DatasetId dataset_id, TopicDescriptor descriptor, TopicId requested_id) {
  auto it = impl_->datasets.find(dataset_id);
  if (it == impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("Dataset {} not found", dataset_id));
  }

  // schema_id 0 means inline columns (scalar series).
  if (descriptor.schema_id != 0) {
    if (impl_->type_registry.lookup(descriptor.schema_id) == nullptr) {
      return PJ::unexpected(fmt::format("Schema {} not found", descriptor.schema_id));
    }
  }

  TopicId id = requested_id != 0 ? requested_id : impl_->next_topic_id;
  if (requested_id != 0) {
    if (impl_->topics.find(requested_id) != impl_->topics.end()) {
      return PJ::unexpected(fmt::format("Topic {} already exists", requested_id));
    }
    if (impl_->next_topic_id <= id) {
      impl_->next_topic_id = id + 1;
    }
  } else {
    ++impl_->next_topic_id;
  }
  descriptor.dataset_id = dataset_id;
  impl_->topics.emplace(id, std::make_unique<TopicStorage>(id, std::move(descriptor)));
  it.value().topic_ids.push_back(id);
  return id;
}

Expected<FieldId> DataEngine::createTopicField(
    TopicId topic_id, std::string_view field_name, PrimitiveType type, std::optional<FieldId> requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto topic_it = impl_->topics.find(topic_id);
  if (topic_it == impl_->topics.end()) {
    return PJ::unexpected(fmt::format("createTopicField: topic {} not found", topic_id));
  }
  TopicStorage& storage = *topic_it.value();

  // A different type for an existing name means two registrations disagree on the shape.
  const auto& cols = storage.columnDescriptors();
  for (const auto& col : cols) {
    if (col.field_path == field_name) {
      if (col.logical_type != type) {
        return PJ::unexpected(
            fmt::format("createTopicField: field '{}' already exists with a different type", field_name));
      }
      if (requested_id.has_value() && *requested_id != col.field_id) {
        return PJ::unexpected(
            fmt::format(
                "createTopicField: field '{}' already exists with id {} but requested {}", field_name, col.field_id,
                *requested_id));
      }
      return col.field_id;
    }
  }

  // A forced id of 0 is legitimate (the first field), hence std::nullopt rather than 0 for "auto".
  const FieldId next_id = static_cast<FieldId>(cols.size());
  if (requested_id.has_value() && *requested_id != next_id) {
    return PJ::unexpected(
        fmt::format(
            "createTopicField: requested_id {} would create a non-dense field layout "
            "(topic {} currently has {} columns; next dense id is {})",
            *requested_id, topic_id, cols.size(), next_id));
  }

  std::vector<ColumnDescriptor> new_cols = cols;
  ColumnDescriptor desc;
  desc.field_id = next_id;
  desc.logical_type = type;
  desc.field_path = std::string(field_name);
  new_cols.push_back(std::move(desc));
  storage.setColumnDescriptors(std::move(new_cols));
  return next_id;
}

TopicStorage* DataEngine::getTopicStorage(TopicId id) {
  auto it = impl_->topics.find(id);
  if (it == impl_->topics.end()) {
    return nullptr;
  }
  return it.value().get();
}

const TopicStorage* DataEngine::getTopicStorage(TopicId id) const {
  auto it = impl_->topics.find(id);
  if (it == impl_->topics.end()) {
    return nullptr;
  }
  return it->second.get();
}

// ---------------------------------------------------------------------------
// Schema registry
// ---------------------------------------------------------------------------

TypeRegistry& DataEngine::typeRegistry() {
  return impl_->type_registry;
}

const TypeRegistry& DataEngine::typeRegistry() const {
  return impl_->type_registry;
}

// ---------------------------------------------------------------------------
// Time domains
// ---------------------------------------------------------------------------

Expected<TimeDomainId> DataEngine::createTimeDomain(std::string name, TimeDomainId requested_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  TimeDomainId id = requested_id != 0 ? requested_id : impl_->next_time_domain_id;
  if (requested_id != 0) {
    if (impl_->time_domains.find(requested_id) != impl_->time_domains.end()) {
      return PJ::unexpected(fmt::format("Time domain {} already exists", requested_id));
    }
    if (impl_->next_time_domain_id <= id) {
      impl_->next_time_domain_id = id + 1;
    }
  } else {
    ++impl_->next_time_domain_id;
  }
  TimeDomain td;
  td.id = id;
  td.name = std::move(name);
  impl_->time_domains.emplace(id, std::move(td));
  return id;
}

const TimeDomain* DataEngine::getTimeDomain(TimeDomainId id) const {
  auto it = impl_->time_domains.find(id);
  if (it == impl_->time_domains.end()) {
    return nullptr;
  }
  return &it->second;
}

void DataEngine::setDisplayOffset(TimeDomainId id, Timestamp offset) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto it = impl_->time_domains.find(id);
  if (it != impl_->time_domains.end()) {
    it.value().display_offset = offset;
  }
}

// ---------------------------------------------------------------------------
// Commit cycle
// ---------------------------------------------------------------------------

std::vector<TopicId> DataEngine::commitChunks(
    std::vector<std::pair<TopicId, TopicChunk>> chunks) {  // NOLINT(performance-unnecessary-value-param)
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return commitChunksLocked(std::move(chunks));
}

std::vector<TopicId> DataEngine::commitChunksLocked(
    std::vector<std::pair<TopicId, TopicChunk>> chunks) {  // NOLINT(performance-unnecessary-value-param)
  std::vector<TopicId> changed;
  for (auto& [topic_id, chunk] : chunks) {
    auto* storage = getTopicStorage(topic_id);
    if (storage != nullptr) {
      auto status = storage->appendSealedChunk(std::move(chunk));
      if (!status.has_value()) {
        continue;  // overlapping chunks are accepted; only a real append failure lands here
      }
      // A recomputed filter output retired by a reload reappears once fresh chunks land.
      impl_->retired_topic_ids.erase(topic_id);
      if (changed.empty() || changed.back() != topic_id) {
        changed.push_back(topic_id);
      }
    }
  }
  // flushAll() may emit several chunks for one topic.
  std::sort(changed.begin(), changed.end());
  changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
  return changed;
}

void DataEngine::enforceRetention(Timestamp retention_window_ns) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& storage = *it.value();
    if (!storage.empty()) {
      Timestamp t_max = storage.timeMax();
      storage.evictBefore(saturatingSub(t_max, retention_window_ns));
    }
  }
}

void DataEngine::enforceRetention(Timestamp retention_window_ns, DatasetId dataset_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& storage = *it.value();
    if (storage.descriptor().dataset_id != dataset_id || storage.empty()) {
      continue;
    }
    Timestamp t_max = storage.timeMax();
    storage.evictBefore(saturatingSub(t_max, retention_window_ns));
  }
}

void DataEngine::evictTopicHistory(TopicId topic_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto it = impl_->topics.find(topic_id);
  if (it == impl_->topics.end()) {
    return;
  }
  auto& storage = *it.value();
  if (!storage.empty()) {
    storage.evictBefore(saturatingAdd(storage.timeMax(), 1));
  }
}

Expected<TopicId> DataEngine::createColumnGroupTopic(
    DatasetId dataset_id, const std::string& topic_name, const std::string& schema_name,
    const std::vector<GroupColumn>& columns) {
  std::vector<std::shared_ptr<TypeTreeNode>> fields;
  fields.reserve(columns.size());
  for (const GroupColumn& column : columns) {
    fields.push_back(makePrimitive(column.name, column.type));
  }
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);  // the registry is unsynchronized
  auto schema = impl_->type_registry.registerOrGet(schema_name, makeStruct(schema_name, std::move(fields)));
  if (!schema.has_value()) {
    return unexpected(schema.error());
  }
  return createTopicLocked(
      dataset_id, TopicDescriptor{.name = topic_name, .schema_id = *schema, .dataset_id = dataset_id}, 0);
}

void DataEngine::clearTopicHistory(TopicId topic_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  if (auto it = impl_->topics.find(topic_id); it != impl_->topics.end()) {
    it.value()->clearChunks();
  }
}

void DataEngine::retireTopic(TopicId topic_id) {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  auto it = impl_->topics.find(topic_id);
  if (it == impl_->topics.end()) {
    return;
  }
  // Safe to free: adapters create a fresh reader per access, so no TopicChunk* survives.
  it.value()->clearChunks();
  impl_->retired_topic_ids.insert(topic_id);
}

void DataEngine::removeDataset(DatasetId dataset_id) {
  // A worker may be ingesting another dataset meanwhile.
  auto lock = lockEngine();
  auto ds_it = impl_->datasets.find(dataset_id);
  if (ds_it == impl_->datasets.end()) {
    return;
  }
  for (const TopicId tid : ds_it->second.topic_ids) {
    impl_->topics.erase(tid);
    impl_->retired_topic_ids.erase(tid);
  }
  impl_->datasets.erase(ds_it);
  impl_->labels.erase(dataset_id);
  impl_->merged_into.erase(dataset_id);
  // next_*_id are not rewound, so a later dataset or topic never aliases an erased one.
}

void DataEngine::clearDatasetChunks(DatasetId dataset_id) {
  auto lock = lockEngine();
  for (const TopicId topic_id : listTopicsLocked(dataset_id)) {
    if (TopicStorage* storage = getTopicStorage(topic_id)) {
      storage->clearChunks();
    }
  }
}

DataEngine::DatasetChunkSnapshot DataEngine::detachDatasetChunks(DatasetId dataset_id) {
  auto lock = lockEngine();
  DatasetChunkSnapshot snapshot;
  snapshot.dataset_id = dataset_id;
  snapshot.prior_topic_ids = listTopicsLocked(dataset_id);
  if (snapshot.prior_topic_ids.empty()) {
    return snapshot;  // `valid` stays false
  }
  for (const TopicId topic_id : snapshot.prior_topic_ids) {
    TopicStorage* storage = getTopicStorage(topic_id);
    if (storage == nullptr) {
      continue;
    }
    DatasetChunkSnapshot::TopicSnapshot topic_snapshot;
    topic_snapshot.chunks = std::move(storage->sealed_chunks_);
    topic_snapshot.column_descriptors = storage->column_descriptors_;
    topic_snapshot.retention_floor = storage->retention_floor_;
    topic_snapshot.evicted_through_chunk_id = storage->evicted_through_chunk_id_;
    topic_snapshot.max_observed_array_length = storage->max_observed_array_length_;
    topic_snapshot.truncated_sample_count = storage->truncated_sample_count_;
    topic_snapshot.array_expansion_counts = storage->array_expansion_counts_;
    topic_snapshot.retired_columns = storage->retired_columns_;
    // Leaves the storage as clearDatasetChunks() would: columns and ratchet stats stay, so a refill rebinds.
    storage->clearChunks();
    snapshot.topics.emplace(topic_id, std::move(topic_snapshot));
  }
  snapshot.valid = true;
  return snapshot;
}

void DataEngine::reattachDatasetChunks(DatasetId dataset_id, DatasetChunkSnapshot&& snapshot) {
  auto lock = lockEngine();
  if (!snapshot.valid) {
    return;
  }
  const std::unordered_set<TopicId> prior(snapshot.prior_topic_ids.begin(), snapshot.prior_topic_ids.end());
  // listTopicsLocked() returns a copy, so retiring inside the loop is safe.
  for (const TopicId topic_id : listTopicsLocked(dataset_id)) {
    if (prior.find(topic_id) == prior.end()) {
      retireTopic(topic_id);
    } else if (TopicStorage* storage = getTopicStorage(topic_id)) {
      storage->clearChunks();
    }
  }
  // Direct assignment, not the ratchet setters, so a smaller prior value is restored.
  for (auto& [topic_id, topic_snapshot] : snapshot.topics) {
    TopicStorage* storage = getTopicStorage(topic_id);
    if (storage == nullptr) {
      continue;
    }
    storage->sealed_chunks_ = std::move(topic_snapshot.chunks);
    storage->invalidateChunkAggregate();
    storage->column_descriptors_ = std::move(topic_snapshot.column_descriptors);
    storage->retention_floor_ = topic_snapshot.retention_floor;
    storage->evicted_through_chunk_id_ = topic_snapshot.evicted_through_chunk_id;
    storage->max_observed_array_length_ = topic_snapshot.max_observed_array_length;
    storage->truncated_sample_count_ = topic_snapshot.truncated_sample_count;
    storage->array_expansion_counts_ = std::move(topic_snapshot.array_expansion_counts);
    storage->retired_columns_ = std::move(topic_snapshot.retired_columns);
  }
  snapshot.valid = false;
}

Status DataEngine::flushTo(DataEngine& dst) {
  if (&dst == this) {
    return PJ::unexpected("flushTo: source and destination are the same engine");
  }
  std::unique_lock<std::recursive_mutex> lock_this(impl_->mutex_, std::defer_lock);
  std::unique_lock<std::recursive_mutex> lock_dst(dst.impl_->mutex_, std::defer_lock);
  std::lock(lock_this, lock_dst);

  // Validate everything before moving anything.
  struct Step {
    TopicStorage* src;
    TopicStorage* dst;
  };
  std::vector<Step> plan;
  plan.reserve(impl_->topics.size());

  for (auto it = impl_->topics.begin(); it != impl_->topics.end(); ++it) {
    auto& src_storage = *it.value();
    if (src_storage.empty()) {
      continue;
    }
    TopicStorage* dst_storage = nullptr;
    for (auto dst_it = dst.impl_->topics.begin(); dst_it != dst.impl_->topics.end(); ++dst_it) {
      auto& candidate = *dst_it.value();
      if (candidate.descriptor().dataset_id == src_storage.descriptor().dataset_id &&
          candidate.descriptor().name == src_storage.descriptor().name) {
        dst_storage = &candidate;
        break;
      }
    }
    if (dst_storage == nullptr) {
      return PJ::unexpected(
          "flushTo: destination has no topic '" + src_storage.descriptor().name + "' for dataset " +
          std::to_string(src_storage.descriptor().dataset_id));
    }
    plan.push_back({&src_storage, dst_storage});
  }

  for (auto& step : plan) {
    adoptChunksFrom(*step.dst, *step.src);
  }

  return {};
}

void DataEngine::adoptChunksFrom(TopicStorage& dst, TopicStorage& src) {
  std::deque<TopicChunk> drained = std::move(src.sealed_chunks_);
  src.sealed_chunks_.clear();
  src.invalidateChunkAggregate();
  const TopicId dst_id = dst.topicId();
  for (auto& chunk : drained) {
    chunk.topic_id = dst_id;
    // Rows older than dst's newest are kept as overlapping chunks, as ingest keeps them; the reads merge them.
    (void)dst.appendSealedChunk(std::move(chunk));
  }
  // src's floor keeps a straddling chunk's sub-floor rows hidden; its evicted-through id lets a consumer that never
  // saw the dropped rows notice the gap.
  dst.retention_floor_ = std::max(dst.retention_floor_, src.retention_floor_);
  dst.evicted_through_chunk_id_ = std::max(dst.evicted_through_chunk_id_, src.evicted_through_chunk_id_);
  src.retention_floor_ = kNoRetentionFloor;
  src.evicted_through_chunk_id_ = 0;
  // src changed outside appendSealedChunk(), so its incremental sample caches must rebuild; dst's too, as before.
  ++src.series_generation_;
  ++dst.series_generation_;
}

Expected<DatasetReplaceResult> DataEngine::replaceDatasetFrom(
    DataEngine& staged, DatasetId staged_id, DatasetId primary_id) {
  if (&staged == this) {
    return PJ::unexpected("replaceDatasetFrom: staged and primary are the same engine");
  }
  // One std::lock for both: holding one and plain-locking the other deadlocks against the write host mirroring to
  // its staging engine.
  std::unique_lock<std::recursive_mutex> lock_this(impl_->mutex_, std::defer_lock);
  std::unique_lock<std::recursive_mutex> lock_staged(staged.impl_->mutex_, std::defer_lock);
  std::lock(lock_this, lock_staged);

  auto primary_it = impl_->datasets.find(primary_id);
  if (primary_it == impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("replaceDatasetFrom: primary dataset {} not found", primary_id));
  }
  if (staged.impl_->datasets.find(staged_id) == staged.impl_->datasets.end()) {
    return PJ::unexpected(fmt::format("replaceDatasetFrom: staged dataset {} not found", staged_id));
  }

  // Includes ids retired by an earlier replace, so a topic that comes back gets its old id.
  std::unordered_map<std::string, TopicId> primary_by_name;
  for (const TopicId tid : primary_it->second.topic_ids) {
    if (const auto* storage = getTopicStorage(tid)) {
      primary_by_name.emplace(storage->descriptor().name, tid);
    }
  }

  DatasetReplaceResult result;
  std::unordered_set<std::string> staged_names;

  for (const TopicId staged_tid : staged.listTopicsLocked(staged_id)) {
    TopicStorage* staged_storage = staged.getTopicStorage(staged_tid);
    if (staged_storage == nullptr) {
      continue;
    }
    const std::string& name = staged_storage->descriptor().name;
    staged_names.insert(name);

    const auto match = primary_by_name.find(name);
    TopicId primary_tid = 0;
    if (match == primary_by_name.end()) {
      // Schema-less: reads resolve columns from the moved chunks and the copied layout; the staged schema is not
      // registered in this engine.
      TopicDescriptor desc = staged_storage->descriptor();
      desc.dataset_id = primary_id;
      desc.schema_id = 0;
      auto created = createTopicLocked(primary_id, std::move(desc));
      if (!created.has_value()) {
        return PJ::unexpected("replaceDatasetFrom: createTopic failed for '" + name + "': " + created.error());
      }
      primary_tid = *created;
      result.added_topics.push_back(primary_tid);
    } else {
      primary_tid = match->second;
      impl_->retired_topic_ids.erase(primary_tid);
      result.replaced_topics.push_back(primary_tid);
    }

    TopicStorage* primary_storage = getTopicStorage(primary_tid);
    primary_storage->clearChunks();
    primary_storage->setColumnDescriptors(std::move(staged_storage->column_descriptors_));
    primary_storage->retired_columns_ = std::move(staged_storage->retired_columns_);  // ids of the staged layout
    adoptChunksFrom(*primary_storage, *staged_storage);
  }

  for (const auto& [name, primary_tid] : primary_by_name) {
    if (staged_names.count(name) == 0 && impl_->retired_topic_ids.count(primary_tid) == 0) {
      if (auto* storage = getTopicStorage(primary_tid)) {
        storage->clearChunks();
      }
      impl_->retired_topic_ids.insert(primary_tid);
      result.retired_topics.push_back(primary_tid);
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Listing helpers
// ---------------------------------------------------------------------------

std::vector<DatasetId> DataEngine::listDatasets() const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  std::vector<DatasetId> result;
  result.reserve(impl_->datasets.size());
  for (const auto& [id, info] : impl_->datasets) {
    result.push_back(id);
  }
  return result;
}

std::vector<TopicId> DataEngine::listTopics(DatasetId dataset_id) const {
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex_);
  return listTopicsLocked(dataset_id);
}

std::vector<TopicId> DataEngine::listTopicsLocked(DatasetId dataset_id) const {
  auto it = impl_->datasets.find(dataset_id);
  if (it == impl_->datasets.end()) {
    return {};
  }
  if (impl_->retired_topic_ids.empty()) {
    return it->second.topic_ids;
  }
  std::vector<TopicId> result;
  result.reserve(it->second.topic_ids.size());
  for (const TopicId tid : it->second.topic_ids) {
    if (impl_->retired_topic_ids.count(tid) == 0) {
      result.push_back(tid);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Dataset merge (destructive union)
// ---------------------------------------------------------------------------

Expected<DatasetMergeReport> DataEngine::mergeDatasets(
    DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources) {
  // A streaming worker may be committing to these topics.
  auto lock = lockEngine();
  // Every reachable error is caught here, before anything changes. The error returns further down guard internal
  // invariants only; one firing would leave a partial merge.
  if (getDataset(anchor_id) == nullptr) {
    return PJ::unexpected(fmt::format("mergeDatasets: anchor dataset {} not found", anchor_id));
  }
  std::unordered_set<DatasetId> seen_sources;
  for (const auto& src : sources) {
    if (src.dataset_id == anchor_id) {
      return PJ::unexpected(fmt::format("mergeDatasets: source dataset {} is the anchor", src.dataset_id));
    }
    if (getDataset(src.dataset_id) == nullptr) {
      return PJ::unexpected(fmt::format("mergeDatasets: source dataset {} not found", src.dataset_id));
    }
    // A repeated source would fold its samples twice.
    if (!seen_sources.insert(src.dataset_id).second) {
      return PJ::unexpected(fmt::format("mergeDatasets: source dataset {} listed more than once", src.dataset_id));
    }
  }

  DatasetMergeReport report;

  // Every anchor topic, empty ones included, so a shared name keeps the anchor's id.
  std::unordered_map<std::string, TopicId> anchor_topic_by_name;
  for (const TopicId tid : listTopics(anchor_id)) {
    if (const TopicStorage* st = getTopicStorage(tid)) {
      anchor_topic_by_name.emplace(st->descriptor().name, tid);
    }
  }

  // Anchor first, so its columns seed the union order.
  struct Contributor {
    DatasetId dataset_id;
    TopicId topic_id;
    Timestamp shift;
  };
  std::vector<std::string> ordered_names;
  std::unordered_map<std::string, std::vector<Contributor>> contributors_by_name;
  auto add_data_topic = [&](DatasetId ds, TopicId tid, Timestamp shift) {
    const TopicStorage* st = getTopicStorage(tid);
    if (st == nullptr || st->empty()) {
      return;
    }
    auto [it, inserted] = contributors_by_name.try_emplace(st->descriptor().name);
    if (inserted) {
      ordered_names.push_back(st->descriptor().name);
    }
    it->second.push_back({ds, tid, shift});
  };
  for (const TopicId tid : listTopics(anchor_id)) {
    add_data_topic(anchor_id, tid, 0);
  }
  for (const auto& src : sources) {
    for (const TopicId tid : listTopics(src.dataset_id)) {
      add_data_topic(src.dataset_id, tid, src.raw_shift_ns);
    }
  }

  std::unordered_set<std::string> skipped_seen;
  for (const std::string& name : ordered_names) {
    const std::vector<Contributor> all = contributors_by_name[name];

    // Union of columns by field_path; a contributor whose storage kind conflicts on a shared field is skipped whole.
    std::vector<ColumnDescriptor> union_cols;
    std::unordered_map<std::string, std::size_t> union_index;
    std::vector<Contributor> kept;
    bool has_source = false;
    // Seeded from the anchor's advertised columns even when it has no rows: curve keys are (topic_id, column_index),
    // and an empty anchor topic would otherwise take a source's column order.
    if (const auto anchor_it = anchor_topic_by_name.find(name); anchor_it != anchor_topic_by_name.end()) {
      if (const TopicStorage* anchor_st = getTopicStorage(anchor_it->second)) {
        for (const ColumnDescriptor& col : anchor_st->columnDescriptors()) {
          if (union_index.emplace(col.field_path, union_cols.size()).second) {
            union_cols.push_back(
                ColumnDescriptor{static_cast<FieldId>(union_cols.size()), col.logical_type, col.field_path});
          }
        }
      }
    }
    for (const Contributor& c : all) {
      const TopicStorage* st = getTopicStorage(c.topic_id);
      if (st == nullptr) {
        continue;
      }
      std::vector<std::pair<std::string, PrimitiveType>> local;
      std::unordered_set<std::string> seen_local;
      for (const TopicChunk& chunk : st->sealedChunks()) {
        for (const auto& col : chunk.columns) {
          // A retired column holds no value: neither a field of the union nor a type conflict.
          if (col.descriptor && !st->isColumnRetired(col.descriptor->field_id) &&
              seen_local.insert(col.descriptor->field_path).second) {
            local.emplace_back(col.descriptor->field_path, col.descriptor->logical_type);
          }
        }
      }
      bool conflict = false;
      for (const auto& [field_path, type] : local) {
        const auto it = union_index.find(field_path);
        if (it != union_index.end() && storageKindOf(union_cols[it->second].logical_type) != storageKindOf(type)) {
          conflict = true;
          break;
        }
      }
      if (conflict) {
        if (skipped_seen.insert(name).second) {
          report.skipped_topics.push_back(name);
        }
        continue;
      }
      for (const auto& [field_path, type] : local) {
        if (union_index.find(field_path) == union_index.end()) {
          union_index.emplace(field_path, union_cols.size());
          union_cols.push_back(ColumnDescriptor{static_cast<FieldId>(union_cols.size()), type, field_path});
        }
      }
      kept.push_back(c);
      has_source = has_source || c.dataset_id != anchor_id;
    }

    // Anchor-only, or every source skipped: leave the anchor's topic alone.
    if (!has_source || union_cols.empty()) {
      continue;
    }

    TopicId dest_tid = 0;
    bool dest_is_new = false;
    if (const auto anchor_it = anchor_topic_by_name.find(name); anchor_it != anchor_topic_by_name.end()) {
      dest_tid = anchor_it->second;
    } else {
      TopicDescriptor desc;
      desc.name = name;
      desc.schema_id = 0;
      desc.dataset_id = anchor_id;
      auto created = createTopic(anchor_id, std::move(desc));
      if (!created.has_value()) {
        // Unreachable: the anchor exists, schema_id and requested_id are 0.
        return PJ::unexpected("mergeDatasets: createTopic failed for '" + name + "': " + created.error());
      }
      dest_tid = *created;
      dest_is_new = true;
    }

    // Stable sort: equal timestamps keep contributor order and all survive.
    struct RowRef {
      Timestamp ts;
      const TopicChunk* chunk;
      std::size_t row;
      const std::vector<int>* colmap;  // chunk column -> union column, -1 to drop
    };
    std::deque<std::vector<int>> colmap_pool;  // stable addresses for RowRef::colmap
    std::vector<RowRef> rows;
    for (const Contributor& c : kept) {
      const TopicStorage* st = getTopicStorage(c.topic_id);
      if (st == nullptr) {
        continue;
      }
      for (const TopicChunk& chunk : st->sealedChunks()) {
        std::vector<int> colmap(chunk.columns.size(), -1);
        for (std::size_t i = 0; i < chunk.columns.size(); ++i) {
          if (!chunk.columns[i].descriptor) {
            continue;
          }
          if (const auto it = union_index.find(chunk.columns[i].descriptor->field_path); it != union_index.end()) {
            colmap[i] = static_cast<int>(it->second);
          }
        }
        colmap_pool.push_back(std::move(colmap));
        const std::vector<int>* colmap_ptr = &colmap_pool.back();
        // Rows under the contributor's floor are evicted; the rebuilt topic has no floor, so drop them here.
        const Timestamp floor = st->retentionFloor();
        for (std::size_t r = 0; r < chunk.stats.row_count; ++r) {
          const Timestamp ts = chunk.readTimestamp(r);
          if (ts >= floor) {
            rows.push_back(RowRef{saturatingAdd(ts, c.shift), &chunk, r, colmap_ptr});
          }
        }
      }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const RowRef& a, const RowRef& b) { return a.ts < b.ts; });

    // Build into a local vector: the rows point into chunks the destination clear would free.
    uint32_t max_chunk_rows = 1024;
    if (const TopicStorage* dest_st = getTopicStorage(dest_tid)) {
      max_chunk_rows = dest_st->descriptor().max_chunk_rows;
    }
    std::vector<TopicChunk> built;
    TopicChunkBuilder builder(dest_tid, /*schema_id=*/0, union_cols, max_chunk_rows);
    for (const RowRef& rr : rows) {
      builder.beginRow(rr.ts);
      const TopicChunk& chunk = *rr.chunk;
      const std::vector<int>& colmap = *rr.colmap;
      for (std::size_t lc = 0; lc < colmap.size(); ++lc) {
        if (colmap[lc] < 0 || chunk.isNull(lc, rr.row)) {
          continue;  // a column a row does not set reads null
        }
        const auto uc = static_cast<std::size_t>(colmap[lc]);
        switch (storageKindOf(union_cols[uc].logical_type)) {
          case StorageKind::kFloat32:
            builder.set<float>(uc, static_cast<float>(chunk.readNumericAsDouble(lc, rr.row)));
            break;
          case StorageKind::kFloat64:
            builder.set<double>(uc, chunk.readNumericAsDouble(lc, rr.row));
            break;
          case StorageKind::kInt32:
            builder.set<int32_t>(uc, static_cast<int32_t>(chunk.readNumericAsInt64(lc, rr.row)));
            break;
          case StorageKind::kInt64:
            builder.set<int64_t>(uc, chunk.readNumericAsInt64(lc, rr.row));
            break;
          case StorageKind::kUint64:
            builder.set<uint64_t>(uc, chunk.readNumericAsUint64(lc, rr.row));
            break;
          case StorageKind::kBool:
            builder.set<bool>(uc, chunk.readBool(lc, rr.row));
            break;
          case StorageKind::kString:
            builder.set<std::string_view>(uc, chunk.readString(lc, rr.row));
            break;
        }
      }
      builder.finishRow();
      if (builder.isFull()) {
        built.push_back(builder.seal());
        builder = TopicChunkBuilder(dest_tid, /*schema_id=*/0, union_cols, max_chunk_rows);
      }
    }
    if (builder.rowCount() > 0) {
      built.push_back(builder.seal());
    }

    TopicStorage* dest = getTopicStorage(dest_tid);
    if (dest == nullptr) {
      // Unreachable: nothing above retires a topic.
      return PJ::unexpected(fmt::format("mergeDatasets: destination topic {} vanished", dest_tid));
    }
    dest->clearChunks();
    dest->setColumnDescriptors(union_cols);
    for (TopicChunk& chunk : built) {
      chunk.topic_id = dest_tid;
      (void)dest->appendSealedChunk(std::move(chunk));
    }
    (dest_is_new ? report.added_topics : report.modified_topics).push_back(dest_tid);
  }

  // Empty the consumed source datasets (destructive). They stay registered, with no topics: a same-named topic left
  // on one would read as a second recording to whoever walks the datasets. The host removes them from its catalog.
  for (const auto& src : sources) {
    for (const TopicId tid : listTopics(src.dataset_id)) {
      retireTopic(tid);
    }
    impl_->merged_into.insert_or_assign(src.dataset_id, anchor_id);
    report.consumed_datasets.push_back(src.dataset_id);
  }

  return report;
}

// ---------------------------------------------------------------------------
// Writer/Reader factories
// ---------------------------------------------------------------------------

DataWriter DataEngine::createWriter() {
  return DataWriter(*this);
}

DataReader DataEngine::createReader() const {
  return DataReader(*this);
}

}  // namespace PJ
