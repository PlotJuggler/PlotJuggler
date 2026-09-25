#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRhiWidget>
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "pj_widgets/GraphicsApi.h"

class QLabel;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiRenderPassDescriptor;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QwtPlot;

namespace PJ {

// Interleaved position+color vertex (12 B), laid out to match the vertex input
// attributes wired in createPipeline (Float2 clip-space position, UNormByte4
// straight-alpha RGBA). Lives here only so the reusable per-frame vertex buffer
// can be a canvas member.
struct PlotRhiVertex {
  float x = 0.0F;
  float y = 0.0F;
  std::array<std::uint8_t, 4> rgba{};
};

struct PlotRhiTextVertex {
  float x = 0.0F;
  float y = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
};

// QRhi plot canvas, used wherever PJ_RHI_WIDGETS is defined (the desktop QRhi
// build and WebAssembly). Qwt continues to own axes, data, and interaction
// state; this widget translates the visible Qwt curves, grid and markers into
// bounded triangle geometry, and rasterises text and every other plot item
// (legend, marker labels, PlotMarkersItem, unknown QwtPlotItems) with the
// item's own QPainter drawing into one texture atlas. Any QRhi backend works:
// clip-space corrections are folded into the CPU vertex transform.
//
// It is installed as the QwtPlot canvas (QwtPlot::setCanvas), so QwtPlot::replot
// reaches it reflectively — see the replot() slot.
//
// Failure state: when its colour pipeline or its vertex buffer cannot be
// created, the canvas records clear-only passes (never a draw with a missing
// resource), shows a banner, emits renderFailed() once per onset, and retries
// the pipelines only in the next initialize() (new QRhi or render target),
// never per frame. Text is optional: a text pipeline or atlas failure drops
// legends and labels with a one-time warning while the curves still draw.
// renderFailed() may be emitted from inside render(): a receiver that deletes
// the canvas must connect with Qt::QueuedConnection.
class PlotRhiCanvas final : public QRhiWidget {
  Q_OBJECT

 public:
  // `plot` is the QwtPlot whose visible items are rendered. It is stored, not
  // owned, and must outlive this canvas. Passing nullptr is tolerated: while
  // plot_ is null render() records a clear-only pass (no geometry, no failure
  // banner) because a null plot is a transient wiring state — a QwtPlot adopts
  // its canvas during its own constructor, so the canvas necessarily exists
  // before setPlot() runs. Wire the real plot in as soon as it is available.
  explicit PlotRhiCanvas(QwtPlot* plot = nullptr, QWidget* parent = nullptr);
  ~PlotRhiCanvas() override;

  // Binds the source plot after construction. Needed because a QwtPlot adopts
  // its canvas during its own constructor, so the canvas exists first.
  void setPlot(QwtPlot* plot);

  // QRhi::backendName() of the backend in use ("OpenGLES2/WebGL" for OpenGL
  // ES/WebGL), valid only after initialize() has run. Test seam.
  [[nodiscard]] QString rendererBackend() const;

  // Vertex count uploaded on the last frame that rebuilt geometry (a skipped
  // dirty-gate frame leaves this unchanged). Test seam.
  [[nodiscard]] std::size_t lastVertexCount() const noexcept;

  // CPU time of the last render() call, and the GPU time of the last completed
  // frame (0 unless the QRhi was created with timestamps, e.g. QSG_RHI_PROFILE=1),
  // both in milliseconds. Benchmark seams.
  [[nodiscard]] double lastRenderCpuMs() const noexcept;
  [[nodiscard]] double lastGpuFrameMs() const noexcept;

  // Bytes the canvas keeps between frames for mapping curve samples: only the
  // samples of the last rebuild's visible ranges, and none past a small cap.
  // Test seam.
  [[nodiscard]] std::size_t retainedSampleScratchBytes() const noexcept;

  // PJ::grabPresentedFrame() on this canvas.
  void grabPresentedFrame(std::function<void(const QImage&)> done);

 public slots:
  // Invoked by QwtPlot::replot via QMetaObject::invokeMethod(canvas, "replot").
  // Must keep this exact name/signature for that reflective call to bind. This
  // is the geometry-invalidation source: QwtPlot::replot() fires on every
  // semantic change (data, pens, items, axes), so this slot marks the retained
  // geometry dirty before scheduling a repaint. See geometry_dirty_.
  void replot();

 protected:
  // QRhiWidget lifecycle. Call order per frame series: initialize() (once per
  // QRhi, and again after a device/context loss) → render() (every frame) →
  // releaseResources() (on teardown or before a re-initialize). initialize()
  // (re)builds the pipeline and clears the failure latch; releaseResources()
  // drops all GPU resources and resets the telemetry latches so a fresh
  // context re-emits the READY/FRAME_OK breadcrumbs.
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;

 private:
  struct DrawBatch {
    bool text = false;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
  };

  bool createPipeline(QRhiCommandBuffer* command_buffer);
  void clearTextLayers();
  // Shows the banner; once per failure onset also logs and, when `emit_signal`,
  // emits renderFailed(). QRhiWidget's own renderFailed() ("No QRhi") comes in
  // with emit_signal false so it is not echoed.
  void reportFailure(bool emit_signal);

  QPointer<QwtPlot> plot_;
  QRhi* rhi_ = nullptr;
  QRhiBuffer* vertex_buffer_ = nullptr;
  QRhiBuffer* text_vertex_buffer_ = nullptr;

  // Reused across frames (clear() retains capacity) to avoid a per-frame heap
  // allocation of the built geometry.
  std::vector<PlotRhiVertex> vertices_;
  std::vector<PlotRhiTextVertex> text_vertices_;
  // Retained native item order across the geometry and text buffers. A marker
  // draws its line/symbol geometry and then its label before the next same-Z
  // marker, matching Qwt rather than flattening every label above all markers.
  std::vector<DrawBatch> draw_batches_;
  // Desktop only: one curve's samples mapped to canvas pixels (its visible
  // range for an x-sorted solid curve, else all of it; 16 B per sample), reused
  // across curves; released after a rebuild that grew it past a small cap.
  std::vector<QPointF> mapped_scratch_;
  // Reusable CPU staging surface for tight text rasters. Its active rectangle
  // is cleared before every paint; it is scratch capacity, not a text cache.
  QImage text_raster_scratch_;

  QRhiShaderResourceBindings* shader_resources_ = nullptr;
  QRhiGraphicsPipeline* pipeline_ = nullptr;
  QRhiGraphicsPipeline* text_pipeline_ = nullptr;
  QRhiShaderResourceBindings* text_layout_bindings_ = nullptr;
  QRhiTexture* text_placeholder_texture_ = nullptr;
  QRhiTexture* text_texture_ = nullptr;
  QSize text_texture_capacity_;
  QRhiSampler* text_sampler_ = nullptr;
  QRhiShaderResourceBindings* text_bindings_ = nullptr;
  QLabel* failure_label_ = nullptr;
  qsizetype vertex_buffer_capacity_ = 0;
  qsizetype text_vertex_buffer_capacity_ = 0;
  std::size_t last_vertex_count_ = 0;
  QString renderer_backend_;
  // The target the pipelines were set up for on rhi_, whether or not that
  // setup succeeded.
  RhiTargetKey built_target_;
  double last_render_cpu_ms_ = 0.0;
  double last_gpu_frame_ms_ = 0.0;

  // Geometry-invalidation contract (four signals, one retained buffer):
  //   * replot()  = a SEMANTIC change (data, pens, items, axes) — Qwt drives it
  //                 reflectively, so it is the authoritative dirty source.
  //   * env key   = a VIEW change (logical size, target pixel size, clear color,
  //                 dpr; plus the 4 axis bounds as belt-and-braces, since
  //                 zoomers mutate scale divisions without a full replot).
  //   * setPlot() = a SOURCE rebind, which invalidates the retained content and
  //                 schedules a frame even when the new environment is equal.
  //   * render()  = one self-dirty retry after a transient text allocation
  //                 failure; a repeated failure waits for another dirty source.
  //   * everything else (the frameSubmitted compositor refresh, expose repaints)
  //                 reuses the retained vertex buffer — no rebuild.
  // render() rebuilds+uploads geometry when geometry_dirty_ is set OR the env
  // key changed, then clears geometry_dirty_ only after a successful upload.
  // Starts dirty (first frame) and is reset dirty by releaseResources().
  bool geometry_dirty_ = true;
  std::uint64_t env_key_ = 0;
  bool env_key_valid_ = false;

  // One-shot telemetry latches. ready_/content_ reset in releaseResources so a
  // recreated context re-logs; failure_ leaves on the next successful
  // initialize() or frame, which re-arms renderFailed() for a later onset.
  bool ready_reported_ = false;
  bool content_reported_ = false;
  RhiFailureLatch failure_;

  // Latches frame-wide or per-curve vertex-limit warnings so each onset logs
  // once, not every frame while over budget. Cleared by a within-budget frame.
  bool truncation_reported_ = false;
  bool text_truncation_reported_ = false;
  bool text_failure_reported_ = false;

  // Fingerprint of the last content logged via PJ_WASM_PLOT_FRAME_OK, so the
  // breadcrumb re-emits when the plotted content actually changes, not only on
  // the first-ever frame.
  std::uint64_t content_fingerprint_ = 0;

  // Which top-level window has already had its raster backing store refreshed
  // after the first RHI frame. Tracked (not a bare bool) so a reparent/float to
  // a different top-level re-arms the one-shot refresh for the new window.
  bool composition_refresh_queued_ = false;
  QPointer<QWidget> last_refreshed_window_;
};

}  // namespace PJ
