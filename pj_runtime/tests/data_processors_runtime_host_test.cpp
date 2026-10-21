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
#include "pj_runtime/DataProcessorsKindRouter.h"
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

// The three-way check over transforms always decides: a filter that compiles is VALID, one that does not is INVALID
// with the compile error, never UNCHECKED. A kind or language transforms do not run is a call that could not be made:
// false, the status untouched, no compiler run.
TEST(DataProcessorsRuntimeHostTest, CheckScriptAnswersValidOrInvalid) {
  Fixture fx;
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  ASSERT_TRUE(PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, bridge.raw().vtable, check_data_processor_script));
  const sdk::DataProcessorsHostView view(bridge.raw());

  // A validate compiles the filter class whose id is "__validate__" (DataProcessorService::validateScript).
  constexpr const char* kValidated = R"LUAU(-- pj-script: luau
return { id="__validate__", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";
  const auto valid = view.checkScript("transform", "luau", kValidated);
  ASSERT_TRUE(valid) << valid.error();
  EXPECT_EQ(valid->status, sdk::ScriptCheckStatus::kValid) << valid->message;
  const auto invalid = view.checkScript("transform", "luau", "return {");
  ASSERT_TRUE(invalid) << invalid.error();
  EXPECT_EQ(invalid->status, sdk::ScriptCheckStatus::kInvalid);
  EXPECT_FALSE(invalid->message.empty());

  const PJ_data_processors_host_t raw = bridge.raw();
  const auto refused = [&raw](std::string_view kind, std::string_view language, std::string_view expected) {
    uint32_t status = 77;
    PJ_error_t err{};
    EXPECT_FALSE(raw.vtable->check_data_processor_script(
        raw.ctx, sdk::toAbiString(kind), sdk::toAbiString(language), sdk::toAbiString("return {"),
        sdk::toAbiString("{}"), &status, &err))
        << kind << "/" << language;
    EXPECT_EQ(status, 77u) << kind << "/" << language;
    EXPECT_NE(std::string_view(err.message).find(expected), std::string_view::npos) << err.message;
  };
  refused("transform", "ruby", "unsupported script language 'ruby'");
  refused("bogus", "luau", "unknown data processor kind 'bogus'");
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

  // Explicit teardown (session close) still removes it.
  fx.service.clearAllTransforms();
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

// A persistent create over a large history returns after its first slice and
// finishes from the event loop; the ephemeral preview stays synchronous because
// its caller reads the whole output right back.
TEST(DataProcessorsRuntimeHostTest, PersistentCreateIsTimeSlicedAndEphemeralIsSynchronous) {
  constexpr std::size_t kRows = 200'000;
  Fixture fx;
  fx.seed("speed", std::vector<double>(kRows, 1.0));
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const std::string_view inputs[] = {"speed"};
  const std::string_view preview_outputs[] = {"preview"};
  ASSERT_TRUE(view.createEphemeralTransform(
      "negate", PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(preview_outputs), kNegate,
      "{}"));
  EXPECT_FALSE(fx.service.materializationPending());
  ASSERT_TRUE(view.remove("negate"));

  const auto status = createNegate(view, "negate", "speed", "speed/out");
  ASSERT_TRUE(status) << status.error();
  EXPECT_TRUE(fx.service.materializationPending());
  const TopicId out = fx.service.transformRecipes().front().output_topic_ids.front();
  EXPECT_LT(readValues(fx.engine, out).size(), kRows);

  while (!fx.service.stepMaterialization()) {}
  EXPECT_EQ(readValues(fx.engine, out), std::vector<double>(kRows, -1.0));
}

sdk::DataProcessorRequest negateRequest(std::string_view id) {
  sdk::DataProcessorRequest request;
  request.id = std::string(id);
  request.kind = "transform";
  request.language = "luau";
  request.script = kNegate;
  request.params_json = "{}";
  request.inputs = {"speed"};
  request.outputs = {{"speed/out", ""}};
  return request;
}

// ABI-34: an untyped typed create is the legacy create.
TEST(DataProcessorsRuntimeHostTest, CreateV2FallsBackToLegacyCreate) {
  Fixture fx;
  fx.seed("speed", {-1.0, 2.0, -3.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const auto topics = view.createV2(negateRequest("negate"));
  ASSERT_TRUE(topics.has_value()) << topics.error();
  EXPECT_EQ(*topics, std::vector<std::string>{"speed/out"});

  while (!fx.service.stepMaterialization()) {}
  const auto recipes = fx.service.transformRecipes();
  ASSERT_EQ(recipes.size(), 1u);
  EXPECT_EQ(recipes.front().key, "pluginA/negate");
  EXPECT_EQ(readValues(fx.engine, recipes.front().output_topic_ids.front()), (std::vector<double>{1.0, -2.0, 3.0}));
  EXPECT_TRUE(bridge.owns("negate"));
  EXPECT_FALSE(bridge.owns("absent"));
}

// ABI-34: the transform kind has no on-demand evaluation; the router refuses a submit
// naming the kind (the bridge has no evaluation slots to reach).
TEST(DataProcessorsRuntimeHostTest, SubmitOnTransformIsRejected) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  DataProcessorsKindRouter router(
      PJ_data_processors_host_t{}, bridge.raw(), PJ_data_processors_host_t{},
      {.transforms = [&bridge](std::string_view id) { return bridge.owns(id); }});
  sdk::DataProcessorsHostView view(router.raw());
  ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/out"));

  sdk::DataProcessorRequest request;
  request.id = "negate";
  request.kind = "transform";
  request.instant_ns = 0;
  const auto handle = view.submitEvaluation(request);
  ASSERT_FALSE(handle.has_value());
  EXPECT_NE(handle.error().find("no on-demand evaluation for kind 'transform'"), std::string::npos) << handle.error();
}

// Until the catalog v2 work, the transform kind takes no time flag, typed output or
// dataset_id: each is refused naming the kind, and nothing is created.
TEST(DataProcessorsRuntimeHostTest, CreateV2WithATimeFlagOrDatasetIdIsRejectedNamingTheKind) {
  Fixture fx;
  fx.seed("speed", {1.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());

  const auto expectRefused = [&](const sdk::DataProcessorRequest& request, std::string_view needle) {
    const auto topics = view.createV2(request);
    ASSERT_FALSE(topics.has_value()) << needle;
    EXPECT_NE(topics.error().find("kind 'transform'"), std::string::npos) << topics.error();
    EXPECT_NE(topics.error().find(needle), std::string::npos) << topics.error();
  };
  sdk::DataProcessorRequest windowed = negateRequest("negate");
  windowed.window = std::pair<int64_t, int64_t>{0, 10};
  expectRefused(windowed, "time_flags");
  sdk::DataProcessorRequest pinned = negateRequest("negate");
  pinned.instant_ns = 0;
  expectRefused(pinned, "time_flags");
  sdk::DataProcessorRequest on_dataset = negateRequest("negate");
  on_dataset.dataset_id = fx.ds;
  expectRefused(on_dataset, "takes no dataset_id yet: qualify the inputs with '<source>:'");
  sdk::DataProcessorRequest python = negateRequest("negate");
  python.language = "python";
  expectRefused(python, "language 'luau' only, not 'python'");
  sdk::DataProcessorRequest typed_output = negateRequest("negate");
  typed_output.outputs = {{"speed/out", "number"}};
  expectRefused(typed_output, "untyped outputs");

  EXPECT_TRUE(fx.service.transformRecipes().empty());
  EXPECT_FALSE(bridge.owns("negate"));
}

// Previews are not the plugin's inventory: `list` hides them, while `config` by exact
// id and the router's ownership probe still see them, so a plugin can remove one.
TEST(DataProcessorsRuntimeHostTest, ListHidesEphemeralTransformsButConfigAnswersTheOwner) {
  Fixture fx;
  fx.seed("speed", {1.0, 2.0});
  DataProcessorsRuntimeHost bridge(fx.service, "pluginA");
  sdk::DataProcessorsHostView view(bridge.raw());
  const std::string_view inputs[] = {"speed"};
  const std::string_view preview_outputs[] = {"preview"};
  std::string probe_script = kNegate;  // a script names the processor id it serves
  probe_script.replace(probe_script.find("id=\"negate\""), 11, "id=\"probe\"");
  const Status preview = view.createEphemeralTransform(
      "probe", PJ::Span<const std::string_view>(inputs), PJ::Span<const std::string_view>(preview_outputs),
      probe_script, "{}");
  ASSERT_TRUE(preview) << preview.error();
  ASSERT_TRUE(createNegate(view, "negate", "speed", "speed/out"));

  const auto ids = view.list();
  ASSERT_TRUE(ids.has_value());
  EXPECT_EQ(*ids, std::vector<std::string>{"negate"});
  EXPECT_TRUE(view.recipeOf("probe").has_value());
  EXPECT_TRUE(bridge.owns("probe")) << "the host still sees the preview";
  EXPECT_TRUE(bridge.owns("negate"));
  EXPECT_FALSE(bridge.owns("absent"));
  EXPECT_TRUE(view.remove("probe"));
  EXPECT_FALSE(bridge.owns("probe"));
}

}  // namespace
}  // namespace PJ
