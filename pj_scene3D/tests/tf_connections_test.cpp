// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/tf/tf_connections.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "pj_tf/tf_buffer.h"
#include "pj_tf/transform.h"

namespace pj::scene3d {
namespace {

using pj::tf::StampedTransform;
using pj::tf::Transform;

using namespace std::chrono_literals;

constexpr TimePoint tp(std::chrono::nanoseconds ns) {
  return TimePoint{ns};
}

Transform translation(double x, double y, double z) {
  return Transform{{x, y, z}, glm::dquat{1.0, 0.0, 0.0, 0.0}};
}

StampedTransform stamped(const std::string& parent, const std::string& child, TimePoint stamp, const Transform& tf) {
  return StampedTransform{stamp, parent, child, tf};
}

// First segment whose child endpoint matches `c` (within tolerance), or nullptr.
const TfConnectionSegment* findByChild(const std::vector<TfConnectionSegment>& segs, const glm::vec3& c) {
  constexpr float kTol = 1e-4f;
  for (const auto& seg : segs) {
    if (glm::length(seg.child - c) < kTol) {
      return &seg;
    }
  }
  return nullptr;
}

// TF R6: the lines join the triads at the cursor, in the entry clock. The 200 ns
// sample (x = 2) is logged at 300 ns, so at 250 ns the line still ends at x = 1.
TEST(TfConnectionsTest, SegmentsReadTheEntryClock) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  ASSERT_TRUE(tf.setTransform(stamped("world", "a", tp(100ns), translation(1, 0, 0)), tp(100ns)).has_value());
  ASSERT_TRUE(tf.setTransform(stamped("world", "a", tp(200ns), translation(2, 0, 0)), tp(300ns)).has_value());

  const auto segs = buildTfConnectionSegments(tf, "world", tp(250ns));
  ASSERT_EQ(segs.size(), 1u);
  EXPECT_NE(findByChild(segs, {1, 0, 0}), nullptr);
}

TEST(TfConnectionsTest, OneSegmentPerNonRootFrameToParentOrigin) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  (void)tf.setTransform(stamped("world", "a", tp(10ns), translation(1, 0, 0)));
  (void)tf.setTransform(stamped("world", "b", tp(10ns), translation(0, 2, 0)));
  (void)tf.setTransform(stamped("a", "c", tp(10ns), translation(0, 0, 3)));

  const auto segs = buildTfConnectionSegments(tf, "world", tp(100ns));

  // a, b, c each yield one segment; world is a root and yields none.
  ASSERT_EQ(segs.size(), 3u);

  const auto* sa = findByChild(segs, {1, 0, 0});
  ASSERT_NE(sa, nullptr);
  EXPECT_NEAR(glm::length(sa->parent - glm::vec3(0, 0, 0)), 0.0f, 1e-4f);

  const auto* sb = findByChild(segs, {0, 2, 0});
  ASSERT_NE(sb, nullptr);
  EXPECT_NEAR(glm::length(sb->parent - glm::vec3(0, 0, 0)), 0.0f, 1e-4f);

  // c sits at world (1,0,3); its parent a sits at world (1,0,0).
  const auto* sc = findByChild(segs, {1, 0, 3});
  ASSERT_NE(sc, nullptr);
  EXPECT_NEAR(glm::length(sc->parent - glm::vec3(1, 0, 0)), 0.0f, 1e-4f);
}

TEST(TfConnectionsTest, RootFrameProducesNoSegment) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  (void)tf.setTransform(stamped("world", "a", tp(10ns), translation(1, 0, 0)));

  const auto segs = buildTfConnectionSegments(tf, "world", tp(100ns));

  ASSERT_EQ(segs.size(), 1u);
  // The only segment is for "a"; none originates at the world root.
  EXPECT_EQ(findByChild(segs, {0, 0, 0}), nullptr);
}

TEST(TfConnectionsTest, SkipsFrameUnresolvableAtTime) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  (void)tf.setTransform(stamped("world", "a", tp(10ns), translation(1, 0, 0)));
  // d is only sampled in the future; at t=100ns its edge has no sample yet.
  (void)tf.setTransform(stamped("world", "d", tp(500ns), translation(9, 9, 9)));

  const auto segs = buildTfConnectionSegments(tf, "world", tp(100ns));

  ASSERT_EQ(segs.size(), 1u);
  EXPECT_NE(findByChild(segs, {1, 0, 0}), nullptr);
  EXPECT_EQ(findByChild(segs, {9, 9, 9}), nullptr);
}

TEST(TfConnectionsTest, OutParameterIsClearedBeforeAppending) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  (void)tf.setTransform(stamped("world", "a", tp(10ns), translation(1, 0, 0)));

  std::vector<TfConnectionSegment> out;
  out.push_back({glm::vec3(7, 7, 7), glm::vec3(8, 8, 8)});  // stale content
  buildTfConnectionSegments(tf, "world", tp(100ns), out);

  ASSERT_EQ(out.size(), 1u);  // stale entry cleared, one real segment
  EXPECT_NE(findByChild(out, {1, 0, 0}), nullptr);
}

}  // namespace
}  // namespace pj::scene3d
