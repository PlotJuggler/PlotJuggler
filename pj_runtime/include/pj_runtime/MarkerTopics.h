// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"        // sdk::kGlobalMarkerTopic, markerObjectTopicName
#include "pj_base/types.hpp"                       // DatasetId
#include "pj_datastore/marker_topic_metadata.hpp"  // MarkerTopicInfo
#include "pj_datastore/object_store.hpp"           // ObjectStore, ObjectTopicId

namespace PJ {

/// True if `descriptor` describes a marker set (any marker topic, preview or not):
/// its metadata says so (`pj_derived: "markers"`), whatever its name. The single
/// host-side predicate every consumer of ObjectStore topics uses to treat marker
/// snapshots as reserved internal state — excluded from dataset time-bounds, the
/// curve tree, playback focus, refill pruning, and the generic dataset-merge fold
/// (markers are merged set-aware by the batch engine, not folded as opaque object
/// entries, because they publish at a sentinel timestamp with single-entry
/// retention). Cheap enough to run over every object topic of a dataset.
[[nodiscard]] bool isMarkerTopic(const ObjectTopicDescriptor& descriptor);

/// Separates a marker key from the generator that owns the set in the object-topic
/// NAME the host mints for it: `__markers__/<key>#<owner>`. The name is an opaque
/// id: what a topic is (scope, target, owner) is in its metadata. A marker topic is
/// a FAMILY of object topics — a direct write (no owner) plus one per generator —
/// and a plot draws their union, so two generators on one key never clobber each
/// other and retiring one touches only its own topic. Reserved:
/// `BatchEngine::upsertGenerator` refuses an id or key containing it, since `a` +
/// `r#s` and `a#r` + `s` would mint one name.
inline constexpr char kMarkerOwnerSeparator = '#';

/// Object-topic name the host mints for `owner`'s set of `marker_topic`.
[[nodiscard]] inline std::string markerOwnerTopicName(std::string_view marker_topic, std::string_view owner) {
  std::string name = sdk::markerObjectTopicName(marker_topic);
  name.push_back(kMarkerOwnerSeparator);
  name.append(owner);
  return name;
}

/// Which marker set a plot's overlay draws for one dataset (see the vocabulary above).
/// Both scopes can coexist on the same dataset and are hidden/shown independently.
enum class MarkerScope { kDataset, kAllDatasets };

/// One dataset's marker scope — the curve editor's scope-row unit and the plot's
/// hidden-flag key.
using MarkerScopeKey = std::pair<DatasetId, MarkerScope>;

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

/// What a plot looks up for `scope`: its kind, no target (`markerInfoForKey` of
/// `markerScopeTopic(scope)` without the string round trip).
[[nodiscard]] inline MarkerTopicInfo markerInfoForScope(MarkerScope scope) {
  return MarkerTopicInfo{scope == MarkerScope::kAllDatasets ? MarkerScopeKind::kAll : MarkerScopeKind::kDataset};
}

/// A marker topic of a dataset and what its metadata says about it.
struct MarkerTopic {
  ObjectTopicId id;
  MarkerTopicInfo info;
};

/// Every marker topic on `dataset`: the direct writes first, then the generators'
/// topics, each group in registration order. One pass over the dataset's
/// descriptors, parsing the metadata of the derived ones only — a cost per call, so
/// a caller resolving several requests against one dataset scans it once and
/// matches in memory (`markerTopicsOf`). The ALL-DATASETS scope lives on
/// `kAllDatasetsMarkerDataset`.
[[nodiscard]] std::vector<MarkerTopic> scanMarkerTopics(const ObjectStore& store, DatasetId dataset);

/// The members of `topics` whose scope and target equal `request`'s (its owner is
/// ignored), in `topics` order: the direct write first (when one exists), then each
/// generator's topic. The overlay draws their union.
[[nodiscard]] std::vector<ObjectTopicId> markerTopicsOf(
    const std::vector<MarkerTopic>& topics, const MarkerTopicInfo& request);

/// The (dataset, scope) rows with at least one family member published, per dataset
/// of `datasets` in order and per dataset in `MarkerScope` order — the rows a plot's
/// curve editor lists. Answered from the ObjectStore, not from any generator
/// bookkeeping: a direct toolbox write and a retired generator's tombstone are rows
/// too.
[[nodiscard]] std::vector<MarkerScopeKey> publishedMarkerScopes(
    const ObjectStore& store, std::span<const DatasetId> datasets);

}  // namespace PJ
