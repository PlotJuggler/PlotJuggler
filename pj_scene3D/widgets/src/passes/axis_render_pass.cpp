// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/axis_render_pass.h"

#include <array>
#include <glm/glm.hpp>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

// Derive arrow geometry from the user-facing axis_length. Keeps the silhouette
// proportional regardless of the length setting.
ArrowGizmo::Params paramsForLength(float length) {
  ArrowGizmo::Params p;
  p.length = length;
  p.shaft_radius = length * 0.04f;  // shaft = 4% of length
  p.head_length = length * 0.30f;   // cone is 30% of length
  p.head_radius = length * 0.10f;   // cone base radius = 10% of length
  p.segments = 16;
  return p;
}

}  // namespace

void AxisRenderPass::initializeGL() {
  arrow_.initializeGL(paramsForLength(axis_length_));
  initialized_ = true;
  // initializeGL already built from the latest axis_length_.
  gizmo_dirty_ = false;
}

void AxisRenderPass::releaseGL() {
  arrow_.releaseGL();
  initialized_ = false;
}

void AxisRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!initialized_) {
    return;
  }

  // Flush a deferred length change (setAxisLength) under the now-current context.
  if (gizmo_dirty_) {
    arrow_.rebuild(paramsForLength(axis_length_));
    gizmo_dirty_ = false;
  }

  // The alpha channel carries the Part-C "Gizmos opacity" (annotation coverage).
  const std::array<glm::vec4, 3> colors{
      glm::vec4{look::kAxisTriadX, opacity_}, glm::vec4{look::kAxisTriadY, opacity_},
      glm::vec4{look::kAxisTriadZ, opacity_}};

  // Brightened copy for the hovered frame's triad (luminance highlight), built
  // only when a frame is actually hovered. Alpha (the gizmo-opacity marker) is
  // preserved; only the RGB luminance is boosted.
  const bool has_highlight = !highlighted_frame_.empty();
  std::array<glm::vec4, 3> highlight_colors{};
  if (has_highlight) {
    for (std::size_t axis = 0; axis < colors.size(); ++axis) {
      highlight_colors[axis] =
          glm::vec4(glm::min(glm::vec3(colors[axis]) * look::kHighlightGain, glm::vec3(1.0f)), colors[axis].a);
    }
  }

  // Solid 3D arrows participate in normal depth ordering — back ones get
  // occluded by front ones, and arrows hide behind opaque scene geometry.
  withGlFunctions([](auto& f) {
    f.glEnable(GL_DEPTH_TEST);
    f.glLineWidth(1.0f);
  });

  // Reuse the scratch vector's capacity across frames instead of allocating a
  // fresh frame-name vector every paint (L.54). Bind the arrow program ONCE
  // for the whole pass: N frames now cost 1 program bind, not 3N (L.54/L.93).
  frame_ctx.tf.getAllFrames(frames_scratch_);
  arrow_.bindForRender();
  for (const std::string& frame : frames_scratch_) {
    const auto transform = frame_ctx.lookup(frame);
    if (!transform.has_value()) {
      continue;
    }
    const glm::mat4 frame_model = glm::mat4(transform->matrix());
    const bool highlighted = has_highlight && frame == highlighted_frame_;
    renderTriadBound(
        arrow_, view_params.proj, view_params.view, frame_model, glm::mat4{1.0f},
        highlighted ? highlight_colors : colors);
  }
  arrow_.unbindAfterRender();
}

void AxisRenderPass::setAxisLength(float length_m) {
  if (length_m == axis_length_) {
    return;
  }
  axis_length_ = length_m;
  // Defer the GL rebuild to render(): this setter is reachable from a GUI slot
  // with no (or a foreign) GL context current. initializeGL builds from the
  // latest length, so dirtying only matters once we are already initialized.
  gizmo_dirty_ = initialized_;
}

float AxisRenderPass::axisLength() const noexcept {
  return axis_length_;
}

}  // namespace pj::scene3d
