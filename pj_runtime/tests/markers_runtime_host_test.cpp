// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// MarkersRuntimeHost bridges the pj.data_processors.v1 C ABI to MarkerService. These tests
// drive it exactly as a plugin would — through the SDK's DataProcessorsHostView over the
// host's raw fat pointer — proving the full host-driven chain: ABI create_data_processor →
// trampoline → MarkerService → engine → ObjectStore, plus per-plugin id namespacing,
// list (plugin-local ids), config round-trip, and remove.

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/MarkersRuntimeHost.h"

namespace {

using PJ::MarkerService;
using PJ::MarkersRuntimeHost;

constexpr PJ::DatasetId kDataset = 1;

MarkerService::SeriesResolver rampResolver() {
  return [](PJ::DatasetId, const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    MarkerService::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
}

TEST(MarkersRuntimeHostTest, PluginCreateRunsPublishesAndNamespaces) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });

  PJ::sdk::DataProcessorsHostView view(bridge.raw());
  ASSERT_TRUE(view.valid());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created =
      view.createMarkers("gen1", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(created->front(), PJ::sdk::markerObjectTopicName("in"));

  // Markers were published by the HOST into the ObjectStore under __markers__/in.
  EXPECT_TRUE(store.findTopic(kDataset, PJ::sdk::markerObjectTopicName("in")).has_value());

  // The service stored it under the plugin-namespaced key.
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes()[0].id, "anomaly/gen1");

  // list() returns the plugin-LOCAL id (namespace stripped).
  const PJ::Expected<std::vector<std::string>> ids = view.list();
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u);
  EXPECT_EQ((*ids)[0], "gen1");

  // config round-trips kind + inputs + outputs.
  const PJ::Expected<std::string> recipe = view.recipeOf("gen1");
  ASSERT_TRUE(recipe) << recipe.error();
  EXPECT_NE(recipe->find("\"kind\":\"markers\""), std::string::npos);
  EXPECT_NE(recipe->find("\"outputs\":[\"in\"]"), std::string::npos);

  // remove drops it.
  EXPECT_TRUE(view.remove("gen1"));
  EXPECT_TRUE(service.recipes().empty());
}

TEST(MarkersRuntimeHostTest, RemoveUnknownErrors) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  EXPECT_FALSE(view.remove("nope"));
}

// Two plugins never see each other's generators (per-plugin id isolation).
TEST(MarkersRuntimeHostTest, PerPluginIsolation) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost host_a(service, "plugA", [](const std::vector<std::string>&) { return kDataset; });
  MarkersRuntimeHost host_b(service, "plugB", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView a(host_a.raw());
  PJ::sdk::DataProcessorsHostView b(host_b.raw());

  const std::string_view inputs[] = {"in"};
  ASSERT_TRUE(a.createMarkers("g", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}"));

  const PJ::Expected<std::vector<std::string>> a_ids = a.list();
  const PJ::Expected<std::vector<std::string>> b_ids = b.list();
  ASSERT_TRUE(a_ids);
  ASSERT_TRUE(b_ids);
  EXPECT_EQ(a_ids->size(), 1u);
  EXPECT_TRUE(b_ids->empty());  // B cannot see A's generator
  EXPECT_FALSE(b.remove("g"));  // nor remove it
}

// With no active dataset (0), a create is rejected rather than orphaning output on
// the invalid dataset 0.
TEST(MarkersRuntimeHostTest, NoActiveDatasetRejected) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return PJ::DatasetId{0}; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  EXPECT_FALSE(view.createMarkers("g", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}"));
}

// params {"scope":"all"} publishes a global marker across every listed dataset.
TEST(MarkersRuntimeHostTest, ScopeAllPublishesAcrossDatasets) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  ASSERT_TRUE(view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})"));

  const std::string topic = PJ::sdk::markerObjectTopicName(PJ::sdk::kGlobalMarkerTopic);
  EXPECT_TRUE(store.findTopic(1, topic).has_value());
  EXPECT_TRUE(store.findTopic(2, topic).has_value());
}

// Preview is now create(EPHEMERAL) + remove: the host publishes ephemerally (returns
// the auto-named preview topic, readable in the store, excluded from recipes()).
TEST(MarkersRuntimeHostTest, PreviewViaEphemeralFlagPublishesAndIsEphemeral) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> topics = view.createMarkers(
      "__preview__", PJ::Span<const std::string_view>(inputs), /*output=*/"", "createMarker(0.0)\n", "{}",
      PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  ASSERT_TRUE(topics) << topics.error();
  ASSERT_EQ(topics->size(), 1u);
  EXPECT_TRUE(PJ::sdk::isPreviewMarkerTopic(topics->front()));
  EXPECT_TRUE(store.findTopic(kDataset, topics->front()).has_value());  // host published it
  EXPECT_TRUE(service.recipes().empty());                               // ephemeral, not persisted

  EXPECT_TRUE(view.remove("__preview__"));
  EXPECT_FALSE(store.findTopic(kDataset, topics->front()).has_value());
}

// The ABI contract (plugin_data_api.h's `flags` paragraph) requires the host to
// REJECT a reserved/unknown bit, not silently drop it. Bit 5 is unassigned by
// any PJ_DATA_PROCESSOR_FLAG_*.
TEST(MarkersRuntimeHostTest, ReservedFlagBitIsRejected) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "gen1", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}", /*flags=*/1u << 5);

  ASSERT_FALSE(created);
  EXPECT_NE(created.error().find("reserved"), std::string::npos) << created.error();
  EXPECT_TRUE(service.recipes().empty());
}

// The `config` slot echoes history_exempt so a plugin can PROBE whether its
// exemption request took.
TEST(MarkersRuntimeHostTest, ConfigEchoesHistoryExemptBit) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", [](const std::vector<std::string>&) { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const uint32_t history_exempt_flag = PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
  ASSERT_TRUE(view.createMarkers(
      "exempt", PJ::Span<const std::string_view>(inputs), "exempt_out", "createMarker(0.0)\n", "{}",
      history_exempt_flag));
  const PJ::Expected<std::string> exempt_recipe = view.recipeOf("exempt");
  ASSERT_TRUE(exempt_recipe) << exempt_recipe.error();
  EXPECT_NE(exempt_recipe->find("\"history_exempt\":true"), std::string::npos) << *exempt_recipe;

  ASSERT_TRUE(
      view.createMarkers("plain", PJ::Span<const std::string_view>(inputs), "plain_out", "createMarker(0.0)\n", "{}"));
  const PJ::Expected<std::string> plain_recipe = view.recipeOf("plain");
  ASSERT_TRUE(plain_recipe) << plain_recipe.error();
  EXPECT_NE(plain_recipe->find("\"history_exempt\":false"), std::string::npos) << *plain_recipe;
}

}  // namespace
