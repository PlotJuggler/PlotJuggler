// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// M4.2 Task 4 — the DataProcessorService *transform* API (plugin-created,
// named, owned, persisted nodes), distinct from the per-curve filter path.
// Transforms share the eager IncrementalEngine substrate but live in their own
// recipe map keyed by "<plugin_id>/<id>".

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "event_pump_test_utils.h"
#include "marker_test_utils.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/incremental_engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"

namespace PJ {
namespace {

// A self-contained Luau transform class. First line is the backend directive the
// host dispatches on (data-only ABI contract). `id` must match the transform id.
constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

constexpr const char* kTimes10 = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return v * 10.0 end } end }
)LUAU";

constexpr const char* kNegateBoom = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) error("boom") end } end }
)LUAU";

// A class whose calculate() raises a runtime error on the first sample.
constexpr const char* kBoom = R"LUAU(-- pj-script: luau
return { id="boom", name="Boom",
  create = function(p) return { calculate = function(t, v) error("boom") end } end }
)LUAU";

// A 1-input / 2-output MIMO class: returns (value, -value).
constexpr const char* kSplit = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) return v, -v end } end }
)LUAU";

constexpr const char* kSplitTimes10 = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) return v * 10.0, v * -10.0 end } end }
)LUAU";

constexpr const char* kSplitBoom = R"LUAU(-- pj-script: luau
return { id="split", name="Split",
  create = function(p) return { calculate = function(t, v) error("boom") end } end }
)LUAU";

// A 2-input MIMO class: returns a + b.
constexpr const char* kSum = R"LUAU(-- pj-script: luau
return { id="sum", name="Sum",
  create = function(p) return { calculate = function(t, a, b) return a + b end } end }
)LUAU";

constexpr const char* kScale = R"LUAU(return { {
  id = "scale", name = "Scale", output = "same",
  create = function(params)
    return { calculate = function(time, value) return value * 2 end }
  end
} })LUAU";

// Read one column of a topic as doubles (readValues above is column-0 only).
std::vector<double> readColumn(DataEngine& engine, TopicId tid, std::size_t col) {
  std::vector<double> out;
  const TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(col, row.row_index)); });
  return out;
}

std::vector<double> readValues(DataEngine& engine, TopicId tid) {
  std::vector<double> out;
  const TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<Timestamp>::max());
  cursor.forEach([&](const SampleRow& row) { out.push_back(row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

// Build a single float64 series named `name` in a fresh dataset and commit the
// given samples (one per second). Returns the dataset id and input topic id.
struct Fixture {
  SessionManager session;
  DataEngine& engine = session.dataEngine();
  DataProcessorService& service = session.dataProcessorService();
  DatasetId ds = 0;
  TopicId input = 0;

  void seed(const char* name, const std::vector<double>& values) {
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    auto handle = writer.registerScalarSeries(ds, name, NumericType::kFloat64);
    input = handle->topic_id;
    for (std::size_t i = 0; i < values.size(); ++i) {
      writer.appendScalar(*handle, static_cast<Timestamp>(i) * 1'000'000'000LL, values[i]);
    }
    engine.commitChunks(writer.flushAll());
  }

  // Build a schemaless (schema_id == 0) multi-column topic — the shape a JSON
  // scalar-only ingest produces — with the given named columns. `field_paths` are the
  // stored column descriptors (dotted for nested JSON, e.g. "orientation.w"); each
  // `columns[c]` feeds field_paths[c]. Returns via `input`/`ds`.
  void seedMultiColumn(
      const char* topic_name, const std::vector<std::string>& field_paths,
      const std::vector<std::vector<double>>& columns) {
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    input = *writer.registerTopic(ds, TopicDescriptor{.name = topic_name, .schema_id = 0, .dataset_id = ds});
    for (const auto& path : field_paths) {
      (void)writer.ensureColumn(input, path, PrimitiveType::kFloat64);
    }
    const std::size_t rows = columns.empty() ? 0 : columns.front().size();
    for (std::size_t r = 0; r < rows; ++r) {
      EXPECT_TRUE(writer.beginRow(input, static_cast<Timestamp>(r) * 1'000'000'000LL).has_value());
      for (std::size_t c = 0; c < field_paths.size(); ++c) {
        writer.set<double>(input, c, columns[c][r]);
      }
      EXPECT_TRUE(writer.finishRow(input).has_value());
    }
    engine.commitChunks(writer.flushAll());
  }

  [[nodiscard]] bool listed(TopicId t) const {
    const auto v = engine.listTopics(ds);
    return std::find(v.begin(), v.end(), t) != v.end();
  }
};

DataProcessorService::TransformRecipe negateRecipe(DataProcessorService::TransformInputBinding binding) {
  DataProcessorService::TransformRecipe recipe;
  recipe.key = "pluginA/negate";
  recipe.owner_plugin = "pluginA";
  recipe.user_id = "negate";
  recipe.inputs = {binding.topic_name};
  recipe.input_bindings = {std::move(binding)};
  recipe.outputs = {"out"};
  recipe.script = kNegate;
  recipe.params_json = "{}";
  return recipe;
}

TEST(DataProcessorTransformTest, UpsertCreatesNamedTopic) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0, -3.0});

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/negated"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->key, "pluginA/negate");
  EXPECT_EQ(rec->owner_plugin, "pluginA");
  EXPECT_EQ(rec->user_id, "negate");
  EXPECT_EQ(rec->backend, "luau");
  EXPECT_EQ(rec->dataset_id, fx.ds);  // output dataset = the input topic's dataset
  ASSERT_EQ(rec->input_bindings.size(), 1u);
  EXPECT_EQ(rec->input_bindings.front().dataset_id, fx.ds);
  EXPECT_EQ(rec->input_bindings.front().dataset_source, "s");
  EXPECT_EQ(rec->input_bindings.front().topic_name, "speed");
  EXPECT_EQ(rec->input_bindings.front().field_path, "value");
  EXPECT_EQ(rec->input_bindings.front().column_index, 0u);
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);

  const TopicId out = rec->output_topic_ids.front();
  EXPECT_TRUE(fx.listed(out));
  EXPECT_EQ(readValues(fx.engine, out), (std::vector<double>{1.0, -2.0, 3.0}));
}

TEST(DataProcessorTransformTest, SourceTopicsForOutputTraversesTransformChain) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto first = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}");
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto second = fx.service.upsertTransform("pluginB", "negate", {"mid"}, {"final"}, kNegate, "{}");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(
      fx.service.sourceTopicsForOutput(second->output_topic_ids.front()),
      (std::vector<std::pair<DatasetId, std::string>>{{fx.ds, "speed"}}));
}

TEST(DataProcessorTransformTest, SourceTopicsForOutputDoesNotJumpToSameNamedReplacement) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto recipe = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(recipe.has_value()) << recipe.error();

  const DatasetId other = *fx.engine.createDataset(DatasetDescriptor{.source_name = "other", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto same_name = writer.registerScalarSeries(other, "speed", NumericType::kFloat64);
  writer.appendScalar(*same_name, 0, 99.0);
  fx.engine.commitChunks(writer.flushAll());
  fx.engine.retireTopic(fx.input);
  EXPECT_TRUE(fx.service.sourceTopicsForOutput(recipe->output_topic_ids.front()).empty());
}

TEST(DataProcessorTransformTest, DependencyPreflightAndCascadeKeepSameNamedDatasetsIsolated) {
  SessionManager session;
  DataEngine& engine = session.dataEngine();
  DataProcessorService& service = session.dataProcessorService();
  const DatasetId dataset_a = *engine.createDataset(DatasetDescriptor{.source_name = "a.mcap"});
  const DatasetId dataset_b = *engine.createDataset(DatasetDescriptor{.source_name = "b.mcap"});
  DataWriter writer = engine.createWriter();
  auto input_a = writer.registerScalarSeries(dataset_a, "speed", NumericType::kFloat64);
  auto input_b = writer.registerScalarSeries(dataset_b, "speed", NumericType::kFloat64);
  writer.appendScalar(*input_a, 0, 1.0);
  writer.appendScalar(*input_b, 0, 9.0);
  engine.commitChunks(writer.flushAll());

  const auto transform_b = service.restoreTransform(negateRecipe(
      DataProcessorService::TransformInputBinding{
          .dataset_id = dataset_b,
          .dataset_source = "b.mcap",
          .topic_name = "speed",
          .field_path = "value",
          .column_index = 0,
      }));
  ASSERT_TRUE(transform_b.has_value()) << transform_b.error();
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(catalogue->addBundledSource(kScale, "test").has_value());
  service.setFilterCatalogue(std::move(catalogue));
  const auto filter_a = service.applyFilter(input_a->topic_id, dataset_a, "scale", "a_filtered");
  ASSERT_TRUE(filter_a.has_value()) << filter_a.error();
  const auto transform_a = service.upsertTransform("pluginC", "negate", {"a_filtered"}, {"a_final"}, kNegate, "{}");
  ASSERT_TRUE(transform_a.has_value()) << transform_a.error();

  const auto affected_a = service.transformsDependingOn({input_a->topic_id});
  ASSERT_EQ(affected_a.size(), 1u);
  EXPECT_EQ(affected_a.front().key, transform_a->key);
  const auto affected_b = service.transformsDependingOn({input_b->topic_id});
  ASSERT_EQ(affected_b.size(), 1u);
  EXPECT_EQ(affected_b.front().key, transform_b->key);
  ASSERT_TRUE(service.removeFilter(filter_a->node_id).has_value());
  const auto remaining = service.transformRecipes();
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_EQ(remaining.front().key, transform_b->key);
}

TEST(DataProcessorTransformTest, RemoveFilterRejectsTransformNodeId) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto recipe = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(recipe.has_value()) << recipe.error();

  EXPECT_FALSE(fx.service.removeFilter(recipe->node_id).has_value());
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(recipe->node_id));
  EXPECT_TRUE(fx.listed(recipe->output_topic_ids.front()));
}

TEST(DataProcessorTransformTest, UpsertReplacesExistingNode) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto first = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(first.has_value()) << first.error();
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));

  // Same key, new script -> replace in place; one recipe, new behaviour.
  const auto second = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kTimes10, "{}");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_EQ(second->node_id, first->node_id);
  EXPECT_EQ(second->output_topic_ids, first->output_topic_ids);
  EXPECT_EQ(readValues(fx.engine, second->output_topic_ids.front()), (std::vector<double>{10.0, 20.0}));
}

TEST(DataProcessorTransformTest, ExistingKeyCannotChangePersistentLifetime) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto persistent =
      fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}", false);
  ASSERT_TRUE(persistent.has_value()) << persistent.error();

  const auto preview = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kTimes10, "{}", true);
  ASSERT_FALSE(preview.has_value());
  const auto saved = fx.service.transformRecipes();
  ASSERT_EQ(saved.size(), 1u);
  EXPECT_FALSE(saved.front().ephemeral);
  EXPECT_EQ(saved.front().node_id, persistent->node_id);
}

TEST(DataProcessorTransformTest, ReplacingUpstreamPreservesAndRecomputesExactTopicDependents) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto parent = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}");
  ASSERT_TRUE(parent.has_value()) << parent.error();
  const auto child = fx.service.upsertTransform("pluginB", "negate", {"mid"}, {"final"}, kNegate, "{}");
  ASSERT_TRUE(child.has_value()) << child.error();

  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(catalogue->addBundledSource(kScale, "test").has_value());
  fx.service.setFilterCatalogue(std::move(catalogue));
  const auto filtered = fx.service.applyFilter(child->output_topic_ids.front(), fx.ds, "scale", "filtered");
  ASSERT_TRUE(filtered.has_value()) << filtered.error();

  const auto replaced = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kTimes10, "{}");
  ASSERT_TRUE(replaced.has_value()) << replaced.error();
  EXPECT_EQ(replaced->node_id, parent->node_id);
  EXPECT_EQ(replaced->output_topic_ids, parent->output_topic_ids);
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(child->node_id));
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(filtered->node_id));
  EXPECT_EQ(readValues(fx.engine, parent->output_topic_ids.front()), (std::vector<double>{10.0, 20.0}));
  EXPECT_EQ(readValues(fx.engine, child->output_topic_ids.front()), (std::vector<double>{-10.0, -20.0}));
  EXPECT_EQ(readValues(fx.engine, filtered->output_topic_id), (std::vector<double>{-20.0, -40.0}));
}

TEST(DataProcessorTransformTest, FailedCompatibleReplacementRollsBackRootAndDependents) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto parent = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}");
  ASSERT_TRUE(parent.has_value()) << parent.error();
  const auto child = fx.service.upsertTransform("pluginB", "negate", {"mid"}, {"final"}, kNegate, "{}");
  ASSERT_TRUE(child.has_value()) << child.error();

  const auto failed = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegateBoom, "{}");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(fx.service.transformRecipes().size(), 2u);
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(parent->node_id));
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(child->node_id));
  EXPECT_EQ(readValues(fx.engine, parent->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
  EXPECT_EQ(readValues(fx.engine, child->output_topic_ids.front()), (std::vector<double>{1.0, 2.0}));
}

TEST(DataProcessorTransformTest, StructuralReplacementIsRejectedWithoutMutatingGraph) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto parent = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}");
  ASSERT_TRUE(parent.has_value()) << parent.error();
  const auto child = fx.service.upsertTransform("pluginB", "negate", {"mid"}, {"final"}, kNegate, "{}");
  ASSERT_TRUE(child.has_value()) << child.error();

  const auto rejected = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"renamed"}, kTimes10, "{}");
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(fx.service.transformRecipes().size(), 2u);
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(parent->node_id));
  EXPECT_TRUE(fx.service.incrementalEngine().hasNode(child->node_id));
  EXPECT_EQ(readValues(fx.engine, parent->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
  EXPECT_EQ(readValues(fx.engine, child->output_topic_ids.front()), (std::vector<double>{1.0, 2.0}));
}

TEST(DataProcessorTransformTest, RemovingTransformCascadesDownstreamFilter) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto parent = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}");
  ASSERT_TRUE(parent.has_value()) << parent.error();

  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(catalogue->addBundledSource(kScale, "test").has_value());
  fx.service.setFilterCatalogue(std::move(catalogue));
  const auto child = fx.service.applyFilter(parent->output_topic_ids.front(), fx.ds, "scale", "final");
  ASSERT_TRUE(child.has_value()) << child.error();

  ASSERT_TRUE(fx.service.removeTransform(parent->key).has_value());
  EXPECT_TRUE(fx.service.transformRecipes().empty());
  EXPECT_TRUE(fx.service.recipes().empty());
  EXPECT_FALSE(fx.listed(child->output_topic_id));
}

TEST(DataProcessorTransformTest, RemoveRetiresTopic) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const TopicId out = rec->output_topic_ids.front();
  ASSERT_TRUE(fx.listed(out));

  ASSERT_TRUE(fx.service.removeTransform("pluginA/negate").has_value());
  EXPECT_FALSE(fx.listed(out));  // retired -> dropped from the catalog
  EXPECT_TRUE(fx.service.transformRecipes().empty());

  // Removing an unknown key is an error.
  EXPECT_FALSE(fx.service.removeTransform("pluginA/ghost").has_value());
}

TEST(DataProcessorTransformTest, RollbackLeavesNoZombieTopic) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  // A runtime-erroring script must fail the upsert AND leave the output name free.
  EXPECT_FALSE(fx.service.upsertTransform("pluginA", "boom", {"speed"}, {"speed/out"}, kBoom, "{}").has_value());
  EXPECT_TRUE(fx.service.transformRecipes().empty());

  // A healthy transform can re-use the freed name.
  const auto ok = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  EXPECT_TRUE(ok.has_value()) << ok.error();
}

TEST(DataProcessorTransformTest, OutputNameCollisionRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto a = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}");
  ASSERT_TRUE(a.has_value()) << a.error();

  // A different transform claiming the same output name is rejected, not clobbered.
  const auto b = fx.service.upsertTransform("pluginB", "other", {"speed"}, {"speed/out"}, kNegate, "{}");
  EXPECT_FALSE(b.has_value());
  // A's output is untouched and A's recipe survives.
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_TRUE(fx.listed(a->output_topic_ids.front()));
}

// Field-level input binding on a schemaless topic: a "<topic>/<field>" input must
// resolve to the named column (separator normalized '/'->'.'), not topic column 0.
// Regression: schema_id == 0 topics (JSON scalar-only ingest) were unresolvable by
// field name, and nested fields failed on the '/' vs '.' separator mismatch.
TEST(DataProcessorTransformTest, ResolvesSchemalessNestedFieldInput) {
  Fixture fx;
  // /imu with dotted nested columns; "orientation.w" is the LAST column (index 3),
  // so resolving to column 0 instead would negate the wrong series.
  fx.seedMultiColumn(
      "/imu", {"orientation.x", "orientation.y", "orientation.z", "orientation.w"},
      {{5.0, 5.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 2.0}});

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"/imu/orientation/w"}, {"roll"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);
  // Negated "orientation.w" {1,2} -> {-1,-2}; if it read column 0 it would be {-5,-5}.
  EXPECT_EQ(readValues(fx.engine, rec->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
}

TEST(DataProcessorTransformTest, ExplicitInputColumnIsCapturedAndRestored) {
  Fixture fx;
  fx.seedMultiColumn("/imu", {"x", "selected"}, {{100.0, 200.0}, {1.0, 2.0}});
  const auto recipe = fx.service.upsertTransform("pluginA", "negate", {"/imu"}, {"out"}, kNegate, "{}", false, 1);
  ASSERT_TRUE(recipe.has_value()) << recipe.error();
  ASSERT_EQ(recipe->input_bindings.size(), 1u);
  EXPECT_EQ(recipe->input_bindings.front().field_path, "selected");
  const TopicId original_output = recipe->output_topic_ids.front();

  const DataProcessorService::TransformRecipe snapshot = *recipe;
  fx.service.clearAllTransforms();
  const auto restored = fx.service.restoreTransform(snapshot);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_NE(restored->output_topic_ids.front(), original_output);
  EXPECT_EQ(restored->input_bindings.front().column_index, 1u);
  EXPECT_EQ(readValues(fx.engine, restored->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
}

TEST(DataProcessorTransformTest, QualifiedFieldPathSurvivesColumnReordering) {
  Fixture fx;
  fx.seedMultiColumn("/imu", {"other", "selected"}, {{100.0, 200.0}, {1.0, 2.0}});
  auto recipe = negateRecipe(
      DataProcessorService::TransformInputBinding{
          .dataset_id = fx.ds,
          .dataset_source = "s",
          .topic_name = "/imu",
          .field_path = "selected",
          .column_index = 0,
      });
  const auto restored = fx.service.restoreTransform(recipe);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_EQ(restored->input_bindings.front().column_index, 1u);
  EXPECT_EQ(readValues(fx.engine, restored->output_topic_ids.front()), (std::vector<double>{-1.0, -2.0}));
}

TEST(DataProcessorTransformTest, InPlaceReloadRebindKeepsOutputTopicIdStable) {
  SessionManager session;
  DataEngine& engine = session.dataEngine();
  const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  const TopicId input =
      *writer.registerTopic(dataset, TopicDescriptor{.name = "/imu", .schema_id = 0, .dataset_id = dataset});
  (void)writer.ensureColumn(input, "other", PrimitiveType::kFloat64);
  (void)writer.ensureColumn(input, "selected", PrimitiveType::kFloat64);
  for (std::size_t row = 0; row < 2; ++row) {
    ASSERT_TRUE(writer.beginRow(input, static_cast<Timestamp>(row)).has_value());
    writer.set<double>(input, 0, 100.0 + static_cast<double>(row));
    writer.set<double>(input, 1, 1.0 + static_cast<double>(row));
    ASSERT_TRUE(writer.finishRow(input).has_value());
  }
  engine.commitChunks(writer.flushAll());

  auto installed =
      session.dataProcessorService().upsertTransform("pluginA", "negate", {"/imu/selected"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(installed.has_value()) << installed.error();
  const TopicId output = installed->output_topic_ids.front();
  EXPECT_EQ(readValues(engine, output), (std::vector<double>{-1.0, -2.0}));

  DataEngine staged;
  const DatasetId staged_dataset = *staged.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter staged_writer = staged.createWriter();
  const TopicId staged_input = *staged_writer.registerTopic(
      staged_dataset, TopicDescriptor{.name = "/imu", .schema_id = 0, .dataset_id = staged_dataset});
  (void)staged_writer.ensureColumn(staged_input, "selected", PrimitiveType::kFloat64);
  (void)staged_writer.ensureColumn(staged_input, "other", PrimitiveType::kFloat64);
  for (std::size_t row = 0; row < 2; ++row) {
    ASSERT_TRUE(staged_writer.beginRow(staged_input, static_cast<Timestamp>(row)).has_value());
    staged_writer.set<double>(staged_input, 0, 10.0 + static_cast<double>(row));
    staged_writer.set<double>(staged_input, 1, 900.0 + static_cast<double>(row));
    ASSERT_TRUE(staged_writer.finishRow(staged_input).has_value());
  }
  staged.commitChunks(staged_writer.flushAll());

  auto replaced = engine.replaceDatasetFrom(staged, staged_dataset, dataset);
  ASSERT_TRUE(replaced.has_value()) << replaced.error();
  auto replayed = session.dataProcessorService().rebindAndRecomputeForReplacedSources(replaced->replaced_topics);
  ASSERT_TRUE(replayed.has_value()) << replayed.error();
  EXPECT_EQ(session.dataProcessorService().transformRecipes().front().output_topic_ids.front(), output);
  EXPECT_EQ(readValues(engine, output), (std::vector<double>{-10.0, -11.0}));
}

// A single structured "topic:f1,f2" output collapses into ONE topic with those
// named columns (single-topic parity), instead of N separate scalar topics.
TEST(DataProcessorTransformTest, GroupsStructuredOutputIntoOneTopic) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0, 3.0});

  const auto rec = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"rpy:a,b"}, kSplit, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  // Grouped: exactly ONE output topic, not two.
  ASSERT_EQ(rec->output_topic_ids.size(), 1u);
  const TopicId out = rec->output_topic_ids.front();

  const auto lock = fx.engine.lockEngine();
  const TopicStorage* st = fx.engine.getTopicStorage(out);
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->descriptor().name, "rpy");
  // The topic's struct schema carries the two suffixes as named leaf columns, in order.
  const PJ::SchemaId schema_id = st->descriptor().schema_id;
  ASSERT_NE(schema_id, 0u);
  const PJ::TypeTreeNode* root = fx.engine.typeRegistry().lookup(schema_id);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(PJ::flattenFieldPaths(*root), (std::vector<std::string>{"a", "b"}));
  // Column 0 = value, column 1 = -value.
  EXPECT_EQ(readColumn(fx.engine, out, 0), (std::vector<double>{1.0, 2.0, 3.0}));
  EXPECT_EQ(readColumn(fx.engine, out, 1), (std::vector<double>{-1.0, -2.0, -3.0}));
}

// Plain (non-structured) output names stay one topic per name (back-compat: the
// transform editor's multi-output path is untouched).
TEST(DataProcessorTransformTest, PlainMimoOutputsStaySeparateTopics) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto rec = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplit, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->output_topic_ids.size(), 2u);  // two separate scalar topics
}

TEST(DataProcessorTransformTest, CompatibleMimoReplacementPreservesOutputIds) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto first = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplit, "{}");
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto replaced =
      fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplitTimes10, "{}");
  ASSERT_TRUE(replaced.has_value()) << replaced.error();
  EXPECT_EQ(replaced->node_id, first->node_id);
  EXPECT_EQ(replaced->output_topic_ids, first->output_topic_ids);
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids[0]), (std::vector<double>{10.0, 20.0}));
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids[1]), (std::vector<double>{-10.0, -20.0}));
}

TEST(DataProcessorTransformTest, FailedMimoReplacementRestoresOldOperatorAndData) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto first = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplit, "{}");
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto failed = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"alpha", "beta"}, kSplitBoom, "{}");
  ASSERT_FALSE(failed.has_value());
  const auto recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  EXPECT_EQ(recipes.front().output_topic_ids, first->output_topic_ids);
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids[0]), (std::vector<double>{1.0, 2.0}));
  EXPECT_EQ(readValues(fx.engine, first->output_topic_ids[1]), (std::vector<double>{-1.0, -2.0}));
}

TEST(DataProcessorTransformTest, InputNameNotFoundRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"nonexistent"}, {"out"}, kNegate, "{}");
  EXPECT_FALSE(rec.has_value());
}

TEST(DataProcessorTransformTest, UnqualifiedUpsertRejectsDuplicateNamesAcrossDatasets) {
  Fixture fx;
  fx.seed("speed", {3.0});
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "second", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 8.0);
  fx.engine.commitChunks(writer.flushAll());
  EXPECT_FALSE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"out"}, kNegate, "{}").has_value());
}

TEST(DataProcessorTransformTest, QualifiedRestoreUsesExactIdToDisambiguateDuplicateSource) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto second_input = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*second_input, 0, 9.0);
  fx.engine.commitChunks(writer.flushAll());

  auto recipe = negateRecipe(
      DataProcessorService::TransformInputBinding{
          .dataset_id = second,
          .dataset_source = "s",
          .topic_name = "speed",
          .field_path = "value",
          .column_index = 0,
      });
  const auto restored = fx.service.restoreTransform(recipe);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_EQ(restored->input_topic_ids, (std::vector<TopicId>{second_input->topic_id}));
  EXPECT_EQ(readValues(fx.engine, restored->output_topic_ids.front()), (std::vector<double>{-9.0}));
}

// The plugin-facing side of the same disambiguation: a plugin cannot send a
// TransformInputBinding through the ABI, but it can send the name in the form
// the host itself prints — "dataset_source:topic/field".
TEST(DataProcessorTransformTest, DatasetQualifiedUpsertPicksItsDataset) {
  Fixture fx;
  fx.seed("speed", {3.0});  // dataset source "s"
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "second", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 8.0);
  fx.engine.commitChunks(writer.flushAll());

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"second:speed"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->input_topic_ids, (std::vector<TopicId>{duplicate->topic_id}));
  EXPECT_EQ(readValues(fx.engine, rec->output_topic_ids.front()), (std::vector<double>{-8.0}));
  // Canonical form: the stored name is bare; the dataset travels in the binding,
  // exactly as it does for a restored recipe.
  ASSERT_EQ(rec->inputs.size(), 1u);
  EXPECT_EQ(rec->inputs.front(), "speed");
  ASSERT_EQ(rec->input_bindings.size(), 1u);
  EXPECT_EQ(rec->input_bindings.front().dataset_source, "second");
}

TEST(DataProcessorTransformTest, DatasetQualifiedUpsertWorksWithStreamStyleSourceNames) {
  Fixture fx;
  fx.seed("speed", {3.0});
  const DatasetId second =
      *fx.engine.createDataset(DatasetDescriptor{.source_name = "[stream] UDP Server", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 8.0);
  fx.engine.commitChunks(writer.flushAll());

  const auto rec =
      fx.service.upsertTransform("pluginA", "negate", {"[stream] UDP Server:speed"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_EQ(rec->input_topic_ids, (std::vector<TopicId>{duplicate->topic_id}));
}

TEST(DataProcessorTransformTest, DatasetQualifiedUpsertRejectsADuplicatedSourceName) {
  Fixture fx;
  fx.seed("speed", {1.0});  // source "s"
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 9.0);
  fx.engine.commitChunks(writer.flushAll());

  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"s:speed"}, {"out"}, kNegate, "{}");
  ASSERT_FALSE(rec.has_value());
  EXPECT_NE(rec.error().find("ambiguous"), std::string::npos) << rec.error();
}

// A ':' whose prefix is no loaded source stays part of the name; the error
// hints at the near-miss instead of leaving the caller guessing.
TEST(DataProcessorTransformTest, ColonNameThatMatchesNoSourceGetsTheQualifierHint) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"nope:speed"}, {"out"}, kNegate, "{}");
  ASSERT_FALSE(rec.has_value());
  EXPECT_NE(rec.error().find("dataset qualifier"), std::string::npos) << rec.error();
}

// Bug (PR #619 #4): qualified inputs naming two different datasets are resolved
// independently and accepted; the SDK contract says they must agree on one.
TEST(DataProcessorTransformTest, QualifiedInputsNamingTwoDatasetsAreRejected) {
  Fixture fx;
  fx.seed("speed", {3.0});  // source "s"
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "second", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 8.0);
  fx.engine.commitChunks(writer.flushAll());

  const auto rec = fx.service.upsertTransform("pluginA", "sum", {"s:speed", "second:speed"}, {"out"}, kSum, "{}");
  ASSERT_FALSE(rec.has_value()) << "conflicting dataset qualifiers were accepted";
  EXPECT_NE(rec.error().find("dataset"), std::string::npos) << rec.error();
}

// Bug (PR #619 #3): the config read-back a plugin re-edits from (transformRecipeJson,
// behind data_processor_config) returns bare input names, so resubmitting it is
// ambiguous once `speed` exists in two datasets.
TEST(DataProcessorTransformTest, ConfigReadBackResubmitsUnambiguously) {
  Fixture fx;
  fx.seed("speed", {3.0});  // source "s"
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "second", .time_domain_id = 0});
  DataWriter writer = fx.engine.createWriter();
  auto duplicate = writer.registerScalarSeries(second, "speed", NumericType::kFloat64);
  writer.appendScalar(*duplicate, 0, 8.0);
  fx.engine.commitChunks(writer.flushAll());
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"second:speed"}, {"out"}, kNegate, "{}").has_value());

  const std::optional<std::string> json = fx.service.transformRecipeJson("pluginA/negate");
  ASSERT_TRUE(json.has_value());
  const std::vector<std::string> inputs = nlohmann::json::parse(*json).at("inputs").get<std::vector<std::string>>();

  EXPECT_EQ(inputs, (std::vector<std::string>{"second:speed"}));

  const auto resubmitted = fx.service.upsertTransform("pluginB", "negate", inputs, {"out2"}, kNegate, "{}");
  ASSERT_TRUE(resubmitted.has_value()) << resubmitted.error();
  EXPECT_EQ(resubmitted->input_topic_ids, (std::vector<TopicId>{duplicate->topic_id}));
}

// One float64 series `name` in `dataset`, one sample; returns its topic id.
TopicId addSeries(DataEngine& engine, DatasetId dataset, const char* name, double value) {
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset, name, NumericType::kFloat64);
  writer.appendScalar(*handle, 0, value);
  engine.commitChunks(writer.flushAll());
  return handle->topic_id;
}

std::vector<std::string> configInputs(const DataProcessorService& service, const char* key) {
  const std::optional<std::string> json = service.transformRecipeJson(key);
  EXPECT_TRUE(json.has_value());
  return json.has_value() ? nlohmann::json::parse(*json).at("inputs").get<std::vector<std::string>>()
                          : std::vector<std::string>{};
}

// Read-back qualifies an input only when its bare name is ambiguous: a unique
// bare name echoes unchanged.
TEST(DataProcessorTransformTest, ConfigReadBackKeepsAUniqueBareInputBare) {
  Fixture fx;
  fx.seed("speed", {3.0});
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"out"}, kNegate, "{}").has_value());
  EXPECT_EQ(configInputs(fx.service, "pluginA/negate"), (std::vector<std::string>{"speed"}));
}

// Sources "a" and "a:b": an input bound to "a" as bare "b:x" cannot be spelled
// "a:b:x" (the splitter would pick "a:b"), so read-back keeps the bare name even
// once it turned ambiguous.
TEST(DataProcessorTransformTest, ConfigReadBackKeepsBareWhenALongerSourcePrefixShadowsTheQualifier) {
  Fixture fx;
  const DatasetId a = *fx.engine.createDataset(DatasetDescriptor{.source_name = "a", .time_domain_id = 0});
  const DatasetId ab = *fx.engine.createDataset(DatasetDescriptor{.source_name = "a:b", .time_domain_id = 0});
  const TopicId bound = addSeries(fx.engine, a, "b:x", 1.0);
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"b:x"}, {"out"}, kNegate, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();
  ASSERT_EQ(rec->input_topic_ids, (std::vector<TopicId>{bound}));
  (void)addSeries(fx.engine, ab, "b:x", 2.0);
  EXPECT_EQ(configInputs(fx.service, "pluginA/negate"), (std::vector<std::string>{"b:x"}));
}

// A source loaded twice cannot be addressed by name: read-back must not emit
// "s:speed", which the resolver rejects as ambiguous.
TEST(DataProcessorTransformTest, ConfigReadBackKeepsBareWhenTheSourceIsLoadedTwice) {
  Fixture fx;
  fx.seed("speed", {3.0});  // source "s"
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"out"}, kNegate, "{}").has_value());
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  (void)addSeries(fx.engine, second, "speed", 8.0);
  EXPECT_EQ(configInputs(fx.service, "pluginA/negate"), (std::vector<std::string>{"speed"}));
}

// A bare cross-dataset transform (x in a, y in b) is accepted; its read-back must
// stay resubmittable instead of turning into two disagreeing qualifiers.
TEST(DataProcessorTransformTest, CrossDatasetBareTransformRoundTripsThroughConfig) {
  Fixture fx;
  fx.seed("x", {3.0});  // source "s"
  const DatasetId second = *fx.engine.createDataset(DatasetDescriptor{.source_name = "second", .time_domain_id = 0});
  (void)addSeries(fx.engine, second, "y", 8.0);
  const auto rec = fx.service.upsertTransform("pluginA", "sum", {"x", "y"}, {"out"}, kSum, "{}");
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const std::vector<std::string> inputs = configInputs(fx.service, "pluginA/sum");
  const auto resubmitted = fx.service.upsertTransform("pluginB", "sum", inputs, {"out2"}, kSum, "{}");
  ASSERT_TRUE(resubmitted.has_value()) << resubmitted.error();
  EXPECT_EQ(resubmitted->input_topic_ids, rec->input_topic_ids);
}

// Two topics named alike inside ONE dataset: the qualified input is ambiguous
// there, not "not found", and no shorter topic-prefix reading is attempted.
TEST(DataProcessorTransformTest, DuplicateTopicNameInsideOneDatasetIsReportedAmbiguous) {
  Fixture fx;
  fx.seed("speed", {3.0});
  const TopicId twin = addSeries(fx.engine, fx.ds, "speed", 4.0);
  ASSERT_NE(twin, fx.input);
  const auto rec = fx.service.upsertTransform("pluginA", "negate", {"s:speed"}, {"out"}, kNegate, "{}");
  ASSERT_FALSE(rec.has_value());
  EXPECT_NE(rec.error().find("ambiguous"), std::string::npos) << rec.error();
}

TEST(DataProcessorTransformTest, MissingBackendDiagnosesNotDrops) {
  Fixture fx;
  fx.seed("speed", {1.0});

  // A persisted recipe whose backend is unavailable in this build (python).
  DataProcessorService::TransformRecipe py;
  py.key = "pluginA/py";
  py.owner_plugin = "pluginA";
  py.user_id = "py";
  py.inputs = {"speed"};
  py.outputs = {"speed/out"};
  py.script = "# pj-script: python\nreturn None";
  py.params_json = "{}";

  const auto restored = fx.service.restoreTransform(py);
  EXPECT_FALSE(restored.has_value());                  // diagnosed, not silently materialized
  EXPECT_TRUE(fx.service.transformRecipes().empty());  // service did not store it (caller keeps the XML)
}

TEST(DataProcessorTransformTest, RestoreRoundTripRebindsAndRuns) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0});
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"speed/out"}, kNegate, "{}").has_value());

  // Snapshot the recipe the way the layout serializer would, then reconcile.
  const auto recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  const DataProcessorService::TransformRecipe snapshot = recipes.front();

  fx.service.clearAllTransforms();
  ASSERT_TRUE(fx.service.transformRecipes().empty());

  const auto restored = fx.service.restoreTransform(snapshot);
  ASSERT_TRUE(restored.has_value()) << restored.error();
  EXPECT_EQ(readValues(fx.engine, restored->output_topic_ids.front()), (std::vector<double>{1.0, -2.0}));
}

// --- History exemption: clearTransforms(RestoreIntent) / exemptDependentsOf ---

// A predicate clear over a chain (parent -> child, both non-exempt) removes
// both, leaf-first (the child's node must not outlive the parent it reads),
// exactly like clearAllTransforms; a sibling history_exempt transform is left
// untouched regardless of the predicate ever visiting it.
TEST(DataProcessorTransformTest, PredicateClearRemovesOnlyNonExemptLeafFirst) {
  Fixture fx;
  fx.seed("speed", {1.0});

  const auto parent = fx.service.upsertTransform(
      "pluginA", "negate", {"speed"}, {"mid"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(parent.has_value()) << parent.error();
  const auto child = fx.service.upsertTransform(
      "pluginB", "negate", {"mid"}, {"final"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(child.has_value()) << child.error();
  const auto exempt_sibling = fx.service.upsertTransform(
      "pluginC", "negate", {"speed"}, {"exempt_out"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/true);
  ASSERT_TRUE(exempt_sibling.has_value()) << exempt_sibling.error();

  fx.service.clearTransforms(PJ::RestoreIntent::kHistory);

  EXPECT_TRUE(fx.service.transformIdsForPlugin("pluginA").empty());
  EXPECT_TRUE(fx.service.transformIdsForPlugin("pluginB").empty());
  ASSERT_EQ(fx.service.transformIdsForPlugin("pluginC").size(), 1u);
  EXPECT_TRUE(fx.listed(exempt_sibling->output_topic_ids.front()));  // the exempt one survives, untouched
}

// unchangedTransformKeys names the live transforms a snapshot leaves as they
// are: same key + identical persisted recipe (bindings included). A changed
// script or a binding-less legacy entry is a change, so the key is not returned.
TEST(DataProcessorTransformTest, UnchangedTransformKeysMatchOnlyIdenticalPersistedRecipes) {
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto live = fx.service.upsertTransform(
      "pluginA", "negate", {"speed"}, {"neg"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(live.has_value()) << live.error();
  const std::vector<PJ::DataProcessorService::TransformRecipe> recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  ASSERT_FALSE(recipes.front().input_bindings.empty());

  std::vector<PJ::DataProcessorService::TransformRecipe> wanted = recipes;
  EXPECT_EQ(
      fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kHistory),
      (std::unordered_set<std::string>{"pluginA/negate"}));
  EXPECT_EQ(
      fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kReplace),
      (std::unordered_set<std::string>{"pluginA/negate"}));

  wanted.front().script += "\n-- edited";
  EXPECT_TRUE(fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kHistory).empty());

  wanted = recipes;
  wanted.front().input_bindings.clear();
  EXPECT_TRUE(fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kHistory).empty());
}

// The script of a transform loaded from a file on Windows carries CRLF line
// endings; its layout-snapshot copy carries LF after XML normalization. Line
// endings are not identity, so the live transform still counts as unchanged.
TEST(DataProcessorTransformTest, UnchangedTransformKeysIgnoreLineEndingDifferencesInScript) {
  std::string crlf_script;
  for (const char ch : std::string_view(kNegate)) {
    if (ch == '\n') {
      crlf_script.push_back('\r');
    }
    crlf_script.push_back(ch);
  }
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto live = fx.service.upsertTransform(
      "pluginA", "negate", {"speed"}, {"neg"}, crlf_script, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(live.has_value()) << live.error();
  std::vector<PJ::DataProcessorService::TransformRecipe> wanted = fx.service.transformRecipes();
  ASSERT_EQ(wanted.size(), 1u);
  ASSERT_EQ(wanted.front().script, crlf_script);

  wanted.front().script = kNegate;
  EXPECT_EQ(
      fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kHistory),
      (std::unordered_set<std::string>{"pluginA/negate"}));
}

// Only the CRLF pair is a line-ending difference. A lone '\r' inside a Luau
// long bracket is data the script can observe, so a snapshot without it
// describes a different transform and the live key must not be returned.
TEST(DataProcessorTransformTest, UnchangedTransformKeysTreatStandaloneCarriageReturnAsData) {
  const std::string live_script = std::string(kNegate) + "--[[a\rb]]\n";
  Fixture fx;
  fx.seed("speed", {1.0});
  const auto live = fx.service.upsertTransform(
      "pluginA", "negate", {"speed"}, {"neg"}, live_script, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(live.has_value()) << live.error();
  std::vector<PJ::DataProcessorService::TransformRecipe> wanted = fx.service.transformRecipes();
  ASSERT_EQ(wanted.size(), 1u);
  ASSERT_EQ(wanted.front().script, live_script);

  wanted.front().script = std::string(kNegate) + "--[[ab]]\n";
  EXPECT_TRUE(fx.service.unchangedTransformKeys(wanted, PJ::RestoreIntent::kHistory).empty());
}

// exemptDependentsOf finds a transitive exempt dependent through a NON-exempt
// intermediate: exempt E reads U's output directly, but the same closure must
// also catch it when a non-exempt transform sits between U and E.
TEST(DataProcessorTransformTest, ExemptDependentsOfFindsTransitiveExemptDependent) {
  Fixture fx;
  fx.seed("speed", {1.0});

  const auto u = fx.service.upsertTransform(
      "user-u", "negate", {"speed"}, {"u_out"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(u.has_value()) << u.error();
  const auto mid = fx.service.upsertTransform(
      "user-mid", "negate", {"u_out"}, {"mid_out"}, kNegate, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(mid.has_value()) << mid.error();
  const auto e = fx.service.upsertTransform(
      "assistant-plugin", "negate", {"mid_out"}, {"e_out"}, kNegate, "{}", /*ephemeral=*/false,
      /*input_column_index=*/0,
      /*history_exempt=*/true);
  ASSERT_TRUE(e.has_value()) << e.error();

  const std::vector<NodeId> removable = fx.service.liveNonExemptNodeIds();
  // U and mid are non-exempt (removable); E is exempt (excluded).
  EXPECT_EQ(removable.size(), 2u);

  const std::vector<std::string> blocked = fx.service.exemptDependentsOf(removable);
  ASSERT_EQ(blocked.size(), 1u);
  EXPECT_EQ(blocked.front(), "e_out");
  EXPECT_EQ(fx.service.exemptDependentsOf({u->node_id}), std::vector<std::string>{"e_out"});
}

// No exempt dependent anywhere in the graph -> exemptDependentsOf reports empty,
// so a history restore may proceed.
TEST(DataProcessorTransformTest, ExemptDependentsOfEmptyWhenNoExemptDependent) {
  Fixture fx;
  fx.seed("speed", {1.0});

  const auto a = fx.service.upsertTransform(
      "pluginA", "negate", {"speed"}, {"a_out"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/false);
  ASSERT_TRUE(a.has_value()) << a.error();
  // An exempt transform that does NOT depend on `a` — unrelated input.
  fx.seed("other", {1.0});
  const auto exempt_unrelated = fx.service.upsertTransform(
      "pluginB", "negate", {"other"}, {"b_out"}, kNegate, "{}", /*ephemeral=*/false, /*input_column_index=*/0,
      /*history_exempt=*/true);
  ASSERT_TRUE(exempt_unrelated.has_value()) << exempt_unrelated.error();

  const std::vector<NodeId> removable = fx.service.liveNonExemptNodeIds();
  EXPECT_EQ(removable.size(), 1u);
  EXPECT_TRUE(fx.service.exemptDependentsOf(removable).empty());
}

// Regression [Codex #6]: removalImpact() reports the grouped DECLARATION
// "rpy:a,b", not the materialized topic "rpy", so an exempt marker
// generator reading "rpy/a" is not found as a dependent and undo can strand it.
TEST(DataProcessorTransformTest, StrandPreflightSeesGroupedTransformOutputTopic) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});

  const auto grouped = fx.service.upsertTransform("pluginA", "split", {"speed"}, {"rpy:a,b"}, kSplit, "{}");
  ASSERT_TRUE(grouped.has_value()) << grouped.error();

  BatchEngine& markers = fx.session.batchEngine();
  markers.setResolver([](DatasetId, const std::string&, const BatchEngine::Window&) {
    BatchEngine::ResolvedSeries series;
    series.timestamps = {0.0};
    series.values = {1.0};
    return std::optional<BatchEngine::ResolvedSeries>{std::move(series)};
  });
  BatchEngine::GeneratorRecipe exempt;
  exempt.id = "assistant/exempt";
  exempt.kind = GeneratorKind::kMarkers;
  exempt.dataset_id = fx.ds;
  exempt.inputs = {"rpy/a"};
  exempt.outputs = {"exempt_markers"};
  exempt.script = "createMarker(0.0)\n";
  exempt.history_exempt = true;
  ASSERT_TRUE(markers.upsertGenerator(exempt).has_value());

  const std::vector<std::string> removed =
      fx.service.removalImpact(RestoreIntent::kHistory, {}, {}).removed_output_names;
  EXPECT_EQ(markers.exemptDependentsOf(removed), std::vector<std::string>{"exempt_markers"})
      << "removable outputs reported as: " << (removed.empty() ? std::string("<none>") : removed.front());
}

// Only a single structured entry "topic:f1,f2" is grouped into one topic; every other
// output, colon or not, is its own topic under its declared name.
TEST(DataProcessorTransformTest, MaterializedTopicNamesFollowTheGroupedOutputRule) {
  DataProcessorService::TransformRecipe recipe;
  recipe.outputs = {"rpy:roll,pitch"};
  EXPECT_EQ(DataProcessorService::materializedTopicNames(recipe), std::vector<std::string>{"rpy"});

  recipe.outputs = {"speed", "a:b"};
  EXPECT_EQ(DataProcessorService::materializedTopicNames(recipe), (std::vector<std::string>{"speed", "a:b"}));

  recipe.outputs = {"a:"};
  EXPECT_EQ(DataProcessorService::materializedTopicNames(recipe), std::vector<std::string>{"a:"});
}

// A preview ends with the panel that made it and takes what reads it along, so a
// persistent recipe may not read a preview's output: the create is refused, another
// preview may, and ending the preview leaves every persistent recipe in place.
TEST(DataProcessorTransformTest, PersistentRecipesCannotReadAPreviewOutput) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(catalogue->addBundledSource(kScale, "test").has_value());
  fx.service.setFilterCatalogue(std::move(catalogue));
  ASSERT_TRUE(fx.service.upsertTransform("user", "negate", {"speed"}, {"user_out"}, kNegate, "{}").has_value());
  const auto preview =
      fx.service.upsertTransform("panel", "negate", {"speed"}, {"preview_out"}, kNegate, "{}", /*ephemeral=*/true);
  ASSERT_TRUE(preview.has_value()) << preview.error();

  const auto transform =
      fx.service.upsertTransform("assistant", "negate", {"preview_out"}, {"kept_out"}, kNegate, "{}");
  ASSERT_FALSE(transform.has_value()) << "a persistent transform reading a preview";
  EXPECT_NE(transform.error().find("preview"), std::string::npos) << transform.error();
  EXPECT_NE(transform.error().find("as a persistent one first"), std::string::npos)
      << "the message says what to do: " << transform.error();
  const auto filter = fx.service.applyFilter(preview->output_topic_ids.front(), fx.ds, "scale", "preview_scaled");
  ASSERT_FALSE(filter.has_value()) << "a filter on a preview";
  EXPECT_NE(filter.error().find("preview"), std::string::npos) << filter.error();
  EXPECT_NE(filter.error().find("as a persistent one first"), std::string::npos)
      << "the message says what to do: " << filter.error();
  ASSERT_TRUE(fx.service
                  .upsertTransform(
                      "chain", "negate", {"preview_out"}, {"chained_preview"}, kNegate, "{}",
                      /*ephemeral=*/true)
                  .has_value())
      << "a preview may read a preview";

  ASSERT_TRUE(fx.service.removeTransform(preview->key).has_value());
  const std::vector<DataProcessorService::TransformRecipe> persistent = fx.service.transformRecipes();
  ASSERT_EQ(persistent.size(), 1u);
  EXPECT_EQ(persistent.front().key, "user/negate");
  EXPECT_TRUE(fx.service.recipes().empty());
}

// A marker rule on dataset `dataset` reading the series `topic`/value. It needs no
// sample: it draws one fixed marker.
BatchEngine::GeneratorRecipe ruleReading(DatasetId dataset, const std::string& topic) {
  BatchEngine::GeneratorRecipe recipe;
  recipe.id = "p/rule";
  recipe.dataset_id = dataset;
  recipe.inputs = {topic + "/value"};
  recipe.outputs = {"rule_markers"};
  recipe.script = "createMarker(0.0)\n";
  return recipe;
}

// Editing a transform in place, or removing it, marks the marker rules reading its
// output stale: their markers describe the old series. A rule reading another
// series is left alone.
TEST(DataProcessorTransformTest, EditingOrRemovingATransformMarksTheRulesReadingItStale) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  BatchEngine& markers = fx.session.batchEngine();
  markers.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"neg"}, kNegate, "{}").has_value());
  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "neg")).has_value());
  BatchEngine::GeneratorRecipe other = ruleReading(fx.ds, "speed");
  other.id = "p/other";
  ASSERT_TRUE(markers.upsertGenerator(other).has_value());
  ASSERT_FALSE(markers.isStale("p/rule"));

  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"neg"}, kTimes10, "{}").has_value());
  EXPECT_TRUE(markers.isStale("p/rule")) << "an in-place edit";
  EXPECT_FALSE(markers.isStale("p/other"));

  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "neg")).has_value());
  ASSERT_FALSE(markers.isStale("p/rule"));
  ASSERT_TRUE(fx.service.removeTransform("pluginA/negate").has_value());
  EXPECT_TRUE(markers.isStale("p/rule")) << "a removal";
  EXPECT_FALSE(markers.isStale("p/other"));
}

// ---------------------------------------------------------------------------
// Time-sliced first materialization (a new persistent transform)
// ---------------------------------------------------------------------------

// Large enough that the Luau replay needs several 8 ms slices even on a fast box.
constexpr std::size_t kLargeRows = 200'000;

// Stateful Luau running sum: any row a slice boundary dropped, repeated or
// reordered changes every later value. `extra_work` spins a Luau loop on each of
// the first `slow_rows` samples to make the transform slower there.
std::string accumulateScript(
    const std::string& id, double gain = 1.0, int extra_work = 0, std::size_t slow_rows = kLargeRows * 2) {
  return "-- pj-script: luau\nreturn { id=\"" + id + "\", name=\"Acc\", output=\"same\",\n" +
         "  create = function(p) local s = 0 local n = 0 return { calculate = function(t, v)\n" +
         "    n = n + 1 if n <= " + std::to_string(slow_rows) + " then for _ = 1, " + std::to_string(extra_work) +
         " do s = s + 0 end end\n" + "    s = s + v * " + std::to_string(gain) + " return s end } end }\n";
}

std::vector<std::pair<Timestamp, double>> readRows(DataEngine& engine, TopicId tid) {
  std::vector<std::pair<Timestamp, double>> out;
  const TopicStorage* storage = engine.getTopicStorage(tid);
  if (!storage) {
    return out;
  }
  auto cursor = rangeQuery(storage->sealedChunks(), 0, std::numeric_limits<Timestamp>::max());
  cursor.forEach(
      [&](const SampleRow& row) { out.emplace_back(row.timestamp, row.chunk->readNumericAsDouble(0, row.row_index)); });
  return out;
}

std::vector<double> largeInput() {
  std::vector<double> values(kLargeRows);
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<double>(static_cast<int>(i % 7) - 3);
  }
  return values;
}

// A persistent create is time-sliced; an ephemeral one replays synchronously.
Expected<DataProcessorService::TransformRecipe> createAccumulator(
    DataProcessorService& service, const std::string& id, const std::string& input, const std::string& output,
    double gain = 1.0, bool ephemeral = false, int extra_work = 0, std::size_t slow_rows = kLargeRows * 2) {
  return service.upsertTransform(
      "p", id, {input}, {output}, accumulateScript(id, gain, extra_work, slow_rows), "{}", ephemeral);
}

// Synchronous reference output of an accumulator over the current input.
std::vector<std::pair<Timestamp, double>> synchronousAccumulator(
    Fixture& fx, const std::string& input, double gain = 1.0) {
  auto sync = createAccumulator(fx.service, "reference", input, "reference_out", gain, /*ephemeral=*/true);
  EXPECT_TRUE(sync.has_value()) << sync.error();
  auto rows = readRows(fx.engine, sync->output_topic_ids.front());
  EXPECT_TRUE(fx.service.removeTransform("p/reference").has_value());
  return rows;
}

struct SliceLog {
  int slices = 0;
  std::chrono::nanoseconds longest{0};
};

SliceLog stepUntilDone(DataProcessorService& service) {
  SliceLog log;
  for (int guard = 0; guard < 1'000'000; ++guard) {
    const auto start = std::chrono::steady_clock::now();
    const bool done = service.stepMaterialization();
    log.longest = std::max<std::chrono::nanoseconds>(log.longest, std::chrono::steady_clock::now() - start);
    ++log.slices;
    if (done) {
      break;
    }
  }
  return log;
}

TEST(DataProcessorTimeSliceTest, CreateYieldsInSlicesAndMatchesTheSynchronousCreate) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  const TopicId out = sliced->output_topic_ids.front();

  // The call returned after one slice: recipe live, output partial.
  EXPECT_TRUE(fx.service.materializationPending());
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  const std::size_t after_create = readRows(fx.engine, out).size();
  EXPECT_LT(after_create, kLargeRows);

  const SliceLog log = stepUntilDone(fx.service);
  EXPECT_GE(log.slices, 2);
  // Budget plus the per-slice notify tail, with sanitizer headroom.
  EXPECT_LT(log.longest, 4 * DataProcessorService::kMaterializationSliceBudget);
  EXPECT_FALSE(fx.service.materializationPending());
  const auto rows = readRows(fx.engine, out);
  EXPECT_EQ(rows.size(), kLargeRows);
  EXPECT_EQ(rows, synchronousAccumulator(fx, "x"));
}

TEST(DataProcessorTimeSliceTest, TimerDrivesTheReplayToCompletion) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (fx.service.materializationPending() && std::chrono::steady_clock::now() < give_up) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_FALSE(fx.service.materializationPending());
  EXPECT_EQ(readRows(fx.engine, sliced->output_topic_ids.front()), synchronousAccumulator(fx, "x"));
}

TEST(DataProcessorTimeSliceTest, DeleteDuringReplayDropsIt) {
  Fixture fx;
  fx.seed("x", largeInput());
  ASSERT_TRUE(createAccumulator(fx.service, "acc", "x", "acc_out").has_value());
  ASSERT_TRUE(fx.service.materializationPending());
  ASSERT_TRUE(fx.service.removeTransform("p/acc").has_value());
  EXPECT_FALSE(fx.service.materializationPending());
  EXPECT_TRUE(fx.service.stepMaterialization());
  EXPECT_TRUE(fx.service.transformRecipes().empty());
  // The name is free again.
  EXPECT_TRUE(createAccumulator(fx.service, "acc", "x", "acc_out").has_value());
}

// An in-place edit replays the new script synchronously, which ends the pending
// replay at once: nothing is left pending, and the output is the new script's.
TEST(DataProcessorTimeSliceTest, EditDuringReplayReplaysTheNewScriptInFull) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());

  auto edited = createAccumulator(fx.service, "acc", "x", "acc_out", /*gain=*/2.0);
  ASSERT_TRUE(edited.has_value()) << edited.error();
  EXPECT_EQ(edited->output_topic_ids, sliced->output_topic_ids);
  EXPECT_FALSE(fx.service.materializationPending());
  EXPECT_EQ(readRows(fx.engine, sliced->output_topic_ids.front()), synchronousAccumulator(fx, "x", 2.0));
  EXPECT_TRUE(fx.service.stepMaterialization());
}

TEST(DataProcessorTimeSliceTest, AppendsDuringReplayAreIncluded) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());

  // Streaming tail committed mid-replay through the session commit path.
  DataWriter writer = fx.engine.createWriter();
  for (std::size_t i = kLargeRows; i < kLargeRows + 5000; ++i) {
    ASSERT_TRUE(writer.beginRow(fx.input, static_cast<Timestamp>(i) * 1'000'000'000LL).has_value());
    writer.set(fx.input, 0, 0.5);
    ASSERT_TRUE(writer.finishRow(fx.input).has_value());
  }
  (void)fx.session.commitChunks(writer.flushAll());

  stepUntilDone(fx.service);
  const auto rows = readRows(fx.engine, sliced->output_topic_ids.front());
  EXPECT_EQ(rows.size(), kLargeRows + 5000);
  EXPECT_EQ(rows, synchronousAccumulator(fx, "x"));
}

TEST(DataProcessorTimeSliceTest, ReplacedDatasetDuringReplayRecomputesOverTheNewData) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());

  // In-place reload: the dataset's chunks are swapped for new data, then the
  // replaced inputs replay.
  fx.engine.clearDatasetChunks(fx.ds);
  DataWriter writer = fx.engine.createWriter();
  for (std::size_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(writer.beginRow(fx.input, static_cast<Timestamp>(i) * 1'000'000'000LL).has_value());
    writer.set(fx.input, 0, 2.0);
    ASSERT_TRUE(writer.finishRow(fx.input).has_value());
  }
  fx.engine.commitChunks(writer.flushAll());
  ASSERT_TRUE(fx.service.rebindAndRecomputeForReplacedSources({fx.input}).has_value());

  stepUntilDone(fx.service);
  const auto rows = readRows(fx.engine, sliced->output_topic_ids.front());
  ASSERT_EQ(rows.size(), 1000u);
  EXPECT_DOUBLE_EQ(rows.back().second, 2000.0);
  EXPECT_EQ(rows, synchronousAccumulator(fx, "x"));
}

TEST(DataProcessorTimeSliceTest, UndoAndRedoOfACreateDuringReplay) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());
  const std::vector<DataProcessorService::TransformRecipe> snapshot = fx.service.transformRecipes();
  ASSERT_EQ(snapshot.size(), 1u);

  // Undo: restore the pre-create history state (no transforms).
  fx.service.clearTransforms(RestoreIntent::kHistory);
  EXPECT_FALSE(fx.service.materializationPending());
  EXPECT_TRUE(fx.service.transformRecipes().empty());

  // Redo: restore the snapshot taken mid-replay; the restore replays in full.
  auto redone = fx.service.restoreTransform(snapshot.front());
  ASSERT_TRUE(redone.has_value()) << redone.error();
  EXPECT_FALSE(fx.service.materializationPending());
  const auto rows = readRows(fx.engine, redone->output_topic_ids.front());
  EXPECT_EQ(rows.size(), kLargeRows);
  EXPECT_EQ(rows, synchronousAccumulator(fx, "x"));
}

TEST(DataProcessorTimeSliceTest, DestroyingTheSessionDuringReplayIsSafe) {
  auto fx = std::make_unique<Fixture>();
  fx->seed("x", largeInput());
  ASSERT_TRUE(createAccumulator(fx->service, "acc", "x", "acc_out").has_value());
  ASSERT_TRUE(fx->service.materializationPending());
  fx.reset();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);  // no timer may fire into the dead service
}

// Running sum that raises a runtime error once it has seen `fail_after` rows.
std::string lateFailureScript(const std::string& id, std::size_t fail_after) {
  return "-- pj-script: luau\nreturn { id=\"" + id + "\", name=\"Late\", output=\"same\",\n" +
         "  create = function(p) local s = 0 local n = 0 return { calculate = function(t, v)\n" +
         "    n = n + 1 if n > " + std::to_string(fail_after) + " then error(\"late boom\") end\n" +
         "    s = s + v return s end } end }\n";
}

TEST(DataProcessorTimeSliceTest, FailureAfterTheFirstSliceIsReportedAsAnError) {
  Fixture fx;
  fx.seed("x", largeInput());
  std::vector<Diagnostic> reported;
  fx.service.setDiagnosticSink([&reported](const Diagnostic& d) { reported.push_back(d); });

  constexpr std::size_t kFailAfter = kLargeRows - 1000;
  auto sliced =
      fx.service.upsertTransform("p", "late", {"x"}, {"late_out"}, lateFailureScript("late", kFailAfter), "{}");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();  // the first slice is healthy
  ASSERT_TRUE(fx.service.materializationPending());
  EXPECT_TRUE(reported.empty());

  stepUntilDone(fx.service);
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported.front().level, DiagnosticLevel::kError);
  EXPECT_EQ(reported.front().id, "p/late");
  EXPECT_NE(reported.front().message.find("late_out"), std::string::npos) << reported.front().message;
  EXPECT_NE(reported.front().message.find("late boom"), std::string::npos) << reported.front().message;
  EXPECT_NE(reported.front().message.find("incomplete"), std::string::npos) << reported.front().message;
  // The transform stays, with the rows committed before the failing slice.
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_LT(readRows(fx.engine, sliced->output_topic_ids.front()).size(), kFailAfter);
}

// A streaming transform that starts failing is reported once, not on every
// commit; after an edit fixes it, a new failure is reported again.
TEST(DataProcessorTimeSliceTest, StreamingTransformThatStartsFailingIsReportedOnce) {
  Fixture fx;
  fx.seed("x", {1.0, 2.0, 3.0});
  std::vector<Diagnostic> reported;
  fx.service.setDiagnosticSink([&reported](const Diagnostic& d) { reported.push_back(d); });
  const auto commit_rows = [&fx](std::size_t first, std::size_t count) {
    DataWriter writer = fx.engine.createWriter();
    for (std::size_t i = first; i < first + count; ++i) {
      ASSERT_TRUE(writer.beginRow(fx.input, static_cast<Timestamp>(i) * 1'000'000'000LL).has_value());
      writer.set(fx.input, 0, 1.0);
      ASSERT_TRUE(writer.finishRow(fx.input).has_value());
    }
    (void)fx.session.commitChunks(writer.flushAll());
  };
  ASSERT_TRUE(
      fx.service.upsertTransform("p", "late", {"x"}, {"late_out"}, lateFailureScript("late", 5), "{}").has_value());
  ASSERT_FALSE(fx.service.materializationPending());

  commit_rows(3, 1);  // 4 rows seen: healthy
  EXPECT_TRUE(reported.empty());
  commit_rows(4, 3);  // the 6th row fails
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported.front().id, "p/late");
  EXPECT_NE(reported.front().message.find("late_out"), std::string::npos) << reported.front().message;
  EXPECT_NE(reported.front().message.find("late boom"), std::string::npos) << reported.front().message;
  commit_rows(7, 3);  // still failing: not reported again
  EXPECT_EQ(reported.size(), 1u);

  // An edit replays the history healthily; its next failure is a new one.
  ASSERT_TRUE(
      fx.service.upsertTransform("p", "late", {"x"}, {"late_out"}, lateFailureScript("late", 12), "{}").has_value());
  commit_rows(10, 1);  // 11 rows: healthy
  EXPECT_EQ(reported.size(), 1u);
  commit_rows(11, 2);  // the 13th row fails
  EXPECT_EQ(reported.size(), 2u);
}

// A late commit fully recomputes the upstream transform and cascades into its
// chained one. Only the chained one fails, so only it is reported.
TEST(DataProcessorTimeSliceTest, LateCommitCascadeReportsOnlyTheFailingChainedTransform) {
  Fixture fx;
  fx.seed("x", {1.0, 2.0, 3.0});
  std::vector<Diagnostic> reported;
  fx.service.setDiagnosticSink([&reported](const Diagnostic& d) { reported.push_back(d); });
  ASSERT_TRUE(createAccumulator(fx.service, "acc", "x", "acc_out").has_value());
  ASSERT_TRUE(fx.service.upsertTransform("p", "late", {"acc_out"}, {"late_out"}, lateFailureScript("late", 3), "{}")
                  .has_value());
  ASSERT_FALSE(fx.service.materializationPending());
  ASSERT_TRUE(reported.empty());

  DataWriter writer = fx.engine.createWriter();  // behind the watermark: forces a full recompute
  ASSERT_TRUE(writer.beginRow(fx.input, 500'000'000LL).has_value());
  writer.set(fx.input, 0, 1.0);
  ASSERT_TRUE(writer.finishRow(fx.input).has_value());
  (void)fx.session.commitChunks(writer.flushAll());

  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported.front().id, "p/late");
  EXPECT_NE(reported.front().message.find("late boom"), std::string::npos) << reported.front().message;
}

// The growing output is announced about every kMaterializationNotifyInterval, not
// on every tick, and once more when the replay completes.
TEST(DataProcessorTimeSliceTest, GrowthIsAnnouncedAtTheNotifyRateAndOnCompletion) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  const TopicId out = sliced->output_topic_ids.front();
  struct Announcement {
    std::chrono::steady_clock::time_point at;
    std::size_t rows;
  };
  std::vector<Announcement> announcements;
  QObject::connect(&fx.session, &SessionManager::samplesIngested, [&](const QVector<TopicId>& ids, bool /*live*/) {
    if (ids.contains(out)) {
      announcements.push_back({std::chrono::steady_clock::now(), readRows(fx.engine, out).size()});
    }
  });

  int ticks = 0;
  for (bool done = false; !done; ++ticks) {
    done = fx.service.stepMaterialization(std::chrono::milliseconds(1));
  }
  ASSERT_GE(announcements.size(), 2u);
  EXPECT_LT(announcements.size(), static_cast<std::size_t>(ticks) / 2) << ticks << " ticks";
  // The slot stamps each announcement when it is delivered, not when the service
  // decided to announce, so two stamps can be a little closer than the interval.
  constexpr auto kDeliverySlack = std::chrono::milliseconds(1);
  for (std::size_t i = 1; i + 1 < announcements.size(); ++i) {
    EXPECT_GE(
        announcements[i].at - announcements[i - 1].at,
        DataProcessorService::kMaterializationNotifyInterval - kDeliverySlack);
  }
  EXPECT_EQ(announcements.back().rows, kLargeRows);
}

// A processor chained on a replaying output follows it inside the tick's budget,
// even when it is much slower than its parent, and ends like a synchronous chain.
TEST(DataProcessorTimeSliceTest, ChainedProcessorFollowsWithinTheBudget) {
  Fixture fx;
  fx.seed("x", largeInput());
  // The parent is slow on its first rows only, so its first slice leaves little
  // output and the (uniformly slow) child created on it catches up at once: from
  // then on the child is a caught-up dependent, not a replay of its own.
  auto parent =
      createAccumulator(fx.service, "acc", "x", "acc_out", 1.0, false, /*extra_work=*/2000, /*slow_rows=*/2000);
  ASSERT_TRUE(parent.has_value()) << parent.error();
  auto child = createAccumulator(fx.service, "slow", "acc_out", "slow_out", 1.0, false, /*extra_work=*/200);
  ASSERT_TRUE(child.has_value()) << child.error();
  ASSERT_EQ(fx.service.incrementalEngine().deferredNodes(), std::vector<NodeId>{parent->node_id});

  const SliceLog log = stepUntilDone(fx.service);
  EXPECT_LT(log.longest, 4 * DataProcessorService::kMaterializationSliceBudget);
  const auto parent_rows = readRows(fx.engine, parent->output_topic_ids.front());
  EXPECT_EQ(parent_rows, synchronousAccumulator(fx, "x"));
  EXPECT_EQ(readRows(fx.engine, child->output_topic_ids.front()), synchronousAccumulator(fx, "acc_out"));
}

// A whole-series read completes the replay behind the topic it reads — including
// through a processor chained on it — and leaves unrelated replays to the timer.
TEST(DataProcessorTimeSliceTest, FinishPendingCompletesOnlyTheReplayBehindTheReadTopic) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto first = createAccumulator(fx.service, "first", "x", "first_out");
  auto second = createAccumulator(fx.service, "second", "x", "second_out");
  ASSERT_TRUE(first.has_value() && second.has_value());
  auto chained = createAccumulator(fx.service, "chained", "first_out", "chained_out");
  ASSERT_TRUE(chained.has_value()) << chained.error();
  ASSERT_TRUE(fx.service.materializationPending());

  fx.service.finishPendingMaterialization(fx.input);  // not a replay output
  const std::vector<NodeId> pending = fx.service.incrementalEngine().deferredNodes();
  EXPECT_NE(std::find(pending.begin(), pending.end(), first->node_id), pending.end());

  fx.engine.awaitCompleteSeries(chained->output_topic_ids.front());
  const std::vector<NodeId> left = fx.service.incrementalEngine().deferredNodes();
  EXPECT_EQ(left, std::vector<NodeId>{second->node_id});
  EXPECT_EQ(readRows(fx.engine, first->output_topic_ids.front()), synchronousAccumulator(fx, "x"));
  EXPECT_EQ(readRows(fx.engine, chained->output_topic_ids.front()), synchronousAccumulator(fx, "first_out"));
  stepUntilDone(fx.service);
  EXPECT_EQ(readRows(fx.engine, second->output_topic_ids.front()), synchronousAccumulator(fx, "x"));
}

// The service is GUI-thread only: a whole-series read from another thread (a
// toolbox worker) gets the current snapshot and leaves the replay to the GUI thread.
TEST(DataProcessorTimeSliceTest, CompleteReadFromAnotherThreadLeavesTheReplayPending) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());
  const std::vector<NodeId> pending = fx.service.incrementalEngine().deferredNodes();

  std::thread reader([&fx, out = sliced->output_topic_ids.front()] { fx.engine.awaitCompleteSeries(out); });
  reader.join();

  EXPECT_TRUE(fx.service.materializationPending());
  EXPECT_EQ(fx.service.incrementalEngine().deferredNodes(), pending);
  fx.engine.awaitCompleteSeries(sliced->output_topic_ids.front());  // the GUI thread still completes it
  EXPECT_FALSE(fx.service.materializationPending());
}

TEST(DataProcessorTimeSliceTest, MarkerGeneratorReadsTheCompleteOutput) {
  Fixture fx;
  fx.seed("x", largeInput());
  auto sliced = createAccumulator(fx.service, "acc", "x", "acc_out");
  ASSERT_TRUE(sliced.has_value()) << sliced.error();
  ASSERT_TRUE(fx.service.materializationPending());
  const TopicId out = sliced->output_topic_ids.front();

  // Stand-in for the session's catalog resolver (AppSession): the complete-read
  // barrier, then the rows it hands the generator.
  std::size_t rows_seen = 0;
  fx.session.batchEngine().setResolver(
      [&](DatasetId, const std::string& key, const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "acc_out/value") {
          return std::nullopt;
        }
        fx.engine.awaitCompleteSeries(out);
        BatchEngine::ResolvedSeries resolved;
        for (const auto& [ts, value] : readRows(fx.engine, out)) {
          resolved.timestamps.push_back(static_cast<double>(ts));
          resolved.values.push_back(value);
        }
        rows_seen = resolved.values.size();
        return resolved;
      });
  BatchEngine::GeneratorRecipe recipe;
  recipe.id = "p/gen";
  recipe.kind = GeneratorKind::kMarkers;
  recipe.dataset_id = fx.ds;
  recipe.inputs = {"acc_out/value"};
  recipe.outputs = {"acc_markers"};
  recipe.script = "local s = series(\"acc_out/value\")\ncreateMarker(s:at(0).t, s:at(0).v, {label=\"first\"})\n";
  ASSERT_TRUE(fx.session.batchEngine().upsertGenerator(recipe).has_value());

  EXPECT_EQ(rows_seen, kLargeRows);
  EXPECT_FALSE(fx.service.materializationPending());
}

// A time-sliced create marks the rules reading its output stale once, when the
// replay completes: not on the partial output nor on a replay tick, and not again
// on a later idle tick.
TEST(DataProcessorTimeSliceTest, CompletedReplayMarksTheRulesReadingItStaleOnce) {
  Fixture fx;
  fx.seed("x", largeInput());
  BatchEngine& markers = fx.session.batchEngine();
  markers.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "acc_out")).has_value());
  ASSERT_TRUE(createAccumulator(fx.service, "acc", "x", "acc_out").has_value());
  ASSERT_TRUE(fx.service.materializationPending());
  EXPECT_FALSE(markers.isStale("p/rule")) << "the partial output";

  bool stale_during_replay = false;
  while (!fx.service.stepMaterialization()) {
    stale_during_replay = stale_during_replay || markers.isStale("p/rule");
  }
  EXPECT_FALSE(stale_during_replay);
  EXPECT_TRUE(markers.isStale("p/rule")) << "the completed output";

  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "acc_out")).has_value());
  EXPECT_TRUE(fx.service.stepMaterialization());
  EXPECT_FALSE(markers.isStale("p/rule"));
}

// In automatic mode the brake re-runs a rule over the transform it reads, once
// that transform was edited in place.
TEST(DataProcessorTransformTest, AutomaticModeReRunsARuleOverAnEditedTransform) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto created = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"neg"}, kNegate, "{}");
  ASSERT_TRUE(created.has_value()) << created.error();
  const TopicId output = created->output_topic_ids.front();
  BatchEngine& markers = fx.session.batchEngine();
  markers.setResolver(
      [&](DatasetId, const std::string& key, const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "neg/value") {
          return std::nullopt;
        }
        BatchEngine::ResolvedSeries resolved;
        for (const auto& [timestamp, value] : readRows(fx.engine, output)) {
          resolved.timestamps.push_back(static_cast<double>(timestamp));
          resolved.values.push_back(value);
        }
        return resolved;
      });
  BatchEngine::GeneratorRecipe rule = ruleReading(fx.ds, "neg");
  rule.script = "local s = series(\"neg/value\")\ncreateMarker(s:at(0).t, s:at(0).v, {label=\"first\"})\n";
  ASSERT_TRUE(markers.upsertGenerator(rule).has_value());
  const auto first_value = [&]() -> std::optional<double> {
    const sdk::PlotMarkers set = test::readMarkerUnion(fx.session.objectStore(), fx.ds, "rule_markers");
    return set.markers.empty() ? std::nullopt : std::optional<double>(set.markers.front().value_low);
  };
  ASSERT_EQ(first_value(), -1.0);

  ASSERT_TRUE(fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"neg"}, kTimes10, "{}").has_value());
  test::pumpUntil([&] { return first_value() == 10.0; });
  EXPECT_EQ(first_value(), 10.0);
}

// A filter output is a series a marker rule can read, so applying, editing in place
// and removing the filter mark the rules reading it stale. A rule reading another
// series is left alone.
TEST(DataProcessorTransformTest, ApplyingEditingOrRemovingAFilterMarksTheRulesReadingItStale) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(catalogue->addBundledSource(kScale, "test").has_value());
  fx.service.setFilterCatalogue(catalogue);
  BatchEngine& markers = fx.session.batchEngine();
  markers.setMode(BatchEngine::Mode::kManual);
  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "scaled")).has_value());
  BatchEngine::GeneratorRecipe other = ruleReading(fx.ds, "speed");
  other.id = "p/other";
  ASSERT_TRUE(markers.upsertGenerator(other).has_value());
  ASSERT_FALSE(markers.isStale("p/rule"));

  const auto filter = fx.service.applyFilter(fx.input, fx.ds, "scale", "scaled");
  ASSERT_TRUE(filter.has_value()) << filter.error();
  EXPECT_TRUE(markers.isStale("p/rule")) << "applying the filter";
  EXPECT_FALSE(markers.isStale("p/other"));

  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "scaled")).has_value());
  ASSERT_FALSE(markers.isStale("p/rule"));
  auto edited = catalogue->makeProcessor("scale", "{}");
  ASSERT_TRUE(edited.has_value());
  ASSERT_TRUE(
      fx.service.updateFilter(filter->node_id, std::shared_ptr<proc::DataProcessor>(std::move(*edited))).has_value());
  EXPECT_TRUE(markers.isStale("p/rule")) << "an in-place edit";
  EXPECT_FALSE(markers.isStale("p/other"));

  ASSERT_TRUE(markers.upsertGenerator(ruleReading(fx.ds, "scaled")).has_value());
  ASSERT_FALSE(markers.isStale("p/rule"));
  ASSERT_TRUE(fx.service.removeFilter(filter->node_id).has_value());
  EXPECT_TRUE(markers.isStale("p/rule")) << "removing the filter";
  EXPECT_FALSE(markers.isStale("p/other"));
}

// A reload swaps the dataset's raw data and recomputes the transforms over it; a rule
// reading a transform output ends with the markers of the NEW output, not of the
// output the reload replaced.
TEST(DataProcessorTransformTest, ReloadRunsARuleOverTheRecomputedTransformOutput) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  const auto created = fx.service.upsertTransform("pluginA", "negate", {"speed"}, {"neg"}, kTimes10, "{}");
  ASSERT_TRUE(created.has_value()) << created.error();
  const TopicId output = created->output_topic_ids.front();
  BatchEngine& markers = fx.session.batchEngine();
  markers.setResolver(
      [&](DatasetId, const std::string& key, const BatchEngine::Window&) -> std::optional<BatchEngine::ResolvedSeries> {
        if (key != "neg/value") {
          return std::nullopt;
        }
        BatchEngine::ResolvedSeries resolved;
        for (const auto& [timestamp, value] : readRows(fx.engine, output)) {
          resolved.timestamps.push_back(static_cast<double>(timestamp));
          resolved.values.push_back(value);
        }
        return resolved;
      });
  BatchEngine::GeneratorRecipe rule = ruleReading(fx.ds, "neg");
  rule.script = "local s = series(\"neg/value\")\ncreateMarker(s:at(0).t, s:at(0).v, {label=\"first\"})\n";
  ASSERT_TRUE(markers.upsertGenerator(rule).has_value());
  const auto first_value = [&]() -> std::optional<double> {
    const sdk::PlotMarkers set = test::readMarkerUnion(fx.session.objectStore(), fx.ds, "rule_markers");
    return set.markers.empty() ? std::nullopt : std::optional<double>(set.markers.front().value_low);
  };
  ASSERT_EQ(first_value(), 10.0);

  {
    RefillGuard guard = fx.session.beginRefill(fx.ds);
    DataWriter writer = fx.engine.createWriter();
    writer.appendScalar(ScalarSeriesHandle{fx.input, 0}, 0, 5.0);
    writer.appendScalar(ScalarSeriesHandle{fx.input, 0}, 1'000'000'000LL, 6.0);
    ASSERT_FALSE(
        fx.engine.commitChunks(writer.flushAll()).empty());  // the loader bypasses SessionManager::commitChunks
    ASSERT_TRUE(guard.recomputeProcessors().has_value());
    guard.pruneVanishedTopics();
    guard.commit();
  }

  EXPECT_EQ(first_value(), 50.0);
}

}  // namespace
}  // namespace PJ
