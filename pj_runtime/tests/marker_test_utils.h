// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Readers for the per-owner marker publish model. A marker topic is a FAMILY of
// object topics on one dataset — the bare `__markers__/<key>` (a direct write, no
// owner) plus one `__markers__/<key>#<owner>` per generator — and what a plot draws
// is their union. These helpers read the store the way the overlay does, so a test
// asserts on the drawn result rather than on which topic carried it.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerTopics.h"

namespace PJ::test {

/// Every object topic of `marker_topic`'s family on `dataset`, in store order.
/// Accepts the bare key or its `markerObjectTopicName` form.
inline std::vector<ObjectTopicId> markerFamily(
    const ObjectStore& store, DatasetId dataset, std::string_view marker_topic) {
  const std::string family = marker_topic.starts_with(sdk::kMarkerObjectTopicPrefix)
                                 ? std::string(marker_topic)
                                 : sdk::markerObjectTopicName(marker_topic);
  std::vector<ObjectTopicId> ids;
  for (const ObjectTopicId id : store.listTopics(dataset)) {
    const std::string name = store.descriptor(id).topic_name;
    if (name == family || (name.starts_with(family) && name[family.size()] == kMarkerOwnerSeparator)) {
      ids.push_back(id);
    }
  }
  return ids;
}

/// The currently published entry of one object topic (nullopt when never published).
inline std::optional<ResolvedObjectEntry> latestEntry(const ObjectStore& store, ObjectTopicId id) {
  return store.latestAt(id, std::numeric_limits<Timestamp>::max());
}

/// The union a plot draws for `marker_topic` on `dataset`: every family member's
/// decoded set, concatenated. A tombstone (empty blob) contributes nothing; an
/// absent family reads as an empty set.
inline sdk::PlotMarkers readMarkerUnion(const ObjectStore& store, DatasetId dataset, std::string_view marker_topic) {
  sdk::PlotMarkers set;
  for (const ObjectTopicId id : markerFamily(store, dataset, marker_topic)) {
    const std::optional<ResolvedObjectEntry> entry = latestEntry(store, id);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      continue;
    }
    const Expected<sdk::PlotMarkers> decoded =
        deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
    EXPECT_TRUE(decoded.has_value()) << "undecodable marker blob";
    if (decoded.has_value()) {
      set.markers.insert(set.markers.end(), decoded->markers.begin(), decoded->markers.end());
    }
  }
  return set;
}

/// The publish uid of `owner`'s own topic for `marker_topic` on `dataset` — a
/// republish mints a new one, an untouched topic keeps its own. Nullopt when the
/// owner never published there.
inline std::optional<std::uint64_t> ownerPublishUid(
    const ObjectStore& store, DatasetId dataset, std::string_view marker_topic, std::string_view owner) {
  const std::optional<ObjectTopicId> id = store.findTopic(dataset, markerOwnerTopicName(marker_topic, owner));
  const std::optional<ResolvedObjectEntry> entry = id.has_value() ? latestEntry(store, *id) : std::nullopt;
  return entry.has_value() ? std::optional<std::uint64_t>{entry->sequential_uid.value} : std::nullopt;
}

/// True when `owner`'s topic still exists on `dataset` but holds the empty blob a
/// retired persistent generator leaves behind.
inline bool ownerTombstoned(
    const ObjectStore& store, DatasetId dataset, std::string_view marker_topic, std::string_view owner) {
  const std::optional<ObjectTopicId> id = store.findTopic(dataset, markerOwnerTopicName(marker_topic, owner));
  const std::optional<ResolvedObjectEntry> entry = id.has_value() ? latestEntry(store, *id) : std::nullopt;
  return entry.has_value() && entry->payload.bytes.empty();
}

}  // namespace PJ::test
