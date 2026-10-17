// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QContextMenuEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QPalette>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QWheelEvent>
#include <QtMath>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "passes/grid_rhi_pass.h"
#include "passes/gridmap_rhi_pass.h"
#include "passes/marker_rhi_pass.h"
#include "passes/model_rhi_pass.h"
#include "passes/occupancy_rhi_pass.h"
#include "passes/overlay_rhi_pass.h"
#include "passes/point_rhi_pass.h"
#include "passes/pose_rhi_pass.h"
#include "passes/trail_rhi_pass.h"
#include "passes/voxel_rhi_pass.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_core/shadow_camera.h"
#include "pj_scene3d_core/tf/frame_picking.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/hud_overlay.h"
#include "pj_scene3d_widgets/layers/grid_map_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/rhi_conventions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_state_xml.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/Fnv1a.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/GraphicsApi.h"
#include "rhi_scene_pass.h"
#include "scene_view_widget_rhi_quality_p.h"

// The resource object lives in a static archive.  Explicit initialization both
// retains it at link time and keeps Q_INIT_RESOURCE's generated symbol lookup in
// the global namespace.
static void initializeScene3dRhiResources() {
  Q_INIT_RESOURCE(scene3d_rhi_shaders);
}

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

constexpr quint32 kModelUniformBytes = 128U;
constexpr quint32 kRenderModeUniformBytes = 16U;
// Cursor pick radius for TF frames, in logical pixels.
constexpr float kFramePickRadiusPx = 20.0F;

#ifdef PJ_RHI_TEST_HOOKS
std::int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif

struct alignas(16) ModelUniforms {
  std::array<float, 4> camera_position{};
  std::array<float, 4> lighting{};
  std::array<float, 4> environment{};
  std::array<float, 16> light_view_projection{};
  std::array<float, 4> shadow_params{};
};
static_assert(sizeof(ModelUniforms) == kModelUniformBytes);

struct alignas(16) RenderModeUniforms {
  std::array<std::int32_t, 4> mode{};
};
static_assert(sizeof(RenderModeUniforms) == kRenderModeUniformBytes);

// The overlay pass uploads images as is into RGBA8 textures. Painting straight
// into RGBA8888 rounds some antialiased pixels differently from ARGB32, so paint
// in ARGB32 and convert once per re-rasterisation.
QImage toOverlayFormat(const QImage& image) {
  return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

// The output target's sample count in the direct fallback (the HDR chain's is its own).
constexpr int kDirectSampleCount = 4;

// The current theme's scene colours (application-palette Window lightness ->
// framework DataBackdrop token), one source of truth for every backend. Display-referred: the HDR target and the
// grid shaders linearize them.
struct SceneThemeColors {
  QColor backdrop;
  look::GridColors grid;
};
SceneThemeColors sceneThemeColors() {
  const QColor window_background = QGuiApplication::palette().color(QPalette::Window);
  const auto theme = PJ::theme::themeFor(window_background.lightness() >= 128);
  const QColor backdrop = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, theme);
  const QColor foreground = PJ::theme::onSurface(PJ::theme::Surface::DataBackdrop, PJ::theme::Emphasis::Default, theme);
  return {
      backdrop, look::gridColors(
                    {backdrop.redF(), backdrop.greenF(), backdrop.blueF()},
                    {foreground.redF(), foreground.greenF(), foreground.blueF()})};
}

// The backend's 3D-texture limit (QRhi 6.11 reports none); 0 when it cannot be
// asked. OpenGL: the widget's context is current inside initialize()/render().
// Vulkan: the physical device, where the build has the Vulkan headers. D3D11:
// the device's feature level. D3D12/Metal: the documented constant.
int queriedMax3dTextureSize(QRhi& rhi) {
  switch (rhi.backend()) {
    case QRhi::OpenGLES2: {
      QOpenGLContext* context = QOpenGLContext::currentContext();
      if (context == nullptr) {
        return 0;
      }
      GLint limit = 0;
      context->functions()->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &limit);
      return std::max(limit, 0);
    }
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    case QRhi::Vulkan: {
      const auto* handles = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
      if (handles == nullptr || handles->inst == nullptr || handles->physDev == VK_NULL_HANDLE) {
        return 0;
      }
      VkPhysicalDeviceProperties properties{};
      handles->inst->functions()->vkGetPhysicalDeviceProperties(handles->physDev, &properties);
      return static_cast<int>(std::min<uint32_t>(properties.limits.maxImageDimension3D, INT_MAX));
    }
#endif
#if defined(Q_OS_WIN)
    case QRhi::D3D11: {
      const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi.nativeHandles());
      return d3d11Max3dTextureSize(handles != nullptr ? handles->featureLevel : 0);
    }
    case QRhi::D3D12:
      return kDocumentedMax3dTextureSize;
#endif
    case QRhi::Metal:
      return kDocumentedMax3dTextureSize;
    default:
      return 0;
  }
}

// queriedMax3dTextureSize(), or the API-guaranteed floor when the backend could
// not be asked (logged once per process).
int max3dTextureSize(QRhi& rhi) {
  const int queried = queriedMax3dTextureSize(rhi);
  const int limit = queried > 0 ? queried : kGuaranteedMax3dTextureSize;
  static bool logged = false;
  if (queried <= 0 && !std::exchange(logged, true)) {
    qWarning(
        "SceneViewWidget QRhi: %s cannot report its 3D-texture limit; voxel grids use the guaranteed %d",
        rhi.backendName(), limit);
  }
  return limit;
}

// Records `build` into a fresh update batch and submits it on `command_buffer`
// when `build` succeeds. A failed build's batch is released unsubmitted: it may
// name resources the failure path deletes. Returns what `build` returned.
template <typename Build>
bool submitIfBuilt(QRhi& rhi, QRhiCommandBuffer& command_buffer, Build&& build) {
  QRhiResourceUpdateBatch* batch = rhi.nextResourceUpdateBatch();
  const bool built = build(batch);
  if (built) {
    command_buffer.resourceUpdate(batch);
  } else {
    batch->release();
  }
  return built;
}

// Output pixels per logical pixel of `widget` rendering `output_size`: the
// fixed colour-buffer size and the DPR both show in it.
qreal outputDpr(const QWidget& widget, const QSize& output_size) {
  return widget.height() > 0 ? static_cast<qreal>(output_size.height()) / widget.height() : 1.0;
}

template <class... Visitors>
struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

// Submits one prepared draw through its family: the std::visit visitor over
// PreparedFrame::DrawRef.
struct SubmitDraw {
  QRhiCommandBuffer* command_buffer;
  ScenePass pass;
  const ScenePipelineRegistry& registry;
  const QRhiViewport& viewport;

  bool operator()(std::monostate /*unused*/) const {
    return false;
  }
  template <typename Ref>
  bool operator()(const Ref& ref) const {
    return ref.family->submit(command_buffer, *ref.draw, pass, registry, viewport);
  }
};

// What a family keys `layer`'s draws by (its drawsFor() argument) given the
// layer's registration; nullptr when the layer contributes nothing to it.
const PointRenderable* familyKey(
    const PointRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.points;
}
const Scene3DLayer* familyKey(
    const TrailRhiPass& /*family*/, const Scene3DLayer* layer, const SceneContributions& contributes) {
  return contributes.trail != nullptr || contributes.poses != nullptr ? layer : nullptr;
}
const PosesInFrameLayer* familyKey(
    const PoseRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.poses;
}
const OccupancyGridLayer* familyKey(
    const OccupancyRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.occupancy;
}
const VoxelGridLayer* familyKey(
    const VoxelRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.voxels;
}
const SceneEntitiesLayer* familyKey(
    const MarkerRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.markers;
}
const ModelRenderable* familyKey(
    const ModelRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.models;
}
const GridMapLayer* familyKey(
    const GridMapRhiPass& /*family*/, const Scene3DLayer* /*layer*/, const SceneContributions& contributes) {
  return contributes.grid_map;
}

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QRhiWidget(parent) {
  // Each per-view budget fills one quint32-sized buffer (the point and pose
  // budgets bound the sum of per-layer buffers). Prove both policies fit; the
  // marker and model passes check their own budgets.
  constexpr auto fits_buffers = [](const SceneLimits& limits) {
    constexpr std::uint64_t kMaxBytes = std::numeric_limits<quint32>::max();
    return limits.max_point_vertices_per_view * sizeof(PointVertex) <= kMaxBytes &&
           limits.max_pose_arms_per_view * sizeof(PoseTriadInstance) <= kMaxBytes;
  };
  static_assert(fits_buffers(kBrowserSceneLimits));
  static_assert(fits_buffers(kDesktopSceneLimits));

  pipelines_ = std::make_unique<ScenePipelineRegistry>();
  shared_geometry_ = std::make_unique<RhiSharedGeometry>();
  shadow_ = std::make_unique<ShadowResources>();
  hdr_ = std::make_unique<HdrResources>();
  ssao_ = std::make_unique<SsaoResources>();
  edl_ = std::make_unique<EdlResources>();
  occupancy_pass_ = std::make_unique<OccupancyRhiPass>();
  grid_pass_ = std::make_unique<GridRhiPass>();
  overlay_pass_ = std::make_unique<OverlayRhiPass>();
  pose_pass_ = std::make_unique<PoseRhiPass>();
  voxel_pass_ = std::make_unique<VoxelRhiPass>();
  marker_pass_ = std::make_unique<MarkerRhiPass>();
  model_pass_ = std::make_unique<ModelRhiPass>();
  point_pass_ = std::make_unique<PointRhiPass>();
  gridmap_pass_ = std::make_unique<GridMapRhiPass>();
  trail_pass_ = std::make_unique<TrailRhiPass>();
  frame_ = std::make_unique<PreparedFrame>();

  static const bool resources_initialized = [] {
    initializeScene3dRhiResources();
    return true;
  }();
  (void)resources_initialized;

  // QRhiWidget's API is immutable after platform resources exist.  Set it while
  // the widget is still parentless; SceneDockWidget reparents only after this
  // constructor returns.
  setApi(PJ::preferredGraphicsApi());
  setSampleCount(1);  // initialize() picks the output target's sample count per QRhi
  setObjectName(QStringLiteral("scene3dRhiCanvas"));
  setMinimumSize(320, 240);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  // Qt's own failure (no QRhi for this window, e.g. its API differs from the one
  // another widget pinned the window to) reaches the log and the dock warning too.
  connect(this, &QRhiWidget::renderFailed, this, [this] {
    if (!render_failure_.failed()) {
      setRenderFailure(tr("Scene3D has no graphics device: Qt could not create a QRhi for this window"), false);
    }
  });
}

SceneViewWidget::~SceneViewWidget() {
  releaseResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  // A new buffer can reuse the old one's address and revision: rebuild the TF geometry.
  tf_triad_key_.reset();
  grid_pass_->invalidate();
  last_frames_revision_ = ~uint64_t{0};
  refreshAvailableFrames();
  invalidateContent();
}

void SceneViewWidget::setLayers(const std::vector<Scene3DLayer*>& ordered_layers) {
  std::vector<PointRenderable*> points;
  std::vector<TrailRhiPass::Owner> trails;
  std::vector<PosesInFrameLayer*> poses;
  std::vector<OccupancyGridLayer*> occupancy;
  std::vector<VoxelGridLayer*> voxels;
  std::vector<SceneEntitiesLayer*> markers;
  std::vector<ModelRenderable*> models;
  std::vector<GridMapLayer*> grid_maps;
  const auto add = [](auto& family_layers, auto* layer) {
    if (layer != nullptr) {
      family_layers.push_back(layer);
    }
  };
  layers_ = ordered_layers;
  layer_contributions_.clear();
  for (Scene3DLayer* layer : ordered_layers) {
    const SceneContributions contributes = layer->sceneContributions();
    layer_contributions_.push_back(contributes);
    add(points, contributes.points);
    if (contributes.trail != nullptr || contributes.poses != nullptr) {
      trails.push_back({layer, contributes.trail, contributes.poses});
    }
    add(poses, contributes.poses);
    add(occupancy, contributes.occupancy);
    add(voxels, contributes.voxels);
    add(markers, contributes.markers);
    add(models, contributes.models);
    add(grid_maps, contributes.grid_map);
  }
  point_pass_->setLayers(points);
  trail_pass_->setLayers(trails);
  pose_pass_->setLayers(poses);
  occupancy_pass_->setLayers(occupancy);
  voxel_pass_->setLayers(voxels);
  marker_pass_->setLayers(markers);
  model_pass_->setLayers(models);
  gridmap_pass_->setLayers(grid_maps);
  invalidateContent();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint time) {
  if (render_time_ == time) {
    return;
  }
  render_time_ = time;
  refreshAvailableFrames();
  applyFollow();
  invalidateContent();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  follow_seeded_ = false;
  invalidateContent();  // the dock owns and persists the fixed frame, and reports its changes
}

void SceneViewWidget::setFollowFrame(const std::string& frame) {
  if (follow_frame_ == frame) {
    return;
  }
  follow_frame_ = frame;
  follow_seeded_ = false;
  applyFollow();
  invalidateContent();
}

std::optional<Transform> SceneViewWidget::followTransform(PJ::Timepoint time) const {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return std::nullopt;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, time, pj::tf::Clock::kEntry);
  return transform ? std::optional<Transform>(*transform) : std::nullopt;
}

void SceneViewWidget::applyFollow() {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, render_time_, pj::tf::Clock::kEntry);
  if (!transform) {
    follow_seeded_ = false;
    return;
  }
  if (!follow_seeded_) {
    follow_previous_origin_ = transform->t;
    follow_seeded_ = true;
    return;
  }
  const glm::dvec3 delta = transform->t - follow_previous_origin_;
  follow_previous_origin_ = transform->t;
  if (delta != glm::dvec3(0.0)) {
    camera_->followShift(glm::vec3(delta));
  }
}

void SceneViewWidget::recenterOnFollowFrame() {
  const auto transform = followTransform(render_time_);
  if (!transform) {
    return;
  }
  const CameraState before = camera_->state();
  camera_->followShift(glm::vec3(transform->t) - before.focal);
  follow_previous_origin_ = transform->t;
  follow_seeded_ = true;
  invalidateContent();
  emitPresentationIfChanged(before);
}

uint64_t SceneViewWidget::followRenderKey(PJ::Timepoint time) const {
  const auto transform = followTransform(time);
  if (!transform) {
    return 0;
  }
  uint64_t hash = PJ::kFnv1aOffsetBasis;
  for (const double component : {transform->t.x, transform->t.y, transform->t.z}) {
    hash = PJ::fnv1aMixBits(hash, component);
  }
  return hash == 0 ? 1U : hash;
}

void SceneViewWidget::setCameraModel(CameraModel model) {
  if (camera_model_ == model) {
    return;
  }
  const CameraState state = camera_->state();
  std::unique_ptr<ICamera> replacement;
  switch (model) {
    case CameraModel::kOrbit:
      replacement = std::make_unique<OrbitCamera>();
      break;
    case CameraModel::kXyOrbit:
      replacement = std::make_unique<XYOrbitCamera>();
      break;
    case CameraModel::kFly:
      replacement = std::make_unique<FlyCamera>();
      break;
    case CameraModel::kTopDownOrtho:
      replacement = std::make_unique<TopDownOrthoCamera>();
      break;
  }
  replacement->adoptState(state);
  replacement->setSceneBounds(scene_bounds_);
  camera_ = std::move(replacement);
  camera_model_ = model;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::resetCamera() {
  const CameraState before = camera_->state();
  camera_->reset();
  invalidateContent();
  emitPresentationIfChanged(before);
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::setGridStyle(GridStyle style) {
  if (grid_style_ == style) {
    return;
  }
  grid_style_ = style;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setGridDivisions(int divisions) {
  divisions = std::clamp(divisions, 1, 200);
  if (grid_divisions_ == divisions) {
    return;
  }
  grid_divisions_ = divisions;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setGridExtentMetres(float extent_m) {
  extent_m = std::clamp(extent_m, kGridExtentMinM, kGridExtentMaxM);
  if (grid_extent_m_ == extent_m) {
    return;
  }
  grid_extent_m_ = extent_m;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setGridVisible(bool visible) {
  if (grid_visible_ == visible) {
    return;
  }
  grid_visible_ = visible;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoSize(float length_m) {
  length_m = std::clamp(length_m, kGizmoSizeMinM, kGizmoSizeMaxM);
  if (gizmo_size_m_ == length_m) {
    return;
  }
  gizmo_size_m_ = length_m;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (gizmo_opacity_ == opacity) {
    return;
  }
  gizmo_opacity_ = opacity;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setTfConnectionsVisible(bool visible) {
  if (tf_connections_visible_ == visible) {
    return;
  }
  tf_connections_visible_ = visible;
  invalidateContent();
  emit presentationChanged();
}

void SceneViewWidget::setMeshShadingParams(const MeshShadingParams& params) {
  const bool changed = shading_params_.meshes_visible != params.meshes_visible ||
                       shading_params_.mesh_opacity != params.mesh_opacity ||
                       shading_params_.collisions_visible != params.collisions_visible ||
                       shading_params_.collision_opacity != params.collision_opacity ||
                       shading_params_.shadows_enabled != params.shadows_enabled;
  shading_params_ = params;
  if (changed) {
    invalidateContent();
    emit presentationChanged();
  }
}

void SceneViewWidget::grabPresentedFrame(std::function<void(const QImage&)> done) {
  invalidateContent();  // the presented frame must be a real one (TECHNICAL_NOTES §3)
  PJ::grabPresentedFrame(*this, std::move(done));
}

void SceneViewWidget::invalidateContent() {
  content_changed_ = true;
  update();
}

void SceneViewWidget::refreshAvailableFrames() {
  QList<FrameRow> rows;
  if (tf_ != nullptr) {
    const uint64_t revision = tf_->revision();
    if (revision == last_frames_revision_) {
      return;
    }
    last_frames_revision_ = revision;
    for (FrameRow& row : tf_->getFrameHierarchy()) {
      rows.append(std::move(row));
    }
  } else {
    last_frames_revision_ = ~uint64_t{0};
  }
  if (rows != last_frame_list_) {
    last_frame_list_ = rows;
    emit framesChanged(rows);
  }
}

// Places a layer's source frame in the fixed frame (placeObject(): at the object's
// own stamp, else at render_time_, with the layer's hold); nullopt when either frame
// is empty or TF cannot resolve the pair this frame — the caller skips the layer
// without consuming its per-view budget. Even a frame equal to the fixed one goes
// through placeObject(): a stamped object in a moving fixed frame stays where it was
// captured, as the dock's scene bounds and the layer's render key place it.
std::optional<Transform> SceneViewWidget::resolveFixedFromSource(
    const std::string& source_frame, const std::string& fixed_frame, std::optional<PJ::Timepoint> object_stamp,
    PlacementHold* hold) const {
  if (source_frame.empty() || fixed_frame.empty()) {
    return std::nullopt;
  }
  if (tf_ == nullptr) {
    return source_frame == fixed_frame ? std::optional<Transform>(Transform{}) : std::nullopt;
  }
  return placeObject(*tf_, fixed_frame, source_frame, object_stamp, render_time_, hold);
}

uint64_t SceneViewWidget::tfRenderKey(PJ::Timepoint time) const {
  const std::string fixed = effectiveFixedFrame();
  if (tf_ == nullptr || fixed.empty() || (!axes_visible_ && !tf_connections_visible_)) {
    return 0;
  }
  tf_->getAllFrames(tf_render_key_frames_);
  uint64_t combined = 0;
  for (const std::string& frame : tf_render_key_frames_) {
    uint64_t hash = std::hash<std::string>{}(frame);
    if (const auto transform = tf_->tryLookupTransform(fixed, frame, time, pj::tf::Clock::kEntry); transform) {
      hash = foldTransformIntoKey(hash, transform->matrix());
    } else {
      hash ^= 0xD15C0FFEEULL;
    }
    combined ^= hash ^ (hash >> 29U);
  }
  return combined;
}

std::string SceneViewWidget::effectiveFixedFrame() const {
  if (!fixed_frame_.empty()) {
    return fixed_frame_;
  }
  if (tf_ != nullptr) {
    const std::vector<FrameRow> hierarchy = tf_->getFrameHierarchy();
    if (!hierarchy.empty()) {
      return hierarchy.front().name;
    }
  }
  // A raw cloud is useful without /tf: use its own source frame as the fixed
  // frame and draw with the identity transform instead of silently producing an
  // empty view.
  for (const Scene3DLayer* layer : layers_) {
    if (const std::string& source = layer->renderFrameId(); !source.empty()) {
      return source;
    }
  }
  return {};
}

void SceneViewWidget::buildTfTriads(const glm::dvec3& render_origin, const std::string& fixed_frame) {
  TfTriadKey key{
      .tf = tf_.get(),
      .tf_revision = tf_ != nullptr ? tf_->revision() : 0U,
      .time = render_time_,
      .fixed_frame = fixed_frame,
      .render_origin = render_origin,
      .axes_visible = axes_visible_,
      .size = gizmo_size_m_,
      .opacity = gizmo_opacity_,
      .hovered_frame = hovered_frame_,
  };
  if (tf_triad_key_ == key) {
    return;
  }
  tf_triad_key_ = std::move(key);
  ++tf_triad_revision_;
  last_resolved_frame_count_ = 0;
  tf_triad_instances_.clear();
  if (tf_ == nullptr || fixed_frame.empty() || !axes_visible_) {
    return;
  }
  // Solid arrow triads. The frame's
  // fixed-from-frame transform (camera-relative) is baked into each arm's model,
  // so the draw uses an identity fixed_from_source uniform.
  const PoseTriadStyle triad_style{
      .axis_length = gizmo_size_m_, .opacity = gizmo_opacity_, .x_arrow_only = false, .override_color = false};
  std::pair<std::size_t, std::size_t> hovered_arms{0, 0};
  tf_->getAllFrames(tf_triad_frames_);
  for (const std::string& frame : tf_triad_frames_) {
    const auto transform = tf_->tryLookupTransform(fixed_frame, frame, render_time_, pj::tf::Clock::kEntry);
    if (!transform) {
      continue;
    }
    ++last_resolved_frame_count_;
    const std::size_t first_arm = tf_triad_instances_.size();
    appendTriadArms(relativeModel(transform->matrix(), render_origin), triad_style, tf_triad_instances_);
    if (hovered_frame_ == frame) {
      for (std::size_t arm = first_arm; arm < tf_triad_instances_.size(); ++arm) {
        glm::vec4& color = tf_triad_instances_[arm].color;
        color = glm::vec4(glm::min(glm::vec3(color) * look::kHighlightGain, glm::vec3(1.0F)), color.a);
      }
      hovered_arms = {first_arm, tf_triad_instances_.size()};
    }
  }
  // The hovered triad draws last, so it wins the LessOrEqual depth tie with a
  // coincident frame.
  const auto arms_begin = tf_triad_instances_.begin();
  std::rotate(
      arms_begin + static_cast<std::ptrdiff_t>(hovered_arms.first),
      arms_begin + static_cast<std::ptrdiff_t>(hovered_arms.second), tf_triad_instances_.end());
}

// The scene families in the order one layer's draws submit: a pose layer's
// trail under its triads, a SceneEntities layer's markers before its
// models.
template <typename Visitor>
void SceneViewWidget::forEachFamily(Visitor&& visitor) const {
  visitor(*point_pass_);
  visitor(*trail_pass_);
  visitor(*pose_pass_);
  visitor(*occupancy_pass_);
  visitor(*voxel_pass_);
  visitor(*marker_pass_);
  visitor(*model_pass_);
  visitor(*gridmap_pass_);
}

void SceneViewWidget::initialize(QRhiCommandBuffer* command_buffer) {
  // QRhiWidget calls this whenever it (re)created the render target: no frame
  // of ours is on it, so the Show skip must not present it.
  last_frame_request_.reset();
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr || renderTarget() == nullptr) {
    return;
  }
  const PJ::RhiTargetKey target = PJ::RhiTargetKey::of(*renderTarget(), sampleCount());
  if (resource_rhi_ != current_rhi || target != built_target_) {
    PJ::noteRhiAdoption(*this, resource_rhi_, current_rhi);
    releaseResources();
    resource_rhi_ = current_rhi;
    rhi_backend_name_ = QString::fromLatin1(current_rhi->backendName());
    max_3d_texture_size_ = max3dTextureSize(*current_rhi);
    // The output multisamples only where the GPU cannot run the HDR chain (the
    // direct pass draws the whole scene on it); over the chain's (already
    // multisampled) present it is single-sample, and the corner gizmo brings
    // its own 4x target. Decided per QRhi, not per frame: a change rebuilds
    // every resource, so a runtime HDR refusal (size, budget, a missing
    // pipeline variant) draws its direct pass without MSAA.
    if (const int samples = hdrUnsupportedReason(*current_rhi).isEmpty() ? 1 : kDirectSampleCount;
        sampleCount() != samples) {
      // QRhiWidget rebuilds its target and calls initialize() again: nothing is
      // built for this one (render() clears it).
      setSampleCount(samples);
      return;
    }
  }
  if (!baseResourcesReady() || !familiesReady(*current_rhi)) {
    if (const char* failed_part = createSceneResources(current_rhi, command_buffer); failed_part != nullptr) {
      failScene(failed_part);
      return;
    }
    built_target_ = target;
  }
  if (!overlay_pass_->ready() && !submitIfBuilt(*current_rhi, *command_buffer, [&](QRhiResourceUpdateBatch* updates) {
        RhiPassSetup setup = passSetup(current_rhi);
        setup.updates = updates;
        return overlay_pass_->initialize(setup, outputDpr(*this, renderTarget()->pixelSize()));
      })) {
    overlay_pass_->release();  // retried on the next initialize()
  }
  const QString overlay_failure =
      overlay_pass_->ready() ? QString{} : tr("The view overlays (corner axes, HUD, frame labels) are unavailable");
  if (overlay_failure != overlay_failure_) {
    overlay_failure_ = overlay_failure;
    if (!overlay_failure_.isEmpty()) {
      qWarning("SceneViewWidget QRhi: %s", qPrintable(overlay_failure_));
    }
    updateRenderingWarning();
  }
}

RhiPassSetup SceneViewWidget::passSetup(QRhi* rhi) const {
  return {
      .rhi = rhi,
      .render_mode_uniforms = render_mode_uniform_buffer_,
      .annotation_render_mode_uniforms = annotation_render_mode_uniform_buffer_,
      .view_uniforms = uniform_buffer_,
      .geometry = shared_geometry_.get(),
      .pipelines = pipelines_.get(),
  };
}

bool SceneViewWidget::baseResourcesReady() const {
  return uniform_buffer_ != nullptr && render_mode_uniform_buffer_ != nullptr &&
         annotation_render_mode_uniform_buffer_ != nullptr && model_uniform_buffer_ != nullptr &&
         shared_geometry_->cube_vertices != nullptr && pose_pass_->ready() && grid_pass_->ready();
}

bool SceneViewWidget::familiesReady(QRhi& rhi) const {
  bool ready = true;
  forEachLazyFamily(rhi, [&ready](const auto& family, const char* /*part*/, const auto& /*initialize*/) {
    ready = ready && family.ready();
  });
  return ready;
}

// The families built on demand, each with what createSceneResources() reports
// when it fails and how it is built; `visitor` sees only those the view needs
// now (a layer of theirs is in the view). Voxels need 3D textures.
template <typename Visitor>
void SceneViewWidget::forEachLazyFamily(QRhi& rhi, Visitor&& visitor) const {
  const auto visit = [&visitor](const auto& family, const char* part, const auto& initialize) {
    if (family.hasLayers()) {
      visitor(family, part, initialize);
    }
  };
  visit(*point_pass_, "point pipelines", [this](const RhiPassSetup& setup) { return point_pass_->initialize(setup); });
  visit(*trail_pass_, "trail pipelines", [this](const RhiPassSetup& setup) { return trail_pass_->initialize(setup); });
  visit(*occupancy_pass_, "occupancy pipelines", [this](const RhiPassSetup& setup) {
    return occupancy_pass_->initialize(setup);
  });
  if (rhi.isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
    visit(*voxel_pass_, "voxel pipelines", [this](const RhiPassSetup& setup) {
      return voxel_pass_->initialize(setup, max_3d_texture_size_);
    });
  }
  visit(
      *marker_pass_, "marker pipelines", [this](const RhiPassSetup& setup) { return marker_pass_->initialize(setup); });
  visit(*model_pass_, "model pipelines", [this](const RhiPassSetup& setup) {
    return model_pass_->initialize(setup, model_uniform_buffer_);
  });
  visit(*gridmap_pass_, "grid-map pipelines", [this](const RhiPassSetup& setup) {
    return gridmap_pass_->initialize(setup);
  });
}

const char* SceneViewWidget::createSceneResources(QRhi* rhi, QRhiCommandBuffer* command_buffer) {
  const char* failed_part = nullptr;
  submitIfBuilt(*rhi, *command_buffer, [&](QRhiResourceUpdateBatch* constants) {
    failed_part = createMissingResources(rhi, constants);
    return failed_part == nullptr;
  });
  return failed_part;
}

const char* SceneViewWidget::createMissingResources(QRhi* rhi, QRhiResourceUpdateBatch* constants) {
#ifdef PJ_RHI_TEST_HOOKS
  const std::int64_t build_begin_ns = steadyNowNs();
  const auto account = qScopeGuard([this, build_begin_ns] {
    ++stage_times_.base_builds;
    stage_times_.base_build_ns += steadyNowNs() - build_begin_ns;
    stage_times_.last_base_build_ms = static_cast<double>(steadyNowNs() - build_begin_ns) / 1.0e6;
  });
#endif
  if (!baseResourcesReady()) {
    if (!ensureUniformBuffer(rhi, uniform_buffer_, 64U) ||
        !ensureUniformBuffer(rhi, render_mode_uniform_buffer_, kRenderModeUniformBytes) ||
        !ensureUniformBuffer(rhi, annotation_render_mode_uniform_buffer_, kRenderModeUniformBytes) ||
        !ensureUniformBuffer(rhi, model_uniform_buffer_, kModelUniformBytes) ||
        !ensureUniformBuffer(rhi, ssao_uniform_buffer_, kSsaoUniformBytes)) {
      return "uniform buffers";
    }
    // SSAO's constant kernel; its frames write only the part before it.
    constants->updateDynamicBuffer(
        ssao_uniform_buffer_, static_cast<quint32>(offsetof(SsaoUniforms, kernel)),
        static_cast<quint32>(sizeof(SsaoUniforms::kernel)), ssaoKernel().data());
    if (shared_geometry_->cube_vertices == nullptr && !shared_geometry_->create(rhi, constants)) {
      return "shared meshes";
    }
    render_mode_hdr_written_ = -1;
    pipelines_->setDirectTarget(rhi, renderTarget()->renderPassDescriptor(), sampleCount());
    // The grid and the TF triads (the pose pass) draw without any layer.
    RhiPassSetup base_setup = passSetup(rhi);
    base_setup.updates = constants;
    base_setup.part = "pose pipelines";
    if (!pose_pass_->ready() && !pose_pass_->initialize(base_setup)) {
      return base_setup.part;
    }
    base_setup.part = "grid pipelines";
    if (!grid_pass_->ready() && !grid_pass_->initialize(base_setup)) {
      return base_setup.part;
    }
  }
  const char* failed_part = nullptr;
  const bool had_model = model_pass_->ready();
  forEachLazyFamily(*rhi, [&](const auto& family, const char* part, const auto& initialize) {
    if (failed_part != nullptr || family.ready()) {
      return;
    }
    // Registering builds no pipeline: the HDR chain creates only the variants
    // it draws with, and the direct ones are created on a direct frame.
    RhiPassSetup setup = passSetup(rhi);
    setup.updates = constants;
    setup.part = part;
    if (!initialize(setup)) {
      failed_part = part;
    }
  });
  if (model_pass_->ready() != had_model && shadow_->texture != nullptr) {
    // The shadow resources hold the model family's caster pipeline: rebuilt with
    // it on the next shadowed frame.
    releaseShadowResources(true);
  }
  return failed_part;
}

void SceneViewWidget::keepLastFrameOrClear(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target) {
  last_view_projection_ = shown_view_projection_;
  last_render_origin_ = shown_render_origin_;
  if (retainsLastFrame()) {
    return;  // recording nothing into the target keeps its last frame on screen
  }
  clearToBackdrop(command_buffer, target);
}

void SceneViewWidget::clearToBackdrop(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target) {
  command_buffer->beginPass(target, sceneThemeColors().backdrop, {1.0F, 0});
  command_buffer->endPass();
}

void SceneViewWidget::failScene(const char* failed_part) {
  releaseResources();
  setRenderFailure(tr("Scene3D cannot render with %1: its %2 could not be created")
                       .arg(rhi_backend_name_, QLatin1String(failed_part)));
}

void SceneViewWidget::setRenderFailure(QString reason, bool emit_failed) {
  const PJ::RhiFailureLatch::Change change = render_failure_.set(std::move(reason));
  if (change == PJ::RhiFailureLatch::Change::kNone) {
    return;
  }
  updateRenderingWarning();
  if (change == PJ::RhiFailureLatch::Change::kEntered) {
    qWarning("SceneViewWidget QRhi: %s", qPrintable(render_failure_.reason()));
    if (emit_failed) {
      emit renderFailed();
    }
  }
}

// The frame's camera: every matrix is relative to render_origin.
struct SceneViewWidget::FrameView {
  glm::dvec3 render_origin{0.0};
  glm::mat4 projection{1.0F};  // GL clip convention
  glm::mat4 view{1.0F};
  QMatrix4x4 clip_correction;
  QMatrix4x4 corrected_projection;
  QMatrix4x4 view_projection;  // corrected
  RhiConventions conventions;
  QMatrix4x4 texture_projection;  // view space -> render-target texture space (u, v, stored depth)
  QMatrix4x4 inverse_projection;
  bool inverse_projection_valid = false;  // SSAO and EDL rebuild view positions with it
};

// The prepared draws of one frame and the quality stages it runs. Every pass
// replays the draws (the HDR colour pass, the depth replay or the direct pass,
// the EDL mesh mask, the shadow map).
struct SceneViewWidget::PreparedFrame {
  template <typename Family>
  struct DrawOf {
    const Family* family = nullptr;
    const typename Family::Draw* draw = nullptr;
  };
  // std::monostate first: Clang 22 with libstdc++ 12 never emits a templated
  // visitor's call for alternative 0 (SubmitDraw), so that slot holds no draw.
  using DrawRef = std::variant<
      std::monostate, DrawOf<PointRhiPass>, DrawOf<TrailRhiPass>, DrawOf<PoseRhiPass>, DrawOf<OccupancyRhiPass>,
      DrawOf<VoxelRhiPass>, DrawOf<MarkerRhiPass>, DrawOf<ModelRhiPass>, DrawOf<GridMapRhiPass>>;
  // One prepared draw in submission terms: its logical slot (index into
  // layers_), blending class and family record.
  struct SceneDraw {
    std::size_t slot = 0;
    DrawClass draw_class = DrawClass::kOpaque;
    DrawRef draw;
  };

  // Everything the shadow map's content is a function of: the shadow resources
  // (a new map is empty), the model casters (a revision that moves whenever
  // their draws or instance data may have changed), the fitted light and the
  // render origin the casters are relative to.
  struct ShadowKey {
    std::uint64_t resource_generation = 0;
    std::uint64_t model_casters = 0;
    glm::mat4 light_view_projection{0.0F};
    glm::dvec3 render_origin{0.0};
    bool operator==(const ShadowKey&) const = default;
  };

  bool hdr_active = false;
  bool ssao_active = false;
  bool edl_active = false;
  bool shadow_active = false;
  ShadowKey shadow_key;
  // The key of the map the shadow texture holds (nullopt: none). The prepared
  // frame outlives the frame, so this is last shadowed frame's.
  std::optional<ShadowKey> rendered_shadow_key;
  QRhiViewport viewport;
  std::vector<SceneDraw> scene_draws;  // logical (dock) order
};

void SceneViewWidget::render(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  QRhiRenderTarget* target = renderTarget();
  if (current_rhi == nullptr || target == nullptr) {
    return;
  }
#ifdef PJ_RHI_TEST_HOOKS
  ++stage_times_.render_calls;
#endif
  // The failure state (or resources of another QRhi): no draw may reference them.
  if (resource_rhi_ != current_rhi || !baseResourcesReady()) {
    clearToBackdrop(command_buffer, target);
    return;
  }
  // A required pipeline that could not be created when a direct frame needed it.
  if (const auto& failure = pipelines_->failure();
      failure.has_value() && failure->severity == PipelineFailure::Severity::kRequired) {
    failScene(failure->part);
    clearToBackdrop(command_buffer, target);
    return;
  }
  // A family whose first layer arrived since initialize(): built before any
  // pass of this frame binds it.
  if (!familiesReady(*current_rhi)) {
    if (const char* failed_part = createSceneResources(current_rhi, command_buffer); failed_part != nullptr) {
      failScene(failed_part);
      clearToBackdrop(command_buffer, target);
      return;
    }
  }
  content_changed_ = false;  // an invalidateContent() from here on marks it again

#ifdef PJ_RHI_TEST_HOOKS
  stage_times_.render_begin_ns = steadyNowNs();
  stage_times_.last_gpu_ms = command_buffer->lastCompletedGpuTime() * 1000.0;
#endif
  if (show_perf_hud_) {
    cpu_timer_.start();
    if (const double gpu_seconds = command_buffer->lastCompletedGpuTime(); gpu_seconds > 0.0) {
      gpu_average_.add(gpu_seconds * 1000.0);
    }
  }
  // The HDR chain renders at render_scale x the output and the present pass
  // downsamples it (SSAA); the direct fallback stays at output size.
  const QSize output_size = target->pixelSize();
  const QSize scaled_size(
      std::max(1, static_cast<int>(std::lround(static_cast<float>(output_size.width()) * render_scale_))),
      std::max(1, static_cast<int>(std::lround(static_cast<float>(output_size.height()) * render_scale_))));
  // The one owner of the frame's stage flags: HDR, SSAO and EDL here, shadows in
  // prepareFrame() once the casters are known.
  PreparedFrame& frame = *frame_;
  if (pipelines_->failure().has_value()) {
    // Last frame drew without a pipeline whose HDR variant failed (a required
    // failure returned above); the direct pass has them all.
    rejectHdr(scaled_size, tr("A scene pipeline has no HDR variant on this GPU"));
  }
  frame.hdr_active = ensureHdrResources(current_rhi, scaled_size);
  hdr_->state.last_active = frame.hdr_active;
  const QSize render_size = frame.hdr_active ? scaled_size : output_size;

  // The frame contract (rhi_scene_pass.h): admit once, prepare once;
  // every pass below replays `frame`.
  FrameAdmission admission;
#ifdef PJ_RHI_TEST_HOOKS
  stage_times_.targets_ready_ns = steadyNowNs();
#endif
  admitFrame(current_rhi, admission);
#ifdef PJ_RHI_TEST_HOOKS
  stage_times_.admitted_ns = steadyNowNs();
#endif
  const FrameView view = frameView(current_rhi);
  shown_view_projection_ = last_view_projection_;
  shown_render_origin_ = last_render_origin_;
  last_view_projection_ = view.projection * view.view;
  last_render_origin_ = view.render_origin;
  frame.ssao_active = frame.hdr_active && composite_params_.ssao_enabled &&
                      ensureSsaoResources(current_rhi, scaled_size) && view.inverse_projection_valid;
  frame.edl_active = frame.hdr_active && composite_params_.edl_enabled &&
                     ensureEdlResources(current_rhi, scaled_size) && view.inverse_projection_valid;
  ssao_->state.last_active = frame.ssao_active;
  edl_->state.last_active = frame.edl_active;
  QRhiResourceUpdateBatch* updates = prepareFrame(current_rhi, render_size, view, admission, frame);
  if (show_perf_hud_) {
    cpu_average_.add(static_cast<double>(cpu_timer_.nsecsElapsed()) / 1.0e6);
  }
  prepareOverlays(updates, output_size, view.view, admission.fixed_frame);
#ifdef PJ_RHI_TEST_HOOKS
  stage_times_.prepared_ns = steadyNowNs();
#endif
  // Without the HDR chain the direct pipelines draw: every one is created
  // before any pass, so a pipeline that cannot be created shows no frame with
  // its draws missing. The frame's uploads still run; the target keeps its last
  // frame (or the backdrop), and the next render() enters the failure state.
  if (!frame.hdr_active && !pipelines_->createDirects()) {
    command_buffer->resourceUpdate(updates);
    keepLastFrameOrClear(command_buffer, target);
    invalidateContent();
#ifdef PJ_RHI_TEST_HOOKS
    ++stage_times_.frames_withheld;
#endif
    return;
  }
  updates = overlay_pass_->submitGizmoTarget(command_buffer, updates);
  if (frame.shadow_active) {
    // The map is kept across frames: re-rendered only when an input changed
    // (an orbit of a static scene keeps it; a pan moves the render origin).
    if (frame.rendered_shadow_key != frame.shadow_key) {
      submitShadowPass(command_buffer, updates);
      updates = nullptr;
      frame.rendered_shadow_key = frame.shadow_key;
#ifdef PJ_RHI_TEST_HOOKS
      ++stage_times_.shadow_passes;
    } else {
      ++stage_times_.shadow_passes_reused;
#endif
    }
  }

  // Scene backdrop tracks the active theme (the same token, read per frame). A
  // PaletteChange invalidates the content, so a theme switch re-applies here.
  // The HDR target is linear-light — the present pass re-encodes to sRGB — so
  // linearize the display-referred token on write.
  const QColor scene_bg = sceneThemeColors().backdrop;
  const auto linear_channel = [](float channel) { return std::pow(channel, 2.2F); };
  const QColor hdr_scene_clear = QColor::fromRgbF(
      linear_channel(static_cast<float>(scene_bg.redF())), linear_channel(static_cast<float>(scene_bg.greenF())),
      linear_channel(static_cast<float>(scene_bg.blueF())), 1.0F);
  if (frame.hdr_active) {
    submitScenePass(command_buffer, hdr_->target.get(), ScenePass::kHdrColor, hdr_scene_clear, updates, frame);
    updates = nullptr;
    // Reusing the same immutable HDR mode keeps both passes independent of
    // dynamic-buffer update ordering. The replay's RGBA8 alpha records the
    // shaders' post-discard opacity while its D32F attachment records depth.
    if (hdr_->depth_resolve) {
      // The HDR pass resolved the depth; only the coverage the stages below write into needs clearing.
      command_buffer->beginPass(hdr_->depth_target.get(), Qt::transparent, {1.0F, 0});
      command_buffer->endPass();
    } else {
      submitScenePass(
          command_buffer, hdr_->depth_target.get(), ScenePass::kDepthReplay, Qt::transparent, nullptr, frame);
    }
    if (frame.ssao_active && ssao_->target != nullptr) {
      // Preserve replay alpha/red and write the raw scene-wide AO into the
      // otherwise-unused green channel. The sampled D32F texture is not attached
      // to this color-only target, avoiding a read/write feedback loop.
      command_buffer->beginPass(ssao_->target.get(), Qt::transparent, {1.0F, 0});
      command_buffer->setGraphicsPipeline(ssao_->pipeline.get());
      command_buffer->setViewport(frame.viewport);
      command_buffer->setShaderResources(ssao_->bindings.get());
      command_buffer->draw(3U);
      command_buffer->endPass();
    }
    // The mask pass preserves the coverage it is begun on: with no model draw it
    // would change nothing (EDL then treats every pixel as non-mesh anyway).
    if (frame.edl_active && edl_->target != nullptr && model_pass_->meshMaskReady() && model_pass_->hasDraws()) {
      submitEdlMeshMask(command_buffer, frame);
    }
    if (pipelines_->failure().has_value()) {
      // A variant could not be created: its draws are missing from the chain.
      // Present nothing of it; the next frame draws without the HDR chain.
      keepLastFrameOrClear(command_buffer, target);
      invalidateContent();
#ifdef PJ_RHI_TEST_HOOKS
      ++stage_times_.frames_withheld;
#endif
      return;
    }
    command_buffer->beginPass(target, Qt::transparent, {1.0F, 0});
    command_buffer->setGraphicsPipeline(hdr_->present_pipeline.get());
    command_buffer->setViewport(
        QRhiViewport(0.0F, 0.0F, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    command_buffer->setShaderResources(hdr_->present_bindings.get());
    command_buffer->draw(3U);
    overlay_pass_->submit(command_buffer);
    command_buffer->endPass();
  } else {
    // Direct-to-backing fallback: no HDR encode, so use the theme token as-is.
    submitScenePass(command_buffer, target, ScenePass::kDirect, scene_bg, updates, frame);
  }
  if (pipelines_->failure().has_value()) {
    invalidateContent();  // the next frame drops HDR
  }
  last_frame_request_ = requestedTarget();
  // A full frame: whatever failed before (a pipeline, no QRhi) works now. Not
  // cleared in initialize(): pipelines are created as frames draw with them.
  setRenderFailure({});
#ifdef PJ_RHI_TEST_HOOKS
  stage_times_.point_uploads = point_pass_->uploadCountForTest();
  stage_times_.point_upload_bytes = point_pass_->uploadedBytesForTest();
  stage_times_.marker_rebuilds = marker_pass_->layerRebuildCountForTest();
  stage_times_.model_mesh_builds = static_cast<std::uint64_t>(model_pass_->cacheCountersForTest().mesh_builds);
  stage_times_.render_end_ns = steadyNowNs();
  ++stage_times_.full_renders;
  stage_times_.targets_ns += stage_times_.targets_ready_ns - stage_times_.render_begin_ns;
  stage_times_.admit_ns += stage_times_.admitted_ns - stage_times_.targets_ready_ns;
  stage_times_.prepare_ns += stage_times_.prepared_ns - stage_times_.admitted_ns;
  stage_times_.submit_ns += stage_times_.render_end_ns - stage_times_.prepared_ns;
#endif
}

bool SceneViewWidget::event(QEvent* event) {
#ifdef PJ_RHI_TEST_HOOKS
  if (event->type() == QEvent::Paint) {
    ++stage_times_.paint_events;
  }
#endif
  if (event->type() != QEvent::Show) {
    return QRhiWidget::event(event);
  }
  // QRhiWidget's Show handling sends its paint event synchronously, back through
  // this function: the flag holds for the whole Show, nested Paint included.
  const QScopedValueRollback in_show(show_paint_pending_, true);
  return QRhiWidget::event(event);
}

void SceneViewWidget::paintEvent(QPaintEvent* event) {
  // Qt paints a render-to-texture widget on its own too: synchronously inside a
  // Show, and in a parent's repaint once a repaint request reached it while hidden.
  if ((show_paint_pending_ || !content_changed_) && retainsLastFrame()) {
#ifdef PJ_RHI_TEST_HOOKS
    ++stage_times_.paints_skipped;
#endif
    // No offscreen frame at all (on Vulkan its end waits on a fence even when
    // empty): the target keeps the last frame for the top-level to composite.
    if (content_changed_) {
      QMetaObject::invokeMethod(this, qOverload<>(&QWidget::update), Qt::QueuedConnection);
    }
    return;
  }
  QRhiWidget::paintEvent(event);
}

bool SceneViewWidget::retainsLastFrame() const {
  // last_frame_request_ is reset by every release (a new top-level or a destroyed
  // QRhi) and every initialize() (a recreated target), so a set one means the
  // target holds our last full frame, as that request built it. Comparing it
  // with the current request catches a resize or a format change QRhiWidget has
  // not applied yet: setColorBufferFormat(), setFixedColorBufferSize() and a
  // resize only request a new target, which QRhiWidget builds in the paint this
  // would skip. A resize or DPR change under a fixed colour-buffer size keeps the
  // target but moves the overlays, which the logical size and DPR catch.
  return last_frame_request_.has_value() && *last_frame_request_ == requestedTarget() && rhi() != nullptr &&
         rhi() == resource_rhi_;
}

SceneViewWidget::TargetRequest SceneViewWidget::requestedTarget() const {
  // QRhiWidget's own rule (ensureTexture()): the fixed size, else the widget in device pixels.
  const QSize fixed = fixedColorBufferSize();
  const qreal dpr = devicePixelRatio();
  return {colorBufferFormat(), fixed.isEmpty() ? size() * dpr : fixed, sampleCount(), size(), dpr};
}

// Frame contract step 1: runs every visible layer's prepareForRender() against
// the per-view budgets and places it in the fixed frame. The only place a
// budget is consumed.
void SceneViewWidget::admitFrame(QRhi* rhi, FrameAdmission& admission) {
  admission.fixed_frame = effectiveFixedFrame();
  const std::string& fixed_frame = admission.fixed_frame;
  const PlaceFn place = [this, &fixed_frame](
                            const std::string& source_frame, std::optional<PJ::Timepoint> object_stamp,
                            PlacementHold* hold) {
    return resolveFixedFromSource(source_frame, fixed_frame, object_stamp, hold);
  };
  point_pass_->admit(place);
  pose_pass_->admit(place);
  occupancy_pass_->admit(place);
  gridmap_pass_->admit(place);
  voxel_pass_->admit(rhi, place);
  marker_pass_->admit(place);
  model_pass_->admit(place, shading_params_, shading_params_.shadows_enabled && tf_ != nullptr);
}

// Frame contract step 2: the camera matrices. The aspect is the logical
// widget's, whatever the DPR and render scale round the framebuffer to.
SceneViewWidget::FrameView SceneViewWidget::frameView(QRhi* rhi) const {
  FrameView view;
  view.render_origin = glm::dvec3(camera_->state().focal);
  const float aspect = static_cast<float>(width()) / static_cast<float>(std::max(height(), 1));
  view.projection = camera_->projMatrix(aspect);
  view.view = camera_->viewMatrixRelativeTo(view.render_origin);
  view.clip_correction = rhi->clipSpaceCorrMatrix();
  view.corrected_projection = view.clip_correction * toQMatrix(view.projection);
  view.view_projection = view.clip_correction * toQMatrix(view.projection * view.view);
  view.conventions = rhiConventions(*rhi);
  view.texture_projection = clipToTextureMatrix(view.conventions) * view.corrected_projection;
  view.inverse_projection = view.texture_projection.inverted(&view.inverse_projection_valid);
  return view;
}

// Frame contract step 3: builds this frame's GPU data (streams, instances,
// uploads, uniforms) from the admission and orders every draw for submission.
// Returns the update batch the first pass must consume.
QRhiResourceUpdateBatch* SceneViewWidget::prepareFrame(
    QRhi* rhi, const QSize& render_size, const FrameView& view, const FrameAdmission& admission, PreparedFrame& frame) {
  const glm::dvec3& render_origin = view.render_origin;
  QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
  const RhiFrameContext context{rhi, updates, render_origin, view.view_projection, view.view};
  if (const int hdr = frame.hdr_active ? 1 : 0; hdr != render_mode_hdr_written_) {
    const RenderModeUniforms data_render_mode{{hdr, 0, 0, 0}};
    const RenderModeUniforms annotation_render_mode{{hdr, 1, 0, 0}};
    updates->updateDynamicBuffer(render_mode_uniform_buffer_, 0, kRenderModeUniformBytes, &data_render_mode);
    updates->updateDynamicBuffer(
        annotation_render_mode_uniform_buffer_, 0, kRenderModeUniformBytes, &annotation_render_mode);
    render_mode_hdr_written_ = hdr;
  }

  buildTfTriads(render_origin, admission.fixed_frame);
  grid_pass_->build(
      GridRhiPass::Settings{
          grid_visible_, grid_style_, grid_extent_m_, grid_divisions_, tf_connections_visible_,
          sceneThemeColors().grid},
      tf_.get(), admission.fixed_frame, render_time_, render_origin);
  last_line_vertex_count_ = grid_pass_->lastLineVertexCount();
  last_triangle_vertex_count_ = grid_pass_->lastTriangleVertexCount();
  const bool shadows_requested = shading_params_.shadows_enabled && tf_ != nullptr;
  // A family never built (no layer of its kind since the last release) prepares nothing.
  marker_pass_->prepare(context);
  model_pass_->prepare(context, shading_params_);

  if (!shading_params_.shadows_enabled && shadow_->texture != nullptr) {
    releaseShadowResources(true);
  }
  AABB render_shadow_bounds = model_pass_->shadowCasterBounds();
  if (render_shadow_bounds.valid) {
    render_shadow_bounds = extendAabbToGroundShadow(
        render_shadow_bounds, shading_params_.key_light_dir, static_cast<float>(-render_origin.z));
  }
  last_shadow_bounds_ = render_shadow_bounds;
  const ShadowCameraFit shadow_fit =
      fitDirectionalShadowCamera(render_shadow_bounds, shading_params_.key_light_dir, kShadowMapSize);
  last_shadow_fit_valid_ = shadow_fit.valid;
  const bool shadow_requested = shadows_requested && model_pass_->hasShadowCasters() && shadow_fit.valid;
  bool shadow_active = shadow_requested && ensureShadowResources(rhi);
  if (shadow_active && !model_pass_->uploadShadowCasters(context)) {
    shadow_->state.capability_error = tr("Could not allocate the shadow instance buffer");
    qWarning("SceneViewWidget QRhi shadows unavailable: %s", qPrintable(shadow_->state.capability_error));
    releaseShadowResources(true);
    shadow_active = false;
  }
  frame.shadow_active = shadow_active;
  frame.shadow_key = {
      shadow_->state.resource_generation, model_pass_->shadowRevision(), shadow_fit.light_view_proj, render_origin};
  shadow_->state.last_active = shadow_active;
  if (shadow_requested && !shadow_active && !shadow_->state.capability_error.isEmpty()) {
    if (noted_shadow_failure_ != shadow_->state.capability_error) {
      noted_shadow_failure_ = shadow_->state.capability_error;
      model_pass_->noteShadowFailure(shadow_->state.capability_error);
    }
  } else {
    noted_shadow_failure_.clear();
  }

  const QMatrix4x4 shadow_matrix =
      view.clip_correction * toQMatrix(shadow_fit.valid ? shadow_fit.light_view_proj : glm::mat4(1.0F));
  if (frame.hdr_active) {
    CompositeUniforms composite;
    composite.params = {
        composite_params_.exposure,
        composite_params_.saturation,
        kBackgroundCoverageThreshold,
        composite_params_.edl_floor,
    };
    copyMatrix(view.inverse_projection, composite.inverse_projection);
    // The EDL neighbour radius is in render pixels: scale it with the supersampling.
    composite.edl_params = {
        look::kEdlStrength,
        look::kEdlRadiusPx * render_scale_,
        look::kEdlMaxGap,
        frame.edl_active ? 1.0F : 0.0F,
    };
    composite.ssao_params = {
        composite_params_.ao_strength,
        frame.ssao_active ? 1.0F : 0.0F,
        hdr_->depth_resolve ? 1.0F : 0.0F,
        0.0F,
    };
    composite.grade_params = {static_cast<float>(std::clamp(composite_params_.tonemap_mode, 0, 3)), 0.0F, 0.0F, 0.0F};
    updates->updateDynamicBuffer(hdr_->composite_uniforms.get(), 0, kCompositeUniformBytes, &composite);
    if (frame.ssao_active && ssao_uniform_buffer_ != nullptr) {
      SsaoUniforms ssao_uniforms;
      copyMatrix(view.texture_projection, ssao_uniforms.projection);
      copyMatrix(view.inverse_projection, ssao_uniforms.inverse_projection);
      ssao_uniforms.params = {
          look::kSsaoRadiusM,
          look::kSsaoPower,
          look::kSsaoBias,
          textureVSign(view.conventions),
      };
      updates->updateDynamicBuffer(
          ssao_uniform_buffer_, 0, static_cast<quint32>(offsetof(SsaoUniforms, kernel)), &ssao_uniforms);
    }
  }
  if (uniform_buffer_ != nullptr) {
    const QMatrix4x4 view_uniform = view.corrected_projection * toQMatrix(view.view);
    updates->updateDynamicBuffer(uniform_buffer_, 0, 64, view_uniform.constData());
  }
  if (shadow_active && shadow_->uniforms != nullptr) {
    updates->updateDynamicBuffer(shadow_->uniforms.get(), 0, kShadowUniformBytes, shadow_matrix.constData());
  }
  if (model_uniform_buffer_ != nullptr) {
    const glm::dvec3 camera_position = glm::dvec3(camera_->position()) - render_origin;
    const glm::vec3 key_direction = glm::normalize(shading_params_.key_light_dir);
    ModelUniforms model_uniforms;
    model_uniforms.camera_position = {
        static_cast<float>(camera_position.x), static_cast<float>(camera_position.y),
        static_cast<float>(camera_position.z), 0.0F};
    model_uniforms.lighting = {
        shading_params_.reflectivity, shading_params_.ambient_scale, shading_params_.direct_scale,
        shading_params_.fill_light_scale};
    model_uniforms.environment = {key_direction.x, key_direction.y, key_direction.z, shading_params_.env_intensity};
    copyMatrix(clipToTextureMatrix(view.conventions) * shadow_matrix, model_uniforms.light_view_projection);
    model_uniforms.shadow_params = {
        shadow_active ? shadow_fit.world_units_per_texel * look::kShadowNormalOffsetTexels : 0.0F,
        look::kShadowSoftnessTexels, shadow_active ? 1.0F : 0.0F, static_cast<float>(shadow_->mapSize())};
    updates->updateDynamicBuffer(model_uniform_buffer_, 0, kModelUniformBytes, &model_uniforms);
  }

  // Screen-space point sizes are in output pixels; the HDR chain renders at
  // render_scale times them.
  const float pixel_scale = frame.hdr_active ? render_scale_ : 1.0F;
  point_pass_->prepare(
      context,
      PointRhiPass::ViewInputs{view.projection, render_size, static_cast<float>(devicePixelRatio()), pixel_scale});
  pose_pass_->prepare(context, tf_triad_instances_, tf_triad_revision_);
  grid_pass_->prepare(context, shadow_active);
  occupancy_pass_->prepare(context);
  voxel_pass_->prepare(context);
  gridmap_pass_->prepare(context, shading_params_);
  trail_pass_->prepare(context, admission.fixed_frame, render_time_, render_size, pixel_scale);

  frame.viewport =
      QRhiViewport(0.0F, 0.0F, static_cast<float>(render_size.width()), static_cast<float>(render_size.height()));

  // Logical order: the dock's heterogeneous layer order; within a slot, the
  // forEachFamily() order.
  frame.scene_draws.clear();
  for (std::size_t slot = 0; slot < layers_.size(); ++slot) {
    forEachFamily([&](const auto& family) {
      if (const auto* key = familyKey(family, layers_[slot], layer_contributions_[slot]); key != nullptr) {
        using Family = std::remove_cvref_t<decltype(family)>;
        for (const auto& draw : family.drawsFor(key)) {
          frame.scene_draws.push_back({slot, draw.draw_class, PreparedFrame::DrawOf<Family>{&family, &draw}});
        }
      }
    });
  }
  return updates;
}

void SceneViewWidget::submitShadowPass(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates) {
  command_buffer->beginPass(shadow_->target.get(), Qt::transparent, {1.0F, 0}, updates);
  const QRhiViewport viewport(0.0F, 0.0F, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize));
  model_pass_->submitShadowCasters(command_buffer, viewport);
  command_buffer->endPass();
}

// Records one scene pass: the prelude (grid, TF lines and triads), then every
// prepared opaque draw in logical order, then every translucent one.
void SceneViewWidget::submitScenePass(
    QRhiCommandBuffer* command_buffer, QRhiRenderTarget* target, ScenePass scene_pass, const QColor& clear,
    QRhiResourceUpdateBatch* updates, const PreparedFrame& frame) {
  command_buffer->beginPass(target, clear, {1.0F, 0}, updates);
  const QRhiViewport& viewport = frame.viewport;
  grid_pass_->submit(command_buffer, scene_pass, *pipelines_, viewport);
  pose_pass_->submitTfTriads(command_buffer, scene_pass, *pipelines_, viewport);
  submitted_slots_.assign(layers_.size(), 0);
  for (const DrawClass phase : {DrawClass::kOpaque, DrawClass::kTranslucent}) {
    for (const PreparedFrame::SceneDraw& scene_draw : frame.scene_draws) {
      if (scene_draw.draw_class != phase) {
        continue;
      }
      if (std::visit(SubmitDraw{command_buffer, scene_pass, *pipelines_, viewport}, scene_draw.draw)) {
        submitted_slots_[scene_draw.slot] = 1;
      }
    }
  }
  if (scene_pass != ScenePass::kDepthReplay) {
    last_submitted_layer_ids_.clear();
    for (std::size_t slot = 0; slot < submitted_slots_.size(); ++slot) {
      if (submitted_slots_[slot] != 0) {
        last_submitted_layer_ids_.push_back(layers_[slot]->info().topic_id.id);
      }
    }
  }
  if (scene_pass == ScenePass::kDirect) {
    overlay_pass_->submit(command_buffer);
  }
  command_buffer->endPass();
}

// Writes the mesh-only EDL mask into the replay coverage. Reuses the prepared
// model ranges, so this visibility pass sits outside every per-frame budget.
void SceneViewWidget::submitEdlMeshMask(QRhiCommandBuffer* command_buffer, const PreparedFrame& frame) {
  // Preserve the full-scene replay depth and alpha coverage, then reserve
  // coverage.r for the mesh-only EDL mask.
  command_buffer->beginPass(edl_->target.get(), Qt::transparent, {1.0F, 0});
  model_pass_->submitMeshMask(command_buffer, frame.viewport);
  command_buffer->endPass();
}

void SceneViewWidget::discardShadowMap() {
  frame_->rendered_shadow_key.reset();
}

void SceneViewWidget::releaseResources() {
  PJ::noteRhiRelease(*this, resource_rhi_);
  last_frame_request_.reset();
  releaseShadowResources(false);
  releaseHdrResources();
  // Every scene pipeline, before the family resources (layout SRBs) they bind.
  pipelines_->clear();
  forEachFamily([](auto& family) { family.release(); });
  grid_pass_->release();
  overlay_pass_->release();
  frame_->scene_draws.clear();
  discardShadowMap();
  // After every pipeline that draws the shared cube/arrow meshes.
  shared_geometry_->release();
  delete ssao_uniform_buffer_;
  ssao_uniform_buffer_ = nullptr;
  delete model_uniform_buffer_;
  delete annotation_render_mode_uniform_buffer_;
  delete render_mode_uniform_buffer_;
  delete uniform_buffer_;
  model_uniform_buffer_ = nullptr;
  annotation_render_mode_uniform_buffer_ = nullptr;
  render_mode_uniform_buffer_ = nullptr;
  uniform_buffer_ = nullptr;
  max_3d_texture_size_ = 0;
  shadow_->state.capability_error.clear();
  shadow_->state.rejected_size = {};
  noted_shadow_failure_.clear();
  // Preserve an active warning across RHI teardown. Clearing the rejected
  // sizes below permits the new RHI to probe again; a successful allocation
  // then clears the warning through renderingWarningChanged().
  hdr_->state.rejected_size = {};
  ssao_->state.rejected_size = {};
  edl_->state.rejected_size = {};
  last_shadow_fit_valid_ = false;
  last_shadow_bounds_ = {};
  resource_rhi_ = nullptr;
  built_target_ = {};
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_position_ = event->position().toPoint();
  active_button_ = event->button();
  camera_gesture_changed_ = false;
  // The hover label stays hidden for the whole gesture; the next button-free move re-picks it.
  setHoveredFrame(std::nullopt);
  event->accept();
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == active_button_) {
    active_button_ = Qt::NoButton;
    if (camera_gesture_changed_) {
      camera_gesture_changed_ = false;
      emit presentationChanged();
    }
  }
  event->accept();
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  if (active_button_ != Qt::NoButton && !(event->buttons() & active_button_)) {
    // The release went elsewhere (e.g. to the frame context menu's popup, which
    // grabs the mouse on the press): end the stale gesture as its release would.
    active_button_ = Qt::NoButton;
    if (camera_gesture_changed_) {
      camera_gesture_changed_ = false;
      emit presentationChanged();
    }
  }
  if (active_button_ == Qt::NoButton) {
    setHoveredFrame(frameAt(event->position()));
  }
  const QPoint position = event->position().toPoint();
  const float delta_x = static_cast<float>(position.x() - last_mouse_position_.x());
  const float delta_y = static_cast<float>(position.y() - last_mouse_position_.y());
  last_mouse_position_ = position;
  const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
  const CameraState before = camera_->state();
  if (active_button_ == Qt::LeftButton && !shift) {
    camera_->rotate(delta_x, delta_y);
  } else if (active_button_ == Qt::MiddleButton || (active_button_ == Qt::LeftButton && shift)) {
    camera_->pan(delta_x, delta_y);
  } else if (active_button_ == Qt::RightButton) {
    // Centre-of-view zoom: anchoring each drag delta at the cursor would walk the focal.
    camera_->zoom(delta_y * 0.01F);
  } else {
    return;
  }
  if (before != camera_->state()) {
    camera_gesture_changed_ = true;
  }
  invalidateContent();
  event->accept();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0F;
  const CameraState before = camera_->state();
  camera_->zoomToCursor(ticks, glm::vec2(event->position().x(), event->position().y()), width(), height());
  invalidateContent();
  emitPresentationIfChanged(before);
  event->accept();
}

void SceneViewWidget::changeEvent(QEvent* event) {
  QRhiWidget::changeEvent(event);
  if (event->type() == QEvent::PaletteChange) {
    // Theme colours are baked into the overlay images.
    hud_lines_.clear();
    hover_label_frame_.clear();
    hover_ring_image_ = {};
    invalidateContent();
  }
}

void SceneViewWidget::leaveEvent(QEvent* event) {
  setHoveredFrame(std::nullopt);
  QRhiWidget::leaveEvent(event);
}

void SceneViewWidget::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_P) {
    setShowPerfHud(!show_perf_hud_);
    event->accept();
    return;
  }
  QRhiWidget::keyPressEvent(event);
}

void SceneViewWidget::contextMenuEvent(QContextMenuEvent* event) {
  if (const auto frame = frameAt(event->pos()); frame.has_value()) {
    emit frameContextMenuRequested(QString::fromStdString(*frame), event->globalPos());
    event->accept();
    return;
  }
  event->ignore();
}

void SceneViewWidget::setShowPerfHud(bool on) {
  if (show_perf_hud_ == on) {
    return;
  }
  show_perf_hud_ = on;
  invalidateContent();
}

void SceneViewWidget::setRenderScale(float scale) {
  const float clamped = std::clamp(scale, 1.0F, 4.0F);
  if (render_scale_ == clamped) {
    return;
  }
  render_scale_ = clamped;
  invalidateContent();
}

std::optional<glm::vec2> SceneViewWidget::projectToWidget(const glm::dvec3& point) const {
  return projectFrameOrigin(
      last_view_projection_, glm::vec2(static_cast<float>(width()), static_cast<float>(height())),
      glm::vec3(point - last_render_origin_));
}

std::optional<std::string> SceneViewWidget::frameAt(const QPointF& logical_pos) const {
  const std::string fixed = effectiveFixedFrame();
  if (!axes_visible_ || tf_ == nullptr || fixed.empty() || width() <= 0 || height() <= 0) {
    return std::nullopt;
  }
  tf_->getAllFrames(pick_frames_);
  // Index-aligned with pick_frames_; an unresolvable frame gets an off-screen sentinel.
  pick_points_.clear();
  for (const std::string& frame : pick_frames_) {
    std::optional<glm::vec2> projected;
    if (const auto transform = tf_->tryLookupTransform(fixed, frame, render_time_, pj::tf::Clock::kEntry); transform) {
      projected = projectToWidget(transform->t);
    }
    pick_points_.push_back(projected.value_or(glm::vec2(-1.0e6F)));
  }
  const auto index = pickNearestFrame(
      pick_points_, glm::vec2(static_cast<float>(logical_pos.x()), static_cast<float>(logical_pos.y())),
      kFramePickRadiusPx);
  return index.has_value() ? std::optional<std::string>(pick_frames_[*index]) : std::nullopt;
}

void SceneViewWidget::setHoveredFrame(std::optional<std::string> frame) {
  if (frame == hovered_frame_) {
    return;
  }
  hovered_frame_ = std::move(frame);
  invalidateContent();
  emit hoveredFrameChanged(hovered_frame_ ? QString::fromStdString(*hovered_frame_) : QString{});
}

void SceneViewWidget::prepareOverlays(
    QRhiResourceUpdateBatch* updates, const QSize& output_size, const glm::mat4& view, const std::string& fixed_frame) {
  if (!overlay_pass_->ready()) {
    last_overlay_rects_ = {};
    return;
  }
  const qreal dpr = outputDpr(*this, output_size);
  if (dpr != overlay_image_dpr_) {
    overlay_image_dpr_ = dpr;
    hud_lines_.clear();
    hover_label_frame_.clear();
    hover_ring_image_ = {};
  }
  const auto fw_theme = PJ::theme::appTheme();
  std::array<OverlayRhiPass::Image, OverlayRhiPass::kSlotCount> images{};

  // Re-rasterise (and re-upload) the HUD at a readable rate, not every frame.
  constexpr qint64 kHudRefreshMs = 250;
  if (!show_perf_hud_) {
    hud_refresh_timer_.invalidate();
  } else if (hud_lines_.isEmpty() || !hud_refresh_timer_.isValid() || hud_refresh_timer_.elapsed() >= kHudRefreshMs) {
    hud_refresh_timer_.start();
    QStringList lines;
    lines << (gpu_average_.empty() ? u"GPU  --"_s : u"GPU  %1 ms"_s.arg(gpu_average_.average(), 0, 'f', 2));
    lines << u"CPU  %1 ms"_s.arg(cpu_average_.average(), 0, 'f', 2);
    lines << u"MSAA %1x"_s.arg(hdr_->state.last_active ? kHdrSampleCount : sampleCount());
    if (lines != hud_lines_) {
      QFont font;
      font.setFamily(u"monospace"_s);
      font.setStyleHint(QFont::Monospace);
      font.setPointSizeF(9.5);
      hud_image_ = toOverlayFormat(
          renderHudPanel(lines, font, dpr, /*padding=*/8, /*panel_alpha=*/150, PJ::theme::onOverlayHud(fw_theme)));
      hud_lines_ = lines;
    }
  }
  if (show_perf_hud_) {
    // Bottom-left, clear of the dock's top-left frame selector.
    images[OverlayRhiPass::kPerfHud] = {
        hud_image_, QPointF(8.0, height() - hud_image_.deviceIndependentSize().height() - 8.0)};
  }

  if (hovered_frame_.has_value() && axes_visible_ && tf_ != nullptr && !fixed_frame.empty()) {
    // Re-project the live origin so the label follows streaming TF and the camera.
    std::optional<glm::vec2> anchor;
    if (const auto transform =
            tf_->tryLookupTransform(fixed_frame, *hovered_frame_, render_time_, pj::tf::Clock::kEntry);
        transform) {
      anchor = projectToWidget(transform->t);
    }
    if (anchor.has_value()) {
      if (hover_label_frame_ != *hovered_frame_) {
        QFont font;
        font.setPointSizeF(9.5);
        hover_label_image_ = toOverlayFormat(renderHudPanel(
            {QString::fromStdString(*hovered_frame_)}, font, dpr, /*padding=*/6, /*panel_alpha=*/170,
            PJ::theme::onOverlayHud(fw_theme)));
        hover_label_frame_ = *hovered_frame_;
      }
      if (hover_ring_image_.isNull()) {
        // A 1.5 px ring of radius 3 on the frame origin ties the label to its gizmo.
        constexpr qreal kRingBox = 10.0;
        hover_ring_image_ =
            QImage(QSize(qCeil(kRingBox * dpr), qCeil(kRingBox * dpr)), QImage::Format_ARGB32_Premultiplied);
        hover_ring_image_.setDevicePixelRatio(dpr);
        hover_ring_image_.fill(Qt::transparent);
        QPainter painter(&hover_ring_image_);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(
            PJ::theme::outline(PJ::theme::OutlineRole::Interactive, PJ::theme::OutlineState::Hovered, fw_theme), 1.5));
        painter.drawEllipse(QPointF(kRingBox / 2.0, kRingBox / 2.0), 3.0, 3.0);
        painter.end();
        hover_ring_image_ = toOverlayFormat(hover_ring_image_);
      }
      // Above-right of the origin, clamped inside the view.
      const QSizeF box = hover_label_image_.deviceIndependentSize();
      const int box_w = static_cast<int>(std::lround(box.width()));
      const int box_h = static_cast<int>(std::lround(box.height()));
      const int box_x = std::clamp(static_cast<int>(std::lround(anchor->x)) + 12, 2, std::max(2, width() - box_w - 2));
      const int box_y =
          std::clamp(static_cast<int>(std::lround(anchor->y)) - box_h - 12, 2, std::max(2, height() - box_h - 2));
      images[OverlayRhiPass::kHoverLabel] = {hover_label_image_, QPointF(box_x, box_y)};
      images[OverlayRhiPass::kHoverRing] = {hover_ring_image_, QPointF(anchor->x - 5.0, anchor->y - 5.0)};
    }
  }
  for (std::size_t slot = 0; slot < images.size(); ++slot) {
    const OverlayRhiPass::Image& image = images[slot];
    last_overlay_rects_[slot] =
        image.image.isNull()
            ? QRect()
            : QRect(QPoint(qRound(image.top_left.x() * dpr), qRound(image.top_left.y() * dpr)), image.image.size());
  }
  overlay_pass_->prepare(updates, view, output_size, dpr, images);
}

void SceneViewWidget::emitPresentationIfChanged(const CameraState& before) {
  if (before != camera_->state()) {
    emit presentationChanged();
  }
}

}  // namespace pj::scene3d
