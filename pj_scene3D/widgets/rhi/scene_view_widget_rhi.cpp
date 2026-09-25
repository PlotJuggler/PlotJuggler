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
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
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
// Cursor pick radius for TF frames, in logical pixels (the GL view's value).
constexpr float kFramePickRadiusPx = 20.0F;

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

// Scene backdrop colour for the current theme, read the same way the desktop GL
// view does (application-palette Window lightness -> framework DataBackdrop
// token) so both backends share one source of truth.
QColor sceneBackdropColor() {
  const QColor window_background = QGuiApplication::palette().color(QPalette::Window);
  const auto theme = PJ::theme::themeFor(window_background.lightness() >= 128);
  return PJ::theme::surface(PJ::theme::Surface::DataBackdrop, theme);
}

// QRhi 6.11 reports no 3D-texture limit, so ask the backend. OpenGL: the widget's
// context is current inside initialize()/render(). Vulkan: the physical device.
// D3D/Metal: 2048, the D3D11 feature-level-11 and Metal minimum.
int max3dTextureSize(QRhi& rhi) {
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
    default:
      return 2048;
  }
}

bool cameraStatesEqual(const CameraState& left, const CameraState& right) {
  return left.focal == right.focal && left.radius == right.radius && left.azimuth == right.azimuth &&
         left.elevation == right.elevation && left.fov_y == right.fov_y && left.ortho_scale == right.ortho_scale &&
         left.perspective == right.perspective;
}

// One FNV-1a step over the bits of `value`.
uint64_t fnvMix(uint64_t hash, double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return (hash ^ bits) * 0x100000001b3ULL;
}

uint64_t hashTransform(const Transform& transform, uint64_t seed) {
  const glm::dmat4 matrix = transform.matrix();
  uint64_t hash = seed;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      hash = fnvMix(hash, matrix[column][row]);
    }
  }
  return hash;
}

template <class... Visitors>
struct Overloaded : Visitors... {
  using Visitors::operator()...;
};

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
  setSampleCount(4);
  setObjectName(QStringLiteral("scene3dRhiCanvas"));
  setMinimumSize(320, 240);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  connect(this, &QRhiWidget::renderFailed, this, []() { qWarning("SceneViewWidget QRhi: render failed"); });
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
  update();
}

void SceneViewWidget::setLayers(const std::vector<Scene3DLayer*>& ordered_layers) {
  std::vector<PointRenderable*> points;
  std::vector<PosesInFrameLayer*> poses;
  std::vector<OccupancyGridLayer*> occupancy;
  std::vector<VoxelGridLayer*> voxels;
  std::vector<SceneEntitiesLayer*> markers;
  std::vector<ModelRenderable*> models;
  std::vector<GridMapLayer*> grid_maps;
  std::vector<Scene3DLayer*> trail_owners;
  std::vector<CameraFitPolicy::LayerKey> fit_keys;
  ordered_layers_.clear();
  for (Scene3DLayer* layer : ordered_layers) {
    fit_keys.push_back(dynamic_cast<const void*>(layer));
    const std::uint32_t topic_id = layer->info().topic_id.id;
    if (auto* point = dynamic_cast<PointRenderable*>(layer); point != nullptr) {
      ordered_layers_.push_back({point, topic_id});
      points.push_back(point);
    } else if (auto* pose = dynamic_cast<PosesInFrameLayer*>(layer); pose != nullptr) {
      ordered_layers_.push_back({pose, topic_id});
      poses.push_back(pose);
      trail_owners.push_back(pose);
    } else if (auto* grid = dynamic_cast<OccupancyGridLayer*>(layer); grid != nullptr) {
      ordered_layers_.push_back({grid, topic_id});
      occupancy.push_back(grid);
    } else if (auto* voxel = dynamic_cast<VoxelGridLayer*>(layer); voxel != nullptr) {
      ordered_layers_.push_back({voxel, topic_id});
      voxels.push_back(voxel);
    } else if (auto* marker = dynamic_cast<SceneEntitiesLayer*>(layer); marker != nullptr) {
      ordered_layers_.push_back({marker, topic_id});
      markers.push_back(marker);
      models.push_back(marker);
    } else if (auto* model = dynamic_cast<ModelRenderable*>(layer); model != nullptr) {
      ordered_layers_.push_back({model, topic_id});
      models.push_back(model);
    } else if (auto* grid_map = dynamic_cast<GridMapLayer*>(layer); grid_map != nullptr) {
      ordered_layers_.push_back({grid_map, topic_id});
      grid_maps.push_back(grid_map);
    } else if (auto* trail = dynamic_cast<TrailLayer*>(layer); trail != nullptr) {
      ordered_layers_.push_back({trail, topic_id});
      trail_owners.push_back(trail);
    }
  }
  camera_fit_.setLayers(fit_keys);
  point_pass_->setLayers(points);
  pose_pass_->setLayers(poses);
  occupancy_pass_->setLayers(occupancy);
  voxel_pass_->setLayers(voxels);
  marker_pass_->setLayers(markers);
  model_pass_->setLayers(models);
  gridmap_pass_->setLayers(grid_maps);
  trail_pass_->setLayers(trail_owners);
  update();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint time) {
  if (render_time_ == time) {
    return;
  }
  render_time_ = time;
  refreshAvailableFrames();
  applyFollow();
  update();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  follow_seeded_ = false;
  update();  // the dock owns and persists the fixed frame, and reports its changes
}

void SceneViewWidget::setFollowFrame(const std::string& frame) {
  if (follow_frame_ == frame) {
    return;
  }
  follow_frame_ = frame;
  follow_seeded_ = false;
  applyFollow();
  update();
}

std::optional<Transform> SceneViewWidget::followTransform(PJ::Timepoint time) const {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return std::nullopt;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, time);
  return transform ? std::optional<Transform>(*transform) : std::nullopt;
}

void SceneViewWidget::applyFollow() {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, render_time_);
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
  update();
  emitPresentationIfChanged(before);
}

uint64_t SceneViewWidget::followRenderKey(PJ::Timepoint time) const {
  const auto transform = followTransform(time);
  if (!transform) {
    return 0;
  }
  uint64_t hash = 1469598103934665603ULL;
  for (const double component : {transform->t.x, transform->t.y, transform->t.z}) {
    hash = fnvMix(hash, component);
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
  update();
  emit presentationChanged();
}

void SceneViewWidget::resetCamera() {
  const CameraState before = camera_->state();
  camera_->reset();
  update();
  emitPresentationIfChanged(before);
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::noteCameraRestored() {
  camera_fit_.noteCameraRestored();
}

int SceneViewWidget::lastPointVertexCountForTest() const {
  return point_pass_->lastPointVertexCount();
}

int SceneViewWidget::lastPointLayerCountForTest() const {
  return point_pass_->lastPointLayerCount();
}

int SceneViewWidget::lastCubeLayerCountForTest() const {
  return point_pass_->lastCubeLayerCount();
}

int SceneViewWidget::lastCubeInstanceCountForTest() const {
  return point_pass_->lastCubeInstanceCount();
}

int SceneViewWidget::lastCubeSpriteLayerCountForTest() const {
  return point_pass_->lastCubeSpriteLayerCount();
}

int SceneViewWidget::lastPointListLayerCountForTest() const {
  return point_pass_->lastPointListLayerCount();
}

bool SceneViewWidget::pipelinesBuiltForTargetForTest() const {
  const QRhiRenderTarget* target = renderTarget();
  if (target == nullptr) {
    return false;
  }
  const QRhiRenderPassDescriptor* render_pass = target->renderPassDescriptor();
  return pipelines_->directsBuiltFor(render_pass, sampleCount()) &&
         overlay_pass_->builtFor(render_pass, sampleCount()) &&
         pipelineBuiltFor(present_pipeline_, render_pass, sampleCount());
}

AABB SceneViewWidget::lastPointBoundsForTest() const {
  return point_pass_->lastPointBounds();
}

int SceneViewWidget::lastDepthLayerCountForTest() const {
  return point_pass_->lastDepthLayerCount();
}

int SceneViewWidget::lastDepthVertexCountForTest() const {
  return point_pass_->lastDepthVertexCount();
}

AABB SceneViewWidget::lastDepthBoundsForTest() const {
  return point_pass_->lastDepthBounds();
}

int SceneViewWidget::lastOccupancyLayerCountForTest() const {
  return occupancy_pass_->lastLayerCount();
}

int SceneViewWidget::lastOccupancyCellCountForTest() const {
  return occupancy_pass_->lastCellCount();
}

int SceneViewWidget::occupancyFullUploadCountForTest() const {
  return occupancy_pass_->fullUploadCount();
}

int SceneViewWidget::occupancyPartialUploadCountForTest() const {
  return occupancy_pass_->partialUploadCount();
}

AABB SceneViewWidget::lastOccupancyBoundsForTest() const {
  return occupancy_pass_->lastBounds();
}

int SceneViewWidget::lastGridMapLayerCountForTest() const {
  return gridmap_pass_->lastLayerCount();
}

int SceneViewWidget::lastGridMapVertexCountForTest() const {
  return gridmap_pass_->lastVertexCount();
}

int SceneViewWidget::gridMapUploadCountForTest() const {
  return gridmap_pass_->uploadCount();
}

int SceneViewWidget::lastTrailCountForTest() const {
  return trail_pass_->lastTrailCount();
}

int SceneViewWidget::lastTrailVertexCountForTest() const {
  return trail_pass_->lastVertexCount();
}

int SceneViewWidget::lastPoseLayerCountForTest() const {
  return pose_pass_->lastLayerCount();
}

int SceneViewWidget::lastPoseArmCountForTest() const {
  return pose_pass_->lastArmCount();
}

AABB SceneViewWidget::lastPoseBoundsForTest() const {
  return pose_pass_->lastBounds();
}

int SceneViewWidget::lastVoxelLayerCountForTest() const {
  return voxel_pass_->lastLayerCount();
}

int SceneViewWidget::lastVoxelCountForTest() const {
  return voxel_pass_->lastVoxelCount();
}

int SceneViewWidget::voxelFullUploadCountForTest() const {
  return voxel_pass_->fullUploadCount();
}

AABB SceneViewWidget::lastVoxelBoundsForTest() const {
  return voxel_pass_->lastBounds();
}

int SceneViewWidget::lastMarkerLayerCountForTest() const {
  return marker_pass_->lastLayerCount();
}

int SceneViewWidget::lastMarkerInstanceCountForTest() const {
  return marker_pass_->lastInstanceCount();
}

int SceneViewWidget::lastMarkerStreamVertexCountForTest() const {
  return marker_pass_->lastStreamVertexCount();
}

AABB SceneViewWidget::lastMarkerBoundsForTest() const {
  return marker_pass_->lastBounds();
}

int SceneViewWidget::lastModelLayerCountForTest() const {
  return model_pass_->lastLayerCount();
}

int SceneViewWidget::lastModelDrawCountForTest() const {
  return model_pass_->lastDrawCount();
}

int SceneViewWidget::lastModelTriangleCountForTest() const {
  return model_pass_->lastTriangleCount();
}

std::array<int, 5> SceneViewWidget::lastModelTextureSlotCountsForTest() const {
  return model_pass_->lastTextureSlotCounts();
}

AABB SceneViewWidget::lastModelBoundsForTest() const {
  return model_pass_->lastBounds();
}

int SceneViewWidget::lastShadowDrawCountForTest() const {
  return model_pass_->lastShadowDrawCount() + marker_pass_->lastShadowDrawCount();
}

int SceneViewWidget::lastShadowTriangleCountForTest() const {
  return model_pass_->lastShadowTriangleCount() + marker_pass_->lastShadowTriangleCount();
}

void SceneViewWidget::setGridStyle(GridStyle style) {
  if (grid_style_ == style) {
    return;
  }
  grid_style_ = style;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridDivisions(int divisions) {
  divisions = std::clamp(divisions, 1, 200);
  if (grid_divisions_ == divisions) {
    return;
  }
  grid_divisions_ = divisions;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridExtentMetres(float extent_m) {
  extent_m = std::clamp(extent_m, kGridExtentMinM, kGridExtentMaxM);
  if (grid_extent_m_ == extent_m) {
    return;
  }
  grid_extent_m_ = extent_m;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridVisible(bool visible) {
  if (grid_visible_ == visible) {
    return;
  }
  grid_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoSize(float length_m) {
  length_m = std::clamp(length_m, kGizmoSizeMinM, kGizmoSizeMaxM);
  if (gizmo_size_m_ == length_m) {
    return;
  }
  gizmo_size_m_ = length_m;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (gizmo_opacity_ == opacity) {
    return;
  }
  gizmo_opacity_ = opacity;
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
                       shading_params_.collision_opacity != params.collision_opacity ||
                       shading_params_.shadows_enabled != params.shadows_enabled;
  shading_params_ = params;
  if (changed) {
    update();
    emit presentationChanged();
  }
}

void SceneViewWidget::grabPresentedFrame(std::function<void(const QImage&)> done) {
  if (!isVisible()) {
    done(grabFramebuffer());
    return;
  }
  // The presented frame and the timeout race; whichever comes first delivers.
  auto pending = std::make_shared<std::function<void(const QImage&)>>(std::move(done));
  auto presented = std::make_shared<QMetaObject::Connection>();
  const auto deliver = [pending, presented](const QImage& image) {
    QObject::disconnect(*presented);
    // A second frameSubmitted may already be queued when the first one delivers.
    if (*pending) {
      std::exchange(*pending, nullptr)(image);
    }
  };
  // Queued: grab after frameSubmitted returns, never from inside the frame.
  *presented = connect(
      this, &QRhiWidget::frameSubmitted, this, [this, deliver]() { deliver(grabFramebuffer()); }, Qt::QueuedConnection);
  QTimer::singleShot(2000, this, [pending, deliver]() {
    if (*pending) {
      qWarning("Scene3D: the shown view presented no frame to grab (occluded window?)");
      deliver(QImage());
    }
  });
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

// Places a layer's source frame in the fixed frame at render_time_: identity
// when the frames match, nullopt when either frame is empty or TF cannot
// resolve the pair this frame — the caller skips the layer without consuming
// its per-view budget.
std::optional<Transform> SceneViewWidget::resolveFixedFromSource(
    const std::string& source_frame, const std::string& fixed_frame) const {
  if (source_frame.empty() || fixed_frame.empty()) {
    return std::nullopt;
  }
  if (source_frame == fixed_frame) {
    return Transform{};
  }
  if (tf_ == nullptr) {
    return std::nullopt;
  }
  const auto transform = tf_->tryLookupTransform(fixed_frame, source_frame, render_time_);
  if (!transform.has_value()) {
    return std::nullopt;
  }
  return *transform;
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
    if (const auto transform = tf_->tryLookupTransform(fixed, frame, time); transform) {
      hash = hashTransform(*transform, hash);
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
  // frame and draw with the identity transform, matching native's fallback-frame
  // behavior instead of silently producing an empty view.
  for (const OrderedLayerEntry& entry : ordered_layers_) {
    const std::string source = std::visit(
        [](const auto* layer) -> std::string {
          if constexpr (std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(layer)>>, TrailLayer>) {
            return {};  // points are already in the fixed frame
          } else {
            return layer != nullptr ? layer->renderFrameId() : std::string{};
          }
        },
        entry.layer);
    if (!source.empty()) {
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
  // Solid arrow triads, matching the desktop AxisRenderPass. The frame's
  // fixed-from-frame transform (camera-relative) is baked into each arm's model,
  // so the draw uses an identity fixed_from_source uniform.
  const PoseTriadStyle triad_style{
      .axis_length = gizmo_size_m_, .opacity = gizmo_opacity_, .x_arrow_only = false, .override_color = false};
  std::pair<std::size_t, std::size_t> hovered_arms{0, 0};
  tf_->getAllFrames(tf_triad_frames_);
  for (const std::string& frame : tf_triad_frames_) {
    const auto transform = tf_->tryLookupTransform(fixed_frame, frame, render_time_);
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
// trail under its triads (like GL), a SceneEntities layer's markers before its
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

void SceneViewWidget::initialize(QRhiCommandBuffer* /*command_buffer*/) {
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr || renderTarget() == nullptr) {
    return;
  }
  const bool new_rhi = resource_rhi_ != current_rhi;
  // The descriptor's address catches a new target whose format serializes the same
  // (OpenGL); the serialized format catches a new one allocated at the old address.
  const QRhiRenderPassDescriptor* target_render_pass = renderTarget()->renderPassDescriptor();
  const QVector<quint32> target_format = target_render_pass->serializedFormat();
  if (new_rhi || target_render_pass != built_render_pass_ || target_format != built_target_format_ ||
      sampleCount() != built_sample_count_) {
    releaseResources();
    resource_rhi_ = current_rhi;
    rhi_backend_name_ = QString::fromLatin1(current_rhi->backendName());
    max_3d_texture_size_ = max3dTextureSize(*current_rhi);
    if (new_rhi) {
      PJ::logGraphicsBackend(*this, current_rhi);
    }
  }
  const bool three_d_textures = current_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures);
  if (render_mode_uniform_buffer_ != nullptr && annotation_render_mode_uniform_buffer_ != nullptr &&
      model_uniform_buffer_ != nullptr && point_pass_->ready() && marker_pass_->ready() && model_pass_->ready() &&
      pose_pass_->ready() && occupancy_pass_->ready() && grid_pass_->ready() && overlay_pass_->ready() &&
      gridmap_pass_->ready() && trail_pass_->ready() && (!three_d_textures || voxel_pass_->ready())) {
    return;
  }

  if (!ensureUniformBuffer(current_rhi, uniform_buffer_, 64U) ||
      !ensureUniformBuffer(current_rhi, render_mode_uniform_buffer_, kRenderModeUniformBytes) ||
      !ensureUniformBuffer(current_rhi, annotation_render_mode_uniform_buffer_, kRenderModeUniformBytes) ||
      !ensureUniformBuffer(current_rhi, model_uniform_buffer_, kModelUniformBytes) ||
      !shared_geometry_->create(current_rhi)) {
    releaseResources();
    return;
  }
  render_mode_hdr_written_ = -1;

  const RhiPassSetup pass_setup{
      current_rhi,
      renderTarget()->renderPassDescriptor(),
      sampleCount(),
      render_mode_uniform_buffer_,
      annotation_render_mode_uniform_buffer_,
      uniform_buffer_,
      shared_geometry_.get()};
  if (!point_pass_->initialize(pass_setup) || !trail_pass_->initialize(pass_setup) ||
      !pose_pass_->initialize(pass_setup) || !occupancy_pass_->initialize(pass_setup) ||
      (three_d_textures && !voxel_pass_->initialize(pass_setup, max_3d_texture_size_)) ||
      !marker_pass_->initialize(pass_setup) || !model_pass_->initialize(pass_setup, model_uniform_buffer_) ||
      !gridmap_pass_->initialize(pass_setup) || !grid_pass_->initialize(pass_setup) ||
      !overlay_pass_->initialize(pass_setup)) {
    releaseResources();
    return;
  }
  // Every direct pipeline the scene passes bind (the grid's shadow receiver
  // registers itself in ensureShadowResources).
  grid_pass_->registerPipelines(*pipelines_);
  forEachFamily([this](const auto& family) { family.registerPipelines(*pipelines_); });
  built_render_pass_ = target_render_pass;
  built_target_format_ = target_format;
  built_sample_count_ = sampleCount();
}

// What admission decided for one frame: the effective fixed frame (resolved
// once per frame) and the scene bounds of the placed layers. Filled once by
// admitFrame(); prepareFrame() reads it and no later step re-admits a layer.
struct SceneViewWidget::FrameAdmission {
  std::string fixed_frame;
  SceneBoundsCollector bounds;
};

// The frame's camera, after the fit: every matrix is relative to render_origin.
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
  using DrawRef = std::variant<
      DrawOf<PointRhiPass>, DrawOf<TrailRhiPass>, DrawOf<PoseRhiPass>, DrawOf<OccupancyRhiPass>, DrawOf<VoxelRhiPass>,
      DrawOf<MarkerRhiPass>, DrawOf<ModelRhiPass>, DrawOf<GridMapRhiPass>>;
  // One prepared draw in submission terms: its logical slot (index into
  // ordered_layers_), blending class and family record.
  struct SceneDraw {
    std::size_t slot = 0;
    DrawClass draw_class = DrawClass::kOpaque;
    DrawRef draw;
  };

  bool hdr_active = false;
  bool ssao_active = false;
  bool edl_active = false;
  bool shadow_active = false;
  QRhiViewport viewport;
  std::vector<SceneDraw> scene_draws;  // logical (dock) order
};

void SceneViewWidget::render(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  QRhiRenderTarget* target = renderTarget();
  if (current_rhi == nullptr || target == nullptr) {
    return;
  }

  if (show_perf_hud_) {
    cpu_timer_.start();
    if (const double gpu_seconds = command_buffer->lastCompletedGpuTime(); gpu_seconds > 0.0) {
      gpu_average_.add(gpu_seconds * 1000.0);
    }
  }
  // The HDR chain renders at render_scale x the output and the present pass
  // downsamples it (the GL view's SSAA); the direct fallback stays at output size.
  const QSize output_size = target->pixelSize();
  const QSize scaled_size(
      std::max(1, static_cast<int>(std::lround(static_cast<float>(output_size.width()) * render_scale_))),
      std::max(1, static_cast<int>(std::lround(static_cast<float>(output_size.height()) * render_scale_))));
  // The one owner of the frame's stage flags: HDR, SSAO and EDL here, shadows in
  // prepareFrame() once the casters are known.
  PreparedFrame& frame = *frame_;
  frame.hdr_active = ensureHdrResources(current_rhi, scaled_size);
  hdr_stage_.last_active = frame.hdr_active;
  const QSize render_size = frame.hdr_active ? scaled_size : output_size;

  // The frame contract (rhi_scene_pass.h): admit once, fit once, prepare once;
  // every pass below replays `frame`.
  FrameAdmission admission;
  admitFrame(current_rhi, admission);
  applySceneBounds(admission.bounds);
  const FrameView view = frameView(current_rhi, render_size);
  frame.ssao_active = frame.hdr_active && composite_params_.ssao_enabled &&
                      ensureSsaoResources(current_rhi, scaled_size) && view.inverse_projection_valid;
  frame.edl_active = frame.hdr_active && composite_params_.edl_enabled &&
                     ensureEdlResources(current_rhi, scaled_size) && view.inverse_projection_valid;
  ssao_stage_.last_active = frame.ssao_active;
  edl_stage_.last_active = frame.edl_active;
  QRhiResourceUpdateBatch* updates = prepareFrame(current_rhi, render_size, view, admission, frame);
  if (show_perf_hud_) {
    cpu_average_.add(static_cast<double>(cpu_timer_.nsecsElapsed()) / 1.0e6);
  }
  prepareOverlays(updates, output_size, admission.fixed_frame);
  // Compute runs outside render passes, so it goes first and takes the uploads.
  if (point_pass_->recordCompute(command_buffer, updates)) {
    updates = nullptr;
  }
  if (point_pass_->gpuBoundsPending()) {
    update();  // the bounds readback lands in a later frame; render until it does
  }

  if (frame.shadow_active) {
    submitShadowPass(command_buffer, updates);
    updates = nullptr;
  }

  // Scene backdrop tracks the active theme (matching the desktop GL view, which
  // reads the same token per frame). PaletteChange calls update(), so a theme
  // switch re-applies here. The HDR target is linear-light — the present pass
  // re-encodes to sRGB — so linearize the display-referred token on write.
  const QColor scene_bg = sceneBackdropColor();
  const auto linear_channel = [](float channel) { return std::pow(channel, 2.2F); };
  const QColor hdr_scene_clear = QColor::fromRgbF(
      linear_channel(static_cast<float>(scene_bg.redF())), linear_channel(static_cast<float>(scene_bg.greenF())),
      linear_channel(static_cast<float>(scene_bg.blueF())), 1.0F);
  if (frame.hdr_active) {
    submitScenePass(command_buffer, hdr_render_target_, ScenePass::kHdrColor, hdr_scene_clear, updates, frame);
    updates = nullptr;
    // Reusing the same immutable HDR mode keeps both passes independent of
    // dynamic-buffer update ordering. The replay's RGBA8 alpha records the
    // shaders' post-discard opacity while its D32F attachment records depth.
    submitScenePass(command_buffer, hdr_depth_render_target_, ScenePass::kDepthReplay, Qt::transparent, nullptr, frame);
    if (frame.ssao_active && ssao_raw_render_target_ != nullptr) {
      // Preserve replay alpha/red and write native's raw scene-wide AO into the
      // otherwise-unused green channel. The sampled D32F texture is not attached
      // to this color-only target, avoiding a read/write feedback loop.
      command_buffer->beginPass(ssao_raw_render_target_, Qt::transparent, {1.0F, 0});
      command_buffer->setGraphicsPipeline(ssao_pipeline_);
      command_buffer->setViewport(frame.viewport);
      command_buffer->setShaderResources(ssao_shader_resources_);
      command_buffer->draw(3U);
      command_buffer->endPass();
    }
    if (frame.edl_active && edl_mesh_mask_render_target_ != nullptr) {
      submitEdlMeshMask(command_buffer, frame);
    }
    command_buffer->beginPass(target, Qt::transparent, {1.0F, 0});
    command_buffer->setGraphicsPipeline(present_pipeline_);
    command_buffer->setViewport(
        QRhiViewport(0.0F, 0.0F, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    command_buffer->setShaderResources(present_shader_resources_);
    command_buffer->draw(3U);
    overlay_pass_->submit(command_buffer);
    command_buffer->endPass();
  } else {
    // Direct-to-backing fallback: no HDR encode, so use the theme token as-is.
    submitScenePass(command_buffer, target, ScenePass::kDirect, scene_bg, updates, frame);
  }
}

// Frame contract step 1: runs every visible layer's prepareForRender() against
// the per-view budgets, places it in the fixed frame and collects the scene
// bounds. The only place a budget is consumed.
void SceneViewWidget::admitFrame(QRhi* rhi, FrameAdmission& admission) {
  admission.fixed_frame = effectiveFixedFrame();
  const std::string& fixed_frame = admission.fixed_frame;
  const PlaceFn place = [this, &fixed_frame](const std::string& source_frame) {
    return resolveFixedFromSource(source_frame, fixed_frame);
  };
  point_pass_->admit(place, admission.bounds);
  pose_pass_->admit(place, admission.bounds);
  occupancy_pass_->admit(place, admission.bounds);
  gridmap_pass_->admit(place, admission.bounds);
  voxel_pass_->admit(rhi, place, admission.bounds);
  marker_pass_->admit(place, admission.bounds);
  model_pass_->admit(place, shading_params_, shading_params_.shadows_enabled && tf_ != nullptr, admission.bounds);
}

// Frame contract step 2, the one scene-bounds + camera-fit policy of this view.
// Scene bounds = the union of every ADMITTED and PLACED draw this frame: points
// and depth clouds, poses, occupancy grids, voxels, SceneEntities markers, and
// model layers whose contributesToSceneBounds() is true (SceneEntities models;
// robots never, so a moving robot cannot yank the camera). Hidden, unplaceable
// (orphaned) and over-budget layers contribute nothing; grid maps contribute
// their placed box, trails nothing (the GL view's rule). The
// camera then fits per CameraFitPolicy: once per new layer, never over a
// restored camera.
void SceneViewWidget::applySceneBounds(const SceneBoundsCollector& bounds) {
  setSceneBounds(bounds.bounds);
  if (camera_fit_.takeFit(bounds.contributors, bounds.bounds)) {
    camera_->fitToBoundingBox(bounds.bounds);
    ++camera_fit_count_;
  }
}

SceneViewWidget::FrameView SceneViewWidget::frameView(QRhi* rhi, const QSize& render_size) const {
  FrameView view;
  view.render_origin = glm::dvec3(camera_->state().focal);
  const float aspect = render_size.height() > 0
                           ? static_cast<float>(render_size.width()) / static_cast<float>(render_size.height())
                           : 1.0F;
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
  shared_geometry_->upload(updates);

  buildTfTriads(render_origin, admission.fixed_frame);
  grid_pass_->build(
      GridRhiPass::Settings{grid_visible_, grid_style_, grid_extent_m_, grid_divisions_, tf_connections_visible_},
      tf_.get(), admission.fixed_frame, render_time_, render_origin);
  last_line_vertex_count_ = grid_pass_->lastLineVertexCount();
  last_triangle_vertex_count_ = grid_pass_->lastTriangleVertexCount();
  const bool shadows_requested = shading_params_.shadows_enabled && tf_ != nullptr;
  marker_pass_->prepare(context, shadows_requested);
  model_pass_->prepare(context, shading_params_);

  if (!shading_params_.shadows_enabled && shadow_texture_ != nullptr) {
    releaseShadowResources(true);
  }
  AABB render_shadow_bounds = unionAABB(model_pass_->shadowCasterBounds(), marker_pass_->shadowCasterBounds());
  if (render_shadow_bounds.valid) {
    render_shadow_bounds = extendAabbToGroundShadow(
        render_shadow_bounds, shading_params_.key_light_dir, static_cast<float>(-render_origin.z));
  }
  last_shadow_bounds_ = render_shadow_bounds;
  const ShadowCameraFit shadow_fit =
      fitDirectionalShadowCamera(render_shadow_bounds, shading_params_.key_light_dir, kShadowMapSize);
  last_shadow_fit_valid_ = shadow_fit.valid;
  const bool shadow_requested =
      shadows_requested && (model_pass_->hasShadowCasters() || marker_pass_->hasShadowCasters()) && shadow_fit.valid;
  bool shadow_active = shadow_requested && ensureShadowResources(rhi);
  if (shadow_active && !model_pass_->uploadShadowCasters(context)) {
    shadow_stage_.capability_error = tr("Could not allocate the shadow instance buffer");
    qWarning("SceneViewWidget QRhi shadows unavailable: %s", qPrintable(shadow_stage_.capability_error));
    releaseShadowResources(true);
    shadow_active = false;
  }
  frame.shadow_active = shadow_active;
  shadow_stage_.last_active = shadow_active;
  if (shadow_requested && !shadow_active && !shadow_stage_.capability_error.isEmpty()) {
    if (noted_shadow_failure_ != shadow_stage_.capability_error) {
      noted_shadow_failure_ = shadow_stage_.capability_error;
      model_pass_->noteShadowFailure(shadow_stage_.capability_error);
      marker_pass_->noteShadowFailure(shadow_stage_.capability_error);
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
        0.0F,
        0.0F,
    };
    composite.grade_params = {static_cast<float>(std::clamp(composite_params_.tonemap_mode, 0, 3)), 0.0F, 0.0F, 0.0F};
    updates->updateDynamicBuffer(composite_uniform_buffer_, 0, kCompositeUniformBytes, &composite);
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
      if (ssao_kernel_pending_) {
        ssao_uniforms.kernel = ssaoKernel();
        updates->updateDynamicBuffer(ssao_uniform_buffer_, 0, kSsaoUniformBytes, &ssao_uniforms);
        ssao_kernel_pending_ = false;
      } else {
        updates->updateDynamicBuffer(
            ssao_uniform_buffer_, 0, static_cast<quint32>(offsetof(SsaoUniforms, kernel)), &ssao_uniforms);
      }
    }
  }
  if (uniform_buffer_ != nullptr) {
    const QMatrix4x4 view_uniform = view.corrected_projection * toQMatrix(view.view);
    updates->updateDynamicBuffer(uniform_buffer_, 0, 64, view_uniform.constData());
  }
  if (shadow_active && shadow_uniform_buffer_ != nullptr) {
    updates->updateDynamicBuffer(shadow_uniform_buffer_, 0, kShadowUniformBytes, shadow_matrix.constData());
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
        look::kShadowSoftnessTexels, shadow_active ? 1.0F : 0.0F, static_cast<float>(shadow_map_size_)};
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
  for (std::size_t slot = 0; slot < ordered_layers_.size(); ++slot) {
    std::visit(
        [&](const auto* layer) {
          forEachFamily([&](const auto& family) {
            if constexpr (requires { family.drawsFor(layer); }) {
              using Family = std::remove_cvref_t<decltype(family)>;
              for (const auto& draw : family.drawsFor(layer)) {
                frame.scene_draws.push_back({slot, draw.draw_class, PreparedFrame::DrawOf<Family>{&family, &draw}});
              }
            }
          });
        },
        ordered_layers_[slot].layer);
  }
  return updates;
}

void SceneViewWidget::submitShadowPass(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates) {
  command_buffer->beginPass(shadow_render_target_, Qt::transparent, {1.0F, 0}, updates);
  const QRhiViewport viewport(0.0F, 0.0F, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize));
  model_pass_->submitShadowCasters(command_buffer, viewport);
  marker_pass_->submitShadowCasters(command_buffer, viewport);
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
  submitted_slots_.assign(ordered_layers_.size(), 0);
  for (const DrawClass phase : {DrawClass::kOpaque, DrawClass::kTranslucent}) {
    for (const PreparedFrame::SceneDraw& scene_draw : frame.scene_draws) {
      if (scene_draw.draw_class == phase && std::visit(
                                                [&](const auto& ref) {
                                                  return ref.family->submit(
                                                      command_buffer, *ref.draw, scene_pass, *pipelines_, viewport);
                                                },
                                                scene_draw.draw)) {
        submitted_slots_[scene_draw.slot] = 1;
      }
    }
  }
  last_submitted_layer_ids_.clear();
  for (std::size_t slot = 0; slot < submitted_slots_.size(); ++slot) {
    if (submitted_slots_[slot] != 0) {
      last_submitted_layer_ids_.push_back(ordered_layers_[slot].topic_id);
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
  // coverage.r for native's mesh-only EDL mask.
  command_buffer->beginPass(edl_mesh_mask_render_target_, Qt::transparent, {1.0F, 0});
  model_pass_->submitMeshMask(
      command_buffer, edl_mesh_mask_triangle_pipeline_, edl_mesh_mask_line_pipeline_, frame.viewport);
  command_buffer->endPass();
}

void SceneViewWidget::releaseResources() {
  releaseShadowResources(false);
  releaseHdrResources();
  forEachFamily([](auto& family) { family.release(); });
  grid_pass_->release();
  overlay_pass_->release();
  frame_->scene_draws.clear();
  // After every pipeline that draws the shared cube/arrow meshes.
  shared_geometry_->release();
  delete model_uniform_buffer_;
  delete annotation_render_mode_uniform_buffer_;
  delete render_mode_uniform_buffer_;
  delete uniform_buffer_;
  model_uniform_buffer_ = nullptr;
  annotation_render_mode_uniform_buffer_ = nullptr;
  render_mode_uniform_buffer_ = nullptr;
  uniform_buffer_ = nullptr;
  max_3d_texture_size_ = 0;
  shadow_stage_.capability_error.clear();
  shadow_stage_.rejected_size = {};
  noted_shadow_failure_.clear();
  // Preserve an active warning across RHI teardown. Clearing the rejected
  // size below permits the new RHI to probe again; a successful allocation
  // then clears the warning through renderingWarningChanged().
  hdr_stage_.rejected_size = {};
  shadow_stage_.last_active = false;
  hdr_stage_.last_active = false;
  last_shadow_fit_valid_ = false;
  last_shadow_bounds_ = {};
  pipelines_->clear();
  resource_rhi_ = nullptr;
  built_render_pass_ = nullptr;
  built_target_format_.clear();
  built_sample_count_ = 0;
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_position_ = event->position().toPoint();
  active_button_ = event->button();
  camera_gesture_changed_ = false;
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
    // grabs the mouse on the press): drop the stale gesture latch.
    active_button_ = Qt::NoButton;
  }
  if (active_button_ == Qt::NoButton) {
    setHoveredFrame(frameAt(event->position()));
  }
  const QPoint position = event->position().toPoint();
  const float delta_x = static_cast<float>(position.x() - last_mouse_position_.x());
  const float delta_y = static_cast<float>(position.y() - last_mouse_position_.y());
  last_mouse_position_ = position;
  if (active_button_ == Qt::LeftButton) {
    camera_->rotate(delta_x, delta_y);
  } else if (active_button_ == Qt::RightButton || active_button_ == Qt::MiddleButton) {
    camera_->pan(delta_x, delta_y);
  } else {
    return;
  }
  camera_gesture_changed_ = true;
  update();
  event->accept();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0F;
  if (ticks == 0.0F) {
    return;
  }
  const CameraState before = camera_->state();
  camera_->zoomToCursor(ticks, glm::vec2(event->position().x(), event->position().y()), width(), height());
  update();
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
    update();
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
  update();
}

void SceneViewWidget::setRenderScale(float scale) {
  const float clamped = std::clamp(scale, 1.0F, 4.0F);
  if (render_scale_ == clamped) {
    return;
  }
  render_scale_ = clamped;
  update();
}

std::optional<glm::vec2> SceneViewWidget::projectToWidget(const glm::dvec3& point) const {
  const glm::dvec3 origin(camera_->state().focal);
  const float aspect = static_cast<float>(width()) / static_cast<float>(std::max(height(), 1));
  return projectFrameOrigin(
      camera_->projMatrix(aspect) * camera_->viewMatrixRelativeTo(origin),
      glm::vec2(static_cast<float>(width()), static_cast<float>(height())), glm::vec3(point - origin));
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
    if (const auto transform = tf_->tryLookupTransform(fixed, frame, render_time_); transform) {
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
  update();
  emit hoveredFrameChanged(hovered_frame_ ? QString::fromStdString(*hovered_frame_) : QString{});
}

void SceneViewWidget::prepareOverlays(
    QRhiResourceUpdateBatch* updates, const QSize& output_size, const std::string& fixed_frame) {
  const qreal dpr = height() > 0 ? static_cast<qreal>(output_size.height()) / height() : 1.0;
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
    lines << u"MSAA %1x"_s.arg(hdr_stage_.last_active ? 4 : sampleCount());
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
    if (const auto transform = tf_->tryLookupTransform(fixed_frame, *hovered_frame_, render_time_); transform) {
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
  overlay_pass_->prepare(updates, camera_->viewMatrix(), output_size, dpr, images);
}

void SceneViewWidget::emitPresentationIfChanged(const CameraState& before) {
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

}  // namespace pj::scene3d
