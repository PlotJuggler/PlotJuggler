// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QImage>
#include <QPointF>
#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <memory>
#include <optional>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

/// Screen-space overlays drawn on the output render target after presentation
/// (or at the end of the direct pass), at output resolution: the corner axis
/// gizmo and CPU-rasterised images (perf HUD, TF hover label). Everything is
/// display-referred: nothing here is tonemapped or scaled by render_scale.
///
/// The gizmo is drawn into its own target (4x where the GPU multisamples render
/// buffers, else 1x) by submitGizmoTarget() and composited as a premultiplied
/// quad. That target is optional: without it the gizmo is not drawn.
class OverlayRhiPass {
 public:
  /// The image slots; each keeps its texture while the image's cacheKey() holds.
  enum Slot : std::size_t { kPerfHud, kHoverLabel, kHoverRing, kSlotCount };

  /// One image to draw this frame; a null image leaves its slot empty.
  struct Image {
    QImage image;      ///< Format_RGBA8888_Premultiplied (uploaded as is), tagged with its device-pixel ratio
    QPointF top_left;  ///< logical widget pixels
  };

  /// Draws on the registry's direct target (the output render target, set by
  /// ScenePipelineRegistry::setDirectTarget()); `dpr` (output pixels per logical
  /// pixel) sizes the gizmo's target.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup, qreal dpr);
  [[nodiscard]] bool ready() const {
    return image_pipeline_ != nullptr;
  }
  /// The pipeline drawing on the output satisfies pipelineBuiltFor(render_pass, sample_count).
  [[nodiscard]] bool builtFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const {
    return pipelineBuiltFor(image_pipeline_.get(), render_pass, sample_count);
  }

  /// Uploads this frame's overlays into `updates`. `view` is the camera's view
  /// matrix relative to the render origin (only its rotation orients the gizmo);
  /// `dpr` maps logical to output pixels (output height / widget height).
  void prepare(
      QRhiResourceUpdateBatch* updates, const glm::mat4& view, const QSize& output_px, qreal dpr,
      const std::array<Image, kSlotCount>& images);
  /// Records the gizmo's own pass, handing it `updates` (which carry its
  /// uniforms), when prepare() found its rotation or size changed since the last
  /// recorded one: otherwise its texture still holds it. Call after prepare()
  /// and before the first pass of the frame. Returns the batch still to submit,
  /// nullptr once consumed.
  [[nodiscard]] QRhiResourceUpdateBatch* submitGizmoTarget(
      QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates);
  /// Records the overlays inside the open pass on the output target.
  void submit(QRhiCommandBuffer* command_buffer) const;
  void release();

  /// The gizmo's footprint in logical pixels (bottom-right corner, no margin).
  static constexpr int kGizmoSizePx = 80;
  /// The gizmo's side in output pixels at `dpr`.
  [[nodiscard]] static int gizmoPixels(qreal dpr) {
    return std::max(1, static_cast<int>(static_cast<double>(kGizmoSizePx) * dpr));
  }
  /// Gizmo passes recorded since initialize().
  [[nodiscard]] std::uint64_t gizmoPassCountForTest() const {
    return gizmo_pass_count_;
  }

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

  /// What the gizmo's image is a function of.
  struct GizmoKey {
    glm::mat3 rotation{1.0F};
    int size = 0;
    bool operator==(const GizmoKey&) const = default;
  };

  /// The gizmo's target: `color` multisampled and resolved into `quad.texture`,
  /// or `quad.texture` itself at one sample (`color` null).
  struct GizmoTarget {
    QRhiRenderBuffer* color = nullptr;
    QRhiRenderBuffer* depth = nullptr;
    QRhiTextureRenderTarget* target = nullptr;
    QRhiRenderPassDescriptor* render_pass = nullptr;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    ImageGpu quad;                   ///< the gizmo's texture, drawn with the image pipeline
    std::optional<GizmoKey> drawn;   ///< what quad.texture holds (nullopt: nothing)
    std::optional<GizmoKey> queued;  ///< prepared this frame, for submitGizmoTarget() to record
  };

  bool ensureImage(QRhiResourceUpdateBatch* updates, ImageGpu& gpu, const QImage& image);
  /// Builds gizmo_target_ at `size` px (all or nothing); false leaves it empty.
  bool createGizmoTarget(const RhiPassSetup& setup, const QSize& size, const PipelineDesc& gizmo_desc);
  /// Resizes the attachments (dropping the drawn image); false releases the target.
  bool resizeGizmoTarget(const QSize& size);
  void releaseGizmoTarget();
  /// The per-image SRB shape (also the pipeline's layout SRB); nullptr on failure.
  QRhiShaderResourceBindings* createImageBindings(QRhiBuffer* uniforms, QRhiTexture* texture) const;
  static void releaseImage(ImageGpu& gpu);

  QRhi* rhi_ = nullptr;
  QRhiBuffer* gizmo_uniform_buffer_ = nullptr;
  QRhiBuffer* gizmo_instance_buffer_ = nullptr;
  QRhiBuffer* gizmo_arrow_vertices_ = nullptr;
  QRhiBuffer* gizmo_arrow_indices_ = nullptr;
  QRhiShaderResourceBindings* gizmo_shader_resources_ = nullptr;
  QRhiSampler* image_sampler_ = nullptr;
  QRhiShaderResourceBindings* image_layout_shader_resources_ = nullptr;
  QRhiBuffer* image_layout_uniform_buffer_ = nullptr;
  QRhiTexture* image_layout_texture_ = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> image_pipeline_;
  std::array<ImageGpu, kSlotCount> images_{};
  GizmoTarget gizmo_target_;
  QRhiViewport output_viewport_;
  std::uint64_t gizmo_pass_count_ = 0;
};

}  // namespace pj::scene3d
