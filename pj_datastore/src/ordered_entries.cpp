// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/ordered_entries.hpp"

#include <algorithm>
#include <cassert>
#include <numeric>

#include "saturating_time.hpp"

namespace PJ {

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
  if (uid_order_.empty()) {
    return nullptr;
  }
  const auto it = std::lower_bound(uid_order_.begin(), uid_order_.end(), uid, [this](uint32_t pos, SequentialUID u) {
    return entries_[pos].sequential_uid < u;
  });
  if (it == uid_order_.end() || entries_[*it].sequential_uid != uid) {
    return nullptr;
  }
  return &entries_[*it];
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

// --- Mutation ---

OrderedEntries::PushOrder OrderedEntries::push(ObjectEntry&& entry) {
  const Timestamp timestamp = entry.timestamp;
  // uid_order positions index `entries_` as uint32_t. A series this large is far
  // beyond any real retained window; assert rather than silently narrow.
  assert(entries_.size() < UINT32_MAX);
  // In-order fast path (the overwhelming common case): amortized O(1) append.
  if (entry_timestamps_.empty() || timestamp >= entry_timestamps_.back()) {
    entries_.push_back(std::move(entry));
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
  // Out-of-order: insert at the sorted position so entry_timestamps stays ordered
  // (latestAt/indexAt/timestamps() all rely on it). upper_bound puts the new entry
  // AFTER any equal timestamps, preserving arrival order among equals. O(n) shift,
  // but regressions are rare (e.g. multi-publisher /tf whose publish stamps
  // interleave ~1%); the in-order path is untouched.
  const auto pos = std::upper_bound(entry_timestamps_.begin(), entry_timestamps_.end(), timestamp);
  const auto idx = pos - entry_timestamps_.begin();
  entry_timestamps_.insert(pos, timestamp);
  entries_.insert(entries_.begin() + idx, std::move(entry));
  // Every existing position at or past `idx` shifted up by one; then the new slot
  // (which carries the max UID by precondition) lands at the UID-order tail.
  // Shift-then-push so the freshly appended slot is not itself incremented.
  for (auto& p : uid_order_) {
    if (p >= idx) {
      ++p;
    }
  }
  uid_order_.push_back(static_cast<uint32_t>(idx));
#ifndef NDEBUG
  checkInvariant();
#endif
  return PushOrder::kOutOfOrderInsert;
}

ObjectEntry OrderedEntries::evictFront() {
  assert(!entries_.empty());
  ObjectEntry front = std::move(entries_.front());
  entries_.pop_front();
  entry_timestamps_.erase(entry_timestamps_.begin());
  // Front entry (position 0) is gone: drop its slot from uid_order (exactly one
  // slot holds 0 for a non-empty series), then shift every remaining position down
  // by one. Independent of the pop above — it only renumbers integer positions.
  const auto z = std::find(uid_order_.begin(), uid_order_.end(), 0u);
  assert(z != uid_order_.end() && "evictFront: uid_order lost the front slot (desynced from entries)");
  if (z != uid_order_.end()) {
    uid_order_.erase(z);
  }
  for (auto& p : uid_order_) {
    --p;
  }
#ifndef NDEBUG
  checkInvariant();
#endif
  return front;
}

void OrderedEntries::clear() {
  entries_.clear();
  entry_timestamps_.clear();
  uid_order_.clear();
}

bool OrderedEntries::shift(Timestamp delta) {
  if (delta == 0 || entries_.empty()) {
    return false;
  }
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
}

void OrderedEntries::restoreRebuild(std::deque<ObjectEntry>&& entries_in, std::vector<Timestamp>&& stamps_in) {
  entries_ = std::move(entries_in);
  entry_timestamps_ = std::move(stamps_in);
  rebuildUidOrder();
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
