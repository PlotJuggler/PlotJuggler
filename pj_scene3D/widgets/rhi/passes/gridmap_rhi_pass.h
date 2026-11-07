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

class GridMapLayer;

/// The GridMap family of the QRhi view: a lit, colormapped heightfield per layer,
/// drawn attribute-less (6 procedural vertices per quad, elevation and colour
/// fetched from two R32F textures) with in-shader colormaps (colormap.glslinc). Owns its
/// per-layer textures and uniforms (the view's registry its pipelines); the view owns ordering and the
/// command buffer. Per frame: admit() (once), prepare() (once), then submit()
/// per ScenePass for each draw.
class GridMapRhiPass {
 public:
  struct Draw {
    GridMapLayer* layer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 vertex_count = 0;
    DrawClass draw_class = DrawClass::kOpaque;  ///< translucent below full layer opacity
  };

  /// Registers the pipelines; false leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return opaque_pipeline_ != nullptr && translucent_pipeline_ != nullptr;
  }
  /// Adopts the view's grid-map layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<GridMapLayer*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Drains each visible layer's pending sample and places it.
  void admit(const PlaceFn& place);
  /// Uploads changed grids and this frame's uniforms and builds the draws.
  void prepare(const RhiFrameContext& frame, const MeshShadingParams& shading);
  /// The prepared draw of `layer` (none or one).
  [[nodiscard]] std::span<const Draw> drawsFor(const GridMapLayer* layer) const {
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
  [[nodiscard]] int uploadCount() const {
    return upload_count_;
  }

 private:
  struct LayerGpu {
    QRhiTexture* elevation = nullptr;
    QRhiTexture* color = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 width = 0;
    quint32 height = 0;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };

  bool ensureLayerGpu(QRhi* rhi, GridMapLayer* layer, LayerGpu& gpu);
  /// The per-layer SRB shape (also the pipelines' layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createLayerBindings(
      QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* elevation, QRhiTexture* color) const;
  static void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* render_mode_uniforms_ = nullptr;  // view-owned
  QRhiSampler* texel_sampler_ = nullptr;        // nearest: R32F is not filterable on WebGL2
  QRhiTexture* layout_texture_ = nullptr;
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  const ScenePipeline* opaque_pipeline_ = nullptr;       // writes depth
  const ScenePipeline* translucent_pipeline_ = nullptr;  // depth test only

  std::vector<GridMapLayer*> layers_;
  std::unordered_map<GridMapLayer*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<GridMapLayer>> admitted_;
  std::vector<Draw> draws_;

  int last_layer_count_ = 0;
  int last_vertex_count_ = 0;
  int upload_count_ = 0;
};

}  // namespace pj::scene3d
