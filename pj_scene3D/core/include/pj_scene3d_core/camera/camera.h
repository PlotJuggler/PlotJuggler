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

// Axis-aligned bounding box in one coordinate frame. `valid == false` means no bounds yet and is the identity of
// unionAABB().
struct AABB {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
  friend bool operator==(const AABB&, const AABB&) = default;
};

// Grow `box` to include `p` (a degenerate box at `p` when invalid). Callers pre-filter non-finite points.
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

// Union of two boxes; an invalid box is the identity.
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

[[nodiscard]] inline float aabbDiagonal(const AABB& box) {
  return glm::length(box.max - box.min);
}

// Min/max along one axis (0=x, 1=y, 2=z, clamped) of `box` after transforming it by `transform`: the extent along a
// DESTINATION-frame axis. A cloud coloured by its fixed-frame x/y/z derives colour from the transformed position in the
// shader, so its colormap bounds must be measured in that frame. Exact for the transformed box (all 8 corners); for a
// point set inside it a conservative outer bound, loose under large pitch/roll. {0, 1} for an invalid box.
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

// Row `index` of a 4x4 (glm stores columns).
[[nodiscard]] inline glm::vec4 matrixRow(const glm::mat4& matrix, int index) {
  return glm::vec4(matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
}

// The corner of `box` furthest along `direction` (AABB support function): a linear functional's extreme over a box is
// attained at a corner.
[[nodiscard]] inline glm::vec3 aabbSupport(const AABB& box, const glm::vec3& direction) {
  return glm::vec3{
      direction.x >= 0.0f ? box.max.x : box.min.x, direction.y >= 0.0f ? box.max.y : box.min.y,
      direction.z >= 0.0f ? box.max.z : box.min.z};
}

// True when `box` lies entirely outside the frustum of `clip_from_box`, the matrix from the box's OWN frame to clip
// space (e.g. proj * view * model). Gribb-Hartmann planes; conservative: never culls something visible, but a box
// wedged between two planes can survive. An invalid box is never culled (unknown extent means draw).
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

// [min, max] of clip-space w over `box` under `clip_from_box`. Under perspective w is the view-axis depth, so this
// bounds how near and far a cloud's points are without touching them; under orthographic projection w is 1 and the
// range collapses. An invalid box has no range.
[[nodiscard]] inline std::pair<float, float> aabbClipWRange(const AABB& box, const glm::mat4& clip_from_box) {
  if (!box.valid) {
    return {0.0f, 0.0f};
  }
  const glm::vec4 w_row = matrixRow(clip_from_box, 3);
  const glm::vec3 normal(w_row);
  return {glm::dot(normal, aabbSupport(box, -normal)) + w_row.w, glm::dot(normal, aabbSupport(box, normal)) + w_row.w};
}

// Smallest axis-aligned box enclosing `local` after transforming it by `transform` (all 8 corners): a conservative
// outer bound, loose under large pitch/roll. Invalid when `local` is.
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

// AABB of an occupancy grid from its origin (min corner, in the grid's source frame), resolution (m/cell) and cell
// dimensions. Planar: zero z extent at origin.z; the origin rotation is ignored (scene bounds drive framing and
// adaptive near/far, not culling). Invalid for an empty or degenerate grid.
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

// Serializable camera pose shared by every controller: state() exports it and adoptState() adopts it, so switching
// camera models keeps the view. `perspective == false` selects an orthographic projection driven by `ortho_scale`
// (half-height) instead of `radius`/`fov_y`.
struct CameraState {
  glm::vec3 focal{0.0f};                 ///< Orbit pivot / look-at target (world).
  float radius{5.0f};                    ///< Eye distance from focal (perspective).
  float azimuth{0.0f};                   ///< Yaw about +Z (radians).
  float elevation{glm::radians(30.0f)};  ///< Pitch above the XY plane (radians).
  float fov_y{glm::radians(45.0f)};      ///< Vertical field of view (perspective).
  float ortho_scale{5.0f};               ///< Half-height of the ortho frustum (world units).
  bool perspective{true};                ///< Projection kind.

  friend bool operator==(const CameraState&, const CameraState&) = default;
};

// JSON (de)serialization of CameraState for layout persistence. cameraStateFromJson is tolerant: unparseable or
// wrong-typed input returns `fallback`, a missing field comes from `fallback`, and surviving values are sanitized
// (finite, in range) so a corrupt layout cannot drive a degenerate view.
[[nodiscard]] std::string cameraStateToJson(const CameraState& state);
[[nodiscard]] CameraState cameraStateFromJson(const std::string& json, const CameraState& fallback = CameraState{});

// Abstract camera controller. Models (Orbit, XYOrbit, TopDownOrtho, Fly) differ in how input maps to CameraState and
// in the projection they emit; all expose the same view/projection/position. Qt/GL-free: headless-testable.
class ICamera {
 public:
  virtual ~ICamera() = default;

  [[nodiscard]] virtual glm::mat4 viewMatrix() const = 0;

  // View matrix for CAMERA-RELATIVE rendering: eye and focal are expressed relative to `render_origin`, subtracted in
  // DOUBLE precision before the float cast. Pair it with model matrices offset by the same origin: at large world
  // coordinates (e.g. following a far frame in a UTM map) a float32 eye->geometry delta cancels and the scene swims on
  // zoom. `render_origin == {0,0,0}` equals viewMatrix().
  [[nodiscard]] virtual glm::mat4 viewMatrixRelativeTo(const glm::dvec3& render_origin) const = 0;

  [[nodiscard]] virtual glm::mat4 projMatrix(float aspect) const = 0;
  [[nodiscard]] virtual glm::vec3 position() const = 0;

  // Latest fixed-frame scene extent, used for adaptive near/far and framing. An
  // invalid AABB means "unknown" → working-distance fallback.
  virtual void setSceneBounds(const AABB& bounds) = 0;

  // Shift the camera's anchor by a world-space delta without changing orbit angle, zoom or orientation: the primitive
  // "follow a frame" applies each tick (translation only). Orbit/TopDown shift focal, Fly the eye; XYOrbit drops z to
  // stay ground-locked. See SceneViewWidget::applyFollow.
  virtual void followShift(const glm::vec3& world_delta) = 0;

  virtual void rotate(float dx_pixels, float dy_pixels) = 0;
  virtual void pan(float dx_pixels, float dy_pixels) = 0;
  virtual void zoom(float scroll_ticks) = 0;
  // Zoom keeping the world point under the cursor fixed; cursor_px in widget pixels (origin top-left). Falls back to
  // centre zoom() when no ground/focal-plane hit exists.
  virtual void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) = 0;

  virtual void reset() = 0;  ///< Restore this model's default view.
  virtual void fitToBoundingBox(const AABB& bounds) = 0;

  [[nodiscard]] virtual CameraState state() const = 0;
  virtual void adoptState(const CameraState& state) = 0;
};

// Spherical orbit camera, Z-up: the eye orbits `focal` at `radius` (azimuth/elevation), with adaptive near/far and
// cursor-anchored zoom. Non-final: XYOrbitCamera derives from it.
class OrbitCamera : public ICamera {
 public:
  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // Shared with XYOrbitCamera: focal + radius·dir is the eye for both, so the ground lock never affects the relative
  // view.
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
  // Sets the spherical state so position() == eye and the camera looks at `focal` (clamps radius/elevation but keeps
  // the eye fixed). Shared by zoomToCursor and the XYOrbit re-lock.
  void setEyeFocal(const glm::vec3& eye, const glm::vec3& focal);

  CameraState state_{};
  AABB scene_bounds_{};  // last reported scene extent; consumed by adaptive near/far
};

// Orbit camera with the pivot locked to the ground plane (z=0): pan slides the focal in the floor plane and zoom keeps
// focal.z == 0.
class XYOrbitCamera final : public OrbitCamera {
 public:
  // Ground-locked: shifts focal in XY only, so the pivot stays on the floor while following a frame with vertical
  // motion.
  void followShift(const glm::vec3& world_delta) override;
  void pan(float dx_pixels, float dy_pixels) override;
  void zoomToCursor(float scroll_ticks, glm::vec2 cursor_px, int viewport_w, int viewport_h) override;
  // Re-locks the pivot to the floor (z=0) after adopting an arbitrary pose, keeping the eye fixed; an off-floor pivot
  // would make the inherited orbit/zoom act on an airborne point and the first pan snap the eye vertically.
  void adoptState(const CameraState& state) override;
};

// Orthographic bird's-eye camera looking down -Z, rotatable about the vertical (map heading); the 2D mode for
// costmaps, occupancy grids and trajectories. `ortho_scale` is the view's half-height in world units; near/far derive
// from it plus the scene's vertical extent (flat-scene guard: a zoomed costmap never clips).
class TopDownOrthoCamera final : public ICamera {
 public:
  TopDownOrthoCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // The eye floats eyeHeight() straight above the focal, not focal+radius·dir: the generic orbit reconstruction does
  // not apply.
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
  // Eye height above the focal plane, shared by position() and projMatrix() so they cannot desync: the zoom-driven base
  // height plus a scene-aware lift that keeps geometry above the focal plane in front of the near plane however far
  // the user zooms in.
  [[nodiscard]] float eyeHeight() const;

  CameraState state_{};
  AABB scene_bounds_{};
};

// First-person camera, Z-up: free eye + yaw/pitch, no orbit pivot. LEFT looks around, pan strafes, wheel dollies along
// forward (zoom-to-cursor degenerates to that dolly).
class FlyCamera final : public ICamera {
 public:
  FlyCamera();

  [[nodiscard]] glm::mat4 viewMatrix() const override;
  // Fly owns a free eye (no orbit pivot): offsets eye_ directly and keeps the yaw/pitch forward.
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
  // Motion-step basis: workingDistance() floored at kFlyNominalDistance so pan/zoom steps never collapse near the AABB
  // centre.
  [[nodiscard]] float motionDistance() const;

  glm::vec3 eye_{5.0f, 5.0f, 10.0f};
  float yaw_{0.0f};
  float pitch_{0.0f};
  // Fly has no orbit radius or ortho frustum but must round-trip every CameraState field across model switches: fov_y_
  // drives its projection, ortho_scale_ is carried for a later switch to ortho.
  float fov_y_{glm::radians(45.0f)};
  float ortho_scale_{5.0f};
  AABB scene_bounds_{};
};

}  // namespace pj::scene3d
