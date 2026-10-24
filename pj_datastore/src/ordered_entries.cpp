// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/ordered_entries.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iterator>
#include <numeric>

#include "pj_datastore/saturating_time.hpp"

namespace PJ {

OrderedEntries::OrderedEntries() {
  changed(true);
}

void OrderedEntries::changed(bool rewrite) noexcept {
  static std::atomic<uint64_t> next{1};
  identity_.content = next.fetch_add(1, std::memory_order_relaxed);
  if (rewrite) {
    identity_.rewrite = identity_.content;
  }
}

// --- Entry access ---

const ObjectEntry* OrderedEntries::atIndex(size_t index) const {
  if (index >= entries_.size()) {
    return nullptr;
  }
  return &entries_[index];
}

// --- Time lookups ---

std::optional<size_t> OrderedEntries::indexAtOrBefore(Timestamp t) const {
  if (entry_timestamps_.empty()) {
    return std::nullopt;
  }
  auto it = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), t);
  if (it == entry_timestamps_.begin()) {
    return std::nullopt;
  }
  --it;
  return static_cast<size_t>(it - entry_timestamps_.begin());
}

std::vector<OrderedEntries::TimeRangeEntry> OrderedEntries::rangeByTime(Timestamp lo, Timestamp hi) const {
  std::vector<TimeRangeEntry> out;
  if (hi <= lo) {
    return out;  // empty window (also rejects a reversed lo/hi)
  }
  // entry_timestamps_ is ascending and index-aligned with entries_, so (lo, hi]
  // maps to the half-open index range [upper_bound(lo), upper_bound(hi)) and the
  // walk yields ascending-timestamp order with equal-timestamp arrival order
  // preserved (out-of-order inserts land after equals via upper_bound).
  const auto first = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), lo);
  const auto last = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), hi);
  out.reserve(static_cast<size_t>(last - first));
  for (auto it = first; it != last; ++it) {
    const auto idx = static_cast<size_t>(it - entry_timestamps_.begin());
    out.push_back(TimeRangeEntry{entries_[idx].sequential_uid, *it});
  }
  return out;
}

// --- UID-cursor lookups ---

const ObjectEntry* OrderedEntries::atUid(SequentialUID uid) const {
  const std::optional<size_t> index = indexOfUid(uid);
  return index ? &entries_[*index] : nullptr;
}

std::optional<size_t> OrderedEntries::indexOfUid(SequentialUID uid) const {
  const auto it = std::lower_bound(uid_order_.begin(), uid_order_.end(), uid, [this](uint32_t pos, SequentialUID u) {
    return entries_[pos].sequential_uid < u;
  });
  if (it == uid_order_.end() || entries_[*it].sequential_uid != uid) {
    return std::nullopt;
  }
  return *it;
}

SequentialUID OrderedEntries::firstUid() const {
  if (uid_order_.empty()) {
    return {};
  }
  return entries_[uid_order_.front()].sequential_uid;  // smallest RETAINED uid
}

SequentialUID OrderedEntries::nextUidAfter(SequentialUID after) const {
  const auto it = std::upper_bound(uid_order_.begin(), uid_order_.end(), after, [this](SequentialUID u, uint32_t pos) {
    return u < entries_[pos].sequential_uid;
  });
  if (it == uid_order_.end()) {
    return {};
  }
  return entries_[*it].sequential_uid;
}

SequentialUID OrderedEntries::maxUidAtOrBefore(Timestamp t) const {
  const auto end = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), t);
  SequentialUID max_uid;  // invalid (0) when the prefix is empty
  for (auto it = entry_timestamps_.begin(); it != end; ++it) {
    const auto idx = static_cast<size_t>(it - entry_timestamps_.begin());
    if (max_uid < entries_[idx].sequential_uid) {
      max_uid = entries_[idx].sequential_uid;
    }
  }
  return max_uid;
}

std::optional<Timestamp> OrderedEntries::oldestTimestampAfterUid(SequentialUID after) const {
  auto it = std::upper_bound(uid_order_.begin(), uid_order_.end(), after, [this](SequentialUID u, uint32_t pos) {
    return u < entries_[pos].sequential_uid;
  });
  std::optional<Timestamp> oldest;
  for (; it != uid_order_.end(); ++it) {
    const Timestamp stamp = entry_timestamps_[*it];
    oldest = std::min(oldest.value_or(stamp), stamp);
  }
  return oldest;
}

// --- Mutation ---

OrderedEntries::PushOrder OrderedEntries::push(ObjectEntry&& entry) {
  reserveOneMore(entry_timestamps_);
  reserveOneMore(uid_order_);
  // The one allocation left; a deque push_back changes nothing when it throws.
  entries_.push_back(std::move(entry));
  return indexBack();
}

OrderedEntries::PushOrder OrderedEntries::indexBack() {
  const Timestamp timestamp = entries_.back().timestamp;
  // uid_order positions index `entries_` as uint32_t. A series this large is far
  // beyond any real retained window; assert rather than silently narrow.
  assert(entries_.size() < UINT32_MAX);
  // In-order fast path (the overwhelming common case): amortized O(1) append.
  if (entry_timestamps_.empty() || timestamp >= entry_timestamps_.back()) {
    changed(false);  // lands after every entry, an equal stamp's too: a UID cursor finds it
    entry_timestamps_.push_back(timestamp);
    // Newest (max) UID appended at the array tail => also the UID-order tail. O(1)
    // tail check only (the full O(n) checker would make debug ingest O(n^2)).
    uid_order_.push_back(static_cast<uint32_t>(entries_.size() - 1));
    assert(
        uid_order_.size() == entries_.size() &&
        (uid_order_.size() < 2 ||
         entries_[uid_order_[uid_order_.size() - 2]].sequential_uid < entries_[uid_order_.back()].sequential_uid));
    return PushOrder::kInOrderAppend;
  }
  // Out-of-order: move it to the sorted position so entry_timestamps stays ordered
  // (latestAt/indexAt/timestamps() all rely on it). upper_bound puts the new entry
  // AFTER any equal timestamps, preserving arrival order among equals. O(n) shift,
  // but regressions are rare (e.g. multi-publisher /tf whose publish stamps
  // interleave ~1%); the in-order path is untouched.
  const auto pos = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), timestamp);
  const auto idx = pos - entry_timestamps_.begin();
  entry_timestamps_.insert(pos, timestamp);
  std::rotate(entries_.begin() + idx, entries_.end() - 1, entries_.end());
  // Every existing position at or past `idx` shifted up by one; then the new slot
  // (which carries the max UID by precondition) lands at the UID-order tail.
  // Shift-then-push so the freshly appended slot is not itself incremented.
  for (auto& p : uid_order_) {
    if (p >= idx) {
      ++p;
    }
  }
  uid_order_.push_back(static_cast<uint32_t>(idx));
  changed(true);
#ifndef NDEBUG
  checkInvariant();
#endif
  return PushOrder::kOutOfOrderInsert;
}

void OrderedEntries::evictFront(size_t count, std::vector<ObjectEntry>& removed) {
  assert(count <= entries_.size());
  if (count == 0) {
    return;
  }
  reserveMore(removed, count);
  changed(false);
  const auto end = entries_.begin() + static_cast<std::ptrdiff_t>(count);
  std::move(entries_.begin(), end, std::back_inserter(removed));
  entries_.erase(entries_.begin(), end);
  entry_timestamps_.erase(entry_timestamps_.begin(), entry_timestamps_.begin() + static_cast<std::ptrdiff_t>(count));
  // Positions below `count` are gone; the rest slide down by `count`, keeping their UID order.
  const auto first = static_cast<uint32_t>(count);
  size_t next = 0;
  for (const uint32_t position : uid_order_) {
    if (position >= first) {
      uid_order_[next++] = position - first;
    }
  }
  uid_order_.resize(next);
#ifndef NDEBUG
  checkInvariant();
#endif
}

OrderedEntries::Removal OrderedEntries::planRemoval(const EntryPredicate& drop) const {
  Removal plan;
  plan.new_position.resize(entries_.size());
  uint32_t kept = 0;
  auto entry = entries_.begin();
  for (size_t i = 0; i < entries_.size(); ++i, ++entry) {
    if (drop(entry_timestamps_[i], entry->sequential_uid)) {
      plan.new_position[i] = Removal::kRemoved;
      ++plan.count;
    } else {
      plan.new_position[i] = kept++;
    }
  }
  return plan;
}

void OrderedEntries::remove(const Removal& plan, std::vector<ObjectEntry>& removed) {
  if (plan.count == 0) {
    return;
  }
  removed.reserve(removed.size() + plan.count);
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(compact(plan, removed)), entries_.end());
#ifndef NDEBUG
  checkInvariant();
#endif
}

OrderedEntries::PushOrder OrderedEntries::removeAndPush(
    const Removal& plan, ObjectEntry&& entry, std::vector<ObjectEntry>& removed) {
  if (plan.count == 0) {
    return push(std::move(entry));
  }
  removed.reserve(removed.size() + plan.count);
  // The new entry takes a slot the removal freed, so the deque does not grow and nothing
  // below allocates: the vectors had room for the entries removed.
  const auto kept = static_cast<std::ptrdiff_t>(compact(plan, removed));
  entries_[static_cast<size_t>(kept)] = std::move(entry);
  entries_.erase(entries_.begin() + kept + 1, entries_.end());
  return indexBack();
}

size_t OrderedEntries::compact(const Removal& plan, std::vector<ObjectEntry>& removed) {
  assert(plan.new_position.size() == entries_.size());
  assert(removed.capacity() - removed.size() >= plan.count);
  changed(true);
  // ponytail: survivors always shift toward the front, so the sliding window's front
  // removal moves every entry (~7 ns per entry with the predicate). Upgrade: erase a
  // contiguous run with deque::erase, which moves the shorter side.
  auto entry = entries_.begin();
  auto kept_entry = entries_.begin();
  const size_t kept = plan.new_position.size() - plan.count;
  for (size_t i = 0; i < plan.new_position.size(); ++i, ++entry) {
    const uint32_t to = plan.new_position[i];
    if (to == Removal::kRemoved) {
      removed.push_back(std::move(*entry));
      continue;
    }
    if (to != i) {
      *kept_entry = std::move(*entry);
      entry_timestamps_[to] = entry_timestamps_[i];
    }
    ++kept_entry;
  }
  entry_timestamps_.resize(kept);
  // A removal never reorders the survivors' UIDs, so uid_order keeps its order and only
  // renumbers the positions: no sort.
  size_t next = 0;
  for (const uint32_t position : uid_order_) {
    if (plan.new_position[position] != Removal::kRemoved) {
      uid_order_[next++] = plan.new_position[position];
    }
  }
  uid_order_.resize(next);
  return kept;
}

void OrderedEntries::clear() {
  changed(true);
  entries_.clear();
  entry_timestamps_.clear();
  uid_order_.clear();
}

bool OrderedEntries::shift(Timestamp delta) {
  if (delta == 0 || entries_.empty()) {
    return false;
  }
  changed(true);
  for (size_t i = 0; i < entries_.size(); ++i) {
    entries_[i].timestamp = saturatingAdd(entries_[i].timestamp, delta);
    entry_timestamps_[i] = entries_[i].timestamp;
    // Slide the store clock AND remember the slide for payload-embedded stamps:
    // the payload bytes (a serialized canonical object or a wire message) are not
    // rewritten, so a consumer reading their inner timestamps must add this delta.
    entries_[i].payload_stamp_shift = saturatingAdd(entries_[i].payload_stamp_shift, delta);
  }
  return true;
}

void OrderedEntries::reuidAll() {
  changed(true);
  for (ObjectEntry& entry : entries_) {
    entry.sequential_uid = SequentialUID::getNext();
  }
  // Whole array re-UID'd in array order => UID order == array order == identity.
  uid_order_.resize(entries_.size());
  std::iota(uid_order_.begin(), uid_order_.end(), 0u);
}

void OrderedEntries::rebuildUidOrder() {
  uid_order_.resize(entries_.size());
  std::iota(uid_order_.begin(), uid_order_.end(), 0u);
  std::stable_sort(uid_order_.begin(), uid_order_.end(), [this](uint32_t a, uint32_t b) {
    return entries_[a].sequential_uid < entries_[b].sequential_uid;
  });
}

// --- Bulk cross-series ops ---

void OrderedEntries::appendReuidFrom(OrderedEntries& src) {
  for (ObjectEntry& entry : src.entries_) {
    entry.sequential_uid = SequentialUID::getNext();
    entries_.push_back(std::move(entry));
  }
  entry_timestamps_.insert(entry_timestamps_.end(), src.entry_timestamps_.begin(), src.entry_timestamps_.end());
  // Combined entries: our pre-existing prefix may be non-identity (an earlier
  // out-of-order insert); the moved entries carry fresh max UIDs. Rebuild rather
  // than iota so our pre-existing UIDs stay stable (consumer cursors hold them).
  rebuildUidOrder();
  changed(true);
  src.clear();
}

void OrderedEntries::adoptReuidFrom(OrderedEntries& src) {
  entries_ = std::move(src.entries_);
  entry_timestamps_ = std::move(src.entry_timestamps_);
  // The moved entries are timestamp-sorted, so UID order == array order: identity.
  reuidAll();
  src.clear();
}

void OrderedEntries::moveEntriesInto(std::vector<ObjectEntry>& out) {
  for (ObjectEntry& entry : entries_) {
    out.push_back(std::move(entry));
  }
  clear();
}

void OrderedEntries::assignSortedReuid(std::vector<ObjectEntry>&& sorted) {
  entries_.clear();
  entry_timestamps_.clear();
  entry_timestamps_.reserve(sorted.size());
  for (ObjectEntry& entry : sorted) {
    entry_timestamps_.push_back(entry.timestamp);
    entries_.push_back(std::move(entry));
  }
  reuidAll();
}

// --- Detach/reattach snapshot transfer ---

void OrderedEntries::detachInto(std::deque<ObjectEntry>& entries_out, std::vector<Timestamp>& stamps_out) {
  entries_out = std::move(entries_);
  stamps_out = std::move(entry_timestamps_);
  entries_.clear();
  entry_timestamps_.clear();
  uid_order_.clear();
  changed(true);
}

void OrderedEntries::restoreRebuild(std::deque<ObjectEntry>&& entries_in, std::vector<Timestamp>&& stamps_in) {
  entries_ = std::move(entries_in);
  entry_timestamps_ = std::move(stamps_in);
  rebuildUidOrder();
  changed(true);
}

#ifndef NDEBUG
void OrderedEntries::checkInvariant() const {
  assert(uid_order_.size() == entries_.size());
  for (size_t k = 1; k < uid_order_.size(); ++k) {
    assert(entries_[uid_order_[k - 1]].sequential_uid < entries_[uid_order_[k]].sequential_uid);
  }
}
#endif

}  // namespace PJ
