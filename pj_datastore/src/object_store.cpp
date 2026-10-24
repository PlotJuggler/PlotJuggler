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

#include "pj_datastore/marker_topic_metadata.hpp"
#include "pj_datastore/resident_payload_pool.hpp"
#include "pj_datastore/saturating_time.hpp"

namespace PJ {

namespace {

// How many of the oldest entries are stamped before `threshold`: the prefix an eviction before it removes.
size_t countBefore(const OrderedEntries& ordered, Timestamp threshold) {
  const auto& stamps = ordered.timestamps();
  return static_cast<size_t>(std::lower_bound(stamps.begin(), stamps.end(), threshold) - stamps.begin());
}

// Bytes an entry counts against its series' memory budget: an owned payload's
// size, or what a lazy closure itself pins (LazyPayload::resident_bytes).
size_t chargedBytes(const ObjectEntry& entry) {
  if (const auto* owned = std::get_if<SharedBuffer>(&entry.payload)) {
    return *owned ? (*owned)->size() : 0;
  }
  return std::get<LazyPayload>(entry.payload).resident_bytes;
}

// An entry without its UID, which insertLocked mints under the lock.
ObjectEntry unstampedEntry(Timestamp timestamp, ObjectEntryPayload payload) {
  ObjectEntry entry;
  entry.timestamp = timestamp;
  entry.payload = std::move(payload);
  return entry;
}

SharedBuffer ownedPayload(std::vector<uint8_t> bytes) {
  return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

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
  auto series = std::make_unique<ObjectSeries>();
  series->descriptor = std::move(descriptor);
  topics_.emplace_back(id, std::move(series));
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
  auto series = std::make_unique<ObjectSeries>();
  series->descriptor = std::move(descriptor);
  topics_.emplace_back(id, std::move(series));
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
  return s->descriptor;  // copy under the lock — no reference escapes into the caller
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
      retained.entry_count = series->ordered.size();
      if (retained.entry_count > 0) {
        retained.first = series->ordered.frontTimestamp();
        retained.last = series->ordered.backTimestamp();
      }
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

  // Build the entry BEFORE taking the series write lock: the make_shared control-block
  // allocation is the costliest step on this path and needs nothing the lock protects,
  // and a GUI reader draining this same series waits out every nanosecond spent here.
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
  };
  std::vector<Step> steps;
  std::vector<SequentialUID> created;
  std::unique_lock store_lock(store_mutex_);

  // Phase 1 decides everything and changes no topic, so a rejection leaves every topic
  // as it was. It also allocates what phase 2 would.
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
      Step& step = steps.emplace_back(Step{.series = series, .removal = {}, .entry = {}});
      if (write.drop) {
        step.removal = series->ordered.planRemoval(write.drop);
      }
      if (write.append) {
        step.entry = unstampedEntry(write.timestamp, ownedPayload(std::move(write.payload)));
      }
      // What phase 2 can move to the graveyard: the planned removals, or, under a retention
      // budget, every entry the topic holds after the write.
      const RetentionBudget& budget = series->budget;
      const bool retains = budget.time_window_ns > 0 || budget.max_memory_bytes > 0 || budget.max_entries > 0;
      graveyard_room += retains ? series->ordered.size() + (write.append ? 1 : 0) : step.removal.count;
    }
    graveyard.entries.reserve(graveyard_room);
    graveyard.cached.reserve(writes.size());  // a series holds at most one cached value
  } catch (const std::exception& error) {
    return unexpected(std::string("object bundle rejected: ") + error.what());
  }

  // Phase 2 changes each topic in place, whole or not at all; the exclusive lock keeps
  // every reader out until the last one changed. Only the deque growth of an append
  // that removes nothing can still throw.
  for (size_t index = 0; index < writes.size(); ++index) {
    Step& step = steps[index];
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
    // Admitting can evict other residents, so reject an unknown topic first. (The
    // topic can still vanish before pushLazyEntry re-checks; that race only costs
    // the evictions this check avoids in the common case.)
    {
      std::shared_lock store_lock(store_mutex_);
      if (findSeries(id) == nullptr) {
        return unexpected("unknown topic");
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
  // Declared before the lock: if the entry was evicted while it resolved, this copy holds the
  // last reference to a lazy fetch context, whose cleanup may call back into the store.
  ObjectEntry snapshot;
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  // Snapshot the entry under the series lock, then RELEASE it before resolving:
  // a slow lazy fetch (file re-read + decompress) must never block writers
  // pushing to this series. The snapshot is cheap — the payload variant copies
  // as refcount bumps / a closure copy. store_lock stays held so the series
  // object itself cannot be destroyed. An entry evicted mid-resolve is harmless:
  // the snapshot owns every capture it needs.
  {
    std::shared_lock lock(series->mutex);
    const auto idx = series->ordered.indexAtOrBefore(timestamp);
    if (!idx.has_value()) {
      return std::nullopt;
    }
    const ObjectEntry& entry = series->ordered.entryAt(*idx);

    // Warm cache: a ~60 Hz reader landing on the same sample as last time is
    // served without re-invoking the (possibly decompressing/file-backed) lazy
    // fetcher. Keyed by sequential_uid, which is never reused, so a hit is
    // always the exact same entry.
    {
      std::lock_guard cache_guard(series->cache_mutex);
      if (series->cached_latest && series->cached_latest->sequential_uid == entry.sequential_uid) {
        return *series->cached_latest;
      }
    }
    snapshot = entry;
  }

  // Drop store_mutex_ too, not just the series lock: the resolve below can re-read
  // and decompress a file, and store_mutex_ is the store-WIDE lock — holding it
  // shared across that stalls every exclusive acquirer (registerTopic / flushTo /
  // clearDataset), which in turn blocks readers of unrelated topics once it gets in.
  // It also keeps a fetch callback that re-enters the store off a recursive shared
  // acquisition, which some std::shared_mutex implementations deadlock on.
  // The snapshot owns every capture the resolve needs, so nothing here depends on
  // `series` staying alive.
  store_lock.unlock();

  bool served_from_resident = false;
  ResolvedObjectEntry resolved = resolveEntry(snapshot, &served_from_resident);
  // Don't memoize a failed/empty resolve — let the next read retry instead of
  // latching the failure. A resident-slot hit is not memoized either: it is
  // already cheap, and caching it would hold a second anchor owner outside the
  // ResidentPayloadPool's byte budget. (Once the slot is evicted the fallback
  // resolve lands here and is cached like any lazy entry.)
  if (!resolved.payload.bytes.empty() && !served_from_resident) {
    // Re-find rather than reuse `series`: the pointer was only valid under the lock
    // we just dropped, and the topic may have been removed meanwhile (then there is
    // simply nothing to warm).
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

  // Snapshot-then-resolve, same as latestAt: the series lock must not be held
  // across a slow lazy fetch.
  ObjectEntry snapshot;
  {
    std::shared_lock lock(series->mutex);
    const ObjectEntry* entry = series->ordered.atIndex(index);
    if (entry == nullptr) {
      return std::nullopt;
    }
    snapshot = *entry;
  }
  store_lock.unlock();  // never resolve under store_mutex_ — see latestAt
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
    const ObjectEntry* entry = series->ordered.atUid(sequential_uid);
    if (entry == nullptr) {
      return std::nullopt;
    }
    snapshot = *entry;
  }
  store_lock.unlock();  // never resolve under store_mutex_ — see latestAt
  return resolveEntry(snapshot);
}

std::optional<size_t> ObjectStore::indexAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }

  std::shared_lock lock(series->mutex);
  return series->ordered.indexAtOrBefore(timestamp);
}

std::optional<size_t> ObjectStore::indexOf(ObjectTopicId id, SequentialUID uid) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  std::shared_lock lock(series->mutex);
  return series->ordered.indexOfUid(uid);
}

OrderedEntries::ContentIdentity ObjectStore::contentIdentity(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return series->ordered.contentIdentity();
}

SequentialUID ObjectStore::firstSequentialUID(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  return series->ordered.firstUid();
}

std::vector<ResolvedObjectEntry> ObjectStore::drainNewSince(ObjectTopicId id, SequentialUID& cursor) const {
  // Phase 1 — copy the unconsumed arrivals out under ONE pass of the two locks.
  // The copy is cheap (the payload variant copies as a refcount bump / closure
  // copy) and it is what lets phase 2 run lock-free.
  std::vector<ObjectEntry> batch;
  {
    const std::shared_lock<std::shared_mutex> store_lock(store_mutex_);
    const auto* series = findSeries(id);
    if (series == nullptr) {
      return {};  // unknown topic: an empty batch
    }
    const std::shared_lock<std::shared_mutex> lock(series->mutex);
    batch.reserve(series->ordered.size());
    for (SequentialUID uid = series->ordered.nextUidAfter(cursor); uid.valid();
         uid = series->ordered.nextUidAfter(uid)) {
      const ObjectEntry* entry = series->ordered.atUid(uid);
      if (entry == nullptr) {
        break;  // unreachable while the lock is held; bail rather than spin on a bad index
      }
      batch.push_back(*entry);
    }
  }

  // Phase 2 — resolve with NO lock held: a lazy fetch may re-read and decompress
  // from a file, which must stall neither this series' writers nor, via
  // store_mutex_, every other topic. Each snapshot owns everything it needs.
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
  return series->ordered.maxUidAtOrBefore(t);
}

std::optional<Timestamp> ObjectStore::oldestArrivalAfter(ObjectTopicId id, SequentialUID after) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  std::shared_lock lock(series->mutex);
  return series->ordered.oldestTimestampAfterUid(after);
}

std::optional<ObjectStore::TimeRangeEntry> ObjectStore::latestEntryIdAt(ObjectTopicId id, Timestamp timestamp) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return std::nullopt;
  }
  // latestAt()'s locked prefix without the snapshot/resolve: the same
  // indexAtOrBefore pick, read as pure metadata.
  std::shared_lock lock(series->mutex);
  const auto idx = series->ordered.indexAtOrBefore(timestamp);
  if (!idx.has_value()) {
    return std::nullopt;
  }
  const ObjectEntry& entry = series->ordered.entryAt(*idx);
  return TimeRangeEntry{entry.sequential_uid, entry.timestamp};
}

std::vector<ObjectStore::TimeRangeEntry> ObjectStore::rangeByTime(ObjectTopicId id, Timestamp lo, Timestamp hi) const {
  if (hi <= lo) {
    return {};  // empty window (also rejects a reversed lo/hi), before taking any lock
  }
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }
  std::shared_lock lock(series->mutex);
  return series->ordered.rangeByTime(lo, hi);
}

size_t ObjectStore::entryCount(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return 0;
  }

  std::shared_lock lock(series->mutex);
  return series->ordered.size();
}

std::pair<Timestamp, Timestamp> ObjectStore::timeRange(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {0, 0};
  }

  std::shared_lock lock(series->mutex);
  if (series->ordered.empty()) {
    return {0, 0};
  }
  return {series->ordered.frontTimestamp(), series->ordered.backTimestamp()};
}

EntryTimestampsView ObjectStore::entryTimestamps(ObjectTopicId id) const {
  std::shared_lock store_lock(store_mutex_);
  const auto* series = findSeries(id);
  if (series == nullptr) {
    return {};
  }

  std::shared_lock lock(series->mutex);
  return {std::move(lock), &series->ordered.timestamps()};
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
  evictFront(*series, countBefore(series->ordered, threshold), graveyard);
}

void ObjectStore::evictAllBefore(Timestamp threshold) {
  Graveyard graveyard;
  std::shared_lock store_lock(store_mutex_);
  for (auto& [tid, series] : topics_) {
    std::unique_lock lock(series->mutex);
    evictFront(*series, countBefore(series->ordered, threshold), graveyard);
  }
}

// --- Cross-store flush ---

Status ObjectStore::flushTo(ObjectStore& dst) {
  if (&dst == this) {
    return unexpected("flushTo: source and destination are the same store");
  }

  Graveyard graveyard;
  // Deterministic lock order by address to avoid deadlock with concurrent flushTo calls.
  ObjectStore* first = this < &dst ? this : &dst;
  ObjectStore* second = first == this ? &dst : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  // Phase 1: validate every source series can be matched to a destination
  // topic by descriptor and that the move respects monotonicity. No mutation.
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
    if (!dst_series->ordered.empty() && src_series->ordered.frontTimestamp() < dst_series->ordered.backTimestamp()) {
      return unexpected("flushTo: monotonicity violation for topic '" + src_series->descriptor.topic_name + "'");
    }
    plan.push_back({src_series.get(), dst_series});
  }

  // Phase 2: execute the moves. Holding both store_mutex_ unique blocks new
  // readers/writers, but a reader that acquired an EntryTimestampsView BEFORE this
  // call still holds only the series lock — drain those before reallocating the
  // timestamp vectors we are about to move/insert into.
  for (auto& step : plan) {
    drainSeriesReaders(*step.src);
    drainSeriesReaders(*step.dst);
    // Re-UID the moved entries into dst, append them, extend timestamps, rebuild
    // dst's uid_order (dst keeps its own UIDs stable), and empty src's triple.
    step.dst->ordered.appendReuidFrom(step.src->ordered);
    step.dst->memory_bytes += step.src->memory_bytes;
    step.src->memory_bytes = 0;
    // src's entries were moved out (and re-UIDed into dst); its warm cache now
    // refers to entries it no longer owns. dst keeps its cache: its pre-existing
    // entries are untouched, and any newly-appended entry has a fresh UID that
    // simply misses.
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
  // Deterministic lock order by address (same discipline as flushTo).
  ObjectStore* first = this < &staged ? this : &staged;
  ObjectStore* second = first == this ? &staged : this;
  std::unique_lock first_lock(first->store_mutex_);
  std::unique_lock second_lock(second->store_mutex_);

  ObjectDatasetReplaceResult result;

  // Index primary (this) series under primary_id, by name -> (series, id).
  // Pointers stay valid across topics_.emplace_back below because the
  // ObjectSeries are heap-owned by unique_ptr — a vector realloc moves the
  // pointers, not the pointees.
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

    // Resolve target series+id once (matched keeps its ObjectTopicId + adopts metadata; added mints a fresh one),
    // then run the identical entry move below a single time.
    ObjectSeries* primary_series = nullptr;
    ObjectTopicId primary_tid;
    if (auto match = primary_by_name.find(name); match != primary_by_name.end()) {
      primary_series = match->second.first;
      primary_tid = match->second.second;
      primary_series->descriptor.metadata_json = staged_series->descriptor.metadata_json;  // adopt reloaded metadata
    } else {
      primary_tid = ObjectTopicId{next_id_++};
      auto series = std::make_unique<ObjectSeries>();
      series->descriptor = staged_series->descriptor;
      series->descriptor.dataset_id = primary_id;
      primary_series = series.get();  // heap-owned: stays valid after emplace_back reallocs topics_
      topics_.emplace_back(primary_tid, std::move(series));
      series_index_.emplace(primary_tid.id, primary_series);
    }
    // A matched primary series may have outstanding views (the staged one may too);
    // drain both before replacing/moving their timestamp vectors. A freshly added
    // primary_series has no readers yet, so its drain is uncontended.
    drainSeriesReaders(*primary_series);
    drainSeriesReaders(*staged_series);
    // Retire the primary's old entries (and its warm cache) into the graveyard,
    // then adopt the staged entries + timestamps, mint fresh ascending identity
    // UIDs (the moved staged entries are timestamp-sorted, so UID order == array
    // order), and empty the staged triple.
    clearEntriesLocked(*primary_series, graveyard);
    primary_series->ordered.adoptReuidFrom(staged_series->ordered);
    primary_series->memory_bytes = staged_series->memory_bytes;
    result.remapped.emplace_back(sid, primary_tid);
    staged_series->memory_bytes = 0;  // entries/timestamps already moved-from
    // The staged series' warm cache still refers to moved-from entries; drop it.
    dropCache(*staged_series, graveyard);
  }

  // Remove primary topics the reloaded dataset no longer provides.
  for (const auto& [name, slot] : primary_by_name) {
    if (staged_names.count(name) == 0) {
      result.removed_topics.push_back(slot.second);
      eraseTopicLocked(slot.second, graveyard);
    }
  }

  return result;
}

// Lazy callbacks own their backing through captured anchors; dataset removal only drops entries, so move them by value.
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
      continue;  // owner merges this topic set-aware; keep it out of the fold
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
        continue;  // left in place for the owner's set-aware merge
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

    // Reseat the destination from the sorted pool with fresh ascending identity
    // UIDs; its entries are all new, so drop its warm cache.
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
  // Acquire-and-release: blocks until in-flight shared readers drain. With
  // store_mutex_ held exclusively by the caller, nothing can re-acquire the series
  // lock after this returns, so the series is safe to destroy or mutate.
  std::unique_lock<std::shared_mutex> drain(series.mutex);
}

void ObjectStore::clearDataset(DatasetId dataset_id) {
  // Exclusive store lock blocks new readers/writers; for each series in the
  // dataset, drain any reader holding only the series lock (an EntryTimestampsView)
  // before emptying its timestamp vector, then clear it in place — keeping the
  // ObjectTopicId registered (never removeTopic+re-register). Unknown dataset_id
  // matches no series -> no-op; clearing an empty series is a no-op (idempotent).
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
  // Same discipline as clearDataset, but MOVE each series' entries aside instead of
  // dropping them. drainSeriesReaders before touching a series so no EntryTimestampsView
  // dangles into the timestamp vector we move out.
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
    // Move entries + timestamps out (uid_order is derivable — reattach rebuilds it).
    series->ordered.detachInto(series_snapshot.entries, series_snapshot.entry_timestamps);
    series_snapshot.budget = series->budget;
    series_snapshot.memory_bytes = series->memory_bytes;
    // Normalize the now-empty series in place (memory accounting + warm cache),
    // keeping the ObjectTopicId registered — exactly like clearDataset.
    clearEntriesLocked(*series, graveyard);
    snapshot.series.emplace(tid.id, std::move(series_snapshot));
  }
  if (snapshot.prior_object_topic_ids.empty()) {
    return snapshot;  // unknown / empty dataset: `valid` stays false
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
  // Reconcile the current series set against the prior one. Snapshot the current
  // ids first since eraseTopicLocked mutates topics_. A series the failed refill
  // added (not in `prior`) is erased; a prior series is cleared before its entries
  // are moved back. (When the caller already evicted the added topics, `current`
  // simply has none to erase — tolerated.)
  std::vector<ObjectTopicId> current;
  for (auto& [tid, series] : topics_) {
    if (series->descriptor.dataset_id == dataset_id) {
      current.push_back(tid);
    }
  }
  for (const ObjectTopicId tid : current) {
    if (prior.find(tid.id) == prior.end()) {
      eraseTopicLocked(tid, graveyard);  // drains + erases internally
    } else if (ObjectSeries* series = findSeries(tid)) {
      drainSeriesReaders(*series);
      clearEntriesLocked(*series, graveyard);
    }
  }
  // Move the prior entries + budget + memory back into the (still-registered)
  // series. clearEntriesLocked above already reset each prior series' warm cache.
  for (auto& [raw_id, series_snapshot] : snapshot.series) {
    ObjectSeries* series = findSeries(ObjectTopicId{raw_id});
    if (series == nullptr || series->descriptor.dataset_id != dataset_id) {
      // The prior series was removed while detached (evicted, or clear()ed — which
      // also restarts id allocation, so the raw id may now name ANOTHER dataset's
      // topic). Its entries have nowhere to go; never restore them into a stranger.
      for (ObjectEntry& entry : series_snapshot.entries) {
        graveyard.entries.push_back(std::move(entry));
      }
      continue;
    }
    // Restored entries may carry preserved out-of-order UID inversions, so
    // restoreRebuild sorts uid_order from them rather than assuming identity.
    series->ordered.restoreRebuild(std::move(series_snapshot.entries), std::move(series_snapshot.entry_timestamps));
    series->budget = series_snapshot.budget;
    series->memory_bytes = series_snapshot.memory_bytes;
  }
  snapshot.valid = false;
}

void ObjectStore::clearEntriesLocked(ObjectSeries& series, Graveyard& graveyard) {
  series.ordered.moveEntriesInto(graveyard.entries);
  series.memory_bytes = 0;
  // The warm cache now refers to dropped entries.
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
    // Span the vector, anchor on the same shared_ptr — refcount bump, no copy.
    // A default-constructed entry holds a null SharedBuffer, so guard it.
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
    // Forward the closure's PayloadView verbatim. The anchor stays opaque (no
    // cast), so producers can back it with arrow::Buffer, mmap, or a C-ABI anchor.
    // No fetcher at all counts as a failed fetch, not a legitimately empty payload.
    if (auto fetched = lazy->fetch ? lazy->fetch() : std::nullopt; fetched.has_value()) {
      resolved.payload = std::move(*fetched);
    } else {
      resolved.fetch_failed = true;  // source could not re-produce the bytes
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
  // A mid-stream (out-of-order) insert can change an at-or-before lookup that
  // landed on the entry just after it, so drop the warm cache (keyed by uid).
  if (order == OrderedEntries::PushOrder::kOutOfOrderInsert) {
    dropCache(series, graveyard);
  }
  // Retention is anchored to the newest sample retained, not the just-pushed
  // timestamp (which may be older on an out-of-order insert).
  applyRetention(series, series.ordered.backTimestamp(), graveyard);
  return uid;
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
  // Room for the entries and, when the warm cache holds one of them, the displaced cache value
  // before anything is unlinked, so an allocation failure leaves the series whole.
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
  if (series.budget.time_window_ns > 0) {
    evictFront(series, countBefore(series.ordered, saturatingSub(newest_ts, series.budget.time_window_ns)), graveyard);
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
