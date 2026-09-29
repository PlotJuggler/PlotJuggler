// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QPointer>
#include <QSize>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_base/time.hpp"

namespace pj::scene3d {

class PosesInFrameLayer;
class Scene3DLayer;
class TrailLayer;

/// The trail family of the QRhi view: each trail is a screen-space ribbon (a
/// triangle strip, two vertices per polyline point) drawn as a past and a future half in their own colours from one
/// vertex buffer. A trail is either a standalone TrailLayer or the one a PosesInFrameLayer owns; the owner is what the
/// view orders. Trails have no per-view budget and add nothing to the scene bounds.
class TrailRhiPass {
 public:
  struct Draw {
    const Scene3DLayer* layer = nullptr;  ///< the owner
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiShaderResourceBindings* past_resources = nullptr;
    QRhiShaderResourceBindings* future_resources = nullptr;
    quint32 past_vertex_count = 0;    ///< strip vertices [0, past_vertex_count)
    quint32 future_first_vertex = 0;  ///< starts at the last past point: the ribbon stays continuous
    quint32 future_vertex_count = 0;
    DrawClass draw_class = DrawClass::kOpaque;
  };

  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return pipeline_ != nullptr;
  }
  /// A trail's owner, as the view registered it (SceneContributions): the layer
  /// the view orders, which keys the draw, and where its trail comes from.
  struct Owner {
    Scene3DLayer* layer = nullptr;
    TrailLayer* trail = nullptr;         ///< a standalone trail (the layer itself)
    PosesInFrameLayer* poses = nullptr;  ///< or the pose layer whose trail() comes and goes
  };
  /// Adopts the view's trail owners, in logical order. Drops the GPU state of
  /// owners that left.
  void setLayers(const std::vector<Owner>& owners);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !owners_.empty();
  }

  /// Syncs each visible trail to `fixed_frame`, uploads changed polylines and this
  /// frame's uniforms, and splits each ribbon at `time`. `render_size` is the
  /// scene target in pixels and `render_scale` its supersampling factor.
  void prepare(
      const RhiFrameContext& frame, const std::string& fixed_frame, PJ::Timepoint time, const QSize& render_size,
      float render_scale);
  /// The prepared draw of `owner` (none or one).
  [[nodiscard]] std::span<const Draw> drawsFor(const Scene3DLayer* owner) const {
    return findDraws(draws_, owner);
  }
  /// Records the owner's past and future halves; false when nothing was recorded.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  [[nodiscard]] int lastTrailCount() const {
    return last_trail_count_;
  }
  /// Strip vertices drawn last frame, both halves.
  [[nodiscard]] int lastVertexCount() const {
    return last_vertex_count_;
  }

 private:
  struct OwnerGpu {
    // Null once the trail it was built from is destroyed, so a new trail at the
    // same address never reuses a stale upload.
    QPointer<TrailLayer> trail;
    std::uint64_t uploaded_revision = 0;
    glm::dvec3 uploaded_origin{0.0};
    QRhiBuffer* vertex_buffer = nullptr;
    quint32 vertex_capacity = 0;
    QRhiBuffer* uniform_buffer = nullptr;  // past block, then future block
    QRhiShaderResourceBindings* past_resources = nullptr;
    QRhiShaderResourceBindings* future_resources = nullptr;
  };

  /// The owner's trail this frame; nullptr while a pose layer has none.
  static TrailLayer* trailOf(const Owner& owner);

  bool ensureOwnerGpu(QRhi* rhi, OwnerGpu& gpu, quint32 vertex_bytes);
  static void releaseOwnerGpu(OwnerGpu& gpu);

  const ScenePipeline* pipeline_ = nullptr;
  QRhiShaderResourceBindings* layout_resources_ = nullptr;
  QRhiBuffer* layout_uniform_buffer_ = nullptr;
  quint32 block_stride_ = 0;  // one uniform block, rounded to the backend's alignment

  std::vector<Owner> owners_;
  std::unordered_map<Scene3DLayer*, OwnerGpu> owner_gpu_;
  std::vector<Draw> draws_;
  std::vector<float> scratch_;

  int last_trail_count_ = 0;
  int last_vertex_count_ = 0;
};

}  // namespace pj::scene3d
