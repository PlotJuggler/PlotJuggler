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
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_widgets/grid_geometry.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/moving_average.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_tf/tf_buffer.h"
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

using pj::tf::FrameRow;
using pj::tf::Transform;
using pj::tf::TransformBuffer;

class Scene3DLayer;
class OccupancyRhiPass;
class MarkerRhiPass;
class ModelRhiPass;
class GridMapRhiPass;
class TrailRhiPass;
class GridRhiPass;
class OverlayRhiPass;
class PoseRhiPass;
class VoxelRhiPass;
class PointRhiPass;
class ScenePipelineRegistry;
class SceneViewTestAccess;
struct EdlResources;
struct HdrResources;
struct PlacementHold;
struct QualityStageState;
struct RhiPassSetup;
struct RhiSharedGeometry;
struct SceneContributions;
struct ShadowResources;
struct SsaoResources;
enum class ScenePass;

// The Scene3D view, a QRhiWidget on the desktop and in the browser. It owns
// every GPU resource of the scene (the layers own none); camera and TF semantics come from pj_scene3d_core. The frame
// contract and pass vocabulary live in rhi/rhi_scene_pass.h.
//
// Failure state: when the base pipeline set (every scene family, the shared
// geometry and uniforms) cannot be built on the current QRhi, the view submits
// no scene draw. It clears to the backdrop, shows the reason through
// renderingWarningChanged(), emits renderFailed() once, and retries on the next
// initialize() (a new QRhi, or a new output size, format or sample count).
// Optional stages (HDR and its pipeline variants, SSAO, EDL, shadows, overlays)
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
  // Latest fixed-frame scene extent; forwarded to the active camera for adaptive
  // near/far and framing. The dock owns it (Scene3DDockWidget::updateSceneBounds);
  // the camera is never fitted to it automatically.
  void setSceneBounds(const AABB& bounds);
  void setLayers(const std::vector<Scene3DLayer*>& ordered_layers);
  // The layers of the last setLayers(), in logical order.
  [[nodiscard]] const std::vector<Scene3DLayer*>& layers() const {
    return layers_;
  }

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

  // Present-pass look knobs (tonemap, exposure, saturation, AO/EDL): read every
  // frame, call invalidateContent() after changing.
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
  // is close enough. Projects with the last painted frame's camera, so a pick
  // matches the image on screen even after the camera moved; before the first
  // frame that is the identity projection.
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
  // QRhi::backendName() of the last initialize() ("OpenGL", "Vulkan", ...):
  // the backend actually used, which can differ from api() after a fallback.
  // Kept after releaseResources(), so it survives an offscreen grab.
  [[nodiscard]] const QString& rhiBackendName() const {
    return rhi_backend_name_;
  }

  // Marks what the view draws as changed and requests a frame. Every change to
  // the scene goes through here (the view's setters and camera moves, the dock's
  // refresh, async completions, a capture): a plain update() or an expose only
  // repaints, and a repaint of an unchanged view on an unchanged target presents
  // the retained frame without rendering one (paintEvent()). A hidden view
  // renders what changed on its next Show. Target changes (size, format, sample
  // count, DPR) render on their own, without an invalidation.
  void invalidateContent();

  // The reason the view currently draws less than it should (a failed stage,
  // family or overlay), or empty; renderingWarningChanged() reports each change.
  [[nodiscard]] const QString& renderingWarning() const {
    return rendering_warning_;
  }
  // The last setSceneBounds().
  [[nodiscard]] const AABB& sceneBounds() const {
    return scene_bounds_;
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
  // Marks the Show scope for paintEvent() (QRhiWidget paints synchronously inside it).
  bool event(QEvent* event) override;
  // Skips a paint, offscreen frame included, while the target holds the last
  // full frame (on the same live QRhi, at the size and format the paint would
  // render) and either nothing changed since (invalidateContent()) or the paint is the one
  // QRhiWidget sends synchronously inside a Show; that one queues the frame when
  // something changed while hidden. So a hidden tab shown again composites its
  // last frame at once and renders only what changed. A paint after a release,
  // a resize or a target change always renders.
  void paintEvent(QPaintEvent* event) override;
  // Clears the hover label when the cursor leaves the view.
  void leaveEvent(QEvent* event) override;
  // 'P' toggles the perf HUD.
  void keyPressEvent(QKeyEvent* event) override;
  // Accepts only on a TF frame (emits frameContextMenuRequested); otherwise
  // ignore()s so the event reaches the host dock's standard context menu.
  void contextMenuEvent(QContextMenuEvent* event) override;

 private:
  // Test-only access to the view's internals (pj_scene3d_widgets_testing): the
  // tests, the benchmarks and the browser acceptance probes read the last
  // frame's counters and force stage fallbacks through it, never the app.
  friend class SceneViewTestAccess;

  // The optional quality stages, for SceneViewTestAccess::forceStageFallback().
  enum class QualityStage { kHdr, kSsao, kEdl, kShadow };
  // Holds `stage` down (releasing its resources) as if it had failed, with a
  // capability error naming the override, or lets it build again.
  void forceStageFallback(QualityStage stage, bool force);
  // The bookkeeping of `stage`, held by its resource owner.
  [[nodiscard]] QualityStageState& stageState(QualityStage stage) const;

#ifdef PJ_RHI_TEST_HOOKS
  /// Stage stamps of the last render() and the cost of every initialize() that
  /// built the base resources, for the frame benchmarks' stage split (read
  /// through SceneViewTestAccess). Stamps are std::chrono::steady_clock
  /// nanoseconds, 0 until set. On OpenGL the recorded commands and their buffer
  /// uploads execute after render_end_ns, in the top-level's QRhi::endFrame().
  struct StageTimes {
    std::int64_t render_begin_ns = 0;
    std::int64_t targets_ready_ns = 0;  // the HDR chain's targets and pipelines exist (built on a resize or new QRhi)
    std::int64_t admitted_ns = 0;       // admitFrame(): layer prepareForRender() (sync decode and pack), placement
    std::int64_t prepared_ns = 0;       // prepareFrame() and the overlays: CPU streams, upload enqueue, uniforms
    std::int64_t render_end_ns = 0;     // every pass recorded (Vulkan copies uploads to staging here)
    // GPU time of the last completed frame of this widget's command buffer, ms;
    // 0 unless the QRhi has timestamps (QSG_RHI_PROFILE=1 for a QRhiWidget).
    double last_gpu_ms = 0.0;
    int base_builds = 0;              // initialize() calls that built the base resources (a new QRhi or target)
    double last_base_build_ms = 0.0;  // uniforms, shared meshes and every family's pipelines
    // Cumulative upload work, as of render_end_ns: point-cloud uploads and their
    // bytes, marker layer rebuilds (expansion and upload), model mesh builds.
    std::uint64_t point_uploads = 0;
    std::uint64_t point_upload_bytes = 0;
    std::uint64_t marker_rebuilds = 0;
    std::uint64_t model_mesh_builds = 0;
    // Cumulative callbacks, so a lifecycle operation (float, redock, show, tab
    // switch) counts every one it ran, not just the last: paint events received,
    // the paints skipped (no offscreen frame, paintEvent()), render() calls and
    // those that drew a full frame.
    std::uint64_t paint_events = 0;
    std::uint64_t paints_skipped = 0;
    std::uint64_t render_calls = 0;
    std::uint64_t full_renders = 0;
    // Shadowed frames that rendered the shadow map, and those that kept it.
    std::uint64_t shadow_passes = 0;
    std::uint64_t shadow_passes_reused = 0;
    // Frames withheld because a pipeline could not be created (keepLastFrameOrClear()).
    std::uint64_t frames_withheld = 0;
    // Cumulative ns of the full renders' stages (the spans between the stamps
    // above) and of every base build.
    std::int64_t targets_ns = 0;
    std::int64_t admit_ns = 0;
    std::int64_t prepare_ns = 0;
    std::int64_t submit_ns = 0;
    std::int64_t base_build_ns = 0;
  };
#endif

  // Per-frame state of the frame contract (rhi/rhi_scene_pass.h): what admission
  // decided (here, so SceneViewTestAccess can run admission without a QRhi), and,
  // defined in the .cpp, the frame's camera matrices and the prepared draws every
  // pass replays.
  // The effective fixed frame, resolved once per frame. Filled once by
  // admitFrame(); prepareFrame() reads it and no later step re-admits a layer.
  struct FrameAdmission {
    std::string fixed_frame;
  };
  struct FrameView;
  struct PreparedFrame;

  // Shared placement step of the per-family render loops; see the definition.
  [[nodiscard]] std::optional<Transform> resolveFixedFromSource(
      const std::string& source_frame, const std::string& fixed_frame, std::optional<PJ::Timepoint> object_stamp,
      PlacementHold* hold) const;
  // The follow frame in the effective fixed frame at `time`; nullopt when there
  // is no follow frame, no TF or no transform.
  [[nodiscard]] std::optional<Transform> followTransform(PJ::Timepoint time) const;
  // Where the fixed-frame `point` lands in logical widget pixels through the last
  // painted frame's camera; nullopt when behind it.
  [[nodiscard]] std::optional<glm::vec2> projectToWidget(const glm::dvec3& point) const;
  // What every family's initialize() binds from this view on `rhi`.
  [[nodiscard]] RhiPassSetup passSetup(QRhi* rhi) const;
  // The resources every frame binds exist: the uniforms, the shared meshes, and
  // the families that draw without a layer (the grid, the TF triads' pose pass).
  [[nodiscard]] bool baseResourcesReady() const;
  // Every family that has a layer in the view is built. The others are not: a
  // family's pipelines cost a program each (tens of ms per new QRhi on OpenGL)
  // and most scenes use two or three families.
  [[nodiscard]] bool familiesReady(QRhi& rhi) const;
  template <typename Visitor>
  void forEachLazyFamily(QRhi& rhi, Visitor&& visitor) const;
  // The render target holds this view's last full frame, on the live QRhi it was
  // drawn with, at the size and format the next paint would render.
  [[nodiscard]] bool retainsLastFrame() const;
  // The target QRhiWidget's next paint builds (its format, size and samples),
  // and the logical size and DPR the overlays are laid out for: a fixed
  // colour-buffer size keeps the target across a resize, not the overlays.
  struct TargetRequest {
    TextureFormat format = TextureFormat::RGBA8;
    QSize pixel_size;
    int samples = 1;
    QSize logical_size;
    qreal dpr = 1.0;
    bool operator==(const TargetRequest&) const = default;
  };
  [[nodiscard]] TargetRequest requestedTarget() const;
  // Creates what is missing on `rhi`: the base resources, then each family with
  // a layer, registering its pipelines. Their constant uploads are submitted on
  // `command_buffer` when every part was built, discarded otherwise. The name of
  // the part that failed, or nullptr; a failure leaves them partly built:
  // failScene() next. Runs in initialize(), and in render() for a family whose
  // first layer came later.
  [[nodiscard]] const char* createSceneResources(QRhi* rhi, QRhiCommandBuffer* command_buffer);
  // createSceneResources() minus the submission: records the constant uploads
  // into `constants` (RhiPassSetup::updates).
  [[nodiscard]] const char* createMissingResources(QRhi* rhi, QRhiResourceUpdateBatch* constants);
  // releaseResources() and the failure state for `failed_part`: only the
  // backdrop draws until the next initialize() (a new target or QRhi) retries.
  void failScene(const char* failed_part);
  // A frame that lost a pipeline is not presented: the target keeps the last
  // full frame where it holds one, else it is cleared to the backdrop.
  void keepLastFrameOrClear(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target);
  // An empty pass cleared to the theme backdrop: a frame that draws nothing.
  void clearToBackdrop(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target);
  // Enters (non-empty `reason`) or leaves the failure state; see the class
  // comment. `emit_failed` false when echoing QRhiWidget's own renderFailed().
  void setRenderFailure(QString reason, bool emit_failed = true);

  // Frame contract steps, once per render() in this order.
  void admitFrame(QRhi* rhi, FrameAdmission& admission);
  [[nodiscard]] FrameView frameView(QRhi* rhi) const;
  QRhiResourceUpdateBatch* prepareFrame(
      QRhi* rhi, const QSize& render_size, const FrameView& view, const FrameAdmission& admission,
      PreparedFrame& frame);
  void submitShadowPass(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates);
  void submitScenePass(
      QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target, ScenePass pass, const QColor& clear,
      QRhiResourceUpdateBatch* updates, const PreparedFrame& frame);
  void submitEdlMeshMask(QRhiCommandBuffer* command_buffer, const PreparedFrame& frame);

  // Calls `visitor` with each scene family (the passes with per-layer draws), in
  // the order one layer's draws submit.
  template <typename Visitor>
  void forEachFamily(Visitor&& visitor) const;
  void applyFollow();
  [[nodiscard]] std::string effectiveFixedFrame() const;
  // Rebuilds the TF frame triads (drawn right after the grid prelude) when
  // their inputs changed since the last frame.
  void buildTfTriads(const glm::dvec3& render_origin, const std::string& fixed_frame);
  bool ensureShadowResources(QRhi* owner);
  void releaseShadowResources(bool restore_model_bindings);
  // The next shadowed frame renders the shadow map again instead of keeping it.
  void discardShadowMap();
  bool ensureHdrResources(QRhi* owner, const QSize& size);
  // Why `rhi` can never run the HDR chain; empty when it can (the chain may still
  // be refused at runtime: size, budget, a missing pipeline variant).
  [[nodiscard]] static QString hdrUnsupportedReason(QRhi& rhi);
  // The per-size HDR part, after SSAO's and EDL's targets that reference it.
  void releaseHdrTargetResources();
  void releaseHdrResources();
  // Drops the HDR chain for output `size` (a resize re-probes) with `reason` as
  // its warning, logged when it changes. Always false.
  bool rejectHdr(const QSize& size, QString reason);
  bool ensureSsaoResources(QRhi* owner, const QSize& size);
  bool ensureEdlResources(QRhi* owner, const QSize& size);
  // EDL's target, after the model pass's mask pipelines that target its render pass.
  void releaseEdlResources();
  void updateRenderingWarning();
  void setHoveredFrame(std::optional<std::string> frame);
  // Uploads the corner gizmo, oriented by `view` (the render-origin-relative
  // view matrix), and this frame's HUD / hover-label images.
  void prepareOverlays(
      QRhiResourceUpdateBatch* updates, const QSize& output_size, const glm::mat4& view,
      const std::string& fixed_frame);
  void emitPresentationIfChanged(const CameraState& before);

  std::shared_ptr<TransformBuffer> tf_;
  // The cursor, in the entry clock (Clock::kEntry); a layer's object is placed at its own stamp instead
  // (resolveFixedFromSource()).
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;
  std::string follow_frame_;
  glm::dvec3 follow_previous_origin_{0.0};
  bool follow_seeded_ = false;

  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};
  CameraModel camera_model_ = CameraModel::kOrbit;
  AABB scene_bounds_{};
  // proj * view of the last painted frame, relative to last_render_origin_: what
  // frame picking projects with, so a pick matches the image on screen. Identity
  // and the world origin until the first frame.
  glm::mat4 last_view_projection_{1.0F};
  glm::dvec3 last_render_origin_{0.0};
  // The pair above as it was before the frame in flight: a withheld frame
  // (keepLastFrameOrClear) restores it, since its image never reaches the screen.
  glm::mat4 shown_view_projection_{1.0F};
  glm::dvec3 shown_render_origin_{0.0};

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
  // The owner of every scene pipeline: the direct ones and their HDR-colour and depth-replay variants.
  std::unique_ptr<ScenePipelineRegistry> pipelines_;
  // Unit meshes shared by the families (cube, cube fan, arrow).
  std::unique_ptr<RhiSharedGeometry> shared_geometry_;
  // Camera/lighting/shadow parameters: binding 6 of the model materials,
  // binding 1 of the grid shadow receiver.
  QRhiBuffer* model_uniform_buffer_ = nullptr;
  bool force_depth_replay_for_test_ = false;
  // The quality stages' resources, one owner each
  // (rhi/scene_view_widget_rhi_quality_p.h), released in the order
  // releaseResources() documents.
  std::unique_ptr<ShadowResources> shadow_;
  std::unique_ptr<HdrResources> hdr_;
  std::unique_ptr<SsaoResources> ssao_;
  std::unique_ptr<EdlResources> edl_;
  QRhiBuffer* ssao_uniform_buffer_ = nullptr;  // a base resource: its kernel is a constant upload
  int last_line_vertex_count_ = 0;
  int last_triangle_vertex_count_ = 0;
  int last_resolved_frame_count_ = 0;
  // The layers in logical (dock) order, and each one's registration
  // (Scene3DLayer::sceneContributions()), read once in setLayers().
  std::vector<Scene3DLayer*> layers_;
  std::vector<SceneContributions> layer_contributions_;
  std::vector<std::uint32_t> last_submitted_layer_ids_;
  // Reused across frames: the prepared frame and the per-slot "submitted" marks.
  std::unique_ptr<PreparedFrame> frame_;
  std::vector<char> submitted_slots_;

  std::unique_ptr<PointRhiPass> point_pass_;

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
  bool show_paint_pending_ = false;  // inside a Show event (event()), nested paint included
  bool content_changed_ = true;      // invalidateContent() since the last full frame
  // requestedTarget() when the last full frame drew; empty: none on this target.
  std::optional<TargetRequest> last_frame_request_;
  bool last_shadow_fit_valid_ = false;
  AABB last_shadow_bounds_;
  // Last shadow failure already forwarded to the caster layers, so a persistent
  // error is noted once instead of re-marked every frame.
  QString noted_shadow_failure_;
  QString rendering_warning_;
  // Why the base resources could not be built; set only in the failure state.
  PJ::RhiFailureLatch render_failure_;
  // Why the overlays (corner gizmo, HUD, hover label) are off; empty while they draw.
  QString overlay_failure_;

  std::unique_ptr<OverlayRhiPass> overlay_pass_;
  CompositeParams composite_params_;
  float render_scale_ = 1.0F;
  bool show_perf_hud_ = false;
  QElapsedTimer cpu_timer_;
#ifdef PJ_RHI_TEST_HOOKS
  StageTimes stage_times_;
#endif
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
