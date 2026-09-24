#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMatrix4x4>

namespace pj::scene3d {

/// The three per-backend QRhi conventions a screen-space pass depends on, as
/// reported by `QRhi::isYUpInNDC()`, `isYUpInFramebuffer()` and
/// `isClipDepthZeroToOne()`. Plain bools so the math is testable without a QRhi.
struct RhiConventions {
  bool y_up_in_ndc = true;
  bool y_up_in_framebuffer = true;
  bool clip_depth_zero_to_one = false;
};

/// True where texture row 0 is not at NDC y = -1 (D3D, Metal): a fullscreen
/// triangle must then flip v to sample a render target upright.
[[nodiscard]] inline bool fullscreenNeedsYFlip(const RhiConventions& conventions) {
  return conventions.y_up_in_ndc != conventions.y_up_in_framebuffer;
}

/// +1 where texture v grows with view-space y (OpenGL), -1 where it grows
/// downwards (Vulkan, D3D, Metal). A normal rebuilt from depth texels as
/// cross(d/du, d/dv) must be multiplied by it to face the camera, under both
/// perspective and orthographic projection.
[[nodiscard]] inline float textureVSign(const RhiConventions& conventions) {
  return conventions.y_up_in_framebuffer ? 1.0F : -1.0F;
}

/// Maps corrected clip space (after `QRhi::clipSpaceCorrMatrix()`) to the texture
/// space of a render target: (u, v, depth as stored in the depth buffer). Folding
/// this into the matrices sent to shaders keeps them free of GL-only `*0.5+0.5`
/// and `*2-1` remaps. Apply before the perspective divide; it is affine.
[[nodiscard]] inline QMatrix4x4 clipToTextureMatrix(const RhiConventions& conventions) {
  const float v_scale = fullscreenNeedsYFlip(conventions) ? -0.5F : 0.5F;
  const float depth_scale = conventions.clip_depth_zero_to_one ? 1.0F : 0.5F;
  const float depth_bias = conventions.clip_depth_zero_to_one ? 0.0F : 0.5F;
  // Row-major scalar constructor.
  return QMatrix4x4(
      0.5F, 0.0F, 0.0F, 0.5F,               //
      0.0F, v_scale, 0.0F, 0.5F,            //
      0.0F, 0.0F, depth_scale, depth_bias,  //
      0.0F, 0.0F, 0.0F, 1.0F);
}

}  // namespace pj::scene3d
