// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// followShift(world_delta) shifts the camera's anchor by a world-space delta WITHOUT touching orbit angle / zoom /
// orientation. Each model shifts the anchor it owns (Orbit/TopDown: focal; Fly: eye); XYOrbit drops the Z component to
// stay ground-locked.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include "pj_scene3d_core/camera/camera.h"

namespace {
using pj::scene3d::CameraState;
using pj::scene3d::FlyCamera;
using pj::scene3d::OrbitCamera;
using pj::scene3d::TopDownOrthoCamera;
using pj::scene3d::XYOrbitCamera;

constexpr float kEps = 1e-4f;
constexpr glm::vec3 kDelta{1.0f, 2.0f, 3.0f};

TEST(CameraFollow, OrbitShiftsFocalAndEyeRigidly) {
  OrbitCamera cam;
  const CameraState before = cam.state();
  const glm::vec3 eye_before = cam.position();

  cam.followShift(kDelta);

  const CameraState after = cam.state();
  EXPECT_NEAR(after.focal.x - before.focal.x, kDelta.x, kEps);
  EXPECT_NEAR(after.focal.y - before.focal.y, kDelta.y, kEps);
  EXPECT_NEAR(after.focal.z - before.focal.z, kDelta.z, kEps);
  EXPECT_NEAR(after.radius, before.radius, kEps);
  EXPECT_NEAR(after.azimuth, before.azimuth, kEps);
  EXPECT_NEAR(after.elevation, before.elevation, kEps);
  const glm::vec3 eye_after = cam.position();
  EXPECT_NEAR(eye_after.x - eye_before.x, kDelta.x, kEps);
  EXPECT_NEAR(eye_after.y - eye_before.y, kDelta.y, kEps);
  EXPECT_NEAR(eye_after.z - eye_before.z, kDelta.z, kEps);
}

TEST(CameraFollow, XYOrbitDropsZToStayGroundLocked) {
  XYOrbitCamera cam;
  const CameraState before = cam.state();
  cam.followShift(kDelta);
  const CameraState after = cam.state();
  EXPECT_NEAR(after.focal.x - before.focal.x, kDelta.x, kEps);
  EXPECT_NEAR(after.focal.y - before.focal.y, kDelta.y, kEps);
  EXPECT_NEAR(after.focal.z - before.focal.z, 0.0f, kEps) << "ground-locked: vertical component dropped";
}

TEST(CameraFollow, TopDownShiftsFocalByDelta) {
  TopDownOrthoCamera cam;
  const CameraState before = cam.state();
  cam.followShift(kDelta);
  const CameraState after = cam.state();
  EXPECT_NEAR(after.focal.x - before.focal.x, kDelta.x, kEps);
  EXPECT_NEAR(after.focal.y - before.focal.y, kDelta.y, kEps);
  EXPECT_NEAR(after.focal.z - before.focal.z, kDelta.z, kEps);
}

TEST(CameraFollow, FlyShiftsEyeByDelta) {
  FlyCamera cam;
  const glm::vec3 eye_before = cam.position();
  cam.followShift(kDelta);
  const glm::vec3 eye_after = cam.position();
  EXPECT_NEAR(eye_after.x - eye_before.x, kDelta.x, kEps);
  EXPECT_NEAR(eye_after.y - eye_before.y, kDelta.y, kEps);
  EXPECT_NEAR(eye_after.z - eye_before.z, kDelta.z, kEps);
}
}  // namespace
