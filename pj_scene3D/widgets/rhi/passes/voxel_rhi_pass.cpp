// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "voxel_rhi_pass.h"

#include <QCoreApplication>
#include <QVarLengthArray>
#include <algorithm>
#include <array>
#include <cstddef>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "pj_scene3d_core/cube_draw_policy.h"
#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kVoxelUniformBytes = 256U;

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

}  // namespace

QRhiShaderResourceBindings* VoxelRhiPass::createLayerBindings(
    QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* volume) const {
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms),
          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::VertexStage, volume, sampler_),
          QRhiShaderResourceBinding::sampledTexture(
              2, QRhiShaderResourceBinding::FragmentStage, colormap_texture_, colormap_sampler_),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

bool VoxelRhiPass::initialize(const RhiPassSetup& setup, int max_3d_texture_size) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.geometry == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.render_mode_uniforms;
  geometry_ = setup.geometry;
  max_3d_texture_size_ = max_3d_texture_size;
  colormap_texture_ = rhi->newTexture(QRhiTexture::RGBA8, QSize(PJ::kColormapLutWidth, PJ::kColormapCount));
  colormap_sampler_ = rhi->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
  layout_texture_ = rhi->newTexture(QRhiTexture::R32F, 1, 1, 1, 1, QRhiTexture::ThreeDimensional);
  sampler_ = rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  if (!colormap_texture_->create() || !colormap_sampler_->create() ||
      !ensureUniformBuffer(rhi, layout_uniform_buffer_, kVoxelUniformBytes) || !layout_texture_->create() ||
      !sampler_->create()) {
    return false;
  }
  static const std::vector<std::uint8_t> colormap_lut = PJ::buildColormapLut();
  QRhiTextureSubresourceUploadDescription colormap(colormap_lut.data(), static_cast<quint32>(colormap_lut.size()));
  colormap.setSourceSize(QSize(PJ::kColormapLutWidth, PJ::kColormapCount));
  setup.updates->uploadTexture(colormap_texture_, QRhiTextureUploadDescription({0, 0, colormap}));
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniform_buffer_, layout_texture_);
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(CubeVertex))});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, px)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(CubeVertex, nx)),
  });
  pipeline_ = setup.pipelines->add(
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/voxel.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/voxel.frag.qsb")),
          .input_layout = layout,
          .bindings = layout_shader_resources_,
          .cull = QRhiGraphicsPipeline::Back,
      },
      setup.part);
  return pipeline_ != nullptr;
}

void VoxelRhiPass::setLayers(const std::vector<VoxelGridLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

void VoxelRhiPass::admit(QRhi* rhi, const PlaceFn& place) {
  last_bounds_ = admitLayers(
      layers_, kSceneLimits.max_voxels_per_view, place, admitted_, layer_gpu_, releaseLayerGpu,
      [&](VoxelGridLayer* layer) {
        if (!layer->hasVolume()) {
          return false;
        }
        const QString rejection = gpuRejection(rhi, layer);
        if (!rejection.isEmpty()) {
          layer->rejectVolumeForRender(rejection);
          return false;
        }
        return true;
      },
      [](const VoxelGridLayer* layer) { return layer->voxelCount(); },
      [](const VoxelGridLayer* layer, const Transform& placed) -> std::optional<AABB> {
        const auto source_bounds = layer->sourceBounds();
        return source_bounds.has_value() ? std::optional(transformedBounds(*source_bounds, placed)) : std::nullopt;
      });
}

void VoxelRhiPass::prepare(const RhiFrameContext& frame) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  last_layer_count_ = 0;
  last_voxel_count_ = 0;
  for (const auto& admitted : admitted_) {
    VoxelGridLayer* layer = admitted.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    bool texture_recreated = false;
    if (!ensureLayerGpu(frame.rhi, layer, gpu, texture_recreated)) {
      continue;
    }
    if (texture_recreated || gpu.uploaded_revision != layer->textureRevision()) {
      const std::uint64_t slice_voxels = static_cast<std::uint64_t>(layer->columnCount()) * layer->rowCount();
      const std::uint64_t slice_bytes = slice_voxels * 4U;
      QVarLengthArray<QRhiTextureUploadEntry, 32> entries;
      entries.reserve(static_cast<qsizetype>(layer->sliceCount()));
      for (std::uint32_t slice = 0; slice < layer->sliceCount(); ++slice) {
        const std::uint64_t offset = static_cast<std::uint64_t>(slice) * slice_bytes;
        const void* slice_data =
            layer->valueKind() == VoxelValueKind::kScalar
                ? static_cast<const void*>(reinterpret_cast<const std::uint8_t*>(layer->scalarVolume().data()) + offset)
                : static_cast<const void*>(layer->rgbaVolume().data() + offset);
        QRhiTextureSubresourceUploadDescription description(slice_data, static_cast<quint32>(slice_bytes));
        description.setSourceSize(QSize(static_cast<int>(layer->columnCount()), static_cast<int>(layer->rowCount())));
        entries.append(QRhiTextureUploadEntry(static_cast<int>(slice), 0, description));
      }
      QRhiTextureUploadDescription upload;
      upload.setEntries(entries.cbegin(), entries.cend());
      frame.updates->uploadTexture(gpu.texture, upload);
      gpu.uploaded_revision = layer->textureRevision();
      ++full_upload_count_;
    }

    const Transform source_from_grid = poseTransform(layer->origin());

    const glm::vec3 cell_size = layer->cellSize();
    const glm::mat4 fixed_from_grid =
        relativeModel(admitted.fixed_from_source.matrix() * source_from_grid.matrix(), frame.render_origin);
    // Whole-grid frustum reject, from the shared policy: voxel
    // centres, inflated by half the largest cell side. The clip-space-corrected
    // view-projection only loosens the near plane (0..w depth), never over-culls.
    CubeCloudView cloud_view;
    const glm::vec3 grid_extent = glm::vec3(
                                      static_cast<float>(layer->columnCount()), static_cast<float>(layer->rowCount()),
                                      static_cast<float>(layer->sliceCount())) *
                                  cell_size;
    cloud_view.centre_bounds = AABB{cell_size * 0.5F, grid_extent - cell_size * 0.5F, true};
    cloud_view.clip_from_cloud = glm::make_mat4(frame.view_projection.constData()) * fixed_from_grid;
    cloud_view.cube_size_m = std::max({cell_size.x, cell_size.y, cell_size.z});
    constexpr CubeDrawCapabilities kSolidOnly{/*sprite=*/false, /*fan=*/false, /*solid=*/true};
    if (chooseCubeDrawMode(cloud_view, kSolidOnly) == CubeDrawMode::kSkip) {
      continue;
    }

    const auto [color_minimum, color_maximum] = layer->colorRange();
    VoxelUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(fixed_from_grid, uniforms.fixed_from_grid);
    copyMatrix(frame.view, uniforms.view);
    uniforms.cell_size_opacity = {cell_size.x, cell_size.y, cell_size.z, layer->opacity()};
    uniforms.dims_kind = {
        static_cast<float>(layer->columnCount()), static_cast<float>(layer->rowCount()),
        static_cast<float>(layer->sliceCount()), layer->valueKind() == VoxelValueKind::kRgba ? 1.0F : 0.0F};
    uniforms.range_params = {layer->threshold(), layer->rangeMinimum(), layer->rangeMaximum(), color_minimum};
    uniforms.mode_params = {
        color_maximum, static_cast<float>(layer->drawMode()), static_cast<float>(layer->colormap()), 0.0F};
    frame.updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kVoxelUniformBytes, &uniforms);
    // Voxels write depth either way; below full opacity they blend, so they
    // draw after every opaque draw.
    draws_.push_back(
        Draw{
            layer, gpu.shader_resources, static_cast<quint32>(layer->voxelCount()),
            layer->opacity() < 0.999 ? DrawClass::kTranslucent : DrawClass::kOpaque});
    ++last_layer_count_;
    last_voxel_count_ += static_cast<int>(layer->voxelCount());
  }
}

bool VoxelRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(pipeline_, pass);
  if (pipeline == nullptr || geometry_ == nullptr || geometry_->cube_vertices == nullptr ||
      geometry_->cube_indices == nullptr) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.shader_resources);
  const QRhiCommandBuffer::VertexInput input(geometry_->cube_vertices, 0);
  command_buffer->setVertexInput(0, 1, &input, geometry_->cube_indices, 0, QRhiCommandBuffer::IndexUInt16);
  command_buffer->drawIndexed(static_cast<quint32>(kCubeIndices.size()), draw.instance_count);
  return true;
}

bool VoxelRhiPass::ensureLayerGpu(QRhi* owner, VoxelGridLayer* layer, LayerGpu& gpu, bool& texture_recreated) {
  texture_recreated = false;
  if (owner == nullptr || layer == nullptr || sampler_ == nullptr || colormap_texture_ == nullptr ||
      colormap_sampler_ == nullptr) {
    return false;
  }
  const QString rejection = gpuRejection(owner, layer);
  if (!rejection.isEmpty()) {
    layer->noteRenderFailure(rejection);
    return false;
  }
  const quint32 columns = layer->columnCount();
  const quint32 rows = layer->rowCount();
  const quint32 slices = layer->sliceCount();
  if (!ensureUniformBuffer(owner, gpu.uniform_buffer, kVoxelUniformBytes)) {
    layer->noteRenderFailure(tr("Could not create the voxel-grid uniform buffer"));
    releaseLayerGpu(gpu);
    return false;
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
    gpu.uploaded_revision = ~std::uint64_t{0};
    texture_recreated = true;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = createLayerBindings(owner, gpu.uniform_buffer, gpu.texture);
    if (gpu.shader_resources == nullptr) {
      layer->noteRenderFailure(tr("Could not create voxel-grid shader bindings"));
      releaseLayerGpu(gpu);
      return false;
    }
  }
  layer->noteRenderSuccess();
  return true;
}

QString VoxelRhiPass::gpuRejection(QRhi* owner, const VoxelGridLayer* layer) const {
  if (owner == nullptr || layer == nullptr) {
    return tr("The voxel-grid renderer is not available");
  }
  if (!owner->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
    return tr("This GPU does not support 3D textures");
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
  return tr("Voxel grid is %1 x %2 x %3; this GPU supports 3D textures up to %4 per dimension")
      .arg(QString::number(columns), QString::number(rows), QString::number(slices), QString::number(effective_limit));
}

void VoxelRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

void VoxelRhiPass::release() {
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  delete layout_shader_resources_;
  delete layout_uniform_buffer_;
  delete sampler_;
  delete layout_texture_;
  delete colormap_sampler_;
  delete colormap_texture_;
  pipeline_ = nullptr;
  layout_shader_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  sampler_ = nullptr;
  layout_texture_ = nullptr;
  render_mode_uniforms_ = nullptr;
  colormap_texture_ = nullptr;
  colormap_sampler_ = nullptr;
  geometry_ = nullptr;
  max_3d_texture_size_ = 0;
}

}  // namespace pj::scene3d
