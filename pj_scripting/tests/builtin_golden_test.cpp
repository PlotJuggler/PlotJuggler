// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Golden behaviour of the bundled Luau filters (builtin_filters.luau): each test drives one class over fixed inputs
// and asserts the expected values, hand-traced where the comment says so.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_datastore/column_buffer.hpp"   // PJ::StorageKind
#include "pj_datastore/data_processor.hpp"  // PJ::proc::DataProcessor, Sample
#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/script_engine.h"

#ifndef BUILTIN_FILTERS_PATH
#error "BUILTIN_FILTERS_PATH must be defined by CMake (path to builtin_filters.luau)"
#endif

using PJ::proc::Sample;

namespace {

constexpr std::int64_t kOneSecondNs = 1'000'000'000;
constexpr std::int64_t kMs = 1'000'000;

std::string readFile(const char* path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

PJ::scripting::ScriptEngine& engine() {
  static auto e = PJ::scripting::makeLuauEngine();
  return *e;
}
const std::vector<PJ::scripting::FilterClass>& bundledClasses() {
  static std::vector<PJ::scripting::FilterClass> classes = [] {
    auto r = engine().inspectModule(readFile(BUILTIN_FILTERS_PATH), "bundled");
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error());
    return r.has_value() ? r.value() : std::vector<PJ::scripting::FilterClass>{};
  }();
  return classes;
}
const PJ::scripting::FilterClass* findClass(const std::string& id) {
  for (const auto& c : bundledClasses()) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

// The bundled class `id` configured by `params_json`.
std::unique_ptr<PJ::scripting::LuaSisoTransform> makeLuau(const std::string& id, const std::string& params_json) {
  const PJ::scripting::FilterClass* k = findClass(id);
  EXPECT_NE(k, nullptr) << "no bundled class with id " << id;
  if (k == nullptr) {
    return nullptr;
  }
  return std::make_unique<PJ::scripting::LuaSisoTransform>(
      std::shared_ptr<PJ::scripting::ScriptEngine>(&engine(), [](auto*) {}), *k, params_json);
}

Sample S(std::int64_t ts_ns, double v) {
  return Sample::scalar(ts_ns, PJ::VarValue{v});
}
Sample Si(std::int64_t ts_ns, std::int64_t v) {
  return Sample::scalar(ts_ns, PJ::VarValue{v});
}
std::optional<Sample> step(PJ::proc::DataProcessor& p, std::int64_t ts_ns, double v) {
  return p.calculateNextPoint(S(ts_ns, v));
}
std::vector<std::optional<Sample>> drive(PJ::proc::DataProcessor& p, const std::vector<Sample>& in) {
  std::vector<std::optional<Sample>> out;
  out.reserve(in.size());
  for (const Sample& s : in) {
    out.push_back(p.calculateNextPoint(s));
  }
  return out;
}
double dval(const std::optional<Sample>& o) {
  return std::get<double>(o->value());
}

}  // namespace

// ----------------------------- none / absolute ------------------------------
// The mirror builtins (output="same") keep the input's column kind and, within 2^53, its value.
TEST(GoldenNone, PassesValueAndTimestampThrough) {
  auto t = makeLuau("none", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 10, 3.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 10);
  EXPECT_DOUBLE_EQ(dval(out), 3.0);
}

TEST(GoldenNone, OutputKindMirrorsInput) {
  auto t = makeLuau("none", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in_kind = PJ::StorageKind::kInt64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_kind, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kInt64);
}

TEST(GoldenNone, PreservesInt64ValuePassthrough) {
  auto t = makeLuau("none", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const std::int64_t vals[] = {5, -7, 1'000'000'000'000LL, -42, 0};
  std::int64_t ts = 0;
  for (std::int64_t v : vals) {
    const auto out = t->calculateNextPoint(Si(ts, v));
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(out->value()));
    EXPECT_EQ(std::get<std::int64_t>(out->value()), v);  // passthrough
    ts += kOneSecondNs;
  }
}

TEST(GoldenAbsolute, NegatesDoubleSign) {
  auto t = makeLuau("absolute", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 5, -2.5);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 2.5);
}

TEST(GoldenAbsolute, PreservesInt64KindAndAbsValue) {
  auto t = makeLuau("absolute", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in_kind = PJ::StorageKind::kInt64;
  EXPECT_EQ(t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_kind, 1))[0], PJ::StorageKind::kInt64);
  const std::int64_t vals[] = {5, -7, 1'000'000'000'000LL, -42, 0};
  const std::int64_t expected[] = {5, 7, 1'000'000'000'000LL, 42, 0};  // |v|, exact within 2^53
  std::int64_t ts = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    const auto out = t->calculateNextPoint(Si(ts, vals[i]));
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(out->value()));
    EXPECT_EQ(std::get<std::int64_t>(out->value()), expected[i]);
    ts += kOneSecondNs;
  }
}

// -------------------------------- derivative --------------------------------
TEST(GoldenDerivative, ConstantSlopeOverOneSecondSteps) {
  auto t = makeLuau("derivative", "{}");
  ASSERT_FALSE(t->failed()) << t->error();

  // y = 3 * t (seconds): values 0, 3, 6, 9 at t = 0,1,2,3 s.
  const auto first = step(*t, 0, 0.0);
  EXPECT_FALSE(first.has_value());  // no previous point yet

  const auto second = step(*t, kOneSecondNs, 3.0);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->raw_ts_ns, kOneSecondNs);  // emitted at CURRENT ts, not prev
  EXPECT_DOUBLE_EQ(dval(second), 3.0);

  const auto third = step(*t, 2 * kOneSecondNs, 6.0);
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->raw_ts_ns, 2 * kOneSecondNs);
  EXPECT_DOUBLE_EQ(dval(third), 3.0);

  const auto fourth = step(*t, 3 * kOneSecondNs, 9.0);
  ASSERT_TRUE(fourth.has_value());
  EXPECT_DOUBLE_EQ(dval(fourth), 3.0);
}

TEST(GoldenDerivative, SubSecondStepUsesNanosecondSpine) {
  auto t = makeLuau("derivative", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 10.0);  // suppressed
  const auto out = step(*t, kOneSecondNs / 2, 11.0);
  ASSERT_TRUE(out.has_value());
  // (11 - 10) / 0.5 s = 2.0
  EXPECT_DOUBLE_EQ(dval(out), 2.0);
}

TEST(GoldenDerivative, ZeroDtYieldsZeroNotInfOrNan) {
  auto t = makeLuau("derivative", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, kOneSecondNs, 5.0);                   // suppressed
  const auto out = step(*t, kOneSecondNs, 9.0);  // same ts
  ASSERT_TRUE(out.has_value());
  const double result = dval(out);
  EXPECT_DOUBLE_EQ(result, 0.0);
  EXPECT_FALSE(std::isinf(result));
  EXPECT_FALSE(std::isnan(result));
}

TEST(GoldenDerivative, CustomDtOverridesMeasuredGap) {
  auto t = makeLuau("derivative", R"({"use_custom_dt":true,"custom_dt":2.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 0.0);  // suppressed
  const auto out = step(*t, kOneSecondNs, 8.0);
  ASSERT_TRUE(out.has_value());
  // (8 - 0) / 2.0 = 4.0  (measured gap of 1 s is ignored)
  EXPECT_DOUBLE_EQ(dval(out), 4.0);
}

TEST(GoldenDerivative, CustomDtZeroYieldsZero) {
  auto t = makeLuau("derivative", R"({"use_custom_dt":true,"custom_dt":0.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 0.0);  // suppressed
  const auto out = step(*t, kOneSecondNs, 8.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 0.0);
}

TEST(GoldenDerivative, ResetReSuppressesFirstSample) {
  auto t = makeLuau("derivative", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 0.0);  // suppressed
  ASSERT_TRUE(step(*t, kOneSecondNs, 3.0).has_value());

  t->reset();

  const auto afterReset = step(*t, 2 * kOneSecondNs, 100.0);
  EXPECT_FALSE(afterReset.has_value());  // first sample post-reset is suppressed

  const auto next = step(*t, 3 * kOneSecondNs, 103.0);
  ASSERT_TRUE(next.has_value());
  EXPECT_DOUBLE_EQ(dval(next), 3.0);
}

TEST(GoldenDerivative, OutputKindIsAlwaysFloat) {
  auto t = makeLuau("derivative", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in_kind = PJ::StorageKind::kInt64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_kind, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);  // always float, even from int input
}

// -------------------------------- integral --------------------------------
TEST(GoldenIntegral, OutputKindIsAlwaysFloat64) {
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in_kind = PJ::StorageKind::kInt64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_kind, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);
}

TEST(GoldenIntegral, FirstSampleIsSuppressed) {
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 0, 1.0);
  EXPECT_FALSE(out.has_value());
}

TEST(GoldenIntegral, ConstantUnitInputAccumulatesArea) {
  // y == 1.0 sampled every 1 second -> trapezoid area per step is 1.0, so the
  // running integral after step k (k >= 1) is exactly k.
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  ASSERT_FALSE(step(*t, 0, 1.0).has_value());

  for (std::int64_t k = 1; k <= 5; ++k) {
    const auto out = step(*t, k * kOneSecondNs, 1.0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->raw_ts_ns, k * kOneSecondNs);
    EXPECT_DOUBLE_EQ(dval(out), static_cast<double>(k));
  }
}

TEST(GoldenIntegral, TrapezoidRuleOnRampValue) {
  // y ramps 0,2,4 over 1s steps. Trapezoid areas: (0+2)/2=1, then (2+4)/2=3 ->
  // cumulative 1, then 4.
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  ASSERT_FALSE(step(*t, 0, 0.0).has_value());

  const auto first = step(*t, kOneSecondNs, 2.0);
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(dval(first), 1.0);

  const auto second = step(*t, 2 * kOneSecondNs, 4.0);
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(dval(second), 4.0);
}

TEST(GoldenIntegral, CustomDtOverridesTimestampDelta) {
  // With use_custom_dt, the timestamp delta is ignored; every step uses
  // custom_dt seconds. Here custom_dt = 2.0, y == 1.0 -> area 2.0 per step.
  auto t = makeLuau("integral", R"({"use_custom_dt":true,"custom_dt":2.0})");
  ASSERT_FALSE(t->failed()) << t->error();

  // Timestamps deliberately not 1s apart, to prove they are ignored.
  ASSERT_FALSE(step(*t, 0, 1.0).has_value());

  const auto first = step(*t, 5 * kOneSecondNs, 1.0);
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(dval(first), 2.0);

  const auto second = step(*t, 7 * kOneSecondNs, 1.0);
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(dval(second), 4.0);
}

TEST(GoldenIntegral, ResetClearsAccumulatorAndSuppressesNextFirstSample) {
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  ASSERT_FALSE(step(*t, 0, 1.0).has_value());
  const auto before = step(*t, kOneSecondNs, 1.0);
  ASSERT_TRUE(before.has_value());
  EXPECT_DOUBLE_EQ(dval(before), 1.0);

  t->reset();

  // After reset the next sample primes the state again (suppressed)...
  EXPECT_FALSE(step(*t, 2 * kOneSecondNs, 1.0).has_value());
  // ...and the accumulator restarts from 0, not from the pre-reset total.
  const auto after = step(*t, 3 * kOneSecondNs, 1.0);
  ASSERT_TRUE(after.has_value());
  EXPECT_DOUBLE_EQ(dval(after), 1.0);
}

TEST(GoldenIntegral, IntegerInputIsCoercedToDouble) {
  // Input arm may be int64; toDouble widens it. y == 3 (int) every 1s ->
  // running integral 3, 6, ... The integral output is always float64.
  auto t = makeLuau("integral", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  ASSERT_FALSE(t->calculateNextPoint(Si(0, 3)).has_value());

  const auto first = t->calculateNextPoint(Si(kOneSecondNs, 3));
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(dval(first), 3.0);

  const auto second = t->calculateNextPoint(Si(2 * kOneSecondNs, 3));
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(dval(second), 6.0);
}

// -------------------------------- scale --------------------------------
// Identity: default params (scale=1, offset=0, time_offset=0) pass the value and
// timestamp through unchanged, but the output is always a double.
TEST(GoldenScale, DefaultsAreIdentity) {
  auto t = makeLuau("scale", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, kOneSecondNs, 7.5);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 1'000'000'000);
  EXPECT_TRUE(std::holds_alternative<double>(out->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 7.5);
}

// Affine value math: value_scale * y + value_offset.
TEST(GoldenScale, AppliesScaleAndOffset) {
  auto t = makeLuau("scale", R"({"value_scale":2.0,"value_offset":-1.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 0, 10.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 19.0);  // 2*10 - 1
}

// Integer input widens to double for the affine math, then yields a double.
TEST(GoldenScale, IntegerInputWidensToDouble) {
  auto t = makeLuau("scale", R"({"value_scale":0.5})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = t->calculateNextPoint(Si(0, 8));
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(std::holds_alternative<double>(out->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 4.0);
}

// time_offset_sec shifts the int64-ns spine (raw_ts_ns + llround(sec*1e9)).
// A 0.25 s shift adds exactly 250'000'000 ns.
TEST(GoldenScale, TimeOffsetShiftsNanosecondSpine) {
  auto t = makeLuau("scale", R"({"time_offset_sec":0.25})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, kOneSecondNs, 3.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 1'250'000'000);
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 3.0);
}

// A negative time_offset_sec shifts the spine backwards; rounds to nearest ns.
TEST(GoldenScale, NegativeTimeOffsetShiftsBackwards) {
  auto t = makeLuau("scale", R"({"time_offset_sec":-0.001})");  // -1 ms
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 5'000'000, 1.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 4'000'000);
}

// Stateless: never suppresses, and identical inputs give identical outputs
// regardless of call history (reset is a no-op).
TEST(GoldenScale, StatelessNeverSuppresses) {
  auto t = makeLuau("scale", R"({"value_scale":3.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto first = step(*t, 0, 2.0);
  t->reset();
  auto second = step(*t, 0, 2.0);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(first->value()), std::get<double>(second->value()));
}

// Output kind is always kFloat64, independent of the declared input kind.
TEST(GoldenScale, OutputKindAlwaysFloat64) {
  auto t = makeLuau("scale", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind int_in[] = {PJ::StorageKind::kInt64};
  auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(int_in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);

  auto empty = t->outputKinds(PJ::Span<const PJ::StorageKind>{});
  ASSERT_EQ(empty.size(), 1u);
  EXPECT_EQ(empty[0], PJ::StorageKind::kFloat64);
}

// -------------------------------- moving_average --------------------------------
TEST(GoldenMovingAverage, OutputKindIsFloat64) {
  auto t = makeLuau("moving_average", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in = PJ::StorageKind::kInt64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);  // base default
}

// An underfull window is padded with the current value and divided by the full window; traced by hand with window=3
// over y = 10, 20, 30, 0 at ts 0, 100, 200, 300.
TEST(GoldenMovingAverage, PadsUnderfullWindowWithCurrentValue) {
  auto t = makeLuau("moving_average", R"({"window":3})");
  ASSERT_FALSE(t->failed()) << t->error();

  // Sample 1: buffer={10}, padded with two copies of 10 -> (10*2 + 10)/3 = 10.
  auto out = step(*t, 0, 10.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 10.0);
  EXPECT_EQ(out->raw_ts_ns, 0);

  // Sample 2: buffer={10,20}, padded with one copy of 20 -> (20 + 10 + 20)/3.
  out = step(*t, 100, 20.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 50.0 / 3.0);
  EXPECT_EQ(out->raw_ts_ns, 100);

  // Sample 3: buffer={10,20,30}, full -> (10 + 20 + 30)/3 = 20.
  out = step(*t, 200, 30.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 20.0);

  // Sample 4: oldest (10) evicted, buffer={20,30,0} -> (20 + 30 + 0)/3.
  out = step(*t, 300, 0.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 50.0 / 3.0);
  EXPECT_EQ(out->raw_ts_ns, 300);
}

// window <= 0 is guarded to 1, so the average is the passthrough value.
TEST(GoldenMovingAverage, WindowGuardedToAtLeastOne) {
  auto t = makeLuau("moving_average", R"({"window":0})");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 5, 7.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 7.0);
  EXPECT_EQ(out->raw_ts_ns, 5);
}

// Integer input is read as double; output is float64.
TEST(GoldenMovingAverage, ReadsIntegerInputAsDouble) {
  auto t = makeLuau("moving_average", R"({"window":2})");
  ASSERT_FALSE(t->failed()) << t->error();
  // Sample 1: buffer={4}, padded -> (4 + 4)/2 = 4.
  auto out = t->calculateNextPoint(Si(0, 4));
  ASSERT_TRUE(out.has_value());
  ASSERT_TRUE(std::holds_alternative<double>(out->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 4.0);
  // Sample 2: buffer={4,6}, full -> (4 + 6)/2 = 5.
  out = t->calculateNextPoint(Si(10, 6));
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 5.0);
}

// Without compensation the output timestamp is the current input timestamp.
TEST(GoldenMovingAverage, TimestampIsInputWhenNotCompensating) {
  auto t = makeLuau("moving_average", R"({"window":10,"compensate_time_offset":false})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 1.0);
  const auto out = step(*t, 100, 2.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 100);
}

// With compensation the output timestamp is centered on the buffered span:
// the midpoint of the oldest and newest buffered timestamps.
TEST(GoldenMovingAverage, CompensateCentersTimestampOnBufferedSpan) {
  auto t = makeLuau("moving_average", R"({"window":10,"compensate_time_offset":true})");
  ASSERT_FALSE(t->failed()) << t->error();

  // Sample 1: only one buffered sample -> no shift, timestamp is the input.
  auto out = step(*t, 0, 1.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 0);

  // Sample 2: buffer ts {0,100} -> midpoint 50.
  out = step(*t, 100, 2.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 50);

  // Sample 3: buffer ts {0,100,200} -> midpoint 100.
  out = step(*t, 200, 3.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 100);
}

// reset() clears the ring so the pad-with-current ramp restarts from scratch.
TEST(GoldenMovingAverage, ResetClearsBuffer) {
  auto t = makeLuau("moving_average", R"({"window":3})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 10.0);
  step(*t, 100, 20.0);
  t->reset();
  // First sample after reset behaves like the very first sample: padded to itself.
  const auto out = step(*t, 200, 30.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 30.0);
}

// -------------------------------- moving_rms --------------------------------
// Output kind: int64 input -> float64 output (Luau twin always emits a number).
TEST(GoldenMovingRMS, OutputKindIsFloat64) {
  auto t = makeLuau("moving_rms", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind kind = PJ::StorageKind::kInt64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&kind, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);
}

// An underfull window is padded with the current value: window==10, a single sample y==3 returns |y| == 3.
TEST(GoldenMovingRMS, FirstSamplePadsToCurrentValue) {
  auto t = makeLuau("moving_rms", "{}");  // default window == 10
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 100, 3.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 100);
  EXPECT_DOUBLE_EQ(dval(out), 3.0);
}

// A constant stream always reports the constant, regardless of fill state.
TEST(GoldenMovingRMS, ConstantStreamIsTheConstant) {
  auto t = makeLuau("moving_rms", R"({"window":4})");
  ASSERT_FALSE(t->failed()) << t->error();
  for (std::int64_t i = 0; i < 6; ++i) {
    auto out = step(*t, i, 7.0);
    ASSERT_TRUE(out.has_value());
    EXPECT_DOUBLE_EQ(dval(out), 7.0);
  }
}

// Hand-computed window=2 over [3, 4].
//   step1 [3]:   sqrt((3^2*(2-1) + 3^2)/2) = sqrt(18/2) = 3.0
//   step2 [3,4]: sqrt((4^2*(2-2) + 3^2 + 4^2)/2) = sqrt(25/2) = 3.53553390593...
TEST(GoldenMovingRMS, HandComputedWindowTwo) {
  auto t = makeLuau("moving_rms", R"({"window":2})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto s1 = step(*t, 0, 3.0);
  ASSERT_TRUE(s1.has_value());
  EXPECT_DOUBLE_EQ(dval(s1), 3.0);

  auto s2 = step(*t, 1, 4.0);
  ASSERT_TRUE(s2.has_value());
  EXPECT_NEAR(dval(s2), 3.5355339059327378, 1e-12);
}

// Hand-computed window=3 over [3, 4, 0].
//   s1 [3]:     sqrt((9*2 + 9)/3)        = sqrt(27/3) = 3.0
//   s2 [3,4]:   sqrt((16*1 + 9 + 16)/3)  = sqrt(41/3) = 3.69684550213...
//   s3 [3,4,0]: sqrt((9 + 16 + 0)/3)     = sqrt(25/3) = 2.88675134594...
TEST(GoldenMovingRMS, HandComputedWindowThree) {
  auto t = makeLuau("moving_rms", R"({"window":3})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_DOUBLE_EQ(dval(step(*t, 0, 3.0)), 3.0);
  EXPECT_NEAR(dval(step(*t, 1, 4.0)), 3.696845502136472, 1e-12);
  EXPECT_NEAR(dval(step(*t, 2, 0.0)), 2.886751345948129, 1e-12);
}

// Once the window is full, the oldest sample is evicted (no padding).
// window=2 over [3,4,5]: after 5 the buffer is [4,5] -> sqrt((16+25)/2).
TEST(GoldenMovingRMS, EvictsOldestWhenFull) {
  auto t = makeLuau("moving_rms", R"({"window":2})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 3.0);
  step(*t, 1, 4.0);
  auto out = step(*t, 2, 5.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_NEAR(dval(out), 4.527692569068709, 1e-12);
}

// Integer input widens to double: window of 1 -> RMS == |y|.
TEST(GoldenMovingRMS, ReadsIntegerInputAsDouble) {
  auto t = makeLuau("moving_rms", R"({"window":1})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = t->calculateNextPoint(Si(0, 5));
  ASSERT_TRUE(out.has_value());
  ASSERT_TRUE(std::holds_alternative<double>(out->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(out->value()), 5.0);
}

// window <= 0 clamps to 1 (RMS of one sample == |y|).
TEST(GoldenMovingRMS, NonPositiveWindowClampsToOne) {
  auto t = makeLuau("moving_rms", R"({"window":0})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 0, 5.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 5.0);
}

// reset() clears the window: post-reset behaves like a fresh start.
TEST(GoldenMovingRMS, ResetClearsWindow) {
  auto t = makeLuau("moving_rms", R"({"window":3})");
  ASSERT_FALSE(t->failed()) << t->error();
  step(*t, 0, 10.0);
  step(*t, 1, 20.0);
  t->reset();
  // After reset, a single 4.0 must read as 4.0 (pad-with-current), not blended
  // with the pre-reset 10/20.
  auto out = step(*t, 2, 4.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 4.0);
}

// Output timestamp is the input timestamp (no time-domain math).
TEST(GoldenMovingRMS, OutputTimestampIsInputTimestamp) {
  auto t = makeLuau("moving_rms", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 123456789, 2.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 123456789);
}

// -------------------------------- moving_variance --------------------------------
TEST(GoldenMovingVariance, OutputKindIsFloat64) {
  auto t = makeLuau("moving_variance", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in = PJ::StorageKind::kFloat64;
  const std::vector<PJ::StorageKind> kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);
}

// Full window 1..10: avg = 5.5, variance = 82.5 / 10 = 8.25.
TEST(GoldenMovingVariance, VarianceOfFullKnownWindow) {
  auto t = makeLuau("moving_variance", R"({"window":10})");
  ASSERT_FALSE(t->failed()) << t->error();
  std::int64_t ts = 0;
  std::optional<Sample> out;
  for (const double y : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}) {
    out = step(*t, ts++, y);
  }
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 8.25);
}

// Same window, std_dev branch -> sqrt(8.25).
TEST(GoldenMovingVariance, StdDevIsSqrtOfVariance) {
  auto t = makeLuau("moving_variance", R"({"window":10,"std_dev":true})");
  ASSERT_FALSE(t->failed()) << t->error();
  std::int64_t ts = 0;
  std::optional<Sample> out;
  for (const double y : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}) {
    out = step(*t, ts++, y);
  }
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), std::sqrt(8.25));
}

// A single sample in a window>1 pads the missing slots with the current value, so variance is 0.
TEST(GoldenMovingVariance, SingleSamplePaddedIsZeroVariance) {
  auto t = makeLuau("moving_variance", R"({"window":10})");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 0, 7.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 0.0);
}

// window=2, deterministic by hand:
//   y=2 -> buf{2}, pad=1: avg=2, var=0
//   y=4 -> buf{2,4}, pad=0: avg=3, var=(1+1)/2=1
TEST(GoldenMovingVariance, SmallWindowPadThenFull) {
  auto t = makeLuau("moving_variance", R"({"window":2})");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto a = step(*t, 0, 2.0);
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(dval(a), 0.0);
  const auto b = step(*t, 1, 4.0);
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(dval(b), 1.0);
}

// Front-eviction keeps the newest `window` samples:
//   window=2, feed 2,4,10 -> buf{4,10}, avg=7, var=(9+9)/2=9.
TEST(GoldenMovingVariance, EvictsOldestBeyondWindow) {
  auto t = makeLuau("moving_variance", R"({"window":2})");
  ASSERT_FALSE(t->failed()) << t->error();
  std::int64_t ts = 0;
  std::optional<Sample> out;
  for (const double y : {2.0, 4.0, 10.0}) {
    out = step(*t, ts++, y);
  }
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 9.0);
}

// reset() clears the window so a re-fed single sample is padded fresh (var 0),
// not contaminated by the prior fully-spread window.
TEST(GoldenMovingVariance, ResetClearsBuffer) {
  auto t = makeLuau("moving_variance", R"({"window":4})");
  ASSERT_FALSE(t->failed()) << t->error();
  std::int64_t ts = 0;
  for (const double y : {0.0, 10.0, 0.0, 10.0}) {
    step(*t, ts++, y);  // spread window, nonzero variance
  }
  t->reset();
  const auto out = step(*t, ts++, 5.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 0.0);
}

// Output timestamp passes through the int64-ns spine unchanged.
TEST(GoldenMovingVariance, PreservesTimestamp) {
  auto t = makeLuau("moving_variance", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 123456789, 1.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 123456789);
}

// -------------------------------- outlier_removal --------------------------------
// outputKinds: output="same" -> int64 in mirrors to int64 out.
TEST(GoldenOutlierRemoval, OutputKindMirrorsInput) {
  auto t = makeLuau("outlier_removal", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in[] = {PJ::StorageKind::kInt64};
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kInt64);
}

// A smooth ramp with one spike drops the spike and emits the previous sample on
// each accepted step (one-sample delay). Suppressed sample -> nullopt.
TEST(GoldenOutlierRemoval, ApplyBatchDropsSpikeInSmoothSeries) {
  auto t = makeLuau("outlier_removal", R"({"outlier_factor":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  const std::vector<double> ys = {0.0, 1.0, 2.0, 100.0, 4.0, 5.0, 6.0};
  std::vector<Sample> in;
  for (std::size_t i = 0; i < ys.size(); ++i) {
    in.push_back(S(static_cast<std::int64_t>(i), ys[i]));
  }
  const std::vector<Sample> out = t->applyBatch(in);  // the authoritative batch path (compacts suppressed)
  std::vector<double> got;
  for (const Sample& s : out) {
    got.push_back(std::get<double>(s.value()));
  }
  // [0,1,2] pass through (ring not full); i=3 emits prev (2); i=4 drops spike;
  // i=5 emits 4; i=6 emits 5. The final sample (6) is undelivered (one-sample delay).
  const std::vector<double> expected = {0.0, 1.0, 2.0, 2.0, 4.0, 5.0};
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(got[i], expected[i]) << "at index " << i;
  }
  // The spike value never appears in the output.
  for (double v : got) {
    EXPECT_NE(v, 100.0);
  }
}

// A gentle (non-outlier) series with a high factor drops nothing.
TEST(GoldenOutlierRemoval, ApplyBatchKeepsNonOutliers) {
  auto t = makeLuau("outlier_removal", "{}");  // default factor 100 -> only huge spikes drop
  ASSERT_FALSE(t->failed()) << t->error();
  const std::vector<double> ys = {0.0, 1.0, 2.0, 3.5, 4.0, 5.0};
  std::vector<Sample> in;
  for (std::size_t i = 0; i < ys.size(); ++i) {
    in.push_back(S(static_cast<std::int64_t>(i), ys[i]));
  }
  const std::vector<Sample> out = t->applyBatch(in);  // the authoritative batch path
  std::vector<double> got;
  for (const Sample& s : out) {
    got.push_back(std::get<double>(s.value()));
  }
  const std::vector<double> expected = {0.0, 1.0, 2.0, 2.0, 3.5, 4.0};
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(got[i], expected[i]) << "at index " << i;
  }
}

// calculateNextPoint mirrors the batch path: the spike call returns nullopt,
// each accepted step passes the previous sample's value + timestamp through.
TEST(GoldenOutlierRemoval, CalculateNextPointDropsSpike) {
  auto t = makeLuau("outlier_removal", R"({"outlier_factor":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  const std::vector<double> ys = {0.0, 1.0, 2.0, 100.0, 4.0, 5.0, 6.0};

  std::vector<std::optional<Sample>> results;
  for (std::size_t i = 0; i < ys.size(); ++i) {
    results.push_back(step(*t, static_cast<std::int64_t>(i), ys[i]));
  }

  // First three are passthrough (ring not full yet).
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(results[static_cast<std::size_t>(i)].has_value());
    EXPECT_DOUBLE_EQ(dval(results[static_cast<std::size_t>(i)]), ys[static_cast<std::size_t>(i)]);
  }
  // i=3 emits the previous sample (idx 2, value 2.0, ts 2).
  ASSERT_TRUE(results[3].has_value());
  EXPECT_DOUBLE_EQ(dval(results[3]), 2.0);
  EXPECT_EQ(results[3]->raw_ts_ns, 2);
  // i=4 is the spike-detection step -> suppressed.
  EXPECT_FALSE(results[4].has_value());
  // i=5 emits idx 4 (value 4.0); i=6 emits idx 5 (value 5.0).
  ASSERT_TRUE(results[5].has_value());
  EXPECT_DOUBLE_EQ(dval(results[5]), 4.0);
  ASSERT_TRUE(results[6].has_value());
  EXPECT_DOUBLE_EQ(dval(results[6]), 5.0);
}

// Value passthrough preserves the input StorageKind: integers stay integers.
TEST(GoldenOutlierRemoval, PreservesInt64ValueKind) {
  auto t = makeLuau("outlier_removal", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 7, 0.0) ? std::optional<Sample>{} : std::optional<Sample>{};
  (void)out;
  // First sample is passthrough (ring not full); feed an int64 sample.
  const auto res = t->calculateNextPoint(Si(7, std::int64_t{42}));
  ASSERT_TRUE(res.has_value());
  ASSERT_TRUE(std::holds_alternative<std::int64_t>(res->value()));
  EXPECT_EQ(std::get<std::int64_t>(res->value()), 42);
}

// reset() clears the ring so a fresh series starts from passthrough again.
TEST(GoldenOutlierRemoval, ResetClearsRing) {
  auto t = makeLuau("outlier_removal", R"({"outlier_factor":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  // Prime the ring with a spike scenario.
  const std::vector<double> ys = {0.0, 1.0, 2.0, 100.0};
  for (std::size_t i = 0; i < ys.size(); ++i) {
    step(*t, static_cast<std::int64_t>(i), ys[i]);
  }
  t->reset();
  // After reset the first sample is passthrough again (ring empty).
  const auto out = step(*t, 100, 9.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 9.0);
  EXPECT_EQ(out->raw_ts_ns, 100);
}

// -------------------------------- samples_counter --------------------------------
TEST(GoldenSamplesCounter, OutputKindIsInt64) {
  auto t = makeLuau("samples_counter", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  PJ::StorageKind in_kind = PJ::StorageKind::kFloat64;
  auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_kind, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kInt64);
}

TEST(GoldenSamplesCounter, OutputTimestampMirrorsInput) {
  auto t = makeLuau("samples_counter", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 42 * kMs, 7.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 42 * kMs);
}

TEST(GoldenSamplesCounter, FirstSampleCountsItself) {
  auto t = makeLuau("samples_counter", R"({"samples_ms":3.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  auto out = step(*t, 0, 0.0);  // current sample is always in its own window
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(std::get<std::int64_t>(out->value()), 1);
}

// Window of 3 ms over 1 ms steps: the count rises to 4 then plateaus as old
// samples age out.
TEST(GoldenSamplesCounter, RisesThenPlateaus) {
  auto t = makeLuau("samples_counter", R"({"samples_ms":3.0})");
  ASSERT_FALSE(t->failed()) << t->error();

  auto c0 = step(*t, 0 * kMs, 0.0);  // {0}
  ASSERT_TRUE(c0.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c0->value()), 1);
  auto c1 = step(*t, 1 * kMs, 0.0);  // {0,1}
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c1->value()), 2);
  auto c2 = step(*t, 2 * kMs, 0.0);  // {0,1,2}
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c2->value()), 3);
  auto c3 = step(*t, 3 * kMs, 0.0);  // {0,1,2,3}, lower bound = 0
  ASSERT_TRUE(c3.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c3->value()), 4);
  auto c4 = step(*t, 4 * kMs, 0.0);  // {1,2,3,4}, 0 aged out (0 < 1ms)
  ASSERT_TRUE(c4.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c4->value()), 4);
  auto c5 = step(*t, 5 * kMs, 0.0);  // {2,3,4,5} plateau
  ASSERT_TRUE(c5.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c5->value()), 4);
  auto c6 = step(*t, 6 * kMs, 0.0);  // {3,4,5,6} plateau
  ASSERT_TRUE(c6.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c6->value()), 4);
}

// Lower bound is inclusive: a sample exactly samples_ms old still counts.
TEST(GoldenSamplesCounter, LowerBoundIsInclusive) {
  auto t = makeLuau("samples_counter", R"({"samples_ms":2.0})");  // window = 2 ms
  ASSERT_FALSE(t->failed()) << t->error();

  auto c0 = step(*t, 0 * kMs, 0.0);  // {0}
  ASSERT_TRUE(c0.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c0->value()), 1);
  auto c2 = step(*t, 2 * kMs, 0.0);  // lower bound = 0, the t=0 sample is exactly on it
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c2->value()), 2);
  auto c4 = step(*t, 4 * kMs, 0.0);  // lower bound = 2 ms; t=0 drops, t=2,t=4 remain
  ASSERT_TRUE(c4.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c4->value()), 2);
}

// reset() clears the timestamp buffer so counting restarts from scratch.
TEST(GoldenSamplesCounter, ResetClearsBuffer) {
  auto t = makeLuau("samples_counter", R"({"samples_ms":1000.0})");
  ASSERT_FALSE(t->failed()) << t->error();

  auto c0 = step(*t, 0 * kMs, 0.0);
  ASSERT_TRUE(c0.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c0->value()), 1);
  auto c1 = step(*t, 1 * kMs, 0.0);
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c1->value()), 2);
  auto c2 = step(*t, 2 * kMs, 0.0);
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c2->value()), 3);

  t->reset();

  auto c3 = step(*t, 3 * kMs, 0.0);  // buffer empty again, only the new sample counts
  ASSERT_TRUE(c3.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c3->value()), 1);
}

// Irregular spacing: only the window width matters, not the sample count.
TEST(GoldenSamplesCounter, IrregularSpacing) {
  auto t = makeLuau("samples_counter", R"({"samples_ms":10.0})");  // window = 10 ms
  ASSERT_FALSE(t->failed()) << t->error();

  auto c0 = step(*t, 0 * kMs, 0.0);  // {0}
  ASSERT_TRUE(c0.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c0->value()), 1);
  auto c5 = step(*t, 5 * kMs, 0.0);  // {0,5}
  ASSERT_TRUE(c5.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c5->value()), 2);
  auto c12 = step(*t, 12 * kMs, 0.0);  // lower bound = 2 ms; t=0 aged out, {5,12}
  ASSERT_TRUE(c12.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c12->value()), 2);
  auto c13 = step(*t, 13 * kMs, 0.0);  // lower bound = 3 ms; {5,12,13}
  ASSERT_TRUE(c13.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c13->value()), 3);
  auto c30 = step(*t, 30 * kMs, 0.0);  // lower bound = 20 ms; only {30} survives
  ASSERT_TRUE(c30.has_value());
  EXPECT_EQ(std::get<std::int64_t>(c30->value()), 1);
}

// -------------------------------- binary_filter --------------------------------
TEST(GoldenBinaryFilter, OutputKindIsAlwaysInt64) {
  auto t = makeLuau("binary_filter", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const PJ::StorageKind in[] = {PJ::StorageKind::kFloat64};
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(in, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kInt64);
}

TEST(GoldenBinaryFilter, OutputKindInt64WithNoDeclaredInput) {
  auto t = makeLuau("binary_filter", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>{});
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kInt64);
}

TEST(GoldenBinaryFilter, PreservesTimestampAndEmitsInt64) {
  auto t = makeLuau("binary_filter", R"({"binary_op":3,"binary_a":0.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = step(*t, 4242, 1.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->raw_ts_ns, 4242);
  ASSERT_TRUE(std::holds_alternative<std::int64_t>(out->value()));
  EXPECT_EQ(std::get<std::int64_t>(out->value()), 1);
}

TEST(GoldenBinaryFilter, GreaterIsDefaultOp) {
  // Default class config: kGreater (binary_op=3) against a=0.0.
  auto t = makeLuau("binary_filter", "{}");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 1.0)->value()), 1);   // 1 > 0
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 0.0)->value()), 0);   // 0 > 0 is false
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, -1.0)->value()), 0);  // -1 > 0 is false
}

TEST(GoldenBinaryFilter, Greater) {
  auto t = makeLuau("binary_filter", R"({"binary_op":3,"binary_a":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 6.0)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 5.0)->value()), 0);  // strict
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 4.0)->value()), 0);
}

TEST(GoldenBinaryFilter, GreaterEq) {
  auto t = makeLuau("binary_filter", R"({"binary_op":4,"binary_a":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 6.0)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 5.0)->value()), 1);  // inclusive
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 4.0)->value()), 0);
}

TEST(GoldenBinaryFilter, Less) {
  auto t = makeLuau("binary_filter", R"({"binary_op":1,"binary_a":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 4.0)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 5.0)->value()), 0);  // strict
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 6.0)->value()), 0);
}

TEST(GoldenBinaryFilter, LessEq) {
  auto t = makeLuau("binary_filter", R"({"binary_op":2,"binary_a":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 4.0)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 5.0)->value()), 1);  // inclusive
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 6.0)->value()), 0);
}

TEST(GoldenBinaryFilter, EqualWithinDefaultTolerance) {
  // Default tol is 1e-9; exact and within-tol both pass.
  auto t = makeLuau("binary_filter", R"({"binary_op":0,"binary_a":3.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 3.0)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 3.0 + 5e-10)->value()), 1);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 3.0 - 5e-10)->value()), 1);
  // Outside tolerance fails.
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 103, 3.0 + 1e-6)->value()), 0);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 104, 3.5)->value()), 0);
}

TEST(GoldenBinaryFilter, EqualHonorsCustomFlatTolerance) {
  // binary_tol is a FLAT absolute window, not value-scaled.
  auto t = makeLuau("binary_filter", R"({"binary_op":0,"binary_a":10.0,"binary_tol":0.5})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 10.4)->value()), 1);  // |10.4 - 10| = 0.4 <= 0.5
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 10.5)->value()), 1);  // boundary inclusive
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 10.6)->value()), 0);  // 0.6 > 0.5
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 103, 9.5)->value()), 1);   // symmetric below
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 104, 9.4)->value()), 0);
}

TEST(GoldenBinaryFilter, Range) {
  auto t = makeLuau("binary_filter", R"({"binary_op":5,"binary_a":2.0,"binary_b":8.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 2.0)->value()), 1);  // lower bound inclusive
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 5.0)->value()), 1);  // interior
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 8.0)->value()), 1);  // upper bound inclusive
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 103, 1.9)->value()), 0);  // below
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 104, 8.1)->value()), 0);  // above
}

TEST(GoldenBinaryFilter, InvertedRangeYieldsAllZero) {
  // No auto min/max swap — an inverted range never passes.
  auto t = makeLuau("binary_filter", R"({"binary_op":5,"binary_a":8.0,"binary_b":2.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 100, 5.0)->value()), 0);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 101, 2.0)->value()), 0);
  EXPECT_EQ(std::get<std::int64_t>(step(*t, 102, 8.0)->value()), 0);
}

TEST(GoldenBinaryFilter, ReadsIntegerInputThroughToDouble) {
  // Int64 input coerced to double before comparison; output is int64.
  auto t = makeLuau("binary_filter", R"({"binary_op":4,"binary_a":5.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  const auto out = t->calculateNextPoint(Si(7, std::int64_t{5}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(std::get<std::int64_t>(out->value()), 1);
}

TEST(GoldenBinaryFilter, NeverSuppresses) {
  // Stateless one-to-one: every input produces an output row (0 or 1).
  auto t = makeLuau("binary_filter", R"({"binary_op":3,"binary_a":0.0})");
  ASSERT_FALSE(t->failed()) << t->error();
  EXPECT_TRUE(step(*t, 1, -1.0).has_value());
  EXPECT_TRUE(step(*t, 2, 1.0).has_value());
}

// -------------------------------- time_since_previous --------------------------------
TEST(GoldenTimeSincePrevious, OutputKindIsDoubleRegardlessOfInput) {
  auto t = makeLuau("time_since_previous", "{}");
  ASSERT_FALSE(t->failed()) << t->error();

  const PJ::StorageKind in_float = PJ::StorageKind::kFloat64;
  const auto kinds = t->outputKinds(PJ::Span<const PJ::StorageKind>(&in_float, 1));
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0], PJ::StorageKind::kFloat64);

  // Empty input kind still maps to double (the class declares output = "double").
  const PJ::StorageKind* no_kinds = nullptr;
  const auto empty = t->outputKinds(PJ::Span<const PJ::StorageKind>(no_kinds, std::size_t{0}));
  ASSERT_EQ(empty.size(), 1u);
  EXPECT_EQ(empty[0], PJ::StorageKind::kFloat64);
}

TEST(GoldenTimeSincePrevious, FirstSampleSuppressedThenDeltasInSeconds) {
  auto t = makeLuau("time_since_previous", "{}");
  ASSERT_FALSE(t->failed()) << t->error();

  // Timestamps 0, 1e9, 3e9 ns (the filter ignores the value). `t` is in seconds, so the gap comes back in seconds as a
  // double.
  const auto first = t->calculateNextPoint(Si(0, 0));
  EXPECT_FALSE(first.has_value());  // no predecessor -> suppressed

  const auto second = t->calculateNextPoint(Si(1'000'000'000, 0));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->raw_ts_ns, 1'000'000'000);
  ASSERT_TRUE(std::holds_alternative<double>(second->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(second->value()), 1.0);  // 1e9 ns gap == 1 second

  const auto third = t->calculateNextPoint(Si(3'000'000'000, 0));
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->raw_ts_ns, 3'000'000'000);
  ASSERT_TRUE(std::holds_alternative<double>(third->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(third->value()), 2.0);  // 2e9 ns gap == 2 seconds
}

TEST(GoldenTimeSincePrevious, ResetReSuppressesFirstSample) {
  auto t = makeLuau("time_since_previous", "{}");
  ASSERT_FALSE(t->failed()) << t->error();

  ASSERT_FALSE(t->calculateNextPoint(Si(0, 0)).has_value());
  ASSERT_TRUE(t->calculateNextPoint(Si(1'000'000'000, 0)).has_value());

  t->reset();

  // After reset the next sample again has no predecessor.
  const auto after = t->calculateNextPoint(Si(5'000'000'000, 0));
  EXPECT_FALSE(after.has_value());

  // And the one after that resumes producing a delta off the post-reset anchor.
  const auto delta = t->calculateNextPoint(Si(7'000'000'000, 0));
  ASSERT_TRUE(delta.has_value());
  ASSERT_TRUE(std::holds_alternative<double>(delta->value()));
  EXPECT_DOUBLE_EQ(std::get<double>(delta->value()), 2.0);  // 2e9 ns gap == 2 seconds
}

// ------------------------------- text input --------------------------------
// The filters that read the value need numbers: over a text column each fails with the script's own error instead of
// reading the text as 0.
TEST(GoldenTextInput, NumericFiltersFailOverText) {
  for (const char* id :
       {"none", "absolute", "scale", "derivative", "integral", "moving_average", "moving_rms", "moving_variance",
        "outlier_removal", "binary_filter"}) {
    auto t = makeLuau(id, "{}");
    ASSERT_FALSE(t->failed()) << id << ": " << t->error();
    bool emitted = false;
    for (std::int64_t i = 0; i < 5 && !t->failed(); ++i) {
      emitted =
          t->calculateNextPoint(Sample::scalar(i * kOneSecondNs, PJ::VarValue{std::string("walking")})).has_value() ||
          emitted;
    }
    EXPECT_TRUE(t->failed()) << id << " read text without failing";
    EXPECT_FALSE(t->error().empty()) << id;
    EXPECT_FALSE(emitted) << id << " emitted a value computed from text";
  }
}

// The filters that only look at the timestamps work over text.
TEST(GoldenTextInput, TimestampFiltersWorkOverText) {
  auto gap = makeLuau("time_since_previous", "{}");
  auto count = makeLuau("samples_counter", "{}");
  for (std::int64_t i = 0; i < 3; ++i) {
    const Sample s = Sample::scalar(i * kOneSecondNs, PJ::VarValue{std::string("walking")});
    const auto g = gap->calculateNextPoint(s);
    const auto c = count->calculateNextPoint(s);
    ASSERT_TRUE(c.has_value()) << count->error();
    if (i > 0) {
      ASSERT_TRUE(g.has_value()) << gap->error();
      EXPECT_DOUBLE_EQ(std::get<double>(g->value()), 1.0);
    }
  }
  EXPECT_FALSE(gap->failed()) << gap->error();
  EXPECT_FALSE(count->failed()) << count->error();
}

// Malformed params JSON leaves the filter at its declared defaults (createInstance seeds every default and applies
// overrides only when the JSON parses to an object): a `scale` built from non-JSON passes values through unchanged.
TEST(GoldenParamsJson, MalformedJsonKeepsDefaults) {
  auto t = makeLuau("scale", "definitely not json");
  ASSERT_FALSE(t->failed()) << t->error();
  // Defaults kept => gain 1.0, offset 0.0, no time shift => output == input.
  auto out = step(*t, kOneSecondNs, 9.0);
  ASSERT_TRUE(out.has_value());
  EXPECT_DOUBLE_EQ(dval(out), 9.0);
  EXPECT_EQ(out->raw_ts_ns, kOneSecondNs);
}
