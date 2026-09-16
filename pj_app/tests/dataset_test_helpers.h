// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Shared gtest fixtures for the pj_app dataset-identity + timeline tests
// (multi_dataset_rebind_test, pending_display_binder_test,
// main_window_source_layout_test, main_window_fanout_ambiguous_test,
// main_window_viewport_reframe_test): create a dataset and append a two-sample
// scalar topic through the live AppSession, then refresh the catalog. Failures
// are signaled with ADD_FAILURE (not ASSERT_*) so the helpers stay usable from
// value-returning call sites; the caller checks the returned id (0 = failure)
// where it cares. Also the readers of what a marker generator published
// (publishedMarkers / markerTimes), shared by the history and restore suites.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"

namespace pj_test {

// Creates a dataset with the given raw source_name (defaults to "test" for the
// single-dataset tests that don't care about the label). Returns 0 on failure.
// When `own_time_domain` is set, the dataset is bound to a fresh per-source
// TimeDomain (mirroring FileLoader's one-domain-per-source) instead of the
// default (id 0) domain — required by any test that writes a per-source display
// offset (Source Timeline / "align starts"), since that shift lives on the
// domain and the default domain carries none.
inline PJ::DatasetId createDataset(
    PJ::AppSession& app_session, std::string_view source_name = "test", bool own_time_domain = false) {
  PJ::DatasetDescriptor descriptor{.source_name = std::string(source_name)};
  if (own_time_domain) {
    auto domain = app_session.sessionManager().dataEngine().createTimeDomain(std::string(source_name));
    if (!domain.has_value()) {
      ADD_FAILURE() << domain.error();
      return 0;
    }
    descriptor.time_domain_id = *domain;
  }
  auto dataset = app_session.sessionManager().dataEngine().createDataset(descriptor);
  if (!dataset.has_value()) {
    ADD_FAILURE() << dataset.error();
    return 0;
  }
  return *dataset;
}

// Appends a two-sample float64 scalar series named `topic_name` to `dataset_id`
// at the given timestamps, commits it, and rebuilds the catalog. Returns the new
// topic's id (0 on failure).
inline PJ::TopicId addScalarTopic(
    PJ::AppSession& app_session, PJ::DatasetId dataset_id, std::string_view topic_name, PJ::Timestamp first_ts,
    PJ::Timestamp second_ts) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  if (!handle.has_value()) {
    ADD_FAILURE() << handle.error();
    return 0;
  }
  writer.appendScalar(*handle, first_ts, 1.0);
  writer.appendScalar(*handle, second_ts, 2.0);
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return 0;
  }
  app_session.catalogModel().rebuildFromDatastore();
  return handle->topic_id;
}

// Convenience overload at the default two-sample timestamps (100, 200 ns).
inline PJ::TopicId addScalarTopic(PJ::AppSession& app_session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  return addScalarTopic(app_session, dataset_id, topic_name, /*first_ts=*/100, /*second_ts=*/200);
}

// The set marker generator `owner` last published for `output` on `dataset`
// (nullopt when it never did). An all-datasets rule publishes on
// PJ::kAllDatasetsMarkerDataset under PJ::kAllDatasetsMarkerTopic, with t_start
// already in the display frame (raw − the contributing dataset's offset).
inline std::optional<PJ::sdk::PlotMarkers> publishedMarkers(
    PJ::AppSession& app_session, PJ::DatasetId dataset, std::string_view output, std::string_view owner) {
  const PJ::ObjectStore& store = app_session.sessionManager().objectStore();
  const std::optional<PJ::ObjectTopicId> id = store.findTopic(dataset, PJ::markerOwnerTopicName(output, owner));
  if (!id.has_value()) {
    return std::nullopt;
  }
  const std::optional<PJ::ResolvedObjectEntry> entry = store.latestAt(*id, std::numeric_limits<PJ::Timestamp>::max());
  if (!entry.has_value()) {
    return std::nullopt;
  }
  const PJ::Expected<PJ::sdk::PlotMarkers> decoded =
      PJ::deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
  return decoded.has_value() ? std::optional<PJ::sdk::PlotMarkers>{*decoded} : std::nullopt;
}

// Sorted t_start of publishedMarkers(...); empty when nothing was published.
inline std::vector<PJ::Timestamp> markerTimes(
    PJ::AppSession& app_session, PJ::DatasetId dataset, std::string_view output, std::string_view owner) {
  std::vector<PJ::Timestamp> times;
  if (const auto published = publishedMarkers(app_session, dataset, output, owner)) {
    for (const PJ::sdk::PlotMarker& marker : published->markers) {
      times.push_back(marker.t_start);
    }
  }
  std::sort(times.begin(), times.end());
  return times;
}

}  // namespace pj_test
