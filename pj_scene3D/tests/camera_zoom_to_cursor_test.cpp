// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Cursor-anchored zoom must keep the world point under the cursor pixel-locked: pick a pixel, unproject it to the
// ground point P, zoomToCursor at that pixel, re-project P and assert its NDC is unchanged (within 1e-3); no NaN near
// the nadir singularity.

#include <gtest/gtest.h>

#include <cmath>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/camera/camera_math.h"

using pj::scene3d::CameraState;
using pj::scene3d::OrbitCamera;
using pj::scene3d::Ray;
using pj::scene3d::rayPlane;
using pj::scene3d::TopDownOrthoCamera;
using pj::scene3d::unprojectRay;
using pj::scene3d::XYOrbitCamera;

namespace {
constexpr int kW = 800;
constexpr int kH = 600;
constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

glm::vec2 pixelToNdc(glm::vec2 px) {
  return glm::vec2{2.0f * px.x / static_cast<float>(kW) - 1.0f, 1.0f - 2.0f * px.y / static_cast<float>(kH)};
}

glm::vec2 projectToNdc(const glm::vec3& p, const glm::mat4& view, const glm::mat4& proj) {
  const glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
  return glm::vec2{clip.x / clip.w, clip.y / clip.w};
}

bool groundPointUnderPixel(const OrbitCamera& cam, glm::vec2 pixel, glm::vec3& out) {
  const glm::mat4 view = cam.viewMatrix();
  const glm::mat4 proj = cam.projMatrix(kAspect);
  const Ray ray = unprojectRay(pixel, kW, kH, view, proj);
  float t = 0.0f;
  if (!rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t) || t <= 0.0f) {
    return false;
  }
  out = ray.origin + t * ray.dir;
  return true;
}

OrbitCamera makeOffAxisCamera() {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f, 0.0f, 0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.6f;
  s.elevation = 0.5f;  // eye above ground, looking down toward origin
  cam.adoptState(s);
  return cam;
}
}  // namespace

TEST(ZoomToCursor, CursorPointStaysPixelLockedZoomingIn) {
  for (const glm::vec2 pixel : {glm::vec2{520.0f, 300.0f}, glm::vec2{300.0f, 420.0f}, glm::vec2{640.0f, 180.0f}}) {
    OrbitCamera cam = makeOffAxisCamera();
    glm::vec3 p{};
    ASSERT_TRUE(groundPointUnderPixel(cam, pixel, p)) << "pixel sees the ground";
    const glm::vec2 ndc_before = pixelToNdc(pixel);

    cam.zoomToCursor(2.0f, pixel, kW, kH);  // zoom in two ticks

    const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
    EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f) << "pixel=(" << pixel.x << "," << pixel.y << ")";
    EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f) << "pixel=(" << pixel.x << "," << pixel.y << ")";
  }
}

TEST(ZoomToCursor, CursorPointStaysPixelLockedZoomingOut) {
  const glm::vec2 pixel{560.0f, 260.0f};
  OrbitCamera cam = makeOffAxisCamera();
  glm::vec3 p{};
  ASSERT_TRUE(groundPointUnderPixel(cam, pixel, p));
  const glm::vec2 ndc_before = pixelToNdc(pixel);

  cam.zoomToCursor(-3.0f, pixel, kW, kH);  // zoom out three ticks

  const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
  EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f);
  EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f);
}

TEST(ZoomToCursor, ZoomingInActuallyShrinksRadius) {
  const glm::vec2 pixel{520.0f, 300.0f};
  OrbitCamera cam = makeOffAxisCamera();
  const float before = cam.state().radius;
  cam.zoomToCursor(1.0f, pixel, kW, kH);
  EXPECT_LT(cam.state().radius, before);  // it's a real zoom, not just a pan
}

TEST(ZoomToCursor, NoNaNNearNadir) {
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.0f;
  s.elevation = glm::radians(89.0f);  // looking almost straight down
  cam.adoptState(s);

  cam.zoomToCursor(1.0f, glm::vec2{400.0f, 300.0f}, kW, kH);  // center-ish, near nadir

  const CameraState r = cam.state();
  EXPECT_TRUE(std::isfinite(r.focal.x) && std::isfinite(r.focal.y) && std::isfinite(r.focal.z));
  EXPECT_TRUE(std::isfinite(r.radius));
  EXPECT_TRUE(std::isfinite(r.azimuth));
  EXPECT_TRUE(std::isfinite(r.elevation));
  EXPECT_GT(r.radius, 0.0f);
}

TEST(ZoomToCursorOrtho, CursorPointStaysPixelLocked) {
  // The ortho top-down branch keeps the ground point under the cursor fixed by
  // shifting the focal by the unproject delta. Exact and singularity-free.
  TopDownOrthoCamera cam;
  CameraState s = cam.state();
  s.focal = glm::vec3{2.0f, -1.0f, 0.0f};
  s.ortho_scale = 10.0f;
  s.azimuth = 0.3f;
  s.perspective = false;
  cam.adoptState(s);

  const glm::vec2 pixel{560.0f, 260.0f};
  const Ray ray = unprojectRay(pixel, kW, kH, cam.viewMatrix(), cam.projMatrix(kAspect));
  float t = 0.0f;
  ASSERT_TRUE(rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t));
  const glm::vec3 p = ray.origin + t * ray.dir;
  const glm::vec2 ndc_before = pixelToNdc(pixel);

  cam.zoomToCursor(2.0f, pixel, kW, kH);

  const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
  EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f);
  EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f);
}

TEST(ZoomToCursor, UpwardSkyPixelStaysFinite) {
  // A pixel toward the sky may miss the ground plane; the focal-plane fallback (or centre zoom) keeps the result finite
  // and shrinking, never NaN or runaway.
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.0f;
  s.elevation = -0.5f;  // eye below the focal, looking up
  cam.adoptState(s);
  const float before = cam.state().radius;

  cam.zoomToCursor(1.0f, glm::vec2{400.0f, 50.0f}, kW, kH);  // a pixel toward the sky

  EXPECT_TRUE(std::isfinite(cam.state().radius));
  EXPECT_LE(cam.state().radius, before);  // a zoom-in shrinks (or holds) the radius, no blow-up
}

TEST(ZoomToCursor, HorizonGrazeDoesNotTeleport) {
  // Low elevation plus a cursor just below the horizon makes the z=0 ground ray graze the plane (t in the hundreds of
  // metres); the grazing hit must be rejected in favour of the focal-plane anchor, keeping the move bounded.
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{0.0f};
  s.radius = 10.0f;
  s.azimuth = 0.0f;
  s.elevation = glm::radians(3.0f);  // nearly horizontal — grazing geometry
  cam.adoptState(s);

  const glm::vec3 before = cam.position();
  // A pixel just above the screen centre → a ray grazing the ground ~48 m out,
  // far beyond max(sceneReach, 4*radius)=40 m: the ground hit must be rejected.
  cam.zoomToCursor(1.0f, glm::vec2{400.0f, 270.0f}, kW, kH);

  const float moved = glm::length(cam.position() - before);
  EXPECT_LT(moved, 0.5f * s.radius) << "grazing-horizon zoom teleported the camera";
  EXPECT_TRUE(std::isfinite(cam.position().x) && std::isfinite(cam.position().y));
}

TEST(ZoomToCursor, PreservesOrientationAtLargeCoordinates) {
  // "Follow a frame" parks the pivot at the followed frame's world origin; far away (UTM/GPS scale, ~1e5..1e7 m)
  // float32 resolves only ~0.1..0.6 m there. A homothety about the cursor cannot rotate the orbit, yet re-deriving
  // azimuth/elevation by differencing two ~1e6 eye/focal vectors injects rounding noise that spins the view (about 1.9°
  // per tick at focal 5e6). Pin orientation across a zoom at a far pivot: the angles hold and it is still a real
  // zoom-in.
  for (const float far_origin : {1.0e5f, 1.0e6f, 5.0e6f}) {
    OrbitCamera cam;
    CameraState s;
    s.focal = glm::vec3{far_origin, far_origin * 0.6f, 0.0f};
    s.radius = 8.0f;
    s.azimuth = 0.6f;
    s.elevation = 0.5f;
    cam.adoptState(s);
    const float az0 = cam.state().azimuth;
    const float el0 = cam.state().elevation;

    cam.zoomToCursor(1.0f, glm::vec2{520.0f, 300.0f}, kW, kH);

    EXPECT_NEAR(cam.state().azimuth, az0, 1e-4f) << "spurious yaw at focal " << far_origin;
    EXPECT_NEAR(cam.state().elevation, el0, 1e-4f) << "spurious pitch at focal " << far_origin;
    EXPECT_LT(cam.state().radius, s.radius) << "still a real zoom-in at focal " << far_origin;
  }
}

TEST(ZoomToCursor, CursorStaysPixelLockedWhenRadiusClamps) {
  // A very deep zoom-in drives s*radius below the radius floor (lo), so the radius clamps. The pivot slide must use
  // the EFFECTIVE (post-clamp) factor, else the point under the cursor slips off the pointer exactly when the clamp
  // bites.
  const glm::vec2 pixel{520.0f, 300.0f};
  OrbitCamera cam = makeOffAxisCamera();  // focal 0, radius 10, off-axis; no scene bounds
  glm::vec3 p{};
  ASSERT_TRUE(groundPointUnderPixel(cam, pixel, p));
  const glm::vec2 ndc_before = pixelToNdc(pixel);

  cam.zoomToCursor(60.0f, pixel, kW, kH);  // 60 ticks: s*10 ≈ 0.018 < lo, so radius clamps

  // The clamp must actually engage: lo = 2*max(r*1e-2,1e-3) = 0.2 for r=10, far above the unclamped 0.018.
  ASSERT_NEAR(cam.state().radius, 0.2f, 1e-3f) << "radius clamp did not engage";
  const glm::vec2 ndc_after = projectToNdc(p, cam.viewMatrix(), cam.projMatrix(kAspect));
  EXPECT_NEAR(ndc_after.x, ndc_before.x, 1e-3f) << "cursor point slipped when radius clamped";
  EXPECT_NEAR(ndc_after.y, ndc_before.y, 1e-3f) << "cursor point slipped when radius clamped";
}

TEST(ZoomToCursor, XYOrbitGroundLockPreservesOrientationAtLargeCoordinates) {
  // The ground-locked XYOrbit re-flattens its pivot to z=0 after the inherited zoom; re-deriving the angles from
  // (eye − focal) there (two ~1e6 vectors when following a far frame) would reintroduce the spin. Drive a cursor that
  // misses the ground (so the focal-plane fallback lifts focal.z and the flatten fires) at UTM-scale coordinates, and
  // assert heading/pitch hold and the pivot returns exactly to z=0.
  for (const float far_origin : {1.0e5f, 1.0e6f, 5.0e6f}) {
    XYOrbitCamera cam;
    CameraState s;
    s.focal = glm::vec3{far_origin, far_origin * 0.6f, 0.0f};
    s.radius = 8.0f;
    s.azimuth = 0.4f;
    s.elevation = glm::radians(20.0f);  // looking down at the floor
    cam.adoptState(s);
    const float az0 = cam.state().azimuth;
    const float el0 = cam.state().elevation;

    // A pixel near the top aims above the horizon: the z=0 ground ray misses, so zoomToCursor falls back to the
    // focal-plane anchor (target.z != 0).
    cam.zoomToCursor(1.0f, glm::vec2{400.0f, 15.0f}, kW, kH);

    EXPECT_NEAR(cam.state().azimuth, az0, 1e-4f) << "XYOrbit heading spun at focal " << far_origin;
    EXPECT_NEAR(cam.state().elevation, el0, 1e-4f) << "XYOrbit pitch drifted at focal " << far_origin;
    EXPECT_NEAR(cam.state().focal.z, 0.0f, 1e-3f) << "XYOrbit pivot left the floor at focal " << far_origin;
  }
}

TEST(CameraRelativeView, EqualsAbsoluteViewAtZeroOrigin) {
  // viewMatrixRelativeTo({0,0,0}) must equal the absolute viewMatrix(), so small-coordinate scenes render bit-for-bit
  // unchanged.
  OrbitCamera cam;
  CameraState s;
  s.focal = glm::vec3{1.0f, 2.0f, 3.0f};
  s.radius = 7.0f;
  s.azimuth = 0.3f;
  s.elevation = 0.4f;
  cam.adoptState(s);
  const glm::mat4 absolute = cam.viewMatrix();
  const glm::mat4 relative = cam.viewMatrixRelativeTo(glm::dvec3{0.0});
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      EXPECT_NEAR(absolute[col][row], relative[col][row], 1e-5f) << "col=" << col << " row=" << row;
    }
  }
}

TEST(CameraRelativeView, FocalStaysPixelLockedZoomingAtLargeCoordinates) {
  // A camera following a far frame sits at ~1e6, where the absolute viewMatrix() loses the eye→geometry delta to
  // float32 cancellation and the scene SWIMS even during a pure radius change (the look-at point drifts 8..26 px).
  // Rendering relative to the focal restores an exact pixel lock at any scale: project the focal (origin-relative,
  // ~0) before and after a pure zoom; it must not move.
  for (const float far_origin : {1.0e5f, 1.0e6f, 5.0e6f}) {
    OrbitCamera cam;
    CameraState s;
    s.focal = glm::vec3{far_origin, far_origin * 0.6f, 3.0f};
    s.radius = 8.0f;
    s.azimuth = 0.6f;
    s.elevation = 0.5f;
    cam.adoptState(s);

    const glm::dvec3 origin = glm::dvec3(cam.state().focal);
    const glm::vec3 focal_rel = glm::vec3(glm::dvec3(cam.state().focal) - origin);  // ~0 by construction
    const glm::vec2 before = projectToNdc(focal_rel, cam.viewMatrixRelativeTo(origin), cam.projMatrix(kAspect));

    cam.zoom(4.0f);  // pure radius change; zoom() leaves the focal untouched

    const glm::vec3 focal_rel2 = glm::vec3(glm::dvec3(cam.state().focal) - origin);
    const glm::vec2 after = projectToNdc(focal_rel2, cam.viewMatrixRelativeTo(origin), cam.projMatrix(kAspect));

    EXPECT_NEAR(after.x, before.x, 1e-4f) << "scene swam horizontally at focal " << far_origin;
    EXPECT_NEAR(after.y, before.y, 1e-4f) << "scene swam vertically at focal " << far_origin;
  }
}
