// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// MarkersRuntimeHost bridges the pj.data_processors.v1 C ABI to BatchEngine. These tests
// drive it exactly as a plugin would — through the SDK's DataProcessorsHostView over the
// host's raw fat pointer — proving the full host-driven chain: ABI create_data_processor →
// trampoline → BatchEngine → engine → ObjectStore, plus per-plugin id namespacing,
// list (plugin-local ids), config round-trip, and remove.

#include <gtest/gtest.h>

#include <QThread>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "batch_engine_test_utils.h"
#include "marker_test_utils.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/DatasetQualifiedName.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/MarkersRuntimeHost.h"

namespace {

using PJ::BatchEngine;
using PJ::kAllDatasetsMarkerDataset;
using PJ::MarkersRuntimeHost;
using PJ::test::markerFamily;
using PJ::test::readMarkerUnion;

constexpr PJ::DatasetId kDataset = 1;

BatchEngine::SeriesResolver rampResolver() {
  return [](PJ::DatasetId, const std::string& key,
            const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
}

// A plottable catalog row for the bare marker key `key` in dataset `ds`.
PJ::CatalogItem catalogRow(PJ::DatasetId ds, const char* key) {
  PJ::CatalogItem item;
  item.dataset_id = ds;
  item.topic_name = QString::fromUtf8(key);
  PJ::ScalarFieldPayload payload;
  payload.logical_type = PJ::PrimitiveType::kFloat64;
  item.payload = payload;
  return item;
}

std::optional<std::string> sourceNameOf(PJ::DatasetId id) {
  return id == 1 ? "a" : "b";
}

// The dataset callback exactly as MainWindow wires it: the real resolver over a
// catalog holding bare key "in" in BOTH datasets 1 ("a") and 2 ("b").
std::function<PJ::Expected<PJ::DatasetId>(std::vector<std::string>&, std::vector<std::string>&)>
realResolverOverTwoDatasets() {
  return [](std::vector<std::string>& inputs, std::vector<std::string>& outputs) -> PJ::Expected<PJ::DatasetId> {
    const std::vector<PJ::CatalogItem> items = {catalogRow(1, "in"), catalogRow(2, "in")};
    return PJ::resolveMarkerDataset(inputs, outputs, items, {1, 2}, sourceNameOf);
  };
}

// The production wiring over a catalog the test can grow after a create.
using SharedCatalog = std::shared_ptr<std::vector<PJ::CatalogItem>>;
std::function<PJ::Expected<PJ::DatasetId>(std::vector<std::string>&, std::vector<std::string>&)> realResolverOver(
    SharedCatalog items) {
  return [items](std::vector<std::string>& inputs, std::vector<std::string>& outputs) -> PJ::Expected<PJ::DatasetId> {
    return PJ::resolveMarkerDataset(inputs, outputs, *items, {1, 2}, sourceNameOf);
  };
}

TEST(MarkersRuntimeHostTest, PluginCreateRunsPublishesAndNamespaces) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });

  PJ::sdk::DataProcessorsHostView view(bridge.raw());
  ASSERT_TRUE(view.valid());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created =
      view.createMarkers("gen1", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(created->front(), PJ::markerOwnerTopicName("in", "anomaly/gen1")) << "the physical topic, readable back";

  // Markers were published by the HOST into the ObjectStore, in the __markers__/in family.
  EXPECT_EQ(markerFamily(store, kDataset, "in").size(), 1u);

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

// The three-way check over markers always decides: a rule that compiles is VALID, one that does not is INVALID with
// the compile error, never UNCHECKED. A kind or language markers do not run is a call that could not be made: false,
// the status untouched, no compiler run.
TEST(MarkersRuntimeHostTest, CheckScriptAnswersValidOrInvalid) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  ASSERT_TRUE(PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, bridge.raw().vtable, check_data_processor_script));
  const PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const auto valid = view.checkScript("markers", "luau", "createMarker(0.0)\n");
  ASSERT_TRUE(valid) << valid.error();
  EXPECT_EQ(valid->status, PJ::sdk::ScriptCheckStatus::kValid);
  const auto invalid = view.checkScript("markers", "luau", "createMarker(\n");
  ASSERT_TRUE(invalid) << invalid.error();
  EXPECT_EQ(invalid->status, PJ::sdk::ScriptCheckStatus::kInvalid);
  EXPECT_FALSE(invalid->message.empty());

  const PJ_data_processors_host_t raw = bridge.raw();
  const auto refused = [&raw](std::string_view kind, std::string_view language, std::string_view expected) {
    uint32_t status = 77;
    PJ_error_t err{};
    EXPECT_FALSE(raw.vtable->check_data_processor_script(
        raw.ctx, PJ::sdk::toAbiString(kind), PJ::sdk::toAbiString(language),
        PJ::sdk::toAbiString("createMarker(0.0)\n"), PJ::sdk::toAbiString("{}"), &status, &err))
        << kind << "/" << language;
    EXPECT_EQ(status, 77u) << kind << "/" << language;
    EXPECT_NE(std::string_view(err.message).find(expected), std::string_view::npos) << err.message;
  };
  refused("markers", "python", "unsupported script language 'python'");
  refused("bogus", "luau", "unknown generator kind 'bogus'");
}

// The dataset callback owns qualified-key handling: an error it returns fails
// the create through the ABI (nothing may land on a dataset the caller did not
// name), and the keys it normalizes are what the service stores.
TEST(MarkersRuntimeHostTest, ActiveDatasetErrorFailsTheCreate) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly", [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> {
        return PJ::unexpected("input 'in' exists in several datasets");
      });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created =
      view.createMarkers("gen1", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}");
  ASSERT_FALSE(created);
  EXPECT_NE(created.error().find("several datasets"), std::string::npos) << created.error();
  EXPECT_TRUE(service.recipes().empty());
}

TEST(MarkersRuntimeHostTest, CallbackNormalizedKeysAreWhatTheServiceStores) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>& inputs, std::vector<std::string>& outputs) -> PJ::Expected<PJ::DatasetId> {
        for (std::vector<std::string>* keys : {&inputs, &outputs}) {
          for (std::string& key : *keys) {
            if (key.rfind("run:", 0) == 0) {
              key = key.substr(4);
            }
          }
        }
        return kDataset;
      });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"run:in"};
  const PJ::Expected<std::vector<std::string>> created =
      view.createMarkers("gen1", PJ::Span<const std::string_view>(inputs), "run:in", "createMarker(0.0)\n", "{}");
  ASSERT_TRUE(created) << created.error();
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes()[0].inputs, (std::vector<std::string>{"in"}));
  EXPECT_EQ(service.recipes()[0].outputs, (std::vector<std::string>{"in"}));
  EXPECT_EQ(markerFamily(store, kDataset, "in").size(), 1u);
}

TEST(MarkersRuntimeHostTest, RemoveUnknownErrors) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  EXPECT_FALSE(view.remove("nope"));
}

// Two plugins never see each other's generators (per-plugin id isolation).
TEST(MarkersRuntimeHostTest, PerPluginIsolation) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost host_a(
      service, "plugA",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  MarkersRuntimeHost host_b(
      service, "plugB",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
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
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly", [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> {
        return PJ::DatasetId{0};
      });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  EXPECT_FALSE(view.createMarkers("g", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}"));
}

// params {"scope":"all"} publishes a global marker across every listed dataset, on
// the ALL-DATASETS key — the "__global__" the plugin sent is normalized host-side.
TEST(MarkersRuntimeHostTest, ScopeAllPublishesAcrossDatasets) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(created->front(), PJ::markerOwnerTopicName(PJ::kAllDatasetsMarkerTopic, "anomaly/g"));

  EXPECT_EQ(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).size(), 1u);
  EXPECT_TRUE(markerFamily(store, 1, PJ::kAllDatasetsMarkerTopic).empty());
  EXPECT_TRUE(markerFamily(store, 2, PJ::kAllDatasetsMarkerTopic).empty());
}

// The Anomaly Detector submits a Dataset-scope and a Global-scope rule with the SAME
// output key ("__global__") and only the id and the scope hint to tell them apart. Both
// must survive as separate generators and publish to separate topics: the collision this
// guards against left one of the two rules silently unpublished.
TEST(MarkersRuntimeHostTest, DatasetAndGlobalScopeRulesCoexistOverTheAbi) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const std::string output(PJ::sdk::kGlobalMarkerTopic);
  ASSERT_TRUE(view.createMarkers(
      "rule/__global__", PJ::Span<const std::string_view>(inputs), output, "createMarker(0.0)\n", "{}"));
  ASSERT_TRUE(view.createMarkers(
      "rule/__all__", PJ::Span<const std::string_view>(inputs), output, "createMarker(1.0)\n", R"({"scope":"all"})"));

  EXPECT_EQ(service.recipes().size(), 2u);  // neither upsert replaced the other

  EXPECT_EQ(markerFamily(store, 1, PJ::sdk::kGlobalMarkerTopic).size(), 1u);
  EXPECT_TRUE(markerFamily(store, 2, PJ::sdk::kGlobalMarkerTopic).empty());  // bound to dataset 1 only
  EXPECT_EQ(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).size(), 1u);

  // Retiring the all-datasets rule leaves the dataset-scope one publishing.
  ASSERT_TRUE(view.remove("rule/__all__"));
  EXPECT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(readMarkerUnion(store, 1, PJ::sdk::kGlobalMarkerTopic).markers.size(), 1u);
  EXPECT_TRUE(readMarkerUnion(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).markers.empty());
}

// Preview is now create(EPHEMERAL) + remove: the host publishes ephemerally (returns
// the auto-named preview topic, readable in the store, excluded from recipes()).
TEST(MarkersRuntimeHostTest, PreviewViaEphemeralFlagPublishesAndIsEphemeral) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> topics = view.createMarkers(
      "__preview__", PJ::Span<const std::string_view>(inputs), /*output=*/"", "createMarker(0.0)\n", "{}",
      PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  ASSERT_TRUE(topics) << topics.error();
  ASSERT_EQ(topics->size(), 1u);
  EXPECT_TRUE(PJ::sdk::isPreviewMarkerTopic(topics->front()));
  EXPECT_EQ(markerFamily(store, kDataset, topics->front()).size(), 1u);  // host published it
  EXPECT_TRUE(service.recipes().empty());                                // ephemeral, not persisted
  EXPECT_FALSE(view.recipeOf("__preview__").has_value()) << "config hides a preview";
  EXPECT_TRUE(bridge.owns("__preview__")) << "the router's probe still sees it";
  EXPECT_FALSE(bridge.owns("absent"));

  EXPECT_TRUE(view.remove("__preview__"));
  EXPECT_FALSE(bridge.owns("__preview__"));
  EXPECT_TRUE(markerFamily(store, kDataset, topics->front()).empty());
}

// The Anomaly Detector submits its Dataset-scope id ("rule/__global__") on whatever
// dataset is active when the user (re)runs it — the SAME plugin-local id, over and
// over, on different datasets as the active one changes. This must produce one
// binding PER dataset (one id, several bindings), not the second overwriting the
// first, and the ABI surface (list/remove) must still see one plugin-local id.
TEST(MarkersRuntimeHostTest, DatasetScopeRuleOnEachDatasetKeepsBoth) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  PJ::DatasetId active = 1;
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [&active](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> {
        return active;
      });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const std::string output(PJ::sdk::kGlobalMarkerTopic);
  ASSERT_TRUE(view.createMarkers(
      "rule/__global__", PJ::Span<const std::string_view>(inputs), output, "createMarker(0.0)\n", "{}"));

  active = 2;
  ASSERT_TRUE(view.createMarkers(
      "rule/__global__", PJ::Span<const std::string_view>(inputs), output, "createMarker(1.0)\n", "{}"));

  EXPECT_EQ(service.recipes().size(), 2u);

  EXPECT_EQ(readMarkerUnion(store, 1, PJ::sdk::kGlobalMarkerTopic).markers.size(), 1u)
      << "dataset 1's own marker survives";
  EXPECT_EQ(readMarkerUnion(store, 2, PJ::sdk::kGlobalMarkerTopic).markers.size(), 1u) << "dataset 2 published its own";

  const PJ::Expected<std::vector<std::string>> ids = view.list();
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u) << "one plugin-local id, not one row per binding";
  EXPECT_EQ((*ids)[0], "rule/__global__");

  EXPECT_TRUE(view.remove("rule/__global__"));
  EXPECT_TRUE(service.recipes().empty());
}

// Bug (PR #619 #2a): onCreate resolves the dataset BEFORE reading {"scope":"all"},
// so a global generator whose bare input lives in two datasets is refused as
// ambiguous although scope=all evaluates every dataset separately.
TEST(MarkersRuntimeHostTest, ScopeAllWithBareInputInTwoDatasetsIsNotAmbiguous) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOverTwoDatasets());
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).size(), 1u);
}

// Bug (PR #619 #2b): the same ordering makes a constant, input-free scope=all
// generator fail with "none of the generator's inputs matches a loaded series".
TEST(MarkersRuntimeHostTest, ScopeAllWithZeroInputsSucceeds) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOverTwoDatasets());
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>{}, std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).size(), 1u);
}

// Bug (PR #619 #1): the callback strips "b:in" to "in" for storage, but the script
// is kept verbatim, so series("b:in") — the name the plugin declared — is nil.
TEST(MarkersRuntimeHostTest, ScriptCanReadTheQualifiedInputNameItDeclared) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOverTwoDatasets());
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"b:in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), "b:in",
      "local s = series(\"b:in\")\nassert(s ~= nil, \"series('b:in') is nil\")\ncreateMarker(s:at(0).t)\n", "{}");
  ASSERT_TRUE(created) << created.error();
  EXPECT_EQ(markerFamily(store, 2, "in").size(), 1u);
}

// The ABI contract (plugin_data_api.h's `flags` paragraph) requires the host to
// REJECT a reserved/unknown bit, not silently drop it. Bit 5 is unassigned by
// any PJ_DATA_PROCESSOR_FLAG_*.
TEST(MarkersRuntimeHostTest, ReservedFlagBitIsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
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
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
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

// Config read-back keeps a unique bare input bare: qualifying it would change a
// valid address for no reason.
TEST(MarkersRuntimeHostTest, ConfigKeepsAUniqueBareInputBare) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  const SharedCatalog items = std::make_shared<std::vector<PJ::CatalogItem>>();
  items->push_back(catalogRow(1, "in"));
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOver(items), sourceNameOf);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  ASSERT_TRUE(view.createMarkers("g", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}"));
  const PJ::Expected<std::string> recipe = view.recipeOf("g");
  ASSERT_TRUE(recipe) << recipe.error();
  EXPECT_NE(recipe->find("\"inputs\":[\"in\"]"), std::string::npos) << *recipe;
}

// Once the same series appears in a second dataset, the bare name no longer
// resubmits; read-back qualifies it with the bound dataset's source.
TEST(MarkersRuntimeHostTest, ConfigQualifiesAnInputOnceItsBareNameIsAmbiguous) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  const SharedCatalog items = std::make_shared<std::vector<PJ::CatalogItem>>();
  items->push_back(catalogRow(1, "in"));
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOver(items), sourceNameOf);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"in"};
  ASSERT_TRUE(view.createMarkers("g", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}"));
  items->push_back(catalogRow(2, "in"));
  const PJ::Expected<std::string> recipe = view.recipeOf("g");
  ASSERT_TRUE(recipe) << recipe.error();
  EXPECT_NE(recipe->find("\"inputs\":[\"a:in\"]"), std::string::npos) << *recipe;
}

// Under scope=all every dataset is evaluated with literal keys, so a qualifier
// naming a loaded source is a contradiction — refused, not silently used as a
// key that resolves nowhere.
TEST(MarkersRuntimeHostTest, ScopeAllRejectsARecognizedDatasetQualifier) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOverTwoDatasets(), sourceNameOf);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"b:in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})");
  ASSERT_FALSE(created);
  EXPECT_NE(created.error().find("scope=all"), std::string::npos) << created.error();
  EXPECT_TRUE(service.recipes().empty());
}

// A colon name whose prefix is no loaded source stays a literal key under scope=all.
// The literal key must exist on some dataset: a scope=all rule whose inputs resolve
// nowhere is rejected by the service.
TEST(MarkersRuntimeHostTest, ScopeAllKeepsAColonNameThatMatchesNoSource) {
  PJ::ObjectStore store;
  BatchEngine service(store, [](PJ::DatasetId, const std::string& key, const BatchEngine::Window& window) {
    return rampResolver()(1, key == "zz:in" ? "in" : key, window);
  });
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  MarkersRuntimeHost bridge(service, "anomaly", realResolverOverTwoDatasets(), sourceNameOf);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"zz:in"};
  const PJ::Expected<std::vector<std::string>> created = view.createMarkers(
      "g", PJ::Span<const std::string_view>(inputs), std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n",
      R"({"scope":"all"})");
  ASSERT_TRUE(created) << created.error();
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes().front().inputs, (std::vector<std::string>{"zz:in"}));
}

// config tells a plugin what its rule is doing: "computing" while a run is in flight,
// and "error" (only then) when the last run failed, so a late failure is actionable.
TEST(MarkersRuntimeHostTest, ConfigReportsComputingAndTheLastError) {
  std::atomic<bool> released{true};
  const PJ::test::ScopedComputeProbe probe([&released](QThread*) {
    for (int waited_ms = 0; !released.load() && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  std::atomic<int> value{1};
  PJ::ObjectStore store;
  BatchEngine service(
      store,
      [&value](PJ::DatasetId, const std::string& key, const BatchEngine::Window&)
          -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "in") {
          return std::nullopt;
        }
        return BatchEngine::ResolvedSeries{.timestamps = {0.0}, .values = {static_cast<double>(value.load())}};
      });
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());
  const std::string_view inputs[] = {"in"};
  ASSERT_TRUE(view.createMarkers(
      "gen", PJ::Span<const std::string_view>(inputs), "in",
      "local s = series(\"in\")\nif s:at(0).v >= 5 then error(\"value too big\") end\ncreateMarker(0.0)\n", "{}"));
  pool.waitIdle();
  const auto config = [&view] {
    const PJ::Expected<std::string> recipe = view.recipeOf("gen");
    return recipe.has_value() ? *recipe : recipe.error();
  };
  EXPECT_NE(config().find("\"computing\":false"), std::string::npos) << config();

  released = false;
  service.refresh("anomaly/gen");
  EXPECT_NE(config().find("\"computing\":true"), std::string::npos) << config();
  released = true;
  pool.waitIdle();
  EXPECT_NE(config().find("\"computing\":false"), std::string::npos) << config();
  EXPECT_EQ(config().find("\"error\""), std::string::npos) << "no failure, no error key: " << config();

  value = 5;
  service.refresh("anomaly/gen");
  pool.waitIdle();
  EXPECT_NE(config().find("\"error\":"), std::string::npos) << config();
  EXPECT_NE(config().find("value too big"), std::string::npos) << config();
}

// Released plugins read a rule's markers right after create returns (the anomaly
// detector's preview caches them until its input changes), so a create through the
// plugin ABI runs to completion, ephemeral or persistent, whatever the create budget
// and however long the run: the markers are readable when it returns.
TEST(MarkersRuntimeHostTest, CreateReturnsWithTheRunsMarkersPublished) {
  const PJ::test::ScopedCreateBudget budget(BatchEngine::kCreateBudget);
  std::atomic<bool> released{false};
  const PJ::test::ScopedComputeProbe probe(PJ::test::holdPoolComputesUntil(released));
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  PJ::EnginePool pool;
  service.setPool(&pool);
  MarkersRuntimeHost bridge(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
  PJ::sdk::DataProcessorsHostView view(bridge.raw());
  const std::string_view inputs[] = {"in"};

  const PJ::Expected<std::vector<std::string>> persistent =
      view.createMarkers("gen", PJ::Span<const std::string_view>(inputs), "in", "createMarker(0.0)\n", "{}");
  ASSERT_TRUE(persistent) << persistent.error();
  EXPECT_EQ(readMarkerUnion(store, kDataset, "in").markers.size(), 1u) << "published before the create returned";

  const PJ::Expected<std::vector<std::string>> preview = view.createMarkers(
      "__preview__", PJ::Span<const std::string_view>(inputs), /*output=*/"", "createMarker(0.0)\n", "{}",
      PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  ASSERT_TRUE(preview) << preview.error();
  ASSERT_EQ(preview->size(), 1u);
  EXPECT_EQ(readMarkerUnion(store, kDataset, preview->front()).markers.size(), 1u)
      << "the preview's markers are readable when the create returns";
  released = true;
}

MarkersRuntimeHost fixedDatasetBridge(BatchEngine& service) {
  return MarkersRuntimeHost(
      service, "anomaly",
      [](std::vector<std::string>&, std::vector<std::string>&) -> PJ::Expected<PJ::DatasetId> { return kDataset; });
}

PJ::sdk::DataProcessorRequest markerRequest(std::string_view id, std::string script) {
  PJ::sdk::DataProcessorRequest request;
  request.id = std::string(id);
  request.kind = "markers";
  request.language = "luau";
  request.inputs = {"in"};
  request.outputs = {{"in", ""}};
  request.script = std::move(script);
  return request;
}

// BAT-10: a WINDOW on the typed create installs a generator clipped to it.
TEST(MarkersRuntimeHostTest, CreateV2WithWindowInstallsAClippedGenerator) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge = fixedDatasetBridge(service);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  PJ::sdk::DataProcessorRequest request = markerRequest("windowed", R"(
    local s = series("in")
    for i = 0, s:size() - 1 do
      local p = s:at(i)
      createMarker(p.t, p.v)
    end
  )");
  request.window = std::pair<int64_t, int64_t>{2, 3};

  const PJ::Expected<std::vector<std::string>> created = view.createV2(request);
  ASSERT_TRUE(created) << created.error();
  ASSERT_EQ(service.recipes().size(), 1u);
  ASSERT_TRUE(service.recipes().front().window.has_value());
  EXPECT_EQ(*service.recipes().front().window, (std::pair<PJ::Timestamp, PJ::Timestamp>{2, 3}));
  EXPECT_EQ(readMarkerUnion(store, kDataset, "in").markers.size(), 2u) << "only samples 2 and 3 fall in the window";
}

// BAT-10: config reports the window the last run actually staged (coverage_ns), not the
// requested one: the ramp has samples 0..4, so [1, 100] staged [1, 4]. An unwindowed
// rule reports no coverage.
TEST(MarkersRuntimeHostTest, ConfigReportsTheStagedCoverage) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge = fixedDatasetBridge(service);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  PJ::sdk::DataProcessorRequest request = markerRequest("coverage", "createMarker(0.0)\n");
  request.window = std::pair<int64_t, int64_t>{1, 100};
  ASSERT_TRUE(view.createV2(request));
  const PJ::Expected<std::string> recipe = view.recipeOf("coverage");
  ASSERT_TRUE(recipe) << recipe.error();
  EXPECT_NE(recipe->find(R"("coverage_ns":{"first_ns":1,"last_ns":4})"), std::string::npos) << *recipe;

  ASSERT_TRUE(view.createV2(markerRequest("whole", "createMarker(0.0)\n")));
  const PJ::Expected<std::string> whole = view.recipeOf("whole");
  ASSERT_TRUE(whole) << whole.error();
  EXPECT_EQ(whole->find("coverage_ns"), std::string::npos) << *whole;
}

// BAT-10: a window whose start is after its end is refused; nothing is installed.
TEST(MarkersRuntimeHostTest, CreateV2WindowStartAfterEndIsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge = fixedDatasetBridge(service);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  PJ::sdk::DataProcessorRequest request = markerRequest("backwards", "createMarker(0.0)\n");
  request.window = std::pair<int64_t, int64_t>{40, 20};
  const PJ::Expected<std::vector<std::string>> created = view.createV2(request);
  ASSERT_FALSE(created);
  EXPECT_NE(created.error().find("window"), std::string::npos) << created.error();
  EXPECT_TRUE(service.recipes().empty());
}

// The markers kind takes no INSTANT (a marker rule has no pinned evaluation); refused
// naming the kind, nothing installed.
TEST(MarkersRuntimeHostTest, CreateV2WithInstantIsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge = fixedDatasetBridge(service);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  PJ::sdk::DataProcessorRequest request = markerRequest("pinned", "createMarker(0.0)\n");
  request.instant_ns = 2;
  const PJ::Expected<std::vector<std::string>> created = view.createV2(request);
  ASSERT_FALSE(created);
  EXPECT_NE(created.error().find("kind 'markers'"), std::string::npos) << created.error();
  EXPECT_NE(created.error().find("INSTANT"), std::string::npos) << created.error();
  EXPECT_TRUE(service.recipes().empty());
}

// Until the catalog v2 work, the markers kind takes no typed output and no dataset_id;
// each is refused naming the kind, nothing installed.
TEST(MarkersRuntimeHostTest, CreateV2WithATypedOutputOrDatasetIdIsRejectedNamingTheKind) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver());
  MarkersRuntimeHost bridge = fixedDatasetBridge(service);
  PJ::sdk::DataProcessorsHostView view(bridge.raw());

  PJ::sdk::DataProcessorRequest typed_output = markerRequest("typed", "createMarker(0.0)\n");
  typed_output.outputs = {{"in", "number"}};
  const auto typed = view.createV2(typed_output);
  ASSERT_FALSE(typed);
  EXPECT_NE(typed.error().find("kind 'markers' takes untyped outputs"), std::string::npos) << typed.error();

  PJ::sdk::DataProcessorRequest on_dataset = markerRequest("on_dataset", "createMarker(0.0)\n");
  on_dataset.dataset_id = kDataset;
  const auto refused = view.createV2(on_dataset);
  ASSERT_FALSE(refused);
  EXPECT_NE(
      refused.error().find("kind 'markers' takes no dataset_id yet: qualify the inputs with '<source>:'"),
      std::string::npos)
      << refused.error();
  EXPECT_TRUE(service.recipes().empty());
}

}  // namespace
