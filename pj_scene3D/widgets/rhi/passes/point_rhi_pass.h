// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_core/pointcloud.h"  // PointVertexFormat

namespace pj::scene3d {

class PointRenderable;

/// The point family of the QRhi view: every PointRenderable (point clouds and
/// depth clouds) as points, sphere imposters or cubes. Points, spheres and the
/// cube LOD sprites are instanced camera-facing quads, which every backend can
/// draw (D3D has no gl_PointSize) and which keep their visible part at the
/// frustum edge; where the backend has QRhi::VertexShaderPointSize, clouds whose
/// sprites stay small draw as a point list instead, about twice as fast (the
/// measurements are in docs/TECHNICAL_NOTES.md). Cubes are the hexagon fan (cube_mesh.h). A cloud with faded
/// out-of-range points draws twice: its in-range points opaque, then the faded
/// ones translucent with depth writes off, so they tint without occluding.
/// Bounds come from the layer's conversion (PointRenderable::sourceBounds()),
/// which every non-empty cloud has.
/// Per frame the view calls admit() (once), prepare() (once), then submit() per
/// ScenePass for each draw.
class PointRhiPass {
 public:
  /// How a cloud's instances are drawn: camera-facing quads, the cube hexagon
  /// fan, or (small sprites) a point list.
  enum class Geometry : std::uint8_t { kQuad, kFan, kPointList };

  /// The instance buffer layout: 16-byte records (xyz + one 32-bit attribute:
  /// kXyzScalar, kXyzRgba) or 12-byte xyz (kXyz).
  enum class Layout : std::uint8_t { kAttribute, kXyz };

  struct Draw {
    PointRenderable* layer = nullptr;
    QRhiBuffer* instances = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_count = 0;
    Geometry geometry = Geometry::kQuad;
    Layout layout = Layout::kAttribute;
    /// kOpaque: every point, or the in-range half; kTranslucent: the faded half.
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// What prepare() needs beyond RhiFrameContext, all for this frame.
  struct ViewInputs {
    glm::mat4 projection{1.0F};  ///< GL clip convention (uncorrected)
    QSize device_size;           ///< render target, device pixels
    float device_pixel_ratio = 1.0F;
    /// Render-target pixels per output pixel (the HDR chain's render_scale):
    /// pixel sizes are output pixels and scale with it, so supersampling keeps
    /// what the user sees.
    float pixel_scale = 1.0F;
  };

  /// Binds the view's resources; registers no pipeline: each layout and
  /// geometry pair is registered with the setup's registry on first use
  /// (prepare()), so a new QRhi pays only for the ones its clouds draw with. A
  /// point list that cannot be created falls back to quads; a quad or fan pair
  /// that cannot be registered drops its clouds with a warning.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const {
    return layout_shader_resources_ != nullptr;
  }
  /// Lets clouds whose sprites are at most kPointListMaxPx draw as a point list
  /// (gl_PointSize), where the backend has QRhi::VertexShaderPointSize. On by
  /// default; the benchmark turns it off to time the quads alone.
  void setPointListEnabled(bool enabled) {
    point_list_enabled_ = enabled;
  }
  [[nodiscard]] bool pointListActive() const {
    return point_list_enabled_ && point_size_supported_;
  }
  /// Adopts the view's point layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<PointRenderable*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Runs each visible layer's prepareForRender() against the per-view vertex
  /// budget, places it, and records the families' fixed-frame bounds
  /// (lastPointBounds(), lastDepthBounds()).
  void admit(const PlaceFn& place);
  /// Uploads admitted clouds and uniforms and builds this frame's draws.
  void prepare(const RhiFrameContext& frame, const ViewInputs& view);
  /// The prepared draws of `layer` (none, one, or opaque then faded).
  [[nodiscard]] std::span<const Draw> drawsFor(const PointRenderable* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one draw; false when its pipeline is unavailable for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  void release();

  [[nodiscard]] int lastPointLayerCount() const {
    return last_point_layer_count_;
  }
  [[nodiscard]] int lastPointVertexCount() const {
    return last_point_vertex_count_;
  }
  [[nodiscard]] int lastDepthLayerCount() const {
    return last_depth_layer_count_;
  }
  [[nodiscard]] int lastDepthVertexCount() const {
    return last_depth_vertex_count_;
  }
  [[nodiscard]] int lastCubeLayerCount() const {
    return last_cube_layer_count_;
  }
  [[nodiscard]] int lastCubeInstanceCount() const {
    return last_cube_instance_count_;
  }
  /// Cube layers drawn as LOD sprites last frame (every cube sub-pixel).
  [[nodiscard]] int lastCubeSpriteLayerCount() const {
    return last_cube_sprite_layer_count_;
  }
  /// Layers drawn as a point list last frame.
  [[nodiscard]] int lastPointListLayerCount() const {
    return last_point_list_layer_count_;
  }
  [[nodiscard]] AABB lastPointBounds() const {
    return last_point_bounds_;
  }
  [[nodiscard]] AABB lastDepthBounds() const {
    return last_depth_bounds_;
  }
#ifdef PJ_RHI_TEST_HOOKS
  /// Cloud uploads since construction (a new sample, or a rebuild on a new QRhi) and their bytes.
  [[nodiscard]] std::uint64_t uploadCountForTest() const {
    return upload_count_;
  }
  [[nodiscard]] std::uint64_t uploadedBytesForTest() const {
    return uploaded_bytes_;
  }
#endif

 private:
  struct LayerGpu {
    QRhiBuffer* instances = nullptr;
    quint32 capacity = 0;
    quint32 count = 0;
    PointVertexFormat format = PointVertexFormat::kXyz;  ///< of the uploaded records
    /// The layer re-uploaded into this buffer: a stream keeps a Static buffer (its
    /// staging copy is reused), a cloud uploaded once an Immutable one (staging freed).
    bool streamed = false;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
    QRhiBuffer* uniforms = nullptr;  ///< two slots: [0] all / in-range, [1] faded
    std::array<QRhiShaderResourceBindings*, 2> shader_resources{};
  };

  /// Registers `layout`'s opaque pipeline for `geometry`, and its faded one with
  /// `faded`, unless registered; false when one could not be (not retried until
  /// the next initialize()).
  bool ensurePipelines(Layout layout, Geometry geometry, bool faded);
  /// A registered point-list pipeline of `layout` could not be built on this GPU.
  [[nodiscard]] bool pointListFailed(Layout layout, bool faded) const;
  /// An SRB over `uniforms` bytes [offset, offset + kPointUniformBytes) and the render mode.
  QRhiShaderResourceBindings* createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms, quint32 offset) const;
  bool ensureLayerGpu(QRhi* rhi, LayerGpu& gpu);
  bool uploadInstances(QRhi* rhi, QRhiResourceUpdateBatch* updates, PointRenderable* layer, LayerGpu& gpu);
  static void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* render_mode_uniforms_ = nullptr;   // view-owned
  const RhiSharedGeometry* geometry_ = nullptr;  // view-owned (the fan indices); may be null without fan draws
  quint32 uniform_stride_ = 0;
  QRhiBuffer* layout_uniforms_ = nullptr;
  QRhiShaderResourceBindings* layout_shader_resources_ = nullptr;
  /// [Layout][Geometry][faded]: faded pipelines skip depth writes. The point-list
  /// pairs exist only with QRhi::VertexShaderPointSize.
  std::array<std::array<std::array<const ScenePipeline*, 2>, 3>, 2> pipelines_{};
  /// The view's registry the pairs register with on first use, and their part.
  ScenePipelineRegistry* registry_ = nullptr;
  const char* part_ = nullptr;
  bool point_size_supported_ = false;
  /// [Layout][Geometry][faded]: creation was attempted; a failed one is not retried per frame.
  std::array<std::array<std::array<bool, 2>, 3>, 2> pipelines_attempted_{};
  bool pipeline_warned_ = false;
  bool point_list_enabled_ = true;

  std::vector<PointRenderable*> layers_;
  std::unordered_map<PointRenderable*, LayerGpu> layer_gpu_;
  std::vector<PlacedLayer<PointRenderable>> admitted_;
  std::vector<Draw> draws_;  ///< contiguous per layer, in admission order

  int last_point_layer_count_ = 0;
  int last_point_vertex_count_ = 0;
  int last_depth_layer_count_ = 0;
  int last_depth_vertex_count_ = 0;
  int last_cube_layer_count_ = 0;
  int last_cube_instance_count_ = 0;
  int last_cube_sprite_layer_count_ = 0;
  int last_point_list_layer_count_ = 0;
  AABB last_point_bounds_;
  AABB last_depth_bounds_;
#ifdef PJ_RHI_TEST_HOOKS
  std::uint64_t upload_count_ = 0;
  std::uint64_t uploaded_bytes_ = 0;
#endif
};

}  // namespace pj::scene3d
