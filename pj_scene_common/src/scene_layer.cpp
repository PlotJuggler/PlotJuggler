// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/scene_layer.h"

namespace PJ {

PJ::Range<PJ::Timepoint> liveTopicTimeRange(const ObjectStore* store, ObjectTopicId topic_id) {
  if (store == nullptr || store->entryCount(topic_id) == 0) {
    return {PJ::Timepoint::max(), PJ::Timepoint::min()};  // inverted: empty
  }
  const auto [lo, hi] = store->timeRange(topic_id);
  return {PJ::fromRaw(lo), PJ::fromRaw(hi)};
}

uint64_t latestEntryRenderKey(const ObjectStore* store, ObjectTopicId topic_id, PJ::Timepoint time) {
  const auto entry = store != nullptr ? store->latestEntryIdAt(topic_id, PJ::toRaw(time)) : std::nullopt;
  return entry.has_value() ? entry->uid.value : kNoSampleRenderKey;
}

ISceneLayer::ISceneLayer(QObject* parent) : QObject(parent) {}

ISceneLayer::~ISceneLayer() = default;

void ISceneLayer::setFixedFrame(const QString& /*frame*/) {}

QDomElement ISceneLayer::xmlSaveState(QDomDocument& /*doc*/) const {
  return {};
}

bool ISceneLayer::xmlLoadState(const QDomElement& /*element*/) {
  return true;
}

}  // namespace PJ
