// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/tf_connections.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"

namespace pj::scene3d {

class TransformBuffer;

/// The view's prelude family: the ground grid (lines, or filled cells that
/// receive the mesh shadow) and the TF parent-connection lines. Drawn before
/// every layer in each scene pass, grid as data (graded) and connections as
/// annotations (tonemap bypass, annotation alpha blend). The TF triads that
/// follow the prelude belong to the pose family. The vertices are rebuilt and
/// re-uploaded only when their inputs change.
class GridRhiPass {
 public:
  struct Settings {
    bool grid_visible = true;
    GridStyle style = GridStyle::kLines;
    float extent_m = 10.0F;
    int divisions = 10;
    bool connections_visible = true;

    bool operator==(const Settings&) const = default;
  };

  /// Creates the line/triangle pipelines; false leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return line_pipeline_ != nullptr && triangle_pipeline_ != nullptr && connection_line_pipeline_ != nullptr;
  }
  void registerPipelines(ScenePipelineRegistry& registry) const;

  /// The shadow-receiving grid pipeline, sampling the view's shadow map with the
  /// view's lighting uniforms. Built with the shadow stage, released before it.
  [[nodiscard]] bool createShadowReceiver(
      QRhiBuffer* lighting_uniforms, QRhiTexture* shadow_map, QRhiSampler* shadow_sampler,
      ScenePipelineRegistry& registry);
  void releaseShadowReceiver(ScenePipelineRegistry& registry);
  [[nodiscard]] bool shadowReceiverReady() const {
    return shadow_pipeline_ != nullptr;
  }

  /// Builds the grid and connection vertices relative to `render_origin`, unless
  /// every input (including `tf`'s revision) matches the last build; `tf` may be
  /// null (no connections). The next prepare() uploads what was rebuilt.
  void build(
      const Settings& settings, const TransformBuffer* tf, const std::string& fixed_frame, PJ::Timepoint time,
      const glm::dvec3& render_origin);
  /// Forces the next build(): the view's TF buffer was replaced (a new buffer
  /// can reuse the old address and revision).
  void invalidate() {
    built_key_.reset();
  }
  /// Uploads freshly built vertices and picks the cell pipeline: the shadow
  /// receiver when `shadow_active`. When a vertex buffer cannot grow the pass
  /// draws nothing until an upload succeeds.
  void prepare(const RhiFrameContext& frame, bool shadow_active);
  /// Records the prelude: grid cells, grid lines, then connection lines.
  void submit(
      QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  [[nodiscard]] int lastLineVertexCount() const {
    return static_cast<int>(line_vertices_.size());
  }
  [[nodiscard]] int lastTriangleVertexCount() const {
    return static_cast<int>(triangle_vertices_.size());
  }

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

  struct BuildKey {
    Settings settings;
    const TransformBuffer* tf = nullptr;
    std::uint64_t tf_revision = 0;
    std::string fixed_frame;
    PJ::Timepoint time{};
    glm::dvec3 render_origin{0.0};

    bool operator==(const BuildKey&) const = default;
  };

  static QRhiVertexInputLayout vertexLayout();
  static void appendVertex(std::vector<Vertex>& out, const glm::dvec3& point, const glm::vec4& color);

  RhiPassSetup setup_;  // view-owned uniforms; the shadow receiver is built later against it
  QRhiShaderResourceBindings* shader_resources_ = nullptr;
  QRhiShaderResourceBindings* annotation_shader_resources_ = nullptr;
  QRhiShaderResourceBindings* shadow_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_pipeline_ = nullptr;
  // The line pipeline with the annotation alpha blend: its HDR clone draws the
  // connections (the direct pass and depth replay use line_pipeline_'s).
  QRhiGraphicsPipeline* connection_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* shadow_pipeline_ = nullptr;
  QRhiBuffer* line_buffer_ = nullptr;
  QRhiBuffer* triangle_buffer_ = nullptr;
  quint32 line_capacity_ = 0;
  quint32 triangle_capacity_ = 0;
  bool buffers_ready_ = false;
  std::optional<BuildKey> built_key_;
  bool upload_pending_ = true;
  bool receive_shadow_ = false;

  std::vector<Vertex> line_vertices_;  // grid lines, then connection lines
  std::vector<Vertex> triangle_vertices_;
  std::vector<TfConnectionSegment> segments_;
  quint32 grid_line_vertex_count_ = 0;
};

}  // namespace pj::scene3d
