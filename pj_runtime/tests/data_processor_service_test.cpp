// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/script_engine.h"

namespace PJ {
namespace {

// Inline single-filter catalogues for the by-id service tests. The real bundled
// resource isn't linked into this headless binary; an inline Luau twin is enough
// to exercise the service mechanics (materialize, column-bind, recipe, gate).
std::shared_ptr<scripting::FilterCatalogue> catalogueFromSource(const char* src) {
  auto cat = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  EXPECT_TRUE(cat->addBundledSource(src, "bundled").has_value());
  return cat;
}
constexpr const char* kAbsoluteSrc = R"LUAU(
return { { id="absolute", name="Absolute", output="same",
  create = function(p) return { calculate = function(t, v) return math.abs(v) end } end } }
)LUAU";
constexpr const char* kIntegralSrc = R"LUAU(
return { { id="integral", name="Integral",
  create = function(p)
    local acc, has_prev, pt, pv = 0.0, false, 0.0, 0.0
    return { reset = function() acc = 0.0; has_prev = false end,
      calculate = function(t, v)
        if not has_prev then has_prev, pt, pv = true, t, v; return nil end
        acc = acc + (v + pv) * (t - pt) / 2.0; pt, pv = t, v; return acc
      end } end } }
)LUAU";
constexpr const char* kScaleSrc = R"LUAU(
return { { id="scale", name="Scale",
  parameters = { { name="value_scale", type="number", default=1.0 } },
  create = function(p) return { calculate = function(t, v) return v * p.value_scale end } end } }
)LUAU";
constexpr const char* kNegateTransformSrc = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

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

TEST(DataProcessorServiceTest, AppliesAbsoluteFilterAndMaterializesOutput) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kAbsoluteSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "speed", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, -1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  writer.appendScalar(*handle, 2'000'000'000LL, -3.0);
  engine.commitChunks(writer.flushAll());

  const auto result = service.applyFilter(src, ds, "absolute", "speed[Absolute]");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_NE(result->output_topic_id, 0u);
  EXPECT_EQ(result->output_name, "speed[Absolute]");

  const auto vals = readValues(engine, result->output_topic_id);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_DOUBLE_EQ(vals[0], 1.0);
  EXPECT_DOUBLE_EQ(vals[1], 2.0);
  EXPECT_DOUBLE_EQ(vals[2], 3.0);
}

TEST(DataProcessorServiceTest, SourceTopicsForOutputResolvesFilterInput) {
  // Demand-tracking support (M5): a displayed filter OUTPUT must resolve back to
  // its source topic so TopicDemandTracker keeps the input subscribed.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kAbsoluteSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "speed", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, -1.0);
  engine.commitChunks(writer.flushAll());

  const auto result = service.applyFilter(src, ds, "absolute", "speed[Absolute]");
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto sources = service.sourceTopicsForOutput(result->output_topic_id);
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources.front().first, ds);
  EXPECT_EQ(sources.front().second, "speed");

  // An id that names no filter/transform output resolves to nothing.
  EXPECT_TRUE(service.sourceTopicsForOutput(src).empty());
}

TEST(DataProcessorServiceTest, SourceTopicsForOutputTraversesFilterChain) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64);
  writer.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(writer.flushAll());

  const auto first = service.applyFilter(handle->topic_id, dataset, "scale", "speed[Scale]");
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto second = service.applyFilter(first->output_topic_id, dataset, "scale", "speed[Scale][Scale]");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(
      service.sourceTopicsForOutput(second->output_topic_id),
      (std::vector<std::pair<DatasetId, std::string>>{{dataset, "speed"}}));
}

TEST(DataProcessorServiceTest, SourceTopicsForOutputTraversesMixedProcessorChains) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64);
  writer.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(writer.flushAll());

  const auto filter = service.applyFilter(handle->topic_id, dataset, "scale", "speed[Scale]");
  ASSERT_TRUE(filter.has_value()) << filter.error();
  const auto transform =
      service.upsertTransform("pluginA", "negate", {"speed[Scale]"}, {"filtered/final"}, kNegateTransformSrc, "{}");
  ASSERT_TRUE(transform.has_value()) << transform.error();
  EXPECT_EQ(
      service.sourceTopicsForOutput(transform->output_topic_ids.front()),
      (std::vector<std::pair<DatasetId, std::string>>{{dataset, "speed"}}));

  const auto transform_first =
      service.upsertTransform("pluginB", "negate", {"speed"}, {"speed/negated"}, kNegateTransformSrc, "{}");
  ASSERT_TRUE(transform_first.has_value()) << transform_first.error();
  const auto filter_last = service.applyFilter(transform_first->output_topic_ids.front(), dataset, "scale", "final");
  ASSERT_TRUE(filter_last.has_value()) << filter_last.error();
  EXPECT_EQ(
      service.sourceTopicsForOutput(filter_last->output_topic_id),
      (std::vector<std::pair<DatasetId, std::string>>{{dataset, "speed"}}));
}

TEST(DataProcessorServiceTest, RemovedUpstreamFilterDoesNotBecomeRawDemand) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64);
  writer.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(writer.flushAll());

  const auto parent = service.applyFilter(handle->topic_id, dataset, "scale", "parent");
  ASSERT_TRUE(parent.has_value()) << parent.error();
  const auto child = service.applyFilter(parent->output_topic_id, dataset, "scale", "child");
  ASSERT_TRUE(child.has_value()) << child.error();
  ASSERT_TRUE(service.removeFilter(parent->node_id).has_value());
  EXPECT_TRUE(service.recipes().empty());
  EXPECT_TRUE(service.sourceTopicsForOutput(child->output_topic_id).empty());
}

TEST(DataProcessorServiceTest, RemoveFilterRetiresOutputAndFreesName) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId dataset = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(dataset, "speed", NumericType::kFloat64);
  writer.appendScalar(*handle, 0, 2.0);
  engine.commitChunks(writer.flushAll());

  const auto first = service.applyFilter(handle->topic_id, dataset, "scale", "speed[Scale]");
  ASSERT_TRUE(first.has_value()) << first.error();
  const TopicId stale_output = first->output_topic_id;
  ASSERT_TRUE(service.removeFilter(first->node_id).has_value());
  EXPECT_TRUE(readValues(engine, stale_output).empty());
  const auto recreated = service.applyFilter(handle->topic_id, dataset, "scale", "speed[Scale]");
  ASSERT_TRUE(recreated.has_value()) << recreated.error();
  EXPECT_NE(recreated->output_topic_id, stale_output);
}

TEST(DataProcessorServiceTest, IntegralFilterAccumulates) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kIntegralSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "rate", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  for (int i = 0; i < 4; ++i) {
    writer.appendScalar(*handle, static_cast<Timestamp>(i) * 1'000'000'000LL, 1.0);
  }
  engine.commitChunks(writer.flushAll());

  const auto result = service.applyFilter(src, ds, "integral", "rate[Integral]");
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto vals = readValues(engine, result->output_topic_id);
  // First sample suppressed; trapezoid of constant 1.0 over 1s steps -> 1, 2, 3.
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_DOUBLE_EQ(vals[0], 1.0);
  EXPECT_DOUBLE_EQ(vals[1], 2.0);
  EXPECT_DOUBLE_EQ(vals[2], 3.0);
}

TEST(DataProcessorServiceTest, ScriptRuntimeFailureSurfacesAsError) {
  // A filter whose calculate() raises a runtime error must FAIL the apply, not
  // silently materialize a truncated/empty output and report success.
  DataEngine engine;
  DataProcessorService service(engine);
  constexpr const char* kBoomSrc = R"LUAU(
return { { id="boom", name="Boom",
  create = function(p) return { calculate = function(t, v) error("boom") end } end } }
)LUAU";
  service.setFilterCatalogue(catalogueFromSource(kBoomSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  engine.commitChunks(writer.flushAll());

  const auto result = service.applyFilter(src, ds, "boom", "x[Boom]");
  EXPECT_FALSE(result.has_value());  // the script error is surfaced, not swallowed
}

TEST(DataProcessorServiceTest, ReloadRecomputesFilterOutput) {
  // After a dataset reload swaps the input chunks wholesale, the filter's output must
  // be RESET + REPLAYED over the new input, not left stale (or appended).
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "drive", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  engine.commitChunks(writer.flushAll());

  // Scale with default value_scale=1.0 -> output mirrors the input.
  const auto result = service.applyFilter(src, ds, "scale", "x[Scale]");
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(readValues(engine, result->output_topic_id), (std::vector<double>{1.0, 2.0}));

  // Stage a reload of the SAME dataset/topic with NEW values, then swap it in.
  DataEngine staged;
  const DatasetId staged_ds = *staged.createDataset(DatasetDescriptor{.source_name = "drive", .time_domain_id = 0});
  DataWriter sw = staged.createWriter();
  auto sh = sw.registerScalarSeries(staged_ds, "x", NumericType::kFloat64);
  sw.appendScalar(*sh, 0, 10.0);
  sw.appendScalar(*sh, 1'000'000'000LL, 20.0);
  staged.commitChunks(sw.flushAll());

  const auto rep = engine.replaceDatasetFrom(staged, staged_ds, ds);
  ASSERT_TRUE(rep.has_value()) << rep.error();

  // Recompute the filters whose input was just swapped.
  const auto outputs = service.rebindAndRecomputeForReplacedSources(rep->replaced_topics);
  ASSERT_TRUE(outputs.has_value()) << outputs.error();
  EXPECT_FALSE(outputs->empty());  // the filter on the reloaded input was recomputed
  EXPECT_EQ(readValues(engine, result->output_topic_id), (std::vector<double>{10.0, 20.0}));  // NEW data, not stale
}

// [f] A reload must propagate through CHAINED filters (a filter of a filter), not just the
// recipes whose direct input was reloaded — and every affected output must be REPORTED so the
// UI refreshes. Also guards [e]: the reloaded chained output is back in the catalog.
TEST(DataProcessorServiceTest, RecomputeForReplacedSourcesPropagatesToChainedFilter) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "drive", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  engine.commitChunks(writer.flushAll());

  // Chain: x -> scale -> scale (default value_scale=1.0, so both mirror the input).
  const auto out1 = service.applyFilter(src, ds, "scale", "x[Scale]");
  ASSERT_TRUE(out1.has_value()) << out1.error();
  const auto out2 = service.applyFilter(out1->output_topic_id, ds, "scale", "x[Scale][Scale]");
  ASSERT_TRUE(out2.has_value()) << out2.error();
  EXPECT_EQ(readValues(engine, out2->output_topic_id), (std::vector<double>{1.0, 2.0}));

  // Reload x with new values, swap it in.
  DataEngine staged;
  const DatasetId staged_ds = *staged.createDataset(DatasetDescriptor{.source_name = "drive", .time_domain_id = 0});
  DataWriter sw = staged.createWriter();
  auto sh = sw.registerScalarSeries(staged_ds, "x", NumericType::kFloat64);
  sw.appendScalar(*sh, 0, 10.0);
  sw.appendScalar(*sh, 1'000'000'000LL, 20.0);
  staged.commitChunks(sw.flushAll());
  const auto rep = engine.replaceDatasetFrom(staged, staged_ds, ds);
  ASSERT_TRUE(rep.has_value()) << rep.error();

  const auto outputs = service.rebindAndRecomputeForReplacedSources(rep->replaced_topics);
  ASSERT_TRUE(outputs.has_value()) << outputs.error();
  // BOTH outputs must be reported — out2 is the chained one a direct-input-only walk would miss.
  EXPECT_NE(std::find(outputs->begin(), outputs->end(), out1->output_topic_id), outputs->end());
  EXPECT_NE(std::find(outputs->begin(), outputs->end(), out2->output_topic_id), outputs->end());
  EXPECT_EQ(readValues(engine, out2->output_topic_id), (std::vector<double>{10.0, 20.0}));
  // [e] integration: the reloaded chained output is back in the catalog (un-retired on commit).
  const auto topics = engine.listTopics(ds);
  EXPECT_NE(std::find(topics.begin(), topics.end(), out2->output_topic_id), topics.end());
}

// [g] An initial-run failure must roll back the half-registered node + output topic so the
// output NAME is freed — otherwise the editor can never re-use that name.
TEST(DataProcessorServiceTest, InitialRunFailureRollsBackAndFreesOutputName) {
  DataEngine engine;
  DataProcessorService service(engine);
  constexpr const char* kBoomSrc = R"LUAU(
return { { id="boom", name="Boom",
  create = function(p) return { calculate = function(t, v) error("boom") end } end } }
)LUAU";
  auto cat = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  ASSERT_TRUE(cat->addBundledSource(kBoomSrc, "bundled").has_value());
  ASSERT_TRUE(cat->addBundledSource(kScaleSrc, "bundled").has_value());
  service.setFilterCatalogue(cat);
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  engine.commitChunks(writer.flushAll());

  EXPECT_FALSE(service.applyFilter(src, ds, "boom", "x[Out]").has_value());  // initial run fails
  // The failed apply must have freed "x[Out]" — a healthy filter can re-use the name.
  const auto retry = service.applyFilter(src, ds, "scale", "x[Out]");
  EXPECT_TRUE(retry.has_value()) << retry.error();
}

TEST(DataProcessorServiceTest, AppliesFilterToSelectedColumn) {
  // The GUI bug: applying a SISO filter to ONE field of a multi-column topic.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kAbsoluteSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  // 3-column float64 topic: col0=i, col1=-(i+1), col2=i+100.
  DataWriter writer = engine.createWriter();
  auto schema = makeStruct(
      "xyz", {makePrimitive("x", PrimitiveType::kFloat64), makePrimitive("y", PrimitiveType::kFloat64),
              makePrimitive("z", PrimitiveType::kFloat64)});
  const SchemaId schema_id = *writer.registerSchema("xyz", schema);
  TopicDescriptor td;
  td.name = "xyz";
  td.schema_id = schema_id;
  td.dataset_id = ds;
  const TopicId src = *writer.registerTopic(ds, td);
  for (int i = 0; i < 3; ++i) {
    (void)writer.beginRow(src, static_cast<Timestamp>(i) * 1'000'000'000LL);
    writer.set(src, 0, static_cast<double>(i));
    writer.set(src, 1, -static_cast<double>(i + 1));
    writer.set(src, 2, static_cast<double>(i + 100));
    (void)writer.finishRow(src);
  }
  engine.commitChunks(writer.flushAll());

  // Absolute filter bound to column 1 -> |-(i+1)| = i+1, NOT |col0| = i.
  const auto result = service.applyFilter(src, ds, "absolute", "y[Absolute]", /*input_column_index=*/1);
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto vals = readValues(engine, result->output_topic_id);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_DOUBLE_EQ(vals[0], 1.0);
  EXPECT_DOUBLE_EQ(vals[1], 2.0);
  EXPECT_DOUBLE_EQ(vals[2], 3.0);
}

// --- The streaming-integral correctness gate, proven through a LUAU LuaSisoTransform ---
// A trapezoidal integral filter written as a self-describing Luau class, with
// custom_dt=1.0 so each consumed sample adds exactly 1.0 (matching the C++ gate).
static constexpr const char* kLuauIntegral = R"LUAU(
return {
  id = "integral", name = "Integral",
  parameters = {
    { name="use_custom_dt", type="boolean", default=false },
    { name="custom_dt", type="number", default=1.0 },
  },
  create = function(params)
    local use_custom = params.use_custom_dt
    local fixed = params.custom_dt
    local acc, has_prev, pt, pv = 0.0, false, 0.0, 0.0
    return {
      reset = function() acc = 0.0; has_prev = false end,
      calculate = function(t, v)
        if not has_prev then has_prev, pt, pv = true, t, v; return nil end
        local dt = use_custom and fixed or (t - pt)
        acc = acc + (v + pv) * dt / 2.0
        pt, pv = t, v
        return acc
      end,
    }
  end,
}
)LUAU";

static std::pair<double, std::size_t> runPausedIntegralLuau(bool advance_before_evict) {
  constexpr int kN = 8;
  constexpr double kRetainSec = 2.5;
  DataEngine engine;
  DataProcessorService service(engine);
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});

  DataWriter w0 = engine.createWriter();
  auto handle = w0.registerScalarSeries(ds, "rate", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  w0.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(w0.flushAll());

  auto luau = PJ::scripting::makeLuauEngine();
  auto classes = luau->inspectModule(kLuauIntegral, "test");
  if (!classes.has_value()) {
    ADD_FAILURE() << "inspectModule failed: " << classes.error();
    return {0.0, 0};
  }
  auto integral = std::make_unique<PJ::scripting::LuaSisoTransform>(
      luau, classes->front(), R"({"use_custom_dt":true,"custom_dt":1.0})");
  const auto applied = service.applyFilter(src, ds, std::move(integral), "rate[Integral]");
  if (!applied.has_value()) {
    ADD_FAILURE() << "applyFilter failed: " << applied.error();
    return {0.0, 0};
  }

  std::vector<TopicId> changed_all;
  for (int i = 1; i < kN; ++i) {
    DataWriter writer = engine.createWriter();
    (void)writer.beginRow(src, static_cast<Timestamp>(i) * 1'000'000'000LL);
    writer.set(src, 0, 1.0);
    (void)writer.finishRow(src);
    const auto changed = engine.commitChunks(writer.flushAll());
    changed_all.insert(changed_all.end(), changed.begin(), changed.end());
  }

  const Timestamp window = static_cast<Timestamp>(kRetainSec * 1e9);
  if (advance_before_evict) {
    (void)service.advanceOnCommit(changed_all);
    engine.enforceRetention(window);
  } else {
    engine.enforceRetention(window);
    (void)service.advanceOnCommit(changed_all);
  }

  const auto vals = readValues(engine, applied->output_topic_id);
  const TopicStorage* in_storage = engine.getTopicStorage(src);
  return {vals.empty() ? 0.0 : vals.back(), in_storage ? in_storage->sealedChunks().size() : 0};
}

TEST(DataProcessorServiceTest, StreamingIntegralGate_Luau_AdvanceBeforeEvict) {
  // The gate must hold for a Luau-backed stateful node exactly as for the C++
  // one — the ProcessorSisoAdapter proof does NOT transfer; re-prove explicitly.
  const auto [correct, surviving_correct] = runPausedIntegralLuau(/*advance_before_evict=*/true);
  const auto [wrong, surviving_wrong] = runPausedIntegralLuau(/*advance_before_evict=*/false);

  EXPECT_DOUBLE_EQ(correct, 7.0);    // full cumulative integral over all 8 samples, via Luau
  EXPECT_LT(surviving_correct, 8u);  // old input chunks really were evicted
  (void)surviving_wrong;
  EXPECT_LT(wrong, correct);  // control: evict-before-advance loses samples
}

TEST(DataProcessorServiceTest, CatalogueRoutesByIdApplyFilterToLuau) {
  // With a FilterCatalogue installed, the by-id applyFilter resolves through it,
  // so the applied filter is a Luau LuaSisoTransform (not the C++ builtin).
  DataEngine engine;
  DataProcessorService service(engine);
  auto cat = std::make_shared<PJ::scripting::FilterCatalogue>(PJ::scripting::makeLuauEngine());
  ASSERT_TRUE(cat->addBundledSource(
                     R"LUAU(
    return { { id="absolute", name="Absolute", output="same",
      create = function(p) return { calculate = function(t, v) return math.abs(v) end } end } }
  )LUAU",
                     "bundled")
                  .has_value());
  service.setFilterCatalogue(cat);

  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter w = engine.createWriter();
  auto handle = w.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  w.appendScalar(*handle, 0, -1.0);
  w.appendScalar(*handle, 1'000'000'000LL, -2.0);
  w.appendScalar(*handle, 2'000'000'000LL, 3.0);
  engine.commitChunks(w.flushAll());

  const auto result = service.applyFilter(src, ds, "absolute", "x[Absolute]");
  ASSERT_TRUE(result.has_value());
  const auto* recipe = service.filterConfig(result->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->processor_id, "absolute");  // the Luau twin reports its class id
  const auto vals = readValues(engine, result->output_topic_id);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_DOUBLE_EQ(vals[0], 1.0);
  EXPECT_DOUBLE_EQ(vals[1], 2.0);
  EXPECT_DOUBLE_EQ(vals[2], 3.0);
}

// --- M7: persistence — filter_source capture + makeRestoredProcessor resolve ---

// A minimal bundled module defining "scale" (v*factor) with a known body, so a
// test can assert the recipe captured this exact text for <source_fallback>.
static constexpr const char* kBundledScale = R"LUAU(
return { { id="scale", name="Scale",
  parameters = { { name="factor", type="number", default=2.0 } },
  create = function(p) return { calculate = function(t, v) return v * p.factor end } end } }
)LUAU";

static std::shared_ptr<PJ::scripting::FilterCatalogue> makeScaleCatalogue() {
  auto cat = std::make_shared<PJ::scripting::FilterCatalogue>(PJ::scripting::makeLuauEngine());
  EXPECT_TRUE(cat->addBundledSource(kBundledScale, "bundled").has_value());
  return cat;
}

TEST(DataProcessorServiceTest, RecipeCarriesFilterSourceForLuauFilter) {
  // A catalogued (Luau) filter records its defining module source on the recipe,
  // so the layout can embed it as <source_fallback> for cross-machine portability.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(makeScaleCatalogue());
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter w = engine.createWriter();
  auto handle = w.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  w.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(w.flushAll());

  const auto result = service.applyFilter(src, ds, "scale", "x[Scale]");
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* recipe = service.filterConfig(result->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->processor_id, "scale");
  EXPECT_EQ(recipe->filter_source, kBundledScale);
}

TEST(DataProcessorServiceTest, MakeRestoredProcessor_ResolvesViaCatalogue) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(makeScaleCatalogue());
  auto built = service.makeRestoredProcessor("scale", R"({"factor":3.0})", /*source_fallback=*/"");
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(std::string(built->id()), "scale");
}

TEST(DataProcessorServiceTest, MakeRestoredProcessor_ResolvesViaEmbeddedSourceWhenIdAbsent) {
  // Layout portability: the installed catalogue lacks this id, but the layout
  // embedded the filter's source — restore compiles it from the embedded text.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(makeScaleCatalogue());  // has "scale", NOT "myfilter"
  static constexpr const char* kEmbedded = R"LUAU(
    return { id="myfilter", name="My",
      create = function(p) return { calculate = function(t, v) return v + 100.0 end } end }
  )LUAU";
  auto built = service.makeRestoredProcessor("myfilter", "{}", kEmbedded);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(std::string(built->id()), "myfilter");
}

TEST(DataProcessorServiceTest, MakeRestoredProcessor_SkipsUnknownWithNoFallback) {
  // Catalogue installed, id unknown, no embedded source -> null (caller logs skip).
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(makeScaleCatalogue());
  EXPECT_EQ(service.makeRestoredProcessor("nope", "{}", ""), nullptr);
}

TEST(DataProcessorServiceTest, MakeRestoredProcessor_OldLayoutIdResolvesViaCatalogue) {
  // An old processor_id-only layout (no embedded source) resolves via the live
  // catalogue, which always carries the canonical builtin ids.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kAbsoluteSrc));
  auto built = service.makeRestoredProcessor("absolute", "{}", "");
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(std::string(built->id()), "absolute");
}

TEST(DataProcessorServiceTest, MakeRestoredProcessor_EmbeddedSourceWorksWithoutCatalogue) {
  // A layout carrying its own <source_fallback> must restore even when NO catalogue
  // is installed (e.g. the bundled resource was unavailable) — the embedded source
  // is self-sufficient, compiled on a transient engine.
  DataEngine engine;
  DataProcessorService service(engine);  // no setFilterCatalogue
  static constexpr const char* kEmbedded = R"LUAU(
    return { id="myfilter", name="My",
      create = function(p) return { calculate = function(t, v) return v + 1.0 end } end }
  )LUAU";
  auto built = service.makeRestoredProcessor("myfilter", "{}", kEmbedded);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(std::string(built->id()), "myfilter");
}

TEST(DataProcessorServiceTest, EmbeddedSourceRestoreReCapturesFilterSourceForReSave) {
  // A filter restored from an embedded <source_fallback> (id NOT in this machine's
  // catalogue) must re-capture its source on the recipe, so the NEXT save keeps the
  // <source_fallback> instead of dropping it — cross-machine round-trip stability.
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(makeScaleCatalogue());  // has "scale", NOT "myfilter"
  static constexpr const char* kEmbedded = R"LUAU(
    return { id="myfilter", name="My",
      create = function(p) return { calculate = function(t, v) return v + 100.0 end } end }
  )LUAU";
  auto built = service.makeRestoredProcessor("myfilter", "{}", kEmbedded);
  ASSERT_NE(built, nullptr);

  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  DataWriter w = engine.createWriter();
  auto handle = w.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  w.appendScalar(*handle, 0, 1.0);
  engine.commitChunks(w.flushAll());

  const auto result = service.applyFilter(src, ds, std::move(built), "x[My]");
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto* recipe = service.filterConfig(result->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->filter_source, kEmbedded);  // re-captured from the live processor
}

TEST(DataProcessorServiceTest, FilterConfigRoundTripAndInPlaceUpdate) {
  // The param round-trip: a filter records its recipe (so the editor can re-open
  // and repopulate), and updating params recomputes the SAME output topic.
  DataEngine engine;
  DataProcessorService service(engine);
  auto cat = catalogueFromSource(kScaleSrc);
  service.setFilterCatalogue(cat);
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  const TopicId src = handle->topic_id;
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 2.0);
  writer.appendScalar(*handle, 2'000'000'000LL, 3.0);
  engine.commitChunks(writer.flushAll());

  // Apply Scale(x2) as a Luau twin via the configured overload.
  auto scale2 = cat->makeProcessor("scale", R"({"value_scale":2.0})");
  ASSERT_TRUE(scale2.has_value()) << scale2.error();
  const auto result = service.applyFilter(src, ds, std::move(scale2).value(), "x[Scale]");
  ASSERT_TRUE(result.has_value()) << result.error();

  // Recipe round-trip: filterConfig returns enough to repopulate the editor.
  const auto* recipe = service.filterConfig(result->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->processor_id, "scale");
  EXPECT_EQ(recipe->input_topic_id, src);
  EXPECT_EQ(recipe->node_id, result->node_id);
  ASSERT_NE(recipe->processor, nullptr);

  const auto before = readValues(engine, result->output_topic_id);
  ASSERT_EQ(before.size(), 3u);
  EXPECT_DOUBLE_EQ(before[0], 2.0);
  EXPECT_DOUBLE_EQ(before[2], 6.0);

  // Edit the params (x3) in place: same output topic, recomputed.
  auto scale3_built = cat->makeProcessor("scale", R"({"value_scale":3.0})");
  ASSERT_TRUE(scale3_built.has_value()) << scale3_built.error();
  std::shared_ptr<proc::DataProcessor> scale3 = std::move(scale3_built).value();
  ASSERT_TRUE(service.updateFilter(result->node_id, scale3).has_value());

  const auto after = readValues(engine, result->output_topic_id);
  ASSERT_EQ(after.size(), 3u);
  EXPECT_DOUBLE_EQ(after[0], 3.0);
  EXPECT_DOUBLE_EQ(after[2], 9.0);
  EXPECT_EQ(service.filterConfig(result->output_topic_id)->processor.get(), scale3.get());
}

TEST(DataProcessorServiceTest, UnknownProcessorIdFails) {
  DataEngine engine;
  DataProcessorService service(engine);
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  const auto result = service.applyFilter(1, ds, "bogus_filter", "out");
  EXPECT_FALSE(result.has_value());
}

// --- clearAllFilters: the snapshot-restore reconcile primitive (undo/redo) ---
// Dropping all filters must (a) forget every recipe, (b) retire each output topic
// so the catalog (listTopics) no longer lists it, yet (c) keep its TopicStorage
// alive so a cached reader pointer sees an empty deque, not freed memory. It must
// be idempotent, and a re-apply afterwards must mint a fresh output id under the
// same name.
TEST(DataProcessorServiceTest, ClearAllFiltersRetiresOutputsButKeepsStorageAlive) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  auto hy = writer.registerScalarSeries(ds, "y", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 1.0);
  writer.appendScalar(*hy, 0, 2.0);
  engine.commitChunks(writer.flushAll());

  const auto fx = service.applyFilter(hx->topic_id, ds, "scale", "x[Scale]");
  const auto fy = service.applyFilter(hy->topic_id, ds, "scale", "y[Scale]");
  ASSERT_TRUE(fx.has_value());
  ASSERT_TRUE(fy.has_value());
  EXPECT_EQ(service.recipes().size(), 2u);

  const auto lists = [&] { return engine.listTopics(ds); };
  const auto listed = [&](TopicId t) {
    const auto v = lists();
    return std::find(v.begin(), v.end(), t) != v.end();
  };
  ASSERT_TRUE(listed(fx->output_topic_id));
  ASSERT_TRUE(listed(fy->output_topic_id));

  service.clearAllFilters();

  EXPECT_TRUE(service.recipes().empty());
  EXPECT_FALSE(listed(fx->output_topic_id));  // retired -> dropped from the catalog
  EXPECT_FALSE(listed(fy->output_topic_id));
  EXPECT_NE(engine.getTopicStorage(fx->output_topic_id), nullptr);  // storage kept alive
  EXPECT_NE(engine.getTopicStorage(fy->output_topic_id), nullptr);

  // Idempotent: a second clear is a harmless no-op.
  service.clearAllFilters();
  EXPECT_TRUE(service.recipes().empty());

  // Re-apply after clear works, with a fresh output id under the same name.
  const auto fx2 = service.applyFilter(hx->topic_id, ds, "scale", "x[Scale]");
  ASSERT_TRUE(fx2.has_value());
  EXPECT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(fx2->output_name, "x[Scale]");
  EXPECT_NE(fx2->output_topic_id, fx->output_topic_id);
  EXPECT_TRUE(listed(fx2->output_topic_id));
}

// The exact clear->recreate cycle restoreDataProcessors performs on undo/redo: snapshot
// a filter's (id, params, output_name), clearAllFilters, then re-apply from the captured
// snapshot. The recreated filter must reproduce the same output series — proving the
// snapshot's id+params are enough to round-trip a canonical filter (the derived data is
// recomputed, never stored).
TEST(DataProcessorServiceTest, ReconcileRoundTripRecreatesFilterFromParams) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});

  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 2.0);
  writer.appendScalar(*hx, 1'000'000'000LL, 4.0);
  engine.commitChunks(writer.flushAll());

  // A non-default param, so params genuinely matter to the round-trip.
  auto built = service.makeRestoredProcessor("scale", R"({"value_scale":3.0})", "");
  ASSERT_TRUE(built != nullptr);
  const auto applied = service.applyFilter(hx->topic_id, ds, std::move(built), "x[Scale]");
  ASSERT_TRUE(applied.has_value());
  const std::vector<double> before = readValues(engine, applied->output_topic_id);
  ASSERT_EQ(before.size(), 2u);
  EXPECT_DOUBLE_EQ(before[0], 6.0);   // 2 * 3
  EXPECT_DOUBLE_EQ(before[1], 12.0);  // 4 * 3

  // Capture the recipe the way saveDataProcessors serializes it.
  const auto* recipe = service.filterConfig(applied->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  const std::string id = recipe->processor_id;
  const std::string params = recipe->processor->saveParams();
  const std::string out_name = recipe->output_name;

  // Reconcile: drop everything, then recreate from the captured snapshot.
  service.clearAllFilters();
  ASSERT_TRUE(service.recipes().empty());

  auto rebuilt = service.makeRestoredProcessor(id, params, "");
  ASSERT_TRUE(rebuilt != nullptr);
  const auto reapplied = service.applyFilter(hx->topic_id, ds, std::move(rebuilt), out_name);
  ASSERT_TRUE(reapplied.has_value());
  EXPECT_EQ(reapplied->output_name, out_name);
  EXPECT_EQ(readValues(engine, reapplied->output_topic_id), before);  // same series reproduced
}

// unchangedFilterOutputs names (by output TopicId) the live filters a snapshot
// leaves as they are: same (dataset, output_name) + identical persisted fields
// (resolved input, processor id, params, embedded source). Any difference means
// "replace", so the id is not returned; clearAllFilters(keep) then leaves the
// unchanged one live under its TopicId and removes the rest.
TEST(DataProcessorServiceTest, UnchangedFilterOutputsMatchOnlyIdenticalPersistedFilters) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  auto hy = writer.registerScalarSeries(ds, "y", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 2.0);
  writer.appendScalar(*hy, 0, 5.0);
  engine.commitChunks(writer.flushAll());
  auto scaled = service.makeRestoredProcessor("scale", R"({"value_scale":3.0})", "");
  ASSERT_TRUE(scaled != nullptr);
  const auto kept = service.applyFilter(hx->topic_id, ds, std::move(scaled), "x[Scale]");
  ASSERT_TRUE(kept.has_value()) << kept.error();
  const auto dropped = service.applyFilter(hy->topic_id, ds, "scale", "y[Scale]");
  ASSERT_TRUE(dropped.has_value()) << dropped.error();
  const auto* live = service.filterConfig(kept->output_topic_id);
  ASSERT_NE(live, nullptr);

  DataProcessorService::PersistedFilter same;
  same.input_topic_id = live->input_topic_id;
  same.dataset_id = live->dataset_id;
  same.input_column_index = live->input_column_index;
  same.processor_id = live->processor_id;
  same.params_json = live->processor->saveParams();
  same.filter_source = live->filter_source;
  same.output_name = live->output_name;
  EXPECT_EQ(service.unchangedFilterOutputs({same}), (std::unordered_set<TopicId>{kept->output_topic_id}));

  DataProcessorService::PersistedFilter other_params = same;
  other_params.params_json = R"({"value_scale":4.0})";
  EXPECT_TRUE(service.unchangedFilterOutputs({other_params}).empty());
  DataProcessorService::PersistedFilter other_input = same;
  other_input.input_topic_id = hy->topic_id;
  EXPECT_TRUE(service.unchangedFilterOutputs({other_input}).empty());
  DataProcessorService::PersistedFilter other_column = same;
  other_column.input_column_index = 1;
  EXPECT_TRUE(service.unchangedFilterOutputs({other_column}).empty());
  DataProcessorService::PersistedFilter other_processor = same;
  other_processor.processor_id = "absolute";
  EXPECT_TRUE(service.unchangedFilterOutputs({other_processor}).empty());
  DataProcessorService::PersistedFilter renamed = same;
  renamed.output_name = "x[Scaled]";
  EXPECT_TRUE(service.unchangedFilterOutputs({renamed}).empty());
  DataProcessorService::PersistedFilter other_dataset = same;
  other_dataset.dataset_id = ds + 1;
  EXPECT_TRUE(service.unchangedFilterOutputs({other_dataset}).empty());

  // The keep set feeds both the removal view and the clear.
  EXPECT_EQ(service.liveNonExemptNodeIds({}, {kept->output_topic_id}), std::vector<NodeId>{dropped->node_id});
  EXPECT_EQ(service.liveNonExemptOutputNames({}, {kept->output_topic_id}), std::vector<std::string>{"y[Scale]"});
  service.clearAllFilters({kept->output_topic_id});
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes().front().output_topic_id, kept->output_topic_id);
  EXPECT_EQ(service.filterConfig(dropped->output_topic_id), nullptr);
  service.clearAllFilters();
  EXPECT_TRUE(service.recipes().empty());
}

// Filter output names are unique per dataset, not globally: the
// same name in two datasets is two filters, and a snapshot naming only one of
// them keeps exactly that one.
TEST(DataProcessorServiceTest, UnchangedFilterOutputsKeySameNameByDataset) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds_a = *engine.createDataset(DatasetDescriptor{.source_name = "a", .time_domain_id = 0});
  const DatasetId ds_b = *engine.createDataset(DatasetDescriptor{.source_name = "b", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto xa = writer.registerScalarSeries(ds_a, "x", NumericType::kFloat64);
  auto xb = writer.registerScalarSeries(ds_b, "x", NumericType::kFloat64);
  writer.appendScalar(*xa, 0, 2.0);
  writer.appendScalar(*xb, 0, 5.0);
  engine.commitChunks(writer.flushAll());
  const auto in_a = service.applyFilter(xa->topic_id, ds_a, "scale", "filtered");
  const auto in_b = service.applyFilter(xb->topic_id, ds_b, "scale", "filtered");
  ASSERT_TRUE(in_a.has_value()) << in_a.error();
  ASSERT_TRUE(in_b.has_value()) << in_b.error();

  DataProcessorService::PersistedFilter wanted_a;
  wanted_a.input_topic_id = xa->topic_id;
  wanted_a.dataset_id = ds_a;
  wanted_a.processor_id = "scale";
  wanted_a.params_json = service.filterConfig(in_a->output_topic_id)->processor->saveParams();
  wanted_a.filter_source = service.filterConfig(in_a->output_topic_id)->filter_source;
  wanted_a.output_name = "filtered";
  const std::unordered_set<TopicId> kept = service.unchangedFilterOutputs({wanted_a});
  EXPECT_EQ(kept, (std::unordered_set<TopicId>{in_a->output_topic_id}));

  service.clearAllFilters(kept);
  ASSERT_EQ(service.recipes().size(), 1u);
  EXPECT_EQ(service.recipes().front().output_topic_id, in_a->output_topic_id);
}

// A Windows checkout gives the bundled filter sources CRLF line endings, while
// the snapshot copy of the same text went through the layout XML (CDATA), where
// end-of-line normalization leaves "\n". Line endings are not identity: the live
// filter stays "unchanged", or every Luau-sourced filter would replay on undo.
TEST(DataProcessorServiceTest, UnchangedFilterOutputsIgnoreLineEndingDifferencesInSource) {
  std::string crlf_source;
  for (const char ch : std::string_view(kScaleSrc)) {
    if (ch == '\n') {
      crlf_source.push_back('\r');
    }
    crlf_source.push_back(ch);
  }
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(crlf_source.c_str()));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 2.0);
  engine.commitChunks(writer.flushAll());
  const auto live = service.applyFilter(hx->topic_id, ds, "scale", "x[Scale]");
  ASSERT_TRUE(live.has_value()) << live.error();
  const auto* recipe = service.filterConfig(live->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  ASSERT_EQ(recipe->filter_source, crlf_source);

  DataProcessorService::PersistedFilter wanted;
  wanted.input_topic_id = recipe->input_topic_id;
  wanted.dataset_id = recipe->dataset_id;
  wanted.input_column_index = recipe->input_column_index;
  wanted.processor_id = recipe->processor_id;
  wanted.params_json = recipe->processor->saveParams();
  wanted.filter_source = kScaleSrc;
  wanted.output_name = recipe->output_name;
  EXPECT_EQ(service.unchangedFilterOutputs({wanted}), (std::unordered_set<TopicId>{live->output_topic_id}));
}

// Only the CRLF pair is a line-ending difference. A lone '\r' inside a Luau
// long bracket is data the script can observe, so a persisted copy without it
// describes a different filter and the live one must not count as unchanged.
TEST(DataProcessorServiceTest, UnchangedFilterOutputsTreatStandaloneCarriageReturnAsData) {
  const std::string live_source = std::string(kScaleSrc) + "--[[a\rb]]\n";
  const std::string persisted_source = std::string(kScaleSrc) + "--[[ab]]\n";
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(live_source.c_str()));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 2.0);
  engine.commitChunks(writer.flushAll());
  const auto live = service.applyFilter(hx->topic_id, ds, "scale", "x[Scale]");
  ASSERT_TRUE(live.has_value()) << live.error();
  const auto* recipe = service.filterConfig(live->output_topic_id);
  ASSERT_NE(recipe, nullptr);
  ASSERT_EQ(recipe->filter_source, live_source);

  DataProcessorService::PersistedFilter wanted;
  wanted.input_topic_id = recipe->input_topic_id;
  wanted.dataset_id = recipe->dataset_id;
  wanted.input_column_index = recipe->input_column_index;
  wanted.processor_id = recipe->processor_id;
  wanted.params_json = recipe->processor->saveParams();
  wanted.filter_source = persisted_source;
  wanted.output_name = recipe->output_name;
  EXPECT_TRUE(service.unchangedFilterOutputs({wanted}).empty());
}

// The names a history restore removes include the cascade: a kept
// filter reading a removed one falls with it, so a marker generator reading the
// kept filter's output must see that name in the removal view.
TEST(DataProcessorServiceTest, LiveNonExemptOutputNamesIncludeCascadedDependents) {
  DataEngine engine;
  DataProcessorService service(engine);
  service.setFilterCatalogue(catalogueFromSource(kScaleSrc));
  const DatasetId ds = *engine.createDataset(DatasetDescriptor{.source_name = "t", .time_domain_id = 0});
  DataWriter writer = engine.createWriter();
  auto hx = writer.registerScalarSeries(ds, "x", NumericType::kFloat64);
  writer.appendScalar(*hx, 0, 2.0);
  engine.commitChunks(writer.flushAll());
  const auto head = service.applyFilter(hx->topic_id, ds, "scale", "head");
  ASSERT_TRUE(head.has_value()) << head.error();
  const auto tail = service.applyFilter(head->output_topic_id, ds, "scale", "tail");
  ASSERT_TRUE(tail.has_value()) << tail.error();

  EXPECT_EQ(service.liveNonExemptOutputNames({}, {tail->output_topic_id}), (std::vector<std::string>{"head", "tail"}));
  EXPECT_EQ(service.liveNonExemptOutputNames({}, {head->output_topic_id}), (std::vector<std::string>{"tail"}));
  EXPECT_TRUE(service.liveNonExemptOutputNames({}, {head->output_topic_id, tail->output_topic_id}).empty());
}

}  // namespace
}  // namespace PJ
