#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>
#include <vector>

#include "pj_scene3d_widgets/scene_view_widget.h"

namespace pj::scene3d {

class GridMapRhiPass;
class GridRhiPass;
class MarkerRhiPass;
class ModelRhiPass;
class OccupancyRhiPass;
class OverlayRhiPass;
class PointRhiPass;
class PoseRhiPass;
class ScenePipelineRegistry;
class TrailRhiPass;
class VoxelRhiPass;
struct QualityStageState;

// Test-only access to a SceneViewWidget's internals, through the view's
// friendship: what the last submitted frame drew, the state of its quality
// stages, and the test overrides. Linked only by the tests, the benchmarks and
// the browser acceptance probes (target pj_scene3d_widgets_testing), which
// builds without PJ_RHI_TEST_HOOKS too. Reads observe; the setters change the
// view as a failure or an option would and invalidate its content.
class SceneViewTestAccess {
 public:
  using Stage = SceneViewWidget::QualityStage;

  // ---- The last submitted frame ----
  // The families' own counters (lastLayerCount(), lastBounds(), ...) are read
  // through the passes below; the view's own are here.
  [[nodiscard]] static int lastLineVertexCount(const SceneViewWidget& view) {
    return view.last_line_vertex_count_;
  }
  [[nodiscard]] static int lastTriangleVertexCount(const SceneViewWidget& view) {
    return view.last_triangle_vertex_count_;
  }
  [[nodiscard]] static int lastResolvedFrameCount(const SceneViewWidget& view) {
    return view.last_resolved_frame_count_;
  }
  // proj * view of the last painted frame, relative to its render origin (GL clip convention).
  [[nodiscard]] static const glm::mat4& lastViewProjection(const SceneViewWidget& view) {
    return view.last_view_projection_;
  }
  [[nodiscard]] static int max3DTextureSize(const SceneViewWidget& view) {
    return view.max_3d_texture_size_;
  }
  // Topic ids of the layers that submitted at least one colour draw (direct or
  // HDR), in logical (dock) order even when a translucent draw was deferred.
  // The depth replay, which skips no-depth-write draws, does not count.
  [[nodiscard]] static const std::vector<std::uint32_t>& lastSubmittedLayerIds(const SceneViewWidget& view) {
    return view.last_submitted_layer_ids_;
  }
  // Output pixel rectangle (top-left origin) of each overlay image drawn, in
  // OverlayRhiPass::Slot order (perf HUD, hover label, hover ring); empty for a
  // slot that drew nothing.
  [[nodiscard]] static const std::array<QRect, 3>& lastOverlayRects(const SceneViewWidget& view) {
    return view.last_overlay_rects_;
  }
  // Every pipeline that draws into the widget's own render target (the direct
  // scene pipelines, the overlays, the HDR present) was built for the current
  // one: its render pass descriptor and sample count.
  [[nodiscard]] static bool pipelinesBuiltForTarget(const SceneViewWidget& view);
  // The scene pipelines registered, and their variants created so far: direct
  // (the widget's target) and HDR (HDR colour plus depth replay).
  struct PipelineCounts {
    int registered = 0;
    int directs = 0;
    int hdr_variants = 0;
  };
  [[nodiscard]] static PipelineCounts pipelineCounts(const SceneViewWidget& view);
  [[nodiscard]] static const ScenePipelineRegistry& pipelines(const SceneViewWidget& view) {
    return *view.pipelines_;
  }
  // The scene families built on demand that are built now, by name ("points",
  // "trails", "occupancy", "voxels", "markers", "models", "grid maps").
  [[nodiscard]] static QStringList builtFamilies(const SceneViewWidget& view);

  // The passes themselves (rhi/passes/*.h): their last-frame counters and caches.
  [[nodiscard]] static PointRhiPass& pointPass(const SceneViewWidget& view) {
    return *view.point_pass_;
  }
  [[nodiscard]] static PoseRhiPass& posePass(const SceneViewWidget& view) {
    return *view.pose_pass_;
  }
  [[nodiscard]] static OccupancyRhiPass& occupancyPass(const SceneViewWidget& view) {
    return *view.occupancy_pass_;
  }
  [[nodiscard]] static VoxelRhiPass& voxelPass(const SceneViewWidget& view) {
    return *view.voxel_pass_;
  }
  [[nodiscard]] static TrailRhiPass& trailPass(const SceneViewWidget& view) {
    return *view.trail_pass_;
  }
  [[nodiscard]] static GridMapRhiPass& gridMapPass(const SceneViewWidget& view) {
    return *view.gridmap_pass_;
  }
  [[nodiscard]] static MarkerRhiPass& markerPass(const SceneViewWidget& view) {
    return *view.marker_pass_;
  }
  [[nodiscard]] static GridRhiPass& gridPass(const SceneViewWidget& view) {
    return *view.grid_pass_;
  }
  [[nodiscard]] static ModelRhiPass& modelPass(const SceneViewWidget& view) {
    return *view.model_pass_;
  }
  [[nodiscard]] static OverlayRhiPass& overlayPass(const SceneViewWidget& view) {
    return *view.overlay_pass_;
  }

  // ---- Quality stages: last frame's state, resources and capability error ----
  // last_active, capability_error, resource_generation, ...
  // (rhi/scene_view_widget_rhi_quality_p.h).
  [[nodiscard]] static const QualityStageState& stage(const SceneViewWidget& view, Stage stage) {
    return view.stageState(stage);
  }
  [[nodiscard]] static bool shadowResourcesReady(const SceneViewWidget& view);
  [[nodiscard]] static bool lastShadowFitValid(const SceneViewWidget& view) {
    return view.last_shadow_fit_valid_;
  }
  [[nodiscard]] static int shadowMapSize(const SceneViewWidget& view);
  [[nodiscard]] static AABB lastShadowBounds(const SceneViewWidget& view) {
    return view.last_shadow_bounds_;
  }
  [[nodiscard]] static bool hdrResourcesReady(const SceneViewWidget& view);
  [[nodiscard]] static QSize hdrRenderSize(const SceneViewWidget& view);
  [[nodiscard]] static std::uint64_t hdrAllocatedBytes(const SceneViewWidget& view);
  // The live HDR chain resolves its multisample depth instead of replaying the scene.
  [[nodiscard]] static bool depthResolveActive(const SceneViewWidget& view);
  [[nodiscard]] static bool ssaoResourcesReady(const SceneViewWidget& view);
  // The mask target and the model pass's mask pipelines exist.
  [[nodiscard]] static bool edlResourcesReady(const SceneViewWidget& view);

  // ---- Placement without a display ----
  // Frame contract step 1 (every family's layers admitted and placed) and the TF
  // triads, at the view's tracker time, without a QRhi: what render() places.
  static void admitFrame(SceneViewWidget& view);
  // The TF triad arms admitFrame() or the last render built, relative to the render origin.
  [[nodiscard]] static const std::vector<PoseTriadInstance>& tfTriads(const SceneViewWidget& view) {
    return view.tf_triad_instances_;
  }

  // ---- Test overrides ----
  // Holds `stage` down as a failure would (its capability error names the
  // override; the browser acceptance specs match it), or lets it build again.
  static void forceStageFallback(SceneViewWidget& view, Stage stage, bool force) {
    view.forceStageFallback(stage, force);
  }
  // Keeps the depth replay where the depth-resolve fast path is available
  // (rebuilds the HDR chain), for comparing the two.
  static void setForceDepthReplay(SceneViewWidget& view, bool force);
  // The next shadowed frame renders the shadow map again, even when the
  // retained one is exact.
  static void discardShadowMap(SceneViewWidget& view);

#ifdef PJ_RHI_TEST_HOOKS
  [[nodiscard]] static const SceneViewWidget::StageTimes& stageTimes(const SceneViewWidget& view) {
    return view.stage_times_;
  }
#endif
};

}  // namespace pj::scene3d
