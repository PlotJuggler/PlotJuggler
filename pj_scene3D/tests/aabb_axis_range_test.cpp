// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// transformedAabbAxisRange(): the auto-range helper behind FIXED-FRAME (x/y/z) point-cloud colouring. The renderer
// colours such clouds in the destination frame on the GPU, so the colormap bounds must be measured there too, not in
// the sensor-local source frame.

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "pj_scene3d_core/camera/camera.h"

namespace pj::scene3d {
namespace {

AABB makeBox(glm::vec3 lo, glm::vec3 hi) {
  AABB box;
  box.min = lo;
  box.max = hi;
  box.valid = true;
  return box;
}

constexpr int kAxisX = 0;
constexpr int kAxisY = 1;
constexpr int kAxisZ = 2;

TEST(TransformedAabbAxisRange, IdentityReturnsSourceAxisRange) {
  const AABB box = makeBox({-1.0f, -2.0f, 3.0f}, {4.0f, 5.0f, 6.0f});
  const auto [lo, hi] = transformedAabbAxisRange(box, glm::mat4(1.0f), kAxisZ);
  EXPECT_FLOAT_EQ(lo, 3.0f);
  EXPECT_FLOAT_EQ(hi, 6.0f);
}

// A sensor mounted +10 m up in the fixed frame must colour by the FIXED-frame height: its z range shifts by the mount
// height instead of staying the raw sensor-local z (two lidars at the same world height get the same colours).
TEST(TransformedAabbAxisRange, TranslationShiftsTheAxisRange) {
  const AABB box = makeBox({-1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
  const glm::mat4 lift = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
  const auto [lo, hi] = transformedAabbAxisRange(box, lift, kAxisZ);
  EXPECT_FLOAT_EQ(lo, 10.0f);
  EXPECT_FLOAT_EQ(hi, 11.0f);
}

// A 90° yaw about z leaves the z range untouched but maps the source y extent onto
// the destination x axis — confirms the axis is sampled AFTER the transform.
TEST(TransformedAabbAxisRange, YawMapsSourceYontoDestinationX) {
  const AABB box = makeBox({-1.0f, -3.0f, 0.0f}, {1.0f, 3.0f, 2.0f});
  const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));

  const auto [zlo, zhi] = transformedAabbAxisRange(box, yaw, kAxisZ);
  EXPECT_FLOAT_EQ(zlo, 0.0f);
  EXPECT_FLOAT_EQ(zhi, 2.0f);

  const auto [xlo, xhi] = transformedAabbAxisRange(box, yaw, kAxisX);
  EXPECT_NEAR(xlo, -3.0f, 1e-5f);
  EXPECT_NEAR(xhi, 3.0f, 1e-5f);
}

// An invalid (unreported) box yields a safe unit range, never a degenerate span.
TEST(TransformedAabbAxisRange, InvalidBoxYieldsUnitRange) {
  const auto [lo, hi] = transformedAabbAxisRange(AABB{}, glm::mat4(1.0f), kAxisY);
  EXPECT_FLOAT_EQ(lo, 0.0f);
  EXPECT_FLOAT_EQ(hi, 1.0f);
}

}  // namespace
}  // namespace pj::scene3d
