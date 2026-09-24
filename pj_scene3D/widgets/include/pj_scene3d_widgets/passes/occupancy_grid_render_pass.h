// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms.hpp"            // PJ::sdk::Pose
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"  // ReconstructedGrid, CellRect
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Draws a reconstructed OccupancyGrid as a textured quad placed in the grid's
// frame (via TF) at its origin Pose, sized width*resolution x height*resolution
// in the local xy-plane. Cell bytes (-1 unknown, 0..100 occupancy %) live in a
// single-channel R8 texture; the fragment shader maps them per the color scheme
// and discards unknown cells (transparent).
class OccupancyGridRenderPass : public IRenderPass {
 public:
  using ColorScheme = OccupancyColorScheme;

  OccupancyGridRenderPass();
  ~OccupancyGridRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Replace the grid being shown. Stages a texture upload for the next render()
  // (which runs with a current GL context): a full glTexImage2D when
  // `full_rebuild` is set or the dims changed, otherwise an incremental
  // glTexSubImage2D of each `dirty_rects` entry. Safe to call from the GUI
  // thread outside paintGL.
  void setGrid(const ReconstructedGrid& grid, bool full_rebuild, const std::vector<CellRect>& dirty_rects);
  void clearGrid();

  void setColorScheme(ColorScheme scheme) {
    color_scheme_ = scheme;
  }
  void setOpacity(float opacity) {
    opacity_ = opacity;
  }
  void setVisible(bool visible) {
    visible_ = visible;
  }
  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

 private:
  std::string frame_id_;
  PJ::sdk::Pose origin_;
  double resolution_{0.0};
  uint32_t width_{0};
  uint32_t height_{0};
  bool has_grid_{false};
  bool pending_full_{false};             // staged: full glTexImage2D on next render()
  std::vector<CellRect> pending_rects_;  // staged: incremental glTexSubImage2D rects
  std::vector<uint8_t> pending_cells_;   // full current grid; sub-rect uploads index into it

  ColorScheme color_scheme_{ColorScheme::kMap};
  float opacity_{0.7f};
  bool visible_{true};
  bool initialized_{false};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  gl::Texture texture_;
};

}  // namespace pj::scene3d
