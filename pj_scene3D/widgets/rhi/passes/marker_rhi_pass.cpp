// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "marker_rhi_pass.h"

#include <QColor>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#include "pj_scene3d_core/scene_entities_budget.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kInitialBufferBytes = 64U * 1024U;
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

QString tr(const char* text) {
  return QCoreApplication::translate("pj::scene3d::SceneViewWidget", text);
}

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

UnitMesh makeCubeEdgeOverlay() {
  UnitMesh mesh;
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

// Local bounds of each unit mesh, indexed like unitMeshes().
const std::array<AABB, 5>& unitMeshBounds() {
  static const std::array<AABB, 5> bounds = [] {
    std::array<AABB, 5> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
      for (const CubeVertex& vertex : unitMeshes()[index].vertices) {
        expandAABB(result[index], glm::vec3(vertex.px, vertex.py, vertex.pz));
      }
    }
    return result;
  }();
  return bounds;
}

// Camera-relative bounds of one caster instance: its unit mesh's local box,
// widened by the cone taper, under the instance model.
AABB instanceBounds(const MarkerRhiPass::Instance& instance, std::uint32_t mesh) {
  const AABB& local = unitMeshBounds()[mesh];
  const float taper = std::max({instance.bottom_scale, instance.top_scale, 1.0F});
  glm::mat4 model;
  std::memcpy(&model[0][0], instance.model.data(), sizeof(model));
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 point{
        ((corner & 1) != 0 ? local.max.x : local.min.x) * taper,
        ((corner & 2) != 0 ? local.max.y : local.min.y) * taper,
        (corner & 4) != 0 ? local.max.z : local.min.z,
    };
    const glm::vec3 transformed = glm::vec3(model * glm::vec4(point, 1.0F));
    if (std::isfinite(transformed.x) && std::isfinite(transformed.y) && std::isfinite(transformed.z)) {
      expandAABB(output, transformed);
    }
  }
  return output;
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
  if (data_bindings_ == nullptr || annotation_bindings_ == nullptr) {
    return false;
  }

  QRhiVertexInputLayout instance_layout;
  instance_layout.setBindings({
      QRhiVertexInputBinding(sizeof(CubeVertex)),
      QRhiVertexInputBinding(sizeof(Instance), QRhiVertexInputBinding::PerInstance),
  });
  instance_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)),
      // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, 0U, 0),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float4, 16U, 1),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float4, 32U, 2),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 48U, 3),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 64U),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 80U),
  });
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
  const auto create_instanced = [&](QRhiGraphicsPipeline::Topology topology, QRhiGraphicsPipeline::CullMode cull,
                                    bool depth_write) {
    return createScenePipeline(
        setup, {
                   .topology = topology,
                   .vertex = marker_vertex,
                   .fragment = marker_fragment,
                   .input_layout = instance_layout,
                   .bindings = data_bindings_,
                   .cull = cull,
                   .depth_write = depth_write,
               });
  };
  const auto create_stream = [&](QRhiGraphicsPipeline::Topology topology, bool depth_write) {
    return createScenePipeline(
        setup, {
                   .topology = topology,
                   .vertex = stream_vertex,
                   .fragment = stream_fragment,
                   .input_layout = stream_layout,
                   .bindings = data_bindings_,
                   .depth_write = depth_write,
               });
  };
  triangle_pipeline_ = create_instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::None, true);
  triangle_no_depth_pipeline_ = create_instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::None, false);
  triangle_cull_pipeline_ = create_instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back, true);
  triangle_cull_no_depth_pipeline_ =
      create_instanced(QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::Back, false);
  line_pipeline_ = create_instanced(QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::None, true);
  line_no_depth_pipeline_ = create_instanced(QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::None, false);
  stream_line_pipeline_ = create_stream(QRhiGraphicsPipeline::Lines, true);
  stream_line_no_depth_pipeline_ = create_stream(QRhiGraphicsPipeline::Lines, false);
  stream_triangle_pipeline_ = create_stream(QRhiGraphicsPipeline::Triangles, true);
  stream_triangle_no_depth_pipeline_ = create_stream(QRhiGraphicsPipeline::Triangles, false);
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
  }
  mesh_upload_pending_ = true;
  return true;
}

bool MarkerRhiPass::ready() const {
  return triangle_pipeline_ != nullptr && triangle_no_depth_pipeline_ != nullptr &&
         triangle_cull_pipeline_ != nullptr && triangle_cull_no_depth_pipeline_ != nullptr &&
         line_pipeline_ != nullptr && line_no_depth_pipeline_ != nullptr && stream_line_pipeline_ != nullptr &&
         stream_line_no_depth_pipeline_ != nullptr && stream_triangle_pipeline_ != nullptr &&
         stream_triangle_no_depth_pipeline_ != nullptr;
}

void MarkerRhiPass::registerPipelines(ScenePipelineRegistry& registry) const {
  for (QRhiGraphicsPipeline* pipeline :
       {triangle_pipeline_, triangle_no_depth_pipeline_, triangle_cull_pipeline_, triangle_cull_no_depth_pipeline_,
        line_pipeline_, line_no_depth_pipeline_, stream_line_pipeline_, stream_line_no_depth_pipeline_,
        stream_triangle_pipeline_, stream_triangle_no_depth_pipeline_}) {
    registry.add(pipeline);
  }
}

void MarkerRhiPass::setLayers(const std::vector<SceneEntitiesLayer*>& layers) {
  layers_ = layers;
}

const Transform* MarkerRhiPass::placement(const Admitted& admitted, std::uint32_t frame_index) const {
  if (frame_index >= admitted.frame_count) {
    return nullptr;
  }
  const std::optional<Transform>& placed = placements_[admitted.first_frame + frame_index];
  return placed.has_value() ? &*placed : nullptr;
}

void MarkerRhiPass::admit(const PlaceFn& place, SceneBoundsCollector& bounds) {
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
      const std::optional<Transform>& placed = placements_.emplace_back(place(geometry.frames[index]));
      if (placed.has_value() && index < geometry.frame_bounds.size()) {
        layer_bounds = unionAABB(layer_bounds, transformedMarkerBounds(geometry.frame_bounds[index], *placed));
      }
    }
    const auto placed = [&](std::uint32_t frame_index) { return placement(prepared, frame_index) != nullptr; };
    const auto count_instances = [&placed](const auto& instances) {
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
        prepared.stream_vertices += batch.vertices.size();
      }
    }
    for (const MarkerStreamBatch& batch : geometry.triangles) {
      if (placed(batch.frame_index)) {
        prepared.stream_vertices += batch.vertices.size() * (layer->wireframe() ? 2U : 1U);
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
    bounds.add(dynamic_cast<const void*>(layer), layer_bounds);
    admitted_.push_back(prepared);
  }
  last_bounds_ = family_bounds;
}

void MarkerRhiPass::prepare(const RhiFrameContext& frame, bool cast_shadows) {
  const glm::dvec3& render_origin = frame.render_origin;
  draws_.clear();
  casters_.clear();
  caster_bounds_ = {};
  instances_.clear();
  line_vertices_.clear();
  triangle_vertices_.clear();
  last_layer_count_ = 0;
  last_instance_count_ = 0;
  last_stream_vertex_count_ = 0;

  for (const Admitted& prepared : admitted_) {
    SceneEntitiesLayer* layer = prepared.layer;
    const MarkerGeometry& geometry = layer->geometry();
    const std::size_t first_draw = draws_.size();
    const bool pass_translucent = layer->opacity() < 0.999F;
    const auto placed = [&](std::uint32_t frame_index) { return placement(prepared, frame_index); };
    const auto add_range = [&](Kind kind, std::uint32_t mesh, quint32 offset, quint32 count, bool cull_back,
                               bool depth_write) {
      draws_.push_back(
          {layer, kind, mesh, offset, count, cull_back, depth_write,
           depth_write ? DrawClass::kOpaque : DrawClass::kTranslucent});
    };
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
    const auto stream_vertex = [&](const glm::dmat4& model, const MarkerStreamVertex& source) {
      const glm::dvec3 point = glm::dvec3(model * glm::dvec4(source.position, 1.0)) - render_origin;
      const glm::vec4 color = apply_color(source.color);
      return StreamVertex{
          static_cast<float>(point.x),
          static_cast<float>(point.y),
          static_cast<float>(point.z),
          color.r,
          color.g,
          color.b,
          color.a};
    };
    const auto append_instances = [&](const std::vector<MarkerInstanceSource>& sources,
                                      bool cube_edge_overlay = false) {
      const quint32 first = static_cast<quint32>(instances_.size());
      for (const MarkerInstanceSource& source : sources) {
        const Transform* fixed_from_frame = placed(source.frame_index);
        if (fixed_from_frame == nullptr) {
          continue;
        }
        Instance instance;
        copyMatrix(relativeModel(fixed_from_frame->matrix() * source.model, render_origin), instance.model);
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
        instances_.push_back(instance);
      }
      return std::pair{first, static_cast<quint32>(instances_.size()) - first};
    };
    const auto add_instance_range = [&](std::uint32_t mesh, quint32 offset, quint32 count, bool cull_back,
                                        bool depth_write, bool as_lines) {
      if (count != 0U) {
        add_range(
            as_lines ? Kind::kInstanceLines : Kind::kInstanceTriangles, mesh, offset, count, cull_back, depth_write);
      }
    };
    // Solid fills cast whatever their opacity, like the layer's model draws.
    const auto add_caster = [&](std::uint32_t mesh, quint32 offset, quint32 count) {
      if (!cast_shadows || layer->wireframe() || count == 0U) {
        return;
      }
      casters_.push_back({layer, mesh, offset, count});
      for (quint32 index = offset; index < offset + count; ++index) {
        caster_bounds_ = unionAABB(caster_bounds_, instanceBounds(instances_[index], mesh));
      }
    };

    // Native family order: cube fill + edge overlay, sphere, cylinder,
    // arrows/axes, user lines, user triangles.
    const auto [cube_offset, cube_count] = append_instances(geometry.cubes);
    add_caster(kCubeMesh, cube_offset, cube_count);
    bool cubes_translucent = pass_translucent;
    for (quint32 index = 0; index < cube_count; ++index) {
      cubes_translucent = cubes_translucent || instances_[cube_offset + index].a < 0.999F;
    }
    if (layer->wireframe()) {
      add_instance_range(kCubeMesh, cube_offset, cube_count, false, !pass_translucent, true);
    } else {
      add_instance_range(kCubeMesh, cube_offset, cube_count, true, !cubes_translucent, false);
      // Desktop's normal cube overlay contains only the twelve geometric box
      // edges. Explicit wireframe mode above still uses the triangle mesh edges,
      // including face diagonals, like native polygon mode.
      const auto [edge_offset, edge_count] = append_instances(geometry.cubes, true);
      add_instance_range(kCubeEdgeOverlayMesh, edge_offset, edge_count, false, !pass_translucent, true);
      if (edge_count != 0U && cubes_translucent) {
        draws_.back().draw_class = DrawClass::kTranslucent;
      }
    }
    const auto append_family = [&](const auto& sources, std::uint32_t mesh, bool cull_back, bool casts) {
      const auto [offset, count] = append_instances(sources);
      add_instance_range(
          mesh, offset, count, layer->wireframe() ? false : cull_back, !pass_translucent, layer->wireframe());
      if (casts) {
        add_caster(mesh, offset, count);
      }
    };
    append_family(geometry.spheres, kSphereMesh, true, true);
    append_family(geometry.cylinders, kCylinderMesh, true, true);
    append_family(geometry.arrows, kArrowMesh, false, true);
    append_family(geometry.axes, kArrowMesh, false, false);

    const quint32 line_offset = static_cast<quint32>(line_vertices_.size());
    for (const MarkerStreamBatch& batch : geometry.lines) {
      const Transform* fixed_from_frame = placed(batch.frame_index);
      if (fixed_from_frame == nullptr) {
        continue;
      }
      const glm::dmat4 model = fixed_from_frame->matrix() * batch.model;
      for (const MarkerStreamVertex& source : batch.vertices) {
        line_vertices_.push_back(stream_vertex(model, source));
      }
    }
    const quint32 line_count = static_cast<quint32>(line_vertices_.size()) - line_offset;
    if (line_count != 0U) {
      add_range(Kind::kStreamLines, 0U, line_offset, line_count, false, !pass_translucent);
    }

    if (layer->wireframe()) {
      const quint32 edge_offset = static_cast<quint32>(line_vertices_.size());
      for (const MarkerStreamBatch& batch : geometry.triangles) {
        const Transform* fixed_from_frame = placed(batch.frame_index);
        if (fixed_from_frame == nullptr) {
          continue;
        }
        const glm::dmat4 model = fixed_from_frame->matrix() * batch.model;
        for (std::size_t index = 0; index + 2U < batch.vertices.size(); index += 3U) {
          const StreamVertex a = stream_vertex(model, batch.vertices[index]);
          const StreamVertex b = stream_vertex(model, batch.vertices[index + 1U]);
          const StreamVertex c = stream_vertex(model, batch.vertices[index + 2U]);
          line_vertices_.insert(line_vertices_.end(), {a, b, b, c, c, a});
        }
      }
      const quint32 edge_count = static_cast<quint32>(line_vertices_.size()) - edge_offset;
      if (edge_count != 0U) {
        add_range(Kind::kStreamLines, 0U, edge_offset, edge_count, false, !pass_translucent);
      }
    } else {
      const quint32 triangle_offset = static_cast<quint32>(triangle_vertices_.size());
      for (const MarkerStreamBatch& batch : geometry.triangles) {
        const Transform* fixed_from_frame = placed(batch.frame_index);
        if (fixed_from_frame == nullptr) {
          continue;
        }
        const glm::dmat4 model = fixed_from_frame->matrix() * batch.model;
        for (const MarkerStreamVertex& source : batch.vertices) {
          triangle_vertices_.push_back(stream_vertex(model, source));
        }
      }
      const quint32 triangle_count = static_cast<quint32>(triangle_vertices_.size()) - triangle_offset;
      if (triangle_count != 0U) {
        add_range(Kind::kStreamTriangles, 0U, triangle_offset, triangle_count, false, !pass_translucent);
      }
    }
    if (draws_.size() != first_draw) {
      ++last_layer_count_;
      last_instance_count_ += static_cast<int>(prepared.instances);
      last_stream_vertex_count_ += static_cast<int>(prepared.stream_vertices);
    }
  }

  QRhi* rhi = frame.rhi;
  QRhiResourceUpdateBatch* updates = frame.updates;
  const bool buffers_ready =
      ensureVertexBuffer(
          rhi, instance_buffer_, instance_capacity_, static_cast<quint32>(instances_.size() * sizeof(Instance)),
          kInitialBufferBytes) &&
      ensureVertexBuffer(
          rhi, line_buffer_, line_capacity_, static_cast<quint32>(line_vertices_.size() * sizeof(StreamVertex)),
          kInitialBufferBytes) &&
      ensureVertexBuffer(
          rhi, triangle_buffer_, triangle_capacity_,
          static_cast<quint32>(triangle_vertices_.size() * sizeof(StreamVertex)), kInitialBufferBytes);
  if (!buffers_ready) {
    draws_.clear();
    casters_.clear();
    caster_bounds_ = {};
  }
  if (mesh_upload_pending_) {
    const auto& mesh_data = unitMeshes();
    for (std::size_t index = 0; index < mesh_data.size(); ++index) {
      updates->uploadStaticBuffer(meshes_[index].vertex_buffer, mesh_data[index].vertices.data());
      if (meshes_[index].triangle_index_buffer != nullptr) {
        updates->uploadStaticBuffer(meshes_[index].triangle_index_buffer, mesh_data[index].triangle_indices.data());
      }
      if (meshes_[index].edge_index_buffer != nullptr) {
        updates->uploadStaticBuffer(meshes_[index].edge_index_buffer, mesh_data[index].edge_indices.data());
      }
    }
    mesh_upload_pending_ = false;
  }
  if (!buffers_ready) {
    return;
  }
  const auto upload_vector = [updates](QRhiBuffer* buffer, const auto& values) {
    const quint32 bytes = static_cast<quint32>(values.size() * sizeof(values.front()));
    if (bytes != 0U) {
      updates->updateDynamicBuffer(buffer, 0, bytes, values.data());
    }
  };
  upload_vector(instance_buffer_, instances_);
  upload_vector(line_buffer_, line_vertices_);
  upload_vector(triangle_buffer_, triangle_vertices_);
}

QRhiGraphicsPipeline* MarkerRhiPass::instancePipeline(const Draw& draw) const {
  switch (draw.kind) {
    case Kind::kInstanceLines:
      return draw.depth_write ? line_pipeline_ : line_no_depth_pipeline_;
    case Kind::kInstanceTriangles:
      return draw.cull_back ? (draw.depth_write ? triangle_cull_pipeline_ : triangle_cull_no_depth_pipeline_)
                            : (draw.depth_write ? triangle_pipeline_ : triangle_no_depth_pipeline_);
    case Kind::kStreamLines:
      return draw.depth_write ? stream_line_pipeline_ : stream_line_no_depth_pipeline_;
    case Kind::kStreamTriangles:
      return draw.depth_write ? stream_triangle_pipeline_ : stream_triangle_no_depth_pipeline_;
  }
  return nullptr;
}

bool MarkerRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(instancePipeline(draw), pass);
  if (pipeline == nullptr || draw.count == 0U) {
    return false;
  }
  if (draw.kind == Kind::kStreamLines || draw.kind == Kind::kStreamTriangles) {
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(annotation_bindings_);
    QRhiBuffer* buffer = draw.kind == Kind::kStreamLines ? line_buffer_ : triangle_buffer_;
    const QRhiCommandBuffer::VertexInput input(buffer, draw.offset * static_cast<quint32>(sizeof(StreamVertex)));
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(draw.count);
    return true;
  }
  if (draw.mesh >= meshes_.size() || instance_buffer_ == nullptr) {
    return false;
  }
  const MeshGpu& mesh = meshes_[draw.mesh];
  const bool lines = draw.kind == Kind::kInstanceLines;
  QRhiBuffer* index_buffer = lines ? mesh.edge_index_buffer : mesh.triangle_index_buffer;
  const quint32 index_count = lines ? mesh.edge_index_count : mesh.triangle_index_count;
  if (mesh.vertex_buffer == nullptr || index_buffer == nullptr || index_count == 0U) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(data_bindings_);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(mesh.vertex_buffer, 0),
      QRhiCommandBuffer::VertexInput(instance_buffer_, draw.offset * static_cast<quint32>(sizeof(Instance))),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), index_buffer, 0, QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(index_count, draw.count);
  return true;
}

bool MarkerRhiPass::ensureShadowPipeline(
    QRhi* rhi, QRhiRenderPassDescriptor* render_pass, QRhiShaderResourceBindings* bindings) {
  if (shadow_pipeline_ != nullptr) {
    return true;
  }
  if (render_pass == nullptr) {
    return false;
  }
  QRhiVertexInputLayout layout;
  layout.setBindings({
      QRhiVertexInputBinding(sizeof(CubeVertex)),
      QRhiVertexInputBinding(sizeof(Instance), QRhiVertexInputBinding::PerInstance),
  });
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
      // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, 0U, 0),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float4, 16U, 1),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float4, 32U, 2),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 48U, 3),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, offsetof(Instance, bottom_scale)),
  });
  // Same caster state as the model casters: two-sided, biased depth.
  shadow_pipeline_ = createScenePipeline(
      {rhi, render_pass, 1}, {
                                 .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/marker_shadow.vert.qsb")),
                                 .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/shadow.frag.qsb")),
                                 .input_layout = layout,
                                 .bindings = bindings,
                                 .blend = std::nullopt,
                                 .depth_bias = static_cast<int>(look::kShadowDepthBiasUnits),
                                 .slope_scaled_depth_bias = look::kShadowSlopeScaledDepthBias,
                             });
  if (shadow_pipeline_ == nullptr) {
    return false;
  }
  shadow_bindings_ = bindings;
  return true;
}

void MarkerRhiPass::releaseShadowResources() {
  delete shadow_pipeline_;
  shadow_pipeline_ = nullptr;
  shadow_bindings_ = nullptr;
}

void MarkerRhiPass::submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const {
  if (shadow_pipeline_ == nullptr || instance_buffer_ == nullptr || casters_.empty()) {
    return;
  }
  command_buffer->setGraphicsPipeline(shadow_pipeline_);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(shadow_bindings_);
  for (const Caster& caster : casters_) {
    const MeshGpu& mesh = meshes_[caster.mesh];
    if (mesh.vertex_buffer == nullptr || mesh.triangle_index_buffer == nullptr) {
      continue;
    }
    const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
        QRhiCommandBuffer::VertexInput(mesh.vertex_buffer, 0),
        QRhiCommandBuffer::VertexInput(instance_buffer_, caster.offset * static_cast<quint32>(sizeof(Instance))),
    };
    command_buffer->setVertexInput(
        0, static_cast<int>(inputs.size()), inputs.data(), mesh.triangle_index_buffer, 0,
        QRhiCommandBuffer::IndexUInt32);
    command_buffer->drawIndexed(mesh.triangle_index_count, caster.count);
  }
}

void MarkerRhiPass::noteShadowFailure(const QString& error) {
  SceneEntitiesLayer* noted = nullptr;
  for (const Caster& caster : casters_) {
    if (caster.layer != noted) {
      caster.layer->noteRenderFailure(error);
      noted = caster.layer;
    }
  }
}

int MarkerRhiPass::lastShadowTriangleCount() const {
  int triangles = 0;
  for (const Caster& caster : casters_) {
    triangles += static_cast<int>(caster.count * (meshes_[caster.mesh].triangle_index_count / 3U));
  }
  return triangles;
}

void MarkerRhiPass::release() {
  releaseShadowResources();
  for (MeshGpu& mesh : meshes_) {
    delete mesh.edge_index_buffer;
    delete mesh.triangle_index_buffer;
    delete mesh.vertex_buffer;
    mesh = {};
  }
  for (QRhiGraphicsPipeline** pipeline :
       {&triangle_pipeline_, &triangle_no_depth_pipeline_, &triangle_cull_pipeline_, &triangle_cull_no_depth_pipeline_,
        &line_pipeline_, &line_no_depth_pipeline_, &stream_line_pipeline_, &stream_line_no_depth_pipeline_,
        &stream_triangle_pipeline_, &stream_triangle_no_depth_pipeline_}) {
    delete *pipeline;
    *pipeline = nullptr;
  }
  delete annotation_bindings_;
  delete data_bindings_;
  delete triangle_buffer_;
  delete line_buffer_;
  delete instance_buffer_;
  annotation_bindings_ = nullptr;
  data_bindings_ = nullptr;
  triangle_buffer_ = nullptr;
  line_buffer_ = nullptr;
  instance_buffer_ = nullptr;
  instance_capacity_ = 0;
  line_capacity_ = 0;
  triangle_capacity_ = 0;
  mesh_upload_pending_ = false;
  admitted_.clear();
  placements_.clear();
  draws_.clear();
  casters_.clear();
  caster_bounds_ = {};
}

}  // namespace pj::scene3d
