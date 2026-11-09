// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scripting/filter_catalogue.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <variant>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {
constexpr const char* kTwo = R"LUAU(
  local a = { id="a", name="A", create=function(p) return { calculate=function(t,v) return v end } end }
  local b = { id="b", name="B", create=function(p) return { calculate=function(t,v) return -v end } end }
  return { a, b }
)LUAU";
}  // namespace

TEST(FilterCatalogue, RegistersEveryClassInAMultiClassSource) {
  FilterCatalogue cat(makeLuauEngine());
  auto n = cat.addBundledSource(kTwo, "bundled");
  ASSERT_TRUE(n.has_value()) << (n.has_value() ? "" : n.error());
  EXPECT_EQ(*n, 2u);
  EXPECT_EQ(cat.entries().size(), 2u);
  ASSERT_NE(cat.find("a"), nullptr);
  ASSERT_NE(cat.find("b"), nullptr);
  EXPECT_EQ(cat.find("missing"), nullptr);
}

TEST(FilterCatalogue, MakeProcessorBuildsALuaSisoTransform) {
  FilterCatalogue cat(makeLuauEngine());
  ASSERT_TRUE(cat.addBundledSource(kTwo, "bundled").has_value());
  auto p = cat.makeProcessor("b", "{}");
  ASSERT_TRUE(p.has_value()) << (p.has_value() ? "" : p.error());
  ASSERT_NE(p->get(), nullptr);
  EXPECT_STREQ((*p)->id(), "b");
  auto out = (*p)->calculateNextPoint(PJ::proc::Sample::scalar(0, PJ::VarValue{4.0}));
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), -4.0);  // class "b" negates
}

TEST(FilterCatalogue, StandaloneClassScriptSelectsOneClassWithItsDefaults) {
  FilterCatalogue cat(makeLuauEngine());
  ASSERT_TRUE(cat.addBundledSource(
                     R"LUAU(
    local scale = { id="scale", name="Scale", parameters={ { name="k", default=3 } },
      create=function(p) return { calculate=function(t,v) return v * p.k end } end }
    local other = { id="other", name="Other", create=function(p) return { calculate=function(t,v) return 0 end } end }
    return { other, scale }
  )LUAU",
                     "bundled")
                  .has_value());
  EXPECT_FALSE(cat.standaloneClassScript("missing").has_value());
  const auto script = cat.standaloneClassScript("scale");
  ASSERT_TRUE(script.has_value());
  // Empty params: the class's own default (k = 3) applies.
  auto p = cat.makeProcessorFromSource(*script, "scale", "{}");
  ASSERT_TRUE(p.has_value()) << (p.has_value() ? "" : p.error());
  auto out = (*p)->calculateNextPoint(PJ::proc::Sample::scalar(0, PJ::VarValue{2.0}));
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 6.0);
}

TEST(FilterCatalogue, UnknownIdIsAnError) {
  FilterCatalogue cat(makeLuauEngine());
  EXPECT_FALSE(cat.makeProcessor("nope", "{}").has_value());
}

TEST(FilterCatalogue, DuplicateIdFirstWins) {
  FilterCatalogue cat(makeLuauEngine());
  ASSERT_TRUE(cat.addBundledSource(kTwo, "bundled").has_value());
  // Re-adding a source whose ids already exist registers nothing new.
  auto n = cat.addBundledSource(
      R"LUAU(
    return { { id="a", name="A2", create=function(p) return { calculate=function(t,v) return v end } end } }
  )LUAU",
      "user");
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 0u);
  EXPECT_EQ(cat.find("a")->cls.name, "A");  // the bundled one kept
}

TEST(FilterCatalogue, MalformedSourceIsAnError) {
  FilterCatalogue cat(makeLuauEngine());
  EXPECT_FALSE(cat.addBundledSource("return 42", "bundled").has_value());
}

#ifdef BUILTIN_FILTERS_PATH
TEST(FilterCatalogue, LoadsTheRealBundledResource) {
  std::ifstream f(BUILTIN_FILTERS_PATH);
  std::stringstream ss;
  ss << f.rdbuf();
  FilterCatalogue cat(makeLuauEngine());
  auto n = cat.addBundledSource(ss.str(), "bundled");
  ASSERT_TRUE(n.has_value()) << (n.has_value() ? "" : n.error());
  EXPECT_EQ(*n, 12u);
  EXPECT_NE(cat.find("integral"), nullptr);
  EXPECT_NE(cat.find("binary_filter"), nullptr);
}
#endif
