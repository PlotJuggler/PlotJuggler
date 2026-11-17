// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/camera/camera.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <numbers>

#include "pj_scene3d_core/camera/camera_math.h"

namespace pj::scene3d {

namespace {
// Drag sensitivity: screen pixels per radian of orbit rotation.
constexpr float kPixelsPerRadian = 200.0f;
// World units of focal/eye translation per drag pixel, per unit of working distance.
constexpr float kPanUnitsPerPixel = 0.001f;
// Elevation stays just short of the poles (gimbal singularity).
constexpr float kPolarLimit = std::numbers::pi_v<float> * 0.5f - 0.05f;

// Shared pan kernel: translates `distance * kPanUnitsPerPixel` per pixel along `right`/`up` (view-matrix rows for
// OrbitCamera, forward cross products for FlyCamera).
glm::vec3 panOffset(const glm::vec3& right, const glm::vec3& up, float dx_pixels, float dy_pixels, float distance) {
  return (-dx_pixels * right + dy_pixels * up) * distance * kPanUnitsPerPixel;
}

// Wraps an angle to [-pi, pi], the value nearest 0 (the canonical azimuth range).
float wrapToPi(float angle) {
  return std::remainder(angle, 2.0f * std::numbers::pi_v<float>);
}

// Unit direction from focal toward eye for a given azimuth/elevation (Z-up).
glm::vec3 sphericalDir(float azimuth, float elevation) {
  return glm::vec3{
      std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
}

// Azimuth (yaw about +Z) and pole-clamped elevation of a unit direction: the inverse of sphericalDir, with the same
// gimbal clamp.
struct AzimuthElevation {
  float azimuth;
  float elevation;
};
AzimuthElevation azimuthElevationFromDir(const glm::vec3& dir) {
  return {std::atan2(dir.y, dir.x), std::clamp(std::asin(std::clamp(dir.z, -1.0f, 1.0f)), -kPolarLimit, kPolarLimit)};
}

// Min/max orbit radius for the current scene: `lo` keeps the eye outside the near plane, `hi` is scene-aware so a
// large costmap is reachable (1e7 when the extent is unknown); `working_radius` is the radius the near plane derives
// from. `lo` uses the working-distance-only near term, NOT adaptiveNearFar()'s ratio-capped near: the cap can lift
// near to far/1e5, which with one distant finite outlier (reach ~1e6) would inflate lo to tens of metres and
// hard-lock zoom-in.
void radiusLimits(const AABB& bounds, const glm::vec3& focal, float working_radius, float& lo, float& hi) {
  const float reach = sceneReach(bounds, focal);
  const float working_near = std::max(working_radius * 1e-2f, 1e-3f);  // near from working distance only
  lo = std::max(2.0f * working_near, 1e-3f);
  hi = bounds.valid ? std::max(1e7f, reach * 5.0f) : 1e7f;
}

// Repairs an externally sourced CameraState (deserialized layout, state adopted across a model switch) so it cannot
// drive a degenerate projection: non-finite fields take the default, ranges are clamped (radius/ortho_scale > 0, fov_y
// off 0/π, elevation off the poles). One guard for every untrusted entry point.
CameraState sanitizeCameraState(CameraState state) {
  const CameraState defaults;
  const auto finite_or = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
  state.focal = glm::vec3{
      finite_or(state.focal.x, defaults.focal.x), finite_or(state.focal.y, defaults.focal.y),
      finite_or(state.focal.z, defaults.focal.z)};
  state.azimuth = finite_or(state.azimuth, defaults.azimuth);
  state.elevation = std::clamp(finite_or(state.elevation, defaults.elevation), -kPolarLimit, kPolarLimit);
  state.radius = std::max(finite_or(state.radius, defaults.radius), 1e-3f);
  state.ortho_scale = std::max(finite_or(state.ortho_scale, defaults.ortho_scale), 1e-3f);
  state.fov_y = std::clamp(finite_or(state.fov_y, defaults.fov_y), glm::radians(1.0f), glm::radians(179.0f));
  return state;
}
}  // namespace

glm::mat4 OrbitCamera::viewMatrix() const {
  return glm::lookAt(position(), state_.focal, glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 OrbitCamera::viewMatrixRelativeTo(const glm::dvec3& render_origin) const {
  // The focal minus the origin is taken in DOUBLE, then the small orbit offset radius·dir is added: eye_rel/focal_rel
  // stay at radius scale even with the focal at 1e6.
  const glm::vec3 focal_rel = toRenderSpace(state_.focal, render_origin);
  const glm::vec3 eye_rel = focal_rel + state_.radius * sphericalDir(state_.azimuth, state_.elevation);
  return glm::lookAt(eye_rel, focal_rel, glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const {
  // Near tracks the working distance (the orbit radius: always perspective) so close inspection never clips; far
  // reaches the whole scene, ratio-capped for depth precision.
  const float d = state_.radius;
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  adaptiveNearFar(d, sceneReach(scene_bounds_, state_.focal), near_plane, far_plane);
  return glm::perspective(state_.fov_y, aspect, near_plane, far_plane);
}

glm::vec3 OrbitCamera::position() const {
  return state_.focal + state_.radius * sphericalDir(state_.azimuth, state_.elevation);
}

void OrbitCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void OrbitCamera::followShift(const glm::vec3& world_delta) {
  // The eye derives from focal + spherical(azimuth, elevation, radius), so it rides the pivot rigidly.
  state_.focal += world_delta;
}

void OrbitCamera::rotate(float dx_pixels, float dy_pixels) {
  state_.azimuth -= dx_pixels / kPixelsPerRadian;
  state_.elevation = std::clamp(state_.elevation + dy_pixels / kPixelsPerRadian, -kPolarLimit, kPolarLimit);
}

void OrbitCamera::pan(float dx_pixels, float dy_pixels) {
  const glm::mat4 view = viewMatrix();
  const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
  const glm::vec3 up{view[0][1], view[1][1], view[2][1]};
  state_.focal += panOffset(right, up, dx_pixels, dy_pixels, state_.radius);
}

void OrbitCamera::zoom(float scroll_ticks) {
  // Geometric step: each tick scales the radius by 0.9 (zoom in) or 1/0.9, so the zoom rate is uniform at every scale;
  // limits are scene-aware.
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, state_.focal, state_.radius, lo, hi);
  const float factor = std::pow(0.9f, scroll_ticks);  // +ticks = zoom in = shrink
  state_.radius = std::clamp(state_.radius * factor, lo, hi);
}

void OrbitCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  if (viewport_w <= 0 || viewport_h <= 0) {
    zoom(scroll_ticks);
    return;
  }
  const float aspect = static_cast<float>(viewport_w) / static_cast<float>(viewport_h);
  const glm::mat4 view = viewMatrix();
  const glm::mat4 proj = projMatrix(aspect);
  const Ray ray = unprojectRay(cursor_px, viewport_w, viewport_h, view, proj);
  const glm::vec3 eye = position();

  // World point under the cursor: ground plane z=0 first (the robotics common case), else the focal plane facing the
  // eye; no forward hit means centre zoom. The ground hit is REJECTED beyond a sanity bound (a grazing ray, or a scene
  // far from z=0, puts the intersection hundreds of metres out and would fling the camera); the focal-plane anchor is
  // always near the viewed geometry.
  const float max_hit = std::max(sceneReach(scene_bounds_, state_.focal), 4.0f * state_.radius);
  glm::vec3 target{0.0f};
  float t = 0.0f;
  bool hit = false;
  if (rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t) && t > 0.0f && t <= max_hit) {
    target = ray.origin + t * ray.dir;
    hit = true;
  }
  if (!hit) {
    const glm::vec3 n = glm::normalize(state_.focal - eye);
    if (rayPlane(ray.origin, ray.dir, state_.focal, n, t) && t > 0.0f) {
      target = ray.origin + t * ray.dir;
      hit = true;
    }
  }
  if (!hit) {
    zoom(scroll_ticks);
    return;
  }

  // Homothety about the cursor point: scaling eye AND focal toward `target` by the same factor cannot rotate the
  // eye→focal direction, so azimuth/elevation stay and the hovered point stays pixel-locked. Recovering the angles from
  // new_eye − new_focal (setEyeFocal) loses low bits to float32 cancellation when the pivot sits at ~1e6 m (a followed
  // frame in a UTM map) and spins the view about 1.9° per tick at 5e6. So scale the radius and slide the pivot along
  // the cursor lever (target − focal), never differencing two large absolutes (eye = focal + radius·dir, so
  // new_eye = new_focal + s·(eye − focal)).
  const float s = std::pow(0.9f, scroll_ticks);
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, state_.focal, state_.radius, lo, hi);
  const float new_radius = std::clamp(s * state_.radius, lo, hi);
  // Slide by the EFFECTIVE post-clamp factor (new_radius/radius), so the cursor point stays pixel-locked even when the
  // radius hits [lo,hi]; without a clamp this is exactly s.
  const float s_eff = new_radius / state_.radius;
  state_.focal += (1.0f - s_eff) * (target - state_.focal);
  state_.radius = new_radius;
}

void OrbitCamera::setEyeFocal(const glm::vec3& eye, const glm::vec3& focal) {
  const glm::vec3 d = eye - focal;
  const float r_raw = glm::length(d);
  if (r_raw < 1e-6f) {
    state_.focal = focal;
    return;
  }
  const glm::vec3 dir = d / r_raw;
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, focal, r_raw, lo, hi);
  const float r = std::clamp(r_raw, lo, hi);
  const AzimuthElevation orientation = azimuthElevationFromDir(dir);
  state_.azimuth = orientation.azimuth;
  state_.elevation = orientation.elevation;
  state_.radius = r;
  // Re-anchors the focal so position() lands exactly on `eye` after radius/elevation clamping, keeping the cursor point
  // pixel-locked.
  state_.focal = eye - r * sphericalDir(orientation.azimuth, orientation.elevation);
}

void OrbitCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  state_ = sanitized;
  if (!sanitized.perspective) {
    // From an orthographic model: seed the radius from its scale so the view frames roughly the same area.
    state_.radius = sanitized.ortho_scale;
  }
  state_.perspective = true;
}

void OrbitCamera::reset() {
  // Restores the default view in place (`*this = OrbitCamera{}` would slice through an ICamera*). Scene bounds are
  // scene-derived, not view state, and are kept.
  state_ = CameraState{};
}

void OrbitCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  state_.focal = aabbCenter(bounds);
  state_.radius = std::max(aabbDiagonal(bounds) * 0.75f, 0.5f);
}

void XYOrbitCamera::followShift(const glm::vec3& world_delta) {
  // Ground-locked: tracks the followed frame's horizontal motion only, so the inherited orbit never goes airborne.
  state_.focal.x += world_delta.x;
  state_.focal.y += world_delta.y;
}

void XYOrbitCamera::pan(float dx_pixels, float dy_pixels) {
  OrbitCamera::pan(dx_pixels, dy_pixels);
  state_.focal.z = 0.0f;  // keep the pivot on the floor
}

void XYOrbitCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  OrbitCamera::zoomToCursor(scroll_ticks, cursor_px, viewport_w, viewport_h);
  if (std::abs(state_.focal.z) <= 1e-6f) {
    return;  // pivot already on the floor — the common case (ground-hit cursor target)
  }
  // Re-lock the pivot to z=0 WITHOUT re-deriving the angles: hold the eye and view direction and slide the pivot down
  // the view ray until focal.z == 0. setEyeFocal() would recover the angles from (eye − focal), a difference of two
  // ~1e6 vectors that reintroduces the large-coordinate angle noise OrbitCamera::zoomToCursor avoids; subtracting a
  // small radius·dir from the ~1e6 eye keeps full precision.
  const glm::vec3 dir = sphericalDir(state_.azimuth, state_.elevation);  // focal → eye, unit
  if (dir.z <= 1e-3f) {
    return;  // looking along/under the horizon: no finite ground pivot ahead, leave as-is
  }
  const glm::vec3 eye = position();
  const float ground_radius = eye.z / dir.z;  // eye.z − ground_radius·dir.z == 0
  float lo = 0.0f;
  float hi = 0.0f;
  radiusLimits(scene_bounds_, state_.focal, ground_radius, lo, hi);
  state_.radius = std::clamp(ground_radius, lo, hi);
  state_.focal = eye - state_.radius * dir;
  state_.focal.z = 0.0f;  // exact floor (the expression above is already ~0 when unclamped)
}

void XYOrbitCamera::adoptState(const CameraState& state) {
  OrbitCamera::adoptState(state);
  if (std::abs(state_.focal.z) > 1e-6f) {
    // Re-lock the adopted pivot to the floor, eye unchanged (as in zoomToCursor's flatten): re-derive the spherical
    // state so position() still lands on the eye.
    const glm::vec3 eye = position();
    glm::vec3 floor_focal = state_.focal;
    floor_focal.z = 0.0f;
    setEyeFocal(eye, floor_focal);
  }
}

namespace {
constexpr float kDefaultOrthoScale = 10.0f;
constexpr float kFlyNominalDistance = 5.0f;
// Base eye height as a multiple of ortho_scale above the focal (before eyeHeight()'s scene-aware lift); projMatrix()'s
// near is proportional to it.
constexpr float kOrthoEyeHeightFactor = 2.0f;

glm::vec3 mapUp(float azimuth) {
  return glm::vec3{-std::sin(azimuth), std::cos(azimuth), 0.0f};
}
glm::vec3 mapRight(float azimuth) {
  return glm::vec3{std::cos(azimuth), std::sin(azimuth), 0.0f};
}
}  // namespace

TopDownOrthoCamera::TopDownOrthoCamera() {
  reset();
}

glm::mat4 TopDownOrthoCamera::viewMatrix() const {
  return glm::lookAt(position(), state_.focal, mapUp(state_.azimuth));
}

glm::mat4 TopDownOrthoCamera::viewMatrixRelativeTo(const glm::dvec3& render_origin) const {
  const glm::vec3 focal_rel = toRenderSpace(state_.focal, render_origin);
  const glm::vec3 eye_rel = focal_rel + glm::vec3{0.0f, 0.0f, eyeHeight()};
  return glm::lookAt(eye_rel, focal_rel, mapUp(state_.azimuth));
}

float TopDownOrthoCamera::eyeHeight() const {
  const float base = kOrthoEyeHeightFactor * state_.ortho_scale;  // zoom-driven height
  // Scene-aware lift: raises the eye so geometry above the focal plane (a 1.5 m robot on a costmap zoomed to
  // ortho_scale=0.5) stays in front of the near plane. Zero for an unknown scene or geometry below the focal.
  const float lift = scene_bounds_.valid ? std::max(0.0f, scene_bounds_.max.z - state_.focal.z) : 0.0f;
  return base + lift;
}

glm::mat4 TopDownOrthoCamera::projMatrix(float aspect) const {
  const float base = kOrthoEyeHeightFactor * state_.ortho_scale;
  const float eye_height = eyeHeight();
  // Near is proportional to the zoom (base), not the lifted height: the lift already puts geometry above the focal in
  // front of the eye.
  const float near_plane = 0.01f * base;
  // Far reaches the lowest scene point below the eye plus a 0.5*base margin; lowest_z is clamped to the focal so a
  // scene above the focal keeps the margin.
  const float lowest_z = scene_bounds_.valid ? std::min(scene_bounds_.min.z, state_.focal.z) : state_.focal.z;
  const float far_plane = eye_height + (state_.focal.z - lowest_z) + 0.5f * base;
  const float half_w = state_.ortho_scale * aspect;
  return glm::ortho(-half_w, half_w, -state_.ortho_scale, state_.ortho_scale, near_plane, far_plane);
}

glm::vec3 TopDownOrthoCamera::position() const {
  return state_.focal + glm::vec3{0.0f, 0.0f, eyeHeight()};
}

void TopDownOrthoCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void TopDownOrthoCamera::followShift(const glm::vec3& world_delta) {
  // Pans the view with the followed frame; eyeHeight() and near/far derive from focal, so framing stays consistent as
  // the focal plane tracks z.
  state_.focal += world_delta;
}

void TopDownOrthoCamera::rotate(float dx_pixels, float /*dy_pixels*/) {
  state_.azimuth -= dx_pixels / kPixelsPerRadian;  // heading only; top-down has no pitch
}

void TopDownOrthoCamera::pan(float dx_pixels, float dy_pixels) {
  // Drags the map: translates the focal in the ground plane; sensitivity scales with ortho_scale so a pixel moves the
  // same fraction of the view at any zoom.
  constexpr float kPanFraction = 0.0025f;
  state_.focal +=
      (-dx_pixels * mapRight(state_.azimuth) + dy_pixels * mapUp(state_.azimuth)) * state_.ortho_scale * kPanFraction;
}

void TopDownOrthoCamera::zoom(float scroll_ticks) {
  state_.ortho_scale = std::clamp(state_.ortho_scale * std::pow(0.9f, scroll_ticks), 1e-3f, 1e7f);
}

void TopDownOrthoCamera::zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) {
  if (viewport_w <= 0 || viewport_h <= 0) {
    zoom(scroll_ticks);
    return;
  }
  const float aspect = static_cast<float>(viewport_w) / static_cast<float>(viewport_h);
  // Ground point under the cursor before and after the scale change; the focal shifts by the difference so it stays
  // pixel-locked.
  const auto ground_hit = [&](glm::vec2 px) -> glm::vec3 {
    const Ray ray = unprojectRay(px, viewport_w, viewport_h, viewMatrix(), projMatrix(aspect));
    float t = 0.0f;
    if (rayPlane(ray.origin, ray.dir, glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, t)) {
      return ray.origin + t * ray.dir;
    }
    return ray.origin;
  };
  const glm::vec3 before = ground_hit(cursor_px);
  zoom(scroll_ticks);  // same clamped geometric scale step as center-of-view zoom
  const glm::vec3 after = ground_hit(cursor_px);
  state_.focal += (before - after);
}

void TopDownOrthoCamera::reset() {
  state_ = CameraState{};
  state_.perspective = false;
  state_.ortho_scale = kDefaultOrthoScale;
  state_.focal = glm::vec3{0.0f};
  state_.azimuth = 0.0f;
}

void TopDownOrthoCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  state_.focal = glm::vec3{0.5f * (bounds.min.x + bounds.max.x), 0.5f * (bounds.min.y + bounds.max.y), 0.0f};
  const float half_extent = 0.5f * std::max(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y);
  state_.ortho_scale = std::max(half_extent, 1e-3f);
}

void TopDownOrthoCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  state_ = sanitized;
  if (sanitized.perspective) {
    // From a perspective model: seed ortho_scale from the orbit radius so the view shows a comparable area.
    state_.ortho_scale = std::max(sanitized.radius, 1e-3f);
  }
  // The model's invariant is a ground-level pivot (reset()/fitToBoundingBox() enforce it): an elevated focal.z (e.g.
  // Fly at altitude) would centre the near/far range on it and far-clip the whole ground scene to a blank view.
  state_.focal.z = 0.0f;
  state_.perspective = false;
}

FlyCamera::FlyCamera() {
  resetToDefault();
}

void FlyCamera::resetToDefault() {
  eye_ = glm::vec3{5.0f, 5.0f, 10.0f};
  const glm::vec3 fwd = glm::normalize(glm::vec3{0.0f} - eye_);  // look at the origin
  const AzimuthElevation orientation = azimuthElevationFromDir(fwd);
  yaw_ = orientation.azimuth;
  pitch_ = orientation.elevation;
}

glm::vec3 FlyCamera::forward() const {
  return sphericalDir(yaw_, pitch_);
}

float FlyCamera::workingDistance() const {
  if (scene_bounds_.valid) {
    return std::max(glm::length(eye_ - aabbCenter(scene_bounds_)), 1e-2f);
  }
  return kFlyNominalDistance;
}

float FlyCamera::motionDistance() const {
  // Floors the motion step so pan/zoom never collapse near the AABB centre, where workingDistance() → ~0 freezes the
  // controls (zoom() decays Zeno-style).
  return std::max(workingDistance(), kFlyNominalDistance);
}

glm::mat4 FlyCamera::viewMatrix() const {
  return glm::lookAt(eye_, eye_ + forward(), glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 FlyCamera::viewMatrixRelativeTo(const glm::dvec3& render_origin) const {
  const glm::vec3 eye_rel = toRenderSpace(eye_, render_origin);
  return glm::lookAt(eye_rel, eye_rel + forward(), glm::vec3{0.0f, 0.0f, 1.0f});
}

glm::mat4 FlyCamera::projMatrix(float aspect) const {
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  adaptiveNearFar(workingDistance(), sceneReach(scene_bounds_, eye_), near_plane, far_plane);
  // Near is clamped small: distance to the AABB centre says nothing about what is in front of a fly camera, so a
  // peripheral eye in a large scene would get a metre-scale near that clips geometry the user flew in to inspect. The
  // far/near ratio cap stays as the lower bound for depth precision.
  near_plane = std::max(std::min(near_plane, 0.05f), far_plane * 1e-5f);
  return glm::perspective(fov_y_, aspect, near_plane, far_plane);
}

glm::vec3 FlyCamera::position() const {
  return eye_;
}

void FlyCamera::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
}

void FlyCamera::followShift(const glm::vec3& world_delta) {
  // No orbit pivot: translates the eye and keeps yaw/pitch, so the flier keeps its heading while following.
  eye_ += world_delta;
}

void FlyCamera::rotate(float dx_pixels, float dy_pixels) {
  // Drag turns the camera in place, opposite in sign to orbit (which drags the scene about its pivot): drag right looks
  // right.
  yaw_ += dx_pixels / kPixelsPerRadian;
  pitch_ = std::clamp(pitch_ - dy_pixels / kPixelsPerRadian, -kPolarLimit, kPolarLimit);
}

void FlyCamera::pan(float dx_pixels, float dy_pixels) {
  const glm::vec3 fwd = forward();
  const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3{0.0f, 0.0f, 1.0f}));
  const glm::vec3 up = glm::cross(right, fwd);
  eye_ += panOffset(right, up, dx_pixels, dy_pixels, motionDistance());
}

void FlyCamera::zoom(float scroll_ticks) {
  eye_ += forward() * scroll_ticks * motionDistance() * 0.1f;  // +ticks = dolly forward
}

void FlyCamera::zoomToCursor(float scroll_ticks, glm::vec2 /*cursor_px*/, int /*viewport_w*/, int /*viewport_h*/) {
  zoom(scroll_ticks);  // degenerates to a forward dolly for fly (documented)
}

void FlyCamera::reset() {
  resetToDefault();
}

void FlyCamera::fitToBoundingBox(const AABB& bounds) {
  if (!bounds.valid) {
    return;
  }
  eye_ = aabbCenter(bounds) - forward() * std::max(aabbDiagonal(bounds), 1.0f);
}

CameraState FlyCamera::state() const {
  CameraState s;
  s.focal = eye_ + forward() * kFlyNominalDistance;  // synthesize a pivot in front (nominal)
  s.radius = kFlyNominalDistance;
  // CameraState azimuth/elevation parameterize the FOCAL-TO-EYE direction (OrbitCamera::position() = focal +
  // radius*sphericalDir(az, el)), the REVERSE of forward(): emit the reversed direction so an orbit/XYOrbit adopting
  // this state lands its eye on eye_ and looks along the original forward.
  s.azimuth = wrapToPi(yaw_ + std::numbers::pi_v<float>);
  s.elevation = -pitch_;  // pitch_ is pole-clamped, so -pitch_ is within kPolarLimit
  s.fov_y = fov_y_;
  s.ortho_scale = ortho_scale_;
  s.perspective = true;
  return s;
}

void FlyCamera::adoptState(const CameraState& state) {
  const CameraState sanitized = sanitizeCameraState(state);
  // Carried verbatim (parity with the other models); fov_y_ also drives projMatrix().
  fov_y_ = sanitized.fov_y;
  ortho_scale_ = sanitized.ortho_scale;
  if (sanitized.perspective) {
    // Sit at the orbit eye and look back toward its focal.
    eye_ = sanitized.focal + sanitized.radius * sphericalDir(sanitized.azimuth, sanitized.elevation);
    const AzimuthElevation orientation = azimuthElevationFromDir(glm::normalize(sanitized.focal - eye_));
    yaw_ = orientation.azimuth;
    pitch_ = orientation.elevation;
  } else {
    // From top-down ortho: float above the focal looking straight down.
    eye_ = sanitized.focal + glm::vec3{0.0f, 0.0f, kOrthoEyeHeightFactor * sanitized.ortho_scale};
    yaw_ = sanitized.azimuth;
    pitch_ = -kPolarLimit;
  }
}

std::string cameraStateToJson(const CameraState& state) {
  const nlohmann::json j{
      {"focal", {state.focal.x, state.focal.y, state.focal.z}},
      {"radius", state.radius},
      {"azimuth", state.azimuth},
      {"elevation", state.elevation},
      {"fov_y", state.fov_y},
      {"ortho_scale", state.ortho_scale},
      {"perspective", state.perspective},
  };
  return j.dump();
}

CameraState cameraStateFromJson(const std::string& json, const CameraState& fallback) {
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return fallback;  // unparseable / not an object → fallback unchanged
  }
  CameraState state = fallback;
  // value() throws json::type_error for a present key of the wrong type (a hand-edited "radius":"fast");
  // allow_exceptions=false only suppresses parse errors. Such a field is malformed: fall back.
  try {
    if (const auto it = parsed.find("focal"); it != parsed.end() && it->is_array() && it->size() == 3 &&
                                              (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number()) {
      state.focal = glm::vec3{(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    }
    state.radius = parsed.value("radius", state.radius);
    state.azimuth = parsed.value("azimuth", state.azimuth);
    state.elevation = parsed.value("elevation", state.elevation);
    state.fov_y = parsed.value("fov_y", state.fov_y);
    state.ortho_scale = parsed.value("ortho_scale", state.ortho_scale);
    state.perspective = parsed.value("perspective", state.perspective);
  } catch (const nlohmann::json::exception&) {
    return fallback;  // wrong-typed field → fallback unchanged
  }
  // Clamps out-of-range or non-finite values (e.g. "radius":0) before they reach glm::perspective / glm::lookAt.
  return sanitizeCameraState(state);
}

}  // namespace pj::scene3d
