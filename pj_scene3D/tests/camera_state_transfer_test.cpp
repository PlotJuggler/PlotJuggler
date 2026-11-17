// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Per-model reset defaults and state round-trips across model switches.

#include <gtest/gtest.h>

#include <cmath>

#include "pj_scene3d_core/camera/camera.h"

using pj::scene3d::CameraState;
using pj::scene3d::cameraStateFromJson;
using pj::scene3d::cameraStateToJson;
using pj::scene3d::FlyCamera;
using pj::scene3d::OrbitCamera;
using pj::scene3d::TopDownOrthoCamera;
using pj::scene3d::XYOrbitCamera;

TEST(CameraReset, OrbitDefaults) {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{9.0f, -4.0f, 2.0f};
  s.radius = 123.0f;
  s.azimuth = 1.2f;
  cam.adoptState(s);
  cam.reset();

  const CameraState d = cam.state();
  EXPECT_EQ(d.focal, glm::vec3{0.0f});
  EXPECT_FLOAT_EQ(d.radius, 5.0f);
  EXPECT_FLOAT_EQ(d.azimuth, 0.0f);
  EXPECT_FLOAT_EQ(d.elevation, glm::radians(30.0f));
  EXPECT_TRUE(d.perspective);
}

TEST(CameraReset, TopDownOrthoDefaults) {
  TopDownOrthoCamera cam;
  cam.rotate(300.0f, 0.0f);  // change heading
  cam.pan(50.0f, 50.0f);     // move focal
  cam.zoom(3.0f);            // change scale
  cam.reset();

  const CameraState d = cam.state();
  EXPECT_EQ(d.focal, glm::vec3{0.0f});
  EXPECT_FLOAT_EQ(d.azimuth, 0.0f);
  EXPECT_FALSE(d.perspective);
  EXPECT_GT(d.ortho_scale, 0.0f);
}

TEST(CameraReset, FlyDefaultsLookAtOrigin) {
  FlyCamera cam;
  cam.rotate(200.0f, 100.0f);
  cam.pan(40.0f, 40.0f);
  cam.reset();

  EXPECT_FLOAT_EQ(cam.position().x, 5.0f);
  EXPECT_FLOAT_EQ(cam.position().y, 5.0f);
  EXPECT_FLOAT_EQ(cam.position().z, 10.0f);
  const glm::vec3 to_focal = glm::normalize(cam.state().focal - cam.position());
  const glm::vec3 to_origin = glm::normalize(glm::vec3{0.0f} - cam.position());
  EXPECT_NEAR(glm::dot(to_focal, to_origin), 1.0f, 1e-4f);
}

TEST(CameraStateTransfer, OrbitToTopDownToOrbitRoundTrip) {
  CameraState s;
  // focal.z = 0: TopDownOrtho flattens focal.z on adopt (its pivot is ground-level), so a round-trip through it is
  // lossless only for a ground-level focal.
  s.focal = glm::vec3{3.0f, -2.0f, 0.0f};
  s.radius = 42.0f;
  s.azimuth = 0.7f;
  s.elevation = 0.4f;
  s.perspective = true;

  OrbitCamera orbit;
  orbit.adoptState(s);

  TopDownOrthoCamera top_down;
  top_down.adoptState(orbit.state());
  EXPECT_FALSE(top_down.state().perspective);
  EXPECT_FLOAT_EQ(top_down.state().ortho_scale, 42.0f);  // seeded from radius
  EXPECT_NEAR(top_down.state().focal.z, 0.0f, 1e-4f);    // pivot re-locked to the floor

  OrbitCamera orbit2;
  orbit2.adoptState(top_down.state());
  const CameraState r = orbit2.state();
  EXPECT_TRUE(r.perspective);
  EXPECT_NEAR(r.focal.x, s.focal.x, 1e-4f);
  EXPECT_NEAR(r.focal.y, s.focal.y, 1e-4f);
  EXPECT_NEAR(r.focal.z, s.focal.z, 1e-4f);
  EXPECT_NEAR(r.azimuth, s.azimuth, 1e-4f);
  EXPECT_NEAR(r.radius, s.radius, 1e-4f);  // radius → ortho_scale → radius
}

// FlyCamera::state() must export the focal-to-eye direction, which CameraState's azimuth/elevation parameterize
// (OrbitCamera::position() = focal + radius*sphericalDir(az, el)), not its own eye-to-focal forward: otherwise any
// model adopting a Fly state teleports forward and flips 180°.

TEST(CameraStateTransfer, FlyRoundTripPreservesPose) {
  FlyCamera fly;
  fly.rotate(150.0f, -60.0f);  // yaw/pitch away from the default
  fly.pan(40.0f, 25.0f);       // translate the eye

  const glm::vec3 eye_before = fly.position();
  const glm::vec3 fwd_before = glm::normalize(fly.state().focal - eye_before);

  FlyCamera fly2;
  fly2.adoptState(fly.state());

  EXPECT_NEAR(fly2.position().x, eye_before.x, 1e-3f);
  EXPECT_NEAR(fly2.position().y, eye_before.y, 1e-3f);
  EXPECT_NEAR(fly2.position().z, eye_before.z, 1e-3f);
  const glm::vec3 fwd_after = glm::normalize(fly2.state().focal - fly2.position());
  EXPECT_NEAR(glm::dot(fwd_before, fwd_after), 1.0f, 1e-3f);
}

TEST(CameraStateTransfer, FlyToOrbitContinuity) {
  FlyCamera fly;
  fly.rotate(220.0f, -40.0f);  // yaw right + pitch up
  fly.pan(-30.0f, 50.0f);

  const glm::vec3 fly_eye = fly.position();
  const glm::vec3 fly_forward = glm::normalize(fly.state().focal - fly_eye);

  OrbitCamera orbit;
  orbit.adoptState(fly.state());

  // The orbit eye must land on the fly eye, and the orbit's look direction
  // (focal - eye) must match the fly's forward — no teleport, no 180° flip.
  EXPECT_NEAR(orbit.position().x, fly_eye.x, 1e-3f);
  EXPECT_NEAR(orbit.position().y, fly_eye.y, 1e-3f);
  EXPECT_NEAR(orbit.position().z, fly_eye.z, 1e-3f);
  const glm::vec3 orbit_forward = glm::normalize(orbit.state().focal - orbit.position());
  EXPECT_NEAR(glm::dot(orbit_forward, fly_forward), 1.0f, 1e-3f);
}

TEST(CameraStateTransfer, FlyPreservesFovY) {
  // A custom fov_y set on an orbit view must survive a switch into Fly and back (Fly stores and emits fov_y, and
  // projMatrix uses it).
  OrbitCamera orbit;
  CameraState s;
  s.fov_y = glm::radians(60.0f);
  orbit.adoptState(s);

  FlyCamera fly;
  fly.adoptState(orbit.state());
  EXPECT_NEAR(fly.state().fov_y, glm::radians(60.0f), 1e-4f);

  const glm::mat4 proj = fly.projMatrix(1.0f);
  EXPECT_NEAR(proj[1][1], 1.0f / std::tan(glm::radians(30.0f)), 1e-3f);
}

TEST(CameraStateTransfer, XYOrbitAdoptFlattensFocal) {
  // Adopting an elevated-pivot perspective state must re-lock the pivot to the
  // floor (z=0) while keeping the eye fixed — the XYOrbit floor invariant.
  CameraState s;
  s.focal = glm::vec3{2.0f, -3.0f, 4.0f};  // pivot off the floor
  s.radius = 12.0f;
  s.azimuth = 0.5f;
  s.elevation = 0.6f;
  s.perspective = true;

  OrbitCamera reference;  // same state through a plain orbit, to read the eye
  reference.adoptState(s);
  const glm::vec3 eye_before = reference.position();

  XYOrbitCamera xy;
  xy.adoptState(s);
  EXPECT_NEAR(xy.state().focal.z, 0.0f, 1e-4f);
  EXPECT_NEAR(xy.position().x, eye_before.x, 1e-3f);
  EXPECT_NEAR(xy.position().y, eye_before.y, 1e-3f);
  EXPECT_NEAR(xy.position().z, eye_before.z, 1e-3f);
}

TEST(CameraStateTransfer, TopDownAdoptFlattensFocalZ) {
  // Adopting a perspective state with an elevated focal must reset TopDownOrtho's focal.z to 0, else its far plane
  // (derived around the focal) clips the whole ground scene to blank.
  CameraState s;
  s.focal = glm::vec3{1.0f, 2.0f, 50.0f};
  s.radius = 5.0f;
  s.perspective = true;

  TopDownOrthoCamera top_down;
  top_down.adoptState(s);
  EXPECT_NEAR(top_down.state().focal.z, 0.0f, 1e-4f);
}

TEST(CameraStateSerialize, JsonRoundTrip) {
  CameraState s;
  s.focal = glm::vec3{3.5f, -2.25f, 1.0f};
  s.radius = 42.0f;
  s.azimuth = 0.7f;
  s.elevation = 0.4f;
  s.fov_y = glm::radians(50.0f);
  s.ortho_scale = 12.5f;
  s.perspective = false;

  const CameraState r = cameraStateFromJson(cameraStateToJson(s));
  EXPECT_NEAR(r.focal.x, s.focal.x, 1e-5f);
  EXPECT_NEAR(r.focal.y, s.focal.y, 1e-5f);
  EXPECT_NEAR(r.focal.z, s.focal.z, 1e-5f);
  EXPECT_NEAR(r.radius, s.radius, 1e-5f);
  EXPECT_NEAR(r.azimuth, s.azimuth, 1e-5f);
  EXPECT_NEAR(r.elevation, s.elevation, 1e-5f);
  EXPECT_NEAR(r.fov_y, s.fov_y, 1e-5f);
  EXPECT_NEAR(r.ortho_scale, s.ortho_scale, 1e-5f);
  EXPECT_EQ(r.perspective, s.perspective);
}

TEST(CameraStateSerialize, MissingFieldsTakeFallback) {
  CameraState fallback;
  fallback.radius = 99.0f;
  fallback.azimuth = 0.0f;
  const CameraState r = cameraStateFromJson(R"({"azimuth": 1.5})", fallback);
  EXPECT_FLOAT_EQ(r.azimuth, 1.5f);  // present → taken
  EXPECT_FLOAT_EQ(r.radius, 99.0f);  // missing → fallback
}

TEST(CameraStateSerialize, MalformedReturnsFallback) {
  CameraState fallback;
  fallback.radius = 7.0f;
  EXPECT_FLOAT_EQ(cameraStateFromJson("not json at all", fallback).radius, 7.0f);
  EXPECT_FLOAT_EQ(cameraStateFromJson("", fallback).radius, 7.0f);
}

TEST(CameraStateSerialize, WrongTypedFieldReturnsFallback) {
  CameraState fallback;
  fallback.radius = 7.0f;
  // A present-but-wrong-typed field makes nlohmann's value() throw json::type_error;
  // the loader must catch it and fall back rather than crash layout restore.
  CameraState recovered = fallback;
  EXPECT_NO_THROW(recovered = cameraStateFromJson(R"({"radius": "fast"})", fallback));
  EXPECT_FLOAT_EQ(recovered.radius, 7.0f);
}

TEST(CameraStateSerialize, OutOfRangeValuesAreSanitized) {
  // A corrupt/hand-edited layout with non-positive or non-finite fields must never
  // reach glm::perspective/lookAt as-is; the loader clamps them to a usable range.
  const CameraState recovered = cameraStateFromJson(R"({"radius": 0, "ortho_scale": -5, "fov_y": 0})");
  EXPECT_GT(recovered.radius, 0.0f);
  EXPECT_GT(recovered.ortho_scale, 0.0f);
  EXPECT_GT(recovered.fov_y, 0.0f);
}
