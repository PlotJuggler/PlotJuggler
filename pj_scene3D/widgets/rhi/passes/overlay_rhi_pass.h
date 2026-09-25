// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QImage>
#include <QPointF>
#include <array>
#include <glm/mat4x4.hpp>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

/// Screen-space overlays drawn on the output render target after presentation
/// (or at the end of the direct pass), at output resolution: the corner axis
/// gizmo and CPU-rasterised images (perf HUD, TF hover label). Everything is
/// display-referred: nothing here is tonemapped or scaled by render_scale.
class OverlayRhiPass {
 public:
  /// The image slots; each keeps its texture while the image's cacheKey() holds.
  enum Slot : std::size_t { kPerfHud, kHoverLabel, kHoverRing, kSlotCount };

  /// One image to draw this frame; a null image leaves its slot empty.
  struct Image {
    QImage image;      ///< Format_RGBA8888_Premultiplied (uploaded as is), tagged with its device-pixel ratio
    QPointF top_left;  ///< logical widget pixels
  };

  /// `setup.render_pass`/`sample_count` must describe the output render target.
  /// The gizmo draws the setup's unit arrow.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return gizmo_pipeline_ != nullptr && image_pipeline_ != nullptr;
  }
  /// Both pipelines satisfy pipelineBuiltFor(render_pass, sample_count).
  [[nodiscard]] bool builtFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const {
    return pipelineBuiltFor(gizmo_pipeline_, render_pass, sample_count) &&
           pipelineBuiltFor(image_pipeline_, render_pass, sample_count);
  }

  /// Uploads this frame's overlays into `updates`. `view` is the scene camera's
  /// view matrix (only its rotation orients the gizmo); `dpr` maps logical to
  /// output pixels (output height / widget height).
  void prepare(
      QRhiResourceUpdateBatch* updates, const glm::mat4& view, const QSize& output_px, qreal dpr,
      const std::array<Image, kSlotCount>& images);
  /// Records the overlays inside the open pass on the output target.
  void submit(QRhiCommandBuffer* command_buffer) const;
  void release();

  /// The gizmo's footprint in logical pixels (bottom-right corner, no margin),
  /// matching the GL renderer's AxisOverlayPass defaults.
  static constexpr int kGizmoSizePx = 80;

 private:
  struct ImageGpu {
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    qint64 cache_key = 0;
    std::array<float, 4> rect{};  ///< clip-space rect last written to uniform_buffer
    bool rect_written = false;
    bool visible = false;
  };

  bool ensureImage(QRhiResourceUpdateBatch* updates, ImageGpu& gpu, const QImage& image);
  /// The per-image SRB shape (also the pipeline's layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createImageBindings(QRhiBuffer* uniforms, QRhiTexture* texture) const;
  static void releaseImage(ImageGpu& gpu);

  QRhi* rhi_ = nullptr;
  const RhiSharedGeometry* geometry_ = nullptr;
  QRhiBuffer* gizmo_uniform_buffer_ = nullptr;
  QRhiBuffer* gizmo_instance_buffer_ = nullptr;
  QRhiShaderResourceBindings* gizmo_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* gizmo_pipeline_ = nullptr;
  QRhiSampler* image_sampler_ = nullptr;
  QRhiShaderResourceBindings* image_layout_shader_resources_ = nullptr;
  QRhiBuffer* image_layout_uniform_buffer_ = nullptr;
  QRhiTexture* image_layout_texture_ = nullptr;
  QRhiGraphicsPipeline* image_pipeline_ = nullptr;
  std::array<ImageGpu, kSlotCount> images_{};
  QRhiViewport gizmo_viewport_;
  QRhiScissor gizmo_scissor_;
  QRhiViewport output_viewport_;
  bool gizmo_arms_upload_pending_ = false;
};

}  // namespace pj::scene3d
