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
// Every scene family exposes the same shape: initialize(RhiPassSetup),
// registerPipelines(registry), setLayers(layers), admit(place, bounds),
// prepare(RhiFrameContext, ...), drawsFor(layer) (a span of Draws, each with a
// draw_class), submit(command_buffer, draw, ScenePass, registry, viewport) and
// release().

#include <rhi/qrhi.h>

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
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/rhi_conventions.h"

namespace pj::scene3d {

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

/// Owns the HDR-colour and depth-replay clones of the view's direct pipelines.
/// Each family registers the pipelines it creates; resolve() clones one the first
/// time a ScenePass binds it, once the view has named the clone targets. The
/// clones copy every state of the source (see clonePipelineForRenderPass), so a
/// family never builds HDR variants itself.
class ScenePipelineRegistry {
 public:
  /// Registers a direct pipeline (nullptr is ignored; re-adding is a no-op).
  void add(QRhiGraphicsPipeline* direct);
  /// Unregisters `direct` and deletes its clones; call before deleting it.
  void remove(QRhiGraphicsPipeline* direct);
  /// The render passes clones target: HDR colour (`hdr`, `hdr_samples`) and depth
  /// replay (`depth`, one sample, alpha-only writes). New targets drop every
  /// clone first.
  void setCloneTargets(QRhi* rhi, QRhiRenderPassDescriptor* hdr, int hdr_samples, QRhiRenderPassDescriptor* depth);
  /// Deletes every clone and forgets the targets; keeps the registrations (the
  /// HDR chain went down).
  void releaseClones();
  /// releaseClones() and forgets every registration (direct pipelines dying).
  void clear();
  /// The pipeline `pass` binds for `direct`: `direct` itself for kDirect, else its
  /// clone, created on this first request; nullptr when unregistered, without
  /// targets, or when the clone cannot be created (then retried next call).
  [[nodiscard]] QRhiGraphicsPipeline* resolve(QRhiGraphicsPipeline* direct, ScenePass pass) const;
  /// Every direct pipeline satisfies pipelineBuiltFor(render_pass, sample_count).
  [[nodiscard]] bool directsBuiltFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const;

 private:
  struct Entry {
    QRhiGraphicsPipeline* direct = nullptr;
    QRhiGraphicsPipeline* hdr = nullptr;
    QRhiGraphicsPipeline* depth = nullptr;
  };
  // The clones are a cache that resolve() fills, hence mutable.
  mutable std::vector<Entry> entries_;
  QRhi* rhi_ = nullptr;
  QRhiRenderPassDescriptor* hdr_ = nullptr;
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
  QRhiBuffer* arrow_vertices = nullptr;    ///< unit pose arrow: pose triads, TF triads, corner gizmo
  QRhiBuffer* arrow_indices = nullptr;     ///< uint32
  quint32 arrow_index_count = 0;
  bool upload_pending = false;

  bool create(QRhi* rhi);
  /// Queues the static uploads once after create().
  void upload(QRhiResourceUpdateBatch* updates);
  void release();
};

/// The frame's scene bounds as admission builds them, plus the layers whose
/// placed bounds went into it: the input of the view's camera-fit policy. A
/// layer is identified by its most-derived address (dynamic_cast<const void*>),
/// the same key SceneViewWidget::setLayers() hands the policy.
struct SceneBoundsCollector {
  AABB bounds;
  std::vector<const void*> contributors;

  void add(const void* layer, const AABB& layer_bounds) {
    bounds = unionAABB(bounds, layer_bounds);
    contributors.push_back(layer);
  }
};

/// Places a source frame in the fixed frame at the frame's render time; nullopt
/// when it cannot be placed this frame (the family then skips the layer without
/// consuming its budget).
using PlaceFn = std::function<std::optional<Transform>(const std::string& source_frame)>;

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

/// Grows `buffer` (a Dynamic vertex buffer) to hold `required_bytes`, doubling
/// from max(capacity, minimum_capacity); keeps it when already large enough.
/// Recreating drops the old contents. False when it cannot be allocated.
bool ensureVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity);
/// Creates `buffer` as a `bytes`-sized Dynamic uniform buffer when it is null;
/// false (and `buffer` null) when it cannot be created.
bool ensureUniformBuffer(QRhi* owner, QRhiBuffer*& buffer, quint32 bytes);
/// A created SRB holding `bindings`, or nullptr when it cannot be created.
QRhiShaderResourceBindings* createBindings(QRhi* owner, std::initializer_list<QRhiShaderResourceBinding> bindings);

/// What a family's initialize() binds from the view; valid until the view's
/// releaseResources().
struct RhiPassSetup {
  QRhi* rhi = nullptr;
  QRhiRenderPassDescriptor* render_pass = nullptr;  ///< direct pipelines target this
  int sample_count = 1;
  QRhiBuffer* render_mode_uniforms = nullptr;             ///< binding 8 of every data-graded draw
  QRhiBuffer* annotation_render_mode_uniforms = nullptr;  ///< binding 8 of annotation (tonemap-bypass) draws
  QRhiBuffer* view_uniforms = nullptr;                    ///< binding 0: the camera-relative view-projection (mat4)
  const RhiSharedGeometry* geometry = nullptr;            ///< the view's shared unit meshes
};

/// Straight-alpha source-over, the blend of every data-graded scene pipeline.
inline QRhiGraphicsPipeline::TargetBlend dataAlphaBlend() {
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  return blend;
}

/// dataAlphaBlend() that adds nothing to destination alpha (the present pass's
/// data/annotation split): annotations (triads, TF lines) are not graded.
inline QRhiGraphicsPipeline::TargetBlend annotationAlphaBlend() {
  QRhiGraphicsPipeline::TargetBlend blend = dataAlphaBlend();
  blend.srcAlpha = QRhiGraphicsPipeline::Zero;
  return blend;
}

/// The state of one pipeline; the defaults are the scene's (data blend, depth
/// test and write, LessOrEqual, no culling).
struct PipelineDesc {
  QRhiGraphicsPipeline::Topology topology = QRhiGraphicsPipeline::Triangles;
  QShader vertex{};
  QShader fragment{};
  QRhiVertexInputLayout input_layout{};
  QRhiShaderResourceBindings* bindings = nullptr;                             ///< the layout SRB
  std::optional<QRhiGraphicsPipeline::TargetBlend> blend = dataAlphaBlend();  ///< nullopt: no colour target
  QRhiGraphicsPipeline::CullMode cull = QRhiGraphicsPipeline::None;
  bool depth_test = true;
  bool depth_write = true;
  QRhiGraphicsPipeline::CompareOp depth_op = QRhiGraphicsPipeline::LessOrEqual;
  int depth_bias = 0;
  float slope_scaled_depth_bias = 0.0F;
  QRhiGraphicsPipeline::Flags flags{};
};
/// A created pipeline for `setup`'s render pass and sample count, or nullptr.
QRhiGraphicsPipeline* createScenePipeline(const RhiPassSetup& setup, const PipelineDesc& desc);

/// Per-frame values a family reads in prepare(), after admission and camera fit.
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

/// Every family's setLayers(): releases the GPU state of layers that left the
/// list or are hidden (layers without visible(), the trail owners, only when they
/// leave), then adopts the new list.
template <typename Layer, typename Map, typename Release>
void adoptLayerList(const std::vector<Layer*>& layers, std::vector<Layer*>& current, Map& gpu, Release&& release) {
  std::erase_if(gpu, [&](auto& entry) {
    bool drop = std::find(layers.cbegin(), layers.cend(), entry.first) == layers.cend();
    if constexpr (requires { entry.first->visible(); }) {
      drop = drop || !entry.first->visible();
    }
    if (drop) {
      detail::releaseGpu(entry.first, entry.second, release);
    }
    return drop;
  });
  current = layers;
}

QShader loadRhiShader(const QString& path);

/// A copy of `source` for `render_pass`/`sample_count`; `coverage_alpha_only`
/// masks colour writes to alpha (the depth replay's coverage).
QRhiGraphicsPipeline* clonePipelineForRenderPass(
    QRhi* owner, const QRhiGraphicsPipeline* source, QRhiRenderPassDescriptor* render_pass, int sample_count,
    bool coverage_alpha_only = false);

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
