// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "occupancy_rhi_pass.h"

#include <QByteArray>
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kOccupancyUniformBytes = 144U;
constexpr std::array<float, 12> kOccupancyQuad = {0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F,
                                                  0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};

struct alignas(16) OccupancyUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_grid{};
  std::array<float, 4> display_params{};
};
static_assert(sizeof(OccupancyUniforms) == kOccupancyUniformBytes);

AABB occupancyBoundsInFixed(const ReconstructedGrid& grid, const Transform& fixed_from_source) {
  if (grid.empty() || grid.width == 0U || grid.height == 0U || grid.resolution <= 0.0) {
    return {};
  }
  const Transform fixed_from_grid = fixed_from_source * poseTransform(grid.origin);
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

QRhiShaderResourceBindings* OccupancyRhiPass::createLayerBindings(
    QRhi* rhi, QRhiBuffer* uniforms, QRhiTexture* texture) const {
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms),
          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture, sampler_),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

bool OccupancyRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.render_mode_uniforms;
  layout_texture_ = rhi->newTexture(QRhiTexture::R8, QSize(1, 1));
  sampler_ = rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  quad_buffer_ = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(kOccupancyQuad.size() * sizeof(float)));
  if (!ensureUniformBuffer(rhi, layout_uniform_buffer_, kOccupancyUniformBytes) || !layout_texture_->create() ||
      !sampler_->create() || !quad_buffer_->create()) {
    return false;
  }
  setup.updates->uploadStaticBuffer(quad_buffer_, kOccupancyQuad.data());
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniform_buffer_, layout_texture_);
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(2U * sizeof(float))});
  layout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0U)});
  pipeline_ = setup.pipelines->add(
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/occupancy.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/occupancy.frag.qsb")),
          .input_layout = layout,
          .bindings = layout_shader_resources_,
          .depth_write = false,
          .depth_bias = -1,
      },
      setup.part);
  return pipeline_ != nullptr;
}

void OccupancyRhiPass::setLayers(const std::vector<OccupancyGridLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

void OccupancyRhiPass::admit(const PlaceFn& place) {
  last_bounds_ = admitLayers(
      layers_, kSceneLimits.max_occupancy_cells_per_view, place, admitted_, layer_gpu_, releaseLayerGpu,
      [](const OccupancyGridLayer* layer) { return !layer->grid().empty(); },
      [](const OccupancyGridLayer* layer) {
        return static_cast<std::uint64_t>(layer->grid().width) * layer->grid().height;
      },
      [](const OccupancyGridLayer* layer, const Transform& placed) -> std::optional<AABB> {
        const AABB layer_bounds = occupancyBoundsInFixed(layer->grid(), placed);
        return layer_bounds.valid ? std::optional(layer_bounds) : std::nullopt;
      });
}

void OccupancyRhiPass::prepare(const RhiFrameContext& frame) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  draws_.reserve(admitted_.size());
  last_layer_count_ = 0;
  last_cell_count_ = 0;
  for (const auto& admitted : admitted_) {
    OccupancyGridLayer* layer = admitted.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    bool texture_recreated = false;
    if (!ensureLayerGpu(frame.rhi, layer, gpu, texture_recreated)) {
      continue;
    }
    const ReconstructedGrid& grid = layer->grid();
    if (gpu.uploaded_revision != layer->textureRevision()) {
      const bool full_upload = texture_recreated || !layer->dirtyRectsCatchUp(gpu.uploaded_revision);
      if (full_upload) {
        QRhiTextureSubresourceUploadDescription description(grid.cells.data(), static_cast<quint32>(grid.cells.size()));
        description.setSourceSize(QSize(static_cast<int>(grid.width), static_cast<int>(grid.height)));
        frame.updates->uploadTexture(gpu.texture, QRhiTextureUploadDescription({0, 0, description}));
        ++full_upload_count_;
      } else {
        for (const CellRect& rect : layer->dirtyRects()) {
          const qsizetype rect_width = static_cast<qsizetype>(rect.width);
          const qsizetype rect_height = static_cast<qsizetype>(rect.height);
          QByteArray packed(rect_width * rect_height, Qt::Uninitialized);
          for (std::uint32_t row = 0; row < rect.height; ++row) {
            std::memcpy(
                packed.data() + static_cast<qsizetype>(row) * rect_width,
                grid.cells.data() + static_cast<std::size_t>(rect.y + row) * grid.width + rect.x, rect.width);
          }
          QRhiTextureSubresourceUploadDescription description(packed);
          description.setSourceSize(QSize(static_cast<int>(rect.width), static_cast<int>(rect.height)));
          description.setDestinationTopLeft(QPoint(static_cast<int>(rect.x), static_cast<int>(rect.y)));
          frame.updates->uploadTexture(gpu.texture, QRhiTextureUploadDescription({0, 0, description}));
        }
        if (!layer->dirtyRects().empty()) {
          ++partial_upload_count_;
        }
      }
      gpu.uploaded_revision = layer->textureRevision();
    }

    const glm::dmat4 fixed_from_grid = glm::scale(
        admitted.fixed_from_source.matrix() * poseTransform(grid.origin).matrix(),
        glm::dvec3(
            grid.resolution * static_cast<double>(grid.width), grid.resolution * static_cast<double>(grid.height),
            1.0));

    OccupancyUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(relativeModel(fixed_from_grid, frame.render_origin), uniforms.fixed_from_grid);
    uniforms.display_params = {
        layer->opacity(), layer->colorScheme() == OccupancyColorScheme::kCostmap ? 1.0F : 0.0F, 0.0F, 0.0F};
    frame.updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kOccupancyUniformBytes, &uniforms);
    // The grid never writes depth; below full opacity it blends over whatever
    // opaque geometry lies under it, so it draws with the translucent phase.
    const DrawClass draw_class = layer->opacity() < 0.999F ? DrawClass::kTranslucent : DrawClass::kOpaque;
    draws_.push_back(Draw{layer, gpu.shader_resources, draw_class});
    ++last_layer_count_;
    last_cell_count_ += static_cast<int>(grid.cells.size());
  }
}

bool OccupancyRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(pipeline_, pass);
  if (pipeline == nullptr || quad_buffer_ == nullptr) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.shader_resources);
  const QRhiCommandBuffer::VertexInput input(quad_buffer_, 0);
  command_buffer->setVertexInput(0, 1, &input);
  command_buffer->draw(6U);
  return true;
}

bool OccupancyRhiPass::ensureLayerGpu(QRhi* owner, OccupancyGridLayer* layer, LayerGpu& gpu, bool& texture_recreated) {
  texture_recreated = false;
  if (owner == nullptr || layer == nullptr || sampler_ == nullptr) {
    return false;
  }
  const ReconstructedGrid& grid = layer->grid();
  const int texture_limit = owner->resourceLimit(QRhi::TextureSizeMax);
  if (texture_limit <= 0 || grid.width == 0U || grid.height == 0U || grid.width > static_cast<quint32>(texture_limit) ||
      grid.height > static_cast<quint32>(texture_limit)) {
    layer->noteRenderFailure(
        tr("Occupancy grid is %1 x %2; this GPU supports textures up to %3 x %3")
            .arg(QString::number(grid.width), QString::number(grid.height), QString::number(texture_limit)));
    return false;
  }
  if (!ensureUniformBuffer(owner, gpu.uniform_buffer, kOccupancyUniformBytes)) {
    layer->noteRenderFailure(tr("Could not create the occupancy-grid uniform buffer"));
    releaseLayerGpu(gpu);
    return false;
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
    gpu.uploaded_revision = ~std::uint64_t{0};
    texture_recreated = true;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = createLayerBindings(owner, gpu.uniform_buffer, gpu.texture);
    if (gpu.shader_resources == nullptr) {
      layer->noteRenderFailure(tr("Could not create occupancy-grid shader bindings"));
      releaseLayerGpu(gpu);
      return false;
    }
  }
  layer->noteRenderSuccess();
  return true;
}

void OccupancyRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

void OccupancyRhiPass::release() {
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  delete layout_shader_resources_;
  delete layout_uniform_buffer_;
  delete quad_buffer_;
  delete sampler_;
  delete layout_texture_;
  pipeline_ = nullptr;
  layout_shader_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  quad_buffer_ = nullptr;
  sampler_ = nullptr;
  layout_texture_ = nullptr;
  render_mode_uniforms_ = nullptr;
}

}  // namespace pj::scene3d
