// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The watchdog turns a runaway script (CPU or memory) into a recoverable error; the host survives.

#include <gtest/gtest.h>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {
// Tight budgets so the tests abort fast.
BudgetLimits smallBudget() {
  BudgetLimits b;
  b.call_fuel = 100'000;
  b.setup_fuel = 500'000;
  b.mem_bytes = 8ull << 20;  // 8 MiB
  return b;
}
}  // namespace

TEST(LuauWatchdog, InfiniteLoopInCalculateAbortsAndFailsInstance) {
  constexpr const char* kLoop = R"LUAU(
    return { id="loop", name="L",
      create = function(p) return { calculate = function(t, v) while true do end return v end } end }
  )LUAU";
  auto engine = makeLuauEngine(smallBudget());
  auto classes = engine->inspectModule(kLoop, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);  // must return, not hang
  EXPECT_TRUE(r.suppress);
  EXPECT_TRUE((*inst)->failed());
}

TEST(LuauWatchdog, InfiniteLoopAtModuleTopLevelAbortsInspect) {
  constexpr const char* kTopLoop = R"LUAU(
    while true do end
    return { id="x", name="X", create = function(p) return { calculate = function(t, v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine(smallBudget());
  EXPECT_FALSE(engine->inspectModule(kTopLoop, "test").has_value());  // aborts, never hangs
}

TEST(LuauWatchdog, MemoryHogAbortsAndFailsInstance) {
  constexpr const char* kHog = R"LUAU(
    return { id="hog", name="H",
      create = function(p) return { calculate = function(t, v)
        local s = string.rep("x", 50000000)  -- ~50 MB >> the 8 MB cap
        return #s
      end } end }
  )LUAU";
  auto engine = makeLuauEngine(smallBudget());
  auto classes = engine->inspectModule(kHog, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);
  EXPECT_TRUE(r.suppress);
  EXPECT_TRUE((*inst)->failed());
}

// A script cannot pcall-swallow the watchdog error and report success: the host
// checks the budget "tripped" flag after the call and fails the node anyway.
TEST(LuauWatchdog, ScriptCannotSwallowBudgetTrip) {
  constexpr const char* kSneaky = R"LUAU(
    return { id="sneaky", name="S",
      create = function(p) return { calculate = function(t, v)
        pcall(function() while true do end end)  -- try to swallow the budget error
        return v
      end } end }
  )LUAU";
  auto engine = makeLuauEngine(smallBudget());
  auto classes = engine->inspectModule(kSneaky, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 5.0);
  EXPECT_TRUE(r.suppress);
  EXPECT_TRUE((*inst)->failed());
}

// A second instance is unaffected by a sibling that blew its budget (isolation).
TEST(LuauWatchdog, SiblingInstanceSurvivesAnotherFailure) {
  constexpr const char* kLoop = R"LUAU(
    return { id="loop", name="L",
      create = function(p) return { calculate = function(t, v) while true do end return v end } end }
  )LUAU";
  constexpr const char* kGood = R"LUAU(
    return { id="good", name="G",
      create = function(p) return { calculate = function(t, v) return v + 1 end } end }
  )LUAU";
  auto engine = makeLuauEngine(smallBudget());
  auto bad = engine->createInstance(engine->inspectModule(kLoop, "t").value().front(), "{}");
  auto good = engine->createInstance(engine->inspectModule(kGood, "t").value().front(), "{}");
  ASSERT_TRUE(bad.has_value());
  ASSERT_TRUE(good.has_value());
  (void)(*bad)->calculate(0.0, 1.0);  // blows its budget
  auto r = (*good)->calculate(0.0, 41.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 42.0);
  EXPECT_FALSE((*good)->failed());
}
