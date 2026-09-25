#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <memory>

#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

// Draws the reference ground grid on the z=0 plane at the fixed-frame origin.
// TF-independent (ignores FrameContext); extent, divisions, style, and colors
// are configurable at runtime (geometry regenerates lazily on next render).
//
// Two styles share one shader and pick one vertex buffer each:
//   kLines       — the line buffer, drawn as GL_LINES in the line color.
//   kFilledCells — the full two-tone checkerboard (every cell, both tones) with
//                  the grid lines synthesized *procedurally* in the fragment
//                  shader (distance-to-cell-edge, anti-aliased via fwidth). The
//                  lines live in the same surface as the fill — no separate
//                  coplanar pass — so they can't z-fight it at grazing angles.
class GridRenderPass : public IRenderPass {
 public:
  using Style = GridStyle;

  void initializeGL() override;
  void render(const ViewParams& view_params, [[maybe_unused]] const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // The grid-line color (used by kLines, and the lines overlaid in kFilledCells).
  void setColor(const glm::vec3& color);
  // The two alternating tile tones for the kFilledCells checkerboard.
  void setCellColors(const glm::vec3& tone_a, const glm::vec3& tone_b);
  void setExtentMetres(float extent_m);
  void setDivisions(int divisions);  // cells per side, clamped to [1, 200]
  void setStyle(Style style);

  [[nodiscard]] float extentMetres() const {
    return extent_m_;
  }
  [[nodiscard]] int divisions() const {
    return divisions_;
  }
  [[nodiscard]] Style style() const {
    return style_;
  }

 private:
  // (Re)build + upload the vertex buffers for the current extent/divisions/style.
  // Requires a current GL context (called from initializeGL/render). kLines only
  // needs the line buffer; kFilledCells builds both the cell and line buffers.
  void rebuildGeometry();

  // Set from the theme every frame before render() (SceneViewWidget::paintGL).
  glm::vec3 color_{0.0f};
  glm::vec3 cell_color_a_{0.0f};
  glm::vec3 cell_color_b_{0.0f};
  float extent_m_{10.0f};
  int divisions_{10};
  Style style_{Style::kLines};
  bool geometry_dirty_{true};
  int line_vertex_count_{0};
  int cell_vertex_count_{0};
  bool initialized_{false};
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray line_vao_;
  gl::Buffer line_vbo_;
  gl::VertexArray cell_vao_;
  gl::Buffer cell_vbo_;
};

}  // namespace pj::scene3d
