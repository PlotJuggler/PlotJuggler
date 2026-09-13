// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// MarkerService backs the unified pj.data_processors.v1 service (kind=markers — the object
// engine). These tests drive it against a synthetic resolver + a real ObjectStore —
// no SDK service, no catalog — proving the host-driven execution path end to end:
// resolve → run engine → serialize → publish to ObjectStore → read back.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerService.h"

namespace {

using PJ::GeneratorKind;
using PJ::MarkerService;

constexpr PJ::DatasetId kDataset = 1;

// A resolver backed by one synthetic ramp series "in": t = i ns, v = i, i in [0,n).
MarkerService::SeriesResolver rampResolver(std::size_t n) {
  return [n](PJ::DatasetId, const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
    if (key != "in") {
      return std::nullopt;
    }
    MarkerService::ResolvedSeries s;
    for (std::size_t i = 0; i < n; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
}

// Read back and decode the PlotMarkers published for `marker_topic` on `dataset`.
PJ::sdk::PlotMarkers readPublished(PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& marker_topic) {
  const std::string topic_name = PJ::sdk::markerObjectTopicName(marker_topic);
  const std::optional<PJ::ObjectTopicId> id = store.findTopic(dataset, topic_name);
  EXPECT_TRUE(id.has_value()) << "marker object topic was not registered";
  if (!id.has_value()) {
    return {};
  }
  const std::optional<PJ::ResolvedObjectEntry> entry = store.latestAt(*id, PJ::Timestamp{0});
  EXPECT_TRUE(entry.has_value()) << "no published marker payload";
  if (!entry.has_value()) {
    return {};
  }
  PJ::Expected<PJ::sdk::PlotMarkers> decoded =
      PJ::deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
  EXPECT_TRUE(decoded.has_value());
  return decoded.has_value() ? *decoded : PJ::sdk::PlotMarkers{};
}

// A kind=markers generator with a single output topic key.
MarkerService::GeneratorRecipe markerRecipe(std::string id, std::string output, std::string script) {
  MarkerService::GeneratorRecipe r;
  r.id = std::move(id);
  r.kind = GeneratorKind::kMarkers;
  r.dataset_id = kDataset;
  r.inputs = {"in"};
  r.outputs = {std::move(output)};
  r.script = std::move(script);
  return r;
}

// The host resolves the input series, runs the script, and publishes its markers.
TEST(MarkerServiceTest, RunsScriptAndPublishesMarkers) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(10));

  auto recipe = markerRecipe("plug/gen1", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v, {label="first"})
  )");

  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());
  EXPECT_EQ(ok->front(), PJ::sdk::markerObjectTopicName("in"));

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
TEST(MarkerServiceTest, WithoutAResolverTheRunFailsInsteadOfPublishingAnEmptySet) {
  PJ::ObjectStore store;
  MarkerService service(store, MarkerService::SeriesResolver{});

  auto recipe = markerRecipe("plug/no_resolver", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v, {label="first"})
  )");

  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_FALSE(store.findTopic(kDataset, PJ::sdk::markerObjectTopicName("in")).has_value());
  EXPECT_TRUE(service.recipes().empty());
}

TEST(MarkerServiceTest, ScriptErrorReturnsMessageAndDoesNotRegister) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/bad", "__global__", "this is not valid lua %%%");
  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// recomputeForChangedInputs re-runs only generators that read a changed key; an
// unrelated change is a no-op. removeGenerator drops the recipe.
TEST(MarkerServiceTest, RecomputeMatchesChangedKeyAndRemoveDrops) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(8));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());

  EXPECT_TRUE(service.recomputeForChangedInputs({"other"}, MarkerService::kAllDatasets).empty());

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, MarkerService::kAllDatasets);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(affected[0], PJ::sdk::markerObjectTopicName("in"));

  EXPECT_TRUE(service.removeGenerator("plug/gen").has_value());
  EXPECT_FALSE(service.removeGenerator("plug/gen").has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// The "__preview__/" namespace is reserved: a committed (non-ephemeral) marker
// generator targeting it is rejected.
TEST(MarkerServiceTest, CommittedGeneratorRejectsPreviewPrefix) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/x", "__preview__/sneaky", "createMarker(0.0)\n");
  EXPECT_FALSE(service.upsertGenerator(recipe).has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A global-across-all generator publishes to every dataset the lister returns.
TEST(MarkerServiceTest, GlobalAllDatasetsPublishesEverywhere) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  const std::string topic = PJ::sdk::markerObjectTopicName(PJ::sdk::kGlobalMarkerTopic);
  EXPECT_TRUE(store.findTopic(1, topic).has_value());
  EXPECT_TRUE(store.findTopic(2, topic).has_value());
}

// Retargeting a live generator's output must not strand the old topic: nothing else
// ever names it again, so it would keep drawing a stale set forever.
TEST(MarkerServiceTest, UpsertRetargetTombstonesThePreviousOutput) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "old", "createMarker(0.0)\n")).has_value());
  ASSERT_FALSE(readPublished(store, kDataset, "old").markers.empty());

  // Same id, different output topic.
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "new", "createMarker(0.0)\n")).has_value());

  EXPECT_FALSE(readPublished(store, kDataset, "new").markers.empty()) << "the new output is live";
  const std::optional<PJ::ObjectTopicId> old_id = store.findTopic(kDataset, PJ::sdk::markerObjectTopicName("old"));
  ASSERT_TRUE(old_id.has_value()) << "the tombstone keeps the topic registered";
  const std::optional<PJ::ResolvedObjectEntry> entry =
      store.latestAt(*old_id, std::numeric_limits<PJ::Timestamp>::max());
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(entry->payload.bytes.empty()) << "the abandoned output is emptied, not left stale";
}

// An unchanged upsert must NOT tombstone its own fresh output — the overlap between
// the previous and the new publish targets is exactly what the run just rewrote.
TEST(MarkerServiceTest, UpsertInPlaceKeepsItsOutputPublished) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(0.0)\n")).has_value());
  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/gen", "in", "createMarker(1.0)\n")).has_value());

  EXPECT_FALSE(readPublished(store, kDataset, "in").markers.empty());
}

// A merge folds the sources into the anchor and drops them. Without the rebind the
// recipe keeps naming a dataset that no longer exists: the merged blob stays on screen
// (nothing overwrites it) but the rule is dead, and the next recompute publishes an
// empty set onto a phantom topic instead of refreshing the anchor.
TEST(MarkerServiceTest, MergeRemapsConsumedRecipesOntoTheAnchor) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, DropRecipesForDatasetKeepsGlobalGenerators) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{kDataset}; });

  ASSERT_TRUE(service.upsertGenerator(markerRecipe("plug/bound", "in", "createMarker(0.0)\n")).has_value());
  auto global = markerRecipe("plug/global", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  global.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(global).has_value());
  ASSERT_EQ(service.recipes().size(), 2u);

  service.clearGeneratorsForDataset(kDataset);

  const std::vector<MarkerService::GeneratorRecipe> left = service.recipes();
  ASSERT_EQ(left.size(), 1u) << "the dataset-bound recipe is gone";
  EXPECT_EQ(left.front().id, "plug/global");
  EXPECT_TRUE(left.front().all_datasets);
}

// A recompute names the dataset whose data moved. An all_datasets generator must then
// republish THERE ONLY — re-running it over every loaded dataset is unbounded work on
// the hottest path in the app (one streaming tick per kPollPeriodMs), and a dataset
// that did not change would yield the identical set anyway.
TEST(MarkerServiceTest, ScopedRecomputeSkipsDatasetsThatDidNotChange) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());  // upsert lands on both

  const std::string topic = PJ::sdk::markerObjectTopicName(PJ::sdk::kGlobalMarkerTopic);
  // A republish mints a new entry uid; an untouched topic keeps its own. A missing
  // topic yields nullopt, which fails the comparisons below on its own.
  const auto publishUid = [&store, &topic](PJ::DatasetId dataset) -> std::optional<std::uint64_t> {
    const std::optional<PJ::ObjectTopicId> id = store.findTopic(dataset, topic);
    const std::optional<PJ::ResolvedObjectEntry> entry =
        id.has_value() ? store.latestAt(*id, std::numeric_limits<PJ::Timestamp>::max()) : std::nullopt;
    return entry.has_value() ? std::optional<std::uint64_t>{entry->sequential_uid.value} : std::nullopt;
  };
  const std::optional<std::uint64_t> before_ingesting = publishUid(1);
  const std::optional<std::uint64_t> before_idle = publishUid(2);
  ASSERT_TRUE(before_ingesting.has_value());
  ASSERT_TRUE(before_idle.has_value());

  // Dataset 1 ingested; dataset 2 sat still.
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, /*scope=*/1).empty());

  EXPECT_NE(publishUid(1), before_ingesting) << "the dataset that ingested is republished";
  EXPECT_EQ(publishUid(2), before_idle) << "an untouched dataset must not be recomputed";

  // Differential: the same call unscoped DOES reach dataset 2. Without this the test
  // would still pass if the scope argument were ignored and nothing republished at all.
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, MarkerService::kAllDatasets).empty());
  EXPECT_NE(publishUid(2), before_idle) << "unscoped, a global generator still lands everywhere";
}

// The same attribution applies to per-dataset generators: `changed` carries bare topic
// names, and two datasets routinely share them, so without the scope a stream tick on
// one dataset would re-run a generator bound to the other.
TEST(MarkerServiceTest, ScopedRecomputeIgnoresGeneratorsBoundElsewhere) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/gen", "in", "createMarker(0.0)\n");  // bound to kDataset (1)
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());

  // A change on dataset 2 names the same series key, but this generator reads dataset 1.
  EXPECT_TRUE(service.recomputeForChangedInputs({"in"}, /*scope=*/2).empty());
  // Unscoped, the caller cannot attribute the change, so the name match still wins.
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, MarkerService::kAllDatasets).empty());
}

// An ephemeral preview is published but excluded from recipes() (never persisted);
// remove() tears it down. Preview is now just create(ephemeral) + remove.
TEST(MarkerServiceTest, EphemeralPreviewPublishesAndRemoves) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(6));

  MarkerService::GeneratorRecipe recipe;
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
  EXPECT_TRUE(store.findTopic(kDataset, object_topic).has_value());  // published...
  EXPECT_TRUE(service.recipes().empty());                            // ...but not persisted

  EXPECT_TRUE(service.removeGenerator("plug/__preview__").has_value());
  EXPECT_FALSE(store.findTopic(kDataset, object_topic).has_value());  // topic removed
}

// H3: removing a PERSISTENT generator tombstones its output — the topic survives
// but decodes to an empty set (so a later merge/reload reads live-but-empty, not
// stale), unlike an ephemeral preview whose throwaway topic is removed outright.
TEST(MarkerServiceTest, RemovePersistentGeneratorTombstonesOutput) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(10));

  auto recipe = markerRecipe("plug/gen", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v)
  )");
  ASSERT_TRUE(service.upsertGenerator(recipe).has_value());
  EXPECT_FALSE(readPublished(store, kDataset, "in").markers.empty());  // has markers

  ASSERT_TRUE(service.removeGenerator("plug/gen").has_value());
  // Topic survives (tombstone), but its published blob is now empty: the empty set
  // serializes to zero bytes, so the overlay draws nothing. Checked at the store
  // level (not via deserialize, which treats an empty buffer as an error).
  const std::optional<PJ::ObjectTopicId> id = store.findTopic(kDataset, PJ::sdk::markerObjectTopicName("in"));
  ASSERT_TRUE(id.has_value());
  const std::optional<PJ::ResolvedObjectEntry> entry = store.latestAt(*id, PJ::Timestamp{0});
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->payload.bytes.size(), 0u);
}

// clearGenerators(RestoreIntent::kHistory) reconciles only the non-exempt persistent
// generators a history restore has authority over, leaving a history_exempt one
// (and any ephemeral preview) untouched — clearAllGenerators is the same
// predicate always removing.
TEST(MarkerServiceTest, PredicateClearRemovesOnlyNonExemptGenerators) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(4));

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

TEST(MarkerServiceTest, FindsExemptGeneratorDependingOnRemovedProcessorOutput) {
  PJ::ObjectStore store;
  MarkerService service(store, [](PJ::DatasetId, const std::string&) {
    MarkerService::ResolvedSeries series;
    series.timestamps = {0.0};
    series.values = {1.0};
    return std::optional<MarkerService::ResolvedSeries>{std::move(series)};
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
TEST(MarkerServiceTest, MergeMarkerTopicsConcatenatesAndShifts) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(10));

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

}  // namespace
