// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {

// A self-contained derivative class (single-class module).
constexpr const char* kDerivative = R"LUAU(
return {
  id = "derivative",
  name = "Derivative",
  description = "dv/dt",
  version = "1.0.0",
  parameters = {
    { name="use_custom_dt", type="boolean", default=false, label="Use fixed dt",
      tooltip="ignore timestamps" },
    { name="custom_dt", type="number", default=0.01, label="Fixed dt", unit="s",
      min=1e-9, step=0.001, decimals=6,
      visible_when={ param="use_custom_dt", equals=true } },
  },
  create = function(params)
    local use_custom = params.use_custom_dt
    local fixed = params.custom_dt
    local has_prev, pt, pv = false, 0.0, 0.0
    return {
      reset = function() has_prev = false end,
      calculate = function(t, v)
        if not has_prev then has_prev, pt, pv = true, t, v; return nil end
        local dt = use_custom and fixed or (t - pt)
        local d = (dt ~= 0.0) and (v - pv) / dt or 0.0
        pt, pv = t, v
        return d
      end,
    }
  end,
}
)LUAU";

// A multi-class module (a list of class tables) — the bundled-resource shape.
constexpr const char* kTwoClasses = R"LUAU(
local scale = {
  id = "scale", name = "Scale",
  parameters = { { name="factor", type="number", default=2.0 } },
  create = function(p) return { calculate = function(t, v) return v * p.factor end } end,
}
local negate = {
  id = "negate", name = "Negate",
  create = function(p) return { calculate = function(t, v) return -v end } end,
}
return { scale, negate }
)LUAU";

// A TRUE metatable class: per-instance state lives in `self`, the methods are
// shared on the class via __index, and create() returns setmetatable(state, Class).
// Exercises the engine's self-passing path (calculate/reset must be reached through
// __index and called with the instance as self).
constexpr const char* kClassAccumulator = R"LUAU(
local Accumulator = {
  id = "accumulator", name = "Accumulator",
  parameters = { { name = "start", type = "number", default = 0.0 } },
}
Accumulator.__index = Accumulator

function Accumulator.create(p)
  return setmetatable({ sum = p.start }, Accumulator)
end

function Accumulator:reset()
  self.sum = 0.0
end

function Accumulator:calculate(t, v)
  self.sum = self.sum + v
  return self.sum
end

return Accumulator
)LUAU";

}  // namespace

TEST(LuauEngine, InspectsSingleClassMetadataWithoutCreate) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kDerivative, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  const FilterClass& fc = classes->front();
  EXPECT_EQ(fc.id, "derivative");
  EXPECT_EQ(fc.name, "Derivative");
  EXPECT_EQ(fc.version, "1.0.0");
  ASSERT_EQ(fc.parameters.size(), 2u);
  EXPECT_EQ(fc.parameters[0].name, "use_custom_dt");
  EXPECT_EQ(fc.parameters[0].type, ParamType::kBoolean);
  EXPECT_EQ(fc.parameters[0].default_value, false);
  EXPECT_EQ(fc.parameters[1].name, "custom_dt");
  EXPECT_EQ(fc.parameters[1].type, ParamType::kNumber);
  EXPECT_EQ(fc.parameters[1].label, "Fixed dt");
  EXPECT_EQ(fc.parameters[1].unit, "s");
  ASSERT_TRUE(fc.parameters[1].min.has_value());
  ASSERT_TRUE(fc.parameters[1].decimals.has_value());
  EXPECT_EQ(*fc.parameters[1].decimals, 6);
  ASSERT_TRUE(fc.parameters[1].visible_when_param.has_value());
  EXPECT_EQ(*fc.parameters[1].visible_when_param, "use_custom_dt");
  EXPECT_EQ(fc.parameters[1].visible_when_equals, true);
  EXPECT_EQ(fc.source, kDerivative);  // source retained for instancing / persistence
}

TEST(LuauEngine, InspectsListOfClasses) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kTwoClasses, "bundled");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 2u);
  EXPECT_EQ((*classes)[0].id, "scale");
  EXPECT_EQ((*classes)[1].id, "negate");
}

TEST(LuauEngine, InspectDoesNotRunCreate) {
  // create() raises, but inspect must still succeed (it never calls create).
  constexpr const char* kBadCreate = R"LUAU(
    return { id="x", name="X", create = function(p) error("must not run") end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kBadCreate, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  EXPECT_EQ(classes->front().id, "x");
}

TEST(LuauEngine, CreateInstanceAppliesDefaultsAndComputes) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kDerivative, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());

  auto r0 = (*inst)->calculate(0.0, 0.0);  // first sample: suppressed
  EXPECT_TRUE(r0.suppress);
  auto r1 = (*inst)->calculate(1.0, 2.0);  // dt=1, (2-0)/1 = 2
  EXPECT_FALSE(r1.suppress);
  EXPECT_DOUBLE_EQ(r1.value, 2.0);
  EXPECT_FALSE((*inst)->failed());
}

TEST(LuauEngine, ParamsOverrideDefaults) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kTwoClasses, "bundled");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], R"({"factor": 5.0})");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 3.0);
  EXPECT_DOUBLE_EQ(r.value, 15.0);  // 3 * 5
}

TEST(LuauEngine, EnumValuesAreTyped) {
  // An int-keyed enum (e.g. binary_filter's binary_op) must keep numeric values,
  // not stringify to "3" — else the generated-form params would fail op==3.
  constexpr const char* kEnum = R"LUAU(
    return { id="e", name="E",
      parameters = { { name="op", type="enum", default=3,
        values = { { value=0, label="zero" }, { value=3, label="three" } } } },
      create = function(p) return { calculate = function(t, v) return p.op end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kEnum, "test");
  ASSERT_TRUE(classes.has_value());
  const auto& params = classes->front().parameters;
  ASSERT_EQ(params.size(), 1u);
  ASSERT_EQ(params[0].values.size(), 2u);
  EXPECT_TRUE(params[0].values[0].value.is_number());
  EXPECT_EQ(params[0].values[1].value, 3);
  EXPECT_EQ(params[0].values[1].label, "three");
  EXPECT_EQ(params[0].default_value, 3);
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 0.0).value, 3.0);  // int default flows to create()
}

TEST(LuauEngine, RejectsNonTableModule) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule("return 42", "test");
  EXPECT_FALSE(classes.has_value());
}

TEST(LuauEngine, RejectsUnknownParamType) {
  constexpr const char* kBadType = R"LUAU(
    return { id="x", name="X", parameters={ {name="p", type="frobnicate", default=1} },
             create=function(p) return { calculate=function(t,v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kBadType, "test");
  EXPECT_FALSE(classes.has_value());
}

TEST(LuauEngine, CalculateReturnShapes) {
  // value | (time,value) | nil
  constexpr const char* kShapes = R"LUAU(
    return { id="shapes", name="S",
      parameters = { { name="mode", type="string", default="value" } },
      create = function(p)
        return { calculate = function(t, v)
          if p.mode == "suppress" then return nil
          elseif p.mode == "timeval" then return t + 10.0, v * 2
          else return v + 1 end
        end }
      end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kShapes, "test");
  ASSERT_TRUE(classes.has_value());

  auto val = engine->createInstance(classes->front(), R"({"mode":"value"})");
  ASSERT_TRUE(val.has_value());
  auto rv = (*val)->calculate(3.0, 7.0);
  EXPECT_FALSE(rv.suppress);
  EXPECT_DOUBLE_EQ(rv.value, 8.0);
  EXPECT_FALSE(rv.out_time.has_value());

  auto tv = engine->createInstance(classes->front(), R"({"mode":"timeval"})");
  ASSERT_TRUE(tv.has_value());
  auto rt = (*tv)->calculate(3.0, 7.0);
  ASSERT_TRUE(rt.out_time.has_value());
  EXPECT_DOUBLE_EQ(*rt.out_time, 13.0);
  EXPECT_DOUBLE_EQ(rt.value, 14.0);

  auto sp = engine->createInstance(classes->front(), R"({"mode":"suppress"})");
  ASSERT_TRUE(sp.has_value());
  EXPECT_TRUE((*sp)->calculate(3.0, 7.0).suppress);
}

// A class declaring `output = "string"` returns text: a label, (time, label), or nil.
TEST(LuauEngine, StringClassReturnsText) {
  constexpr const char* kLabel = R"LUAU(
    return { id="state", name="State", output="string",
      parameters = { { name="mode", type="string", default="value" } },
      create = function(p)
        return { calculate = function(t, v, ...)
          if p.mode == "suppress" then return nil
          elseif p.mode == "timeval" then return t + 1.0, "late"
          elseif p.mode == "number" then return 3
          elseif p.mode == "mimo" then return "a", "b"
          else return v > 0 and "walking" or "standing" end
        end }
      end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kLabel, "test");
  ASSERT_TRUE(classes.has_value()) << classes.error();
  EXPECT_EQ(classes->front().output_kind, "string");

  auto val = engine->createInstance(classes->front(), R"({"mode":"value"})");
  ASSERT_TRUE(val.has_value());
  auto rv = (*val)->calculate(0.0, 1.0);
  EXPECT_FALSE(rv.suppress);
  EXPECT_EQ(rv.text, "walking");
  EXPECT_EQ((*val)->calculate(0.0, -1.0).text, "standing");

  auto tv = engine->createInstance(classes->front(), R"({"mode":"timeval"})");
  ASSERT_TRUE(tv.has_value());
  auto rt = (*tv)->calculate(2.0, 0.0);
  ASSERT_TRUE(rt.out_time.has_value());
  EXPECT_DOUBLE_EQ(*rt.out_time, 3.0);
  EXPECT_EQ(rt.text, "late");

  auto sp = engine->createInstance(classes->front(), R"({"mode":"suppress"})");
  ASSERT_TRUE(sp.has_value());
  EXPECT_TRUE((*sp)->calculate(0.0, 1.0).suppress);

  auto mimo = engine->createInstance(classes->front(), R"({"mode":"mimo"})");
  ASSERT_TRUE(mimo.has_value());
  const double inputs[] = {1.0, 2.0};
  auto rm = (*mimo)->calculateMimo(0.0, PJ::Span<const double>(inputs, 2));
  EXPECT_FALSE(rm.suppress);
  EXPECT_EQ(rm.texts, (std::vector<std::string>{"a", "b"}));

  // A number where text is declared is a mistake, not a label.
  auto num = engine->createInstance(classes->front(), R"({"mode":"number"})");
  ASSERT_TRUE(num.has_value());
  EXPECT_TRUE((*num)->calculate(0.0, 1.0).suppress);
  EXPECT_TRUE((*num)->failed());
  EXPECT_NE((*num)->error().find("string"), std::string::npos) << (*num)->error();
}

// Text from a numeric class fails, and the error names the declaration that would make it valid.
TEST(LuauEngine, NumericClassReturningTextNamesTheStringOutput) {
  constexpr const char* kText = R"LUAU(
    return { id="t", name="T",
      create = function(p) return { calculate = function(t, ...) return "walking" end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kText, "test");
  ASSERT_TRUE(classes.has_value());
  auto siso = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(siso.has_value());
  EXPECT_TRUE((*siso)->calculate(0.0, 1.0).suppress);
  EXPECT_NE((*siso)->error().find(R"(output = "string")"), std::string::npos) << (*siso)->error();

  auto mimo = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(mimo.has_value());
  const double inputs[] = {1.0, 2.0};
  EXPECT_TRUE((*mimo)->calculateMimo(0.0, PJ::Span<const double>(inputs, 2)).suppress);
  EXPECT_NE((*mimo)->error().find(R"(output = "string")"), std::string::npos) << (*mimo)->error();
}

TEST(LuauEngine, RuntimeErrorFailsInstanceStickily) {
  constexpr const char* kBoom = R"LUAU(
    return { id="boom", name="B",
      create = function(p) return { calculate = function(t, v) error("kaboom") end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kBoom, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);
  EXPECT_TRUE(r.suppress);
  EXPECT_TRUE((*inst)->failed());
  EXPECT_FALSE((*inst)->error().empty());
}

TEST(LuauEngine, RejectsMalformedTwoReturn) {
  // (number, non-number) is NOT a valid (time, value) — must fail, not be
  // silently treated as a scalar value.
  constexpr const char* kBad = R"LUAU(
    return { id="b", name="B",
      create = function(p) return { calculate = function(t, v) return v, "nope" end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kBad, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(1.0, 2.0);
  EXPECT_TRUE(r.suppress);
  EXPECT_TRUE((*inst)->failed());
}

// A metatable class (methods shared via __index, state in `self`) must be driven
// with the instance passed as self on every calculate()/reset() — the engine has
// to resolve the method through the metatable and bind self, not raw-read a direct
// field. State must accumulate across calls and zero on reset.
TEST(LuauEngine, DrivesMetatableClassWithSelf) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kClassAccumulator, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  EXPECT_EQ(classes->front().id, "accumulator");

  auto inst = engine->createInstance(classes->front(), R"({"start": 10.0})");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());

  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 1.0).value, 11.0);  // 10 + 1
  EXPECT_DOUBLE_EQ((*inst)->calculate(1.0, 2.0).value, 13.0);  // + 2
  EXPECT_DOUBLE_EQ((*inst)->calculate(2.0, 3.0).value, 16.0);  // + 3

  (*inst)->reset();                                           // self.sum -> 0
  EXPECT_DOUBLE_EQ((*inst)->calculate(3.0, 5.0).value, 5.0);  // 0 + 5
}
