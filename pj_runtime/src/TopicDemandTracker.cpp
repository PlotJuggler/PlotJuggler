// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/TopicDemandTracker.h"

namespace PJ {

TopicDemandTracker::TopicDemandTracker(QObject* parent) : QObject(parent) {}

std::vector<QString> TopicDemandTracker::computeActive(const DatasetState& state) {
  std::set<QString> active(state.infra);
  active.insert(state.derived_inputs.begin(), state.derived_inputs.end());
  active.insert(state.forced.begin(), state.forced.end());
  for (const auto& [name, count] : state.ref_counts) {
    if (count > 0) {
      active.insert(name);
    }
  }
  return {active.begin(), active.end()};  // std::set -> already sorted & unique
}

void TopicDemandTracker::recomputeAndEmit(DatasetId dataset_id, DatasetState& state) {
  std::vector<QString> active = computeActive(state);
  if (active == state.last_emitted) {
    return;
  }
  state.last_emitted = active;
  deliver(dataset_id, std::move(active));
}

void TopicDemandTracker::deliver(DatasetId dataset_id, std::vector<QString> active) {
  pending_emits_[dataset_id] = std::move(active);
  if (emitting_) {
    return;  // the outer deliver() drains the queue after its emit returns
  }
  emitting_ = true;
  while (!pending_emits_.empty()) {
    const auto it = pending_emits_.begin();
    const DatasetId ds = it->first;
    const std::vector<QString> set = std::move(it->second);
    pending_emits_.erase(it);
    emit activeTopicsChanged(ds, set);
  }
  emitting_ = false;
}

bool TopicDemandTracker::hasAnyReference(DatasetId dataset_id) const {
  const auto it = datasets_.find(dataset_id);
  return it != datasets_.end() && !it->second.ref_counts.empty();  // a count reaching 0 is erased
}

void TopicDemandTracker::addReference(DatasetId dataset_id, const QString& topic_name) {
  DatasetState& state = datasets_[dataset_id];
  const bool was_referenced = !state.ref_counts.empty();
  ++state.ref_counts[topic_name];
  recomputeAndEmit(dataset_id, state);
  if (!was_referenced) {
    emit referenceDemandChanged(dataset_id);
  }
}

void TopicDemandTracker::removeReference(DatasetId dataset_id, const QString& topic_name) {
  const auto ds_it = datasets_.find(dataset_id);
  if (ds_it == datasets_.end()) {
    return;
  }
  DatasetState& state = ds_it->second;
  const auto rc_it = state.ref_counts.find(topic_name);
  if (rc_it == state.ref_counts.end() || rc_it->second <= 0) {
    return;  // no outstanding reference — defensive against unbalanced calls
  }
  if (--rc_it->second == 0) {
    state.ref_counts.erase(rc_it);
  }
  const bool referenced = !state.ref_counts.empty();
  recomputeAndEmit(dataset_id, state);
  if (!referenced) {
    emit referenceDemandChanged(dataset_id);
  }
}

void TopicDemandTracker::setInfrastructureTopics(DatasetId dataset_id, const std::vector<QString>& topic_names) {
  replaceTier(dataset_id, &DatasetState::infra, topic_names);
}

void TopicDemandTracker::setDerivedInputTopics(DatasetId dataset_id, const std::vector<QString>& topic_names) {
  replaceTier(dataset_id, &DatasetState::derived_inputs, topic_names);
}

void TopicDemandTracker::replaceTier(
    DatasetId dataset_id, std::set<QString> DatasetState::* tier, const std::vector<QString>& names) {
  if (names.empty() && datasets_.find(dataset_id) == datasets_.end()) {
    return;  // avoid materializing phantom state for a dataset we know nothing about
  }
  DatasetState& state = datasets_[dataset_id];
  std::set<QString> next(names.begin(), names.end());
  if (next == state.*tier) {
    return;
  }
  state.*tier = std::move(next);
  recomputeAndEmit(dataset_id, state);
}

void TopicDemandTracker::setTopicForced(DatasetId dataset_id, const QString& topic_name, bool forced) {
  if (!forced && datasets_.find(dataset_id) == datasets_.end()) {
    return;  // unforcing a dataset we know nothing about — nothing to do
  }
  DatasetState& state = datasets_[dataset_id];
  const bool changed = forced ? state.forced.insert(topic_name).second : state.forced.erase(topic_name) > 0;
  if (changed) {
    emit forcedTopicsChanged(dataset_id);
    recomputeAndEmit(dataset_id, state);
  }
}

bool TopicDemandTracker::isTopicForced(DatasetId dataset_id, const QString& topic_name) const {
  const auto it = datasets_.find(dataset_id);
  return it != datasets_.end() && it->second.forced.count(topic_name) > 0;
}

std::vector<QString> TopicDemandTracker::forcedTopics(DatasetId dataset_id) const {
  const auto it = datasets_.find(dataset_id);
  if (it == datasets_.end()) {
    return {};
  }
  return {it->second.forced.begin(), it->second.forced.end()};
}

void TopicDemandTracker::clearDataset(DatasetId dataset_id) {
  const auto it = datasets_.find(dataset_id);
  if (it == datasets_.end()) {
    return;
  }
  const bool had_active = !it->second.last_emitted.empty();
  const bool had_forced = !it->second.forced.empty();
  const bool had_references = !it->second.ref_counts.empty();
  datasets_.erase(it);
  if (had_forced) {
    emit forcedTopicsChanged(dataset_id);
  }
  if (had_references) {
    emit referenceDemandChanged(dataset_id);
  }
  if (had_active) {
    deliver(dataset_id, {});
  }
}

std::vector<QString> TopicDemandTracker::activeTopics(DatasetId dataset_id) const {
  const auto it = datasets_.find(dataset_id);
  if (it == datasets_.end()) {
    return {};
  }
  return computeActive(it->second);
}

}  // namespace PJ
