// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/frame_transforms.hpp"  // PJ::sdk::Pose
#include "pj_scene3d_core/grid_map_view.h"       // GridMapUpload
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/texture.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_widgets/Colormap.h"  // PJ::Colormap

namespace pj::scene3d {

// Draws a GridMap as a lit, colormapped heightfield surface. The geometry is
// procedural: `6 * (cols-1) * (rows-1)` vertices from gl_VertexID (no VBO, no index
// buffer), each fetching its cell corner's elevation from an R32F texture, so a new
// sample costs two texture uploads and nothing per vertex on the CPU. A triangle
// touching a NaN corner is dropped by a `flat` validity varying (holes stay holes).
// Normals are central differences of the elevation texture with one-sided / up
// fallbacks so a NaN neighbour never poisons a valid triangle. Colormap, range,
// lighting and opacity are uniform-only.
class GridMapRenderPass : public IRenderPass {
 public:
  GridMapRenderPass();
  ~GridMapRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Stage a grid for upload on the next render() (which runs with a current GL
  // context). Safe to call from the GUI thread outside paintGL. A grid with fewer
  // than two rows or columns has no quads and draws nothing.
  void setGrid(GridMapUpload upload);
  void clearGrid();
  // Why the staged grid could not be uploaded on this context (it exceeds
  // GL_MAX_TEXTURE_SIZE or the draw limits), known only after render(). The
  // payload stays staged so a recreated context retries it; the layer polls
  // this after render() to surface a status warning.
  [[nodiscard]] const std::optional<QString>& rejection() const {
    return rejection_;
  }

  void setAutoRange(bool on) {
    auto_range_ = on;
  }
  void setManualRange(float lo, float hi) {
    manual_lo_ = lo;
    manual_hi_ = hi;
  }
  void setColormap(PJ::Colormap colormap) {
    colormap_ = colormap;
  }
  void setLighting(bool on) {
    lighting_ = on;
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

#ifdef PJ_SCENE3D_TEST_HOOKS
  [[nodiscard]] bool hasStagedGridForTest() const {
    return has_grid_;
  }
  [[nodiscard]] const GridMapUpload& stagedGridForTest() const {
    return grid_;
  }
#endif

 private:
  void uploadPending();  // consume pending_full_: fill both textures (re-allocating only on a size change)
  // Fill `texture` from row-major floats; re-allocates only when `dims` (its last
  // uploaded size, updated here) differs from the staged grid.
  void uploadChannel(gl::Texture& texture, glm::uvec2& dims, const float* data);

  // Staged CPU payload, retained across context loss so releaseGL() can re-arm a
  // full re-upload without the layer re-packing.
  GridMapUpload grid_;
  bool has_grid_{false};
  bool pending_full_{false};
  std::optional<QString> rejection_;

  bool auto_range_{true};
  float manual_lo_{0.0f};
  float manual_hi_{1.0f};
  PJ::Colormap colormap_{PJ::Colormap::kTurbo};
  bool lighting_{true};
  float opacity_{1.0f};
  bool visible_{true};
  bool initialized_{false};
  GLint max_texture_size_{0};  // GL_MAX_TEXTURE_SIZE, queried once per context

  glm::uvec2 elevation_dims_{0};  // last uploaded size per texture, (0,0) = never
  glm::uvec2 color_dims_{0};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;  // attribute-less, but core profile still needs a bound VAO
  gl::Texture elevation_tex_;
  gl::Texture color_tex_;
};

}  // namespace pj::scene3d
