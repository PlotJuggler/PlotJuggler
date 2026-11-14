// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <glm/glm.hpp>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_core/finite_range.h"
#include "pj_scene3d_widgets/channel_coloring.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

/// The camera and light terms of the lit colormapped surfaces (gridmap.frag and
/// meshmap.frag shade alike: ambient + key light + a headlight fill, without
/// AO), the same for every layer of a frame.
struct LitSurfaceFrame {
  std::array<float, 4> eye{};     ///< camera position, relative to the render origin
  std::array<float, 4> view_z{};  ///< towards the viewer
  std::array<float, 4> key_dir{};
  std::array<float, 3> weights{};  ///< ambient, key, fill
};

[[nodiscard]] inline LitSurfaceFrame litSurfaceFrame(const RhiFrameContext& frame, const MeshShadingParams& shading) {
  const glm::mat4 view_to_scene = glm::inverse(frame.view);
  const glm::vec3 eye(view_to_scene[3]);
  const glm::vec3 view_z = glm::normalize(glm::vec3(view_to_scene[2]));
  const float key_length = glm::length(shading.key_light_dir);
  const glm::vec3 key_dir = key_length > 1e-6F ? shading.key_light_dir / key_length : glm::vec3(0.0F, 0.0F, 1.0F);
  return {
      .eye = {eye.x, eye.y, eye.z, 0.0F},
      .view_z = {view_z.x, view_z.y, view_z.z, 0.0F},
      .key_dir = {key_dir.x, key_dir.y, key_dir.z, 0.0F},
      .weights = {
          look::kLitSurfaceAmbient * shading.ambient_scale, look::kLitSurfaceDirect * shading.direct_scale,
          look::kLitSurfaceFill * shading.fill_light_scale}};
}

/// Fills the fields the two surfaces' uniform blocks share: `eye`, `view_z`,
/// `key_dir`, `color_params` (the colormap range of `data` under `coloring`,
/// the colormap id, `opacity`) and `light_params` (the weights, then the
/// `lighting` switch).
template <typename Uniforms>
void fillLitSurfaceUniforms(
    Uniforms& uniforms, const LitSurfaceFrame& frame, const ChannelColoring& coloring, const FiniteRange& data,
    float opacity, bool lighting) {
  const auto [color_lo, color_hi] = coloring.effectiveRange(data);
  uniforms.eye = frame.eye;
  uniforms.view_z = frame.view_z;
  uniforms.key_dir = frame.key_dir;
  uniforms.color_params = {color_lo, color_hi, static_cast<float>(coloring.colormap()), opacity};
  uniforms.light_params = {frame.weights[0], frame.weights[1], frame.weights[2], lighting ? 1.0F : 0.0F};
}

}  // namespace pj::scene3d
