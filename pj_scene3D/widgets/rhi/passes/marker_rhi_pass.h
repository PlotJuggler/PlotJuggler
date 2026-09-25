// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

class SceneEntitiesLayer;

/// The SceneEntities marker family of the QRhi view: instanced unit primitives
/// (cube + its edge overlay, sphere, cylinder/cone, arrow/axes) and the
/// expanded line/triangle stream batches, each layer's geometry expanded into
/// per-frame instance and stream buffers this pass owns. The layer's model
/// primitives belong to ModelRhiPass. Per frame the view calls admit() (once),
/// prepare() (once), then submit() per ScenePass for each draw, and
/// submitShadowCasters() into its shadow pass. Solid cubes, spheres, cylinders
/// and arrows of non-wireframe layers cast shadows (the GL MarkerRenderPass
/// casts none); axes, edge overlays, lines and triangle lists never do.
class MarkerRhiPass {
 public:
  enum class Kind : std::uint8_t { kInstanceTriangles, kInstanceLines, kStreamTriangles, kStreamLines };

  /// One prepared range of a layer: `offset`/`count` index the instance buffer
  /// (instanced kinds, `mesh` selects the unit mesh) or the stream buffer.
  struct Draw {
    SceneEntitiesLayer* layer = nullptr;
    Kind kind = Kind::kInstanceTriangles;
    std::uint32_t mesh = 0;
    quint32 offset = 0;
    quint32 count = 0;
    bool cull_back = false;
    bool depth_write = true;
    /// Translucent when it skips depth writes; the edge overlay of translucent
    /// cubes writes depth but stays with its fill, drawn right after it.
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// Binds the setup's view uniforms (binding 0), data render mode (binding 8
  /// of the instanced primitives) and annotation render mode (binding 8 of the
  /// stream batches). False leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const;
  void registerPipelines(ScenePipelineRegistry& registry) const;
  void setLayers(const std::vector<SceneEntitiesLayer*>& layers);

  /// Runs each visible layer's prepareForRender(), places its frames, applies
  /// the per-layer and per-view instance/stream budgets (row warnings on
  /// rejection) and adds its fixed-frame bounds to `bounds`.
  void admit(const PlaceFn& place, SceneBoundsCollector& bounds);
  /// Expands the admitted layers into camera-relative instances and stream
  /// vertices and uploads them (and the unit meshes once); when a buffer cannot
  /// grow the frame has no marker draws. `cast_shadows` also collects the
  /// shadow-caster ranges and their bounds.
  void prepare(const RhiFrameContext& frame, bool cast_shadows = false);
  /// The prepared ranges of `layer` in native family order.
  [[nodiscard]] std::span<const Draw> drawsFor(const SceneEntitiesLayer* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one range; false when nothing could be recorded for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  // ---- Shadow casting (the shadow target is owned and begun by the view).
  [[nodiscard]] bool hasShadowCasters() const {
    return !casters_.empty();
  }
  /// This frame's caster bounds, relative to the prepare() render origin.
  [[nodiscard]] AABB shadowCasterBounds() const {
    return caster_bounds_;
  }
  /// Creates the depth-only caster pipeline for the view's shadow target, whose
  /// `bindings` (binding 0: light view-projection) it binds; true when it exists.
  bool ensureShadowPipeline(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, QRhiShaderResourceBindings* bindings);
  void releaseShadowResources();
  /// Records every caster range (instanced, from the colour instance buffer).
  void submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const;
  /// Notes `error` on every layer with shadow casters this frame.
  void noteShadowFailure(const QString& error);
  [[nodiscard]] int lastShadowDrawCount() const {
    return static_cast<int>(casters_.size());
  }
  [[nodiscard]] int lastShadowTriangleCount() const;

  void release();

  [[nodiscard]] int lastLayerCount() const {
    return last_layer_count_;
  }
  [[nodiscard]] int lastInstanceCount() const {
    return last_instance_count_;
  }
  [[nodiscard]] int lastStreamVertexCount() const {
    return last_stream_vertex_count_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }

  /// One instanced primitive: 96 bytes, the marker.vert per-instance layout.
  struct Instance {
    std::array<float, 16> model{};
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
    float bottom_scale = 1.0F;  ///< cone taper at local z = -0.5
    float top_scale = 1.0F;     ///< cone taper at local z = +0.5
    float unused0 = 0.0F;
    float unused1 = 0.0F;
  };
  /// One stream vertex (scene.vert layout): camera-relative position + colour.
  struct StreamVertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
  };

 private:
  struct MeshGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* triangle_index_buffer = nullptr;
    QRhiBuffer* edge_index_buffer = nullptr;
    quint32 triangle_index_count = 0;
    quint32 edge_index_count = 0;
  };
  struct Admitted {
    SceneEntitiesLayer* layer = nullptr;
    std::size_t first_frame = 0;  ///< the layer's frames in placements_
    std::size_t frame_count = 0;
    std::uint64_t instances = 0;
    std::uint64_t stream_vertices = 0;
  };
  struct Caster {
    SceneEntitiesLayer* layer = nullptr;
    std::uint32_t mesh = 0;
    quint32 offset = 0;
    quint32 count = 0;
  };

  QRhiGraphicsPipeline* instancePipeline(const Draw& draw) const;

  /// The placement of `frame_index` of `admitted`, or nullptr when unplaced.
  [[nodiscard]] const Transform* placement(const Admitted& admitted, std::uint32_t frame_index) const;

  QRhiShaderResourceBindings* data_bindings_ = nullptr;
  QRhiShaderResourceBindings* annotation_bindings_ = nullptr;
  QRhiGraphicsPipeline* triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_cull_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_cull_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* stream_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* stream_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* stream_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* stream_triangle_no_depth_pipeline_ = nullptr;
  std::array<MeshGpu, 5> meshes_{};
  bool mesh_upload_pending_ = false;
  QRhiBuffer* instance_buffer_ = nullptr;
  QRhiBuffer* line_buffer_ = nullptr;
  QRhiBuffer* triangle_buffer_ = nullptr;
  quint32 instance_capacity_ = 0;
  quint32 line_capacity_ = 0;
  quint32 triangle_capacity_ = 0;

  std::vector<SceneEntitiesLayer*> layers_;
  std::vector<Admitted> admitted_;
  std::vector<std::optional<Transform>> placements_;  ///< every admitted layer's frames, contiguous per layer
  std::vector<Draw> draws_;                           ///< contiguous per layer, in admission order
  std::vector<Instance> instances_;
  std::vector<StreamVertex> line_vertices_;
  std::vector<StreamVertex> triangle_vertices_;
  std::vector<Caster> casters_;
  AABB caster_bounds_;
  QRhiGraphicsPipeline* shadow_pipeline_ = nullptr;
  QRhiShaderResourceBindings* shadow_bindings_ = nullptr;  // view-owned

  int last_layer_count_ = 0;
  int last_instance_count_ = 0;
  int last_stream_vertex_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
