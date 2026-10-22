// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Who may delete a Custom Topics row (CustomTopicsPolicy.h): the editor's own recipes, and never in a way that
// reaches another plugin's work.

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CustomTopicsPolicy.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

using Reason = CustomTopicDeleteRefusal::Reason;

const QString kEditor = u"editor"_s;

// A self-contained Luau transform class whose id is `id`.
std::string negateScript(const std::string& id) {
  return "-- pj-script: luau\nreturn { id=\"" + id +
         "\", name=\"Negate\", output=\"same\",\n"
         "  create = function(p) return { calculate = function(t, v) return -v end } end }\n";
}

// A graph over a fake catalog holding the point cloud `cloud` and the transform output `speed/chain`.
struct GraphFixture {
  GraphFixture()
      : graph([this](std::string_view input) {
          const auto found = catalog.find(std::string(input));
          return found == catalog.end() ? std::vector<RecipeGraph::InputCandidate>{} : found->second;
        }) {}
  std::map<std::string, std::vector<RecipeGraph::InputCandidate>> catalog{
      {"cloud", {{1, sdk::BuiltinObjectType::kPointCloud, "cloud"}}},
      {"speed/chain", {{1, sdk::BuiltinObjectType::kNone, "speed/chain"}}}};
  RecipeGraph graph;
};

// A session with one float64 series `speed`, an on-demand graph and a marker engine beside it.
struct TransformFixture {
  SessionManager session;
  DataProcessorService& service = session.dataProcessorService();
  GraphFixture on_demand;
  ObjectStore store;
  BatchEngine markers{store, [](DatasetId, const std::string&, const BatchEngine::Window&) {
                        BatchEngine::ResolvedSeries series;
                        series.timestamps = {0.0};
                        series.values = {1.0};
                        return std::optional<BatchEngine::ResolvedSeries>{std::move(series)};
                      }};

  TransformFixture() {
    DataEngine& engine = session.dataEngine();
    const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    auto handle = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64);
    writer.appendScalar(*handle, 0, 1.0);
    engine.commitChunks(writer.flushAll());
  }
  // The output topic of a new transform `<owner>/<id>` reading `input` into `output`.
  TopicId upsert(
      const std::string& owner, const std::string& input, const std::string& output, const std::string& id = "neg") {
    const auto recipe = service.upsertTransform(owner, id, {input}, {output}, negateScript(id), "{}");
    EXPECT_TRUE(recipe.has_value()) << (recipe.has_value() ? "" : recipe.error());
    return recipe.has_value() ? recipe->output_topic_ids.front() : 0;
  }
  [[nodiscard]] std::optional<CustomTopicDeleteRefusal> refusal(TopicId output, const QString& editor = kEditor) {
    return transformDeleteRefusal(service, on_demand.graph, markers, output, editor);
  }
};

TEST(CustomTopicsPolicyTest, TransformDeleteIsTheEditorsOwnAndNeverCascadesIntoAnotherPlugin) {
  TransformFixture fx;
  const TopicId foreign = fx.upsert("assistant", "speed", "speed/foreign");
  const TopicId mine = fx.upsert(kEditor.toStdString(), "speed", "speed/mine");

  const auto read_only = fx.refusal(foreign);
  ASSERT_TRUE(read_only.has_value());
  EXPECT_EQ(read_only->reason, Reason::kReadOnly);
  EXPECT_EQ(read_only->user_id, "neg");
  EXPECT_EQ(read_only->owner_plugin, "assistant");
  EXPECT_EQ(fx.refusal(mine, QString{})->reason, Reason::kReadOnly) << "no editor declared";
  EXPECT_FALSE(fx.refusal(mine).has_value());
  EXPECT_EQ(fx.refusal(987654)->reason, Reason::kGone);

  fx.upsert("assistant2", "speed/mine", "speed/reader");
  const auto cascade = fx.refusal(mine);
  ASSERT_TRUE(cascade.has_value());
  EXPECT_EQ(cascade->reason, Reason::kForeignReader);
  EXPECT_EQ(cascade->user_id, "neg");
  EXPECT_EQ(cascade->owner_plugin, "assistant2");
  EXPECT_EQ(cascade->reader, "neg");
}

RecipeGraph::Recipe cropRecipe(const std::string& owner, const std::string& id, const std::string& input = "cloud") {
  RecipeGraph::Recipe recipe;
  recipe.owner_plugin = owner;
  recipe.user_id = id;
  recipe.inputs = {input};
  recipe.outputs = {{"cropped", scripting::OutputType::kObject, sdk::BuiltinObjectType::kPointCloud}};
  recipe.script = "local inputs = ...\nreturn {cropped = inputs[\"" + input + "\"]}\n";
  return recipe;
}

// The delete is an undo step, so its redo must not strand a history-exempt reader of anything the cascade removes:
// an on-demand recipe reading a downstream transform's output, a marker generator reading the transform's own.
TEST(CustomTopicsPolicyTest, TransformDeleteNeverStrandsAHistoryExemptReader) {
  {
    TransformFixture fx;
    const TopicId mine = fx.upsert(kEditor.toStdString(), "speed", "speed/mine");
    fx.upsert(kEditor.toStdString(), "speed/mine", "speed/chain", "chain");
    auto reader = cropRecipe("assistant", "reader", "cloud");
    reader.inputs = {"cloud", "speed/chain"};
    ASSERT_TRUE(fx.on_demand.graph.upsert(reader).has_value());
    EXPECT_FALSE(fx.refusal(mine).has_value()) << "an ordinary reader reports a missing input";

    auto finding = cropRecipe("assistant", "finding", "cloud");
    finding.inputs = {"cloud", "speed/chain"};
    finding.history_exempt = true;
    ASSERT_TRUE(fx.on_demand.graph.upsert(finding).has_value());
    const auto exempt = fx.refusal(mine);
    ASSERT_TRUE(exempt.has_value());
    EXPECT_EQ(exempt->reason, Reason::kExemptReader);
    EXPECT_EQ(exempt->user_id, "neg");
    EXPECT_EQ(exempt->reader, "assistant/finding/cropped");
  }
  {
    TransformFixture fx;
    const TopicId mine = fx.upsert(kEditor.toStdString(), "speed", "speed/mine");
    BatchEngine::GeneratorRecipe rule;
    rule.id = "assistant/rule";
    rule.dataset_id = 1;
    rule.inputs = {"speed/mine/value"};
    rule.outputs = {"exempt_markers"};
    rule.script = "createMarker(0.0)\n";
    rule.history_exempt = true;
    ASSERT_TRUE(fx.markers.upsertGenerator(rule).has_value());
    const auto exempt = fx.refusal(mine);
    ASSERT_TRUE(exempt.has_value());
    EXPECT_EQ(exempt->reason, Reason::kExemptReader);
    EXPECT_EQ(exempt->reader, "exempt_markers");
  }
}

TEST(CustomTopicsPolicyTest, OnDemandDeleteIsTheEditorsOwnAndNeverStrandsAnExemptReader) {
  GraphFixture fx;
  const auto foreign = cropRecipe("assistant", "crop");
  const auto mine = cropRecipe(kEditor.toStdString(), "crop");
  ASSERT_TRUE(fx.graph.upsert(foreign).has_value());
  ASSERT_TRUE(fx.graph.upsert(mine).has_value());

  const auto read_only = onDemandDeleteRefusal(fx.graph, foreign.key(), kEditor);
  ASSERT_TRUE(read_only.has_value());
  EXPECT_EQ(read_only->reason, Reason::kReadOnly);
  EXPECT_EQ(read_only->user_id, "crop");
  EXPECT_EQ(read_only->owner_plugin, "assistant");
  EXPECT_EQ(onDemandDeleteRefusal(fx.graph, mine.key(), QString{})->reason, Reason::kReadOnly) << "no editor declared";
  EXPECT_EQ(onDemandDeleteRefusal(fx.graph, "editor/absent", kEditor)->reason, Reason::kGone);

  // An ordinary reader of another plugin stays and reports a missing input: no refusal.
  ASSERT_TRUE(fx.graph.upsert(cropRecipe("assistant", "reader", mine.key() + "/cropped")).has_value());
  EXPECT_FALSE(onDemandDeleteRefusal(fx.graph, mine.key(), kEditor).has_value());

  auto finding = cropRecipe("assistant", "finding", mine.key() + "/cropped");
  finding.history_exempt = true;
  ASSERT_TRUE(fx.graph.upsert(finding).has_value());
  const auto exempt = onDemandDeleteRefusal(fx.graph, mine.key(), kEditor);
  ASSERT_TRUE(exempt.has_value());
  EXPECT_EQ(exempt->reason, Reason::kExemptReader);
  EXPECT_EQ(exempt->user_id, "crop");
  EXPECT_EQ(exempt->reader, "assistant/finding/cropped");
}

}  // namespace
}  // namespace PJ
