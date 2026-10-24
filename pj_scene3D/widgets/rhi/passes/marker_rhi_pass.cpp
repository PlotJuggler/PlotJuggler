// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "marker_rhi_pass.h"

#include <QColor>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "pj_scene3d_core/scene_entities_budget.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kMinimumLayerBufferBytes = 1024U;
// A layer re-uploaded again within this many prepare() calls keeps its CPU
// expansion (pan, follow and streams upload about every frame, with the odd
// unchanged frame between); one idle longer gives it back.
constexpr std::uint64_t kExpansionRetainFrames = 4U;
// And so does a view that rendered no frame for this long.
constexpr int kExpansionRetainIdleMs = 250;
constexpr std::uint32_t kCubeMesh = 0U;
constexpr std::uint32_t kSphereMesh = 1U;
constexpr std::uint32_t kCylinderMesh = 2U;
constexpr std::uint32_t kArrowMesh = 3U;
constexpr std::uint32_t kCubeEdgeOverlayMesh = 4U;

static_assert(sizeof(MarkerRhiPass::Instance) == 96U);
static_assert(offsetof(MarkerRhiPass::Instance, r) == 64U);
static_assert(offsetof(MarkerRhiPass::Instance, bottom_scale) == 80U);
static_assert(sizeof(MarkerRhiPass::StreamVertex) == 7U * sizeof(float));
// Every per-view budget fills one quint32-sized buffer.
constexpr bool fitsBuffers(const SceneLimits& limits) {
  constexpr std::uint64_t kMaxBytes = std::numeric_limits<quint32>::max();
  return limits.max_marker_instances_per_view * sizeof(MarkerRhiPass::Instance) <= kMaxBytes &&
         limits.max_marker_stream_vertices_per_view * sizeof(MarkerRhiPass::StreamVertex) <= kMaxBytes;
}
static_assert(fitsBuffers(kBrowserSceneLimits));
static_assert(fitsBuffers(kDesktopSceneLimits));

struct UnitMesh {
  std::vector<CubeVertex> vertices;
  std::vector<std::uint32_t> triangle_indices;
  std::vector<std::uint32_t> edge_indices;
};

void buildTriangleEdges(UnitMesh& mesh) {
  mesh.edge_indices.reserve(mesh.triangle_indices.size() * 2U);
  for (std::size_t index = 0; index + 2U < mesh.triangle_indices.size(); index += 3U) {
    const std::uint32_t a = mesh.triangle_indices[index];
    const std::uint32_t b = mesh.triangle_indices[index + 1U];
    const std::uint32_t c = mesh.triangle_indices[index + 2U];
    mesh.edge_indices.insert(mesh.edge_indices.end(), {a, b, b, c, c, a});
  }
}

UnitMesh makeCube() {
  UnitMesh mesh;
  mesh.vertices.assign(kCubeVertices.cbegin(), kCubeVertices.cend());
  mesh.triangle_indices.assign(kCubeIndices.cbegin(), kCubeIndices.cend());
  buildTriangleEdges(mesh);
  return mesh;
}

// The twelve box edges as a line list, two vertices per edge; each vertex
// carries its edge's midpoint in the normal slot (marker_edge.vert's facing test).
UnitMesh makeCubeEdgeOverlay() {
  constexpr std::array<glm::vec3, 8> kCorners = {{
      {-0.5F, -0.5F, -0.5F},
      {0.5F, -0.5F, -0.5F},
      {0.5F, 0.5F, -0.5F},
      {-0.5F, 0.5F, -0.5F},
      {-0.5F, -0.5F, 0.5F},
      {0.5F, -0.5F, 0.5F},
      {0.5F, 0.5F, 0.5F},
      {-0.5F, 0.5F, 0.5F},
  }};
  constexpr std::array<std::uint32_t, 24> kEdges = {
      0U, 1U, 1U, 2U, 2U, 3U, 3U, 0U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 4U, 0U, 4U, 1U, 5U, 2U, 6U, 3U, 7U,
  };
  UnitMesh mesh;
  for (std::size_t index = 0; index < kEdges.size(); index += 2U) {
    const glm::vec3 a = kCorners[kEdges[index]];
    const glm::vec3 b = kCorners[kEdges[index + 1U]];
    const glm::vec3 centre = 0.5F * (a + b);
    for (const glm::vec3& end : {a, b}) {
      mesh.edge_indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
      mesh.vertices.push_back({end.x, end.y, end.z, centre.x, centre.y, centre.z});
    }
  }
  return mesh;
}

UnitMesh makeSphere() {
  UnitMesh mesh;
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

UnitMesh makeCylinder() {
  UnitMesh mesh;
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

UnitMesh makeArrow() {
  const ArrowMeshData source = buildArrowMesh(
      ArrowMeshParams{.length = 1.0F, .shaft_radius = 0.2F, .head_length = 0.3F, .head_radius = 0.5F, .segments = 16});
  UnitMesh mesh;
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

// Indexed by kCubeMesh .. kCubeEdgeOverlayMesh.
const std::array<UnitMesh, 5>& unitMeshes() {
  static const std::array<UnitMesh, 5> meshes{
      makeCube(), makeSphere(), makeCylinder(), makeArrow(), makeCubeEdgeOverlay()};
  return meshes;
}

// The unit-mesh + per-instance input of the marker shaders: mesh position (0),
// the mesh normal or edge centre (1), the instance model rows (2-5), its colour
// (6) and optionally its cone taper (7).
QRhiVertexInputLayout instanceLayout(bool taper) {
  using Instance = MarkerRhiPass::Instance;
  QRhiVertexInputLayout input;
  input.setBindings({
      QRhiVertexInputBinding(sizeof(CubeVertex)),
      QRhiVertexInputBinding(sizeof(Instance), QRhiVertexInputBinding::PerInstance),
  });
  std::vector<QRhiVertexInputAttribute> attributes{
      {0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)},
      {0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)},
  };
  // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
  for (int row = 0; row < 4; ++row) {
    attributes.emplace_back(1, 2 + row, QRhiVertexInputAttribute::Float4, static_cast<quint32>(16 * row), row);
  }
  attributes.emplace_back(1, 6, QRhiVertexInputAttribute::Float4, offsetof(Instance, r));
  if (taper) {
    attributes.emplace_back(1, 7, QRhiVertexInputAttribute::Float4, offsetof(Instance, bottom_scale));
  }
  input.setAttributes(attributes.cbegin(), attributes.cend());
  return input;
}

AABB transformedMarkerBounds(const MarkerBounds& source, const Transform& fixed_from_frame) {
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

}  // namespace

MarkerRhiPass::MarkerRhiPass() {
  expansion_release_timer_.setSingleShot(true);
  QObject::connect(&expansion_release_timer_, &QTimer::timeout, [this] {
    if (idle_release_paused_) {
      return;
    }
    // Armed once, not per frame: re-armed for the rest of the spell while frames still come.
    if (const qint64 idle_ms = last_retaining_prepare_.elapsed(); idle_ms < kExpansionRetainIdleMs) {
      expansion_release_timer_.start(static_cast<int>(kExpansionRetainIdleMs - idle_ms));
      return;
    }
    for (auto& [layer, gpu] : layer_gpu_) {
      (void)layer;
      gpu.cpu.reset();
    }
  });
}

bool MarkerRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.view_uniforms == nullptr || setup.annotation_render_mode_uniforms == nullptr) {
    return false;
  }
  data_bindings_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.render_mode_uniforms),
           });
  annotation_bindings_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.annotation_render_mode_uniforms),
           });
  if (!ensureUniformBuffer(rhi, camera_view_uniforms_, sizeof(glm::mat4))) {
    return false;
  }
  edge_bindings_ = createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
          QRhiShaderResourceBinding::uniformBuffer(4, QRhiShaderResourceBinding::VertexStage, camera_view_uniforms_),
          QRhiShaderResourceBinding::uniformBuffer(
              8, QRhiShaderResourceBinding::FragmentStage, setup.render_mode_uniforms),
      });
  if (data_bindings_ == nullptr || annotation_bindings_ == nullptr || edge_bindings_ == nullptr) {
    return false;
  }

  const QRhiVertexInputLayout instance_layout = instanceLayout(true);
  QRhiVertexInputLayout stream_layout;
  stream_layout.setBindings({QRhiVertexInputBinding(sizeof(StreamVertex))});
  stream_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(StreamVertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(StreamVertex, r)),
  });
  const QShader marker_vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/marker.vert.qsb"));
  const QShader marker_fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/marker.frag.qsb"));
  const QShader stream_vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/scene.vert.qsb"));
  const QShader stream_fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/scene.frag.qsb"));
  const QShader edge_vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/marker_edge.vert.qsb"));
  const auto instanced = [&](QRhiGraphicsPipeline::Topology topology, QRhiGraphicsPipeline::CullMode cull,
                             int depth_bias = 0) {
    return PipelineDesc{
        .topology = topology,
        .vertex = marker_vertex,
        .fragment = marker_fragment,
        .input_layout = instance_layout,
        .bindings = data_bindings_,
        .cull = cull,
        .depth_bias = depth_bias,
        .slope_scaled_depth_bias = static_cast<float>(depth_bias),
    };
  };
  const auto stream = [&](QRhiGraphicsPipeline::Topology topology) {
    return PipelineDesc{
        .topology = topology,
        .vertex = stream_vertex,
        .fragment = stream_fragment,
        .input_layout = stream_layout,
        .bindings = data_bindings_,
    };
  };
  // Indexed by Pipeline; each is built without and with depth writes.
  const std::array<PipelineDesc, kPipelineCount> descs{
      instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::None),
      instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back),
      instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back, 1),
      PipelineDesc{
          .topology = QRhiGraphicsPipeline::Lines,
          .vertex = edge_vertex,
          .fragment = marker_fragment,
          .input_layout = instanceLayout(false),
          .bindings = edge_bindings_,
      },
      instanced(QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::None),
      stream(QRhiGraphicsPipeline::Lines),
      stream(QRhiGraphicsPipeline::Triangles),
  };
  for (std::size_t index = 0; index < descs.size(); ++index) {
    PipelineDesc desc = descs[index];
    desc.depth_write = false;
    pipelines_[index][0] = setup.pipelines->add(desc, setup.part);
    desc.depth_write = true;
    pipelines_[index][1] = setup.pipelines->add(desc, setup.part);
  }
  if (!ready()) {
    return false;
  }

  const auto& mesh_data = unitMeshes();
  for (std::size_t index = 0; index < mesh_data.size(); ++index) {
    const UnitMesh& mesh = mesh_data[index];
    MeshGpu& gpu = meshes_[index];
    gpu.vertex_buffer = rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
        static_cast<quint32>(mesh.vertices.size() * sizeof(CubeVertex)));
    if (!mesh.triangle_indices.empty()) {
      gpu.triangle_index_buffer = rhi->newBuffer(
          QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
          static_cast<quint32>(mesh.triangle_indices.size() * sizeof(std::uint32_t)));
    }
    if (!mesh.edge_indices.empty()) {
      gpu.edge_index_buffer = rhi->newBuffer(
          QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
          static_cast<quint32>(mesh.edge_indices.size() * sizeof(std::uint32_t)));
    }
    if (!gpu.vertex_buffer->create() ||
        (gpu.triangle_index_buffer != nullptr && !gpu.triangle_index_buffer->create()) ||
        (gpu.edge_index_buffer != nullptr && !gpu.edge_index_buffer->create())) {
      return false;
    }
    gpu.triangle_index_count = static_cast<quint32>(mesh.triangle_indices.size());
    gpu.edge_index_count = static_cast<quint32>(mesh.edge_indices.size());
    setup.updates->uploadStaticBuffer(gpu.vertex_buffer, mesh.vertices.data());
    if (gpu.triangle_index_buffer != nullptr) {
      setup.updates->uploadStaticBuffer(gpu.triangle_index_buffer, mesh.triangle_indices.data());
    }
    if (gpu.edge_index_buffer != nullptr) {
      setup.updates->uploadStaticBuffer(gpu.edge_index_buffer, mesh.edge_indices.data());
    }
  }
  return true;
}

bool MarkerRhiPass::ready() const {
  return std::all_of(pipelines_.cbegin(), pipelines_.cend(), [](const auto& variants) {
    return variants[0] != nullptr && variants[1] != nullptr;
  });
}

void MarkerRhiPass::setLayers(const std::vector<SceneEntitiesLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

const Transform* MarkerRhiPass::placement(const Admitted& admitted, std::uint32_t frame_index) const {
  if (frame_index >= admitted.frame_count) {
    return nullptr;
  }
  const std::optional<Transform>& placed = placements_[admitted.first_frame + frame_index];
  return placed.has_value() ? &*placed : nullptr;
}

void MarkerRhiPass::admit(const PlaceFn& place) {
  admitted_.clear();
  placements_.clear();
  AABB family_bounds;
  std::uint64_t remaining_instances = kSceneLimits.max_marker_instances_per_view;
  std::uint64_t remaining_stream_vertices = kSceneLimits.max_marker_stream_vertices_per_view;
  for (SceneEntitiesLayer* layer : layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender();
    const MarkerGeometry& geometry = layer->geometry();
    if (geometry.empty()) {
      continue;
    }
    // Rejected layers leave their placements behind; nothing reads them.
    Admitted prepared{.layer = layer, .first_frame = placements_.size(), .frame_count = geometry.frames.size()};
    AABB layer_bounds;
    for (std::size_t index = 0; index < geometry.frames.size(); ++index) {
      // Cursor-placed (entry clock): entities carry their own stamps and frame_locked,
      // which header-clock placement would have to honour per entity. A finding is
      // placed at its stamp (renderStamp()), where it was measured.
      const std::optional<Transform>& placed = placements_.emplace_back(
          place(geometry.frames[index], layer->renderStamp(), layer->placementHoldFor(geometry.frames[index])));
      if (placed.has_value() && index < geometry.frame_bounds.size()) {
        layer_bounds = unionAABB(layer_bounds, transformedMarkerBounds(geometry.frame_bounds[index], *placed));
      }
    }
    const auto placed = [&](std::uint32_t frame_index) { return placement(prepared, frame_index) != nullptr; };
    // Every item's frame_index indexes geometry.frames: with all of them placed,
    // every item is.
    const bool all_placed = std::all_of(
        placements_.cbegin() + static_cast<std::ptrdiff_t>(prepared.first_frame), placements_.cend(),
        [](const std::optional<Transform>& placement) { return placement.has_value(); });
    const auto count_instances = [&placed, all_placed](const auto& instances) {
      if (all_placed) {
        return static_cast<std::uint64_t>(instances.size());
      }
      return static_cast<std::uint64_t>(std::count_if(
          instances.cbegin(), instances.cend(), [&placed](const auto& item) { return placed(item.frame_index); }));
    };
    const std::uint64_t cube_instances = count_instances(geometry.cubes);
    const std::uint64_t logical_instances = cube_instances + count_instances(geometry.spheres) +
                                            count_instances(geometry.cylinders) + count_instances(geometry.arrows) +
                                            count_instances(geometry.axes);
    const auto submitted_instances =
        markerSubmittedInstanceCount(logical_instances, cube_instances, layer->wireframe());
    if (!submitted_instances.has_value()) {
      layer->noteRenderFailure(tr("Marker instance accounting overflowed the view limit"));
      continue;
    }
    prepared.instances = *submitted_instances;
    for (const MarkerStreamBatch& batch : geometry.lines) {
      if (placed(batch.frame_index)) {
        prepared.stream_vertices += batch.vertexCount();
      }
    }
    for (const MarkerStreamBatch& batch : geometry.triangles) {
      if (placed(batch.frame_index)) {
        prepared.stream_vertices += batch.vertexCount() * (layer->wireframe() ? 2U : 1U);
      }
    }
    if (prepared.instances == 0U && prepared.stream_vertices == 0U) {
      continue;
    }
    if (!markerLayerCountsFit(prepared.instances, prepared.stream_vertices, geometry.frames.size())) {
      layer->noteRenderFailure(
          tr("Markers need %1 submitted instances and %2 streamed vertices; the layer limits are %3 and %4")
              .arg(
                  QString::number(prepared.instances), QString::number(prepared.stream_vertices),
                  QString::number(kSceneLimits.max_marker_instances_per_layer),
                  QString::number(kSceneLimits.max_marker_stream_vertices_per_layer)));
      continue;
    }
    if (prepared.instances > remaining_instances || prepared.stream_vertices > remaining_stream_vertices) {
      layer->noteRenderFailure(
          tr("Markers need %1 instances and %2 streamed vertices; only %3 and %4 remain in this view's budget")
              .arg(
                  QString::number(prepared.instances), QString::number(prepared.stream_vertices),
                  QString::number(remaining_instances), QString::number(remaining_stream_vertices)));
      continue;
    }
    if (!tryConsumeBudget(prepared.instances, remaining_instances) ||
        !tryConsumeBudget(prepared.stream_vertices, remaining_stream_vertices)) {
      continue;
    }
    layer->noteRenderSuccess();
    family_bounds = unionAABB(family_bounds, layer_bounds);
    admitted_.push_back(prepared);
  }
  last_bounds_ = family_bounds;
}

namespace {

// A layer key's colour rule: the override (opaque) when set, then the layer opacity.
glm::vec4 applyColor(const glm::vec4& source, const std::optional<glm::vec3>& override_color, float opacity) {
  glm::vec4 color = override_color.has_value() ? glm::vec4(*override_color, 1.0F) : source;
  color.a *= opacity;
  return color;
}

// The placement matrix of `frame_index`, or nullptr when unplaced.
const glm::dmat4* placedFrame(
    const std::vector<std::optional<glm::dmat4>>& fixed_from_frame, std::uint32_t frame_index) {
  if (frame_index >= fixed_from_frame.size() || !fixed_from_frame[frame_index].has_value()) {
    return nullptr;
  }
  return &*fixed_from_frame[frame_index];
}

}  // namespace

quint32 MarkerRhiPass::expandStreams(const MarkerGeometry& geometry, const LayerKey& key, Expansion& out) {
  const ContentKey& content = key.content;
  const glm::dvec3& render_origin = key.render_origin;
  // Places every point of `batch` once (a strip's inner points serve two
  // segments), with the arithmetic of a per-vertex placement: the double
  // transform, the double subtraction, then the float narrowing.
  const auto place = [&](const MarkerStreamBatch& batch) -> bool {
    const glm::dmat4* fixed_from_frame = placedFrame(out.fixed_from_frame, batch.frame_index);
    if (fixed_from_frame == nullptr) {
      return false;
    }
    const glm::dmat4 model = *fixed_from_frame * batch.model;
    out.placed_points.resize(batch.points.size());
    for (std::size_t index = 0; index < batch.points.size(); ++index) {
      out.placed_points[index] = glm::vec3(glm::dvec3(model * glm::dvec4(batch.points[index], 1.0)) - render_origin);
    }
    return true;
  };
  const auto vertex = [&](const MarkerStreamBatch& batch, const glm::vec4& uniform, std::uint32_t point) {
    const glm::vec3& position = out.placed_points[point];
    const glm::vec4 color = batch.colors.empty()
                                ? uniform
                                : applyColor(markerColor(batch.colors[point]), content.override_color, content.opacity);
    return StreamVertex{position.x, position.y, position.z, color.r, color.g, color.b, color.a};
  };
  const auto placed = [&](const MarkerStreamBatch& batch) {
    return placedFrame(out.fixed_from_frame, batch.frame_index) != nullptr;
  };
  std::size_t line_total = 0;
  std::size_t triangle_total = 0;
  for (const MarkerStreamBatch& batch : geometry.lines) {
    line_total += placed(batch) ? batch.vertexCount() : 0U;
  }
  const std::size_t line_count = line_total;
  for (const MarkerStreamBatch& batch : geometry.triangles) {
    if (placed(batch)) {
      (content.wireframe ? line_total : triangle_total) += batch.vertexCount() * (content.wireframe ? 2U : 1U);
    }
  }
  out.lines.resizeForOverwrite(line_total);
  out.triangles.resizeForOverwrite(triangle_total);
  StreamVertex* lines = out.lines.data();
  StreamVertex* triangles = out.triangles.data();
  for (const MarkerStreamBatch& batch : geometry.lines) {
    if (place(batch)) {
      const glm::vec4 uniform = applyColor(batch.color, content.override_color, content.opacity);
      for (const std::uint32_t point : batch.indices) {
        *lines++ = vertex(batch, uniform, point);
      }
    }
  }
  for (const MarkerStreamBatch& batch : geometry.triangles) {
    if (!place(batch)) {
      continue;
    }
    const glm::vec4 uniform = applyColor(batch.color, content.override_color, content.opacity);
    if (content.wireframe) {
      for (std::size_t index = 0; index + 2U < batch.indices.size(); index += 3U) {
        const StreamVertex a = vertex(batch, uniform, batch.indices[index]);
        const StreamVertex b = vertex(batch, uniform, batch.indices[index + 1U]);
        const StreamVertex c = vertex(batch, uniform, batch.indices[index + 2U]);
        for (const StreamVertex& edge_vertex : {a, b, b, c, c, a}) {
          *lines++ = edge_vertex;
        }
      }
    } else {
      for (const std::uint32_t point : batch.indices) {
        *triangles++ = vertex(batch, uniform, point);
      }
    }
  }
  return static_cast<quint32>(line_count);
}

void MarkerRhiPass::expandLayer(const MarkerGeometry& geometry, const LayerKey& key, bool keep_world, Expansion& out) {
  const ContentKey& content = key.content;
  out.instance_world.clear();
  out.draws.clear();
  out.fixed_from_frame.clear();
  // Upper bound (unplaced frames add nothing), truncated once the families are in.
  const auto instance_sources = static_cast<std::size_t>(geometry.instanceCount());
  out.instances.resizeForOverwrite(instance_sources);
  if (keep_world) {
    out.instance_world.reserve(instance_sources);
  }
  Instance* const instances = out.instances.data();
  quint32 instance_count = 0;
  for (const std::optional<Transform>& placement : content.placements) {
    out.fixed_from_frame.push_back(placement.has_value() ? std::optional(placement->matrix()) : std::nullopt);
  }
  const glm::dvec3& render_origin = key.render_origin;
  const bool pass_translucent = content.opacity < 0.999F;
  const auto add_range = [&](Kind kind, std::uint32_t mesh, quint32 offset, quint32 count, bool cull_back,
                             bool depth_write) {
    out.draws.push_back(
        {nullptr, nullptr, kind, mesh, offset, count, cull_back, depth_write,
         depth_write ? DrawClass::kOpaque : DrawClass::kTranslucent});
  };
  const auto append_instances = [&](const std::vector<MarkerInstanceSource>& sources) {
    const quint32 first = instance_count;
    for (const MarkerInstanceSource& source : sources) {
      const glm::dmat4* fixed_from_frame = placedFrame(out.fixed_from_frame, source.frame_index);
      if (fixed_from_frame == nullptr) {
        continue;
      }
      const glm::dmat4 world = *fixed_from_frame * source.model;
      Instance instance;
      copyMatrix(relativeModel(world, render_origin), instance.model);
      const glm::vec4 color = applyColor(source.color, content.override_color, content.opacity);
      instance.r = color.r;
      instance.g = color.g;
      instance.b = color.b;
      instance.a = color.a;
      instance.bottom_scale = source.bottom_scale;
      instance.top_scale = source.top_scale;
      instances[instance_count++] = instance;
      if (keep_world) {
        out.instance_world.emplace_back(world[3]);
      }
    }
    return std::pair{first, instance_count - first};
  };
  const auto add_instance_range = [&](std::uint32_t mesh, quint32 offset, quint32 count, bool cull_back,
                                      bool depth_write, bool as_lines) {
    if (count != 0U) {
      add_range(
          as_lines ? Kind::kInstanceLines : Kind::kInstanceTriangles, mesh, offset, count, cull_back, depth_write);
    }
  };

  // Native family order: cube fill + edge overlay, sphere, cylinder,
  // arrows/axes, user lines, user triangles.
  const auto [cube_offset, cube_count] = append_instances(geometry.cubes);
  bool cubes_translucent = pass_translucent;
  for (quint32 index = 0; index < cube_count; ++index) {
    cubes_translucent = cubes_translucent || instances[cube_offset + index].a < 0.999F;
  }
  if (content.wireframe) {
    add_instance_range(kCubeMesh, cube_offset, cube_count, false, !pass_translucent, true);
  } else {
    add_instance_range(kCubeMesh, cube_offset, cube_count, true, !cubes_translucent, false);
    // The normal cube overlay contains only the twelve geometric box
    // edges, over the fill's own instances. Explicit wireframe mode above still
    // uses the triangle mesh edges, including face diagonals, as a line polygon mode would.
    if (cube_count != 0U) {
      add_range(Kind::kCubeEdges, kCubeEdgeOverlayMesh, cube_offset, cube_count, false, !pass_translucent);
      if (cubes_translucent) {
        out.draws.back().draw_class = DrawClass::kTranslucent;
      }
    }
  }
  const auto append_family = [&](const auto& sources, std::uint32_t mesh, bool cull_back) {
    const auto [offset, count] = append_instances(sources);
    add_instance_range(
        mesh, offset, count, content.wireframe ? false : cull_back, !pass_translucent, content.wireframe);
  };
  append_family(geometry.spheres, kSphereMesh, true);
  append_family(geometry.cylinders, kCylinderMesh, true);
  append_family(geometry.arrows, kArrowMesh, false);
  append_family(geometry.axes, kArrowMesh, false);
  out.instances.truncate(instance_count);

  const quint32 line_count = expandStreams(geometry, key, out);
  if (line_count != 0U) {
    add_range(Kind::kStreamLines, 0U, 0U, line_count, false, !pass_translucent);
  }
  if (const quint32 edge_count = static_cast<quint32>(out.lines.size()) - line_count; edge_count != 0U) {
    add_range(Kind::kStreamLines, 0U, line_count, edge_count, false, !pass_translucent);
  }
  if (!out.triangles.empty()) {
    add_range(Kind::kStreamTriangles, 0U, 0U, static_cast<quint32>(out.triangles.size()), false, !pass_translucent);
  }
}

void MarkerRhiPass::moveOrigin(const MarkerGeometry& geometry, const LayerKey& key, Expansion& out) {
  const glm::dvec3& render_origin = key.render_origin;
  Instance* const instances = out.instances.data();
  for (std::size_t index = 0; index < out.instance_world.size(); ++index) {
    // relativeModel()'s arithmetic: the double subtraction, then the float narrowing.
    const glm::dvec3 translation = out.instance_world[index] - render_origin;
    instances[index].model[12] = static_cast<float>(translation.x);
    instances[index].model[13] = static_cast<float>(translation.y);
    instances[index].model[14] = static_cast<float>(translation.z);
  }
  (void)expandStreams(geometry, key, out);
}

bool MarkerRhiPass::uploadLayer(QRhi* rhi, QRhiResourceUpdateBatch* updates, SceneEntitiesLayer* layer, LayerGpu& gpu) {
  const auto bytes = [](const auto& values) { return static_cast<quint32>(values.bytes().size()); };
  const auto allocate = [&](QRhiBuffer*& buffer, quint32& capacity, quint32 required, bool stream) {
    if (required == 0U) {
      delete buffer;
      buffer = nullptr;
      capacity = 0;
      return true;
    }
    if (stream && fail_stream_allocation_) {
      return false;
    }
    return ensureStaticVertexBuffer(rhi, buffer, capacity, required, kMinimumLayerBufferBytes);
  };
  const Expansion& expansion = *gpu.cpu;
  if (!allocate(gpu.instances, gpu.instance_capacity, bytes(expansion.instances), false) ||
      !allocate(gpu.lines, gpu.line_capacity, bytes(expansion.lines), true) ||
      !allocate(gpu.triangles, gpu.triangle_capacity, bytes(expansion.triangles), true)) {
    releaseLayerGpu(gpu);
    return false;
  }
  const auto upload = [&](QRhiBuffer* buffer, const auto& values) {
    if (buffer != nullptr) {
      // Shares the bytes, one piece: UploadArray pads them for anv.
      updates->uploadStaticBuffer(buffer, 0, values.bytes());
    }
  };
  upload(gpu.instances, expansion.instances);
  upload(gpu.lines, expansion.lines);
  upload(gpu.triangles, expansion.triangles);
  gpu.draws = expansion.draws;
  for (Draw& draw : gpu.draws) {
    draw.layer = layer;
    draw.buffer = draw.kind == Kind::kStreamLines       ? gpu.lines
                  : draw.kind == Kind::kStreamTriangles ? gpu.triangles
                                                        : gpu.instances;
  }
  return true;
}

void MarkerRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.instances;
  delete gpu.lines;
  delete gpu.triangles;
  gpu = {};
}

void MarkerRhiPass::prepare(const RhiFrameContext& frame) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  last_layer_count_ = 0;
  last_instance_count_ = 0;
  last_stream_vertex_count_ = 0;
  QRhi* rhi = frame.rhi;
  QRhiResourceUpdateBatch* updates = frame.updates;

  updates->updateDynamicBuffer(camera_view_uniforms_, 0, sizeof(glm::mat4), &frame.view[0][0]);
  ++frame_serial_;
  for (const Admitted& prepared : admitted_) {
    SceneEntitiesLayer* layer = prepared.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    gpu.admitted_frame = frame_serial_;
    LayerKey& key = scratch_key_;
    ContentKey& content = key.content;
    content.geometry_revision = layer->geometryRevision();
    key.render_origin = frame.render_origin;
    content.opacity = layer->opacity();
    content.override_color.reset();
    if (layer->colorOverrideEnabled()) {
      const QColor color = layer->overrideColor();
      content.override_color = glm::vec3(
          static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF()));
    }
    content.wireframe = layer->wireframe();
    const auto first = placements_.cbegin() + static_cast<std::ptrdiff_t>(prepared.first_frame);
    content.placements.assign(first, first + static_cast<std::ptrdiff_t>(prepared.frame_count));
    if (!cache_enabled_ || gpu.key != key) {
      const bool recurring = gpu.key.has_value() && frame_serial_ - gpu.uploaded_frame <= kExpansionRetainFrames;
      if (cache_enabled_ && gpu.cpu.has_value() && gpu.key.has_value() && gpu.key->content == content) {
        moveOrigin(layer->geometry(), key, *gpu.cpu);
        ++origin_update_count_;
      } else {
        if (!gpu.cpu.has_value()) {
          gpu.cpu.emplace();
        }
        expandLayer(layer->geometry(), key, /*keep_world=*/recurring, *gpu.cpu);
      }
      if (!uploadLayer(rhi, updates, layer, gpu)) {
        continue;
      }
      gpu.key = key;
      gpu.uploaded_frame = frame_serial_;
      ++layer_rebuild_count_;
      if (!recurring) {
        gpu.cpu.reset();
      }
    } else if (frame_serial_ - gpu.uploaded_frame > kExpansionRetainFrames) {
      gpu.cpu.reset();
    }
    if (gpu.draws.empty()) {
      continue;
    }
    draws_.insert(draws_.end(), gpu.draws.cbegin(), gpu.draws.cend());
    ++last_layer_count_;
    last_instance_count_ += static_cast<int>(prepared.instances);
    last_stream_vertex_count_ += static_cast<int>(prepared.stream_vertices);
  }
  // Layers not admitted this frame (hidden, empty, unplaced, over budget) give their buffers back.
  std::erase_if(layer_gpu_, [this](auto& entry) {
    if (entry.second.admitted_frame == frame_serial_) {
      return false;
    }
    releaseLayerGpu(entry.second);
    return true;
  });
  if (std::any_of(
          layer_gpu_.cbegin(), layer_gpu_.cend(), [](const auto& entry) { return entry.second.cpu.has_value(); })) {
    last_retaining_prepare_.start();
    if (!expansion_release_timer_.isActive()) {
      expansion_release_timer_.start(kExpansionRetainIdleMs);
    }
  } else {
    expansion_release_timer_.stop();
  }
}

const ScenePipeline* MarkerRhiPass::instancePipeline(const Draw& draw) const {
  const Pipeline pipeline = [&draw] {
    switch (draw.kind) {
      case Kind::kInstanceLines:
        return kLinePipeline;
      case Kind::kCubeEdges:
        return kEdgePipeline;
      case Kind::kStreamLines:
        return kStreamLinePipeline;
      case Kind::kStreamTriangles:
        return kStreamTrianglePipeline;
      case Kind::kInstanceTriangles:
        break;
    }
    if (!draw.cull_back) {
      return kTrianglePipeline;
    }
    return draw.mesh == kCubeMesh ? kCubeFillPipeline : kTriangleCullPipeline;
  }();
  return pipelines_[pipeline][draw.depth_write ? 1 : 0];
}

bool MarkerRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(instancePipeline(draw), pass);
  if (pipeline == nullptr || draw.count == 0U) {
    return false;
  }
  if (draw.buffer == nullptr) {
    return false;
  }
  if (draw.kind == Kind::kStreamLines || draw.kind == Kind::kStreamTriangles) {
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(annotation_bindings_);
    const QRhiCommandBuffer::VertexInput input(draw.buffer, draw.offset * static_cast<quint32>(sizeof(StreamVertex)));
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(draw.count);
    return true;
  }
  if (draw.mesh >= meshes_.size()) {
    return false;
  }
  const MeshGpu& mesh = meshes_[draw.mesh];
  const bool lines = draw.kind == Kind::kInstanceLines || draw.kind == Kind::kCubeEdges;
  QRhiBuffer* index_buffer = lines ? mesh.edge_index_buffer : mesh.triangle_index_buffer;
  const quint32 index_count = lines ? mesh.edge_index_count : mesh.triangle_index_count;
  if (mesh.vertex_buffer == nullptr || index_buffer == nullptr || index_count == 0U) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.kind == Kind::kCubeEdges ? edge_bindings_ : data_bindings_);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(mesh.vertex_buffer, 0),
      QRhiCommandBuffer::VertexInput(draw.buffer, draw.offset * static_cast<quint32>(sizeof(Instance))),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), index_buffer, 0, QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(index_count, draw.count);
  return true;
}

void MarkerRhiPass::release() {
  expansion_release_timer_.stop();
  for (MeshGpu& mesh : meshes_) {
    delete mesh.edge_index_buffer;
    delete mesh.triangle_index_buffer;
    delete mesh.vertex_buffer;
    mesh = {};
  }
  pipelines_ = {};
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  delete annotation_bindings_;
  delete data_bindings_;
  delete edge_bindings_;
  delete camera_view_uniforms_;
  annotation_bindings_ = nullptr;
  data_bindings_ = nullptr;
  edge_bindings_ = nullptr;
  camera_view_uniforms_ = nullptr;
  admitted_.clear();
  placements_.clear();
  draws_.clear();
}

}  // namespace pj::scene3d
