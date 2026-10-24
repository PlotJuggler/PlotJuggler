// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/tf_connections.h"
#include "pj_scene3d_widgets/grid_geometry.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {

/// The view's prelude family: the ground grid (lines, or filled cells with the
/// lines drawn analytically in the cell shader, which receive the mesh shadow)
/// and the TF parent-connection lines. Drawn before
/// every layer in each scene pass, grid as data (graded) and connections as
/// annotations (tonemap bypass, annotation alpha blend). The TF triads that
/// follow the prelude belong to the pose family. The grid is built once per
/// settings change, at the fixed-frame origin, into a Static buffer; grid.vert and
/// grid_cell.vert add the origin offset (fixed-frame origin - render origin), so
/// panning, following and playback upload nothing. Grid positions and the render
/// origin (the camera focal) are floats, so the shader's float add equals the
/// double subtraction it replaces bit for bit. The TF connections (a Dynamic
/// buffer) are camera-relative like every TF placement, rebuilt with the TF
/// buffer, its revision, the fixed frame, the time or the render origin, and
/// drawn with a zero offset.
class GridRhiPass {
 public:
  struct Settings {
    bool grid_visible = true;
    GridStyle style = GridStyle::kLines;
    float extent_m = 10.0F;
    int divisions = 10;
    bool connections_visible = true;
    look::GridColors colors;  ///< from the theme; a change rebuilds the grid

    bool operator==(const Settings&) const = default;
  };

  /// Registers the line/triangle pipelines; false leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return line_pipeline_ != nullptr && triangle_pipeline_ != nullptr && connection_line_pipeline_ != nullptr;
  }

  /// The shadow-receiving grid pipeline, sampling the view's shadow map with the
  /// view's lighting uniforms. Registered with the shadow stage, released before
  /// it; false only for an incomplete registration. Optional: a GPU that cannot
  /// build it draws the cells unshadowed, then shadowReceiverFailed(), which the
  /// caller answers by turning shadows off.
  [[nodiscard]] bool createShadowReceiver(
      QRhiBuffer* lighting_uniforms, QRhiTexture* shadow_map, QRhiSampler* shadow_sampler,
      ScenePipelineRegistry& registry);
  void releaseShadowReceiver(ScenePipelineRegistry& registry);
  [[nodiscard]] bool shadowReceiverReady() const {
    return shadow_pipeline_ != nullptr;
  }
  [[nodiscard]] bool shadowReceiverFailed(const ScenePipelineRegistry& registry) const {
    return registry.optionalFailed(shadow_pipeline_);
  }

  /// Rebuilds the grid and the connection vertices whose inputs (including `tf`'s
  /// revision) changed since their last build, and takes the origin offset; `tf`
  /// may be null (no connections). The next prepare() uploads what was rebuilt.
  void build(
      const Settings& settings, const TransformBuffer* tf, const std::string& fixed_frame, PJ::Timepoint time,
      const glm::dvec3& render_origin);
  /// Forces the next build(): the view's TF buffer was replaced (a new buffer
  /// can reuse the old address and revision).
  void invalidate() {
    connection_key_.reset();
  }
  /// Uploads freshly built vertices and the origin offset, and picks the cell
  /// pipeline: the shadow receiver when `shadow_active`. When a vertex buffer
  /// cannot grow, its part draws nothing until an upload succeeds.
  void prepare(const RhiFrameContext& frame, bool shadow_active);
  /// Records the prelude: grid cells, grid lines, then connection lines.
  void submit(
      QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  /// Grid lines (line style only) plus connection lines.
  [[nodiscard]] int lastLineVertexCount() const {
    return static_cast<int>(grid_line_vertex_count_ + connection_vertices_.size());
  }
  [[nodiscard]] int lastTriangleVertexCount() const {
    return static_cast<int>(grid_cell_vertex_count_);
  }
#ifdef PJ_RHI_TEST_HOOKS
  /// Grid and connection rebuilds since initialization; cumulative.
  [[nodiscard]] std::uint64_t gridBuildCountForTest() const {
    return grid_build_count_;
  }
  [[nodiscard]] std::uint64_t connectionBuildCountForTest() const {
    return connection_build_count_;
  }
#endif

 private:
  struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
  };

  struct ConnectionKey {
    bool visible = true;
    const TransformBuffer* tf = nullptr;
    std::uint64_t tf_revision = 0;
    std::string fixed_frame;
    PJ::Timepoint time{};
    glm::dvec3 render_origin{0.0};

    bool operator==(const ConnectionKey&) const = default;
  };

  static QRhiVertexInputLayout vertexLayout();
  static void appendVertex(std::vector<Vertex>& out, const glm::dvec3& point, const glm::vec4& color);

  RhiPassSetup setup_;  // view-owned uniforms; the shadow receiver is built later against it
  QRhiShaderResourceBindings* shader_resources_ = nullptr;
  QRhiShaderResourceBindings* annotation_shader_resources_ = nullptr;
  QRhiShaderResourceBindings* shadow_shader_resources_ = nullptr;
  const ScenePipeline* line_pipeline_ = nullptr;
  const ScenePipeline* triangle_pipeline_ = nullptr;
  // The line pipeline with the annotation alpha blend: its HDR variant draws the
  // connections (the direct pass and depth replay use line_pipeline_'s).
  const ScenePipeline* connection_line_pipeline_ = nullptr;
  const ScenePipeline* shadow_pipeline_ = nullptr;
  // binding 3 of grid.vert/grid_cell.*: the grid's offset and line parameters,
  // and a zero offset for the connections
  QRhiBuffer* origin_offset_uniforms_ = nullptr;
  QRhiBuffer* zero_offset_uniforms_ = nullptr;
  std::array<float, 8> grid_uniforms_{};  // origin offset + cell-edge phase, then line colour + cell side
  QRhiBuffer* grid_buffer_ = nullptr;     // Static: cells, then lines
  QRhiBuffer* connection_buffer_ = nullptr;
  quint32 grid_capacity_ = 0;
  quint32 connection_capacity_ = 0;
  bool grid_ready_ = false;
  bool connections_ready_ = false;
  std::optional<Settings> grid_key_;  ///< the settings of the built grid
  std::optional<ConnectionKey> connection_key_;
  bool grid_upload_pending_ = true;
  bool connection_upload_pending_ = true;
  bool receive_shadow_ = false;
  std::uint64_t grid_build_count_ = 0;
  std::uint64_t connection_build_count_ = 0;

  std::vector<Vertex> grid_vertices_;  // cells, then lines
  std::vector<Vertex> connection_vertices_;
  std::vector<TfConnectionSegment> segments_;
  quint32 grid_cell_vertex_count_ = 0;
  quint32 grid_line_vertex_count_ = 0;
};

}  // namespace pj::scene3d
