// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/axis_overlay_pass.h"

#include <array>
#include <glm/gtc/matrix_transform.hpp>

#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

void AxisOverlayPass::setArrowParams(const ArrowGizmo::Params& params) {
  arrow_params_ = params;
  // Defer the GL rebuild to render(): this setter may run from a GUI slot with
  // no (or a foreign) GL context current. initializeGL consumes arrow_params_,
  // so dirtying only matters once we are already initialized.
  params_dirty_ = initialized_;
}

void AxisOverlayPass::initializeGL() {
  if (initialized_) {
    return;
  }
  arrow_.initializeGL(arrow_params_);
  initialized_ = true;
  // initializeGL already built from the latest arrow_params_.
  params_dirty_ = false;
}

void AxisOverlayPass::releaseGL() {
  arrow_.releaseGL();
  initialized_ = false;
}

void AxisOverlayPass::render(const ViewParams& view_params, const FrameContext& /*frame_ctx*/) {
  if (!initialized_) {
    return;
  }

  // Flush a deferred params change (setArrowParams) under the now-current context.
  if (params_dirty_) {
    arrow_.rebuild(arrow_params_);
    params_dirty_ = false;
  }

  // Save the outer viewport so we can restore it after drawing the HUD.
  std::array<GLint, 4> saved_vp{};
  withGlFunctions([&](auto& f) { f.glGetIntegerv(GL_VIEWPORT, saved_vp.data()); });

  // Outer viewport is framebuffer pixels; size_px_ / margin_px_ are widget
  // pixels. ViewParams.viewport_height_px gives us the widget-pixel height,
  // so dpr = framebuffer_h / widget_h. Assume isotropic dpr.
  const float dpr = view_params.viewport_height_px > 0
                        ? static_cast<float>(saved_vp[3]) / static_cast<float>(view_params.viewport_height_px)
                        : 1.0f;
  const int size_fb = static_cast<int>(static_cast<float>(size_px_) * dpr);
  const int margin_fb = static_cast<int>(static_cast<float>(margin_px_) * dpr);

  // GL origin is bottom-left. Translate the corner choice into (x, y).
  int x = saved_vp[0] + margin_fb;
  int y = saved_vp[1] + margin_fb;
  switch (corner_) {
    case Corner::kTopLeft:
      x = saved_vp[0] + margin_fb;
      y = saved_vp[1] + saved_vp[3] - size_fb - margin_fb;
      break;
    case Corner::kTopRight:
      x = saved_vp[0] + saved_vp[2] - size_fb - margin_fb;
      y = saved_vp[1] + saved_vp[3] - size_fb - margin_fb;
      break;
    case Corner::kBottomLeft:
      // already set above
      break;
    case Corner::kBottomRight:
      x = saved_vp[0] + saved_vp[2] - size_fb - margin_fb;
      y = saved_vp[1] + margin_fb;
      break;
  }

  // Solid arrows sit directly on top of the scene — no background. Clear
  // the depth inside the small viewport so arrows occlude each other
  // independently of scene depth, then restore the outer viewport at the end.
  withGlFunctions([&](auto& f) {
    f.glViewport(x, y, size_fb, size_fb);
    f.glEnable(GL_DEPTH_TEST);
    // Scissor so the depth clear doesn't bleed past the HUD viewport into
    // the rest of the framebuffer.
    f.glEnable(GL_SCISSOR_TEST);
    f.glScissor(x, y, size_fb, size_fb);
    f.glClear(GL_DEPTH_BUFFER_BIT);
    f.glDisable(GL_SCISSOR_TEST);
  });

  const glm::mat4 ortho = glm::ortho(-1.3f, 1.3f, -1.3f, 1.3f, -10.0f, 10.0f);
  const glm::mat4 view = glm::mat4(glm::mat3(view_params.view));

  // View is the scene camera's rotation only, so the triad tracks orbit motion.
  // renderTriadBound owns the +Y/+Z arm rotations (shared with the axis pass).
  const std::array<glm::vec4, 3> colors{
      glm::vec4(look::kHudTriadX, 1.0f), glm::vec4(look::kHudTriadY, 1.0f), glm::vec4(look::kHudTriadZ, 1.0f)};

  arrow_.bindForRender();
  renderTriadBound(arrow_, ortho, view, glm::mat4{1.0f}, glm::mat4{1.0f}, colors);
  arrow_.unbindAfterRender();

  // Restore the outer viewport for any subsequent passes.
  withGlFunctions([&](auto& f) { f.glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]); });
}

}  // namespace pj::scene3d
