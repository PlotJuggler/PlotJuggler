// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scripting/lua_siso_transform.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pj_datastore/processor_detail.hpp"  // proc::detail::toDouble
#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;
using PJ::proc::Sample;

namespace {

constexpr const char* kDerivative = R"LUAU(
return {
  id = "derivative", name = "Derivative",
  parameters = {
    { name="use_custom_dt", type="boolean", default=false },
    { name="custom_dt", type="number", default=0.01 },
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

// Pass-through filter declaring output_kind="same" (mirror mode): the output column reuses
// the input's StorageKind. Used to exercise the uint64 mirror path.
constexpr const char* kIdentitySame = R"LUAU(
return {
  id = "identity_same", name = "Identity", output = "same",
  parameters = {},
  create = function(params)
    return { calculate = function(t, v) return v end }
  end,
}
)LUAU";

FilterClass loadDerivative(const std::shared_ptr<ScriptEngine>& engine) {
  auto classes = engine->inspectModule(kDerivative, "test");
  EXPECT_TRUE(classes.has_value());
  return classes->front();
}

double dbl(const Sample& s) {
  return PJ::proc::detail::toDouble(s.value());
}

}  // namespace

TEST(LuaSisoTransform, FirstSampleSuppressedThenDifferentiates) {
  auto engine = makeLuauEngine();
  LuaSisoTransform xf(engine, loadDerivative(engine), "{}");

  EXPECT_FALSE(xf.calculateNextPoint(Sample::scalar(0, PJ::VarValue{0.0})).has_value());
  auto a = xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{2.0}));  // +1s, v=2
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(dbl(*a), 2.0);
  auto b = xf.calculateNextPoint(Sample::scalar(2'000'000'000LL, PJ::VarValue{6.0}));  // +1s, v=6
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(dbl(*b), 4.0);
  // The output timestamp is the absolute input spine.
  EXPECT_EQ(b->raw_ts_ns, 2'000'000'000LL);
}

TEST(LuaSisoTransform, ResetClearsStateViaConstructNewAndSwap) {
  auto engine = makeLuauEngine();
  LuaSisoTransform xf(engine, loadDerivative(engine), "{}");
  (void)xf.calculateNextPoint(Sample::scalar(0, PJ::VarValue{0.0}));
  (void)xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{2.0}));
  xf.reset();
  // After reset the next sample is "first" again → suppressed.
  EXPECT_FALSE(xf.calculateNextPoint(Sample::scalar(5'000'000'000LL, PJ::VarValue{9.0})).has_value());
}

TEST(LuaSisoTransform, IdentityAndParamsRoundTrip) {
  auto engine = makeLuauEngine();
  LuaSisoTransform xf(engine, loadDerivative(engine), R"({"use_custom_dt":true,"custom_dt":2.0})");
  EXPECT_STREQ(xf.id(), "derivative");
  EXPECT_EQ(xf.saveParams(), R"({"use_custom_dt":true,"custom_dt":2.0})");
  // custom_dt=2.0 → each step divides by 2: (4-0)/2 = 2 after the first sample.
  (void)xf.calculateNextPoint(Sample::scalar(0, PJ::VarValue{0.0}));
  auto a = xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{4.0}));
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(dbl(*a), 2.0);
}

TEST(LuaSisoTransform, OutputKindIsFloat64) {
  auto engine = makeLuauEngine();
  LuaSisoTransform xf(engine, loadDerivative(engine), "{}");
  const PJ::StorageKind in = PJ::StorageKind::kFloat64;
  auto kinds = xf.outputKinds(PJ::Span<const PJ::StorageKind>(&in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);
}

TEST(LuaSisoTransform, NonFiniteScriptTimeIsSuppressed) {
  // A script returning a NaN/Inf out_time must NOT produce a corrupt timestamp
  // (llround of NaN is UB) — the host suppresses the sample.
  constexpr const char* kBadTime = R"LUAU(
    return { id="bt", name="BT",
      create = function(p) return { calculate = function(t, v) return (0.0/0.0), v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kBadTime, "test");
  ASSERT_TRUE(classes.has_value());
  LuaSisoTransform xf(engine, classes->front(), "{}");
  EXPECT_FALSE(xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{5.0})).has_value());
}

TEST(LuaSisoTransform, BadScriptStartsFailed) {
  auto engine = makeLuauEngine();
  FilterClass bad;
  bad.id = "bad";
  bad.source = "return 123";  // not a class table
  LuaSisoTransform xf(engine, bad, "{}");
  EXPECT_TRUE(xf.failed());
  EXPECT_FALSE(xf.calculateNextPoint(Sample::scalar(0, PJ::VarValue{1.0})).has_value());
}

// output_kind="same" on a uint64 column must not clamp the upper half of the domain to INT64_MAX: a pass-through
// filter keeps a value above 2^63 above 2^63.
TEST(LuaSisoTransform, MirrorUint64DoesNotClampUpperHalf) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kIdentitySame, "test");
  ASSERT_TRUE(classes.has_value()) << classes.error();
  LuaSisoTransform xf(engine, classes->front(), "{}");

  const std::uint64_t big = std::numeric_limits<std::uint64_t>::max();
  auto out = xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{big}));
  ASSERT_TRUE(out.has_value());
  ASSERT_TRUE(std::holds_alternative<std::uint64_t>(out->value()));
  EXPECT_GT(
      std::get<std::uint64_t>(out->value()), static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

// `output = "string"` makes a text column: the label is the sample value, nil still skips the sample.
TEST(LuaSisoTransform, StringOutputEmitsTextSamples) {
  constexpr const char* kState = R"LUAU(
    return { id="state", name="State", output="string",
      create = function(p) return { calculate = function(t, v)
        if v == 0 then return nil end
        return v > 0 and "walking" or "falling"
      end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kState, "test");
  ASSERT_TRUE(classes.has_value()) << classes.error();
  LuaSisoTransform xf(engine, classes->front(), "{}");
  const PJ::StorageKind in = PJ::StorageKind::kFloat64;
  EXPECT_EQ(
      xf.outputKinds(PJ::Span<const PJ::StorageKind>(&in, 1)),
      (std::vector<PJ::StorageKind>{PJ::StorageKind::kString}));

  auto walking = xf.calculateNextPoint(Sample::scalar(0, PJ::VarValue{1.0}));
  ASSERT_TRUE(walking.has_value());
  ASSERT_TRUE(std::holds_alternative<std::string>(walking->value()));
  EXPECT_EQ(std::get<std::string>(walking->value()), "walking");
  auto falling = xf.calculateNextPoint(Sample::scalar(1'000'000'000LL, PJ::VarValue{-1.0}));
  ASSERT_TRUE(falling.has_value());
  EXPECT_EQ(std::get<std::string>(falling->value()), "falling");
  EXPECT_FALSE(xf.calculateNextPoint(Sample::scalar(2'000'000'000LL, PJ::VarValue{0.0})).has_value());
  EXPECT_FALSE(xf.failed());
}
