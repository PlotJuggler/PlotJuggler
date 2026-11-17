#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <glm/glm.hpp>
#include <optional>
#include <span>

namespace pj::scene3d {

// Projects a frame origin (in the fixed/world frame) to logical screen pixels (top-left origin, +y DOWN, comparable
// with a QMouseEvent position). `view_proj` is proj * view; `viewport_px` is {width, height} in LOGICAL widget pixels.
// nullopt when the origin is at or behind the camera plane (clip.w <= 0): it must never be a hover candidate. Pure
// glm: unit-testable without GL or Qt.
[[nodiscard]] inline std::optional<glm::vec2> projectFrameOrigin(
    const glm::mat4& view_proj, glm::vec2 viewport_px, const glm::vec3& world_origin) {
  const glm::vec4 clip = view_proj * glm::vec4(world_origin, 1.0f);
  // clip.w <= 0 means the origin is on or behind the camera plane: the
  // perspective divide would mirror it to a bogus on-screen position.
  if (clip.w <= 0.0f) {
    return std::nullopt;
  }
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  // y-flip: NDC +y is up, widget pixels grow downward from the top-left.
  return glm::vec2{(ndc.x * 0.5f + 0.5f) * viewport_px.x, (0.5f - ndc.y * 0.5f) * viewport_px.y};
}

// Index into `points` of the frame whose projected pixel is closest to the
// cursor AND within `radius_px`, or nullopt when none qualifies. On an exact
// distance tie (two frames at the same pixel) the first wins, so the pick is
// deterministic.
[[nodiscard]] inline std::optional<std::size_t> pickNearestFrame(
    std::span<const glm::vec2> points, glm::vec2 cursor_px, float radius_px) {
  const float radius2 = radius_px * radius_px;
  std::optional<std::size_t> best;
  float best_dist2 = 0.0f;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const glm::vec2 delta = points[i] - cursor_px;
    const float dist2 = glm::dot(delta, delta);
    if (dist2 > radius2) {
      continue;  // outside the pick radius
    }
    if (!best.has_value() || dist2 < best_dist2) {  // first hit, or strictly closer (first wins a tie)
      best = i;
      best_dist2 = dist2;
    }
  }
  return best;
}

}  // namespace pj::scene3d
