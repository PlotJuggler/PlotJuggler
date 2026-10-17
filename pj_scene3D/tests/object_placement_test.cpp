// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// TF R6 object placement: an object is placed at its own header stamp in the TF
// header clock; an unstamped object, or a stamp the header clock cannot resolve,
// is placed at the cursor in the entry clock.
#include "pj_scene3d_core/tf/object_placement.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {
namespace {

using pj::tf::StampedTransform;
using namespace std::chrono_literals;

constexpr TimePoint ms(int64_t value) {
  return TimePoint{std::chrono::milliseconds(value)};
}

// map<-lidar stamped 100 and 200 ms (x = 1, 2), logged 40 ms later.
void feedLoggedLate(TransformBuffer& buffer) {
  for (int sample = 1; sample <= 2; ++sample) {
    StampedTransform edge{
        ms(100 * sample), "map", "lidar", Transform{{sample, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
    EXPECT_TRUE(buffer.setTransform(edge, edge.stamp + 40ms).has_value());
  }
}

constexpr TimePoint sec(int64_t value) {
  return TimePoint{std::chrono::seconds(value)};
}

// base_link moves along x in map (x = 10, 20, 30 at 1, 2, 3 s, logged on time); the
// lidar is mounted 1 m ahead of it (a static edge).
void feedMovingBody(TransformBuffer& buffer) {
  for (int second = 1; second <= 3; ++second) {
    StampedTransform edge{
        sec(second), "map", "base_link", Transform{{10.0 * second, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
    EXPECT_TRUE(buffer.setTransform(edge).has_value());
  }
  StampedTransform mount{sec(1), "base_link", "lidar", Transform{{1.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
  EXPECT_TRUE(buffer.setTransform(mount).has_value());
}

std::optional<double> placedX(
    const TransformBuffer& tf, std::optional<TimePoint> object_stamp, TimePoint cursor,
    const std::string& fixed = "map", const std::string& source = "lidar", PlacementHold* hold = nullptr) {
  const auto placed = placeObject(tf, fixed, source, object_stamp, cursor, hold);
  return placed.has_value() ? std::optional<double>(placed->t.x) : std::nullopt;
}

TEST(ObjectPlacement, PlacesAStampedObjectAtItsOwnHeaderStamp) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedLoggedLate(tf);
  // A cloud measured at 195 ms, shown at cursor 245 ms (when the 200 ms TF had arrived).
  EXPECT_EQ(placedX(tf, ms(195), ms(245)), 1.0);
  EXPECT_EQ(placedX(tf, ms(205), ms(205)), 2.0) << "the 200 ms TF, not yet logged at 205 ms";
}

TEST(ObjectPlacement, UnstampedObjectFollowsTheCursor) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedLoggedLate(tf);
  EXPECT_EQ(placedX(tf, std::nullopt, ms(245)), 2.0);
  EXPECT_EQ(placedX(tf, std::nullopt, ms(230)), 1.0);
}

TEST(ObjectPlacement, FallsBackToTheCursorWhenTheStampPredatesTheTree) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedLoggedLate(tf);
  EXPECT_EQ(placedX(tf, ms(50), ms(245)), 2.0);
  EXPECT_EQ(placedX(tf, ms(50), ms(100)), std::nullopt) << "nothing resolves at either time";
}

// A body-fixed view: a map-frame object (a plan, a voxel map) stays put in the world,
// so it moves in the view as the body moves.
TEST(ObjectPlacement, BodyFixedViewKeepsAWorldFrameObjectInTheWorld) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedMovingBody(tf);
  EXPECT_EQ(placedX(tf, sec(1), sec(1), "base_link", "map"), -10.0);
  EXPECT_EQ(placedX(tf, sec(1), sec(3), "base_link", "map"), -30.0) << "the map-frame object rode with the body";
}

// A scan stays where it was captured in the world, seen from where the body is at the cursor.
TEST(ObjectPlacement, BodyFixedViewKeepsAScanWhereItWasCaptured) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedMovingBody(tf);
  EXPECT_EQ(placedX(tf, sec(1), sec(1), "base_link", "lidar"), 1.0);
  EXPECT_EQ(placedX(tf, sec(1), sec(3), "base_link", "lidar"), -19.0) << "captured at x = 11, the body is now at 30";
}

// With the tree's root as the fixed frame, placement is the header-clock lookup at the stamp.
TEST(ObjectPlacement, RootFixedViewIsTheLookupAtTheStamp) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedMovingBody(tf);
  for (const TimePoint cursor : {sec(1), sec(2), sec(3)}) {
    EXPECT_EQ(placedX(tf, sec(1), cursor, "map", "lidar"), 11.0);
  }
  EXPECT_EQ(placedX(tf, std::nullopt, sec(2), "map", "lidar"), 21.0) << "unstamped: the cursor";
}

// An object stamped far past the newest TF on its chain (data in wall time, TF in sim
// time) follows the cursor instead of holding the last pose forever; a stamp a little
// past it (live TF lags the data) still holds it.
TEST(ObjectPlacement, StampFarPastTheTfFollowsTheCursor) {
  TransformBuffer tf(TransformBuffer::kKeepAll);
  feedLoggedLate(tf);
  EXPECT_EQ(placedX(tf, ms(200) + kMaxStampPastTf - 100ms, ms(150)), 2.0);
  EXPECT_EQ(placedX(tf, ms(200) + kMaxStampPastTf + 100ms, ms(150)), 1.0) << "the cursor's sample";
}

// Live streaming: the lidar stopped after its cloud at 1 s, so the object store keeps
// that cloud, while TF (a 10 s window) runs on to 12 s and evicts the sample at 1 s.
// The layer's hold keeps the cloud where it was captured instead of moving it to the cursor.
TEST(ObjectPlacement, HoldKeepsAPlacedObjectWhenItsTfSampleIsEvicted) {
  TransformBuffer tf(std::chrono::seconds(10));
  const auto feed_until = [&tf](int64_t first_ms, int64_t last_ms) {
    for (int64_t time = first_ms; time <= last_ms; time += 500) {
      StampedTransform edge{
          ms(time), "map", "lidar",
          Transform{{static_cast<double>(time) / 1000.0, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
      EXPECT_TRUE(tf.setTransform(edge).has_value());
    }
  };
  feed_until(0, 2000);
  PlacementHold hold;
  EXPECT_EQ(placedX(tf, sec(1), sec(2), "map", "lidar", &hold), 1.0);
  feed_until(2500, 12000);
  ASSERT_FALSE(tf.tryLookupTransform("map", "lidar", sec(1), pj::tf::Clock::kHeader).has_value())
      << "the TF sample at 1 s was evicted";
  EXPECT_EQ(placedX(tf, sec(1), sec(12), "map", "lidar", &hold), 1.0) << "the cloud stays where it was captured";
  EXPECT_EQ(placedX(tf, sec(1), sec(12)), 12.0) << "without a hold: the cursor";
  EXPECT_EQ(placedX(tf, ms(1500), sec(12), "map", "lidar", &hold), 12.0) << "another stamp does not reuse the hold";
}

TEST(ObjectPlacement, ObjectStampFollowsTheMergeShift) {
  EXPECT_EQ(objectStamp(1'000, 0), TimePoint{std::chrono::nanoseconds(1'000)});
  EXPECT_EQ(objectStamp(1'000, 5'000), TimePoint{std::chrono::nanoseconds(6'000)});
  EXPECT_EQ(objectStamp(0, 5'000), std::nullopt) << "an unstamped object stays unstamped";
}

}  // namespace
}  // namespace pj::scene3d
