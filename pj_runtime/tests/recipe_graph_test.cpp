// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pj_runtime/RecipeGraph.h"
#include "pj_scripting/python_object_script.h"
#ifdef PJ_PYTHON_TEST_PROBE
#include "python_test_probe.h"
#endif

namespace PJ {
namespace {

using scripting::OutputType;
using Candidate = RecipeGraph::InputCandidate;

constexpr DatasetId kDataset = 1;  ///< source name "s"
constexpr DatasetId kOther = 2;    ///< source name "other"
constexpr Timestamp kSec = 1'000'000'000;

/// A graph over a fake catalog: each input name maps to the datasets it resolves in. The fake
/// does what the session's resolver does with a dataset qualifier: "s:cloud" resolves in "s" only,
/// as the bare topic "cloud".
struct Fixture {
  Fixture()
      : graph([this](std::string_view input) {
          const auto found = catalog.find(std::string(input));
          return found == catalog.end() ? std::vector<Candidate>{} : found->second;
        }) {}

  std::map<std::string, std::vector<Candidate>> catalog{
      {"cloud", {{kDataset, sdk::BuiltinObjectType::kPointCloud, "cloud"}}},
      {"threshold", {{kDataset, sdk::BuiltinObjectType::kNone, "threshold"}}},
      {"s:cloud", {{kDataset, sdk::BuiltinObjectType::kPointCloud, "cloud"}}},
      {"other:cloud", {{kOther, sdk::BuiltinObjectType::kPointCloud, "cloud"}}},
  };
  RecipeGraph graph;
};

/// `owner/<id>`: crops `cloud` by `threshold` into the point-cloud output `cropped`.
RecipeGraph::Recipe cropRecipe(std::string id = "crop") {
  RecipeGraph::Recipe recipe;
  recipe.owner_plugin = "owner";
  recipe.user_id = std::move(id);
  recipe.inputs = {"cloud", "threshold"};
  recipe.outputs = {{"cropped", OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}};
  recipe.script = "local inputs = ...\nreturn {cropped = inputs.cloud}\n";
  return recipe;
}

/// `owner/<id>`: reads the one object `input` (another node's output) and passes it on as `cropped`.
RecipeGraph::Recipe chainedRecipe(std::string id, const std::string& input) {
  RecipeGraph::Recipe recipe = cropRecipe(std::move(id));
  recipe.inputs = {input};
  recipe.script = "local inputs = ...\nreturn {cropped = inputs[\"" + input + "\"]}\n";
  return recipe;
}

bool contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// A Python recipe compiles once per revision into the opaque handle its runs share: an identical upsert keeps it, an
// edit compiles another; a script the Python checker refuses (a syntax error, Luau source) leaves the node as it was,
// and a language no runner has is named. A build without Python refuses it with the reason.
TEST(RecipeGraphTest, PythonScriptIsCompiledOncePerRevision) {
  Fixture fx;
  auto python = cropRecipe();
  python.language = "python";
  python.script = "def evaluate(inputs, params):\n    return {}\n";
  const Status installed = fx.graph.upsert(python);
  if (!scripting::pythonAvailable()) {
    ASSERT_FALSE(installed.has_value());
    EXPECT_TRUE(contains(installed.error(), "python is not available")) << installed.error();
    return;
  }
  ASSERT_TRUE(installed.has_value()) << installed.error();
  const RecipeGraph::Node* node = fx.graph.find(python.key());
  ASSERT_NE(node, nullptr);
  const uint64_t revision = node->revision;
  const void* compiled = node->compiled.get();
  ASSERT_NE(compiled, nullptr);

#ifdef PJ_PYTHON_TEST_PROBE
  const int compiles = test::pythonCompileEvents();
#endif
  ASSERT_TRUE(fx.graph.upsert(python).has_value());
#ifdef PJ_PYTHON_TEST_PROBE
  EXPECT_EQ(test::pythonCompileEvents(), compiles) << "an identical upsert compiles nothing";
#endif
  EXPECT_EQ(fx.graph.find(python.key())->revision, revision);
  EXPECT_EQ(fx.graph.find(python.key())->compiled.get(), compiled) << "an identical upsert keeps the compiled script";

  for (const char* refused : {"def evaluate(inputs, params:\n    return {}\n", "local inputs = ...\nreturn {}\n"}) {
    auto broken = python;
    broken.script = refused;
    EXPECT_FALSE(fx.graph.upsert(broken).has_value()) << refused;
    EXPECT_EQ(fx.graph.find(python.key())->compiled.get(), compiled) << refused;
  }
  auto other = python;
  other.language = "ruby";
  const Status unknown = fx.graph.upsert(other);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_TRUE(contains(unknown.error(), "unsupported language 'ruby'")) << unknown.error();

  auto edited = python;
  edited.script = "def evaluate(inputs, params):\n    return {'x': 1}\n";
  ASSERT_TRUE(fx.graph.upsert(edited).has_value());
#ifdef PJ_PYTHON_TEST_PROBE
  EXPECT_GT(test::pythonCompileEvents(), compiles) << "an edit compiles its new source";
#endif
  EXPECT_GT(fx.graph.find(python.key())->revision, revision);
  EXPECT_NE(fx.graph.find(python.key())->compiled.get(), compiled);
}

// REC-02 (and the recipe-level half of REC-04): a bad recipe is refused, and nothing is installed.
TEST(RecipeGraphTest, RejectsBadRecipes) {
  Fixture fx;
  const auto rejected = [&fx](RecipeGraph::Recipe recipe) { return !fx.graph.upsert(std::move(recipe)).has_value(); };

  auto unknown_language = cropRecipe();
  unknown_language.language = "ruby";
  EXPECT_TRUE(rejected(unknown_language));
  auto syntax_error = cropRecipe();
  syntax_error.script = "return {";
  EXPECT_TRUE(rejected(syntax_error));
  auto no_owner = cropRecipe();
  no_owner.owner_plugin.clear();
  EXPECT_TRUE(rejected(no_owner));
  auto no_outputs = cropRecipe();
  no_outputs.outputs.clear();
  EXPECT_TRUE(rejected(no_outputs)) << "an installed recipe declares its outputs (REC-04)";
  auto duplicate_outputs = cropRecipe();
  duplicate_outputs.outputs.push_back(duplicate_outputs.outputs.front());
  EXPECT_TRUE(rejected(duplicate_outputs));
  auto colon_output = cropRecipe();
  colon_output.outputs.front().name = "bad:name";
  EXPECT_TRUE(rejected(colon_output));
  auto unproducible = cropRecipe();
  unproducible.outputs.front().object_type = sdk::BuiltinObjectType::kOccupancyGrid;
  EXPECT_TRUE(rejected(unproducible)) << "only a type a script can produce";
  auto self_input = cropRecipe();
  self_input.inputs.push_back("cropped");
  EXPECT_TRUE(rejected(self_input)) << "an input named like one of the recipe's outputs";

  auto unknown_input = cropRecipe();
  unknown_input.inputs = {"cloud", "does_not_exist"};
  const Status unknown = fx.graph.upsert(unknown_input);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_TRUE(contains(unknown.error(), "unknown input 'does_not_exist'")) << unknown.error();

  // A second dataset carrying the same names leaves two datasets that bind every input (D7).
  fx.catalog["cloud"].push_back({kOther, sdk::BuiltinObjectType::kPointCloud, "cloud"});
  fx.catalog["threshold"].push_back({kOther, sdk::BuiltinObjectType::kNone, "threshold"});
  const Status ambiguous = fx.graph.upsert(cropRecipe());
  ASSERT_FALSE(ambiguous.has_value());
  EXPECT_TRUE(contains(ambiguous.error(), "more than one dataset")) << ambiguous.error();

  EXPECT_EQ(fx.graph.find("owner/crop"), nullptr) << "a rejected upsert installs nothing";
}

// REC-03
TEST(RecipeGraphTest, RejectsAnObjectInputScriptsCannotReadYet) {
  Fixture fx;
  fx.catalog["grid"] = {{kDataset, sdk::BuiltinObjectType::kOccupancyGrid, "grid"}};  // no field table
  auto recipe = cropRecipe();
  recipe.inputs = {"grid", "threshold"};
  const Status result = fx.graph.upsert(recipe);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(contains(result.error(), "input 'grid' has type")) << result.error();
  EXPECT_TRUE(contains(result.error(), "scripts cannot read yet")) << result.error();
  EXPECT_EQ(fx.graph.find(recipe.key()), nullptr);
}

// REC-05
TEST(RecipeGraphTest, QualifiedInputsBindTheSelectedDatasetAndPreserveAliases) {
  Fixture fx;
  auto recipe = cropRecipe();
  recipe.inputs = {"s:cloud"};
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const RecipeGraph::Node* node = fx.graph.find(recipe.key());
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->dataset, kDataset);
  EXPECT_EQ(node->recipe.inputs, std::vector<std::string>{"s:cloud"}) << "the script reads inputs['s:cloud']";
  ASSERT_EQ(node->bindings.size(), 1u);
  EXPECT_EQ(node->bindings.front().object_type, sdk::BuiltinObjectType::kPointCloud);
  EXPECT_EQ(node->bindings.front().name, "cloud") << "the topic the input reads inside its dataset";

  recipe.inputs.push_back("other:cloud");
  const Status spanning = fx.graph.upsert(recipe);
  ASSERT_FALSE(spanning.has_value()) << "one node reads one dataset (D7)";
  EXPECT_TRUE(contains(spanning.error(), "one dataset")) << spanning.error();
  EXPECT_EQ(fx.graph.find(recipe.key())->recipe.inputs.size(), 1u) << "the installed node is untouched";
}

// GAP-31: a recipe naming its dataset binds every input there: a name two datasets carry is no longer ambiguous, and
// an input its dataset does not carry is refused, naming both.
TEST(RecipeGraphTest, ARecipeDatasetBindsItsInputsThereOnly) {
  Fixture fx;
  fx.catalog["cloud"].push_back({kOther, sdk::BuiltinObjectType::kPointCloud, "cloud"});
  auto shared = chainedRecipe("shared", "cloud");
  ASSERT_FALSE(fx.graph.upsert(shared).has_value()) << "carried by two datasets: ambiguous";
  shared.dataset = kOther;
  ASSERT_TRUE(fx.graph.upsert(shared).has_value());
  EXPECT_EQ(fx.graph.find("owner/shared")->dataset, kOther);

  auto elsewhere = cropRecipe("elsewhere");  // `threshold` is on kDataset only
  elsewhere.dataset = kOther;
  const Status refused = fx.graph.upsert(elsewhere);
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(contains(refused.error(), "input 'threshold' is not on dataset 2, which the recipe binds"))
      << refused.error();
  EXPECT_EQ(fx.graph.find("owner/elsewhere"), nullptr);
}

// LAY-14: the exemption of an installed key never changes, so no history snapshot holds it under the other authority;
// a recipe changes it by being removed and created again.
TEST(RecipeGraphTest, HistoryExemptionCannotChangeOnAnInstalledRecipe) {
  Fixture fx;
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  auto exempt = cropRecipe();
  exempt.history_exempt = true;
  const Status promoted = fx.graph.upsert(exempt);
  ASSERT_FALSE(promoted.has_value());
  EXPECT_TRUE(contains(promoted.error(), "history_exempt cannot change")) << promoted.error();
  EXPECT_FALSE(fx.graph.find("owner/crop")->recipe.history_exempt);

  ASSERT_TRUE(fx.graph.remove("owner/crop").has_value());
  ASSERT_TRUE(fx.graph.upsert(exempt).has_value()) << "created again, exempt";
  EXPECT_FALSE(fx.graph.upsert(cropRecipe()).has_value()) << "nor demoted";
  EXPECT_TRUE(fx.graph.find("owner/crop")->recipe.history_exempt);
}

// DAG-03
TEST(RecipeGraphTest, CycleAndOwnOutputRejectionLeaveTheGraphUntouched) {
  Fixture fx;
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("child", "owner/crop/cropped")).has_value());
  const RecipeGraph::Node* child = fx.graph.find("owner/child");
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->dataset, kDataset) << "an upstream output resolves in its producer's dataset";
  EXPECT_EQ(child->bindings.front().upstream, "owner/crop");
  EXPECT_EQ(child->bindings.front().name, "cropped") << "the upstream's output it reads";
  const uint64_t child_revision = child->revision;

  const auto cyclic = chainedRecipe("crop", "owner/child/cropped");
  const Status rejected = fx.graph.upsert(cyclic);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_TRUE(contains(rejected.error(), "cycle")) << rejected.error();
  EXPECT_EQ(fx.graph.find("owner/crop")->revision, 1u) << "a rejected upsert takes no revision";
  EXPECT_EQ(fx.graph.find("owner/crop")->recipe.inputs, cropRecipe().inputs);

  const Status self = fx.graph.upsert(chainedRecipe("self", "owner/self/cropped"));
  ASSERT_FALSE(self.has_value());
  EXPECT_TRUE(contains(self.error(), "own output")) << self.error();
  EXPECT_EQ(fx.graph.find("owner/self"), nullptr);

  // Replacing the reader with a raw-input recipe removes its edge, so the former cycle installs.
  ASSERT_TRUE(fx.graph.upsert(cropRecipe("child")).has_value());
  EXPECT_GT(fx.graph.find("owner/child")->revision, child_revision) << "an edit takes a new revision";
  EXPECT_TRUE(fx.graph.upsert(cyclic).has_value());
}

// A revision is graph-wide and monotonic, and changes only when the node's meaning does.
TEST(RecipeGraphTest, RevisionChangesOnlyWithItsMeaningAndIsNeverReused) {
  Fixture fx;
  auto recipe = cropRecipe();
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const uint64_t first = fx.graph.find(recipe.key())->revision;
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  EXPECT_EQ(fx.graph.find(recipe.key())->revision, first) << "an identical upsert changes nothing";

  recipe.params_json = R"({"zmin": -5})";
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const uint64_t edited = fx.graph.find(recipe.key())->revision;
  EXPECT_GT(edited, first);

  ASSERT_TRUE(fx.graph.upsert(cropRecipe("other")).has_value());
  ASSERT_TRUE(fx.graph.remove(recipe.key()).has_value());
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  EXPECT_GT(fx.graph.find(recipe.key())->revision, fx.graph.find("owner/other")->revision)
      << "a reinstalled key never reuses a revision: a result of the removed node is never current";
}

// DAG-05: the graph half (the node states and results are the scheduler's).
TEST(RecipeGraphTest, RemovedUpstreamLeavesItsReadersMissingAndRelinksByName) {
  Fixture fx;
  auto source = cropRecipe();
  ASSERT_TRUE(fx.graph.upsert(source).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("child", "owner/crop/cropped")).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("grandchild", "owner/child/cropped")).has_value());
  EXPECT_FALSE(fx.graph.missingInput("owner/grandchild").has_value());

  ASSERT_TRUE(fx.graph.remove(source.key()).has_value());
  ASSERT_NE(fx.graph.find("owner/grandchild"), nullptr) << "a removal does not cascade";
  const auto removed = fx.graph.missingInput("owner/grandchild");
  ASSERT_TRUE(removed.has_value()) << "transitively";
  EXPECT_TRUE(contains(*removed, "removed")) << *removed;
  EXPECT_TRUE(contains(fx.graph.missingInput("owner/child").value_or(""), "removed"));

  ASSERT_TRUE(fx.graph.upsert(source).has_value());
  EXPECT_FALSE(fx.graph.missingInput("owner/grandchild").has_value()) << "relinked by name";

  source.outputs.front().name = "renamed";
  source.script = "local inputs = ...\nreturn {renamed = inputs.cloud}\n";
  ASSERT_TRUE(fx.graph.upsert(source).has_value());
  const auto renamed = fx.graph.missingInput("owner/child");
  ASSERT_TRUE(renamed.has_value());
  EXPECT_TRUE(contains(*renamed, "no longer provides")) << *renamed;
  EXPECT_TRUE(fx.graph.missingInput("owner/grandchild").has_value());
}

// DAG-10
TEST(RecipeGraphTest, DependentsOfWalksReadersTransitively) {
  Fixture fx;
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("mid", "owner/crop/cropped")).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("tail", "owner/mid/cropped")).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("branch", "owner/crop/cropped")).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("other", "cloud")).has_value());

  EXPECT_EQ(
      fx.graph.dependentsOf({"owner/crop"}), (std::vector<std::string>{"owner/branch", "owner/mid", "owner/tail"}));
  EXPECT_EQ(fx.graph.dependentsOf({"owner/mid"}), std::vector<std::string>{"owner/tail"});
  EXPECT_TRUE(fx.graph.dependentsOf({"owner/tail"}).empty());
  EXPECT_TRUE(fx.graph.dependentsOf({"owner/other"}).empty());
  EXPECT_TRUE(fx.graph.dependentsOf({"owner/unknown"}).empty());
}

// LAY-02
TEST(RecipeGraphTest, ClearRecipesHonoursHistoryIntent) {
  Fixture fx;
  auto exempt = cropRecipe("exempt");
  exempt.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(exempt).has_value());
  ASSERT_TRUE(fx.graph.upsert(cropRecipe("ordinary")).has_value());
  ASSERT_TRUE(fx.graph.hasHistoryExemptRecipes());

  fx.graph.clearRecipes(RestoreIntent::kHistory);
  EXPECT_NE(fx.graph.find(exempt.key()), nullptr) << "history has no authority over an exempt node";
  EXPECT_EQ(fx.graph.find("owner/ordinary"), nullptr);

  fx.graph.clearRecipes(RestoreIntent::kReplace);
  EXPECT_EQ(fx.graph.find(exempt.key()), nullptr) << "kReplace clears everything, exempt included";
  EXPECT_FALSE(fx.graph.hasHistoryExemptRecipes());
}

// A restore keeps the unchanged nodes as they are and clears the rest.
TEST(RecipeGraphTest, ClearRecipesLeavesTheKeptNodesAsTheyAre) {
  for (const RestoreIntent intent : {RestoreIntent::kHistory, RestoreIntent::kReplace}) {
    Fixture fx;
    ASSERT_TRUE(fx.graph.upsert(cropRecipe("kept")).has_value());
    ASSERT_TRUE(fx.graph.upsert(cropRecipe("cleared")).has_value());
    const uint64_t revision = fx.graph.find("owner/kept")->revision;

    fx.graph.clearRecipes(intent, {"owner/kept"});
    ASSERT_NE(fx.graph.find("owner/kept"), nullptr);
    EXPECT_EQ(fx.graph.find("owner/kept")->revision, revision);
    EXPECT_EQ(fx.graph.find("owner/cleared"), nullptr);
  }
}

// LAY-03
TEST(RecipeGraphTest, UnchangedRecipeKeysMatchFieldByField) {
  Fixture fx;
  const auto recipe = cropRecipe();
  ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  const std::unordered_set<std::string> just_it{recipe.key()};

  EXPECT_EQ(fx.graph.unchangedRecipeKeys({recipe}, RestoreIntent::kReplace), just_it);
  auto crlf = recipe;
  crlf.script = "local inputs = ...\r\nreturn {cropped = inputs.cloud}\r\n";
  EXPECT_EQ(fx.graph.unchangedRecipeKeys({crlf}, RestoreIntent::kReplace), just_it) << "line endings ignored";

  auto changed_params = recipe;
  changed_params.params_json = R"({"zmin": -5})";
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({changed_params}, RestoreIntent::kReplace).empty());
  auto changed_clock = recipe;
  changed_clock.query_time = false;
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({changed_clock}, RestoreIntent::kReplace).empty());
  auto changed_output = recipe;
  changed_output.outputs.front().object_type = sdk::BuiltinObjectType::kSceneEntities;
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({changed_output}, RestoreIntent::kReplace).empty());
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({}, RestoreIntent::kReplace).empty()) << "not wanted at all";

  auto exempt = cropRecipe("exempt");
  exempt.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(exempt).has_value());
  EXPECT_EQ(fx.graph.unchangedRecipeKeys({recipe, exempt}, RestoreIntent::kHistory), just_it)
      << "a live history-exempt node is outside kHistory's authority";
}

// The rule BatchEngine applies to its inputs: a recipe the snapshot leaves as it is still runs again
// when the restore re-creates or removes the node it reads.
TEST(RecipeGraphTest, AnUnchangedRecipeWhoseUpstreamIsRecreatedIsNotUnchanged) {
  Fixture fx;
  const auto source = cropRecipe();
  const auto child = chainedRecipe("child", "owner/crop/cropped");
  const auto grandchild = chainedRecipe("grandchild", "owner/child/cropped");
  for (const auto& recipe : {source, child, grandchild}) {
    ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  }
  EXPECT_EQ(
      fx.graph.unchangedRecipeKeys({source, child, grandchild}, RestoreIntent::kReplace),
      (std::unordered_set<std::string>{source.key(), child.key(), grandchild.key()}));

  auto edited = source;
  edited.params_json = R"({"zmin": -5})";
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({edited, child, grandchild}, RestoreIntent::kReplace).empty())
      << "the edited source is re-created, so are its readers, transitively";
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({child, grandchild}, RestoreIntent::kReplace).empty())
      << "the restore removes the source";

  ASSERT_TRUE(fx.graph.remove(source.key()).has_value());
  EXPECT_TRUE(fx.graph.unchangedRecipeKeys({source, child, grandchild}, RestoreIntent::kReplace).empty())
      << "the restore creates the missing source";
  EXPECT_EQ(
      fx.graph.unchangedRecipeKeys({child, grandchild}, RestoreIntent::kReplace),
      (std::unordered_set<std::string>{child.key(), grandchild.key()}))
      << "nothing re-creates a source neither live nor wanted";

  // A history snapshot omits the exempt source, which history leaves live: its reader is kept.
  auto exempt_source = cropRecipe();
  exempt_source.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(exempt_source).has_value());
  EXPECT_EQ(
      fx.graph.unchangedRecipeKeys({child, grandchild}, RestoreIntent::kHistory),
      (std::unordered_set<std::string>{child.key(), grandchild.key()}));
}

// LAY-04
TEST(RecipeGraphTest, HistoryExemptionProtectsTransitiveRawAndDerivedAncestors) {
  Fixture fx;
  auto finding = chainedRecipe("finding", "owner/child/cropped");
  finding.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("child", "owner/crop/cropped")).has_value());
  ASSERT_TRUE(fx.graph.upsert(finding).has_value());

  const std::vector<std::string> stranded{"owner/finding/cropped"};
  EXPECT_EQ(fx.graph.exemptDependentsOf({"cloud"}), stranded) << "a raw ancestor";
  EXPECT_EQ(fx.graph.exemptDependentsOf({"owner/crop/cropped"}), stranded) << "a derived ancestor";
  EXPECT_TRUE(fx.graph.exemptDependentsOf({"unrelated"}).empty());
  EXPECT_TRUE(fx.graph.exemptDependentsOf({"clo"}).empty()) << "slash-bounded: 'clo' is not 'cloud'";
}

// A dataset-qualified input reads the bare topic, so removing that topic strands the exempt reader.
TEST(RecipeGraphTest, ExemptDependentsMatchTheBareTopicOfAQualifiedInput) {
  Fixture fx;
  auto exempt = cropRecipe("exempt");
  exempt.inputs = {"s:cloud"};
  exempt.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(exempt).has_value());
  EXPECT_EQ(fx.graph.exemptDependentsOf({"cloud"}), std::vector<std::string>{"owner/exempt/cropped"});
}

// OWN-01: a preview survives a restore of either intent and ends with the plugin instance that made it.
TEST(RecipeGraphTest, PreviewsSurviveClearRecipesAndEndWithTheirInstance) {
  for (const RestoreIntent intent : {RestoreIntent::kHistory, RestoreIntent::kReplace}) {
    Fixture fx;
    auto preview = cropRecipe("preview");
    preview.ephemeral = true;
    auto sibling = cropRecipe("sibling");
    sibling.ephemeral = true;
    ASSERT_TRUE(fx.graph.upsert(preview).has_value());
    ASSERT_TRUE(fx.graph.upsert(sibling).has_value());
    ASSERT_TRUE(fx.graph.upsert(cropRecipe("ordinary")).has_value());

    fx.graph.clearRecipes(intent);
    EXPECT_NE(fx.graph.find(preview.key()), nullptr) << "the preview survives the clear";
    EXPECT_EQ(fx.graph.find("owner/ordinary"), nullptr);
    EXPECT_TRUE(fx.graph.unchangedRecipeKeys({preview}, intent).empty()) << "a preview is never the restore's";

    ASSERT_TRUE(fx.graph.upsert(cropRecipe("ordinary")).has_value());
    EXPECT_TRUE(fx.graph.removeEphemerals({preview.key(), "owner/ordinary", "owner/gone"}));
    EXPECT_EQ(fx.graph.find(preview.key()), nullptr) << "its instance ended it";
    EXPECT_NE(fx.graph.find(sibling.key()), nullptr) << "another instance's preview of the same plugin stays";
    EXPECT_NE(fx.graph.find("owner/ordinary"), nullptr) << "a persistent recipe is the user's data";
    EXPECT_FALSE(fx.graph.removeEphemerals({preview.key()}));
  }
}

// OWN-16
TEST(RecipeGraphTest, PersistentRecipesCannotReadAPreviewOutput) {
  Fixture fx;
  auto preview = cropRecipe("preview");
  preview.ephemeral = true;
  ASSERT_TRUE(fx.graph.upsert(preview).has_value());

  const Status persistent = fx.graph.upsert(chainedRecipe("kept", "owner/preview/cropped"));
  ASSERT_FALSE(persistent.has_value());
  EXPECT_TRUE(contains(persistent.error(), "preview 'owner/preview'")) << persistent.error();
  auto chained_preview = chainedRecipe("chained", "owner/preview/cropped");
  chained_preview.ephemeral = true;
  ASSERT_TRUE(fx.graph.upsert(chained_preview).has_value()) << "a preview may read a preview";
  ASSERT_TRUE(fx.graph.upsert(cropRecipe("user")).has_value());

  ASSERT_TRUE(fx.graph.removeEphemerals({preview.key()}));
  EXPECT_NE(fx.graph.find("owner/user"), nullptr);
  EXPECT_EQ(fx.graph.find("owner/kept"), nullptr);
}

// OWN-16, the other direction: a recipe a persistent one reads may not turn into a preview, by an
// edit or by a reinstall after its removal (the reader keeps naming it).
TEST(RecipeGraphTest, APreviewCannotBecomeTheUpstreamOfAPersistentReader) {
  Fixture fx;
  auto source = cropRecipe();
  ASSERT_TRUE(fx.graph.upsert(source).has_value());
  ASSERT_TRUE(fx.graph.upsert(chainedRecipe("reader", "owner/crop/cropped")).has_value());
  auto as_preview = source;
  as_preview.ephemeral = true;

  const Status edited = fx.graph.upsert(as_preview);
  ASSERT_FALSE(edited.has_value());
  EXPECT_TRUE(contains(edited.error(), "owner/reader")) << edited.error();
  EXPECT_FALSE(fx.graph.find(source.key())->recipe.ephemeral);

  ASSERT_TRUE(fx.graph.remove(source.key()).has_value());
  const Status reinstalled = fx.graph.upsert(as_preview);
  ASSERT_FALSE(reinstalled.has_value());
  EXPECT_TRUE(contains(reinstalled.error(), "owner/reader")) << reinstalled.error();
  EXPECT_EQ(fx.graph.find(source.key()), nullptr);

  ASSERT_TRUE(fx.graph.upsert(source).has_value()) << "a persistent reinstall relinks the reader";
  auto preview_reader = chainedRecipe("preview_reader", "owner/crop/cropped");
  preview_reader.ephemeral = true;
  ASSERT_TRUE(fx.graph.upsert(preview_reader).has_value());
  ASSERT_TRUE(fx.graph.remove("owner/reader").has_value());
  EXPECT_TRUE(fx.graph.upsert(as_preview).has_value()) << "only previews read it now";
}

// BUD-01
TEST(RecipeGraphTest, PinnedPerOwnerCapIsEnforced) {
  Fixture fx;
  const auto pinned = [](std::size_t index) {
    auto recipe = cropRecipe("pin" + std::to_string(index));
    recipe.pinned_t = 1 * kSec;
    return recipe;
  };
  for (std::size_t index = 0; index < RecipeGraph::kMaxPinnedPerOwner; ++index) {
    ASSERT_TRUE(fx.graph.upsert(pinned(index)).has_value());
  }
  const Status over = fx.graph.upsert(pinned(RecipeGraph::kMaxPinnedPerOwner));
  ASSERT_FALSE(over.has_value());
  EXPECT_TRUE(contains(over.error(), "cap")) << over.error();
  auto replacement = pinned(0);
  replacement.pinned_t = 2 * kSec;
  EXPECT_TRUE(fx.graph.upsert(replacement).has_value()) << "replacing a pinned key is not a new pin";
  EXPECT_TRUE(fx.graph.upsert(cropRecipe("live")).has_value()) << "an unpinned recipe is not counted";
  auto other_owner = pinned(RecipeGraph::kMaxPinnedPerOwner);
  other_owner.owner_plugin = "other";
  EXPECT_TRUE(fx.graph.upsert(other_owner).has_value()) << "the cap is per owner";
}

// Slice 8a-1: `bind` makes a transient evaluation's node as `upsert` would, installing nothing. It may declare no
// output (an explicit evaluation infers them); `upsert` still needs one.
TEST(RecipeGraphTest, BindValidatesWithoutInstalling) {
  Fixture fx;
  ASSERT_TRUE(fx.graph.upsert(cropRecipe()).has_value());
  auto reader = chainedRecipe("reader", "owner/crop/cropped");
  reader.outputs.clear();

  const Expected<RecipeGraph::Node> bound = fx.graph.bind(reader);
  ASSERT_TRUE(bound.has_value()) << bound.error();
  EXPECT_EQ(bound->revision, 0u) << "never a graph revision";
  EXPECT_EQ(bound->dataset, kDataset);
  ASSERT_EQ(bound->bindings.size(), 1u);
  EXPECT_EQ(bound->bindings[0].upstream, "owner/crop");
  EXPECT_EQ(bound->bindings[0].name, "cropped");
  EXPECT_NE(bound->compiled, nullptr);
  EXPECT_EQ(fx.graph.find("owner/reader"), nullptr) << "nothing installed";
  EXPECT_TRUE(fx.graph.dependentsOf({"owner/crop"}).empty()) << "no edge recorded";

  auto unknown = cropRecipe("unknown");
  unknown.inputs = {"nowhere"};
  const auto refused = fx.graph.bind(unknown);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), "on-demand: unknown input 'nowhere'");
  auto syntax_error = cropRecipe("syntax");
  syntax_error.script = "return {";
  EXPECT_FALSE(fx.graph.bind(syntax_error).has_value());
  EXPECT_FALSE(fx.graph.upsert(reader).has_value()) << "an installed recipe declares its outputs";
}

// PIN-01: the graph half (retiring the finding's topic is the ring's).
TEST(RecipeGraphTest, DismissAndDismissAllRemoveFindings) {
  Fixture fx;
  auto first = cropRecipe("first");
  first.pinned_t = 10 * kSec;
  auto second = first;
  second.user_id = "second";
  auto other = first;
  other.owner_plugin = "other";
  for (const auto& recipe : {first, second, other}) {
    ASSERT_TRUE(fx.graph.upsert(recipe).has_value());
  }
  ASSERT_TRUE(fx.graph.upsert(cropRecipe("live")).has_value());
  EXPECT_EQ(fx.graph.find(first.key())->recipe.pinned_t, 10 * kSec) << "the pin time is the finding's label";

  EXPECT_FALSE(fx.graph.dismiss("owner/live").has_value()) << "a live recipe is not a finding";
  EXPECT_FALSE(fx.graph.dismiss("owner/missing").has_value());
  ASSERT_TRUE(fx.graph.dismiss(first.key()).has_value());
  EXPECT_EQ(fx.graph.find(first.key()), nullptr);
  EXPECT_NE(fx.graph.find(second.key()), nullptr);

  EXPECT_TRUE(fx.graph.dismissAll("owner"));
  EXPECT_EQ(fx.graph.find(second.key()), nullptr);
  EXPECT_NE(fx.graph.find("owner/live"), nullptr) << "dismissAll removes findings only";
  EXPECT_NE(fx.graph.find(other.key()), nullptr) << "another owner's finding is untouched";
  EXPECT_FALSE(fx.graph.dismissAll("owner"));
}

// A persistent id never starts with `~`, the previews' namespace (`kPreviewIdPrefix`), so a preview never holds a key
// a layout can name; a preview may, and so may a transient evaluation.
TEST(RecipeGraphTest, APersistentIdMayNotStartWithTheReservedPrefix) {
  Fixture fx;
  auto reserved = cropRecipe();
  reserved.user_id = std::string(RecipeGraph::kPreviewIdPrefix) + "crop";
  const Status refused = fx.graph.upsert(reserved);
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(contains(refused.error(), "'~'")) << refused.error();
  EXPECT_EQ(fx.graph.find(reserved.key()), nullptr);
  reserved.ephemeral = true;
  EXPECT_TRUE(fx.graph.upsert(reserved).has_value());
  auto transient = cropRecipe();
  transient.user_id = "~evaluation/crop";
  transient.ephemeral = true;
  EXPECT_TRUE(fx.graph.bind(transient).has_value());
}

}  // namespace
}  // namespace PJ
