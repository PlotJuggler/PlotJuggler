// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/MarkerTopics.h"

#include "pj_datastore/object_store.hpp"

namespace PJ {

std::vector<MarkerScope> publishedMarkerScopes(const ObjectStore& store, DatasetId dataset) {
  std::vector<MarkerScope> scopes;
  for (const MarkerScope scope : {MarkerScope::kDataset, MarkerScope::kAllDatasets}) {
    if (store.findTopic(dataset, sdk::markerObjectTopicName(markerScopeTopic(scope)))) {
      scopes.push_back(scope);
    }
  }
  return scopes;
}

}  // namespace PJ
