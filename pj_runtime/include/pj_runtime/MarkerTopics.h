// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::kMarkerObjectTopicPrefix
#include "pj_base/types.hpp"                 // DatasetId

namespace PJ {

class ObjectStore;

/// True if `object_topic` names a marker set (any marker topic, preview or not).
/// The single host-side predicate every consumer of ObjectStore topics uses to
/// treat marker snapshots as reserved internal state — excluded from dataset
/// time-bounds, the curve tree, playback focus, refill pruning, and the generic
/// dataset-merge fold (markers are merged set-aware by the marker service, not
/// folded as opaque object entries, because they publish at a sentinel timestamp
/// with single-entry retention). Centralizes what was an inline
/// starts_with(sdk::kMarkerObjectTopicPrefix) scattered across those consumers.
[[nodiscard]] inline bool isMarkerObjectTopic(std::string_view object_topic) {
  return object_topic.starts_with(sdk::kMarkerObjectTopicPrefix);
}

/// Marker-topic key of the ALL-DATASETS scope (object topic "__markers__/__all__").
/// Vocabulary (host + UI): sdk::kGlobalMarkerTopic "__global__" = DATASET scope
/// (drawn on every plot of ONE dataset; UI row "Dataset markers");
/// kAllDatasetsMarkerTopic "__all__" = ALL-DATASETS scope (an all_datasets generator
/// publishes one such blob per loaded dataset; UI row "Global markers" — the two
/// rules a producer offers, per-dataset and all-datasets). Producers keep sending
/// "__global__" + {"scope":"all"}; MarkerService::upsertGenerator rewrites the key
/// host-side.
inline constexpr std::string_view kAllDatasetsMarkerTopic = "__all__";

/// Which marker set a plot's overlay draws for one dataset (see the vocabulary above).
/// Both scopes can coexist on the same dataset and are hidden/shown independently.
enum class MarkerScope { kDataset, kAllDatasets };

/// The marker-topic key backing `scope` — the ObjectStore object topic is
/// sdk::markerObjectTopicName(markerScopeTopic(scope)).
[[nodiscard]] inline constexpr std::string_view markerScopeTopic(MarkerScope scope) {
  switch (scope) {
    case MarkerScope::kDataset:
      return sdk::kGlobalMarkerTopic;
    case MarkerScope::kAllDatasets:
      return kAllDatasetsMarkerTopic;
  }
  return {};
}

/// The scopes whose marker object topic exists on `dataset`, in `MarkerScope`
/// order — the rows a plot's curve editor lists. Answered from the ObjectStore,
/// not from any generator bookkeeping: a direct toolbox write and a retired
/// generator's tombstone are rows too.
[[nodiscard]] std::vector<MarkerScope> publishedMarkerScopes(const ObjectStore& store, DatasetId dataset);

}  // namespace PJ
