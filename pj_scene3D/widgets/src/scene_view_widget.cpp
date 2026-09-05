// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_view_widget.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QString>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_scene3d_core/camera/camera_math.h"  // toRenderSpace()
#include "pj_scene3d_core/shadow_camera.h"       // fitDirectionalShadowCamera, kShadowMapSize
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/debug.h"
#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"  // unuseProgram()
#include "pj_scene3d_widgets/hud_overlay.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {

Q_LOGGING_CATEGORY(lcSceneViewWidget, "pj.scene3d.scene_view")

// Desired MSAA for the offscreen scene HDR chain. SceneHdrFbo owns its own
// multisample textures (resolved to single-sample, then composited by a
// fullscreen draw), so its sample count is INDEPENDENT of the window/context: a
// QOpenGLWidget composited in an ADS dock reports context samples==0, yet the
// chain can still be 4x. Seeding from this constant rather than the context is
// what makes MSAA work docked, not just in the more top-level demo.
inline constexpr int kDefaultMsaaSamples = 4;

bool cameraStatesEqual(const CameraState& lhs, const CameraState& rhs) {
  return lhs.focal.x == rhs.focal.x && lhs.focal.y == rhs.focal.y && lhs.focal.z == rhs.focal.z &&
         lhs.radius == rhs.radius && lhs.azimuth == rhs.azimuth && lhs.elevation == rhs.elevation &&
         lhs.fov_y == rhs.fov_y && lhs.ortho_scale == rhs.ortho_scale && lhs.perspective == rhs.perspective;
}

QSurfaceFormat makeDefaultFormat() {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  // MSAA on the backing FBO. Composited in an ADS dock the app context negotiates
  // samples==0 regardless (Qt's QOpenGLWidget backing store is single-sample); the
  // scene's AA comes from the independent SceneHdrFbo chain, so this only governs
  // the rare direct-to-backing fallback. paintGL must glEnable(GL_MULTISAMPLE).
  fmt.setSamples(kDefaultMsaaSamples);
  fmt.setSwapInterval(1);  // vsync — caps render at ~60Hz on standard monitors
  // Request a debug context only when GL debug output is actually wanted; a
  // DebugContext has measurable CPU overhead on some drivers (extra validation
  // layer), so it must not be on by default in release builds.
  if (gl::debugOutputRequested()) {
    fmt.setOption(QSurfaceFormat::DebugContext);
  }
  return fmt;
}

constexpr std::string_view kPresentVertSrc = R"GLSL(
#version 450 core
out vec2 v_uv;

void main() {
  float x = float(gl_VertexID == 1) * 4.0 - 1.0;
  float y = float(gl_VertexID == 2) * 4.0 - 1.0;
  gl_Position = vec4(x, y, 0.0, 1.0);
  v_uv = vec2(x, y) * 0.5 + 0.5;
}
)GLSL";

// Composite/tonemap present (Phase 0B). The scene FBO now holds LINEAR-light
// HDR; this pass tonemaps and applies the single manual sRGB encode (the
// backing FBO is not sRGB-capable; GL_FRAMEBUFFER_SRGB stays disabled).
// AgX: adapted from three.js tonemapping_pars_fragment (MIT; Filament/Sobotka
// derived). ACES: Narkowicz (CC0). Neutral: Khronos PBR Neutral (Apache-2.0,
// via three.js). sRGB OETF: IEC 61966-2-1. Full license texts:
// pj_scene3D/THIRDPARTY.md. Will become CompositePass : IPostPass when EDL/SSAO
// inputs land (plan §A.6).
constexpr std::string_view kPresentFragSrc = R"GLSL(
#version 450 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_scene;
uniform sampler2D u_depth;
uniform sampler2D u_ao;
uniform bool u_has_ao = false;
uniform sampler2D u_edl;
uniform bool u_has_edl = false;
uniform int u_tonemap_mode = 1;  // 0 None, 1 ACES, 2 AgX, 3 Neutral
uniform float u_exposure = 1.1;
uniform float u_ao_strength = 1.0;
uniform float u_edl_floor;         // set each frame from CompositeParams::edl_floor (look::kEdlFloor)
uniform float u_saturation = 1.2;  // post-tonemap saturation boost

vec3 sRGB(vec3 c) {
  bvec3 k = lessThanEqual(c, vec3(0.0031308));
  return mix(1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(k));
}
vec3 ACES(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 agxContrast(vec3 x) {
  vec3 x2 = x * x;
  vec3 x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
// GLSL mat3 ctors are COLUMN-major; these match three.js's AgXInset/Outset
// columns exactly. (Transposing them tints greys blue: the transposed outset's
// blue row sums to ~1.22.)
const mat3 AGX_IN = mat3(0.856627, 0.137319, 0.111898, 0.0951212, 0.761242, 0.0767994, 0.0482516, 0.101439, 0.811302);
const mat3 AGX_OUT = mat3(1.127101, -0.141330, -0.141330, -0.110607, 1.157824, -0.110607, -0.016494, -0.016494, 1.251936);
const mat3 S2R = mat3(0.627404, 0.069097, 0.016392, 0.329282, 0.919540, 0.088013, 0.043314, 0.011361, 0.895595);
const mat3 R2S = mat3(1.660500, -0.124551, -0.018151, -0.587641, 1.132900, -0.100579, -0.072850, -0.008349, 1.118730);
vec3 AgX(vec3 c) {
  c = S2R * c;
  c = AGX_IN * c;
  c = max(c, vec3(1e-10));
  c = log2(c);
  c = clamp((c + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);
  c = agxContrast(c);
  c = AGX_OUT * c;
  c = pow(max(c, vec3(0.0)), vec3(2.2));
  c = R2S * c;
  return clamp(c, 0.0, 1.0);
}
// Khronos PBR Neutral tone mapper: preserves hue/saturation far better than
// ACES (which shifts saturated colors), which matters when the scene is full of
// turbo/viridis costmaps and colormapped clouds whose hue IS the data. Operates
// in linear; output is linear [0,1]. Source: Khronos glTF Sample Viewer, via
// three.js NeutralToneMapping.
vec3 PBRNeutral(vec3 c) {
  const float kStartCompression = 0.8 - 0.04;
  const float kDesaturation = 0.15;
  float x = min(c.r, min(c.g, c.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  c -= offset;
  float peak = max(c.r, max(c.g, c.b));
  if (peak < kStartCompression) {
    return c;
  }
  float d = 1.0 - kStartCompression;
  float new_peak = 1.0 - d * d / (peak + d - kStartCompression);
  c *= new_peak / peak;
  float g = 1.0 - 1.0 / (kDesaturation * (peak - new_peak) + 1.0);
  return mix(c, vec3(new_peak), g);
}
void main() {
  vec4 scene = texture(u_scene, v_uv);
  float depth = texture(u_depth, v_uv).r;
  vec3 hdr = scene.rgb * u_exposure;
  if (u_has_ao) {
    hdr *= mix(1.0, texture(u_ao, v_uv).r, u_ao_strength);
  }
  if (u_has_edl) {
    // Floor the eye-dome darkening so creases bottom out at a hue-preserving dark
    // grey (u_edl_floor * color) instead of pure black. u_edl_floor == 0 is the
    // original multiply-to-black behavior. EDL is mesh-only (the pass outputs 1
    // for non-mesh pixels), so point clouds / grid / background pass through
    // unchanged here.
    float edl = texture(u_edl, v_uv).r;  // shade factor (1 = untouched, →0 = max)
    hdr *= mix(u_edl_floor, 1.0, edl);
  }
  vec3 graded = u_tonemap_mode == 1   ? ACES(hdr)
                : u_tonemap_mode == 2 ? AgX(hdr)
                : u_tonemap_mode == 3 ? PBRNeutral(hdr)
                                      : clamp(hdr, vec3(0.0), vec3(1.0));
  float luma = dot(graded, vec3(0.2126, 0.7152, 0.0722));
  graded = clamp(mix(vec3(luma), graded, u_saturation), vec3(0.0), vec3(1.0));
  if (depth >= 0.999999) {
    graded = clamp(scene.rgb, vec3(0.0), vec3(1.0));
  }
  // scene.a is the annotation marker (TF axes/HUD write 0): bypass the grade so
  // synthetic markers keep their flat vivid colors, while data objects (alpha 1)
  // get the filmic look. MSAA resolve averages the marker, feathering the seam.
  vec3 ldr = mix(clamp(scene.rgb, vec3(0.0), vec3(1.0)), graded, scene.a);
  frag = vec4(sRGB(ldr), 1.0);  // alpha forced 1.0: the Wayland opaque-surface invariant
}
)GLSL";

// Cursor pick radius for TF frame hover labels, in logical pixels: a frame whose
// projected origin is within this distance of the cursor is labelled. Generous
// enough to grab a small triad without snapping across a dense frame cluster.
constexpr float kHoverRadiusPx = 20.0f;

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QOpenGLWidget(parent) {
  setFormat(makeDefaultFormat());
  setMinimumSize(320, 240);
  // Track motion with no button pressed so hover over a TF frame can show its
  // name (updateHoverFrame). Camera gestures still gate on a held button.
  setMouseTracking(true);
  // Accept keyboard focus so the 'P' perf-HUD toggle reaches keyPressEvent
  // (click/tab focus; harmless to the dock's existing mouse interaction).
  setFocusPolicy(Qt::StrongFocus);
  // Orientation gizmo sits in the bottom-right corner; this frees the top-right
  // for the dock's camera-model combo + Home button (which then align flush to
  // the right edge — see Scene3DDockWidget::layoutFrameOverlayCombo). The perf
  // HUD is bottom-LEFT, so the gizmo's corner stays clear.
  overlay_.setCorner(AxisOverlayPass::Corner::kBottomRight);
}

SceneViewWidget::~SceneViewWidget() {
  // Disconnect the teardown hook first so aboutToBeDestroyed can't fire on this
  // half-destroyed object when the base QOpenGLWidget destroys the context, then
  // free GL resources while our members and the context are still alive.
  QObject::disconnect(context_cleanup_connection_);
  releaseGlResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint t) {
  if (render_time_ == t) {
    return;
  }
  render_time_ = t;
  refreshAvailableFrames();
  applyFollow();
  update();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  // The followed origin was cached in the OLD fixed frame; re-seed so the basis
  // change doesn't produce a spurious shift on the next tick.
  follow_seeded_ = false;
  update();
}

void SceneViewWidget::setFollowFrame(const std::string& frame) {
  if (follow_frame_ == frame) {
    return;
  }
  follow_frame_ = frame;
  follow_seeded_ = false;  // next applyFollow seeds the baseline (no shift)
  applyFollow();           // seed now at the current time so a later tick has a baseline
  update();
}

void SceneViewWidget::applyFollow() {
  if (follow_frame_.empty() || tf_ == nullptr || fixed_frame_.empty()) {
    return;
  }
  const auto x = tf_->tryLookupTransform(fixed_frame_, follow_frame_, render_time_);
  if (!x) {
    // Followed frame not resolvable now: hold the camera and re-seed when it
    // reappears, so a lookup gap can't teleport the view on resume.
    follow_seeded_ = false;
    return;
  }
  const glm::dvec3 origin = x->t;
  if (!follow_seeded_) {
    follow_prev_origin_ = origin;
    follow_seeded_ = true;
    return;  // seeding tick applies no shift (no jump on enable / fixed-frame change)
  }
  const glm::dvec3 delta = origin - follow_prev_origin_;
  follow_prev_origin_ = origin;
  if (delta != glm::dvec3{0.0}) {
    camera_->followShift(glm::vec3(delta));
  }
}

void SceneViewWidget::recenterOnFollowFrame() {
  if (follow_frame_.empty() || tf_ == nullptr || fixed_frame_.empty()) {
    return;
  }
  const auto x = tf_->tryLookupTransform(fixed_frame_, follow_frame_, render_time_);
  if (!x) {
    return;
  }
  const glm::dvec3 target = x->t;
  // Snap whatever anchor the active camera owns onto the target, keeping orbit
  // angle/zoom: followShift(target - current look-at). state().focal is the look-at
  // point for every model (Fly synthesizes it from eye + forward), so this recenters
  // generically. Keep the follow baseline consistent so the next tick adds only the
  // frame's subsequent motion, not the jump we just applied.
  const CameraState before = camera_->state();
  camera_->followShift(glm::vec3(target) - before.focal);
  follow_prev_origin_ = target;
  follow_seeded_ = true;
  update();
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

uint64_t SceneViewWidget::followRenderKey(PJ::Timepoint time) const {
  if (follow_frame_.empty() || tf_ == nullptr || fixed_frame_.empty()) {
    return 0;
  }
  const auto x = tf_->tryLookupTransform(fixed_frame_, follow_frame_, time);
  if (!x) {
    return 0;  // unresolvable → camera holds → no follow-driven repaint
  }
  // FNV-1a over the followed origin's bits (Position-only follow shifts the camera
  // by this translation). Never 0 while following+resolvable, so the dock's gate
  // can tell "following" from "no TF overlay" (which also returns 0).
  uint64_t h = 1469598103934665603ULL;
  const double comps[3] = {x->t.x, x->t.y, x->t.z};
  for (const double c : comps) {
    uint64_t bits = 0;
    std::memcpy(&bits, &c, sizeof(bits));
    h = (h ^ bits) * 1099511628211ULL;
  }
  return h == 0 ? 1U : h;
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setTfConnectionsVisible(bool visible) {
  if (tf_connections_visible_ == visible) {
    return;
  }
  tf_connections_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setMeshShadingParams(const MeshShadingParams& params) {
  const bool changed = shading_params_.meshes_visible != params.meshes_visible ||
                       shading_params_.mesh_opacity != params.mesh_opacity ||
                       shading_params_.collisions_visible != params.collisions_visible ||
                       shading_params_.collision_opacity != params.collision_opacity;
  shading_params_ = params;
  if (changed) {
    update();
    emit presentationChanged();
  }
}

void SceneViewWidget::setLayers(const std::vector<Scene3DLayer*>& ordered) {
  if (layers_ == ordered) {
    return;
  }
  if (context() != nullptr) {
    makeCurrent();
    for (Scene3DLayer* layer : layers_) {
      if (layer != nullptr && std::find(ordered.begin(), ordered.end(), layer) == ordered.end()) {
        layer->releaseGL();
      }
    }
    doneCurrent();
  }
  layers_ = ordered;
  update();
}

void SceneViewWidget::refreshAvailableFrames() {
  QList<FrameRow> list;
  if (tf_) {
    auto rows = tf_->getFrameHierarchy();
    list.reserve(static_cast<qsizetype>(rows.size()));
    for (auto&& r : rows) {
      list.append(FrameRow{std::move(r.name), r.depth});
    }
  }
  if (list != last_frame_list_) {
    last_frame_list_ = list;
    emit framesChanged(list);
  }
}

uint64_t SceneViewWidget::tfRenderKey(PJ::Timepoint time) const {
  // No TF overlay drawn → contribute nothing (and forgo no coalescing). render()
  // poses the axis triads + parent-connection lines from this exact frame set and
  // these exact fixed←frame transforms (see the FrameContext it builds), so hashing
  // them here makes the dock's gate repaint exactly when that overlay would move.
  if (!tf_ || (!axes_visible_ && !tf_connections_visible_)) {
    return 0;
  }
  tf_->getAllFrames(tf_render_key_frames_);
  uint64_t key = 0;
  for (const std::string& frame : tf_render_key_frames_) {
    uint64_t h = std::hash<std::string>{}(frame);
    if (const auto xf = tf_->tryLookupTransform(fixed_frame_, frame, time); xf.has_value()) {
      h = foldTransformIntoKey(h, glm::mat4(xf->matrix()));
    } else {
      h ^= 0xD15C0FFEEULL;  // frame present in the forest but unresolved at this time
    }
    key ^= h ^ (h >> 29);  // order-independent fold (getAllFrames order must not matter)
  }
  return key;
}

void SceneViewWidget::initializeGL() {
  // Qt calls this once per GL context — on first realize and again after every
  // context recreation (QOpenGLWidget rebuilds its context when reparented by
  // ADS dock/float/split). Rewire the teardown hook to THIS context so the
  // dying context releases its own resources; releaseGlResources() already ran
  // for the previous context (via its aboutToBeDestroyed), clearing the passes'
  // initialized_ latches, so the rebuild below starts from a clean slate.
  QObject::disconnect(context_cleanup_connection_);
  context_cleanup_connection_ = connect(
      context(), &QOpenGLContext::aboutToBeDestroyed, this, &SceneViewWidget::releaseGlResources, Qt::DirectConnection);

  gl::installDebugCallback();
  // Seed the scene HDR FBO's MSAA from the fixed default, NOT the context's
  // negotiated samples (0 when composited in an ADS dock) — SceneHdrFbo is an
  // independent multisample chain that clamps to GL_MAX_*_TEXTURE_SAMPLES. The
  // benchmark sweep overrides the level via setSceneSamples. Attachments are
  // (re)allocated lazily in paintGL.
  const int scene_samples = scene_samples_override_ >= 0 ? scene_samples_override_ : kDefaultMsaaSamples;
  scene_fbo_.configure(scene_samples);
  initializePresentProgram();
  scene_fbo_fallback_logged_ = false;  // a fresh context may succeed; re-arm the warning

  axes_.initializeGL();
  grid_.initializeGL();
  tf_connections_.initializeGL();
  overlay_.initializeGL();
  // Layer GL is initialised lazily in paintGL — layers may be added
  // dynamically after the widget is already realised, so initializing
  // here would miss late entries. releaseGlResources() reset their lazy-init
  // guards, so they rebuild on the first paint after a context recreation.
}

void SceneViewWidget::initializePresentProgram() {
  auto result = gl::Program::fromSources(kPresentVertSrc, kPresentFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    present_program_.emplace(std::move(*program));
    // The sampler units are CONSTANT (u_scene=0, u_depth=1, u_ao=2, u_edl=3) — set
    // them once at build time rather than every frame in paintGL (L.57). Only the
    // texture *binds* and the value/flag uniforms change per frame.
    present_program_->use();
    present_program_->setInt("u_scene", 0);
    present_program_->setInt("u_depth", 1);
    present_program_->setInt("u_ao", 2);
    present_program_->setInt("u_edl", 3);
    if (auto* ctx = QOpenGLContext::currentContext(); ctx != nullptr) {
      if (auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx); funcs != nullptr) {
        funcs->glUseProgram(0);
      }
    }
  } else {
    qCWarning(lcSceneViewWidget) << "present shader error:" << std::get<std::string>(result).c_str();
    present_program_.reset();
  }
}

void SceneViewWidget::releaseGlResources() {
  // Drop every pass's and layer's GL objects with a context current so their
  // glDelete* actually run (the wrappers self-skip without a current context).
  // Called from the context's aboutToBeDestroyed (reparent or teardown) and the
  // destructor. context() is null before the first show / after full teardown.
  if (context() == nullptr) {
    return;
  }
  makeCurrent();
  axes_.releaseGL();
  grid_.releaseGL();
  tf_connections_.releaseGL();
  overlay_.releaseGL();
  for (Scene3DLayer* layer : layers_) {
    if (layer != nullptr) {
      layer->releaseGL();
    }
  }
  // The HDR chain and the present program/VAO are per-context like every other
  // GL object here; zeroing them under the dying context forces a clean lazy
  // rebuild in the next context (initializeGL + first paint).
  scene_fbo_.releaseGL();
  ssao_.releaseGL();
  edl_.releaseGL();
  shadow_pass_.releaseGL();
  scene_profiler_.releaseGL();
  present_program_.reset();
  present_vao_ = gl::VertexArray{};
  doneCurrent();
}

void SceneViewWidget::resizeGL(int /*w*/, int /*h*/) {
  // Nothing to do: Qt sets the backing-FBO viewport itself, and the HDR scene
  // FBO is (re)sized lazily in paintGL from that viewport's device-pixel size.
  // (Qt 6 passes LOGICAL units here, so the viewport read is the exact source.)
}

void SceneViewWidget::paintGL() {
  auto* ctx = QOpenGLContext::currentContext();
  auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx);
  if (funcs == nullptr) {
    return;
  }

  // Perf instrumentation is gated on the HUD: when it's off (the production
  // default), the scene pays nothing for timing it doesn't display. The CPU
  // stopwatch and the GPU timer bracket the whole scene render.
  if (show_perf_hud_) {
    cpu_timer_.restart();
    scene_profiler_.beginFrame();
  }

  // Device-pixel size: Qt bound the backing FBO and set the viewport to its
  // exact device size right before paintGL — reading it back is exact even at
  // fractional DPR (Qt 6 passes LOGICAL units to resizeGL, so that is not).
  GLint viewport[4] = {0, 0, 0, 0};
  funcs->glGetIntegerv(GL_VIEWPORT, viewport);
  // Per-paint locals (recomputed each frame from the viewport Qt just set);
  // there is no cross-frame device-size state worth keeping as a member.
  const int device_width_px = viewport[2];
  const int device_height_px = viewport[3];

  // Supersampling: the off-screen HDR chain renders at scale x device pixels and
  // the present pass downsamples to device resolution (true SSAA). scale == 1.0
  // (the default) reproduces the original path byte-for-byte. Floor at 1px so a
  // tiny widget can't request a zero-sized FBO.
  const float scale = render_scale_;
  const int scene_width_px = std::max(1, static_cast<int>(std::lround(static_cast<float>(device_width_px) * scale)));
  const int scene_height_px = std::max(1, static_cast<int>(std::lround(static_cast<float>(device_height_px) * scale)));

  // (Re)allocate the off-screen HDR chain; idempotent at unchanged size. If the
  // chain or the present shader is unavailable, fall back to rendering directly
  // into the backing FBO exactly as before Phase 0A (degrade, never go blank).
  scene_fbo_.resize(scene_width_px, scene_height_px);
  const bool offscreen = scene_fbo_.ready() && present_program_.has_value();
  if (offscreen) {
    scene_fbo_.bind();
    funcs->glViewport(0, 0, scene_width_px, scene_height_px);
    // Multisample rasterization must be explicitly enabled when drawing into our
    // own multisample FBO: GL_MULTISAMPLE defaults to enabled in the spec, but
    // relying on that left the MSAA buffer allocated and resolved yet producing
    // zero edge anti-aliasing (every sample got the single-sample value). Enable
    // it here so coverage actually varies per sample. Harmless at samples == 1.
    funcs->glEnable(GL_MULTISAMPLE);
  } else {
    // Fallback: render directly into the backing FBO at device resolution (no
    // supersampling without the off-screen chain). scene_fbo_.resize() above
    // leaves an off-screen FBO bound whenever it reallocates (M.33), so we must
    // explicitly rebind the backing target here — otherwise renderScene would
    // draw into the off-screen (possibly incomplete) FBO.
    gl::Framebuffer::bindDefault(defaultFramebufferObject());
    funcs->glViewport(0, 0, device_width_px, device_height_px);
    if (!scene_fbo_fallback_logged_) {
      qCWarning(lcSceneViewWidget) << "HDR scene FBO unavailable — rendering directly into the backing framebuffer";
      scene_fbo_fallback_logged_ = true;
    }
  }

  // Pixel dims fed to passes that size primitives in device pixels: the scene
  // render resolution when off-screen (so gl_PointSize etc. supersample with the
  // buffer and the post passes match it), the backing size on the fallback path.
  const int render_width_px = offscreen ? scene_width_px : device_width_px;
  const int render_height_px = offscreen ? scene_height_px : device_height_px;

  const float aspect = static_cast<float>(width()) / static_cast<float>(std::max(height(), 1));
  // viewport_width/height_px keep their historical LOGICAL-pixel semantics: the
  // HUD overlay derives the device-pixel ratio as saved_vp[3] / viewport_height_px.
  // device_*_px carry the (possibly supersampled) FRAMEBUFFER size so passes that
  // size primitives in device pixels (gl_PointSize in PointcloudRenderPass) and
  // the screen-space post passes match the buffer they draw into.
  // Camera-relative rendering origin for THIS frame: the camera focal. Geometry
  // (via FrameContext::lookup) and the view matrix are both expressed relative to
  // it, in double, so a camera following a far frame at large world coordinates
  // doesn't lose the eye→geometry delta to float32 cancellation (the scene-swims-
  // on-zoom bug). Stored so the async hover hit-test stays consistent with the
  // camera-relative last_view_proj_ below.
  render_origin_ = glm::dvec3(camera_->state().focal);

  // The TF-resolution triple (plus the camera-relative render origin), bundled for
  // the passes/layers that need it. Built BEFORE view_params (it used to follow it)
  // because the shadow pre-pass needs it to gather caster bounds + depth-draw casters,
  // and its resulting shadow-map id is baked into the const view_params the receivers
  // read. Grid never consults the TF buffer; safe even when tf_ is null. fixed_frame_
  // is the long-lived member (no per-frame string copy). render_origin_ makes lookup()
  // return render-space transforms consistent with the camera-relative view, so the
  // render-space draw cache built by the shadow pre-pass is reused by the color pass.
  static const TransformBuffer k_empty_buffer;
  const TransformBuffer& tf_ref = tf_ ? *tf_ : k_empty_buffer;
  const FrameContext frame_ctx{tf_ref, fixed_frame_, render_time_, render_origin_};

  // --- Shadow pre-pass: depth-render mesh casters BEFORE the scene render ----------
  // Fit the directional light frustum to the MESH caster bounds (meshShadowBounds —
  // NOT scene_bounds_, which deliberately omits the robot), depth-draw every caster
  // from the light's POV into shadow_pass_, then rebind the scene target. The result
  // is baked into view_params below; degrades to no shadows (shadow_map_id == 0) on an
  // invalid frustum fit or an unavailable map, and is skipped when shadows are off.
  unsigned shadow_map_id = 0U;
  glm::mat4 shadow_light_vp{1.0f};
  float shadow_texel = 0.0f;
  // Gate on tf_ exactly like the color pass below (`if (tf_)`): a caster can only be
  // posed through a TransformBuffer, so without one there is nothing legitimate to
  // cast. The gate matters because a layer keeps its last draw cache after the data
  // is deleted — the dock clears the binding via setTransformBuffer(nullptr) but does
  // not dirty surviving layers — so an ungated pre-pass would depth-draw that STALE
  // geometry and the floor would keep sampling a shadow whose mesh has vanished.
  if (shading_params_.shadows_enabled && tf_ != nullptr) {
    AABB caster_bounds;
    for (Scene3DLayer* layer : layers_) {
      if (layer == nullptr) {
        continue;
      }
      if (const auto bounds = layer->meshShadowBounds(frame_ctx); bounds.has_value()) {
        caster_bounds = unionAABB(caster_bounds, *bounds);
      }
    }
    // Extend the caster bounds to include where their shadows land on the ground
    // along the light direction, so the light frustum also covers the receiving
    // floor — without this the frustum hugs the casters and the cast shadow falls
    // outside it (guarded as lit). The floor is a receiver but never a caster, so it
    // is otherwise absent from caster_bounds. caster_bounds is in render space, so the
    // world floor at z=0 sits at render-space z = -render_origin.z (NOT 0); pass that
    // so the fit matches the render-space floor the grid receiver projects.
    const float ground_z_render = static_cast<float>(-render_origin_.z);
    caster_bounds = extendAabbToGroundShadow(caster_bounds, shading_params_.key_light_dir, ground_z_render);
    const ShadowCameraFit fit =
        fitDirectionalShadowCamera(caster_bounds, shading_params_.key_light_dir, kShadowMapSize);
    // Only allocate the shadow FBO when there is actually a mesh to cast — a
    // point-cloud-only scene with shadows enabled stays a cheap no-op (no 16 MB
    // depth map, receivers render lit via shadow_map_id == 0).
    if (fit.valid) {
      shadow_pass_.ensure();
    }
    if (fit.valid && shadow_pass_.ready()) {
      shadow_pass_.begin();
      for (Scene3DLayer* layer : layers_) {
        if (layer != nullptr) {
          layer->renderShadowCasters(fit.light_view_proj, frame_ctx);
        }
      }
      shadow_pass_.end();
      // The shadow pass left its own FBO + viewport bound; restore the scene target.
      if (offscreen) {
        scene_fbo_.bind();
        funcs->glViewport(0, 0, scene_width_px, scene_height_px);
      } else {
        gl::Framebuffer::bindDefault(defaultFramebufferObject());
        funcs->glViewport(0, 0, device_width_px, device_height_px);
      }
      shadow_map_id = shadow_pass_.depthTextureId();
      shadow_light_vp = fit.light_view_proj;
      shadow_texel = fit.world_units_per_texel;
    }
  }
  last_shadow_map_id_ = shadow_map_id;  // test seam (see lastShadowMapIdForTest)

  const ViewParams view_params{
      camera_->viewMatrixRelativeTo(render_origin_),
      camera_->projMatrix(aspect),
      height(),
      width(),
      toRenderSpace(camera_->position(), render_origin_),  // eye in render space (mesh lighting)
      render_width_px,
      render_height_px,
      shading_params_,  // this view's mesh/collision look knobs (per-view, see header)
      // Supersample factor for the offscreen post passes (EDL radius scaling); the
      // fallback path renders at device res and runs no post passes, so 1.0 there.
      offscreen ? render_scale_ : 1.0f,
      // write_mesh_mask: have the mesh pass mark the scene FBO's is-mesh mask so
      // EDL can restrict its contour to meshes. Only on the off-screen path (the
      // fallback FBO has no mask attachment) and only when EDL is on (else the
      // extra draw-buffer toggling is wasted).
      offscreen && composite_params_.edl_enabled,
      // Shadow receive inputs from the pre-pass above (0 id => receivers stay lit).
      shadow_map_id,
      shadow_light_vp,
      shadow_texel,
  };

  // Cache proj*view for the hover hit-test (a mouse-move event, async from
  // paint) and the label draw, so both use the exact matrices this frame used.
  last_view_proj_ = view_params.proj * view_params.view;

  // Push the current hover highlight to the TF axis pass before it draws, so the
  // hovered frame's triad renders brighter than its neighbours.
  axes_.setHighlightedFrame(hovered_frame_.value_or(std::string{}));

  // On the direct-to-backing fallback the legacy Wayland alpha guard applies
  // (set inside renderScene, after the clear); the off-screen path instead
  // forces alpha=1.0 in the present shader.
  renderScene(view_params, frame_ctx, /*mask_alpha_writes=*/!offscreen);

  if (!offscreen) {
    // Restore the alpha write mask so the next frame's glClear repaints alpha=1.0.
    funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    finishFrameInstrumentation();
    drawHoverLabel(frame_ctx);  // QPainter overlay, after all GL submission
    return;
  }

  // Resolve MSAA -> single-sample, then present into the backing FBO. The blit
  // and the fullscreen draw must cover the full target: clear any pass-leaked
  // state (scissor/blend/depth) first rather than assuming the passes left it
  // clean.
  funcs->glDisable(GL_SCISSOR_TEST);
  funcs->glDisable(GL_BLEND);
  funcs->glDisable(GL_DEPTH_TEST);
  funcs->glDepthMask(GL_TRUE);
  funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Resolve the mask attachment only when EDL will sample it this frame.
  scene_fbo_.resolve(composite_params_.edl_enabled);

  // SSAO over the resolved depth (Phase D). Runs in the post-chain state set
  // above; binds its own FBOs, so it must precede the bindDefault below. The
  // present degrades to no-AO (u_has_ao=0) when the pass is unavailable. Sized to
  // the off-screen (supersampled) resolution so the AO aligns with the resolved
  // scene texture and its cost is measured at the true render resolution.
  if (composite_params_.ssao_enabled) {
    ssao_.initializeGL();
    ssao_.resize(scene_width_px, scene_height_px);
    ssao_.setDepthTexture(scene_fbo_.resolvedDepthTextureId());
    ssao_.renderAo(view_params);
  }
  if (composite_params_.edl_enabled) {
    edl_.initializeGL();
    edl_.resize(scene_width_px, scene_height_px);
    edl_.setDepthTexture(scene_fbo_.resolvedDepthTextureId());
    // Restrict the contour to mesh surfaces (point clouds/grid/background excluded).
    edl_.setMaskTexture(scene_fbo_.resolvedMaskTextureId());
    edl_.renderEdl(view_params);
  }

  // Present as a fullscreen passthrough DRAW (not a blit): the backing FBO is
  // itself multisampled, and single->MSAA blits are invalid while MSAA->MSAA
  // blits require identical formats (ours is RGBA16F, Qt's is RGBA8). The
  // shader forces alpha to 1.0, which keeps the Wayland opaque-surface
  // invariant on this path (the role the old geometry-phase glColorMask guard
  // played when geometry still wrote the backing FBO directly).
  gl::Framebuffer::bindDefault(defaultFramebufferObject());
  funcs->glViewport(0, 0, device_width_px, device_height_px);
  // u_scene/u_depth/u_ao/u_edl sampler units are constant and were set once at
  // program build (initializePresentProgram); only the binds and flags vary here.
  present_program_->use();
  funcs->glActiveTexture(GL_TEXTURE0);
  funcs->glBindTexture(GL_TEXTURE_2D, scene_fbo_.resolvedColorTextureId());
  funcs->glActiveTexture(GL_TEXTURE1);
  funcs->glBindTexture(GL_TEXTURE_2D, scene_fbo_.resolvedDepthTextureId());
  const bool ao_active = composite_params_.ssao_enabled && ssao_.ready();
  present_program_->setInt("u_has_ao", ao_active ? 1 : 0);
  if (ao_active) {
    funcs->glActiveTexture(GL_TEXTURE2);
    funcs->glBindTexture(GL_TEXTURE_2D, ssao_.outputTextureId());
  }
  const bool edl_active = composite_params_.edl_enabled && edl_.ready();
  present_program_->setInt("u_has_edl", edl_active ? 1 : 0);
  if (edl_active) {
    funcs->glActiveTexture(GL_TEXTURE3);
    funcs->glBindTexture(GL_TEXTURE_2D, edl_.outputTextureId());
  }
  present_program_->setInt("u_tonemap_mode", composite_params_.tonemap_mode);
  present_program_->setFloat("u_exposure", composite_params_.exposure);
  present_program_->setFloat("u_saturation", composite_params_.saturation);
  present_program_->setFloat("u_ao_strength", composite_params_.ao_strength);
  present_program_->setFloat("u_edl_floor", composite_params_.edl_floor);
  present_vao_.bind();
  funcs->glDrawArrays(GL_TRIANGLES, 0, 3);
  present_vao_.unbind();
  funcs->glActiveTexture(GL_TEXTURE1);
  funcs->glBindTexture(GL_TEXTURE_2D, 0);
  funcs->glActiveTexture(GL_TEXTURE0);
  funcs->glBindTexture(GL_TEXTURE_2D, 0);

  // Leave depth/blend enabled — the state the legacy path ended each frame with.
  funcs->glEnable(GL_DEPTH_TEST);
  funcs->glEnable(GL_BLEND);

  finishFrameInstrumentation();
  drawHoverLabel(frame_ctx);  // QPainter overlay, after all GL submission
}

void SceneViewWidget::renderScene(
    const ViewParams& view_params, const FrameContext& frame_ctx, bool mask_alpha_writes) {
  auto* ctx = QOpenGLContext::currentContext();
  auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx);
  if (funcs == nullptr) {
    return;
  }

  // Theme-aware background + grid — see Phase 1 commit (theme-aware
  // background + high-contrast grid color) for the full rationale.
  // Read the APPLICATION palette, not this widget's: QStyleSheetStyle's polish
  // rewrites widget palettes from QSS rules (the app stylesheet's transparent
  // QWidget background lands as #000000 in QPalette::Window), while the
  // application palette is kept in lockstep with the theme by pj_app's Theme.
  const QPalette pal = QGuiApplication::palette();
  const QColor window_bg = pal.color(QPalette::Window);
  const auto fw_theme = PJ::theme::themeFor(window_bg.lightness() >= 128);
  const QColor bg = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, fw_theme);
  const QColor fg = PJ::theme::onSurface(PJ::theme::Surface::DataBackdrop, PJ::theme::Emphasis::Default, fw_theme);
  // Off-screen path: the scene FBO is linear-light (the composite present
  // re-encodes to sRGB), so display-referred theme colors must be linearized on
  // write. The direct-to-backing fallback has no encode — leave them as-is there.
  const bool linear_target = !mask_alpha_writes;
  const auto lin = [linear_target](qreal c) {
    return linear_target ? static_cast<float>(std::pow(c, 2.2)) : static_cast<float>(c);
  };
  funcs->glClearColor(lin(bg.redF()), lin(bg.greenF()), lin(bg.blueF()), 1.0f);
  // Grid colors are derived from the theme (no user color controls). blend(t) is
  // the linearized bg→fg mix at fraction t. The line color is the high-contrast
  // 0.35 blend (unchanged); the two checkerboard tile tones are subtler blends
  // just above the background, so the lines read clearly on top of the fill.
  const auto blend = [&](double frac) {
    return glm::vec3{
        lin(bg.redF() * (1.0 - frac) + fg.redF() * frac),
        lin(bg.greenF() * (1.0 - frac) + fg.greenF() * frac),
        lin(bg.blueF() * (1.0 - frac) + fg.blueF() * frac),
    };
  };
  grid_.setColor(blend(0.35));                    // grid lines
  grid_.setCellColors(blend(0.10), blend(0.24));  // checkerboard tile tones (A, B)
  funcs->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  // Clear the is-mesh mask (COLOR_ATTACHMENT1) to 0 each frame so only the mesh
  // pass marks it. The glClear above only touches the FBO's standing draw buffer
  // (COLOR_ATTACHMENT0) + depth, so the mask gets its own clear, which needs its
  // draw buffer temporarily enabled. Off-screen + EDL only (mirrors the
  // write_mesh_mask flag); the fallback path has no mask attachment. !mask_alpha_writes
  // == offscreen already implies the chain is ready, so no separate ready() check.
  if (!mask_alpha_writes && composite_params_.edl_enabled) {
    const std::array<GLenum, 2> both{GL_COLOR_ATTACHMENT0, SceneHdrFbo::kMaskAttachment};
    const GLenum color_only = GL_COLOR_ATTACHMENT0;
    const std::array<GLfloat, 4> zero{0.0F, 0.0F, 0.0F, 0.0F};
    funcs->glDrawBuffers(2, both.data());
    funcs->glClearBufferfv(GL_COLOR, 1, zero.data());  // draw-buffer index 1 == mask
    funcs->glDrawBuffers(1, &color_only);              // restore color-only for non-mesh passes
  }
  funcs->glEnable(GL_DEPTH_TEST);
  funcs->glEnable(GL_BLEND);
  // The scene FBO's alpha is the per-pixel tonemap marker (1 = data, graded;
  // 0 = annotation, raw). Data draws must RESTORE it — coverage-union alpha
  // (ONE, ONE_MINUS_SRC_ALPHA): an opaque draw stamps 1, a translucent draw
  // raises it proportionally, untouched annotation pixels keep 0. Preserving
  // destination alpha instead (ZERO, ONE) left stale alpha-0 from occluded TF
  // arrows under the robot body, ghosting raw arrow shapes through the mesh.
  funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  if (mask_alpha_writes) {
    // Direct-to-backing path only. The 3D viewport is opaque: the clear above
    // set the framebuffer alpha to 1.0, and masking alpha writes keeps it there
    // through every pass. RGB still blends normally (src.a is the blend
    // *factor*, not an alpha write), so transparent content like the occupancy
    // grid (opacity < 1) looks correct — but no pass can lower the framebuffer's
    // alpha. Without this, a sub-1.0 alpha left in the QOpenGLWidget's FBO makes
    // the Wayland compositor treat those regions as translucent and bleed the
    // previous frame through them. The caller restores the mask after rendering
    // so the next frame's glClear can repaint alpha (glClear honours the mask).
    funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  }

  // Annotation blend mode (axes + HUD): frag alpha is the gizmo opacity; RGB
  // blends normally while the alpha factors (ZERO, 1-SRC_ALPHA) DECREASE the
  // tonemap-bypass marker by the annotation's coverage — at opacity 1 this is
  // identical to the old blend-off + alpha-0 write (RGB=src, marker→0).
  const auto annotation_blend = [funcs] {
    funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
  };
  const auto data_blend = [funcs] {
    funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  };

  if (grid_visible_) {
    grid_.render(view_params, frame_ctx);
  }
  // TF overlays (parent-connection lines + axis triads) share the annotation
  // blend so they bypass the tonemap and keep their true colors (the magenta
  // lines would otherwise desaturate through AgX). Lines first, under the
  // triads, so the arrows sit on top of the line ends at each frame origin.
  if (tf_ && (tf_connections_visible_ || axes_visible_)) {
    annotation_blend();
    if (tf_connections_visible_) {
      tf_connections_.render(view_params, frame_ctx);
    }
    if (axes_visible_) {
      axes_.render(view_params, frame_ctx);
    }
    data_blend();
  }
  // Iterate layers in the order supplied by SceneDockWidget. Each layer is responsible
  // for its own GL state — initializeGL() is intentionally called per
  // frame, and layers (like the render passes they own) guard
  // against double-init via an internal `initialized_` flag. The
  // per-frame call lets layers added *after* the widget realises
  // initialise on their first paint without needing a current GL
  // context at attach time. See Scene3DLayer::initializeGL for the
  // contract.
  if (tf_) {
    for (Scene3DLayer* layer : layers_) {
      if (layer == nullptr) {
        continue;
      }
      // Re-assert the ambient blend state before each layer (defense in depth):
      // a pass that leaks a different blend func/enable (H.10/M.31) can't then
      // poison the rest of the frame, so each layer starts from the data
      // contract regardless of what the previous one left behind.
      funcs->glEnable(GL_BLEND);
      data_blend();
      layer->initializeGL();
      layer->render(view_params, frame_ctx);
    }
  }

  // Camera-orientation HUD (bottom-right; corner set in the constructor). Drawn
  // last so the solid arrows sit on top of every scene-space pass. Annotation,
  // opacity 1.
  annotation_blend();
  overlay_.render(view_params, frame_ctx);
  data_blend();
}

void SceneViewWidget::setCameraModel(CameraModel model) {
  if (camera_model_ == model) {
    return;
  }
  const CameraState carried = camera_->state();
  std::unique_ptr<ICamera> next;
  switch (model) {
    case CameraModel::kOrbit:
      next = std::make_unique<OrbitCamera>();
      break;
    case CameraModel::kTopDownOrtho:
      next = std::make_unique<TopDownOrthoCamera>();
      break;
    case CameraModel::kFly:
      next = std::make_unique<FlyCamera>();
      break;
    case CameraModel::kXyOrbit:
      next = std::make_unique<XYOrbitCamera>();
      break;
  }
  next->adoptState(carried);            // carry the pose across so the view doesn't jump
  next->setSceneBounds(scene_bounds_);  // bounds aren't part of CameraState
  camera_ = std::move(next);
  camera_model_ = model;
  update();
  emit presentationChanged();
}

void SceneViewWidget::resetCamera() {
  const CameraState before = camera_->state();
  camera_->reset();
  update();
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->position().toPoint();
  active_button_ = event->button();
  camera_gesture_changed_ = false;
  // Hide any hover label for the duration of a camera gesture; it re-appears on
  // the next button-free move (the hit-test below only runs with no button).
  if (hovered_frame_.has_value()) {
    hovered_frame_.reset();
    update();
  }
}

void SceneViewWidget::endActiveGesture() {
  active_button_ = Qt::NoButton;
  if (camera_gesture_changed_) {
    camera_gesture_changed_ = false;
    emit presentationChanged();
  }
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  // Clear the active-gesture latch when its button is released so a chorded
  // drag (e.g. press LMB then MMB, release MMB, keep dragging LMB) doesn't keep
  // applying the released button's gesture.
  if (event->button() == active_button_) {
    endActiveGesture();
  }
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  if (active_button_ != Qt::NoButton && !(event->buttons() & active_button_)) {
    // Qt's own live button state disagrees with our latch: the matching release
    // was never delivered (e.g. swallowed by a right-click context menu's nested
    // QMenu::exec() event loop — see pj_scene3D/CLAUDE.md). Self-heal so a stray
    // hover move doesn't keep applying the gesture forever.
    endActiveGesture();
  }
  if (active_button_ == Qt::NoButton) {
    // No gesture in progress: this is a hover. Update the TF frame label.
    updateHoverFrame(event->position());
    return;
  }
  const QPoint current = event->position().toPoint();
  const QPoint delta = current - last_mouse_pos_;
  last_mouse_pos_ = current;

  const float dx = static_cast<float>(delta.x());
  const float dy = static_cast<float>(delta.y());
  const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
  const CameraState before = camera_->state();
  bool handled = true;

  if (active_button_ == Qt::LeftButton && !shift) {
    camera_->rotate(dx, dy);
  } else if (active_button_ == Qt::MiddleButton || (active_button_ == Qt::LeftButton && shift)) {
    camera_->pan(dx, dy);
  } else if (active_button_ == Qt::RightButton) {
    // Right-drag stays center-of-view zoom — cursor-anchoring per drag delta
    // walks the focal (focal creep); only the wheel is cursor-anchored.
    camera_->zoom(dy * 0.01f);
  } else {
    handled = false;
  }
  if (handled && !cameraStatesEqual(before, camera_->state())) {
    camera_gesture_changed_ = true;
  }

  update();
}

void SceneViewWidget::leaveEvent(QEvent* event) {
  if (hovered_frame_.has_value()) {
    hovered_frame_.reset();
    update();
  }
  QOpenGLWidget::leaveEvent(event);
}

std::optional<std::string> SceneViewWidget::pickFrameAt(const QPointF& pos_logical) const {
  // Only pick when the triads are actually drawn and a TF buffer exists; the
  // pick is an affordance for the visible gizmos, not the raw transform tree.
  if (!axes_visible_ || !tf_) {
    return std::nullopt;
  }
  const TransformBuffer& tf_ref = *tf_;
  // Seed with render_origin_ from the last paint so the projected origins are in
  // the same render space as last_view_proj_ (the camera-relative proj*view).
  const FrameContext frame_ctx{tf_ref, fixed_frame_, render_time_, render_origin_};
  tf_ref.getAllFrames(hover_all_frames_);

  // Project each origin, keeping hover_points_ index-aligned with
  // hover_all_frames_: a frame that can't resolve or is behind the camera gets
  // an off-screen sentinel (always outside the pick radius) so the winning
  // index still maps straight back to hover_all_frames_.
  const glm::vec2 offscreen_sentinel{-1.0e6f, -1.0e6f};
  hover_points_.clear();
  hover_points_.reserve(hover_all_frames_.size());
  const glm::vec2 viewport{static_cast<float>(width()), static_cast<float>(height())};
  for (const std::string& name : hover_all_frames_) {
    const auto transform = frame_ctx.lookup(name);
    const auto projected =
        transform.has_value() ? projectFrameOrigin(last_view_proj_, viewport, glm::vec3(transform->t)) : std::nullopt;
    hover_points_.push_back(projected.value_or(offscreen_sentinel));
  }

  const glm::vec2 cursor{static_cast<float>(pos_logical.x()), static_cast<float>(pos_logical.y())};
  const auto idx = pickNearestFrame(hover_points_, cursor, kHoverRadiusPx);
  if (!idx.has_value()) {
    return std::nullopt;
  }
  return hover_all_frames_[*idx];
}

void SceneViewWidget::updateHoverFrame(const QPointF& pos_logical) {
  std::optional<std::string> picked = pickFrameAt(pos_logical);
  if (picked != hovered_frame_) {
    hovered_frame_ = std::move(picked);
    update();  // repaint only when the hovered frame actually changes
  }
}

void SceneViewWidget::contextMenuEvent(QContextMenuEvent* event) {
  // Accept ONLY when a frame gizmo is under the cursor: an empty-space
  // right-click must fall through (ignore()) to the host dock's standard
  // Split/Clear menu.
  if (const auto frame = pickFrameAt(event->pos()); frame.has_value()) {
    emit frameContextMenuRequested(QString::fromStdString(*frame), event->globalPos());
    event->accept();
    return;
  }
  event->ignore();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0f;
  const QPointF pos = event->position();
  const CameraState before = camera_->state();
  camera_->zoomToCursor(ticks, glm::vec2{static_cast<float>(pos.x()), static_cast<float>(pos.y())}, width(), height());
  update();
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

void SceneViewWidget::changeEvent(QEvent* event) {
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
    update();
  }
  QOpenGLWidget::changeEvent(event);
}

void SceneViewWidget::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_P) {
    setShowPerfHud(!show_perf_hud_);
    event->accept();
    return;
  }
  QOpenGLWidget::keyPressEvent(event);
}

void SceneViewWidget::setShowPerfHud(bool on) {
  if (show_perf_hud_ == on) {
    return;
  }
  show_perf_hud_ = on;
  update();
}

void SceneViewWidget::setRenderScale(float scale) {
  // Clamp to a sane band: below 1 is downsampling (blurry, pointless here) and
  // above 4 risks exhausting VRAM / GL_MAX_TEXTURE_SIZE on large viewports.
  const float clamped = std::clamp(scale, 1.0f, 4.0f);
  if (render_scale_ == clamped) {
    return;
  }
  render_scale_ = clamped;
  update();
}

void SceneViewWidget::setSceneSamples(int samples) {
  scene_samples_override_ = samples;
  // Reconfigure immediately so the next paint reallocates at the new count. A
  // current context lets configure()'s release actually free the old chain
  // (otherwise the handles are merely dropped — a transient leak until repaint).
  if (context() != nullptr) {
    makeCurrent();
    scene_fbo_.configure(samples);
    doneCurrent();
  }
  update();
}

int SceneViewWidget::achievedSceneSamples() const {
  return scene_fbo_.samples();
}

void SceneViewWidget::drawPerfHud() {
  // QPainter over the QOpenGLWidget's FBO — the documented 2D-over-3D path. The
  // scene passes left a program/VAO bound; reset the program so the paint
  // engine starts from a clean slate (it manages its own VAO).
  unuseProgram();

  QStringList lines;
  if (scene_profiler_.hasResult()) {
    lines << u"GPU  %1 ms"_s.arg(scene_profiler_.averageMillis(), 0, 'f', 2);
  } else {
    lines << u"GPU  --"_s;
  }
  lines << u"CPU  %1 ms"_s.arg(cpuFrameMillis(), 0, 'f', 2);
  lines << u"MSAA %1x"_s.arg(achievedSceneSamples());

  QFont font;
  font.setFamily(u"monospace"_s);
  font.setStyleHint(QFont::Monospace);
  font.setPointSizeF(9.5);

  const auto fw_theme = PJ::theme::appTheme();

  // Rasterize the panel + text on the CPU and blit it: a glyph-atlas-free path
  // that survives the GL context recreation ADS triggers on dock reparent /
  // layout restore (see hud_overlay.h). drawImage is a plain textured quad.
  const QImage panel = renderHudPanel(
      lines, font, devicePixelRatioF(), /*padding=*/8, /*panel_alpha=*/150, PJ::theme::onOverlayHud(fw_theme));
  if (panel.isNull()) {
    return;
  }
  const QSizeF box = panel.deviceIndependentSize();
  // Bottom-left corner — clear of the top-left fixed-frame overlay combo the
  // Scene3D dock places over the view.
  const QPointF top_left(8.0, height() - box.height() - 8.0);

  QPainter painter(this);
  painter.drawImage(top_left, panel);
}

void SceneViewWidget::drawHoverLabel(const FrameContext& frame_ctx) {
  if (!hovered_frame_.has_value() || !axes_visible_) {
    return;
  }
  // Re-project the live origin so the box tracks the frame as the scene streams
  // or the camera moves between the hover hit-test and this paint.
  const auto transform = frame_ctx.lookup(*hovered_frame_);
  if (!transform.has_value()) {
    return;  // frame went away (e.g. eviction); drop the label silently
  }
  const glm::vec2 viewport{static_cast<float>(width()), static_cast<float>(height())};
  const auto anchor = projectFrameOrigin(last_view_proj_, viewport, glm::vec3(transform->t));
  if (!anchor.has_value()) {
    return;  // now behind the camera
  }

  // Reset the program the scene passes left bound; the paint engine starts clean
  // (same prelude as drawPerfHud).
  unuseProgram();

  // Rasterize the label on the CPU and blit it (glyph-atlas-free — see
  // hud_overlay.h / drawPerfHud) so it stays crisp after a layout-restore
  // context recreation. The connector ring below is a vector draw, which the GL
  // paint engine renders correctly regardless.
  QFont font;
  font.setPointSizeF(9.5);
  const auto fw_theme = PJ::theme::appTheme();
  const QImage panel = renderHudPanel(
      {QString::fromStdString(*hovered_frame_)}, font, devicePixelRatioF(),
      /*padding=*/6, /*panel_alpha=*/170, PJ::theme::onOverlayHud(fw_theme));
  if (panel.isNull()) {
    return;
  }
  const QSizeF box = panel.deviceIndependentSize();
  const int box_w = static_cast<int>(std::lround(box.width()));
  const int box_h = static_cast<int>(std::lround(box.height()));

  // Anchor just above-right of the frame origin, then clamp inside the widget so
  // a frame near an edge keeps its label fully visible.
  const int box_x = std::clamp(static_cast<int>(std::lround(anchor->x)) + 12, 2, std::max(2, width() - box_w - 2));
  const int box_y =
      std::clamp(static_cast<int>(std::lround(anchor->y)) - box_h - 12, 2, std::max(2, height() - box_h - 2));

  QPainter painter(this);
  painter.drawImage(QPointF(box_x, box_y), panel);

  // A small ring on the frame origin ties the label to the gizmo it names.
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(
      QPen(PJ::theme::outline(PJ::theme::OutlineRole::Interactive, PJ::theme::OutlineState::Hovered, fw_theme), 1.5));
  painter.drawEllipse(QPointF(anchor->x, anchor->y), 3.0, 3.0);
}

void SceneViewWidget::finishFrameInstrumentation() {
  // Gated on the HUD to match the beginFrame()/restart() guard at the top of
  // paintGL, so production frames that don't display the numbers don't measure
  // them. The CPU stopwatch stops BEFORE the HUD draw so the overlay's own cost
  // doesn't pollute the scene's CPU number.
  if (!show_perf_hud_) {
    return;
  }
  scene_profiler_.endFrame();
  cpu_avg_.add(static_cast<double>(cpu_timer_.nsecsElapsed()) / 1.0e6);
  drawPerfHud();
}

}  // namespace pj::scene3d
