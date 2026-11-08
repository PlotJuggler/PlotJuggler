// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pure helpers of the recipe library. Path resolution against a catalog is covered by the
// tool registry tests (they drive it through the same functions); this file pins the text and
// parsing contracts the executors rely on.
#include <gtest/gtest.h>

#include "pj_runtime/agent_tools/derived_recipes.hpp"

using namespace PJ::agent_tools;

TEST(DerivedRecipes, ParseTypedOutputsSplitsNameAndType) {
  const auto parsed = parseTypedOutputs(nlohmann::json::array({"cropped:kPointCloud", "count:number"}));
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;
  ASSERT_EQ(parsed.outputs.size(), 2u);
  EXPECT_EQ(parsed.outputs[0].name, "cropped");
  EXPECT_EQ(parsed.outputs[0].type, "kPointCloud");
  EXPECT_EQ(parsed.outputs[1].type, "number");
}

TEST(DerivedRecipes, ParseTypedOutputsRejectsMalformedEntries) {
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array()).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({"noType"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({":number"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({"name:"})).error.empty());
  EXPECT_FALSE(parseTypedOutputs(nlohmann::json::array({1})).error.empty());
}

TEST(DerivedRecipes, LuaStringEscapeProtectsTheLiteral) {
  EXPECT_EQ(luaStringEscape("a\"b\\c\nd\re\tf"), "a\\\"b\\\\c\\nd\\re\\tf");
}

TEST(DerivedRecipes, OnDemandChunkBindsInputsAndParamsFromVarargs) {
  const std::string chunk = buildOnDemandChunk("return {}");
  EXPECT_EQ(chunk, "-- pj-script: luau\nlocal inputs, params = ...\nreturn {}\n");
}

TEST(DerivedRecipes, ResolvedChunkAliasesEveryScriptKeyToTheQualifiedRequestKey) {
  ResolvedEvalInputs resolved;
  resolved.aliases = {{"/cloud", "run1:/cloud"}, {"run1:/cloud", "run1:/cloud"}};
  const std::string chunk = buildResolvedOnDemandChunk("return {}", resolved);
  EXPECT_NE(chunk.find("[\"/cloud\"] = inputs[\"run1:/cloud\"]"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("local inputs, params = ...\ninputs = {"), std::string::npos) << chunk;
  EXPECT_NE(chunk.find("return {}"), std::string::npos);
}

TEST(DerivedRecipes, LuauTransformKeepsTheValueAndExtraInputParameters) {
  const std::string script = buildTransformScript("id", "name", "g = 1", "return value + v1", 1);
  EXPECT_NE(script.find("return function(time, value, v1)"), std::string::npos) << script;
  EXPECT_NE(script.find("id = \"id\""), std::string::npos);
}

TEST(DerivedRecipes, ToRawNsIsEmptyWithoutAPlaybackView) {
  PJ::sdk::PlaybackHostView unbound;
  EXPECT_FALSE(toRawNs(unbound, PJ::sdk::DataSourceHandle{}, 1.0).has_value());
}

TEST(DerivedRecipes, QualifyAndJoinHelpers) {
  EXPECT_EQ(qualifyWithDataset("", "a/b"), "a/b");
  EXPECT_EQ(qualifyWithDataset("run1", "a/b"), "run1:a/b");
  EXPECT_EQ(joinSeriesPath("topic", "/field"), "topic/field");
  EXPECT_EQ(joinSeriesPath("topic", "field"), "topic/field");
  EXPECT_EQ(canonicalSeriesPath("a//b///c"), "a/b/c");
  EXPECT_TRUE(isMarkerObjectTopic("__markers__/x"));
  EXPECT_FALSE(isMarkerObjectTopic("/cloud"));
}

TEST(DerivedRecipes, TransformScriptEscapesTheIdAndName) {
  const std::string script = buildTransformScript("a\"b", "n\nm", "", "return value", 0);
  EXPECT_NE(script.find("id = \"a\\\"b\""), std::string::npos) << script;
  EXPECT_NE(script.find("name = \"n\\nm\""), std::string::npos) << script;
}
