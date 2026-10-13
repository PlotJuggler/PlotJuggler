// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/trail_sampling.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {
namespace {

using pj::tf::StampedTransform;
using pj::tf::Transform;

using namespace std::chrono_literals;

constexpr TimePoint tp(std::chrono::nanoseconds ns) {
  return TimePoint{ns};
}

StampedTransform stampedAt(std::chrono::nanoseconds ns, std::string parent, std::string child, double x) {
  StampedTransform tf;
  tf.stamp = tp(ns);
  tf.parent_frame = std::move(parent);
  tf.child_frame = std::move(child);
  tf.transform.t = glm::dvec3(x, 0.0, 0.0);
  return tf;
}

TEST(DecimateEvenly, KeepsAllWhenUnderCap) {
  EXPECT_EQ(decimateEvenly(3, 10), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(decimateEvenly(0, 10), std::vector<std::size_t>{});
  EXPECT_EQ(decimateEvenly(1, 10), std::vector<std::size_t>{0});
  EXPECT_EQ(decimateEvenly(5, 0), std::vector<std::size_t>{});
  EXPECT_EQ(decimateEvenly(5, 1), std::vector<std::size_t>{0});
}

TEST(DecimateEvenly, KeepsFirstAndLastAndIsDeterministic) {
  const auto kept = decimateEvenly(1001, 100);
  ASSERT_EQ(kept.size(), 100U);
  EXPECT_EQ(kept.front(), 0U);
  EXPECT_EQ(kept.back(), 1000U);
  EXPECT_EQ(kept, decimateEvenly(1001, 100));
  for (std::size_t i = 1; i < kept.size(); ++i) {
    EXPECT_LT(kept[i - 1], kept[i]);
  }
}

TEST(BuildTfFrameTrail, TracksMovingFrame) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(stampedAt(10ns, "odom", "base", 1.0));
  (void)buffer.setTransform(stampedAt(20ns, "odom", "base", 2.0));
  (void)buffer.setTransform(stampedAt(30ns, "odom", "base", 3.0));

  const TrailPolyline trail = buildTfFrameTrail(buffer, "odom", "base", TimePoint::min(), TimePoint::max(), 1000);
  ASSERT_EQ(trail.size(), 3U);
  EXPECT_EQ(trail[0].t, tp(10ns));
  EXPECT_DOUBLE_EQ(trail[0].pos.x, 1.0);
  EXPECT_DOUBLE_EQ(trail[2].pos.x, 3.0);
}

TEST(BuildTfFrameTrail, StaticChildInheritsParentMotion) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(stampedAt(10ns, "odom", "base", 1.0));
  (void)buffer.setTransform(stampedAt(20ns, "odom", "base", 2.0));
  (void)buffer.setTransform(stampedAt(5ns, "base", "lidar", 0.5));

  const TrailPolyline trail = buildTfFrameTrail(buffer, "odom", "lidar", TimePoint::min(), TimePoint::max(), 1000);
  // Stamp 5 is unresolvable (odom->base has no sample yet) -> skipped (gap rule).
  ASSERT_EQ(trail.size(), 2U);
  EXPECT_DOUBLE_EQ(trail[0].pos.x, 1.5);
  EXPECT_DOUBLE_EQ(trail[1].pos.x, 2.5);
}

TEST(BuildTfFrameTrail, DisconnectedIsEmpty) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(stampedAt(10ns, "odom", "base", 1.0));
  EXPECT_TRUE(buildTfFrameTrail(buffer, "map", "base", TimePoint::min(), TimePoint::max(), 1000).empty());
}

TEST(BuildTfFrameTrail, CapDecimatesButKeepsEndpoints) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  for (int64_t i = 0; i < 500; ++i) {
    (void)buffer.setTransform(stampedAt(std::chrono::nanoseconds(i * 10), "odom", "base", static_cast<double>(i)));
  }
  const TrailPolyline trail = buildTfFrameTrail(buffer, "odom", "base", TimePoint::min(), TimePoint::max(), 100);
  ASSERT_EQ(trail.size(), 100U);
  EXPECT_EQ(trail.front().t, tp(0ns));
  EXPECT_EQ(trail.back().t, tp(4990ns));
}

TEST(BuildPoseTopicTrail, TransformsEachSampleAtItsOwnStamp) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);
  (void)buffer.setTransform(stampedAt(10ns, "map", "odom", 100.0));
  (void)buffer.setTransform(stampedAt(20ns, "map", "odom", 200.0));

  std::vector<PoseTrailSample> samples;
  samples.push_back(PoseTrailSample{tp(10ns), glm::dvec3(1.0, 0.0, 0.0), "odom"});
  samples.push_back(PoseTrailSample{tp(20ns), glm::dvec3(1.0, 0.0, 0.0), "odom"});
  samples.push_back(PoseTrailSample{tp(20ns), glm::dvec3(0.0, 0.0, 0.0), "ghost_frame"});  // skipped

  const TrailPolyline trail = buildPoseTopicTrail(buffer, "map", samples);
  ASSERT_EQ(trail.size(), 2U);
  EXPECT_DOUBLE_EQ(trail[0].pos.x, 101.0);
  EXPECT_DOUBLE_EQ(trail[1].pos.x, 201.0);
}

TEST(BuildPoseTopicTrail, IdentityWhenAlreadyInFixedFrame) {
  TransformBuffer buffer(TransformBuffer::kKeepAll);  // empty: no TF needed for same-frame poses
  std::vector<PoseTrailSample> samples;
  samples.push_back(PoseTrailSample{tp(10ns), glm::dvec3(7.0, 0.0, 0.0), "map"});
  const TrailPolyline trail = buildPoseTopicTrail(buffer, "map", samples);
  ASSERT_EQ(trail.size(), 1U);
  EXPECT_DOUBLE_EQ(trail[0].pos.x, 7.0);
}

}  // namespace
}  // namespace pj::scene3d
