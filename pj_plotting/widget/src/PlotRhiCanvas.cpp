// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotRhiCanvas.h"

#include <pj_widgets/GraphicsApi.h>
#include <pj_widgets/RhiBlend.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_text.h>
#include <rhi/qrhi.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QPen>
#include <QPointF>
#include <QSize>
#include <QTimer>
#include <QTransform>
#include <QWidget>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "PlotRhiGeometry.h"
#include "PlotRhiTextAtlas.h"

using namespace Qt::StringLiterals;

void pjPlotRhiInitResources() {
  Q_INIT_RESOURCE(pj_plot_rhi_shaders);
}

namespace PJ {
Q_LOGGING_CATEGORY(lcPlotRhi, "pj.plotting.rhi")

namespace plot_rhi {

// The vertex buffer a draw reads: grid+curves+frame, markers, or text.
enum class Layer : std::uint8_t { Curves, Overlay, Text };

// An index range of a geometry layer, a vertex range of the text layer.
struct DrawBatch {
  Layer layer = Layer::Curves;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

// The CPU bookkeeping of the retained layers, between frames.
struct RetainedLayers {
  // The text quads of the last text build.
  std::vector<TextVertex> text_vertices;
  // Per-item spans of the last build of each layer (index ranges in a geometry
  // layer, vertex ranges in the text layer); the frame's span is kept apart
  // because it draws over everything.
  std::vector<ItemSpan> curve_spans;
  std::vector<ItemSpan> overlay_spans;
  std::vector<ItemSpan> text_spans;
  ItemSpan frame_span;
  // Retained native item order across the three buffers. A marker draws its
  // line/symbol geometry and then its label before the next same-Z marker,
  // matching Qwt rather than flattening every label above all markers.
  std::vector<DrawBatch> draw_batches;
  // Text layers of the last text build, keyed by their recorded drawing (a
  // QPicture of the item's paint, its bounds and the dpr). Dropped with the GPU
  // resources.
  QHash<QByteArray, TextCacheEntry> text_cache;
  // What the uploaded atlas and text quads were built from.
  TextSignature text_signature;
};

}  // namespace plot_rhi

namespace {

using plot_rhi::buildCurveLayer;
using plot_rhi::buildOverlayLayer;
using plot_rhi::buildTextRasters;
using plot_rhi::computeCurveKeys;
using plot_rhi::computeEnvKey;
using plot_rhi::CurveKeys;
using plot_rhi::DrawBatch;
using plot_rhi::hashDouble;
using plot_rhi::hashUInt;
using plot_rhi::ItemSpan;
using plot_rhi::kMapSamplesOnce;
using plot_rhi::kMaxGeometryVertices;
using plot_rhi::kRetainedMappedSamples;
using plot_rhi::Layer;
using plot_rhi::packTextAtlas;
using plot_rhi::PlotGeometry;
using plot_rhi::refreshTextRasterStats;
using plot_rhi::TextRasterBuild;
using plot_rhi::TextRasterLayer;
using plot_rhi::TextSignature;
using plot_rhi::TextVertex;
using plot_rhi::Vertex;

[[nodiscard]] qreal renderTargetScale(QSize logical_size, QSize target_size) {
  if (logical_size.isEmpty() || target_size.isEmpty()) {
    return 1.0;
  }
  return std::max(
      static_cast<qreal>(target_size.width()) / logical_size.width(),
      static_cast<qreal>(target_size.height()) / logical_size.height());
}

// Logical widget pixels (y down) to the backend's clip space: GL-style NDC
// (y up) followed by QRhi::clipSpaceCorrMatrix(), which flips y on Vulkan. The
// shaders pass positions through unchanged, so this is the only place that
// knows the backend's conventions. z stays 0 and w 1, inside every backend's
// depth range; only the 2D part of the correction matters.
[[nodiscard]] QTransform logicalToClip(QSize logical_size, const QMatrix4x4& clip_correction) {
  if (logical_size.isEmpty()) {
    return QTransform(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  }
  const QTransform to_ndc(2.0 / logical_size.width(), 0.0, 0.0, -2.0 / logical_size.height(), -1.0, 1.0);
  const QTransform correction(
      clip_correction(0, 0), clip_correction(1, 0), clip_correction(0, 1), clip_correction(1, 1), clip_correction(0, 3),
      clip_correction(1, 3));
  return to_ndc * correction;
}

// QRhi's own name ("OpenGL", "Vulkan", "D3D11", "Metal"), except in the
// browser, whose breadcrumbs report WebGL as "OpenGLES2/WebGL".
[[nodiscard]] QString backendName(const QRhi& rhi) {
#ifdef PJ_TARGET_WASM
  if (rhi.backend() == QRhi::OpenGLES2) {
    return u"OpenGLES2/WebGL"_s;
  }
#endif
  return QString::fromLatin1(rhi.backendName());
}

// Grows `buffer` (a Static buffer of `usage`, vertex by default) to hold `bytes`,
// doubling, never past `ceiling`. False when the replacement cannot be created;
// the old buffer stays.
[[nodiscard]] bool growStaticBuffer(
    QRhi& rhi, QRhiBuffer*& buffer, qsizetype& capacity, qsizetype bytes,
    qsizetype ceiling = std::numeric_limits<qsizetype>::max(),
    QRhiBuffer::UsageFlags usage = QRhiBuffer::VertexBuffer) {
  if (bytes <= capacity) {
    return true;
  }
  const qsizetype new_capacity = std::min(ceiling, std::max(bytes, capacity * 2));
  auto replacement = std::unique_ptr<QRhiBuffer>(rhi.newBuffer(QRhiBuffer::Static, usage, new_capacity));
  if (!replacement->create()) {
    return false;
  }
  delete buffer;
  buffer = replacement.release();
  capacity = new_capacity;
  return true;
}

}  // namespace

PlotRhiCanvas::PlotRhiCanvas(QwtPlot* plot, QWidget* parent)
    : QRhiWidget(parent), plot_(plot), layers_(std::make_unique<plot_rhi::RetainedLayers>()) {
  pjPlotRhiInitResources();
  setApi(preferredGraphicsApi());
  // One sample per pixel into a colour-only target of our own: the shader
  // antialiases the geometry, and nothing uses depth or stencil.
  setAutoRenderTarget(false);
  setContentsMargins(1, 1, 1, 1);
  setAutoFillBackground(false);
  setMouseTracking(true);
  connect(this, &QRhiWidget::renderFailed, this, [this]() { reportFailure(/*emit_signal=*/false); });
  connect(this, &QRhiWidget::frameSubmitted, this, [this]() {
    QWidget* top_level = window();
    // Qt/WASM switches a raster top-level to its RHI compositor when the first
    // real plot is shown. Repaint the whole window only after that first frame
    // has actually been submitted, so the raster backing texture is fully
    // initialized before later partial QWidget updates. The refresh is
    // per-top-level: a reparent/float to a new window re-arms it. Latch BEFORE
    // scheduling the update so the update it triggers cannot re-enter this arm.
    if (composition_refresh_queued_ && last_refreshed_window_ == top_level) {
      return;
    }
    composition_refresh_queued_ = true;
    last_refreshed_window_ = top_level;
    QTimer::singleShot(0, top_level, [top_level]() { top_level->update(); });
  });
}

PlotRhiCanvas::~PlotRhiCanvas() {
  releaseResources();
}

void PlotRhiCanvas::setPlot(QwtPlot* plot) {
  if (plot_ == plot) {
    return;
  }
  plot_ = plot;
  curves_dirty_ = true;
  overlay_dirty_ = true;
  env_key_.reset();
  content_reported_ = false;
  update();
}

QString PlotRhiCanvas::rendererBackend() const {
  return renderer_backend_;
}

std::size_t PlotRhiCanvas::lastVertexCount() const noexcept {
  return last_vertex_count_;
}

#ifdef PJ_RHI_TEST_HOOKS
std::size_t PlotRhiCanvas::curveLayerBuildsForTest() const noexcept {
  return curve_layer_builds_;
}

std::size_t PlotRhiCanvas::textLayerRasterizationsForTest() const noexcept {
  return text_layer_rasterizations_;
}
#endif

int PlotRhiCanvas::renderTargetBytesPerPixel() const {
  constexpr int kRgba8 = 4;
  int bytes = colorTexture() != nullptr ? kRgba8 : 0;
  if (msaaColorBuffer() != nullptr) {
    bytes += kRgba8 * msaaColorBuffer()->sampleCount() + (resolveTexture() != nullptr ? kRgba8 : 0);
  }
  if (depthStencilBuffer() != nullptr) {
    bytes += kRgba8 * depthStencilBuffer()->sampleCount();
  }
  return bytes;
}

void PlotRhiCanvas::setOverlayOnlyReplot(bool overlay_only) noexcept {
  overlay_only_replot_ = overlay_only;
}

std::size_t PlotRhiCanvas::retainedSampleScratchBytes() const noexcept {
  return mapped_scratch_.capacity() * sizeof(QPointF);
}

double PlotRhiCanvas::lastRenderCpuMs() const noexcept {
  return last_render_cpu_ms_;
}

double PlotRhiCanvas::lastGpuFrameMs() const noexcept {
  return last_gpu_frame_ms_;
}

void PlotRhiCanvas::grabPresentedFrame(std::function<void(const QImage&)> done) {
  PJ::grabPresentedFrame(*this, std::move(done));
}

void PlotRhiCanvas::replot() {
  // Authoritative geometry-invalidation source: QwtPlot::replot() reaches here
  // reflectively on every semantic change (data, pens, items, axes). Mark dirty
  // so the next render() rebuilds instead of reusing the retained buffers.
  replot_requested_ = replot_requested_ || !overlay_only_replot_;
  overlay_dirty_ = true;
  update();
}

void PlotRhiCanvas::clearTextLayers() {
  delete text_bindings_;
  text_bindings_ = nullptr;
  delete text_texture_;
  text_texture_ = nullptr;
  text_texture_capacity_ = {};
  layers_->text_vertices.clear();
  layers_->text_spans.clear();
  layers_->text_signature = {};
  layers_->text_cache.clear();
  layers_->draw_batches.clear();
}

void PlotRhiCanvas::releaseDrawResources() {
  clearTextLayers();
  text_raster_scratch_ = {};
  mapped_scratch_.clear();
  mapped_scratch_.shrink_to_fit();
  delete text_pipeline_;
  text_pipeline_ = nullptr;
  delete text_layout_bindings_;
  text_layout_bindings_ = nullptr;
  delete text_placeholder_texture_;
  text_placeholder_texture_ = nullptr;
  delete text_sampler_;
  text_sampler_ = nullptr;
  delete text_vertex_buffer_;
  text_vertex_buffer_ = nullptr;
  text_vertex_buffer_capacity_ = 0;
  delete pipeline_;
  pipeline_ = nullptr;
  delete shader_resources_;
  shader_resources_ = nullptr;
  delete vertex_buffer_;
  vertex_buffer_ = nullptr;
  vertex_buffer_capacity_ = 0;
  delete overlay_vertex_buffer_;
  overlay_vertex_buffer_ = nullptr;
  overlay_vertex_buffer_capacity_ = 0;
  delete index_buffer_;
  index_buffer_ = nullptr;
  index_buffer_capacity_ = 0;
  delete overlay_index_buffer_;
  overlay_index_buffer_ = nullptr;
  overlay_index_buffer_capacity_ = 0;
  layers_->curve_spans.clear();
  layers_->overlay_spans.clear();
  layers_->frame_span = {};
  // A fresh context has no uploaded geometry: start dirty and drop the env key
  // so the first frame after re-initialize() rebuilds and re-uploads.
  curves_dirty_ = true;
  overlay_dirty_ = true;
  env_key_.reset();
  ready_reported_ = false;
  content_reported_ = false;
  truncation_reported_ = false;
  text_truncation_reported_ = false;
  text_failure_reported_ = false;
}

void PlotRhiCanvas::releaseResources() {
  noteRhiRelease(*this, rhi_);
  releaseDrawResources();
  color_target_.release();
  built_target_ = {};
  rhi_ = nullptr;
  composition_refresh_queued_ = false;
  last_refreshed_window_.clear();
}

bool PlotRhiCanvas::createPipeline(QRhiCommandBuffer* /*command_buffer*/) {
  if (rhi_ == nullptr || color_target_.renderPass() == nullptr) {
    return false;
  }
  const QShader vertex_shader = loadRhiShader(u":/pj_plotting/shaders/plot_color.vert.qsb"_s);
  const QShader fragment_shader = loadRhiShader(u":/pj_plotting/shaders/plot_color.frag.qsb"_s);
  const QShader text_vertex_shader = loadRhiShader(u":/pj_plotting/shaders/plot_text.vert.qsb"_s);
  const QShader text_fragment_shader = loadRhiShader(u":/pj_plotting/shaders/plot_text.frag.qsb"_s);
  if (!vertex_shader.isValid() || !fragment_shader.isValid() || !text_vertex_shader.isValid() ||
      !text_fragment_shader.isValid()) {
    qCritical("PlotRhiCanvas: failed to load plot shaders");
    return false;
  }

  if (!rhi_->isFeatureSupported(QRhi::ElementIndexUint)) {
    qCWarning(lcPlotRhi) << "PlotRhiCanvas: the backend has no 32-bit index buffers";
    return false;
  }
  auto shader_resources = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  shader_resources->setBindings({});
  if (!shader_resources->create()) {
    return false;
  }

  // Static (device-local): the geometry is retained across frames and only
  // re-uploaded on a rebuild, and a Dynamic buffer would be host-visible and
  // duplicated per frame in flight on Vulkan/D3D/Metal.
  constexpr qsizetype kInitialVertexBufferCapacity = 64 * 1024;
  auto vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, kInitialVertexBufferCapacity));
  if (!vertex_buffer->create()) {
    return false;
  }

  QRhiVertexInputLayout input_layout;
  input_layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  input_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::UNormByte4, offsetof(Vertex, rgba)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, offsetof(Vertex, aux)),
  });
  auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
  pipeline->setShaderStages({
      {QRhiShaderStage::Vertex, vertex_shader},
      {QRhiShaderStage::Fragment, fragment_shader},
  });
  pipeline->setVertexInputLayout(input_layout);
  pipeline->setShaderResourceBindings(shader_resources.get());
  pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  pipeline->setCullMode(QRhiGraphicsPipeline::None);
  pipeline->setSampleCount(sampleCount());
  pipeline->setTargetBlends({PJ::straightAlphaBlend()});
  pipeline->setRenderPassDescriptor(color_target_.renderPass());
  if (!createRhiPipeline(*this, *pipeline, "color")) {
    return false;
  }
  shader_resources_ = shader_resources.release();
  vertex_buffer_ = vertex_buffer.release();
  vertex_buffer_capacity_ = kInitialVertexBufferCapacity;
  pipeline_ = pipeline.release();

  // The text pipeline is optional: without it legends and labels drop, the
  // curves still draw. There is no raster fallback in the browser.
  const auto drop_text = [] {
    qCWarning(lcPlotRhi) << "PlotRhiCanvas: failed to create the text pipeline; rendering geometry without text";
    return true;
  };
  auto text_sampler = std::unique_ptr<QRhiSampler>(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!text_sampler->create()) {
    return drop_text();
  }
  auto text_placeholder_texture = std::unique_ptr<QRhiTexture>(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  if (!text_placeholder_texture->create()) {
    return drop_text();
  }
  auto text_layout_bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  text_layout_bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
      0, QRhiShaderResourceBinding::FragmentStage, text_placeholder_texture.get(), text_sampler.get())});
  if (!text_layout_bindings->create()) {
    return drop_text();
  }
  constexpr qsizetype kInitialTextVertexBufferCapacity = 16 * 1024;
  auto text_vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, kInitialTextVertexBufferCapacity));
  if (!text_vertex_buffer->create()) {
    return drop_text();
  }
  QRhiVertexInputLayout text_input_layout;
  text_input_layout.setBindings({QRhiVertexInputBinding(sizeof(TextVertex))});
  text_input_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, offsetof(TextVertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, offsetof(TextVertex, u)),
  });
  auto text_pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
  text_pipeline->setShaderStages({
      {QRhiShaderStage::Vertex, text_vertex_shader},
      {QRhiShaderStage::Fragment, text_fragment_shader},
  });
  text_pipeline->setVertexInputLayout(text_input_layout);
  text_pipeline->setShaderResourceBindings(text_layout_bindings.get());
  text_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  text_pipeline->setCullMode(QRhiGraphicsPipeline::None);
  text_pipeline->setSampleCount(sampleCount());
  QRhiGraphicsPipeline::TargetBlend text_blend;
  text_blend.enable = true;
  // QPainter produces premultiplied RGBA rasters.
  text_blend.srcColor = QRhiGraphicsPipeline::One;
  text_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  text_blend.srcAlpha = QRhiGraphicsPipeline::One;
  text_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  text_pipeline->setTargetBlends({text_blend});
  text_pipeline->setRenderPassDescriptor(color_target_.renderPass());
  if (!createRhiPipeline(*this, *text_pipeline, "text")) {
    return drop_text();
  }

  text_sampler_ = text_sampler.release();
  text_placeholder_texture_ = text_placeholder_texture.release();
  text_layout_bindings_ = text_layout_bindings.release();
  text_vertex_buffer_ = text_vertex_buffer.release();
  text_vertex_buffer_capacity_ = kInitialTextVertexBufferCapacity;
  text_pipeline_ = text_pipeline.release();
  return true;
}

void PlotRhiCanvas::initialize(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  // A new QRhi (reparent to another top-level, device loss) invalidates every
  // resource; a new output format or sample count invalidates the pipelines.
  if (rhi_ != current_rhi) {
    noteRhiAdoption(*this, rhi_, current_rhi);
    releaseResources();
    rhi_ = current_rhi;
  }
  // The widget rebuilds its colour buffer in place on a resize: refresh the
  // target on it every time.
  if (!color_target_.ensure(*rhi_, colorTexture(), msaaColorBuffer(), resolveTexture(), /*refresh=*/true)) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  const RhiTargetKey target = RhiTargetKey::of(*color_target_.target(), sampleCount());
  if (target != built_target_) {
    releaseDrawResources();
    built_target_ = target;
  }
  renderer_backend_ = backendName(*rhi_);
  // After a failure this is the only retry: render() does not call it again
  // until the QRhi or the render target changes.
  if (pipeline_ == nullptr && !createPipeline(command_buffer)) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  (void)failure_.set({});
  if (!ready_reported_) {
    ready_reported_ = true;
    const QRhiDriverInfo driver = rhi_->driverInfo();
    qInfo(
        "PJ_WASM_PLOT_RHI_READY backend=%s device=%s samples=%d", qPrintable(renderer_backend_),
        driver.deviceName.constData(), sampleCount());
  }
}

bool PlotRhiCanvas::rebuildTextLayers(
    const QwtPlot& plot, qreal device_pixel_ratio, const QTransform& to_clip, QImage& upload, qsizetype& bytes) {
  TextRasterBuild text_raster =
      text_pipeline_ != nullptr
          ? buildTextRasters(&plot, size(), device_pixel_ratio, text_raster_scratch_, layers_->text_cache)
          : TextRasterBuild{};
  text_layer_rasterizations_ += text_raster.rasterized;
  layers_->text_cache = std::move(text_raster.cache);
  bool force_text_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  force_text_failure = property("pjWasmForceTextAtlasFailure").toBool();
  if (force_text_failure) {
    const int attempts = property("pjWasmTextFailureAttempts").toInt() + 1;
    setProperty("pjWasmTextFailureAttempts", attempts);
    qInfo("PJ_WASM_TEXT_FAILURE_ATTEMPT count=%d", attempts);
  }
#endif
  // The same layers at the same places over the same canvas: the uploaded
  // atlas and quads are still right.
  TextSignature signature{.canvas = size(), .truncated = text_raster.truncated};
  signature.layer_keys.reserve(static_cast<qsizetype>(text_raster.layers.size()));
  for (const TextRasterLayer& layer : text_raster.layers) {
    signature.layer_keys.append(layer.cache_key);
  }
  if (!force_text_failure && !text_raster.allocation_failed && signature == layers_->text_signature) {
    // Equal keys may come from other items (a marker replaced by an identical
    // one): the spans follow this build's items.
    for (std::size_t index = 0; index < std::min(layers_->text_spans.size(), text_raster.layers.size()); ++index) {
      layers_->text_spans[index].item = text_raster.layers[index].source_item;
    }
    return false;
  }
  layers_->text_signature = {};
  layers_->text_vertices.clear();
  layers_->text_spans.clear();
  bytes = 0;
  upload = {};

  bool retry = false;
  bool text_failed = false;
  QString text_failure_reason;
  const auto drop_text_for_frame = [&](const QString& reason) {
    if (!text_failed && !text_failure_reported_) {
      // Retry one later frame for a transient allocation failure. A repeated
      // failure remains geometry-only until another dirty source (semantic,
      // view, rebind, or lifecycle) instead of entering an unbounded loop.
      retry = true;
    }
    text_failed = true;
    text_failure_reason = reason;
    text_raster.layers.clear();
    text_raster.truncated = true;
    refreshTextRasterStats(text_raster);
    layers_->text_vertices.clear();
    layers_->text_spans.clear();
    bytes = 0;
    upload = {};
  };
  const int max_texture_size = rhi_->resourceLimit(QRhi::TextureSizeMax);
  if (force_text_failure) {
    drop_text_for_frame(u"test-injected text atlas failure"_s);
  } else if (text_raster.allocation_failed || !packTextAtlas(text_raster, max_texture_size, upload)) {
    drop_text_for_frame(u"failed to allocate or pack the text atlas"_s);
  }
  if (!text_failed && !text_raster.layers.empty()) {
    const QSize required = upload.size();
    if (text_texture_ == nullptr || required.width() > text_texture_capacity_.width() ||
        required.height() > text_texture_capacity_.height()) {
      const QSize grown(
          std::min(max_texture_size, std::max(required.width(), std::max(64, text_texture_capacity_.width() * 2))),
          std::min(max_texture_size, std::max(required.height(), std::max(64, text_texture_capacity_.height() * 2))));
      if (grown.width() < required.width() || grown.height() < required.height()) {
        drop_text_for_frame(tr("text atlas %1x%2 exceeds backend texture limit %3")
                                .arg(required.width())
                                .arg(required.height())
                                .arg(max_texture_size));
      }
      if (!text_failed) {
        auto replacement_texture = std::unique_ptr<QRhiTexture>(rhi_->newTexture(QRhiTexture::RGBA8, grown));
        if (!replacement_texture->create()) {
          drop_text_for_frame(u"failed to allocate the text atlas texture"_s);
        }
        if (!text_failed) {
          auto replacement_bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
          replacement_bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
              0, QRhiShaderResourceBinding::FragmentStage, replacement_texture.get(), text_sampler_)});
          if (!replacement_bindings->create()) {
            drop_text_for_frame(u"failed to bind the text atlas texture"_s);
          } else {
            delete text_bindings_;
            delete text_texture_;
            text_texture_ = replacement_texture.release();
            text_bindings_ = replacement_bindings.release();
            text_texture_capacity_ = grown;
          }
        }
      }
    }

    if (!text_failed) {
      for (const TextRasterLayer& layer : text_raster.layers) {
        const QRectF uv_rect(
            static_cast<qreal>(layer.atlas_position.x()) / text_texture_capacity_.width(),
            static_cast<qreal>(layer.atlas_position.y()) / text_texture_capacity_.height(),
            static_cast<qreal>(layer.image.width()) / text_texture_capacity_.width(),
            static_cast<qreal>(layer.image.height()) / text_texture_capacity_.height());
        const std::size_t before = layers_->text_vertices.size();
        appendTextQuad(layers_->text_vertices, layer.logical_rect, uv_rect, size(), to_clip);
        layers_->text_spans.push_back({layer.source_item, before, layers_->text_vertices.size() - before});
      }
      bytes = static_cast<qsizetype>(layers_->text_vertices.size() * sizeof(TextVertex));
      if (!growStaticBuffer(*rhi_, text_vertex_buffer_, text_vertex_buffer_capacity_, bytes)) {
        drop_text_for_frame(u"failed to allocate the text vertex buffer"_s);
      }
    }
    if (!text_failed) {
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
      qreal raster_scale_x = std::numeric_limits<qreal>::max();
      qreal raster_scale_y = std::numeric_limits<qreal>::max();
      for (const TextRasterLayer& layer : text_raster.layers) {
        raster_scale_x = std::min(raster_scale_x, layer.image.width() / layer.logical_rect.width());
        raster_scale_y = std::min(raster_scale_y, layer.image.height() / layer.logical_rect.height());
      }
      qInfo(
          "PJ_WASM_PLOT_TEXT_OK legend_layers=%lld marker_labels=%lld alpha_pixels=%lld vertices=%llu atlas=%dx%d "
          "rect=%.3f,%.3f,%.3fx%.3f raster_scale=%.3f,%.3f",
          static_cast<long long>(text_raster.legend_layers), static_cast<long long>(text_raster.marker_layers),
          static_cast<long long>(text_raster.alpha_pixels),
          static_cast<unsigned long long>(layers_->text_vertices.size()), upload.width(), upload.height(),
          text_raster.logical_bounds.x(), text_raster.logical_bounds.y(), text_raster.logical_bounds.width(),
          text_raster.logical_bounds.height(), raster_scale_x, raster_scale_y);
#endif
    }
  }
  if (text_failed) {
    if (!text_failure_reported_) {
      text_failure_reported_ = true;
      qCWarning(lcPlotRhi) << "PlotRhiCanvas:" << text_failure_reason << "; rendering geometry without text";
    }
  } else {
    layers_->text_signature = signature;
    text_failure_reported_ = false;
    if (text_raster.truncated) {
      if (!text_truncation_reported_) {
        text_truncation_reported_ = true;
        qCWarning(lcPlotRhi) << "PlotRhiCanvas: text layer/atlas ceiling reached; later text items were omitted";
      }
    } else {
      text_truncation_reported_ = false;
    }
  }
  return retry;
}

void PlotRhiCanvas::render(QRhiCommandBuffer* command_buffer) {
  QElapsedTimer cpu_timer;
  cpu_timer.start();
  // QRhiWidget does not re-run initialize() for every new QRhi (a grab of a
  // hidden widget creates one), so never draw with resources of another QRhi.
  // A new colour buffer always gets its initialize().
  if (rhi_ != rhi()) {
    initialize(command_buffer);
  }
  QwtPlot* plot = plot_;
  const QColor clear_color = plot != nullptr ? plot->canvasBackground().color() : QColor(Qt::transparent);
  bool force_pipeline_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  force_pipeline_failure = property("pjWasmForcePipelineFailure").toBool();
#endif
  // Failed state (no device, pipeline, or render target): a clear-only pass.
  // Without a render target no pass can be recorded, so the widget's untouched
  // colour buffer is composited under the opaque failure banner (on desktop the
  // raster canvas then replaces this one).
  QRhiTextureRenderTarget* render_target = color_target_.target();
  if (force_pipeline_failure || rhi_ == nullptr || pipeline_ == nullptr || render_target == nullptr) {
    if (render_target != nullptr) {
      command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
      command_buffer->endPass();
    }
    reportFailure(/*emit_signal=*/true);
    return;
  }
  if (plot == nullptr) {
    // A null plot is a transient wiring state (setPlot() runs right after
    // construction), not a GPU failure. Record a clear-only pass so the frame is
    // well-defined, without the failure banner/telemetry. Stay dirty so the
    // first frame after wiring rebuilds geometry.
    command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    return;
  }
  const QSize target_size = render_target->pixelSize();
  const qreal device_pixel_ratio = renderTargetScale(size(), target_size);
  // Qwt owns the canvas brush. QRhiWidget does not participate in Qwt's
  // QPainter background path, so read the brush back from the plot instead of
  // assuming the platform's accelerated-surface palette.
  const auto fail_rebuild = [this, command_buffer, render_target, &clear_color]() {
    // A failed allocation/upload must not leave an old text-bearing frame on
    // screen or hand QRhi an unrecorded render callback. Submit a defined clear
    // and keep the state dirty. The renderFailed() onset makes a desktop
    // PlotWidgetBase swap in the raster canvas for good; in the browser, which
    // keeps this canvas, a later frame retries from live Qwt state.
    command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    env_key_.reset();
    reportFailure(/*emit_signal=*/true);
  };

  // Rebuild the curve layer when a full replot flagged it, the view (env key)
  // or the curve structure changed; the overlay and text on any rebuild or an
  // overlay-only replot. Otherwise re-draw the retained buffers without
  // rebuilding or re-uploading: compositor refreshes and expose repaints hit
  // neither branch.
  const QColor frame_color = palette().color(QPalette::WindowText);
  const std::optional<std::uint64_t> env_key = computeEnvKey(plot, size(), target_size, clear_color, frame_color);
  const CurveKeys curve_keys = computeCurveKeys(plot);
  // A full replot reuses the curve layer when every curve's content revision,
  // the view and the structure are unchanged; a curve without a revision
  // rebuilds it.
  const bool rebuild_curves = curves_dirty_ || !env_key.has_value() || env_key != env_key_ ||
                              curve_keys.structure != curve_key_ ||
                              (curve_keys.content.has_value() ? curve_keys.content != content_key_ : replot_requested_);
  const bool rebuild_overlay = rebuild_curves || overlay_dirty_;
  const QTransform to_clip = logicalToClip(size(), rhi_->clipSpaceCorrMatrix());

  qsizetype bytes = 0;
  qsizetype overlay_bytes = 0;
  qsizetype text_bytes = 0;
  QImage text_upload;
  bool retry_text_rebuild = false;
  if (rebuild_curves) {
    vertices_.resize(0);  // keeps the allocation
    indices_.resize(0);
    const PlotGeometry geometry = buildCurveLayer(
        vertices_, indices_, plot, size(), device_pixel_ratio,
        static_cast<std::size_t>(std::max(1, target_size.width())), to_clip,
        kMapSamplesOnce ? &mapped_scratch_ : nullptr, frame_color);
    if (mapped_scratch_.capacity() > kRetainedMappedSamples) {
      mapped_scratch_.clear();
      mapped_scratch_.shrink_to_fit();
    }
    ++curve_layer_builds_;
    last_vertex_count_ = static_cast<std::size_t>(vertices_.size()) / sizeof(Vertex);
    bytes = vertices_.size() + indices_.size();
    layers_->curve_spans = geometry.item_spans;
    layers_->frame_span = geometry.frame;

    // Warn once per limit onset (not per frame) when the frame-wide ceiling or
    // an exact per-curve share drops geometry; clear the latch on a
    // within-budget frame so a later onset re-warns.
    if (geometry.truncated || geometry.budget_limited_curves > 0 || geometry.budget_reduced_curves > 0) {
      if (!truncation_reported_) {
        truncation_reported_ = true;
        if (geometry.truncated) {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: geometry vertex ceiling"
                               << static_cast<qulonglong>(kMaxGeometryVertices) << "reached; plot geometry truncated";
        } else if (geometry.budget_limited_curves > 0) {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: exact per-curve vertex shares limited"
                               << static_cast<qulonglong>(geometry.budget_limited_curves)
                               << "curve(s) even at endpoint density; incomplete prefixes were omitted";
        } else {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: reducer budgets lowered for"
                               << static_cast<qulonglong>(geometry.budget_reduced_curves)
                               << "curve(s) to keep every curve complete; discarded trial vertices="
                               << static_cast<qulonglong>(geometry.budget_discarded_trial_vertices);
        }
      }
    } else {
      truncation_reported_ = false;
    }

#ifdef PJ_TARGET_WASM
    // Content breadcrumb: emit on the first curve-bearing frame and again
    // whenever the plotted content changes. Format is fixed (Playwright greps).
    // Browser-only: a streaming desktop plot would log it on every frame.
    const QwtPlotItemList curves = plot->itemList(QwtPlotItem::Rtti_PlotCurve);
    const auto* first_curve = curves.empty() ? nullptr : dynamic_cast<const QwtPlotCurve*>(curves.front());
    if (first_curve != nullptr && first_curve->dataSize() > 0) {
      const QPointF first = first_curve->sample(0);
      const QPointF last = first_curve->sample(first_curve->dataSize() - 1);
      std::uint64_t fingerprint = PJ::kFnv1aOffsetBasis;
      hashUInt(fingerprint, static_cast<std::uint64_t>(first_curve->dataSize()));
      hashDouble(fingerprint, first.x());
      hashDouble(fingerprint, first.y());
      hashDouble(fingerprint, last.x());
      hashDouble(fingerprint, last.y());
      hashUInt(fingerprint, static_cast<std::uint64_t>(first_curve->pen().style()));
      hashDouble(fingerprint, first_curve->pen().widthF());
      hashUInt(fingerprint, static_cast<std::uint64_t>(geometry.curve_vertices));
      hashUInt(fingerprint, geometry.retained_samples);
      hashUInt(fingerprint, geometry.input_runs);
      hashUInt(fingerprint, geometry.dropped_runs);
      hashUInt(fingerprint, geometry.budget_reduced_curves);
      hashUInt(fingerprint, geometry.budget_reduction_retries);
      hashUInt(fingerprint, geometry.budget_discarded_trial_vertices);
      hashUInt(fingerprint, geometry.budget_dropped_samples);
      hashUInt(fingerprint, geometry.budget_omitted_curves);
      hashUInt(fingerprint, geometry.budget_limited_curves);
      hashUInt(fingerprint, geometry.truncated ? 1U : 0U);
      if (!content_reported_ || fingerprint != content_fingerprint_) {
        content_reported_ = true;
        content_fingerprint_ = fingerprint;
        qInfo(
            "PJ_WASM_PLOT_FRAME_OK curves=%d vertices=%llu curve_vertices=%llu first_curve=%s samples=%llu "
            "reduced_samples=%llu runs=%llu dropped_runs=%llu budget_limited_curves=%llu "
            "budget_reduced_curves=%llu budget_reduction_retries=%llu budget_discarded_trial_vertices=%llu "
            "budget_dropped_samples=%llu budget_omitted_curves=%llu "
            "endpoints=%.9g,%.9g..%.9g,%.9g logical=%dx%d target=%dx%d clear=%s pen=%s truncated=%d",
            curves.size(), static_cast<unsigned long long>(last_vertex_count_),
            static_cast<unsigned long long>(geometry.curve_vertices), qPrintable(first_curve->title().text()),
            static_cast<unsigned long long>(first_curve->dataSize()),
            static_cast<unsigned long long>(geometry.retained_samples),
            static_cast<unsigned long long>(geometry.input_runs),
            static_cast<unsigned long long>(geometry.dropped_runs),
            static_cast<unsigned long long>(geometry.budget_limited_curves),
            static_cast<unsigned long long>(geometry.budget_reduced_curves),
            static_cast<unsigned long long>(geometry.budget_reduction_retries),
            static_cast<unsigned long long>(geometry.budget_discarded_trial_vertices),
            static_cast<unsigned long long>(geometry.budget_dropped_samples),
            static_cast<unsigned long long>(geometry.budget_omitted_curves), first.x(), first.y(), last.x(), last.y(),
            width(), height(), target_size.width(), target_size.height(), qPrintable(clear_color.name(QColor::HexArgb)),
            qPrintable(first_curve->pen().color().name(QColor::HexArgb)), geometry.truncated ? 1 : 0);
      }
    }
#endif

    // Never grow past the frame-wide ceilings: the builder already caps the
    // vertex count there, and a primitive has at most three indices per vertex.
    if (!growStaticBuffer(
            *rhi_, vertex_buffer_, vertex_buffer_capacity_, vertices_.size(),
            static_cast<qsizetype>(kMaxGeometryVertices * sizeof(Vertex))) ||
        !growStaticBuffer(
            *rhi_, index_buffer_, index_buffer_capacity_, indices_.size(),
            static_cast<qsizetype>(3 * kMaxGeometryVertices * sizeof(quint32)), QRhiBuffer::IndexBuffer)) {
      fail_rebuild();
      return;
    }
  }

  if (rebuild_overlay) {
    overlay_vertices_.resize(0);
    overlay_indices_.resize(0);
    layers_->overlay_spans =
        buildOverlayLayer(overlay_vertices_, overlay_indices_, plot, size(), device_pixel_ratio, to_clip);
    overlay_bytes = overlay_vertices_.size() + overlay_indices_.size();
    if (!growStaticBuffer(
            *rhi_, overlay_vertex_buffer_, overlay_vertex_buffer_capacity_, overlay_vertices_.size(),
            static_cast<qsizetype>(kMaxGeometryVertices * sizeof(Vertex))) ||
        !growStaticBuffer(
            *rhi_, overlay_index_buffer_, overlay_index_buffer_capacity_, overlay_indices_.size(),
            static_cast<qsizetype>(3 * kMaxGeometryVertices * sizeof(quint32)), QRhiBuffer::IndexBuffer)) {
      fail_rebuild();
      return;
    }
    retry_text_rebuild = rebuildTextLayers(*plot, device_pixel_ratio, to_clip, text_upload, text_bytes);

    // Qwt paints items in increasing Z and stable attachment order. The layers
    // live in separate GPU buffers, so retain an explicit batch list instead of
    // flattening every label above every marker. This matters when the
    // playback label, hover marker, and XY ride-along markers overlap.
    layers_->draw_batches.clear();
    const auto append_batch = [this](Layer layer, const ItemSpan& span) {
      if (span.count == 0) {
        return;
      }
      const auto first = static_cast<std::uint32_t>(span.first);
      const auto count = static_cast<std::uint32_t>(span.count);
      if (!layers_->draw_batches.empty()) {
        DrawBatch& previous = layers_->draw_batches.back();
        if (previous.layer == layer && previous.first + previous.count == first) {
          previous.count += count;
          return;
        }
      }
      layers_->draw_batches.push_back({.layer = layer, .first = first, .count = count});
    };
    const auto append_item = [&append_batch](Layer layer, const std::vector<ItemSpan>& spans, const void* item) {
      const auto span = std::find_if(
          spans.cbegin(), spans.cend(), [item](const ItemSpan& candidate) { return candidate.item == item; });
      if (span != spans.cend()) {
        append_batch(layer, *span);
      }
    };
    for (const QwtPlotItem* item : plot->itemList()) {
      append_item(Layer::Curves, layers_->curve_spans, item);
      append_item(Layer::Overlay, layers_->overlay_spans, item);
      append_item(Layer::Text, layers_->text_spans, item);
    }
    append_batch(Layer::Curves, layers_->frame_span);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    QByteArray batch_sequence;
    batch_sequence.reserve(static_cast<qsizetype>(layers_->draw_batches.size()));
    for (const DrawBatch& batch : layers_->draw_batches) {
      batch_sequence.append(batch.layer == Layer::Text ? 'T' : 'G');
    }
    qInfo(
        "PJ_WASM_PLOT_DRAW_BATCHES count=%lld sequence=%s", static_cast<long long>(layers_->draw_batches.size()),
        batch_sequence.constData());
#endif
  }
  if (rebuild_curves) {
    env_key_ = env_key;
    curve_key_ = curve_keys.structure;
    content_key_ = curve_keys.content;
  }

  QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
  if (rebuild_curves && bytes > 0) {
    // In pieces: anv records a copy with an awkward size slowly.
    uploadStaticBufferInPieces(updates, vertex_buffer_, 0, vertices_);
    uploadStaticBufferInPieces(updates, index_buffer_, 0, indices_);
  }
  if (rebuild_overlay && overlay_bytes > 0) {
    uploadStaticBufferInPieces(updates, overlay_vertex_buffer_, 0, overlay_vertices_);
    uploadStaticBufferInPieces(updates, overlay_index_buffer_, 0, overlay_indices_);
  }
  if (text_bytes > 0 && text_texture_ != nullptr && !text_upload.isNull()) {
    QRhiTextureSubresourceUploadDescription upload(text_upload);
    upload.setSourceSize(text_upload.size());
    updates->uploadTexture(text_texture_, QRhiTextureUploadDescription({0, 0, upload}));
    uploadStaticBufferInPieces(
        updates, text_vertex_buffer_, 0, static_cast<quint32>(text_bytes), layers_->text_vertices.data());
  }
  command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, updates);
  const QRhiViewport viewport(
      0.0F, 0.0F, static_cast<float>(target_size.width()), static_cast<float>(target_size.height()));
  for (const DrawBatch& batch : layers_->draw_batches) {
    if (batch.layer == Layer::Text) {
      if (text_pipeline_ == nullptr || text_bindings_ == nullptr || text_vertex_buffer_ == nullptr) {
        continue;
      }
      command_buffer->setGraphicsPipeline(text_pipeline_);
      command_buffer->setShaderResources(text_bindings_);
      const QRhiCommandBuffer::VertexInput binding(text_vertex_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &binding);
      // After the pipeline: QRhi records the viewport as dynamic state of the
      // bound pipeline, and Vulkan draws nothing without it.
      command_buffer->setViewport(viewport);
      command_buffer->draw(batch.count, 1, batch.first);
      continue;
    }
    const bool overlay = batch.layer == Layer::Overlay;
    QRhiBuffer* vertices = overlay ? overlay_vertex_buffer_ : vertex_buffer_;
    QRhiBuffer* indices = overlay ? overlay_index_buffer_ : index_buffer_;
    if (vertices == nullptr || indices == nullptr) {
      continue;
    }
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setShaderResources(shader_resources_);
    const QRhiCommandBuffer::VertexInput binding(vertices, 0);
    command_buffer->setVertexInput(0, 1, &binding, indices, 0, QRhiCommandBuffer::IndexUInt32);
    command_buffer->setViewport(viewport);
    command_buffer->drawIndexed(batch.count, 1, batch.first);
  }
  command_buffer->endPass();
  // Allocation failures are retryable. If the retry succeeds without a QRhi
  // re-initialize() cycle, remove the overlay here rather than leaving a healthy
  // renderer hidden underneath a stale diagnostic.
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  (void)failure_.set({});
  last_render_cpu_ms_ = static_cast<double>(cpu_timer.nsecsElapsed()) / 1e6;
  last_gpu_frame_ms_ = command_buffer->lastCompletedGpuTime() * 1000.0;
  // Geometry is now uploaded and drawn. A text-only allocation failure gets one
  // delayed retry while the usable geometry remains visible; repeated failures
  // wait for another semantic/view/rebind/lifecycle dirty source instead of
  // rebuilding every frame.
  curves_dirty_ = false;
  replot_requested_ = false;
  overlay_dirty_ = retry_text_rebuild;
  if (retry_text_rebuild) {
    QTimer::singleShot(100, this, [this]() {
      if (overlay_dirty_) {
        update();
      }
    });
  }
}

void PlotRhiCanvas::reportFailure(bool emit_signal) {
  if (failure_label_ == nullptr) {
    failure_label_ = new QLabel(tr("GPU plot renderer unavailable"), this);
    failure_label_->setAlignment(Qt::AlignCenter);
    failure_label_->setStyleSheet(u"background:#5b1b1b;color:white;padding:8px;"_s);
  }
  failure_label_->setGeometry(rect());
  failure_label_->show();
  failure_label_->raise();
  if (failure_.set(failure_label_->text()) != RhiFailureLatch::Change::kEntered) {
    return;
  }
#ifdef PJ_TARGET_WASM
  qCritical("PJ_WASM_PLOT_RHI_FAILED");
#else
  qCWarning(lcPlotRhi) << "PlotRhiCanvas:" << failure_.reason();
#endif
  if (emit_signal) {
    emit renderFailed();
  }
}

}  // namespace PJ
