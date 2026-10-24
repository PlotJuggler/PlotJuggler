// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_tf/tf_buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/sdk/platform.hpp"

namespace PJ {
namespace {

using pj::tf::Clock;
using pj::tf::Duration;
using pj::tf::SetTransformError;
using pj::tf::StampedTransform;
using pj::tf::TimePoint;
using pj::tf::Transform;
using pj::tf::TransformBuffer;
using pj::tf::TransformSnapshot;
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

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns), Clock::kHeader).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns), Clock::kHeader), std::runtime_error);

  const std::array<std::pair<TimePoint, double>, 6> queries{
      {{tp(10ns), 10.0}, {tp(15ns), 10.0}, {tp(20ns), 20.0}, {tp(25ns), 20.0}, {tp(30ns), 30.0}, {tp(35ns), 30.0}}};

  for (const auto& [stamp, expected_x] : queries) {
    const auto transform = buffer.lookupTransform("world", "A", stamp, Clock::kHeader);
    expectTranslation(transform, expected_x);
  }
}

TEST(TransformBufferTest, TreeWalkComposition) {
  TransformBuffer buffer;
  const TimePoint stamp = tp(100ns);

  (void)buffer.setTransform(makeStamped("map", "odom", stamp, makeTranslation(10.0, 0.0, 0.0)));
  (void)buffer.setTransform(makeStamped("odom", "base_link", stamp, makeTranslation(0.0, 2.0, 0.0)));

  const auto transform = buffer.lookupTransform("base_link", "map", stamp, Clock::kHeader);
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
  expectTranslation(buffer.lookupTransform("world", "A", TimePoint{}, Clock::kHeader), 42.0, -7.0, 3.0);
  expectTranslation(buffer.lookupTransform("world", "A", distant_future, Clock::kHeader), 42.0, -7.0, 3.0);

  // Boundary of the unified model: a query strictly BEFORE the only sample has no
  // value yet. For /tf_static stamped at the recording start this slice never
  // occurs during playback. (Switch sampleAt to clamp-before-first if a late
  // static stamp must still resolve earlier — a 2-line change.)
  const auto distant_past = tp(-std::chrono::hours(24));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", distant_past, Clock::kHeader).has_value());
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
    const auto tf = buffer.tryLookupTransform("base_link", "camera", stamp, Clock::kHeader);
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
  expectTranslation(buffer.lookupTransform("map", "sensor", tp(0s), Clock::kHeader), 9.0);
  expectTranslation(
      buffer.lookupTransform("map", "sensor", tp(5s), Clock::kHeader), 9.0);  // held forward past last stamp
  const auto distant_future = tp(std::chrono::hours(24));
  expectTranslation(buffer.lookupTransform("map", "sensor", distant_future, Clock::kHeader), 9.0);
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
  ASSERT_TRUE(buffer.tryLookupTransform("odom", "base", tp(1'000'000ns), Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("odom", "base", tp(1'000'000ns), Clock::kHeader), 10.0);
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
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(200ns), Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(200ns), Clock::kHeader), 100.0);
  // ...the sample spanning the cutoff (90ns) still answers between its stamp and the jump...
  expectTranslation(buffer.lookupTransform("map", "odom", tp(75ns), Clock::kHeader), 50.0);
  // ...older sample evicted: a scrub back below the retained tail has no value.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(25ns), Clock::kHeader).has_value());
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

  EXPECT_FALSE(buffer.tryLookupTransform("world", "missing", tp(10ns), Clock::kHeader).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "missing", tp(10ns), Clock::kHeader), std::runtime_error);

  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns), Clock::kHeader).has_value());
  EXPECT_THROW((void)buffer.lookupTransform("world", "A", tp(5ns), Clock::kHeader), std::runtime_error);
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
  EXPECT_FALSE(buffer.tryLookupTransform("A", "unrelated", tp(10ns), Clock::kHeader).has_value());
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
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(500ms), Clock::kHeader).has_value());
  // ...while the retained tail still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s), Clock::kHeader).has_value());
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
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s), Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("map", "odom", tp(1s), Clock::kHeader), 0.0);
  expectTranslation(
      buffer.lookupTransform("map", "odom", tp(6s), Clock::kHeader),
      static_cast<double>(tp(5s).time_since_epoch().count()));
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s), Clock::kHeader).has_value());
}

TEST(TransformBufferTest, QuaternionNormalize) {
  TransformBuffer buffer;
  const Transform unnormalized{{1.0, 2.0, 3.0}, glm::dquat{2.0, 0.0, 0.0, 0.0}};

  (void)buffer.setTransform(makeStamped("world", "A", tp(10ns), unnormalized));

  const auto transform = buffer.lookupTransform("world", "A", tp(10ns), Clock::kHeader);
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
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(10ns), Clock::kHeader).has_value());
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

TEST(TransformBufferTest, RootOfWalksToTheTopmostAncestor) {
  TransformBuffer buffer;
  (void)buffer.setTransform(makeStamped("map", "odom", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));

  EXPECT_EQ(buffer.rootOf("base"), "map");
  EXPECT_EQ(buffer.rootOf("odom"), "map");
  EXPECT_EQ(buffer.rootOf("map"), "map");
  EXPECT_EQ(buffer.rootOf("ghost"), "ghost") << "an unknown frame is its own root";
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
  ASSERT_TRUE(buffer.tryLookupTransform("map", "odom", tp(500ms), Clock::kHeader).has_value());

  buffer.setCacheWindow(10s);
  EXPECT_EQ(buffer.cacheWindow(), Duration(10s));

  // cutoff = 12s - 10s = 2s: the 1s sample spans it and is kept, so the 0s sample
  // is evicted retroactively.
  EXPECT_FALSE(buffer.tryLookupTransform("map", "odom", tp(500ms), Clock::kHeader).has_value());
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(1s), Clock::kHeader).has_value());
  // The retained tail (and so any recent stamp) still resolves.
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(11s), Clock::kHeader).has_value());
  EXPECT_TRUE(buffer.tryLookupTransform("map", "odom", tp(12s), Clock::kHeader).has_value());
}

// A single-sample (static) edge survives any window: setCacheWindow never empties
// an edge, so a static frame stays resolvable forever.
TEST(TransformBufferTest, SetCacheWindowKeepsStaticEdge) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("base_link", "camera", tp(0s), makeTranslation(0.5)));

  buffer.setCacheWindow(1ns);  // aggressively small window
  const auto distant_future = tp(std::chrono::hours(24));
  ASSERT_TRUE(buffer.tryLookupTransform("base_link", "camera", distant_future, Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("base_link", "camera", distant_future, Clock::kHeader), 0.5);
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
  buffer.chainSampleTimes("odom", "lidar", tp(0ns), tp(100ns), Clock::kHeader, stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{5, 10, 20, 30}));
}

TEST(TransformBufferChainSampleTimes, CrossBranchMeetsAtCommonAncestor) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  // odom and cam live in sibling branches under map; the connecting path is
  // odom->map->cam, so both edges' stamps contribute.
  (void)buffer.setTransform(makeStamped("map", "odom", tp(1ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("map", "cam", tp(2ns), makeTranslation(1.0)));

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("cam", "odom", tp(0ns), tp(100ns), Clock::kHeader, stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{1, 2}));
}

TEST(TransformBufferChainSampleTimes, ClipsToWindowAndDeduplicates) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(20ns), makeTranslation(2.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(30ns), makeTranslation(3.0)));
  (void)buffer.setTransform(makeStamped("base", "lidar", tp(20ns), makeTranslation(0.5)));  // duplicate stamp

  std::vector<TimePoint> stamps;
  buffer.chainSampleTimes("odom", "lidar", tp(15ns), tp(25ns), Clock::kHeader, stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{20}));

  // Inverted window: cleared output, no stamps.
  buffer.chainSampleTimes("odom", "lidar", tp(25ns), tp(15ns), Clock::kHeader, stamps);
  EXPECT_TRUE(stamps.empty());
}

TEST(TransformBufferChainSampleTimes, DegenerateWindowOnAnExactStampKeepsIt) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("odom", "base", tp(20ns), makeTranslation(2.0)));

  std::vector<TimePoint> stamps;
  // lo == hi landing exactly on a sample: the inclusive [lo, hi] window keeps
  // it (the classic lower_bound/upper_bound off-by-one).
  buffer.chainSampleTimes("odom", "base", tp(20ns), tp(20ns), Clock::kHeader, stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{20}));
  // lo == hi between samples: empty, not the neighbour.
  buffer.chainSampleTimes("odom", "base", tp(15ns), tp(15ns), Clock::kHeader, stamps);
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
  buffer.chainSampleTimes("odom", "cam", tp(0ns), tp(100ns), Clock::kHeader, stamps);
  EXPECT_EQ(rawStamps(stamps), (std::vector<int64_t>{5, 10, 20}));
}

TEST(TransformBufferChainSampleTimes, DisconnectedUnknownOrIdenticalIsEmpty) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(makeStamped("odom", "base", tp(10ns), makeTranslation(1.0)));
  (void)buffer.setTransform(makeStamped("island_root", "island", tp(10ns), makeTranslation(1.0)));

  std::vector<TimePoint> stamps{tp(999ns)};  // must be cleared even on failure
  buffer.chainSampleTimes("odom", "island", tp(0ns), tp(100ns), Clock::kHeader, stamps);
  EXPECT_TRUE(stamps.empty());
  buffer.chainSampleTimes("odom", "no_such_frame", tp(0ns), tp(100ns), Clock::kHeader, stamps);
  EXPECT_TRUE(stamps.empty());
  // target == source: zero hops -> no motion -> no trail stamps.
  buffer.chainSampleTimes("base", "base", tp(0ns), tp(100ns), Clock::kHeader, stamps);
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
  buffer.chainSampleTimes(
      target, source, tp(-std::chrono::hours(1)), tp(std::chrono::hours(1)), Clock::kHeader, stamps);
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
    const auto result = buffer.tryLookupTransform("world", "A", stamp, Clock::kHeader);
    ASSERT_TRUE(result.has_value());
    expectTranslation(*result, 1.0, 2.0, 3.0);
  }
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(5ns), Clock::kHeader).has_value());

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
  expectTranslation(buffer.lookupTransform("world", "A", tp(20ns), Clock::kHeader), 1.0);
  expectTranslation(buffer.lookupTransform("world", "A", tp(29ns), Clock::kHeader), 1.0);
  expectTranslation(buffer.lookupTransform("world", "A", tp(30ns), Clock::kHeader), 2.0);
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
  EXPECT_EQ(buffer.lookupTransform("world", "A", tp(20ns), Clock::kHeader).t.x, one_ulp_up);
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
  expectTranslation(buffer.lookupTransform("world", "A", tp(10ns), Clock::kHeader), 2.0);
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

  EXPECT_EQ(buffer.latestCommonTime("fixed", "moving", Clock::kHeader), tp(30ns));
  EXPECT_EQ(buffer.latestCommonTime("once", "moving", Clock::kHeader), tp(50ns));
  EXPECT_EQ(buffer.latestCommonTime("once", "world", Clock::kHeader), TimePoint{});
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
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(1s), Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s), Clock::kHeader), 2.0);

  // A lone stored sample is pinned however far the repeats advance.
  (void)buffer.setTransform(makeStamped("world", "B", tp(0s), value_a));
  (void)buffer.setTransform(makeStamped("world", "B", tp(100s), value_a));
  expectTranslation(buffer.lookupTransform("world", "B", tp(50s), Clock::kHeader), 1.0);
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
    const auto result = buffer.tryLookupTransform("world", "A", stamp, Clock::kHeader);
    ASSERT_TRUE(result.has_value()) << "query at " << stamp.time_since_epoch().count();
    expectTranslation(*result, 1.0);
  }
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s), Clock::kHeader), 2.0);

  // The spanning sample goes the moment its successor is at or before the cutoff.
  (void)buffer.setTransform(makeStamped("world", "A", tp(25s), value_b));  // cutoff = 15s: kept
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 20'000'000'000}));
  (void)buffer.setTransform(makeStamped("world", "A", tp(30s), value_b));  // cutoff = 20s: successor on it
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{20'000'000'000}));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(15s), Clock::kHeader).has_value());
  expectTranslation(buffer.lookupTransform("world", "A", tp(20s), Clock::kHeader), 2.0);
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
  expectTranslation(buffer.lookupTransform("world", "A", tp(3s), Clock::kHeader), 1.0);

  const uint64_t before_repeat = buffer.revision();
  (void)buffer.setTransform(makeStamped("world", "A", tp(6s), value_b));  // cutoff = 1s: nothing evicted
  EXPECT_EQ(buffer.revision(), before_repeat);
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{0, 5'000'000'000}));

  (void)buffer.setTransform(makeStamped("world", "A", tp(12s), value_b));  // cutoff = 7s: A evicted
  EXPECT_GT(buffer.revision(), before_repeat);
  EXPECT_EQ(chainTimes(buffer, "world", "A"), (std::vector<int64_t>{5'000'000'000}));
  EXPECT_FALSE(buffer.tryLookupTransform("world", "A", tp(3s), Clock::kHeader).has_value());

  const uint64_t after_eviction = buffer.revision();
  (void)buffer.setTransform(makeStamped("world", "A", tp(13s), value_b));  // lone sample: nothing to evict
  EXPECT_EQ(buffer.revision(), after_eviction);
}

// Reference model: every sample ever fed, unfiltered. Header clock: the
// newest stamp at or before t. Entry clock: the newest stamp among the samples
// whose entry time is at or before t ("what had arrived by t"). Equal stamps: the
// last write fed wins, in both clocks from the stamp's earliest entry time on.
// That is a known limit of the buffer, pinned here: it keeps one value per stamp,
// so a stamp written at entry 110 (v1) and replaced at entry 300 (v2) reads v2 at
// 200 in the entry clock, before v2 arrived.
struct RawSample {
  TimePoint stamp;
  TimePoint entry;
  Transform value;
};

struct RawEdge {
  std::string parent;
  std::string child;
  std::vector<RawSample> samples;  // in feed order

  std::optional<Transform> holdAt(TimePoint time, Clock clock) const {
    std::optional<TimePoint> held_stamp;
    for (const RawSample& sample : samples) {
      const TimePoint key = clock == Clock::kHeader ? sample.stamp : sample.entry;
      if (key <= time && (!held_stamp || sample.stamp > *held_stamp)) {
        held_stamp = sample.stamp;
      }
    }
    const RawSample* last_write = nullptr;
    for (const RawSample& sample : samples) {
      if (held_stamp && sample.stamp == *held_stamp) {
        last_write = &sample;
      }
    }
    return last_write != nullptr ? std::optional<Transform>(last_write->value) : std::nullopt;
  }
};

// world <- last edge: the chain's edges composed, each held at `time` in `clock`.
std::optional<Transform> composeHeld(const std::vector<RawEdge>& edges, TimePoint time, Clock clock) {
  Transform composed = Transform::identity();
  for (std::size_t edge_index = edges.size(); edge_index-- > 0;) {
    const auto held = edges[edge_index].holdAt(time, clock);
    if (!held) {
      return std::nullopt;
    }
    composed = (*held) * composed;
  }
  return composed;
}

// How a generated recording reaches the buffer.
enum class FeedOrder {
  kStamp,    // per-edge stamp order; values repeat ~70% of the time (exercises the repeat skip)
  kArrival,  // global entry-time order, as a file or a stream delivers it; every value distinct, and
             // ~10% of the samples replace the edge's previous stamp (a same-stamp replace)
};

struct ReferenceCase {
  Duration window = TransformBuffer::kKeepAll;
  int64_t entry_jitter_ns = 0;  // entry = stamp + uniform[-jitter, +jitter]; 0 = clocks coincide
  FeedOrder order = FeedOrder::kStamp;
};

// Feeds random 3-edge chains and compares every lookup, in both clocks, within the
// buffer's guaranteed range against the reference model. kKeepAll guarantees the
// whole timeline; a finite window guarantees t >= newest - window per clock.
void expectLookupsMatchReference(const ReferenceCase& test_case) {
  const std::array<Transform, 4> palette{
      makeTranslation(0.0), makeTranslation(1.0, -2.0, 0.5),
      Transform{{3.0, 0.0, 1.0}, glm::normalize(glm::dquat{0.9, 0.1, 0.2, 0.3})},
      Transform{{-1.0, 4.0, 2.0}, glm::normalize(glm::dquat{0.5, -0.5, 0.5, 0.5})}};
  constexpr int kSamplesPerEdge = 300;
  const std::array<std::pair<const char*, const char*>, 3> topology{{{"world", "a"}, {"a", "b"}, {"b", "c"}}};
  const bool keep_all = test_case.window >= TransformBuffer::kKeepAll;
  constexpr std::array<Clock, 2> kClocks{Clock::kHeader, Clock::kEntry};

  for (const uint32_t seed : {1U, 2U, 3U, 7U, 42U}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rng(seed);
    auto roll = [&rng](uint32_t bound) { return static_cast<uint32_t>(rng() % bound); };

    // Generate the recording: per edge, strictly increasing stamps.
    struct Event {
      std::size_t edge_index;
      RawSample sample;
    };
    std::vector<Event> events;
    std::vector<int64_t> stamp_clock(topology.size(), 0);
    std::vector<Transform> current(topology.size(), palette[0]);
    double distinct = 0.0;
    for (int step = 0; step < kSamplesPerEdge; ++step) {
      for (std::size_t edge_index = 0; edge_index < topology.size(); ++edge_index) {
        const bool same_stamp = test_case.order == FeedOrder::kArrival && step > 0 && roll(10) == 0;
        if (!same_stamp) {
          stamp_clock[edge_index] += 1 + roll(10);
        }
        if (test_case.order == FeedOrder::kArrival) {
          current[edge_index] = makeTranslation(++distinct);
        } else if (roll(10) < 3) {
          current[edge_index] = palette[roll(static_cast<uint32_t>(palette.size()))];
        }
        const int64_t jitter =
            test_case.entry_jitter_ns == 0
                ? 0
                : static_cast<int64_t>(roll(static_cast<uint32_t>((2 * test_case.entry_jitter_ns) + 1))) -
                      test_case.entry_jitter_ns;
        const TimePoint stamp = tp(std::chrono::nanoseconds(stamp_clock[edge_index]));
        events.push_back({edge_index, RawSample{stamp, stamp + std::chrono::nanoseconds(jitter), current[edge_index]}});
      }
    }
    if (test_case.order == FeedOrder::kArrival) {
      std::stable_sort(events.begin(), events.end(), [](const Event& lhs, const Event& rhs) {
        return lhs.sample.entry < rhs.sample.entry;
      });
    }

    std::vector<RawEdge> edges;
    for (const auto& [parent, child] : topology) {
      edges.push_back(RawEdge{parent, child, {}});
    }
    TransformBuffer buffer(test_case.window);
    std::array<int64_t, 2> lowest{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()};
    std::array<int64_t, 2> newest{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min()};

    // Random queries in [first_ns, last_ns] in `clock`, each compared with the reference.
    auto expectMatchesReference = [&](Clock clock, int64_t first_ns, int64_t last_ns, int query_count) {
      for (int query = 0; query < query_count; ++query) {
        const TimePoint time =
            tp(std::chrono::nanoseconds(
                first_ns + static_cast<int64_t>(roll(static_cast<uint32_t>(last_ns - first_ns + 1)))));
        const auto expected = composeHeld(edges, time, clock);
        const auto actual = buffer.tryLookupTransform("world", "c", time, clock);
        ASSERT_EQ(actual.has_value(), expected.has_value())
            << (clock == Clock::kHeader ? "header" : "entry") << " query at " << time.time_since_epoch().count();
        if (actual && expected) {
          expectTranslation(*actual, expected->t.x, expected->t.y, expected->t.z);
          EXPECT_NEAR(glm::dot(actual->q, expected->q), 1.0, kTolerance);
        }
      }
    };

    for (const Event& event : events) {
      auto& edge = edges[event.edge_index];
      edge.samples.push_back(event.sample);
      ASSERT_TRUE(
          buffer
              .setTransform(
                  makeStamped(edge.parent, edge.child, event.sample.stamp, event.sample.value), event.sample.entry)
              .has_value());
      const std::array<int64_t, 2> keys{
          event.sample.stamp.time_since_epoch().count(), event.sample.entry.time_since_epoch().count()};
      for (std::size_t clock_index = 0; clock_index < kClocks.size(); ++clock_index) {
        lowest[clock_index] = std::min(lowest[clock_index], keys[clock_index]);
        newest[clock_index] = std::max(newest[clock_index], keys[clock_index]);
      }
      if (!keep_all) {
        // Streaming: the range [newest - window, now] must stay exact after every write.
        for (std::size_t clock_index = 0; clock_index < kClocks.size(); ++clock_index) {
          expectMatchesReference(
              kClocks[clock_index], newest[clock_index] - test_case.window.count(), newest[clock_index] + 5, 3);
        }
        if (::testing::Test::HasFailure()) {
          return;
        }
      }
    }

    for (std::size_t clock_index = 0; clock_index < kClocks.size(); ++clock_index) {
      if (keep_all) {
        expectMatchesReference(kClocks[clock_index], lowest[clock_index] - 5, newest[clock_index] + 20, 600);
      } else {
        expectMatchesReference(
            kClocks[clock_index], newest[clock_index] - test_case.window.count(), newest[clock_index] + 20, 100);
      }
    }
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(TransformBufferDedup, MatchesUnfilteredZeroOrderHoldReference) {
  expectLookupsMatchReference({});
}

// Repeat runs longer than the window are routine here (~70% repeats, ~3.5 samples
// per window): eviction must keep the value valid at every cutoff.
TEST(TransformBufferDedup, MatchesUnfilteredZeroOrderHoldReferenceUnderFiniteWindow) {
  expectLookupsMatchReference({.window = 20ns});
}

// ---------------------------------------------------------------------------
// Two clocks (TF R6): each sample keeps its header stamp and its entry time (the
// store timestamp of the message that carried it). The cursor reads the entry
// clock; an object placed at its own stamp reads the header clock.
// ---------------------------------------------------------------------------

constexpr TimePoint ms(int64_t value) {
  return tp(std::chrono::milliseconds(value));
}

// The x translation the lookup resolved (each fixture sample moves along x only).
std::optional<double> lookupX(const TransformBuffer& buffer, TimePoint time, Clock clock) {
  const auto result = buffer.tryLookupTransform("map", "base", time, clock);
  return result.has_value() ? std::optional<double>(result->t.x) : std::nullopt;
}

// One sample of the parent->child edge: header `stamp`, logged at `entry`, moved to `x` along x.
void feed(
    TransformBuffer& buffer, TimePoint stamp, TimePoint entry, double x, const char* parent = "map",
    const char* child = "base") {
  ASSERT_TRUE(buffer.setTransform(makeStamped(parent, child, stamp, makeTranslation(x)), entry).has_value());
}

// /tf logged 40 ms after each header stamp: stamps 100, 200, 300 ms, entries 140, 240, 340 ms.
void feedLoggedLate(TransformBuffer& buffer) {
  for (int sample = 1; sample <= 3; ++sample) {
    const auto stamp = ms(100 * sample);
    feed(buffer, stamp, stamp + 40ms, sample);
  }
}

TEST(TransformBufferTwoClocks, EntryClockReturnsTheSampleReceivedByTheCursor) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  feedLoggedLate(buffer);
  EXPECT_EQ(lookupX(buffer, ms(139), Clock::kEntry), std::nullopt) << "nothing had arrived yet";
  EXPECT_EQ(lookupX(buffer, ms(230), Clock::kEntry), 1.0) << "the 200 ms sample arrives at 240 ms";
  EXPECT_EQ(lookupX(buffer, ms(240), Clock::kEntry), 2.0);
  EXPECT_EQ(lookupX(buffer, ms(1000), Clock::kEntry), 3.0);
}

TEST(TransformBufferTwoClocks, HeaderClockPlacesAnObjectAtItsOwnStamp) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  feedLoggedLate(buffer);
  // An object stamped 195 ms, shown at cursor 245 ms: where the frame was when it
  // was measured is the 100 ms sample, not the one the cursor has received.
  EXPECT_EQ(lookupX(buffer, ms(195), Clock::kHeader), 1.0);
  EXPECT_EQ(lookupX(buffer, ms(245), Clock::kEntry), 2.0);
  EXPECT_EQ(lookupX(buffer, ms(99), Clock::kHeader), std::nullopt);
  EXPECT_EQ(lookupX(buffer, ms(300), Clock::kHeader), 3.0);
}

TEST(TransformBufferTwoClocks, LateOlderStampDoesNotRewindTheEntryClock) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  feed(buffer, ms(200), ms(210), 2.0);
  feed(buffer, ms(150), ms(260), 1.5);
  EXPECT_EQ(lookupX(buffer, ms(205), Clock::kEntry), std::nullopt);
  EXPECT_EQ(lookupX(buffer, ms(215), Clock::kEntry), 2.0);
  EXPECT_EQ(lookupX(buffer, ms(300), Clock::kEntry), 2.0) << "the newest stamp known wins, not the last arrival";
  EXPECT_EQ(lookupX(buffer, ms(180), Clock::kHeader), 1.5);
  EXPECT_EQ(lookupX(buffer, ms(210), Clock::kHeader), 2.0);
}

TEST(TransformBufferTwoClocks, SimulatedTimeHeaderStampsResolveInBothClocks) {
  // Header stamps in simulated time (seconds since the simulation started), logged
  // on the wall clock.
  const TimePoint wall_start = tp(std::chrono::seconds(1'700'000'000));
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  for (int second = 0; second < 10; ++second) {
    const auto sim = tp(std::chrono::seconds(second));
    const auto logged = wall_start + std::chrono::seconds(second) + 5ms;
    feed(buffer, sim, logged, second);
  }
  EXPECT_EQ(lookupX(buffer, wall_start + 4500ms, Clock::kEntry), 4.0);
  EXPECT_EQ(lookupX(buffer, tp(4500ms), Clock::kHeader), 4.0);
  EXPECT_EQ(lookupX(buffer, tp(4500ms), Clock::kEntry), std::nullopt) << "a sim stamp is no entry time";
}

TEST(TransformBufferTwoClocks, RepeatThatArrivedEarlierStillCountsInTheEntryClock) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  feed(buffer, ms(100), ms(150), 1.0);
  const uint64_t before = buffer.revision();
  // Same value, newer stamp, but logged before its predecessor: not stored, yet by
  // 130 ms the value had arrived.
  feed(buffer, ms(200), ms(120), 1.0);
  EXPECT_EQ(lookupX(buffer, ms(130), Clock::kEntry), 1.0);
  EXPECT_GT(buffer.revision(), before) << "what entry-clock lookups resolve changed";
  EXPECT_EQ(chainTimes(buffer, "map", "base"), (std::vector<int64_t>{100'000'000}));
}

TEST(TransformBufferTwoClocks, LatestCommonTimeAndChainSampleTimesFollowTheClock) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  for (int sample = 1; sample <= 2; ++sample) {
    const auto stamp = ms(100 * sample);
    feed(buffer, stamp, stamp + 40ms, sample, "map", "odom");
    feed(buffer, stamp, stamp + 10ms, sample, "odom", "base");
  }
  EXPECT_EQ(buffer.latestCommonTime("map", "base", Clock::kHeader), ms(200));
  EXPECT_EQ(buffer.latestCommonTime("map", "base", Clock::kEntry), ms(210));

  std::vector<TimePoint> times;
  buffer.chainSampleTimes("map", "base", ms(0), ms(1000), Clock::kEntry, times);
  EXPECT_EQ(times, (std::vector<TimePoint>{ms(110), ms(140), ms(210), ms(240)}));
  buffer.chainSampleTimes("map", "base", ms(0), ms(1000), Clock::kHeader, times);
  EXPECT_EQ(times, (std::vector<TimePoint>{ms(100), ms(200)}));
}

// Eviction keeps each clock's live window (newest - window). A backlog flushed in
// one burst: stamps 0-600 ms every 10 ms, logged 1 ms apart from 1000 ms on. The
// header window alone would drop the samples the cursor still needs.
TEST(TransformBufferTwoClocks, FiniteWindowKeepsTheEntryClockWindow) {
  TransformBuffer buffer(100ms);
  for (int index = 0; index <= 60; ++index) {
    const auto stamp = ms(10 * index);
    feed(buffer, stamp, ms(1000 + index), index);
  }
  EXPECT_EQ(lookupX(buffer, ms(1000), Clock::kEntry), 0.0);
  EXPECT_EQ(lookupX(buffer, ms(1030), Clock::kEntry), 30.0);
  EXPECT_EQ(lookupX(buffer, ms(505), Clock::kHeader), 50.0);
}

// The entry window alone would drop the samples a recent object stamp needs: one
// sample logged 300 ms late pushes the newest entry time ahead of the stream.
TEST(TransformBufferTwoClocks, FiniteWindowKeepsTheHeaderClockWindow) {
  TransformBuffer buffer(100ms);
  for (int index = 0; index <= 60; ++index) {
    const auto stamp = ms(10 * index);
    const auto entry = stamp + (index == 60 ? 300ms : 1ms);
    feed(buffer, stamp, entry, index);
  }
  EXPECT_EQ(lookupX(buffer, ms(505), Clock::kHeader), 50.0);
  EXPECT_EQ(lookupX(buffer, ms(800), Clock::kEntry), 59.0);
  EXPECT_EQ(lookupX(buffer, ms(900), Clock::kEntry), 60.0);
}

// Recordings whose entry clock differs from their header stamps, fed in stamp order
// (with the repeat skip) and in arrival order (a sample can arrive after a newer one).
TEST(TransformBufferTwoClocks, MatchesReferenceWithJitteredEntryTimes) {
  expectLookupsMatchReference({.entry_jitter_ns = 30});
}

TEST(TransformBufferTwoClocks, MatchesReferenceWithJitteredEntryTimesUnderFiniteWindow) {
  expectLookupsMatchReference({.window = 20ns, .entry_jitter_ns = 30});
}

TEST(TransformBufferTwoClocks, MatchesReferenceInArrivalOrder) {
  expectLookupsMatchReference({.entry_jitter_ns = 30, .order = FeedOrder::kArrival});
}

TEST(TransformBufferTwoClocks, MatchesReferenceInArrivalOrderUnderFiniteWindow) {
  expectLookupsMatchReference({.window = 20ns, .entry_jitter_ns = 30, .order = FeedOrder::kArrival});
}

// With header == entry for every sample (simulated-time logs, bags logged with
// header stamps) the two clocks agree with each other and with a buffer fed through
// the stamp-only overload, on lookups, sample grids and revision.
TEST(TransformBufferTwoClocks, CoincidingClocksMatchTheStampOnlyBuffer) {
  std::mt19937 rng(11);
  auto roll = [&rng](uint32_t bound) { return static_cast<uint32_t>(rng() % bound); };
  TransformBuffer stamp_only(20ns);
  TransformBuffer both(20ns);
  const std::array<std::pair<const char*, const char*>, 2> topology{{{"map", "odom"}, {"odom", "base"}}};
  for (int step = 0; step < 400; ++step) {
    const auto& [parent, child] = topology[roll(2)];
    // Out of order, same-stamp replaces and repeats all occur.
    const TimePoint stamp = tp(std::chrono::nanoseconds(step + static_cast<int64_t>(roll(8)) - 4));
    const Transform value = makeTranslation(roll(3));
    const uint64_t stamp_only_before = stamp_only.revision();
    const uint64_t both_before = both.revision();
    ASSERT_EQ(
        stamp_only.setTransform(makeStamped(parent, child, stamp, value)).has_value(),
        both.setTransform(makeStamped(parent, child, stamp, value), stamp).has_value());
    // Revisions are unique across buffers, so the two agree on whether a write bumped.
    ASSERT_EQ(stamp_only.revision() != stamp_only_before, both.revision() != both_before) << "step " << step;
    for (int query = 0; query < 4; ++query) {
      const TimePoint time = tp(std::chrono::nanoseconds(step - 30 + static_cast<int64_t>(roll(40))));
      const auto reference = stamp_only.tryLookupTransform("map", "base", time, Clock::kHeader);
      for (const Clock clock : {Clock::kHeader, Clock::kEntry}) {
        const auto actual = both.tryLookupTransform("map", "base", time, clock);
        ASSERT_EQ(actual.has_value(), reference.has_value()) << "step " << step;
        if (actual) {
          EXPECT_EQ(*actual, *reference);
        }
      }
    }
  }
  std::vector<TimePoint> reference_times;
  stamp_only.chainSampleTimes("map", "base", tp(-1s), tp(1s), Clock::kHeader, reference_times);
  for (const Clock clock : {Clock::kHeader, Clock::kEntry}) {
    std::vector<TimePoint> times;
    both.chainSampleTimes("map", "base", tp(-1s), tp(1s), clock, times);
    EXPECT_EQ(times, reference_times);
    EXPECT_EQ(both.latestCommonTime("map", "base", clock), stamp_only.latestCommonTime("map", "base", Clock::kHeader));
  }
}

// -----------------------------------------------------------------------------
// TransformSnapshot (docs/derived_objects/ARCHITECTURE.md §12, R1/R4): an
// immutable state of a buffer, taken in O(1); the buffer copies its state on the
// next write while a snapshot holds it.
// -----------------------------------------------------------------------------

// A live append that evicts, a new frame, a retention change and a reset all leave
// an existing snapshot unchanged; a snapshot taken afterwards sees the new state.
TEST(TransformSnapshot, SnapshotIsImmutableWhileTheBufferMutates) {
  TransformBuffer buffer(10ns);
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(1.0))).has_value());
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(105ns), makeTranslation(2.0))).has_value());
  const std::shared_ptr<const TransformSnapshot> snapshot = buffer.snapshot();
  ASSERT_NE(snapshot, nullptr);
  const uint64_t revision = snapshot->revision();
  const auto expect_frozen = [&](const char* after) {
    EXPECT_EQ(snapshot->revision(), revision) << after;
    const auto at_100 = snapshot->tryLookupTransform("map", "odom", tp(100ns), Clock::kHeader);
    ASSERT_TRUE(at_100.has_value()) << after;
    expectTranslation(*at_100, 1.0);
    const auto at_120 = snapshot->tryLookupTransform("map", "odom", tp(120ns), Clock::kHeader);
    ASSERT_TRUE(at_120.has_value()) << after;
    expectTranslation(*at_120, 2.0);
    EXPECT_EQ(
        snapshot->tryLookupTransform("map", "base", tp(120ns), Clock::kHeader).error(),
        pj::tf::LookupError::kUnknownSource)
        << after;
    std::vector<std::string> frames = snapshot->getAllFrames();
    std::sort(frames.begin(), frames.end());
    EXPECT_EQ(frames, (std::vector<std::string>{"map", "odom"})) << after;
  };

  // 120 is past the 10 ns window: the sample at 100 is evicted from the buffer.
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(120ns), makeTranslation(3.0))).has_value());
  ASSERT_TRUE(buffer.setTransform(makeStamped("odom", "base", tp(120ns), makeTranslation(4.0))).has_value());
  ASSERT_FALSE(buffer.tryLookupTransform("map", "odom", tp(100ns), Clock::kHeader).has_value());
  expect_frozen("live append");
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(121ns), makeTranslation(5.0))).has_value());

  // Retention alone, on a state a snapshot shares: the 1 ns window drops the sample
  // at 105, which still holds the value at 110.
  const auto before_retention = buffer.snapshot();
  buffer.setCacheWindow(1ns);
  EXPECT_NE(buffer.revision(), before_retention->revision()) << "the eviction must bump the revision";
  ASSERT_TRUE(before_retention->tryLookupTransform("map", "odom", tp(110ns), Clock::kHeader).has_value())
      << "retention evicted from a shared state";
  expectTranslation(*before_retention->tryLookupTransform("map", "odom", tp(110ns), Clock::kHeader), 2.0);
  EXPECT_FALSE(buffer.snapshot()->tryLookupTransform("map", "odom", tp(110ns), Clock::kHeader).has_value());
  expect_frozen("retention");

  const auto before_clear = buffer.snapshot();
  buffer.clear();
  EXPECT_EQ(before_clear->getAllFrames().size(), 3U) << "clear emptied a shared state";
  expect_frozen("clear");

  const auto fresh = buffer.snapshot();
  EXPECT_NE(fresh->revision(), revision);
  EXPECT_NE(fresh->revision(), before_clear->revision());
  EXPECT_TRUE(fresh->getAllFrames().empty());
}

// adoptStateOf gives a buffer another buffer's current state, shared without a
// copy, so holders of the adopting buffer see the replacement; the epoch counts
// such whole-state replacements and clear(), never a write.
TEST(TransformSnapshot, AdoptStateOfSharesTheOtherBuffersState) {
  TransformBuffer active;
  TransformBuffer staging;
  ASSERT_TRUE(active.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(1.0))).has_value());
  ASSERT_TRUE(staging.setTransform(makeStamped("map", "base", tp(100ns), makeTranslation(2.0))).has_value());
  const auto before = active.snapshot();
  const uint64_t epoch = active.epoch();

  active.adoptStateOf(staging);
  EXPECT_EQ(active.snapshot(), staging.snapshot()) << "one state, no copy";
  EXPECT_EQ(active.revision(), staging.revision());
  EXPECT_EQ(active.epoch(), epoch + 1);
  EXPECT_TRUE(active.tryLookupTransform("map", "base", tp(100ns), Clock::kHeader).has_value());
  EXPECT_FALSE(active.tryLookupTransform("map", "odom", tp(100ns), Clock::kHeader).has_value());
  EXPECT_TRUE(before->tryLookupTransform("map", "odom", tp(100ns), Clock::kHeader).has_value());

  // A write to one copies the shared state first and leaves the other alone.
  ASSERT_TRUE(active.setTransform(makeStamped("map", "lidar", tp(100ns), makeTranslation(3.0))).has_value());
  EXPECT_FALSE(staging.tryLookupTransform("map", "lidar", tp(100ns), Clock::kHeader).has_value());
  EXPECT_EQ(active.epoch(), epoch + 1) << "a write is not a replacement";
  active.clear();
  EXPECT_EQ(active.epoch(), epoch + 2);
}

// Storage identity, not timing: a snapshot is the buffer's own state object until
// the next write, which moves the buffer to a copy and leaves the snapshot alone.
TEST(TransformSnapshot, ASnapshotSharesTheBufferStateUntilTheNextWrite) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(1.0))).has_value());
  const auto first = buffer.snapshot();
  const auto second = buffer.snapshot();
  EXPECT_EQ(first, second) << "no write in between: one state object, no copy";
  EXPECT_EQ(first.use_count(), 3) << "held by the buffer, `first` and `second`";

  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "odom", tp(200ns), makeTranslation(2.0))).has_value());
  EXPECT_EQ(first.use_count(), 2) << "the write copied the state: the buffer no longer holds the snapshot";
  EXPECT_NE(buffer.snapshot(), first);
}

// A snapshot answers both clocks exactly as its buffer did when it was taken. The
// header stamps are 40 ns before their entry (log) times.
TEST(TransformSnapshot, SnapshotLookupHonoursBothClocks) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "base", tp(100ns), makeTranslation(1.0)), tp(140ns)).has_value());
  ASSERT_TRUE(buffer.setTransform(makeStamped("map", "base", tp(200ns), makeTranslation(2.0)), tp(240ns)).has_value());
  const auto snapshot = buffer.snapshot();

  // At 120 the sample stamped 100 exists but has not arrived; at 220 the one
  // stamped 200 exists but the entry clock still holds the earlier one.
  EXPECT_FALSE(snapshot->tryLookupTransform("map", "base", tp(120ns), Clock::kEntry).has_value());
  expectTranslation(*snapshot->tryLookupTransform("map", "base", tp(120ns), Clock::kHeader), 1.0);
  expectTranslation(*snapshot->tryLookupTransform("map", "base", tp(220ns), Clock::kEntry), 1.0);
  expectTranslation(*snapshot->tryLookupTransform("map", "base", tp(220ns), Clock::kHeader), 2.0);
  for (const int64_t time : {90, 100, 139, 140, 199, 200, 239, 240, 300}) {
    for (const Clock clock : {Clock::kHeader, Clock::kEntry}) {
      const auto from_snapshot = snapshot->tryLookupTransform("map", "base", tp(std::chrono::nanoseconds(time)), clock);
      const auto from_buffer = buffer.tryLookupTransform("map", "base", tp(std::chrono::nanoseconds(time)), clock);
      ASSERT_EQ(from_snapshot.has_value(), from_buffer.has_value()) << time;
      if (from_snapshot) {
        EXPECT_EQ(*from_snapshot, *from_buffer) << time;
      }
    }
  }
}

// Revisions come from one process-wide counter: two buffers fed the same writes
// never report the same revision, so a revision identifies one state even after
// the buffer holding it is replaced.
TEST(TransformSnapshot, RevisionsAreUniqueAcrossBuffers) {
  TransformBuffer first;
  TransformBuffer second;
  EXPECT_NE(first.revision(), second.revision());
  for (TransformBuffer* buffer : {&first, &second}) {
    ASSERT_TRUE(buffer->setTransform(makeStamped("map", "odom", tp(100ns), makeTranslation(1.0))).has_value());
  }
  EXPECT_NE(first.snapshot()->revision(), second.snapshot()->revision());
}

// PJ_BENCH=1 prints what a snapshot costs: the snapshot() call itself, and the
// state copy the next write pays while a snapshot holds the state, for a complete
// 1 h file buffer and live windows of 5 s and 60 s (25 edges at 100 Hz, every
// sample distinct). Baselines: docs/derived_objects/ARCHITECTURE.md §19.
TEST(TransformSnapshotBench, SnapshotAndCopyOnWriteCost) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr int kEdges = 25;
  constexpr int64_t kTickMs = 10;  // 100 Hz
  constexpr std::size_t kSampleBytes = sizeof(TimePoint) * 2 + sizeof(Transform);
  std::vector<std::string> children;
  for (int edge = 0; edge < kEdges; ++edge) {
    children.push_back("e" + std::to_string(edge));
  }
  int64_t tick = 0;
  const auto write_tick = [&](TransformBuffer& buffer) {
    const TimePoint stamp = tp(std::chrono::milliseconds(tick * kTickMs));
    for (const std::string& child : children) {
      (void)buffer.setTransform(makeStamped("root", child, stamp, makeTranslation(static_cast<double>(tick))));
    }
    ++tick;
  };
  const auto micros = [](auto begin, auto end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
  };
  const auto measure = [&](const char* label, TransformBuffer& buffer) {
    std::vector<TimePoint> edge_times;
    buffer.chainSampleTimes("root", "e0", TimePoint::min(), TimePoint::max(), Clock::kHeader, edge_times);
    const std::size_t samples = edge_times.size() * kEdges;

    constexpr int kCalls = 1000;
    const auto calls_begin = std::chrono::steady_clock::now();
    std::shared_ptr<const TransformSnapshot> held;
    for (int call = 0; call < kCalls; ++call) {
      held = buffer.snapshot();
    }
    const double snapshot_us = micros(calls_begin, std::chrono::steady_clock::now()) / kCalls;
    EXPECT_EQ(held.use_count(), 2) << "a snapshot shares the buffer state: nothing copied";

    std::vector<double> copy_us;
    for (int run = 0; run < 5; ++run) {
      held = buffer.snapshot();
      const auto write_begin = std::chrono::steady_clock::now();
      write_tick(buffer);
      copy_us.push_back(micros(write_begin, std::chrono::steady_clock::now()));
      EXPECT_EQ(held.use_count(), 1) << "the write copied the state";
    }
    std::sort(copy_us.begin(), copy_us.end());
    std::printf(
        "[bench] %-22s %9zu samples (%7.1f MB of samples): snapshot() %.3f us, 0 bytes; "
        "first write after it (state copy) median %.2f ms\n",
        label, samples, static_cast<double>(samples * kSampleBytes) / 1e6, snapshot_us, copy_us[2] / 1000.0);
  };

  for (const int window_s : {5, 60}) {
    TransformBuffer live{std::chrono::seconds(window_s)};
    tick = 0;
    for (int64_t fill = 0; fill < int64_t{2} * window_s * 1000 / kTickMs; ++fill) {
      write_tick(live);
    }
    measure(window_s == 5 ? "live, 5 s window" : "live, 60 s window", live);
  }
  TransformBuffer file(TransformBuffer::kKeepAll);
  tick = 0;
  for (int64_t fill = 0; fill < int64_t{3600} * 1000 / kTickMs; ++fill) {
    write_tick(file);
  }
  measure("complete 1 h file", file);
}

}  // namespace
}  // namespace PJ
