// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::kMarkerObjectTopicPrefix
#include "pj_base/types.hpp"                 // DatasetId
#include "pj_datastore/object_store.hpp"     // ObjectStore, ObjectTopicId

namespace PJ {

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
/// publishes its set once, on `kAllDatasetsMarkerDataset`, and every plot draws it;
/// UI row "Global markers" — the two rules a producer offers, per-dataset and
/// all-datasets). Producers keep sending "__global__" + {"scope":"all"};
/// MarkerService::upsertGenerator rewrites the key host-side.
inline constexpr std::string_view kAllDatasetsMarkerTopic = "__all__";

/// Dataset-independent home of every ALL-DATASETS marker topic. An all_datasets set
/// belongs to the session, not to a dataset, so it is published ONCE here — in the
/// DISPLAY frame (raw − the contributor's alignment offset), so the overlay draws it
/// at the same display instant on every plot without any per-dataset copy or
/// rebase. The ObjectStore does not tie a topic to a live dataset, so this id never
/// collides with one (the DataEngine mints ids from 1).
inline constexpr DatasetId kAllDatasetsMarkerDataset = 0;

/// Separates a marker topic from the generator that owns the set:
/// `__markers__/<key>#<owner>`. A marker topic is a FAMILY of object topics — the
/// bare name (a direct write, no owner) plus one per generator — and a plot draws
/// their union, so two generators on one key never clobber each other and retiring
/// one touches only its own topic. Reserved: `MarkerService::upsertGenerator`
/// refuses an id or key containing it, since `a` + `r#s` and `a#r` + `s` would
/// spell one topic.
inline constexpr char kMarkerOwnerSeparator = '#';

/// Object-topic name of `owner`'s set for `marker_topic`.
[[nodiscard]] inline std::string markerOwnerTopicName(std::string_view marker_topic, std::string_view owner) {
  std::string name = sdk::markerObjectTopicName(marker_topic);
  name.push_back(kMarkerOwnerSeparator);
  name.append(owner);
  return name;
}

/// Which marker set a plot's overlay draws for one dataset (see the vocabulary above).
/// Both scopes can coexist on the same dataset and are hidden/shown independently.
enum class MarkerScope { kDataset, kAllDatasets };

/// The dataset `scope`'s family lives on when drawn for `dataset`: the dataset itself
/// for the DATASET scope, `kAllDatasetsMarkerDataset` for the ALL-DATASETS one.
[[nodiscard]] inline constexpr DatasetId markerScopeDataset(MarkerScope scope, DatasetId dataset) {
  return scope == MarkerScope::kAllDatasets ? kAllDatasetsMarkerDataset : dataset;
}

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

/// Every object topic of `marker_topic`'s family on `dataset`: the bare
/// `__markers__/<key>` first (when a direct write registered it), then each
/// `__markers__/<key>#<owner>` in registration order. The overlay draws their
/// union; a caller asking about the ALL-DATASETS scope passes
/// `kAllDatasetsMarkerDataset`. Accepts the bare key or its object-topic form.
[[nodiscard]] std::vector<ObjectTopicId> markerTopicsOf(
    const ObjectStore& store, DatasetId dataset, std::string_view marker_topic);

/// The scopes with at least one family member published for `dataset`, in
/// `MarkerScope` order — the rows a plot's curve editor lists. Answered from the
/// ObjectStore, not from any generator bookkeeping: a direct toolbox write and a
/// retired generator's tombstone are rows too.
[[nodiscard]] std::vector<MarkerScope> publishedMarkerScopes(const ObjectStore& store, DatasetId dataset);

}  // namespace PJ
