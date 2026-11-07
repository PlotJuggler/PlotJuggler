// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkerTopics.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace PJ {

namespace {

/// Whether `dataset` holds a marker topic with `request`'s scope and target. Stops
/// parsing metadata at the first match.
bool hasMarkerTopic(const ObjectStore& store, DatasetId dataset, const MarkerTopicInfo& request) {
  bool found = false;
  store.forEachTopic(dataset, [&](ObjectTopicId, const ObjectTopicDescriptor& descriptor) {
    if (found) {
      return;
    }
    const std::optional<MarkerTopicInfo> info = markerTopicInfo(descriptor.metadata_json);
    found = info && info->scope == request.scope && info->target == request.target;
  });
  return found;
}

}  // namespace

bool isMarkerTopic(const ObjectTopicDescriptor& descriptor) {
  return markerTopicInfo(descriptor.metadata_json).has_value();
}

std::vector<MarkerTopic> scanMarkerTopics(const ObjectStore& store, DatasetId dataset) {
  std::vector<MarkerTopic> topics;
  store.forEachTopic(dataset, [&topics](ObjectTopicId id, const ObjectTopicDescriptor& descriptor) {
    if (std::optional<MarkerTopicInfo> info = markerTopicInfo(descriptor.metadata_json)) {
      topics.push_back(MarkerTopic{id, std::move(*info)});
    }
  });
  std::ranges::stable_partition(topics, [](const MarkerTopic& topic) { return topic.info.owner.empty(); });
  return topics;
}

std::vector<ObjectTopicId> markerTopicsOf(const std::vector<MarkerTopic>& topics, const MarkerTopicInfo& request) {
  std::vector<ObjectTopicId> ids;
  for (const MarkerTopic& topic : topics) {
    if (topic.info.scope == request.scope && topic.info.target == request.target) {
      ids.push_back(topic.id);
    }
  }
  return ids;
}

std::vector<MarkerScopeKey> publishedMarkerScopes(const ObjectStore& store, std::span<const DatasetId> datasets) {
  // The all-datasets home is one dataset for every row: scanned once, not per dataset.
  const bool all_datasets_published =
      hasMarkerTopic(store, kAllDatasetsMarkerDataset, markerInfoForScope(MarkerScope::kAllDatasets));
  std::vector<MarkerScopeKey> rows;
  for (const DatasetId dataset : datasets) {
    if (hasMarkerTopic(store, dataset, markerInfoForScope(MarkerScope::kDataset))) {
      rows.emplace_back(dataset, MarkerScope::kDataset);
    }
    if (all_datasets_published) {
      rows.emplace_back(dataset, MarkerScope::kAllDatasets);
    }
  }
  return rows;
}

}  // namespace PJ
