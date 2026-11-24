// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The host-side int64 time rebasing of LuaSisoTransform, against the failure it avoids: an absolute-epoch timestamp
// cast straight to double quantizes to ~256 ns and collapses a sub-microsecond dt.

#include <gtest/gtest.h>

#include <cmath>
#include <variant>

#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;
using PJ::proc::Sample;

namespace {
constexpr const char* kDerivative = R"LUAU(
return {
  id = "derivative", name = "Derivative",
  create = function(params)
    local has_prev, pt, pv = false, 0.0, 0.0
    return { calculate = function(t, v)
      if not has_prev then has_prev, pt, pv = true, t, v; return nil end
      local dt = t - pt
      local d = (dt ~= 0.0) and (v - pv) / dt or 0.0
      pt, pv = t, v
      return d
    end }
  end,
}
)LUAU";

// A representative absolute-epoch instant (~2026), chosen as an exact multiple of
// 256 so the quantization is unambiguous.
constexpr long long kEpochNs = 1'780'000'000'000'000'000LL;
}  // namespace

TEST(LuaTimePrecision, RebasedDerivativeIsExactAtNanosecondScale) {
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kDerivative, "test");
  ASSERT_TRUE(classes.has_value());
  LuaSisoTransform xf(engine, classes->front(), "{}");

  // Two samples 100 ns apart, sitting at a ~2026 absolute epoch.
  (void)xf.calculateNextPoint(Sample::scalar(kEpochNs, PJ::VarValue{0.0}));
  auto out = xf.calculateNextPoint(Sample::scalar(kEpochNs + 100, PJ::VarValue{1.0}));
  ASSERT_TRUE(out.has_value());
  // dt = 100 ns = 1e-7 s, so d = 1 / 1e-7 = 1e7, exactly (rebasing keeps it).
  EXPECT_NEAR(std::get<double>(out->value()), 1e7, 1.0);
}

TEST(LuaTimePrecision, NaiveAbsoluteEpochInDoubleCollapses100nsDt) {
  // The bug the rebasing avoids: subtracting two absolute-epoch doubles.
  const double t0 = static_cast<double>(kEpochNs);
  const double t1 = static_cast<double>(kEpochNs + 100);
  const double dt_naive_s = (t1 - t0) * 1e-9;
  // 100 ns < the ~256 ns ULP at this magnitude, so the delta vanishes entirely.
  EXPECT_DOUBLE_EQ(dt_naive_s, 0.0);
}
