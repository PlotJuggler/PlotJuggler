#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The metadata that identifies a plot-markers object topic. A marker topic is
// recognized and routed by this metadata, never by its name: names stay opaque ids
// (docs/derived_objects/ARCHITECTURE.md §13). The key names are host constants; the
// SDK takes them later with the host contract.

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

/// `pj_derived` value of a marker set. Logic that belongs to another kind (such as
/// "on_demand") tests its own value, never the key's presence, so it never touches
/// marker topics.
inline constexpr std::string_view kDerivedMarkers = "markers";

inline constexpr std::string_view kMarkerScopeMetadataKey = "pj_marker_scope";
inline constexpr std::string_view kMarkerTargetMetadataKey = "pj_marker_target";
inline constexpr std::string_view kMarkerOwnerMetadataKey = "pj_marker_owner";

/// Which plots draw a marker set: the plots showing one series, every plot of its
/// dataset, or every plot of every dataset.
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

/// Marker key of the ALL-DATASETS scope (object topic "__markers__/__all__").
/// Vocabulary (host + UI): sdk::kGlobalMarkerTopic "__global__" = DATASET scope
/// (drawn on every plot of ONE dataset; UI row "Dataset markers");
/// kAllDatasetsMarkerTopic "__all__" = ALL-DATASETS scope (an all_datasets generator
/// publishes its set once, on the session-level home dataset, and every plot draws
/// it; UI row "Global markers" — the two rules a producer offers, per-dataset and
/// all-datasets). Producers keep sending "__global__" + {"scope":"all"};
/// BatchEngine::upsertGenerator rewrites the key host-side.
inline constexpr std::string_view kAllDatasetsMarkerTopic = "__all__";

/// Dataset-independent home of every ALL-DATASETS marker topic. An all_datasets set
/// belongs to the session, not to a dataset, so it is published ONCE here — in the
/// DISPLAY frame (raw − the contributor's alignment offset), so the overlay draws it
/// at the same display instant on every plot without any per-dataset copy or
/// rebase. The ObjectStore does not tie a topic to a live dataset, so this id never
/// collides with one (the DataEngine mints ids from 1).
inline constexpr DatasetId kAllDatasetsMarkerDataset = 0;

/// What the marker key a producer or a plot addresses means: the two reserved scope
/// keys, otherwise a series key. This is the host's vocabulary for keys (recipe
/// outputs, `MarkerTarget`, a legacy direct write at registration); a stored topic
/// is never classified by its name afterwards.
[[nodiscard]] inline MarkerTopicInfo markerInfoForKey(std::string_view marker_key) {
  if (marker_key == sdk::kGlobalMarkerTopic) {
    return MarkerTopicInfo{MarkerScopeKind::kDataset};
  }
  if (marker_key == kAllDatasetsMarkerTopic) {
    return MarkerTopicInfo{MarkerScopeKind::kAll};
  }
  return MarkerTopicInfo{MarkerScopeKind::kSeries, std::string(marker_key)};
}

/// The metadata JSON a marker topic is registered with: the plot-markers type,
/// `pj_derived`, the scope and, per field of `info`, the target and the owner.
[[nodiscard]] std::string markerTopicMetadataJson(const MarkerTopicInfo& info);

/// Reads a marker topic's metadata; `nullopt` for anything that is not a complete
/// marker description (no `pj_derived: "markers"`, an unknown scope, a series scope
/// without a target).
[[nodiscard]] std::optional<MarkerTopicInfo> markerTopicInfo(std::string_view metadata_json);

/// True when `metadata_json` is a JSON object whose `pj_derived` is the string `kind`.
/// Absent or malformed metadata is false, not an error. Cheap enough to run over every
/// topic of a dataset: most are rejected without parsing.
[[nodiscard]] bool isDerivedOfKind(std::string_view metadata_json, std::string_view kind);

/// Gives a direct write to a 0.35 marker name (`__markers__/<key>`, no owner) the
/// metadata it never carried, deriving scope and target from the key once, here
/// (`markerInfoForKey`).
/// Leaves `descriptor` alone when its name is not in the marker namespace or its
/// metadata already says what it is (any `pj_derived`, or another builtin object
/// type), or the key is not UTF-8 (it cannot be described in JSON). Existing custom
/// fields are kept. `ObjectStore` calls it on registration.
void stampLegacyMarkerMetadata(ObjectTopicDescriptor& descriptor);

}  // namespace PJ
