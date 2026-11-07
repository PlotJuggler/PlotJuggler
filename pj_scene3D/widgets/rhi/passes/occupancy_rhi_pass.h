// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

class OccupancyGridLayer;

/// The occupancy-grid family of the QRhi view: one R8 texture per layer drawn
/// as a textured unit quad scaled to the grid. It owns its layout SRB, quad,
/// per-layer GPU state and counters (the view's registry its pipeline); the view owns ordering, budgets'
/// frame lifetime and the command buffer. Per frame the view calls, in order:
/// admit() (once), prepare() (once), then submit() per ScenePass for each draw.
class OccupancyRhiPass {
 public:
  struct Draw {
    OccupancyGridLayer* layer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    DrawClass draw_class = DrawClass::kOpaque;  ///< translucent below full layer opacity
  };

  /// Registers the pipeline and creates the shared resources; false leaves the pass unusable
  /// (the view then releases everything, as for any other family).
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return pipeline_ != nullptr;
  }
  /// Adopts the view's occupancy layers; drops GPU state of layers that left or
  /// are hidden.
  void setLayers(const std::vector<OccupancyGridLayer*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Runs each visible layer's prepareForRender() against the per-view cell
  /// budget, places it, and records the family's fixed-frame bounds (lastBounds()).
  void admit(const PlaceFn& place);
  /// Uploads admitted layers' textures (full or dirty rects) and uniforms and
  /// builds this frame's draws.
  void prepare(const RhiFrameContext& frame);
  /// The prepared draw of `layer` (none or one).
  [[nodiscard]] std::span<const Draw> drawsFor(const OccupancyGridLayer* layer) const {
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
  [[nodiscard]] int lastCellCount() const {
    return last_cell_count_;
  }
  [[nodiscard]] int fullUploadCount() const {
    return full_upload_count_;
  }
  [[nodiscard]] int partialUploadCount() const {
    return partial_upload_count_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }

 private:
  struct LayerGpu {
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 width = 0;
    quint32 height = 0;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };

  bool ensureLayerGpu(QRhi* owner, OccupancyGridLayer* layer, LayerGpu& gpu, bool& texture_recreated);
  /// The per-layer SRB shape (also the pipeline's layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* texture) const;
  static void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* render_mode_uniforms_ = nullptr;  // view-owned
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  QRhiTexture* layout_texture_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  QRhiBuffer* quad_buffer_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  const ScenePipeline* pipeline_ = nullptr;

  std::vector<OccupancyGridLayer*> layers_;
  std::unordered_map<OccupancyGridLayer*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<OccupancyGridLayer>> admitted_;
  std::vector<Draw> draws_;

  int last_layer_count_ = 0;
  int last_cell_count_ = 0;
  int full_upload_count_ = 0;
  int partial_upload_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
