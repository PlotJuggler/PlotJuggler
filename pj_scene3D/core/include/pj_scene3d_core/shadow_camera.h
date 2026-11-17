// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <glm/glm.hpp>

#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// Square shadow-map resolution in texels. Sized independently of the scene FBO and its render_scale supersampling:
// the shadow map is a separate target, so doubling scene supersampling must not quadruple its memory. 2048 gives ~mm
// texels on a single-robot scene; larger scenes trade sharpness for coverage. The light frustum is fit at this
// resolution so the texel snap in fitDirectionalShadowCamera matches the actual map.
inline constexpr int kShadowMapSize = 2048;

// Result of fitting an orthographic shadow camera to a directional light.
struct ShadowCameraFit {
  // World -> light clip space. Identity when `valid` is false. The receiver
  // shader projects a world position with this, maps clip.xy*0.5+0.5 to shadow-map
  // UV and clip.z*0.5+0.5 to the stored depth, and PCF-compares.
  glm::mat4 light_view_proj{1.0f};
  // World units spanned by one shadow-map texel (the ortho frustum width divided by
  // the map resolution). The receiver uses it to size the world-space normal offset
  // that pushes the comparison point off the surface (peter-panning / acne control),
  // so the bias tracks shadow-map resolution instead of being a magic constant.
  float world_units_per_texel{0.0f};
  // False => the caller must DISABLE shadows this frame (render unshadowed). Set
  // when there are no caster bounds, the bounds are degenerate (a point), or the
  // light direction is degenerate — fitting a frustum to any of those yields a
  // garbage or NaN matrix.
  bool valid{false};
};

// Fits an orthographic directional-light shadow camera tightly covering `bounds` (the union of mesh-caster AABBs, in
// the world / fixed frame).
//
// `to_light_dir` is the world-space direction TO the light (Z-up), need not be normalized. `shadow_map_size` is the
// square shadow-map resolution in texels; it drives the texel snapping that keeps the shadow edge from crawling as
// the scene moves.
//
// The ortho extents come from the AABB's bounding SPHERE (rotation-invariant, so the covered area and shadow
// resolution do not pulse as the camera or scene rotates), padded by a few texels to absorb the snap; the frustum is
// texel-snapped so a world point projects to the same texel until it moves a full texel.
//
// `valid == false` for invalid/degenerate bounds or a zero light direction.
[[nodiscard]] ShadowCameraFit fitDirectionalShadowCamera(
    const AABB& bounds, const glm::vec3& to_light_dir, int shadow_map_size);

// Extends `bounds` to enclose where its contents' shadows land on the plane z == ground_z, for a directional light
// toward `to_light_dir` (Z-up): each of the 8 corners is marched along -to_light_dir to the ground plane and unioned
// in. The floor receives but never casts, so a caster-only frustum would miss the shadow falling on it. Unchanged when
// `bounds` is invalid or the light is horizontal/below (`to_light_dir.z <= 0`); corners at or below the plane add
// nothing.
[[nodiscard]] inline AABB extendAabbToGroundShadow(const AABB& bounds, const glm::vec3& to_light_dir, float ground_z) {
  if (!bounds.valid || to_light_dir.z <= 1e-3f) {
    return bounds;
  }
  const glm::vec3 to_light = glm::normalize(to_light_dir);
  AABB out = bounds;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 c{
        (corner & 1) != 0 ? bounds.max.x : bounds.min.x, (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
        (corner & 4) != 0 ? bounds.max.z : bounds.min.z};
    const float height = c.z - ground_z;
    if (height <= 0.0f) {
      continue;  // already on/below the floor — its shadow would be behind the light
    }
    expandAABB(out, c - to_light * (height / to_light.z));  // march to z == ground_z
  }
  return out;
}

}  // namespace pj::scene3d
