// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"

namespace pj::scene3d {

class SceneEntitiesLayer;
struct MarkerGeometry;

/// The SceneEntities marker family of the QRhi view: instanced unit primitives
/// (cube + its edge overlay, sphere, cylinder/cone, arrow/axes) and the
/// expanded line/triangle stream batches. Each admitted layer owns Static
/// instance and stream buffers (LayerGpu), rebuilt only when its LayerKey
/// changes: geometry revision, style, shadow casting, the render origin and the
/// resolved placement of every frame it references (a TF-only move bumps no
/// revision). Orbit, zoom and playback without new samples or TF motion reuse the
/// buffers; pan and follow move the render origin and rebuild (vertices stay
/// camera-relative, subtracted in double, for pixel parity). The layer's model primitives belong to ModelRhiPass. Per
/// frame the view calls admit() (once), prepare() (once), then submit() per ScenePass for each draw, and
/// submitShadowCasters() into its shadow pass. Solid cubes, spheres, cylinders and arrows of non-wireframe layers cast
/// shadows (the GL MarkerRenderPass casts none); axes, edge overlays, lines and triangle lists never do.
class MarkerRhiPass {
 public:
  /// kCubeEdges: the cube fill's instance range again, drawn as the twelve box
  /// edges by marker_edge.vert (facing-dependent colour, as on GL).
  enum class Kind : std::uint8_t { kInstanceTriangles, kInstanceLines, kCubeEdges, kStreamTriangles, kStreamLines };

  /// One prepared range of a layer: `offset`/`count` index `buffer`, the layer's
  /// instance buffer (instanced kinds, `mesh` selects the unit mesh) or one of
  /// its stream buffers.
  struct Draw {
    SceneEntitiesLayer* layer = nullptr;
    QRhiBuffer* buffer = nullptr;
    Kind kind = Kind::kInstanceTriangles;
    std::uint32_t mesh = 0;
    quint32 offset = 0;
    quint32 count = 0;
    bool cull_back = false;
    bool depth_write = true;
    /// Translucent when it skips depth writes; the edge overlay of translucent
    /// cubes writes depth but stays with its fill, drawn right after it.
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// Binds the setup's view uniforms (binding 0), data render mode (binding 8
  /// of the instanced primitives) and annotation render mode (binding 8 of the
  /// stream batches). False leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const;
  void registerPipelines(ScenePipelineRegistry& registry) const;
  /// Adopts the view's marker layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<SceneEntitiesLayer*>& layers);

  /// Runs each visible layer's prepareForRender(), places its frames, applies
  /// the per-layer and per-view instance/stream budgets (row warnings on
  /// rejection) and adds its fixed-frame bounds to `bounds`.
  void admit(const PlaceFn& place, SceneBoundsCollector& bounds);
  /// Rebuilds and uploads the admitted layers whose LayerKey changed (and the
  /// unit meshes once) and releases the GPU state of layers not admitted this
  /// frame. A layer whose buffers cannot be allocated has
  /// no draws this frame and retries the next. `cast_shadows` also collects the
  /// shadow-caster ranges and their bounds.
  void prepare(const RhiFrameContext& frame, bool cast_shadows = false);
  /// The prepared ranges of `layer` in native family order.
  [[nodiscard]] std::span<const Draw> drawsFor(const SceneEntitiesLayer* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one range; false when nothing could be recorded for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;
  // ---- Shadow casting (the shadow target is owned and begun by the view).
  [[nodiscard]] bool hasShadowCasters() const {
    return !casters_.empty();
  }
  /// This frame's caster bounds, relative to the prepare() render origin.
  [[nodiscard]] AABB shadowCasterBounds() const {
    return caster_bounds_;
  }
  /// Creates the depth-only caster pipeline for the view's shadow target, whose
  /// `bindings` (binding 0: light view-projection) it binds; true when it exists.
  bool ensureShadowPipeline(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, QRhiShaderResourceBindings* bindings);
  void releaseShadowResources();
  /// Records every caster range (instanced, from the colour instance buffer).
  void submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const;
  /// Notes `error` on every layer with shadow casters this frame.
  void noteShadowFailure(const QString& error);
  [[nodiscard]] int lastShadowDrawCount() const {
    return static_cast<int>(casters_.size());
  }
  [[nodiscard]] int lastShadowTriangleCount() const;

  void release();

  [[nodiscard]] int lastLayerCount() const {
    return last_layer_count_;
  }
  [[nodiscard]] int lastInstanceCount() const {
    return last_instance_count_;
  }
  [[nodiscard]] int lastStreamVertexCount() const {
    return last_stream_vertex_count_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }
#ifdef PJ_RHI_TEST_HOOKS
  /// Layer rebuilds (expansion + upload) since initialization; cumulative.
  [[nodiscard]] std::uint64_t layerRebuildCountForTest() const {
    return layer_rebuild_count_;
  }
  /// False rebuilds every admitted layer every frame (the uncached path, for
  /// pixel A/B tests).
  void setMarkerCacheEnabledForTest(bool enabled) {
    cache_enabled_ = enabled;
  }
  /// True fails every stream (line/triangle) buffer allocation of a rebuild,
  /// after its instance buffer was allocated: a partial allocation failure.
  void setFailStreamAllocationForTest(bool fail) {
    fail_stream_allocation_ = fail;
  }
#endif

  /// One instanced primitive: 96 bytes, the marker.vert per-instance layout.
  struct Instance {
    std::array<float, 16> model{};
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
    float bottom_scale = 1.0F;  ///< cone taper at local z = -0.5
    float top_scale = 1.0F;     ///< cone taper at local z = +0.5
    float unused0 = 0.0F;
    float unused1 = 0.0F;
  };
  /// One stream vertex (scene.vert layout): camera-relative position + colour.
  struct StreamVertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
  };

 private:
  /// Everything a layer's expansion depends on. The placements are the
  /// fixed-from-frame transforms of the layer's frames (nullopt: unplaced),
  /// compared exactly.
  struct LayerKey {
    std::uint64_t geometry_revision = 0;
    glm::dvec3 render_origin{0.0};
    float opacity = 1.0F;
    std::optional<glm::vec3> override_color;
    bool wireframe = false;
    bool cast_shadows = false;
    std::vector<std::optional<Transform>> placements;

    bool operator==(const LayerKey&) const = default;
  };
  struct Caster {
    SceneEntitiesLayer* layer = nullptr;
    QRhiBuffer* buffer = nullptr;
    std::uint32_t mesh = 0;
    quint32 offset = 0;
    quint32 count = 0;
  };
  /// One layer's expansion; also the reusable scratch of expandLayer().
  struct Expansion {
    std::vector<std::optional<glm::dmat4>> fixed_from_frame;  ///< the key's placements as matrices
    std::vector<Instance> instances;
    std::vector<StreamVertex> lines;
    std::vector<StreamVertex> triangles;
    std::vector<Draw> draws;  ///< buffer unset: offsets index the vectors above
    std::vector<Caster> casters;
    AABB caster_bounds;  ///< relative to the key's render origin
  };
  struct LayerGpu {
    std::optional<LayerKey> key;  ///< the key of the uploaded expansion; empty: none yet
    QRhiBuffer* instances = nullptr;
    QRhiBuffer* lines = nullptr;
    QRhiBuffer* triangles = nullptr;
    quint32 instance_capacity = 0;
    quint32 line_capacity = 0;
    quint32 triangle_capacity = 0;
    std::vector<Draw> draws;
    std::vector<Caster> casters;
    AABB caster_bounds;
    std::uint64_t admitted_frame = 0;  ///< the prepare() that last admitted the layer
  };

  /// Expands `geometry` as `key` describes it into `out` (cleared first): a pure
  /// function of its arguments, vertices relative to key.render_origin.
  static void expandLayer(const MarkerGeometry& geometry, const LayerKey& key, Expansion& out);
  /// Uploads scratch_ into `gpu`'s buffers and adopts its draws; false (and `gpu`
  /// released) when a buffer cannot be allocated. Every buffer is allocated
  /// before the first upload is queued, so a failure leaves nothing of `gpu` in
  /// `updates`.
  bool uploadLayer(QRhi* rhi, QRhiResourceUpdateBatch* updates, SceneEntitiesLayer* layer, LayerGpu& gpu);
  static void releaseLayerGpu(LayerGpu& gpu);

  struct MeshGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* triangle_index_buffer = nullptr;
    QRhiBuffer* edge_index_buffer = nullptr;
    quint32 triangle_index_count = 0;
    quint32 edge_index_count = 0;
  };
  struct Admitted {
    SceneEntitiesLayer* layer = nullptr;
    std::size_t first_frame = 0;  ///< the layer's frames in placements_
    std::size_t frame_count = 0;
    std::uint64_t instances = 0;
    std::uint64_t stream_vertices = 0;
  };
  QRhiGraphicsPipeline* instancePipeline(const Draw& draw) const;

  /// The placement of `frame_index` of `admitted`, or nullptr when unplaced.
  [[nodiscard]] const Transform* placement(const Admitted& admitted, std::uint32_t frame_index) const;

  QRhiShaderResourceBindings* data_bindings_ = nullptr;
  QRhiShaderResourceBindings* annotation_bindings_ = nullptr;
  QRhiShaderResourceBindings* edge_bindings_ = nullptr;  ///< data_bindings_ plus camera_view_uniforms_
  QRhiBuffer* camera_view_uniforms_ = nullptr;           ///< binding 4 of marker_edge.vert, written per frame
  /// The direct pipelines; each has a depth-writing and a no-depth-write variant.
  enum Pipeline : std::uint8_t {
    kTrianglePipeline,
    kTriangleCullPipeline,
    // The cube fill, depth-biased away from the camera (GL's glPolygonOffset(1, 1))
    // so its edges, drawn at the exact depth, win the depth test.
    kCubeFillPipeline,
    kEdgePipeline,
    kLinePipeline,
    kStreamLinePipeline,
    kStreamTrianglePipeline,
    kPipelineCount,
  };
  /// [pipeline][depth_write]
  std::array<std::array<QRhiGraphicsPipeline*, 2>, kPipelineCount> pipelines_{};
  std::array<MeshGpu, 5> meshes_{};
  bool mesh_upload_pending_ = false;

  std::vector<SceneEntitiesLayer*> layers_;
  std::unordered_map<SceneEntitiesLayer*, LayerGpu> layer_gpu_;
  std::vector<Admitted> admitted_;
  std::vector<std::optional<Transform>> placements_;  ///< every admitted layer's frames, contiguous per layer
  std::vector<Draw> draws_;                           ///< contiguous per layer, in admission order
  std::vector<Caster> casters_;
  AABB caster_bounds_;
  LayerKey scratch_key_;
  Expansion scratch_;
  bool cache_enabled_ = true;            // test hook
  bool fail_stream_allocation_ = false;  // test hook
  std::uint64_t frame_serial_ = 0;
  std::uint64_t layer_rebuild_count_ = 0;
  QRhiGraphicsPipeline* shadow_pipeline_ = nullptr;
  QRhiShaderResourceBindings* shadow_bindings_ = nullptr;  // view-owned

  int last_layer_count_ = 0;
  int last_instance_count_ = 0;
  int last_stream_vertex_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
