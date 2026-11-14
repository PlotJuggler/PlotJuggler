// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"

namespace pj::scene3d {

class MeshMapLayer;

/// The MeshMap family of the QRhi view: per layer, the de-indexed triangles of a
/// channel mesh (positions + one colour attribute: a channel value through the
/// in-shader colormap, or an RGBA8 vertex colour) and, optionally, its link
/// segments as a line list. A categorical channel arrives as RGBA8 colours,
/// coloured on the CPU from its category colours or a palette. Flat normals come from screen-space derivatives in
/// the fragment shader, which WebGL2 supports. Owns its per-layer buffers and
/// uniforms (the view's registry its pipelines); the view owns ordering and the
/// command buffer. Per frame: admit() (once), prepare() (once), then submit()
/// per ScenePass for each draw.
class MeshMapRhiPass {
 public:
  struct Draw {
    MeshMapLayer* layer = nullptr;
    bool lines = false;  ///< the link segments, not the surface
    quint32 vertex_count = 0;
    /// Translucent below full layer opacity; the surface also when a colour's alpha is below 255.
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// Registers the pipelines; false leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return opaque_pipeline_ != nullptr && translucent_pipeline_ != nullptr && line_pipeline_ != nullptr &&
           translucent_line_pipeline_ != nullptr;
  }
  /// Adopts the view's mesh-map layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<MeshMapLayer*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Drains each visible layer's pending sample and places it.
  void admit(const PlaceFn& place);
  /// Uploads changed meshes and this frame's uniforms and builds the draws.
  void prepare(const RhiFrameContext& frame, const MeshShadingParams& shading);
  /// The prepared draws of `layer` (surface, then links).
  [[nodiscard]] std::span<const Draw> drawsFor(const MeshMapLayer* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one draw; false when its pipeline is unavailable for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  [[nodiscard]] int lastLayerCount() const {
    return last_layer_count_;
  }
  [[nodiscard]] int lastVertexCount() const {
    return last_vertex_count_;
  }
  [[nodiscard]] int geometryUploadCount() const {
    return geometry_upload_count_;
  }
  /// Bytes of colour attributes uploaded since the pass was created.
  [[nodiscard]] std::uint64_t colorUploadBytes() const {
    return color_upload_bytes_;
  }

 private:
  struct LayerGpu {
    QRhiBuffer* positions = nullptr;
    quint32 positions_capacity = 0;
    QRhiBuffer* attributes = nullptr;  ///< channel values (float) or vertex colours (RGBA8)
    quint32 attributes_capacity = 0;
    QRhiBuffer* link_positions = nullptr;
    quint32 link_positions_capacity = 0;
    QRhiBuffer* link_colors = nullptr;
    quint32 link_colors_capacity = 0;
    QRhiBuffer* surface_uniforms = nullptr;
    QRhiBuffer* line_uniforms = nullptr;
    QRhiShaderResourceBindings* surface_bindings = nullptr;
    QRhiShaderResourceBindings* line_bindings = nullptr;
    std::uint64_t uploaded_geometry = ~std::uint64_t{0};
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
    quint32 surface_vertices = 0;
    quint32 link_vertices = 0;
    bool attributes_in_use = false;  ///< `attributes` holds this mesh's colours (not solid mode)
  };

  bool ensureLayerGpu(QRhi* rhi, MeshMapLayer* layer, LayerGpu& gpu);
  /// Uploads what changed since the last frame: the whole colour buffer, or only
  /// the layer's stagedColorDelta() when the pass holds the revision before it.
  /// False (render failure noted, the layer's buffers released, no upload
  /// recorded) when a buffer cannot be allocated.
  bool upload(QRhiResourceUpdateBatch* updates, QRhi* rhi, MeshMapLayer* layer, LayerGpu& gpu);
  QRhiShaderResourceBindings* createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms) const;
  static void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* render_mode_uniforms_ = nullptr;  // view-owned
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  const ScenePipeline* opaque_pipeline_ = nullptr;            // writes depth
  const ScenePipeline* translucent_pipeline_ = nullptr;       // depth test only
  const ScenePipeline* line_pipeline_ = nullptr;              // link segments
  const ScenePipeline* translucent_line_pipeline_ = nullptr;  // link segments, depth test only

  std::vector<MeshMapLayer*> layers_;
  std::unordered_map<MeshMapLayer*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<MeshMapLayer>> admitted_;
  std::vector<Draw> draws_;

  int last_layer_count_ = 0;
  int last_vertex_count_ = 0;
  int geometry_upload_count_ = 0;
  std::uint64_t color_upload_bytes_ = 0;
};

}  // namespace pj::scene3d
