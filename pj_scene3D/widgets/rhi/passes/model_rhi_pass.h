// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/model_renderable.h"

namespace pj::scene3d {

/// The model family of the QRhi view: every ModelRenderable (RobotModel/URDF
/// visuals and collisions, SceneEntities model primitives) as indexed meshes
/// with PBR/Blinn materials and up to five textures, drawn one instance per
/// submesh. It owns the model pipelines, per-layer mesh/material/texture GPU
/// state, the colour and shadow-caster instance buffers and the shadow-caster
/// pipeline. The view owns the shadow map and EDL mask targets and replays this
/// pass's prepared draws into them (submitShadowCasters, submitMeshMask).
///
/// Caches, all valid for one QRhi (release() drops them):
///  - GPU meshes per layer, keyed by mesh key and MeshData identity: a
///    modelRevision() bump rebuilds only keys that are new or whose pointer changed;
///  - decoded textures per pass, keyed by TextureSource::key + colour space and
///    reference-counted by the meshes that bind them, shared across layers;
///  - wireframe edge lists, built on a mesh's first wireframe draw; the CPU copy
///    is dropped once queued for upload;
///  - the colour and shadow instances: rebuilt and re-uploaded only when the render
///    origin, the shading inputs or the admitted draws (layer, revision, look and
///    every draw's resolved TF placement) differ from the last prepared frame.
class ModelRhiPass {
 public:
  struct MeshGpu;
  struct MaterialGpu;

  /// One submesh draw of the colour pass (also replayed by the EDL mesh mask).
  struct Draw {
    ModelRenderable* layer = nullptr;
    MeshGpu* mesh = nullptr;
    MaterialGpu* material = nullptr;
    quint32 instance_offset = 0;
    quint32 index_offset = 0;  ///< in indices
    quint32 index_count = 0;
    bool lines = false;  ///< wireframe: the mesh's triangle-edge index list
    /// Translucent draws (collisions, alpha < 1, glTF BLEND) skip depth writes.
    DrawClass draw_class = DrawClass::kOpaque;
  };

  /// Binds the setup's view uniforms (binding 0) and render mode (binding 8)
  /// plus `lighting_uniforms` (binding 6: camera, lights, shadow parameters),
  /// all view-owned and outliving release().
  [[nodiscard]] bool initialize(const RhiPassSetup& setup, QRhiBuffer* lighting_uniforms);
  [[nodiscard]] bool ready() const;
  void registerPipelines(ScenePipelineRegistry& registry) const;
  /// Adopts the view's model layers; drops GPU state of layers that left or
  /// are hidden.
  void setLayers(const std::vector<ModelRenderable*>& layers);

  /// Places every draw call, applies the per-layer and per-view draw/triangle
  /// budgets (colour and, when `shadows_requested`, shadow casting: hidden
  /// robot visuals still cast, collisions never do), notes row warnings and
  /// adds the bounds of layers that contribute to the scene bounds.
  void admit(
      const PlaceFn& place, const MeshShadingParams& shading, bool shadows_requested, SceneBoundsCollector& bounds);
  /// Creates or refreshes GPU meshes/materials of admitted layers, builds this
  /// frame's colour draws, shadow-caster draws and caster bounds (relative to
  /// the render origin) and uploads the colour instances and pending
  /// mesh/texture data. When the colour instance buffer cannot grow the frame
  /// has no colour draws.
  void prepare(const RhiFrameContext& frame, const MeshShadingParams& shading);
  /// The prepared colour draws of `layer`.
  [[nodiscard]] std::span<const Draw> drawsFor(const ModelRenderable* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one colour draw; false when nothing could be recorded for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;

  // ---- Shadow casting and EDL mask (targets owned and orchestrated by the view).
  [[nodiscard]] bool hasShadowCasters() const {
    return !shadow_draws_.empty();
  }
  /// This frame's caster bounds, relative to the prepare() render origin.
  [[nodiscard]] AABB shadowCasterBounds() const {
    return shadow_bounds_;
  }
  /// Creates the depth-only caster pipeline for the view's shadow target, whose
  /// `bindings` (binding 0: light view-projection) it binds; true when it exists.
  bool ensureShadowPipeline(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, QRhiShaderResourceBindings* bindings);
  /// Grows the caster instance buffer and uploads this frame's casters; false
  /// when it cannot grow.
  bool uploadShadowCasters(const RhiFrameContext& frame);
  /// Releases the caster pipeline and instance buffer (the shadow chain went down).
  void releaseShadowResources();
  /// Records every caster draw into the view's already-begun shadow pass.
  void submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const;
  /// Rebinds every material to the view's shadow map (binding 7); false leaves
  /// the previous bindings in place.
  bool setShadowMap(QRhi* rhi, QRhiTexture* texture, QRhiSampler* sampler);
  /// The shadow map is going away: rebind materials to the white texture, or,
  /// when `restore_bindings` is false or that fails, drop every binding (a
  /// failed rebind also forces each layer to rebuild next frame).
  void clearShadowMap(QRhi* rhi, bool restore_bindings);
  /// Notes `error` on every layer with shadow casters this frame.
  void noteShadowFailure(const QString& error);
  /// Records the mask draws (every colour draw) with the view's mask pipelines.
  void submitMeshMask(
      QRhiCommandBuffer* command_buffer, QRhiGraphicsPipeline* triangle_pipeline, QRhiGraphicsPipeline* line_pipeline,
      const QRhiViewport& viewport) const;
  /// Source state for the view's EDL mask pipelines.
  [[nodiscard]] QRhiGraphicsPipeline* trianglePipeline() const {
    return triangle_pipeline_;
  }
  [[nodiscard]] QRhiGraphicsPipeline* linePipeline() const {
    return line_pipeline_;
  }
  [[nodiscard]] QRhiShaderResourceBindings* layoutBindings() const {
    return layout_bindings_;
  }

  void release();

  [[nodiscard]] int lastLayerCount() const {
    return last_layer_count_;
  }
  [[nodiscard]] int lastDrawCount() const {
    return last_draw_count_;
  }
  [[nodiscard]] int lastTriangleCount() const {
    return last_triangle_count_;
  }
  [[nodiscard]] std::array<int, 5> lastTextureSlotCounts() const {
    return last_texture_slot_counts_;
  }
  [[nodiscard]] AABB lastBounds() const {
    return last_bounds_;
  }
  [[nodiscard]] int lastShadowDrawCount() const {
    return static_cast<int>(shadow_draws_.size());
  }
  [[nodiscard]] int lastShadowTriangleCount() const {
    return last_shadow_triangle_count_;
  }

#ifdef PJ_RHI_TEST_HOOKS
  /// False rebuilds every mesh of a layer on each modelRevision() bump and the
  /// instances every frame (the textures stay shared).
  void setCachesEnabledForTest(bool enabled) {
    caches_enabled_ = enabled;
  }
#endif
  /// GPU meshes built, textures decoded, wireframe edge lists built, and colour /
  /// shadow instance uploads queued, over the pass lifetime.
  struct CacheCounters {
    int mesh_builds = 0;
    int texture_decodes = 0;
    int edge_builds = 0;
    int instance_uploads = 0;
    int shadow_instance_uploads = 0;
  };
#ifdef PJ_RHI_TEST_HOOKS
  [[nodiscard]] const CacheCounters& cacheCountersForTest() const {
    return counters_;
  }
  /// Live entries of the pass texture cache.
  [[nodiscard]] int cachedTextureCountForTest() const {
    return static_cast<int>(textures_.size());
  }
#endif

  /// Per-submesh instance data (144 bytes, the model.vert per-instance layout).
  struct Instance {
    std::array<float, 16> model{};
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> params{};
    std::array<float, 4> texture_flags{};
    std::array<float, 4> pbr_factors{};
    std::array<float, 4> emissive_factor{};
  };
  struct ShadowInstance {
    std::array<float, 16> model{};
  };

  struct TextureGpu {
    QRhiTexture* texture = nullptr;  ///< the white texture or a textures_ entry's
    bool present = false;
  };
  struct MaterialGpu {
    std::array<TextureGpu, 5> textures{};
    QRhiShaderResourceBindings* shader_resources = nullptr;
  };
  /// A texture cache key: TextureSource::key (a content hash or resolved path)
  /// and whether it samples as sRGB.
  using TextureKey = std::pair<std::string, bool>;
  struct MeshGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* triangle_index_buffer = nullptr;
    QRhiBuffer* edge_index_buffer = nullptr;  ///< null until the first wireframe draw
    std::vector<quint32> edge_indices;        ///< held only until queued for upload
    std::vector<quint32> edge_offsets;
    std::vector<quint32> edge_counts;
    std::vector<MaterialGpu> materials;
    std::vector<TextureKey> texture_refs;  ///< one textures_ reference per distinct key
    std::uint64_t texture_pixels = 0;      ///< charged to the layer texture budget
    std::shared_ptr<const MeshData> source;
    bool upload_pending = false;
    bool edges_built = false;
    bool edge_upload_pending = false;
  };

 private:
  struct LayerGpu {
    std::unordered_map<std::string, MeshGpu> meshes;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };
  /// One admitted draw call. The layer's revision pins what `draw_index` names
  /// (mesh, colour, group), so the index and the placement are the whole key.
  struct Call {
    std::uint32_t draw_index = 0;      ///< into the layer's modelDrawCalls()
    glm::dmat4 fixed_from_model{1.0};  ///< the resolved TF placement times the draw's model
    bool color = false;                ///< drawn in the colour pass
    bool shadow = false;               ///< cast into the shadow map
    bool operator==(const Call&) const = default;
  };
  /// One admitted layer: equal Admitted lists (with the same render origin and
  /// shading) produce the same instances, which is what the instance skip keys on.
  struct Admitted {
    ModelRenderable* layer = nullptr;
    std::uint64_t revision = 0;
    float opacity = 1.0F;
    bool color_override = false;
    QColor override_color;
    bool wireframe = false;
    std::vector<Call> calls;
    bool casts = false;  ///< some call casts a shadow
    std::uint64_t draws = 0;
    std::uint64_t triangles = 0;
    std::uint64_t shadow_draws = 0;
    std::uint64_t shadow_triangles = 0;
    bool operator==(const Admitted&) const = default;
  };
  struct CachedTexture {
    QRhiTexture* texture = nullptr;
    QImage pending_image;  ///< decoded, not yet queued for upload
    std::uint64_t pixels = 0;
    int references = 0;
  };
  struct ShadowDraw {
    MeshGpu* mesh = nullptr;
    quint32 instance_offset = 0;
    quint32 index_count = 0;
  };

  /// Brings `gpu` to the layer's modelRevision(); false when the layer cannot draw
  /// this frame. Sets `changed` when it built or dropped anything.
  bool ensureLayerGpu(QRhi* rhi, ModelRenderable* layer, LayerGpu& gpu, bool& changed);
  /// Builds the GPU mesh of `source`: buffers, and materials whose textures come
  /// from (or are decoded into) textures_. `remaining_layer_pixels` is the layer
  /// texture budget left; the mesh's charge is taken from it on success. Null
  /// `error` on success.
  bool buildMesh(
      QRhi* rhi, const std::shared_ptr<const MeshData>& source, std::uint64_t& remaining_layer_pixels, MeshGpu& mesh,
      const char*& error);
  /// Builds `mesh`'s triangle-edge index list and buffer on its first wireframe
  /// draw; false when the buffer cannot be allocated.
  bool ensureEdges(QRhi* rhi, MeshGpu& mesh);
  /// Rebuilds this frame's colour and shadow draws and instances from admitted_.
  void buildInstances(QRhi* rhi, const glm::dvec3& render_origin, const MeshShadingParams& shading);
  void releaseTexture(const TextureKey& key);
  QRhiShaderResourceBindings* createMaterialBindings(QRhi* rhi, const MaterialGpu& material) const;
  bool rebuildMaterialBindings(QRhi* rhi, MaterialGpu& material) const;
  bool rebuildAllMaterialBindings(QRhi* rhi);
  void dropMaterialBindings(bool invalidate_layers);
  /// Frees `gpu`'s resources and drops it from pending_uploads_.
  void releaseMeshGpu(MeshGpu& gpu);
  void releaseLayerGpu(LayerGpu& gpu);

  QRhiBuffer* view_uniforms_ = nullptr;         // view-owned
  QRhiBuffer* lighting_uniforms_ = nullptr;     // view-owned
  QRhiBuffer* render_mode_uniforms_ = nullptr;  // view-owned
  QRhiTexture* shadow_texture_ = nullptr;       // view-owned; nullptr: white
  QRhiSampler* shadow_sampler_ = nullptr;       // view-owned
  QRhiTexture* white_texture_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  QRhiShaderResourceBindings* layout_bindings_ = nullptr;
  QRhiGraphicsPipeline* triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* line_no_depth_pipeline_ = nullptr;
  QRhiBuffer* instance_buffer_ = nullptr;
  quint32 instance_capacity_ = 0;
  bool white_upload_pending_ = false;
  QRhiGraphicsPipeline* shadow_pipeline_ = nullptr;
  QRhiShaderResourceBindings* shadow_bindings_ = nullptr;  // view-owned
  QRhiBuffer* shadow_instance_buffer_ = nullptr;
  quint32 shadow_instance_capacity_ = 0;

  std::vector<ModelRenderable*> layers_;
  std::unordered_map<ModelRenderable*, LayerGpu> layer_gpu_;
  /// Vertex bounds per immutable mesh, computed once. Holding the key keeps a
  /// released mesh alive only until admit() prunes entries nobody else holds.
  std::unordered_map<std::shared_ptr<const MeshData>, AABB> bounds_cache_;
  /// Meshes (in layer_gpu_) with buffer uploads still to queue;
  /// releaseMeshGpu() removes a mesh before it is freed.
  std::vector<MeshGpu*> pending_uploads_;
  std::map<TextureKey, CachedTexture> textures_;
  /// textures_ entries with a decoded image still to queue.
  std::vector<CachedTexture*> pending_textures_;
  std::vector<Admitted> admitted_;
  /// The admitted_ that the current instances were built from, and its origin and
  /// shading inputs. admit() rotates it in only after a prepare(), so an admission
  /// that never reached prepare() cannot stand in for the uploaded data.
  std::vector<Admitted> prepared_admitted_;
  bool admitted_prepared_ = false;
  glm::dvec3 prepared_origin_{0.0};
  std::array<float, 3> prepared_shading_{};
  bool instances_valid_ = false;
  bool shadow_upload_pending_ = false;
  bool caches_enabled_ = true;  // test hook
  CacheCounters counters_;      // test counters
  std::vector<Draw> draws_;     ///< contiguous per layer, in admission order
  std::vector<Instance> instances_;
  std::vector<ShadowInstance> shadow_instances_;
  std::vector<ShadowDraw> shadow_draws_;
  AABB shadow_bounds_;

  int last_layer_count_ = 0;
  int last_draw_count_ = 0;
  int last_triangle_count_ = 0;
  int last_shadow_triangle_count_ = 0;
  std::array<int, 5> last_texture_slot_counts_{};
  AABB last_bounds_;
};

}  // namespace pj::scene3d
