// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Golden tests for the GL-free directional shadow-camera fit (fitDirectionalShadowCamera). They pin three properties
// the shadow feature depends on that are hard to eyeball in a screenshot:
//   1. Coverage: every corner of the caster AABB lands inside the light's clip cube [-1,1]^3, so no caster falls
//      outside the shadow map.
//   2. Validity: degenerate inputs (no bounds / a point box / a zero light direction) return valid == false, so the
//      caller disables shadows instead of fitting a garbage frustum.
//   3. Texel snap: the frustum quantizes to the shadow-map texel grid, so the shadow edge does not crawl as the scene
//      moves sub-texel.

#include "pj_scene3d_core/shadow_camera.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"  // AABB, aabbCenter, aabbDiagonal

namespace pj::scene3d {
namespace {

constexpr int kSize = 2048;

AABB boxFromMinMax(glm::vec3 lo, glm::vec3 hi) {
  AABB b;
  b.min = lo;
  b.max = hi;
  b.valid = true;
  return b;
}

glm::vec3 toClip(const ShadowCameraFit& fit, glm::vec3 world) {
  const glm::vec4 clip = fit.light_view_proj * glm::vec4(world, 1.0f);
  return glm::vec3(clip) / clip.w;
}

TEST(ShadowCameraFit, CoversAllEightCornersInClipCube) {
  const AABB box = boxFromMinMax({-1.0f, -2.0f, 0.0f}, {3.0f, 1.0f, 2.0f});
  const glm::vec3 to_light(0.4f, 0.3f, 1.0f);  // deliberately not normalized
  const ShadowCameraFit fit = fitDirectionalShadowCamera(box, to_light, kSize);
  ASSERT_TRUE(fit.valid);
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 corner{
        (i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z};
    const glm::vec3 c = toClip(fit, corner);
    EXPECT_GE(c.x, -1.0f) << "corner " << i;
    EXPECT_LE(c.x, 1.0f) << "corner " << i;
    EXPECT_GE(c.y, -1.0f) << "corner " << i;
    EXPECT_LE(c.y, 1.0f) << "corner " << i;
    EXPECT_GE(c.z, -1.0f) << "corner " << i;
    EXPECT_LE(c.z, 1.0f) << "corner " << i;
  }
}

TEST(ShadowCameraFit, InvalidBoundsDisablesShadows) {
  const AABB invalid;  // valid == false
  EXPECT_FALSE(fitDirectionalShadowCamera(invalid, {0, 0, 1}, kSize).valid);
}

TEST(ShadowCameraFit, DegenerateBoxDisablesShadows) {
  const AABB point = boxFromMinMax({1, 1, 1}, {1, 1, 1});  // zero diagonal
  EXPECT_FALSE(fitDirectionalShadowCamera(point, {0, 0, 1}, kSize).valid);
}

TEST(ShadowCameraFit, DegenerateLightDirectionDisablesShadows) {
  const AABB box = boxFromMinMax({0, 0, 0}, {1, 1, 1});
  EXPECT_FALSE(fitDirectionalShadowCamera(box, {0, 0, 0}, kSize).valid);
}

TEST(ShadowCameraFit, SubTexelShiftYieldsIdenticalFrustum) {
  // Texel snapping: a caster shift that stays inside one texel cell must produce a BYTE-IDENTICAL light matrix, so a
  // static receiver's shadow texels never crawl as other casters jitter the bounds (per-frame re-centering without
  // the snap would shift the matrix by the sub-texel delta and fail this).
  const AABB box0 = boxFromMinMax({-1, -1, 0}, {1, 1, 1});
  const glm::vec3 to_light(0.2f, 0.1f, 1.0f);
  const ShadowCameraFit f0 = fitDirectionalShadowCamera(box0, to_light, kSize);
  ASSERT_TRUE(f0.valid);
  const float s = 0.001f * f0.world_units_per_texel;  // far inside one texel cell
  const AABB box1 = boxFromMinMax(box0.min + glm::vec3(s), box0.max + glm::vec3(s));
  const ShadowCameraFit f1 = fitDirectionalShadowCamera(box1, to_light, kSize);
  ASSERT_TRUE(f1.valid);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(f0.light_view_proj[c][r], f1.light_view_proj[c][r], 1e-5f) << "m[" << c << "][" << r << "]";
    }
  }
}

TEST(ExtendAabbToGroundShadow, OverheadLightDropsFloorDirectlyBelow) {
  // Light straight up: each corner's shadow lands at the same xy, on z=0. So the box
  // only grows DOWNWARD to the floor; its xy footprint is unchanged.
  const AABB box = boxFromMinMax({0, 0, 1}, {1, 1, 2});
  const AABB out = extendAabbToGroundShadow(box, {0, 0, 1}, 0.0f);
  ASSERT_TRUE(out.valid);
  EXPECT_FLOAT_EQ(out.min.z, 0.0f);
  EXPECT_EQ(out.min.x, 0.0f);
  EXPECT_EQ(out.max.x, 1.0f);
}

TEST(ExtendAabbToGroundShadow, AngledLightExtendsFootprintAlongShadow) {
  // Light tilted toward +x+z: shadows fall toward -x, so the floor footprint must grow
  // in -x past the box. A corner at z=2 under L=normalize(1,0,1) lands ~2 units in -x.
  const AABB box = boxFromMinMax({0, 0, 1}, {1, 1, 2});
  const AABB out = extendAabbToGroundShadow(box, {1, 0, 1}, 0.0f);
  ASSERT_TRUE(out.valid);
  EXPECT_LT(out.min.x, -1.0f);          // grew well into -x to cover the cast shadow
  EXPECT_NEAR(out.min.z, 0.0f, 1e-5f);  // reaches the floor (within float rounding)
}

TEST(ExtendAabbToGroundShadow, HorizontalOrInvalidLightIsNoOp) {
  const AABB box = boxFromMinMax({0, 0, 1}, {1, 1, 2});
  EXPECT_EQ(extendAabbToGroundShadow(box, {1, 0, 0}, 0.0f).min, box.min);  // horizontal
  EXPECT_FALSE(extendAabbToGroundShadow(AABB{}, {0, 0, 1}, 0.0f).valid);   // invalid box
}

TEST(TransformedAABB, IdentityIsUnchanged) {
  const AABB box = boxFromMinMax({-1, -2, -3}, {4, 5, 6});
  const AABB out = transformedAABB(glm::mat4(1.0f), box);
  ASSERT_TRUE(out.valid);
  EXPECT_EQ(out.min, box.min);
  EXPECT_EQ(out.max, box.max);
}

TEST(TransformedAABB, TranslationShiftsBox) {
  const AABB box = boxFromMinMax({-1, -1, -1}, {1, 1, 1});
  const AABB out = transformedAABB(glm::translate(glm::mat4(1.0f), {10, 20, 30}), box);
  EXPECT_EQ(out.min, glm::vec3(9, 19, 29));
  EXPECT_EQ(out.max, glm::vec3(11, 21, 31));
}

TEST(TransformedAABB, RotationGrowsAxisAlignedExtent) {
  // A unit cube rotated 45 deg about Z has its XY footprint grow to +/-sqrt(2).
  const AABB box = boxFromMinMax({-1, -1, -1}, {1, 1, 1});
  const glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0, 0, 1));
  const AABB out = transformedAABB(r, box);
  EXPECT_NEAR(out.max.x, std::sqrt(2.0f), 1e-5f);
  EXPECT_NEAR(out.max.y, std::sqrt(2.0f), 1e-5f);
  EXPECT_NEAR(out.max.z, 1.0f, 1e-5f);  // z extent unchanged by a Z rotation
}

TEST(TransformedAABB, InvalidBoxStaysInvalid) {
  EXPECT_FALSE(transformedAABB(glm::mat4(1.0f), AABB{}).valid);
}

}  // namespace
}  // namespace pj::scene3d
