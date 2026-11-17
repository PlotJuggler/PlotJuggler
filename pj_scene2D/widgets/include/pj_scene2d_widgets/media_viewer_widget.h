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
#include <cstddef>
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
#include "pj_widgets/GraphicsApi.h"

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

/// The QRhiWidget that draws a MediaSource's frames: render() polls takeFrame(). Pixel layers, overlays and the
/// view transform are described in docs/ARCHITECTURE.md "Rendering".
///
/// GPU failure: when the image pipeline, its samplers or its base textures cannot be created, the viewer only clears
/// to its background, emits renderFailed() and renderingWarningChanged() once, and retries on every later
/// initialize(); a window that gets no QRhi at all sets the same warning. A failed overlay or frame texture is
/// dropped, and without the blending pipeline the pixel layers draw opaque. No draw may reference a resource whose
/// create() failed (D3D11 crashes in beginPass on one).
class MediaViewerWidget : public QRhiWidget {
  Q_OBJECT
  Q_PROPERTY(QColor clearColor READ clearColor WRITE setClearColor)

 public:
  /// Construct it parentless and then insert it: Qt enables QRhi composition for a
  /// window that is already shown only when a QRhiWidget is reparented into it, so
  /// passing a parent here leaves that window raster ("QRhiWidget: No QRhi").
  explicit MediaViewerWidget(QWidget* parent = nullptr);
  ~MediaViewerWidget() override;

  /// Not owned.
  void setMediaSource(MediaSource* source);

  void setTimestamp(int64_t ts_ns);

  /// Reset zoom to 1x and pan to origin; keeps the rotation.
  void resetView();

  [[nodiscard]] MediaViewState viewState() const noexcept;

  /// Rejects, leaving the view as is, non-finite values, zoom outside [1, 20], non-zero pan at 1x and a rotation
  /// outside kViewRotations. Does not emit viewInteractionCommitted().
  bool setViewState(const MediaViewState& state);

  [[nodiscard]] static bool isViewStateValid(const MediaViewState& state) noexcept;

  void setClearColor(const QColor& color);
  [[nodiscard]] QColor clearColor() const;

  /// Enables the hover pixel magnifier used by the global "Show point" toggle.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

#ifdef PJ_RHI_TEST_HOOKS
  /// Lowers the frame-size limit below the backend's TextureSizeMax (0 restores
  /// it), so a test can exercise the rejection without a huge image.
  void setMaxTextureSizeForTest(int pixels);
  /// Frame texture uploads queued since construction; cumulative.
  [[nodiscard]] std::uint64_t frameUploadCountForTest() const noexcept {
    return frame_upload_count_;
  }
  /// Label textures rasterized since construction (cache misses); cumulative.
  [[nodiscard]] std::uint64_t labelRasterizationsForTest() const noexcept {
    return label_rasterizations_;
  }
  [[nodiscard]] std::size_t labelCacheSizeForTest() const noexcept {
    return text_cache_.size();
  }
#endif

 signals:
  /// Non-empty while the viewer cannot render (see the class comment); empty
  /// once a later initialize() recovers. Emitted on changes only.
  void renderingWarningChanged(const QString& warning);
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
  // Builds the samplers, placeholder and base texture layer and the image
  // pipeline; false leaves partially created resources for releaseResources().
  [[nodiscard]] bool createBaseResources(const QShader& vert, const QShader& frag);
  // Enters (non-empty) or leaves (empty) the failed state.
  void setRenderingWarning(const QString& warning);
  // The returned entry is owned by text_cache_.
  struct TextEntry;
  TextEntry* getOrCreateTextTexture(const std::string& text, double font_size, QRhiResourceUpdateBatch* updates);
  // Caller holds frame_mutex_, or Qt has stopped rendering (releaseResources()).
  void clearTextCache();
  // Evicts the least recently used labels the last text rebuild did not use
  // until at most kMaxGlyphCacheEntries remain (or none unused is left).
  void trimTextCache();

  // The shader path. Values are the `pixelFormat` uniform of shaders/yuv_to_rgb.frag: change both together. NV12
  // falls back to a CPU deinterleave when the backend lacks RG8.
  enum class TexturePathFormat : int32_t {
    kYUV420P = 0,
    kNV12 = 1,
    kRGBA = 2,
    kMono8 = 3,
    kBGRA = 4,
    kDepth = 5,  ///< R32F depth; the colormap LUT is in u_tex
  };

  // RGB888, RGBA8888, BGR888 and Mono16 are converted on the CPU to the RGBA path; the others upload natively.
  [[nodiscard]] static TexturePathFormat texturePathFor(PixelFormat format) noexcept;
  [[nodiscard]] static bool isUploadablePixelFormat(PixelFormat format) noexcept;

  struct TextureLayerResources {
    QRhiTexture* tex_y = nullptr;
    QRhiTexture* tex_u = nullptr;
    QRhiTexture* tex_v = nullptr;
    QRhiTexture* tex_remap = nullptr;  ///< RG32UI LUT holding the float bits of the source UV; null: placeholder
    QRhiBuffer* uniform_buf = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    int width = 0;  ///< Uploaded size: the raw source's on the GPU-rectify path.
    int height = 0;
    TexturePathFormat format = TexturePathFormat::kRGBA;
    float opacity = 1.0f;
    int32_t rectify = 0;
    int remap_w = 0;
    int remap_h = 0;
    const void* remap_map_key = nullptr;  ///< The UndistortMap tex_remap was uploaded from.
    int32_t invert = 0;
    float near_m = 0.0f;
    float far_m = 1.0f;
    int32_t colormap = 0;
    YuvColorSpace color_space = YuvColorSpace::kBt709;
    YuvColorRange color_range = YuvColorRange::kFull;
    // Rebuilt only when the colorimetry changes.
    std::array<float, 16> color_matrix = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
    // A change rebuilds the SRB.
    MagFilter mag_filter = MagFilter::kLinear;
    // A (re)create failed during this upload: uploadLayerFrame() destroys the layer.
    bool dropped = false;
    // The pixel buffer the textures hold. A frame that brings the same buffer
    // (another layer of a composite changed) uploads nothing; holding the
    // reference keeps a new buffer from reusing its address meanwhile.
    std::shared_ptr<std::vector<uint8_t>> uploaded_pixels;
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
  // Marks a texture layer whose GPU resources could not be (re)created, for
  // uploadLayerFrame() to destroy; it is not drawn until the next upload
  // rebuilds it. Always returns false.
  bool dropTextureLayer(TextureLayerResources& layer);
  bool ensureTextureLayer(TextureLayerResources& layer);
  bool uploadDecodedFrameToTexture(
      const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates);
  // Uploads the LUT once per map and rebinds the SRB.
  bool ensureRemapTexture(TextureLayerResources& layer, const UndistortMap& map, QRhiResourceUpdateBatch* updates);
  // For the base and the pixel layers alike. Returns the LOGICAL size (the map's output size when rectifying on the
  // GPU) that overlays, aspect and inspector use; empty on failure. The uploads join `updates` only if the layer
  // survives them: a dropped layer's queued uploads reference deleted textures.
  [[nodiscard]] QSize uploadLayerFrame(
      const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates);
  // Takes no lock: setMediaSource() calls it with frame_mutex_ held.
  void applyGpuRectifyCapability();
  void updateTextureLayerUniform(
      TextureLayerResources& layer, const QMatrix4x4& view, QRhiResourceUpdateBatch* updates) const;
  // Depth samples nearest (blending would mix in the no-data 0) with a linear LUT; the rest follow MagFilter.
  void layerSamplers(const TextureLayerResources& layer, QRhiSampler*& y_sampler, QRhiSampler*& uv_sampler) const;
  [[nodiscard]] bool rebuildLayerSrb(TextureLayerResources& layer);
  void destroyOverlayPipeline(OverlayPipeline& overlay);
  bool createOverlayVbo(OverlayPipeline& overlay, size_t initial_capacity);
  bool createUniformOverlaySrb(OverlayPipeline& overlay);
  // `warn` logs a creation failure; false while retrying after a logged one.
  bool createOverlayGraphicsPipeline(
      OverlayPipeline& overlay, const QShader& vert, const QShader& frag, QRhiGraphicsPipeline::Topology topology,
      const QRhiVertexInputLayout& input_layout, const char* name, bool warn);
  // Builds whichever optional part is missing, so a dropped one returns on the next initialize(). Needs the base
  // resources.
  void createOptionalResources(QRhi* r, const QShader& vert, const QShader& frag);
  void uploadOverlayVertexData(OverlayPipeline& overlay, QRhiResourceUpdateBatch* updates);

  QRhi* rhi_cached_ = nullptr;  ///< The QRhi this viewer's resources belong to; null after releaseResources().
  // autoRenderTarget is off: nothing depth-tests, and QRhiWidget's depth-stencil would cost 4 B/px. The descriptor
  // outlives resizes, so the pipelines do too.
  PJ::RhiColorTarget color_target_;
  int max_texture_size_ = 0;
  int max_texture_size_for_test_ = 0;  ///< 0: none
  bool oversize_frame_logged_ = false;
  std::uint64_t frame_upload_count_ = 0;
  std::uint64_t label_rasterizations_ = 0;
  bool optional_failures_logged_ = false;  ///< per QRhi
#ifdef PJ_TARGET_WASM
  // Qt/WASM switches the top-level from raster to RHI composition when this
  // late-created widget submits its first frame. Re-armed by releaseResources().
  bool composition_refresh_queued_ = false;
#endif
  QRhiGraphicsPipeline* pipeline_ = nullptr;  ///< Null while the viewer is in its failed state.
  QRhiGraphicsPipeline* composite_pipeline_ = nullptr;
  RhiFailureLatch rendering_warning_;  ///< Failed while set; survives releaseResources().
  QRhiSampler* sampler_ = nullptr;
  QRhiSampler* mag_nearest_sampler_ = nullptr;
  QRhiSampler* remap_sampler_ = nullptr;
  // 1x1 RG32UI placeholder bound at SRB slot 4 for layers that don't rectify, so
  // every texture-layer pipeline shares one SRB layout.
  QRhiTexture* remap_placeholder_tex_ = nullptr;
  // Set once initialize() has built the base pipeline, which binds an RG32UI placeholder LUT.
  std::atomic_bool gpu_rectify_supported_{false};
  TextureLayerResources base_texture_;

  MediaSource* media_source_ = nullptr;

  // Guards media_source_, the pending frames and inspector_frame_; GUI thread only, it keeps a source swap and the
  // frame staging atomic. The last frame is kept after upload, so a new QRhi can restore it.
  std::mutex frame_mutex_;
  DecodedFrame pending_decoded_;
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

  // std140; must match the Uniforms block of shaders/yuv_to_rgb.{vert,frag} field for field:
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

  // Shared by the three overlay pipelines, so they follow the image's view transform.
  QRhiBuffer* marker_uniform_buf_ = nullptr;
  std::vector<SceneFrame> last_overlays_;
  bool overlays_dirty_ = false;
  // On-screen px per image px at the last stroke expansion; the 1 px floor depends on it, so a change re-expands.
  // 0 = never expanded.
  double last_overlay_scale_ = 0.0;

  // kPoints squares and line-loop and circle fills.
  OverlayPipeline points_overlay_;

  // Every stroke, as triangles: line primitives are 1 px wide and GL can cull whole segments (guard band) when zoomed
  // far in.
  OverlayPipeline thick_overlay_;

  // One quad per TextAnnotation over an R8 glyph mask, tinted by the vertex colour, so labels differing only in colour
  // share a texture.
  OverlayPipeline text_overlay_;  // its layout SRB binds the placeholder texture
  QRhiTexture* text_placeholder_tex_ = nullptr;
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
    QRhiShaderResourceBindings* srb = nullptr;  // per entry, so a draw cannot mix textures
    int width = 0;
    int height = 0;
    std::uint64_t last_used = 0;  ///< text_rebuilds_ when a rebuild last drew it
  };
  // Bounded, or a stream of ever-new strings (counters, timestamps) grows it forever: after each text rebuild the
  // least recently used labels that rebuild did not draw are evicted down to kMaxGlyphCacheEntries.
  std::unordered_map<TextKey, TextEntry, TextKeyHash> text_cache_;
  static constexpr std::size_t kMaxGlyphCacheEntries = 512;
  std::uint64_t text_rebuilds_ = 0;
  struct TextDrawItem {
    QRhiShaderResourceBindings* srb;  // pointer borrowed from text_cache_
    size_t vbo_offset_bytes;
  };
  std::vector<TextDrawItem> text_draw_items_;

  // std140. frameSize is a vec4 (only .xy used): a vec2 hit an std140 alignment quirk of the OpenGL backend.
  struct alignas(16) MarkerUbo {
    float view[16];
    float frame_size[4];
  };
  static constexpr int kMarkerUniformBufSize = sizeof(MarkerUbo);
};

}  // namespace PJ
