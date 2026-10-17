// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "gridmap_rhi_pass.h"

#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <glm/gtc/quaternion.hpp>
#include <limits>

#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_widgets/layers/grid_map_layer.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

struct alignas(16) GridMapUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> model{};
  std::array<float, 16> normal_to_world{};
  std::array<float, 4> eye{};
  std::array<float, 4> view_z{};
  std::array<float, 4> key_dir{};
  std::array<float, 4> cell_dims{};
  std::array<float, 4> color_params{};
  std::array<float, 4> light_params{};
  std::array<float, 4> flags{};
};
constexpr quint32 kUniformBytes = sizeof(GridMapUniforms);
static_assert(kUniformBytes == 304U);

// The staged grid has quads and a complete payload.
bool drawable(const GridMapUpload& grid) {
  const std::uint64_t cells = static_cast<std::uint64_t>(grid.column_count) * grid.row_count;
  const bool have_elevation = grid.elevation.size() == cells;
  const bool have_color = grid.color_is_elevation ? have_elevation : grid.color.size() == cells;
  return grid.column_count >= 2 && grid.row_count >= 2 && have_color && (grid.elevation.empty() || have_elevation);
}

void uploadChannel(
    QRhiResourceUpdateBatch* updates, QRhiTexture* texture, const GridMapUpload& grid, const float* data) {
  QRhiTextureSubresourceUploadDescription description(
      data, static_cast<quint32>(static_cast<std::size_t>(grid.column_count) * grid.row_count * sizeof(float)));
  description.setSourceSize(QSize(static_cast<int>(grid.column_count), static_cast<int>(grid.row_count)));
  updates->uploadTexture(texture, QRhiTextureUploadDescription({0, 0, description}));
}

}  // namespace

QRhiShaderResourceBindings* GridMapRhiPass::createLayerBindings(
    QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* elevation, QRhiTexture* color) const {
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms),
          QRhiShaderResourceBinding::sampledTexture(
              1, QRhiShaderResourceBinding::VertexStage, elevation, texel_sampler_),
          QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::VertexStage, color, texel_sampler_),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

bool GridMapRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.render_mode_uniforms;
  texel_sampler_ = rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  layout_texture_ = rhi->newTexture(QRhiTexture::R32F, QSize(1, 1));
  if (!texel_sampler_->create() || !layout_texture_->create() ||
      !ensureUniformBuffer(rhi, layout_uniform_buffer_, kUniformBytes)) {
    return false;
  }
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniform_buffer_, layout_texture_, layout_texture_);
  PipelineDesc desc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/gridmap.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/gridmap.frag.qsb")),
      .bindings = layout_shader_resources_,  // procedural: gl_VertexIndex only, no vertex input
  };
  opaque_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.depth_write = false;  // depth test only
  translucent_pipeline_ = setup.pipelines->add(desc, setup.part);
  return ready();
}

void GridMapRhiPass::setLayers(const std::vector<GridMapLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

void GridMapRhiPass::admit(const PlaceFn& place) {
  admitted_.clear();
  for (GridMapLayer* layer : layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender();
    const std::optional<GridMapUpload>& grid = layer->stagedGrid();
    if (!grid.has_value() || !drawable(*grid)) {
      pruneLayerGpu(layer_gpu_, layer, releaseLayerGpu);
      continue;
    }
    const auto placed = place(grid->frame_id, layer->renderStamp(), layer->placementHold());
    if (!placed.has_value()) {
      continue;
    }
    admitted_.push_back({layer, *placed});
  }
}

void GridMapRhiPass::prepare(const RhiFrameContext& frame, const MeshShadingParams& shading) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  last_layer_count_ = 0;
  last_vertex_count_ = 0;
  const glm::mat4 view_to_scene = glm::inverse(frame.view);
  const glm::vec3 eye(view_to_scene[3]);
  const glm::vec3 view_z = glm::normalize(glm::vec3(view_to_scene[2]));
  const float key_length = glm::length(shading.key_light_dir);
  const glm::vec3 key_dir = key_length > 1e-6F ? shading.key_light_dir / key_length : glm::vec3(0.0F, 0.0F, 1.0F);
  for (const auto& admitted : admitted_) {
    GridMapLayer* layer = admitted.layer;
    const GridMapUpload& grid = *layer->stagedGrid();
    LayerGpu& gpu = layer_gpu_[layer];
    if (!ensureLayerGpu(frame.rhi, layer, gpu)) {
      continue;
    }
    const bool has_elevation = !grid.elevation.empty();
    if (gpu.uploaded_revision != layer->stagedRevision()) {
      if (has_elevation) {
        uploadChannel(frame.updates, gpu.elevation, grid, grid.elevation.data());
      }
      if (!grid.color_is_elevation) {
        uploadChannel(frame.updates, gpu.color, grid, grid.color.data());
      }
      gpu.uploaded_revision = layer->stagedRevision();
      ++upload_count_;
    }

    const glm::mat4 model_f =
        relativeModel(admitted.fixed_from_source.matrix() * poseTransform(grid.origin).matrix(), frame.render_origin);
    // An auto range with no finite sample collapses to lo == hi: the shader then
    // paints the LUT midpoint instead of dividing by zero.
    float color_lo = static_cast<float>(layer->manualRange().first);
    float color_hi = static_cast<float>(layer->manualRange().second);
    if (layer->autoRange()) {
      color_lo = grid.color_range.valid ? grid.color_range.lo : 0.0F;
      color_hi = grid.color_range.valid ? grid.color_range.hi : 0.0F;
    }
    const auto opacity = static_cast<float>(layer->opacity());

    GridMapUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(model_f, uniforms.model);
    copyMatrix(glm::mat4(glm::transpose(glm::inverse(glm::mat3(model_f)))), uniforms.normal_to_world);
    uniforms.eye = {eye.x, eye.y, eye.z, 0.0F};
    uniforms.view_z = {view_z.x, view_z.y, view_z.z, 0.0F};
    uniforms.key_dir = {key_dir.x, key_dir.y, key_dir.z, 0.0F};
    uniforms.cell_dims = {
        grid.cell_size.x, grid.cell_size.y, static_cast<float>(grid.column_count), static_cast<float>(grid.row_count)};
    uniforms.color_params = {color_lo, color_hi, static_cast<float>(layer->colormap()), opacity};
    uniforms.light_params = {
        look::kGridMapAmbient * shading.ambient_scale, look::kGridMapDirect * shading.direct_scale,
        look::kGridMapFill * shading.fill_light_scale, layer->lighting() ? 1.0F : 0.0F};
    uniforms.flags = {has_elevation ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};
    frame.updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kUniformBytes, &uniforms);

    const auto vertex_count = static_cast<quint32>(6U * (grid.column_count - 1U) * (grid.row_count - 1U));
    draws_.push_back(
        Draw{
            layer, gpu.shader_resources, vertex_count,
            opacity < 0.999F ? DrawClass::kTranslucent : DrawClass::kOpaque});
    ++last_layer_count_;
    last_vertex_count_ += static_cast<int>(vertex_count);
  }
}

bool GridMapRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline =
      registry.resolve(draw.draw_class == DrawClass::kTranslucent ? translucent_pipeline_ : opaque_pipeline_, pass);
  if (pipeline == nullptr) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.shader_resources);
  command_buffer->draw(draw.vertex_count);
  return true;
}

bool GridMapRhiPass::ensureLayerGpu(QRhi* rhi, GridMapLayer* layer, LayerGpu& gpu) {
  const GridMapUpload& grid = *layer->stagedGrid();
  const int texture_limit = rhi->resourceLimit(QRhi::TextureSizeMax);
  const std::uint64_t vertices = 6ULL * (grid.column_count - 1U) * (grid.row_count - 1U);
  if (texture_limit <= 0 || grid.column_count > static_cast<quint32>(texture_limit) ||
      grid.row_count > static_cast<quint32>(texture_limit) || vertices > std::numeric_limits<quint32>::max()) {
    layer->noteRenderFailure(tr("Grid map %1 x %2 exceeds this GPU's texture or draw limits")
                                 .arg(QString::number(grid.column_count), QString::number(grid.row_count)));
    releaseLayerGpu(gpu);
    return false;
  }
  if (!ensureUniformBuffer(rhi, gpu.uniform_buffer, kUniformBytes)) {
    layer->noteRenderFailure(tr("Could not create the grid-map uniform buffer"));
    releaseLayerGpu(gpu);
    return false;
  }
  const bool has_elevation = !grid.elevation.empty();
  const bool resized = gpu.width != grid.column_count || gpu.height != grid.row_count;
  const bool elevation_missing = has_elevation != (gpu.elevation != nullptr);
  const bool color_missing = !grid.color_is_elevation != (gpu.color != nullptr);
  if (resized || elevation_missing || color_missing || gpu.shader_resources == nullptr) {
    delete gpu.shader_resources;
    delete gpu.elevation;
    delete gpu.color;
    gpu.shader_resources = nullptr;
    gpu.elevation = nullptr;
    gpu.color = nullptr;
    const QSize size(static_cast<int>(grid.column_count), static_cast<int>(grid.row_count));
    if (has_elevation) {
      gpu.elevation = rhi->newTexture(QRhiTexture::R32F, size);
    }
    if (!grid.color_is_elevation) {
      gpu.color = rhi->newTexture(QRhiTexture::R32F, size);
    }
    if ((gpu.elevation != nullptr && !gpu.elevation->create()) || (gpu.color != nullptr && !gpu.color->create())) {
      layer->noteRenderFailure(tr("Could not allocate the grid-map textures"));
      releaseLayerGpu(gpu);
      return false;
    }
    gpu.width = grid.column_count;
    gpu.height = grid.row_count;
    gpu.uploaded_revision = ~std::uint64_t{0};
    // A flat plane samples the 1x1 layout texture (never read); a shared channel
    // binds the elevation texture twice.
    QRhiTexture* elevation = gpu.elevation != nullptr ? gpu.elevation : layout_texture_;
    QRhiTexture* color = gpu.color != nullptr ? gpu.color : elevation;
    gpu.shader_resources = createLayerBindings(rhi, gpu.uniform_buffer, elevation, color);
    if (gpu.shader_resources == nullptr) {
      layer->noteRenderFailure(tr("Could not create grid-map shader bindings"));
      releaseLayerGpu(gpu);
      return false;
    }
  }
  layer->noteRenderSuccess();
  return true;
}

void GridMapRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.elevation;
  delete gpu.color;
  delete gpu.uniform_buffer;
  gpu = {};
}

void GridMapRhiPass::release() {
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  delete layout_shader_resources_;
  delete layout_uniform_buffer_;
  delete layout_texture_;
  delete texel_sampler_;
  opaque_pipeline_ = nullptr;
  translucent_pipeline_ = nullptr;
  layout_shader_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  layout_texture_ = nullptr;
  texel_sampler_ = nullptr;
  render_mode_uniforms_ = nullptr;
}

}  // namespace pj::scene3d
