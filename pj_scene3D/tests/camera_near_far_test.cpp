// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Decoupled adaptive near/far, geometric zoom and the lifted radius clamp.

#include <gtest/gtest.h>

#include <cmath>
#include <glm/gtc/constants.hpp>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/camera/camera_math.h"

using pj::scene3d::AABB;
using pj::scene3d::adaptiveNearFar;
using pj::scene3d::CameraState;
using pj::scene3d::FlyCamera;
using pj::scene3d::OrbitCamera;
using pj::scene3d::sceneReach;
using pj::scene3d::TopDownOrthoCamera;

namespace {
// Reconstruct (near, far) from a glm::ortho matrix (column-major). glm::ortho
// maps z linearly: m[2][2] = -2/(far-near), m[3][2] = -(far+near)/(far-near).
struct ClipRange {
  float near_plane;
  float far_plane;
};
ClipRange orthoClipRange(const glm::mat4& ortho) {
  const float two_over_span = -ortho[2][2];  // = 2/(far-near)
  const float sum_over_span = -ortho[3][2];  // = (far+near)/(far-near)
  const float span = 2.0f / two_over_span;   // far - near
  const float sum = sum_over_span * span;    // far + near
  return ClipRange{0.5f * (sum - span), 0.5f * (sum + span)};
}
}  // namespace

namespace {
// A scene a few thousand units across.
constexpr float kSceneHalf = 2500.0f;
const AABB kScene{
    glm::vec3{-kSceneHalf, -kSceneHalf, -kSceneHalf}, glm::vec3{kSceneHalf, kSceneHalf, kSceneHalf}, true};
}  // namespace

TEST(AdaptiveNearFar, InvariantsAcrossRadii) {
  const float reach = sceneReach(kScene, glm::vec3{0.0f});
  ASSERT_GT(reach, 0.0f);

  for (const float working_distance : {0.5f, 2.0f, 50.0f, 5000.0f, 1.0e6f}) {
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    adaptiveNearFar(working_distance, reach, near_plane, far_plane);

    EXPECT_GT(near_plane, 0.0f) << "wd=" << working_distance;
    // The eye-to-focal distance for an orbit camera IS the working distance, so
    // near strictly inside it means close inspection never clips.
    EXPECT_LT(near_plane, working_distance) << "wd=" << working_distance;
    // Far reaches the whole scene.
    EXPECT_GE(far_plane, reach) << "wd=" << working_distance;
    // Depth precision stays bounded.
    EXPECT_LE(far_plane / near_plane, 1.0e5f + 1.0f) << "wd=" << working_distance;
  }
}

TEST(AdaptiveNearFar, UnknownBoundsFallsBackToWorkingDistance) {
  // reach == 0 (no scene bounds): far is driven purely by working distance.
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  adaptiveNearFar(50.0f, 0.0f, near_plane, far_plane);
  EXPECT_FLOAT_EQ(far_plane, 50.0f * 4.0f * 1.5f);  // max(200, 0) * 1.5
  EXPECT_LT(near_plane, 50.0f);
}

TEST(SceneReach, OffsetFocalIncreasesReach) {
  const float centered = sceneReach(kScene, glm::vec3{0.0f});
  const float offset = sceneReach(kScene, glm::vec3{kSceneHalf, kSceneHalf, kSceneHalf});
  EXPECT_GT(offset, centered);                                 // pivot off to one side extends the reach
  EXPECT_FLOAT_EQ(sceneReach(AABB{}, glm::vec3{0.0f}), 0.0f);  // invalid → 0
}

TEST(OrbitZoom, GeometricStep) {
  OrbitCamera cam;  // no scene bounds
  CameraState s;
  s.radius = 100.0f;
  cam.adoptState(s);

  cam.zoom(1.0f);  // one tick in
  EXPECT_NEAR(cam.state().radius, 90.0f, 1e-3f);
  cam.zoom(-1.0f);  // one tick back out
  EXPECT_NEAR(cam.state().radius, 100.0f, 1e-2f);
}

TEST(OrbitZoom, OldThousandUnitClampIsGone) {
  OrbitCamera cam;
  CameraState s;
  s.radius = 5000.0f;  // no radius cap
  cam.adoptState(s);
  cam.zoom(1.0f);
  EXPECT_NEAR(cam.state().radius, 4500.0f, 1.0f);
  EXPECT_GT(cam.state().radius, 1000.0f);
}

TEST(OrbitZoom, LargeSceneIsFramable) {
  OrbitCamera cam;
  cam.setSceneBounds(kScene);
  CameraState s;
  s.radius = 2.0f;  // start zoomed in
  cam.adoptState(s);

  const float scene_diag = glm::length(kScene.max - kScene.min);
  for (int i = 0; i < 200; ++i) {
    cam.zoom(-1.0f);  // keep zooming out
  }
  EXPECT_GT(cam.state().radius, scene_diag);
}

// TopDownOrtho clip range must contain above-ground geometry even at deep zoom: the eye lifts with the scene's max.z so
// the near plane never clips tall geometry, and the far plane reaches the scene floor.

TEST(TopDownOrthoClip, ContainsAboveGroundGeometryWhenZoomedIn) {
  // A 1.5 m-tall object on a flat map, viewed at a tight 0.5 m half-height zoom.
  const AABB scene{glm::vec3{-5.0f, -5.0f, 0.0f}, glm::vec3{5.0f, 5.0f, 1.5f}, true};
  TopDownOrthoCamera cam;
  cam.setSceneBounds(scene);
  CameraState s = cam.state();
  s.ortho_scale = 0.5f;  // zoomed well below the geometry height
  s.focal = glm::vec3{0.0f};
  s.perspective = false;
  cam.adoptState(s);

  const float eye_z = cam.position().z;
  const ClipRange clip = orthoClipRange(cam.projMatrix(1.0f));
  // glm::lookAt down -Z maps world depth (eye_z - world_z) onto the clip range.
  // A world z is visible iff near <= (eye_z - z) <= far.
  for (const float world_z : {0.0f, 0.75f, 1.5f}) {
    const float depth = eye_z - world_z;
    EXPECT_GE(depth, clip.near_plane) << "world_z=" << world_z << " clipped by near";
    EXPECT_LE(depth, clip.far_plane) << "world_z=" << world_z << " clipped by far";
  }
}

// Fly near plane must stay small near the scene periphery: flying close to a wall at the edge of a large scene must
// not near-clip it.

TEST(FlyNearPlane, StaysSmallAtScenePeriphery) {
  // A 100 m-scale scene centred at the origin; eye 90 m out near one edge.
  const AABB scene{glm::vec3{-50.0f, -50.0f, -50.0f}, glm::vec3{50.0f, 50.0f, 50.0f}, true};
  FlyCamera cam;
  cam.setSceneBounds(scene);
  CameraState s = cam.state();
  s.focal = glm::vec3{91.0f, 0.0f, 0.0f};  // eye ~90 m from centre via adoptState
  s.radius = 1.0f;
  s.azimuth = glm::radians(180.0f);  // look back toward the scene
  s.elevation = 0.0f;
  s.perspective = true;
  cam.adoptState(s);
  ASSERT_GT(glm::length(cam.position()), 80.0f);  // genuinely at the periphery

  const glm::mat4 proj = cam.projMatrix(1.0f);
  const float near_plane = proj[3][2] / (proj[2][2] - 1.0f);
  EXPECT_LE(near_plane, 0.05f + 1e-3f) << "near plane near-clips peripheral geometry";
}

// Fly motion steps must not collapse near the AABB center: the motion basis is floored at kFlyNominalDistance so
// zoom/pan never freeze.

TEST(FlyMotion, ZoomMovesEvenAtSceneCenter) {
  const AABB scene{glm::vec3{-50.0f, -50.0f, -50.0f}, glm::vec3{50.0f, 50.0f, 50.0f}, true};
  FlyCamera cam;
  cam.setSceneBounds(scene);
  // Park the eye at the AABB centre, where distance-to-center → 0 would freeze the step: azimuth=pi puts the eye at
  // {0,0,0} looking back toward {5,0,0} (forward = +x).
  CameraState s = cam.state();
  s.focal = glm::vec3{5.0f, 0.0f, 0.0f};
  s.radius = 5.0f;
  s.azimuth = glm::pi<float>();
  s.elevation = 0.0f;
  s.perspective = true;
  cam.adoptState(s);
  ASSERT_LT(glm::length(cam.position()), 1e-3f);  // at the centre

  const glm::vec3 before = cam.position();
  cam.zoom(1.0f);  // one tick forward
  const float moved = glm::length(cam.position() - before);
  // Step basis floored at kFlyNominalDistance (5) * 0.1 per tick = 0.5; allow slack.
  EXPECT_GT(moved, 0.1f * 5.0f * 0.9f);
}
