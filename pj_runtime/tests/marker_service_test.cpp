// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// MarkerService backs the unified pj.data_processors.v1 service (kind=markers — the object
// engine). These tests drive it against a synthetic resolver + a real ObjectStore —
// no SDK service, no catalog — proving the host-driven execution path end to end:
// resolve → run engine → serialize → publish to ObjectStore → read back.

#include <gtest/gtest.h>

#include <QObject>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "marker_test_utils.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/merge_result.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace {

using PJ::GeneratorKind;
using PJ::kAllDatasetsMarkerDataset;
using PJ::MarkerService;
using PJ::test::markerFamily;
using PJ::test::ownerPublishUid;
using PJ::test::ownerTombstoned;

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

// rampResolver(5), but only on the datasets in `datasets` — the all_datasets cases
// need "the rule's series exists on some datasets, not others".
MarkerService::SeriesResolver resolverOn(std::set<PJ::DatasetId> datasets) {
  return [datasets, ramp = rampResolver(5)](
             PJ::DatasetId dataset, const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
    return datasets.contains(dataset) ? ramp(dataset, key) : std::nullopt;
  };
}

// What a plot draws for `marker_topic` on `dataset`: the union of every family
// member (the bare topic plus one per owner). Empty when nothing is published.
PJ::sdk::PlotMarkers readPublished(PJ::ObjectStore& store, PJ::DatasetId dataset, const std::string& marker_topic) {
  return PJ::test::readMarkerUnion(store, dataset, marker_topic);
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
TEST(MarkerServiceTest, WithoutAResolverTheRunFailsInsteadOfPublishingAnEmptySet) {
  PJ::ObjectStore store;
  MarkerService service(store, MarkerService::SeriesResolver{});

  auto recipe = markerRecipe("plug/no_resolver", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t, s:at(0).v, {label="first"})
  )");

  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(markerFamily(store, kDataset, "in").empty());
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

  EXPECT_TRUE(service.recomputeForChangedInputs({"other"}, MarkerService::kAnyDataset).empty());

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, MarkerService::kAnyDataset);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(affected[0], PJ::markerOwnerTopicName("in", "plug/gen"));

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

// A global-across-all generator publishes ONCE, on the dataset-independent home of
// the ALL-DATASETS key — the "__global__" it sent is normalized host-side — and
// never copies its set onto the datasets it reads from.
TEST(MarkerServiceTest, GlobalAllDatasetsPublishesOnceOnTheSharedDataset) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
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
TEST(MarkerServiceTest, AllDatasetsOutputNormalizesGlobalToAll) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{kDataset}; });

  auto recipe = markerRecipe("plug/g", std::string(PJ::sdk::kGlobalMarkerTopic), "createMarker(0.0)\n");
  recipe.all_datasets = true;
  const PJ::Expected<std::vector<std::string>> ok = service.upsertGenerator(recipe);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? std::string{} : ok.error());
  EXPECT_EQ(ok->front(), PJ::markerOwnerTopicName(PJ::kAllDatasetsMarkerTopic, "plug/g"));
}

// A dataset-bound recipe (all_datasets false) may never target the reserved
// ALL-DATASETS key — that key's ownership is exclusively all_datasets generators.
TEST(MarkerServiceTest, AllTopicOnDatasetBoundRecipeIsRejected) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  auto recipe = markerRecipe("plug/g", std::string(PJ::kAllDatasetsMarkerTopic), "createMarker(0.0)\n");
  EXPECT_FALSE(service.upsertGenerator(recipe).has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A Dataset-scope rule and a Global-scope rule can target the same dataset without
// colliding: they normalize to different object topics ("__global__" vs "__all__").
TEST(MarkerServiceTest, DatasetAndAllScopesCoexistOnOneDataset) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
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

// Two generators on one marker topic each own their own object topic: the plot
// draws both sets, and retiring one tombstones only its topic — the other's is
// not even republished.
TEST(MarkerServiceTest, TwoGeneratorsOnOneTopicOwnTheirOwnObjectTopics) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, RecomputeOneGeneratorKeepsOthersPart) {
  PJ::ObjectStore store;
  MarkerService::SeriesResolver resolver = [](PJ::DatasetId,
                                              const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
    if (key != "in" && key != "other") {
      return std::nullopt;
    }
    MarkerService::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
  MarkerService service(store, resolver);

  auto in_recipe = markerRecipe("plug/in", "shared", "createMarker(0.0)\n");
  in_recipe.inputs = {"in"};
  ASSERT_TRUE(service.upsertGenerator(in_recipe).has_value());

  auto other_recipe = markerRecipe("plug/other", "shared", "createMarker(1.0)\n");
  other_recipe.inputs = {"other"};
  ASSERT_TRUE(service.upsertGenerator(other_recipe).has_value());

  ASSERT_EQ(readPublished(store, kDataset, "shared").markers.size(), 2u);

  const std::vector<std::string> affected = service.recomputeForChangedInputs({"in"}, kDataset);
  ASSERT_EQ(affected.size(), 1u);
  EXPECT_EQ(readPublished(store, kDataset, "shared").markers.size(), 2u)
      << "the 'in' owner's part is replaced, not appended";
}

// A retarget must drop only the retargeted generator's own part of the old
// target, never a sibling owner's part still publishing there.
TEST(MarkerServiceTest, UpsertRetargetDropsOnlyOwnPart) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, DirectWriteIsAnotherFamilyMember) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, UpsertRetargetTombstonesThePreviousOutput) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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

// A recompute names the dataset whose data moved. An all_datasets generator's set
// is one shared union, so a contributor's change republishes that one topic.
TEST(MarkerServiceTest, ScopedRecomputeOfGlobalRuleRepublishesTheSharedSet) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));
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
  EXPECT_NE(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"), before)
      << "the shared set is republished";
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
  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, MarkerService::kAnyDataset).empty());
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
  EXPECT_EQ(markerFamily(store, kDataset, object_topic).size(), 1u);  // published...
  EXPECT_TRUE(service.recipes().empty());                             // ...but not persisted

  EXPECT_TRUE(service.removeGenerator("plug/__preview__").has_value());
  EXPECT_TRUE(markerFamily(store, kDataset, object_topic).empty());  // topic removed, not tombstoned
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
  // The owner's topic survives (tombstone), but its published blob is now empty: the
  // empty set serializes to zero bytes, so the overlay draws nothing.
  EXPECT_TRUE(ownerTombstoned(store, kDataset, "in", "plug/gen"));
  EXPECT_TRUE(readPublished(store, kDataset, "in").markers.empty());
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

// Ordering fix: the anchor's PRE-MERGE content (here a foreign blob, pushed with no
// owning generator) must be folded before any source part is moved onto the same
// target, or the moved-in source part would look like "already has parts" to the
// anchor step and get shifted a second time. The anchor's blob is unshifted; the
// source's cached part is shifted by raw_shift_ns exactly once.
TEST(MarkerServiceTest, MergeAdoptsAnchorForeignBlobAndShiftsSourcesOnce) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, MergeThenRecomputeOneGeneratorKeepsOthers) {
  PJ::ObjectStore store;
  MarkerService::SeriesResolver resolver = [](PJ::DatasetId,
                                              const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
    if (key != "in" && key != "other") {
      return std::nullopt;
    }
    MarkerService::ResolvedSeries s;
    for (std::size_t i = 0; i < 5; ++i) {
      s.timestamps.push_back(static_cast<double>(i));
      s.values.push_back(static_cast<double>(i));
    }
    return s;
  };
  MarkerService service(store, resolver);

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
  ASSERT_EQ(affected.size(), 1u) << "only A ('in') re-runs";
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
TEST(MarkerServiceTest, DatasetRuleOnSecondDatasetKeepsTheFirst) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, ReapplyOnSameDatasetReplaces) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, RemoveRetiresTheIdOnEveryDataset) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, MergeCollapsesSameIdRecipesOntoTheAnchor) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, RemoveAfterMergeRetiresEveryTopicTheIdOwns) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
MarkerService::GeneratorRecipe globalRuleReadingIn() {
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
TEST(MarkerServiceTest, GlobalRuleIsPublishedOnceOnTheSharedDataset) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
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
TEST(MarkerServiceTest, GlobalRuleUnionsEveryContributor) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1, 3}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  EXPECT_EQ(
      readPublished(store, kAllDatasetsMarkerDataset, std::string(PJ::kAllDatasetsMarkerTopic)).markers.size(), 2u);
}

// An all_datasets recipe whose series exists on NO loaded dataset has nothing to
// compute: rejected, nothing published, nothing stored.
TEST(MarkerServiceTest, GlobalRuleWithNoInputsAnywhereIsRejected) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(globalRuleReadingIn());
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
  EXPECT_TRUE(markerFamily(store, kAllDatasetsMarkerDataset, PJ::kAllDatasetsMarkerTopic).empty());
}

// A dataset-bound recipe never skips: a missing input on ITS dataset is a plain
// user error.
TEST(MarkerServiceTest, BoundRuleWithMissingInputStillErrors) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({}));

  auto recipe = markerRecipe("plug/g", "in", R"(
    local s = series("in")
    createMarker(s:at(0).t)
  )");
  recipe.dataset_id = kDataset;
  const PJ::Expected<std::vector<std::string>> res = service.upsertGenerator(recipe);
  EXPECT_FALSE(res.has_value());
  EXPECT_TRUE(service.recipes().empty());
}

// A dataset loaded AFTER an all_datasets recipe was applied, without the series the
// rule reads, needs no work at all: the set lives once on the shared dataset, so an
// unrelated recompute republishes nothing and the roster hook re-runs the rule once
// (it cannot know the newcomer has no inputs without running) with no duplication.
TEST(MarkerServiceTest, ADatasetLoadedLaterWithoutTheInputsNeedsNoCopy) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  const std::optional<std::uint64_t> before = ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g");
  ASSERT_TRUE(before.has_value());

  // Dataset 2 loads after the rule was applied, without the series the rule reads.
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  EXPECT_TRUE(service.recomputeForChangedInputs({"/unrelated"}, MarkerService::kAnyDataset).empty());
  EXPECT_EQ(ownerPublishUid(store, kAllDatasetsMarkerDataset, all_topic, "plug/g"), before) << "nothing to copy";
  EXPECT_TRUE(markerFamily(store, 2, all_topic).empty());

  EXPECT_EQ(service.reachNewDatasets().size(), 1u) << "the catalog grew: the rule is re-run";
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u) << "no duplication";
}

// A Global set is stored in the DISPLAY frame (raw − the contributor's alignment
// offset), so a marker found on a dataset with another clock sits at the same
// display instant as one found on an aligned dataset, with no per-dataset copy.
TEST(MarkerServiceTest, GlobalRuleIsStoredInTheDisplayFrame) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
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
  std::vector<PJ::Timestamp> times;
  for (const auto& marker : readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers) {
    times.push_back(marker.t_start);
  }
  std::sort(times.begin(), times.end());
  EXPECT_EQ(times, (std::vector<PJ::Timestamp>{-10, 5}));
}

// A dataset loaded later WITH the series is a new contributor: its topics arrive in a
// `changed` batch, the union is recomputed and grows on every dataset.
TEST(MarkerServiceTest, GlobalRuleGrowsWhenAContributorLoadsLater) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });

  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());

  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2, 3}; });
  service.setResolver(resolverOn({1, 3}));

  EXPECT_FALSE(service.recomputeForChangedInputs({"in"}, /*scope=*/3).empty());
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// With declared (qualified) names, the script enumerates each series once under
// the spelling it declared, and each name reads its own series — even when a
// declared name equals another input's bare key ("s:x" is both).
TEST(MarkerServiceTest, DeclaredNamesAreEnumeratedOnceEachAndReadTheirOwnSeries) {
  PJ::ObjectStore store;
  MarkerService service(
      store, [](PJ::DatasetId, const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
        if (key != "x" && key != "s:x") {
          return std::nullopt;
        }
        MarkerService::ResolvedSeries s;
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
TEST(MarkerServiceTest, UpsertRejectsADeclaredInputCountMismatch) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(3));
  auto recipe = markerRecipe("plug/gen", "out", "createMarker(0.0)\n");
  recipe.declared_inputs = {"in", "extra"};
  const PJ::Expected<std::vector<std::string>> rejected = service.upsertGenerator(recipe);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().find("declared"), std::string::npos) << rejected.error();
  EXPECT_TRUE(service.recipes().empty());
}

}  // namespace

// history_exempt and ephemeral are per-id properties (clearGenerators, removeGenerator
// and generatorIds all read the first binding), so a sibling binding that disagrees
// would let an undo delete an exempt rule, or a replay skip a plain one.
TEST(MarkerServiceTest, UpsertRejectsABindingWhoseFlagsDifferFromItsSiblings) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

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
TEST(MarkerServiceTest, ChangingScopeReplacesTheWholeId) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1, 2}));
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
TEST(MarkerServiceTest, RemovingAContributorShrinksTheGlobalSetOnSurvivors) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1, 2}));
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
TEST(MarkerServiceTest, RemovingTheLastContributorClearsTheGlobalSetEverywhere) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
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
TEST(MarkerServiceTest, MergeLeavesTheSharedGlobalSetAlone) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
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
TEST(MarkerServiceTest, ReachNewDatasetsRerunsGlobalRulesForANewContributor) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1, 2}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  EXPECT_EQ(service.reachNewDatasets().size(), 1u);
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// The catalog grows topic by topic: a dataset already listed may only now expose the
// series a Global rule reads, so the hook re-runs on every catalog addition rather
// than on a dataset-roster change.
TEST(MarkerServiceTest, ReachNewDatasetsRerunsWhenAContributorAppearsUnderTheSameRoster) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
  service.setDatasetLister([] { return std::vector<PJ::DatasetId>{1, 2}; });
  ASSERT_TRUE(service.upsertGenerator(globalRuleReadingIn()).has_value());
  const std::string all_topic(PJ::kAllDatasetsMarkerTopic);
  ASSERT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 1u);

  service.setResolver(resolverOn({1, 2}));  // dataset 2's series just reached the catalog
  EXPECT_EQ(service.reachNewDatasets().size(), 1u);
  EXPECT_EQ(readPublished(store, kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u);
}

// A recompute that finds no contributor left for an existing Global rule (the last
// one was replaced by data without its inputs) empties the set instead of leaving
// the stale findings on screen; only the initial upsert treats it as an error.
TEST(MarkerServiceTest, RecomputeWithNoContributorLeftClearsTheSharedSet) {
  PJ::ObjectStore store;
  MarkerService service(store, resolverOn({1}));
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
TEST(MarkerServiceTest, UpsertRejectsTheOwnerSeparatorInIdsAndOutputs) {
  PJ::ObjectStore store;
  MarkerService service(store, rampResolver(5));

  EXPECT_FALSE(service.upsertGenerator(markerRecipe("plug/a#b", "in", "createMarker(0.0)\n")).has_value());
  EXPECT_FALSE(service.upsertGenerator(markerRecipe("plug/a", "in#x", "createMarker(0.0)\n")).has_value());
  EXPECT_TRUE(service.recipes().empty());
  EXPECT_TRUE(store.listTopics().empty()) << "rejected before publishing";
}

// Production ordering: a dataset's commit recompute runs BEFORE the catalog (the
// dataset lister and the series resolver) publishes it, so no recompute ever sees
// the new dataset — the catalog publication itself must re-run every Global rule.
TEST(MarkerServiceTest, ANewlyCatalogedContributorJoinsTheGlobalSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  MarkerService& service = session.markerService();
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

  EXPECT_EQ(readPublished(session.objectStore(), kAllDatasetsMarkerDataset, all_topic).markers.size(), 2u)
      << "the newcomer's contribution joins the shared set";
  EXPECT_GE(repaints, 1) << "overlays are told to repaint";
}

// A refill publishes markers for the data it attempted; a rollback restores the
// dataset but the shared set lives outside it, so the rollback must re-run the
// generators over the restored data.
TEST(MarkerServiceTest, RefillRollbackRecomputesTheSharedSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  MarkerService& service = session.markerService();
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
    service.setResolver([](PJ::DatasetId, const std::string& key) -> std::optional<MarkerService::ResolvedSeries> {
      if (key != "in") {
        return std::nullopt;
      }
      MarkerService::ResolvedSeries series;
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
TEST(MarkerServiceTest, ASourceTimelineDragRepublishesNoMarkerSet) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  MarkerService& service = session.markerService();
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
