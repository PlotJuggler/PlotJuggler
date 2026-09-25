// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "point_rhi_pass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "pj_scene3d_core/cube_draw_policy.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/point_shading.h"

namespace pj::scene3d {
namespace {

// Mirrors PointUniforms in rhi/shaders/point_uniforms.glslinc.
struct alignas(16) PointUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
  std::array<float, 4> face_pick_eye{};
  std::array<float, 4> color_axis_offset{};
  std::array<float, 4> size_params{};
  std::array<float, 4> clamp_params{};
  std::array<float, 4> range_params{};
  std::array<float, 4> solid_color{};  // w unused
  std::array<float, 2> clip_per_px{};
  float pixel_scale = 1.0F;
  float clip_y_up = 1.0F;
  std::array<std::int32_t, 4> modes{};
  std::array<std::int32_t, 4> shape{};
};
constexpr quint32 kPointUniformBytes = 336U;
static_assert(sizeof(PointUniforms) == kPointUniformBytes);

constexpr quint32 kQuadVertexCount = 4U;  // one triangle strip per instance
constexpr quint32 kMinimumInstanceBytes = 1024U;
// Largest sprite (device px) a point list may draw. Point lists cost about half
// what quads do for many small sprites, but a point is culled whole once its
// centre leaves the view: above this size that edge pop would show.
constexpr float kPointListMaxPx = 4.0F;

enum class Partition : std::int32_t { kAll = 0, kInRange = 1, kOutside = 2 };
enum class ShapeMode : std::int32_t { kPoint = 0, kSphere = 1, kCubeSprite = 2 };

}  // namespace

QRhiShaderResourceBindings* PointRhiPass::createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms, quint32 offset) const {
  constexpr auto kStages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
  return createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, kStages, uniforms, offset, kPointUniformBytes),
               QRhiShaderResourceBinding::uniformBuffer(8, kStages, render_mode_uniforms_),
           });
}

bool PointRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.render_mode_uniforms;
  geometry_ = setup.geometry;
  uniform_stride_ = static_cast<quint32>(rhi->ubufAligned(static_cast<int>(kPointUniformBytes)));
  if (!ensureUniformBuffer(rhi, layout_uniforms_, kPointUniformBytes)) {
    return false;
  }
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniforms_, 0U);

  // Instance data only: the quad corners and fan corners come from the vertex index.
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(PointVertex), QRhiVertexInputBinding::PerInstance)});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(PointVertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float, offsetof(PointVertex, scalar)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, offsetof(PointVertex, rgba)),
  });
  // The point list reads the same attributes once per vertex.
  QRhiVertexInputLayout list_layout = layout;
  list_layout.setBindings({QRhiVertexInputBinding(sizeof(PointVertex))});
  const auto create_pair = [&](Geometry geometry, QRhiGraphicsPipeline::Topology topology, const QString& shader,
                               const QRhiVertexInputLayout& input_layout,
                               QRhiGraphicsPipeline::CullMode cull = QRhiGraphicsPipeline::None) {
    PipelineDesc desc{
        .topology = topology,
        .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/%1.vert.qsb").arg(shader)),
        .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/%1.frag.qsb").arg(shader)),
        .input_layout = input_layout,
        .bindings = layout_shader_resources_,
        .cull = cull,
    };
    auto& pair = pipelines_[static_cast<std::size_t>(geometry)];
    pair[0] = createScenePipeline(setup, desc);
    desc.depth_write = false;
    pair[1] = createScenePipeline(setup, desc);
  };
  create_pair(Geometry::kQuad, QRhiGraphicsPipeline::TriangleStrip, QStringLiteral("point"), layout);
  // The fan picks its faces from the eye against the cube's CENTRE: with the eye
  // inside an axis slab (or inside the cube) a picked face is seen from behind.
  // Opaque cubes hide it by depth, but the faded half writes no depth and would
  // blend the interior in; culling drops exactly those faces (the fan is
  // CCW-outward, cube_mesh.h).
  create_pair(
      Geometry::kFan, QRhiGraphicsPipeline::Triangles, QStringLiteral("cube"), layout, QRhiGraphicsPipeline::Back);
  // Optional: without compute (WebGL2, GL < 4.3) every layer keeps CPU bounds.
  (void)aabb_reducer_.initialize(rhi);
  if (rhi->isFeatureSupported(QRhi::VertexShaderPointSize)) {
    create_pair(Geometry::kPointList, QRhiGraphicsPipeline::Points, QStringLiteral("point_list"), list_layout);
  }
  return ready();
}

void PointRhiPass::registerPipelines(ScenePipelineRegistry& registry) const {
  for (const auto& pair : pipelines_) {
    registry.add(pair[0]);
    registry.add(pair[1]);
  }
}

void PointRhiPass::setLayers(const std::vector<PointRenderable*>& layers) {
  adoptLayerList(
      layers, layers_, layer_gpu_, [this](PointRenderable* layer, LayerGpu& gpu) { dropLayerGpu(layer, gpu); });
}

void PointRhiPass::dropLayerGpu(PointRenderable* layer, LayerGpu& gpu) {
  aabb_reducer_.forget(layer);
  releaseLayerGpu(gpu);
}

std::optional<AABB> PointRhiPass::boundsOf(const PointRenderable* layer, const LayerGpu& gpu, bool current_only) {
  if (auto bounds = layer->sourceBounds(); bounds.has_value()) {
    return bounds;
  }
  if (gpu.gpu_bounds_revision.has_value() && gpu.gpu_bounds.valid &&
      (!current_only || *gpu.gpu_bounds_revision == layer->geometryRevision())) {
    return gpu.gpu_bounds;
  }
  return std::nullopt;
}

// A result replaces the held box only when it describes a newer sample: a
// reduction that lands late never rolls the bounds back.
void PointRhiPass::adoptGpuBounds() {
  for (const PointAabbReducer::Result& result : aabb_reducer_.takeResults()) {
    auto* layer = static_cast<PointRenderable*>(const_cast<void*>(result.key));
    if (const auto gpu = layer_gpu_.find(layer);
        gpu != layer_gpu_.end() &&
        (!gpu->second.gpu_bounds_revision.has_value() || result.revision > *gpu->second.gpu_bounds_revision)) {
      gpu->second.gpu_bounds = result.bounds;
      gpu->second.gpu_bounds_revision = result.revision;
    }
  }
}

void PointRhiPass::admit(const PlaceFn& place, SceneBoundsCollector& bounds) {
  admitted_.clear();
  adoptGpuBounds();
  AABB point_bounds;
  AABB depth_bounds;
  std::uint64_t remaining_vertices = kSceneLimits.max_point_vertices_per_view;
  for (PointRenderable* layer : layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender(remaining_vertices);
    if (layer->vertices().empty()) {
      pruneLayerGpu(layer_gpu_, layer, [this](PointRenderable* dropped, LayerGpu& gpu) { dropLayerGpu(dropped, gpu); });
      continue;
    }
    const auto placed = place(layer->renderFrameId());
    if (!placed.has_value()) {
      continue;
    }
    // Placement first, budget second: a temporarily unplaceable layer must not
    // reserve capacity ahead of a later layer whose frame resolves this frame.
    if (!tryConsumeBudget(static_cast<std::uint64_t>(layer->vertices().size()), remaining_vertices)) {
      continue;  // prepareForRender() normally clears this; keep subtraction fail-closed.
    }
    const auto gpu = layer_gpu_.find(layer);
    const auto source_bounds =
        gpu != layer_gpu_.end() ? boundsOf(layer, gpu->second, /*current_only=*/false) : layer->sourceBounds();
    if (source_bounds.has_value()) {
      const AABB placed_bounds = transformedBounds(*source_bounds, *placed);
      AABB& family_bounds = layer->isDepthCloud() ? depth_bounds : point_bounds;
      family_bounds = unionAABB(family_bounds, placed_bounds);
      bounds.add(dynamic_cast<const void*>(layer), placed_bounds);
    }
    admitted_.push_back({layer, *placed});
  }
  last_point_bounds_ = point_bounds;
  last_depth_bounds_ = depth_bounds;
}

void PointRhiPass::prepare(const RhiFrameContext& frame, const ViewInputs& view) {
  draws_.clear();
  last_point_layer_count_ = 0;
  last_point_vertex_count_ = 0;
  last_depth_layer_count_ = 0;
  last_depth_vertex_count_ = 0;
  last_cube_layer_count_ = 0;
  last_cube_instance_count_ = 0;
  last_cube_sprite_layer_count_ = 0;
  last_point_list_layer_count_ = 0;
  gpu_bounds_pending_ = false;
  const float device_width = static_cast<float>(std::max(view.device_size.width(), 1));
  const float device_height = static_cast<float>(std::max(view.device_size.height(), 1));
  const float dpr = std::max(view.device_pixel_ratio, 1.0F);
  const float pixel_scale = std::max(view.pixel_scale, 1.0F);
  const bool perspective = view.projection[3][3] == 0.0F;
  const glm::vec4 face_pick_eye = facePickReference(frame.view, view.projection);
  const float clip_y_up = rhiConventions(*frame.rhi).y_up_in_ndc ? 1.0F : -1.0F;
  const float pixels_per_metre = view.projection[1][1] * device_height * 0.5F;

  for (const auto& admitted : admitted_) {
    PointRenderable* layer = admitted.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    if (!ensureLayerGpu(frame.rhi, gpu) || !uploadInstances(frame.rhi, frame.updates, layer, gpu) || gpu.count == 0) {
      continue;
    }
    const std::optional<AABB> source_bounds = boundsOf(layer, gpu, /*current_only=*/true);
    if (!layer->sourceBounds().has_value() && gpu.gpu_bounds_revision != gpu.uploaded_revision &&
        aabb_reducer_.available()) {
      aabb_reducer_.request(layer, gpu.uploaded_revision, gpu.instances, gpu.count);
      gpu_bounds_pending_ = true;
    }
    const PointShape shape = layer->shape();
    const bool cube = shape == PointShape::kCube;
    if (layer->isDepthCloud()) {
      last_depth_vertex_count_ += static_cast<int>(gpu.count);
      ++last_depth_layer_count_;
    } else {
      last_point_vertex_count_ += static_cast<int>(gpu.count);
      ++last_point_layer_count_;
      if (cube) {
        last_cube_instance_count_ += static_cast<int>(gpu.count);
        ++last_cube_layer_count_;
      }
    }

    const glm::mat4 model = relativeModel(admitted.fixed_from_source.matrix(), frame.render_origin);

    const auto [range_minimum, range_maximum] = layer->scalarRange(glm::mat4(admitted.fixed_from_source.matrix()));
    layer->noteRenderedScalarRange(range_minimum, range_maximum);

    // One decision for every shape (the GL pass's): frustum reject against the
    // geometry, camera-inside fallback, and the whole-cloud cube LOD.
    CubeCloudView cloud_view;
    cloud_view.centre_bounds = source_bounds.value_or(AABB{});
    cloud_view.clip_from_cloud = view.projection * frame.view * model;
    if (perspective) {
      cloud_view.eye_in_cloud = glm::vec3(glm::inverse(model) * glm::vec4(glm::vec3(face_pick_eye), 1.0F));
    }
    cloud_view.cube_size_m = shape == PointShape::kPoint ? 0.0F : layer->sizeMeters();
    cloud_view.pixels_per_metre = pixels_per_metre;
    cloud_view.lod_threshold_px = cube ? kDefaultCubeLodThresholdPx * pixel_scale : 0.0F;
    // The widest screen-sized sprite this shape can draw, so the reject keeps a quad
    // whose centre is off screen: a point's clamped pixel size, a sphere's size clamp
    // (its minimum and its flattened far size can outgrow the projected sphere), a LOD
    // sprite's threshold size. A fan's cubes are covered by their world extent.
    const float min_px = kMinPointPx * dpr * pixel_scale;
    const float max_px = kMaxPointPx * dpr * pixel_scale;
    const float sprite_px = shape == PointShape::kPoint ? std::clamp(layer->sizePixels() * pixel_scale, min_px, max_px)
                            : shape == PointShape::kSphere ? max_px
                                                           : kDefaultCubeLodThresholdPx * 2.0F * pixel_scale;
    cloud_view.screen_reach_ndc = {sprite_px / device_width, sprite_px / device_height};
    const CubeDrawMode mode =
        chooseCubeDrawMode(cloud_view, CubeDrawCapabilities{/*sprite=*/cube, /*fan=*/true, /*solid=*/false});
    if (mode == CubeDrawMode::kSkip) {
      continue;
    }
    const bool sprites = cube && mode == CubeDrawMode::kSprite;
    last_cube_sprite_layer_count_ += sprites ? 1 : 0;

    // The largest sprite this cloud can draw: a point's fixed size, a sphere at the
    // nearest the cloud comes (clip w is the view depth), a LOD sprite at most the
    // threshold. Unknown bounds count as large.
    float largest_px = max_px;
    if (shape == PointShape::kPoint) {
      largest_px = std::clamp(layer->sizePixels() * pixel_scale, min_px, max_px);
    } else if (sprites) {
      largest_px = kDefaultCubeLodThresholdPx * 2.0F * pixel_scale;
    } else if (shape == PointShape::kSphere && cloud_view.centre_bounds.valid) {
      float size_px = layer->sizeMeters() * 0.5F * view.projection[1][1] * device_height;
      if (perspective) {
        const float depth = std::max(aabbClipWRange(cloud_view.centre_bounds, cloud_view.clip_from_cloud).first, 0.01F);
        size_px /= depth <= kSphereDepthThreshold ? depth : std::sqrt(depth * kSphereDepthThreshold);
      }
      largest_px = std::clamp(size_px, min_px, max_px);
    }
    const bool point_list = !(cube && !sprites) && largest_px <= kPointListMaxPx * pixel_scale && pointListActive();
    last_point_list_layer_count_ += point_list ? 1 : 0;

    // Only a colormapped field has a range to leave; 0 hides, (0, 1) fades.
    const float outside_alpha =
        layer->colorType() == PointColorType::kField ? std::clamp(layer->outsideRangeAlpha(), 0.0F, 1.0F) : 1.0F;
    const bool faded_half = outside_alpha > 0.0F && outside_alpha < 1.0F;

    PointUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(model, uniforms.fixed_from_source);
    copyMatrix(frame.view, uniforms.view);
    uniforms.face_pick_eye = {face_pick_eye.x, face_pick_eye.y, face_pick_eye.z, face_pick_eye.w};
    uniforms.color_axis_offset = {
        static_cast<float>(frame.render_origin.x), static_cast<float>(frame.render_origin.y),
        static_cast<float>(frame.render_origin.z), 0.0F};
    uniforms.size_params = {
        layer->sizeMeters(), layer->sizePixels() * pixel_scale, device_height, view.projection[1][1]};
    uniforms.clamp_params = {min_px, max_px, kSphereDepthThreshold, perspective ? 1.0F : 0.0F};
    const QColor solid = layer->solidColor();
    uniforms.solid_color = {
        static_cast<float>(solid.redF()), static_cast<float>(solid.greenF()), static_cast<float>(solid.blueF()), 0.0F};
    uniforms.clip_per_px = {2.0F / device_width, 2.0F / device_height};
    uniforms.pixel_scale = pixel_scale;
    uniforms.clip_y_up = clip_y_up;
    uniforms.modes = {
        pointColorMode(layer->colorType()), static_cast<std::int32_t>(layer->colormap()), layer->invertLut() ? 1 : 0,
        layer->scalarAxis()};
    const ShapeMode shape_mode = sprites                       ? ShapeMode::kCubeSprite
                                 : shape == PointShape::kPoint ? ShapeMode::kPoint
                                                               : ShapeMode::kSphere;
    uniforms.shape = {static_cast<std::int32_t>(shape_mode), 0, 0, 0};
    const Geometry geometry = cube && !sprites ? Geometry::kFan : point_list ? Geometry::kPointList : Geometry::kQuad;

    const auto add_draw = [&](std::size_t slot, Partition partition, DrawClass draw_class) {
      uniforms.range_params = {range_minimum, range_maximum, outside_alpha, static_cast<float>(partition)};
      frame.updates->updateDynamicBuffer(
          gpu.uniforms, static_cast<quint32>(slot) * uniform_stride_, kPointUniformBytes, &uniforms);
      draws_.push_back(Draw{layer, gpu.instances, gpu.shader_resources[slot], gpu.count, geometry, draw_class});
    };
    add_draw(0U, faded_half ? Partition::kInRange : Partition::kAll, DrawClass::kOpaque);
    if (faded_half) {
      add_draw(1U, Partition::kOutside, DrawClass::kTranslucent);
    }
  }
}

bool PointRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  const bool opaque = draw.draw_class == DrawClass::kOpaque;
  if (!opaque && pass == ScenePass::kDepthReplay) {
    // The faded half writes no depth, so it leaves no coverage either: like the GL
    // renderer, whose composite finds the background by depth, the pixels it tints
    // over the background stay ungraded background.
    return true;
  }
  const bool fan = draw.geometry == Geometry::kFan;
  QRhiGraphicsPipeline* pipeline =
      registry.resolve(pipelines_[static_cast<std::size_t>(draw.geometry)][opaque ? 0 : 1], pass);
  if (pipeline == nullptr || draw.instances == nullptr || draw.instance_count == 0U ||
      (fan && (geometry_ == nullptr || geometry_->cube_fan_indices == nullptr))) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.shader_resources);
  const QRhiCommandBuffer::VertexInput input(draw.instances, 0);
  if (fan) {
    command_buffer->setVertexInput(0, 1, &input, geometry_->cube_fan_indices, 0, QRhiCommandBuffer::IndexUInt16);
    command_buffer->drawIndexed(static_cast<quint32>(kCubeFanIndices.size()), draw.instance_count);
  } else if (draw.geometry == Geometry::kPointList) {
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(draw.instance_count);
  } else {
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(kQuadVertexCount, draw.instance_count);
  }
  return true;
}

bool PointRhiPass::ensureLayerGpu(QRhi* rhi, LayerGpu& gpu) {
  if (rhi == nullptr || render_mode_uniforms_ == nullptr) {
    return false;
  }
  if (!ensureUniformBuffer(rhi, gpu.uniforms, 2U * uniform_stride_)) {
    releaseLayerGpu(gpu);
    return false;
  }
  for (std::size_t slot = 0; slot < gpu.shader_resources.size(); ++slot) {
    if (gpu.shader_resources[slot] == nullptr) {
      gpu.shader_resources[slot] = createLayerBindings(rhi, gpu.uniforms, static_cast<quint32>(slot) * uniform_stride_);
      if (gpu.shader_resources[slot] == nullptr) {
        releaseLayerGpu(gpu);
        return false;
      }
    }
  }
  return true;
}

// Static, not Dynamic: a cloud changes per sample, not per frame, and a Dynamic
// buffer keeps one host-visible copy per frame in flight.
bool PointRhiPass::uploadInstances(QRhi* rhi, QRhiResourceUpdateBatch* updates, PointRenderable* layer, LayerGpu& gpu) {
  if (gpu.uploaded_revision == layer->geometryRevision()) {
    return true;
  }
  const auto& vertices = layer->vertices();
  const std::uint64_t bytes = static_cast<std::uint64_t>(vertices.size()) * sizeof(PointVertex);
  if (bytes > std::numeric_limits<quint32>::max()) {
    return false;
  }
  const auto required = static_cast<quint32>(bytes);
  // Reallocate to grow, or to give back a buffer more than twice the cloud.
  if (gpu.instances == nullptr || gpu.capacity < required ||
      gpu.capacity > std::max<quint32>(required, kMinimumInstanceBytes / 2U) * 2U) {
    delete gpu.instances;
    gpu.capacity = std::max(required, kMinimumInstanceBytes);
    // Storage usage lets the bounds reduction read the cloud where it already is.
    const QRhiBuffer::UsageFlags usage =
        aabb_reducer_.available() ? QRhiBuffer::UsageFlags(QRhiBuffer::VertexBuffer | QRhiBuffer::StorageBuffer)
                                  : QRhiBuffer::UsageFlags(QRhiBuffer::VertexBuffer);
    gpu.instances = rhi->newBuffer(QRhiBuffer::Static, usage, gpu.capacity);
    if (!gpu.instances->create()) {
      delete gpu.instances;
      gpu.instances = nullptr;
      gpu.capacity = 0;
      gpu.count = 0;
      return false;
    }
  }
  if (required != 0U) {
    updates->uploadStaticBuffer(gpu.instances, 0, required, vertices.data());
  }
  gpu.count = static_cast<quint32>(vertices.size());
  gpu.uploaded_revision = layer->geometryRevision();
  return true;
}

void PointRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  for (QRhiShaderResourceBindings* bindings : gpu.shader_resources) {
    delete bindings;
  }
  delete gpu.uniforms;
  delete gpu.instances;
  gpu = {};
}

void PointRhiPass::release() {
  aabb_reducer_.release();
  gpu_bounds_pending_ = false;
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  for (auto& pair : pipelines_) {
    for (QRhiGraphicsPipeline*& pipeline : pair) {
      delete pipeline;
      pipeline = nullptr;
    }
  }
  delete layout_shader_resources_;
  delete layout_uniforms_;
  layout_shader_resources_ = nullptr;
  layout_uniforms_ = nullptr;
  render_mode_uniforms_ = nullptr;
  geometry_ = nullptr;
}

}  // namespace pj::scene3d
