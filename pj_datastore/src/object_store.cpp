// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/object_store.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/marker_topic_metadata.hpp"
#include "pj_datastore/resident_payload_pool.hpp"
#include "pj_datastore/saturating_time.hpp"

namespace PJ {

namespace {

size_t countBefore(const std::vector<Timestamp>& stamps, Timestamp threshold) {
  return static_cast<size_t>(std::lower_bound(stamps.begin(), stamps.end(), threshold) - stamps.begin());
}

size_t chargedBytes(const ObjectEntry& entry) {
  if (const auto* owned = std::get_if<SharedBuffer>(&entry.payload)) {
    return *owned ? (*owned)->size() : 0;
  }
  return std::get<LazyPayload>(entry.payload).resident_bytes;
}

// insertLocked() mints the UID under the lock: OrderedEntries needs each push to carry the series' newest UID.
ObjectEntry unstampedEntry(Timestamp timestamp, ObjectEntryPayload payload) {
  ObjectEntry entry;
  entry.timestamp = timestamp;
  entry.payload = std::move(payload);
  return entry;
}

SharedBuffer ownedPayload(std::vector<uint8_t> bytes) {
  return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

// The two storages share their index API, so the index-only reads run on either.
template <typename Series, typename F>
decltype(auto) onEntries(Series& series, F&& f) {
  return series.log ? f(*series.log) : f(series.ordered);
}

Expected<sdk::Log> decodeLog(const std::vector<uint8_t>& bytes) {
  auto object = deserializeBuiltinObject(sdk::BuiltinObjectType::kLog, bytes.data(), bytes.size());
  if (!object) {
    return unexpected("kLog payload: " + object.error());
  }
  const sdk::Log* log = object->get<sdk::Log>();
  if (log == nullptr) {
    return unexpected("kLog payload: not a log");
  }
  return *log;
}

constexpr auto kLogTakesOwned = "kLog topics store decoded logs: push them owned";

}  // namespace

// --- Registration ---

Expected<ObjectTopicId> ObjectStore::registerTopic(ObjectTopicDescriptor descriptor, ObjectTopicId requested_id) {
  stampLegacyMarkerMetadata(descriptor);
  std::unique_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.topic_name == descriptor.topic_name &&
        series->descriptor.dataset_id == descriptor.dataset_id) {
      return unexpected("topic already registered: " + descriptor.topic_name);
    }
    if (requested_id.id != 0 && tid.id == requested_id.id) {
      return unexpected("object topic id already in use: " + std::to_string(requested_id.id));
    }
  }
  ObjectTopicId id;
  if (requested_id.id != 0) {
    id = requested_id;
    if (next_id_ <= id.id) {
      next_id_ = id.id + 1;
    }
  } else {
    id = ObjectTopicId{next_id_++};
  }
  topics_.emplace_back(id, makeSeries(std::move(descriptor)));
  series_index_.emplace(id.id, topics_.back().second.get());
  return id;
}

Expected<ObjectTopicId> ObjectStore::findOrRegisterTopic(ObjectTopicDescriptor descriptor) {
  stampLegacyMarkerMetadata(descriptor);
  std::unique_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.topic_name == descriptor.topic_name &&
        series->descriptor.dataset_id == descriptor.dataset_id) {
      if (series->descriptor.metadata_json != descriptor.metadata_json) {
        return unexpected("topic already registered with different metadata: " + descriptor.topic_name);
      }
      return tid;
    }
  }
  ObjectTopicId id{next_id_++};
  topics_.emplace_back(id, makeSeries(std::move(descriptor)));
  series_index_.emplace(id.id, topics_.back().second.get());
  return id;
}

std::optional<ObjectTopicId> ObjectStore::findTopic(DatasetId dataset_id, std::string_view topic_name) const {
  std::shared_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id && series->descriptor.topic_name == topic_name) {
      return tid;
    }
  }
  return std::nullopt;
}

bool ObjectStore::isTopic(ObjectTopicId id, DatasetId dataset_id, std::string_view topic_name) const {
  std::shared_lock lock(store_mutex_);
  const auto* series = findSeries(id);
  return series != nullptr && series->descriptor.dataset_id == dataset_id &&
         series->descriptor.topic_name == topic_name;
}

ObjectTopicDescriptor ObjectStore::descriptor(ObjectTopicId id) const {
  std::shared_lock lock(store_mutex_);
  const auto* s = findSeries(id);
  if (s == nullptr) {
    return {};
  }
  return s->descriptor;
}

std::vector<ObjectTopicId> ObjectStore::listTopics() const {
  std::shared_lock lock(store_mutex_);
  std::vector<ObjectTopicId> result;
  result.reserve(topics_.size());
  for (const auto& [tid, _] : topics_) {
    result.push_back(tid);
  }
  return result;
}

std::vector<ObjectTopicId> ObjectStore::listTopics(DatasetId dataset_id) const {
  std::shared_lock lock(store_mutex_);
  std::vector<ObjectTopicId> result;
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      result.push_back(tid);
    }
  }
  return result;
}

void ObjectStore::forEachTopic(
    DatasetId dataset_id, const std::function<void(ObjectTopicId, const ObjectTopicDescriptor&)>& visit) const {
  std::shared_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      visit(tid, series->descriptor);
    }
  }
}

void ObjectStore::forEachTopicRetained(
    const std::function<void(ObjectTopicId, const ObjectTopicDescriptor&, const RetainedRange&)>& visit) const {
  std::shared_lock lock(store_mutex_);
  for (const auto& [tid, series] : topics_) {
    RetainedRange retained;
    {
      std::shared_lock series_lock(series->mutex);
      onEntries(*series, [&retained](const auto& entries) {
        retained.entry_count = entries.size();
        if (retained.entry_count > 0) {
          retained.first = entries.frontTimestamp();
          retained.last = entries.backTimestamp();
        }
      });
    }
    visit(tid, series->descriptor, retained);
  }
}

// --- Write ---

Status ObjectStore::pushOwned(ObjectTopicId id, Timestamp timestamp, std::vector<uint8_t> payload) {
  Graveyard graveyard;  // retention victims die after the locks below release
  ObjectEntry entry;    // ours until inserted: if the insertion fails, it dies after the locks
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return unexpected("unknown topic");
  }

  if (series->log) {
    // Decoded before taking the series lock, so readers do not wait on it.
    auto log = decodeLog(payload);
    if (!log) {
      return unexpected(log.error());
    }
    std::unique_lock lock(series->mutex);
    insertLogLocked(*series, timestamp, *log, graveyard);
    return {};
  }

  // Allocated before taking the series lock, so readers of the series do not wait on the allocation.
  entry = unstampedEntry(timestamp, ownedPayload(std::move(payload)));
  std::unique_lock lock(series->mutex);
  insertLocked(*series, std::move(entry), graveyard);
  return {};
}

Expected<std::vector<SequentialUID>> ObjectStore::pushOwnedBundle(std::vector<OwnedObjectWrite> writes) {
  if (writes.empty()) {
    return unexpected("empty object bundle");
  }
  Graveyard graveyard;  // every removed or retention-evicted entry dies after the lock releases
  struct Step {
    ObjectSeries* series;
    OrderedEntries::Removal removal;
    ObjectEntry entry;
    std::optional<sdk::Log> log;  ///< the decoded append of a log series
  };
  std::vector<Step> steps;
  std::vector<SequentialUID> created;
  std::unique_lock store_lock(store_mutex_);

  // Phase 1 decides everything and allocates what phase 2 needs, but changes no topic: a rejection leaves every
  // topic as it was.
  try {
    steps.reserve(writes.size());
    created.resize(writes.size());
    size_t graveyard_room = 0;
    for (size_t index = 0; index < writes.size(); ++index) {
      OwnedObjectWrite& write = writes[index];
      ObjectSeries* series = findSeries(write.topic);
      if (series == nullptr) {
        return unexpected("unknown topic in object bundle");
      }
      // ponytail: O(W^2) for W writes, and W is a recipe's object outputs (single digits);
      // a sorted id list if bundles grow.
      for (size_t earlier = 0; earlier < index; ++earlier) {
        if (writes[earlier].topic == write.topic) {
          return unexpected("duplicate topic in object bundle");
        }
      }
      // Phase 2 changes the timestamp vectors: wait out views still reading them.
      drainSeriesReaders(*series);
      Step& step = steps.emplace_back(Step{.series = series, .removal = {}, .entry = {}, .log = {}});
      if (series->log) {
        if (write.drop) {
          step.removal = series->log->planRemoval(write.drop);
        }
        if (write.append) {
          auto log = decodeLog(write.payload);
          if (!log) {
            return unexpected("object bundle rejected: " + log.error());
          }
          step.log = std::move(*log);
        }
        continue;  // nothing moves to the graveyard
      }
      if (write.drop) {
        step.removal = series->ordered.planRemoval(write.drop);
      }
      if (write.append) {
        step.entry = unstampedEntry(write.timestamp, ownedPayload(std::move(write.payload)));
      }
      // What phase 2 can move to the graveyard: the planned removals or, under a retention budget, every entry the
      // topic holds after the write.
      const RetentionBudget& budget = series->budget;
      const bool retains = budget.time_window_ns > 0 || budget.max_memory_bytes > 0 || budget.max_entries > 0;
      graveyard_room += retains ? series->ordered.size() + (write.append ? 1 : 0) : step.removal.count;
    }
    graveyard.entries.reserve(graveyard_room);
    graveyard.cached.reserve(writes.size());  // a series holds at most one cached value
  } catch (const std::exception& error) {
    return unexpected(std::string("object bundle rejected: ") + error.what());
  }

  // Phase 2 changes each topic whole or not at all, and the exclusive lock keeps readers out until the last one is
  // done. Only an append that removes nothing still allocates, and it throws before changing its topic.
  for (size_t index = 0; index < writes.size(); ++index) {
    Step& step = steps[index];
    if (step.series->log) {
      if (writes[index].append) {
        created[index] = insertLogLocked(*step.series, writes[index].timestamp, *step.log, graveyard, &step.removal);
      } else {
        accountLogRemoved(*step.series, step.series->log->remove(step.removal), graveyard);
      }
      continue;
    }
    if (writes[index].append) {
      created[index] = insertLocked(*step.series, std::move(step.entry), graveyard, &step.removal);
    } else {
      const size_t first_removed = graveyard.entries.size();
      step.series->ordered.remove(step.removal, graveyard.entries);
      accountRemoved(*step.series, graveyard, first_removed);
    }
  }
  return created;
}

Status ObjectStore::pushLazy(ObjectTopicId id, Timestamp timestamp, LazyCallback fetch, size_t resident_bytes) {
  return pushLazyEntry(id, timestamp, ObjectEntryPayload{LazyPayload{std::move(fetch), nullptr, resident_bytes}});
}

Status ObjectStore::pushLazyWithSeed(ObjectTopicId id, Timestamp timestamp, sdk::PayloadView seed, LazyCallback fetch) {
  std::shared_ptr<ResidentSlot> slot;
  if (resident_pool_ != nullptr) {
    // Admitting can evict other residents, so an unknown topic is rejected first. A topic removed after this check
    // only costs those evictions.
    {
      std::shared_lock store_lock(store_mutex_);
      const ObjectSeries* series = findSeries(id);
      if (series == nullptr) {
        return unexpected("unknown topic");
      }
      if (series->log) {
        return unexpected(kLogTakesOwned);
      }
    }
    slot = resident_pool_->admit(std::move(seed));
  }
  return pushLazyEntry(id, timestamp, ObjectEntryPayload{LazyPayload{std::move(fetch), std::move(slot)}});
}

Status ObjectStore::pushLazyEntry(ObjectTopicId id, Timestamp timestamp, ObjectEntryPayload payload) {
  Graveyard graveyard;  // retention victims die after the locks below release
  ObjectEntry entry;    // ours until inserted: a failed insertion must not run its cleanup under the locks
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return unexpected("unknown topic");
  }
  if (series->log) {
    return unexpected(kLogTakesOwned);
  }

  entry = unstampedEntry(timestamp, std::move(payload));
  std::unique_lock lock(series->mutex);
  insertLocked(*series, std::move(entry), graveyard);
  return {};
}

void ObjectStore::setResidentPayloadPool(std::shared_ptr<ResidentPayloadPool> pool) {
  resident_pool_ = std::move(pool);
}

// --- Read ---

std::optional<ResolvedObjectEntry> ObjectStore::latestAt(ObjectTopicId id, Timestamp timestamp) const {
  std::optional<ResolvedObjectEntry> displaced;  // the replaced cache value dies after the locks
  // Declared before the lock: if the entry was evicted while it resolved, this copy holds the last reference to a lazy
  // fetch context, whose cleanup may call back into the store.
  ObjectEntry snapshot;
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  // Resolved from a snapshot after the series lock drops, so a slow lazy fetch never blocks this series' writers.
  // The snapshot owns every capture it needs, so an eviction mid-resolve is harmless.
  if (series->log) {
    // Rebuilt under the series lock: no fetch to wait on, and the bytes are the result's own.
    std::shared_lock lock(series->mutex);
    const auto idx = series->log->indexAtOrBefore(timestamp);
    if (!idx.has_value()) {
      return std::nullopt;
    }
    std::lock_guard cache_guard(series->cache_mutex);
    if (series->cached_latest && series->cached_latest->sequential_uid == series->log->uidAt(*idx)) {
      return *series->cached_latest;
    }
    ResolvedObjectEntry resolved = resolveLog(*series->log, *idx);
    displaced = std::exchange(series->cached_latest, resolved);
    return resolved;
  }
  {
    std::shared_lock lock(series->mutex);
    const auto idx = series->ordered.indexAtOrBefore(timestamp);
    if (!idx.has_value()) {
      return std::nullopt;
    }
    const ObjectEntry& entry = series->ordered.entryAt(*idx);

    // Warm cache, matched by the never-reused UID: a reader landing on the same entry again skips the lazy fetch.
    {
      std::lock_guard cache_guard(series->cache_mutex);
      if (series->cached_latest && series->cached_latest->sequential_uid == entry.sequential_uid) {
        return *series->cached_latest;
      }
    }
    snapshot = entry;
  }

  // No store lock during the resolve either: holding it shared across a file re-read stalls every exclusive acquirer,
  // and a fetch callback that re-enters the store would lock store_mutex_ shared twice, which is undefined behavior.
  store_lock.unlock();

  bool served_from_resident = false;
  ResolvedObjectEntry resolved = resolveEntry(snapshot, &served_from_resident);
  // Not cached: a failed or empty resolve (the next read retries), or a resident-slot hit (caching it would hold an
  // anchor outside the ResidentPayloadPool's byte budget).
  if (!resolved.payload.bytes.empty() && !served_from_resident) {
    // Found again: `series` was valid only under the dropped lock, and the topic may be gone.
    store_lock.lock();
    if (const auto* cached_series = findSeries(id); cached_series != nullptr) {
      // An entry evicted while it resolved must not come back through the cache.
      std::shared_lock series_lock(cached_series->mutex);
      if (cached_series->ordered.atUid(snapshot.sequential_uid) != nullptr) {
        std::lock_guard cache_guard(cached_series->cache_mutex);
        displaced = std::exchange(cached_series->cached_latest, resolved);
      }
    }
  }
  return resolved;
}

std::optional<ResolvedObjectEntry> ObjectStore::at(ObjectTopicId id, size_t index) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  ObjectEntry snapshot;
  {
    std::shared_lock lock(series->mutex);
    if (series->log) {
      if (index >= series->log->size()) {
        return std::nullopt;
      }
      return resolveLog(*series->log, index);
    }
    const ObjectEntry* entry = series->ordered.atIndex(index);
    if (entry == nullptr) {
      return std::nullopt;
    }
    snapshot = *entry;
  }
  store_lock.unlock();  // resolve unlocked, as in latestAt()
  return resolveEntry(snapshot);
}

std::optional<ResolvedObjectEntry> ObjectStore::at(ObjectTopicId id, SequentialUID sequential_uid) const {
  if (!sequential_uid.valid()) {
    return std::nullopt;
  }

  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  ObjectEntry snapshot;
  {
    std::shared_lock lock(series->mutex);
    if (series->log) {
      const auto index = series->log->indexOfUid(sequential_uid);
      if (!index) {
        return std::nullopt;
      }
      return resolveLog(*series->log, *index);
    }
    const ObjectEntry* entry = series->ordered.atUid(sequential_uid);
    if (entry == nullptr) {
      return std::nullopt;
    }
    snapshot = *entry;
  }
  store_lock.unlock();  // resolve unlocked, as in latestAt()
  return resolveEntry(snapshot);
}

std::optional<size_t> ObjectStore::indexAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.indexAtOrBefore(timestamp); });
}

std::optional<size_t> ObjectStore::indexOf(ObjectTopicId id, SequentialUID uid) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.indexOfUid(uid); });
}

OrderedEntries::ContentIdentity ObjectStore::contentIdentity(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.contentIdentity(); });
}

SequentialUID ObjectStore::firstSequentialUID(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.firstUid(); });
}

std::vector<ResolvedObjectEntry> ObjectStore::drainNewSince(ObjectTopicId id, SequentialUID& cursor) const {
  // Copies the unconsumed arrivals under both locks, then resolves them with no lock held, as latestAt() does.
  std::vector<ObjectEntry> batch;
  {
    const std::shared_lock<std::shared_mutex> store_lock(store_mutex_);
    const auto* series = findSeries(id);
    if (series == nullptr) {
      return {};
    }
    const std::shared_lock<std::shared_mutex> lock(series->mutex);
    if (series->log) {
      // Arrival order may differ from row order; the rows decode in row order, one chunk at a time.
      const LogEntries& log = *series->log;
      std::vector<size_t> rows;
      for (SequentialUID uid = log.nextUidAfter(cursor); uid.valid(); uid = log.nextUidAfter(uid)) {
        rows.push_back(*log.indexOfUid(uid));
      }
      if (rows.empty()) {
        return {};
      }
      const auto [lo, hi] = std::ranges::minmax(rows);
      std::vector<ResolvedObjectEntry> by_row(hi - lo + 1);
      std::vector<bool> wanted(hi - lo + 1, false);
      for (const size_t row : rows) {
        wanted[row - lo] = true;
      }
      log.records(lo, hi - lo + 1, [&](size_t row, LogRecord&& record) {
        if (!wanted[row - lo]) {
          return;
        }
        auto bytes = serializeBuiltinObject(sdk::BuiltinObject(std::move(record.log)));
        by_row[row - lo] = ResolvedObjectEntry{
            .timestamp = record.timestamp,
            .sequential_uid = record.uid,
            .payload = bytes ? sdk::makePayloadView(std::move(*bytes)) : sdk::PayloadView{},
        };
      });
      std::vector<ResolvedObjectEntry> out;
      out.reserve(rows.size());
      for (const size_t row : rows) {
        out.push_back(std::move(by_row[row - lo]));
      }
      cursor = out.back().sequential_uid;
      return out;
    }
    batch.reserve(series->ordered.size());
    for (SequentialUID uid = series->ordered.nextUidAfter(cursor); uid.valid();
         uid = series->ordered.nextUidAfter(uid)) {
      const ObjectEntry* entry = series->ordered.atUid(uid);
      if (entry == nullptr) {
        break;  // unreachable while the lock is held
      }
      batch.push_back(*entry);
    }
  }

  std::vector<ResolvedObjectEntry> out;
  out.reserve(batch.size());
  for (const ObjectEntry& entry : batch) {
    cursor = entry.sequential_uid;
    out.push_back(resolveEntry(entry));
  }
  return out;
}

SequentialUID ObjectStore::maxUidAtOrBefore(ObjectTopicId id, Timestamp t) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.maxUidAtOrBefore(t); });
}

std::optional<Timestamp> ObjectStore::oldestArrivalAfter(ObjectTopicId id, SequentialUID after) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.oldestTimestampAfterUid(after); });
}

std::optional<ObjectStore::TimeRangeEntry> ObjectStore::latestEntryIdAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  // The entry latestAt() would pick, read as metadata only.
  std::shared_lock lock(series->mutex);
  if (series->log) {
    const auto idx = series->log->indexAtOrBefore(timestamp);
    if (!idx.has_value()) {
      return std::nullopt;
    }
    return TimeRangeEntry{series->log->uidAt(*idx), series->log->timestamps()[*idx]};
  }
  const auto idx = series->ordered.indexAtOrBefore(timestamp);
  if (!idx.has_value()) {
    return std::nullopt;
  }
  const ObjectEntry& entry = series->ordered.entryAt(*idx);
  return TimeRangeEntry{entry.sequential_uid, entry.timestamp};
}

std::vector<ObjectStore::TimeRangeEntry> ObjectStore::rangeByTime(ObjectTopicId id, Timestamp lo, Timestamp hi) const {
  if (hi <= lo) {
    return {};
  }
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return onEntries(*series, [&](const auto& entries) { return entries.rangeByTime(lo, hi); });
}

size_t ObjectStore::entryCount(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return 0;
  }

  std::shared_lock lock(series->mutex);
  return onEntries(*series, [](const auto& entries) { return entries.size(); });
}

std::pair<Timestamp, Timestamp> ObjectStore::timeRange(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {0, 0};
  }

  std::shared_lock lock(series->mutex);
  return onEntries(*series, [](const auto& entries) -> std::pair<Timestamp, Timestamp> {
    if (entries.empty()) {
      return {0, 0};
    }
    return {entries.frontTimestamp(), entries.backTimestamp()};
  });
}

EntryTimestampsView ObjectStore::entryTimestamps(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  const std::vector<Timestamp>* stamps = onEntries(*series, [](const auto& entries) { return &entries.timestamps(); });
  return {std::move(lock), stamps};
}

std::vector<EntryTimestampsView> ObjectStore::entryTimestamps(std::vector<ObjectTopicId> ids) const {
  std::ranges::sort(ids, {}, &ObjectTopicId::id);
  const auto [last, end] = std::ranges::unique(ids);
  ids.erase(last, end);
  std::vector<EntryTimestampsView> views;
  views.reserve(ids.size());
  std::shared_lock store_lock(store_mutex_);
  for (const ObjectTopicId id : ids) {
    if (const auto* series = findSeries(id)) {
      std::shared_lock lock(series->mutex);
      const std::vector<Timestamp>* stamps =
          onEntries(*series, [](const auto& entries) { return &entries.timestamps(); });
      views.emplace_back(std::move(lock), stamps);
    }
  }
  return views;
}

// --- Retention ---

void ObjectStore::setRetentionBudget(ObjectTopicId id, RetentionBudget budget) {
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return;
  }
  std::unique_lock lock(series->mutex);
  series->budget = budget;
}

RetentionBudget ObjectStore::retentionBudget(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return series->budget;
}

size_t ObjectStore::memoryUsage(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return 0;
  }
  std::shared_lock lock(series->mutex);
  return series->memory_bytes;
}

// --- Explicit eviction ---

void ObjectStore::evictBefore(ObjectTopicId id, Timestamp threshold) {
  Graveyard graveyard;
  std::shared_lock store_lock(store_mutex_);
  auto* series = findSeries(id);
  if (series == nullptr) {
    return;
  }
  std::unique_lock lock(series->mutex);
  evictFront(
      *series, onEntries(*series, [&](const auto& e) { return countBefore(e.timestamps(), threshold); }), graveyard);
}

void ObjectStore::evictAllBefore(Timestamp threshold) {
  Graveyard graveyard;
  std::shared_lock store_lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    std::unique_lock lock(series->mutex);
    evictFront(
        *series, onEntries(*series, [&](const auto& e) { return countBefore(e.timestamps(), threshold); }), graveyard);
  }
}

// --- Cross-store flush ---

Status ObjectStore::flushTo(ObjectStore& dst) {
  if (&dst == this) {
    return unexpected("flushTo: source and destination are the same store");
  }

  Graveyard graveyard;
  // Address order, so two flushTo() calls in opposite directions cannot deadlock.
  ObjectStore* first = this < &dst ? this : &dst;
  ObjectStore* second = first == this ? &dst : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  // Phase 1 validates and changes nothing.
  struct Step {
    ObjectSeries* src;
    ObjectSeries* dst;
  };
  std::vector<Step> plan;
  plan.reserve(topics_.size());

  for (auto& [src_id, src_series] : topics_) {
    if (src_series->ordered.empty()) {
      continue;
    }
    ObjectSeries* dst_series = nullptr;
    for (auto& [dst_id, dst_series_ptr] : dst.topics_) {
      if (dst_series_ptr->descriptor.dataset_id == src_series->descriptor.dataset_id &&
          dst_series_ptr->descriptor.topic_name == src_series->descriptor.topic_name) {
        dst_series = dst_series_ptr.get();
        break;
      }
    }
    if (dst_series == nullptr) {
      return unexpected(
          "flushTo: destination has no topic '" + src_series->descriptor.topic_name + "' for dataset " +
          std::to_string(src_series->descriptor.dataset_id));
    }
    plan.push_back({src_series.get(), dst_series});
  }

  // The store locks keep new readers out, but an EntryTimestampsView taken earlier holds only its series lock: drain
  // those before the timestamp vectors change.
  for (auto& step : plan) {
    drainSeriesReaders(*step.src);
    drainSeriesReaders(*step.dst);
    step.dst->ordered.appendReuidFrom(step.src->ordered);
    step.dst->memory_bytes += step.src->memory_bytes;
    step.src->memory_bytes = 0;
    // src's warm cache names entries it no longer holds; dst's stays valid (the moved entries get fresh UIDs).
    dropCache(*step.src, graveyard);

    const Timestamp newest = step.dst->ordered.empty() ? 0 : step.dst->ordered.backTimestamp();
    applyRetention(*step.dst, newest, graveyard);
  }

  return {};
}

Expected<ObjectDatasetReplaceResult> ObjectStore::replaceDatasetFrom(
    ObjectStore& staged, DatasetId staged_id, DatasetId primary_id) {
  if (&staged == this) {
    return unexpected("replaceDatasetFrom: staged and primary are the same store");
  }

  Graveyard graveyard;
  // Address order, as in flushTo().
  ObjectStore* first = this < &staged ? this : &staged;
  ObjectStore* second = first == this ? &staged : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  ObjectDatasetReplaceResult result;

  // The ObjectSeries are heap-owned, so these pointers survive topics_ reallocating below.
  std::unordered_map<std::string, std::pair<ObjectSeries*, ObjectTopicId>> primary_by_name;
  for (auto& [pid, series] : topics_) {
    if (series->descriptor.dataset_id == primary_id) {
      primary_by_name.emplace(series->descriptor.topic_name, std::pair{series.get(), pid});
    }
  }

  std::unordered_set<std::string> staged_names;

  for (auto& [sid, staged_series] : staged.topics_) {
    if (staged_series->descriptor.dataset_id != staged_id) {
      continue;
    }
    const std::string& name = staged_series->descriptor.topic_name;
    staged_names.insert(name);

    ObjectSeries* primary_series = nullptr;
    ObjectTopicId primary_tid;
    if (auto match = primary_by_name.find(name); match != primary_by_name.end()) {
      primary_series = match->second.first;
      primary_tid = match->second.second;
      primary_series->descriptor.metadata_json = staged_series->descriptor.metadata_json;
    } else {
      primary_tid = ObjectTopicId{next_id_++};
      auto series = std::make_unique<ObjectSeries>();
      series->descriptor = staged_series->descriptor;
      series->descriptor.dataset_id = primary_id;
      primary_series = series.get();
      topics_.emplace_back(primary_tid, std::move(series));
      series_index_.emplace(primary_tid.id, primary_series);
    }
    // Both may have outstanding views: drain them before the timestamp vectors move.
    drainSeriesReaders(*primary_series);
    drainSeriesReaders(*staged_series);
    clearEntriesLocked(*primary_series, graveyard);
    primary_series->ordered.adoptReuidFrom(staged_series->ordered);
    primary_series->memory_bytes = staged_series->memory_bytes;
    result.remapped.emplace_back(sid, primary_tid);
    staged_series->memory_bytes = 0;
    // Its warm cache names moved-out entries.
    dropCache(*staged_series, graveyard);
  }

  for (const auto& [name, slot] : primary_by_name) {
    if (staged_names.count(name) == 0) {
      result.removed_topics.push_back(slot.second);
      eraseTopicLocked(slot.second, graveyard);
    }
  }

  return result;
}

// Entries move by value: a lazy callback owns its backing through captured anchors, so it outlives its source dataset.
Expected<ObjectDatasetMergeReport> ObjectStore::mergeDatasets(
    DatasetId anchor_id, const std::vector<DatasetMergeSource>& sources,
    const std::function<bool(const ObjectTopicDescriptor&)>& exclude) {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);

  std::unordered_set<DatasetId> seen_sources;
  for (const DatasetMergeSource& src : sources) {
    if (src.dataset_id == anchor_id) {
      return unexpected("mergeDatasets: source dataset " + std::to_string(src.dataset_id) + " is the anchor");
    }
    if (!seen_sources.insert(src.dataset_id).second) {
      return unexpected("mergeDatasets: source dataset " + std::to_string(src.dataset_id) + " listed more than once");
    }
  }

  struct Contributor {
    ObjectTopicId topic_id;
    ObjectSeries* series;
    Timestamp shift;
  };
  struct Group {
    ObjectTopicId destination_id;
    ObjectSeries* destination;
    Timestamp destination_shift = 0;
    bool destination_needs_reparent = false;
    std::vector<Contributor> sources;
  };

  ObjectDatasetMergeReport report;
  std::vector<Group> groups;
  groups.reserve(topics_.size());
  std::unordered_map<std::string, size_t> group_by_name;

  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id != anchor_id) {
      continue;
    }
    if (exclude && exclude(series->descriptor)) {
      continue;  // its owner merges it
    }
    const auto index = groups.size();
    groups.push_back(
        Group{
            .destination_id = tid,
            .destination = series.get(),
            .destination_shift = 0,
            .destination_needs_reparent = false,
            .sources = {},
        });
    group_by_name.emplace(series->descriptor.topic_name, index);
  }

  for (const DatasetMergeSource& src : sources) {
    std::vector<std::pair<ObjectTopicId, ObjectSeries*>> source_topics;
    for (auto& [tid, series] : topics_) {
      if (series->descriptor.dataset_id == src.dataset_id) {
        source_topics.emplace_back(tid, series.get());
      }
    }
    if (source_topics.empty()) {
      continue;
    }
    report.consumed_datasets.push_back(src.dataset_id);

    for (const auto& [source_id, source_series] : source_topics) {
      if (exclude && exclude(source_series->descriptor)) {
        continue;  // left in place: its owner merges it
      }
      const std::string& name = source_series->descriptor.topic_name;
      const auto group_it = group_by_name.find(name);
      if (group_it == group_by_name.end()) {
        const auto index = groups.size();
        groups.push_back(
            Group{
                .destination_id = source_id,
                .destination = source_series,
                .destination_shift = src.raw_shift_ns,
                .destination_needs_reparent = true,
                .sources = {},
            });
        group_by_name.emplace(name, index);
        report.added_topics.push_back(source_id);
      } else {
        Group& group = groups[group_it->second];
        group.sources.push_back(Contributor{.topic_id = source_id, .series = source_series, .shift = src.raw_shift_ns});
        report.remapped.emplace_back(source_id, group.destination_id);
      }
    }
  }

  for (Group& group : groups) {
    bool has_source_entries = false;
    for (const Contributor& contributor : group.sources) {
      if (!contributor.series->ordered.empty()) {
        has_source_entries = true;
        break;
      }
    }

    if (!group.destination_needs_reparent && group.sources.empty()) {
      continue;
    }
    if (!group.destination_needs_reparent && !has_source_entries) {
      continue;  // shared-name source topics were already empty; only the report changes.
    }

    drainSeriesReaders(*group.destination);
    if (group.destination_needs_reparent) {
      if (group.destination->ordered.shift(group.destination_shift)) {
        dropCache(*group.destination, graveyard);
      }
      group.destination->descriptor.dataset_id = anchor_id;
    }

    std::vector<Contributor> non_empty_sources;
    non_empty_sources.reserve(group.sources.size());
    for (const Contributor& contributor : group.sources) {
      if (contributor.series->ordered.empty()) {
        continue;
      }
      drainSeriesReaders(*contributor.series);
      if (contributor.series->ordered.shift(contributor.shift)) {
        dropCache(*contributor.series, graveyard);
      }
      non_empty_sources.push_back(contributor);
    }

    if (non_empty_sources.empty()) {
      group.destination->ordered.reuidAll();
      dropCache(*group.destination, graveyard);
      const Timestamp newest = group.destination->ordered.empty() ? 0 : group.destination->ordered.backTimestamp();
      applyRetention(*group.destination, newest, graveyard);
      continue;
    }

    std::vector<ObjectEntry> merged;
    merged.reserve(group.destination->ordered.size());
    for (const Contributor& contributor : non_empty_sources) {
      merged.reserve(merged.capacity() + contributor.series->ordered.size());
    }

    size_t merged_memory = group.destination->memory_bytes;
    group.destination->ordered.moveEntriesInto(merged);
    for (const Contributor& contributor : non_empty_sources) {
      merged_memory += contributor.series->memory_bytes;
      contributor.series->ordered.moveEntriesInto(merged);
    }
    std::stable_sort(merged.begin(), merged.end(), [](const ObjectEntry& lhs, const ObjectEntry& rhs) {
      return lhs.timestamp < rhs.timestamp;
    });

    // Every entry gets a fresh UID, so the warm cache goes.
    group.destination->ordered.assignSortedReuid(std::move(merged));
    group.destination->memory_bytes = merged_memory;
    dropCache(*group.destination, graveyard);

    for (const Contributor& contributor : non_empty_sources) {
      clearEntriesLocked(*contributor.series, graveyard);
    }

    const Timestamp newest = group.destination->ordered.empty() ? 0 : group.destination->ordered.backTimestamp();
    applyRetention(*group.destination, newest, graveyard);
  }

  return report;
}

// --- Lifecycle ---

void ObjectStore::removeTopic(ObjectTopicId id) {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);
  eraseTopicLocked(id, graveyard);
}

void ObjectStore::eraseTopicLocked(ObjectTopicId id, Graveyard& graveyard) {
  auto it = std::find_if(topics_.begin(), topics_.end(), [&](const auto& pair) { return pair.first == id; });
  if (it != topics_.end()) {
    drainSeriesReaders(*it->second);  // let outstanding views release before the series dies
    series_index_.erase(id.id);
    graveyard.series.push_back(std::move(it->second));
    topics_.erase(it);
  }
}

void ObjectStore::drainSeriesReaders(ObjectSeries& series) {
  // Waits out the series lock's holders (an EntryTimestampsView keeps it after the store lock drops). Every
  // acquisition goes through store_mutex_, which the caller holds exclusively, so no new holder follows.
  std::unique_lock<std::shared_mutex> drain(series.mutex);
}

void ObjectStore::clearDataset(DatasetId dataset_id) {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id != dataset_id) {
      continue;
    }
    drainSeriesReaders(*series);
    clearEntriesLocked(*series, graveyard);
  }
}

ObjectStore::ObjectDatasetSnapshot ObjectStore::detachDataset(DatasetId dataset_id) {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);
  ObjectDatasetSnapshot snapshot;
  snapshot.dataset_id = dataset_id;
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id != dataset_id) {
      continue;
    }
    snapshot.prior_object_topic_ids.push_back(tid);
    drainSeriesReaders(*series);
    ObjectDatasetSnapshot::SeriesSnapshot series_snapshot;
    // uid_order is not kept: reattachDataset() rebuilds it.
    series->ordered.detachInto(series_snapshot.entries, series_snapshot.entry_timestamps);
    series_snapshot.budget = series->budget;
    series_snapshot.memory_bytes = series->memory_bytes;
    clearEntriesLocked(*series, graveyard);
    snapshot.series.emplace(tid.id, std::move(series_snapshot));
  }
  if (snapshot.prior_object_topic_ids.empty()) {
    return snapshot;  // `valid` stays false
  }
  snapshot.valid = true;
  return snapshot;
}

void ObjectStore::reattachDataset(DatasetId dataset_id, ObjectDatasetSnapshot&& snapshot) {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);
  if (!snapshot.valid) {
    return;
  }
  const std::unordered_set<uint32_t> prior([&snapshot] {
    std::unordered_set<uint32_t> set;
    set.reserve(snapshot.prior_object_topic_ids.size());
    for (const ObjectTopicId id : snapshot.prior_object_topic_ids) {
      set.insert(id.id);
    }
    return set;
  }());
  // Series added since the detach go; prior ones are emptied for the refill below. Ids first: eraseTopicLocked()
  // mutates topics_.
  std::vector<ObjectTopicId> current;
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      current.push_back(tid);
    }
  }
  for (const ObjectTopicId tid : current) {
    if (prior.find(tid.id) == prior.end()) {
      eraseTopicLocked(tid, graveyard);
    } else if (ObjectSeries* series = findSeries(tid)) {
      drainSeriesReaders(*series);
      clearEntriesLocked(*series, graveyard);
    }
  }
  for (auto& [raw_id, series_snapshot] : snapshot.series) {
    ObjectSeries* series = findSeries(ObjectTopicId{raw_id});
    if (series == nullptr || series->descriptor.dataset_id != dataset_id) {
      // Removed while detached. clear() restarts the ids, so the raw id may now name another dataset's topic: the
      // entries are dropped.
      for (ObjectEntry& entry : series_snapshot.entries) {
        graveyard.entries.push_back(std::move(entry));
      }
      continue;
    }
    series->ordered.restoreRebuild(std::move(series_snapshot.entries), std::move(series_snapshot.entry_timestamps));
    series->budget = series_snapshot.budget;
    series->memory_bytes = series_snapshot.memory_bytes;
  }
  snapshot.valid = false;
}

void ObjectStore::clearEntriesLocked(ObjectSeries& series, Graveyard& graveyard) {
  if (series.log) {
    series.log->clear();
  }
  series.ordered.moveEntriesInto(graveyard.entries);
  series.memory_bytes = 0;
  dropCache(series, graveyard);
}

void ObjectStore::dropCache(const ObjectSeries& series, Graveyard& graveyard) {
  std::lock_guard cache_guard(series.cache_mutex);
  if (series.cached_latest.has_value()) {
    graveyard.cached.push_back(std::move(*series.cached_latest));
    series.cached_latest.reset();
  }
}

void ObjectStore::clear() {
  Graveyard graveyard;
  std::unique_lock lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    drainSeriesReaders(*series);
    graveyard.series.push_back(std::move(series));
  }
  topics_.clear();
  series_index_.clear();
  next_id_ = 1;
}

// --- Private helpers ---

ObjectStore::ObjectSeries* ObjectStore::findSeries(ObjectTopicId id) {
  auto it = series_index_.find(id.id);
  return it != series_index_.end() ? it->second : nullptr;
}

const ObjectStore::ObjectSeries* ObjectStore::findSeries(ObjectTopicId id) const {
  auto it = series_index_.find(id.id);
  return it != series_index_.end() ? it->second : nullptr;
}

ResolvedObjectEntry ObjectStore::resolveEntry(const ObjectEntry& entry, bool* served_from_resident) {
  ResolvedObjectEntry resolved;
  resolved.timestamp = entry.timestamp;
  resolved.sequential_uid = entry.sequential_uid;
  resolved.payload_stamp_shift = entry.payload_stamp_shift;
  if (served_from_resident != nullptr) {
    *served_from_resident = false;
  }

  if (const auto* owned = std::get_if<SharedBuffer>(&entry.payload)) {
    // A default-constructed entry holds a null SharedBuffer.
    if (*owned) {
      resolved.payload = sdk::PayloadView{
          Span<const uint8_t>{(*owned)->data(), (*owned)->size()},
          sdk::BufferAnchor{*owned},
      };
    }
  } else if (const auto* lazy = std::get_if<LazyPayload>(&entry.payload)) {
    // Seeded: resident bytes while the pool keeps them; the fetcher after.
    if (lazy->seed != nullptr) {
      if (auto resident = lazy->seed->load()) {
        resolved.payload = std::move(*resident);
        if (served_from_resident != nullptr) {
          *served_from_resident = true;
        }
        return resolved;
      }
    }
    // No fetcher counts as a failed fetch, not an empty payload.
    if (auto fetched = lazy->fetch ? lazy->fetch() : std::nullopt; fetched.has_value()) {
      resolved.payload = std::move(*fetched);
    } else {
      resolved.fetch_failed = true;
    }
  }

  return resolved;
}

SequentialUID ObjectStore::insertLocked(
    ObjectSeries& series, ObjectEntry&& entry, Graveyard& graveyard, const OrderedEntries::Removal* removal) {
  const size_t charged = chargedBytes(entry);
  entry.sequential_uid = SequentialUID::getNext();
  const SequentialUID uid = entry.sequential_uid;
  const size_t first_removed = graveyard.entries.size();
  const OrderedEntries::PushOrder order =
      removal != nullptr ? series.ordered.removeAndPush(*removal, std::move(entry), graveyard.entries)
                         : series.ordered.push(std::move(entry));
  series.memory_bytes += charged;
  accountRemoved(series, graveyard, first_removed);
  if (order == OrderedEntries::PushOrder::kOutOfOrderInsert) {
    dropCache(series, graveyard);
  }
  // Retention is anchored to the newest sample retained, not the just-pushed
  // timestamp (which may be older on an out-of-order insert).
  applyRetention(series, series.ordered.backTimestamp(), graveyard);
  return uid;
}

SequentialUID ObjectStore::insertLogLocked(
    ObjectSeries& series, Timestamp timestamp, const sdk::Log& log, Graveyard& graveyard,
    const OrderedEntries::Removal* removal) {
  const SequentialUID uid = SequentialUID::getNext();
  std::vector<SequentialUID> removed;
  OrderedEntries::PushOrder order{};
  if (removal != nullptr) {
    auto result = series.log->removeAndPush(*removal, timestamp, uid, log);
    order = result.first;
    removed = std::move(result.second);
  } else {
    order = series.log->push(timestamp, uid, log);
  }
  accountLogRemoved(series, removed, graveyard);
  if (order == OrderedEntries::PushOrder::kOutOfOrderInsert) {
    dropCache(series, graveyard);
  }
  applyRetention(series, series.log->backTimestamp(), graveyard);
  return uid;
}

void ObjectStore::accountLogRemoved(
    ObjectSeries& series, const std::vector<SequentialUID>& removed, Graveyard& graveyard) {
  series.memory_bytes = series.log->memoryBytes();
  if (removed.empty()) {
    return;
  }
  std::lock_guard cache_guard(series.cache_mutex);
  if (series.cached_latest && std::ranges::find(removed, series.cached_latest->sequential_uid) != removed.end()) {
    graveyard.cached.push_back(std::move(*series.cached_latest));
    series.cached_latest.reset();
  }
}

ResolvedObjectEntry ObjectStore::resolveLog(const LogEntries& log, size_t index) {
  return ResolvedObjectEntry{
      .timestamp = log.timestamps()[index],
      .sequential_uid = log.uidAt(index),
      .payload = sdk::makePayloadView(log.canonicalBytesAt(index)),
  };
}

std::unique_ptr<ObjectStore::ObjectSeries> ObjectStore::makeSeries(ObjectTopicDescriptor descriptor) {
  auto series = std::make_unique<ObjectSeries>();
  if (isCanonicalLogMetadata(descriptor.metadata_json)) {
    series->log = std::make_unique<LogEntries>();
  }
  series->descriptor = std::move(descriptor);
  return series;
}

std::optional<LogSeriesView> ObjectStore::logView(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr || !series->log) {
    return std::nullopt;
  }
  return LogSeriesView(std::shared_lock(series->mutex), series->log.get());
}

void ObjectStore::accountRemoved(ObjectSeries& series, Graveyard& graveyard, size_t first) {
  if (first == graveyard.entries.size()) {
    return;
  }
  // The warm cache goes with its entry, so its bytes are not pinned past the entry's life.
  std::lock_guard cache_guard(series.cache_mutex);
  for (size_t index = first; index < graveyard.entries.size(); ++index) {
    const ObjectEntry& removed = graveyard.entries[index];
    series.memory_bytes -= chargedBytes(removed);
    if (series.cached_latest && series.cached_latest->sequential_uid == removed.sequential_uid) {
      graveyard.cached.push_back(std::move(*series.cached_latest));
      series.cached_latest.reset();
    }
  }
}

void ObjectStore::evictFront(ObjectSeries& series, size_t count, Graveyard& graveyard) {
  if (count == 0) {
    return;
  }
  if (series.log) {
    accountLogRemoved(series, series.log->evictFront(count), graveyard);
    return;
  }
  // Reserved before anything is unlinked (the entries, and the cache value when it holds one of them), so an
  // allocation failure leaves the series whole.
  reserveMore(graveyard.entries, count);
  {
    std::lock_guard cache_guard(series.cache_mutex);
    if (series.cached_latest) {
      const auto position = series.ordered.indexOfUid(series.cached_latest->sequential_uid);
      if (position && *position < count) {
        reserveOneMore(graveyard.cached);
      }
    }
  }
  const size_t first = graveyard.entries.size();
  series.ordered.evictFront(count, graveyard.entries);
  accountRemoved(series, graveyard, first);
}

void ObjectStore::applyRetention(ObjectSeries& series, Timestamp newest_ts, Graveyard& graveyard) {
  // Each limit counts the oldest entries it expires and evicts them in one pass, so a jump that expires most of a
  // series is linear, not quadratic.
  if (series.log) {
    LogEntries& log = *series.log;
    if (series.budget.time_window_ns > 0) {
      evictFront(
          series, countBefore(log.timestamps(), saturatingSub(newest_ts, series.budget.time_window_ns)), graveyard);
    }
    // Byte budget: whole chunks at a time (a sealed chunk's bytes are freed only with its last row), so the series may
    // sit up to one chunk over it.
    while (series.budget.max_memory_bytes > 0 && !log.empty() && series.memory_bytes > series.budget.max_memory_bytes) {
      evictFront(series, std::min(log.size(), LogEntries::kChunkRows), graveyard);
    }
    if (series.budget.max_entries > 0 && log.size() > series.budget.max_entries) {
      evictFront(series, log.size() - series.budget.max_entries, graveyard);
    }
    return;
  }
  if (series.budget.time_window_ns > 0) {
    evictFront(
        series, countBefore(series.ordered.timestamps(), saturatingSub(newest_ts, series.budget.time_window_ns)),
        graveyard);
  }
  if (series.budget.max_memory_bytes > 0) {
    size_t count = 0;
    for (size_t bytes = series.memory_bytes; count < series.ordered.size() && bytes > series.budget.max_memory_bytes;
         ++count) {
      bytes -= chargedBytes(series.ordered.entryAt(count));
    }
    evictFront(series, count, graveyard);
  }
  if (series.budget.max_entries > 0 && series.ordered.size() > series.budget.max_entries) {
    evictFront(series, series.ordered.size() - series.budget.max_entries, graveyard);
  }
}

}  // namespace PJ
