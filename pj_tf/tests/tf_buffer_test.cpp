// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_tf/tf_buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace PJ {
namespace {

using pj::tf::Duration;
using pj::tf::SetTransformError;
using pj::tf::StampedTransform;
using pj::tf::TimePoint;
using pj::tf::Transform;
using pj::tf::TransformBuffer;
using namespace std::chrono_literals;

constexpr double kTolerance = 1e-9;

// The tests express stamps as ns/s offsets from the epoch; wrap a duration as an
// absolute TimePoint (TimePoint is a time_point now, not a bare duration). The
// TransformBuffer cache-window arguments stay bare durations, so they are not
// wrapped.
constexpr TimePoint tp(std::chrono::nanoseconds ns) {
  return TimePoint{ns};
}

Transform makeTranslation(double x, double y = 0.0, double z = 0.0) {
  return Transform{{x, y, z}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

Transform makeTranslationFromStamp(TimePoint stamp) {
  return makeTranslation(static_cast<double>(stamp.time_since_epoch().count()));
}

StampedTransform makeStamped(
    const std::string& parent, const std::string& child, TimePoint stamp, const Transform& transform) {
  return StampedTransform{stamp, parent, child, transform};
}

void expectTranslation(const Transform& transform, double x, double y = 0.0, double z = 0.0) {
  EXPECT_NEAR(transform.t.x, x, kTolerance);
  EXPECT_NEAR(transform.t.y, y, kTolerance);
  EXPECT_NEAR(transform.t.z, z, kTolerance);
}

double quaternionNorm(const glm::dquat& q) {
  return std::sqrt((q.w * q.w) + (q.x * q.x) + (q.y * q.y) + (q.z * q.z));
}

TEST(TransformBufferTest, ZOHBoundary) {
  TransformBuffer buffer;

  const std::array<TimePoint, 3> samples{tp(10ns), tp(20ns), tp(30ns)};
  for (const TimePoint stamp : samples) {
    (void)buffer.setTransform(makeStamped("world", "A", stamp, makeTranslationFromStamp(stamp)));
  }

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);

  const std::array<std::pair<TimePoint, double>, 6> queries{
      {{tp(10ns), 10.0}, {tp(15ns), 10.0}, {tp(20ns), 20.0}, {tp(25ns), 20.0}, {tp(30ns), 30.0}, {tp(35ns), 30.0}}};

  for (const auto& [stamp, expected_x] : queries) {
    const auto transform = buffer.lookupTransform("world", "A", stamp);
    expectTranslation(transform, expected_x);
  }
}

TEST(TransformBufferTest, TreeWalkComposition) {
  TransformBuffer buffer;
  const TimePoint stamp = tp(100ns);

  (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslation(10.0, 0.0, 0.0)));
  (void)buffer.setTransform(makeStamped("odom", "base_link", stamp, makeTranslation(0.0, 2.0, 0.0)));

  const auto transform = buffer.lookupTransform("base_link", "map", stamp);
  expectTranslation(transform, -10.0, -2.0, 0.0);
}

TEST(TransformBufferTest, ReparentingIsRejected) {
  TransformBuffer buffer;

  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0))).has_value());

  // A second publisher claims child "A" under a different parent. Rejected with
  // ReparentConflict (not thrown), so a bulk ingest can drop it and continue.
  const auto result = buffer.setTransform(makeStamped("foo", "A", tp(20ns), makeTranslation(2.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::kReparentConflict);
}

// A transform published once resolves at its stamp and every later time, with no
// static flag. This is how /tf_static is modelled now: a single-sample history
// held forward by nearest-previous. /tf_static is conventionally stamped at (or
// near) the recording start, so it covers the whole playback range.
TEST(TransformBufferTest, SingleSampleHoldsForward) {
  TransformBuffer buffer;
  const auto transform = makeTranslation(42.0, -7.0, 3.0);

  (void)buffer.setTransform(makeStamped("world", "A", TimePoint{}, transform));

  const auto distant_future = tp(std::chrono::hours(24));
  expectTranslation(buffer.lookupTransform("world", "A", TimePoint{}), 42.0, -7.0, 3.0);
  expectTranslation(buffer.lookupTransform("world", "A", distant_future), 42.0, -7.0, 3.0);

  // Boundary of the unified model: a query strictly BEFORE the only sample has no
  // value yet. For /tf_static stamped at the recording start this slice never
  // occurs during playback. (Switch sampleAt to clamp-before-first if a late
  // static stamp must still resolve earlier — a 2-line change.)
  const auto distant_past = tp(-std::chrono::hours(24));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", distant_past).has_value());
}

// GNERSIS PR2 review item A1, reproduced at the core level. A namespaced
// "/robot1/tf_static" edge used to be string-matched against "/tf_static" in
// TransformService to decide static-ness, so namespaced static topics fell
// through to dynamic and orphaned. The buffer is now told nothing about the
// topic: a single sample at the recording start resolves for every later query
// regardless of the topic name, so the bug cannot exist.
TEST(TransformBufferTest, NamespacedStaticResolvesWithoutTopicHint) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("base_link", "camera", TimePoint{}, makeTranslation(0.5, 0.0, 1.0)));

  for (const TimePoint stamp : {tp(0ns), tp(1'000'000'000ns), tp(999'000'000'000ns)}) {
    const auto tf = buffer.tryLookupTransform("base_link", "camera", stamp);
    ASSERT_TRUE(tf.has_value());
    expectTranslation(*tf, 0.5, 0.0, 1.0);
  }
}

// A re-latched static transform arrives several times with identical values but
// increasing stamps. No special case: the vector keeps the samples and
// nearest-previous resolves them, including far past the last stamp.
TEST(TransformBufferTest, RepublishedStaticUsesNearestPrevious) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto value = makeTranslation(9.0, 0.0, 0.0);
  for (const TimePoint stamp : {tp(0s), tp(1s), tp(2s)}) {
    (void)buffer.setTransform(makeStamped("map", "sensor", stamp, value));
  }
  expectTranslation(buffer.lookupTransform("map", "sensor", tp(0s)), 9.0);
  expectTranslation(buffer.lookupTransform("map", "sensor", tp(5s)), 9.0);  // held forward past last stamp
  const auto distant_future = tp(std::chrono::hours(24));
  expectTranslation(buffer.lookupTransform("map", "sensor", distant_future), 9.0);
}

// A dynamic edge that stops updating keeps resolving at and after its last sample
// forever — even under a finite (streaming) window — because eviction runs only
// when THAT edge is written and never empties it. This is what the static flag
// used to guarantee, now applied to every edge uniformly.
TEST(TransformBufferTest, StoppedDynamicKeepsResolvingUnderFiniteWindow) {
  TransformBuffer buffer(10ns);  // tiny rolling window
  for (const TimePoint stamp : {tp(0ns), tp(5ns), tp(10ns)}) {
    (void)buffer.setTransform(makeStamped("odom", "base", stamp, makeTranslationFromStamp(stamp)));
  }
  // The edge stops here. A query far in the future still holds the last sample.
  ASSERT_TRUE(buffer.tryLookupTransform("odom", "base", tp(1'000'000ns)).has_value());
  expectTranslation(buffer.lookupTransform("odom", "base", tp(1'000'000ns)), 10.0);
}

// Under a finite window, a time jump much larger than the window evicts stale
// samples, but the most recent one is pinned (samples.size() > 1 guard): the edge
// is never emptied, so lookups at/after the jump resolve. The newest sample at or
// before the cutoff (50ns) is kept because it holds the value valid there; scrubbing
// back below it orphans — the inherent cost of a finite window, absent under kKeepAll.
TEST(TransformBufferTest, FiniteWindowPinsMostRecentSample) {
  TransformBuffer buffer(10ns);
  (void)buffer.setTransform(makeStamped("map", "odom", tp(0ns), makeTranslation(0.0)));
  (void)buffer.setTransform(makeStamped("map", "odom", tp(50ns), makeTranslation(50.0)));
  (void)buffer.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(100.0)));  // jump >> window

  // Most recent sample pinned and resolving...
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(200ns)).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(200ns)), 100.0);
  // ...the sample spanning the cutoff (90ns) still answers between its stamp and the jump...
  expectTranslation(buffer.lookupTransform("map", "odom", tp(75ns)), 50.0);
  // ...older sample evicted: a scrub back below the retained tail has no value.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(25ns)).has_value());
}

TEST(TransformBufferTest, Introspection) {
  TransformBuffer buffer;
  const TimePoint stamp_a = tp(11ns);

  (void)buffer.setTransform(makeStamped("world", "A", stamp_a, makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "B", tp(12ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("A", "C", tp(13ns), makeTranslation(3.0)));

  const auto all_frames = buffer.getAllFrames();
  const std::set<std::string> frames(all_frames.begin(), all_frames.end());
  const std::set<std::string> expected_frames{"world", "A", "B", "C"};

  EXPECT_EQ(all_frames.size(), 4U);
  EXPECT_EQ(frames, expected_frames);

  const auto parent = buffer.getParent("A");
  EXPECT_TRUE(parent.has_value());
  if (parent.has_value()) {
    EXPECT_EQ(*parent, "world");
  }
  EXPECT_FALSE(buffer.getParent("nonexistent").has_value());

  const auto latest_a = buffer.getLatestSample("A");
  EXPECT_TRUE(latest_a.has_value());
  if (latest_a.has_value()) {
    EXPECT_EQ(*latest_a, stamp_a);
  }
  EXPECT_FALSE(buffer.getLatestSample("nonexistent").has_value());

  // B is now just a single-sample edge stamped at 12ns (no static special-case),
  // so its latest sample is 12ns rather than the old static sentinel TimePoint{}.
  const auto latest_b = buffer.getLatestSample("B");
  EXPECT_TRUE(latest_b.has_value());
  if (latest_b.has_value()) {
    EXPECT_EQ(*latest_b, tp(12ns));
  }
}

// The out-param getAllFrames overload (used by the axis pass to reuse capacity
// across frames) must produce the same frame set as the by-value version and
// must CLEAR pre-existing content in the caller's vector first.
TEST(TransformBufferTest, GetAllFramesOutParamMatchesAndClears) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("world", "A", tp(11ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "B", tp(12ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("A", "C", tp(13ns), makeTranslation(3.0)));

  // Pre-seed stale entries to prove the overload clears before refilling.
  std::vector<std::string> scratch{"stale_one", "stale_two"};
  buffer.getAllFrames(scratch);

  const std::set<std::string> got(scratch.begin(), scratch.end());
  const std::set<std::string> expected{"world", "A", "B", "C"};
  EXPECT_EQ(scratch.size(), 4U);
  EXPECT_EQ(got, expected);

  // Same content as the by-value overload it now delegates to.
  const auto by_value = buffer.getAllFrames();
  EXPECT_EQ(std::set<std::string>(by_value.begin(), by_value.end()), got);

  // Reuse on an empty buffer empties the vector (clear actually runs).
  TransformBuffer empty;
  empty.getAllFrames(scratch);
  EXPECT_TRUE(scratch.empty());
}

TEST(TransformBufferTest, TryLookupNoThrow) {
  TransformBuffer buffer;

  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0)));

  EXPECT_FALSE(buffer.tryLookupTransform("world", "missing", tp(10ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "missing", tp(10ns)), std::runtime_error);

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns)), std::runtime_error);
}

TEST(TransformBufferTest, CyclicTreeDoesNotHang) {
  TransformBuffer buffer;

  // Malformed TF tree: A and B parent each other. Neither call is a reparent
  // (each child's parent is set exactly once), so the reparent guard does not
  // reject it. chainToRoot must still terminate.
  (void)buffer.setTransform(makeStamped("B", "A", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("A", "B", tp(10ns), makeTranslation(2.0)));

  // Walking A toward its root traverses the cycle; this must return cleanly
  // rather than spin forever. No path exists to an unrelated frame.
  EXPECT_FALSE(buffer.tryLookupTransform("A", "unrelated", tp(10ns)).has_value());
}

TEST(TransformBufferTest, SelfParentIgnored) {
  TransformBuffer buffer;

  // A frame relative to itself is the identity and must not register a
  // self-loop in the parent map; it is reported as a dropped SelfLoop edge.
  const auto result = buffer.setTransform(makeStamped("A", "A", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::kSelfLoop);

  EXPECT_FALSE(buffer.getParent("A").has_value());
}

// Reproduces the "dynamic-frame object only renders at the end of the timeline"
// bug: the default 10s rolling window, fed a whole file's TF in time order,
// trims each dynamic edge to its tail so lookups before [t_end - window] fail.
TEST(TransformBufferTest, DefaultWindowEvictsHistoryAfterBulkIngest) {
  TransformBuffer buffer;  // default 10s window
  const std::array<TimePoint, 5> stamps{tp(0s), tp(1s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // cutoff = 12s - 10s = 2s: the 1s sample spans it and is kept, the 0s sample is
  // evicted, so a lookup at 0.5s fails...
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(500ms)).has_value());
  // ...while the retained tail still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s)).has_value());
}

// The fix: kKeepAll disables eviction, so the full history stays queryable —
// what the TransformService uses for bulk-ingested files.
TEST(TransformBufferTest, KeepAllRetainsFullHistoryAfterBulkIngest) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const std::array<TimePoint, 4> stamps{tp(0s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // Early sample retained: lookup at 1s holds the 0s value (ZOH).
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(1s)), 0.0);
  expectTranslation(
      buffer.lookupTransform("map", "odom", tp(6s)), static_cast<double>(tp(5s).time_since_epoch().count()));
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s)).has_value());
}

TEST(TransformBufferTest, QuaternionNormalize) {
  TransformBuffer buffer;
  const Transform unnormalized{{1.0, 2.0, 3.0}, glm::dquat{2.0, 0.0, 0.0, 0.0}};

  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), unnormalized));

  const auto transform = buffer.lookupTransform("world", "A", tp(10ns));
  EXPECT_NEAR(quaternionNorm(transform.q), 1.0, kTolerance);
}

// A zero-length quaternion (a realistic wire value: an uninitialized publisher or
// a proto3 decoder that doesn't default w=1) would normalize to all-NaN and
// silently poison every lookup through the edge. It must be rejected, not stored.
TEST(TransformBufferTest, ZeroQuaternionRejected) {
  TransformBuffer buffer;
  const Transform zero_rot{{1.0, 2.0, 3.0}, glm::dquat{0.0, 0.0, 0.0, 0.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), zero_rot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::kInvalidRotation);
  // No edge stored, so the frame is unknown.
  EXPECT_FALSE(buffer.getParent("A").has_value());
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(10ns)).has_value());
}

TEST(TransformBufferTest, NaNQuaternionRejected) {
  TransformBuffer buffer;
  const double nan = std::nan("");
  const Transform nan_rot{{1.0, 2.0, 3.0}, glm::dquat{nan, 0.0, 0.0, 1.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), nan_rot));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::kInvalidRotation);
  EXPECT_FALSE(buffer.getParent("A").has_value());
}

TEST(TransformBufferTest, NonFiniteTranslationRejected) {
  TransformBuffer buffer;
  const double inf = std::numeric_limits<double>::infinity();
  const Transform bad_t{{std::nan(""), 0.0, inf}, glm::dquat{1.0, 0.0, 0.0, 0.0}};

  const auto result = buffer.setTransform(makeStamped("world", "A", tp(10ns), bad_t));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SetTransformError::kNonFiniteTranslation);
  EXPECT_FALSE(buffer.getParent("A").has_value());
}

TEST(TransformBufferTest, EmptyFrameNameRejected) {
  TransformBuffer buffer;

  const auto empty_parent = buffer.setTransform(makeStamped("", "child", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(empty_parent.has_value());
  EXPECT_EQ(empty_parent.error(), SetTransformError::kInvalidFrameName);

  const auto empty_child = buffer.setTransform(makeStamped("parent", "", tp(10ns), makeTranslation(1.0)));
  ASSERT_FALSE(empty_child.has_value());
  EXPECT_EQ(empty_child.error(), SetTransformError::kInvalidFrameName);

  // Neither phantom "" frame leaked into the buffer.
  EXPECT_TRUE(buffer.getAllFrames().empty());
}

// areConnected is a pure reachability predicate (no time arg): true once a common
// ancestor exists, regardless of per-edge sample times.
TEST(TransformBufferTest, AreConnectedReportsReachability) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("map", "odom", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("world", "drone", tp(10ns), makeTranslation(1.0)));

  EXPECT_TRUE(buffer.areConnected("map", "base"));
  EXPECT_TRUE(buffer.areConnected("base", "map"));
  EXPECT_TRUE(buffer.areConnected("base", "base"));   // identity
  EXPECT_FALSE(buffer.areConnected("map", "drone"));  // separate forest
  EXPECT_FALSE(buffer.areConnected("map", "ghost"));  // unknown frame
}

// setCacheWindow trims existing histories with the same rule as ingest eviction,
// always keeping the last sample per edge so recent lookups still succeed.
TEST(TransformBufferTest, SetCacheWindowTrimsButKeepsLastSample) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const std::array<TimePoint, 5> stamps{tp(0s), tp(1s), tp(5s), tp(11s), tp(12s)};
  for (const TimePoint stamp : stamps) {
    (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslationFromStamp(stamp)));
  }
  // Under kKeepAll the full history is present: an early lookup resolves.
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(500ms)).has_value());

  buffer.setCacheWindow(10s);
  EXPECT_EQ(buffer.cacheWindow(), Duration(10s));

  // cutoff = 12s - 10s = 2s: the 1s sample spans it and is kept, so the 0s sample
  // is evicted retroactively.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(500ms)).has_value());
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s)).has_value());
  // The retained tail (and so any recent stamp) still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s)).has_value());
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s)).has_value());
}

// A single-sample (static) edge survives any window: setCacheWindow never empties
// an edge, so a static frame stays resolvable forever.
TEST(TransformBufferTest, SetCacheWindowKeepsStaticEdge) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("base_link", "camera", tp(0s), makeTranslation(0.5)));

  buffer.setCacheWindow(1ns);  // aggressively small window
  const auto distant_future = tp(std::chrono::hours(24));
  ASSERT_TRUE(buffer.tryLookupTransform("base_link", "camera", distant_future).has_value());
  expectTranslation(buffer.lookupTransform("base_link", "camera", distant_future), 0.5);
}

// revision() bumps on every insert, different-value replace and clear(), but NOT on
// a rejected edge — it is a change-detection token for callers gating recompute.
// (Skipped repeats and eviction: see TransformBufferDedup.)
TEST(TransformBufferTest, RevisionBumpsOnMutationNotOnReject) {
  TransformBuffer buffer;
  const uint64_t initial = buffer.revision();

  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(1.0))).has_value());
  const uint64_t after_insert = buffer.revision();
  EXPECT_GT(after_insert, initial);

  // In-place replace at the same stamp is still a mutation: bump.
  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(2.0))).has_value());
  EXPECT_GT(buffer.revision(), after_insert);
  const uint64_t after_replace = buffer.revision();

  // A rejected edge (self-loop) does not bump.
  EXPECT_FALSE(buffer.setTransform(makeStamped("B", "B", tp(20ns), makeTranslation(1.0))).has_value());
  EXPECT_EQ(buffer.revision(), after_replace);

  // A rejected invalid-rotation edge does not bump either.
  const Transform zero_rot{{0.0, 0.0, 0.0}, glm::dquat{0.0, 0.0, 0.0, 0.0}};
  EXPECT_FALSE(buffer.setTransform(makeStamped("world", "C", tp(30ns), zero_rot)).has_value());
  EXPECT_EQ(buffer.revision(), after_replace);

  // clear() bumps.
  buffer.clear();
  EXPECT_GT(buffer.revision(), after_replace);
}

std::vector<int64_t> rawStamps(const std::vector<TimePoint>& stamps) {
  std::vector<int64_t> raw;
  raw.reserve(stamps.size());
  for (const TimePoint stamp : stamps) {
    raw.push_back(stamp.time_since_epoch().count());
  }
  return raw;
}

TEST(TransformBufferChainSampleTimes, UnionsEveryEdgeOnThePath) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  // odom -> base moves (stamps 10/20/30); base -> lidar is static (single stamp
  // 5). The lidar trail in odom must sample at the PARENT edge's stamps too —
  // the lidar's own edge never changes, all its motion comes from odom->base.
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(20ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(30ns), makeTranslation(3.0)));
  (void)buffer.setTransform(makeStamped("base", "lidar", tp(5ns), makeTranslation(0.5)));

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("odom", "lidar", tp(0ns), tp(100ns), stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{5, 10, 20, 30}));
}

TEST(TransformBufferChainSampleTimes, CrossBranchMeetsAtCommonAncestor) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  // odom and cam live in sibling branches under map; the connecting path is
  // odom->map->cam, so both edges' stamps contribute.
  (void)buffer.setTransform(makeStamped("map", "odom", tp(1ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("map", "cam", tp(2ns), makeTranslation(1.0)));

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("cam", "odom", tp(0ns), tp(100ns), stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{1, 2}));
}

TEST(TransformBufferChainSampleTimes, ClipsToWindowAndDeduplicates) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(20ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(30ns), makeTranslation(3.0)));
  (void)buffer.setTransform(makeStamped("base", "lidar", tp(20ns), makeTranslation(0.5)));  // duplicate stamp

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("odom", "lidar", tp(15ns), tp(25ns), stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{20}));

  // Inverted window: cleared output, no stamps.
  buffer.chainSampleTimes("odom", "lidar", tp(25ns), tp(15ns), stamps);
  EXPECT_TRUE(stamps.empty());
}

TEST(TransformBufferChainSampleTimes, DegenerateWindowOnAnExactStampKeepsIt) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(20ns), makeTranslation(2.0)));

  std::vector<TimePoint> stamps;
  // lo == hi landing exactly on a sample: the inclusive [lo, hi] window keeps
  // it (the classic lower_bound/upper_bound off-by-one).
  buffer.chainSampleTimes("odom", "base", tp(20ns), tp(20ns), stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{20}));
  // lo == hi between samples: empty, not the neighbour.
  buffer.chainSampleTimes("odom", "base", tp(15ns), tp(15ns), stamps);
  EXPECT_TRUE(stamps.empty());
}

TEST(TransformBufferChainSampleTimes, TargetSideChainContributesStamps) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  // The MOVING edge is on the target's side of the meet point: map->odom moves,
  // the source (cam) hangs statically off map.
  (void)buffer.setTransform(makeStamped("map", "odom", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("map", "odom", tp(20ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("map", "cam", tp(5ns), makeTranslation(0.5)));

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("odom", "cam", tp(0ns), tp(100ns), stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{5, 10, 20}));
}

TEST(TransformBufferChainSampleTimes, DisconnectedUnknownOrIdenticalIsEmpty) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("island_root", "island", tp(10ns), makeTranslation(1.0)));

  std::vector<TimePoint> stamps{tp(999ns)};  // must be cleared even on failure
  buffer.chainSampleTimes("odom", "island", tp(0ns), tp(100ns), stamps);
  EXPECT_TRUE(stamps.empty());
  buffer.chainSampleTimes("odom", "no_such_frame", tp(0ns), tp(100ns), stamps);
  EXPECT_TRUE(stamps.empty());
  // target == source: zero hops -> no motion -> no trail stamps.
  buffer.chainSampleTimes("base", "base", tp(0ns), tp(100ns), stamps);
  EXPECT_TRUE(stamps.empty());
}

// ---------------------------------------------------------------------------
// Duplicate-sample dedup: an in-order sample exactly equal (component by component,
// no tolerance) to the edge's last stored value is not stored (zero-order hold makes
// it redundant), but the edge still remembers the newest stamp it received so
// latest-sample / latestCommonTime / the eviction cutoff behave as if every sample
// had been kept, and eviction keeps the value valid at the cutoff.
// ---------------------------------------------------------------------------

std::vector<int64_t> chainTimes(const TransformBuffer& buffer, const std::string& target, const std::string& source) {
  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes(target, source, tp(-std::chrono::hours(1)), tp(std::chrono::hours(1)), stamps);
  return rawStamps(stamps);
}

TEST(TransformBufferDedup, RepeatedIdenticalSampleIsNotStored) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto value = makeTranslation(1.0, 2.0, 3.0);
  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), value)).has_value());
  const uint64_t after_first = buffer.revision();
  for (const TimePoint stamp : {tp(20ns), tp(30ns)}) {
    ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", stamp, value)).has_value());
  }

  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{10}));
  EXPECT_EQ(buffer.revision(), after_first);
  EXPECT_EQ(buffer.getLatestSample("A"), tp(30ns));
  for (const TimePoint stamp : {tp(10ns), tp(20ns), tp(30ns), tp(1'000ns)}) {
    const auto result = buffer.tryLookupTransform("world", "A", stamp);
    ASSERT_TRUE(result.has_value());
    expectTranslation(*result, 1.0, 2.0, 3.0);
  }
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns)).has_value());

  // An older stamp with a new value is stored but never moves the newest stamp back.
  ASSERT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(25ns), makeTranslation(2.0))).has_value());
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{10, 25}));
  EXPECT_EQ(buffer.getLatestSample("A"), tp(30ns));
}

TEST(TransformBufferDedup, ChangedValueAfterRepeatsIsStored) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto value_a = makeTranslation(1.0);
  const auto value_b = makeTranslation(2.0);
  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), value_a));
  (void)buffer.setTransform(makeStamped("world", "A", tp(20ns), value_a));
  const uint64_t before_change = buffer.revision();
  (void)buffer.setTransform(makeStamped("world", "A", tp(30ns), value_b));

  EXPECT_GT(buffer.revision(), before_change);
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{10, 30}));
  expectTranslation(buffer.lookupTransform("world", "A", tp(20ns)), 1.0);
  expectTranslation(buffer.lookupTransform("world", "A", tp(29ns)), 1.0);
  expectTranslation(buffer.lookupTransform("world", "A", tp(30ns)), 2.0);
}

// No tolerance: a one-ulp difference is a different value and must be stored.
TEST(TransformBufferDedup, OneUlpDifferenceIsStored) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const double base = 1.0;
  const double one_ulp_up = std::nextafter(base, std::numeric_limits<double>::infinity());
  ASSERT_NE(base, one_ulp_up);
  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), makeTranslation(base, 2.0, 3.0)));
  (void)buffer.setTransform(makeStamped("world", "A", tp(20ns), makeTranslation(one_ulp_up, 2.0, 3.0)));

  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{10, 20}));
  EXPECT_EQ(buffer.lookupTransform("world", "A", tp(20ns)).t.x, one_ulp_up);
}

TEST(TransformBufferDedup, SameStampSameValueIsANoOp) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto value_a = makeTranslation(1.0);
  const auto value_b = makeTranslation(2.0);
  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), value_a));
  (void)buffer.setTransform(makeStamped("world", "A", tp(20ns), value_b));
  const uint64_t baseline = buffer.revision();

  // Equal stamp, equal value: both at the tail and strictly inside the history.
  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(20ns), value_b)).has_value());
  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), value_a)).has_value());
  EXPECT_EQ(buffer.revision(), baseline);

  // Equal stamp, different value still replaces in place and bumps.
  EXPECT_TRUE(buffer.setTransform(makeStamped("world", "A", tp(10ns), value_b)).has_value());
  EXPECT_GT(buffer.revision(), baseline);
  expectTranslation(buffer.lookupTransform("world", "A", tp(10ns)), 2.0);
}

// A republished-identical edge bounds latestCommonTime by the newest stamp it
// received, exactly as if every repeat had been stored; an edge published at a
// single stamp still imposes no bound.
TEST(TransformBufferDedup, LatestCommonTimeIsUnchangedByRepeats) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  const auto fixed_value = makeTranslation(1.0);
  for (const TimePoint stamp : {tp(10ns), tp(20ns), tp(30ns)}) {
    (void)buffer.setTransform(makeStamped("world", "fixed", stamp, fixed_value));
  }
  for (int step = 1; step <= 5; ++step) {
    (void)buffer.setTransform(
        makeStamped("world", "moving", tp(std::chrono::nanoseconds(step * 10)), makeTranslation(step)));
  }
  (void)buffer.setTransform(makeStamped("world", "once", tp(10ns), makeTranslation(7.0)));

  EXPECT_EQ(buffer.latestCommonTime("fixed", "moving"), tp(30ns));
  EXPECT_EQ(buffer.latestCommonTime("once", "moving"), tp(50ns));
  EXPECT_EQ(buffer.latestCommonTime("once", "world"), TimePoint{});
}

// The eviction cutoff is (newest RECEIVED stamp - window), so a skipped repeat
// ages out old samples exactly as a stored one would, and never empties the edge.
TEST(TransformBufferDedup, EvictionCutoffFollowsNewestReceivedStamp) {
  TransformBuffer buffer(10s);
  const auto value_a = makeTranslation(1.0);
  const auto value_b = makeTranslation(2.0);
  (void)buffer.setTransform(makeStamped("world", "A", tp(0s), value_a));
  (void)buffer.setTransform(makeStamped("world", "A", tp(5s), value_b));
  ASSERT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 5'000'000'000}));

  (void)buffer.setTransform(makeStamped("world", "A", tp(20s), value_b));  // skipped; cutoff = 10s

  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{5'000'000'000}));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(1s)).has_value());
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s)), 2.0);

  // A lone stored sample is pinned however far the repeats advance.
  (void)buffer.setTransform(makeStamped("world", "B", tp(0s), value_a));
  (void)buffer.setTransform(makeStamped("world", "B", tp(100s), value_a));
  expectTranslation(buffer.lookupTransform("world", "B", tp(50s)), 1.0);
  EXPECT_EQ(buffer.getLatestSample("B"), tp(100s));
}

// A run of skipped repeats longer than the window must not evict the value that
// holds across the cutoff: only the NEWEST sample at or before the cutoff is kept.
TEST(TransformBufferDedup, RepeatsLongerThanTheWindowKeepTheSpanningValue) {
  TransformBuffer buffer(10s);
  const auto value_a = makeTranslation(1.0);
  const auto value_b = makeTranslation(2.0);
  (void)buffer.setTransform(makeStamped("world", "A", tp(0s), value_a));
  for (int second = 1; second < 20; ++second) {
    (void)buffer.setTransform(makeStamped("world", "A", tp(std::chrono::seconds(second)), value_a));  // skipped
  }
  (void)buffer.setTransform(makeStamped("world", "A", tp(20s), value_b));  // cutoff = 10s

  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 20'000'000'000}));
  for (const TimePoint stamp : {tp(5s), tp(10s), tp(15s), tp(19s)}) {
    const auto result = buffer.tryLookupTransform("world", "A", stamp);
    ASSERT_TRUE(result.has_value()) << "query at " << stamp.time_since_epoch().count();
    expectTranslation(*result, 1.0);
  }
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s)), 2.0);

  // The spanning sample goes the moment its successor is at or before the cutoff.
  (void)buffer.setTransform(makeStamped("world", "A", tp(25s), value_b));  // cutoff = 15s: kept
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 20'000'000'000}));
  (void)buffer.setTransform(makeStamped("world", "A", tp(30s), value_b));  // cutoff = 20s: successor on it
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{20'000'000'000}));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(15s)).has_value());
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s)), 2.0);
}

// A skipped repeat can evict, which changes what lookups resolve: it must bump the
// revision exactly then, and not when it evicts nothing.
TEST(TransformBufferDedup, SkippedRepeatThatEvictsBumpsRevision) {
  TransformBuffer buffer(5s);
  const auto value_a = makeTranslation(1.0);
  const auto value_b = makeTranslation(2.0);
  (void)buffer.setTransform(makeStamped("world", "A", tp(0s), value_a));
  (void)buffer.setTransform(makeStamped("world", "A", tp(5s), value_b));
  ASSERT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 5'000'000'000}));
  expectTranslation(buffer.lookupTransform("world", "A", tp(3s)), 1.0);

  const uint64_t before_repeat = buffer.revision();
  (void)buffer.setTransform(makeStamped("world", "A", tp(6s), value_b));  // cutoff = 1s: nothing evicted
  EXPECT_EQ(buffer.revision(), before_repeat);
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 5'000'000'000}));

  (void)buffer.setTransform(makeStamped("world", "A", tp(12s), value_b));  // cutoff = 7s: A evicted
  EXPECT_GT(buffer.revision(), before_repeat);
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{5'000'000'000}));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(3s)).has_value());

  const uint64_t after_eviction = buffer.revision();
  (void)buffer.setTransform(makeStamped("world", "A", tp(13s), value_b));  // lone sample: nothing to evict
  EXPECT_EQ(buffer.revision(), after_eviction);
}

// Reference model: zero-order hold over the FULL, unfiltered per-edge sample list.
struct RawEdge {
  std::string parent;
  std::string child;
  std::vector<std::pair<TimePoint, Transform>> samples;  // ascending stamps, as fed

  std::optional<Transform> holdAt(TimePoint stamp) const {
    std::optional<Transform> held;
    for (const auto& [sample_stamp, value] : samples) {
      if (sample_stamp <= stamp) {
        held = value;
      }
    }
    return held;
  }
};

// world <- last edge: the chain's edges composed, each held at `stamp`.
std::optional<Transform> composeHeld(const std::vector<RawEdge>& edges, TimePoint stamp) {
  Transform composed = Transform::identity();
  for (std::size_t edge_index = edges.size(); edge_index-- > 0;) {
    const auto held = edges[edge_index].holdAt(stamp);
    if (!held) {
      return std::nullopt;
    }
    composed = (*held) * composed;
  }
  return composed;
}

// Feeds random repeat-heavy 3-edge chains and compares every lookup in the buffer's
// guaranteed range against zero-order hold over the unfiltered samples. kKeepAll
// guarantees the whole timeline; a finite window guarantees t >= newest - window.
void expectLookupsMatchUnfilteredHold(Duration window) {
  const std::array<Transform, 4> palette{
      makeTranslation(0.0), makeTranslation(1.0, -2.0, 0.5),
      Transform{{3.0, 0.0, 1.0}, glm::normalize(glm::dquat{0.9, 0.1, 0.2, 0.3})},
      Transform{{-1.0, 4.0, 2.0}, glm::normalize(glm::dquat{0.5, -0.5, 0.5, 0.5})}};
  constexpr int kSamplesPerEdge = 300;
  const std::array<std::pair<const char*, const char*>, 3> topology{{{"world", "a"}, {"a", "b"}, {"b", "c"}}};
  const bool keep_all = window >= TransformBuffer::kKeepAll;

  for (const uint32_t seed : {1U, 2U, 3U, 7U, 42U}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rng(seed);
    auto roll = [&rng](uint32_t bound) { return static_cast<uint32_t>(rng() % bound); };

    std::vector<RawEdge> edges;
    for (const auto& [parent, child] : topology) {
      edges.push_back(RawEdge{parent, child, {}});
    }
    std::vector<int64_t> clock(edges.size(), 0);
    std::vector<Transform> current(edges.size(), palette[0]);
    TransformBuffer buffer(window);

    // Random queries in [first_ns, last_ns], each compared with the unfiltered hold.
    auto expectMatchesReference = [&](int64_t first_ns, int64_t last_ns, int query_count) {
      for (int query = 0; query < query_count; ++query) {
        const TimePoint stamp =
            tp(std::chrono::nanoseconds(
                first_ns + static_cast<int64_t>(roll(static_cast<uint32_t>(last_ns - first_ns + 1)))));
        const auto expected = composeHeld(edges, stamp);
        const auto actual = buffer.tryLookupTransform("world", "c", stamp);
        ASSERT_EQ(actual.has_value(), expected.has_value()) << "query at " << stamp.time_since_epoch().count();
        if (actual && expected) {
          expectTranslation(*actual, expected->t.x, expected->t.y, expected->t.z);
          EXPECT_NEAR(glm::dot(actual->q, expected->q), 1.0, kTolerance);
        }
      }
    };

    for (int step = 0; step < kSamplesPerEdge; ++step) {
      for (std::size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
        clock[edge_index] += 1 + roll(10);
        if (roll(10) < 3) {  // ~70% of samples repeat the previous value
          current[edge_index] = palette[roll(static_cast<uint32_t>(palette.size()))];
        }
        auto& edge = edges[edge_index];
        const TimePoint stamp = tp(std::chrono::nanoseconds(clock[edge_index]));
        edge.samples.emplace_back(stamp, current[edge_index]);
        ASSERT_TRUE(buffer.setTransform(makeStamped(edge.parent, edge.child, stamp, current[edge_index])).has_value());
      }
      if (!keep_all) {
        // Streaming: the range [newest - window, now] must stay exact after every write.
        const int64_t newest = *std::max_element(clock.begin(), clock.end());
        expectMatchesReference(newest - window.count(), newest + 5, 6);
        if (::testing::Test::HasFailure()) {
          return;
        }
      }
    }

    const int64_t newest = *std::max_element(clock.begin(), clock.end());
    if (keep_all) {
      expectMatchesReference(-5, newest + 20, 600);
    } else {
      expectMatchesReference(newest - window.count(), newest + 20, 100);
    }
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(TransformBufferDedup, MatchesUnfilteredZeroOrderHoldReference) {
  expectLookupsMatchUnfilteredHold(TransformBuffer::kKeepAll);
}

// Repeat runs longer than the window are routine here (~70% repeats, ~3.5 samples
// per window): eviction must keep the value valid at every cutoff.
TEST(TransformBufferDedup, MatchesUnfilteredZeroOrderHoldReferenceUnderFiniteWindow) {
  expectLookupsMatchUnfilteredHold(20ns);
}

}  // namespace
}  // namespace PJ
