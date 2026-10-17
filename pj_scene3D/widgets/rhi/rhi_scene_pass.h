// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Shared vocabulary of the QRhi view's render passes: the scene-pass modes, the
// opaque/translucent draw classes, the pipeline registry every family registers
// its direct pipelines into, the geometry the view shares between families, and
// the helpers every family builds its pipelines, bindings and draws with.
//
// Frame contract (SceneViewWidget::render()): each frame first ADMITS every
// layer once (prepareForRender, fixed-frame placement, per-view budgets, scene
// bounds), then PREPARES once (uploads, uniforms, the classified draw list). Every
// scene-pass mode below, the EDL mesh mask and the shadow pass replay those
// prepared draws; none of them re-admits a layer or consumes a budget again.
//
// Every scene family exposes the same shape: initialize(RhiPassSetup) (which
// registers its pipelines), setLayers(layers), admit(place, bounds),
// prepare(RhiFrameContext, ...), drawsFor(layer) (a span of Draws, each with a
// draw_class), submit(command_buffer, draw, ScenePass, registry, viewport) and
// release().

#include <rhi/qrhi.h>

#include <QCoreApplication>
#include <QMatrix4x4>
#include <QString>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/rhi_conventions.h"
#include "pj_tf/transform.h"
#include "pj_widgets/GraphicsApi.h"
#include "pj_widgets/RhiBlend.h"

namespace pj::scene3d {

using pj::tf::Transform;

struct ArrowMeshData;
struct PlacementHold;

/// The scene passes that replay the prepared draw list with different targets.
enum class ScenePass {
  kDirect,       ///< straight into the widget's render target (no HDR chain)
  kHdrColor,     ///< the multisample RGBA16F HDR target
  kDepthReplay,  ///< single-sample RGBA8 coverage + D32F depth (SSAO/EDL input)
};

/// A prepared draw's blending class. Submission draws every opaque draw (in
/// logical/dock order), then every translucent one (again in logical order): a
/// translucent surface blends over all opaque geometry behind it. Not
/// order-independent transparency: translucent draws are not depth-sorted.
enum class DrawClass : std::uint8_t { kOpaque, kTranslucent };

/// The state of one pipeline; the defaults are the scene's (data blend, depth
/// test and write, LessOrEqual, no culling). A scene pipeline's description is
/// kept (ScenePipelineRegistry), and every variant of it is built from it, so a
/// state set here reaches the direct pipeline and its HDR and depth-replay variants alike.
struct PipelineDesc {
  QRhiGraphicsPipeline::Topology topology = QRhiGraphicsPipeline::Triangles;
  QShader vertex{};
  QShader fragment{};
  QRhiVertexInputLayout input_layout{};
  QRhiShaderResourceBindings* bindings = nullptr;                                     ///< the layout SRB
  std::optional<QRhiGraphicsPipeline::TargetBlend> blend = PJ::straightAlphaBlend();  ///< nullopt: no colour target
  QRhiGraphicsPipeline::CullMode cull = QRhiGraphicsPipeline::None;
  bool depth_test = true;
  bool depth_write = true;
  QRhiGraphicsPipeline::CompareOp depth_op = QRhiGraphicsPipeline::LessOrEqual;
  int depth_bias = 0;
  float slope_scaled_depth_bias = 0.0F;
  QRhiGraphicsPipeline::Flags flags{};
};

/// `desc` built and created for `render_pass` with `sample_count` samples: the
/// one route every graphics pipeline of the view takes (the scene pipelines and
/// their variants, the quality stages', the overlays'). nullptr for an
/// incomplete `desc` or when it cannot be created. So the shader-named fault
/// injection (rhiTestShaderFault(), pj_widgets/RhiTestHooks.h; e.g.
/// PJ_RHI_TEST_FAIL_SHADER=trail.frag.qsb) fails any of them.
std::unique_ptr<QRhiGraphicsPipeline> createPipeline(
    QRhi* rhi, const PipelineDesc& desc, QRhiRenderPassDescriptor* render_pass, int sample_count);

/// A scene pipeline that could not be created, and how far that reaches. An
/// optional pipeline (ScenePipelineRegistry::add(..., optional)) is no failure:
/// its family draws the same thing another way.
struct PipelineFailure {
  enum class Severity : std::uint8_t {
    kRequired,    ///< a built family's own pipeline: the view's failure state, named `part`
    kHdrVariant,  ///< an HDR-colour or depth-replay variant: the HDR chain drops, the scene draws directly
  };
  Severity severity = Severity::kRequired;
  const char* part = nullptr;  ///< the family that registered it
};

/// One registered scene pipeline: its description and the variants built from
/// it. Owned by ScenePipelineRegistry; families keep the handle and resolve() it.
struct ScenePipeline;

/// The one owner of the scene pipelines (every family's, the grid's and the TF
/// triads'): it keeps each description and builds its variants from it on
/// demand, the direct one for the widget's target and the HDR-colour and
/// depth-replay ones for the HDR chain's. Nothing is built at registration
/// (add()): a GPU running the HDR chain builds only the variants its frames
/// bind, and one without it builds the direct ones before its first frame
/// (createDirects()). Families hold handles, never pipelines.
class ScenePipelineRegistry {
 public:
  ScenePipelineRegistry();
  ~ScenePipelineRegistry();
  ScenePipelineRegistry(const ScenePipelineRegistry&) = delete;
  ScenePipelineRegistry& operator=(const ScenePipelineRegistry&) = delete;

  /// The target the direct variants draw into (the widget's render target),
  /// until clear(): createDirects() builds for it (add() builds nothing). The view's other pipelines on that target
  /// (the overlays) read it back (directRenderPass(), directSampleCount()).
  void setDirectTarget(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, int sample_count);
  [[nodiscard]] QRhiRenderPassDescriptor* directRenderPass() const {
    return direct_;
  }
  [[nodiscard]] int directSampleCount() const {
    return direct_samples_;
  }
  /// Registers `desc` as a pipeline of `part` and builds nothing: its direct
  /// variant is created by createDirects(), its HDR variants when a frame first
  /// binds them. nullptr only for an incomplete `desc` (a shader pack that could
  /// not be read, no layout SRB). An `optional` pipeline's failure latches
  /// nothing: it only marks the pipeline optionalFailed(), whose family then
  /// draws the same thing another way (the point list as quads, the grid's
  /// cells unshadowed).
  const ScenePipeline* add(const PipelineDesc& desc, const char* part, bool optional = false);
  /// True once a variant of the optional `pipeline` could not be created; it
  /// resolves to nullptr from then on (nullptr ignored).
  [[nodiscard]] bool optionalFailed(const ScenePipeline* pipeline) const;
  /// Deletes `pipeline` and its variants (nullptr ignored).
  void remove(const ScenePipeline* pipeline);
  /// Creates every direct variant not created yet: what a frame without the HDR
  /// chain does before recording, so no pipeline fails half way through its
  /// passes. False, and nothing retried, once a required one could not be
  /// created (failure() is then kRequired and names its part).
  bool createDirects();
  /// The failure latched since the registrations (kRequired, until clear()) or
  /// since the variants were last released (kHdrVariant); nullopt: none.
  [[nodiscard]] const std::optional<PipelineFailure>& failure() const {
    return failure_;
  }
  /// The render passes the HDR variants target, on the direct target's QRhi:
  /// HDR colour (`hdr`, `hdr_samples`) and depth replay (`depth`, one sample,
  /// alpha-only writes). New targets drop every variant first.
  void setVariantTargets(QRhiRenderPassDescriptor* hdr, int hdr_samples, QRhiRenderPassDescriptor* depth);
  /// Deletes every HDR variant and forgets their targets; keeps the
  /// registrations and the direct variants (the HDR chain went down).
  void releaseVariants();
  /// Deletes every pipeline and forgets every registration and target (the
  /// resources they bind are going away).
  void clear();
  /// The pipeline `pass` binds for `pipeline`: its direct variant for kDirect
  /// (once created), else its HDR variant, created on this first request.
  /// nullptr means "do not draw": always for kDepthReplay when the description
  /// writes no depth (the depth replay records only depth-writing coverage), and
  /// when not created yet, without targets, or when the variant cannot be
  /// created (failure() is then kHdrVariant, unless the pipeline is optional:
  /// see add()). While a failure is latched no HDR
  /// variant is created or returned (that frame is withheld anyway): a
  /// kHdrVariant latch clears in releaseVariants(), a kRequired one in clear().
  [[nodiscard]] QRhiGraphicsPipeline* resolve(const ScenePipeline* pipeline, ScenePass pass) const;
  /// Every created direct variant satisfies pipelineBuiltFor(render_pass, sample_count).
  [[nodiscard]] bool directsBuiltFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const;

  /// Registrations and created variants (test and benchmark instrumentation).
  struct Counts {
    int registered = 0;
    int directs = 0;
    int hdr_variants = 0;  ///< HDR-colour and depth-replay
    bool operator==(const Counts&) const = default;
  };
  [[nodiscard]] Counts counts() const;

 private:
  [[nodiscard]] std::unique_ptr<QRhiGraphicsPipeline> createVariant(
      const ScenePipeline& pipeline, ScenePass pass) const;

  std::vector<std::unique_ptr<ScenePipeline>> pipelines_;
  // resolve() fills the variant cache and may latch a variant failure, hence mutable.
  mutable std::optional<PipelineFailure> failure_;
  QRhi* rhi_ = nullptr;
  QRhiRenderPassDescriptor* direct_ = nullptr;
  int direct_samples_ = 1;
  QRhiRenderPassDescriptor* hdr_ = nullptr;  ///< nullptr: no variant targets
  int hdr_samples_ = 1;
  QRhiRenderPassDescriptor* depth_ = nullptr;
};

/// Unit meshes shared by several families, owned by the view (created in
/// initialize(), released after every pipeline that draws them) and handed to
/// the families that draw them.
struct RhiSharedGeometry {
  QRhiBuffer* cube_vertices = nullptr;     ///< kCubeVertices: voxels
  QRhiBuffer* cube_indices = nullptr;      ///< kCubeIndices, uint16
  QRhiBuffer* cube_fan_indices = nullptr;  ///< kCubeFanIndices, uint16: point cubes
  QRhiBuffer* arrow_vertices = nullptr;    ///< unit pose arrow: pose triads, TF triads
  QRhiBuffer* arrow_indices = nullptr;     ///< uint32
  quint32 arrow_index_count = 0;

  /// Creates the meshes and records their contents into `updates`, which must
  /// be submitted before they are drawn.
  bool create(QRhi* rhi, QRhiResourceUpdateBatch* updates);
  void release();
};

/// Places a source frame in the fixed frame (placeObject()): a stamped object at its
/// own stamp, else at the frame's render time (the cursor), with the layer's
/// placement hold (nullable); nullopt when it cannot be placed this frame (the
/// family then skips the layer without consuming its budget).
using PlaceFn = std::function<std::optional<Transform>(
    const std::string& source_frame, std::optional<PJ::Timepoint> object_stamp, PlacementHold* hold)>;

/// An admitted layer and its placement, the record admit() hands prepare().
template <typename Layer>
struct PlacedLayer {
  Layer* layer = nullptr;
  Transform fixed_from_source;
};

/// The fixed-frame AABB of `source` (a source-frame box) placed by
/// `fixed_from_source`, minus `origin` (subtracted in double, before the float
/// cast): the box around its eight transformed corners, skipping non-finite ones.
AABB transformedBounds(
    const AABB& source, const Transform& fixed_from_source, const glm::dvec3& origin = glm::dvec3(0.0));
/// transformedBounds() for a general (possibly scaled) fixed-from-source matrix.
AABB transformedBounds(
    const AABB& source, const glm::dmat4& fixed_from_source, const glm::dvec3& origin = glm::dvec3(0.0));

/// `model` made camera-relative: `origin` is subtracted from its translation in
/// double, before the float cast, so far-away geometry keeps its precision.
inline glm::mat4 relativeModel(glm::dmat4 model, const glm::dvec3& origin) {
  model[3].x -= origin.x;
  model[3].y -= origin.y;
  model[3].z -= origin.z;
  return glm::mat4(model);
}

/// Sizes `buffer` (a `type` vertex buffer, Static or Immutable, filled by
/// uploadStaticBuffer()) for `required_bytes` with the point pass's rule:
/// reallocates to grow, or to give back a buffer more than twice
/// max(required_bytes, minimum_capacity / 2), to max(required_bytes,
/// minimum_capacity), and when its type differs; keeps it otherwise. A reallocated
/// buffer holds nothing. Zero bytes keeps any buffer. False, and `buffer` null,
/// when it cannot be allocated.
bool ensureStaticVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity,
    QRhiBuffer::Type type = QRhiBuffer::Static);

/// Grows `buffer` (a Dynamic vertex buffer) to hold `required_bytes`, doubling
/// from max(capacity, minimum_capacity); keeps it when already large enough.
/// Recreating drops the old contents. False when it cannot be allocated.
bool ensureVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity);
/// Creates `buffer` as a `bytes`-sized Dynamic uniform buffer when it is null;
/// false (and `buffer` null) when it cannot be created.
bool ensureUniformBuffer(QRhi* owner, QRhiBuffer*& buffer, quint32 bytes);
/// Creates `vertices` and `indices` as Immutable buffers holding `mesh` and,
/// once both exist, records their contents into `updates`. False, with nothing
/// recorded, when either cannot be created (the caller's release() deletes what was).
bool createStaticMesh(
    QRhi* rhi, QRhiResourceUpdateBatch* updates, const ArrowMeshData& mesh, QRhiBuffer*& vertices,
    QRhiBuffer*& indices);
/// A created SRB holding `bindings`, or nullptr when it cannot be created.
QRhiShaderResourceBindings* createBindings(QRhi* owner, std::initializer_list<QRhiShaderResourceBinding> bindings);

/// What a family's initialize() binds from the view; valid until the view's
/// releaseResources().
struct RhiPassSetup {
  QRhi* rhi = nullptr;
  QRhiBuffer* render_mode_uniforms = nullptr;             ///< binding 8 of every data-graded draw
  QRhiBuffer* annotation_render_mode_uniforms = nullptr;  ///< binding 8 of annotation (tonemap-bypass) draws
  QRhiBuffer* view_uniforms = nullptr;                    ///< binding 0: the camera-relative view-projection (mat4)
  const RhiSharedGeometry* geometry = nullptr;            ///< the view's shared unit meshes
  /// Takes the constant contents of the resources initialize() creates (unit
  /// meshes, lookup textures), recorded once there instead of on a frame. The
  /// view submits it before any draw, or discards it when a family fails to
  /// build. Valid only during initialize().
  QRhiResourceUpdateBatch* updates = nullptr;
  /// The owner of the family's scene pipelines: it registers each description
  /// there (add(), named `part`) and keeps the handles. It holds the widget's
  /// render target (ScenePipelineRegistry::setDirectTarget()).
  ScenePipelineRegistry* pipelines = nullptr;
  const char* part = nullptr;
};

/// A pass's user-facing message, translated in the view's context: the view
/// shows what its families report.
inline QString tr(const char* text) {
  return QCoreApplication::translate("pj::scene3d::SceneViewWidget", text);
}

/// PJ::straightAlphaBlend() (every data-graded scene pipeline's) that adds
/// nothing to destination alpha (the present pass's data/annotation split):
/// annotations (triads, TF lines) are not graded.
inline QRhiGraphicsPipeline::TargetBlend annotationAlphaBlend() {
  QRhiGraphicsPipeline::TargetBlend blend = PJ::straightAlphaBlend();
  blend.srcAlpha = QRhiGraphicsPipeline::Zero;
  return blend;
}

/// Per-frame values a family reads in prepare(), after admission.
struct RhiFrameContext {
  QRhi* rhi = nullptr;
  QRhiResourceUpdateBatch* updates = nullptr;
  glm::dvec3 render_origin{0.0};  ///< camera-relative origin subtracted in double
  QMatrix4x4 view_projection;     ///< clip-space-corrected, relative to render_origin
  glm::mat4 view{1.0F};           ///< the camera's view matrix relative to render_origin
};

/// The draws of `layer` in `draws`, where each layer's draws are contiguous (an
/// empty span when it has none). Linear in the frame's draws.
template <typename Draw, typename Layer>
std::span<const Draw> findDraws(const std::vector<Draw>& draws, const Layer* layer) {
  const auto first =
      std::find_if(draws.cbegin(), draws.cend(), [layer](const Draw& draw) { return draw.layer == layer; });
  const auto last = std::find_if(first, draws.cend(), [layer](const Draw& draw) { return draw.layer != layer; });
  return {first, last};
}

namespace detail {
template <typename Key, typename Gpu, typename Release>
void releaseGpu(Key key, Gpu& gpu, Release& release) {
  if constexpr (std::is_invocable_v<Release&, Gpu&>) {
    release(gpu);
  } else {
    release(key, gpu);
  }
}
}  // namespace detail

/// Releases (`release(gpu)` or `release(layer, gpu)`) and erases `layer`'s GPU
/// state, if it has any.
template <typename Map, typename Release>
void pruneLayerGpu(Map& gpu, const typename Map::key_type& layer, Release&& release) {
  if (const auto found = gpu.find(layer); found != gpu.end()) {
    detail::releaseGpu(found->first, found->second, release);
    gpu.erase(found);
  }
}

/// The families' setLayers(): releases the GPU state of layers that left the
/// list or are hidden, then adopts the new list. The families key their caches by layer
/// address and revision; this is what keeps a layer created at a freed layer's
/// address from meeting a stale entry: the dock syncs a layer out of the view's
/// list (SceneDockWidget::removeTopic(), clearLayers()) before destroying it.
template <typename Layer, typename Map, typename Release>
void adoptLayerList(const std::vector<Layer*>& layers, std::vector<Layer*>& current, Map& gpu, Release&& release) {
  std::erase_if(gpu, [&](auto& entry) {
    const bool drop =
        std::find(layers.cbegin(), layers.cend(), entry.first) == layers.cend() || !entry.first->visible();
    if (drop) {
      detail::releaseGpu(entry.first, entry.second, release);
    }
    return drop;
  });
  current = layers;
}

/// The admission loop of the budgeted families (point, pose, occupancy, voxel):
/// refills `admitted` for this frame. Per visible layer, in list order: the layer
/// refreshes against the budget left (prepareForRender(remaining)); one that is
/// not `drawable` has its GPU state pruned; one whose frame cannot be placed is
/// skipped and keeps it. Placement first, budget second: only a placed layer
/// takes its `cost` from the budget, so a temporarily unplaceable one never
/// reserves capacity ahead of a later layer whose frame resolves. An admitted
/// layer's `placed_bounds(layer, fixed_from_source)` (nullopt: none) are
/// unioned into the result, the family's bounds.
template <typename Layer, typename Map, typename Release, typename Drawable, typename Cost, typename PlacedBounds>
AABB admitLayers(
    const std::vector<Layer*>& layers, std::uint64_t budget, const PlaceFn& place,
    std::vector<PlacedLayer<Layer>>& admitted, Map& layer_gpu, Release&& release, Drawable&& drawable, Cost&& cost,
    PlacedBounds&& placed_bounds) {
  admitted.clear();
  AABB family_bounds;
  std::uint64_t remaining = budget;
  for (Layer* layer : layers) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining);
    if (!drawable(layer)) {
      pruneLayerGpu(layer_gpu, layer, release);
      continue;
    }
    const std::optional<Transform> placed = place(layer->renderFrameId(), layer->renderStamp(), layer->placementHold());
    if (!placed.has_value() || !tryConsumeBudget(cost(layer), remaining)) {
      continue;
    }
    if (const std::optional<AABB> layer_bounds = placed_bounds(layer, *placed); layer_bounds.has_value()) {
      family_bounds = unionAABB(family_bounds, *layer_bounds);
    }
    admitted.push_back({layer, *placed});
  }
  return family_bounds;
}

using PJ::loadRhiShader;

/// True when `pipeline` (nullptr counts as true) was built for `render_pass` with
/// `sample_count` samples. Compares the descriptor by address only: a stale
/// pipeline's descriptor may already be deleted.
inline bool pipelineBuiltFor(
    const QRhiGraphicsPipeline* pipeline, const QRhiRenderPassDescriptor* render_pass, int sample_count) {
  return pipeline == nullptr ||
         (pipeline->renderPassDescriptor() == render_pass && pipeline->sampleCount() == sample_count);
}

inline RhiConventions rhiConventions(const QRhi& rhi) {
  return {rhi.isYUpInNDC(), rhi.isYUpInFramebuffer(), rhi.isClipDepthZeroToOne()};
}

inline void copyMatrix(const QMatrix4x4& matrix, std::array<float, 16>& output) {
  std::memcpy(output.data(), matrix.constData(), 16U * sizeof(float));
}

/// The same column-major bytes as copyMatrix(toQMatrix(matrix), output).
inline void copyMatrix(const glm::mat4& matrix, std::array<float, 16>& output) {
  std::memcpy(output.data(), &matrix[0][0], 16U * sizeof(float));
}

inline QMatrix4x4 toQMatrix(const glm::mat4& matrix) {
  // glm stores columns; QMatrix4x4's scalar constructor is row-major.
  return QMatrix4x4(
      matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0], matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
      matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2], matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}

}  // namespace pj::scene3d
