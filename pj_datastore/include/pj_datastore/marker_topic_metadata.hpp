#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The metadata that identifies a plot-markers object topic. A marker topic is recognized and routed by this metadata,
// not by its name.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"       // sdk::kGlobalMarkerTopic
#include "pj_base/sdk/object_topic_metadata.hpp"  // sdk::kDerivedMetadataKey
#include "pj_base/types.hpp"                      // DatasetId
#include "pj_datastore/object_store.hpp"

namespace PJ {

/// Metadata key meaning "produced by a host data processor"; the value names the kind.
using sdk::kDerivedMetadataKey;

/// `pj_derived` value of a marker set. Other kinds (such as "on_demand") test their own value, not the key's presence,
/// so they never match marker topics.
inline constexpr std::string_view kDerivedMarkers = "markers";

inline constexpr std::string_view kMarkerScopeMetadataKey = "pj_marker_scope";
inline constexpr std::string_view kMarkerTargetMetadataKey = "pj_marker_target";
inline constexpr std::string_view kMarkerOwnerMetadataKey = "pj_marker_owner";

/// Which plots draw a marker set: those showing one series, every plot of its dataset, or every plot of every dataset.
enum class MarkerScopeKind { kSeries, kDataset, kAll };

/// What a marker topic's metadata says about it.
struct MarkerTopicInfo {
  MarkerScopeKind scope;
  /// The series key; empty unless `scope` is `kSeries`.
  std::string target;
  /// The generator that publishes the set; empty for a direct write by a plugin.
  std::string owner;

  MarkerTopicInfo(
      MarkerScopeKind scope_kind = MarkerScopeKind::kSeries, std::string series_target = {},
      std::string generator_id = {})
      : scope(scope_kind), target(std::move(series_target)), owner(std::move(generator_id)) {}

  bool operator==(const MarkerTopicInfo&) const = default;
};

/// Marker key of the all-datasets scope (object topic "__markers__/__all__"). sdk::kGlobalMarkerTopic ("__global__")
/// is the key of the dataset scope instead (the UI row "Dataset markers"; this scope is the row "Global markers").
/// Producers always send "__global__"; BatchEngine::upsertGenerator rewrites it to this key for an all_datasets
/// generator.
inline constexpr std::string_view kAllDatasetsMarkerTopic = "__all__";

/// Home dataset of every all-datasets marker topic. Such a set belongs to the session, so it is published once here,
/// in the display frame (raw minus the contributor's alignment offset): every plot draws it at the same display
/// instant with no per-dataset copy. Never a real dataset: DataEngine ids start at 1 and ObjectStore does not require
/// the dataset to exist.
inline constexpr DatasetId kAllDatasetsMarkerDataset = 0;

/// The two reserved scope keys, otherwise a series key. A stored topic is never classified by its name afterwards.
[[nodiscard]] inline MarkerTopicInfo markerInfoForKey(std::string_view marker_key) {
  if (marker_key == sdk::kGlobalMarkerTopic) {
    return MarkerTopicInfo{MarkerScopeKind::kDataset};
  }
  if (marker_key == kAllDatasetsMarkerTopic) {
    return MarkerTopicInfo{MarkerScopeKind::kAll};
  }
  return MarkerTopicInfo{MarkerScopeKind::kSeries, std::string(marker_key)};
}

/// The metadata a marker topic is registered with: plot-markers type, `pj_derived`, scope, the target (series scope
/// only) and the owner (when set). Empty when the metadata builder rejects a field (e.g. non-UTF-8 text).
[[nodiscard]] std::string markerTopicMetadataJson(const MarkerTopicInfo& info);

/// `nullopt` for anything that is not a complete marker description (no `pj_derived: "markers"`, an unknown scope, a
/// series scope without a target).
[[nodiscard]] std::optional<MarkerTopicInfo> markerTopicInfo(std::string_view metadata_json);

/// True when `metadata_json` is a JSON object whose `pj_derived` is the string `kind`. Absent or malformed metadata is
/// false. Topics without the key are rejected before parsing, so it is cheap to run over every topic.
[[nodiscard]] bool isDerivedOfKind(std::string_view metadata_json, std::string_view kind);

/// Stamps marker metadata (scope and target from markerInfoForKey()) on a topic named `__markers__/<key>` that has
/// none: a direct write by a plugin, with no owner. Empty or non-object metadata is replaced; other fields are kept.
/// Leaves `descriptor` alone when its name is outside the marker namespace, its metadata already says what it is (any
/// `pj_derived`, or another builtin object type), or the key is not UTF-8. `ObjectStore` calls it on registration.
void stampLegacyMarkerMetadata(ObjectTopicDescriptor& descriptor);

}  // namespace PJ
