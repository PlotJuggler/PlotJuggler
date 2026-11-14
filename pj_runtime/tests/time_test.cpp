// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Compile-fence + behavior tests for the canonical time vocabulary (Time.h).
// The static_asserts are the real point: they prove the type system rejects the
// mistakes the vocabulary exists to prevent.

#include "pj_runtime/Time.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <type_traits>

namespace {

template <class A, class B>
concept Addable = requires(A a, B b) { a + b; };

template <class T>
concept CanPassBracedDisplaySeconds = requires(T t) { [](PJ::DisplaySeconds) {}({t}); };

template <class T>
concept CanPassBracedDisplayRange = requires(T t) { [](PJ::DisplayRange) {}({t, t}); };

// An absolute instant plus another absolute instant is meaningless and must not
// compile — this is the instant-vs-duration guarantee std::chrono gives us.
static_assert(!Addable<PJ::Timepoint, PJ::Timepoint>, "Timepoint + Timepoint must be ill-formed");
// ...but instant - instant (a span) and instant + duration (a shifted instant) do.
static_assert(Addable<PJ::Timepoint, PJ::Duration>, "Timepoint + Duration must compile");

// DisplaySeconds must never silently interconvert with a bare double: the whole
// point is that an absolute-seconds double can't masquerade as a display coord.
static_assert(!std::is_convertible_v<double, PJ::DisplaySeconds>);
static_assert(!std::is_convertible_v<PJ::DisplaySeconds, double>);
static_assert(!CanPassBracedDisplaySeconds<double>);
static_assert(!CanPassBracedDisplayRange<double>);

// A raw int64 timestamp must go through fromRaw(), never implicitly become a
// Timepoint.
static_assert(!std::is_convertible_v<PJ::Timestamp, PJ::Timepoint>);

TEST(Time, RawRoundTrip) {
  const PJ::Timestamp ts = 1'717'500'000'123'456'789LL;
  EXPECT_EQ(PJ::toRaw(PJ::fromRaw(ts)), ts);
}

TEST(Time, RawToDisplaySecondsSubtractsOffset) {
  const PJ::Timestamp raw_ns = 5'000'000'000LL;     // 5 s since epoch
  const PJ::Timestamp offset_ns = 2'000'000'000LL;  // 2 s display origin
  const PJ::DisplayOffset offset{std::chrono::nanoseconds{offset_ns}};

  const PJ::DisplaySeconds s = PJ::rawToDisplaySeconds(raw_ns, offset);
  EXPECT_DOUBLE_EQ(s.value, static_cast<double>(raw_ns - offset_ns) / PJ::kNanosecondsPerSecond);
  EXPECT_DOUBLE_EQ(s.value, 3.0);
}

TEST(Time, DisplaySecondsToRawIsInverse) {
  const PJ::DisplayOffset offset{std::chrono::nanoseconds{2'000'000'000LL}};
  const PJ::DisplaySeconds s{3.0};

  const PJ::Timestamp raw_ns = PJ::displaySecondsToRaw(s, offset);
  EXPECT_EQ(raw_ns, 5'000'000'000LL);
  EXPECT_DOUBLE_EQ(PJ::rawToDisplaySeconds(raw_ns, offset).value, 3.0);
}

TEST(Time, OffsetOfReadsTimeDomain) {
  PJ::TimeDomain domain;
  domain.display_offset = 7'000'000'000LL;
  EXPECT_EQ(PJ::offsetOf(domain).value, std::chrono::nanoseconds{7'000'000'000LL});
}

TEST(Time, AxisDoubleRoundTrip) {
  const PJ::DisplaySeconds s{42.5};
  EXPECT_DOUBLE_EQ(PJ::toAxisDouble(s), 42.5);
  EXPECT_TRUE(PJ::fromAxisDouble(42.5) == s);
}

TEST(Time, DisplaySecondsHelpers) {
  const PJ::DisplaySeconds s = PJ::displaySeconds(42.5);
  EXPECT_DOUBLE_EQ(s.value, 42.5);

  const PJ::DisplayRange range = PJ::displayRange(1.25, 9.5);
  EXPECT_DOUBLE_EQ(range.min.value, 1.25);
  EXPECT_DOUBLE_EQ(range.max.value, 9.5);
}

TEST(Time, FromRawRangeLiftsBothEnds) {
  const PJ::Range<PJ::Timestamp> raw{1'000'000'000LL, 5'000'000'000LL};
  const PJ::Range<PJ::Timepoint> lifted = PJ::fromRawRange(raw);
  EXPECT_EQ(PJ::toRaw(lifted.min), 1'000'000'000LL);
  EXPECT_EQ(PJ::toRaw(lifted.max), 5'000'000'000LL);
}

TEST(Time, DisplaySecondsOrdersAndSubtracts) {
  const PJ::DisplaySeconds a{10.0};
  const PJ::DisplaySeconds b{4.0};
  EXPECT_TRUE(b < a);
  EXPECT_DOUBLE_EQ(a - b, 6.0);                       // delta is a bare double
  EXPECT_TRUE((a - 1.0) == PJ::DisplaySeconds{9.0});  // shift by a delta stays typed
}

TEST(Time, AtOrAfterLeavesAnInstantPastInt64RangeAtTheNearestTime) {
  // An "end of time" sentinel a plugin may pass to to_display_time: one double step above it no longer
  // fits a Timestamp, so the round-up must not try (it would convert an out-of-range double).
  for (const PJ::Timestamp raw :
       {std::numeric_limits<PJ::Timestamp>::max(), std::numeric_limits<PJ::Timestamp>::min()}) {
    EXPECT_EQ(
        PJ::rawToDisplaySecondsAtOrAfter(raw, PJ::DisplayOffset{}).value,
        PJ::rawToDisplaySeconds(raw, PJ::DisplayOffset{}).value)
        << raw;
  }
}

}  // namespace
