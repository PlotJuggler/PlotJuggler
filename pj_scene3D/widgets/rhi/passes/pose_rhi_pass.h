// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_core/poses_in_frame_render.h"

namespace pj::scene3d {

class PosesInFrameLayer;

/// The vertex layout of an instanced unit-arrow draw: the arrow mesh (binding 0:
/// position, normal) and one PoseTriadInstance per instance (binding 1).
QRhiVertexInputLayout poseTriadInputLayout();

/// The pose-triad family of the QRhi view: instanced solid arrows over the
/// shared unit-arrow mesh, one draw per PosesInFrameLayer plus one for the TF
/// frame triads (the view's "Frames" gizmos), which reuse the same pipeline.
/// Poses are always opaque. Per frame the view calls, in order: admit() (once),
/// prepare() (once), then submit() per ScenePass for each draw and
/// submitTfTriads() in the scene prelude.
class PoseRhiPass {
 public:
  struct Draw {
    PosesInFrameLayer* layer = nullptr;
    QRhiBuffer* instance_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_count = 0;
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// Registers the pipeline, which grades triads as annotations (the setup's
  /// annotation render mode) and draws the setup's arrow mesh.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return pipeline_ != nullptr;
  }
  /// Adopts the view's pose layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<PosesInFrameLayer*>& layers);

  /// Runs each visible layer's prepareForRender() against the per-view arm
  /// budget, places it, and records the family's fixed-frame bounds (lastBounds()).
  void admit(const PlaceFn& place);
  /// Uploads admitted layers' instances (when their revision moved) and
  /// uniforms, and this frame's TF triads (fixed-frame arms relative to the
  /// render origin, drawn with an identity fixed_from_source; re-uploaded only
  /// when `tf_triads_revision` moved).
  void prepare(
      const RhiFrameContext& frame, const std::vector<PoseTriadInstance>& tf_triads, std::uint64_t tf_triads_revision);
  /// The prepared draw of `layer` (none or one).
  [[nodiscard]] std::span<const Draw> drawsFor(const PosesInFrameLayer* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one draw; false when its pipeline is unavailable for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  /// Records this frame's TF triads, if any were prepared.
  void submitTfTriads(
      QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  [[nodiscard]] int lastLayerCount() const {
    return last_layer_count_;
  }
  [[nodiscard]] int lastArmCount() const {
    return last_arm_count_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }

 private:
  struct LayerGpu {
    QRhiBuffer* instance_buffer = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_capacity = 0;
    quint32 instance_count = 0;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };

  bool ensureLayerGpu(QRhi* owner, LayerGpu& gpu) const;
  /// The per-layer SRB shape (also the pipeline's layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms) const;
  static void releaseLayerGpu(LayerGpu& gpu);
  /// Records one instanced arrow draw; false when `pass` has no pipeline.
  bool drawArrows(
      QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport, QRhiShaderResourceBindings* shader_resources, QRhiBuffer* instance_buffer,
      quint32 instance_count) const;

  QRhiBuffer* render_mode_uniforms_ = nullptr;   // view-owned (annotation grading)
  const RhiSharedGeometry* geometry_ = nullptr;  // view-owned
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  const ScenePipeline* pipeline_ = nullptr;

  std::vector<PosesInFrameLayer*> layers_;
  std::unordered_map<PosesInFrameLayer*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<PosesInFrameLayer>> admitted_;
  std::vector<Draw> draws_;
  // The TF triads reuse the per-layer GPU shape, with a Dynamic instance buffer
  // (they follow the render origin and the playback time); the transform is
  // baked into each instance model, so its fixed_from_source uniform is identity.
  LayerGpu tf_triad_gpu_;
  quint32 tf_triad_instance_count_ = 0;
  std::uint64_t tf_triads_uploaded_revision_ = ~std::uint64_t{0};

  int last_layer_count_ = 0;
  int last_arm_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
