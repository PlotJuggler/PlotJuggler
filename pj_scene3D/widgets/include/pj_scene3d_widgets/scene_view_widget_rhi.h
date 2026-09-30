#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRhiWidget>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/camera/camera_fit_policy.h"
#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/moving_average.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_widgets/GraphicsApi.h"

class QEvent;
class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderBuffer;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;

namespace pj::scene3d {

class PointRenderable;
class Scene3DLayer;
class PosesInFrameLayer;
class OccupancyGridLayer;
class VoxelGridLayer;
class SceneEntitiesLayer;
class ModelRenderable;
class OccupancyRhiPass;
class MarkerRhiPass;
class ModelRhiPass;
class GridMapLayer;
class GridMapRhiPass;
class TrailLayer;
class TrailRhiPass;
class GridRhiPass;
class OverlayRhiPass;
class PoseRhiPass;
class VoxelRhiPass;
class PointRhiPass;
class ScenePipelineRegistry;
struct RhiPassSetup;
struct RhiSharedGeometry;
struct SceneBoundsCollector;
enum class ScenePass;

// QRhi implementation of the public SceneViewWidget contract (the browser view,
// and the desktop view with PJ_SCENE3D_RHI); the GL build's class of the same
// name is a QOpenGLWidget (scene_view_widget.h). This class owns only QRhi
// resources; camera and TF semantics come from pj_scene3d_core. The frame
// contract and pass vocabulary live in rhi/rhi_scene_pass.h.
//
// Failure state: when the base pipeline set (every scene family, the shared
// geometry and uniforms) cannot be built on the current QRhi, the view submits
// no scene draw. It clears to the backdrop, shows the reason through
// renderingWarningChanged(), emits renderFailed() once, and retries on the next
// initialize() (a new QRhi, or a new output size, format or sample count).
// Optional stages (HDR and its pipeline clones, SSAO, EDL, shadows, overlays)
// drop on their own, each with a one-time warning.
class SceneViewWidget : public QRhiWidget {
  Q_OBJECT

 public:
  using GridStyle = pj::scene3d::GridStyle;
  enum class CameraModel { kOrbit, kXyOrbit, kFly, kTopDownOrtho };

  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(PJ::Timepoint time);
  void setFixedFrame(const std::string& frame);
  [[nodiscard]] const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  void setFollowFrame(const std::string& frame);
  [[nodiscard]] const std::string& followFrame() const {
    return follow_frame_;
  }
  [[nodiscard]] uint64_t followRenderKey(PJ::Timepoint time) const;
  void recenterOnFollowFrame();

  [[nodiscard]] ICamera& camera() {
    return *camera_;
  }
  [[nodiscard]] const ICamera& camera() const {
    return *camera_;
  }
  void setCameraModel(CameraModel model);
  [[nodiscard]] CameraModel cameraModel() const {
    return camera_model_;
  }
  void resetCamera();
  // Owns the scene bounds in this renderer: every frame it unions what it draws
  // (see applySceneBounds) and fits the camera per CameraFitPolicy. A value set
  // here lasts only until the next frame.
  void setSceneBounds(const AABB& bounds);
  // The dock restored the camera from a layout: no automatic fit overrides it
  // (CameraFitPolicy::noteCameraRestored).
  void noteCameraRestored();
  void setLayers(const std::vector<Scene3DLayer*>& ordered_layers);

  void setGridStyle(GridStyle style);
  [[nodiscard]] GridStyle gridStyle() const {
    return grid_style_;
  }
  void setGridDivisions(int divisions);
  [[nodiscard]] int gridDivisions() const {
    return grid_divisions_;
  }
  void setGridExtentMetres(float extent_m);
  [[nodiscard]] float gridExtentMetres() const {
    return grid_extent_m_;
  }
  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const {
    return grid_visible_;
  }

  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }
  void setGizmoSize(float length_m);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_m_;
  }
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }
  void setTfConnectionsVisible(bool visible);
  [[nodiscard]] bool tfConnectionsVisible() const {
    return tf_connections_visible_;
  }

  [[nodiscard]] MeshShadingParams& meshShadingParams() {
    return shading_params_;
  }
  [[nodiscard]] const MeshShadingParams& meshShadingParams() const {
    return shading_params_;
  }
  void setMeshShadingParams(const MeshShadingParams& params);

  // Present-pass look knobs (tonemap, exposure, saturation, AO/EDL), the GL
  // view's type and semantics: read every frame, call update() after changing.
  using CompositeParams = pj::scene3d::CompositeParams;
  [[nodiscard]] CompositeParams& compositeParams() {
    return composite_params_;
  }

  // The GPU/CPU timing overlay (bottom-left), also toggled with 'P'. Off by default.
  void setShowPerfHud(bool on);
  [[nodiscard]] bool showPerfHud() const {
    return show_perf_hud_;
  }

  // Supersampling factor, clamped to [1, 4]: the HDR chain renders at scale x
  // output pixels and the present pass downsamples it. Screen-space sizes (point
  // pixels, EDL radius) scale with it; the HDR byte budgets apply to the scaled
  // size. Ignored on the direct fallback. Not wired into the app UI.
  void setRenderScale(float scale);
  [[nodiscard]] float renderScale() const {
    return render_scale_;
  }

  // Frame picking, the one owner for the hover label and the frame context menu:
  // the TF frame whose origin projects within 20 logical pixels of `logical_pos`
  // (widget coordinates, independent of DPR and render scale), nearest first;
  // nullopt when the frame triads are hidden, there is no TF buffer or nothing
  // is close enough. Uses the current camera, so it is valid before any frame.
  [[nodiscard]] std::optional<std::string> frameAt(const QPointF& logical_pos) const;
  // The frame under the cursor, tracked while no mouse button is held; see
  // hoveredFrameChanged.
  [[nodiscard]] const std::optional<std::string>& hoveredFrame() const {
    return hovered_frame_;
  }

  void refreshAvailableFrames();
  [[nodiscard]] uint64_t tfRenderKey(PJ::Timepoint time) const;

  // Delivers the current scene as an image to `done`; use it instead of
  // grabFramebuffer(). A shown view first presents one real frame (asynchronously,
  // up to 2 s): Qt 6.11.1's OpenGL QRhi advances its frame number only in
  // swapchain frames, so across grab-only frames a pipeline sharing an SRB keeps
  // stale uniforms (an old camera). The image is null when that frame never comes
  // (occluded window). A never-shown view is grabbed at once, and in Qt 6.11.1
  // only ONCE: each such grab creates a new QRhi without re-running initialize().
  // `done` runs once on the GUI thread, or never if the view is destroyed first.
  // Never blocks: no nested event loop, which Qt for WebAssembly cannot run.
  void grabPresentedFrame(std::function<void(const QImage&)> done);

  // Observation-only browser acceptance seams.  They report the last submitted
  // product frame; no test method mutates the camera, TF buffer, or renderer.
  [[nodiscard]] int lastLineVertexCountForTest() const {
    return last_line_vertex_count_;
  }
  [[nodiscard]] int lastTriangleVertexCountForTest() const {
    return last_triangle_vertex_count_;
  }
  [[nodiscard]] int lastResolvedFrameCountForTest() const {
    return last_resolved_frame_count_;
  }
  [[nodiscard]] int lastPointVertexCountForTest() const;
  [[nodiscard]] int lastPointLayerCountForTest() const;
  [[nodiscard]] int lastCubeLayerCountForTest() const;
  [[nodiscard]] int lastCubeInstanceCountForTest() const;
  // Cube layers the whole-cloud LOD drew as sprites last frame.
  [[nodiscard]] int lastCubeSpriteLayerCountForTest() const;
  // Point layers drawn as a point list (not quads) last frame.
  [[nodiscard]] int lastPointListLayerCountForTest() const;
  [[nodiscard]] int cameraFitCountForTest() const {
    return camera_fit_count_;
  }
  [[nodiscard]] AABB lastPointBoundsForTest() const;
  [[nodiscard]] int lastDepthLayerCountForTest() const;
  [[nodiscard]] int lastDepthVertexCountForTest() const;
  [[nodiscard]] AABB lastDepthBoundsForTest() const;
  [[nodiscard]] int lastPoseLayerCountForTest() const;
  [[nodiscard]] int lastPoseArmCountForTest() const;
  [[nodiscard]] AABB lastPoseBoundsForTest() const;
  [[nodiscard]] int lastOccupancyLayerCountForTest() const;
  [[nodiscard]] int lastOccupancyCellCountForTest() const;
  [[nodiscard]] int occupancyFullUploadCountForTest() const;
  [[nodiscard]] int occupancyPartialUploadCountForTest() const;
  [[nodiscard]] AABB lastOccupancyBoundsForTest() const;
  [[nodiscard]] int lastGridMapLayerCountForTest() const;
  [[nodiscard]] int lastGridMapVertexCountForTest() const;
  [[nodiscard]] int gridMapUploadCountForTest() const;
  // Trails drawn last frame (standalone and pose-embedded) and their strip vertices.
  [[nodiscard]] int lastTrailCountForTest() const;
  [[nodiscard]] int lastTrailVertexCountForTest() const;
  [[nodiscard]] int lastVoxelLayerCountForTest() const;
  [[nodiscard]] int lastVoxelCountForTest() const;
  [[nodiscard]] int voxelFullUploadCountForTest() const;
  [[nodiscard]] int max3DTextureSizeForTest() const {
    return max_3d_texture_size_;
  }
  [[nodiscard]] AABB lastVoxelBoundsForTest() const;
  [[nodiscard]] int lastMarkerLayerCountForTest() const;
  [[nodiscard]] int lastMarkerInstanceCountForTest() const;
  [[nodiscard]] int lastMarkerStreamVertexCountForTest() const;
  [[nodiscard]] AABB lastMarkerBoundsForTest() const;
  [[nodiscard]] int lastModelLayerCountForTest() const;
  [[nodiscard]] int lastModelDrawCountForTest() const;
  [[nodiscard]] int lastModelTriangleCountForTest() const;
  [[nodiscard]] std::array<int, 5> lastModelTextureSlotCountsForTest() const;
  [[nodiscard]] AABB lastModelBoundsForTest() const;
  // The marker, model and grid passes themselves, for their cache tests. Never null.
  [[nodiscard]] MarkerRhiPass& markerPassForTest() const {
    return *marker_pass_;
  }
  [[nodiscard]] GridRhiPass& gridPassForTest() const {
    return *grid_pass_;
  }
  [[nodiscard]] ModelRhiPass& modelPassForTest() const {
    return *model_pass_;
  }
  [[nodiscard]] bool lastShadowActiveForTest() const {
    return shadow_stage_.last_active;
  }
  [[nodiscard]] bool shadowResourcesReadyForTest() const {
    return shadow_texture_ != nullptr && shadow_render_target_ != nullptr;
  }
  [[nodiscard]] bool lastShadowFitValidForTest() const {
    return last_shadow_fit_valid_;
  }
  [[nodiscard]] int shadowMapSizeForTest() const {
    return shadow_map_size_;
  }
  // Shadow-caster draws / triangles of the last prepared frame.
  [[nodiscard]] int lastShadowDrawCountForTest() const;
  [[nodiscard]] int lastShadowTriangleCountForTest() const;
  [[nodiscard]] AABB lastShadowBoundsForTest() const {
    return last_shadow_bounds_;
  }
  [[nodiscard]] const QString& shadowCapabilityErrorForTest() const {
    return shadow_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t shadowResourceGenerationForTest() const {
    return shadow_stage_.resource_generation;
  }
  [[nodiscard]] bool lastHdrActiveForTest() const {
    return hdr_stage_.last_active;
  }
  [[nodiscard]] bool hdrResourcesReadyForTest() const {
    return hdr_render_target_ != nullptr && hdr_depth_render_target_ != nullptr && hdr_resolve_color_ != nullptr &&
           hdr_resolve_depth_ != nullptr && hdr_resolve_coverage_ != nullptr;
  }
  [[nodiscard]] QSize hdrRenderSizeForTest() const {
    return hdr_render_size_;
  }
  [[nodiscard]] std::uint64_t hdrAllocatedBytesForTest() const {
    return hdr_allocated_bytes_;
  }
  [[nodiscard]] const QString& hdrCapabilityErrorForTest() const {
    return hdr_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t hdrResourceGenerationForTest() const {
    return hdr_stage_.resource_generation;
  }
  void setForceHdrFallbackForTest(bool force);
  /// Keeps the depth replay where the depth-resolve fast path is available
  /// (rebuilds the HDR chain).
  void setForceDepthReplayForTest(bool force);
  /// The live HDR chain resolves its multisample depth instead of replaying the scene.
  [[nodiscard]] bool depthResolveActiveForTest() const {
    return hdr_depth_resolve_ && hdr_stage_.last_active;
  }
  [[nodiscard]] bool lastSsaoActiveForTest() const {
    return ssao_stage_.last_active;
  }
  [[nodiscard]] bool ssaoResourcesReadyForTest() const {
    return ssao_raw_render_target_ != nullptr && ssao_pipeline_ != nullptr && ssao_shader_resources_ != nullptr &&
           ssao_uniform_buffer_ != nullptr;
  }
  [[nodiscard]] const QString& ssaoCapabilityErrorForTest() const {
    return ssao_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t ssaoResourceGenerationForTest() const {
    return ssao_stage_.resource_generation;
  }
  void setForceSsaoFallbackForTest(bool force);
  // Same acceptance-only fault injection for the shadow chain: forces the
  // capability error the specs assert on and releases the shadow resources.
  void setForceShadowFallbackForTest(bool force);
  [[nodiscard]] bool lastEdlActiveForTest() const {
    return edl_stage_.last_active;
  }
  [[nodiscard]] bool edlResourcesReadyForTest() const {
    return edl_mesh_mask_render_target_ != nullptr && edl_mesh_mask_triangle_pipeline_ != nullptr &&
           edl_mesh_mask_line_pipeline_ != nullptr;
  }
  [[nodiscard]] const QString& edlCapabilityErrorForTest() const {
    return edl_stage_.capability_error;
  }
  [[nodiscard]] const QString& renderingWarningForTest() const {
    return rendering_warning_;
  }
  [[nodiscard]] std::uint64_t edlResourceGenerationForTest() const {
    return edl_stage_.resource_generation;
  }
  void setForceEdlFallbackForTest(bool force);
  // Topic ids of the layers that submitted at least one colour draw (direct or
  // HDR) last frame, in logical (dock) order even when a translucent draw was
  // deferred. The depth replay, which skips no-depth-write draws, does not count.
  [[nodiscard]] const std::vector<std::uint32_t>& lastSubmittedLayerIdsForTest() const {
    return last_submitted_layer_ids_;
  }
  // QRhi::backendName() of the last initialize() ("OpenGL", "Vulkan", ...):
  // the backend actually used, which can differ from api() after a fallback.
  // Kept after releaseResources(), so it survives an offscreen grab.
  [[nodiscard]] const QString& rhiBackendNameForTest() const {
    return rhi_backend_name_;
  }
  // Every pipeline that draws into the widget's own render target (the direct
  // scene pipelines, the overlays, the HDR present) was built for the current
  // one: its render pass descriptor and sample count.
  [[nodiscard]] bool pipelinesBuiltForTargetForTest() const;
  // Output pixel rectangle (top-left origin) of each overlay image drawn last
  // frame, in OverlayRhiPass::Slot order (perf HUD, hover label, hover ring);
  // empty for a slot that drew nothing.
  [[nodiscard]] const std::array<QRect, 3>& lastOverlayRectsForTest() const {
    return last_overlay_rects_;
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);
  void presentationChanged();
  void renderingWarningChanged(const QString& warning);
  // The hovered TF frame changed (see frameAt); empty when the cursor left every frame.
  void hoveredFrameChanged(const QString& frame);
  // Right-click landed on a TF frame (frameAt). The dock builds and shows the
  // menu; the view stays menu-free. Empty-space right-clicks are not emitted.
  void frameContextMenuRequested(const QString& frame, const QPoint& global_pos);

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;
  // A Show on the same QRhi at the same size (a hidden workspace tab shown again)
  // presents the frame drawn before the hide at once, instead of QRhiWidget's
  // synchronous full re-render; the frame that follows picks up what changed while
  // hidden. An ADS dock-area tab switch reparents instead: a new initialize().
  bool event(QEvent* event) override;
  // Clears the hover label when the cursor leaves the view.
  void leaveEvent(QEvent* event) override;
  // 'P' toggles the perf HUD.
  void keyPressEvent(QKeyEvent* event) override;
  // Accepts only on a TF frame (emits frameContextMenuRequested); otherwise
  // ignore()s so the event reaches the host dock's standard context menu.
  void contextMenuEvent(QContextMenuEvent* event) override;

 private:
  // Per-frame state of the frame contract (rhi/rhi_scene_pass.h), defined in
  // the .cpp: what admission decided, the frame's camera matrices, and the
  // prepared draws every pass replays.
  struct FrameAdmission;
  struct FrameView;
  struct PreparedFrame;

  // Shared placement step of the per-family render loops; see the definition.
  [[nodiscard]] std::optional<Transform> resolveFixedFromSource(
      const std::string& source_frame, const std::string& fixed_frame) const;
  // The follow frame in the effective fixed frame at `time`; nullopt when there
  // is no follow frame, no TF or no transform.
  [[nodiscard]] std::optional<Transform> followTransform(PJ::Timepoint time) const;
  // Where the fixed-frame `point` lands in logical widget pixels (current camera,
  // camera-relative like the drawn frame); nullopt when behind the camera.
  [[nodiscard]] std::optional<glm::vec2> projectToWidget(const glm::dvec3& point) const;
  using OrderedLayer = std::variant<
      PointRenderable*, PosesInFrameLayer*, OccupancyGridLayer*, VoxelGridLayer*, SceneEntitiesLayer*, ModelRenderable*,
      GridMapLayer*, TrailLayer*>;
  struct OrderedLayerEntry {
    OrderedLayer layer;
    std::uint32_t topic_id = 0;
  };

  // What every family's initialize() binds from this view on `rhi`.
  [[nodiscard]] RhiPassSetup passSetup(QRhi* rhi) const;
  // Every resource the scene families draw with exists (the overlays are optional).
  [[nodiscard]] bool baseResourcesReady(QRhi& rhi) const;
  // Creates the base resources on `rhi`; the name of the part that failed, or
  // nullptr. A failure leaves them partly built: releaseResources() next.
  [[nodiscard]] const char* createBaseResources(QRhi* rhi);
  // Enters (non-empty `reason`) or leaves the failure state; see the class
  // comment. `emit_failed` false when echoing QRhiWidget's own renderFailed().
  void setRenderFailure(QString reason, bool emit_failed = true);

  // Frame contract steps, once per render() in this order.
  void admitFrame(QRhi* rhi, FrameAdmission& admission);
  void applySceneBounds(const SceneBoundsCollector& bounds);
  [[nodiscard]] FrameView frameView(QRhi* rhi, const QSize& render_size) const;
  QRhiResourceUpdateBatch* prepareFrame(
      QRhi* rhi, const QSize& render_size, const FrameView& view, const FrameAdmission& admission,
      PreparedFrame& frame);
  void submitShadowPass(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates);
  void submitScenePass(
      QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target, ScenePass pass, const QColor& clear,
      QRhiResourceUpdateBatch* updates, const PreparedFrame& frame);
  void submitEdlMeshMask(QRhiCommandBuffer* command_buffer, const PreparedFrame& frame);

  // Calls `visitor` with each scene family (the passes with per-layer draws).
  template <typename Visitor>
  void forEachFamily(Visitor&& visitor) const;
  void applyFollow();
  [[nodiscard]] std::string effectiveFixedFrame() const;
  // Rebuilds the TF frame triads (drawn right after the grid prelude) when
  // their inputs changed since the last frame.
  void buildTfTriads(const glm::dvec3& render_origin, const std::string& fixed_frame);
  bool ensureShadowResources(QRhi* owner);
  void releaseShadowResources(bool restore_model_bindings);
  bool ensureHdrResources(QRhi* owner, const QSize& size);
  // Why `rhi` can never run the HDR chain; empty when it can (the chain may still
  // be refused at runtime: size, budget, a missing pipeline variant).
  [[nodiscard]] static QString hdrUnsupportedReason(QRhi& rhi);
  void releaseHdrTargetResources();
  void releaseHdrResources();
  // Drops the HDR chain for output `size` (a resize re-probes) with `reason` as
  // its warning, logged when it changes. Always false.
  bool rejectHdr(const QSize& size, QString reason);
  bool ensureSsaoResources(QRhi* owner, const QSize& size);
  void releaseSsaoTargetResources();
  void releaseSsaoResources();
  bool ensureEdlResources(QRhi* owner, const QSize& size);
  void releaseEdlTargetResources();
  void releaseEdlResources();
  void updateRenderingWarning();
  void setHoveredFrame(std::optional<std::string> frame);
  // Uploads the corner gizmo and this frame's HUD / hover-label images.
  void prepareOverlays(QRhiResourceUpdateBatch* updates, const QSize& output_size, const std::string& fixed_frame);
  void emitPresentationIfChanged(const CameraState& before);

  std::shared_ptr<TransformBuffer> tf_;
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;
  std::string follow_frame_;
  glm::dvec3 follow_previous_origin_{0.0};
  bool follow_seeded_ = false;

  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};
  CameraModel camera_model_ = CameraModel::kOrbit;
  AABB scene_bounds_{};
  CameraFitPolicy camera_fit_;

  GridStyle grid_style_ = GridStyle::kLines;
  float grid_extent_m_ = 10.0F;
  int grid_divisions_ = 10;
  bool grid_visible_ = true;
  bool axes_visible_ = true;
  float gizmo_size_m_ = 0.15F;
  float gizmo_opacity_ = 1.0F;
  bool tf_connections_visible_ = true;
  MeshShadingParams shading_params_;

  uint64_t last_frames_revision_ = ~uint64_t{0};
  QList<FrameRow> last_frame_list_;
  mutable std::vector<std::string> tf_render_key_frames_;

  QRhi* resource_rhi_ = nullptr;
  QString rhi_backend_name_;
  // The widget target the passes were built for. setSampleCount() or
  // setColorBufferFormat() recreate it on the same QRhi, and initialize() then
  // rebuilds every resource bound to the old one.
  PJ::RhiTargetKey built_target_;
  QRhiBuffer* uniform_buffer_ = nullptr;
  QRhiBuffer* render_mode_uniform_buffer_ = nullptr;
  QRhiBuffer* annotation_render_mode_uniform_buffer_ = nullptr;
  int render_mode_hdr_written_ = -1;  // the hdr flag both render-mode blocks hold; -1: unwritten
  // Every family's direct pipelines and their HDR-colour and depth-replay clones.
  std::unique_ptr<ScenePipelineRegistry> pipelines_;
  // Unit meshes shared by the families (cube, cube fan, arrow).
  std::unique_ptr<RhiSharedGeometry> shared_geometry_;
  // Camera/lighting/shadow parameters: binding 6 of the model materials,
  // binding 1 of the grid shadow receiver.
  QRhiBuffer* model_uniform_buffer_ = nullptr;
  QRhiTexture* shadow_texture_ = nullptr;
  QRhiSampler* shadow_sampler_ = nullptr;
  QRhiTextureRenderTarget* shadow_render_target_ = nullptr;
  QRhiRenderPassDescriptor* shadow_render_pass_descriptor_ = nullptr;
  QRhiBuffer* shadow_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* shadow_shader_resources_ = nullptr;
  QRhiRenderBuffer* hdr_msaa_color_ = nullptr;
  QRhiRenderBuffer* hdr_msaa_depth_ = nullptr;     ///< the depth replay's chain
  QRhiTexture* hdr_msaa_depth_texture_ = nullptr;  ///< the depth-resolve chain's
  QRhiTexture* hdr_resolve_color_ = nullptr;
  QRhiTexture* hdr_resolve_depth_ = nullptr;
  QRhiTexture* hdr_resolve_coverage_ = nullptr;
  QRhiTextureRenderTarget* hdr_render_target_ = nullptr;
  // The pass that fills hdr_resolve_coverage_ before SSAO and the EDL mask: the
  // depth replay (coverage + depth), or on the depth-resolve chain a clear of the
  // coverage alone (the HDR pass resolved the depth, present reads coverage from it).
  QRhiTextureRenderTarget* hdr_depth_render_target_ = nullptr;
  bool hdr_depth_resolve_ = false;  ///< the chain resolves depth (QRhi::ResolveDepthStencil)
  bool force_depth_replay_for_test_ = false;
  QRhiTextureRenderTarget* ssao_raw_render_target_ = nullptr;
  QRhiTextureRenderTarget* edl_mesh_mask_render_target_ = nullptr;
  QRhiRenderPassDescriptor* hdr_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* hdr_depth_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* ssao_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* edl_mesh_mask_render_pass_descriptor_ = nullptr;
  QRhiSampler* hdr_color_sampler_ = nullptr;
  QRhiSampler* hdr_coverage_sampler_ = nullptr;
  QRhiBuffer* composite_uniform_buffer_ = nullptr;
  QRhiBuffer* ssao_uniform_buffer_ = nullptr;
  bool ssao_kernel_pending_ = false;  // the constant kernel is written once per buffer
  QRhiShaderResourceBindings* present_shader_resources_ = nullptr;
  QRhiShaderResourceBindings* ssao_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* present_pipeline_ = nullptr;
  QRhiGraphicsPipeline* ssao_pipeline_ = nullptr;
  QRhiGraphicsPipeline* edl_mesh_mask_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* edl_mesh_mask_line_pipeline_ = nullptr;
  int last_line_vertex_count_ = 0;
  int last_triangle_vertex_count_ = 0;
  int last_resolved_frame_count_ = 0;
  std::vector<OrderedLayerEntry> ordered_layers_;
  std::vector<std::uint32_t> last_submitted_layer_ids_;
  // Reused across frames: the prepared frame and the per-slot "submitted" marks.
  std::unique_ptr<PreparedFrame> frame_;
  std::vector<char> submitted_slots_;

  std::unique_ptr<PointRhiPass> point_pass_;
  int camera_fit_count_ = 0;

  // Pose triads plus the TF "Frames" gizmos, drawn as solid instanced arrow
  // triads (the desktop look) with the pose pipeline and arrow mesh.
  std::unique_ptr<PoseRhiPass> pose_pass_;
  // TF triad arms of the current frame, each frame's fixed-frame transform
  // (relative to the render origin) baked into its instance model.
  std::vector<PoseTriadInstance> tf_triad_instances_;
  std::vector<std::string> tf_triad_frames_;
  // What tf_triad_instances_ were built from; the revision moves on each rebuild.
  struct TfTriadKey {
    const TransformBuffer* tf = nullptr;
    uint64_t tf_revision = 0;
    PJ::Timepoint time{};
    std::string fixed_frame;
    glm::dvec3 render_origin{0.0};
    bool axes_visible = false;
    float size = 0.0F;
    float opacity = 0.0F;
    std::optional<std::string> hovered_frame;

    bool operator==(const TfTriadKey&) const = default;
  };
  std::optional<TfTriadKey> tf_triad_key_;
  std::uint64_t tf_triad_revision_ = 0;

  std::unique_ptr<OccupancyRhiPass> occupancy_pass_;
  // Grid + TF connection lines, drawn first in every scene pass.
  std::unique_ptr<GridRhiPass> grid_pass_;
  std::unique_ptr<GridMapRhiPass> gridmap_pass_;
  // Standalone and pose-embedded trails.
  std::unique_ptr<TrailRhiPass> trail_pass_;

  std::unique_ptr<VoxelRhiPass> voxel_pass_;
  int max_3d_texture_size_ = 0;

  std::unique_ptr<MarkerRhiPass> marker_pass_;
  std::unique_ptr<ModelRhiPass> model_pass_;
  bool show_paint_pending_ = false;  // inside QRhiWidget's Show handling (event())
  QSize last_frame_size_;            // output size of the last full frame; empty: none since the release
  bool last_shadow_fit_valid_ = false;
  int shadow_map_size_ = 0;
  AABB last_shadow_bounds_;
  // Last shadow failure already forwarded to the caster layers, so a persistent
  // error is noted once instead of re-marked every frame.
  QString noted_shadow_failure_;
  QSize hdr_render_size_;
  std::uint64_t hdr_allocated_bytes_ = 0;
  QString rendering_warning_;
  // Why the base resources could not be built; set only in the failure state.
  PJ::RhiFailureLatch render_failure_;
  // Why the overlays (corner gizmo, HUD, hover label) are off; empty while they draw.
  QString overlay_failure_;

  // Bookkeeping every optional quality stage carries: whether it ran last
  // frame, the acceptance-test fallback override, the size-keyed failure latch
  // (a resize re-probes — a failure never blocks the stage for the QRhi's whole
  // lifetime), and the resource generation the probes report.
  struct QualityStageState {
    bool last_active = false;
    bool force_fallback_for_test = false;
    QSize rejected_size;
    QString capability_error;
    std::uint64_t resource_generation = 0;
    // True while the stage must stay down for this output size: a failure is
    // latched only against the size it happened at.
    [[nodiscard]] bool blockedFor(const QSize& size) const {
      return !capability_error.isEmpty() && rejected_size == size;
    }
  };
  QualityStageState shadow_stage_;
  QualityStageState hdr_stage_;
  QualityStageState ssao_stage_;
  QualityStageState edl_stage_;

  std::unique_ptr<OverlayRhiPass> overlay_pass_;
  CompositeParams composite_params_;
  float render_scale_ = 1.0F;
  bool show_perf_hud_ = false;
  QElapsedTimer cpu_timer_;
  MovingAverage cpu_average_;
  MovingAverage gpu_average_;
  // Overlay images are re-rasterised only when their content changes, so the
  // pass keeps their textures (keyed by QImage::cacheKey()).
  QStringList hud_lines_;
  QImage hud_image_;
  // Paces the HUD text refresh; invalid while the HUD is hidden, so the first
  // frame after showing it refreshes at once.
  QElapsedTimer hud_refresh_timer_;
  std::string hover_label_frame_;
  QImage hover_label_image_;
  QImage hover_ring_image_;
  qreal overlay_image_dpr_ = 0.0;
  std::array<QRect, 3> last_overlay_rects_{};
  std::optional<std::string> hovered_frame_;
  mutable std::vector<std::string> pick_frames_;
  mutable std::vector<glm::vec2> pick_points_;

  QPoint last_mouse_position_;
  Qt::MouseButton active_button_ = Qt::NoButton;
  bool camera_gesture_changed_ = false;
};

}  // namespace pj::scene3d
