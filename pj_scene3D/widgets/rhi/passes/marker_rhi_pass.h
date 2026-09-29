// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi_scene_pass.h"
#include "pj_widgets/GraphicsApi.h"  // padForUpload

namespace pj::scene3d {

class SceneEntitiesLayer;
struct MarkerGeometry;

/// A typed array held in a QByteArray, so an upload shares its bytes (the
/// batch takes a reference: no copy) and a rewrite on a later frame, once the
/// batch has let go, reuses the same pages. Padded for upload
/// (PJ::padForUpload()): the padded array still uploads as one shared piece,
/// which the retention rule depends on. At namespace scope: nested in
/// MarkerRhiPass, Clang emits none of its members (undefined references).
template <typename T>
class MarkerUploadArray {
 public:
  [[nodiscard]] std::size_t size() const {
    return count_;
  }
  [[nodiscard]] bool empty() const {
    return count_ == 0U;
  }
  /// Sizes the array for `count` elements whose contents the caller then
  /// writes: a fresh allocation, not a detaching or growing copy, while an
  /// upload still shares the bytes or they do not fit.
  void resizeForOverwrite(std::size_t count) {
    const auto used = static_cast<qsizetype>(count * sizeof(T));
    if (const qsizetype padded = PJ::paddedUploadSize(used); !bytes_.isDetached() || bytes_.capacity() < padded) {
      bytes_ = QByteArray(padded, Qt::Uninitialized);
    }
    count_ = count;
    PJ::padForUpload(bytes_, used);
  }
  /// Shrinks to `count` elements, keeping the allocation.
  void truncate(std::size_t count) {
    count_ = count;
    PJ::padForUpload(bytes_, static_cast<qsizetype>(count * sizeof(T)));
  }
  /// Detaches (copies) while an upload still shares the bytes.
  [[nodiscard]] T* data() {
    return reinterpret_cast<T*>(bytes_.data());
  }
  [[nodiscard]] const T& operator[](std::size_t index) const {
    return reinterpret_cast<const T*>(bytes_.constData())[index];
  }
  /// The upload: the elements, then the padding.
  [[nodiscard]] const QByteArray& bytes() const {
    return bytes_;
  }

 private:
  QByteArray bytes_;
  std::size_t count_ = 0;
};

/// The SceneEntities marker family of the QRhi view: instanced unit primitives
/// (cube + its edge overlay, sphere, cylinder/cone, arrow/axes) and the
/// expanded line/triangle stream batches. Each admitted layer owns Static
/// instance and stream buffers (LayerGpu), rebuilt only when its LayerKey
/// changes: geometry revision, style, the resolved placement of
/// every frame it references (a TF-only move bumps no revision), and the render
/// origin. Orbit, zoom and playback without new samples or TF motion reuse the
/// buffers. Vertices stay camera-relative, subtracted in double, for pixel parity,
/// so a pan or follow step (a change of the render origin alone) re-uploads: a layer
/// re-uploaded within the last few frames keeps its CPU expansion and the
/// world-space translation of every instance, and an origin-only change then
/// rewrites the translations and re-places the stream vertices in the same buffer
/// layout. A one-off rebuild drops the expansion right after its upload, a
/// recurring one after a few frames without another, or a short idle spell
/// without any frame (the view renders nothing while settled). The layer's model
/// primitives belong to ModelRhiPass. Per frame the view calls admit() (once),
/// prepare() (once), then submit() per ScenePass for each draw. Markers cast no
/// shadows; the layer's model primitives do, through ModelRhiPass.
class MarkerRhiPass {
 public:
  /// kCubeEdges: the cube fill's instance range again, drawn as the twelve box
  /// edges by marker_edge.vert (facing-dependent colour).
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

  MarkerRhiPass();

  /// Binds the setup's view uniforms (binding 0), data render mode (binding 8
  /// of the instanced primitives) and annotation render mode (binding 8 of the
  /// stream batches). False leaves the pass unusable.
  [[nodiscard]] bool initialize(const RhiPassSetup& setup);
  [[nodiscard]] bool ready() const;
  /// Adopts the view's marker layers; drops GPU state of layers that left or are hidden.
  void setLayers(const std::vector<SceneEntitiesLayer*>& layers);
  /// A layer of this family is in the view: the view builds the family (initialize()) only then.
  [[nodiscard]] bool hasLayers() const {
    return !layers_.empty();
  }

  /// Runs each visible layer's prepareForRender(), places its frames, applies
  /// the per-layer and per-view instance/stream budgets (row warnings on
  /// rejection) and records the family's fixed-frame bounds (lastBounds()).
  void admit(const PlaceFn& place);
  /// Rebuilds and uploads the admitted layers whose LayerKey changed (and the
  /// unit meshes once) and releases the GPU state of layers not admitted this
  /// frame. A layer whose buffers cannot be allocated has
  /// no draws this frame and retries the next.
  void prepare(const RhiFrameContext& frame);
  /// The prepared ranges of `layer`, in submission order.
  [[nodiscard]] std::span<const Draw> drawsFor(const SceneEntitiesLayer* layer) const {
    return findDraws(draws_, layer);
  }
  /// Records one range; false when nothing could be recorded for `pass`.
  bool submit(
      QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
      const QRhiViewport& viewport) const;

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
  /// Layer re-uploads (a full expansion or an origin-only update) since
  /// initialization; cumulative.
  [[nodiscard]] std::uint64_t layerRebuildCountForTest() const {
    return layer_rebuild_count_;
  }
  /// The re-uploads of layerRebuildCountForTest() that took the origin-only path.
  [[nodiscard]] std::uint64_t originUpdateCountForTest() const {
    return origin_update_count_;
  }
  /// Layers holding their CPU expansion after the last prepare().
  [[nodiscard]] int retainedExpansionCountForTest() const {
    return static_cast<int>(std::count_if(
        layer_gpu_.cbegin(), layer_gpu_.cend(), [](const auto& entry) { return entry.second.cpu.has_value(); }));
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
  /// True holds kept expansions through idle spells (the wall-clock release),
  /// so a count check cannot race a slow frame; false re-arms that release.
  void setIdleReleasePausedForTest(bool paused) {
    idle_release_paused_ = paused;
    if (!paused && retainedExpansionCountForTest() > 0) {
      expansion_release_timer_.start(0);
    }
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
  /// Everything a layer's expansion depends on but the render origin. The
  /// placements are the fixed-from-frame transforms of the layer's frames
  /// (nullopt: unplaced), compared exactly.
  struct ContentKey {
    std::uint64_t geometry_revision = 0;
    float opacity = 1.0F;
    std::optional<glm::vec3> override_color;
    bool wireframe = false;
    std::vector<std::optional<Transform>> placements;

    bool operator==(const ContentKey&) const = default;
  };
  /// Everything a layer's expansion depends on: an origin-only change keeps
  /// `content`, which the origin-only update relies on.
  struct LayerKey {
    ContentKey content;
    glm::dvec3 render_origin{0.0};

    bool operator==(const LayerKey&) const = default;
  };
  template <typename T>
  using UploadArray = MarkerUploadArray<T>;
  /// One layer's CPU expansion, what its buffers hold.
  struct Expansion {
    std::vector<std::optional<glm::dmat4>> fixed_from_frame;  ///< the key's placements as matrices
    UploadArray<Instance> instances;
    /// Each instance's world (fixed-frame) translation, before the origin is
    /// subtracted and the result narrowed to float; parallel to `instances` in
    /// an expansion kept for origin-only updates, else empty.
    std::vector<glm::dvec3> instance_world;
    UploadArray<StreamVertex> lines;
    UploadArray<StreamVertex> triangles;
    std::vector<glm::vec3> placed_points;  ///< scratch: one stream batch's points, camera-relative
    std::vector<Draw> draws;               ///< buffer unset: offsets index the vectors above
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
    std::uint64_t admitted_frame = 0;  ///< the prepare() that last admitted the layer
    std::uint64_t uploaded_frame = 0;  ///< the prepare() that last re-uploaded the layer
    /// The expansion of the last upload, kept only while the layer keeps
    /// re-uploading (pan, follow, a live stream): origin-only changes then update
    /// it in place, and a full rebuild reuses its capacity.
    std::optional<Expansion> cpu;
  };

  /// Expands `geometry` as `key` describes it into `out` (cleared first, its
  /// capacity kept): a pure function of its arguments, vertices relative to
  /// key.render_origin. `keep_world` fills out.instance_world, which moveOrigin()
  /// needs: only an expansion kept past its upload is ever moved.
  static void expandLayer(const MarkerGeometry& geometry, const LayerKey& key, bool keep_world, Expansion& out);
  /// Turns `out`, the expansion of `geometry` under a key with the same content,
  /// into the one for `key`: instance translations from `instance_world`, the
  /// stream vertices re-placed. Bit-identical to
  /// expandLayer(geometry, key, true, out).
  static void moveOrigin(const MarkerGeometry& geometry, const LayerKey& key, Expansion& out);
  /// Writes the stream vertices of `geometry` under `key` into out.lines (user
  /// lines, then wireframe triangle edges) and out.triangles, replacing their
  /// contents; returns the count of user line vertices.
  static quint32 expandStreams(const MarkerGeometry& geometry, const LayerKey& key, Expansion& out);
  /// Uploads *gpu.cpu into `gpu`'s buffers and adopts its draws; false (and `gpu`
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
  const ScenePipeline* instancePipeline(const Draw& draw) const;

  /// The placement of `frame_index` of `admitted`, or nullptr when unplaced.
  [[nodiscard]] const Transform* placement(const Admitted& admitted, std::uint32_t frame_index) const;

  QRhiShaderResourceBindings* data_bindings_ = nullptr;
  QRhiShaderResourceBindings* annotation_bindings_ = nullptr;
  QRhiShaderResourceBindings* edge_bindings_ = nullptr;  ///< data_bindings_ plus camera_view_uniforms_
  QRhiBuffer* camera_view_uniforms_ = nullptr;           ///< binding 4 of marker_edge.vert, written per frame
  /// The registered pipelines; each has a depth-writing and a no-depth-write registration.
  enum Pipeline : std::uint8_t {
    kTrianglePipeline,
    kTriangleCullPipeline,
    // The cube fill, depth-biased away from the camera (a polygon offset of 1, 1)
    // so its edges, drawn at the exact depth, win the depth test.
    kCubeFillPipeline,
    kEdgePipeline,
    kLinePipeline,
    kStreamLinePipeline,
    kStreamTrianglePipeline,
    kPipelineCount,
  };
  /// [pipeline][depth_write]
  std::array<std::array<const ScenePipeline*, 2>, kPipelineCount> pipelines_{};
  std::array<MeshGpu, 5> meshes_{};

  std::vector<SceneEntitiesLayer*> layers_;
  std::unordered_map<SceneEntitiesLayer*, LayerGpu> layer_gpu_;
  std::vector<Admitted> admitted_;
  std::vector<std::optional<Transform>> placements_;  ///< every admitted layer's frames, contiguous per layer
  std::vector<Draw> draws_;                           ///< contiguous per layer, in admission order
  LayerKey scratch_key_;
  bool cache_enabled_ = true;            // test hook
  bool fail_stream_allocation_ = false;  // test hook
  bool idle_release_paused_ = false;     // test hook
  std::uint64_t frame_serial_ = 0;
  std::uint64_t layer_rebuild_count_ = 0;
  std::uint64_t origin_update_count_ = 0;
  /// Drops every kept expansion once no prepare() ran for a while: a settled
  /// view renders no frame that would otherwise let the frame window expire.
  QTimer expansion_release_timer_;
  QElapsedTimer last_retaining_prepare_;  ///< restarted by every prepare() that keeps an expansion

  int last_layer_count_ = 0;
  int last_instance_count_ = 0;
  int last_stream_vertex_count_ = 0;
  AABB last_bounds_;
};

}  // namespace pj::scene3d
