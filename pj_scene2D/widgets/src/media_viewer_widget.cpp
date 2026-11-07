// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/media_viewer_widget.h"

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
#include <utility>

#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/overlay_geometry.h"
#include "pj_scene2d_core/video_color.h"  // buildYuvMatrix (BT.601/709 + limited/full range)
#include "pj_scene2d_widgets/pixel_inspector.h"
#include "pj_widgets/Colormap.h"  // shared Colormap enum + buildColormapLut + colormapGlsl
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/GraphicsApi.h"
#include "pj_widgets/RhiBlend.h"
#include "pj_widgets/RhiTestHooks.h"
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
  setAutoRenderTarget(false);  // colour only, see color_target_
  setObjectName(u"mediaViewerCanvas"_s);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  // Qt's own failure (no QRhi for this window, e.g. its API differs from the one
  // another widget pinned the window to) never reaches initialize(): show it too.
  // setRenderingWarning() emits renderFailed() itself, after the latch is set.
  connect(this, &QRhiWidget::renderFailed, this, [this] {
    if (!rendering_warning_.failed()) {
      setRenderingWarning(tr("GPU rendering is unavailable: Qt could not create a QRhi for this window."));
    }
  });
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

#ifdef PJ_RHI_TEST_HOOKS
void MediaViewerWidget::setMaxTextureSizeForTest(int pixels) {
  max_texture_size_for_test_ = std::max(pixels, 0);
  oversize_frame_logged_ = false;
}
#endif

bool MediaViewerWidget::pointInspectorEnabled() const noexcept {
  return point_inspector_enabled_.load(std::memory_order_relaxed);
}

void MediaViewerWidget::resetView() {
  static_cast<void>(setViewState({.rotation_deg = rotation_deg_}));
}

MediaViewState MediaViewerWidget::viewState() const noexcept {
  return MediaViewState{.zoom = zoom_, .pan_x = pan_x_, .pan_y = pan_y_, .rotation_deg = rotation_deg_};
}

bool MediaViewerWidget::isViewStateValid(const MediaViewState& state) noexcept {
  constexpr float kMinZoom = 1.0f;
  constexpr float kMaxZoom = 20.0f;
  if (!std::isfinite(state.zoom) || !std::isfinite(state.pan_x) || !std::isfinite(state.pan_y) ||
      state.zoom < kMinZoom || state.zoom > kMaxZoom) {
    return false;
  }
  if (std::find(kViewRotations.begin(), kViewRotations.end(), state.rotation_deg) == kViewRotations.end()) {
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
  rotation_deg_ = state.rotation_deg;
  update();
  if (zoom_ != previous.zoom) {
    emit zoomChanged(zoom_);
  }
  if (rotation_deg_ != previous.rotation_deg) {
    {
      std::lock_guard lock(frame_mutex_);
      overlays_dirty_ = true;  // text quads are counter-rotated per rotation
    }
    emit rotationChanged(rotation_deg_);
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

// The output pixels the image quad covers under `view` (GL clip space). QRhi
// scissors put y = 0 at the bottom, like clip space, on every backend.
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
  const int y0 = std::max(0, static_cast<int>(std::floor((min_y * 0.5f + 0.5f) * height)));
  const int y1 = std::min(output_size.height(), static_cast<int>(std::ceil((max_y * 0.5f + 0.5f) * height)));
  return QRhiScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

// Recreates `tex` with a new format and size; false when the backend refuses it.
bool recreateTexture(QRhiTexture* tex, QRhiTexture::Format format, QSize size) {
  tex->destroy();
  tex->setFormat(format);
  tex->setPixelSize(size);
  return tex->create();
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
  noteRhiRelease(*this, rhi_cached_);
  // Nothing is held on any QRhi after this, so the next initialize() adopts one.
  rhi_cached_ = nullptr;
  optional_failures_logged_ = false;
#ifdef PJ_TARGET_WASM
  composition_refresh_queued_ = false;
#endif
  hidePointInspector();
  delete pipeline_;
  pipeline_ = nullptr;
  delete composite_pipeline_;
  composite_pipeline_ = nullptr;
  color_target_.release();
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
  // The overlay buffers died with the QRhi; rebuild them from the retained
  // annotations, or a reparented viewer (float/redock) shows none until new data.
  overlays_dirty_ = true;
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

void MediaViewerWidget::trimTextCache() {
  if (text_cache_.size() <= kMaxGlyphCacheEntries) {
    return;
  }
  // Only labels the last rebuild did not draw: text_draw_items_ borrows the others.
  std::vector<decltype(text_cache_)::iterator> unused;
  for (auto it = text_cache_.begin(); it != text_cache_.end(); ++it) {
    if (it->second.last_used != text_rebuilds_) {
      unused.push_back(it);
    }
  }
  const std::size_t evict = std::min(unused.size(), text_cache_.size() - kMaxGlyphCacheEntries);
  std::nth_element(
      unused.begin(), unused.begin() + static_cast<std::ptrdiff_t>(evict), unused.end(),
      [](auto a, auto b) { return a->second.last_used < b->second.last_used; });
  for (std::size_t i = 0; i < evict; ++i) {
    delete unused[i]->second.srb;
    delete unused[i]->second.tex;
    text_cache_.erase(unused[i]);
  }
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

bool MediaViewerWidget::dropTextureLayer(TextureLayerResources& layer) {
  layer.dropped = true;
  return false;
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

bool MediaViewerWidget::rebuildLayerSrb(TextureLayerResources& layer) {
  QRhiSampler* y_sampler = nullptr;
  QRhiSampler* uv_sampler = nullptr;
  layerSamplers(layer, y_sampler, uv_sampler);
  layer.srb->destroy();
  setTextureLayerBindings(
      layer.srb, layer.uniform_buf, layer.tex_y, layer.tex_u, layer.tex_v, uv_sampler,
      layer.tex_remap != nullptr ? layer.tex_remap : remap_placeholder_tex_, remap_sampler_, y_sampler);
  return !rhiTestFault(*this, "layer srb") && layer.srb->create();
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
  const int size_limit = max_texture_size_for_test_ > 0 ? max_texture_size_for_test_ : max_texture_size_;
  if (size_limit > 0 && (w > size_limit || h > size_limit)) {
    if (!std::exchange(oversize_frame_logged_, true)) {
      qWarning(
          "MediaViewerWidget: a %d x %d frame exceeds this GPU's %d-pixel texture limit; it is not drawn", w, h,
          size_limit);
    }
    return dropTextureLayer(layer);
  }
  const TexturePathFormat path = texturePathFor(frame.format);
  if (path == TexturePathFormat::kDepth) {
    layer.invert = frame.depth.invert ? 1 : 0;
    layer.near_m = frame.depth.near_m;
    layer.far_m = frame.depth.far_m;
    layer.colormap = static_cast<int32_t>(frame.depth.colormap);
  }
  // The textures already hold this buffer: only the per-frame state above changed.
  if (frame_in.pixels == layer.uploaded_pixels && w == layer.width && h == layer.height && path == layer.format &&
      !mag_changed) {
    return true;
  }
  layer.uploaded_pixels.reset();  // set again once this upload is queued
  ++frame_upload_count_;
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
      if (!recreateTexture(layer.tex_y, QRhiTexture::R8, QSize(w, h)) ||
          !recreateTexture(layer.tex_u, QRhiTexture::R8, QSize(uv_w, uv_h)) ||
          !recreateTexture(layer.tex_v, QRhiTexture::R8, QSize(uv_w, uv_h))) {
        return dropTextureLayer(layer);
      }
      layer.width = w;
      layer.height = h;
      layer.format = TexturePathFormat::kYUV420P;
      if (!rebuildLayerSrb(layer)) {
        return dropTextureLayer(layer);
      }
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
    layer.uploaded_pixels = frame_in.pixels;
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
      // u_tex holds the interleaved UV plane as RG8; v_tex is unused by the NV12
      // shader branch but stays bound (and valid) to keep one SRB layout.
      if (!recreateTexture(layer.tex_y, QRhiTexture::R8, QSize(w, h)) ||
          !recreateTexture(layer.tex_u, QRhiTexture::RG8, QSize(uv_w, uv_h))) {
        return dropTextureLayer(layer);
      }
      layer.width = w;
      layer.height = h;
      layer.format = TexturePathFormat::kNV12;
      if (!rebuildLayerSrb(layer)) {
        return dropTextureLayer(layer);
      }
    }

    QRhiTextureSubresourceUploadDescription y_desc(src, y_size);
    y_desc.setSourceSize(QSize(w, h));
    updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, y_desc}));

    QRhiTextureSubresourceUploadDescription uv_desc(src + y_size, uv_h * uv_row_bytes);
    uv_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, uv_desc}));
    layer.uploaded_pixels = frame_in.pixels;
    return true;
  }

  // Single-plane upload. Mono8 and BGRA go to the GPU verbatim (R8 / RGBA8) and
  // are expanded/swizzled in the shader — no CPU repack. RGB888/BGR888 (3-byte)
  // have no clean 4-byte-row GPU layout, so they still expand to RGBA on the CPU.
  QRhiTexture::Format tex_format = QRhiTexture::RGBA8;
  const uint8_t* upload_data = nullptr;
  size_t upload_size = 0;

  if (path == TexturePathFormat::kDepth) {
    tex_format = QRhiTexture::R32F;
    upload_data = src;  // float32 metric depth; the shader colormaps via the LUT in u_tex
    upload_size = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(float);
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
    if (!recreateTexture(layer.tex_y, tex_format, QSize(w, h))) {
      return dropTextureLayer(layer);
    }

    if (path == TexturePathFormat::kDepth) {
      // u_tex carries the colormap LUT: kColormapLutWidth (t) x kColormapCount (rows) RGBA8.
      static const std::vector<uint8_t> kColormapLut = buildColormapLut();
      const QSize lut_size(kColormapLutWidth, kColormapCount);
      if (!recreateTexture(layer.tex_u, QRhiTexture::RGBA8, lut_size)) {
        return dropTextureLayer(layer);
      }
      QRhiTextureSubresourceUploadDescription lut_desc(kColormapLut.data(), static_cast<quint32>(kColormapLut.size()));
      lut_desc.setSourceSize(lut_size);
      updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, lut_desc}));
    }

    layer.width = w;
    layer.height = h;
    layer.format = path;
    // rebuildLayerSrb() reads layer.format (set above): depth keeps its NEAREST
    // y-plane + LINEAR LUT; other formats honour the layer's magnification filter.
    if (!rebuildLayerSrb(layer)) {
      return dropTextureLayer(layer);
    }
  }

  QRhiTextureSubresourceUploadDescription sub_desc(upload_data, static_cast<quint32>(upload_size));
  sub_desc.setSourceSize(QSize(w, h));
  updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, sub_desc}));
  layer.uploaded_pixels = frame_in.pixels;
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
    layer.tex_remap = r->newTexture(QRhiTexture::RG32UI, QSize(out_w, out_h));
    if (!layer.tex_remap->create()) {
      delete layer.tex_remap;
      layer.tex_remap = nullptr;
      layer.remap_w = 0;
      layer.remap_h = 0;
      layer.remap_map_key = nullptr;
      // The SRB may still bind the LUT just deleted: rebind the placeholder.
      if (!rebuildLayerSrb(layer)) {
        dropTextureLayer(layer);
      }
      return false;
    }
    layer.remap_w = out_w;
    layer.remap_h = out_h;
    layer.remap_map_key = nullptr;  // force a re-upload into the new texture.
    // Bind slot 4 to the new LUT instead of the placeholder.
    if (!rebuildLayerSrb(layer)) {
      return dropTextureLayer(layer);
    }
  }

  // Upload the LUT only when the underlying map changes (calibration is constant
  // per camera, so this runs once). QRhi has no RG32F, so the normalized source UV
  // goes up as the bits of its two floats (RG32UI, 8 B/px) and the shader
  // reinterprets them.
  if (layer.remap_map_key != static_cast<const void*>(&map)) {
    const std::vector<float> rg = undistortMapToNormalizedRG(map);
    if (rg.size() != static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 2) {
      return false;
    }
    QRhiTextureSubresourceUploadDescription desc(rg.data(), static_cast<quint32>(rg.size() * sizeof(float)));
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
  // A failed (re)create drops the layer and deletes its textures, which uploads
  // already queued for this layer (a depth LUT, the frame planes) reference: they
  // go to a batch of their own, discarded then instead of reaching render()'s.
  QRhiResourceUpdateBatch* layer_updates = rhi()->nextResourceUpdateBatch();
  QSize logical;
  if (uploadDecodedFrameToTexture(frame, layer, layer_updates)) {
    // GPU path: the uploaded frame is RAW; the displayed (logical) size — which
    // annotation/aspect/inspector coords use — is the map's output size.
    const bool rectify_on_gpu = gpu_rectify_supported_.load(std::memory_order_relaxed) &&
                                frame.rectify_map != nullptr &&
                                ensureRemapTexture(layer, *frame.rectify_map, layer_updates);
    if (rectify_on_gpu) {
      logical = {frame.rectify_map->out_width, frame.rectify_map->out_height};
    } else {
      layer.rectify = 0;
      logical = {layer.width, layer.height};
    }
  }
  if (layer.dropped) {
    layer_updates->release();
    destroyTextureLayer(layer);
    static bool warned = false;
    if (!std::exchange(warned, true)) {
      qWarning("MediaViewerWidget: could not create a frame texture; the frame is not drawn");
    }
    return {};
  }
  updates->merge(layer_updates);
  layer_updates->release();
  return logical;
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
    const QRhiVertexInputLayout& input_layout, const char* name, bool warn) {
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
  overlay.pipeline->setRenderPassDescriptor(color_target_.renderPass());
  overlay.pipeline->setTargetBlends({PJ::straightAlphaBlend()});
  if (!createRhiPipeline(*this, *overlay.pipeline, name)) {
    if (warn) {
      qWarning("MediaViewerWidget: failed to create the %s overlay pipeline; that overlay is disabled", name);
    }
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
    if (!overlay.vbo->create()) {
      qWarning("MediaViewerWidget: could not grow an overlay vertex buffer; that overlay is dropped");
      destroyOverlayPipeline(overlay);
      return;
    }
  }
  if (needed > 0) {
    updates->updateDynamicBuffer(overlay.vbo, 0, static_cast<int>(needed), overlay.vertex_data.data());
  }
}

void MediaViewerWidget::setRenderingWarning(const QString& warning) {
  const RhiFailureLatch::Change change = rendering_warning_.set(warning);
  if (change == RhiFailureLatch::Change::kNone) {
    return;
  }
  if (!warning.isEmpty()) {
    qWarning("MediaViewerWidget: %s", qPrintable(warning));
  }
  emit renderingWarningChanged(rendering_warning_.reason());
  if (change == RhiFailureLatch::Change::kEntered) {
    emit renderFailed();
  }
}

bool MediaViewerWidget::createBaseResources(const QShader& vert, const QShader& frag) {
  auto* r = rhi();
  sampler_ = r->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
  // NEAREST sampler for layers requesting MagFilter::kNearest (crisp magnification).
  mag_nearest_sampler_ = r->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  // NEAREST sampler for the rectification LUT (exact per-output-pixel source coord).
  remap_sampler_ = r->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  // 1x1 RG32UI placeholder bound at slot 4 for non-rectifying layers so all
  // texture-layer SRBs share one binding layout (and the shader's usampler2D).
  remap_placeholder_tex_ = r->newTexture(QRhiTexture::RG32UI, QSize(1, 1));
  // Uniform buffer + placeholder textures (1x1) — resized on first frame.
  if (!sampler_->create() || !mag_nearest_sampler_->create() || !remap_sampler_->create() ||
      !remap_placeholder_tex_->create() || !ensureTextureLayer(base_texture_)) {
    return false;
  }

  pipeline_ = r->newGraphicsPipeline();
  pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  pipeline_->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
  pipeline_->setVertexInputLayout(QRhiVertexInputLayout());
  pipeline_->setShaderResourceBindings(base_texture_.srb);
  pipeline_->setRenderPassDescriptor(color_target_.renderPass());
  return createRhiPipeline(*this, *pipeline_, "base");
}

void MediaViewerWidget::initialize(QRhiCommandBuffer* /*cb*/) {
  auto* r = rhi();
  if (r == nullptr) {
    return;
  }

  if (rhi_cached_ != r) {
    noteRhiAdoption(*this, rhi_cached_, r);
    releaseResources();
    rhi_cached_ = r;
    max_texture_size_ = r->resourceLimit(QRhi::TextureSizeMax);
  }
  if (!color_target_.ensure(*r, colorTexture(), msaaColorBuffer(), resolveTexture(), /*refresh=*/true)) {
    releaseResources();
    setRenderingWarning(tr("GPU rendering is unavailable (%1): the render target could not be created.")
                            .arg(QString::fromLatin1(r->backendName())));
    return;
  }

  // Nothing to build: the common case of a resize's initialize().
  if (pipeline_ != nullptr && composite_pipeline_ != nullptr && points_overlay_.pipeline != nullptr &&
      thick_overlay_.pipeline != nullptr && text_overlay_.pipeline != nullptr) {
    return;
  }

  // Use YUV→RGB shader (handles both YUV and RGBA passthrough). Each shader
  // pair loads only for a pipeline being (re)built.
  QShader vert;
  QShader frag;
  if (pipeline_ == nullptr || composite_pipeline_ == nullptr) {
    vert = PJ::loadRhiShader(QStringLiteral(":/shaders/yuv_to_rgb.vert.qsb"));
    frag = PJ::loadRhiShader(QStringLiteral(":/shaders/yuv_to_rgb.frag.qsb"));
  }
  if (pipeline_ == nullptr) {
    if (!vert.isValid() || !frag.isValid() || !createBaseResources(vert, frag)) {
      // Failed state: pipeline_ is null, so render() only clears, and the next
      // initialize() (new QRhi, resized target) tries again.
      releaseResources();
      setRenderingWarning(tr("GPU rendering is unavailable (%1): the image pipeline could not be created.")
                              .arg(QString::fromLatin1(r->backendName())));
      return;
    }
    setRenderingWarning({});
  }
  // The optional parts are (re)built whenever missing, so one dropped at
  // creation or in render() comes back on the next initialize().
  createOptionalResources(r, vert, frag);

  // GPU rectification needs only the base pipeline: its RG32UI placeholder is
  // the LUT's format, so the LUT works wherever the pipeline was created. Not
  // isTextureFormatSupported(RG32UI): Qt's GLES backend reports the 32-bit
  // integer formats from ES 3.1 on, though RG32UI is core in ES 3.0 (WebGL2).
  // When available, the source defers rectification to the GPU (raw upload +
  // map); otherwise it keeps rectifying on the CPU (the precompute fallback).
  gpu_rectify_supported_.store(pipeline_ != nullptr, std::memory_order_relaxed);
  applyGpuRectifyCapability();

  if (has_pending_ || has_pending_pixel_layers_) {
    update();
  }
}

void MediaViewerWidget::createOptionalResources(QRhi* r, const QShader& vert, const QShader& frag) {
  // Warn about a missing part once per QRhi, not on every resize's initialize().
  const bool warn = !optional_failures_logged_;
  optional_failures_logged_ = true;
  const auto warning = [warn](const char* message) {
    if (warn) {
      qWarning("MediaViewerWidget: %s", message);
    }
  };

  if (composite_pipeline_ == nullptr) {
    composite_pipeline_ = r->newGraphicsPipeline();
    composite_pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
    composite_pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
    composite_pipeline_->setVertexInputLayout(QRhiVertexInputLayout());
    composite_pipeline_->setShaderResourceBindings(base_texture_.srb);
    composite_pipeline_->setRenderPassDescriptor(color_target_.renderPass());
    composite_pipeline_->setTargetBlends({PJ::straightAlphaBlend()});
    if (!createRhiPipeline(*this, *composite_pipeline_, "composite")) {
      warning("failed to create the composite pipeline; layers draw without blending");
      delete composite_pipeline_;
      composite_pipeline_ = nullptr;
    }
  }

  // ----- Marker / overlay shaders + shared uniform -----
  // The scene_lines shaders (position + colour) drive both the point quads and the
  // thick (Triangles) outline pipeline below; the uniform buffer (view + frame
  // size) is shared by every overlay pipeline.
  QShader marker_vert;
  QShader marker_frag;
  if (marker_uniform_buf_ == nullptr || thick_overlay_.pipeline == nullptr || points_overlay_.pipeline == nullptr) {
    marker_vert = PJ::loadRhiShader(QStringLiteral(":/shaders/scene_lines.vert.qsb"));
    marker_frag = PJ::loadRhiShader(QStringLiteral(":/shaders/scene_lines.frag.qsb"));
    if (!marker_vert.isValid() || !marker_frag.isValid()) {
      warning("scene_lines shaders not loaded; markers disabled");
    } else if (marker_uniform_buf_ == nullptr) {
      marker_uniform_buf_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMarkerUniformBufSize);
      if (!marker_uniform_buf_->create()) {
        warning("failed to create the overlay uniform buffer; markers disabled");
        delete marker_uniform_buf_;
        marker_uniform_buf_ = nullptr;
      }
    }
  }
  if (marker_uniform_buf_ == nullptr) {
    return;
  }
  bool rebuilt = false;

  // ----- kPoints quad pipeline -----
  // Solid-fill triangles, shares marker_uniform_buf_ but has its own SRB and VBO.
  if (marker_vert.isValid() && marker_frag.isValid() && points_overlay_.pipeline == nullptr) {
    destroyOverlayPipeline(points_overlay_);
    rebuilt = createOverlayVbo(points_overlay_, 64 * 1024) && createUniformOverlaySrb(points_overlay_) &&
              createOverlayGraphicsPipeline(
                  points_overlay_, marker_vert, marker_frag, QRhiGraphicsPipeline::Triangles, colorVertexInputLayout(),
                  "points", warn);
  }

  // ----- Outline pipeline (Triangles) -----
  // All line/circle outlines render here. Each segment is expanded CPU-side
  // (overlay_geometry::appendLineStrokes) into a cosmetic-width quad — same vertex
  // layout as the fills (vec2 pos + vec4 color, stride 24), Triangles topology.
  if (marker_vert.isValid() && marker_frag.isValid() && thick_overlay_.pipeline == nullptr) {
    destroyOverlayPipeline(thick_overlay_);
    rebuilt = (createOverlayVbo(thick_overlay_, 64 * 1024) && createUniformOverlaySrb(thick_overlay_) &&
               createOverlayGraphicsPipeline(
                   thick_overlay_, marker_vert, marker_frag, QRhiGraphicsPipeline::Triangles, colorVertexInputLayout(),
                   "thick", warn)) ||
              rebuilt;
  }

  // ----- Text pipeline (textured quads, R8 alpha mask + tint color) -----
  if (text_overlay_.pipeline == nullptr) {
    const QShader text_vert = PJ::loadRhiShader(QStringLiteral(":/shaders/scene_text.vert.qsb"));
    const QShader text_frag = PJ::loadRhiShader(QStringLiteral(":/shaders/scene_text.frag.qsb"));
    if (!text_vert.isValid() || !text_frag.isValid()) {
      warning("scene_text shaders not loaded; text disabled");
    } else {
      // The cached glyph SRBs reference the old sampler.
      destroyOverlayPipeline(text_overlay_);
      clearTextCache();
      delete text_sampler_;
      delete text_placeholder_tex_;
      text_sampler_ = nullptr;
      text_placeholder_tex_ = nullptr;
      if (createOverlayVbo(text_overlay_, 32 * 1024)) {
        text_sampler_ = r->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
            QRhiSampler::ClampToEdge);

        // Placeholder 1x1 alpha texture used only as the pipeline's layout-compat SRB.
        // Real per-draw SRBs are stored in TextEntry inside text_cache_.
        text_placeholder_tex_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));

        text_overlay_.srb = r->newShaderResourceBindings();
        text_overlay_.srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                marker_uniform_buf_),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage, text_placeholder_tex_, text_sampler_),
        });
        if (!text_sampler_->create() || !text_placeholder_tex_->create() || !text_overlay_.srb->create()) {
          warning("failed to create the text resources; text disabled");
          destroyOverlayPipeline(text_overlay_);
        } else {
          rebuilt = createOverlayGraphicsPipeline(
                        text_overlay_, text_vert, text_frag, QRhiGraphicsPipeline::Triangles, textVertexInputLayout(),
                        "text", warn) ||
                    rebuilt;
        }
      }
    }
  }
  if (rebuilt) {
    overlays_dirty_ = true;  // re-upload the retained annotations into the new buffers
  }
}

void MediaViewerWidget::render(QRhiCommandBuffer* cb) {
  // The failed state released it with everything else, yet still clears.
  if (rhi() == nullptr ||
      !color_target_.ensure(*rhi(), colorTexture(), msaaColorBuffer(), resolveTexture(), /*refresh=*/false)) {
    return;
  }
  QRhiRenderTarget* rt = color_target_.target();
  if (pipeline_ == nullptr) {
    // Failed state: show the background, draw nothing (see initialize()).
    cb->beginPass(rt, clear_color_, {1.0f, 0});
    cb->endPass();
    return;
  }
  auto* r = rhi();
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
        } else {
          // No pixel content (a derived image's tombstone cleared it): drop what is shown.
          pending_pixel_layers_.clear();
          has_pending_pixel_layers_ = true;
          pixel_layers_active_ = true;
          has_pending_ = false;
          pending_decoded_ = {};
          inspector_frame_ = {};
          inspector_frame_changed = true;
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
  // `view` is in GL clip space, which QRhi scissors also use; the shaders get it
  // corrected for the backend (Vulkan's y-down clip space, [0,1] depth elsewhere).
  QMatrix4x4 view = buildViewTransform(output_size);
  const QMatrix4x4 shader_view = r->clipSpaceCorrMatrix() * view;
  updateTextureLayerUniform(base_texture_, shader_view, updates);
  if (pixel_layers_active_) {
    for (auto& layer : pixel_layer_textures_) {
      updateTextureLayerUniform(layer, shader_view, updates);
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
    const int shown_width = isQuarterTurn(rotation_deg_) ? tex_height_ : tex_width_;
    const int shown_height = isQuarterTurn(rotation_deg_) ? tex_width_ : tex_height_;
    const double fit = std::min(
        static_cast<double>(output_size.width()) / static_cast<double>(shown_width),
        static_cast<double>(output_size.height()) / static_cast<double>(shown_height));
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

      // ----- Text rebuild (textured quads) — only when the annotation set or
      // the rotation changes (quads are counter-rotated to stay upright); text
      // size is not cosmetic, so a pure zoom change leaves it. -----
      if (overlays_dirty_ && text_overlay_.pipeline != nullptr) {
        ++text_rebuilds_;
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
              const auto corners = overlay_geometry::uprightTextQuadCorners(
                  ta.position, static_cast<double>(entry->width), static_cast<double>(entry->height), rotation_deg_);
              const float tr = static_cast<float>(ta.color.r) / 255.0f;
              const float tg = static_cast<float>(ta.color.g) / 255.0f;
              const float tb = static_cast<float>(ta.color.b) / 255.0f;
              const float tap = static_cast<float>(ta.color.a) / 255.0f;
              const size_t offset_bytes = text_overlay_.vertex_data.size() * sizeof(float);
              // Two triangles over corners TL,TR,BR,BL; UV (0,0) is the glyph's top-left.
              constexpr std::array<size_t, 6> kTriangleCorners{0, 1, 2, 0, 2, 3};
              constexpr std::array<std::array<float, 2>, 4> kCornerUv{
                  {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
              for (const size_t corner : kTriangleCorners) {
                text_overlay_.vertex_data.insert(
                    text_overlay_.vertex_data.end(),
                    {static_cast<float>(corners[corner].x), static_cast<float>(corners[corner].y), kCornerUv[corner][0],
                     kCornerUv[corner][1], tr, tg, tb, tap});
              }
              text_draw_items_.push_back(TextDrawItem{entry->srb, offset_bytes});
            }
          }
        }
        uploadOverlayVertexData(text_overlay_, updates);
        trimTextCache();
      }

      overlays_dirty_ = false;
    }

    {
      MarkerUbo ubo{};
      std::memcpy(ubo.view, shader_view.constData(), sizeof(ubo.view));
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
  if (pixel_layers_active_) {
    // Without the blending pipeline (its create() failed) the layers draw opaque.
    cb->setGraphicsPipeline(composite_pipeline_ != nullptr ? composite_pipeline_ : pipeline_);
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
  } else if (base_texture_.srb != nullptr) {  // null after a dropped frame texture
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
    const QPointF pan = panAfterZoom(
        QPointF(previous.pan_x, previous.pan_y), previous.zoom, next.zoom, e->position(), size(),
        fitScale(size(), frame_aspect_, rotation_deg_));
    next.pan_x = static_cast<float>(pan.x());
    next.pan_y = static_cast<float>(pan.y());
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
    const QPointF delta = e->position() - last_mouse_pos_;
    const QPointF pan =
        panAfterDrag(QPointF(pan_x_, pan_y_), zoom_, delta, size(), fitScale(size(), frame_aspect_, rotation_deg_));
    pan_x_ = static_cast<float>(pan.x());
    pan_y_ = static_cast<float>(pan.y());
    pan_interaction_changed_ = pan_interaction_changed_ || !delta.isNull();
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
  const QPointF fit = fitScale(output_size, frame_aspect_, rotation_deg_);
  m.scale(static_cast<float>(fit.x()) * zoom_, static_cast<float>(fit.y()) * zoom_);
  m.translate(pan_x_, pan_y_);
  // The image quad spans [-1,1]^2 whatever its aspect, so a quarter turn is an
  // exact axis swap there; fitScale() handles the aspect. Negative angle
  // because clip space is y-up and the rotation is clockwise on screen.
  m.rotate(static_cast<float>(-rotation_deg_), 0.0f, 0.0f, 1.0f);
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

  const auto image_point = widgetPointToImagePixel(
      last_point_inspector_pos_, size(), QSize(logical_w, logical_h), zoom_, pan_x_, pan_y_, rotation_deg_);
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

MediaViewerWidget::TextEntry* MediaViewerWidget::getOrCreateTextTexture(
    const std::string& text, double font_size, QRhiResourceUpdateBatch* updates) {
  // Quantize font size to half-pixel units so near-identical sizes share a texture.
  const auto fq = static_cast<uint32_t>(font_size * 2.0 + 0.5);
  TextKey key{text, fq};
  auto it = text_cache_.find(key);
  if (it != text_cache_.end()) {
    it->second.last_used = text_rebuilds_;
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

  ++label_rasterizations_;
  TextEntry entry{tex, srb, w, h, text_rebuilds_};
  auto [ins_it, _] = text_cache_.emplace(std::move(key), entry);
  return &ins_it->second;
}

}  // namespace PJ
