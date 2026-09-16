// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkerTopics.h"

#include <string>

namespace PJ {

std::vector<ObjectTopicId> markerTopicsOf(const ObjectStore& store, DatasetId dataset, std::string_view marker_topic) {
  const std::string family = marker_topic.starts_with(sdk::kMarkerObjectTopicPrefix)
                                 ? std::string(marker_topic)
                                 : sdk::markerObjectTopicName(marker_topic);
  // The separator keeps `<key>#…` from matching a longer key that merely starts
  // with `<key>`; the bare topic is an exact lookup.
  std::vector<ObjectTopicId> ids = store.listTopics(dataset, family + kMarkerOwnerSeparator);
  if (const std::optional<ObjectTopicId> bare = store.findTopic(dataset, family)) {
    ids.insert(ids.begin(), *bare);
  }
  return ids;
}

std::vector<MarkerScope> publishedMarkerScopes(const ObjectStore& store, DatasetId dataset) {
  std::vector<MarkerScope> scopes;
  for (const MarkerScope scope : {MarkerScope::kDataset, MarkerScope::kAllDatasets}) {
    if (!markerTopicsOf(store, markerScopeDataset(scope, dataset), markerScopeTopic(scope)).empty()) {
      scopes.push_back(scope);
    }
  }
  return scopes;
}

}  // namespace PJ
