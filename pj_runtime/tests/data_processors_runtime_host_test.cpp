// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// M4.2 Task 5/7 — the pj.data_processors.v1 host bridge (DataProcessorsRuntimeHost)
// over DataProcessorService, plus the exit-demo contract: a plugin creates a
// transform through the C ABI; it runs; the plugin (bridge) is destroyed; the
// transform KEEPS RUNNING (the host owns the script). Per-plugin isolation: a
// second plugin can neither see nor remove the first's transforms.

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DataProcessorsRuntimeHost.h"

namespace PJ {
namespace {

constexpr const char* kNegate = R"LUAU(-- pj-script: luau
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

struct Fixture {
  DataEngine engine;
  DataProcessorService service{engine};
  DatasetId ds = 0;

  void seed(const char* name, const std::vector<double>& values) {
    ds = *engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
    DataWriter writer = engine.createWriter();
    auto handle = writer.registerScalarSeries(ds, name, NumericType::kFloat64);
    for (std::size_t i = 0; i < values.size(); ++i) {
      writer.appendScalar(*handle, static_cast<Timestamp>(i) * 1'000'000'000LL, values[i]);
    }
    engine.commitChunks(writer.flushAll());
  }
};

// Create one transform through the bridge's C ABI view.
Status createNegate(
    sdk::DataProcessorsHostView& view, std::string_view id, std::string_view input, std::string_view output) {
  const std::string_view inputs[] = {input};
  const std::string_view outputs[] = {output};
  return view.createTransform(
      id, PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(outputs), kNegate, "{}");
}

TEST(DataProcessorsRuntimeHostTest, CreateRunsTransformViaAbi) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0, -3.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  ASSERT_TRUE(view.valid());
  const auto status = createNegate(view, "negate", "speed", "speed/out");
  ASSERT_TRUE(status) << status.error();

  const auto recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  const auto& recipe = recipes.front();
  EXPECT_EQ(recipe.key, "pluginA/negate");
  EXPECT_EQ(readValues(fx.engine, recipe.output_topic_ids.front()), (std::vector<double>{1.0, -2.0, 3.0}));
}

TEST(DataProcessorsRuntimeHostTest, ListAndConfigRoundTrip) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());
  ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/out"));

  // list: count-then-fill, owned copies (per-plugin scoped).
  const auto ids = view.list();
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u);
  EXPECT_EQ((*ids)[0], "negate");

  // config: the recipe JSON for re-edit.
  const auto recipe = view.recipeOf("negate");
  ASSERT_TRUE(recipe) << recipe.error();
  EXPECT_NE(recipe->find("\"inputs\""), std::string::npos);
  EXPECT_NE(recipe->find("speed"), std::string::npos);
  EXPECT_NE(recipe->find("speed/out"), std::string::npos);

  // config on an unknown id is an error.
  EXPECT_FALSE(view.recipeOf("ghost"));
}

TEST(DataProcessorsRuntimeHostTest, RemoveViaAbi) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());
  ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/out"));

  ASSERT_TRUE(view.remove("negate"));
  EXPECT_TRUE(fx.service.transformRecipes().empty());
  EXPECT_FALSE(view.remove("negate"));  // unknown now -> error
}

TEST(DataProcessorsRuntimeHostTest, SecondPluginCannotSeeOrRemoveFirst) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge_a(fx.service, "pluginA");
  DataProcessorsRuntimeHost bridge_b(fx.service, "pluginB");
  sdk::DataProcessorsHostView view_a(bridge_a.raw());
  sdk::DataProcessorsHostView view_b(bridge_b.raw());

  ASSERT_TRUE(createNegate(view_a, "negate", "speed", "speed/out"));

  const auto b_ids = view_b.list();
  ASSERT_TRUE(b_ids) << b_ids.error();
  EXPECT_TRUE(b_ids->empty());            // B sees none of A's
  EXPECT_FALSE(view_b.remove("negate"));  // B cannot remove A's
  EXPECT_FALSE(view_b.recipeOf("negate"));

  const auto a_ids = view_a.list();
  ASSERT_TRUE(a_ids) << a_ids.error();
  ASSERT_EQ(a_ids->size(), 1u);  // A's is untouched
}

TEST(DataProcessorsRuntimeHostTest, TransformSurvivesBridgeDestruction) {
  // The exit-demo core: the plugin DSO unload is modeled by destroying the bridge.
  // The host owns the script + VM, so the transform node KEEPS RUNNING.
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0});
  TopicId out = 0;
  {
    DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
    sdk::DataProcessorsHostView view(bridge.raw());
    ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/out"));
    ASSERT_EQ(fx.service.transformRecipes().size(), 1u);
    out = fx.service.transformRecipes().front().output_topic_ids.front();
  }  // <- bridge destroyed (DSO unload)

  // Node + output survive and still hold the computed series.
  EXPECT_EQ(fx.service.transformRecipes().size(), 1u);
  EXPECT_EQ(readValues(fx.engine, out), (std::vector<double>{1.0, -2.0}));

  // Explicit teardown (plugin uninstall / session close) still removes it.
  fx.service.clearTransformsForPlugin("pluginA");
  EXPECT_TRUE(fx.service.transformRecipes().empty());
}

TEST(DataProcessorsRuntimeHostTest, CreateErrorSurfacesAcrossAbi) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  // Unknown input name -> the service error is marshalled back as a failed Status.
  const auto status = createNegate(view, "negate", "does_not_exist", "out");
  EXPECT_FALSE(status);
  EXPECT_FALSE(status.error().empty());
  EXPECT_TRUE(fx.service.transformRecipes().empty());
}

// The ABI contract (plugin_data_api.h's `flags` paragraph) requires the host to
// REJECT a reserved/unknown bit, not silently drop it — a stale bit is
// indistinguishable, from the plugin's side, from the host applying it and the
// effect just not taking. Bit 5 is unassigned by any PJ_DATA_PROCESSOR_FLAG_*.
TEST(DataProcessorsRuntimeHostTest, ReservedFlagBitIsRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"speed"};
  const std::string_view outputs[] = {"speed/out"};
  const auto status = view.createTransform(
      "negate", PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(outputs), kNegate, "{}",
      /*flags=*/1u << 5);

  EXPECT_FALSE(status);
  EXPECT_NE(status.error().find("reserved"), std::string::npos) << status.error();
  EXPECT_TRUE(fx.service.transformRecipes().empty());
}

// EPHEMERAL combined with HISTORY_EXEMPT is a known combination and must be
// accepted.
TEST(DataProcessorsRuntimeHostTest, KnownFlagCombinationIsAccepted) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"speed"};
  const std::string_view outputs[] = {"speed/out"};
  const uint32_t known_flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL | PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
  const auto status = view.createTransform(
      "negate", PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(outputs), kNegate, "{}",
      known_flags);

  EXPECT_TRUE(status) << status.error();
}

// The `config` slot echoes history_exempt so a plugin can PROBE whether its
// exemption request took.
TEST(DataProcessorsRuntimeHostTest, ConfigEchoesHistoryExemptBit) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"speed"};
  const std::string_view exempt_outputs[] = {"speed/exempt"};
  const uint32_t history_exempt_flag = PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
  ASSERT_TRUE(view.createTransform(
      "negate", PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(exempt_outputs), kNegate,
      "{}", history_exempt_flag));
  const auto exempt_recipe = view.recipeOf("negate");
  ASSERT_TRUE(exempt_recipe) << exempt_recipe.error();
  EXPECT_NE(exempt_recipe->find("\"history_exempt\":true"), std::string::npos) << *exempt_recipe;

  ASSERT_TRUE(view.remove("negate"));
  ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/plain"));
  const auto plain_recipe = view.recipeOf("negate");
  ASSERT_TRUE(plain_recipe) << plain_recipe.error();
  EXPECT_NE(plain_recipe->find("\"history_exempt\":false"), std::string::npos) << *plain_recipe;
}

}  // namespace
}  // namespace PJ
