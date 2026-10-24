// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

class VoxelGridLayer;

/// The dense voxel-grid family of the QRhi view: one R32F or RGBA8 3D texture
/// per layer, drawn as ONE instanced draw of the shared unit cube with
/// column*row*slice instances (the vertex shader fetches each voxel, evaluates
/// the draw predicate and degenerate-clips culled voxels). Voxels write depth
/// and are translucent below full layer opacity. Needs QRhi::ThreeDimensionalTextures:
/// without it initialize() is not called and admit() rejects every layer with a
/// visible warning. Per frame the view calls admit() (once), prepare() (once),
/// then submit() per ScenePass for each draw.
class VoxelRhiPass {
 public:
  struct Draw {
    VoxelGridLayer* layer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_count = 0;
    DrawClass draw_class = DrawClass::kOpaque;  ///< translucent below full layer opacity
  };

  /// Registers the pipeline and creates the colormap LUT (binding 2); draws the setup's
  /// unit cube. `max_3d_texture_size` is the backend's per-dimension 3D texture
  /// limit.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup, int max_3d_texture_size);
  [[nodiscard]] bool ready() const {
    return pipeline_ != nullptr;
  }
  /// Adopts the view's voxel layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<VoxelGridLayer*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Runs each visible layer's prepareForRender() against the per-view voxel
  /// budget, rejects volumes `rhi` cannot hold, places the rest, and records the
  /// family's fixed-frame bounds (lastBounds()).
  void admit(QRhi* rhi, const PlaceFn& place);
  /// Uploads the colormap (once), admitted layers' volumes (when their revision
  /// moved) and uniforms, and builds this frame's draws: none for a grid wholly
  /// outside the view frustum, which still counts as admitted.
  void prepare(const RhiFrameContext& frame);
  /// The prepared draw of `layer` (none or one).
  [[nodiscard]] std::span<const Draw> drawsFor(const VoxelGridLayer* layer) const {
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
  [[nodiscard]] int lastVoxelCount() const {
    return last_voxel_count_;
  }
  /// Cumulative across frames.
  [[nodiscard]] int fullUploadCount() const {
    return full_upload_count_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }

 private:
  struct LayerGpu {
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 columns = 0;
    quint32 rows = 0;
    quint32 slices = 0;
    int value_kind = -1;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };

  bool ensureLayerGpu(QRhi* owner, VoxelGridLayer* layer, LayerGpu& gpu, bool& texture_recreated);
  /// The per-layer SRB shape (also the pipeline's layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* volume) const;
  /// Why `owner` cannot hold `layer`'s volume; empty when it can.
  [[nodiscard]] QString gpuRejection(QRhi* owner, const VoxelGridLayer* layer) const;
  static void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* render_mode_uniforms_ = nullptr;   // view-owned
  const RhiSharedGeometry* geometry_ = nullptr;  // view-owned
  QRhiTexture* colormap_texture_ = nullptr;
  QRhiSampler* colormap_sampler_ = nullptr;
  int max_3d_texture_size_ = 0;
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  QRhiTexture* layout_texture_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  const ScenePipeline* pipeline_ = nullptr;

  std::vector<VoxelGridLayer*> layers_;
  std::unordered_map<VoxelGridLayer*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<VoxelGridLayer>> admitted_;
  std::vector<Draw> draws_;

  int last_layer_count_ = 0;
  int last_voxel_count_ = 0;
  int full_upload_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
