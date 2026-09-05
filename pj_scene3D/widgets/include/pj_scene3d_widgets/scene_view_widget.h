#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#ifdef PJ_TARGET_WASM

#include "pj_scene3d_widgets/scene_view_widget_rhi.h"

#else

#include <QElapsedTimer>
#include <QList>
#include <QOpenGLWidget>
#include <QPoint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/time.hpp"  // PJ::Timepoint
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/frame_picking.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/gpu_profiler.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/moving_average.h"
#include "pj_scene3d_widgets/passes/axis_overlay_pass.h"
#include "pj_scene3d_widgets/passes/axis_render_pass.h"
#include "pj_scene3d_widgets/passes/edl_pass.h"
#include "pj_scene3d_widgets/passes/grid_render_pass.h"
#include "pj_scene3d_widgets/passes/shadow_map_pass.h"
#include "pj_scene3d_widgets/passes/ssao_pass.h"
#include "pj_scene3d_widgets/passes/tf_connections_render_pass.h"
#include "pj_scene3d_widgets/scene_hdr_fbo.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace pj::scene3d {

class Scene3DLayer;

// Standalone Qt widget that renders the 3D scene: a grid at the fixed-frame
// origin, an XYZ axis triad per TF frame, and zero or more user layers
// (PointCloud topics, future URDF robots, etc.). Layers are non-owning
// — SceneDockWidget owns them and Scene3DDockWidget pushes the base-owned
// render order into this view whenever it changes.
class SceneViewWidget : public QOpenGLWidget {
  Q_OBJECT

 public:
  using GridStyle = GridRenderPass::Style;

  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(PJ::Timepoint t);
  void setFixedFrame(const std::string& frame);

  // Camera "follow a frame" (RViz / Foxglove style). Position-only for now: each
  // tracker tick the active camera's anchor tracks `frame`'s origin in the fixed
  // frame, so the view pans to keep the frame in place while orbit/zoom stay
  // world-referenced. Empty `frame` = off. The followed pose is resolved against
  // the fixed frame, so changing EITHER the fixed frame or the follow frame
  // re-seeds (the next tick applies no shift → no jump). The FollowMode enum is
  // kept for future Heading/Pose modes; today a non-empty frame implies kPosition.
  enum class FollowMode { kOff, kPosition };
  void setFollowFrame(const std::string& frame);
  [[nodiscard]] const std::string& followFrame() const {
    return follow_frame_;
  }
  // Repaint-gate fingerprint of the followed frame's pose at `time` — 0 when not
  // following or unresolvable. Folded into the dock's viewRenderKey so a moving
  // follow target forces a repaint even when the TF overlay is hidden (the camera
  // moves but no layer's renderKey changed). GUI-thread only; no allocation.
  [[nodiscard]] uint64_t followRenderKey(PJ::Timepoint time) const;
  // Recenter the camera on the followed frame: move the camera's anchor to that
  // frame's current origin (resolved in the fixed frame at render_time_), keeping
  // orbit angle / zoom. The on-demand counterpart to the no-jump-on-enable follow
  // policy — the "Follow frame" recenter button drives it. No-op when not following
  // or the frame can't be resolved.
  void recenterOnFollowFrame();

  // Replace the render order: index 0 renders first (behind), the last on top.
  // For coplanar overlays (costmaps) this is what decides the overlap winner.
  void setLayers(const std::vector<Scene3DLayer*>& ordered);

  [[nodiscard]] const std::vector<Scene3DLayer*>& layers() const {
    return layers_;
  }

  AxisRenderPass& axisPass() {
    return axes_;
  }
  GridRenderPass& gridPass() {
    return grid_;
  }
  SsaoPass& ssaoPass() {
    return ssao_;
  }
  EdlPass& edlPass() {
    return edl_;
  }

  // Composite/look knobs (read in paintGL; call update() after changing).
  // Defaults are the User's 2026-06-10 look-dev pick (mesh_viewer demo).
  struct CompositeParams {
    int tonemap_mode = look::kTonemapMode;  // 0 None, 1 ACES, 2 AgX, 3 Khronos PBR Neutral
    float exposure = look::kExposure;
    float saturation = look::kSaturation;  // post-tonemap; data pixels only
    float ao_strength = look::kAoStrength;
    bool ssao_enabled = true;
    bool edl_enabled = true;
    float edl_floor = look::kEdlFloor;  // EDL darkens toward floor*color (0 = old black; 1 = off)
  };
  [[nodiscard]] CompositeParams& compositeParams() {
    return composite_params_;
  }

  // This view's mesh/collision look knobs (read in paintGL into ViewParams::shading;
  // call update() after changing). Per-view, so two 3D docks diverge independently —
  // the scene-controls panel drives only its bound view's copy.
  [[nodiscard]] MeshShadingParams& meshShadingParams() {
    return shading_params_;
  }
  void setMeshShadingParams(const MeshShadingParams& params);
  [[nodiscard]] ICamera& camera() {
    return *camera_;
  }

  // Selectable camera controllers. Enumerator order matches the combo-box order
  // in Scene3DDockWidget, so a combo index casts directly to a CameraModel.
  enum class CameraModel { kOrbit, kXyOrbit, kFly, kTopDownOrtho };
  // Switch the active controller, carrying the current pose across so the view
  // doesn't jump (capture state → construct → adoptState → swap → repaint).
  void setCameraModel(CameraModel model);
  [[nodiscard]] CameraModel cameraModel() const {
    return camera_model_;
  }
  void resetCamera();
  // Latest fixed-frame scene extent; forwarded to the active camera for adaptive
  // near/far and framing.
  void setSceneBounds(const AABB& bounds);
#ifdef PJ_SCENE3D_TEST_HOOKS
  [[nodiscard]] const AABB& sceneBoundsForTest() const {
    return scene_bounds_;
  }
#endif

  // Re-poll the TransformBuffer for the current frame set; emits
  // framesChanged if the set differs from the previous poll.
  void refreshAvailableFrames();

  // Repaint-gate fingerprint of the TF overlay (axis triads + parent-connection
  // lines) at `time`. These render the WHOLE frame forest posed at the tracker
  // time independently of any layer, so without this the dock's per-tick gate
  // would freeze a moving TF tree whenever every visible layer's own renderKey is
  // unchanged (a static/fixed-frame cloud, or a TF-only dock). Returns 0 when the
  // overlay is not drawn (no buffer, or both axes and connections hidden) so it
  // adds no cost and forgoes no coalescing then. Hashes exactly the transforms the
  // next render() would draw, so the key changes iff the rendered TF would — for a
  // moving tree during playback AND for new frames/edges arriving mid-load. Cheap
  // (O(frames), no decode); GUI-thread only (reads the shared TransformBuffer).
  [[nodiscard]] uint64_t tfRenderKey(PJ::Timepoint time) const;

  const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  // Part C scene-controls fan-out (call update() after changing).
  void setGridStyle(GridRenderPass::Style style) {
    const auto before = grid_.style();
    grid_.setStyle(style);
    if (grid_.style() != before) {
      emit presentationChanged();
    }
  }
  void setGridDivisions(int divisions) {
    const int before = grid_.divisions();
    grid_.setDivisions(divisions);
    if (grid_.divisions() != before) {
      emit presentationChanged();
    }
  }
  void setGridExtentMetres(float extent_m) {
    const float before = grid_.extentMetres();
    grid_.setExtentMetres(extent_m);
    if (grid_.extentMetres() != before) {
      emit presentationChanged();
    }
  }
  void setGizmoSize(float length_m) {
    const float before = axes_.axisLength();
    axes_.setAxisLength(length_m);
    if (axes_.axisLength() != before) {
      emit presentationChanged();
    }
  }
  void setGizmoOpacity(float opacity) {
    const float before = axes_.opacity();
    axes_.setOpacity(opacity);
    if (axes_.opacity() != before) {
      emit presentationChanged();
    }
  }
  void setGridVisible(bool visible) {
    if (grid_visible_ == visible) {
      return;
    }
    grid_visible_ = visible;
    emit presentationChanged();
  }

  // Per-view scene-control readback (the inverse of the setters above) so the
  // config panel can REFLECT the focused dock's own look on bind, and the dock
  // can persist it per-dock in xmlSaveState — each 3D view keeps independent
  // grid/frame/mesh settings rather than sharing one global look.
  [[nodiscard]] bool gridVisible() const {
    return grid_visible_;
  }
  [[nodiscard]] GridRenderPass::Style gridStyle() const {
    return grid_.style();
  }
  [[nodiscard]] int gridDivisions() const {
    return grid_.divisions();
  }
  [[nodiscard]] float gridExtentMetres() const {
    return grid_.extentMetres();
  }
  [[nodiscard]] float gizmoSize() const {
    return axes_.axisLength();
  }
  [[nodiscard]] float gizmoOpacity() const {
    return axes_.opacity();
  }

  // Show/hide the per-frame TF axis triads. The TF buffer is still used to
  // transform layers regardless — this only gates drawing the axes. Default
  // visible (TF is a first-class always-on display; see docs/REQUIREMENTS.md §4).
  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }

  // Show/hide the magenta lines connecting each TF frame to its parent (rviz2 /
  // Foxglove "show parent connections"). Independent of axesVisible — the lines
  // can show with the triads hidden and vice versa. Default visible.
  void setTfConnectionsVisible(bool visible);
  [[nodiscard]] bool tfConnectionsVisible() const {
    return tf_connections_visible_;
  }

  // ---- Performance instrumentation & anti-aliasing benchmark hooks ----------
  // These exist to MEASURE rendering cost (GPU/CPU ms per frame) and to sweep
  // anti-aliasing settings; none change the default app look. The HUD is an
  // off-by-default overlay; render scale and the explicit sample override are
  // driven only by the mesh_viewer --benchmark sweep.

  // Toggle the on-screen GPU/CPU timing overlay (drawn over the scene each
  // frame). Also bound to the 'P' key. Off by default.
  void setShowPerfHud(bool on);
  [[nodiscard]] bool showPerfHud() const {
    return show_perf_hud_;
  }

  // Supersampling factor: the off-screen HDR chain renders at device_px * scale
  // and the present pass downsamples it to device resolution (true SSAA — it
  // anti-aliases both silhouettes and in-triangle shading). 1.0 = off (no
  // behavior change). Benchmark/measurement knob; not wired into the app UI.
  void setRenderScale(float scale);
  [[nodiscard]] float renderScale() const {
    return render_scale_;
  }

  // Override the off-screen MSAA sample count (1 disables MSAA; 2/4/8 typical).
  // The scene FBO is independent of the window's backing FBO, so this can be
  // swept at runtime without recreating the context. Reconfigures immediately
  // when a context is current. Benchmark knob; not wired into the app UI.
  void setSceneSamples(int samples);
  // The sample count the chain is actually allocated at (after driver clamping);
  // 1 when single-sampled.
  [[nodiscard]] int achievedSceneSamples() const;

  // Whole-scene-render GPU time (ms), measured non-stalling via a GL_TIME_ELAPSED
  // query and smoothed by a moving average. 0 until enough frames elapse.
  [[nodiscard]] double gpuFrameMillis() const {
    return scene_profiler_.averageMillis();
  }
  [[nodiscard]] bool hasGpuResult() const {
    return scene_profiler_.hasResult();
  }

  // Test seam: the shadow-map texture id the LAST paintGL handed to shadow
  // receivers. Non-zero only when the shadow pre-pass actually ran AND fit a valid
  // light frustum to a caster; 0 when shadows are off, nothing cast, or — the
  // regression this guards — the scene has no TransformBuffer (so no layer may pose
  // a caster). Lets the persistence test assert "deleting the data clears the floor
  // shadow" without fragile shadow-pixel thresholds under software GL.
  [[nodiscard]] unsigned lastShadowMapIdForTest() const {
    return last_shadow_map_id_;
  }
  // CPU wall-time submitting the scene in paintGL (ms), smoothed with the same
  // moving average as the GPU readout. Should stay ~flat across MSAA settings —
  // the AA cost is the GPU's, not the CPU's. Excludes the HUD draw.
  [[nodiscard]] double cpuFrameMillis() const {
    return cpu_avg_.average();
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);
  void presentationChanged();
  // Right-click landed on a TF frame gizmo (hover-pick radius). The dock builds
  // and shows the menu; the view stays menu-free. Empty-space right-clicks are
  // NOT emitted — contextMenuEvent ignore()s them so the host dock's standard
  // Split/Clear menu still appears.
  void frameContextMenuRequested(const QString& frame, const QPoint& global_pos);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;

  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  // Clears any TF frame hover label when the cursor leaves the view, so a stale
  // name doesn't linger after the mouse moves off the widget.
  void leaveEvent(QEvent* event) override;
  // Accepts ONLY when a frame gizmo is under the cursor (emits
  // frameContextMenuRequested); otherwise ignore()s so the event falls through
  // to the host dock's standard context menu.
  void contextMenuEvent(QContextMenuEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  // Clears the active-gesture latch (and commits a pending camera change via
  // presentationChanged, same as a normal matching release). Shared by
  // mouseReleaseEvent and mouseMoveEvent's defensive check for a lost release.
  void endActiveGesture();

  // Per-tick "follow a frame" application (called from setTrackerTime). Resolves
  // the follow target's origin in the fixed frame at render_time_ and shifts the
  // active camera by the delta vs the previous tick's origin (camera_->followShift),
  // so the user's orbit/zoom/pan is preserved and only the frame's motion is added.
  // Seeds (applies no shift) on the first tick after enabling / a fixed-frame change
  // / a lookup gap. No-op when follow is off or the lookup fails (camera holds).
  void applyFollow();

  // Draw the GPU/CPU timing overlay with QPainter on top of the rendered scene.
  // Called at the end of paintGL when show_perf_hud_; uses the latest harvested
  // profiler result (a few frames stale, which is imperceptible for a HUD).
  void drawPerfHud();
  // Hover hit-test: project every resolvable TF frame origin with the last
  // painted proj*view and pick the one under `pos_logical` (logical widget
  // pixels). Updates hovered_frame_ and repaints only when the result changes.
  // A no-op (and clears any hover) when axes are hidden or there is no TF buffer.
  void updateHoverFrame(const QPointF& pos_logical);
  // The pick core shared by hover and the context menu: projects every
  // resolvable frame origin through the cached last_view_proj_ and returns the
  // frame within kHoverRadiusPx of the cursor, if any. Const with mutable
  // scratch (hover_all_frames_/hover_points_) so both call sites stay
  // allocation-free.
  [[nodiscard]] std::optional<std::string> pickFrameAt(const QPointF& pos_logical) const;
  // Draw the hovered TF frame's name in a small HUD box anchored at the frame's
  // CURRENT projected position (re-projected from frame_ctx so it tracks the
  // frame as the scene streams / camera moves). QPainter over the presented FBO,
  // same 2D-over-3D path as drawPerfHud. No-op when nothing is hovered or the
  // frame no longer resolves. Call last in paintGL, after all GL submission.
  void drawHoverLabel(const FrameContext& frame_ctx);
  // Close the GPU timer, record the CPU submission time, and draw the HUD — the
  // shared tail of both paintGL exits. Gated on show_perf_hud_.
  void finishFrameInstrumentation();
  // Release every pass's and layer's GL resources, returning them to their
  // pre-initializeGL state. Connected to the current QOpenGLContext's
  // aboutToBeDestroyed (rewired per context in initializeGL) and also called
  // from the destructor. QOpenGLWidget recreates its context on every reparent
  // (ADS dock/float/split) and destroys it on teardown; VAOs/FBOs aren't shared
  // across contexts, so stale handles must be dropped and rebuilt.
  void releaseGlResources();
  // Compile the fullscreen passthrough used to present the resolved HDR color
  // into the backing FBO. Per-context; rebuilt by initializeGL after recreation.
  void initializePresentProgram();
  // Clear + draw the full pass/layer sequence into the currently bound target.
  // mask_alpha_writes applies the legacy Wayland alpha guard (direct-to-backing
  // fallback only; the off-screen path forces alpha=1.0 in the present shader).
  void renderScene(const ViewParams& view_params, const FrameContext& frame_ctx, bool mask_alpha_writes);

  // Owned passes that don't depend on the layer count.
  AxisRenderPass axes_;
  GridRenderPass grid_;
  TfConnectionsRenderPass tf_connections_;
  AxisOverlayPass overlay_;
  // Off-screen HDR render chain (Phase 0A): geometry renders into an RGBA16F +
  // DEPTH32F FBO at the backing FBO's achieved MSAA count, is resolved to
  // single-sample, and is presented to the backing FBO by the passthrough below.
  // Per-context, like every pass: released in releaseGlResources, rebuilt lazily.
  SceneHdrFbo scene_fbo_;
  // Fullscreen passthrough (empty-VAO triangle) presenting the resolved color
  // into the backing FBO; forces alpha=1.0 (the Wayland opaque-surface guard).
  std::optional<gl::Program> present_program_;
  gl::VertexArray present_vao_;
  // Screen-space AO over the resolved depth (Phase D); composite multiplies its
  // output into the HDR color. Degrades to no-AO when unavailable (u_has_ao=0).
  SsaoPass ssao_;
  // Eye-dome lighting over the resolved depth (Phase B); composite multiplies
  // its shade factor into the HDR color. Same degrade rule as SSAO.
  EdlPass edl_;
  // Mesh-shadow depth-map target (geometry PRE-pass, before renderScene — unlike the
  // post-pass ssao_/edl_). Per-context; released in releaseGlResources(). Active only
  // when shading_params_.shadows_enabled and the light-frustum fit is valid.
  ShadowMapPass shadow_pass_;
  CompositeParams composite_params_;
  // Per-view mesh/collision look knobs, copied into ViewParams::shading each paintGL.
  MeshShadingParams shading_params_;

  // Non-owning layer registry, in the order supplied by SceneDockWidget.
  std::vector<Scene3DLayer*> layers_;

  // Active camera controller (one of the CameraModel kinds). Owned; swapped by
  // setCameraModel(). Defaults to the improved Orbit.
  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};
  CameraModel camera_model_{CameraModel::kOrbit};

  // Latest scene extent, retained so a camera-model swap can re-apply it to the
  // freshly constructed controller (bounds are not part of CameraState).
  AABB scene_bounds_{};

  std::shared_ptr<TransformBuffer> tf_;
  // The time the scene renders at. Distinct from the global playhead: the dock
  // pushes a clamped time via setTrackerTime, and paint is async from ticks, so
  // this is render state, not the clock. Fed to the per-frame FrameContext.
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;
  // "Follow a frame" state. follow_frame_ empty = not following (today a non-empty
  // frame implies FollowMode::kPosition; the enum reserves the slot for future
  // Heading/Pose modes). The camera tracks this frame's origin (resolved in
  // fixed_frame_) each tick. follow_prev_origin_ is that origin at the previous
  // APPLIED tick; the delta to it is what gets fed to camera_->followShift.
  // follow_seeded_ is false until the first lookup seeds follow_prev_origin_, so
  // enabling follow or changing the fixed frame applies no shift on its seeding
  // tick (no jump).
  std::string follow_frame_;
  glm::dvec3 follow_prev_origin_{0.0};
  bool follow_seeded_ = false;
  // Scratch frame list reused by tfRenderKey() each gate check to avoid a per-tick
  // heap allocation (mirrors how the render passes reuse getAllFrames(out)).
  mutable std::vector<std::string> tf_render_key_frames_;

  QList<FrameRow> last_frame_list_;

  // Whether the TF axis triads are drawn (see setAxesVisible). Does not affect
  // layer frame resolution, only the axes pass.
  bool axes_visible_ = true;
  // Whether the TF parent-connection lines draw (see setTfConnectionsVisible).
  bool tf_connections_visible_ = true;
  // Whether the ground grid draws (Part C "Grid" eye toggle).
  bool grid_visible_ = true;
  // One warning per context when the HDR chain is unavailable and paintGL falls
  // back to direct-to-backing rendering; re-armed by initializeGL.
  bool scene_fbo_fallback_logged_ = false;

  // ---- Performance instrumentation state (see the benchmark-hooks block) -----
  // Non-stalling GPU timer over the scene passes; per-context, released in
  // releaseGlResources() and rebuilt lazily.
  gl::GpuProfiler scene_profiler_;
  // CPU stopwatch over the paintGL submission span (excludes the HUD draw).
  QElapsedTimer cpu_timer_;
  // Smoothed CPU submission time (same moving average as the GPU readout).
  MovingAverage cpu_avg_;
  // Supersampling factor for the off-screen chain (1.0 = off; see setRenderScale).
  float render_scale_ = 1.0f;
  // Explicit MSAA override (-1 = follow the context's achieved sample count, the
  // default; >= 0 = forced by setSceneSamples for the benchmark sweep).
  int scene_samples_override_ = -1;
  // Whether the on-screen GPU/CPU timing overlay draws (toggle: 'P').
  bool show_perf_hud_ = false;

  QPoint last_mouse_pos_;
  Qt::MouseButton active_button_{Qt::NoButton};
  bool camera_gesture_changed_ = false;

  // ---- TF frame hover-label state ------------------------------------------
  // proj*view from the most recent paintGL, so the hover hit-test (a mouse-move
  // event, async from paint) projects frame origins with the exact matrices the
  // scene was last drawn with. Identity until the first paint. NOTE: this is the
  // CAMERA-RELATIVE matrix (built against render_origin_), so the hover hit-test
  // must project origins that have had render_origin_ subtracted — which it gets
  // for free because it resolves them through a FrameContext seeded with the same
  // render_origin_ (see updateHoverFrame / camera-relative rendering below).
  glm::mat4 last_view_proj_{1.0f};
  // World point subtracted from every transform when rendering this frame, so a
  // camera at large world coordinates (following a far frame, e.g. a UTM/GPS map)
  // doesn't lose geometry/view deltas to float32 cancellation. Set each paintGL to
  // the camera focal; the async hover hit-test reuses it to stay consistent with
  // last_view_proj_. {0,0,0} until the first paint = absolute world.
  glm::dvec3 render_origin_{0.0};
  // Shadow-map id the last paintGL passed to receivers (see lastShadowMapIdForTest).
  unsigned last_shadow_map_id_ = 0U;
  // The TF frame currently under the cursor, or empty when none. Holds only the
  // name; the label re-projects the live origin each paint so it stays glued.
  std::optional<std::string> hovered_frame_;
  // Reused across hover hit-tests to avoid per-event heap churn (the cursor can
  // emit many move events per second). hover_points_ is INDEX-ALIGNED with
  // hover_all_frames_ — a frame that doesn't project gets an off-screen sentinel
  // — so the winning pick index maps straight back to a frame name.
  mutable std::vector<std::string> hover_all_frames_;
  mutable std::vector<glm::vec2> hover_points_;

  // Connection to the current GL context's aboutToBeDestroyed signal. Rewired to
  // each new context in initializeGL and disconnected in the destructor so the
  // teardown hook never fires on a half-destroyed widget.
  QMetaObject::Connection context_cleanup_connection_;
};

}  // namespace pj::scene3d

#endif  // PJ_TARGET_WASM
