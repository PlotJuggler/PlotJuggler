// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <utility>

namespace pj::scene3d {

// Axis-aligned bounding box in a single coordinate frame. `valid == false` means
// "no bounds reported yet" and acts as the identity element for unionAABB(): an
// invalid box contributes nothing to a union. Lives in pj_scene3d_core (Qt/GL-free)
// so scene geometry and the camera can share it and the math stays headless-testable.
struct AABB {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
  friend bool operator==(const AABB&, const AABB&) = default;
};

// Grow `box` to include point `p`. Initializes `box` to the degenerate box at `p`
// when it was invalid. Callers should pre-filter non-finite points.
inline void expandAABB(AABB& box, const glm::vec3& p) {
  if (!box.valid) {
    box.min = p;
    box.max = p;
    box.valid = true;
    return;
  }
  box.min = glm::min(box.min, p);
  box.max = glm::max(box.max, p);
}

// Union of two boxes. An invalid box is the identity, so unioning over a mix of
// reporting / non-reporting entities "just works".
inline AABB unionAABB(const AABB& a, const AABB& b) {
  if (!a.valid) {
    return b;
  }
  if (!b.valid) {
    return a;
  }
  AABB out;
  out.min = glm::min(a.min, b.min);
  out.max = glm::max(a.max, b.max);
  out.valid = true;
  return out;
}

// Center of a box. Only meaningful when the box is valid.
[[nodiscard]] inline glm::vec3 aabbCenter(const AABB& box) {
  return 0.5f * (box.min + box.max);
}

// Length of the box's space diagonal (0 for a degenerate / point box).
[[nodiscard]] inline float aabbDiagonal(const AABB& box) {
  return glm::length(box.max - box.min);
}

// Min/max of one axis (0=x, 1=y, 2=z) of `box` after transforming it by
// `transform` — i.e. the extent of the box's image along a DESTINATION-frame axis.
// Used to auto-range a point cloud colored by its FIXED-FRAME x/y/z: the per-point
// colour is derived in the shader from the transformed position, so its colormap
// bounds must be measured in that same destination frame, not in the box's own
// source frame.
//
// Exact for the transformed BOX (all 8 corners are tested); for a sparse point set
// inside the box this is a conservative (outer) bound — tight under translation and
// yaw, slightly loose under large pitch/roll. Returns {0, 1} for an invalid box.
// `axis` is clamped to [0, 2].
[[nodiscard]] inline std::pair<float, float> transformedAabbAxisRange(
    const AABB& box, const glm::mat4& transform, int axis) {
  const int selected_axis = glm::clamp(axis, 0, 2);
  if (!box.valid) {
    return {0.0f, 1.0f};
  }
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 source_corner{
        (corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
        (corner & 4) != 0 ? box.max.z : box.min.z};
    const float value = (transform * glm::vec4(source_corner, 1.0f))[selected_axis];
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  return {lo, hi};
}

// One ROW of a 4x4, which glm's column-major storage does not give you directly: row i
// gathers component i across the four columns. Both frustum helpers below read the
// matrix by rows, so this is the shared idiom rather than two hand-indexed copies.
[[nodiscard]] inline glm::vec4 matrixRow(const glm::mat4& matrix, int index) {
  return glm::vec4(matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
}

// The corner of `box` furthest along `direction` (an AABB support function). Both
// helpers below need the extreme of a linear functional over the box, which is attained
// at a corner, and each axis picks its end independently.
[[nodiscard]] inline glm::vec3 aabbSupport(const AABB& box, const glm::vec3& direction) {
  return glm::vec3{
      direction.x >= 0.0f ? box.max.x : box.min.x, direction.y >= 0.0f ? box.max.y : box.min.y,
      direction.z >= 0.0f ? box.max.z : box.min.z};
}

// True when `box` lies entirely outside the frustum of `clip_from_box` — the
// combined matrix taking the box's OWN frame all the way to clip space (e.g.
// proj * view * model), so a caller never has to transform the box first.
//
// Extracts the six clip planes from that matrix (Gribb-Hartmann) and rejects the
// box when the corner furthest along a plane's normal is still on its outside.
// Conservative in the usual way: it never culls something visible, but a box
// wedged into the corner between two planes can survive the test. An invalid box
// is never culled — an unknown extent must mean "draw it".
[[nodiscard]] inline bool aabbOutsideFrustum(const AABB& box, const glm::mat4& clip_from_box) {
  if (!box.valid) {
    return false;
  }
  const glm::vec4 w = matrixRow(clip_from_box, 3);
  const std::array<glm::vec4, 6> planes = {w + matrixRow(clip_from_box, 0), w - matrixRow(clip_from_box, 0),
                                           w + matrixRow(clip_from_box, 1), w - matrixRow(clip_from_box, 1),
                                           w + matrixRow(clip_from_box, 2), w - matrixRow(clip_from_box, 2)};
  for (const glm::vec4& plane : planes) {
    const glm::vec3 normal(plane);
    if (glm::dot(normal, aabbSupport(box, normal)) + plane.w < 0.0f) {
      return true;
    }
  }
  return false;
}

// [min, max] of the clip-space w coordinate over `box`, under the matrix taking the
// box's OWN frame to clip space (the same `clip_from_box` aabbOutsideFrustum takes).
//
// Under a perspective projection that w IS the view-axis depth, so this is how far the
// nearest and furthest of a cloud's points can be — enough to answer "is EVERY point of
// this cloud closer/further than X" without touching the points. Under an orthographic
// projection w is 1 everywhere and the range collapses, which is the correct answer
// there: a parallel projection gives every point the same scale.
//
// An invalid box has no range.
[[nodiscard]] inline std::pair<float, float> aabbClipWRange(const AABB& box, const glm::mat4& clip_from_box) {
  if (!box.valid) {
    return {0.0f, 0.0f};
  }
  const glm::vec4 w_row = matrixRow(clip_from_box, 3);
  const glm::vec3 normal(w_row);
  return {glm::dot(normal, aabbSupport(box, -normal)) + w_row.w, glm::dot(normal, aabbSupport(box, normal)) + w_row.w};
}

// Smallest axis-aligned box that encloses `local` after transforming it by
// `transform` (all 8 corners are mapped). The result is a conservative outer bound
// of the transformed box — tight under translation/yaw, slightly loose under large
// pitch/roll. Used to lift a mesh's model-space AABB into the world for the shadow
// caster-bounds union. Returns an invalid box when `local` is invalid.
[[nodiscard]] inline AABB transformedAABB(const glm::mat4& transform, const AABB& local) {
  if (!local.valid) {
    return {};
  }
  AABB out;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 c{
        (corner & 1) != 0 ? local.max.x : local.min.x, (corner & 2) != 0 ? local.max.y : local.min.y,
        (corner & 4) != 0 ? local.max.z : local.min.z};
    expandAABB(out, glm::vec3(transform * glm::vec4(c, 1.0f)));
  }
  return out;
}

// AABB of an occupancy grid given its origin (the grid's min corner, in the
// grid's own source frame), cell resolution (metres/cell), and dimensions in
// cells. The grid is planar — the z extent is zero at origin.z (origin rotation
// is intentionally ignored; scene bounds drive framing / adaptive near-far, not
// precise culling). Returns an invalid box for an empty or degenerate grid.
inline AABB occupancyGridBounds(const glm::vec3& origin, double resolution, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || resolution <= 0.0) {
    return {};
  }
  const auto res = static_cast<float>(resolution);
  AABB box;
  box.min = origin;
  box.max = origin + glm::vec3{res * static_cast<float>(width), res * static_cast<float>(height), 0.0f};
  box.valid = true;
  return box;
}

// Serializable camera pose, shared by every controller. A controller exports its
// pose with state() and adopts another's with adoptState(), so switching camera
// models preserves where you were looking. `perspective == false` selects an
// orthographic projection driven by `ortho_scale` (half-height in world units)
// instead of `radius`/`fov_y`.
struct CameraState {
  glm::vec3 focal{0.0f};                 ///< Orbit pivot / look-at target (world).
  float radius{5.0f};                    ///< Eye distance from focal (perspective).
  float azimuth{0.0f};                   ///< Yaw about +Z (radians).
  float elevation{glm::radians(30.0f)};  ///< Pitch above the XY plane (radians).
  float fov_y{glm::radians(45.0f)};      ///< Vertical field of view (perspective).
  float ortho_scale{5.0f};               ///< Half-height of the ortho frustum (world units).
  bool perspective{true};                ///< Projection kind.
};

// JSON (de)serialization of CameraState, for layout persistence. Round-trips all
// fields. cameraStateFromJson is tolerant: unparseable, non-object, or wrong-typed
// input returns `fallback` unchanged, and any individually-missing field is taken
// from `fallback` (so an older / partial layout still loads). Surviving values are
// sanitized (finite, in-range) so a corrupt layout can never drive a degenerate view.
[[nodiscard]] std::string cameraStateToJson(const CameraState& state);
[[nodiscard]] CameraState cameraStateFromJson(const std::string& json, const CameraState& fallback = CameraState{});

// Abstract camera controller. Concrete models (Orbit, XYOrbit, TopDownOrtho, Fly)
// differ in how mouse/scroll input maps to CameraState and which projection they
// emit, but all expose the same view/projection/position the renderer consumes.
// Lives in pj_scene3d_core so the math is headless-testable (no Qt / GL).
class ICamera {
 public:
  virtual ~ICamera() = default;

  [[nodiscard]] virtual glm::mat4 viewMatrix() const = 0;

  // View matrix for CAMERA-RELATIVE rendering: same orientation as viewMatrix(),
  // but eye and focal are expressed relative to `render_origin` (subtracted in
  // DOUBLE precision before the float cast). Pair it with geometry whose model
  // matrices are offset by the SAME origin (see FrameContext::render_origin) and a
  // camera sitting at large world coordinates — e.g. "Follow a frame" parking the
  // pivot on a far frame in a UTM/GPS map — no longer loses the eye→geometry delta
  // to float32 cancellation (which made the whole scene swim on zoom). With
  // `render_origin == {0,0,0}` it is exactly viewMatrix().
  [[nodiscard]] virtual glm::mat4 viewMatrixRelativeTo(const glm::dvec3& render_origin) const = 0;

  [[nodiscard]] virtual glm::mat4 projMatrix(float aspect) const = 0;
  [[nodiscard]] virtual glm::vec3 position() const = 0;

  // Latest fixed-frame scene extent, used for adaptive near/far and framing. An
  // invalid AABB means "unknown" → working-distance fallback.
  virtual void setSceneBounds(const AABB& bounds) = 0;

  // Shift the camera's anchor (look-at target / eye) by a world-space delta,
  // WITHOUT changing orbit angle / zoom / orientation — the primitive the "follow
  // a frame" feature applies each tick so the camera rides a moving frame while the
  // user's framing is preserved (it only ADDS the frame's motion). Position-only
  // follow: translation, no rotation. Each model shifts whatever anchor it owns
  // (Orbit/TopDown: focal; Fly: eye); XYOrbit drops the z component to stay
  // ground-locked. See SceneViewWidget::applyFollow.
  virtual void followShift(const glm::vec3& world_delta) = 0;

  virtual void rotate(float dx_pixels, float dy_pixels) = 0;
  virtual void pan(float dx_pixels, float dy_pixels) = 0;
  virtual void zoom(float scroll_ticks) = 0;
  // Zoom keeping the world point under the cursor fixed. cursor_px is in Qt
  // widget pixels (origin top-left). Falls back to center-of-view zoom() when no
  // ground/focal-plane hit exists.
  virtual void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) = 0;

  virtual void reset() = 0;  ///< Restore this model's default view.
  virtual void fitToBoundingBox(const AABB& bounds) = 0;

  [[nodiscard]] virtual CameraState state() const = 0;
  virtual void adoptState(const CameraState& state) = 0;
};

// Spherical orbit camera: eye orbits `focal` at `radius`, parameterized by
// azimuth/elevation. Z-up. Emits an adaptive near/far perspective projection and
// supports cursor-anchored zoom. Non-final — XYOrbitCamera derives from it.
class OrbitCamera : public ICamera {
 public:
  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // Inherited unchanged by XYOrbitCamera (it shares state_): focal+radius·dir is
  // the eye for both, so the ground-lock never affects the relative view build.
  [[nodiscard]] glm::mat4 viewMatrixRelativeTo(const glm::dvec3& render_origin) const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void followShift(const glm::vec3& world_delta) override;  // focal += delta
  void rotate(float dx_pixels, float dy_pixels) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override {
    return state_;
  }
  void adoptState(const CameraState& state) override;

 protected:
  // Set spherical state so position() == eye and the camera looks at `focal`
  // (re-derives radius/azimuth/elevation; clamps radius/elevation but keeps the
  // eye fixed). Shared by zoomToCursor and the XYOrbit ground re-lock.
  void setEyeFocal(const glm::vec3& eye, const glm::vec3& focal);

  CameraState state_{};
  AABB scene_bounds_{};  // last reported scene extent; consumed by adaptive near/far
};

// Orbit camera with the pivot locked to the ground plane (z=0): panning slides the
// focal in the floor plane and zoom keeps focal.z == 0, so the orbit centre never
// floats off the floor. Perspective. Derives from OrbitCamera.
class XYOrbitCamera final : public OrbitCamera {
 public:
  // Ground-locked override: shift focal in XY only (drop world_delta.z) so the
  // pivot never floats off the floor while following a frame with vertical motion.
  void followShift(const glm::vec3& world_delta) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;
  // Re-lock the pivot to the floor (z=0) after adopting an arbitrary pose, keeping
  // the eye fixed — without this, an adopted off-floor pivot makes the inherited
  // center-zoom/rotate orbit an airborne point and the first pan snaps the eye
  // vertically (the stale focal.z is zeroed). See OrbitCamera::adoptState for the base.
  void adoptState(const CameraState& state) override;
};

// Orthographic bird's-eye camera looking straight down -Z, rotatable about the
// vertical (map heading). The robotics "2D mode" for costmaps / occupancy grids /
// trajectories. `ortho_scale` is the half-height of the view in world units; the
// eye floats above the focal and near/far are derived from that height plus the
// scene's vertical extent (the flat-scene guard, so a zoomed costmap never clips).
class TopDownOrthoCamera final : public ICamera {
 public:
  TopDownOrthoCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // Overridden: the eye floats straight up by eyeHeight() above the focal, not
  // focal+radius·dir, so the generic orbit reconstruction does not apply here.
  [[nodiscard]] glm::mat4 viewMatrixRelativeTo(const glm::dvec3& render_origin) const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void followShift(const glm::vec3& world_delta) override;  // focal += delta (pans the top-down view)
  void rotate(float dx_pixels, float dy_pixels) override;   // azimuth (map heading) only
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override {
    return state_;
  }
  void adoptState(const CameraState& state) override;

 private:
  // Eye height above the focal plane, shared verbatim by position() and
  // projMatrix() so they can never desync (M.7/M.8). It is the zoom-driven base
  // height PLUS a scene-aware lift that keeps geometry above the focal plane in
  // front of the near plane no matter how far the user zooms in.
  [[nodiscard]] float eyeHeight() const;

  CameraState state_{};
  AABB scene_bounds_{};
};

// First-person / fly camera: free eye + yaw/pitch, no orbit pivot. LEFT looks
// around, pan strafes, wheel dollies along forward (zoom-to-cursor degenerates to
// a forward dolly, by design). Perspective, Z-up.
class FlyCamera final : public ICamera {
 public:
  FlyCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // Overridden: Fly owns a free eye (no orbit pivot), so the relative build offsets
  // eye_ directly and keeps the yaw/pitch forward direction.
  [[nodiscard]] glm::mat4 viewMatrixRelativeTo(const glm::dvec3& render_origin) const override;
  [[nodiscard]] glm::mat4 projMatrix(float aspect) const override;
  [[nodiscard]] glm::vec3 position() const override;

  void setSceneBounds(const AABB& bounds) override;

  void followShift(const glm::vec3& world_delta) override;  // eye += delta
  void rotate(float dx_pixels, float dy_pixels) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoom(float scroll_ticks) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;

  void reset() override;
  void fitToBoundingBox(const AABB& bounds) override;

  [[nodiscard]] CameraState state() const override;
  void adoptState(const CameraState& state) override;

 private:
  void resetToDefault();                        // eye (5,5,10) looking at the origin
  [[nodiscard]] glm::vec3 forward() const;      // unit view direction from yaw/pitch
  [[nodiscard]] float workingDistance() const;  // synthetic focal distance for near/far
  // Motion-step basis: workingDistance() floored at kFlyNominalDistance so pan/zoom
  // steps never collapse to ~0 near the AABB center (where workingDistance() → 0).
  [[nodiscard]] float motionDistance() const;

  glm::vec3 eye_{5.0f, 5.0f, 10.0f};
  float yaw_{0.0f};
  float pitch_{0.0f};
  // Fly has no orbit radius or ortho frustum, but it must round-trip every
  // CameraState field across model switches (camera.h:77-81). fov_y_ drives its
  // own projection; ortho_scale_ is carried verbatim for a later switch to ortho.
  float fov_y_{glm::radians(45.0f)};
  float ortho_scale_{5.0f};
  AABB scene_bounds_{};
};

}  // namespace pj::scene3d
