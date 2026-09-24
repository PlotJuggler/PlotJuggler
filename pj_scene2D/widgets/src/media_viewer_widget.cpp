// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/media_viewer_widget.h"

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QTimer>
#include <QVector4D>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/overlay_geometry.h"
#include "pj_scene2d_core/video_color.h"  // buildYuvMatrix (BT.601/709 + limited/full range)
#include "pj_scene2d_widgets/pixel_inspector.h"
#include "pj_widgets/Colormap.h"  // shared Colormap enum + buildColormapLut + colormapGlsl
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/GraphicsApi.h"
using namespace Qt::StringLiterals;

void pjMediaQtInitResources() {
  Q_INIT_RESOURCE(shaders);
}

namespace PJ {

static constexpr int kPointInspectorCropSize = 10;

MediaViewerWidget::MediaViewerWidget(QWidget* parent) : QRhiWidget(parent) {
  const auto fw_theme = theme::appTheme();
  clear_color_ = theme::surface(theme::Surface::DataBackdrop, fw_theme);
  setApi(preferredGraphicsApi());
  setObjectName(u"mediaViewerCanvas"_s);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
#ifdef PJ_TARGET_WASM
  connect(this, &QRhiWidget::frameSubmitted, this, [this]() {
    if (composition_refresh_queued_) {
      return;
    }
    composition_refresh_queued_ = true;
    QWidget* top_level = window();
    QTimer::singleShot(0, top_level, [top_level]() { top_level->update(); });
  });
#endif
  static bool resources_initialized = [] {
    pjMediaQtInitResources();
    return true;
  }();
  (void)resources_initialized;
}

MediaViewerWidget::~MediaViewerWidget() {
  // Qt does NOT call releaseResources() on widget destruction — only when the QRhi
  // context changes (reparent / window move). Without this, every GPU resource
  // (the pipelines, textures, buffers, samplers, SRBs, glyph cache and pixel-layer
  // textures) leaks each time a dock destroys a viewer. releaseResources() is
  // idempotent (it nulls each pointer), and the QRhiWidget base that owns the QRhi
  // is destroyed AFTER this derived destructor, so the rhi is still alive here.
  releaseResources();
}

void MediaViewerWidget::setClearColor(const QColor& color) {
  const auto fw_theme = theme::appTheme();
  QColor next = color.isValid() ? color : theme::surface(theme::Surface::DataBackdrop, fw_theme);
  next.setAlpha(255);
  if (next == clear_color_) {
    return;
  }
  clear_color_ = next;
  update();
}

QColor MediaViewerWidget::clearColor() const {
  return clear_color_;
}

void MediaViewerWidget::setPointInspectorEnabled(bool enabled) {
  if (point_inspector_enabled_.load(std::memory_order_relaxed) == enabled) {
    return;
  }
  point_inspector_enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    hidePointInspector();
    return;
  }
  refreshPointInspector();
}

bool MediaViewerWidget::pointInspectorEnabled() const noexcept {
  return point_inspector_enabled_.load(std::memory_order_relaxed);
}

void MediaViewerWidget::resetView() {
  static_cast<void>(setViewState({}));
}

MediaViewState MediaViewerWidget::viewState() const noexcept {
  return MediaViewState{.zoom = zoom_, .pan_x = pan_x_, .pan_y = pan_y_};
}

bool MediaViewerWidget::isViewStateValid(const MediaViewState& state) noexcept {
  constexpr float kMinZoom = 1.0f;
  constexpr float kMaxZoom = 20.0f;
  if (!std::isfinite(state.zoom) || !std::isfinite(state.pan_x) || !std::isfinite(state.pan_y) ||
      state.zoom < kMinZoom || state.zoom > kMaxZoom) {
    return false;
  }
  return state.zoom != kMinZoom || (state.pan_x == 0.0f && state.pan_y == 0.0f);
}

bool MediaViewerWidget::setViewState(const MediaViewState& state) {
  if (!isViewStateValid(state)) {
    return false;
  }
  const MediaViewState previous = viewState();
  if (previous == state) {
    return true;
  }
  zoom_ = state.zoom;
  pan_x_ = state.pan_x;
  pan_y_ = state.pan_y;
  update();
  if (zoom_ != previous.zoom) {
    emit zoomChanged(zoom_);
  }
  if (point_inspector_enabled_.load(std::memory_order_relaxed) &&
      point_inspector_active_.load(std::memory_order_relaxed)) {
    refreshPointInspector();
  }
  return true;
}

void MediaViewerWidget::setMediaSource(MediaSource* source) {
  std::lock_guard lock(frame_mutex_);
  media_source_ = source;
  // Tell the new source whether the GPU can rectify (already probed if the widget
  // is initialized; default false until then, which the next initialize() corrects).
  applyGpuRectifyCapability();
  inspector_frame_ = {};
  point_inspector_active_.store(false, std::memory_order_relaxed);
  hidePointInspector();
  last_overlays_.clear();
  overlays_dirty_ = true;
  // Drop the previous source's pixel-layer stack so its segmentation/depth
  // overlays don't keep compositing over the new source. The GPU textures are
  // reconciled when the next frame arrives; until then pixel_layers_active_ is
  // false, so they stay inert.
  resetPendingPixelLayers();
  // Drop glyph textures keyed to the previous source's labels; new source
  // likely brings a different label set, and the cache currently has no LRU.
  clearTextCache();
}

namespace {

// Overlay tessellation (lines/points/fills/circles) lives in the backend-agnostic
// pj_scene2d_core/overlay_geometry.h so it can be unit-tested; only the
// Qt/QRhi-specific helpers below remain here.

QRhiScissor imageScissor(const QMatrix4x4& view, const QSize& output_size) {
  if (output_size.width() <= 0 || output_size.height() <= 0) {
    return QRhiScissor(0, 0, 0, 0);
  }

  float min_x = 1.0e9f;
  float min_y = 1.0e9f;
  float max_x = -1.0e9f;
  float max_y = -1.0e9f;
  const QVector4D corners[] = {
      {-1.0f, -1.0f, 0.0f, 1.0f},
      {1.0f, -1.0f, 0.0f, 1.0f},
      {-1.0f, 1.0f, 0.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
  };

  for (const auto& corner : corners) {
    QVector4D point = view * corner;
    if (point.w() != 0.0f) {
      point /= point.w();
    }
    min_x = std::min(min_x, point.x());
    min_y = std::min(min_y, point.y());
    max_x = std::max(max_x, point.x());
    max_y = std::max(max_y, point.y());
  }

  min_x = std::clamp(min_x, -1.0f, 1.0f);
  min_y = std::clamp(min_y, -1.0f, 1.0f);
  max_x = std::clamp(max_x, -1.0f, 1.0f);
  max_y = std::clamp(max_y, -1.0f, 1.0f);
  if (max_x <= min_x || max_y <= min_y) {
    return QRhiScissor(0, 0, 0, 0);
  }

  const auto width = static_cast<float>(output_size.width());
  const auto height = static_cast<float>(output_size.height());
  const int x0 = std::max(0, static_cast<int>(std::floor((min_x * 0.5f + 0.5f) * width)));
  const int x1 = std::min(output_size.width(), static_cast<int>(std::ceil((max_x * 0.5f + 0.5f) * width)));
  const int y0 = std::max(0, static_cast<int>(std::floor((1.0f - max_y) * 0.5f * height)));
  const int y1 = std::min(output_size.height(), static_cast<int>(std::ceil((1.0f - min_y) * 0.5f * height)));
  return QRhiScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

QRhiGraphicsPipeline::TargetBlend alphaBlend() {
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  return blend;
}

QRhiVertexInputLayout colorVertexInputLayout() {
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(24)});  // stride: vec2 pos + vec4 color
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, 8),
  });
  return layout;
}

QRhiVertexInputLayout textVertexInputLayout() {
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(32)});  // stride: vec2 pos + vec2 uv + vec4 color
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 8),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, 16),
  });
  return layout;
}

// `y_sampler` filters the y-plane (binding 1) independently of the u/v planes
// (binding 2/3, `sampler`). The depth path needs this: its R32F depth in y_tex
// must be sampled NEAREST so a pixel never interpolates a valid metric depth with
// the 0.0 no-data sentinel (that would paint a "near"-colored fringe around depth
// holes/silhouettes), while its colormap LUT in u_tex still wants LINEAR.
void setTextureLayerBindings(
    QRhiShaderResourceBindings* srb, QRhiBuffer* uniform_buf, QRhiTexture* tex_y, QRhiTexture* tex_u,
    QRhiTexture* tex_v, QRhiSampler* sampler, QRhiTexture* tex_remap, QRhiSampler* remap_sampler,
    QRhiSampler* y_sampler) {
  srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniform_buf),
      QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex_y, y_sampler),
      QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, tex_u, sampler),
      QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, tex_v, sampler),
      QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, tex_remap, remap_sampler),
  });
}

}  // namespace

void MediaViewerWidget::setTimestamp(int64_t ts_ns) {
  if (media_source_ != nullptr) {
    media_source_->setTimestamp(ts_ns);
  }
}

bool MediaViewerWidget::hasRetainedUploadableFrameLocked() const {
  return !pending_decoded_.isNull() && isUploadablePixelFormat(pending_decoded_.format);
}

void MediaViewerWidget::resetPendingPixelLayers() {
  pending_pixel_layers_.clear();
  has_pending_pixel_layers_ = false;
  pixel_layers_active_ = false;
}

void MediaViewerWidget::releaseResources() {
#ifdef PJ_TARGET_WASM
  composition_refresh_queued_ = false;
#endif
  hidePointInspector();
  delete pipeline_;
  pipeline_ = nullptr;
  delete composite_pipeline_;
  composite_pipeline_ = nullptr;
  destroyTextureLayer(base_texture_);
  delete sampler_;
  sampler_ = nullptr;
  delete mag_nearest_sampler_;
  mag_nearest_sampler_ = nullptr;
  delete remap_sampler_;
  remap_sampler_ = nullptr;
  delete remap_placeholder_tex_;
  remap_placeholder_tex_ = nullptr;
  clearPixelLayerTextures();
  destroyOverlayPipeline(points_overlay_);
  destroyOverlayPipeline(thick_overlay_);
  destroyOverlayPipeline(text_overlay_);
  clearTextCache();
  delete marker_uniform_buf_;
  marker_uniform_buf_ = nullptr;
  delete text_sampler_;
  text_sampler_ = nullptr;
  delete text_placeholder_tex_;
  text_placeholder_tex_ = nullptr;
  tex_width_ = 0;
  tex_height_ = 0;
  std::lock_guard lock(frame_mutex_);
  has_pending_pixel_layers_ = !pending_pixel_layers_.empty();
  has_pending_ = hasRetainedUploadableFrameLocked();
}

#ifdef PJ_TARGET_WASM
void MediaViewerWidget::showEvent(QShowEvent* event) {
  QRhiWidget::showEvent(event);
  // Workspace restore constructs and may render this widget while its docker is
  // hidden. Re-arm after the hierarchy becomes visible so the next submitted
  // frame refreshes the mixed raster/RHI top-level at the useful time.
  composition_refresh_queued_ = false;
  update();
}
#endif

void MediaViewerWidget::clearTextCache() {
  for (auto& kv : text_cache_) {
    delete kv.second.srb;
    delete kv.second.tex;
  }
  text_cache_.clear();
  text_draw_items_.clear();
}

void MediaViewerWidget::clearPixelLayerTextures() {
  for (auto& layer : pixel_layer_textures_) {
    destroyTextureLayer(layer);
  }
  pixel_layer_textures_.clear();
}

void MediaViewerWidget::destroyTextureLayer(TextureLayerResources& layer) {
  delete layer.srb;
  delete layer.uniform_buf;
  delete layer.tex_y;
  delete layer.tex_u;
  delete layer.tex_v;
  delete layer.tex_remap;
  layer = {};
}

bool MediaViewerWidget::ensureTextureLayer(TextureLayerResources& layer) {
  auto* r = rhi();
  if (r == nullptr || sampler_ == nullptr || remap_sampler_ == nullptr || remap_placeholder_tex_ == nullptr) {
    return false;
  }
  if (layer.uniform_buf == nullptr) {
    layer.uniform_buf = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBufSize);
    if (!layer.uniform_buf->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  if (layer.tex_y == nullptr) {
    layer.tex_y = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    layer.tex_u = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    layer.tex_v = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    if (!layer.tex_y->create() || !layer.tex_u->create() || !layer.tex_v->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  if (layer.srb == nullptr) {
    layer.srb = r->newShaderResourceBindings();
    setTextureLayerBindings(
        layer.srb, layer.uniform_buf, layer.tex_y, layer.tex_u, layer.tex_v, sampler_,
        layer.tex_remap != nullptr ? layer.tex_remap : remap_placeholder_tex_, remap_sampler_, sampler_);
    if (!layer.srb->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  return true;
}

MediaViewerWidget::TexturePathFormat MediaViewerWidget::texturePathFor(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::kYUV420P:
      return TexturePathFormat::kYUV420P;
    case PixelFormat::kNV12:
      return TexturePathFormat::kNV12;  // native two-plane (R8 Y + RG8 UV) hardware-decode path
    case PixelFormat::kMono8:
      return TexturePathFormat::kMono8;  // R8 texture, expanded to gray in-shader
    case PixelFormat::kBGRA8888:
      return TexturePathFormat::kBGRA;  // RGBA8 texture, swizzled in-shader
    case PixelFormat::kDepthR32F:
      return TexturePathFormat::kDepth;  // R32F texture, colormapped via LUT in-shader
    case PixelFormat::kRGB888:
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGR888:
    case PixelFormat::kMono16:
      return TexturePathFormat::kRGBA;
  }
  return TexturePathFormat::kRGBA;
}

bool MediaViewerWidget::isUploadablePixelFormat(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::kRGB888:
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGR888:
    case PixelFormat::kBGRA8888:
    case PixelFormat::kMono8:
    case PixelFormat::kYUV420P:
    case PixelFormat::kNV12:
    case PixelFormat::kDepthR32F:
      return true;
    case PixelFormat::kMono16:
      return false;
  }
  return false;
}

void MediaViewerWidget::layerSamplers(
    const TextureLayerResources& layer, QRhiSampler*& y_sampler, QRhiSampler*& uv_sampler) const {
  if (layer.format == TexturePathFormat::kDepth) {
    // Depth R32F: y NEAREST so no pixel blends real depth with the 0 no-data
    // sentinel; the colormap LUT in u_tex stays LINEAR.
    y_sampler = remap_sampler_;
    uv_sampler = sampler_;
    return;
  }
  QRhiSampler* mag =
      (layer.mag_filter == MagFilter::kNearest && mag_nearest_sampler_ != nullptr) ? mag_nearest_sampler_ : sampler_;
  y_sampler = mag;
  uv_sampler = mag;
}

void MediaViewerWidget::rebuildLayerSrb(TextureLayerResources& layer) {
  QRhiSampler* y_sampler = nullptr;
  QRhiSampler* uv_sampler = nullptr;
  layerSamplers(layer, y_sampler, uv_sampler);
  layer.srb->destroy();
  setTextureLayerBindings(
      layer.srb, layer.uniform_buf, layer.tex_y, layer.tex_u, layer.tex_v, uv_sampler,
      layer.tex_remap != nullptr ? layer.tex_remap : remap_placeholder_tex_, remap_sampler_, y_sampler);
  layer.srb->create();
}

bool MediaViewerWidget::uploadDecodedFrameToTexture(
    const DecodedFrame& frame_in, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates) {
  if (frame_in.isNull() || frame_in.width <= 0 || frame_in.height <= 0 || frame_in.pixels == nullptr ||
      updates == nullptr) {
    return false;
  }
  if (!isUploadablePixelFormat(frame_in.format)) {
    return false;
  }
  if (!ensureTextureLayer(layer)) {
    return false;
  }

  // NV12's interleaved UV plane needs an RG8 (two-channel) texture. On the rare
  // backend without RG8 (GL < 3.0), deinterleave to planar YUV420P on the CPU so
  // NV12 still displays — the native two-plane GPU path below is the fast default.
  auto* r = rhi();
  DecodedFrame nv12_fallback;
  const DecodedFrame* frame_ptr = &frame_in;
  if (frame_in.format == PixelFormat::kNV12 && (r == nullptr || !r->isTextureFormatSupported(QRhiTexture::RG8))) {
    nv12_fallback = nv12ToYuv420p(frame_in);
    if (nv12_fallback.isNull()) {
      return false;
    }
    frame_ptr = &nv12_fallback;
  }
  const DecodedFrame& frame = *frame_ptr;

  // Carry per-frame colour/display state onto the layer: the YUV→RGB matrix
  // (updateTextureLayerUniform) and the SRB sampler choice read it. A change of
  // magnification filter forces an SRB rebuild (the sampler is baked into bindings).
  layer.color_space = frame.color_space;
  layer.color_range = frame.color_range;
  // Rebuild the YUV→RGB matrix here (only on a new frame) so the per-tick uniform
  // upload in updateTextureLayerUniform() just copies the cached bytes.
  layer.color_matrix = buildYuvMatrix(frame.color_space, frame.color_range);
  const bool mag_changed = (layer.mag_filter != frame.mag_filter);
  layer.mag_filter = frame.mag_filter;

  const int w = frame.width;
  const int h = frame.height;
  const uint8_t* src = frame.pixels->data();
  const size_t src_size = frame.pixels->size();

  if (texturePathFor(frame.format) == TexturePathFormat::kYUV420P) {
    const int uv_w = (w + 1) / 2;
    const int uv_h = (h + 1) / 2;
    const int y_size = w * h;
    const int uv_size = uv_w * uv_h;
    if (src_size < static_cast<size_t>(y_size + 2 * uv_size)) {
      return false;
    }

    if (w != layer.width || h != layer.height || layer.format != TexturePathFormat::kYUV420P || mag_changed) {
      layer.tex_y->destroy();
      layer.tex_y->setFormat(QRhiTexture::R8);
      layer.tex_y->setPixelSize(QSize(w, h));
      layer.tex_y->create();

      layer.tex_u->destroy();
      layer.tex_u->setFormat(QRhiTexture::R8);
      layer.tex_u->setPixelSize(QSize(uv_w, uv_h));
      layer.tex_u->create();

      layer.tex_v->destroy();
      layer.tex_v->setFormat(QRhiTexture::R8);
      layer.tex_v->setPixelSize(QSize(uv_w, uv_h));
      layer.tex_v->create();

      layer.width = w;
      layer.height = h;
      layer.format = TexturePathFormat::kYUV420P;
      rebuildLayerSrb(layer);
    }

    QRhiTextureSubresourceUploadDescription y_desc(src, y_size);
    y_desc.setSourceSize(QSize(w, h));
    updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, y_desc}));

    QRhiTextureSubresourceUploadDescription u_desc(src + y_size, uv_size);
    u_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, u_desc}));

    QRhiTextureSubresourceUploadDescription v_desc(src + y_size + uv_size, uv_size);
    v_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_v, QRhiTextureUploadDescription({0, 0, v_desc}));
    return true;
  }

  if (texturePathFor(frame.format) == TexturePathFormat::kNV12) {
    const int uv_w = (w + 1) / 2;
    const int uv_h = (h + 1) / 2;
    const int y_size = w * h;
    const int uv_row_bytes = 2 * uv_w;  // RG8: uv_w UV-pairs per row
    if (src_size < expectedBufferSize(w, h, PixelFormat::kNV12)) {
      return false;
    }

    if (w != layer.width || h != layer.height || layer.format != TexturePathFormat::kNV12 || mag_changed) {
      layer.tex_y->destroy();
      layer.tex_y->setFormat(QRhiTexture::R8);
      layer.tex_y->setPixelSize(QSize(w, h));
      layer.tex_y->create();

      // u_tex holds the interleaved UV plane as RG8; v_tex is unused by the NV12
      // shader branch but stays bound (and valid) to keep one SRB layout.
      layer.tex_u->destroy();
      layer.tex_u->setFormat(QRhiTexture::RG8);
      layer.tex_u->setPixelSize(QSize(uv_w, uv_h));
      layer.tex_u->create();

      layer.width = w;
      layer.height = h;
      layer.format = TexturePathFormat::kNV12;
      rebuildLayerSrb(layer);
    }

    QRhiTextureSubresourceUploadDescription y_desc(src, y_size);
    y_desc.setSourceSize(QSize(w, h));
    updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, y_desc}));

    QRhiTextureSubresourceUploadDescription uv_desc(src + y_size, uv_h * uv_row_bytes);
    uv_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, uv_desc}));
    return true;
  }

  // Single-plane upload. Mono8 and BGRA go to the GPU verbatim (R8 / RGBA8) and
  // are expanded/swizzled in the shader — no CPU repack. RGB888/BGR888 (3-byte)
  // have no clean 4-byte-row GPU layout, so they still expand to RGBA on the CPU.
  const TexturePathFormat path = texturePathFor(frame.format);
  QRhiTexture::Format tex_format = QRhiTexture::RGBA8;
  const uint8_t* upload_data = nullptr;
  size_t upload_size = 0;

  if (path == TexturePathFormat::kDepth) {
    tex_format = QRhiTexture::R32F;
    upload_data = src;  // float32 metric depth; the shader colormaps via the LUT in u_tex
    upload_size = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(float);
    layer.invert = frame.depth.invert ? 1 : 0;
    layer.near_m = frame.depth.near_m;
    layer.far_m = frame.depth.far_m;
    layer.colormap = static_cast<int32_t>(frame.depth.colormap);
  } else if (path == TexturePathFormat::kMono8) {
    tex_format = QRhiTexture::R8;
    upload_data = src;
    upload_size = static_cast<size_t>(w) * static_cast<size_t>(h);
  } else if (path == TexturePathFormat::kBGRA) {
    upload_data = src;  // shader swizzles .bgra
    upload_size = src_size;
  } else if (frame.format == PixelFormat::kRGBA8888) {
    upload_data = src;
    upload_size = src_size;
  } else if (frame.format == PixelFormat::kRGB888 || frame.format == PixelFormat::kBGR888) {
    const bool is_bgr = (frame.format == PixelFormat::kBGR888);
    rgba_repack_buffer_.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);
    const int pixel_count = w * h;
    for (int i = 0; i < pixel_count; ++i) {
      rgba_repack_buffer_[i * 4 + 0] = src[i * 3 + (is_bgr ? 2 : 0)];
      rgba_repack_buffer_[i * 4 + 1] = src[i * 3 + 1];
      rgba_repack_buffer_[i * 4 + 2] = src[i * 3 + (is_bgr ? 0 : 2)];
      rgba_repack_buffer_[i * 4 + 3] = 255;
    }
    upload_data = rgba_repack_buffer_.data();
    upload_size = rgba_repack_buffer_.size();
  }

  if (upload_data == nullptr) {
    return false;
  }

  if (w != layer.width || h != layer.height || layer.format != path || mag_changed) {
    layer.tex_y->destroy();
    layer.tex_y->setFormat(tex_format);
    layer.tex_y->setPixelSize(QSize(w, h));
    layer.tex_y->create();

    if (path == TexturePathFormat::kDepth) {
      // u_tex carries the colormap LUT: kColormapLutWidth (t) x kColormapCount (rows) RGBA8.
      static const std::vector<uint8_t> kColormapLut = buildColormapLut();
      const QSize lut_size(kColormapLutWidth, kColormapCount);
      layer.tex_u->destroy();
      layer.tex_u->setFormat(QRhiTexture::RGBA8);
      layer.tex_u->setPixelSize(lut_size);
      layer.tex_u->create();
      QRhiTextureSubresourceUploadDescription lut_desc(kColormapLut.data(), static_cast<quint32>(kColormapLut.size()));
      lut_desc.setSourceSize(lut_size);
      updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, lut_desc}));
    }

    layer.width = w;
    layer.height = h;
    layer.format = path;
    // rebuildLayerSrb() reads layer.format (set above): depth keeps its NEAREST
    // y-plane + LINEAR LUT; other formats honour the layer's magnification filter.
    rebuildLayerSrb(layer);
  }

  QRhiTextureSubresourceUploadDescription sub_desc(upload_data, static_cast<quint32>(upload_size));
  sub_desc.setSourceSize(QSize(w, h));
  updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, sub_desc}));
  return true;
}

void MediaViewerWidget::updateTextureLayerUniform(
    TextureLayerResources& layer, const QMatrix4x4& view, QRhiResourceUpdateBatch* updates) const {
  if (layer.uniform_buf == nullptr || updates == nullptr) {
    return;
  }
  updates->updateDynamicBuffer(layer.uniform_buf, 0, 64, view.constData());
  // Color matrix: cached per-layer (rebuilt in uploadDecodedFrameToTexture only when
  // a new frame's colorimetry changes) so SD / limited-range video converts correctly
  // — BT.709 + full reproduces the old hardcoded matrix exactly. Ignored by the
  // shader for RGB/mono/depth paths.
  updates->updateDynamicBuffer(layer.uniform_buf, 64, 64, layer.color_matrix.data());
  const int32_t fmt = static_cast<int32_t>(layer.format);
  updates->updateDynamicBuffer(layer.uniform_buf, 128, 4, &fmt);
  updates->updateDynamicBuffer(layer.uniform_buf, 132, 4, &layer.opacity);
  updates->updateDynamicBuffer(layer.uniform_buf, 136, 4, &layer.rectify);
  // Depth-colormap uniforms (kDepth path); ignored by the shader for other formats.
  updates->updateDynamicBuffer(layer.uniform_buf, 140, 4, &layer.invert);
  updates->updateDynamicBuffer(layer.uniform_buf, 144, 4, &layer.near_m);
  updates->updateDynamicBuffer(layer.uniform_buf, 148, 4, &layer.far_m);
  updates->updateDynamicBuffer(layer.uniform_buf, 152, 4, &layer.colormap);
}

bool MediaViewerWidget::ensureRemapTexture(
    TextureLayerResources& layer, const UndistortMap& map, QRhiResourceUpdateBatch* updates) {
  auto* r = rhi();
  if (r == nullptr || updates == nullptr || !map.valid()) {
    return false;
  }
  const int out_w = map.out_width;
  const int out_h = map.out_height;

  // (Re)create the LUT texture when the rectified output size changes.
  if (layer.tex_remap == nullptr || layer.remap_w != out_w || layer.remap_h != out_h) {
    delete layer.tex_remap;
    layer.tex_remap = r->newTexture(QRhiTexture::RGBA32F, QSize(out_w, out_h));
    if (!layer.tex_remap->create()) {
      delete layer.tex_remap;
      layer.tex_remap = nullptr;
      layer.remap_w = 0;
      layer.remap_h = 0;
      layer.remap_map_key = nullptr;
      return false;
    }
    layer.remap_w = out_w;
    layer.remap_h = out_h;
    layer.remap_map_key = nullptr;  // force a re-upload into the new texture.
    // Rebind slot 4 to the real LUT (was the placeholder). Preserve the layer's
    // sampler choice (magnification filter / depth nearest) via layerSamplers().
    QRhiSampler* y_s = nullptr;
    QRhiSampler* uv_s = nullptr;
    layerSamplers(layer, y_s, uv_s);
    layer.srb->destroy();
    setTextureLayerBindings(
        layer.srb, layer.uniform_buf, layer.tex_y, layer.tex_u, layer.tex_v, uv_s, layer.tex_remap, remap_sampler_,
        y_s);
    if (!layer.srb->create()) {
      return false;
    }
  }

  // Upload the LUT only when the underlying map changes (calibration is constant
  // per camera, so this runs once). RGBA32F: .rg = normalized source UV, .ba = 0.
  if (layer.remap_map_key != static_cast<const void*>(&map)) {
    const std::vector<float> rg = undistortMapToNormalizedRG(map);
    if (rg.size() != static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 2) {
      return false;
    }
    std::vector<float> rgba(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4, 0.0F);
    for (size_t i = 0, n = static_cast<size_t>(out_w) * static_cast<size_t>(out_h); i < n; ++i) {
      rgba[i * 4 + 0] = rg[i * 2 + 0];
      rgba[i * 4 + 1] = rg[i * 2 + 1];
    }
    QRhiTextureSubresourceUploadDescription desc(rgba.data(), static_cast<quint32>(rgba.size() * sizeof(float)));
    desc.setSourceSize(QSize(out_w, out_h));
    updates->uploadTexture(layer.tex_remap, QRhiTextureUploadDescription({0, 0, desc}));
    layer.remap_map_key = static_cast<const void*>(&map);
  }

  layer.rectify = 1;
  return true;
}

void MediaViewerWidget::applyGpuRectifyCapability() {
  if (media_source_ != nullptr) {
    media_source_->setGpuRectificationAvailable(gpu_rectify_supported_.load(std::memory_order_relaxed));
  }
}

QSize MediaViewerWidget::uploadLayerFrame(
    const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates) {
  if (!uploadDecodedFrameToTexture(frame, layer, updates)) {
    return {};
  }
  // GPU path: the uploaded frame is RAW; the displayed (logical) size — which
  // annotation/aspect/inspector coords use — is the map's output size.
  const bool rectify_on_gpu = gpu_rectify_supported_.load(std::memory_order_relaxed) && frame.rectify_map != nullptr &&
                              ensureRemapTexture(layer, *frame.rectify_map, updates);
  if (rectify_on_gpu) {
    return {frame.rectify_map->out_width, frame.rectify_map->out_height};
  }
  layer.rectify = 0;
  return {layer.width, layer.height};
}

void MediaViewerWidget::destroyOverlayPipeline(OverlayPipeline& overlay) {
  delete overlay.pipeline;
  delete overlay.srb;
  delete overlay.vbo;
  overlay.pipeline = nullptr;
  overlay.srb = nullptr;
  overlay.vbo = nullptr;
  overlay.vbo_capacity = 0;
}

bool MediaViewerWidget::createOverlayVbo(OverlayPipeline& overlay, size_t initial_capacity) {
  auto* r = rhi();
  if (r == nullptr) {
    return false;
  }
  overlay.vbo_capacity = initial_capacity;
  overlay.vbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, static_cast<int>(overlay.vbo_capacity));
  if (!overlay.vbo->create()) {
    destroyOverlayPipeline(overlay);
    return false;
  }
  return true;
}

bool MediaViewerWidget::createUniformOverlaySrb(OverlayPipeline& overlay) {
  auto* r = rhi();
  if (r == nullptr || marker_uniform_buf_ == nullptr) {
    return false;
  }
  overlay.srb = r->newShaderResourceBindings();
  overlay.srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
  });
  if (!overlay.srb->create()) {
    destroyOverlayPipeline(overlay);
    return false;
  }
  return true;
}

bool MediaViewerWidget::createOverlayGraphicsPipeline(
    OverlayPipeline& overlay, const QShader& vert, const QShader& frag, QRhiGraphicsPipeline::Topology topology,
    const QRhiVertexInputLayout& input_layout, const char* failure_message) {
  auto* r = rhi();
  if (r == nullptr || overlay.srb == nullptr) {
    return false;
  }
  overlay.pipeline = r->newGraphicsPipeline();
  overlay.pipeline->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
  overlay.pipeline->setTopology(topology);
  overlay.pipeline->setVertexInputLayout(input_layout);
  overlay.pipeline->setShaderResourceBindings(overlay.srb);
  overlay.pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  overlay.pipeline->setTargetBlends({alphaBlend()});
  if (!overlay.pipeline->create()) {
    qWarning("%s", failure_message);
    destroyOverlayPipeline(overlay);
    return false;
  }
  return true;
}

void MediaViewerWidget::uploadOverlayVertexData(OverlayPipeline& overlay, QRhiResourceUpdateBatch* updates) {
  if (overlay.pipeline == nullptr || overlay.vbo == nullptr || updates == nullptr) {
    return;
  }
  const size_t needed = overlay.vertex_data.size() * sizeof(float);
  if (needed > overlay.vbo_capacity) {
    overlay.vbo->destroy();
    overlay.vbo_capacity = std::max(needed * 2, overlay.vbo_capacity);
    overlay.vbo->setSize(static_cast<int>(overlay.vbo_capacity));
    overlay.vbo->create();
  }
  if (needed > 0) {
    updates->updateDynamicBuffer(overlay.vbo, 0, static_cast<int>(needed), overlay.vertex_data.data());
  }
}

void MediaViewerWidget::initialize(QRhiCommandBuffer* /*cb*/) {
  auto* r = rhi();
  if (r == nullptr) {
    return;
  }

  if (rhi_cached_ != r) {
    releaseResources();
    rhi_cached_ = r;
    logGraphicsBackend(*this, r);
  }

  if (pipeline_ != nullptr) {
    return;
  }

  // Use YUV→RGB shader (handles both YUV and RGBA passthrough)
  auto vert = loadShader(":/shaders/yuv_to_rgb.vert.qsb");
  auto frag = loadShader(":/shaders/yuv_to_rgb.frag.qsb");
  if (!vert.isValid() || !frag.isValid()) {
    qWarning("MediaViewerWidget: failed to load yuv_to_rgb shaders");
    return;
  }

  sampler_ = r->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
  sampler_->create();

  // NEAREST sampler for layers requesting MagFilter::kNearest (crisp magnification).
  mag_nearest_sampler_ = r->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  mag_nearest_sampler_->create();

  // NEAREST sampler for the rectification LUT (exact per-output-pixel source coord).
  remap_sampler_ = r->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  remap_sampler_->create();

  // 1x1 RGBA32F placeholder bound at slot 4 for non-rectifying layers so all
  // texture-layer SRBs share one binding layout.
  remap_placeholder_tex_ = r->newTexture(QRhiTexture::RGBA32F, QSize(1, 1));
  remap_placeholder_tex_->create();

  // Uniform buffer + placeholder textures (1x1) — resized on first frame.
  if (!ensureTextureLayer(base_texture_)) {
    qWarning("MediaViewerWidget: failed to create base texture resources");
    return;
  }

  pipeline_ = r->newGraphicsPipeline();
  pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  pipeline_->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});

  QRhiVertexInputLayout input_layout;
  pipeline_->setVertexInputLayout(input_layout);
  pipeline_->setShaderResourceBindings(base_texture_.srb);
  pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!pipeline_->create()) {
    qWarning("MediaViewerWidget: failed to create graphics pipeline");
    pipeline_ = nullptr;
    return;
  }

  composite_pipeline_ = r->newGraphicsPipeline();
  composite_pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  composite_pipeline_->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
  composite_pipeline_->setVertexInputLayout(input_layout);
  composite_pipeline_->setShaderResourceBindings(base_texture_.srb);
  composite_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  composite_pipeline_->setTargetBlends({alphaBlend()});
  if (!composite_pipeline_->create()) {
    qWarning("MediaViewerWidget: failed to create composite graphics pipeline");
    delete composite_pipeline_;
    composite_pipeline_ = nullptr;
  }

  // ----- Marker / overlay shaders + shared uniform -----
  // scene_lines shaders drive the thick (Triangles) outline pipeline below; the
  // uniform buffer (view + frame size) is shared by every overlay pipeline.
  auto marker_vert = loadShader(":/shaders/scene_lines.vert.qsb");
  auto marker_frag = loadShader(":/shaders/scene_lines.frag.qsb");
  if (marker_vert.isValid() && marker_frag.isValid()) {
    marker_uniform_buf_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMarkerUniformBufSize);
    marker_uniform_buf_->create();
  } else {
    qWarning("MediaViewerWidget: scene_lines shaders not loaded; markers disabled");
  }

  // ----- kPoints quad pipeline -----
  // Solid-fill triangles, shares marker_uniform_buf_ but has its own SRB and VBO.
  auto quads_vert = loadShader(":/shaders/scene_quads.vert.qsb");
  auto quads_frag = loadShader(":/shaders/scene_quads.frag.qsb");
  if (quads_vert.isValid() && quads_frag.isValid() && marker_uniform_buf_ != nullptr) {
    if (createOverlayVbo(points_overlay_, 64 * 1024) && createUniformOverlaySrb(points_overlay_)) {
      createOverlayGraphicsPipeline(
          points_overlay_, quads_vert, quads_frag, QRhiGraphicsPipeline::Triangles, colorVertexInputLayout(),
          "MediaViewerWidget: failed to create points pipeline");
    }
  } else if (!quads_vert.isValid() || !quads_frag.isValid()) {
    qWarning("MediaViewerWidget: scene_quads shaders not loaded; kPoints disabled");
  }

  // ----- Outline pipeline (Triangles, reuses scene_lines shaders) -----
  // All line/circle outlines render here. Each segment is expanded CPU-side
  // (overlay_geometry::appendLineStrokes) into a cosmetic-width quad — same vertex
  // layout as the fills (vec2 pos + vec4 color, stride 24), Triangles topology.
  if (marker_vert.isValid() && marker_frag.isValid() && marker_uniform_buf_ != nullptr) {
    if (createOverlayVbo(thick_overlay_, 64 * 1024) && createUniformOverlaySrb(thick_overlay_)) {
      createOverlayGraphicsPipeline(
          thick_overlay_, marker_vert, marker_frag, QRhiGraphicsPipeline::Triangles, colorVertexInputLayout(),
          "MediaViewerWidget: failed to create thick pipeline");
    }
  }

  // ----- Text pipeline (textured quads, R8 alpha mask + tint color) -----
  auto text_vert = loadShader(":/shaders/scene_text.vert.qsb");
  auto text_frag = loadShader(":/shaders/scene_text.frag.qsb");
  if (text_vert.isValid() && text_frag.isValid() && marker_uniform_buf_ != nullptr) {
    if (createOverlayVbo(text_overlay_, 32 * 1024)) {
      text_sampler_ = r->newSampler(
          QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
          QRhiSampler::ClampToEdge);
      text_sampler_->create();

      // Placeholder 1x1 alpha texture used only as the pipeline's layout-compat SRB.
      // Real per-draw SRBs are stored in TextEntry inside text_cache_.
      text_placeholder_tex_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));
      text_placeholder_tex_->create();

      text_overlay_.srb = r->newShaderResourceBindings();
      text_overlay_.srb->setBindings({
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
              marker_uniform_buf_),
          QRhiShaderResourceBinding::sampledTexture(
              1, QRhiShaderResourceBinding::FragmentStage, text_placeholder_tex_, text_sampler_),
      });
      if (!text_overlay_.srb->create()) {
        destroyOverlayPipeline(text_overlay_);
      } else {
        createOverlayGraphicsPipeline(
            text_overlay_, text_vert, text_frag, QRhiGraphicsPipeline::Triangles, textVertexInputLayout(),
            "MediaViewerWidget: failed to create text pipeline");
      }
    }
  } else if (!text_vert.isValid() || !text_frag.isValid()) {
    qWarning("MediaViewerWidget: scene_text shaders not loaded; text disabled");
  }

  // GPU rectification needs the base pipeline plus a sampleable RGBA32F LUT. When
  // available, tell the source to defer rectification to the GPU (raw upload +
  // map); otherwise it keeps rectifying on the CPU (the precompute fallback).
  gpu_rectify_supported_.store(
      pipeline_ != nullptr && r->isTextureFormatSupported(QRhiTexture::RGBA32F), std::memory_order_relaxed);
  applyGpuRectifyCapability();

  if (has_pending_ || has_pending_pixel_layers_) {
    update();
  }
}

void MediaViewerWidget::render(QRhiCommandBuffer* cb) {
  if (pipeline_ == nullptr) {
    return;
  }
  auto* r = rhi();
  if (r == nullptr) {
    return;
  }

  auto* rt = renderTarget();
  const QSize output_size = rt->pixelSize();
  QRhiResourceUpdateBatch* updates = r->nextResourceUpdateBatch();
  bool inspector_frame_changed = false;

  {
    std::lock_guard lock(frame_mutex_);

    // Poll MediaSource if attached. MediaFrame may carry both a pixel base
    // and vector overlays; capture each independently — a frame can update
    // either layer in isolation.
    if (media_source_ != nullptr) {
      auto frame = media_source_->takeFrame();
      if (frame.has_value()) {
        if (!frame->pixel_layers.empty()) {
          pending_pixel_layers_ = std::move(frame->pixel_layers);
          has_pending_pixel_layers_ = true;
          pixel_layers_active_ = true;
          has_pending_ = false;
          pending_decoded_ = {};
          for (const auto& layer : pending_pixel_layers_) {
            if (!layer.frame.isNull()) {
              inspector_frame_ = layer.frame;
              inspector_frame_changed = true;
              break;
            }
          }
        } else if (frame->base.has_value() && !frame->base->isNull()) {
          pending_decoded_ = std::move(*frame->base);
          resetPendingPixelLayers();
          inspector_frame_ = pending_decoded_;
          inspector_frame_changed = true;
          has_pending_ = isUploadablePixelFormat(pending_decoded_.format);
        }
        if (!frame->overlays.empty()) {
          last_overlays_ = std::move(frame->overlays);
          overlays_dirty_ = true;
        }
      }
    }

    if (has_pending_pixel_layers_) {
      if (pending_pixel_layers_.size() < pixel_layer_textures_.size()) {
        for (size_t i = pending_pixel_layers_.size(); i < pixel_layer_textures_.size(); ++i) {
          destroyTextureLayer(pixel_layer_textures_[i]);
        }
      }
      pixel_layer_textures_.resize(pending_pixel_layers_.size());

      bool set_frame_size = false;
      for (size_t i = 0; i < pending_pixel_layers_.size(); ++i) {
        auto& texture = pixel_layer_textures_[i];
        texture.opacity = std::clamp(pending_pixel_layers_[i].opacity, 0.0f, 1.0f);
        // Rectify each layer that carries a map (the camera image flows through
        // here, not `base`, once it's inside a composite). The first uploadable
        // layer's logical size drives the overlay/aspect/inspector coordinate space.
        const QSize logical = uploadLayerFrame(pending_pixel_layers_[i].frame, texture, updates);
        if (!logical.isEmpty() && !set_frame_size) {
          tex_width_ = logical.width();
          tex_height_ = logical.height();
          frame_aspect_ = static_cast<float>(logical.width()) / static_cast<float>(logical.height());
          set_frame_size = true;
        }
      }
      has_pending_pixel_layers_ = false;
    }

    if (has_pending_) {
      const QSize logical = uploadLayerFrame(pending_decoded_, base_texture_, updates);
      if (!logical.isEmpty()) {
        tex_width_ = logical.width();
        tex_height_ = logical.height();
        frame_aspect_ = static_cast<float>(logical.width()) / static_cast<float>(logical.height());
      }
      has_pending_ = false;
    }
  }

  if (inspector_frame_changed) {
    schedulePointInspectorRefresh();
  }

  // Update uniforms
  QMatrix4x4 view = buildViewTransform(output_size);
  updateTextureLayerUniform(base_texture_, view, updates);
  if (pixel_layers_active_) {
    for (auto& layer : pixel_layer_textures_) {
      updateTextureLayerUniform(layer, view, updates);
    }
  }

  // ----- Overlay geometry: rebuild VBOs + update uniforms -----
  // Stroke widths are in image pixels, so they SCALE with zoom — but are floored
  // so a stroke is never thinner than 1px on screen. Without that floor a stroke
  // shown below ~1:1 goes sub-pixel and (no MSAA) drops edges depending on
  // sub-pixel alignment — the "lines disappear when zoomed out" bug. The floor is
  // expressed in screen pixels, so it depends on the effective view scale; stroke
  // geometry is therefore re-expanded whenever that scale changes, not only when
  // the annotation set changes. effective_scale = on-screen px per image px =
  // zoom × aspect-preserving fit (so a single isotropic scalar is exact).
  double effective_scale = 0.0;
  if (tex_width_ > 0 && tex_height_ > 0 && output_size.width() > 0 && output_size.height() > 0) {
    const double fit = std::min(
        static_cast<double>(output_size.width()) / static_cast<double>(tex_width_),
        static_cast<double>(output_size.height()) / static_cast<double>(tex_height_));
    effective_scale = fit * static_cast<double>(zoom_);
  }

  if (marker_uniform_buf_ != nullptr && effective_scale > 0.0) {
    const double image_px_per_screen_px = 1.0 / effective_scale;
    const bool scale_changed =
        std::abs(effective_scale - last_overlay_scale_) > 1e-6 * std::max(1.0, std::abs(last_overlay_scale_));
    if (overlays_dirty_ || scale_changed) {
      thick_overlay_.vertex_data.clear();
      points_overlay_.vertex_data.clear();
      size_t total_points = 0;
      for (const auto& sf : last_overlays_) {
        for (const auto& ia : sf.annotations) {
          for (const auto& pa : ia.points) {
            total_points += pa.points.size();
          }
        }
      }
      thick_overlay_.vertex_data.reserve(total_points * 36);
      points_overlay_.vertex_data.reserve(total_points * 36);
      for (const auto& sf : last_overlays_) {
        for (const auto& ia : sf.annotations) {
          for (const auto& pa : ia.points) {
            overlay_geometry::appendLineStrokes(pa, image_px_per_screen_px, thick_overlay_.vertex_data);
            overlay_geometry::appendLoopFill(pa, points_overlay_.vertex_data);
            overlay_geometry::appendPointQuads(pa, image_px_per_screen_px, points_overlay_.vertex_data);
          }
          for (const auto& ca : ia.circles) {
            overlay_geometry::appendCircleStroke(ca, image_px_per_screen_px, thick_overlay_.vertex_data);
            overlay_geometry::appendCircleFill(ca, points_overlay_.vertex_data);
          }
        }
      }
      uploadOverlayVertexData(thick_overlay_, updates);
      uploadOverlayVertexData(points_overlay_, updates);
      last_overlay_scale_ = effective_scale;

      // ----- Text rebuild (textured quads) — only when the annotation set
      // changes; text size is not cosmetic, so a pure zoom change leaves it. -----
      if (overlays_dirty_ && text_overlay_.pipeline != nullptr) {
        text_overlay_.vertex_data.clear();
        text_draw_items_.clear();
        constexpr size_t kFloatsPerTextQuad = 6 * 8;  // 6 verts × (pos2+uv2+color4) = 48 floats
        size_t total_texts = 0;
        for (const auto& sf : last_overlays_) {
          for (const auto& ia : sf.annotations) {
            total_texts += ia.texts.size();
          }
        }
        text_overlay_.vertex_data.reserve(total_texts * kFloatsPerTextQuad);
        for (const auto& sf : last_overlays_) {
          for (const auto& ia : sf.annotations) {
            for (const auto& ta : ia.texts) {
              auto* entry = getOrCreateTextTexture(ta.text, ta.font_size, updates);
              if (entry == nullptr || entry->tex == nullptr) {
                continue;
              }
              const float x0 = static_cast<float>(ta.position.x);
              const float y0 = static_cast<float>(ta.position.y);
              const float x1 = x0 + static_cast<float>(entry->width);
              const float y1 = y0 + static_cast<float>(entry->height);
              const float tr = static_cast<float>(ta.color.r) / 255.0f;
              const float tg = static_cast<float>(ta.color.g) / 255.0f;
              const float tb = static_cast<float>(ta.color.b) / 255.0f;
              const float tap = static_cast<float>(ta.color.a) / 255.0f;
              const size_t offset_bytes = text_overlay_.vertex_data.size() * sizeof(float);
              const float quad[6][8] = {
                  {x0, y0, 0.0f, 0.0f, tr, tg, tb, tap}, {x1, y0, 1.0f, 0.0f, tr, tg, tb, tap},
                  {x1, y1, 1.0f, 1.0f, tr, tg, tb, tap}, {x0, y0, 0.0f, 0.0f, tr, tg, tb, tap},
                  {x1, y1, 1.0f, 1.0f, tr, tg, tb, tap}, {x0, y1, 0.0f, 1.0f, tr, tg, tb, tap},
              };
              for (const auto& v : quad) {
                for (int k = 0; k < 8; ++k) {
                  text_overlay_.vertex_data.push_back(v[k]);
                }
              }
              text_draw_items_.push_back(TextDrawItem{entry->srb, offset_bytes});
            }
          }
        }
        uploadOverlayVertexData(text_overlay_, updates);
      }

      overlays_dirty_ = false;
    }

    {
      MarkerUbo ubo{};
      std::memcpy(ubo.view, view.constData(), sizeof(ubo.view));
      // Annotation overlays are authored in the displayed image's pixel space.
      // The decode pipeline rectifies a calibrated camera to its native (full)
      // resolution, so the texture dimensions are the correct normalization base.
      ubo.frame_size[0] = static_cast<float>(tex_width_);
      ubo.frame_size[1] = static_cast<float>(tex_height_);
      updates->updateDynamicBuffer(marker_uniform_buf_, 0, kMarkerUniformBufSize, &ubo);
    }
  }
  const size_t points_vertex_count = (points_overlay_.pipeline != nullptr) ? points_overlay_.vertex_data.size() / 6 : 0;
  const size_t thick_vertex_count = (thick_overlay_.pipeline != nullptr) ? thick_overlay_.vertex_data.size() / 6 : 0;

  cb->beginPass(rt, clear_color_, {1.0f, 0}, updates);
  if (pixel_layers_active_ && composite_pipeline_ != nullptr && !pixel_layer_textures_.empty()) {
    cb->setGraphicsPipeline(composite_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setScissor(imageScissor(view, output_size));
    for (auto& layer : pixel_layer_textures_) {
      if (layer.srb == nullptr || layer.width <= 0 || layer.height <= 0) {
        continue;
      }
      cb->setShaderResources(layer.srb);
      cb->draw(3);
    }
  } else {
    cb->setGraphicsPipeline(pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setScissor(imageScissor(view, output_size));
    cb->setShaderResources(base_texture_.srb);
    cb->draw(3);
  }

  // Second draw call: vector overlays (markers) on top of the image, blended.
  // Draw order: image (already drawn) → fills (Triangles) → outlines (Triangles).
  // Rationale: fills must be UNDER strokes so that LineLoop fill_color does not
  // hide its own outline, and circle outlines render on top of circle fills.
  // Viewport is reset on pipeline switch in some QRhi backends, so set explicitly.

  // Fills: kPoints quads + LineLoop fills + circle fills.
  if (points_overlay_.pipeline != nullptr && points_vertex_count > 0 && tex_width_ > 0) {
    cb->setGraphicsPipeline(points_overlay_.pipeline);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setShaderResources(points_overlay_.srb);
    const QRhiCommandBuffer::VertexInput pinput(points_overlay_.vbo, 0);
    cb->setVertexInput(0, 1, &pinput);
    cb->draw(static_cast<quint32>(points_vertex_count));
  }

  // Outlines: all line/circle strokes, expanded to cosmetic-width triangles.
  // (Native GL_LINES was retired — it gave only 1 px and could be guard-band
  // culled when zoomed far in; triangles clip robustly and honour thickness.)
  if (thick_overlay_.pipeline != nullptr && thick_vertex_count > 0 && tex_width_ > 0) {
    cb->setGraphicsPipeline(thick_overlay_.pipeline);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setShaderResources(thick_overlay_.srb);
    const QRhiCommandBuffer::VertexInput tinput(thick_overlay_.vbo, 0);
    cb->setVertexInput(0, 1, &tinput);
    cb->draw(static_cast<quint32>(thick_vertex_count));
  }

  // Text labels — drawn last so they sit on top of all other overlays. Each
  // text uses its own pre-created SRB (one per cached texture) so the bindings
  // are stable between submission and execution. One draw call per text.
  if (text_overlay_.pipeline != nullptr && !text_draw_items_.empty() && tex_width_ > 0) {
    cb->setGraphicsPipeline(text_overlay_.pipeline);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    for (const auto& item : text_draw_items_) {
      cb->setShaderResources(item.srb);
      const QRhiCommandBuffer::VertexInput txi(text_overlay_.vbo, static_cast<quint32>(item.vbo_offset_bytes));
      cb->setVertexInput(0, 1, &txi);
      cb->draw(6);  // 1 quad = 2 triangles = 6 vertices
    }
  }

  cb->endPass();
}

void MediaViewerWidget::wheelEvent(QWheelEvent* e) {
  const MediaViewState previous = viewState();
  MediaViewState next = previous;
  const float delta = e->angleDelta().y() > 0 ? 1.1f : 1.0f / 1.1f;
  next.zoom = std::clamp(next.zoom * delta, 1.0f, 20.0f);

  if (next.zoom <= 1.0f) {
    next.pan_x = 0.0f;
    next.pan_y = 0.0f;
  } else {
    const float mx = (2.0f * static_cast<float>(e->position().x()) / static_cast<float>(width()) - 1.0f);
    const float my = (2.0f * static_cast<float>(e->position().y()) / static_cast<float>(height()) - 1.0f);
    next.pan_x += mx * (1.0f / next.zoom - 1.0f / previous.zoom);
    next.pan_y += my * (1.0f / next.zoom - 1.0f / previous.zoom);
  }

  if (next != previous && setViewState(next)) {
    emit viewInteractionCommitted();
  }
  e->accept();
}

void MediaViewerWidget::mousePressEvent(QMouseEvent* e) {
  pan_interaction_changed_ = false;
  if (e->button() == Qt::LeftButton) {
    hidePointInspector();
  }
  if (e->button() == Qt::LeftButton && zoom_ > 1.0f) {
    last_mouse_pos_ = e->position();
    e->accept();
  }
}

void MediaViewerWidget::mouseMoveEvent(QMouseEvent* e) {
  if ((e->buttons() & Qt::LeftButton) != 0 && zoom_ > 1.0f) {
    auto dx = static_cast<float>(e->position().x() - last_mouse_pos_.x()) / static_cast<float>(width()) * 2.0f / zoom_;
    auto dy = static_cast<float>(e->position().y() - last_mouse_pos_.y()) / static_cast<float>(height()) * 2.0f / zoom_;
    pan_x_ += dx;
    pan_y_ -= dy;
    pan_interaction_changed_ = pan_interaction_changed_ || dx != 0.0f || dy != 0.0f;
    last_mouse_pos_ = e->position();
    update();
    e->accept();
    return;
  }

  last_point_inspector_pos_ = e->position();
  point_inspector_active_.store(true, std::memory_order_relaxed);
  if (point_inspector_enabled_.load(std::memory_order_relaxed)) {
    refreshPointInspector();
  }
}

void MediaViewerWidget::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && pan_interaction_changed_) {
    pan_interaction_changed_ = false;
    emit viewInteractionCommitted();
    e->accept();
    return;
  }
  pan_interaction_changed_ = false;
  QRhiWidget::mouseReleaseEvent(e);
}

void MediaViewerWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  const MediaViewState previous = viewState();
  resetView();
  if (viewState() != previous) {
    emit viewInteractionCommitted();
  }
  e->accept();
}

void MediaViewerWidget::leaveEvent(QEvent* e) {
  point_inspector_active_.store(false, std::memory_order_relaxed);
  hidePointInspector();
  QRhiWidget::leaveEvent(e);
}

QMatrix4x4 MediaViewerWidget::buildViewTransform(QSize output_size) const {
  QMatrix4x4 m;
  float widget_aspect = static_cast<float>(output_size.width()) / static_cast<float>(output_size.height());
  float sx = 1.0f;
  float sy = 1.0f;
  if (frame_aspect_ > 0.0f) {
    if (widget_aspect > frame_aspect_) {
      sx = frame_aspect_ / widget_aspect;
    } else {
      sy = widget_aspect / frame_aspect_;
    }
  }
  m.scale(sx * zoom_, sy * zoom_);
  m.translate(pan_x_, pan_y_);
  return m;
}

void MediaViewerWidget::refreshPointInspector() {
  if (!point_inspector_enabled_.load(std::memory_order_relaxed) ||
      !point_inspector_active_.load(std::memory_order_relaxed)) {
    hidePointInspector();
    return;
  }

  DecodedFrame frame;
  {
    std::lock_guard lock(frame_mutex_);
    frame = inspector_frame_;
  }
  if (frame.isNull() || frame.width <= 0 || frame.height <= 0) {
    hidePointInspector();
    return;
  }

  // On the GPU-rectify path the frame is RAW: the displayed (logical) coordinate
  // space is the map's output size, so the cursor maps there; the sampled value
  // comes from the source pixel the map points at (the same one the GPU shows).
  const bool gpu_rectified = frame.rectify_map != nullptr && frame.rectify_map->valid();
  const int logical_w = gpu_rectified ? frame.rectify_map->out_width : frame.width;
  const int logical_h = gpu_rectified ? frame.rectify_map->out_height : frame.height;

  const auto image_point =
      widgetPointToImagePixel(last_point_inspector_pos_, size(), QSize(logical_w, logical_h), zoom_, pan_x_, pan_y_);
  if (!image_point.has_value()) {
    hidePointInspector();
    return;
  }

  int sample_x = image_point->x();
  int sample_y = image_point->y();
  if (gpu_rectified) {
    const UndistortMap& map = *frame.rectify_map;
    const int lx = std::clamp(image_point->x(), 0, map.out_width - 1);
    const int ly = std::clamp(image_point->y(), 0, map.out_height - 1);
    const size_t idx = static_cast<size_t>(ly) * static_cast<size_t>(map.out_width) + static_cast<size_t>(lx);
    sample_x = static_cast<int>(std::lround(map.src_x[idx]));
    sample_y = static_cast<int>(std::lround(map.src_y[idx]));
  }

  if (point_inspector_ == nullptr) {
    point_inspector_ = std::make_unique<PixelInspector>();
  }

  // Depth frames carry metric depth (metres), not RGB — the colour is the GPU's
  // colormap output. Show the depth value + a swatch of that colour instead of a
  // meaningless RGB readout, and skip the zoom grid. Unlike the RGB path, a no-data
  // pixel still shows the tooltip ("— (no data)") rather than hiding it.
  if (frame.format == PixelFormat::kDepthR32F) {
    const auto depth_m = depthMetersAt(frame, sample_x, sample_y);
    point_inspector_->updateDepth(image_point->x(), image_point->y(), depth_m, frame.depth);
    point_inspector_->showNear(mapToGlobal(last_point_inspector_pos_.toPoint()));
    return;
  }

  auto crop = extractRgbCrop(frame, sample_x, sample_y, kPointInspectorCropSize);
  if (crop.empty()) {
    hidePointInspector();
    return;
  }

  point_inspector_->updatePixel(std::move(crop), kPointInspectorCropSize, image_point->x(), image_point->y());
  point_inspector_->showNear(mapToGlobal(last_point_inspector_pos_.toPoint()));
}

void MediaViewerWidget::schedulePointInspectorRefresh() {
  if (!point_inspector_enabled_.load(std::memory_order_relaxed) ||
      !point_inspector_active_.load(std::memory_order_relaxed)) {
    return;
  }
  QMetaObject::invokeMethod(
      this,
      [this]() {
        if (point_inspector_enabled_.load(std::memory_order_relaxed) &&
            point_inspector_active_.load(std::memory_order_relaxed)) {
          refreshPointInspector();
        }
      },
      Qt::QueuedConnection);
}

void MediaViewerWidget::hidePointInspector() {
  if (point_inspector_ != nullptr) {
    point_inspector_->hideImmediately();
  }
}

QShader MediaViewerWidget::loadShader(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    qWarning("Failed to load shader: %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(f.readAll());
}

MediaViewerWidget::TextEntry* MediaViewerWidget::getOrCreateTextTexture(
    const std::string& text, double font_size, QRhiResourceUpdateBatch* updates) {
  // Quantize font size to half-pixel units so near-identical sizes share a texture.
  const auto fq = static_cast<uint32_t>(font_size * 2.0 + 0.5);
  TextKey key{text, fq};
  auto it = text_cache_.find(key);
  if (it != text_cache_.end()) {
    return &it->second;
  }
  auto* r = rhi();
  if (r == nullptr) {
    return nullptr;
  }
  // Render glyph mask via QPainter into an Alpha8 image (white pixels = opaque).
  QFont font;
  font.setPixelSize(static_cast<int>(font_size + 0.5));
  QFontMetricsF fm(font);
  QString qtext = QString::fromUtf8(text.c_str(), static_cast<int>(text.size()));
  const int padding = 2;
  const int w = static_cast<int>(std::ceil(fm.horizontalAdvance(qtext))) + 2 * padding;
  const int h = static_cast<int>(std::ceil(fm.height())) + 2 * padding;
  if (w <= 0 || h <= 0) {
    return nullptr;
  }
  QImage img(w, h, QImage::Format_Alpha8);
  img.fill(0);
  {
    QPainter p(&img);
    p.setFont(font);
    p.setPen(QColor(255, 255, 255, 255));
    p.drawText(QPointF(padding, fm.ascent() + padding), qtext);
  }
  auto* tex = r->newTexture(QRhiTexture::R8, QSize(w, h));
  if (!tex->create()) {
    delete tex;
    return nullptr;
  }
  QRhiTextureSubresourceUploadDescription sub_desc(img);
  sub_desc.setSourceSize(QSize(w, h));
  updates->uploadTexture(tex, QRhiTextureUploadDescription({0, 0, sub_desc}));

  // Each TextEntry owns its own SRB so per-draw rebinding is impossible —
  // QRhi reads the SRB at submit time, not at the cb->setShaderResources call,
  // so reusing one SRB and mutating it between draws ends up with all draws
  // reading the LAST binding. One SRB per cached texture sidesteps that.
  auto* srb = r->newShaderResourceBindings();
  srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
      QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, text_sampler_),
  });
  if (!srb->create()) {
    delete srb;
    delete tex;
    return nullptr;
  }

  TextEntry entry{tex, srb, w, h};
  auto [ins_it, _] = text_cache_.emplace(std::move(key), entry);
  return &ins_it->second;
}

}  // namespace PJ
