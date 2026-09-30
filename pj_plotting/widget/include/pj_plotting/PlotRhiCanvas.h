#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRhiWidget>
#include <QSize>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "pj_widgets/GraphicsApi.h"

class QLabel;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QTransform;
class QwtPlot;

namespace PJ {

namespace plot_rhi {
struct RetainedLayers;
}

// QRhi plot canvas, the GPU plot canvas on desktop and in the browser. Qwt
// continues to own axes, data, and interaction state; this widget translates
// the visible Qwt curves, grid and markers into bounded triangle geometry, and
// rasterises text and every other plot item (legend, marker labels,
// PlotMarkersItem, unknown QwtPlotItems) with the item's own QPainter drawing
// into one texture atlas. Any QRhi backend works:
// clip-space corrections are folded into the CPU vertex transform.
//
// It renders at one sample per pixel into a colour-only target it owns (no
// depth-stencil): the fragment shader antialiases every geometry edge over one
// device pixel and draws the dashes. setSampleCount() > 1 still works (the
// target then resolves an MSAA buffer), for comparisons only.
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

  // QRhi::backendName() of the backend in use ("OpenGLES2/WebGL" for WebGL in
  // the browser), valid only after initialize() has run. Test seam.
  [[nodiscard]] QString rendererBackend() const;

  // Vertex count of the grid, curve and frame layer last built (a frame that
  // reuses it leaves this unchanged). Test seam.
  [[nodiscard]] std::size_t lastVertexCount() const noexcept;

#ifdef PJ_RHI_TEST_HOOKS
  // How many times the grid/curve layer was built, and how many text layers
  // were rasterised rather than reused from the text cache.
  [[nodiscard]] std::size_t curveLayerBuildsForTest() const noexcept;
  [[nodiscard]] std::size_t textLayerRasterizationsForTest() const noexcept;
#endif

  // Bytes per device pixel of the render target attachments: 4 for the
  // one-sample colour-only target, more with MSAA. Test seam.
  [[nodiscard]] int renderTargetBytesPerPixel() const;

  // While set, a replot() redraws only the overlay (markers, the tracker and
  // text), keeping the grid and curve layer unless its structure or the view
  // changed. PlotWidgetBase::replotOverlays() sets it around one replot.
  void setOverlayOnlyReplot(bool overlay_only) noexcept;

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
  // geometry dirty before scheduling a repaint. See curves_dirty_.
  void replot();

 protected:
  // QRhiWidget lifecycle. Call order per frame series: initialize() (once per
  // QRhi and per new colour buffer: resize, sample count) → render() (every
  // frame) → releaseResources() (on teardown or before a re-initialize).
  // initialize() (re)builds the render target and pipelines and clears the
  // failure latch; releaseResources() drops all GPU resources and resets the
  // telemetry latches so a fresh context re-emits the READY/FRAME_OK breadcrumbs.
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;

 private:
  bool createPipeline(QRhiCommandBuffer* command_buffer);
  // Every GPU resource but the render target, plus the per-context state.
  void releaseDrawResources();
  // Regathers the text layers (rasterising only changed ones) and, unless they
  // match the uploaded atlas, repacks it into `upload` and rebuilds the text
  // quads (`bytes` of them to upload). True when a transient allocation failure
  // dropped the text and one delayed retry is due.
  bool rebuildTextLayers(
      const QwtPlot& plot, qreal device_pixel_ratio, const QTransform& to_clip, QImage& upload, qsizetype& bytes);
  void clearTextLayers();
  // Shows the banner; once per failure onset also logs and, when `emit_signal`,
  // emits renderFailed(). QRhiWidget's own renderFailed() ("No QRhi") comes in
  // with emit_signal false so it is not echoed.
  void reportFailure(bool emit_signal);

  QPointer<QwtPlot> plot_;
  QRhi* rhi_ = nullptr;
  RhiColorTarget color_target_;
  QRhiBuffer* vertex_buffer_ = nullptr;
  QRhiBuffer* index_buffer_ = nullptr;
  QRhiBuffer* overlay_vertex_buffer_ = nullptr;
  QRhiBuffer* overlay_index_buffer_ = nullptr;
  QRhiBuffer* text_vertex_buffer_ = nullptr;

  // plot_rhi::Vertex and quint32 index arrays, built in place and handed to
  // uploadStaticBuffer() by implicit sharing (no copy of tens of MB); their
  // allocations are reused across frames.
  QByteArray vertices_;
  QByteArray indices_;
  QByteArray overlay_vertices_;
  QByteArray overlay_indices_;
  // Item spans, draw batches, text quads and the text cache: the retained
  // layers' bookkeeping, whose types are private to the implementation.
  std::unique_ptr<plot_rhi::RetainedLayers> layers_;
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
  qsizetype overlay_vertex_buffer_capacity_ = 0;
  qsizetype index_buffer_capacity_ = 0;
  qsizetype overlay_index_buffer_capacity_ = 0;
  qsizetype text_vertex_buffer_capacity_ = 0;
  std::size_t last_vertex_count_ = 0;
  std::size_t curve_layer_builds_ = 0;
  std::size_t text_layer_rasterizations_ = 0;
  QString renderer_backend_;
  // The target the pipelines were set up for on rhi_, whether or not that
  // setup succeeded.
  RhiTargetKey built_target_;
  double last_render_cpu_ms_ = 0.0;
  double last_gpu_frame_ms_ = 0.0;

  // Geometry-invalidation contract (three retained layers: grid+curves+frame,
  // overlay = markers and the tracker, text):
  //   * replot()  = a SEMANTIC change (data, pens, items, axes) — Qwt drives it
  //                 reflectively: the overlay, and the curve layer unless every
  //                 curve's data is a RevisionedSeries and the content key below,
  //                 the env key and the curve key are all unchanged. Under
  //                 setOverlayOnlyReplot() only the overlay.
  //   * env key   = a VIEW change (logical size, target pixel size, clear and
  //                 frame colours, dpr; plus the 4 axis bounds as belt-and-braces,
  //                 since zoomers mutate scale divisions without a full replot).
  //   * curve key = the curve/grid STRUCTURE (items, visibility, pens, styles,
  //                 symbols, axes, grid flags, data object and size): keeps an
  //                 overlay-only replot from hiding an added, restyled or grown
  //                 curve. The env key adds the axis maps and grid ticks.
  //   * content key = each curve's content revision (process-unique), when every
  //                 curve has one (RevisionedSeries); a changed one rebuilds even
  //                 without a replot.
  //   * setPlot() = a SOURCE rebind, which invalidates the retained content and
  //                 schedules a frame even when the new environment is equal.
  //   * render()  = one self-dirty retry after a transient text allocation
  //                 failure; a repeated failure waits for another dirty source.
  //   * everything else (the frameSubmitted compositor refresh, expose repaints)
  //                 reuses the retained buffers — no rebuild.
  // Any geometry rebuild regathers the text layers, re-rasterising only those
  // whose recorded drawing changed. Flags clear only after a successful upload;
  // both start dirty and releaseResources() resets them.
  bool curves_dirty_ = true;
  bool overlay_dirty_ = true;
  bool overlay_only_replot_ = false;
  bool replot_requested_ = false;         // a full replot since the last successful frame
  std::optional<std::uint64_t> env_key_;  // empty: rebuild (none yet, or invalidated)
  std::uint64_t curve_key_ = 0;
  std::optional<std::uint64_t> content_key_;

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
