// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/resident_payload_pool.hpp"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace PJ {

namespace detail {

// Payloads at or below this form the small class, evicted only by other small payloads: in one FIFO, a few large
// payloads evict thousands of small ones (docs/TECHNICAL_NOTES.md "ObjectStore costs").
constexpr size_t kSmallPayloadBytes = 64 * 1024;

// Share of the budget reserved for the small class.
constexpr size_t kSmallClassShareDenominator = 8;

// Shared by the pool and every slot it minted, so the accounting survives whichever dies first. Holds only weak slot
// references: a slot destroyed with its entry retires its charge through here.
struct ResidentPoolState {
  size_t capacity_bytes = 0;        // immutable, so read without the mutex
  size_t small_capacity_bytes = 0;  // part of capacity_bytes; immutable

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

  /// Capped by the small class's capacity: a larger payload could never fit it.
  [[nodiscard]] bool isSmall(size_t bytes) const {
    return bytes <= std::min(kSmallPayloadBytes, small_capacity_bytes);
  }

  // Called from ~ResidentSlot on any thread, never with the pool mutex held (see the destructor).
  void subtract(size_t bytes) {
    std::lock_guard lock(mutex);
    resident_bytes -= bytes;
    if (isSmall(bytes)) {
      small_resident_bytes -= bytes;
    }
  }
};

}  // namespace detail

ResidentSlot::ResidentSlot(
    std::shared_ptr<detail::ResidentPoolState> state, sdk::PayloadView payload, size_t charged_bytes)
    : payload_(std::move(payload)), state_(std::move(state)), charged_bytes_(charged_bytes) {}

ResidentSlot::~ResidentSlot() {
  // Every pool path take()s the payload before dropping its strong reference, so a payload still here means the
  // entry died first. The payload and its anchor are released on this thread with no pool lock held. Eviction take()s
  // under the pool lock, so the lock order is pool, then slot: take() must release the slot lock before subtract()
  // takes the pool lock, or the two deadlock.
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

ResidentPayloadPool::ResidentPayloadPool(size_t capacity_bytes)
    : state_(std::make_shared<detail::ResidentPoolState>(capacity_bytes)) {}

ResidentPayloadPool::~ResidentPayloadPool() = default;

std::shared_ptr<ResidentSlot> ResidentPayloadPool::admit(sdk::PayloadView payload) {
  const size_t bytes = payload.bytes.size();
  if (bytes == 0 || state_->capacity_bytes == 0) {
    return nullptr;
  }
  if (bytes > state_->capacity_bytes) {
    // Rejected before evicting anything: it could never fit.
    state_->rejected_oversize.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  // Plain new: make_shared cannot reach the private constructor.
  auto slot = std::shared_ptr<ResidentSlot>(new ResidentSlot(state_, std::move(payload), bytes));

  // Victims' charges are subtracted under the pool lock (the cap is hard), but their payloads are destroyed after it
  // drops: the anchors run plugin release code.
  const bool is_small = state_->isSmall(bytes);
  std::vector<sdk::PayloadView> released;
  {
    std::lock_guard lock(state_->mutex);
    // Evict within the payload's own class only, so large payloads never take the small reservation.
    auto& fifo = is_small ? state_->small_fifo : state_->fifo;
    const size_t class_capacity =
        is_small ? state_->small_capacity_bytes : (state_->capacity_bytes - state_->small_capacity_bytes);
    // small_resident_bytes is part of resident_bytes: the large class uses the difference.
    auto class_bytes = [this, is_small]() -> size_t {
      return is_small ? state_->small_resident_bytes : (state_->resident_bytes - state_->small_resident_bytes);
    };
    auto evict_front = [this, &released](std::deque<std::weak_ptr<ResidentSlot>>& victims, bool small_victims) {
      auto victim = victims.front().lock();
      victims.pop_front();
      if (victim == nullptr) {
        return;  // slot already died with its entry and retired itself
      }
      if (auto taken = victim->take()) {
        state_->resident_bytes -= victim->charged_bytes_;
        if (small_victims) {
          state_->small_resident_bytes -= victim->charged_bytes_;
        }
        state_->evicted += 1;
        released.push_back(std::move(*taken));
      }
    };
    // The total budget is hard. A large payload may borrow unused small reservation but never evicts small residents,
    // so one that cannot fit beside them is rejected before anything is evicted. Its payload leaves through
    // `released`, so the slot's destructor has no charge to retire.
    if (!is_small && state_->small_resident_bytes + bytes > state_->capacity_bytes) {
      if (auto taken = slot->take()) {
        released.push_back(std::move(*taken));
      }
      state_->rejected_oversize.fetch_add(1, std::memory_order_relaxed);
      slot.reset();
    } else {
      while (class_bytes() + bytes > class_capacity && !fifo.empty()) {
        evict_front(fifo, is_small);
      }
      // A small payload's reservation is guaranteed: reclaim space a large payload borrowed from it.
      while (is_small && state_->resident_bytes + bytes > state_->capacity_bytes && !state_->fifo.empty()) {
        evict_front(state_->fifo, /*small_victims=*/false);
      }
      state_->resident_bytes += bytes;
      if (is_small) {
        state_->small_resident_bytes += bytes;
      }
      state_->high_water_bytes = std::max(state_->high_water_bytes, state_->resident_bytes);
      state_->admitted += 1;
      fifo.push_back(slot);
    }
  }
  // `released` drops here, outside the pool mutex. A rejected admission returns nullptr.
  return slot;
}

void ResidentPayloadPool::trim() {
  std::vector<sdk::PayloadView> released;
  {
    std::lock_guard lock(state_->mutex);
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
