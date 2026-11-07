// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// BatchEngine backs the unified pj.data_processors.v1 service (kind=markers — the object
// engine). These tests drive it against a synthetic resolver + a real ObjectStore —
// no SDK service, no catalog — proving the host-driven execution path end to end:
// resolve → run engine → serialize → publish to ObjectStore → read back.

#include <gtest/gtest.h>

#include <QObject>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "batch_engine_test_utils.h"
#include "event_pump_test_utils.h"
#include "marker_test_utils.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace {

using PJ::BatchEngine;
using PJ::GeneratorKind;
using PJ::kAllDatasetsMarkerDataset;
using PJ::test::markerFamily;
using PJ::test::ownerPublishUid;
using PJ::test::ownerTombstoned;
using PJ::test::pumpFor;
using PJ::test::pumpUntil;
using PJ::test::ScopedComputeProbe;
using PJ::test::ScopedCreateBudget;

constexpr PJ::DatasetId kDataset = 1;

// A resolver backed by one synthetic ramp series "in": t = i ns, v = i, i in [0,n).
BatchEngine::SeriesResolver rampResolver(std::size_t n) {
  return [n](PJ::DatasetId, const std::string& key,
             const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    for (std::size_t i = 0; i < n; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
}

// rampResolver(5), but only on the datasets in `datasets` — the all_datasets cases
// need "the rule's series exists on some datasets, not others".
BatchEngine::SeriesResolver resolverOn(std::set<PJ::DatasetId> datasets) {
  return [datasets, ramp = rampResolver(5)](
             PJ::DatasetId dataset, const std::string& key,
             const BatchEngine::Window& window) -> std::optional<BatchEngine::ResolvedSeries> {
    return datasets.contains(dataset) ? ramp(dataset, key, window) : std::nullopt;
  };
}

// What a plot draws for `marker_topic` on `dataset`: the union of every family
// member (the bare topic plus one per owner). Empty when nothing is published.
PJ::sdk::PlotMarkers readPublished(PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& marker_topic) {
  return PJ::test::readMarkerUnion(store, dataset, marker_topic);
}

// A kind=markers generator with a single output topic key.
BatchEngine::GeneratorRecipe markerRecipe(std::string id, std::string output, std::string script) {
  BatchEngine::GeneratorRecipe r;
  r.id = std::move(id);
  r.kind = GeneratorKind::kMarkers;
  r.dataset_id = kDataset;
  r.inputs = {"in"};
  r.outputs = {std::move(output)};
  r.script = std::move(script);
  return r;
}

// The host resolves the input series, runs the script, and publishes its markers.
TEST(BatchEngineTest, RunsScriptAndPublishesMarkers) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(10));

  auto recipe = markerRecipe("plug/gen1", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v, {label="first"})
  )");

  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());
  EXPECT_EQ(ok->front(), PJ::markerOwnerTopicName("in", "plug/gen1")) << "the physical topic, readable back";

  const PJ::sdk::PlotMarkers set = readPublished(store, kDataset, "in");
  ASSERT_EQ(set.markers.size(), 1u);
  EXPECT_EQ(set.markers[0].kind, PJ::sdk::MarkerKind::kEvent);
  EXPECT_EQ(set.markers[0].label, "first");
  EXPECT_EQ(service.recipes().size(), 1u);
}

// A script error is surfaced (not swallowed) and the failed upsert registers nothing.
// Before a resolver is injected the service holds a stub that resolves nothing, so a
// generator's inputs are all absent and its script hits a nil series. That must surface
// as a plain error, never as a silently-empty marker set: the shell decides what to do
// with it, and an empty set would read as "the rule found nothing".
TEST(BatchEngineTest, WithoutAResolverTheRunFailsInsteadOfPublishingAnEmptySet) {
  PJ::ObjectStore store;
  BatchEngine service(store, BatchEngine::SeriesResolver{});

  auto recipe = markerRecipe("plug/no_resolver", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v, {label="first"})
  )");

  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(markerFamily(store, kDataset, "in").empty());
  EXPECT_TRUE(service.recipes().empty());
}

TEST(BatchEngineTest, ScriptErrorReturnsMessageAndDoesNotRegister) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/bad", "__global__", "this is not valid lua %%%");
  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// recomputeForChangedInputs marks stale only the generators that read a changed
// key (it runs nothing, and returns the affected ids); an unrelated change is a
// no-op. removeGenerator drops the recipe and its staleness.
TEST(BatchEngineTest, RecomputeMatchesChangedKeyAndRemoveDrops) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(8));
  service.setMode(BatchEngine::Mode::kManual);  // isolate the marking from the brake

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());

  EXPECT_TRUE(service.recomputeForChangedInputs({"other"}, BatchEngine::kAnyDataset).empty());
  EXPECT_FALSE(service.isStale("plug/gen"));

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, BatchEngine::kAnyDataset);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(affected[0], "plug/gen");
  EXPECT_TRUE(service.isStale("plug/gen"));

  EXPECT_TRUE(service.removeGenerator("plug/gen").has_value());
  EXPECT_FALSE(service.removeGenerator("plug/gen").has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A changed topic marks the rules reading a series rooted at it: the topic itself or
// a field below a slash. `in` is not `in_raw`, so that rule does not re-run.
TEST(BatchEngineTest, ChangedTopicMatchesOnTheSlashBoundary) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(8));
  service.setMode(BatchEngine::Mode::kManual);
  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");
  recipe.inputs = {"in_raw/x"};
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  EXPECT_TRUE(service.recomputeForChangedInputs({"in"}, BatchEngine::kAnyDataset).empty());
  EXPECT_FALSE(service.isStale("plug/gen"));

  EXPECT_EQ(
      service.recomputeForChangedInputs({"in_raw"}, BatchEngine::kAnyDataset), std::vector<std::string>{"plug/gen"});
  EXPECT_TRUE(service.isStale("plug/gen"));
}

// persistentBindingCount counts what recipes() would list: previews are not bindings
// of the layout.
TEST(BatchEngineTest, PersistentBindingCountIgnoresPreviews) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(8));
  EXPECT_EQ(service.persistentBindingCount(), 0u);

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  auto preview = markerRecipe("plug/preview", "in", "createMarker(0.0)\n");
  preview.ephemeral = true;
  ASSERT_TRUE(service.upsertGenerator(preview).has_value());

  EXPECT_EQ(service.persistentBindingCount(), 1u);
  EXPECT_EQ(service.persistentBindingCount(), service.recipes().size());
}

// The "__preview__/" namespace is reserved: a committed (non-ephemeral) marker
// generator targeting it is rejected.
TEST(BatchEngineTest, CommittedGeneratorRejectsPreviewPrefix) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/x", "__preview__/sneaky", "createMarker(0.0)\n");
  EXPECT_FALSE(service.upsertGenerator(recipe).has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A global-across-all generator publishes ONCE, on the dataset-independent home of
// the ALL-DATASETS key — the "__global__" it sent is normalized host-side — and
// never copies its set onto the datasets it reads from.
TEST(BatchEngineTest, GlobalAllDatasetsPublishesOnceOnTheSharedDataset) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  EXPECT_TRUE(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g").has_value());
  EXPECT_TRUE(markerFamily(store, 1, all_topic).empty());
  EXPECT_TRUE(markerFamily(store, 2, all_topic).empty());
  EXPECT_TRUE(markerFamily(store, 1, PJ::sdk::kGlobalMarkerTopic).empty());
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes().front().outputs.front(), PJ::kAllDatasetsMarkerTopic);
}

// all_datasets flips a "__global__" output to the ALL-DATASETS key; the physical
// object topic and the persisted recipe both reflect the rewritten key.
TEST(BatchEngineTest, AllDatasetsOutputNormalizesGlobalToAll) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{kDataset}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());
  EXPECT_EQ(ok->front(), PJ::markerOwnerTopicName(PJ::kAllDatasetsMarkerTopic, "plug/g"));
}

// A dataset-bound recipe (all_datasets false) may never target the reserved
// ALL-DATASETS key — that key's ownership is exclusively all_datasets generators.
TEST(BatchEngineTest, AllTopicOnDatasetBoundRecipeIsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/g", std::string(PJ::kAllDatasetsMarkerTopic), "createMarker(0.0)\n");
  EXPECT_FALSE(service.upsertGenerator(recipe).has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A Dataset-scope rule and a Global-scope rule can target the same dataset without
// colliding: they normalize to different object topics ("__global__" vs "__all__").
TEST(BatchEngineTest, DatasetAndAllScopesCoexistOnOneDataset) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  auto dataset_scope = markerRecipe("rule/__global__", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  dataset_scope.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(dataset_scope).has_value());

  auto all_scope = markerRecipe("rule/__all__", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(1.0)\n");
  all_scope.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(all_scope).has_value());

  EXPECT_EQ(markerFamily(store, 1, PJ::sdk::kGlobalMarkerTopic).size(), 1u);
  EXPECT_TRUE(markerFamily(store, 2, PJ::sdk::kGlobalMarkerTopic).empty());
  EXPECT_EQ(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).size(), 1u);
  EXPECT_TRUE(markerFamily(store, 1, PJ::kAllDatasetsMarkerTopic).empty());
}

// What a stored topic's metadata says about it; nullopt when it is not a marker description.
std::optional<PJ::MarkerTopicInfo> markerInfoOf(
    const PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& topic_name) {
  const auto id = store.findTopic(dataset, topic_name);
  EXPECT_TRUE(id.has_value()) << topic_name;
  return id.has_value() ? PJ::markerTopicInfo(store.descriptor(*id).metadata_json) : std::nullopt;
}

// GAP-43: a generator's topic is registered with the markers type, the scope and target
// its output key means, and its owner. The metadata, not the name, identifies it.
TEST(BatchEngineTest, SeriesScopeTopicCarriesItsMetadata) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/g", "in", "createMarker(0.0)\n")).has_value());

  const std::string name = PJ::markerOwnerTopicName("in", "plug/g");
  const std::string json = store.descriptor(*store.findTopic(kDataset, name)).metadata_json;
  EXPECT_NE(json.find(R"("builtin_object_type":"kPlotMarkers")"), std::string::npos) << json;
  EXPECT_NE(json.find(R"("pj_derived":"markers")"), std::string::npos) << json;
  EXPECT_EQ(markerInfoOf(store, kDataset, name), (PJ::MarkerTopicInfo{PJ::MarkerScopeKind::kSeries, "in", "plug/g"}));
}

TEST(BatchEngineTest, DatasetScopeTopicCarriesItsMetadata) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  EXPECT_EQ(
      markerInfoOf(store, kDataset, PJ::markerOwnerTopicName(PJ::sdk::kGlobalMarkerTopic, "plug/g")),
      (PJ::MarkerTopicInfo{PJ::MarkerScopeKind::kDataset, "", "plug/g"}));
}

// An all-datasets rule publishes on the shared home; its scope follows its output
// key: the all-datasets key means "every plot", a series key means "plots of that
// series on any dataset".
TEST(BatchEngineTest, AllDatasetsTopicsCarryTheirMetadata) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{kDataset}; });

  auto everywhere = markerRecipe("plug/all", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  everywhere.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(everywhere).has_value());
  auto one_series = markerRecipe("plug/series", "in", "createMarker(0.0)\n");
  one_series.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(one_series).has_value());

  EXPECT_EQ(
      markerInfoOf(store, kAllDatasetsMarkerDataset, PJ::markerOwnerTopicName(PJ::kAllDatasetsMarkerTopic, "plug/all")),
      (PJ::MarkerTopicInfo{PJ::MarkerScopeKind::kAll, "", "plug/all"}));
  EXPECT_EQ(
      markerInfoOf(store, kAllDatasetsMarkerDataset, PJ::markerOwnerTopicName("in", "plug/series")),
      (PJ::MarkerTopicInfo{PJ::MarkerScopeKind::kSeries, "in", "plug/series"}));
}

// A merge republishes a source's marker topic on the anchor: the new topic keeps the
// description the source's carried.
TEST(BatchEngineTest, MergedMarkerTopicKeepsItsMetadata) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  auto recipe = markerRecipe("plug/src", "in", "createMarker(5.0)\n");
  recipe.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});

  EXPECT_EQ(
      markerInfoOf(store, 1, PJ::markerOwnerTopicName("in", "plug/src")),
      (PJ::MarkerTopicInfo{PJ::MarkerScopeKind::kSeries, "in", "plug/src"}));
}

// A generator's topics are found by their owner metadata: a direct write whose name
// merely ends with `#<id>` is not one of them.
TEST(BatchEngineTest, RemovingAGeneratorLeavesADirectWriteNamedLikeItsTopic) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  PJ::sdk::PlotMarkers set;
  set.markers.push_back(PJ::sdk::PlotMarker{.kind = PJ::sdk::MarkerKind::kEvent, .t_start = 7});
  const auto direct = store.registerTopic(
      {.dataset_id = kDataset, .topic_name = PJ::sdk::markerObjectTopicName("other#plug/g"), .metadata_json = "{}"});
  ASSERT_TRUE(direct.has_value());
  ASSERT_TRUE(store.pushOwned(*direct, PJ::Timestamp{0}, PJ::serializePlotMarkers(set)).has_value());

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/g", "in", "createMarker(0.0)\n")).has_value());
  ASSERT_TRUE(service.removeGenerator("plug/g").has_value());

  EXPECT_TRUE(ownerTombstoned(store, kDataset, "in", "plug/g"));
  const auto kept = PJ::test::latestEntry(store, *direct);
  ASSERT_TRUE(kept.has_value());
  EXPECT_FALSE(kept->payload.bytes.empty()) << "the direct write must not be tombstoned";
}

// Registers `name` on `dataset` the way a plugin's direct write does and publishes one
// event at `t_start`.
PJ::ObjectTopicId registerDirectMarkerWrite(
    PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& name, PJ::Timestamp t_start,
    const std::string& metadata_json = "{}") {
  PJ::sdk::PlotMarkers set;
  set.markers.push_back(PJ::sdk::PlotMarker{.kind = PJ::sdk::MarkerKind::kEvent, .t_start = t_start});
  const auto id = store.registerTopic({.dataset_id = dataset, .topic_name = name, .metadata_json = metadata_json});
  EXPECT_TRUE(id.has_value()) << name;
  if (!id.has_value()) {
    return {};
  }
  EXPECT_TRUE(store.pushOwned(*id, PJ::Timestamp{0}, PJ::serializePlotMarkers(set)).has_value());
  return *id;
}

bool hasPublishedMarkers(const PJ::ObjectStore& store, PJ::ObjectTopicId id) {
  const auto entry = PJ::test::latestEntry(store, id);
  return entry.has_value() && !entry->payload.bytes.empty();
}

// An id-less recipe (a layout without the `id` attribute) owns no topic, so retiring
// "its" topics would match every direct write: it is refused up front.
TEST(BatchEngineTest, EmptyGeneratorIdIsRejectedAndDirectWritesSurvive) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  const PJ::ObjectTopicId direct =
      registerDirectMarkerWrite(store, kDataset, PJ::sdk::markerObjectTopicName("other"), 7);

  const auto refused = service.upsertGenerator(markerRecipe("", "in", "createMarker(0.0)\n"));
  EXPECT_FALSE(refused.has_value());
  EXPECT_TRUE(service.generatorIds().empty());

  EXPECT_FALSE(service.removeGenerator("").has_value());
  service.clearAllGenerators();
  EXPECT_TRUE(hasPublishedMarkers(store, direct)) << "a direct write must not be tombstoned";
}

// Metadata is JSON: a key that is not UTF-8 cannot be described in it, and the
// generator is refused rather than throwing out of the publish path.
TEST(BatchEngineTest, OutputKeyThatIsNotUtf8IsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  PJ::Expected<std::vector<std::string>> refused = PJ::unexpected(std::string{});
  EXPECT_NO_THROW(refused = service.upsertGenerator(markerRecipe("plug/g", "in\xE9", "createMarker(0.0)\n")));
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("UTF-8"), std::string::npos) << refused.error();
  EXPECT_TRUE(store.listTopics(kDataset).empty());
  EXPECT_TRUE(service.generatorIds().empty());
}

// A topic already holding the generator's name must describe the same marker set: a
// direct write that squats the minted name is not adopted and overwritten.
TEST(BatchEngineTest, GeneratorDoesNotAdoptAnExistingTopicThatDescribesSomethingElse) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  const std::string squatted = PJ::markerOwnerTopicName("in", "plug/g");
  const PJ::ObjectTopicId direct = registerDirectMarkerWrite(store, kDataset, squatted, 7);
  const auto before = PJ::test::latestEntry(store, direct);
  ASSERT_TRUE(before.has_value());

  const auto refused = service.upsertGenerator(markerRecipe("plug/g", "in", "createMarker(0.0)\n"));

  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find(squatted), std::string::npos) << refused.error();
  const auto after = PJ::test::latestEntry(store, direct);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->sequential_uid.value, before->sequential_uid.value) << "the direct write's payload is untouched";
  EXPECT_TRUE(std::ranges::equal(after->payload.bytes, before->payload.bytes));
}

// Same-named marker topics that describe different sets are not concatenated: the
// source's topic is left alone instead of being folded under the anchor's description.
TEST(BatchEngineTest, MergeLeavesASameNamedMarkerTopicThatDescribesAnotherSet) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  const std::string name = PJ::sdk::markerObjectTopicName("x");
  const PJ::ObjectTopicId anchor_topic = registerDirectMarkerWrite(store, 1, name, 100);
  const PJ::ObjectTopicId source_topic = registerDirectMarkerWrite(
      store, 2, name, 5, PJ::markerTopicMetadataJson({PJ::MarkerScopeKind::kSeries, "y", "plug/g"}));

  const auto touched = service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});

  EXPECT_TRUE(touched.empty());
  EXPECT_TRUE(store.findTopic(2, name).has_value()) << "the source topic is not destroyed";
  EXPECT_TRUE(hasPublishedMarkers(store, source_topic));
  const auto anchor_entry = PJ::test::latestEntry(store, anchor_topic);
  ASSERT_TRUE(anchor_entry.has_value());
  const auto anchor_set =
      PJ::deserializePlotMarkers(anchor_entry->payload.bytes.data(), anchor_entry->payload.bytes.size());
  ASSERT_TRUE(anchor_set.has_value());
  EXPECT_EQ(anchor_set->markers.size(), 1u) << "nothing was folded into the anchor's set";
}

// Two generators on one marker topic each own their own object topic: the plot
// draws both sets, and retiring one tombstones only its topic — the other's is
// not even republished.
TEST(BatchEngineTest, TwoGeneratorsOnOneTopicOwnTheirOwnObjectTopics) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/a", "shared", "createMarker(0.0)\n")).has_value());
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/b", "shared", "createMarker(1.0)\n")).has_value());
  EXPECT_EQ(markerFamily(store, kDataset, "shared").size(), 2u) << "one object topic per owner";
  EXPECT_EQ(readPublished(store, kDataset, "shared").markers.size(), 2u);
  const std::optional<std::uint64_t> b_uid = ownerPublishUid(store, kDataset, "shared", "plug/b");
  ASSERT_TRUE(b_uid.has_value());

  ASSERT_TRUE(service.removeGenerator("plug/a").has_value());
  EXPECT_TRUE(ownerTombstoned(store, kDataset, "shared", "plug/a"));
  EXPECT_EQ(ownerPublishUid(store, kDataset, "shared", "plug/b"), b_uid) << "B's topic is untouched by A's retirement";
  EXPECT_EQ(readPublished(store, kDataset, "shared").markers.size(), 1u);

  ASSERT_TRUE(service.removeGenerator("plug/b").has_value());
  EXPECT_TRUE(ownerTombstoned(store, kDataset, "shared", "plug/b"));
  EXPECT_TRUE(readPublished(store, kDataset, "shared").markers.empty());
}

// A recompute rewrites only the re-run generator's own part of a shared target —
// the other owner's part is neither dropped nor duplicated.
TEST(BatchEngineTest, RecomputeOneGeneratorKeepsOthersPart) {
  PJ::ObjectStore store;
  BatchEngine::SeriesResolver resolver = [](PJ::DatasetId, const std::string& key,
                                            const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in" && key != "other") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
  BatchEngine service(store, resolver);

  auto in_recipe = markerRecipe("plug/in", "shared", "createMarker(0.0)\n");
  in_recipe.inputs = {"in"};
  ASSERT_TRUE(service.upsertGenerator(in_recipe).has_value());

  auto other_recipe = markerRecipe("plug/other", "shared", "createMarker(1.0)\n");
  other_recipe.inputs = {"other"};
  ASSERT_TRUE(service.upsertGenerator(other_recipe).has_value());

  ASSERT_EQ(readPublished(store, kDataset, "shared").markers.size(), 2u);

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, kDataset);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(affected[0], "plug/in");
  service.refresh("plug/in");  // force the marked-stale run now (ignores brake/demand)
  EXPECT_EQ(readPublished(store, kDataset, "shared").markers.size(), 2u)
      << "the 'in' owner's part is replaced, not appended";
}

// A retarget must drop only the retargeted generator's own part of the old
// target, never a sibling owner's part still publishing there.
TEST(BatchEngineTest, UpsertRetargetDropsOnlyOwnPart) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/a", "old", "createMarker(0.0)\n")).has_value());
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/b", "old", "createMarker(1.0)\n")).has_value());
  ASSERT_EQ(readPublished(store, kDataset, "old").markers.size(), 2u);

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/b", "new", "createMarker(1.0)\n")).has_value());

  EXPECT_EQ(readPublished(store, kDataset, "old").markers.size(), 1u) << "A's part survives";
  EXPECT_EQ(readPublished(store, kDataset, "new").markers.size(), 1u);
}

// A blob pushed directly to the bare marker object topic (a toolbox write, no
// owning generator) is simply another member of the family: a generator on the
// same key draws beside it, never rewrites it, and its removal leaves it alone.
TEST(BatchEngineTest, DirectWriteIsAnotherFamilyMember) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  PJ::sdk::PlotMarkers foreign;
  PJ::sdk::PlotMarker fm;
  fm.kind = PJ::sdk::MarkerKind::kEvent;
  fm.t_start = 42;
  foreign.markers.push_back(fm);
  const auto id = store.registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = kDataset, .topic_name = PJ::sdk::markerObjectTopicName("in"), .metadata_json = {}});
  ASSERT_TRUE(id.has_value());
  store.setRetentionBudget(*id, PJ::RetentionBudget{.max_entries = 1});
  store.pushOwned(*id, PJ::Timestamp{0}, PJ::serializePlotMarkers(foreign));
  const std::uint64_t direct_uid = PJ::test::latestEntry(store, *id)->sequential_uid.value;

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.size(), 2u) << "both sets are drawn";
  EXPECT_EQ(PJ::test::latestEntry(store, *id)->sequential_uid.value, direct_uid) << "the direct write is not rewritten";

  ASSERT_TRUE(service.removeGenerator("plug/gen").has_value());
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.size(), 1u) << "the direct write survives the owner's removal";
  EXPECT_EQ(PJ::test::latestEntry(store, *id)->sequential_uid.value, direct_uid);
}

// Retargeting a live generator's output must not strand the old topic: nothing else
// ever names it again, so it would keep drawing a stale set forever.
TEST(BatchEngineTest, UpsertRetargetTombstonesThePreviousOutput) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "old", "createMarker(0.0)\n")).has_value());
  ASSERT_FALSE(readPublished(store, kDataset, "old").markers.empty());

  // Same id, different output topic.
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "new", "createMarker(0.0)\n")).has_value());

  EXPECT_FALSE(readPublished(store, kDataset, "new").markers.empty()) << "the new output is live";
  EXPECT_TRUE(ownerTombstoned(store, kDataset, "old", "plug/gen"))
      << "the abandoned output is emptied (topic kept), not left stale";
}

// An unchanged upsert must NOT tombstone its own fresh output — the overlap between
// the previous and the new publish targets is exactly what the run just rewrote.
TEST(BatchEngineTest, UpsertInPlaceKeepsItsOutputPublished) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(1.0)\n")).has_value());

  EXPECT_FALSE(readPublished(store, kDataset, "in").markers.empty());
}

// A merge folds the sources into the anchor and drops them. Without the rebind the
// recipe keeps naming a dataset that no longer exists: the merged blob stays on screen
// (nothing overwrites it) but the rule is dead, and the next recompute publishes an
// empty set onto a phantom topic instead of refreshing the anchor.
TEST(BatchEngineTest, MergeRemapsConsumedRecipesOntoTheAnchor) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  constexpr PJ::DatasetId kAnchor = 1;
  constexpr PJ::DatasetId kConsumed = 2;
  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");
  recipe.dataset_id = kConsumed;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  ASSERT_EQ(service.recipes().front().dataset_id, kConsumed);

  service.remapGeneratorsToAnchor(kAnchor, {kConsumed});

  ASSERT_EQ(service.recipes().size(), 1u) << "the rule survives the merge";
  EXPECT_EQ(service.recipes().front().dataset_id, kAnchor);

  // And it now actually publishes on the anchor.
  EXPECT_FALSE(service.recomputeForDataset(kAnchor).empty());
  EXPECT_FALSE(readPublished(store, kAnchor, "in").markers.empty());
}

// Removing a dataset takes its object topics with it, but a recipe naming the dead id
// would re-register a phantom topic on the next recompute. Session-scoped
// (all_datasets) generators are deliberately kept — they outlive any one dataset.
TEST(BatchEngineTest, DropRecipesForDatasetKeepsGlobalGenerators) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{kDataset}; });

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/bound", "in", "createMarker(0.0)\n")).has_value());
  auto global = markerRecipe("plug/global", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  global.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(global).has_value());
  ASSERT_EQ(service.recipes().size(), 2u);

  service.clearGeneratorsForDataset(kDataset);

  const std::vector<BatchEngine::GeneratorRecipe> left = service.recipes();
  ASSERT_EQ(left.size(), 1u) << "the dataset-bound recipe is gone";
  EXPECT_EQ(left.front().id, "plug/global");
  EXPECT_TRUE(left.front().all_datasets);
}

// A recompute names the dataset whose data moved. An all_datasets generator's set
// is one shared union, so a contributor's change republishes that one topic.
TEST(BatchEngineTest, ScopedRecomputeOfGlobalRuleRepublishesTheSharedSet) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  const std::optional<std::uint64_t> before = ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g");
  ASSERT_TRUE(before.has_value());

  // Dataset 1 ingested; dataset 2 sat still — but the set is shared, so a contributor's
  // change is everyone's change.
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, /*scope=*/1).empty());
  service.refresh("plug/g");  // force the marked-stale run now (ignores brake/demand)
  EXPECT_NE(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"), before)
      << "the shared set is republished";
}

// The same attribution applies to per-dataset generators: `changed` carries bare topic
// names, and two datasets routinely share them, so without the scope a stream tick on
// one dataset would re-run a generator bound to the other.
TEST(BatchEngineTest, ScopedRecomputeIgnoresGeneratorsBoundElsewhere) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");  // bound to kDataset (1)
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  // A change on dataset 2 names the same series key, but this generator reads dataset 1.
  EXPECT_TRUE(service.recomputeForChangedInputs({"in"}, /*scope=*/2).empty());
  // Unscoped, the caller cannot attribute the change, so the name match still wins.
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, BatchEngine::kAnyDataset).empty());
}

// An ephemeral preview is published but excluded from recipes() (never persisted);
// remove() tears it down. Preview is now just create(ephemeral) + remove.
TEST(BatchEngineTest, EphemeralPreviewPublishesAndRemoves) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(6));

  BatchEngine::GeneratorRecipe recipe;
  recipe.id = "plug/__preview__";
  recipe.kind = GeneratorKind::kMarkers;
  recipe.dataset_id = kDataset;
  recipe.inputs = {"in"};
  recipe.ephemeral = true;  // outputs left empty → host auto-names the preview topic
  recipe.script = "createMarker(0.0)\n";

  const PJ::Expected<std::vector<std::string>> topics = service.upsertGenerator(recipe);
  ASSERT_TRUE(topics.has_value()) << (topics.has_value() ? std::string{} : topics.error());
  ASSERT_EQ(topics->size(), 1u);
  const std::string object_topic = topics->front();
  EXPECT_TRUE(PJ::sdk::isPreviewMarkerTopic(object_topic));
  EXPECT_EQ(markerFamily(store, kDataset, object_topic).size(), 1u);  // published...
  EXPECT_TRUE(service.recipes().empty());                             // ...but not persisted

  EXPECT_TRUE(service.removeGenerator("plug/__preview__").has_value());
  EXPECT_TRUE(markerFamily(store, kDataset, object_topic).empty());  // topic removed, not tombstoned
}

// H3: removing a PERSISTENT generator tombstones its output — the topic survives
// but decodes to an empty set (so a later merge/reload reads live-but-empty, not
// stale), unlike an ephemeral preview whose throwaway topic is removed outright.
TEST(BatchEngineTest, RemovePersistentGeneratorTombstonesOutput) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(10));

  auto recipe = markerRecipe("plug/gen", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v)
  )");
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  EXPECT_FALSE(readPublished(store, kDataset, "in").markers.empty());  // has markers

  ASSERT_TRUE(service.removeGenerator("plug/gen").has_value());
  // The owner's topic survives (tombstone), but its published blob is now empty: the
  // empty set serializes to zero bytes, so the overlay draws nothing.
  EXPECT_TRUE(ownerTombstoned(store, kDataset, "in", "plug/gen"));
  EXPECT_TRUE(readPublished(store, kDataset, "in").markers.empty());
}

// clearGenerators(RestoreIntent::kHistory) reconciles only the non-exempt persistent
// generators a history restore has authority over, leaving a history_exempt one
// (and any ephemeral preview) untouched — clearAllGenerators is the same
// predicate always removing.
TEST(BatchEngineTest, PredicateClearRemovesOnlyNonExemptGenerators) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(4));

  auto plain = markerRecipe("plug/plain", "plain_out", "createMarker(0.0)\n");
  ASSERT_TRUE(service.upsertGenerator(plain).has_value());

  auto exempt = markerRecipe("assistant/exempt", "exempt_out", "createMarker(0.0)\n");
  exempt.history_exempt = true;
  ASSERT_TRUE(service.upsertGenerator(exempt).has_value());

  service.clearGenerators(PJ::RestoreIntent::kHistory);

  const auto recipes = service.recipes();
  ASSERT_EQ(recipes.size(), 1u);
  EXPECT_EQ(recipes.front().id, "assistant/exempt");
  EXPECT_TRUE(recipes.front().history_exempt);
}

TEST(BatchEngineTest, FindsExemptGeneratorDependingOnRemovedProcessorOutput) {
  PJ::ObjectStore store;
  BatchEngine service(store, [](PJ::DatasetId, const std::string&, const BatchEngine::Window&) {
    BatchEngine::ResolvedSeries series;
    series.timestamps = {0.0};
    series.values = {1.0};
    return std::optional<BatchEngine::ResolvedSeries>{std::move(series)};
  });

  auto plain = markerRecipe("user/plain", "plain_markers", "createMarker(0.0)\n");
  plain.inputs = {"user_output/value"};
  ASSERT_TRUE(service.upsertGenerator(plain).has_value());

  auto exempt = markerRecipe("assistant/exempt", "assistant_markers", "createMarker(0.0)\n");
  exempt.inputs = {"user_output/value"};
  exempt.history_exempt = true;
  ASSERT_TRUE(service.upsertGenerator(exempt).has_value());

  EXPECT_EQ(service.exemptDependentsOf({"user_output"}), std::vector<std::string>{"assistant_markers"});
  EXPECT_TRUE(service.exemptDependentsOf({"unrelated"}).empty());
  EXPECT_TRUE(service.exemptDependentsOf({"user"}).empty());
}

// N1: markers merge set-aware — the anchor's set plus each source's set shifted
// onto the anchor clock, republished as one blob, and the source topics dropped.
// (This is what markers opt out of the generic ObjectStore fold to do.)
TEST(BatchEngineTest, MergeMarkerTopicsConcatenatesAndShifts) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(10));

  // A single event marker on `dataset` at time `t`, on the "in" marker topic.
  const auto publish = [&store](PJ::DatasetId dataset, PJ::Timestamp t) {
    PJ::sdk::PlotMarkers set;
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kEvent;
    m.t_start = t;
    set.markers.push_back(m);
    const auto id = store.registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset, .topic_name = PJ::sdk::markerObjectTopicName("in"), .metadata_json = {}});
    ASSERT_TRUE(id.has_value());
    store.setRetentionBudget(*id, PJ::RetentionBudget{.max_entries = 1});
    store.pushOwned(*id, PJ::Timestamp{0}, PJ::serializePlotMarkers(set));
  };
  publish(/*anchor=*/1, /*t=*/100);
  publish(/*source=*/2, /*t=*/5);

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});

  const PJ::sdk::PlotMarkers merged = readPublished(store, 1, "in");
  ASSERT_EQ(merged.markers.size(), 2u);  // both datasets' markers survive as ONE set
  std::vector<PJ::Timestamp> times;
  for (const auto& m : merged.markers) {
    times.push_back(m.t_start);
  }
  EXPECT_NE(std::find(times.begin(), times.end(), 100), times.end());   // anchor unshifted
  EXPECT_NE(std::find(times.begin(), times.end(), 1005), times.end());  // source: 5 + 1000
  // The consumed source's marker topic is gone (folded into the anchor).
  EXPECT_FALSE(store.findTopic(2, PJ::sdk::markerObjectTopicName("in")).has_value());
}

// Ordering fix: the anchor's PRE-MERGE content (here a foreign blob, pushed with no
// owning generator) must be folded before any source part is moved onto the same
// target, or the moved-in source part would look like "already has parts" to the
// anchor step and get shifted a second time. The anchor's blob is unshifted; the
// source's cached part is shifted by raw_shift_ns exactly once.
TEST(BatchEngineTest, MergeAdoptsAnchorForeignBlobAndShiftsSourcesOnce) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  constexpr PJ::DatasetId kAnchor = 1;
  constexpr PJ::DatasetId kSource = 2;

  PJ::sdk::PlotMarkers anchor_blob;
  PJ::sdk::PlotMarker am;
  am.kind = PJ::sdk::MarkerKind::kEvent;
  am.t_start = 100;
  anchor_blob.markers.push_back(am);
  const auto anchor_id = store.registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = kAnchor, .topic_name = PJ::sdk::markerObjectTopicName("shared"), .metadata_json = {}});
  ASSERT_TRUE(anchor_id.has_value());
  store.setRetentionBudget(*anchor_id, PJ::RetentionBudget{.max_entries = 1});
  store.pushOwned(*anchor_id, PJ::Timestamp{0}, PJ::serializePlotMarkers(anchor_blob));

  auto source_recipe = markerRecipe("plug/src", "shared", "createMarker(5.0)\n");
  source_recipe.dataset_id = kSource;
  ASSERT_TRUE(service.upsertGenerator(source_recipe).has_value());

  service.mergeMarkerTopics(kAnchor, {PJ::DatasetMergeSource{.dataset_id = kSource, .raw_shift_ns = 1000}});

  const PJ::sdk::PlotMarkers merged = readPublished(store, kAnchor, "shared");
  ASSERT_EQ(merged.markers.size(), 2u);
  std::vector<PJ::Timestamp> times;
  for (const auto& m : merged.markers) {
    times.push_back(m.t_start);
  }
  EXPECT_NE(std::find(times.begin(), times.end(), 100), times.end()) << "the anchor's foreign blob, unshifted";
  EXPECT_NE(std::find(times.begin(), times.end(), 1005), times.end())
      << "the source's cached part, shifted exactly once";
}

// After a merge + remap, a re-run generator replaces only its OWN part of the
// shared target; a sibling generator's part (folded in by the merge, on the
// shifted clock) is left untouched.
TEST(BatchEngineTest, MergeThenRecomputeOneGeneratorKeepsOthers) {
  PJ::ObjectStore store;
  BatchEngine::SeriesResolver resolver = [](PJ::DatasetId, const std::string& key,
                                            const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in" && key != "other") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
  BatchEngine service(store, resolver);

  constexpr PJ::DatasetId kAnchor = 1;
  constexpr PJ::DatasetId kSource = 2;

  auto a = markerRecipe("plug/a", "shared", "createMarker(0.0)\n");
  a.dataset_id = kAnchor;
  a.inputs = {"in"};
  ASSERT_TRUE(service.upsertGenerator(a).has_value());

  auto b = markerRecipe("plug/b", "shared", "createMarker(5.0)\n");
  b.dataset_id = kSource;
  b.inputs = {"other"};
  ASSERT_TRUE(service.upsertGenerator(b).has_value());

  service.mergeMarkerTopics(kAnchor, {PJ::DatasetMergeSource{.dataset_id = kSource, .raw_shift_ns = 1000}});
  service.remapGeneratorsToAnchor(kAnchor, {kSource});
  EXPECT_EQ(markerFamily(store, kAnchor, "shared").size(), 2u) << "each owner's topic is carried through once";
  EXPECT_TRUE(markerFamily(store, kSource, "shared").empty());
  const std::optional<std::uint64_t> b_uid = ownerPublishUid(store, kAnchor, "shared", "plug/b");

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, kAnchor);
  ASSERT_EQ(affected.size(), 1u) << "only A ('in') is marked dirty";
  EXPECT_EQ(affected[0], "plug/a");
  service.refresh("plug/a");  // force the marked-stale run now (ignores brake/demand)
  EXPECT_EQ(ownerPublishUid(store, kAnchor, "shared", "plug/b"), b_uid) << "B's topic is not republished";

  const PJ::sdk::PlotMarkers merged = readPublished(store, kAnchor, "shared");
  ASSERT_EQ(merged.markers.size(), 2u);
  std::vector<PJ::Timestamp> times;
  for (const auto& m : merged.markers) {
    times.push_back(m.t_start);
  }
  EXPECT_NE(std::find(times.begin(), times.end(), 0), times.end()) << "A's re-run part";
  EXPECT_NE(std::find(times.begin(), times.end(), 1005), times.end()) << "B's part, shifted exactly once by the merge";
}

// A dataset-bound id is keyed by (id, dataset): the same id applied to a SECOND
// dataset is a new binding, not a replacement — dropParts must never touch the
// first dataset's part just because a same-id recipe landed elsewhere.
TEST(BatchEngineTest, DatasetRuleOnSecondDatasetKeepsTheFirst) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto on_first = markerRecipe("plug/r", "in", "createMarker(0.0)\n");
  on_first.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(on_first).has_value());

  auto on_second = markerRecipe("plug/r", "in", "createMarker(1.0)\n");
  on_second.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(on_second).has_value());

  EXPECT_EQ(service.recipes().size(), 2u);
  EXPECT_EQ(readPublished(store, 1, "in").markers.size(), 1u);
  EXPECT_EQ(readPublished(store, 2, "in").markers.size(), 1u);
}

// Re-applying the SAME id on the SAME dataset is the ordinary replace, unchanged.
TEST(BatchEngineTest, ReapplyOnSameDatasetReplaces) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto first = markerRecipe("plug/r", "in", "createMarker(0.0)\n");
  first.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(first).has_value());

  auto second = markerRecipe("plug/r", "in", "createMarker(0.0)\ncreateMarker(1.0)\n");
  second.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(second).has_value());

  EXPECT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(readPublished(store, 1, "in").markers.size(), 2u);
}

// removeGenerator retires an id on EVERY dataset it is bound to, not just one.
TEST(BatchEngineTest, RemoveRetiresTheIdOnEveryDataset) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto on_first = markerRecipe("plug/r", "in", "createMarker(0.0)\n");
  on_first.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(on_first).has_value());
  auto on_second = markerRecipe("plug/r", "in", "createMarker(1.0)\n");
  on_second.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(on_second).has_value());

  ASSERT_TRUE(service.removeGenerator("plug/r").has_value());
  EXPECT_TRUE(service.recipes().empty());

  for (const PJ::DatasetId dataset : std::vector<PJ::DatasetId>{1, 2}) {
    EXPECT_TRUE(ownerTombstoned(store, dataset, "in", "plug/r")) << "tombstone, not removed, on dataset " << dataset;
  }
}

// A merge collapses two same-id bindings (one per merged dataset) onto ONE recipe
// bound to the anchor: the anchor's OWN binding survives (its parts already carry
// the source's folded-in content under the shared owner id), and the consumed
// dataset's binding — now redundant — is dropped rather than colliding.
TEST(BatchEngineTest, MergeCollapsesSameIdRecipesOntoTheAnchor) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto on_anchor = markerRecipe("plug/r", "in", "createMarker(0.0)\n");
  on_anchor.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(on_anchor).has_value());
  auto on_source = markerRecipe("plug/r", "in", "createMarker(1.0)\n");
  on_source.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(on_source).has_value());

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});
  service.remapGeneratorsToAnchor(1, {2});

  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes().front().dataset_id, 1u);
  EXPECT_EQ(readPublished(store, 1, "in").markers.size(), 2u) << "the merge concatenated both onto the anchor";

  const std::vector<std::string> affected = service.recomputeForDataset(1);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(readPublished(store, 1, "in").markers.size(), 1u)
      << "the surviving recipe's own output replaces the merged-in part";
}

// A merge moves a consumed binding's topic onto the anchor and drops the binding
// (the anchor's own wins), so the surviving recipe no longer names that output:
// retiring the id must still find every topic it owns.
TEST(BatchEngineTest, RemoveAfterMergeRetiresEveryTopicTheIdOwns) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto on_anchor = markerRecipe("plug/r", "x", "createMarker(0.0)\n");
  on_anchor.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(on_anchor).has_value());
  auto on_source = markerRecipe("plug/r", "y", "createMarker(1.0)\n");
  on_source.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(on_source).has_value());

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}});
  service.remapGeneratorsToAnchor(1, {2});
  ASSERT_EQ(readPublished(store, 1, "y").markers.size(), 1u) << "moved onto the anchor";

  ASSERT_TRUE(service.removeGenerator("plug/r").has_value());
  EXPECT_TRUE(ownerTombstoned(store, 1, "x", "plug/r"));
  EXPECT_TRUE(ownerTombstoned(store, 1, "y", "plug/r")) << "the merged-in output is retired too";
}

// A Global-scope rule that reads the series "in": t = 0 on every dataset that has it.
BatchEngine::GeneratorRecipe globalRuleReadingIn() {
  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), R"(
    local s = series("in")
    createMarker(s:at(0).t)
  )");
  recipe.all_datasets = true;
  return recipe;
}

// Scope says where markers are DRAWN: an all_datasets rule reads from the datasets
// that have its series, and its set lives once on the shared dataset — the overlay
// draws that one topic on every plot, including one whose dataset lacks the series.
TEST(BatchEngineTest, GlobalRuleIsPublishedOnceOnTheSharedDataset) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(globalRuleReadingIn());
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);
  EXPECT_TRUE(markerFamily(store, 1, all_topic).empty()) << "no per-dataset copy on the contributor";
  EXPECT_TRUE(markerFamily(store, 2, all_topic).empty()) << "nor on the dataset without the series";
  EXPECT_EQ(service.recipes().size(), 1u);
}

// The set is the UNION of every contributing dataset's run.
TEST(BatchEngineTest, GlobalRuleUnionsEveryContributor) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1, 3}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  EXPECT_EQ(
      readPublished(store, kAllDatasetsMarkerDataset, std::string(PJ::kAllDatasetsMarkerTopic)).markers.size(), 2u);
}

// An all_datasets recipe whose series exists on NO loaded dataset has nothing to
// compute: rejected, nothing published, nothing stored.
TEST(BatchEngineTest, GlobalRuleWithNoInputsAnywhereIsRejected) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(globalRuleReadingIn());
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
  EXPECT_TRUE(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).empty());
}

// A dataset-bound recipe never skips: a missing input on ITS dataset is a plain
// user error.
TEST(BatchEngineTest, BoundRuleWithMissingInputStillErrors) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({}));

  auto recipe = markerRecipe("plug/g", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t)
  )");
  recipe.dataset_id = kDataset;
  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A marker per staged sample, at its time and value: what a run saw is what it drew.
BatchEngine::GeneratorRecipe rampMarkerRecipe(std::string id) {
  return markerRecipe(std::move(id), "in", R"(
    local s = series("in")
    for i = 0, s:size() - 1 do
      local p = s:at(i)
      createMarker(p.t, p.v)
    end
  )");
}

// GeneratorRecipe::window clips the staged series to [start, end] (raw ns,
// inclusive) before the rule runs, and coverageOf reports that staged range.
TEST(BatchEngineTest, WindowClipsTheStagedSeriesAndCoverageReportsIt) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(100));
  EXPECT_FALSE(service.coverageOf("plug/windowed").has_value()) << "never run yet";

  auto recipe = rampMarkerRecipe("plug/windowed");
  recipe.window = std::pair<PJ::Timestamp, PJ::Timestamp>{20, 40};
  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());

  const PJ::sdk::PlotMarkers set = readPublished(store, kDataset, "in");
  ASSERT_EQ(set.markers.size(), 21u) << "indices 20..40 inclusive";
  EXPECT_EQ(set.markers.front().t_start, 20);
  EXPECT_EQ(set.markers.back().t_start, 40);
  const auto coverage = service.coverageOf("plug/windowed");
  ASSERT_TRUE(coverage.has_value());
  EXPECT_EQ(coverage->first, 20);
  EXPECT_EQ(coverage->second, 40);
}

// A window with no samples inside it is not an error: the rule runs over an empty
// series (a script error stays reserved for a genuinely absent/nil input) and
// publishes an empty set. It staged nothing, so coverageOf stays absent, distinct
// from a window whose staged range happens to equal its own bounds.
TEST(BatchEngineTest, EmptyWindowPublishesAnEmptySetAndNoCoverage) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(100));

  auto recipe = rampMarkerRecipe("plug/empty_window");
  recipe.window = std::pair<PJ::Timestamp, PJ::Timestamp>{200, 300};
  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());

  EXPECT_TRUE(readPublished(store, kDataset, "in").markers.empty());
  EXPECT_FALSE(service.coverageOf("plug/empty_window").has_value());
}

// stageRun asks the resolver for the recipe's window, so the production resolver
// copies only that range; a resolver that ignores it is still clipped (above).
TEST(BatchEngineTest, TheResolverIsAskedForTheRecipeWindow) {
  PJ::ObjectStore store;
  std::vector<BatchEngine::Window> asked;
  BatchEngine service(
      store,
      [&asked, ramp = rampResolver(100)](
          PJ::DatasetId dataset, const std::string& key,
          const BatchEngine::Window& window) -> std::optional<BatchEngine::ResolvedSeries> {
        asked.push_back(window);
        return ramp(dataset, key, window);
      });
  auto recipe = rampMarkerRecipe("plug/asked");
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  recipe.window = std::pair<PJ::Timestamp, PJ::Timestamp>{20, 40};
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  EXPECT_EQ(asked, (std::vector<BatchEngine::Window>{std::nullopt, recipe.window}));
}

// Without any recipe.window, coverageOf still tracks what the resolver actually
// returned on the most recent run — standing in for a streaming input whose
// retention trimmed its head between two runs; no separate retention query needed
// (see StagedRun::coverage_ns's doc-comment).
TEST(BatchEngineTest, CoverageOfTracksWhatTheResolverActuallyReturnedAcrossRuns) {
  PJ::ObjectStore store;
  bool trimmed = false;
  BatchEngine::SeriesResolver resolver = [&trimmed](
                                             PJ::DatasetId, const std::string& key,
                                             const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    const int start = trimmed ? 50 : 0;
    for (int i = start; i < 100; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
  BatchEngine service(store, resolver);

  ASSERT_TRUE(service.upsertGenerator(rampMarkerRecipe("plug/shrinking_cov")).has_value());
  {
    const auto coverage = service.coverageOf("plug/shrinking_cov");
    ASSERT_TRUE(coverage.has_value());
    EXPECT_EQ(coverage->first, 0);
    EXPECT_EQ(coverage->second, 99);
  }

  trimmed = true;
  service.refresh("plug/shrinking_cov");  // no pool installed: runs synchronously

  const auto coverage = service.coverageOf("plug/shrinking_cov");
  ASSERT_TRUE(coverage.has_value());
  EXPECT_EQ(coverage->first, 50);
  EXPECT_EQ(coverage->second, 99);
}

// A dataset loaded AFTER an all_datasets recipe was applied, without the series the
// rule reads, needs no work at all: the set lives once on the shared dataset, so an
// unrelated recompute republishes nothing and the roster hook re-runs the rule once
// (it cannot know the newcomer has no inputs without running) with no duplication.
TEST(BatchEngineTest, ADatasetLoadedLaterWithoutTheInputsNeedsNoCopy) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  const std::optional<std::uint64_t> before = ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g");
  ASSERT_TRUE(before.has_value());

  // Dataset 2 loads after the rule was applied, without the series the rule reads.
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  EXPECT_TRUE(service.recomputeForChangedInputs({"/unrelated"}, BatchEngine::kAnyDataset).empty());
  EXPECT_EQ(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"), before) << "nothing to copy";
  EXPECT_TRUE(markerFamily(store, 2, all_topic).empty());

  service.reachNewDatasets();
  EXPECT_TRUE(service.isStale("plug/g")) << "the catalog grew: the rule is re-run";
  pumpUntil([&] { return !service.isStale("plug/g"); });
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u) << "no duplication";
}

// A Global set is stored in the DISPLAY frame (raw − the contributor's alignment
// offset), so a marker found on a dataset with another clock sits at the same
// display instant as one found on an aligned dataset, with no per-dataset copy.
TEST(BatchEngineTest, GlobalRuleIsStoredInTheDisplayFrame) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  // display = raw − offset: dataset 1's clock runs 10 ns ahead of the display frame.
  service.setDisplayOffsetResolver([](PJ::DatasetId dataset) { return dataset == 1 ? PJ::Timestamp{10} : 0; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());  // marker at raw 0 on dataset 1

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  const PJ::sdk::PlotMarkers shared = readPublished(store, kAllDatasetsMarkerDataset, all_topic);
  ASSERT_EQ(shared.markers.size(), 1u);
  EXPECT_EQ(shared.markers.front().t_start, -10) << "raw 0 on dataset 1 = display -10";

  // A later contributor on yet another clock lands in the same frame.
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });
  service.setResolver(resolverOn({1, 3}));
  service.setDisplayOffsetResolver([](PJ::DatasetId dataset) {
    return dataset == 1 ? PJ::Timestamp{10} : dataset == 3 ? PJ::Timestamp{-5} : 0;
  });
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, /*scope=*/3).empty());
  service.refresh("plug/g");  // force the marked-stale run now (ignores brake/demand)
  std::vector<PJ::Timestamp> times;
  for (const auto& marker : readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers) {
    times.push_back(marker.t_start);
  }
  std::sort(times.begin(), times.end());
  EXPECT_EQ(times, (std::vector<PJ::Timestamp>{-10, 5}));
}

// A dataset loaded later WITH the series is a new contributor: its topics arrive in a
// `changed` batch, the union is recomputed and grows on every dataset.
TEST(BatchEngineTest, GlobalRuleGrowsWhenAContributorLoadsLater) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });
  service.setResolver(resolverOn({1, 3}));

  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, /*scope=*/3).empty());
  service.refresh("plug/g");  // force the marked-stale run now (ignores brake/demand)
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// With declared (qualified) names, the script enumerates each series once under
// the spelling it declared, and each name reads its own series — even when a
// declared name equals another input's bare key ("s:x" is both).
TEST(BatchEngineTest, DeclaredNamesAreEnumeratedOnceEachAndReadTheirOwnSeries) {
  PJ::ObjectStore store;
  BatchEngine service(
      store,
      [](PJ::DatasetId, const std::string& key,
         const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "x" && key != "s:x") {
          return std::nullopt;
        }
        BatchEngine::ResolvedSeries s;
        s.timestamps = {0.0};
        s.values = {key == "x" ? 1.0 : 2.0};
        return s;
      });
  auto recipe = markerRecipe("plug/gen", "out", R"(
    local names = GetSeriesNames()
    assert(#names == 2, "expected 2 names, got " .. #names)
    table.sort(names)
    assert(names[1] == "s:s:x", "names[1] is " .. names[1])
    assert(names[2] == "s:x", "names[2] is " .. names[2])
    assert(series("s:x"):at(0).v == 1, "series('s:x') is not x")
    assert(series("s:s:x"):at(0).v == 2, "series('s:s:x') is not s:x")
    createMarker(0.0)
  )");
  recipe.inputs = {"x", "s:x"};
  recipe.declared_inputs = {"s:x", "s:s:x"};
  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << ok.error();
}

// `declared_inputs` is parallel to `inputs`; a mismatched length is refused at the
// service boundary (the transform path guards its binding count the same way).
TEST(BatchEngineTest, UpsertRejectsADeclaredInputCountMismatch) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(3));
  auto recipe = markerRecipe("plug/gen", "out", "createMarker(0.0)\n");
  recipe.declared_inputs = {"in", "extra"};
  const PJ::Expected<std::vector<std::string>> rejected = service.upsertGenerator(recipe);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().find("declared"), std::string::npos) << rejected.error();
  EXPECT_TRUE(service.recipes().empty());
}

// A one-column chunk of consecutive rows t_start, t_start+step, ...; the value is the
// row's timestamp + 0.5 (distinct from the timestamp, so a swapped column is caught),
// and rows listed in `null_rows` carry a null value.
PJ::TopicChunk makeSeriesChunk(
    PJ::Timestamp t_start, std::uint32_t rows, PJ::Timestamp step, const std::set<std::uint32_t>& null_rows = {}) {
  PJ::TopicChunkBuilder builder(1, 1, {{0, PJ::PrimitiveType::kFloat64, "value"}}, rows);
  for (std::uint32_t row = 0; row < rows; ++row) {
    const PJ::Timestamp timestamp = t_start + static_cast<PJ::Timestamp>(row) * step;
    builder.beginRow(timestamp);
    if (null_rows.contains(row)) {
      builder.setNull(0);
    } else {
      builder.set(0, static_cast<double>(timestamp) + 0.5);
    }
    builder.finishRow();
  }
  return builder.seal();
}

TEST(ReadResolvedSeriesTest, CopiesMultiChunkSeriesSkippingNullsAndSubFloorRows) {
  // Chunk order is deliberately reversed to pin time-ordered output, not chunk order.
  std::deque<PJ::TopicChunk> chunks;
  chunks.push_back(makeSeriesChunk(200, 10, 10));       // t = 200..290
  chunks.push_back(makeSeriesChunk(100, 10, 10, {0}));  // t = 100..190, null at t=100
  chunks.push_back(makeSeriesChunk(0, 10, 10, {3}));    // t = 0..90, null at t=30
  const PJ::SeriesReader reader(chunks, /*column_index=*/0, /*retention_floor=*/50);

  const BatchEngine::ResolvedSeries out = PJ::readResolvedSeries(reader);

  std::vector<double> expected_timestamps;
  std::vector<double> expected_values;
  for (PJ::Timestamp timestamp = 50; timestamp < 300; timestamp += 10) {
    if (timestamp != 100) {
      expected_timestamps.push_back(static_cast<double>(timestamp));
      expected_values.push_back(static_cast<double>(timestamp) + 0.5);
    }
  }
  EXPECT_EQ(out.timestamps, expected_timestamps);
  EXPECT_EQ(out.values, expected_values);
}

TEST(ReadResolvedSeriesTest, CopiesAHugeSeriesInLinearTime) {
  constexpr std::uint32_t kChunks = 5;
  constexpr std::uint32_t kRowsPerChunk = 10'000;
  std::deque<PJ::TopicChunk> chunks;
  for (std::uint32_t chunk = 0; chunk < kChunks; ++chunk) {
    chunks.push_back(makeSeriesChunk(static_cast<PJ::Timestamp>(chunk) * kRowsPerChunk, kRowsPerChunk, 1));
  }
  const PJ::SeriesReader reader(chunks, /*column_index=*/0);

  const auto start = std::chrono::steady_clock::now();
  const BatchEngine::ResolvedSeries out = PJ::readResolvedSeries(reader);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  ASSERT_EQ(out.timestamps.size(), std::size_t{kChunks} * kRowsPerChunk);
  EXPECT_DOUBLE_EQ(out.timestamps.back(), static_cast<double>(kChunks * kRowsPerChunk - 1));
  EXPECT_LT(elapsed.count(), 500) << "copy took " << elapsed.count() << " ms";
}

}  // namespace

// history_exempt and ephemeral are per-id properties (clearGenerators, removeGenerator
// and generatorIds all read the first binding), so a sibling binding that disagrees
// would let an undo delete an exempt rule, or a replay skip a plain one.
TEST(BatchEngineTest, UpsertRejectsABindingWhoseFlagsDifferFromItsSiblings) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  auto plain = markerRecipe("plug/rule", "in", "createMarker(0.0)\n");  // bound to dataset 1
  ASSERT_TRUE(service.upsertGenerator(plain).has_value());

  auto exempt = plain;
  exempt.dataset_id = 2;
  exempt.history_exempt = true;
  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(exempt);
  ASSERT_FALSE(res.has_value()) << "a history_exempt binding joined a non-exempt id";
  EXPECT_NE(res.error().find("plug/rule"), std::string::npos) << res.error();
  EXPECT_TRUE(markerFamily(store, 2, "in").empty()) << "rejected before publishing";
  EXPECT_EQ(service.recipes().size(), 1u);

  auto preview = plain;
  preview.ephemeral = true;
  preview.outputs.clear();
  EXPECT_FALSE(service.upsertGenerator(preview).has_value()) << "an ephemeral binding joined a persistent id";
  EXPECT_EQ(service.recipes().size(), 1u);

  service.clearGenerators(PJ::RestoreIntent::kHistory);
  EXPECT_TRUE(service.recipes().empty()) << "undo authority covers the whole (non-exempt) id";
}

// A change of scope is a change of rule: the id is replaced whole (old bindings and
// their outputs retired), never left owning a Dataset-scope and a Global output at once.
TEST(BatchEngineTest, ChangingScopeReplacesTheWholeId) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1, 2}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  const std::string dataset_topic(PJ::sdk::kGlobalMarkerTopic);
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);

  auto bound = markerRecipe("plug/rule", dataset_topic, "createMarker(0.0)\n");  // bound to dataset 1
  ASSERT_TRUE(service.upsertGenerator(bound).has_value());
  ASSERT_EQ(readPublished(store, 1, dataset_topic).markers.size(), 1u);

  auto global = bound;
  global.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(global).has_value());
  ASSERT_EQ(service.recipes().size(), 1u) << "Dataset -> Global replaces the binding";
  EXPECT_TRUE(service.recipes().front().all_datasets);
  EXPECT_TRUE(readPublished(store, 1, dataset_topic).markers.empty()) << "the Dataset-scope output is retired";
  EXPECT_TRUE(ownerTombstoned(store, 1, dataset_topic, "plug/rule"));
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);

  ASSERT_TRUE(service.upsertGenerator(bound).has_value());
  ASSERT_EQ(service.recipes().size(), 1u) << "Global -> Dataset replaces the binding";
  EXPECT_FALSE(service.recipes().front().all_datasets);
  EXPECT_EQ(readPublished(store, 1, dataset_topic).markers.size(), 1u);
  EXPECT_TRUE(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.empty())
      << "the Global output is retired";
  EXPECT_TRUE(ownerTombstoned(store, kAllDatasetsMarkerDataset, all_topic, "plug/rule"));
}

// Removing a contributing dataset shrinks the shared set on the survivors — the
// removal is the only event that will ever prompt that recompute.
TEST(BatchEngineTest, RemovingAContributorShrinksTheGlobalSetOnSurvivors) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1, 2}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);

  // Dataset 1 is removed: gone from the lister, its series gone from the resolver.
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{2}; });
  service.setResolver(resolverOn({2}));
  EXPECT_EQ(service.clearGeneratorsForDataset(1).size(), 1u) << "the shared set is republished";

  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u)
      << "dataset 1's contribution is gone";
}

// With no contributor left there is nothing to draw: the rule's parts are cleared on
// every dataset (the rule itself survives — a contributor loaded later revives it).
TEST(BatchEngineTest, RemovingTheLastContributorClearsTheGlobalSetEverywhere) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{2}; });
  service.setResolver(resolverOn({}));
  EXPECT_EQ(service.clearGeneratorsForDataset(1).size(), 1u) << "the emptied set is announced";

  EXPECT_TRUE(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.empty())
      << "no contributor left: nothing to draw";
  EXPECT_TRUE(ownerTombstoned(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"));
  ASSERT_EQ(service.recipes().size(), 1u) << "the session-wide rule itself survives";
}

// A Global set lives on the shared dataset, which no merge ever folds: the merge
// leaves it alone — neither concatenated with itself nor republished.
TEST(BatchEngineTest, MergeLeavesTheSharedGlobalSetAlone) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);
  const std::optional<std::uint64_t> before = ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g");

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 0}});

  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);
  EXPECT_EQ(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"), before) << "untouched";
  EXPECT_TRUE(markerFamily(store, 1, all_topic).empty());
  EXPECT_TRUE(markerFamily(store, 2, all_topic).empty());
}

// The hook a dataset's catalog publication fires: a newly listed dataset may be a
// new contributor, so every Global rule is re-run over the grown roster — once.
TEST(BatchEngineTest, ReachNewDatasetsRerunsGlobalRulesForANewContributor) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1, 2}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  service.reachNewDatasets();
  pumpUntil([&] { return !service.isStale("plug/g"); });
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// The catalog grows topic by topic: a dataset already listed may only now expose the
// series a Global rule reads, so the hook re-runs on every catalog addition rather
// than on a dataset-roster change.
TEST(BatchEngineTest, ReachNewDatasetsRerunsWhenAContributorAppearsUnderTheSameRoster) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setResolver(resolverOn({1, 2}));  // dataset 2's series just reached the catalog
  service.reachNewDatasets();
  pumpUntil([&] { return !service.isStale("plug/g"); });
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// The catalog grows on every progressive-load flush and every new streaming topic,
// so the roster hook only marks the Global rules stale: the brake coalesces a burst
// of additions into one run, and nothing runs inside the hook.
TEST(BatchEngineTest, ReachNewDatasetsLeavesTheRunToTheBrake) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1, 2}));
  service.setReloadHz(20.0);  // 50 ms period
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  int runs = 0;
  QObject::connect(&service, &BatchEngine::markersPublished, [&runs] { ++runs; });

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  for (int addition = 0; addition < 3; ++addition) {
    (void)service.reachNewDatasets();
  }
  EXPECT_TRUE(service.isStale("plug/g")) << "marked, not run";
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u) << "nothing ran yet";

  pumpUntil([&] { return runs > 0; });
  pumpFor(100);
  EXPECT_FALSE(service.isStale("plug/g"));
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
  EXPECT_EQ(runs, 1) << "three additions, one run";
}

// A recompute that finds no contributor left for an existing Global rule (the last
// one was replaced by data without its inputs) empties the set instead of leaving
// the stale findings on screen; only the initial upsert treats it as an error.
TEST(BatchEngineTest, RecomputeWithNoContributorLeftClearsTheSharedSet) {
  PJ::ObjectStore store;
  BatchEngine service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setResolver(resolverOn({}));  // dataset 1 was replaced by a file without "in"
  EXPECT_EQ(service.recomputeForDataset(1).size(), 1u) << "the emptied set is announced";
  EXPECT_TRUE(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.empty());
  EXPECT_TRUE(ownerTombstoned(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"));
  EXPECT_EQ(service.recipes().size(), 1u) << "the rule stays live";
}

// The owner separator is reserved: an id or output key carrying it would make two
// different (key, owner) pairs spell the same object topic.
TEST(BatchEngineTest, UpsertRejectsTheOwnerSeparatorInIdsAndOutputs) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));

  EXPECT_FALSE(service.upsertGenerator(markerRecipe("plug/a#b", "in", "createMarker(0.0)\n")).has_value());
  EXPECT_FALSE(service.upsertGenerator(markerRecipe("plug/a", "in#x", "createMarker(0.0)\n")).has_value());
  EXPECT_TRUE(service.recipes().empty());
  EXPECT_TRUE(store.listTopics().empty()) << "rejected before publishing";
}

// The session owns the pool its batch engine submits to: tearing both down with a
// generator installed must not touch a destroyed pool or engine.
TEST(BatchEngineTest, SessionDestructionDisconnectsServicesBeforeThePool) {
  auto session = std::make_unique<PJ::SessionManager>();
  session->batchEngine().setResolver(rampResolver(5));
  ASSERT_TRUE(
      session->batchEngine().upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  session.reset();
}

// Production ordering: a dataset's commit recompute runs BEFORE the catalog (the
// dataset lister and the series resolver) publishes it, so no recompute ever sees
// the new dataset — the catalog publication itself must re-run every Global rule.
TEST(BatchEngineTest, ANewlyCatalogedContributorJoinsTheGlobalSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  BatchEngine& service = session.batchEngine();
  service.setDatasetLister([&catalog] { return catalog.datasetIds(); });

  const auto commit_scalar = [&session](PJ::DatasetId dataset, std::string_view topic) {
    PJ::DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, topic, PJ::NumericType::kFloat64);
    ASSERT_TRUE(handle.has_value()) << handle.error();
    writer.appendScalar(*handle, 100, 1.0);
    ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  };
  const auto first = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(first.has_value());
  commit_scalar(*first, "/imu/x");
  const auto second = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "b.mcap"});
  ASSERT_TRUE(second.has_value());
  service.setResolver(resolverOn({*first, *second}));  // both carry the rule's series
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u)
      << "only the cataloged dataset contributes";

  int repaints = 0;
  QObject::connect(&session, &PJ::SessionManager::markersChanged, [&repaints] { ++repaints; });
  commit_scalar(*second, "/imu/x");  // the second dataset reaches the catalog
  pumpUntil([&] { return repaints > 0; });

  EXPECT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u)
      << "the newcomer's contribution joins the shared set";
  EXPECT_GE(repaints, 1) << "overlays are told to repaint";
}

// A refill publishes markers for the data it attempted; a rollback restores the
// dataset but the shared set lives outside it, so the rollback must re-run the
// generators over the restored data.
TEST(BatchEngineTest, RefillRollbackRecomputesTheSharedSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  BatchEngine& service = session.batchEngine();
  service.setDatasetLister([&catalog] { return catalog.datasetIds(); });

  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap"});
  ASSERT_TRUE(dataset.has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/imu/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 100, 1.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  service.setResolver(resolverOn({*dataset}));
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  ASSERT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.front().t_start, 0);

  {
    PJ::RefillGuard guard = session.beginRefill(*dataset);
    // The attempted refill's series starts at t = 5 where the original started at 0.
    service.setResolver(
        [](PJ::DatasetId, const std::string& key,
           const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
          if (key != "in") {
            return std::nullopt;
          }
          BatchEngine::ResolvedSeries series;
          series.timestamps = {5.0};
          series.values = {0.0};
          return series;
        });
    ASSERT_FALSE(service.recomputeForDataset(*dataset).empty());  // what the refill's recompute does
    ASSERT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.front().t_start, 5);
    service.setResolver(resolverOn({*dataset}));  // what the restored data resolves to
    // guard dtor (no commit) rolls back here.
  }

  const PJ::sdk::PlotMarkers restored = readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic);
  ASSERT_EQ(restored.markers.size(), 1u);
  EXPECT_EQ(restored.markers.front().t_start, 0) << "re-run over the restored data";
}

// A Source Timeline drag moves a dataset's display offset. A Global set is stored in
// the display frame and read there, so the drag republishes nothing: no serialize,
// no markersChanged, the shared topic keeps its entry.
TEST(BatchEngineTest, ASourceTimelineDragRepublishesNoMarkerSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  BatchEngine& service = session.batchEngine();
  service.setDatasetLister([&catalog] { return catalog.datasetIds(); });

  const auto domain = session.dataEngine().createTimeDomain("a");
  ASSERT_TRUE(domain.has_value());
  const auto first =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "a.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(first.has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*first, "/imu/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 100, 1.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  service.setResolver(resolverOn({*first}));
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  const std::optional<std::uint64_t> before =
      ownerPublishUid(session.objectStore(), kAllDatasetsMarkerDataset, all_topic, "plug/g");
  ASSERT_TRUE(before.has_value());

  int repaints = 0;
  QObject::connect(&session, &PJ::SessionManager::markersChanged, [&repaints] { ++repaints; });
  int drags = 0;
  QObject::connect(
      &session, qOverload<PJ::DatasetId>(&PJ::SessionManager::displayOffsetChanged),
      [&drags](PJ::DatasetId) { ++drags; });
  session.setDisplayOffset(*first, PJ::DisplayOffset{PJ::Duration{10}});

  ASSERT_EQ(drags, 1) << "the drag applied";
  EXPECT_EQ(repaints, 0) << "a drag is not a marker change";
  EXPECT_EQ(ownerPublishUid(session.objectStore(), kAllDatasetsMarkerDataset, all_topic, "plug/g"), before);
}

// ---- scheduled runs: staging, the brake, modes, demand, D30 ------------------

namespace {

// A resolver over one mutable value, so a test can simulate successive "changes"
// by writing `value` before calling recomputeForChangedInputs. The script under
// test reads it back via `createMarker(s:at(0).v, s:at(0).v)`, so the published
// marker's t_start IS the value the run was staged against.
BatchEngine::SeriesResolver mutableValueResolver(std::atomic<int>& value) {
  return [&value](
             PJ::DatasetId, const std::string& key,
             const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    BatchEngine::ResolvedSeries s;
    s.timestamps = {0.0};
    s.values = {static_cast<double>(value.load())};
    return s;
  };
}

constexpr const char* kMutableValueScript = "local s = series(\"in\")\ncreateMarker(s:at(0).v, s:at(0).v)\n";

/// What the budget charges for a staged one-sample series: its timestamp and its value.
constexpr std::size_t kSampleBytes = 2 * sizeof(double);
/// Budget room past one marker run's ceiling: enough for a small copy, never for a second run.
constexpr std::size_t kOneRunSlack = 1024;

}  // namespace

// A bounded create computes its first run on the pool while the GUI thread waits for
// it, up to the create budget: a fast rule's markers are drawn when the create returns.
TEST(BatchEngineTest, AFastCreateIsVisibleWhenItReturns) {
  const ScopedCreateBudget budget(PJ::test::kGenerousCreateBudget);
  std::atomic<QThread*> compute_thread{nullptr};
  const ScopedComputeProbe probe([&compute_thread](QThread* thread) { compute_thread = thread; });
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  PJ::EnginePool pool;
  service.setPool(&pool);

  ASSERT_TRUE(
      service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n"), BatchEngine::CreateWait::kBounded)
          .has_value());

  EXPECT_FALSE(readPublished(store, kDataset, "in").markers.empty()) << "published before the create returned";
  ASSERT_NE(compute_thread.load(), nullptr);
  EXPECT_NE(compute_thread.load(), QThread::currentThread()) << "computed on the pool; the GUI thread only waited";
}

// A first run that fails within the budget fails the create transactionally: the
// previous revision of the rule and its markers stay, and nothing else is published.
TEST(BatchEngineTest, AFirstRunErrorFailsTheCreateAndKeepsThePreviousRule) {
  const ScopedCreateBudget budget(PJ::test::kGenerousCreateBudget);
  std::atomic<QThread*> compute_thread{nullptr};
  const ScopedComputeProbe probe([&compute_thread](QThread* thread) { compute_thread = thread; });
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  PJ::EnginePool pool;
  service.setPool(&pool);
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0, 1, {label='v1'})\n")));
  compute_thread = nullptr;

  const PJ::Expected<std::vector<std::string>> failed = service.upsertGenerator(
      markerRecipe("plug/gen", "in", "local s = series(\"in\")\nerror(\"bad sample \" .. s:at(0).v)\n"),
      BatchEngine::CreateWait::kBounded);
  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("bad sample"), std::string::npos) << failed.error();
  ASSERT_NE(compute_thread.load(), nullptr);
  EXPECT_NE(compute_thread.load(), QThread::currentThread()) << "the failing run ran on the pool";

  pool.waitIdle();  // the failed run's pool publish must not land either
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_NE(service.recipes().front().script.find("v1"), std::string::npos) << "the previous revision stays";
  const PJ::sdk::PlotMarkers drawn = readPublished(store, kDataset, "in");
  ASSERT_EQ(drawn.markers.size(), 1u);
  EXPECT_EQ(drawn.markers.front().label, "v1");
  EXPECT_FALSE(service.isStale("plug/gen"));
}

// A first run that outlives the create budget does not hold the GUI thread: the
// create returns once the budget is spent with the rule installed, and the run
// publishes through the pool's drain.
TEST(BatchEngineTest, CreateReturnsBeforeASlowFirstRunWhichPublishesLater) {
  const ScopedCreateBudget budget(BatchEngine::kCreateBudget);
  std::atomic<bool> released{false};
  const ScopedComputeProbe probe([&released](QThread*) {
    for (int waited_ms = 0; !released.load() && waited_ms < 2000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  PJ::EnginePool pool;
  service.setPool(&pool);
  int announcements = 0;
  QObject::connect(&service, &BatchEngine::markersPublished, [&announcements] { ++announcements; });

  ASSERT_TRUE(
      service.upsertGenerator(markerRecipe("plug/slow", "in", "createMarker(0.0)\n"), BatchEngine::CreateWait::kBounded)
          .has_value());
  EXPECT_TRUE(readPublished(store, kDataset, "in").markers.empty()) << "the first run is still computing";
  EXPECT_EQ(service.recipes().size(), 1u) << "the rule is installed";

  released = true;
  pumpUntil([&announcements] { return announcements > 0; });
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.size(), 1u) << "published by the drain";
  EXPECT_FALSE(service.isStale("plug/slow"));
}

// A first run that fails after the budget is not a create error: it is reported
// like any scheduled run's failure, through errorOf.
TEST(BatchEngineTest, AFirstRunFailingAfterTheBudgetIsReportedNotReturned) {
  const ScopedCreateBudget budget(BatchEngine::kCreateBudget);
  std::atomic<bool> released{false};
  const ScopedComputeProbe probe([&released](QThread*) {
    for (int waited_ms = 0; !released.load() && waited_ms < 2000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  PJ::EnginePool pool;
  service.setPool(&pool);
  std::vector<PJ::Diagnostic> reported;
  service.setDiagnosticSink([&reported](const PJ::Diagnostic& diagnostic) { reported.push_back(diagnostic); });

  const PJ::Expected<std::vector<std::string>> created = service.upsertGenerator(
      markerRecipe("plug/late", "in", "error(\"late failure\")\n"), BatchEngine::CreateWait::kBounded);
  ASSERT_TRUE(created.has_value()) << "a failure after the budget is not a create error";
  released = true;
  pumpUntil([&service] { return !service.errorOf("plug/late").empty(); });
  EXPECT_NE(service.errorOf("plug/late").find("late failure"), std::string::npos) << service.errorOf("plug/late");
  EXPECT_TRUE(service.isStale("plug/late"));

  // The bell names the rule and the next step, once per distinct error.
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported.front().level, PJ::DiagnosticLevel::kError);
  EXPECT_NE(reported.front().message.find("Marker rule 'plug/late' failed: "), std::string::npos)
      << reported.front().message;
  EXPECT_NE(reported.front().message.find("late failure"), std::string::npos) << reported.front().message;
  EXPECT_NE(
      reported.front().message.find("Any previous markers are kept; fix the rule and apply it again."),
      std::string::npos)
      << reported.front().message;
  service.refresh("plug/late");
  pool.waitIdle();
  EXPECT_EQ(reported.size(), 1u) << "the same error again is not reported twice";
}

// The running state follows each scheduled run: set when it is submitted, cleared
// when it publishes, is cancelled or fails, and announced through runningChanged.
TEST(BatchEngineTest, RunningStateFollowsEachRun) {
  std::atomic<bool> released{true};
  const ScopedComputeProbe probe([&released](QThread*) {
    for (int waited_ms = 0; !released.load() && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  const std::string failing_script =
      "local s = series(\"in\")\nif s:at(0).v >= 5 then error(\"value too big\") end\ncreateMarker(0.0)\n";
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", failing_script)).has_value());
  EXPECT_FALSE(service.isRunning("plug/gen")) << "the create's run finished within its budget";
  int announced = 0;
  QObject::connect(&service, &BatchEngine::runningChanged, [&announced] { ++announced; });

  released = false;
  service.refresh("plug/gen");
  EXPECT_TRUE(service.isRunning("plug/gen"));
  EXPECT_EQ(service.runningIds(), std::vector<std::string>{"plug/gen"});
  released = true;
  pool.waitIdle();
  EXPECT_FALSE(service.isRunning("plug/gen")) << "published";
  EXPECT_TRUE(service.runningIds().empty());

  released = false;
  value = 5;
  service.refresh("plug/gen");
  EXPECT_TRUE(service.isRunning("plug/gen"));
  released = true;
  pool.waitIdle();
  EXPECT_FALSE(service.isRunning("plug/gen")) << "failed";
  EXPECT_FALSE(service.errorOf("plug/gen").empty());

  released = false;
  service.refresh("plug/gen");
  EXPECT_TRUE(service.isRunning("plug/gen"));
  ASSERT_TRUE(service.removeGenerator("plug/gen").has_value());
  EXPECT_FALSE(service.isRunning("plug/gen")) << "cancelled";
  released = true;
  pool.waitIdle();
  pumpFor(20);
  EXPECT_GE(announced, 3) << "every transition is announced";
}

constexpr const char* kFailsFromFiveScript =
    "local s = series(\"in\")\nif s:at(0).v >= 5 then error(\"value too big\") end\ncreateMarker(0.0)\n";

// A rule applied to two datasets has one run state per binding: a refresh of one dataset runs, and a
// failure lands on that binding only, while the id-level queries answer for either.
TEST(BatchEngineTest, RunningAndErrorStateAreKeptPerBinding) {
  std::atomic<bool> released{true};
  const ScopedComputeProbe probe([&released](QThread*) {
    for (int waited_ms = 0; !released.load() && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  BatchEngine service(
      store,
      [&value](PJ::DatasetId dataset, const std::string& key, const BatchEngine::Window&)
          -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "in") {
          return std::nullopt;
        }
        return BatchEngine::ResolvedSeries{{0.0}, {dataset == kDataset ? static_cast<double>(value.load()) : 1.0}};
      });
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  BatchEngine::GeneratorRecipe on_a = markerRecipe("plug/gen", "in", kFailsFromFiveScript);
  BatchEngine::GeneratorRecipe on_b = on_a;
  on_b.dataset_id = kDataset + 1;
  ASSERT_TRUE(service.upsertGenerator(on_a).has_value());
  ASSERT_TRUE(service.upsertGenerator(on_b).has_value());

  released = false;
  value = 5;
  service.refreshAll(kDataset);
  EXPECT_TRUE(service.isBindingRunning(on_a));
  EXPECT_FALSE(service.isBindingRunning(on_b)) << "only a's dataset was refreshed";
  EXPECT_TRUE(service.isRunning("plug/gen")) << "the id-level query answers for any binding";
  released = true;
  pool.waitIdle();
  pumpFor(20);
  EXPECT_NE(service.bindingError(on_a).find("value too big"), std::string::npos) << service.bindingError(on_a);
  EXPECT_TRUE(service.bindingError(on_b).empty()) << "a's failure is not b's";
  EXPECT_FALSE(service.isBindingRunning(on_a));
}

// Every change of the errors is announced, so a plot showing a failure hears that it is gone: the failure
// itself (also on the synchronous reload path), removing an idle failed rule, and applying it again.
TEST(BatchEngineTest, ErrorChangesAreAnnounced) {
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  service.setMode(BatchEngine::Mode::kManual);
  int announced = 0;
  QObject::connect(&service, &BatchEngine::runningChanged, [&announced] { ++announced; });
  const BatchEngine::GeneratorRecipe recipe = markerRecipe("plug/gen", "in", kFailsFromFiveScript);
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  pumpFor(20);

  announced = 0;
  value = 5;
  EXPECT_TRUE(service.recomputeForDataset(kDataset).empty());
  ASSERT_FALSE(service.errorOf("plug/gen").empty());
  pumpFor(20);
  EXPECT_GT(announced, 0) << "a failure on the synchronous reload path";

  announced = 0;
  value = 1;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  EXPECT_TRUE(service.errorOf("plug/gen").empty());
  pumpFor(20);
  EXPECT_GT(announced, 0) << "applying the rule again clears its error";

  value = 5;
  EXPECT_TRUE(service.recomputeForDataset(kDataset).empty());
  ASSERT_FALSE(service.errorOf("plug/gen").empty());
  pumpFor(20);
  announced = 0;
  ASSERT_TRUE(service.removeGenerator("plug/gen").has_value());
  pumpFor(20);
  EXPECT_GT(announced, 0) << "removing an idle failed rule";
}

// 3 changes inside one brake period coalesce into exactly 1 run, and that run
// sees the LAST change (not an intermediate one).
TEST(BatchEngineTest, ChangeMarksStaleAndTheBrakeCoalescesRuns) {
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  std::atomic<int> resolve_count{0};
  BatchEngine::SeriesResolver counting_resolver =
      [&](PJ::DatasetId dataset, const std::string& key,
          const BatchEngine::Window& window) -> std::optional<BatchEngine::ResolvedSeries> {
    ++resolve_count;
    return mutableValueResolver(value)(dataset, key, window);
  };
  BatchEngine service(store, counting_resolver);
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setReloadHz(20.0);  // 50 ms period

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  resolve_count = 0;  // ignore the upsert's own synchronous run

  for (const int v : {1, 2, 3}) {
    value.store(v);
    (void)service.recomputeForChangedInputs({"in"}, kDataset);
  }
  ASSERT_TRUE(service.isStale("plug/gen"));

  pumpUntil([&] { return !service.isStale("plug/gen"); });
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(resolve_count.load(), 1) << "3 changes within one period → exactly 1 run";
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 3) << "the run saw the last change";
}

// D30: a run whose inputs change while it computes is still published (the display
// moves forward), and the generator stays stale until one follow-up run, with the
// newest inputs, is published too.
TEST(BatchEngineTest, ChangeDuringARunPublishesItAndRerunsWithTheNewestInputs) {
  // Run N blocks in its compute until `released` reaches N (at most 5 s, so a failed
  // assertion cannot hang the pool's destructor).
  std::atomic<int> entered{0};
  std::atomic<int> released{1};  // the upsert's inline run passes
  const ScopedComputeProbe probe([&](QThread*) {
    const int run = ++entered;
    for (int waited_ms = 0; released.load() < run && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setReloadHz(20.0);  // 50 ms period

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  ASSERT_EQ(entered.exchange(0), 1) << "the upsert ran inline, through the probe";
  released = 0;
  std::vector<PJ::Timestamp> published;
  QObject::connect(&service, &BatchEngine::markersPublished, [&] {
    published.push_back(readPublished(store, kDataset, "in").markers.front().t_start);
  });

  value.store(1);
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  pumpUntil([&] { return entered.load() == 1; });
  ASSERT_EQ(entered.load(), 1) << "run #1 is blocked mid-compute";

  value.store(2);  // a change while run #1 computes
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  released = 1;
  pumpUntil([&] { return !published.empty(); });
  ASSERT_EQ(published, std::vector<PJ::Timestamp>{1}) << "run #1 is published although its inputs changed";
  EXPECT_TRUE(service.isStale("plug/gen")) << "a newer change is not shown yet";

  released = 2;
  pumpUntil([&] { return published.size() == 2; }, 5000);
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(published, (std::vector<PJ::Timestamp>{1, 2})) << "one follow-up run, with the newest inputs";
  EXPECT_EQ(entered.load(), 2);
}

// D30: inputs that change faster than the rule runs never freeze the markers. Every
// completed run is published while the stream goes on, and once it stops the
// display converges to the newest inputs.
TEST(BatchEngineTest, MarkersKeepUpdatingWhileInputsKeepChanging) {
  const ScopedComputeProbe probe([](QThread*) { QThread::msleep(30); });  // slower than the stream
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setReloadHz(20.0);  // 50 ms period

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  int published = 0;
  QObject::connect(&service, &BatchEngine::markersPublished, [&published] { ++published; });

  // About 600 ms of a stream ticking every 10 ms: every run sees its inputs change.
  for (int tick = 1; tick <= 60; ++tick) {
    value.store(tick);
    (void)service.recomputeForChangedInputs({"in"}, kDataset);
    pumpFor(10);
  }
  EXPECT_GE(published, 3) << "completed runs keep publishing while the inputs keep changing";

  pumpUntil([&] { return !service.isStale("plug/gen"); });
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 60) << "converged to the newest inputs";
}

// D30 (GAP-08): the stream never lets a run finish on the newest inputs, yet every valid run publishes, in order, so
// the markers never freeze; the stale flag stays up until a run covers the newest input. Each run is held mid-compute
// until the next change has arrived, so the order is the test's, not the clock's.
TEST(BatchEngineTest, StreamingMarkersKeepPublishingUnderConstantChange) {
  constexpr int kRuns = 4;
  std::atomic<int> entered{0};
  std::atomic<int> released{1};  // the upsert's inline run passes
  const ScopedComputeProbe probe([&](QThread*) {
    const int run = ++entered;
    for (int waited_ms = 0; released.load() < run && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setReloadHz(20.0);

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  ASSERT_EQ(entered.exchange(0), 1) << "the upsert ran inline, through the probe";
  released = 0;
  std::vector<PJ::Timestamp> published;
  QObject::connect(&service, &BatchEngine::markersPublished, [&] {
    published.push_back(readPublished(store, kDataset, "in").markers.front().t_start);
  });

  value.store(1);
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  for (int run = 1; run <= kRuns; ++run) {
    pumpUntil([&] { return entered.load() == run; }, 5000);
    ASSERT_EQ(entered.load(), run) << "run " << run << " is blocked mid-compute";
    if (run < kRuns) {
      value.store(run + 1);  // the stream moves on while the run computes
      (void)service.recomputeForChangedInputs({"in"}, kDataset);
    }
    released = run;
    pumpUntil([&] { return static_cast<int>(published.size()) == run; }, 5000);
    ASSERT_EQ(static_cast<int>(published.size()), run) << "run " << run << " is published";
    EXPECT_EQ(published.back(), run) << "the run published the inputs it saw, although newer ones had arrived";
    EXPECT_EQ(service.isStale("plug/gen"), run < kRuns)
        << "stale until the newest input is covered (run " << run << ")";
  }
  EXPECT_EQ(published, (std::vector<PJ::Timestamp>{1, 2, 3, 4}));
}

// A session runs its scheduled rules on its own pool, off the GUI thread, and a run
// that lands later repaints the overlays (markersChanged), which nothing else would.
TEST(BatchEngineTest, ASessionRunsScheduledRulesOnItsPoolAndRepaints) {
  std::atomic<QThread*> compute_thread{nullptr};
  const ScopedComputeProbe probe([&compute_thread](QThread* thread) { compute_thread = thread; });
  PJ::SessionManager session;
  BatchEngine& service = session.batchEngine();
  service.setResolver(rampResolver(5));
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  int repaints = 0;
  QObject::connect(&session, &PJ::SessionManager::markersChanged, [&repaints] { ++repaints; });
  compute_thread = nullptr;  // forget the upsert's inline run

  service.refresh("plug/gen");
  pumpUntil([&] { return repaints > 0; });

  EXPECT_FALSE(service.isStale("plug/gen"));
  ASSERT_NE(compute_thread.load(), nullptr);
  EXPECT_NE(compute_thread.load(), QThread::currentThread()) << "the session installs its pool";
  EXPECT_EQ(repaints, 1);
}

// A scheduled run that fails, by a script error or by a compute that throws, keeps
// the last published set; the generator stays stale and reports why until a run
// succeeds.
TEST(BatchEngineTest, AFailedScheduledRunKeepsTheLastSetAndReportsItsError) {
  std::atomic<bool> throw_in_compute{false};
  const ScopedComputeProbe probe([&throw_in_compute](QThread*) {
    if (throw_in_compute.load()) {
      throw std::runtime_error("compute threw");
    }
  });
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(service
                  .upsertGenerator(markerRecipe(
                      "plug/gen", "in",
                      "local s = series(\"in\")\n"
                      "if s:at(0).v >= 5 then error(\"value too big\") end\n"
                      "createMarker(s:at(0).v, s:at(0).v)\n"))
                  .has_value());
  EXPECT_TRUE(service.errorOf("plug/gen").empty());

  value = 5;
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  service.refresh("plug/gen");
  pool.waitIdle();
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 1) << "the last set stays";
  EXPECT_TRUE(service.isStale("plug/gen"));
  EXPECT_NE(service.errorOf("plug/gen").find("value too big"), std::string::npos) << service.errorOf("plug/gen");

  value = 2;
  throw_in_compute = true;
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  service.refresh("plug/gen");
  pool.waitIdle();
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 1) << "the last set stays";
  EXPECT_TRUE(service.isStale("plug/gen"));
  EXPECT_NE(service.errorOf("plug/gen").find("compute threw"), std::string::npos) << service.errorOf("plug/gen");

  throw_in_compute = false;
  service.refresh("plug/gen");
  pool.waitIdle();
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 2);
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_TRUE(service.errorOf("plug/gen").empty()) << "a successful run clears the error";
}

// Manual mode only marks a generator stale; nothing runs it until refresh.
TEST(BatchEngineTest, ManualModeNeverRunsUntilRefresh) {
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  service.setMode(BatchEngine::Mode::kManual);
  service.setReloadHz(20.0);  // would fire in 50 ms under Automatic — proves Manual never schedules it

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 0);

  value.store(42);
  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);
  EXPECT_TRUE(service.isStale("plug/gen"));

  pumpFor(150);  // well past the brake period under Automatic mode
  EXPECT_TRUE(service.isStale("plug/gen")) << "Manual never schedules a run";
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 0) << "still the pre-change value";

  service.refresh("plug/gen");
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 42);
}

// Switching Manual → Automatic runs everything currently dirty once, with no
// further change needed to arm it.
TEST(BatchEngineTest, SwitchingToAutomaticRunsEverythingDirtyOnce) {
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  service.setMode(BatchEngine::Mode::kManual);
  service.setReloadHz(20.0);

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());

  value.store(7);
  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);
  ASSERT_TRUE(service.isStale("plug/gen"));

  service.setMode(BatchEngine::Mode::kAutomatic);
  pumpUntil([&] { return !service.isStale("plug/gen"); });
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 7);
}

// refresh/refreshAll run right now, ignoring both the brake's rate limit and the
// demand gate.
TEST(BatchEngineTest, RefreshAllIgnoresTheBrake) {
  PJ::ObjectStore store;
  std::atomic<int> value{0};
  BatchEngine service(store, mutableValueResolver(value));
  service.setReloadHz(0.2);                                         // 5 s period — refreshAll must not wait for it
  service.setDemandPredicate([](PJ::DatasetId) { return false; });  // and must ignore demand too

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());

  value.store(9);
  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);
  ASSERT_TRUE(service.isStale("plug/gen"));

  service.refreshAll(kDataset);  // no pool installed → inline, synchronous
  EXPECT_FALSE(service.isStale("plug/gen"));
  EXPECT_EQ(readPublished(store, kDataset, "in").markers.front().t_start, 9);
}

// A generator bound to an undemanded dataset stays stale forever — the brake
// never runs it — but refresh (which ignores demand) still can.
TEST(BatchEngineTest, NoDemandLeavesTheGeneratorStale) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setReloadHz(20.0);                                        // 50 ms period
  service.setDemandPredicate([](PJ::DatasetId) { return false; });  // nothing is ever demanded

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());

  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);
  ASSERT_TRUE(service.isStale("plug/gen"));

  pumpFor(150);
  EXPECT_TRUE(service.isStale("plug/gen")) << "undemanded — stays stale, never auto-runs";

  service.refresh("plug/gen");
  EXPECT_FALSE(service.isStale("plug/gen"));
}

// A stale generator whose dataset becomes demanded after the last ingest must run
// when the demand is announced, not at the next change of its inputs.
TEST(BatchEngineTest, DemandChangedRunsAStaleGeneratorWithoutAnIngest) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setReloadHz(20.0);  // 50 ms period
  std::atomic<bool> demanded{false};
  service.setDemandPredicate([&demanded](PJ::DatasetId) { return demanded.load(); });

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);

  pumpUntil([&] { return !service.isStale("plug/gen"); }, 200);
  ASSERT_TRUE(service.isStale("plug/gen")) << "undemanded — the brake never runs it";

  demanded = true;
  service.demandChanged();
  pumpUntil([&] { return !service.isStale("plug/gen"); });
  EXPECT_FALSE(service.isStale("plug/gen")) << "demand arrived — runs with no further ingest";
}

// stageRun's resolver call runs on the calling (GUI) thread; computeRun's script
// execution runs on a pool worker thread.
TEST(BatchEngineTest, ScriptRunsOffTheGuiThread) {
  std::atomic<QThread*> compute_thread{nullptr};
  const ScopedComputeProbe probe([&compute_thread](QThread* thread) { compute_thread = thread; });
  PJ::ObjectStore store;
  QThread* const gui_thread = QThread::currentThread();
  std::atomic<bool> resolver_ran_off_gui{false};
  BatchEngine::SeriesResolver resolver = [&](PJ::DatasetId, const std::string& key,
                                             const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    if (QThread::currentThread() != gui_thread) {
      resolver_ran_off_gui = true;
    }
    BatchEngine::ResolvedSeries s;
    s.timestamps = {0.0};
    s.values = {1.0};
    return s;
  };
  BatchEngine service(store, resolver);
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setReloadHz(20.0);

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kMutableValueScript)).has_value());
  compute_thread = nullptr;  // forget the upsert's inline run

  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);
  pumpUntil([&] { return !service.isStale("plug/gen"); });

  EXPECT_FALSE(resolver_ran_off_gui.load()) << "stageRun's resolver call runs on the GUI (calling) thread";
  ASSERT_NE(compute_thread.load(), nullptr);
  EXPECT_NE(compute_thread.load(), gui_thread) << "computeRun executes on a pool worker thread";
}

// OWN-04: a removal supersedes the preview's run in flight: it publishes nothing, and its room returns once the
// pool lets the run go.
TEST(BatchEngineTest, RemovedPendingPreviewCannotResurrectItsTopic) {
  PJ::ObjectStore store;
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  BatchEngine service(store, rampResolver(1));
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  service.setMode(BatchEngine::Mode::kManual);
  auto recipe = markerRecipe("audit/pending", "in", "createMarker(0, 1)");
  recipe.ephemeral = true;
  ASSERT_TRUE(service.upsertGenerator(recipe));
  service.refresh(recipe.id);
  EXPECT_EQ(budget.reserved(), BatchEngine::kMarkerRunBytes + kSampleBytes) << "its ceiling and its one-sample copy";
  ASSERT_TRUE(service.removeGenerator(recipe.id));
  pool.waitIdle();
  EXPECT_FALSE(store.findTopic(kDataset, PJ::markerOwnerTopicName("in", recipe.id)));
  EXPECT_EQ(budget.reserved(), 0u);
}

TEST(BatchEngineTest, PendingCompletionCannotOverwriteEditedBinding) {
  PJ::ObjectStore store;
  PJ::EnginePool pool;
  BatchEngine service(store, rampResolver(1));
  service.setPool(&pool);
  auto recipe = markerRecipe("audit/edit", "in", "createMarker(0, 1, {label='old'})");
  ASSERT_TRUE(service.upsertGenerator(recipe));
  service.refresh(recipe.id);
  recipe.script = "createMarker(0, 2, {label='new'})";
  ASSERT_TRUE(service.upsertGenerator(recipe));
  pool.waitIdle();
  const auto markers = readPublished(store, kDataset, "in");
  ASSERT_EQ(markers.markers.size(), 1u);
  EXPECT_EQ(markers.markers.front().label, "new");
}

TEST(BatchEngineTest, RefreshAllTargetsTheRequestedDatasetBinding) {
  PJ::ObjectStore store;
  PJ::EnginePool pool;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  auto recipe = markerRecipe("audit/binding", "in", kMutableValueScript);
  ASSERT_TRUE(service.upsertGenerator(recipe));
  recipe.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(recipe));
  value = 9;
  (void)service.recomputeForChangedInputs({"in"}, 2);
  service.refreshAll(2);
  pool.waitIdle();
  EXPECT_DOUBLE_EQ(readPublished(store, 1, "in").markers.front().value_low, 1);
  EXPECT_DOUBLE_EQ(readPublished(store, 2, "in").markers.front().value_low, 9);
  EXPECT_FALSE(service.isStale(recipe.id));
}

// ASY-13: twelve refreshes during a run cost one more capture, and the one run in flight holds the room of one run.
TEST(BatchEngineTest, RepeatedRefreshStagesOnlyOneLatestPendingSnapshot) {
  PJ::ObjectStore store;
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  std::atomic<int> value{0};
  int captures = 0;
  BatchEngine service(store, [&](PJ::DatasetId dataset, const std::string& key, const BatchEngine::Window& window) {
    ++captures;
    return mutableValueResolver(value)(dataset, key, window);
  });
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  service.setMode(BatchEngine::Mode::kManual);
  auto recipe = markerRecipe("audit/bounded", "in", kMutableValueScript);
  ASSERT_TRUE(service.upsertGenerator(recipe));
  captures = 0;
  for (int i = 1; i <= 12; ++i) {
    value = i;
    (void)service.recomputeForChangedInputs({"in"}, kDataset);
    service.refresh(recipe.id);
  }
  EXPECT_EQ(captures, 1) << "one run in flight; the other refreshes leave one pending request";
  EXPECT_EQ(budget.reserved(), BatchEngine::kMarkerRunBytes + kSampleBytes) << "one run's room";
  pool.waitIdle();
  EXPECT_EQ(captures, 2) << "the pending request captures once, when the run ends";
  EXPECT_DOUBLE_EQ(readPublished(store, kDataset, "in").markers.front().value_low, 12);
  EXPECT_FALSE(service.isStale(recipe.id));
  EXPECT_EQ(budget.reserved(), 0u) << "both runs returned their room";
}

TEST(BatchEngineTest, ReloadAndDatasetRemovalInvalidatePendingDelivery) {
  PJ::ObjectStore store;
  PJ::EnginePool pool;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  service.setPool(&pool);
  auto recipe = markerRecipe("audit/reload", "in", kMutableValueScript);
  ASSERT_TRUE(service.upsertGenerator(recipe));
  service.refresh(recipe.id);
  value = 7;
  EXPECT_FALSE(service.recomputeForDataset(kDataset).empty());
  pool.waitIdle();
  EXPECT_DOUBLE_EQ(readPublished(store, kDataset, "in").markers.front().value_low, 7);
  service.refresh(recipe.id);
  (void)service.clearGeneratorsForDataset(kDataset);
  const auto topic = store.findTopic(kDataset, PJ::markerOwnerTopicName("in", recipe.id));
  ASSERT_TRUE(topic);
  store.removeTopic(*topic);
  pool.waitIdle();
  EXPECT_FALSE(store.findTopic(kDataset, PJ::markerOwnerTopicName("in", recipe.id)));
}

// A streaming tick reports every topic of its dataset. A rule whose input did not
// move since the last tick is not marked stale, so it does not re-run every period.
TEST(BatchEngineTest, ATickWhoseInputDidNotMoveSchedulesNoRun) {
  PJ::SessionManager session;
  BatchEngine& service = session.batchEngine();
  service.setResolver(rampResolver(5));
  service.setReloadHz(20.0);  // 50 ms period
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream"});
  ASSERT_TRUE(dataset.has_value());
  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");
  recipe.dataset_id = *dataset;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "in", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 100, 1.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  ASSERT_TRUE(service.isStale("plug/gen")) << "the input moved";
  pumpUntil([&] { return !service.isStale("plug/gen"); });
  ASSERT_FALSE(service.isStale("plug/gen"));

  const auto tick = [&] { session.recomputeMarkersForChanged(session.createReader().listTopics(*dataset), *dataset); };
  tick();
  EXPECT_FALSE(service.isStale("plug/gen")) << "nothing moved since the last run";

  // A streaming write host commits straight to the engine; only the tick reports it.
  writer.appendScalar(*handle, 200, 2.0);
  ASSERT_FALSE(session.dataEngine().commitChunks(writer.flushAll()).empty());
  tick();
  EXPECT_TRUE(service.isStale("plug/gen")) << "the input moved";
}

// The runs of a burst land in one pool drain, and the plots refresh once for it:
// markersPublished is announced once per drain, not once per run.
TEST(BatchEngineTest, RunsPublishedInOneDrainAnnounceOnce) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  constexpr std::size_t kRuns = 4;
  for (std::size_t run = 0; run < kRuns; ++run) {
    const std::string id = "plug/gen" + std::to_string(run);
    ASSERT_TRUE(service.upsertGenerator(markerRecipe(id, "in", "createMarker(0.0)\n")).has_value());
  }
  pool.waitIdle();  // the creates' own runs leave a (dropped) publish in the pool
  int announcements = 0;
  QObject::connect(&service, &BatchEngine::markersPublished, [&announcements] { ++announcements; });

  service.refreshAll(kDataset);
  // Every compute finishes before the event loop runs the pool's (single) drain.
  for (int waited_ms = 0; pool.pendingPublishes() < kRuns && waited_ms < 5000; ++waited_ms) {
    QThread::msleep(1);
  }
  ASSERT_EQ(pool.pendingPublishes(), kRuns);
  pool.waitIdle();
  pumpFor(50);
  EXPECT_TRUE(service.staleIds().empty());
  EXPECT_EQ(announcements, 1);
}

// The rule that fails once its input value reaches 5 (mutableValueResolver).
// A merge concatenates every consumed set onto the anchor's topic. A run staged before
// it, of the anchor's binding or of an all-datasets rule, would publish over that
// concatenation from the pre-merge data: the merge cancels it, and the anchor's and the
// global rules re-run over the merged data instead.
TEST(BatchEngineTest, AMergeCancelsTheRunsStagedBeforeItAndMarksTheSurvivorsStale) {
  std::atomic<bool> released{false};
  const ScopedComputeProbe probe(PJ::test::holdPoolComputesUntil(released));
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  auto on_anchor = markerRecipe("plug/r", "in", "createMarker(0.0)\n");
  on_anchor.dataset_id = 1;
  ASSERT_TRUE(service.upsertGenerator(on_anchor).has_value());
  auto on_source = markerRecipe("plug/r", "in", "createMarker(1.0)\n");
  on_source.dataset_id = 2;
  ASSERT_TRUE(service.upsertGenerator(on_source).has_value());
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  service.refresh("plug/r");
  service.refresh("plug/g");
  ASSERT_TRUE(service.isRunning("plug/r"));
  ASSERT_TRUE(service.isRunning("plug/g"));

  service.mergeMarkerTopics(1, {PJ::DatasetMergeSource{.dataset_id = 2, .raw_shift_ns = 1000}});
  service.remapGeneratorsToAnchor(1, {2});
  EXPECT_FALSE(service.isRunning("plug/r")) << "cancelled at the merge";
  EXPECT_FALSE(service.isRunning("plug/g")) << "cancelled at the merge";
  released = true;
  pool.waitIdle();

  EXPECT_EQ(readPublished(store, 1, "in").markers.size(), 2u) << "the concatenation stays until the re-run";
  EXPECT_TRUE(service.isStale("plug/r")) << "the anchor re-runs over the merged data";
  EXPECT_TRUE(service.isStale("plug/g")) << "the global rule re-runs over the merged data";
}

// A reload detaches the dataset's markers while it refills. A run staged from the old
// data must not publish into the partially refilled dataset: the refill cancels it, and
// the refill's own synchronous recompute (commit or rollback) is the next run.
TEST(BatchEngineTest, BeginningARefillCancelsTheDatasetsRunInFlight) {
  std::atomic<bool> released{false};
  const ScopedComputeProbe probe(PJ::test::holdPoolComputesUntil(released));
  PJ::EnginePool pool;
  PJ::SessionManager session;
  BatchEngine& service = session.batchEngine();
  service.setResolver(rampResolver(5));
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "file"});
  ASSERT_TRUE(dataset.has_value());
  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");
  recipe.dataset_id = *dataset;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  service.refresh("plug/gen");
  ASSERT_TRUE(service.isRunning("plug/gen"));

  {
    PJ::RefillGuard guard = session.beginRefill(*dataset);
    EXPECT_FALSE(service.isRunning("plug/gen")) << "cancelled when the refill began";
    released = true;
    pool.waitIdle();
    EXPECT_TRUE(readPublished(session.objectStore(), *dataset, "in").markers.empty())
        << "nothing computed from the old data lands in the refilling dataset";
  }
  EXPECT_EQ(readPublished(session.objectStore(), *dataset, "in").markers.size(), 1u)
      << "the rollback's recompute republished";
}

// A cancel reaches a script already inside the interpreter: removing the rule frees the
// pool long before the script would have finished, not when its fuel runs out. Each
// bandPower is one native call over a large series, so the script spends a few thousand
// safepoints (far under its fuel) and many seconds.
TEST(BatchEngineTest, RemovingARuleStopsItsRunningScript) {
  std::atomic<bool> compute_started{false};
  const ScopedComputeProbe probe([&compute_started](QThread*) { compute_started = true; });
  std::atomic<std::size_t> samples{1};  // one sample: the create's inline run is instant
  PJ::ObjectStore store;
  BatchEngine service(
      store,
      [&samples](PJ::DatasetId, const std::string& key, const BatchEngine::Window&)
          -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "in") {
          return std::nullopt;
        }
        BatchEngine::ResolvedSeries series;
        for (std::size_t i = 0; i < samples; ++i) {
          series.timestamps.push_back(static_cast<double>(i));
          series.values.push_back(static_cast<double>(i));
        }
        return series;
      });
  PJ::EnginePool pool;
  service.setPool(&pool);
  service.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(service
                  .upsertGenerator(markerRecipe(
                      "plug/slow", "in",
                      "local s = series(\"in\")\nfor i = 1, 2000 do bandPower(s, 0, 1e9) end\ncreateMarker(0.0)\n"))
                  .has_value());
  samples = std::size_t{1} << 18;
  compute_started = false;  // forget the create's inline run
  service.refresh("plug/slow");
  pumpUntil([&compute_started] { return compute_started.load(); });
  ASSERT_TRUE(compute_started.load()) << "the run never reached a worker";

  ASSERT_TRUE(service.removeGenerator("plug/slow").has_value());
  const auto removed_at = std::chrono::steady_clock::now();
  pool.waitIdle();
  EXPECT_LT(std::chrono::steady_clock::now() - removed_at, std::chrono::seconds(3)) << "the cancelled script ran on";
}

// Retention can hide the oldest samples of a quiet stream without moving its newest
// timestamp or its stored row count: the tick must still report the input as changed.
TEST(BatchEngineTest, ATickAfterRetentionHidSamplesOfAQuietTopicMarksItsRulesStale) {
  PJ::SessionManager session;
  BatchEngine& service = session.batchEngine();
  service.setResolver(rampResolver(5));
  service.setPool(nullptr);
  service.setMode(BatchEngine::Mode::kManual);
  const auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream"});
  ASSERT_TRUE(dataset.has_value());
  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");
  recipe.dataset_id = *dataset;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "in", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  for (const PJ::Timestamp t : {100, 200, 300}) {
    writer.appendScalar(*handle, t, 1.0);
  }
  ASSERT_FALSE(session.dataEngine().commitChunks(writer.flushAll()).empty());
  const auto tick = [&] { session.recomputeMarkersForChanged(session.createReader().listTopics(*dataset), *dataset); };
  tick();
  service.refresh("plug/gen");
  ASSERT_FALSE(service.isStale("plug/gen"));
  tick();
  ASSERT_FALSE(service.isStale("plug/gen")) << "nothing moved";

  // The floor rises to 150 inside the one chunk: the newest sample and the row count stay.
  session.dataEngine().enforceRetention(150, *dataset);
  tick();
  EXPECT_TRUE(service.isStale("plug/gen")) << "the sample at 100 is gone from the series";
}

// Demand of an all-datasets rule is the demand of the datasets that contribute to it:
// one whose only contributors are undemanded streams does not run on a brake pass, and
// runs once one of them is displayed.
TEST(BatchEngineTest, AGlobalRuleRunsOnABrakePassOnlyWhenAContributorIsDemanded) {
  PJ::ObjectStore store;
  // "in" lives only on the streams 2 and 3; the file 1 carries "other".
  const BatchEngine::SeriesResolver ramp = rampResolver(5);
  BatchEngine service(
      store,
      [ramp](PJ::DatasetId dataset, const std::string& key, const BatchEngine::Window& window)
          -> std::optional<BatchEngine::ResolvedSeries> {
        const bool present = (key == "in" && (dataset == 2 || dataset == 3)) || (key == "other" && dataset == 1);
        return present ? ramp(dataset, "in", window) : std::nullopt;
      });
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });
  bool stream_displayed = false;
  // AppSession's predicate: a stream counts once displayed, any other dataset always.
  service.setDemandPredicate(
      [&stream_displayed](PJ::DatasetId dataset) { return stream_displayed || (dataset != 2 && dataset != 3); });
  service.setReloadHz(20.0);  // 50 ms period
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  auto on_file = markerRecipe("plug/file", "other", "createMarker(0.0)\n");
  on_file.inputs = {"other"};
  ASSERT_TRUE(service.upsertGenerator(on_file).has_value());

  (void)service.recomputeForChangedInputs({}, BatchEngine::kAnyDataset);
  pumpUntil([&service] { return !service.isStale("plug/file"); });
  ASSERT_FALSE(service.isStale("plug/file")) << "the brake pass ran";
  EXPECT_TRUE(service.isStale("plug/g")) << "no contributor of the global rule is displayed";

  stream_displayed = true;
  service.demandChanged();
  pumpUntil([&service] { return !service.isStale("plug/g"); });
  EXPECT_FALSE(service.isStale("plug/g")) << "a contributor is displayed";
}

// Manual mode waits for Refresh: switching to it stops a brake that was already armed.
TEST(BatchEngineTest, SwitchingToManualStopsAnArmedBrake) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  service.setReloadHz(20.0);  // 50 ms period
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  ASSERT_EQ(service.recomputeForChangedInputs({"in"}, kDataset).size(), 1u);  // arms the brake

  service.setMode(BatchEngine::Mode::kManual);
  pumpFor(200);
  EXPECT_TRUE(service.isStale("plug/gen")) << "manual mode runs nothing until Refresh";
}

// A failed binding stays stale, but a brake pass armed by another rule's change does
// not resubmit it: it runs again once its own inputs change.
TEST(BatchEngineTest, AFailedRuleIsNotRerunByAnUnrelatedBrakePass) {
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  std::map<std::string, int> resolves;
  const BatchEngine::SeriesResolver mutable_in = mutableValueResolver(value);
  const BatchEngine::SeriesResolver ramp = rampResolver(5);
  BatchEngine service(
      store,
      [&](PJ::DatasetId dataset, const std::string& key,
          const BatchEngine::Window& window) -> std::optional<BatchEngine::ResolvedSeries> {
        ++resolves[key];
        return key == "in" ? mutable_in(dataset, key, window) : ramp(dataset, "in", window);
      });
  service.setReloadHz(20.0);  // 50 ms period
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/failing", "in", kFailsFromFiveScript)).has_value());
  auto other = markerRecipe("plug/other", "other", "createMarker(0.0)\n");
  other.inputs = {"other"};
  ASSERT_TRUE(service.upsertGenerator(other).has_value());

  value = 5;
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  pumpUntil([&service] { return !service.errorOf("plug/failing").empty(); });
  ASSERT_FALSE(service.errorOf("plug/failing").empty());
  const int failing_runs = resolves["in"];

  (void)service.recomputeForChangedInputs({"other"}, kDataset);
  pumpUntil([&service] { return !service.isStale("plug/other"); });
  ASSERT_FALSE(service.isStale("plug/other")) << "the unrelated brake pass ran";
  EXPECT_EQ(resolves["in"], failing_runs) << "the failed rule was not resubmitted";
  EXPECT_TRUE(service.isStale("plug/failing"));

  value = 1;
  (void)service.recomputeForChangedInputs({"in"}, kDataset);
  pumpUntil([&service] { return !service.isStale("plug/failing"); });
  EXPECT_FALSE(service.isStale("plug/failing")) << "its own input changed";
  EXPECT_TRUE(service.errorOf("plug/failing").empty());
}

// A reload that breaks a rule records the failure like any scheduled run: errorOf (the
// config "error" key) says why, the binding stays stale and the bell hears it once.
TEST(BatchEngineTest, AReloadThatBreaksARuleRecordsAndReportsItsError) {
  PJ::ObjectStore store;
  std::atomic<int> value{1};
  BatchEngine service(store, mutableValueResolver(value));
  service.setMode(BatchEngine::Mode::kManual);
  std::vector<PJ::Diagnostic> reported;
  service.setDiagnosticSink([&reported](const PJ::Diagnostic& diagnostic) { reported.push_back(diagnostic); });
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", kFailsFromFiveScript)).has_value());

  value = 5;
  EXPECT_TRUE(service.recomputeForDataset(kDataset).empty());
  EXPECT_NE(service.errorOf("plug/gen").find("value too big"), std::string::npos) << service.errorOf("plug/gen");
  EXPECT_TRUE(service.isStale("plug/gen"));
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_NE(reported.front().message.find("Marker rule 'plug/gen' failed: "), std::string::npos)
      << reported.front().message;
  EXPECT_NE(
      reported.front().message.find("Any previous markers are kept; fix the rule and apply it again."),
      std::string::npos)
      << reported.front().message;

  (void)service.recomputeForDataset(kDataset);
  EXPECT_EQ(reported.size(), 1u) << "the same error is reported once";
}

// ---- the in-flight budget (BUD-11, docs/derived_objects/ARCHITECTURE.md §11) -------------

namespace {

/// One series per input key, of `rows[key]` samples, counting each staging per key.
BatchEngine::SeriesResolver countingResolver(
    std::map<std::string, std::size_t> rows, std::map<std::string, int>& stagings) {
  return [rows = std::move(rows), &stagings](
             PJ::DatasetId, const std::string& key,
             const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    const auto found = rows.find(key);
    if (found == rows.end()) {
      return std::nullopt;
    }
    ++stagings[key];
    return BatchEngine::ResolvedSeries{
        .timestamps = std::vector<double>(found->second, 0.0), .values = std::vector<double>(found->second, 1.0)};
  };
}

/// The rule "plug/<key>" reading `key`.
BatchEngine::GeneratorRecipe ruleReading(const std::string& key, std::string script = "createMarker(0.0)\n") {
  BatchEngine::GeneratorRecipe recipe = markerRecipe("plug/" + key, "in", std::move(script));
  recipe.inputs = {key};
  return recipe;
}

}  // namespace

// BUD-11 (changed): every binding's run reserves the run's ceiling before it stages and its staged copy once staged,
// so the runs together stay within the budget. A large rule whose copy does not fit beside the runs in flight gives
// its room back, keeps its copy and waits at the head of the line: the rule behind it is not let in to stage
// meanwhile, and the large one runs next, on the copy it kept.
TEST(BatchEngineTest, AdmissionBoundsAllBindingsAndTheStagedCopyIsCharged) {
  // Run N computes once `released` reaches N (at most 5 s); the creates' inline runs pass.
  std::atomic<int> entered{0};
  std::atomic<int> released{0};
  QThread* const gui = QThread::currentThread();
  const ScopedComputeProbe probe([&](QThread* thread) {
    if (thread == gui) {
      return;
    }
    const int run = ++entered;
    for (int waited_ms = 0; released.load() < run && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::map<std::string, int> stagings;
  const std::size_t ceiling = BatchEngine::kMarkerRunBytes;
  const std::size_t small_copy = 5 * kSampleBytes;
  const std::size_t large_copy = 128 * kSampleBytes;
  PJ::InFlightBudget budget(2 * ceiling + kOneRunSlack);  // two ceilings and small copies, or one large run
  PJ::EnginePool pool;
  if (pool.sharedLaneThreads() < 2) {
    GTEST_SKIP() << "needs two runs in flight; this host's shared lane has one thread";
  }
  BatchEngine service(store, countingResolver({{"a", 5}, {"x", 5}, {"b", 128}, {"c", 5}}, stagings));
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  service.setMode(BatchEngine::Mode::kManual);
  for (const char* key : {"a", "x", "b", "c"}) {
    ASSERT_TRUE(service.upsertGenerator(ruleReading(key)).has_value());
  }
  stagings.clear();
  const auto within = [&budget] { return budget.reserved() <= budget.capacity(); };

  service.refresh("plug/a");
  service.refresh("plug/x");
  pumpUntil([&] { return entered.load() == 2; });
  ASSERT_EQ(entered.load(), 2) << "two small rules compute side by side";
  EXPECT_EQ(budget.reserved(), 2 * (ceiling + small_copy));
  service.refresh("plug/b");
  service.refresh("plug/c");
  EXPECT_EQ(stagings["b"], 0) << "no room for its ceiling: it waits, unstaged";
  EXPECT_EQ(stagings["c"], 0);
  EXPECT_TRUE(service.isRunning("plug/b"));
  EXPECT_TRUE(within());

  released = 1;  // a publishes: b's ceiling fits, its copy does not
  pumpUntil([&] { return stagings["b"] == 1; });
  ASSERT_EQ(stagings["b"], 1) << "it staged once its ceiling fit";
  EXPECT_EQ(budget.reserved(), ceiling + small_copy) << "its copy did not fit beside x: it gave its room back";
  EXPECT_EQ(stagings["c"], 0) << "c, behind it, is not let in to stage meanwhile";
  EXPECT_TRUE(within());

  released = 2;  // x publishes: room for all of b
  pumpUntil([&] { return entered.load() == 3; });
  ASSERT_EQ(entered.load(), 3);
  EXPECT_EQ(budget.reserved(), ceiling + large_copy) << "b runs next, charged its copy";
  EXPECT_EQ(stagings["b"], 1) << "on the copy it kept";
  EXPECT_EQ(stagings["c"], 0);
  EXPECT_TRUE(within());

  released = 4;
  pool.waitIdle();
  EXPECT_EQ(stagings, (std::map<std::string, int>{{"a", 1}, {"b", 1}, {"c", 1}, {"x", 1}}));
  EXPECT_TRUE(service.staleIds().empty());
  EXPECT_TRUE(service.runningIds().empty());
  EXPECT_EQ(budget.reserved(), 0u);
}

// A run's room returns at its publication whether it published or failed, and, cancelled or orphaned by its
// engine, once the pool lets it go. A run waiting for room that is cancelled never stages.
TEST(BatchEngineTest, EveryReservationReturnsOnPublishFailureCancelAndDestruction) {
  std::atomic<bool> released{true};
  const ScopedComputeProbe probe(PJ::test::holdPoolComputesUntil(released));
  PJ::ObjectStore store;
  std::map<std::string, int> stagings;
  std::atomic<int> value{1};
  const BatchEngine::SeriesResolver resolver =
      [&](PJ::DatasetId, const std::string& key,
          const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    ++stagings[key];
    return BatchEngine::ResolvedSeries{.timestamps = {0.0}, .values = {static_cast<double>(value.load())}};
  };
  const std::size_t one_run = BatchEngine::kMarkerRunBytes + kSampleBytes;
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  auto service = std::make_unique<BatchEngine>(store, resolver);
  service->setPool(&pool);
  service->setInFlightBudget(&budget);
  service->setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(service
                  ->upsertGenerator(ruleReading(
                      "ok",
                      "local s = series(\"ok\")\nif s:at(0).v >= 5 then error(\"value too big\") end\n"
                      "createMarker(0.0)\n"))
                  .has_value());
  ASSERT_TRUE(service->upsertGenerator(ruleReading("waits")).has_value());

  service->refresh("plug/ok");
  EXPECT_EQ(budget.reserved(), one_run);
  pool.waitIdle();
  EXPECT_EQ(budget.reserved(), 0u) << "published";

  value = 5;
  service->refresh("plug/ok");
  EXPECT_EQ(budget.reserved(), one_run);
  pool.waitIdle();
  EXPECT_NE(service->errorOf("plug/ok").find("value too big"), std::string::npos) << service->errorOf("plug/ok");
  EXPECT_EQ(budget.reserved(), 0u) << "failed";
  value = 1;

  stagings.clear();
  released = false;
  service->refresh("plug/ok");
  service->refresh("plug/waits");
  EXPECT_EQ(budget.reserved(), one_run) << "one runs, the other waits";
  ASSERT_TRUE(service->removeGenerator("plug/waits").has_value());
  service->cancelRunsOn(kDataset);  // a refill begins: the run in flight is cancelled
  EXPECT_EQ(budget.reserved(), one_run) << "held until the pool lets the cancelled run go";
  released = true;
  pool.waitIdle();
  EXPECT_EQ(budget.reserved(), 0u) << "cancelled";
  EXPECT_EQ(stagings["waits"], 0) << "removed while it waited: never staged";

  ASSERT_TRUE(service->upsertGenerator(ruleReading("waits")).has_value());
  stagings.clear();
  released = false;
  service->refresh("plug/ok");
  service->refresh("plug/waits");
  service->cancelRunsOn(kDataset);
  released = true;
  pool.waitIdle();
  EXPECT_EQ(stagings["waits"], 0) << "a refill cancels a run that waits for room too";
  EXPECT_EQ(budget.reserved(), 0u);

  released = false;
  service->refresh("plug/ok");
  service->refresh("plug/waits");
  service.reset();
  EXPECT_EQ(budget.reserved(), one_run) << "the pool still holds the orphaned run";
  released = true;
  pool.waitIdle();
  EXPECT_EQ(budget.reserved(), 0u) << "the engine is gone";
  EXPECT_EQ(stagings["waits"], 0);

  {
    PJ::InFlightBudget other_budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);  // outlives the pool below
    PJ::EnginePool own_pool;
    BatchEngine engine(store, resolver);
    engine.setPool(&own_pool);
    engine.setInFlightBudget(&budget);
    engine.setMode(BatchEngine::Mode::kManual);
    ASSERT_TRUE(engine.upsertGenerator(ruleReading("own")).has_value());
    ASSERT_TRUE(engine.upsertGenerator(ruleReading("later")).has_value());
    stagings.clear();
    engine.refresh("plug/own");
    engine.refresh("plug/later");
    engine.setPool(nullptr);  // cancels the run and drops the one waiting: none starts on the old pool
    own_pool.waitIdle();
    EXPECT_EQ(stagings["later"], 0);
    EXPECT_EQ(budget.reserved(), 0u);
    engine.setPool(&own_pool);
    engine.refresh("plug/own");
    engine.refresh("plug/later");
    engine.setInFlightBudget(&other_budget);  // like a pool change: cancels both, the waiting one never starts
    own_pool.waitIdle();
    EXPECT_EQ(stagings["later"], 0);
    EXPECT_EQ(budget.reserved(), 0u) << "the cancelled run returned its room to the budget it took it from";
    engine.refresh("plug/own");
    EXPECT_EQ(other_budget.reserved(), one_run);
    EXPECT_EQ(budget.reserved(), 0u);
  }  // the engine, then its pool, whose runs are never published
  EXPECT_EQ(budget.reserved(), 0u) << "the pool is gone";
}

// A resolver may re-enter the engine while it stages a run the line just let in. A run that was moved to another
// pool, cancelled and refreshed, or removed meanwhile starts nothing: its copy and its room go, and the refresh that
// replaced it runs once.
TEST(BatchEngineTest, ARunChangedWhileItStagesStartsNothing) {
  std::atomic<bool> released{false};
  std::atomic<int> computes{0};
  QThread* const gui = QThread::currentThread();
  const ScopedComputeProbe probe([&](QThread* thread) {
    if (thread == gui) {
      return;
    }
    ++computes;
    for (int waited_ms = 0; !released.load() && waited_ms < 5000; ++waited_ms) {
      QThread::msleep(1);
    }
  });
  PJ::ObjectStore store;
  std::map<std::string, int> stagings;
  std::function<void()> reenter;
  const BatchEngine::SeriesResolver resolver =
      [&](PJ::DatasetId, const std::string& key,
          const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
    ++stagings[key];
    if (key == "b" && reenter) {
      const std::function<void()> hook = std::exchange(reenter, {});
      hook();
    }
    return BatchEngine::ResolvedSeries{.timestamps = {0.0}, .values = {1.0}};
  };
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  PJ::EnginePool other_pool;
  BatchEngine service(store, resolver);
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  service.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(service.upsertGenerator(ruleReading("a")).has_value());
  ASSERT_TRUE(service.upsertGenerator(ruleReading("b")).has_value());
  // a holds the room and b waits for it; when a publishes, the line lets b in and its staging runs `hook`.
  const auto grantB = [&](std::function<void()> hook) {
    stagings.clear();
    computes = 0;
    released = false;
    service.refresh("plug/a");
    service.refresh("plug/b");
    reenter = std::move(hook);
    released = true;
    pool.waitIdle();
    other_pool.waitIdle();
  };

  grantB([&] { service.setPool(&other_pool); });
  EXPECT_EQ(stagings["b"], 1);
  EXPECT_EQ(computes.load(), 1) << "a only: b was staged for a pool it no longer runs on";
  EXPECT_FALSE(service.isRunning("plug/b"));
  EXPECT_EQ(budget.reserved(), 0u);
  service.setPool(&pool);

  grantB([&] {
    service.cancelRunsOn(kDataset);
    service.refresh("plug/b");
  });
  EXPECT_EQ(stagings["b"], 2) << "the refresh staged again";
  EXPECT_EQ(computes.load(), 2) << "a and one run of b";
  EXPECT_FALSE(service.isRunning("plug/b"));
  EXPECT_EQ(budget.reserved(), 0u);

  grantB([&] { ASSERT_TRUE(service.removeGenerator("plug/b").has_value()); });
  EXPECT_EQ(computes.load(), 1) << "a only: b is gone";
  EXPECT_EQ(budget.reserved(), 0u);
}

// A layout load creates its rules one after another, each bounded. A first run that finished within the create's
// wait returns its room at once, so with room for one run every create still runs on the pool and is drawn when it
// returns, none of them left waiting behind a finished one.
TEST(BatchEngineTest, ABoundedCreateThatFinishedReturnsItsRoomAtOnce) {
  const ScopedCreateBudget create_budget(PJ::test::kGenerousCreateBudget);
  std::atomic<int> pool_computes{0};
  QThread* const gui = QThread::currentThread();
  const ScopedComputeProbe probe([&](QThread* thread) {
    if (thread != gui) {
      ++pool_computes;
    }
  });
  PJ::ObjectStore store;
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  BatchEngine service(store, rampResolver(5));
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  for (int rule = 0; rule < 3; ++rule) {
    const std::string id = "plug/load" + std::to_string(rule);
    ASSERT_TRUE(
        service.upsertGenerator(markerRecipe(id, "in", "createMarker(0.0)\n"), BatchEngine::CreateWait::kBounded)
            .has_value());
    EXPECT_FALSE(service.isRunning(id)) << id << ": its first run finished within the create";
    EXPECT_EQ(budget.reserved(), 0u) << id << ": and returned its room";
  }
  EXPECT_EQ(pool_computes.load(), 3) << "every first run computed on the pool";
  EXPECT_EQ(PJ::test::markerFamily(store, kDataset, "in").size(), 3u) << "each drawn when its create returned";
  pool.waitIdle();
}

// A bounded create (a layout load) that finds no room at once does not run inline: the rule is installed and its
// first run waits in the line like a refresh, then stages, runs on the pool and publishes late (BAT-07: a layout
// load's rule may draw progressively). Only a bounded create without a pool runs inline.
TEST(BatchEngineTest, ABoundedCreateWithoutRoomWaitsInTheLine) {
  const ScopedCreateBudget create_budget(PJ::test::kGenerousCreateBudget);
  std::atomic<QThread*> compute_thread{nullptr};
  const ScopedComputeProbe probe([&compute_thread](QThread* thread) { compute_thread = thread; });
  PJ::ObjectStore store;
  PJ::InFlightBudget budget(BatchEngine::kMarkerRunBytes + kOneRunSlack);
  PJ::EnginePool pool;
  BatchEngine service(store, rampResolver(5));
  service.setPool(&pool);
  service.setInFlightBudget(&budget);
  std::optional<PJ::InFlightBudget::Reservation> taken = budget.tryReserve(budget.capacity());
  ASSERT_TRUE(taken.has_value());

  ASSERT_TRUE(
      service.upsertGenerator(markerRecipe("plug/a", "in", "createMarker(0.0)\n"), BatchEngine::CreateWait::kBounded)
          .has_value());
  EXPECT_EQ(compute_thread.load(), nullptr) << "nothing computed while the create held the GUI thread";
  EXPECT_EQ(service.recipes().size(), 1u) << "installed";
  EXPECT_TRUE(service.isRunning("plug/a")) << "its first run waits for room";
  EXPECT_TRUE(PJ::test::markerFamily(store, kDataset, "in").empty()) << "nothing drawn yet";
  EXPECT_EQ(budget.reserved(), budget.capacity()) << "a waiting run holds no room";

  taken.reset();
  pumpUntil([&] { return !service.isRunning("plug/a"); });
  ASSERT_NE(compute_thread.load(), nullptr);
  EXPECT_NE(compute_thread.load(), QThread::currentThread()) << "computed on the pool";
  EXPECT_EQ(PJ::test::markerFamily(store, kDataset, "in").size(), 1u) << "published late, through the drain";
  pool.waitIdle();
  EXPECT_EQ(budget.reserved(), 0u);

  BatchEngine poolless(store, rampResolver(5));
  poolless.setInFlightBudget(&budget);
  compute_thread = nullptr;
  ASSERT_TRUE(
      poolless.upsertGenerator(markerRecipe("plug/b", "in", "createMarker(1.0)\n"), BatchEngine::CreateWait::kBounded)
          .has_value());
  EXPECT_EQ(compute_thread.load(), QThread::currentThread()) << "without a pool: inline";
}

// kMarkerRunBytes holds ONE native output ceiling for a run, however many datasets contribute: an all-datasets
// rule whose contributions each fit the ceiling fails once together they do not.
TEST(BatchEngineTest, AllDatasetContributionsShareOneNativeOutputCeiling) {
  PJ::ObjectStore store;
  BatchEngine service(store, rampResolver(5));
  // About 10 MiB of labels per contribution: one fits the 16 MiB ceiling, two do not.
  auto recipe = markerRecipe(
      "plug/big", std::string(PJ::sdk::kGlobalMarkerTopic),
      "for i = 1, 10 do createMarker(i, i, {label = string.rep('x', 1048576)}) end\n");
  recipe.all_datasets = true;
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });
  const auto alone = service.upsertGenerator(recipe);
  ASSERT_TRUE(alone.has_value()) << alone.error();

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  recipe.id = "plug/bigger";
  const auto together = service.upsertGenerator(recipe);
  ASSERT_FALSE(together.has_value());
  EXPECT_NE(together.error().find("byte ceiling"), std::string::npos) << together.error();
}
