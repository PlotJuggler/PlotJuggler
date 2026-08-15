// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ObjectIngestTap.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace PJ {

struct ObjectIngestTapRegistry::State {
  std::mutex mutex;
  std::unordered_map<sdk::BuiltinObjectType, std::shared_ptr<const ObjectIngestTapFn>> taps;
};

ObjectIngestTapRegistry::TapLease::TapLease(std::shared_ptr<State> state, sdk::BuiltinObjectType kind)
    : state_(std::move(state)), kind_(kind) {}

ObjectIngestTapRegistry::TapLease::~TapLease() {
  release();
}

ObjectIngestTapRegistry::TapLease::TapLease(TapLease&& other) noexcept
    : state_(std::move(other.state_)), kind_(std::exchange(other.kind_, sdk::BuiltinObjectType::kNone)) {}

ObjectIngestTapRegistry::TapLease& ObjectIngestTapRegistry::TapLease::operator=(TapLease&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    kind_ = std::exchange(other.kind_, sdk::BuiltinObjectType::kNone);
  }
  return *this;
}

void ObjectIngestTapRegistry::TapLease::release() {
  if (state_ == nullptr) {
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->taps.erase(kind_);
  }
  state_.reset();
  kind_ = sdk::BuiltinObjectType::kNone;
}

ObjectIngestTapRegistry::ObjectIngestTapRegistry() : state_(std::make_shared<State>()) {}

ObjectIngestTapRegistry::~ObjectIngestTapRegistry() = default;

Expected<ObjectIngestTapRegistry::TapLease> ObjectIngestTapRegistry::claimExclusive(
    sdk::BuiltinObjectType kind, ObjectIngestTapFn fn) {
  if (!fn) {
    return unexpected("Object ingest tap callback is empty");
  }
  std::lock_guard lock(state_->mutex);
  if (state_->taps.contains(kind)) {
    return unexpected(std::string("Object ingest tap already claimed for ") + std::string(sdk::name(kind)));
  }
  state_->taps.emplace(kind, std::make_shared<const ObjectIngestTapFn>(std::move(fn)));
  return TapLease{state_, kind};
}

bool ObjectIngestTapRegistry::claimed(sdk::BuiltinObjectType kind) const {
  std::lock_guard lock(state_->mutex);
  return state_->taps.contains(kind);
}

bool ObjectIngestTapRegistry::invoke(
    sdk::BuiltinObjectType kind, ObjectTopicId topic_id, DatasetId dataset_id, Timestamp store_ts,
    sdk::PayloadView bytes) const {
  std::shared_ptr<const ObjectIngestTapFn> tap;
  {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->taps.find(kind);
    if (it == state_->taps.end()) {
      return false;
    }
    tap = it->second;
  }
  (*tap)(topic_id, dataset_id, store_ts, std::move(bytes));
  return true;
}

}  // namespace PJ
