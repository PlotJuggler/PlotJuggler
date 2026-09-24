// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>

#include <QBuffer>
#include <QByteArray>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPalette>
#include <QVarLengthArray>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include "pj_scene3d_core/shadow_camera.h"
#include "pj_scene3d_core/tf/tf_connections.h"
#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"
#include "pj_scene3d_widgets/rhi_conventions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_state_xml.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/wasm/model_renderable_wasm.h"
#include "pj_scene3d_widgets/wasm/occupancy_grid_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/point_renderable_wasm.h"
#include "pj_scene3d_widgets/wasm/poses_in_frame_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/scene_entities_layer_wasm.h"
#include "pj_scene3d_widgets/wasm/voxel_grid_layer_wasm.h"
#include "pj_widgets/Colormap.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/GraphicsApi.h"
#include "scene_view_widget_rhi_quality_p.h"

// The resource object lives in a static archive.  Explicit initialization both
// retains it at link time and keeps Q_INIT_RESOURCE's generated symbol lookup in
// the global namespace.
static void initializeScene3dWasmResources() {
  Q_INIT_RESOURCE(scene3d_wasm_shaders);
}

namespace pj::scene3d {
namespace {

// Shared body of the four setXxxLayers entry points: prune GPU state and
// pending-fit marks for layers that left the list (or went invisible), queue a
// camera-fit for layers that just arrived, then adopt the new list.
template <typename Layer, typename Gpu, typename Release>
void adoptLayerList(
    const std::vector<Layer*>& layers, std::vector<Layer*>& current, std::unordered_map<Layer*, Gpu>& gpu_cache,
    std::unordered_set<Layer*>& pending_fit, Release&& release_gpu) {
  for (auto iterator = gpu_cache.begin(); iterator != gpu_cache.end();) {
    if (std::find(layers.cbegin(), layers.cend(), iterator->first) == layers.cend() || !iterator->first->visible()) {
      release_gpu(iterator->second);
      iterator = gpu_cache.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = pending_fit.begin(); iterator != pending_fit.end();) {
    if (std::find(layers.cbegin(), layers.cend(), *iterator) == layers.cend()) {
      iterator = pending_fit.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (Layer* layer : layers) {
    if (std::find(current.cbegin(), current.cend(), layer) == current.cend()) {
      pending_fit.insert(layer);
    }
  }
  current = layers;
}

constexpr quint32 kInitialVertexBufferBytes = 64U * 1024U;
constexpr quint32 kPointInitialVertexBufferBytes = 1024U;
constexpr quint32 kPointUniformBytes = 272U;
constexpr quint32 kPoseUniformBytes = 192U;
constexpr quint32 kOccupancyUniformBytes = 144U;
constexpr quint32 kVoxelUniformBytes = 256U;
constexpr quint32 kModelUniformBytes = 128U;
constexpr quint32 kRenderModeUniformBytes = 16U;
static_assert(look::kTonemapMode == 1, "WASM composite implements the native application's ACES default");

constexpr std::size_t kModelTextureSlotCount = 5U;
constexpr std::size_t kBaseColorSlot = 0U;
constexpr std::size_t kMetallicRoughnessSlot = 1U;
constexpr std::size_t kNormalSlot = 2U;
constexpr std::size_t kOcclusionSlot = 3U;
constexpr std::size_t kEmissiveSlot = 4U;
constexpr std::array<float, 12> kOccupancyQuad = {0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
                                                  0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};

struct alignas(16) PointUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
  std::array<float, 4> render_origin{};
  std::array<float, 4> point_params{};
  std::array<float, 4> range_params{};
  std::array<float, 4> solid_params{};
  std::array<std::int32_t, 4> modes{};
};
static_assert(sizeof(PointUniforms) == kPointUniformBytes);

struct alignas(16) PoseUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
};
static_assert(sizeof(PoseUniforms) == kPoseUniformBytes);

struct alignas(16) OccupancyUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_grid{};
  std::array<float, 4> display_params{};
};
static_assert(sizeof(OccupancyUniforms) == kOccupancyUniformBytes);

struct alignas(16) VoxelUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_grid{};
  std::array<float, 16> view{};
  std::array<float, 4> cell_size_opacity{};
  std::array<float, 4> dims_kind{};
  std::array<float, 4> range_params{};
  std::array<float, 4> mode_params{};
};
static_assert(sizeof(VoxelUniforms) == kVoxelUniformBytes);

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

std::array<const TextureSource*, kModelTextureSlotCount> modelTextureSources(const Material& material) {
  return {
      &material.base_color, &material.metallic_roughness, &material.normal, &material.occlusion, &material.emissive};
}

constexpr bool modelTextureIsSrgb(std::size_t slot) {
  return slot == kBaseColorSlot || slot == kEmissiveSlot;
}

bool materialUsesPbrPath(const Material& material) {
  const bool has_emissive =
      material.emissive_factor.r != 0.0F || material.emissive_factor.g != 0.0F || material.emissive_factor.b != 0.0F;
  const auto textures = modelTextureSources(material);
  return material.has_pbr || has_emissive ||
         std::any_of(textures.cbegin(), textures.cend(), [](const TextureSource* texture) {
           return texture != nullptr && !texture->empty();
         });
}

const ArrowMeshData& unitPoseArrowMesh() {
  static const ArrowMeshData mesh = buildArrowMesh(
      ArrowMeshParams{
          .length = 1.0F, .shaft_radius = 0.04F, .head_length = 0.30F, .head_radius = 0.10F, .segments = 16});
  return mesh;
}

// Scene backdrop colour for the current theme, read the same way the desktop GL
// view does (application-palette Window lightness -> framework DataBackdrop
// token) so both backends share one source of truth.
QColor sceneBackdropColor() {
  const QColor window_background = QGuiApplication::palette().color(QPalette::Window);
  const auto theme = PJ::theme::themeFor(window_background.lightness() >= 128);
  return PJ::theme::surface(PJ::theme::Surface::DataBackdrop, theme);
}

struct MarkerMeshData {
  std::vector<CubeVertex> vertices;
  std::vector<std::uint32_t> triangle_indices;
  std::vector<std::uint32_t> edge_indices;
};

void buildTriangleEdges(MarkerMeshData& mesh) {
  mesh.edge_indices.reserve(mesh.triangle_indices.size() * 2U);
  for (std::size_t index = 0; index + 2U < mesh.triangle_indices.size(); index += 3U) {
    const std::uint32_t a = mesh.triangle_indices[index];
    const std::uint32_t b = mesh.triangle_indices[index + 1U];
    const std::uint32_t c = mesh.triangle_indices[index + 2U];
    mesh.edge_indices.insert(mesh.edge_indices.end(), {a, b, b, c, c, a});
  }
}

MarkerMeshData makeMarkerCube() {
  MarkerMeshData mesh;
  mesh.vertices.assign(kCubeVertices.cbegin(), kCubeVertices.cend());
  mesh.triangle_indices.assign(kCubeIndices.cbegin(), kCubeIndices.cend());
  buildTriangleEdges(mesh);
  return mesh;
}

MarkerMeshData makeMarkerCubeEdgeOverlay() {
  MarkerMeshData mesh;
  mesh.vertices = {
      {-0.5F, -0.5F, -0.5F, 0.0F, 0.0F, 0.0F}, {0.5F, -0.5F, -0.5F, 0.0F, 0.0F, 0.0F},
      {0.5F, 0.5F, -0.5F, 0.0F, 0.0F, 0.0F},   {-0.5F, 0.5F, -0.5F, 0.0F, 0.0F, 0.0F},
      {-0.5F, -0.5F, 0.5F, 0.0F, 0.0F, 0.0F},  {0.5F, -0.5F, 0.5F, 0.0F, 0.0F, 0.0F},
      {0.5F, 0.5F, 0.5F, 0.0F, 0.0F, 0.0F},    {-0.5F, 0.5F, 0.5F, 0.0F, 0.0F, 0.0F},
  };
  mesh.edge_indices = {
      0U, 1U, 1U, 2U, 2U, 3U, 3U, 0U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 4U, 0U, 4U, 1U, 5U, 2U, 6U, 3U, 7U,
  };
  return mesh;
}

MarkerMeshData makeMarkerSphere() {
  MarkerMeshData mesh;
  constexpr int kStacks = 16;
  constexpr int kSectors = 32;
  constexpr float kPi = 3.14159265358979323846F;
  mesh.vertices.reserve((kStacks + 1U) * (kSectors + 1U));
  for (int stack = 0; stack <= kStacks; ++stack) {
    const float latitude = -0.5F * kPi + kPi * static_cast<float>(stack) / static_cast<float>(kStacks);
    const float z = std::sin(latitude);
    const float radial = std::cos(latitude);
    for (int sector = 0; sector <= kSectors; ++sector) {
      const float longitude = 2.0F * kPi * static_cast<float>(sector) / static_cast<float>(kSectors);
      const float x = radial * std::cos(longitude);
      const float y = radial * std::sin(longitude);
      mesh.vertices.push_back({0.5F * x, 0.5F * y, 0.5F * z, x, y, z});
    }
  }
  for (int stack = 0; stack < kStacks; ++stack) {
    for (int sector = 0; sector < kSectors; ++sector) {
      const std::uint32_t a = static_cast<std::uint32_t>(stack * (kSectors + 1) + sector);
      const std::uint32_t b = a + static_cast<std::uint32_t>(kSectors + 1);
      if (stack != 0) {
        mesh.triangle_indices.insert(mesh.triangle_indices.end(), {a, b, a + 1U});
      }
      if (stack != kStacks - 1) {
        mesh.triangle_indices.insert(mesh.triangle_indices.end(), {a + 1U, b, b + 1U});
      }
    }
  }
  buildTriangleEdges(mesh);
  return mesh;
}

MarkerMeshData makeMarkerCylinder() {
  MarkerMeshData mesh;
  constexpr int kSegments = 24;
  constexpr float kPi = 3.14159265358979323846F;
  for (int ring = 0; ring < 2; ++ring) {
    const float z = ring == 0 ? -0.5F : 0.5F;
    for (int segment = 0; segment < kSegments; ++segment) {
      const float angle = 2.0F * kPi * static_cast<float>(segment) / static_cast<float>(kSegments);
      const float x = std::cos(angle);
      const float y = std::sin(angle);
      mesh.vertices.push_back({0.5F * x, 0.5F * y, z, x, y, 0.0F});
    }
  }
  for (int segment = 0; segment < kSegments; ++segment) {
    const std::uint32_t next = static_cast<std::uint32_t>((segment + 1) % kSegments);
    const std::uint32_t a = static_cast<std::uint32_t>(segment);
    const std::uint32_t b = next;
    const std::uint32_t c = static_cast<std::uint32_t>(kSegments) + next;
    const std::uint32_t d = static_cast<std::uint32_t>(kSegments + segment);
    mesh.triangle_indices.insert(mesh.triangle_indices.end(), {a, b, c, a, c, d});
  }
  for (int cap = 0; cap < 2; ++cap) {
    const float z = cap == 0 ? -0.5F : 0.5F;
    const float normal = cap == 0 ? -1.0F : 1.0F;
    const std::uint32_t center = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({0.0F, 0.0F, z, 0.0F, 0.0F, normal});
    const std::uint32_t ring = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int segment = 0; segment < kSegments; ++segment) {
      const float angle = 2.0F * kPi * static_cast<float>(segment) / static_cast<float>(kSegments);
      mesh.vertices.push_back({0.5F * std::cos(angle), 0.5F * std::sin(angle), z, 0.0F, 0.0F, normal});
    }
    for (int segment = 0; segment < kSegments; ++segment) {
      const std::uint32_t current = ring + static_cast<std::uint32_t>(segment);
      const std::uint32_t next = ring + static_cast<std::uint32_t>((segment + 1) % kSegments);
      if (cap == 0) {
        mesh.triangle_indices.insert(mesh.triangle_indices.end(), {center, next, current});
      } else {
        mesh.triangle_indices.insert(mesh.triangle_indices.end(), {center, current, next});
      }
    }
  }
  buildTriangleEdges(mesh);
  return mesh;
}

MarkerMeshData makeMarkerArrow() {
  const ArrowMeshData source = buildArrowMesh(
      ArrowMeshParams{.length = 1.0F, .shaft_radius = 0.2F, .head_length = 0.3F, .head_radius = 0.5F, .segments = 16});
  MarkerMeshData mesh;
  mesh.vertices.reserve(source.vertices.size() / 6U);
  for (std::size_t index = 0; index + 5U < source.vertices.size(); index += 6U) {
    mesh.vertices.push_back(
        {source.vertices[index], source.vertices[index + 1U], source.vertices[index + 2U], source.vertices[index + 3U],
         source.vertices[index + 4U], source.vertices[index + 5U]});
  }
  mesh.triangle_indices = source.indices;
  buildTriangleEdges(mesh);
  return mesh;
}

const std::array<MarkerMeshData, 5>& markerMeshes() {
  static const std::array<MarkerMeshData, 5> meshes{
      makeMarkerCube(), makeMarkerSphere(), makeMarkerCylinder(), makeMarkerArrow(), makeMarkerCubeEdgeOverlay()};
  return meshes;
}

AABB transformedMarkerBounds(const WasmMarkerBounds& source, const Transform& fixed_from_frame) {
  if (!source.valid) {
    return {};
  }
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? source.max.x : source.min.x,
        (corner & 2) != 0 ? source.max.y : source.min.y,
        (corner & 4) != 0 ? source.max.z : source.min.z,
    };
    expandAABB(output, glm::vec3(fixed_from_frame * point));
  }
  return output;
}

AABB modelBounds(const MeshData& mesh) {
  AABB output;
  for (const auto& vertex : mesh.vertices) {
    if (std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z)) {
      expandAABB(output, vertex.position);
    }
  }
  return output;
}

AABB transformedModelBounds(const AABB& source, const glm::dmat4& fixed_from_model) {
  if (!source.valid) {
    return {};
  }
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? source.max.x : source.min.x,
        (corner & 2) != 0 ? source.max.y : source.min.y,
        (corner & 4) != 0 ? source.max.z : source.min.z,
    };
    const glm::dvec3 transformed = glm::dvec3(fixed_from_model * glm::dvec4(point, 1.0));
    if (std::isfinite(transformed.x) && std::isfinite(transformed.y) && std::isfinite(transformed.z)) {
      expandAABB(output, glm::vec3(transformed));
    }
  }
  return output;
}

AABB transformedModelBoundsRelative(
    const AABB& source, const glm::dmat4& fixed_from_model, const glm::dvec3& render_origin) {
  if (!source.valid) {
    return {};
  }
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? source.max.x : source.min.x,
        (corner & 2) != 0 ? source.max.y : source.min.y,
        (corner & 4) != 0 ? source.max.z : source.min.z,
    };
    // Subtract the camera-relative origin while the transform is still double
    // precision. Casting world coordinates to float first makes a far-away
    // caster's bounds collapse and its light frustum swim independently of the
    // model matrix, which performs this same subtraction before narrowing.
    const glm::dvec3 transformed = glm::dvec3(fixed_from_model * glm::dvec4(point, 1.0)) - render_origin;
    if (std::isfinite(transformed.x) && std::isfinite(transformed.y) && std::isfinite(transformed.z)) {
      expandAABB(output, glm::vec3(transformed));
    }
  }
  return output;
}

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning("SceneViewWidget WASM: failed to open shader %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

// QRhi 6.11 reports no 3D-texture limit. On OpenGL the widget's context is current
// inside initialize()/render(), so ask the driver; elsewhere use 2048, the D3D11
// feature-level-11 and Metal minimum.
// ponytail: Vulkan only guarantees 256, but desktop drivers report >= 2048; query
// VkPhysicalDeviceLimits through QRhiVulkanNativeHandles if a device rejects it.
int max3dTextureSize(const QRhi& rhi) {
  if (rhi.backend() != QRhi::OpenGLES2) {
    return 2048;
  }
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return 0;
  }
  GLint limit = 0;
  context->functions()->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &limit);
  return std::max(limit, 0);
}

QMatrix4x4 toQMatrix(const glm::mat4& matrix) {
  // glm stores columns; QMatrix4x4's scalar constructor is row-major.
  return QMatrix4x4(
      matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0], matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
      matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2], matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}

bool cameraStatesEqual(const CameraState& left, const CameraState& right) {
  return left.focal == right.focal && left.radius == right.radius && left.azimuth == right.azimuth &&
         left.elevation == right.elevation && left.fov_y == right.fov_y && left.ortho_scale == right.ortho_scale &&
         left.perspective == right.perspective;
}

uint64_t hashTransform(const Transform& transform, uint64_t seed) {
  const glm::dmat4 matrix = transform.matrix();
  uint64_t hash = seed;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      uint64_t bits = 0;
      std::memcpy(&bits, &matrix[column][row], sizeof(bits));
      hash = (hash ^ bits) * 0x100000001b3ULL;
    }
  }
  return hash;
}

void copyMatrix(const QMatrix4x4& matrix, std::array<float, 16>& output) {
  std::memcpy(output.data(), matrix.constData(), 16U * sizeof(float));
}

AABB transformedBounds(const AABB& source, const Transform& fixed_from_source) {
  if (!source.valid) {
    return {};
  }
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? source.max.x : source.min.x,
        (corner & 2) != 0 ? source.max.y : source.min.y,
        (corner & 4) != 0 ? source.max.z : source.min.z,
    };
    const glm::dvec3 transformed = fixed_from_source * point;
    expandAABB(output, glm::vec3(transformed));
  }
  return output;
}

AABB occupancyBoundsInFixed(const ReconstructedGrid& grid, const Transform& fixed_from_source) {
  if (grid.empty() || grid.width == 0U || grid.height == 0U || grid.resolution <= 0.0) {
    return {};
  }
  const auto& p = grid.origin.position;
  const auto& q = grid.origin.orientation;
  const Transform source_from_grid(glm::dvec3(p.x, p.y, p.z), glm::normalize(glm::dquat(q.w, q.x, q.y, q.z)));
  const Transform fixed_from_grid = fixed_from_source * source_from_grid;
  const double width = grid.resolution * static_cast<double>(grid.width);
  const double height = grid.resolution * static_cast<double>(grid.height);
  AABB bounds;
  for (const glm::dvec3 corner :
       {glm::dvec3(0.0, 0.0, 0.0), glm::dvec3(width, 0.0, 0.0), glm::dvec3(0.0, height, 0.0),
        glm::dvec3(width, height, 0.0)}) {
    expandAABB(bounds, glm::vec3(fixed_from_grid * corner));
  }
  return bounds;
}

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QRhiWidget(parent) {
  static const bool resources_initialized = [] {
    initializeScene3dWasmResources();
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
  connect(this, &QRhiWidget::renderFailed, this, []() { qWarning("SceneViewWidget WASM: QRhi render failed"); });
}

SceneViewWidget::~SceneViewWidget() {
  releaseResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  last_frames_revision_ = ~uint64_t{0};
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers) {
  std::vector<OrderedLayerEntry> order;
  std::vector<WasmPointRenderable*> points;
  std::vector<WasmPosesInFrameLayer*> poses;
  std::vector<WasmOccupancyGridLayer*> occupancy;
  std::vector<WasmVoxelGridLayer*> voxels;
  std::vector<WasmSceneEntitiesLayer*> markers;
  std::vector<WasmModelRenderable*> models;
  order.reserve(ordered_layers.size());
  points.reserve(ordered_layers.size());
  poses.reserve(ordered_layers.size());
  occupancy.reserve(ordered_layers.size());
  voxels.reserve(ordered_layers.size());
  markers.reserve(ordered_layers.size());
  models.reserve(ordered_layers.size());
  for (PJ::ISceneLayer* layer : ordered_layers) {
    if (auto* point = dynamic_cast<WasmPointRenderable*>(layer); point != nullptr) {
      order.push_back({point, layer->info().topic_id.id});
      points.push_back(point);
    } else if (auto* pose = dynamic_cast<WasmPosesInFrameLayer*>(layer); pose != nullptr) {
      order.push_back({pose, layer->info().topic_id.id});
      poses.push_back(pose);
    } else if (auto* grid = dynamic_cast<WasmOccupancyGridLayer*>(layer); grid != nullptr) {
      order.push_back({grid, layer->info().topic_id.id});
      occupancy.push_back(grid);
    } else if (auto* voxel = dynamic_cast<WasmVoxelGridLayer*>(layer); voxel != nullptr) {
      order.push_back({voxel, layer->info().topic_id.id});
      voxels.push_back(voxel);
    } else if (auto* marker = dynamic_cast<WasmSceneEntitiesLayer*>(layer); marker != nullptr) {
      order.push_back({marker, layer->info().topic_id.id});
      markers.push_back(marker);
      models.push_back(marker);
    } else if (auto* model = dynamic_cast<WasmModelRenderable*>(layer); model != nullptr) {
      order.push_back({model, layer->info().topic_id.id});
      models.push_back(model);
    }
  }
  ordered_layers_ = std::move(order);
  setPointRenderableLayers(points);
  setPosesInFrameLayers(poses);
  setOccupancyGridLayers(occupancy);
  setVoxelGridLayers(voxels);
  setSceneEntitiesLayers(markers);
  setModelRenderableLayers(models);
}

void SceneViewWidget::setPointRenderableLayers(const std::vector<WasmPointRenderable*>& layers) {
  adoptLayerList(layers, point_layers_, point_layer_gpu_, point_layers_pending_fit_, [this](PointLayerGpu& gpu) {
    releasePointLayerGpu(gpu);
  });
  update();
}

void SceneViewWidget::setPosesInFrameLayers(const std::vector<WasmPosesInFrameLayer*>& layers) {
  adoptLayerList(layers, pose_layers_, pose_layer_gpu_, pose_layers_pending_fit_, [this](PoseLayerGpu& gpu) {
    releasePoseLayerGpu(gpu);
  });
  update();
}

void SceneViewWidget::setOccupancyGridLayers(const std::vector<WasmOccupancyGridLayer*>& layers) {
  adoptLayerList(
      layers, occupancy_layers_, occupancy_layer_gpu_, occupancy_layers_pending_fit_,
      [this](OccupancyLayerGpu& gpu) { releaseOccupancyLayerGpu(gpu); });
  update();
}

void SceneViewWidget::setVoxelGridLayers(const std::vector<WasmVoxelGridLayer*>& layers) {
  adoptLayerList(layers, voxel_layers_, voxel_layer_gpu_, voxel_layers_pending_fit_, [this](VoxelLayerGpu& gpu) {
    releaseVoxelLayerGpu(gpu);
  });
  update();
}

void SceneViewWidget::setSceneEntitiesLayers(const std::vector<WasmSceneEntitiesLayer*>& layers) {
  for (auto iterator = marker_layers_pending_fit_.begin(); iterator != marker_layers_pending_fit_.end();) {
    if (std::find(layers.cbegin(), layers.cend(), *iterator) == layers.cend()) {
      iterator = marker_layers_pending_fit_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (WasmSceneEntitiesLayer* layer : layers) {
    if (std::find(marker_layers_.cbegin(), marker_layers_.cend(), layer) == marker_layers_.cend()) {
      marker_layers_pending_fit_.insert(layer);
    }
  }
  marker_layers_ = layers;
  update();
}

void SceneViewWidget::setModelRenderableLayers(const std::vector<WasmModelRenderable*>& layers) {
  for (auto iterator = model_layer_gpu_.begin(); iterator != model_layer_gpu_.end();) {
    if (std::find(layers.cbegin(), layers.cend(), iterator->first) == layers.cend() || !iterator->first->visible()) {
      releaseModelLayerGpu(iterator->second);
      iterator = model_layer_gpu_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  model_layers_ = layers;
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
  update();
  emit presentationChanged();
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
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, render_time_);
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
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return 0;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, time);
  if (!transform) {
    return 0;
  }
  uint64_t hash = 1469598103934665603ULL;
  for (const double component : {transform->t.x, transform->t.y, transform->t.z}) {
    uint64_t bits = 0;
    std::memcpy(&bits, &component, sizeof(bits));
    hash = (hash ^ bits) * 1099511628211ULL;
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
  // behavior instead of silently producing an empty browser view.
  for (const OrderedLayerEntry& entry : ordered_layers_) {
    const std::string source = std::visit(
        [](const auto* layer) { return layer != nullptr ? layer->sourceFrame() : std::string{}; }, entry.layer);
    if (!source.empty()) {
      return source;
    }
  }
  return {};
}

void SceneViewWidget::appendLine(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color) {
  const auto vertex = [&color](const glm::dvec3& point) {
    return Vertex{
        static_cast<float>(point.x),
        static_cast<float>(point.y),
        static_cast<float>(point.z),
        color.r,
        color.g,
        color.b,
        color.a};
  };
  line_vertices_.push_back(vertex(a));
  line_vertices_.push_back(vertex(b));
}

void SceneViewWidget::appendTriangle(
    const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::vec4& color) {
  const auto vertex = [&color](const glm::dvec3& point) {
    return Vertex{
        static_cast<float>(point.x),
        static_cast<float>(point.y),
        static_cast<float>(point.z),
        color.r,
        color.g,
        color.b,
        color.a};
  };
  triangle_vertices_.push_back(vertex(a));
  triangle_vertices_.push_back(vertex(b));
  triangle_vertices_.push_back(vertex(c));
}

void SceneViewWidget::buildGeometry(const glm::dvec3& render_origin) {
  line_vertices_.clear();
  triangle_vertices_.clear();
  grid_line_vertex_count_ = 0;
  grid_triangle_vertex_count_ = 0;
  last_resolved_frame_count_ = 0;

  if (grid_visible_) {
    if (grid_style_ == GridStyle::kFilledCells) {
      const std::vector<GridVertex> cells = buildCheckerboardCells(grid_extent_m_, grid_divisions_);
      for (std::size_t index = 0; index + 2 < cells.size(); index += 3) {
        const glm::vec4 color =
            cells[index].parity < 0.5F ? glm::vec4(look::kGridCellToneA, 1.0F) : glm::vec4(look::kGridCellToneB, 1.0F);
        appendTriangle(
            glm::dvec3(cells[index].pos) - render_origin, glm::dvec3(cells[index + 1].pos) - render_origin,
            glm::dvec3(cells[index + 2].pos) - render_origin, color);
      }
    }
    for (const GridVertex& vertex : buildGridLines(grid_extent_m_, grid_divisions_)) {
      const glm::dvec3 point = glm::dvec3(vertex.pos) - render_origin;
      line_vertices_.push_back(
          Vertex{
              static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z),
              look::kGridLineColor.r, look::kGridLineColor.g, look::kGridLineColor.b, 1.0F});
    }
    grid_line_vertex_count_ = static_cast<quint32>(line_vertices_.size());
    grid_triangle_vertex_count_ = static_cast<quint32>(triangle_vertices_.size());
  }

  tf_triad_instances_.clear();
  const std::string fixed = effectiveFixedFrame();
  if (tf_ != nullptr && !fixed.empty()) {
    if (tf_connections_visible_) {
      std::vector<TfConnectionSegment> segments;
      buildTfConnectionSegments(*tf_, fixed, render_time_, segments, render_origin);
      for (const TfConnectionSegment& segment : segments) {
        appendLine(glm::dvec3(segment.child), glm::dvec3(segment.parent), glm::vec4(look::kTfConnectionColor, 1.0F));
      }
    }
    if (axes_visible_) {
      // Solid arrow triads, matching the desktop AxisRenderPass. The frame's
      // fixed-from-frame transform (translation shifted into camera-relative
      // space by render_origin) is baked into each arm's model, so the draw
      // uses an identity fixed_from_source uniform.
      const PoseTriadStyle triad_style{
          .axis_length = gizmo_size_m_, .opacity = gizmo_opacity_, .x_arrow_only = false, .override_color = false};
      for (const std::string& frame : tf_->getAllFrames()) {
        const auto transform = tf_->tryLookupTransform(fixed, frame, render_time_);
        if (!transform) {
          continue;
        }
        ++last_resolved_frame_count_;
        glm::dmat4 base = transform->matrix();
        base[3].x -= render_origin.x;
        base[3].y -= render_origin.y;
        base[3].z -= render_origin.z;
        appendTriadArms(glm::mat4(base), triad_style, tf_triad_instances_);
      }
    }
  }

  last_line_vertex_count_ = static_cast<int>(line_vertices_.size());
  last_triangle_vertex_count_ = static_cast<int>(triangle_vertices_.size());
}

bool SceneViewWidget::ensureVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity) {
  if (required_bytes == 0) {
    return true;
  }
  if (buffer != nullptr && capacity >= required_bytes) {
    return true;
  }
  quint32 next_capacity = std::max(capacity, minimum_capacity);
  while (next_capacity < required_bytes && next_capacity <= std::numeric_limits<quint32>::max() / 2U) {
    next_capacity *= 2U;
  }
  if (next_capacity < required_bytes) {
    return false;
  }
  delete buffer;
  buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, next_capacity);
  if (!buffer->create()) {
    delete buffer;
    buffer = nullptr;
    capacity = 0;
    return false;
  }
  capacity = next_capacity;
  return true;
}

bool SceneViewWidget::ensurePointLayerGpu(QRhi* owner, WasmPointRenderable* /*layer*/, PointLayerGpu& gpu) {
  if (owner == nullptr || colormap_texture_ == nullptr || colormap_sampler_ == nullptr) {
    return false;
  }
  if (gpu.uniform_buffer == nullptr) {
    gpu.uniform_buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kPointUniformBytes);
    if (!gpu.uniform_buffer->create()) {
      releasePointLayerGpu(gpu);
      return false;
    }
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = owner->newShaderResourceBindings();
    gpu.shader_resources->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, gpu.uniform_buffer),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, colormap_texture_, colormap_sampler_),
        QRhiShaderResourceBinding::uniformBuffer(
            8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
    });
    if (!gpu.shader_resources->create()) {
      releasePointLayerGpu(gpu);
      return false;
    }
  }
  return true;
}

void SceneViewWidget::releasePointLayerGpu(PointLayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.vertex_buffer;
  delete gpu.uniform_buffer;
  gpu = {};
}

bool SceneViewWidget::ensurePoseLayerGpu(QRhi* owner, PoseLayerGpu& gpu) {
  if (owner == nullptr) {
    return false;
  }
  if (gpu.uniform_buffer == nullptr) {
    gpu.uniform_buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kPoseUniformBytes);
    if (!gpu.uniform_buffer->create()) {
      releasePoseLayerGpu(gpu);
      return false;
    }
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = owner->newShaderResourceBindings();
    gpu.shader_resources->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, gpu.uniform_buffer),
        QRhiShaderResourceBinding::uniformBuffer(
            8, QRhiShaderResourceBinding::FragmentStage, annotation_render_mode_uniform_buffer_),
    });
    if (!gpu.shader_resources->create()) {
      releasePoseLayerGpu(gpu);
      return false;
    }
  }
  return true;
}

void SceneViewWidget::releasePoseLayerGpu(PoseLayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.instance_buffer;
  delete gpu.uniform_buffer;
  gpu = {};
}

bool SceneViewWidget::ensureOccupancyLayerGpu(
    QRhi* owner, WasmOccupancyGridLayer* layer, OccupancyLayerGpu& gpu, bool& texture_recreated) {
  texture_recreated = false;
  if (owner == nullptr || layer == nullptr || occupancy_sampler_ == nullptr) {
    return false;
  }
  const ReconstructedGrid& grid = layer->grid();
  const int texture_limit = owner->resourceLimit(QRhi::TextureSizeMax);
  if (texture_limit <= 0 || grid.width == 0U || grid.height == 0U || grid.width > static_cast<quint32>(texture_limit) ||
      grid.height > static_cast<quint32>(texture_limit)) {
    layer->noteRenderFailure(
        tr("Occupancy grid is %1 x %2; this browser GPU supports textures up to %3 x %3")
            .arg(QString::number(grid.width), QString::number(grid.height), QString::number(texture_limit)));
    return false;
  }
  if (gpu.uniform_buffer == nullptr) {
    gpu.uniform_buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kOccupancyUniformBytes);
    if (!gpu.uniform_buffer->create()) {
      layer->noteRenderFailure(tr("Could not create the occupancy-grid uniform buffer"));
      releaseOccupancyLayerGpu(gpu);
      return false;
    }
  }
  if (gpu.texture == nullptr || gpu.width != grid.width || gpu.height != grid.height) {
    delete gpu.shader_resources;
    delete gpu.texture;
    gpu.shader_resources = nullptr;
    gpu.texture =
        owner->newTexture(QRhiTexture::R8, QSize(static_cast<int>(grid.width), static_cast<int>(grid.height)));
    if (!gpu.texture->create()) {
      delete gpu.texture;
      gpu.texture = nullptr;
      gpu.width = 0;
      gpu.height = 0;
      layer->noteRenderFailure(tr("Could not allocate the occupancy-grid texture"));
      return false;
    }
    gpu.width = grid.width;
    gpu.height = grid.height;
    gpu.uploaded_revision = ~uint64_t{0};
    texture_recreated = true;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = owner->newShaderResourceBindings();
    gpu.shader_resources->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, gpu.uniform_buffer),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, gpu.texture, occupancy_sampler_),
        QRhiShaderResourceBinding::uniformBuffer(
            8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
    });
    if (!gpu.shader_resources->create()) {
      layer->noteRenderFailure(tr("Could not create occupancy-grid shader bindings"));
      releaseOccupancyLayerGpu(gpu);
      return false;
    }
  }
  layer->noteRenderSuccess();
  return true;
}

void SceneViewWidget::releaseOccupancyLayerGpu(OccupancyLayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

bool SceneViewWidget::ensureVoxelLayerGpu(
    QRhi* owner, WasmVoxelGridLayer* layer, VoxelLayerGpu& gpu, bool& texture_recreated) {
  texture_recreated = false;
  if (owner == nullptr || layer == nullptr || voxel_sampler_ == nullptr || colormap_texture_ == nullptr ||
      colormap_sampler_ == nullptr) {
    return false;
  }
  const QString rejection = voxelGpuRejection(owner, layer);
  if (!rejection.isEmpty()) {
    layer->noteRenderFailure(rejection);
    return false;
  }
  const quint32 columns = layer->columnCount();
  const quint32 rows = layer->rowCount();
  const quint32 slices = layer->sliceCount();
  if (gpu.uniform_buffer == nullptr) {
    gpu.uniform_buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kVoxelUniformBytes);
    if (!gpu.uniform_buffer->create()) {
      layer->noteRenderFailure(tr("Could not create the voxel-grid uniform buffer"));
      releaseVoxelLayerGpu(gpu);
      return false;
    }
  }

  const int value_kind = static_cast<int>(layer->valueKind());
  if (gpu.texture == nullptr || gpu.columns != columns || gpu.rows != rows || gpu.slices != slices ||
      gpu.value_kind != value_kind) {
    delete gpu.shader_resources;
    delete gpu.texture;
    gpu.shader_resources = nullptr;
    const QRhiTexture::Format format =
        layer->valueKind() == VoxelValueKind::kScalar ? QRhiTexture::R32F : QRhiTexture::RGBA8;
    gpu.texture = owner->newTexture(
        format, static_cast<int>(columns), static_cast<int>(rows), static_cast<int>(slices), 1,
        QRhiTexture::ThreeDimensional);
    if (!gpu.texture->create()) {
      delete gpu.texture;
      gpu.texture = nullptr;
      gpu.columns = 0;
      gpu.rows = 0;
      gpu.slices = 0;
      gpu.value_kind = -1;
      layer->noteRenderFailure(tr("Could not allocate the voxel-grid 3D texture"));
      return false;
    }
    gpu.columns = columns;
    gpu.rows = rows;
    gpu.slices = slices;
    gpu.value_kind = value_kind;
    gpu.uploaded_revision = ~uint64_t{0};
    texture_recreated = true;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = owner->newShaderResourceBindings();
    gpu.shader_resources->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, gpu.uniform_buffer),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::VertexStage, gpu.texture, voxel_sampler_),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, colormap_texture_, colormap_sampler_),
        QRhiShaderResourceBinding::uniformBuffer(
            8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
    });
    if (!gpu.shader_resources->create()) {
      layer->noteRenderFailure(tr("Could not create voxel-grid shader bindings"));
      releaseVoxelLayerGpu(gpu);
      return false;
    }
  }
  layer->noteRenderSuccess();
  return true;
}

QString SceneViewWidget::voxelGpuRejection(QRhi* owner, const WasmVoxelGridLayer* layer) {
  if (owner == nullptr || layer == nullptr) {
    return tr("The voxel-grid renderer is not available");
  }
  if (!owner->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
    return tr("This browser GPU does not support 3D textures");
  }
  const int effective_limit = std::min(max_3d_texture_size_, owner->resourceLimit(QRhi::TextureSizeMax));
  const quint32 columns = layer->columnCount();
  const quint32 rows = layer->rowCount();
  const quint32 slices = layer->sliceCount();
  if (effective_limit > 0 && columns > 0U && rows > 0U && slices > 0U &&
      columns <= static_cast<quint32>(effective_limit) && rows <= static_cast<quint32>(effective_limit) &&
      slices <= static_cast<quint32>(effective_limit)) {
    return {};
  }
  return tr("Voxel grid is %1 x %2 x %3; this browser GPU supports 3D textures up to %4 per dimension")
      .arg(QString::number(columns), QString::number(rows), QString::number(slices), QString::number(effective_limit));
}

void SceneViewWidget::releaseVoxelLayerGpu(VoxelLayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

void SceneViewWidget::releaseMarkerMeshGpu(MarkerMeshGpu& gpu) {
  delete gpu.edge_index_buffer;
  delete gpu.triangle_index_buffer;
  delete gpu.vertex_buffer;
  gpu = {};
}

void SceneViewWidget::releaseModelMeshGpu(ModelMeshGpu& gpu) {
  // Bindings may reference textures owned by another submesh entry representing
  // the same shared material. Destroy every binding before any owned texture.
  for (ModelMaterialGpu& material : gpu.materials) {
    delete material.shader_resources;
    material.shader_resources = nullptr;
  }
  for (ModelMaterialGpu& material : gpu.materials) {
    for (ModelTextureGpu& texture : material.textures) {
      if (texture.owns_texture) {
        delete texture.texture;
      }
    }
  }
  delete gpu.edge_index_buffer;
  delete gpu.triangle_index_buffer;
  delete gpu.vertex_buffer;
  gpu = {};
}

void SceneViewWidget::releaseModelLayerGpu(ModelLayerGpu& gpu) {
  for (auto& [_, mesh] : gpu.meshes) {
    releaseModelMeshGpu(mesh);
  }
  gpu = {};
}

QRhiShaderResourceBindings* SceneViewWidget::createModelMaterialBindings(
    QRhi* owner, const ModelMaterialGpu& material) {
  if (owner == nullptr || uniform_buffer_ == nullptr || model_uniform_buffer_ == nullptr ||
      model_white_texture_ == nullptr || model_sampler_ == nullptr) {
    return nullptr;
  }
  QRhiTexture* shadow_texture = shadow_texture_ != nullptr ? shadow_texture_ : model_white_texture_;
  QRhiSampler* shadow_sampler = shadow_sampler_ != nullptr ? shadow_sampler_ : model_sampler_;
  QRhiShaderResourceBindings* replacement = owner->newShaderResourceBindings();
  replacement->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, material.textures[kBaseColorSlot].texture, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          2, QRhiShaderResourceBinding::FragmentStage, material.textures[kMetallicRoughnessSlot].texture,
          model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          3, QRhiShaderResourceBinding::FragmentStage, material.textures[kNormalSlot].texture, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          4, QRhiShaderResourceBinding::FragmentStage, material.textures[kOcclusionSlot].texture, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          5, QRhiShaderResourceBinding::FragmentStage, material.textures[kEmissiveSlot].texture, model_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(6, QRhiShaderResourceBinding::FragmentStage, model_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          7, QRhiShaderResourceBinding::FragmentStage, shadow_texture, shadow_sampler),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  if (!replacement->create()) {
    delete replacement;
    return nullptr;
  }
  return replacement;
}

bool SceneViewWidget::rebuildModelMaterialBindings(QRhi* owner, ModelMaterialGpu& material) {
  QRhiShaderResourceBindings* replacement = createModelMaterialBindings(owner, material);
  if (replacement == nullptr) {
    return false;
  }
  delete material.shader_resources;
  material.shader_resources = replacement;
  return true;
}

bool SceneViewWidget::rebuildAllModelMaterialBindings(QRhi* owner) {
  std::vector<std::pair<ModelMaterialGpu*, QRhiShaderResourceBindings*>> replacements;
  for (auto& [_, layer] : model_layer_gpu_) {
    for (auto& [__, mesh] : layer.meshes) {
      for (ModelMaterialGpu& material : mesh.materials) {
        QRhiShaderResourceBindings* replacement = createModelMaterialBindings(owner, material);
        if (replacement == nullptr) {
          for (const auto& [___, pending] : replacements) {
            delete pending;
          }
          return false;
        }
        replacements.emplace_back(&material, replacement);
      }
    }
  }
  // Commit only after every replacement exists. A failed shadow enable/disable
  // must not leave half the materials bound to the old depth texture.
  for (const auto& [material, replacement] : replacements) {
    delete material->shader_resources;
    material->shader_resources = replacement;
  }
  return true;
}

bool SceneViewWidget::ensureModelLayerGpu(QRhi* owner, WasmModelRenderable* layer, ModelLayerGpu& gpu) {
  if (owner == nullptr || layer == nullptr || model_white_texture_ == nullptr || model_sampler_ == nullptr ||
      uniform_buffer_ == nullptr || model_uniform_buffer_ == nullptr) {
    return false;
  }
  if (gpu.uploaded_revision == layer->modelRevision()) {
    return true;
  }
  releaseModelLayerGpu(gpu);
  const int max_texture_size = owner->resourceLimit(QRhi::TextureSizeMax);
  // Decoded QImages and their QRhi textures are retained across the whole
  // layer, not just one mesh. Enforce both envelopes before QImage allocation.
  std::uint64_t remaining_layer_texture_pixels = kBrowserMaxModelTexturePixelsPerLayer;
  for (const auto& [key, source] : layer->modelMeshes()) {
    if (source == nullptr || !source->ok || source->vertices.empty() || source->indices.empty()) {
      continue;
    }
    ModelMeshGpu mesh;
    mesh.source = source;
    const std::uint64_t vertex_bytes = source->vertices.size() * sizeof(pj::scene3d::Vertex);
    const std::uint64_t index_bytes = source->indices.size() * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<quint32>::max() || index_bytes > std::numeric_limits<quint32>::max()) {
      layer->noteRenderFailure(tr("A model GPU buffer exceeds the browser addressable limit"));
      releaseModelLayerGpu(gpu);
      return false;
    }
    mesh.vertex_buffer =
        owner->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(vertex_bytes));
    mesh.triangle_index_buffer =
        owner->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(index_bytes));

    mesh.edge_indices.reserve(source->indices.size() * 2U);
    mesh.edge_offsets.reserve(source->submeshes.size());
    mesh.edge_counts.reserve(source->submeshes.size());
    for (const SubMesh& submesh : source->submeshes) {
      const quint32 first = static_cast<quint32>(mesh.edge_indices.size());
      const std::size_t end = std::min(source->indices.size(), submesh.index_offset + submesh.index_count);
      for (std::size_t index = submesh.index_offset; index + 2U < end; index += 3U) {
        const quint32 a = source->indices[index];
        const quint32 b = source->indices[index + 1U];
        const quint32 c = source->indices[index + 2U];
        mesh.edge_indices.insert(mesh.edge_indices.end(), {a, b, b, c, c, a});
      }
      mesh.edge_offsets.push_back(first);
      mesh.edge_counts.push_back(static_cast<quint32>(mesh.edge_indices.size()) - first);
    }
    if (!mesh.edge_indices.empty()) {
      const std::uint64_t edge_bytes = mesh.edge_indices.size() * sizeof(quint32);
      if (edge_bytes > std::numeric_limits<quint32>::max()) {
        layer->noteRenderFailure(tr("A model wireframe buffer exceeds the browser addressable limit"));
        releaseModelMeshGpu(mesh);
        releaseModelLayerGpu(gpu);
        return false;
      }
      mesh.edge_index_buffer =
          owner->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(edge_bytes));
    }
    if (!mesh.vertex_buffer->create() || !mesh.triangle_index_buffer->create() ||
        (mesh.edge_index_buffer != nullptr && !mesh.edge_index_buffer->create())) {
      layer->noteRenderFailure(tr("Could not allocate model GPU buffers"));
      releaseModelMeshGpu(mesh);
      releaseModelLayerGpu(gpu);
      return false;
    }

    // Decode every distinct material slot against temporary mesh/layer budgets
    // before allocating any texture. A late fifth-slot rejection therefore
    // cannot leave a partially committed material or consume the layer budget.
    struct DecodedTexture {
      QImage image;
      std::size_t slot = 0U;
    };
    std::unordered_map<const TextureSource*, DecodedTexture> decoded_textures;
    std::uint64_t remaining_texture_pixels = kBrowserMaxModelTexturePixelsPerMesh;
    std::uint64_t candidate_layer_texture_pixels = remaining_layer_texture_pixels;
    for (const SubMesh& submesh : source->submeshes) {
      if (submesh.material == nullptr) {
        continue;
      }
      const auto textures = modelTextureSources(*submesh.material);
      for (std::size_t slot = 0; slot < textures.size(); ++slot) {
        const TextureSource* texture = textures[slot];
        if (texture == nullptr || texture->empty() || decoded_textures.contains(texture)) {
          continue;
        }
        QImage image;
        QSize size;
        if (!texture->bytes.empty()) {
          QByteArray encoded(
              reinterpret_cast<const char*>(texture->bytes.data()), static_cast<qsizetype>(texture->bytes.size()));
          QBuffer buffer(&encoded);
          buffer.open(QIODevice::ReadOnly);
          QImageReader reader(&buffer);
          size = reader.size();
          if (size.isValid()) {
            const std::uint64_t pixels =
                static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
            if (size.width() > max_texture_size || size.height() > max_texture_size ||
                !tryConsumeBrowserModelTexturePixels(
                    pixels, remaining_texture_pixels, candidate_layer_texture_pixels)) {
              layer->noteRenderFailure(tr("A model exceeds the browser decoded-texture limit"));
              releaseModelMeshGpu(mesh);
              releaseModelLayerGpu(gpu);
              return false;
            }
            image = reader.read();
          }
        } else {
          QImageReader reader(texture->path);
          size = reader.size();
          if (size.isValid()) {
            const std::uint64_t pixels =
                static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
            if (size.width() > max_texture_size || size.height() > max_texture_size ||
                !tryConsumeBrowserModelTexturePixels(
                    pixels, remaining_texture_pixels, candidate_layer_texture_pixels)) {
              layer->noteRenderFailure(tr("A model exceeds the browser decoded-texture limit"));
              releaseModelMeshGpu(mesh);
              releaseModelLayerGpu(gpu);
              return false;
            }
            image = reader.read();
          }
        }
        if (!size.isValid() || image.isNull()) {
          layer->noteRenderFailure(tr("Could not decode a model material texture"));
          releaseModelMeshGpu(mesh);
          releaseModelLayerGpu(gpu);
          return false;
        }
        decoded_textures.emplace(texture, DecodedTexture{image.convertToFormat(QImage::Format_RGBA8888), slot});
      }
    }
    remaining_layer_texture_pixels = candidate_layer_texture_pixels;

    mesh.materials.resize(source->submeshes.size());
    std::unordered_map<const TextureSource*, QRhiTexture*> shared_textures;
    for (std::size_t index = 0; index < source->submeshes.size(); ++index) {
      const std::shared_ptr<const Material>& material = source->submeshes[index].material;
      ModelMaterialGpu& material_gpu = mesh.materials[index];
      const Material fallback;
      const Material& resolved_material = material != nullptr ? *material : fallback;
      const auto textures = modelTextureSources(resolved_material);
      for (std::size_t slot = 0; slot < textures.size(); ++slot) {
        const TextureSource* texture = textures[slot];
        ModelTextureGpu& texture_gpu = material_gpu.textures[slot];
        texture_gpu.texture = model_white_texture_;
        if (texture == nullptr || texture->empty()) {
          continue;
        }
        texture_gpu.present = true;
        if (const auto shared = shared_textures.find(texture); shared != shared_textures.end()) {
          texture_gpu.texture = shared->second;
          continue;
        }
        auto decoded = decoded_textures.find(texture);
        if (decoded == decoded_textures.end() || decoded->second.image.isNull() || decoded->second.slot != slot) {
          layer->noteRenderFailure(tr("A model material texture lost its decoded upload state"));
          releaseModelMeshGpu(mesh);
          releaseModelLayerGpu(gpu);
          return false;
        }
        const QRhiTexture::Flags flags = modelTextureIsSrgb(slot) ? QRhiTexture::sRGB : QRhiTexture::Flags{};
        texture_gpu.texture = owner->newTexture(QRhiTexture::RGBA8, decoded->second.image.size(), 1, flags);
        texture_gpu.owns_texture = true;
        if (texture_gpu.texture == nullptr || !texture_gpu.texture->create()) {
          layer->noteRenderFailure(tr("Could not allocate a model material texture"));
          releaseModelMeshGpu(mesh);
          releaseModelLayerGpu(gpu);
          return false;
        }
        texture_gpu.pending_image = std::move(decoded->second.image);
        texture_gpu.texture_upload_pending = true;
        shared_textures.emplace(texture, texture_gpu.texture);
      }
      if (!rebuildModelMaterialBindings(owner, material_gpu)) {
        layer->noteRenderFailure(tr("Could not create model material bindings"));
        releaseModelMeshGpu(mesh);
        releaseModelLayerGpu(gpu);
        return false;
      }
    }
    mesh.upload_pending = true;
    gpu.meshes.emplace(key, std::move(mesh));
  }
  gpu.uploaded_revision = layer->modelRevision();
  return true;
}

void SceneViewWidget::initialize(QRhiCommandBuffer* /*command_buffer*/) {
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr || renderTarget() == nullptr) {
    return;
  }
  if (resource_rhi_ != current_rhi) {
    releaseResources();
    resource_rhi_ = current_rhi;
    max_3d_texture_size_ = max3dTextureSize(*current_rhi);
    PJ::logGraphicsBackend(*this, current_rhi);
  }
  if (line_pipeline_ != nullptr && triangle_pipeline_ != nullptr && line_no_depth_pipeline_ != nullptr &&
      triangle_no_depth_pipeline_ != nullptr && marker_triangle_pipeline_ != nullptr &&
      marker_triangle_no_depth_pipeline_ != nullptr && marker_triangle_cull_pipeline_ != nullptr &&
      marker_triangle_cull_no_depth_pipeline_ != nullptr && marker_line_pipeline_ != nullptr &&
      marker_line_no_depth_pipeline_ != nullptr && model_triangle_pipeline_ != nullptr &&
      model_triangle_no_depth_pipeline_ != nullptr && model_line_pipeline_ != nullptr &&
      model_line_no_depth_pipeline_ != nullptr && model_layout_shader_resources_ != nullptr &&
      render_mode_uniform_buffer_ != nullptr && annotation_render_mode_uniform_buffer_ != nullptr &&
      annotation_shader_resources_ != nullptr && model_uniform_buffer_ != nullptr && model_white_texture_ != nullptr &&
      model_sampler_ != nullptr && point_pipeline_ != nullptr && cube_pipeline_ != nullptr &&
      pose_pipeline_ != nullptr && occupancy_pipeline_ != nullptr &&
      (!current_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures) || voxel_pipeline_ != nullptr)) {
    return;
  }

  const QShader vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/scene.vert.qsb"));
  const QShader fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/scene.frag.qsb"));
  const QShader point_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/point.vert.qsb"));
  const QShader point_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/point.frag.qsb"));
  const QShader cube_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/cube.vert.qsb"));
  const QShader cube_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/cube.frag.qsb"));
  const QShader pose_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/pose.vert.qsb"));
  const QShader pose_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/pose.frag.qsb"));
  const QShader occupancy_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/occupancy.vert.qsb"));
  const QShader occupancy_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/occupancy.frag.qsb"));
  const QShader voxel_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/voxel.vert.qsb"));
  const QShader voxel_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/voxel.frag.qsb"));
  const QShader marker_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/marker.vert.qsb"));
  const QShader marker_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/marker.frag.qsb"));
  const QShader model_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/model.vert.qsb"));
  const QShader model_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/model.frag.qsb"));
  if (!vertex_shader.isValid() || !fragment_shader.isValid() || !point_vertex_shader.isValid() ||
      !point_fragment_shader.isValid() || !cube_vertex_shader.isValid() || !cube_fragment_shader.isValid() ||
      !pose_vertex_shader.isValid() || !pose_fragment_shader.isValid() || !occupancy_vertex_shader.isValid() ||
      !occupancy_fragment_shader.isValid() || !voxel_vertex_shader.isValid() || !voxel_fragment_shader.isValid() ||
      !marker_vertex_shader.isValid() || !marker_fragment_shader.isValid() || !model_vertex_shader.isValid() ||
      !model_fragment_shader.isValid()) {
    return;
  }

  uniform_buffer_ = current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64);
  render_mode_uniform_buffer_ =
      current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kRenderModeUniformBytes);
  annotation_render_mode_uniform_buffer_ =
      current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kRenderModeUniformBytes);
  if (!uniform_buffer_->create() || !render_mode_uniform_buffer_->create() ||
      !annotation_render_mode_uniform_buffer_->create()) {
    releaseResources();
    return;
  }
  shader_resources_ = current_rhi->newShaderResourceBindings();
  shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  annotation_shader_resources_ = current_rhi->newShaderResourceBindings();
  annotation_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, annotation_render_mode_uniform_buffer_),
  });
  if (!shader_resources_->create() || !annotation_shader_resources_->create()) {
    releaseResources();
    return;
  }

  model_uniform_buffer_ = current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kModelUniformBytes);
  model_white_texture_ = current_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
  model_sampler_ = current_rhi->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::Repeat, QRhiSampler::Repeat);
  if (!model_uniform_buffer_->create() || !model_white_texture_->create() || !model_sampler_->create()) {
    releaseResources();
    return;
  }
  model_layout_shader_resources_ = current_rhi->newShaderResourceBindings();
  model_layout_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          2, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          3, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          4, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          5, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(6, QRhiShaderResourceBinding::FragmentStage, model_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          7, QRhiShaderResourceBinding::FragmentStage, model_white_texture_, model_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  if (!model_layout_shader_resources_->create()) {
    releaseResources();
    return;
  }
  model_white_upload_pending_ = true;

  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(Vertex, r)),
  });
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

  const auto create_pipeline = [&](QRhiGraphicsPipeline::Topology topology, bool depth_write) {
    QRhiGraphicsPipeline* pipeline = current_rhi->newGraphicsPipeline();
    pipeline->setTopology(topology);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader},
        {QRhiShaderStage::Fragment, fragment_shader},
    });
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(shader_resources_);
    pipeline->setTargetBlends({blend});
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(depth_write);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setSampleCount(sampleCount());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create()) {
      delete pipeline;
      return static_cast<QRhiGraphicsPipeline*>(nullptr);
    }
    return pipeline;
  };
  line_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Lines, true);
  triangle_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Triangles, true);
  line_no_depth_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Lines, false);
  triangle_no_depth_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Triangles, false);

  static_assert(sizeof(MarkerInstance) == 96U);
  static_assert(offsetof(MarkerInstance, r) == 64U);
  static_assert(offsetof(MarkerInstance, bottom_scale) == 80U);
  QRhiVertexInputLayout marker_layout;
  marker_layout.setBindings({
      QRhiVertexInputBinding(sizeof(CubeVertex)),
      QRhiVertexInputBinding(sizeof(MarkerInstance), QRhiVertexInputBinding::PerInstance),
  });
  marker_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)),
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, 0U),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float4, 16U),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float4, 32U),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 48U),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 64U),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 80U),
  });
  const auto create_marker_pipeline = [&](QRhiGraphicsPipeline::Topology topology, QRhiGraphicsPipeline::CullMode cull,
                                          bool depth_write) {
    QRhiGraphicsPipeline* pipeline = current_rhi->newGraphicsPipeline();
    pipeline->setTopology(topology);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, marker_vertex_shader},
        {QRhiShaderStage::Fragment, marker_fragment_shader},
    });
    pipeline->setVertexInputLayout(marker_layout);
    pipeline->setShaderResourceBindings(shader_resources_);
    pipeline->setTargetBlends({blend});
    pipeline->setCullMode(cull);
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(depth_write);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setSampleCount(sampleCount());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create()) {
      delete pipeline;
      return static_cast<QRhiGraphicsPipeline*>(nullptr);
    }
    return pipeline;
  };
  marker_triangle_pipeline_ = create_marker_pipeline(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::None, true);
  marker_triangle_no_depth_pipeline_ =
      create_marker_pipeline(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::None, false);
  marker_triangle_cull_pipeline_ =
      create_marker_pipeline(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back, true);
  marker_triangle_cull_no_depth_pipeline_ =
      create_marker_pipeline(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back, false);
  marker_line_pipeline_ = create_marker_pipeline(QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::None, true);
  marker_line_no_depth_pipeline_ =
      create_marker_pipeline(QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::None, false);

  static_assert(sizeof(ModelInstance) == 144U);
  static_assert(offsetof(ModelInstance, tint) == 64U);
  static_assert(offsetof(ModelInstance, params) == 80U);
  static_assert(offsetof(ModelInstance, texture_flags) == 96U);
  static_assert(offsetof(ModelInstance, pbr_factors) == 112U);
  static_assert(offsetof(ModelInstance, emissive_factor) == 128U);
  QRhiVertexInputLayout model_layout;
  model_layout.setBindings({
      QRhiVertexInputBinding(sizeof(pj::scene3d::Vertex)),
      QRhiVertexInputBinding(sizeof(ModelInstance), QRhiVertexInputBinding::PerInstance),
  });
  model_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, position)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, normal)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, offsetof(pj::scene3d::Vertex, color)),
      QRhiVertexInputAttribute(0, 3, QRhiVertexInputAttribute::Float2, offsetof(pj::scene3d::Vertex, uv)),
      QRhiVertexInputAttribute(0, 4, QRhiVertexInputAttribute::Float4, offsetof(pj::scene3d::Vertex, tangent)),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 0U),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 16U),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 32U),
      QRhiVertexInputAttribute(1, 8, QRhiVertexInputAttribute::Float4, 48U),
      QRhiVertexInputAttribute(1, 9, QRhiVertexInputAttribute::Float4, 64U),
      QRhiVertexInputAttribute(1, 10, QRhiVertexInputAttribute::Float4, 80U),
      QRhiVertexInputAttribute(1, 11, QRhiVertexInputAttribute::Float4, 96U),
      QRhiVertexInputAttribute(1, 12, QRhiVertexInputAttribute::Float4, 112U),
      QRhiVertexInputAttribute(1, 13, QRhiVertexInputAttribute::Float4, 128U),
  });
  const auto create_model_pipeline = [&](QRhiGraphicsPipeline::Topology topology, bool depth_write) {
    QRhiGraphicsPipeline* pipeline = current_rhi->newGraphicsPipeline();
    pipeline->setTopology(topology);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, model_vertex_shader},
        {QRhiShaderStage::Fragment, model_fragment_shader},
    });
    pipeline->setVertexInputLayout(model_layout);
    pipeline->setShaderResourceBindings(model_layout_shader_resources_);
    pipeline->setTargetBlends({blend});
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(depth_write);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setSampleCount(sampleCount());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create()) {
      delete pipeline;
      return static_cast<QRhiGraphicsPipeline*>(nullptr);
    }
    return pipeline;
  };
  model_triangle_pipeline_ = create_model_pipeline(QRhiGraphicsPipeline::Triangles, true);
  model_triangle_no_depth_pipeline_ = create_model_pipeline(QRhiGraphicsPipeline::Triangles, false);
  model_line_pipeline_ = create_model_pipeline(QRhiGraphicsPipeline::Lines, true);
  model_line_no_depth_pipeline_ = create_model_pipeline(QRhiGraphicsPipeline::Lines, false);

  const auto& marker_mesh_data = markerMeshes();
  for (std::size_t index = 0; index < marker_mesh_data.size(); ++index) {
    const MarkerMeshData& data = marker_mesh_data[index];
    MarkerMeshGpu& gpu = marker_meshes_[index];
    gpu.vertex_buffer = current_rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
        static_cast<quint32>(data.vertices.size() * sizeof(CubeVertex)));
    if (!data.triangle_indices.empty()) {
      gpu.triangle_index_buffer = current_rhi->newBuffer(
          QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
          static_cast<quint32>(data.triangle_indices.size() * sizeof(std::uint32_t)));
    }
    if (!data.edge_indices.empty()) {
      gpu.edge_index_buffer = current_rhi->newBuffer(
          QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
          static_cast<quint32>(data.edge_indices.size() * sizeof(std::uint32_t)));
    }
    if (!gpu.vertex_buffer->create() ||
        (gpu.triangle_index_buffer != nullptr && !gpu.triangle_index_buffer->create()) ||
        (gpu.edge_index_buffer != nullptr && !gpu.edge_index_buffer->create())) {
      releaseResources();
      return;
    }
    gpu.triangle_index_count = static_cast<quint32>(data.triangle_indices.size());
    gpu.edge_index_count = static_cast<quint32>(data.edge_indices.size());
  }
  marker_mesh_upload_pending_ = true;

  colormap_texture_ = current_rhi->newTexture(QRhiTexture::RGBA8, QSize(PJ::kColormapLutWidth, PJ::kColormapCount));
  colormap_sampler_ = current_rhi->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
  if (!colormap_texture_->create() || !colormap_sampler_->create()) {
    releaseResources();
    return;
  }
  colormap_upload_pending_ = true;

  QRhiVertexInputLayout point_layout;
  point_layout.setBindings({QRhiVertexInputBinding(sizeof(WasmPointVertex))});
  point_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(WasmPointVertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float, offsetof(WasmPointVertex, scalar)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, offsetof(WasmPointVertex, rgba)),
  });
  point_layout_uniform_buffer_ =
      current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kPointUniformBytes);
  if (!point_layout_uniform_buffer_->create()) {
    releaseResources();
    return;
  }
  point_layout_shader_resources_ = current_rhi->newShaderResourceBindings();
  point_layout_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
          point_layout_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, colormap_texture_, colormap_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  if (!point_layout_shader_resources_->create()) {
    releaseResources();
    return;
  }
  point_pipeline_ = current_rhi->newGraphicsPipeline();
  point_pipeline_->setTopology(QRhiGraphicsPipeline::Points);
  point_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, point_vertex_shader},
      {QRhiShaderStage::Fragment, point_fragment_shader},
  });
  point_pipeline_->setVertexInputLayout(point_layout);
  point_pipeline_->setShaderResourceBindings(point_layout_shader_resources_);
  point_pipeline_->setTargetBlends({blend});
  point_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  point_pipeline_->setDepthTest(true);
  point_pipeline_->setDepthWrite(true);
  point_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  point_pipeline_->setSampleCount(sampleCount());
  point_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!point_pipeline_->create()) {
    delete point_pipeline_;
    point_pipeline_ = nullptr;
  }

  cube_vertex_buffer_ = current_rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(kCubeVertices.size() * sizeof(CubeVertex)));
  cube_index_buffer_ = current_rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
      static_cast<quint32>(kCubeIndices.size() * sizeof(std::uint16_t)));
  if (!cube_vertex_buffer_->create() || !cube_index_buffer_->create()) {
    releaseResources();
    return;
  }
  cube_upload_pending_ = true;

  QRhiVertexInputLayout cube_layout;
  cube_layout.setBindings({
      QRhiVertexInputBinding(sizeof(CubeVertex)),
      QRhiVertexInputBinding(sizeof(WasmPointVertex), QRhiVertexInputBinding::PerInstance),
  });
  cube_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)),
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float3, offsetof(WasmPointVertex, x)),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float, offsetof(WasmPointVertex, scalar)),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::UNormByte4, offsetof(WasmPointVertex, rgba)),
  });
  cube_pipeline_ = current_rhi->newGraphicsPipeline();
  cube_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  cube_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, cube_vertex_shader},
      {QRhiShaderStage::Fragment, cube_fragment_shader},
  });
  cube_pipeline_->setVertexInputLayout(cube_layout);
  cube_pipeline_->setShaderResourceBindings(point_layout_shader_resources_);
  cube_pipeline_->setTargetBlends({blend});
  cube_pipeline_->setCullMode(QRhiGraphicsPipeline::Back);
  cube_pipeline_->setDepthTest(true);
  cube_pipeline_->setDepthWrite(true);
  cube_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  cube_pipeline_->setSampleCount(sampleCount());
  cube_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!cube_pipeline_->create()) {
    delete cube_pipeline_;
    cube_pipeline_ = nullptr;
  }
  const ArrowMeshData& pose_mesh = unitPoseArrowMesh();
  pose_vertex_buffer_ = current_rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(pose_mesh.vertices.size() * sizeof(float)));
  pose_index_buffer_ = current_rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
      static_cast<quint32>(pose_mesh.indices.size() * sizeof(std::uint32_t)));
  if (!pose_vertex_buffer_->create() || !pose_index_buffer_->create()) {
    releaseResources();
    return;
  }
  pose_index_count_ = static_cast<quint32>(pose_mesh.indices.size());
  pose_mesh_upload_pending_ = true;

  pose_layout_uniform_buffer_ =
      current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kPoseUniformBytes);
  if (!pose_layout_uniform_buffer_->create()) {
    releaseResources();
    return;
  }
  pose_layout_shader_resources_ = current_rhi->newShaderResourceBindings();
  pose_layout_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, pose_layout_uniform_buffer_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, annotation_render_mode_uniform_buffer_),
  });
  if (!pose_layout_shader_resources_->create()) {
    releaseResources();
    return;
  }

  static_assert(sizeof(PoseTriadInstance) == 80U);
  static_assert(offsetof(PoseTriadInstance, color) == 64U);
  QRhiVertexInputLayout pose_layout;
  pose_layout.setBindings({
      QRhiVertexInputBinding(6U * sizeof(float)),
      QRhiVertexInputBinding(sizeof(PoseTriadInstance), QRhiVertexInputBinding::PerInstance),
  });
  pose_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3U * sizeof(float)),
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, 0U),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float4, 16U),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float4, 32U),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 48U),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 64U),
  });
  QRhiGraphicsPipeline::TargetBlend pose_blend = blend;
  pose_blend.srcAlpha = QRhiGraphicsPipeline::Zero;
  pose_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  pose_pipeline_ = current_rhi->newGraphicsPipeline();
  pose_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  pose_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, pose_vertex_shader},
      {QRhiShaderStage::Fragment, pose_fragment_shader},
  });
  pose_pipeline_->setVertexInputLayout(pose_layout);
  pose_pipeline_->setShaderResourceBindings(pose_layout_shader_resources_);
  pose_pipeline_->setTargetBlends({pose_blend});
  pose_pipeline_->setCullMode(QRhiGraphicsPipeline::Back);
  pose_pipeline_->setDepthTest(true);
  pose_pipeline_->setDepthWrite(true);
  pose_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  pose_pipeline_->setSampleCount(sampleCount());
  pose_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!pose_pipeline_->create()) {
    delete pose_pipeline_;
    pose_pipeline_ = nullptr;
  }

  occupancy_layout_uniform_buffer_ =
      current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kOccupancyUniformBytes);
  occupancy_layout_texture_ = current_rhi->newTexture(QRhiTexture::R8, QSize(1, 1));
  occupancy_sampler_ = current_rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  occupancy_quad_buffer_ = current_rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(kOccupancyQuad.size() * sizeof(float)));
  if (!occupancy_layout_uniform_buffer_->create() || !occupancy_layout_texture_->create() ||
      !occupancy_sampler_->create() || !occupancy_quad_buffer_->create()) {
    releaseResources();
    return;
  }
  occupancy_quad_upload_pending_ = true;
  occupancy_layout_shader_resources_ = current_rhi->newShaderResourceBindings();
  occupancy_layout_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
          occupancy_layout_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, occupancy_layout_texture_, occupancy_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  if (!occupancy_layout_shader_resources_->create()) {
    releaseResources();
    return;
  }
  QRhiVertexInputLayout occupancy_layout;
  occupancy_layout.setBindings({QRhiVertexInputBinding(2U * sizeof(float))});
  occupancy_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0U),
  });
  occupancy_pipeline_ = current_rhi->newGraphicsPipeline();
  occupancy_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  occupancy_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, occupancy_vertex_shader},
      {QRhiShaderStage::Fragment, occupancy_fragment_shader},
  });
  occupancy_pipeline_->setVertexInputLayout(occupancy_layout);
  occupancy_pipeline_->setShaderResourceBindings(occupancy_layout_shader_resources_);
  occupancy_pipeline_->setTargetBlends({blend});
  occupancy_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  occupancy_pipeline_->setDepthTest(true);
  occupancy_pipeline_->setDepthWrite(false);
  occupancy_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  occupancy_pipeline_->setDepthBias(-1);
  occupancy_pipeline_->setSampleCount(sampleCount());
  occupancy_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!occupancy_pipeline_->create()) {
    delete occupancy_pipeline_;
    occupancy_pipeline_ = nullptr;
  }

  if (current_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
    voxel_layout_uniform_buffer_ =
        current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kVoxelUniformBytes);
    voxel_layout_texture_ = current_rhi->newTexture(QRhiTexture::R32F, 1, 1, 1, 1, QRhiTexture::ThreeDimensional);
    voxel_sampler_ = current_rhi->newSampler(
        QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!voxel_layout_uniform_buffer_->create() || !voxel_layout_texture_->create() || !voxel_sampler_->create()) {
      releaseResources();
      return;
    }
    voxel_layout_shader_resources_ = current_rhi->newShaderResourceBindings();
    voxel_layout_shader_resources_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            voxel_layout_uniform_buffer_),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::VertexStage, voxel_layout_texture_, voxel_sampler_),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, colormap_texture_, colormap_sampler_),
        QRhiShaderResourceBinding::uniformBuffer(
            8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
    });
    if (!voxel_layout_shader_resources_->create()) {
      releaseResources();
      return;
    }
    QRhiVertexInputLayout voxel_layout;
    voxel_layout.setBindings({QRhiVertexInputBinding(sizeof(CubeVertex))});
    voxel_layout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)),
    });
    voxel_pipeline_ = current_rhi->newGraphicsPipeline();
    voxel_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    voxel_pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, voxel_vertex_shader},
        {QRhiShaderStage::Fragment, voxel_fragment_shader},
    });
    voxel_pipeline_->setVertexInputLayout(voxel_layout);
    voxel_pipeline_->setShaderResourceBindings(voxel_layout_shader_resources_);
    voxel_pipeline_->setTargetBlends({blend});
    voxel_pipeline_->setCullMode(QRhiGraphicsPipeline::Back);
    voxel_pipeline_->setDepthTest(true);
    voxel_pipeline_->setDepthWrite(true);
    voxel_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    voxel_pipeline_->setSampleCount(sampleCount());
    voxel_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!voxel_pipeline_->create()) {
      delete voxel_pipeline_;
      voxel_pipeline_ = nullptr;
    }
  }
  if (line_pipeline_ == nullptr || triangle_pipeline_ == nullptr || line_no_depth_pipeline_ == nullptr ||
      triangle_no_depth_pipeline_ == nullptr || marker_triangle_pipeline_ == nullptr ||
      marker_triangle_no_depth_pipeline_ == nullptr || marker_triangle_cull_pipeline_ == nullptr ||
      marker_triangle_cull_no_depth_pipeline_ == nullptr || marker_line_pipeline_ == nullptr ||
      marker_line_no_depth_pipeline_ == nullptr || model_triangle_pipeline_ == nullptr ||
      model_triangle_no_depth_pipeline_ == nullptr || model_line_pipeline_ == nullptr ||
      model_line_no_depth_pipeline_ == nullptr || model_layout_shader_resources_ == nullptr ||
      model_white_texture_ == nullptr || model_sampler_ == nullptr || point_pipeline_ == nullptr ||
      cube_pipeline_ == nullptr || pose_pipeline_ == nullptr || occupancy_pipeline_ == nullptr ||
      (current_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures) && voxel_pipeline_ == nullptr)) {
    releaseResources();
  }
}

void SceneViewWidget::render(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  QRhiRenderTarget* target = renderTarget();
  if (current_rhi == nullptr || target == nullptr) {
    return;
  }

  const QSize output_size = target->pixelSize();
  const bool hdr_active = ensureHdrResources(current_rhi, output_size);
  hdr_stage_.last_active = hdr_active;
  bool ssao_active = hdr_active && ensureSsaoResources(current_rhi, output_size);
  ssao_stage_.last_active = ssao_active;
  bool edl_active = hdr_active && ensureEdlResources(current_rhi, output_size);
  edl_stage_.last_active = edl_active;
  const float aspect = output_size.height() > 0
                           ? static_cast<float>(output_size.width()) / static_cast<float>(output_size.height())
                           : 1.0F;

  struct PreparedPointLayer {
    WasmPointRenderable* layer = nullptr;
    Transform fixed_from_source;
  };
  std::vector<PreparedPointLayer> prepared_points;
  prepared_points.reserve(point_layers_.size());
  std::vector<WasmPointRenderable*> fitted_point_layers;
  AABB fixed_point_bounds;
  AABB fixed_depth_bounds;
  const std::string fixed_frame = effectiveFixedFrame();
  std::uint64_t remaining_point_vertices = kMaxPointVerticesPerView;
  for (WasmPointRenderable* layer : point_layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining_point_vertices);
    if (layer->vertices().empty()) {
      if (auto gpu = point_layer_gpu_.find(layer); gpu != point_layer_gpu_.end()) {
        releasePointLayerGpu(gpu->second);
        point_layer_gpu_.erase(gpu);
      }
      continue;
    }
    const auto placed = resolveFixedFromSource(layer->sourceFrame(), fixed_frame);
    if (!placed.has_value()) {
      continue;
    }
    const Transform& fixed_from_source = *placed;
    // The aggregate applies to actual drawable point-like layers. A temporarily
    // unplaceable point/depth layer must not reserve capacity ahead of a later
    // layer whose source frame resolves in this frame.
    if (!tryConsumeBrowserPointVertices(
            static_cast<std::uint64_t>(layer->vertices().size()), remaining_point_vertices)) {
      continue;  // prepareForRender() normally clears this; keep subtraction fail-closed.
    }
    if (const auto bounds = layer->sourceBounds(); bounds.has_value()) {
      AABB& family_bounds = layer->isDepthCloud() ? fixed_depth_bounds : fixed_point_bounds;
      family_bounds = unionAABB(family_bounds, transformedBounds(*bounds, fixed_from_source));
      if (point_layers_pending_fit_.contains(layer)) {
        fitted_point_layers.push_back(layer);
      }
    }
    prepared_points.push_back(PreparedPointLayer{layer, fixed_from_source});
  }
  last_point_bounds_ = fixed_point_bounds;
  last_depth_bounds_ = fixed_depth_bounds;

  struct PreparedPoseLayer {
    WasmPosesInFrameLayer* layer = nullptr;
    Transform fixed_from_source;
  };
  std::vector<PreparedPoseLayer> prepared_poses;
  prepared_poses.reserve(pose_layers_.size());
  std::vector<WasmPosesInFrameLayer*> fitted_pose_layers;
  AABB fixed_pose_bounds;
  std::uint64_t remaining_pose_arms = kMaxPoseArmsPerView;
  for (WasmPosesInFrameLayer* layer : pose_layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining_pose_arms);
    if (layer->instances().empty()) {
      if (auto gpu = pose_layer_gpu_.find(layer); gpu != pose_layer_gpu_.end()) {
        releasePoseLayerGpu(gpu->second);
        pose_layer_gpu_.erase(gpu);
      }
      continue;
    }
    const auto placed = resolveFixedFromSource(layer->sourceFrame(), fixed_frame);
    if (!placed.has_value()) {
      continue;
    }
    const Transform& fixed_from_source = *placed;
    // Placement first, budget second: a temporarily unplaceable layer must not
    // reserve capacity and starve a later drawable layer of the same family.
    if (!tryConsumeBrowserPoseArms(static_cast<std::uint64_t>(layer->instances().size()), remaining_pose_arms)) {
      continue;
    }
    if (const auto bounds = layer->sourceBounds(); bounds.has_value()) {
      fixed_pose_bounds = unionAABB(fixed_pose_bounds, transformedBounds(*bounds, fixed_from_source));
      if (pose_layers_pending_fit_.contains(layer)) {
        fitted_pose_layers.push_back(layer);
      }
    }
    prepared_poses.push_back(PreparedPoseLayer{layer, fixed_from_source});
  }
  last_pose_bounds_ = fixed_pose_bounds;

  struct PreparedOccupancyLayer {
    WasmOccupancyGridLayer* layer = nullptr;
    Transform fixed_from_source;
  };
  std::vector<PreparedOccupancyLayer> prepared_occupancy;
  prepared_occupancy.reserve(occupancy_layers_.size());
  std::vector<WasmOccupancyGridLayer*> fitted_occupancy_layers;
  AABB fixed_occupancy_bounds;
  std::uint64_t remaining_occupancy_cells = kMaxOccupancyCellsPerView;
  for (WasmOccupancyGridLayer* layer : occupancy_layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining_occupancy_cells);
    const ReconstructedGrid& grid = layer->grid();
    if (grid.empty()) {
      if (auto gpu = occupancy_layer_gpu_.find(layer); gpu != occupancy_layer_gpu_.end()) {
        releaseOccupancyLayerGpu(gpu->second);
        occupancy_layer_gpu_.erase(gpu);
      }
      continue;
    }
    const auto placed = resolveFixedFromSource(layer->sourceFrame(), fixed_frame);
    if (!placed.has_value()) {
      continue;
    }
    const Transform& fixed_from_source = *placed;
    // Placement first, budget second: a temporarily unplaceable layer must not
    // reserve capacity and starve a later drawable layer of the same family.
    const std::uint64_t cells = static_cast<std::uint64_t>(grid.width) * grid.height;
    if (!tryConsumeBrowserOccupancyCells(cells, remaining_occupancy_cells)) {
      continue;
    }
    const AABB bounds = occupancyBoundsInFixed(grid, fixed_from_source);
    if (bounds.valid) {
      fixed_occupancy_bounds = unionAABB(fixed_occupancy_bounds, bounds);
      if (occupancy_layers_pending_fit_.contains(layer)) {
        fitted_occupancy_layers.push_back(layer);
      }
    }
    prepared_occupancy.push_back(PreparedOccupancyLayer{layer, fixed_from_source});
  }
  last_occupancy_bounds_ = fixed_occupancy_bounds;

  struct PreparedVoxelLayer {
    WasmVoxelGridLayer* layer = nullptr;
    Transform fixed_from_source;
  };
  std::vector<PreparedVoxelLayer> prepared_voxels;
  prepared_voxels.reserve(voxel_layers_.size());
  std::vector<WasmVoxelGridLayer*> fitted_voxel_layers;
  AABB fixed_voxel_bounds;
  std::uint64_t remaining_voxels = kMaxVoxelsPerView;
  for (WasmVoxelGridLayer* layer : voxel_layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining_voxels);
    if (!layer->hasVolume()) {
      if (auto gpu = voxel_layer_gpu_.find(layer); gpu != voxel_layer_gpu_.end()) {
        releaseVoxelLayerGpu(gpu->second);
        voxel_layer_gpu_.erase(gpu);
      }
      continue;
    }
    const QString gpu_rejection = voxelGpuRejection(current_rhi, layer);
    if (!gpu_rejection.isEmpty()) {
      layer->rejectVolumeForRender(gpu_rejection);
      if (auto gpu = voxel_layer_gpu_.find(layer); gpu != voxel_layer_gpu_.end()) {
        releaseVoxelLayerGpu(gpu->second);
        voxel_layer_gpu_.erase(gpu);
      }
      continue;
    }
    const auto placed = resolveFixedFromSource(layer->sourceFrame(), fixed_frame);
    if (!placed.has_value()) {
      continue;
    }
    const Transform& fixed_from_source = *placed;
    // The view budget describes submitted/drawable voxels. An otherwise valid
    // layer whose frame cannot currently be placed must not reserve capacity
    // and starve a later layer that can be rendered in this frame.
    if (!tryConsumeBrowserVoxels(layer->voxelCount(), remaining_voxels)) {
      continue;
    }
    if (const auto bounds = layer->sourceBounds(); bounds.has_value()) {
      fixed_voxel_bounds = unionAABB(fixed_voxel_bounds, transformedBounds(*bounds, fixed_from_source));
      if (voxel_layers_pending_fit_.contains(layer)) {
        fitted_voxel_layers.push_back(layer);
      }
    }
    prepared_voxels.push_back(PreparedVoxelLayer{layer, fixed_from_source});
  }
  last_voxel_bounds_ = fixed_voxel_bounds;

  struct PreparedMarkerLayer {
    WasmSceneEntitiesLayer* layer = nullptr;
    std::vector<std::optional<Transform>> fixed_from_frames;
    std::uint64_t instances = 0;
    std::uint64_t stream_vertices = 0;
  };
  std::vector<PreparedMarkerLayer> prepared_markers;
  prepared_markers.reserve(marker_layers_.size());
  std::vector<WasmSceneEntitiesLayer*> fitted_marker_layers;
  AABB fixed_marker_bounds;
  std::uint64_t remaining_marker_instances = kMaxMarkerInstancesPerView;
  std::uint64_t remaining_marker_stream_vertices = kMaxMarkerStreamVerticesPerView;
  for (WasmSceneEntitiesLayer* layer : marker_layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender();
    const WasmMarkerGeometry& geometry = layer->geometry();
    if (geometry.empty() || fixed_frame.empty()) {
      continue;
    }
    PreparedMarkerLayer prepared;
    prepared.layer = layer;
    prepared.fixed_from_frames.resize(geometry.frames.size());
    AABB layer_marker_bounds;
    for (std::size_t index = 0; index < geometry.frames.size(); ++index) {
      const std::string& frame = geometry.frames[index];
      if (frame.empty()) {
        continue;
      }
      if (frame == fixed_frame) {
        prepared.fixed_from_frames[index] = Transform{};
      } else if (tf_ != nullptr) {
        if (auto transform = tf_->tryLookupTransform(fixed_frame, frame, render_time_); transform.has_value()) {
          prepared.fixed_from_frames[index] = *transform;
        }
      }
      if (prepared.fixed_from_frames[index].has_value() && index < geometry.frame_bounds.size()) {
        layer_marker_bounds = unionAABB(
            layer_marker_bounds,
            transformedMarkerBounds(geometry.frame_bounds[index], *prepared.fixed_from_frames[index]));
      }
    }
    const auto count_instances = [&prepared](const auto& instances) {
      return static_cast<std::uint64_t>(
          std::count_if(instances.cbegin(), instances.cend(), [&prepared](const auto& item) {
            return item.frame_index < prepared.fixed_from_frames.size() &&
                   prepared.fixed_from_frames[item.frame_index].has_value();
          }));
    };
    const std::uint64_t cube_instances = count_instances(geometry.cubes);
    const std::uint64_t logical_instances = cube_instances + count_instances(geometry.spheres) +
                                            count_instances(geometry.cylinders) + count_instances(geometry.arrows) +
                                            count_instances(geometry.axes);
    const auto submitted_instances =
        browserMarkerSubmittedInstanceCount(logical_instances, cube_instances, layer->wireframe());
    if (!submitted_instances.has_value()) {
      layer->noteRenderFailure(tr("Marker instance accounting overflowed the browser limit"));
      continue;
    }
    prepared.instances = *submitted_instances;
    for (const WasmMarkerStreamBatch& batch : geometry.lines) {
      if (batch.frame_index < prepared.fixed_from_frames.size() &&
          prepared.fixed_from_frames[batch.frame_index].has_value()) {
        prepared.stream_vertices += batch.vertices.size();
      }
    }
    for (const WasmMarkerStreamBatch& batch : geometry.triangles) {
      if (batch.frame_index < prepared.fixed_from_frames.size() &&
          prepared.fixed_from_frames[batch.frame_index].has_value()) {
        prepared.stream_vertices += batch.vertices.size() * (layer->wireframe() ? 2U : 1U);
      }
    }
    if (prepared.instances == 0U && prepared.stream_vertices == 0U) {
      continue;
    }
    if (!browserMarkerLayerCountsFit(prepared.instances, prepared.stream_vertices, geometry.frames.size())) {
      layer->noteRenderFailure(
          tr("Markers need %1 submitted instances and %2 streamed vertices; the browser layer limits are %3 and %4")
              .arg(
                  QString::number(prepared.instances), QString::number(prepared.stream_vertices),
                  QString::number(kBrowserMaxMarkerInstancesPerLayer),
                  QString::number(kBrowserMaxMarkerStreamVerticesPerLayer)));
      continue;
    }
    if (prepared.instances > remaining_marker_instances ||
        prepared.stream_vertices > remaining_marker_stream_vertices) {
      layer->noteRenderFailure(
          tr("Markers need %1 instances and %2 streamed vertices; only %3 and %4 remain in the browser view budget")
              .arg(
                  QString::number(prepared.instances), QString::number(prepared.stream_vertices),
                  QString::number(remaining_marker_instances), QString::number(remaining_marker_stream_vertices)));
      continue;
    }
    if (!tryConsumeBrowserMarkerInstances(prepared.instances, remaining_marker_instances) ||
        !tryConsumeBrowserMarkerStreamVertices(prepared.stream_vertices, remaining_marker_stream_vertices)) {
      continue;
    }
    layer->noteRenderSuccess();
    fixed_marker_bounds = unionAABB(fixed_marker_bounds, layer_marker_bounds);
    if (marker_layers_pending_fit_.contains(layer)) {
      fitted_marker_layers.push_back(layer);
    }
    prepared_markers.push_back(std::move(prepared));
  }
  last_marker_bounds_ = fixed_marker_bounds;

  struct PreparedModelCall {
    std::string mesh_key;
    std::shared_ptr<const MeshData> mesh;
    glm::dmat4 fixed_from_model{1.0};
    glm::vec4 override_color{1.0F};
    bool use_material = true;
    WasmModelDrawGroup group = WasmModelDrawGroup::kIndependent;
  };
  struct PreparedModelLayer {
    WasmModelRenderable* layer = nullptr;
    std::vector<PreparedModelCall> color_calls;
    std::vector<PreparedModelCall> shadow_calls;
    std::uint64_t draws = 0;
    std::uint64_t triangles = 0;
    std::uint64_t shadow_draws = 0;
    std::uint64_t shadow_triangles = 0;
  };
  std::vector<PreparedModelLayer> prepared_models;
  prepared_models.reserve(model_layers_.size());
  AABB fixed_model_bounds;
  std::uint64_t remaining_model_draws = kBrowserMaxModelDrawsPerView;
  std::uint64_t remaining_model_triangles = kBrowserMaxModelTrianglesPerView;
  std::uint64_t remaining_shadow_draws = kBrowserMaxModelDrawsPerView;
  std::uint64_t remaining_shadow_triangles = kBrowserMaxModelTrianglesPerView;
  std::unordered_map<const MeshData*, AABB> model_bounds_cache;
  for (WasmModelRenderable* layer : model_layers_) {
    if (layer == nullptr || !layer->visible() || fixed_frame.empty() || layer->modelDrawCalls().empty()) {
      continue;
    }
    PreparedModelLayer prepared;
    prepared.layer = layer;
    prepared.color_calls.reserve(layer->modelDrawCalls().size());
    prepared.shadow_calls.reserve(layer->modelDrawCalls().size());
    AABB layer_bounds;
    for (const WasmModelDrawCall& draw : layer->modelDrawCalls()) {
      const bool color_requested = (draw.group != WasmModelDrawGroup::kVisual ||
                                    (shading_params_.meshes_visible && shading_params_.mesh_opacity > 0.0F)) &&
                                   (draw.group != WasmModelDrawGroup::kCollision ||
                                    (shading_params_.collisions_visible && shading_params_.collision_opacity > 0.0F));
      // Native casts SceneEntities model primitives and RobotModel visuals. A
      // hidden Robot visual still casts there; collisions never do. Keep the
      // same intentionally asymmetric visibility contract in the browser.
      const bool shadow_requested =
          shading_params_.shadows_enabled && tf_ != nullptr && draw.group != WasmModelDrawGroup::kCollision;
      if (!color_requested && !shadow_requested) {
        continue;
      }
      const auto mesh = layer->modelMeshes().find(draw.mesh_key);
      if (mesh == layer->modelMeshes().end() || mesh->second == nullptr || !mesh->second->ok || draw.frame_id.empty()) {
        continue;
      }
      const auto placed = resolveFixedFromSource(draw.frame_id, fixed_frame);
      if (!placed.has_value()) {
        continue;
      }
      const Transform& fixed_from_frame = *placed;
      const std::uint64_t submesh_draws = mesh->second->submeshes.size();
      const std::uint64_t triangles = mesh->second->indices.size() / 3U;
      if (color_requested) {
        if (submesh_draws > std::numeric_limits<std::uint64_t>::max() - prepared.draws ||
            triangles > std::numeric_limits<std::uint64_t>::max() - prepared.triangles) {
          prepared.draws = std::numeric_limits<std::uint64_t>::max();
          prepared.triangles = std::numeric_limits<std::uint64_t>::max();
          break;
        }
        prepared.draws += submesh_draws;
        prepared.triangles += triangles;
      }
      if (shadow_requested) {
        if (prepared.shadow_draws == std::numeric_limits<std::uint64_t>::max() ||
            triangles > std::numeric_limits<std::uint64_t>::max() - prepared.shadow_triangles) {
          prepared.shadow_draws = std::numeric_limits<std::uint64_t>::max();
          prepared.shadow_triangles = std::numeric_limits<std::uint64_t>::max();
          break;
        }
        ++prepared.shadow_draws;
        prepared.shadow_triangles += triangles;
      }
      const glm::dmat4 fixed_from_model = fixed_from_frame.matrix() * draw.model;
      const auto [bounds, inserted] = model_bounds_cache.try_emplace(mesh->second.get());
      if (inserted) {
        bounds->second = modelBounds(*mesh->second);
      }
      const AABB draw_bounds = transformedModelBounds(bounds->second, fixed_from_model);
      const PreparedModelCall call{draw.mesh_key,       mesh->second,      fixed_from_model,
                                   draw.override_color, draw.use_material, draw.group};
      if (color_requested) {
        layer_bounds = unionAABB(layer_bounds, draw_bounds);
        prepared.color_calls.push_back(call);
      }
      if (shadow_requested) {
        prepared.shadow_calls.push_back(call);
      }
    }
    if (prepared.color_calls.empty() && prepared.shadow_calls.empty()) {
      continue;
    }
    if (prepared.draws > kBrowserMaxModelDrawsPerLayer || prepared.triangles > kBrowserMaxModelTrianglesPerLayer) {
      layer->noteRenderFailure(tr("Models need %1 draws and %2 triangles; the browser layer limits are %3 and %4")
                                   .arg(
                                       QString::number(prepared.draws), QString::number(prepared.triangles),
                                       QString::number(kBrowserMaxModelDrawsPerLayer),
                                       QString::number(kBrowserMaxModelTrianglesPerLayer)));
      continue;
    }
    if (!prepared.color_calls.empty() &&
        !tryConsumeBrowserModels(
            prepared.draws, prepared.triangles, remaining_model_draws, remaining_model_triangles)) {
      layer->noteRenderFailure(
          tr("Models need %1 draws and %2 triangles; the remaining browser view budget is %3 and %4")
              .arg(
                  QString::number(prepared.draws), QString::number(prepared.triangles),
                  QString::number(remaining_model_draws), QString::number(remaining_model_triangles)));
      continue;
    }
    QString shadow_warning;
    if (prepared.shadow_draws > kBrowserMaxModelDrawsPerLayer ||
        prepared.shadow_triangles > kBrowserMaxModelTrianglesPerLayer ||
        !tryConsumeBrowserModels(
            prepared.shadow_draws, prepared.shadow_triangles, remaining_shadow_draws, remaining_shadow_triangles)) {
      shadow_warning = tr("Model shadows exceed the browser draw/triangle budget");
      prepared.shadow_calls.clear();
      prepared.shadow_draws = 0;
      prepared.shadow_triangles = 0;
    }
    if (layer->contributesToSceneBounds()) {
      fixed_model_bounds = unionAABB(fixed_model_bounds, layer_bounds);
      if (auto* marker = dynamic_cast<WasmSceneEntitiesLayer*>(layer);
          marker != nullptr && marker_layers_pending_fit_.contains(marker) &&
          std::find(fitted_marker_layers.cbegin(), fitted_marker_layers.cend(), marker) ==
              fitted_marker_layers.cend()) {
        fitted_marker_layers.push_back(marker);
      }
    }
    if (shadow_warning.isEmpty()) {
      layer->noteRenderSuccess();
    } else {
      layer->noteRenderFailure(shadow_warning);
    }
    prepared_models.push_back(std::move(prepared));
  }
  last_model_bounds_ = fixed_model_bounds;

  const AABB fixed_scene_bounds = unionAABB(
      unionAABB(
          unionAABB(
              unionAABB(unionAABB(fixed_point_bounds, fixed_depth_bounds), fixed_pose_bounds), fixed_occupancy_bounds),
          fixed_voxel_bounds),
      unionAABB(fixed_marker_bounds, fixed_model_bounds));
  setSceneBounds(fixed_scene_bounds);
  if ((!fitted_point_layers.empty() || !fitted_pose_layers.empty() || !fitted_occupancy_layers.empty() ||
       !fitted_voxel_layers.empty() || !fitted_marker_layers.empty()) &&
      fixed_scene_bounds.valid) {
    camera_->fitToBoundingBox(fixed_scene_bounds);
    for (WasmPointRenderable* layer : fitted_point_layers) {
      point_layers_pending_fit_.erase(layer);
    }
    for (WasmPosesInFrameLayer* layer : fitted_pose_layers) {
      pose_layers_pending_fit_.erase(layer);
    }
    for (WasmOccupancyGridLayer* layer : fitted_occupancy_layers) {
      occupancy_layers_pending_fit_.erase(layer);
    }
    for (WasmVoxelGridLayer* layer : fitted_voxel_layers) {
      voxel_layers_pending_fit_.erase(layer);
    }
    for (WasmSceneEntitiesLayer* layer : fitted_marker_layers) {
      marker_layers_pending_fit_.erase(layer);
    }
    ++camera_fit_count_;
  }

  const glm::dvec3 render_origin(camera_->state().focal);
  buildGeometry(render_origin);
  const quint32 prelude_line_vertices = static_cast<quint32>(line_vertices_.size());

  enum class MarkerDrawKind { kInstanceTriangles, kInstanceLines, kStreamTriangles, kStreamLines };
  struct MarkerDrawRange {
    MarkerDrawKind kind = MarkerDrawKind::kInstanceTriangles;
    std::uint32_t mesh = 0;
    quint32 offset = 0;
    quint32 count = 0;
    bool cull_back = false;
    bool depth_write = true;
  };
  struct MarkerLayerDraw {
    WasmSceneEntitiesLayer* layer = nullptr;
    std::vector<MarkerDrawRange> ranges;
  };
  std::vector<MarkerLayerDraw> marker_draws;
  marker_draws.reserve(prepared_markers.size());
  marker_instances_.clear();
  last_marker_layer_count_ = 0;
  last_marker_instance_count_ = 0;
  last_marker_stream_vertex_count_ = 0;

  for (const PreparedMarkerLayer& prepared : prepared_markers) {
    WasmSceneEntitiesLayer* layer = prepared.layer;
    const WasmMarkerGeometry& geometry = layer->geometry();
    MarkerLayerDraw draw;
    draw.layer = layer;
    const bool pass_translucent = layer->opacity() < 0.999F;
    const auto apply_color = [layer](const glm::vec4& source) {
      glm::vec4 color = source;
      if (layer->colorOverrideEnabled()) {
        const QColor override = layer->overrideColor();
        color = {
            static_cast<float>(override.redF()), static_cast<float>(override.greenF()),
            static_cast<float>(override.blueF()), 1.0F};
      }
      color.a *= layer->opacity();
      return color;
    };
    const auto append_instances = [&](const std::vector<WasmMarkerInstanceSource>& sources,
                                      bool cube_edge_overlay = false) {
      const quint32 first = static_cast<quint32>(marker_instances_.size());
      for (const WasmMarkerInstanceSource& source : sources) {
        if (source.frame_index >= prepared.fixed_from_frames.size() ||
            !prepared.fixed_from_frames[source.frame_index].has_value()) {
          continue;
        }
        glm::dmat4 model = prepared.fixed_from_frames[source.frame_index]->matrix() * source.model;
        model[3].x -= render_origin.x;
        model[3].y -= render_origin.y;
        model[3].z -= render_origin.z;
        const glm::mat4 narrowed(model);
        MarkerInstance instance;
        std::memcpy(instance.model.data(), &narrowed[0][0], sizeof(narrowed));
        glm::vec4 color = apply_color(source.color);
        if (cube_edge_overlay) {
          color.r *= 0.6F;
          color.g *= 0.6F;
          color.b *= 0.6F;
          color.a = 1.0F;
        }
        instance.r = color.r;
        instance.g = color.g;
        instance.b = color.b;
        instance.a = color.a;
        instance.bottom_scale = source.bottom_scale;
        instance.top_scale = source.top_scale;
        marker_instances_.push_back(instance);
      }
      return std::pair{first, static_cast<quint32>(marker_instances_.size()) - first};
    };
    const auto add_instance_range = [&](std::uint32_t mesh, quint32 offset, quint32 count, bool cull_back,
                                        bool depth_write, bool as_lines) {
      if (count != 0U) {
        draw.ranges.push_back(
            {as_lines ? MarkerDrawKind::kInstanceLines : MarkerDrawKind::kInstanceTriangles, mesh, offset, count,
             cull_back, depth_write});
      }
    };

    // Native family order: cube fill + edge overlay, sphere, cylinder,
    // arrows/axes, user lines, user triangles.
    const auto [cube_offset, cube_count] = append_instances(geometry.cubes);
    bool cubes_translucent = pass_translucent;
    for (quint32 index = 0; index < cube_count; ++index) {
      cubes_translucent = cubes_translucent || marker_instances_[cube_offset + index].a < 0.999F;
    }
    if (layer->wireframe()) {
      add_instance_range(0U, cube_offset, cube_count, false, !pass_translucent, true);
    } else {
      add_instance_range(0U, cube_offset, cube_count, true, !cubes_translucent, false);
      // Desktop's normal cube overlay contains only the twelve geometric box
      // edges. Explicit wireframe mode above still uses the triangle mesh edges,
      // including face diagonals, like native polygon mode.
      const auto [edge_offset, edge_count] = append_instances(geometry.cubes, true);
      add_instance_range(4U, edge_offset, edge_count, false, !pass_translucent, true);
    }
    const auto append_family = [&](const auto& sources, std::uint32_t mesh, bool cull_back) {
      const auto [offset, count] = append_instances(sources);
      add_instance_range(
          mesh, offset, count, layer->wireframe() ? false : cull_back, !pass_translucent, layer->wireframe());
    };
    append_family(geometry.spheres, 1U, true);
    append_family(geometry.cylinders, 2U, true);
    append_family(geometry.arrows, 3U, false);
    append_family(geometry.axes, 3U, false);

    const quint32 line_offset = static_cast<quint32>(line_vertices_.size());
    for (const WasmMarkerStreamBatch& batch : geometry.lines) {
      if (batch.frame_index >= prepared.fixed_from_frames.size() ||
          !prepared.fixed_from_frames[batch.frame_index].has_value()) {
        continue;
      }
      const glm::dmat4 model = prepared.fixed_from_frames[batch.frame_index]->matrix() * batch.model;
      for (const WasmMarkerStreamVertex& source : batch.vertices) {
        const glm::dvec3 point = glm::dvec3(model * glm::dvec4(source.position, 1.0)) - render_origin;
        const glm::vec4 color = apply_color(source.color);
        line_vertices_.push_back(
            {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z), color.r, color.g,
             color.b, color.a});
      }
    }
    const quint32 line_count = static_cast<quint32>(line_vertices_.size()) - line_offset;
    if (line_count != 0U) {
      draw.ranges.push_back({MarkerDrawKind::kStreamLines, 0U, line_offset, line_count, false, !pass_translucent});
    }

    if (layer->wireframe()) {
      const quint32 triangle_edge_offset = static_cast<quint32>(line_vertices_.size());
      for (const WasmMarkerStreamBatch& batch : geometry.triangles) {
        if (batch.frame_index >= prepared.fixed_from_frames.size() ||
            !prepared.fixed_from_frames[batch.frame_index].has_value()) {
          continue;
        }
        const glm::dmat4 model = prepared.fixed_from_frames[batch.frame_index]->matrix() * batch.model;
        for (std::size_t index = 0; index + 2U < batch.vertices.size(); index += 3U) {
          std::array<Vertex, 3> vertices;
          for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
            const WasmMarkerStreamVertex& source = batch.vertices[index + corner];
            const glm::dvec3 point = glm::dvec3(model * glm::dvec4(source.position, 1.0)) - render_origin;
            const glm::vec4 color = apply_color(source.color);
            vertices[corner] = {
                static_cast<float>(point.x),
                static_cast<float>(point.y),
                static_cast<float>(point.z),
                color.r,
                color.g,
                color.b,
                color.a};
          }
          line_vertices_.insert(
              line_vertices_.end(), {vertices[0], vertices[1], vertices[1], vertices[2], vertices[2], vertices[0]});
        }
      }
      const quint32 edge_count = static_cast<quint32>(line_vertices_.size()) - triangle_edge_offset;
      if (edge_count != 0U) {
        draw.ranges.push_back(
            {MarkerDrawKind::kStreamLines, 0U, triangle_edge_offset, edge_count, false, !pass_translucent});
      }
    } else {
      const quint32 triangle_offset = static_cast<quint32>(triangle_vertices_.size());
      for (const WasmMarkerStreamBatch& batch : geometry.triangles) {
        if (batch.frame_index >= prepared.fixed_from_frames.size() ||
            !prepared.fixed_from_frames[batch.frame_index].has_value()) {
          continue;
        }
        const glm::dmat4 model = prepared.fixed_from_frames[batch.frame_index]->matrix() * batch.model;
        for (const WasmMarkerStreamVertex& source : batch.vertices) {
          const glm::dvec3 point = glm::dvec3(model * glm::dvec4(source.position, 1.0)) - render_origin;
          const glm::vec4 color = apply_color(source.color);
          triangle_vertices_.push_back(
              {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z), color.r, color.g,
               color.b, color.a});
        }
      }
      const quint32 triangle_count = static_cast<quint32>(triangle_vertices_.size()) - triangle_offset;
      if (triangle_count != 0U) {
        draw.ranges.push_back(
            {MarkerDrawKind::kStreamTriangles, 0U, triangle_offset, triangle_count, false, !pass_translucent});
      }
    }
    if (!draw.ranges.empty()) {
      ++last_marker_layer_count_;
      last_marker_instance_count_ += static_cast<int>(prepared.instances);
      last_marker_stream_vertex_count_ += static_cast<int>(prepared.stream_vertices);
      marker_draws.push_back(std::move(draw));
    }
  }

  struct ModelDrawRange {
    ModelMeshGpu* mesh = nullptr;
    ModelMaterialGpu* material = nullptr;
    quint32 instance_offset = 0;
    quint32 index_offset = 0;
    quint32 index_count = 0;
    bool lines = false;
    bool depth_write = true;
  };
  struct ModelLayerDraw {
    WasmModelRenderable* layer = nullptr;
    std::vector<ModelDrawRange> ranges;
  };
  struct ShadowDrawRange {
    ModelMeshGpu* mesh = nullptr;
    quint32 instance_offset = 0;
    quint32 index_count = 0;
  };
  std::vector<ModelLayerDraw> model_draws;
  model_draws.reserve(prepared_models.size());
  std::vector<ShadowDrawRange> shadow_draws;
  shadow_draws.reserve(model_draws.capacity());
  model_instances_.clear();
  shadow_instances_.clear();
  last_model_layer_count_ = 0;
  last_model_draw_count_ = 0;
  last_model_triangle_count_ = 0;
  last_model_texture_slot_counts_.fill(0);
  last_shadow_draw_count_ = 0;
  last_shadow_triangle_count_ = 0;
  AABB render_shadow_bounds;
  for (const PreparedModelLayer& prepared : prepared_models) {
    WasmModelRenderable* layer = prepared.layer;
    ModelLayerGpu& gpu = model_layer_gpu_[layer];
    if (!ensureModelLayerGpu(current_rhi, layer, gpu)) {
      continue;
    }
    ModelLayerDraw layer_draw;
    layer_draw.layer = layer;
    for (const PreparedModelCall& call : prepared.color_calls) {
      const auto gpu_mesh = gpu.meshes.find(call.mesh_key);
      if (gpu_mesh == gpu.meshes.end() || gpu_mesh->second.source == nullptr) {
        continue;
      }
      ModelMeshGpu& mesh = gpu_mesh->second;
      glm::dmat4 model = call.fixed_from_model;
      model[3].x -= render_origin.x;
      model[3].y -= render_origin.y;
      model[3].z -= render_origin.z;
      const glm::mat4 narrowed(model);
      for (std::size_t index = 0; index < call.mesh->submeshes.size() && index < mesh.materials.size(); ++index) {
        const SubMesh& submesh = call.mesh->submeshes[index];
        if (submesh.index_count == 0U) {
          continue;
        }
        const Material fallback;
        const Material& material = submesh.material != nullptr ? *submesh.material : fallback;
        ModelMaterialGpu& material_gpu = mesh.materials[index];
        const bool pbr_active = materialUsesPbrPath(material);
        bool use_material = call.use_material;
        glm::vec4 tint = use_material ? material.base_color_factor : call.override_color;
        if (layer->colorOverrideEnabled()) {
          const QColor override = layer->overrideColor();
          tint.r = static_cast<float>(override.redF());
          tint.g = static_cast<float>(override.greenF());
          tint.b = static_cast<float>(override.blueF());
          use_material = false;
        }
        float opacity = layer->opacity();
        if (call.group == WasmModelDrawGroup::kVisual) {
          opacity *= shading_params_.mesh_opacity;
        } else if (call.group == WasmModelDrawGroup::kCollision) {
          opacity *= shading_params_.collision_opacity;
        }
        tint.a *= opacity;
        const bool use_texture = material_gpu.textures[kBaseColorSlot].present && (pbr_active || use_material);
        float alpha_mode = 0.0F;
        if ((pbr_active || use_material) && material.alpha_mode == AlphaMode::kMask) {
          alpha_mode = 1.0F;
        } else if ((pbr_active || use_material) && material.alpha_mode == AlphaMode::kBlend) {
          alpha_mode = 2.0F;
        }
        ModelInstance instance;
        std::memcpy(instance.model.data(), &narrowed[0][0], sizeof(narrowed));
        instance.tint = {tint.r, tint.g, tint.b, tint.a};
        instance.params = {use_texture ? 1.0F : 0.0F, alpha_mode, material.alpha_cutoff, use_material ? 1.0F : 0.0F};
        instance.texture_flags = {
            pbr_active && material_gpu.textures[kMetallicRoughnessSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kNormalSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kOcclusionSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kEmissiveSlot].present ? 1.0F : 0.0F};
        instance.pbr_factors = {
            material.has_pbr ? material.metallic_factor : 0.0F,
            material.has_pbr ? material.roughness_factor : shading_params_.roughness, pbr_active ? 1.0F : 0.0F,
            call.group == WasmModelDrawGroup::kCollision ? 1.0F : 0.0F};
        instance.emissive_factor = {
            pbr_active ? material.emissive_factor.r : 0.0F, pbr_active ? material.emissive_factor.g : 0.0F,
            pbr_active ? material.emissive_factor.b : 0.0F, 0.0F};
        for (std::size_t slot = 0; slot < material_gpu.textures.size(); ++slot) {
          last_model_texture_slot_counts_[slot] += material_gpu.textures[slot].present ? 1 : 0;
        }
        const quint32 instance_offset = static_cast<quint32>(model_instances_.size());
        model_instances_.push_back(instance);
        const bool lines = layer->wireframe();
        const quint32 index_offset = lines ? mesh.edge_offsets[index] : static_cast<quint32>(submesh.index_offset);
        const quint32 index_count = lines ? mesh.edge_counts[index] : static_cast<quint32>(submesh.index_count);
        const bool translucent = call.group == WasmModelDrawGroup::kCollision || tint.a < 0.999F || alpha_mode > 1.5F;
        layer_draw.ranges.push_back(
            ModelDrawRange{&mesh, &material_gpu, instance_offset, index_offset, index_count, lines, !translucent});
      }
    }
    if (!layer_draw.ranges.empty()) {
      ++last_model_layer_count_;
      last_model_draw_count_ += static_cast<int>(layer_draw.ranges.size());
      last_model_triangle_count_ += static_cast<int>(prepared.triangles);
      model_draws.push_back(std::move(layer_draw));
    }
    for (const PreparedModelCall& call : prepared.shadow_calls) {
      const auto gpu_mesh = gpu.meshes.find(call.mesh_key);
      if (gpu_mesh == gpu.meshes.end() || gpu_mesh->second.source == nullptr ||
          gpu_mesh->second.triangle_index_buffer == nullptr || call.mesh->indices.empty()) {
        continue;
      }
      glm::dmat4 model = call.fixed_from_model;
      model[3].x -= render_origin.x;
      model[3].y -= render_origin.y;
      model[3].z -= render_origin.z;
      const glm::mat4 narrowed(model);
      ShadowInstance instance;
      std::memcpy(instance.model.data(), &narrowed[0][0], sizeof(narrowed));
      const quint32 instance_offset = static_cast<quint32>(shadow_instances_.size());
      shadow_instances_.push_back(instance);
      const auto cached_bounds = model_bounds_cache.find(call.mesh.get());
      if (cached_bounds != model_bounds_cache.end()) {
        render_shadow_bounds = unionAABB(
            render_shadow_bounds,
            transformedModelBoundsRelative(cached_bounds->second, call.fixed_from_model, render_origin));
      }
      const quint32 index_count = static_cast<quint32>(call.mesh->indices.size());
      shadow_draws.push_back(ShadowDrawRange{&gpu_mesh->second, instance_offset, index_count});
      ++last_shadow_draw_count_;
      last_shadow_triangle_count_ += static_cast<int>(index_count / 3U);
    }
  }

  if (!shading_params_.shadows_enabled && shadow_texture_ != nullptr) {
    releaseShadowResources(true);
  }
  if (render_shadow_bounds.valid) {
    render_shadow_bounds = extendAabbToGroundShadow(
        render_shadow_bounds, shading_params_.key_light_dir, static_cast<float>(-render_origin.z));
  }
  last_shadow_bounds_ = render_shadow_bounds;
  const ShadowCameraFit shadow_fit =
      fitDirectionalShadowCamera(render_shadow_bounds, shading_params_.key_light_dir, kShadowMapSize);
  last_shadow_fit_valid_ = shadow_fit.valid;
  const bool shadow_requested =
      shading_params_.shadows_enabled && tf_ != nullptr && !shadow_draws.empty() && shadow_fit.valid;
  const bool shadow_resources_ready = shadow_requested && ensureShadowResources(current_rhi);

  const quint32 line_bytes = static_cast<quint32>(line_vertices_.size() * sizeof(Vertex));
  const quint32 triangle_bytes = static_cast<quint32>(triangle_vertices_.size() * sizeof(Vertex));
  const quint32 marker_instance_bytes = static_cast<quint32>(marker_instances_.size() * sizeof(MarkerInstance));
  const quint32 model_instance_bytes = static_cast<quint32>(model_instances_.size() * sizeof(ModelInstance));
  const quint32 shadow_instance_bytes = static_cast<quint32>(shadow_instances_.size() * sizeof(ShadowInstance));
  const bool buffers_ready =
      ensureVertexBuffer(current_rhi, line_buffer_, line_buffer_capacity_, line_bytes, kInitialVertexBufferBytes) &&
      ensureVertexBuffer(
          current_rhi, triangle_buffer_, triangle_buffer_capacity_, triangle_bytes, kInitialVertexBufferBytes) &&
      ensureVertexBuffer(
          current_rhi, marker_instance_buffer_, marker_instance_buffer_capacity_, marker_instance_bytes,
          kInitialVertexBufferBytes) &&
      ensureVertexBuffer(
          current_rhi, model_instance_buffer_, model_instance_buffer_capacity_, model_instance_bytes,
          kInitialVertexBufferBytes);
  bool shadow_buffer_ready = false;
  if (shadow_resources_ready) {
    shadow_buffer_ready = ensureVertexBuffer(
        current_rhi, shadow_instance_buffer_, shadow_instance_buffer_capacity_, shadow_instance_bytes,
        kInitialVertexBufferBytes);
    if (!shadow_buffer_ready) {
      shadow_stage_.capability_error = tr("Could not allocate the browser shadow instance buffer");
      qWarning("SceneViewWidget WASM shadows unavailable: %s", qPrintable(shadow_stage_.capability_error));
      releaseShadowResources(true);
    }
  }
  const bool shadow_active = shadow_requested && shadow_resources_ready && shadow_buffer_ready;
  shadow_stage_.last_active = shadow_active;
  if (shadow_requested && !shadow_active && !shadow_stage_.capability_error.isEmpty()) {
    if (noted_shadow_failure_ != shadow_stage_.capability_error) {
      noted_shadow_failure_ = shadow_stage_.capability_error;
      for (const PreparedModelLayer& prepared : prepared_models) {
        if (prepared.layer != nullptr && !prepared.shadow_calls.empty()) {
          prepared.layer->noteRenderFailure(shadow_stage_.capability_error);
        }
      }
    }
  } else {
    noted_shadow_failure_.clear();
  }

  QRhiResourceUpdateBatch* updates = current_rhi->nextResourceUpdateBatch();
  const RenderModeUniforms data_render_mode{{hdr_active ? 1 : 0, 0, 0, 0}};
  const RenderModeUniforms annotation_render_mode{{hdr_active ? 1 : 0, 1, 0, 0}};
  updates->updateDynamicBuffer(render_mode_uniform_buffer_, 0, kRenderModeUniformBytes, &data_render_mode);
  updates->updateDynamicBuffer(
      annotation_render_mode_uniform_buffer_, 0, kRenderModeUniformBytes, &annotation_render_mode);
  if (buffers_ready && line_bytes != 0) {
    updates->updateDynamicBuffer(line_buffer_, 0, line_bytes, line_vertices_.data());
  }
  if (buffers_ready && triangle_bytes != 0) {
    updates->updateDynamicBuffer(triangle_buffer_, 0, triangle_bytes, triangle_vertices_.data());
  }
  if (buffers_ready && marker_instance_bytes != 0) {
    updates->updateDynamicBuffer(marker_instance_buffer_, 0, marker_instance_bytes, marker_instances_.data());
  }
  if (buffers_ready && model_instance_bytes != 0) {
    updates->updateDynamicBuffer(model_instance_buffer_, 0, model_instance_bytes, model_instances_.data());
  }
  if (shadow_active && shadow_instance_bytes != 0) {
    updates->updateDynamicBuffer(shadow_instance_buffer_, 0, shadow_instance_bytes, shadow_instances_.data());
  }
  if (model_white_upload_pending_ && model_white_texture_ != nullptr) {
    static constexpr std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
    QRhiTextureSubresourceUploadDescription description(white.data(), static_cast<quint32>(white.size()));
    description.setSourceSize(QSize(1, 1));
    updates->uploadTexture(model_white_texture_, QRhiTextureUploadDescription({0, 0, description}));
    model_white_upload_pending_ = false;
  }
  for (auto& [_, layer_gpu] : model_layer_gpu_) {
    for (auto& [__, mesh] : layer_gpu.meshes) {
      if (mesh.upload_pending && mesh.source != nullptr) {
        updates->uploadStaticBuffer(mesh.vertex_buffer, mesh.source->vertices.data());
        updates->uploadStaticBuffer(mesh.triangle_index_buffer, mesh.source->indices.data());
        if (mesh.edge_index_buffer != nullptr && !mesh.edge_indices.empty()) {
          updates->uploadStaticBuffer(mesh.edge_index_buffer, mesh.edge_indices.data());
        }
        mesh.upload_pending = false;
      }
      for (ModelMaterialGpu& material : mesh.materials) {
        for (ModelTextureGpu& texture : material.textures) {
          if (texture.texture_upload_pending && texture.texture != nullptr && !texture.pending_image.isNull()) {
            const QRhiTextureSubresourceUploadDescription description(texture.pending_image);
            updates->uploadTexture(texture.texture, QRhiTextureUploadDescription({0, 0, description}));
            texture.pending_image = {};
            texture.texture_upload_pending = false;
          }
        }
      }
    }
  }
  if (colormap_upload_pending_ && colormap_texture_ != nullptr) {
    static const std::vector<std::uint8_t> colormap_lut = PJ::buildColormapLut();
    QRhiTextureSubresourceUploadDescription description(colormap_lut.data(), static_cast<quint32>(colormap_lut.size()));
    description.setSourceSize(QSize(PJ::kColormapLutWidth, PJ::kColormapCount));
    updates->uploadTexture(colormap_texture_, QRhiTextureUploadDescription({0, 0, description}));
    colormap_upload_pending_ = false;
  }
  if (cube_upload_pending_ && cube_vertex_buffer_ != nullptr && cube_index_buffer_ != nullptr) {
    updates->uploadStaticBuffer(cube_vertex_buffer_, kCubeVertices.data());
    updates->uploadStaticBuffer(cube_index_buffer_, kCubeIndices.data());  // already uint16, as QRhi wants
    cube_upload_pending_ = false;
  }
  if (pose_mesh_upload_pending_ && pose_vertex_buffer_ != nullptr && pose_index_buffer_ != nullptr) {
    const ArrowMeshData& pose_mesh = unitPoseArrowMesh();
    updates->uploadStaticBuffer(pose_vertex_buffer_, pose_mesh.vertices.data());
    updates->uploadStaticBuffer(pose_index_buffer_, pose_mesh.indices.data());
    pose_mesh_upload_pending_ = false;
  }
  if (occupancy_quad_upload_pending_ && occupancy_quad_buffer_ != nullptr) {
    updates->uploadStaticBuffer(occupancy_quad_buffer_, kOccupancyQuad.data());
    occupancy_quad_upload_pending_ = false;
  }
  if (marker_mesh_upload_pending_) {
    const auto& mesh_data = markerMeshes();
    for (std::size_t index = 0; index < mesh_data.size(); ++index) {
      updates->uploadStaticBuffer(marker_meshes_[index].vertex_buffer, mesh_data[index].vertices.data());
      if (marker_meshes_[index].triangle_index_buffer != nullptr) {
        updates->uploadStaticBuffer(
            marker_meshes_[index].triangle_index_buffer, mesh_data[index].triangle_indices.data());
      }
      if (marker_meshes_[index].edge_index_buffer != nullptr) {
        updates->uploadStaticBuffer(marker_meshes_[index].edge_index_buffer, mesh_data[index].edge_indices.data());
      }
    }
    marker_mesh_upload_pending_ = false;
  }

  const glm::mat4 projection = camera_->projMatrix(aspect);
  const glm::mat4 view = camera_->viewMatrixRelativeTo(render_origin);
  const glm::mat4 view_projection = projection * view;
  const QMatrix4x4 clip_correction = current_rhi->clipSpaceCorrMatrix();
  const QMatrix4x4 corrected_projection = clip_correction * toQMatrix(projection);
  const QMatrix4x4 matrix = corrected_projection * toQMatrix(view);
  const QMatrix4x4 shadow_matrix =
      clip_correction * toQMatrix(shadow_fit.valid ? shadow_fit.light_view_proj : glm::mat4(1.0F));
  const RhiConventions conventions = rhiConventions(*current_rhi);
  const QMatrix4x4 clip_to_texture = clipToTextureMatrix(conventions);
  // View space -> this backend's render-target texture space (u, v, stored depth).
  const QMatrix4x4 texture_projection = clip_to_texture * corrected_projection;
  if (hdr_active) {
    bool inverse_projection_valid = false;
    const QMatrix4x4 inverse_projection = texture_projection.inverted(&inverse_projection_valid);
    ssao_active = ssao_active && inverse_projection_valid;
    ssao_stage_.last_active = ssao_active;
    edl_active = edl_active && inverse_projection_valid;
    edl_stage_.last_active = edl_active;
    CompositeUniforms composite;
    composite.params = {
        look::kExposure,
        look::kSaturation,
        kBackgroundCoverageThreshold,
        look::kEdlFloor,
    };
    copyMatrix(inverse_projection, composite.inverse_projection);
    composite.edl_params = {
        look::kEdlStrength,
        look::kEdlRadiusPx,
        look::kEdlMaxGap,
        edl_active ? 1.0F : 0.0F,
    };
    composite.ssao_params = {
        look::kAoStrength,
        ssao_active ? 1.0F : 0.0F,
        0.0F,
        0.0F,
    };
    updates->updateDynamicBuffer(composite_uniform_buffer_, 0, kCompositeUniformBytes, &composite);
    if (ssao_active && ssao_uniform_buffer_ != nullptr) {
      SsaoUniforms ssao_uniforms;
      copyMatrix(texture_projection, ssao_uniforms.projection);
      copyMatrix(inverse_projection, ssao_uniforms.inverse_projection);
      ssao_uniforms.params = {
          look::kSsaoRadiusM,
          look::kSsaoPower,
          look::kSsaoBias,
          textureVSign(conventions),
      };
      ssao_uniforms.kernel = ssaoKernel();
      updates->updateDynamicBuffer(ssao_uniform_buffer_, 0, kSsaoUniformBytes, &ssao_uniforms);
    }
  }
  if (uniform_buffer_ != nullptr) {
    updates->updateDynamicBuffer(uniform_buffer_, 0, 64, matrix.constData());
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
    copyMatrix(clip_to_texture * shadow_matrix, model_uniforms.light_view_projection);
    model_uniforms.shadow_params = {
        shadow_active ? shadow_fit.world_units_per_texel * look::kShadowNormalOffsetTexels : 0.0F,
        look::kShadowSoftnessTexels, shadow_active ? 1.0F : 0.0F, static_cast<float>(shadow_map_size_)};
    updates->updateDynamicBuffer(model_uniform_buffer_, 0, kModelUniformBytes, &model_uniforms);
  }

  struct PointDraw {
    WasmPointRenderable* layer = nullptr;
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 vertex_count = 0;
    bool cubes = false;
  };
  std::vector<PointDraw> point_draws;
  point_draws.reserve(prepared_points.size());
  last_point_vertex_count_ = 0;
  last_point_layer_count_ = 0;
  last_depth_vertex_count_ = 0;
  last_depth_layer_count_ = 0;
  last_cube_layer_count_ = 0;
  last_cube_instance_count_ = 0;
  for (const PreparedPointLayer& prepared : prepared_points) {
    WasmPointRenderable* layer = prepared.layer;
    PointLayerGpu& gpu = point_layer_gpu_[layer];
    if (!ensurePointLayerGpu(current_rhi, layer, gpu)) {
      continue;
    }
    if (gpu.uploaded_revision != layer->geometryRevision()) {
      const auto& vertices = layer->vertices();
      const quint32 bytes = static_cast<quint32>(vertices.size() * sizeof(WasmPointVertex));
      const std::uint64_t shrink_threshold =
          std::max<std::uint64_t>(static_cast<std::uint64_t>(bytes) * 2U, kPointInitialVertexBufferBytes);
      if (gpu.vertex_buffer != nullptr && gpu.vertex_capacity > shrink_threshold) {
        delete gpu.vertex_buffer;
        gpu.vertex_buffer = nullptr;
        gpu.vertex_capacity = 0;
      }
      if (bytes != 0 &&
          !ensureVertexBuffer(
              current_rhi, gpu.vertex_buffer, gpu.vertex_capacity, bytes, kPointInitialVertexBufferBytes)) {
        gpu.vertex_count = 0;
        continue;
      }
      if (bytes != 0) {
        updates->updateDynamicBuffer(gpu.vertex_buffer, 0, bytes, vertices.data());
      }
      gpu.vertex_count = static_cast<quint32>(vertices.size());
      gpu.uploaded_revision = layer->geometryRevision();
    }
    if (gpu.vertex_buffer == nullptr || gpu.vertex_count == 0) {
      continue;
    }

    glm::dmat4 relative_model = prepared.fixed_from_source.matrix();
    relative_model[3].x -= render_origin.x;
    relative_model[3].y -= render_origin.y;
    relative_model[3].z -= render_origin.z;
    const glm::mat4 relative_model_float(relative_model);
    const glm::mat4 absolute_model(prepared.fixed_from_source.matrix());
    const auto [range_minimum, range_maximum] = layer->scalarRange(absolute_model);
    layer->noteRenderedScalarRange(range_minimum, range_maximum);

    PointUniforms point_uniforms;
    copyMatrix(clip_correction * toQMatrix(view_projection), point_uniforms.view_projection);
    copyMatrix(toQMatrix(relative_model_float), point_uniforms.fixed_from_source);
    copyMatrix(toQMatrix(view), point_uniforms.view);
    point_uniforms.render_origin = {
        static_cast<float>(render_origin.x), static_cast<float>(render_origin.y), static_cast<float>(render_origin.z),
        camera_->state().perspective ? 1.0F : 0.0F};
    point_uniforms.point_params = {
        layer->sizeMeters() * 0.5F, layer->sizePixels(), static_cast<float>(std::max(output_size.height(), 1)),
        projection[1][1]};
    point_uniforms.range_params = {
        range_minimum, range_maximum, layer->outsideRangeAlpha(),
        layer->shape() == WasmPointShape::kPoint ? 0.0F : 1.0F};
    const QColor solid = layer->solidColor();
    point_uniforms.solid_params = {
        static_cast<float>(solid.redF()), static_cast<float>(solid.greenF()), static_cast<float>(solid.blueF()),
        layer->shape() == WasmPointShape::kSphere ? 1.0F : 0.0F};
    point_uniforms.modes = {
        static_cast<std::int32_t>(layer->colorType()), static_cast<std::int32_t>(layer->colormap()),
        layer->invertLut() ? 1 : 0, layer->scalarAxis()};
    updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kPointUniformBytes, &point_uniforms);
    point_draws.push_back(
        PointDraw{
            layer,
            gpu.vertex_buffer,
            gpu.shader_resources,
            gpu.vertex_count,
            layer->shape() == WasmPointShape::kCube,
        });
    if (layer->isDepthCloud()) {
      last_depth_vertex_count_ += static_cast<int>(gpu.vertex_count);
      ++last_depth_layer_count_;
    } else {
      last_point_vertex_count_ += static_cast<int>(gpu.vertex_count);
      ++last_point_layer_count_;
    }
    if (!layer->isDepthCloud() && layer->shape() == WasmPointShape::kCube) {
      last_cube_instance_count_ += static_cast<int>(gpu.vertex_count);
      ++last_cube_layer_count_;
    }
  }

  struct PoseDraw {
    WasmPosesInFrameLayer* layer = nullptr;
    QRhiBuffer* instance_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_count = 0;
  };
  std::vector<PoseDraw> pose_draws;
  pose_draws.reserve(prepared_poses.size());
  last_pose_layer_count_ = 0;
  last_pose_arm_count_ = 0;
  for (const PreparedPoseLayer& prepared : prepared_poses) {
    WasmPosesInFrameLayer* layer = prepared.layer;
    PoseLayerGpu& gpu = pose_layer_gpu_[layer];
    if (!ensurePoseLayerGpu(current_rhi, gpu)) {
      continue;
    }
    if (gpu.uploaded_revision != layer->geometryRevision()) {
      const auto& instances = layer->instances();
      const quint32 bytes = static_cast<quint32>(instances.size() * sizeof(PoseTriadInstance));
      const std::uint64_t shrink_threshold = std::max<std::uint64_t>(static_cast<std::uint64_t>(bytes) * 2U, 1024U);
      if (gpu.instance_buffer != nullptr && gpu.instance_capacity > shrink_threshold) {
        delete gpu.instance_buffer;
        gpu.instance_buffer = nullptr;
        gpu.instance_capacity = 0;
      }
      if (bytes != 0 && !ensureVertexBuffer(current_rhi, gpu.instance_buffer, gpu.instance_capacity, bytes, bytes)) {
        gpu.instance_count = 0;
        continue;
      }
      if (bytes != 0) {
        updates->updateDynamicBuffer(gpu.instance_buffer, 0, bytes, instances.data());
      }
      gpu.instance_count = static_cast<quint32>(instances.size());
      gpu.uploaded_revision = layer->geometryRevision();
    }
    if (gpu.instance_buffer == nullptr || gpu.instance_count == 0) {
      continue;
    }

    glm::dmat4 relative_model = prepared.fixed_from_source.matrix();
    relative_model[3].x -= render_origin.x;
    relative_model[3].y -= render_origin.y;
    relative_model[3].z -= render_origin.z;
    PoseUniforms pose_uniforms;
    copyMatrix(clip_correction * toQMatrix(view_projection), pose_uniforms.view_projection);
    copyMatrix(toQMatrix(glm::mat4(relative_model)), pose_uniforms.fixed_from_source);
    copyMatrix(toQMatrix(view), pose_uniforms.view);
    updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kPoseUniformBytes, &pose_uniforms);
    pose_draws.push_back(PoseDraw{layer, gpu.instance_buffer, gpu.shader_resources, gpu.instance_count});
    ++last_pose_layer_count_;
    last_pose_arm_count_ += static_cast<int>(gpu.instance_count);
  }

  // TF frame triads: solid instanced arrows reusing the pose arrow mesh/pipeline.
  // The per-frame transform is baked into each instance model, so fixed_from_source
  // is identity.
  quint32 tf_triad_instance_count = 0;
  if (!tf_triad_instances_.empty() && ensurePoseLayerGpu(current_rhi, tf_triad_gpu_)) {
    const quint32 bytes = static_cast<quint32>(tf_triad_instances_.size() * sizeof(PoseTriadInstance));
    if (ensureVertexBuffer(current_rhi, tf_triad_gpu_.instance_buffer, tf_triad_gpu_.instance_capacity, bytes, bytes)) {
      updates->updateDynamicBuffer(tf_triad_gpu_.instance_buffer, 0, bytes, tf_triad_instances_.data());
      PoseUniforms tf_uniforms;
      copyMatrix(clip_correction * toQMatrix(view_projection), tf_uniforms.view_projection);
      copyMatrix(QMatrix4x4(), tf_uniforms.fixed_from_source);  // identity: transform baked into model
      copyMatrix(toQMatrix(view), tf_uniforms.view);
      updates->updateDynamicBuffer(tf_triad_gpu_.uniform_buffer, 0, kPoseUniformBytes, &tf_uniforms);
      tf_triad_instance_count = static_cast<quint32>(tf_triad_instances_.size());
    }
  }

  struct OccupancyDraw {
    WasmOccupancyGridLayer* layer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
  };
  std::vector<OccupancyDraw> occupancy_draws;
  occupancy_draws.reserve(prepared_occupancy.size());
  last_occupancy_layer_count_ = 0;
  last_occupancy_cell_count_ = 0;
  for (const PreparedOccupancyLayer& prepared : prepared_occupancy) {
    WasmOccupancyGridLayer* layer = prepared.layer;
    OccupancyLayerGpu& gpu = occupancy_layer_gpu_[layer];
    bool texture_recreated = false;
    if (!ensureOccupancyLayerGpu(current_rhi, layer, gpu, texture_recreated)) {
      continue;
    }
    const ReconstructedGrid& grid = layer->grid();
    if (gpu.uploaded_revision != layer->textureRevision()) {
      const bool full_upload = texture_recreated || layer->fullUploadPending();
      if (full_upload) {
        QRhiTextureSubresourceUploadDescription description(grid.cells.data(), static_cast<quint32>(grid.cells.size()));
        description.setSourceSize(QSize(static_cast<int>(grid.width), static_cast<int>(grid.height)));
        updates->uploadTexture(gpu.texture, QRhiTextureUploadDescription({0, 0, description}));
        ++occupancy_full_upload_count_;
      } else {
        for (const CellRect& rect : layer->dirtyRects()) {
          const qsizetype rect_width = static_cast<qsizetype>(rect.width);
          const qsizetype rect_height = static_cast<qsizetype>(rect.height);
          const qsizetype packed_size = rect_width * rect_height;
          QByteArray packed(packed_size, Qt::Uninitialized);
          for (std::uint32_t row = 0; row < rect.height; ++row) {
            std::memcpy(
                packed.data() + static_cast<qsizetype>(row) * rect_width,
                grid.cells.data() + static_cast<std::size_t>(rect.y + row) * grid.width + rect.x, rect.width);
          }
          QRhiTextureSubresourceUploadDescription description(packed);
          description.setSourceSize(QSize(static_cast<int>(rect.width), static_cast<int>(rect.height)));
          description.setDestinationTopLeft(QPoint(static_cast<int>(rect.x), static_cast<int>(rect.y)));
          updates->uploadTexture(gpu.texture, QRhiTextureUploadDescription({0, 0, description}));
        }
        if (!layer->dirtyRects().empty()) {
          ++occupancy_partial_upload_count_;
        }
      }
      gpu.uploaded_revision = layer->textureRevision();
    }

    const auto& p = grid.origin.position;
    const auto& q = grid.origin.orientation;
    const Transform source_from_grid(glm::dvec3(p.x, p.y, p.z), glm::normalize(glm::dquat(q.w, q.x, q.y, q.z)));
    glm::dmat4 fixed_from_grid = prepared.fixed_from_source.matrix() * source_from_grid.matrix();
    fixed_from_grid = glm::scale(
        fixed_from_grid, glm::dvec3(
                             grid.resolution * static_cast<double>(grid.width),
                             grid.resolution * static_cast<double>(grid.height), 1.0));
    fixed_from_grid[3].x -= render_origin.x;
    fixed_from_grid[3].y -= render_origin.y;
    fixed_from_grid[3].z -= render_origin.z;

    OccupancyUniforms occupancy_uniforms;
    copyMatrix(clip_correction * toQMatrix(view_projection), occupancy_uniforms.view_projection);
    copyMatrix(toQMatrix(glm::mat4(fixed_from_grid)), occupancy_uniforms.fixed_from_grid);
    occupancy_uniforms.display_params = {
        layer->opacity(), layer->colorScheme() == WasmOccupancyGridLayer::ColorScheme::kCostmap ? 1.0F : 0.0F, 0.0F,
        0.0F};
    updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kOccupancyUniformBytes, &occupancy_uniforms);
    occupancy_draws.push_back(OccupancyDraw{layer, gpu.shader_resources});
    ++last_occupancy_layer_count_;
    last_occupancy_cell_count_ += static_cast<int>(grid.cells.size());
  }

  struct VoxelDraw {
    WasmVoxelGridLayer* layer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_count = 0;
  };
  std::vector<VoxelDraw> voxel_draws;
  voxel_draws.reserve(prepared_voxels.size());
  last_voxel_layer_count_ = 0;
  last_voxel_count_ = 0;
  for (const PreparedVoxelLayer& prepared : prepared_voxels) {
    WasmVoxelGridLayer* layer = prepared.layer;
    VoxelLayerGpu& gpu = voxel_layer_gpu_[layer];
    bool texture_recreated = false;
    if (!ensureVoxelLayerGpu(current_rhi, layer, gpu, texture_recreated)) {
      continue;
    }
    if (texture_recreated || gpu.uploaded_revision != layer->textureRevision()) {
      const std::uint64_t slice_voxels = static_cast<std::uint64_t>(layer->columnCount()) * layer->rowCount();
      const std::uint64_t slice_bytes = slice_voxels * 4U;
      QVarLengthArray<QRhiTextureUploadEntry, 32> entries;
      entries.reserve(static_cast<qsizetype>(layer->sliceCount()));
      for (std::uint32_t slice = 0; slice < layer->sliceCount(); ++slice) {
        const std::uint64_t offset = static_cast<std::uint64_t>(slice) * slice_bytes;
        const void* data =
            layer->valueKind() == VoxelValueKind::kScalar
                ? static_cast<const void*>(reinterpret_cast<const std::uint8_t*>(layer->scalarVolume().data()) + offset)
                : static_cast<const void*>(layer->rgbaVolume().data() + offset);
        QRhiTextureSubresourceUploadDescription description(data, static_cast<quint32>(slice_bytes));
        description.setSourceSize(QSize(static_cast<int>(layer->columnCount()), static_cast<int>(layer->rowCount())));
        entries.append(QRhiTextureUploadEntry(static_cast<int>(slice), 0, description));
      }
      QRhiTextureUploadDescription upload;
      upload.setEntries(entries.cbegin(), entries.cend());
      updates->uploadTexture(gpu.texture, upload);
      gpu.uploaded_revision = layer->textureRevision();
      ++voxel_full_upload_count_;
    }

    const auto& p = layer->origin().position;
    const auto& q = layer->origin().orientation;
    const Transform source_from_grid(glm::dvec3(p.x, p.y, p.z), glm::normalize(glm::dquat(q.w, q.x, q.y, q.z)));
    glm::dmat4 fixed_from_grid = prepared.fixed_from_source.matrix() * source_from_grid.matrix();
    fixed_from_grid[3].x -= render_origin.x;
    fixed_from_grid[3].y -= render_origin.y;
    fixed_from_grid[3].z -= render_origin.z;

    const glm::vec3 cell_size = layer->cellSize();
    const auto [color_minimum, color_maximum] = layer->colorRange();
    VoxelUniforms voxel_uniforms;
    copyMatrix(clip_correction * toQMatrix(view_projection), voxel_uniforms.view_projection);
    copyMatrix(toQMatrix(glm::mat4(fixed_from_grid)), voxel_uniforms.fixed_from_grid);
    copyMatrix(toQMatrix(view), voxel_uniforms.view);
    voxel_uniforms.cell_size_opacity = {cell_size.x, cell_size.y, cell_size.z, layer->opacity()};
    voxel_uniforms.dims_kind = {
        static_cast<float>(layer->columnCount()), static_cast<float>(layer->rowCount()),
        static_cast<float>(layer->sliceCount()), layer->valueKind() == VoxelValueKind::kRgba ? 1.0F : 0.0F};
    voxel_uniforms.range_params = {layer->threshold(), layer->rangeMinimum(), layer->rangeMaximum(), color_minimum};
    voxel_uniforms.mode_params = {
        color_maximum, static_cast<float>(layer->drawMode()), static_cast<float>(layer->colormap()), 0.0F};
    updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kVoxelUniformBytes, &voxel_uniforms);
    voxel_draws.push_back(VoxelDraw{layer, gpu.shader_resources, static_cast<quint32>(layer->voxelCount())});
    ++last_voxel_layer_count_;
    last_voxel_count_ += static_cast<int>(layer->voxelCount());
  }

  if (shadow_active) {
    command_buffer->beginPass(shadow_render_target_, Qt::transparent, {1.0F, 0}, updates);
    command_buffer->setGraphicsPipeline(shadow_pipeline_);
    command_buffer->setViewport(
        QRhiViewport(0.0F, 0.0F, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize)));
    command_buffer->setShaderResources(shadow_shader_resources_);
    for (const ShadowDrawRange& range : shadow_draws) {
      if (range.mesh == nullptr || range.mesh->vertex_buffer == nullptr ||
          range.mesh->triangle_index_buffer == nullptr || range.index_count == 0U) {
        continue;
      }
      const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
          QRhiCommandBuffer::VertexInput(range.mesh->vertex_buffer, 0),
          QRhiCommandBuffer::VertexInput(
              shadow_instance_buffer_, range.instance_offset * static_cast<quint32>(sizeof(ShadowInstance))),
      };
      command_buffer->setVertexInput(
          0, static_cast<int>(inputs.size()), inputs.data(), range.mesh->triangle_index_buffer, 0,
          QRhiCommandBuffer::IndexUInt32);
      command_buffer->drawIndexed(range.index_count, 1U);
    }
    command_buffer->endPass();
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
  const QRhiViewport viewport(
      0.0F, 0.0F, static_cast<float>(output_size.width()), static_cast<float>(output_size.height()));
  enum class ScenePass { kDirect, kHdrColor, kDepthReplay };
  const auto pipeline_for =
      [this](QRhiGraphicsPipeline* direct, QRhiGraphicsPipeline* hdr, ScenePass scene_pass) -> QRhiGraphicsPipeline* {
    if (scene_pass == ScenePass::kHdrColor) {
      return hdr;
    }
    if (scene_pass == ScenePass::kDepthReplay) {
      const auto iterator = hdr_depth_pipelines_.find(direct);
      return iterator != hdr_depth_pipelines_.end() ? iterator->second : nullptr;
    }
    return direct;
  };
  const auto render_scene_pass = [&](QRhiRenderTarget* scene_target, ScenePass scene_pass, const QColor& clear,
                                     QRhiResourceUpdateBatch* pass_updates) {
    command_buffer->beginPass(scene_target, clear, {1.0F, 0}, pass_updates);
    last_submitted_layer_ids_.clear();
    if (buffers_ready && shader_resources_ != nullptr) {
      // Native grades the grid as data, then draws TF connections/axes with the
      // annotation alpha blend. Keep the ranges distinct even though they share
      // one retained vertex buffer.
      if (grid_triangle_vertex_count_ != 0U && triangle_pipeline_ != nullptr) {
        QRhiGraphicsPipeline* grid_pipeline = nullptr;
        if (shadow_active) {
          grid_pipeline = pipeline_for(grid_shadow_pipeline_, hdr_grid_shadow_pipeline_, scene_pass);
        } else {
          grid_pipeline = pipeline_for(triangle_pipeline_, hdr_triangle_pipeline_, scene_pass);
        }
        command_buffer->setGraphicsPipeline(grid_pipeline);
        command_buffer->setViewport(viewport);
        command_buffer->setShaderResources(shadow_active ? grid_shadow_shader_resources_ : shader_resources_);
        const QRhiCommandBuffer::VertexInput input(triangle_buffer_, 0);
        command_buffer->setVertexInput(0, 1, &input);
        command_buffer->draw(grid_triangle_vertex_count_);
      }
      if (grid_line_vertex_count_ != 0U && line_pipeline_ != nullptr) {
        command_buffer->setGraphicsPipeline(pipeline_for(line_pipeline_, hdr_line_pipeline_, scene_pass));
        command_buffer->setViewport(viewport);
        command_buffer->setShaderResources(shader_resources_);
        const QRhiCommandBuffer::VertexInput input(line_buffer_, 0);
        command_buffer->setVertexInput(0, 1, &input);
        command_buffer->draw(grid_line_vertex_count_);
      }
      const quint32 annotation_line_vertices = prelude_line_vertices - grid_line_vertex_count_;
      if (annotation_line_vertices != 0U && line_pipeline_ != nullptr) {
        command_buffer->setGraphicsPipeline(pipeline_for(line_pipeline_, hdr_annotation_line_pipeline_, scene_pass));
        command_buffer->setViewport(viewport);
        command_buffer->setShaderResources(annotation_shader_resources_);
        const QRhiCommandBuffer::VertexInput input(
            line_buffer_, grid_line_vertex_count_ * static_cast<quint32>(sizeof(Vertex)));
        command_buffer->setVertexInput(0, 1, &input);
        command_buffer->draw(annotation_line_vertices);
      }
      if (tf_triad_instance_count != 0U && pose_pipeline_ != nullptr && pose_vertex_buffer_ != nullptr &&
          pose_index_buffer_ != nullptr && tf_triad_gpu_.instance_buffer != nullptr &&
          tf_triad_gpu_.shader_resources != nullptr) {
        command_buffer->setGraphicsPipeline(pipeline_for(pose_pipeline_, hdr_pose_pipeline_, scene_pass));
        command_buffer->setViewport(viewport);
        command_buffer->setShaderResources(tf_triad_gpu_.shader_resources);
        const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
            QRhiCommandBuffer::VertexInput(pose_vertex_buffer_, 0),
            QRhiCommandBuffer::VertexInput(tf_triad_gpu_.instance_buffer, 0),
        };
        command_buffer->setVertexInput(
            0, static_cast<int>(inputs.size()), inputs.data(), pose_index_buffer_, 0, QRhiCommandBuffer::IndexUInt32);
        command_buffer->drawIndexed(pose_index_count_, tf_triad_instance_count);
      }

      // Preparation and GPU ownership remain family-specific, but command
      // submission follows the dock's one heterogeneous order exactly. Each
      // prepared family vector retains that order, so monotonic cursors keep the
      // submission pass linear while naturally skipping non-drawable layers.
      std::size_t point_draw_index = 0;
      std::size_t pose_draw_index = 0;
      std::size_t occupancy_draw_index = 0;
      std::size_t voxel_draw_index = 0;
      std::size_t marker_draw_index = 0;
      std::size_t model_draw_index = 0;
      const auto submit_model_layer = [&](WasmModelRenderable* layer) {
        if (model_draw_index == model_draws.size() || model_draws[model_draw_index].layer != layer) {
          return false;
        }
        bool submitted = false;
        const ModelLayerDraw& model_draw = model_draws[model_draw_index++];
        for (const ModelDrawRange& range : model_draw.ranges) {
          if (range.mesh == nullptr || range.material == nullptr) {
            continue;
          }
          QRhiGraphicsPipeline* direct_pipeline =
              range.lines ? (range.depth_write ? model_line_pipeline_ : model_line_no_depth_pipeline_)
                          : (range.depth_write ? model_triangle_pipeline_ : model_triangle_no_depth_pipeline_);
          QRhiGraphicsPipeline* hdr_pipeline =
              range.lines ? (range.depth_write ? hdr_model_line_pipeline_ : hdr_model_line_no_depth_pipeline_)
                          : (range.depth_write ? hdr_model_triangle_pipeline_ : hdr_model_triangle_no_depth_pipeline_);
          QRhiGraphicsPipeline* pipeline = pipeline_for(direct_pipeline, hdr_pipeline, scene_pass);
          QRhiBuffer* index_buffer = range.lines ? range.mesh->edge_index_buffer : range.mesh->triangle_index_buffer;
          if (pipeline == nullptr || range.mesh->vertex_buffer == nullptr || index_buffer == nullptr ||
              range.material->shader_resources == nullptr || model_instance_buffer_ == nullptr ||
              range.index_count == 0U) {
            continue;
          }
          const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
              QRhiCommandBuffer::VertexInput(range.mesh->vertex_buffer, 0),
              QRhiCommandBuffer::VertexInput(
                  model_instance_buffer_, range.instance_offset * static_cast<quint32>(sizeof(ModelInstance))),
          };
          command_buffer->setGraphicsPipeline(pipeline);
          command_buffer->setViewport(viewport);
          command_buffer->setShaderResources(range.material->shader_resources);
          command_buffer->setVertexInput(
              0, static_cast<int>(inputs.size()), inputs.data(), index_buffer,
              range.index_offset * static_cast<quint32>(sizeof(quint32)), QRhiCommandBuffer::IndexUInt32);
          command_buffer->drawIndexed(range.index_count, 1U);
          submitted = true;
        }
        return submitted;
      };
      for (const OrderedLayerEntry& ordered : ordered_layers_) {
        std::visit(
            [&](auto* layer) {
              using Layer = std::remove_pointer_t<decltype(layer)>;
              if constexpr (std::is_same_v<Layer, WasmPointRenderable>) {
                if (point_pipeline_ == nullptr || cube_pipeline_ == nullptr) {
                  return;
                }
                if (point_draw_index == point_draws.size() || point_draws[point_draw_index].layer != layer) {
                  return;
                }
                const PointDraw& draw = point_draws[point_draw_index++];
                command_buffer->setGraphicsPipeline(
                    draw.cubes ? pipeline_for(cube_pipeline_, hdr_cube_pipeline_, scene_pass)
                               : pipeline_for(point_pipeline_, hdr_point_pipeline_, scene_pass));
                command_buffer->setViewport(viewport);
                command_buffer->setShaderResources(draw.shader_resources);
                if (draw.cubes) {
                  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
                      QRhiCommandBuffer::VertexInput(cube_vertex_buffer_, 0),
                      QRhiCommandBuffer::VertexInput(draw.vertex_buffer, 0),
                  };
                  command_buffer->setVertexInput(
                      0, static_cast<int>(inputs.size()), inputs.data(), cube_index_buffer_, 0,
                      QRhiCommandBuffer::IndexUInt16);
                  command_buffer->drawIndexed(static_cast<quint32>(kCubeIndices.size()), draw.vertex_count);
                } else {
                  const QRhiCommandBuffer::VertexInput input(draw.vertex_buffer, 0);
                  command_buffer->setVertexInput(0, 1, &input);
                  command_buffer->draw(draw.vertex_count);
                }
                last_submitted_layer_ids_.push_back(ordered.topic_id);
              } else if constexpr (std::is_same_v<Layer, WasmPosesInFrameLayer>) {
                if (pose_pipeline_ == nullptr || pose_vertex_buffer_ == nullptr || pose_index_buffer_ == nullptr) {
                  return;
                }
                if (pose_draw_index == pose_draws.size() || pose_draws[pose_draw_index].layer != layer) {
                  return;
                }
                const PoseDraw& draw = pose_draws[pose_draw_index++];
                command_buffer->setGraphicsPipeline(pipeline_for(pose_pipeline_, hdr_pose_pipeline_, scene_pass));
                command_buffer->setViewport(viewport);
                command_buffer->setShaderResources(draw.shader_resources);
                const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
                    QRhiCommandBuffer::VertexInput(pose_vertex_buffer_, 0),
                    QRhiCommandBuffer::VertexInput(draw.instance_buffer, 0),
                };
                command_buffer->setVertexInput(
                    0, static_cast<int>(inputs.size()), inputs.data(), pose_index_buffer_, 0,
                    QRhiCommandBuffer::IndexUInt32);
                command_buffer->drawIndexed(pose_index_count_, draw.instance_count);
                last_submitted_layer_ids_.push_back(ordered.topic_id);
              } else if constexpr (std::is_same_v<Layer, WasmOccupancyGridLayer>) {
                if (occupancy_pipeline_ == nullptr || occupancy_quad_buffer_ == nullptr) {
                  return;
                }
                if (occupancy_draw_index == occupancy_draws.size() ||
                    occupancy_draws[occupancy_draw_index].layer != layer) {
                  return;
                }
                const OccupancyDraw& draw = occupancy_draws[occupancy_draw_index++];
                command_buffer->setGraphicsPipeline(
                    pipeline_for(occupancy_pipeline_, hdr_occupancy_pipeline_, scene_pass));
                command_buffer->setViewport(viewport);
                command_buffer->setShaderResources(draw.shader_resources);
                const QRhiCommandBuffer::VertexInput input(occupancy_quad_buffer_, 0);
                command_buffer->setVertexInput(0, 1, &input);
                command_buffer->draw(6U);
                last_submitted_layer_ids_.push_back(ordered.topic_id);
              } else if constexpr (std::is_same_v<Layer, WasmVoxelGridLayer>) {
                if (voxel_pipeline_ == nullptr || cube_vertex_buffer_ == nullptr || cube_index_buffer_ == nullptr) {
                  return;
                }
                if (voxel_draw_index == voxel_draws.size() || voxel_draws[voxel_draw_index].layer != layer) {
                  return;
                }
                const VoxelDraw& draw = voxel_draws[voxel_draw_index++];
                command_buffer->setGraphicsPipeline(pipeline_for(voxel_pipeline_, hdr_voxel_pipeline_, scene_pass));
                command_buffer->setViewport(viewport);
                command_buffer->setShaderResources(draw.shader_resources);
                const QRhiCommandBuffer::VertexInput input(cube_vertex_buffer_, 0);
                command_buffer->setVertexInput(0, 1, &input, cube_index_buffer_, 0, QRhiCommandBuffer::IndexUInt16);
                command_buffer->drawIndexed(static_cast<quint32>(kCubeIndices.size()), draw.instance_count);
                last_submitted_layer_ids_.push_back(ordered.topic_id);
              } else if constexpr (std::is_same_v<Layer, WasmSceneEntitiesLayer>) {
                bool submitted = false;
                if (marker_draw_index != marker_draws.size() && marker_draws[marker_draw_index].layer == layer) {
                  const MarkerLayerDraw& marker_draw = marker_draws[marker_draw_index++];
                  for (const MarkerDrawRange& range : marker_draw.ranges) {
                    QRhiGraphicsPipeline* pipeline = nullptr;
                    if (range.kind == MarkerDrawKind::kInstanceLines) {
                      pipeline =
                          range.depth_write
                              ? pipeline_for(marker_line_pipeline_, hdr_marker_line_pipeline_, scene_pass)
                              : pipeline_for(
                                    marker_line_no_depth_pipeline_, hdr_marker_line_no_depth_pipeline_, scene_pass);
                    } else if (range.kind == MarkerDrawKind::kInstanceTriangles) {
                      pipeline =
                          range.cull_back
                              ? (range.depth_write ? pipeline_for(
                                                         marker_triangle_cull_pipeline_,
                                                         hdr_marker_triangle_cull_pipeline_, scene_pass)
                                                   : pipeline_for(
                                                         marker_triangle_cull_no_depth_pipeline_,
                                                         hdr_marker_triangle_cull_no_depth_pipeline_, scene_pass))
                              : (range.depth_write
                                     ? pipeline_for(
                                           marker_triangle_pipeline_, hdr_marker_triangle_pipeline_, scene_pass)
                                     : pipeline_for(
                                           marker_triangle_no_depth_pipeline_, hdr_marker_triangle_no_depth_pipeline_,
                                           scene_pass));
                    } else if (range.kind == MarkerDrawKind::kStreamLines) {
                      pipeline = range.depth_write
                                     ? pipeline_for(line_pipeline_, hdr_line_pipeline_, scene_pass)
                                     : pipeline_for(line_no_depth_pipeline_, hdr_line_no_depth_pipeline_, scene_pass);
                    } else {
                      pipeline =
                          range.depth_write
                              ? pipeline_for(triangle_pipeline_, hdr_triangle_pipeline_, scene_pass)
                              : pipeline_for(triangle_no_depth_pipeline_, hdr_triangle_no_depth_pipeline_, scene_pass);
                    }
                    if (pipeline == nullptr || range.count == 0U) {
                      continue;
                    }
                    command_buffer->setGraphicsPipeline(pipeline);
                    command_buffer->setViewport(viewport);
                    const bool stream_geometry =
                        range.kind == MarkerDrawKind::kStreamLines || range.kind == MarkerDrawKind::kStreamTriangles;
                    command_buffer->setShaderResources(
                        stream_geometry ? annotation_shader_resources_ : shader_resources_);
                    if (range.kind == MarkerDrawKind::kInstanceLines ||
                        range.kind == MarkerDrawKind::kInstanceTriangles) {
                      if (range.mesh >= marker_meshes_.size() || marker_instance_buffer_ == nullptr) {
                        continue;
                      }
                      const MarkerMeshGpu& mesh = marker_meshes_[range.mesh];
                      const bool lines = range.kind == MarkerDrawKind::kInstanceLines;
                      QRhiBuffer* index_buffer = lines ? mesh.edge_index_buffer : mesh.triangle_index_buffer;
                      const quint32 index_count = lines ? mesh.edge_index_count : mesh.triangle_index_count;
                      if (mesh.vertex_buffer == nullptr || index_buffer == nullptr || index_count == 0U) {
                        continue;
                      }
                      const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
                          QRhiCommandBuffer::VertexInput(mesh.vertex_buffer, 0),
                          QRhiCommandBuffer::VertexInput(
                              marker_instance_buffer_, range.offset * static_cast<quint32>(sizeof(MarkerInstance))),
                      };
                      command_buffer->setVertexInput(
                          0, static_cast<int>(inputs.size()), inputs.data(), index_buffer, 0,
                          QRhiCommandBuffer::IndexUInt32);
                      command_buffer->drawIndexed(index_count, range.count);
                    } else {
                      QRhiBuffer* buffer = range.kind == MarkerDrawKind::kStreamLines ? line_buffer_ : triangle_buffer_;
                      const QRhiCommandBuffer::VertexInput input(
                          buffer, range.offset * static_cast<quint32>(sizeof(Vertex)));
                      command_buffer->setVertexInput(0, 1, &input);
                      command_buffer->draw(range.count);
                    }
                    submitted = true;
                  }
                }
                submitted = submit_model_layer(layer) || submitted;
                if (submitted) {
                  last_submitted_layer_ids_.push_back(ordered.topic_id);
                }
              } else if constexpr (std::is_same_v<Layer, WasmModelRenderable>) {
                if (submit_model_layer(layer)) {
                  last_submitted_layer_ids_.push_back(ordered.topic_id);
                }
              }
            },
            ordered.layer);
      }
    }
    command_buffer->endPass();
  };

  if (hdr_active) {
    render_scene_pass(hdr_render_target_, ScenePass::kHdrColor, hdr_scene_clear, updates);
    updates = nullptr;
    // Reusing the same immutable HDR mode keeps both passes independent of
    // dynamic-buffer update ordering. The replay's RGBA8 alpha records the
    // shaders' post-discard opacity while its D32F attachment records depth.
    render_scene_pass(hdr_depth_render_target_, ScenePass::kDepthReplay, Qt::transparent, nullptr);
    if (ssao_active && ssao_raw_render_target_ != nullptr) {
      // Preserve replay alpha/red and write native's raw scene-wide AO into the
      // otherwise-unused green channel. The sampled D32F texture is not attached
      // to this color-only target, avoiding a read/write feedback loop.
      command_buffer->beginPass(ssao_raw_render_target_, Qt::transparent, {1.0F, 0});
      command_buffer->setGraphicsPipeline(ssao_pipeline_);
      command_buffer->setViewport(viewport);
      command_buffer->setShaderResources(ssao_shader_resources_);
      command_buffer->draw(3U);
      command_buffer->endPass();
    }
    if (edl_active && edl_mesh_mask_render_target_ != nullptr) {
      // Preserve the full-scene replay depth and alpha coverage, then reserve
      // coverage.r for native's mesh-only EDL mask. Reusing the prepared model
      // ranges keeps this visibility pass outside all per-frame model budgets.
      command_buffer->beginPass(edl_mesh_mask_render_target_, Qt::transparent, {1.0F, 0});
      for (const ModelLayerDraw& model_draw : model_draws) {
        for (const ModelDrawRange& range : model_draw.ranges) {
          if (range.mesh == nullptr || range.material == nullptr || range.index_count == 0U) {
            continue;
          }
          QRhiGraphicsPipeline* pipeline =
              range.lines ? edl_mesh_mask_line_pipeline_ : edl_mesh_mask_triangle_pipeline_;
          QRhiBuffer* index_buffer = range.lines ? range.mesh->edge_index_buffer : range.mesh->triangle_index_buffer;
          if (pipeline == nullptr || range.mesh->vertex_buffer == nullptr || index_buffer == nullptr ||
              range.material->shader_resources == nullptr || model_instance_buffer_ == nullptr) {
            continue;
          }
          const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
              QRhiCommandBuffer::VertexInput(range.mesh->vertex_buffer, 0),
              QRhiCommandBuffer::VertexInput(
                  model_instance_buffer_, range.instance_offset * static_cast<quint32>(sizeof(ModelInstance))),
          };
          command_buffer->setGraphicsPipeline(pipeline);
          command_buffer->setViewport(viewport);
          command_buffer->setShaderResources(range.material->shader_resources);
          command_buffer->setVertexInput(
              0, static_cast<int>(inputs.size()), inputs.data(), index_buffer,
              range.index_offset * static_cast<quint32>(sizeof(quint32)), QRhiCommandBuffer::IndexUInt32);
          command_buffer->drawIndexed(range.index_count, 1U);
        }
      }
      command_buffer->endPass();
    }
    command_buffer->beginPass(target, Qt::transparent, {1.0F, 0});
    command_buffer->setGraphicsPipeline(present_pipeline_);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(present_shader_resources_);
    command_buffer->draw(3U);
    command_buffer->endPass();
  } else {
    // Direct-to-backing fallback: no HDR encode, so use the theme token as-is.
    render_scene_pass(target, ScenePass::kDirect, scene_bg, updates);
  }
}

void SceneViewWidget::releaseResources() {
  releaseShadowResources(false);
  releaseHdrResources();
  for (auto& [layer, gpu] : point_layer_gpu_) {
    (void)layer;
    releasePointLayerGpu(gpu);
  }
  point_layer_gpu_.clear();
  for (auto& [layer, gpu] : pose_layer_gpu_) {
    (void)layer;
    releasePoseLayerGpu(gpu);
  }
  pose_layer_gpu_.clear();
  releasePoseLayerGpu(tf_triad_gpu_);
  for (auto& [layer, gpu] : occupancy_layer_gpu_) {
    (void)layer;
    releaseOccupancyLayerGpu(gpu);
  }
  occupancy_layer_gpu_.clear();
  for (auto& [layer, gpu] : voxel_layer_gpu_) {
    (void)layer;
    releaseVoxelLayerGpu(gpu);
  }
  voxel_layer_gpu_.clear();
  for (MarkerMeshGpu& mesh : marker_meshes_) {
    releaseMarkerMeshGpu(mesh);
  }
  for (auto& [layer, gpu] : model_layer_gpu_) {
    (void)layer;
    releaseModelLayerGpu(gpu);
  }
  model_layer_gpu_.clear();
  delete model_line_no_depth_pipeline_;
  delete model_line_pipeline_;
  delete model_triangle_no_depth_pipeline_;
  delete model_triangle_pipeline_;
  delete model_layout_shader_resources_;
  delete model_sampler_;
  delete model_white_texture_;
  delete model_uniform_buffer_;
  delete model_instance_buffer_;
  delete marker_line_no_depth_pipeline_;
  delete marker_line_pipeline_;
  delete marker_triangle_cull_no_depth_pipeline_;
  delete marker_triangle_cull_pipeline_;
  delete marker_triangle_no_depth_pipeline_;
  delete marker_triangle_pipeline_;
  delete marker_instance_buffer_;
  delete voxel_pipeline_;
  delete voxel_layout_shader_resources_;
  delete voxel_layout_uniform_buffer_;
  delete voxel_sampler_;
  delete voxel_layout_texture_;
  delete occupancy_pipeline_;
  delete occupancy_layout_shader_resources_;
  delete occupancy_layout_uniform_buffer_;
  delete occupancy_quad_buffer_;
  delete occupancy_sampler_;
  delete occupancy_layout_texture_;
  delete pose_pipeline_;
  delete pose_layout_shader_resources_;
  delete pose_layout_uniform_buffer_;
  delete pose_index_buffer_;
  delete pose_vertex_buffer_;
  delete cube_pipeline_;
  delete point_pipeline_;
  delete point_layout_shader_resources_;
  delete point_layout_uniform_buffer_;
  delete cube_index_buffer_;
  delete cube_vertex_buffer_;
  delete colormap_sampler_;
  delete colormap_texture_;
  delete line_pipeline_;
  delete triangle_pipeline_;
  delete line_no_depth_pipeline_;
  delete triangle_no_depth_pipeline_;
  delete annotation_shader_resources_;
  delete shader_resources_;
  delete line_buffer_;
  delete triangle_buffer_;
  delete annotation_render_mode_uniform_buffer_;
  delete render_mode_uniform_buffer_;
  delete uniform_buffer_;
  pose_pipeline_ = nullptr;
  pose_layout_shader_resources_ = nullptr;
  pose_layout_uniform_buffer_ = nullptr;
  pose_index_buffer_ = nullptr;
  pose_vertex_buffer_ = nullptr;
  pose_mesh_upload_pending_ = false;
  pose_index_count_ = 0;
  cube_pipeline_ = nullptr;
  point_pipeline_ = nullptr;
  point_layout_shader_resources_ = nullptr;
  point_layout_uniform_buffer_ = nullptr;
  cube_index_buffer_ = nullptr;
  cube_vertex_buffer_ = nullptr;
  colormap_sampler_ = nullptr;
  colormap_texture_ = nullptr;
  colormap_upload_pending_ = false;
  cube_upload_pending_ = false;
  line_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  line_no_depth_pipeline_ = nullptr;
  triangle_no_depth_pipeline_ = nullptr;
  annotation_shader_resources_ = nullptr;
  shader_resources_ = nullptr;
  line_buffer_ = nullptr;
  triangle_buffer_ = nullptr;
  annotation_render_mode_uniform_buffer_ = nullptr;
  render_mode_uniform_buffer_ = nullptr;
  uniform_buffer_ = nullptr;
  occupancy_pipeline_ = nullptr;
  occupancy_layout_shader_resources_ = nullptr;
  occupancy_layout_uniform_buffer_ = nullptr;
  occupancy_quad_buffer_ = nullptr;
  occupancy_sampler_ = nullptr;
  occupancy_layout_texture_ = nullptr;
  occupancy_quad_upload_pending_ = false;
  voxel_pipeline_ = nullptr;
  voxel_layout_shader_resources_ = nullptr;
  voxel_layout_uniform_buffer_ = nullptr;
  voxel_sampler_ = nullptr;
  voxel_layout_texture_ = nullptr;
  max_3d_texture_size_ = 0;
  marker_triangle_pipeline_ = nullptr;
  marker_triangle_no_depth_pipeline_ = nullptr;
  marker_triangle_cull_pipeline_ = nullptr;
  marker_triangle_cull_no_depth_pipeline_ = nullptr;
  marker_line_pipeline_ = nullptr;
  marker_line_no_depth_pipeline_ = nullptr;
  marker_instance_buffer_ = nullptr;
  marker_instance_buffer_capacity_ = 0;
  marker_mesh_upload_pending_ = false;
  model_triangle_pipeline_ = nullptr;
  model_triangle_no_depth_pipeline_ = nullptr;
  model_line_pipeline_ = nullptr;
  model_line_no_depth_pipeline_ = nullptr;
  model_layout_shader_resources_ = nullptr;
  model_sampler_ = nullptr;
  model_white_texture_ = nullptr;
  model_uniform_buffer_ = nullptr;
  model_instance_buffer_ = nullptr;
  model_instance_buffer_capacity_ = 0;
  model_white_upload_pending_ = false;
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
  last_shadow_draw_count_ = 0;
  last_shadow_triangle_count_ = 0;
  last_shadow_bounds_ = {};
  line_buffer_capacity_ = 0;
  triangle_buffer_capacity_ = 0;
  resource_rhi_ = nullptr;
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
    update();
  }
}

void SceneViewWidget::emitPresentationIfChanged(const CameraState& before) {
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

}  // namespace pj::scene3d
