// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/resident_payload_pool.hpp"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace PJ {

namespace detail {

// Shared between the pool facade and every slot it minted, so accounting stays
// correct regardless of which of the two dies first. Holds only weak slot
// references: a slot destroyed with its entry retires itself through here.
// Payloads at or below this are the "small" class and are evicted only by other
// small payloads. Without the split, a handful of large payloads evict thousands
// of small ones: MEASURED on a 373 MB mcap, 598 PointCloud2 messages filled the
// whole 256 MB budget and evicted 19,817 of 29,360 admissions, so 32% of the
// GUI's TF reads missed and fell back to the lazy path — a file re-read plus a
// whole-chunk decompress each, ~2.7 ms of GUI-thread CPU per message. The two
// classes differ by ~3 orders of magnitude in size (a TF message is ~200 B, a
// point cloud ~500 KB), so one FIFO over one budget cannot serve both.
constexpr size_t kSmallPayloadBytes = 64 * 1024;

// Share of the budget reserved for the small class. Small payloads are numerous
// but tiny — 28,761 TF messages total under 6 MB — so a modest reservation holds
// an entire topic's history while leaving the bulk of the budget to large
// payloads, whose working set is what the budget was sized for.
constexpr size_t kSmallClassShareDenominator = 8;

struct ResidentPoolState {
  size_t capacity_bytes = 0;        // immutable after construction
  size_t small_capacity_bytes = 0;  // reserved subset of capacity_bytes

  std::mutex mutex;
  std::deque<std::weak_ptr<ResidentSlot>> fifo;
  std::deque<std::weak_ptr<ResidentSlot>> small_fifo;
  size_t resident_bytes = 0;
  size_t small_resident_bytes = 0;
  size_t high_water_bytes = 0;
  uint64_t admitted = 0;
  uint64_t evicted = 0;

  std::atomic<uint64_t> rejected_oversize{0};
  std::atomic<uint64_t> resident_hits{0};

  explicit ResidentPoolState(size_t capacity)
      : capacity_bytes(capacity), small_capacity_bytes(capacity / kSmallClassShareDenominator) {}

  /// Whether `bytes` belongs to the reserved small class. A payload larger than
  /// the small class's ENTIRE capacity cannot be small — it could never fit,
  /// and calling it small would make every admission evict the whole class and
  /// still not fit. That also keeps a pool too small to be worth splitting
  /// behaving exactly as a single-class pool.
  [[nodiscard]] bool isSmall(size_t bytes) const {
    return bytes <= std::min(kSmallPayloadBytes, small_capacity_bytes);
  }

  // Subtract a retiring slot's charge. Called from ~ResidentSlot on arbitrary
  // threads (never while the pool mutex is held — see the destructor).
  void subtract(size_t bytes) {
    std::lock_guard lock(mutex);
    resident_bytes -= bytes;
    if (isSmall(bytes)) {
      small_resident_bytes -= bytes;
    }
  }
};

}  // namespace detail

// --- ResidentSlot ---

ResidentSlot::ResidentSlot(
    std::shared_ptr<detail::ResidentPoolState> state, sdk::PayloadView payload, size_t charged_bytes)
    : payload_(std::move(payload)), state_(std::move(state)), charged_bytes_(charged_bytes) {}

ResidentSlot::~ResidentSlot() {
  // Pool eviction paths hold a strong reference while they take(), so this
  // destructor never races them; a still-present payload here means the entry
  // died first. The payload (and its anchor) is released in this scope, on the
  // destroying thread, with no pool lock held — take() and subtract() acquire
  // and release their locks sequentially, never together.
  if (auto taken = take()) {
    state_->subtract(charged_bytes_);
  }
}

std::optional<sdk::PayloadView> ResidentSlot::load() const {
  std::optional<sdk::PayloadView> payload;
  {
    std::lock_guard lock(mutex_);
    payload = payload_;
  }
  if (payload.has_value()) {
    state_->resident_hits.fetch_add(1, std::memory_order_relaxed);
  }
  return payload;
}

std::optional<sdk::PayloadView> ResidentSlot::take() {
  std::lock_guard lock(mutex_);
  auto taken = std::move(payload_);
  payload_.reset();
  return taken;
}

// --- ResidentPayloadPool ---

ResidentPayloadPool::ResidentPayloadPool(size_t capacity_bytes)
    : state_(std::make_shared<detail::ResidentPoolState>(capacity_bytes)) {}

ResidentPayloadPool::~ResidentPayloadPool() = default;

std::shared_ptr<ResidentSlot> ResidentPayloadPool::admit(sdk::PayloadView payload) {
  const size_t bytes = payload.bytes.size();
  if (bytes == 0 || state_->capacity_bytes == 0) {
    return nullptr;
  }
  if (bytes > state_->capacity_bytes) {
    // Never evict useful residents for a payload that cannot fit regardless.
    state_->rejected_oversize.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  // Plain new: the minting constructor is private to this friend, so
  // make_shared cannot reach it.
  auto slot = std::shared_ptr<ResidentSlot>(new ResidentSlot(state_, std::move(payload), bytes));

  // Take FIFO victims' payloads and subtract their charges inside the pool
  // critical section (the cap is hard, never transiently over-reported), but
  // destroy them — running the anchors' plugin release code — only after the
  // lock drops. take() may return nullopt only for a slot another pool path
  // already emptied; its charge was subtracted then.
  const bool is_small = state_->isSmall(bytes);
  std::vector<sdk::PayloadView> released;
  {
    std::lock_guard lock(state_->mutex);
    // Evict within the payload's OWN class only. A large payload may never take
    // a small one's reservation, which is the whole point: otherwise a few
    // hundred multi-hundred-KB payloads roll the entire budget and a numerous,
    // tiny topic is never resident when its reader arrives.
    auto& fifo = is_small ? state_->small_fifo : state_->fifo;
    const size_t class_capacity =
        is_small ? state_->small_capacity_bytes : (state_->capacity_bytes - state_->small_capacity_bytes);
    // small_resident_bytes is a SUBSET of resident_bytes, so the large class's
    // own usage is the difference — comparing the total against the large
    // capacity would charge large payloads for the small reservation twice.
    auto class_bytes = [this, is_small]() -> size_t {
      return is_small ? state_->small_resident_bytes : (state_->resident_bytes - state_->small_resident_bytes);
    };
    while (class_bytes() + bytes > class_capacity && !fifo.empty()) {
      auto victim = fifo.front().lock();
      fifo.pop_front();
      if (victim == nullptr) {
        continue;  // slot already died with its entry and retired itself
      }
      if (auto taken = victim->take()) {
        // Both counters are charged for a small payload (small_resident_bytes is
        // a subset of resident_bytes), so both must be credited back.
        state_->resident_bytes -= victim->charged_bytes_;
        if (is_small) {
          state_->small_resident_bytes -= victim->charged_bytes_;
        }
        state_->evicted += 1;
        released.push_back(std::move(*taken));
      }
    }
    state_->resident_bytes += bytes;
    if (is_small) {
      state_->small_resident_bytes += bytes;
    }
    state_->high_water_bytes = std::max(state_->high_water_bytes, state_->resident_bytes);
    state_->admitted += 1;
    fifo.push_back(slot);
  }
  // `released` drops here — anchors run outside the pool mutex.
  return slot;
}

void ResidentPayloadPool::trim() {
  std::vector<sdk::PayloadView> released;
  {
    std::lock_guard lock(state_->mutex);
    // BOTH classes: trim means "release everything", and the small class is a
    // separate FIFO that the large-class walk cannot see.
    for (auto* fifo : {&state_->fifo, &state_->small_fifo}) {
      const bool is_small = fifo == &state_->small_fifo;
      for (auto& weak : *fifo) {
        auto slot = weak.lock();
        if (slot == nullptr) {
          continue;
        }
        if (auto taken = slot->take()) {
          state_->resident_bytes -= slot->charged_bytes_;
          if (is_small) {
            state_->small_resident_bytes -= slot->charged_bytes_;
          }
          state_->evicted += 1;
          released.push_back(std::move(*taken));
        }
      }
      fifo->clear();
    }
  }
}

ResidentPayloadPool::Stats ResidentPayloadPool::stats() const {
  Stats out;
  out.capacity_bytes = state_->capacity_bytes;
  out.rejected_oversize = state_->rejected_oversize.load(std::memory_order_relaxed);
  out.resident_hits = state_->resident_hits.load(std::memory_order_relaxed);
  std::lock_guard lock(state_->mutex);
  out.resident_bytes = state_->resident_bytes;
  out.high_water_bytes = state_->high_water_bytes;
  out.admitted = state_->admitted;
  out.evicted = state_->evicted;
  return out;
}

}  // namespace PJ
