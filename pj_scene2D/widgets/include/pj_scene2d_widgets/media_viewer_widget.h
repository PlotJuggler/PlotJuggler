#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>

#include <QColor>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QRhiWidget>
#include <QSize>
#include <QWheelEvent>
#ifdef PJ_TARGET_WASM
#include <QShowEvent>
#endif
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_scene2d_core/media_frame.h"
#include "pj_scene2d_core/scene_frame.h"
#include "pj_scene2d_core/undistort_remap.h"
#include "pj_scene2d_core/video_color.h"         // buildYuvMatrix (per-layer YUV->RGB matrix cache)
#include "pj_scene2d_widgets/pixel_inspector.h"  // kViewRotations
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

class MediaSource;
class PixelInspector;

/// XML-visible spatial viewport of a 2D media canvas.
///
/// Pan is expressed in the viewer's normalized clip-space convention. It is
/// meaningful only above 1x zoom; the default view therefore has zero pan.
struct MediaViewState {
  float zoom = 1.0f;
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  /// Clockwise rotation of the whole composite, one of kViewRotations. Pan is
  /// applied after the rotation, so it stays in screen directions.
  int rotation_deg = 0;

  friend bool operator==(const MediaViewState&, const MediaViewState&) = default;
};

/// GPU-accelerated scene/video viewer using QRhiWidget.
///
/// Attach a MediaSource with setMediaSource(), then call setTimestamp() on each
/// application tick. The widget polls the source in render() via takeFrame().
///
/// Supports YUV420P + NV12 (colorspace-aware YUV→RGB: BT.601/709 + limited/full
/// range, selected per-frame), packed RGB/RGBA DecodedFrame payloads, and
/// MediaFrame.pixel_layers alpha-composited in order.
/// SceneFrame overlays (points/lines/circles/text) are tessellated CPU-side and
/// drawn above the image; see ARCHITECTURE.md §7.1.
///
/// Zoom (mouse wheel, cursor-anchored) and pan (mouse drag) via a view
/// transform matrix in the vertex shader. See REQUIREMENTS.md §4.7.
class MediaViewerWidget : public QRhiWidget {
  Q_OBJECT
  Q_PROPERTY(QColor clearColor READ clearColor WRITE setClearColor)

 public:
  explicit MediaViewerWidget(QWidget* parent = nullptr);
  ~MediaViewerWidget() override;

  /// Attach a MediaSource. The widget does NOT take ownership.
  /// Call setTimestamp() to drive the source; render() polls takeFrame().
  void setMediaSource(MediaSource* source);

  /// Forward a timestamp to the attached MediaSource.
  /// No-op if no source is attached.
  void setTimestamp(int64_t ts_ns);

  /// Reset zoom to 1x and pan to origin; keeps the rotation.
  void resetView();

  /// Returns the spatial viewport represented in Scene2D workspace XML.
  [[nodiscard]] MediaViewState viewState() const noexcept;

  /// Applies a validated spatial viewport. Rejects non-finite values, zoom
  /// outside [1, 20], non-zero pan at 1x and a rotation outside kViewRotations
  /// without changing the current view.
  /// Programmatic restore does not emit viewInteractionCommitted().
  bool setViewState(const MediaViewState& state);

  /// Shared validation used to preflight XML before a destructive dock restore.
  [[nodiscard]] static bool isViewStateValid(const MediaViewState& state) noexcept;

  void setClearColor(const QColor& color);
  [[nodiscard]] QColor clearColor() const;

  /// Enables the hover pixel magnifier used by the global "Show point" toggle.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

 signals:
  void zoomChanged(float zoom);
  void rotationChanged(int degrees);
  /// One complete user gesture changed the XML-visible viewport: one wheel
  /// event, a finished pan drag, or a double-click reset.
  void viewInteractionCommitted();

 protected:
  void initialize(QRhiCommandBuffer* cb) override;
  void render(QRhiCommandBuffer* cb) override;
  void releaseResources() override;
#ifdef PJ_TARGET_WASM
  void showEvent(QShowEvent* event) override;
#endif

  void wheelEvent(QWheelEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  [[nodiscard]] QMatrix4x4 buildViewTransform(QSize output_size) const;
  [[nodiscard]] bool hasRetainedUploadableFrameLocked() const;
  void resetPendingPixelLayers();
  void refreshPointInspector();
  void schedulePointInspectorRefresh();
  void hidePointInspector();
  static QShader loadShader(const QString& path);
  // Get-or-create the glyph mask texture for a given (text, font_size). Renders
  // via QPainter on first miss and uploads as an R8 QRhiTexture. The texture
  // pointer is owned by text_cache_; never delete the returned pointer.
  struct TextEntry;
  TextEntry* getOrCreateTextTexture(const std::string& text, double font_size, QRhiResourceUpdateBatch* updates);
  // Delete every TextEntry's owned QRhi resources and clear the cache + per-frame
  // draw items. Caller must hold any locks that protect text_cache_ /
  // text_draw_items_; called by setMediaSource() (under frame_mutex_) and by
  // releaseResources() (Qt has already stopped rendering).
  void clearTextCache();

  // Selects the YUV→RGB shader path. Values must match the `pixelFormat`
  // uniform contract in shaders/yuv_to_rgb.frag. kNV12 uploads natively as a
  // two-plane texture (R8 Y + RG8 interleaved UV) on the hardware-decode path,
  // with a CPU deinterleave-to-YUV420P fallback when the backend lacks RG8.
  // kMono8 (single R8 texture, expanded in-shader) and kBGRA (RGBA8 texture,
  // swizzled in-shader) upload natively, skipping the CPU repack RGB/Mono need.
  enum class TexturePathFormat : int32_t {
    kYUV420P = 0,
    kNV12 = 1,
    kRGBA = 2,
    kMono8 = 3,
    kBGRA = 4,
    kDepth = 5,  ///< R32F metric depth; colormapped via a LUT in u_tex (GPU)
  };

  // Single definition of the PixelFormat -> shader-path projection: planar YUV
  // stays planar; every packed RGB/BGR/mono layout is CPU-converted and lands
  // on the RGBA path.
  [[nodiscard]] static TexturePathFormat texturePathFor(PixelFormat format) noexcept;
  [[nodiscard]] static bool isUploadablePixelFormat(PixelFormat format) noexcept;

  struct TextureLayerResources {
    QRhiTexture* tex_y = nullptr;
    QRhiTexture* tex_u = nullptr;
    QRhiTexture* tex_v = nullptr;
    QRhiTexture* tex_remap = nullptr;  ///< RGBA32F rectification LUT (.rg = source UV); null -> bind placeholder.
    QRhiBuffer* uniform_buf = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    int width = 0;   ///< Uploaded texture width = SOURCE (raw) width on the GPU-rectify path.
    int height = 0;  ///< Uploaded texture height.
    TexturePathFormat format = TexturePathFormat::kRGBA;
    float opacity = 1.0f;
    int32_t rectify = 0;  ///< 1 when the shader must remap through tex_remap (GPU rectification).
    int remap_w = 0;      ///< Size tex_remap was created at (the map's out_width/out_height).
    int remap_h = 0;
    const void* remap_map_key = nullptr;  ///< Identity of the UndistortMap tex_remap was uploaded from.
    // Depth colormap params (kDepth path) -> written into the uniform buffer for the shader.
    int32_t invert = 0;
    float near_m = 0.0f;
    float far_m = 1.0f;
    int32_t colormap = 0;
    // YUV→RGB colorimetry (kYUV420P / kNV12 paths) -> selects the shader color
    // matrix via buildYuvMatrix(); ignored by RGB/mono/depth.
    YuvColorSpace color_space = YuvColorSpace::kBt709;
    YuvColorRange color_range = YuvColorRange::kFull;
    // Cached YUV→RGB matrix for the above (rebuilt only when a frame's colorimetry
    // changes, in uploadDecodedFrameToTexture) so the per-frame uniform upload does
    // not recompute it on the ~60 Hz render path. Default = legacy full-range BT.709.
    std::array<float, 16> color_matrix = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
    // Display magnification filter. A change rebuilds the SRB to swap the sampler.
    MagFilter mag_filter = MagFilter::kLinear;
  };

  struct OverlayPipeline {
    QRhiGraphicsPipeline* pipeline = nullptr;
    QRhiBuffer* vbo = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    size_t vbo_capacity = 0;
    std::vector<float> vertex_data;
  };

  void clearPixelLayerTextures();
  void destroyTextureLayer(TextureLayerResources& layer);
  bool ensureTextureLayer(TextureLayerResources& layer);
  bool uploadDecodedFrameToTexture(
      const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates);
  // Build/upload (once per map) the RGBA32F rectification LUT for `map` into
  // layer.tex_remap, rebinding the layer's SRB to it, and set layer.rectify=1.
  // The GPU then undistorts at draw time. Returns false on allocation failure.
  bool ensureRemapTexture(TextureLayerResources& layer, const UndistortMap& map, QRhiResourceUpdateBatch* updates);
  // Upload `frame` into `layer` and apply GPU rectification when the frame carries
  // a rectify_map and the backend supports it. Used by BOTH the base and the
  // pixel-layer paths so they never diverge. Returns the LOGICAL (displayed) size
  // — the map's output size when rectifying on GPU, else the uploaded texture size
  // — for the caller to drive overlay/aspect/inspector coords; empty on failure.
  [[nodiscard]] QSize uploadLayerFrame(
      const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates);
  // Tell the attached source whether GPU rectification is available (probed in
  // initialize()). Re-applied when a source is attached. No lock taken here.
  void applyGpuRectifyCapability();
  void updateTextureLayerUniform(
      TextureLayerResources& layer, const QMatrix4x4& view, QRhiResourceUpdateBatch* updates) const;
  // Pick the y-plane and chroma/LUT samplers for a layer's SRB: depth always
  // samples its R32F y-plane NEAREST (never blend the no-data sentinel) with a
  // LINEAR LUT; every other format uses the layer's MagFilter for both planes.
  void layerSamplers(const TextureLayerResources& layer, QRhiSampler*& y_sampler, QRhiSampler*& uv_sampler) const;
  // Destroy + recreate a layer's SRB against its current textures and samplers
  // (binds the real remap LUT when present, else the placeholder). Used by every
  // upload branch after a (re)create; keeps the bind list in exactly one place.
  void rebuildLayerSrb(TextureLayerResources& layer);
  void destroyOverlayPipeline(OverlayPipeline& overlay);
  bool createOverlayVbo(OverlayPipeline& overlay, size_t initial_capacity);
  bool createUniformOverlaySrb(OverlayPipeline& overlay);
  bool createOverlayGraphicsPipeline(
      OverlayPipeline& overlay, const QShader& vert, const QShader& frag, QRhiGraphicsPipeline::Topology topology,
      const QRhiVertexInputLayout& input_layout, const char* failure_message);
  void uploadOverlayVertexData(OverlayPipeline& overlay, QRhiResourceUpdateBatch* updates);

  // Pipeline for YUV→RGB shader (video frames)
  QRhi* rhi_cached_ = nullptr;
#ifdef PJ_TARGET_WASM
  // Qt/WASM switches the top-level from raster to RHI composition when this
  // late-created widget submits its first frame. Re-armed by releaseResources().
  bool composition_refresh_queued_ = false;
#endif
  QRhiGraphicsPipeline* pipeline_ = nullptr;
  QRhiGraphicsPipeline* composite_pipeline_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  // NEAREST/ClampToEdge sampler used when a layer requests MagFilter::kNearest
  // (crisp "pixelated" magnification, e.g. for pixel inspection). Distinct from
  // remap_sampler_, which is nearest for a different reason (exact LUT lookup).
  QRhiSampler* mag_nearest_sampler_ = nullptr;
  // NEAREST sampler for the rectification LUT: every output pixel must read its
  // exact precomputed source coord (LINEAR would interpolate across the
  // out-of-bounds sentinel and smear the border).
  QRhiSampler* remap_sampler_ = nullptr;
  // 1x1 RGBA32F placeholder bound at SRB slot 4 for layers that don't rectify, so
  // every texture-layer pipeline shares one SRB layout.
  QRhiTexture* remap_placeholder_tex_ = nullptr;
  // True once initialize() confirms the backend can sample an RGBA32F LUT; gates
  // whether the source is told to defer rectification to the GPU.
  std::atomic_bool gpu_rectify_supported_{false};
  TextureLayerResources base_texture_;

  // MediaSource (not owned)
  MediaSource* media_source_ = nullptr;

  // Last CPU-side frame. `has_pending_` marks whether it still needs an
  // upload; the data itself is intentionally retained after upload so QRhi
  // resource recreation can restore the latest visible image.
  std::mutex frame_mutex_;
  DecodedFrame pending_decoded_;  // YUV420P or RGB frame
  DecodedFrame inspector_frame_;
  bool has_pending_ = false;
  std::vector<PixelLayer> pending_pixel_layers_;
  bool has_pending_pixel_layers_ = false;
  bool pixel_layers_active_ = false;

  int tex_width_ = 0;
  int tex_height_ = 0;
  float frame_aspect_ = 0.0f;

  std::vector<TextureLayerResources> pixel_layer_textures_;
  std::vector<uint8_t> rgba_repack_buffer_;

  float zoom_ = 1.0f;
  float pan_x_ = 0.0f;
  float pan_y_ = 0.0f;
  int rotation_deg_ = 0;
  bool pan_interaction_changed_ = false;
  QPointF last_mouse_pos_;
  QPointF last_point_inspector_pos_;
  QColor clear_color_;
  std::unique_ptr<PixelInspector> point_inspector_;
  std::atomic_bool point_inspector_enabled_{false};
  std::atomic_bool point_inspector_active_{false};

  // Uniform buffer layout (std140) — must match the Uniforms block in
  // shaders/yuv_to_rgb.{vert,frag} field-for-field:
  // mat4  viewTransform (64 bytes, offset 0)
  // mat4  colorMatrix   (64 bytes, offset 64)
  // int   pixelFormat   (4 bytes, offset 128)
  // float opacity       (4 bytes, offset 132)
  // int   rectify       (4 bytes, offset 136)
  // int   invert        (4 bytes, offset 140)  — depth path
  // float near_m        (4 bytes, offset 144)  — depth path
  // float far_m         (4 bytes, offset 148)  — depth path
  // int   colormap_id   (4 bytes, offset 152)  — depth path
  // padding             (4 bytes, → 160, a multiple of the mat4 base alignment)
  static constexpr int kUniformBufSize = 160;

  // ----- Vector overlay pipelines (markers / annotations) -----
  // Drawn on top of the image pass, all sharing this uniform buffer so they
  // track the same pan/zoom/letterbox view transform.
  QRhiBuffer* marker_uniform_buf_ = nullptr;
  std::vector<SceneFrame> last_overlays_;  ///< persisted across renders
  bool overlays_dirty_ = false;            ///< rebuild VBO on next render
  // Effective view scale (on-screen px per image px) the stroke geometry was last
  // expanded at. Stroke width scales with zoom but is floored at 1px on screen,
  // and that floor depends on this scale — so a change here, not just an
  // annotation change, triggers re-expansion. 0 = never expanded.
  double last_overlay_scale_ = 0.0;

  // ----- Fills pipeline (Triangles): kPoints squares + LineLoop/circle fills.
  // Shares marker_uniform_buf_ with its own SRB and VBO. -----
  OverlayPipeline points_overlay_;

  // ----- Outline pipeline (Triangles): all line/circle strokes. -----
  // Each segment is expanded CPU-side (overlay_geometry::appendLineStrokes) to a
  // quad whose width scales with zoom but is floored at 1px on screen so edges
  // never vanish. Replaces the old native GL_LINES path, which gave only 1 px and
  // could be guard-band culled when zoomed far in.
  OverlayPipeline thick_overlay_;

  // ----- Text pipeline (Triangles, textured quads with QPainter masks) -----
  // Fifth QRhi pipeline. One textured quad per TextAnnotation; texture is an
  // R8 alpha mask painted by QPainter, the per-vertex color provides the tint.
  // Cache key = (text, font_size_q): two labels with same text+size but different
  // colors share the same texture (color applied at fragment time).
  OverlayPipeline text_overlay_;                 // pipeline layout SRB uses the placeholder texture
  QRhiTexture* text_placeholder_tex_ = nullptr;  // owned, lives until releaseResources
  QRhiSampler* text_sampler_ = nullptr;

  struct TextKey {
    std::string text;
    uint32_t font_size_q = 0;
    bool operator==(const TextKey& o) const noexcept {
      return font_size_q == o.font_size_q && text == o.text;
    }
  };
  struct TextKeyHash {
    size_t operator()(const TextKey& k) const noexcept {
      return std::hash<std::string>{}(k.text) ^ (static_cast<size_t>(k.font_size_q) * 0x9e3779b9u);
    }
  };
  struct TextEntry {
    QRhiTexture* tex = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;  // Owns its own binding to `tex`.
    int width = 0;
    int height = 0;
  };
  std::unordered_map<TextKey, TextEntry, TextKeyHash> text_cache_;
  // Per-text quad metadata captured at rebuild time, consumed at draw time.
  struct TextDrawItem {
    QRhiShaderResourceBindings* srb;  // pointer borrowed from text_cache_
    size_t vbo_offset_bytes;
  };
  std::vector<TextDrawItem> text_draw_items_;

  // Uniform layout for marker pipeline (std140). frameSize is vec4 (only .xy
  // used) instead of vec2 to dodge a std140 alignment quirk in the OpenGL
  // backend; see scene_lines.vert for context.
  struct alignas(16) MarkerUbo {
    float view[16];
    float frame_size[4];
  };
  static constexpr int kMarkerUniformBufSize = sizeof(MarkerUbo);
};

}  // namespace PJ
