// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pose_rhi_pass.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kPoseUniformBytes = 192U;

struct alignas(16) PoseUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
};
static_assert(sizeof(PoseUniforms) == kPoseUniformBytes);

}  // namespace

QRhiVertexInputLayout poseTriadInputLayout() {
  static_assert(sizeof(PoseTriadInstance) == 80U);
  static_assert(offsetof(PoseTriadInstance, color) == 64U);
  QRhiVertexInputLayout layout;
  layout.setBindings({
      QRhiVertexInputBinding(6U * sizeof(float)),
      QRhiVertexInputBinding(sizeof(PoseTriadInstance), QRhiVertexInputBinding::PerInstance),
  });
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3U * sizeof(float)),
      // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
      QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float4, 0U, 0),
      QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float4, 16U, 1),
      QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float4, 32U, 2),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 48U, 3),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 64U),
  });
  return layout;
}

bool PoseRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.geometry == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.annotation_render_mode_uniforms;
  geometry_ = setup.geometry;
  if (!ensureUniformBuffer(rhi, layout_uniform_buffer_, kPoseUniformBytes)) {
    return false;
  }
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniform_buffer_);
  // Annotation alpha: triads leave destination alpha (the present pass's
  // data/annotation split) unchanged.
  pipeline_ = setup.pipelines->add(
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/pose.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/pose.frag.qsb")),
          .input_layout = poseTriadInputLayout(),
          .bindings = layout_shader_resources_,
          .blend = annotationAlphaBlend(),
          .cull = QRhiGraphicsPipeline::Back,
      },
      setup.part);
  return pipeline_ != nullptr;
}

void PoseRhiPass::setLayers(const std::vector<PosesInFrameLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

QRhiShaderResourceBindings* PoseRhiPass::createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms) const {
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniforms),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

void PoseRhiPass::admit(const PlaceFn& place) {
  last_bounds_ = admitLayers(
      layers_, kSceneLimits.max_pose_arms_per_view, place, admitted_, layer_gpu_, releaseLayerGpu,
      [](const PosesInFrameLayer* layer) { return !layer->instances().empty(); },
      [](const PosesInFrameLayer* layer) { return static_cast<std::uint64_t>(layer->instances().size()); },
      [](const PosesInFrameLayer* layer, const Transform& placed) -> std::optional<AABB> {
        const auto source_bounds = layer->sourceBounds();
        return source_bounds.has_value() ? std::optional(transformedBounds(*source_bounds, placed)) : std::nullopt;
      });
}

void PoseRhiPass::prepare(
    const RhiFrameContext& frame, const std::vector<PoseTriadInstance>& tf_triads, std::uint64_t tf_triads_revision) {
  draws_.clear();
  last_layer_count_ = 0;
  last_arm_count_ = 0;
  for (const auto& admitted : admitted_) {
    PosesInFrameLayer* layer = admitted.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    if (!ensureLayerGpu(frame.rhi, gpu)) {
      continue;
    }
    // Static: poses change per sample, not per frame.
    if (gpu.uploaded_revision != layer->geometryRevision()) {
      const auto& instances = layer->instances();
      const quint32 bytes = static_cast<quint32>(instances.size() * sizeof(PoseTriadInstance));
      if (!ensureStaticVertexBuffer(frame.rhi, gpu.instance_buffer, gpu.instance_capacity, bytes, 1024U)) {
        gpu.instance_count = 0;
        continue;
      }
      if (bytes != 0) {
        PJ::uploadStaticBufferInPieces(frame.updates, gpu.instance_buffer, 0, bytes, instances.data());
      }
      gpu.instance_count = static_cast<quint32>(instances.size());
      gpu.uploaded_revision = layer->geometryRevision();
    }
    if (gpu.instance_buffer == nullptr || gpu.instance_count == 0) {
      continue;
    }

    PoseUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(relativeModel(admitted.fixed_from_source.matrix(), frame.render_origin), uniforms.fixed_from_source);
    copyMatrix(frame.view, uniforms.view);
    frame.updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kPoseUniformBytes, &uniforms);
    draws_.push_back(Draw{layer, gpu.instance_buffer, gpu.shader_resources, gpu.instance_count});
    ++last_layer_count_;
    last_arm_count_ += static_cast<int>(gpu.instance_count);
  }

  if (tf_triads.empty() || !ensureLayerGpu(frame.rhi, tf_triad_gpu_)) {
    tf_triad_instance_count_ = 0;
    return;
  }
  if (tf_triads_revision != tf_triads_uploaded_revision_ || tf_triad_gpu_.instance_buffer == nullptr) {
    const quint32 bytes = static_cast<quint32>(tf_triads.size() * sizeof(PoseTriadInstance));
    if (!ensureVertexBuffer(frame.rhi, tf_triad_gpu_.instance_buffer, tf_triad_gpu_.instance_capacity, bytes, bytes)) {
      tf_triad_instance_count_ = 0;
      return;
    }
    frame.updates->updateDynamicBuffer(tf_triad_gpu_.instance_buffer, 0, bytes, tf_triads.data());
    tf_triads_uploaded_revision_ = tf_triads_revision;
  }
  PoseUniforms uniforms;
  copyMatrix(frame.view_projection, uniforms.view_projection);
  copyMatrix(glm::mat4(1.0F), uniforms.fixed_from_source);
  copyMatrix(frame.view, uniforms.view);
  frame.updates->updateDynamicBuffer(tf_triad_gpu_.uniform_buffer, 0, kPoseUniformBytes, &uniforms);
  tf_triad_instance_count_ = static_cast<quint32>(tf_triads.size());
}

bool PoseRhiPass::drawArrows(
    QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport, QRhiShaderResourceBindings* shader_resources, QRhiBuffer* instance_buffer,
    quint32 instance_count) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(pipeline_, pass);
  if (pipeline == nullptr || geometry_ == nullptr || geometry_->arrow_vertices == nullptr ||
      geometry_->arrow_indices == nullptr) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(shader_resources);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(geometry_->arrow_vertices, 0),
      QRhiCommandBuffer::VertexInput(instance_buffer, 0),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), geometry_->arrow_indices, 0, QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(geometry_->arrow_index_count, instance_count);
  return true;
}

bool PoseRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  return drawArrows(
      command_buffer, pass, registry, viewport, draw.shader_resources, draw.instance_buffer, draw.instance_count);
}

void PoseRhiPass::submitTfTriads(
    QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  if (tf_triad_instance_count_ == 0U || tf_triad_gpu_.instance_buffer == nullptr ||
      tf_triad_gpu_.shader_resources == nullptr) {
    return;
  }
  (void)drawArrows(
      command_buffer, pass, registry, viewport, tf_triad_gpu_.shader_resources, tf_triad_gpu_.instance_buffer,
      tf_triad_instance_count_);
}

bool PoseRhiPass::ensureLayerGpu(QRhi* owner, LayerGpu& gpu) const {
  if (owner == nullptr) {
    return false;
  }
  if (!ensureUniformBuffer(owner, gpu.uniform_buffer, kPoseUniformBytes)) {
    releaseLayerGpu(gpu);
    return false;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = createLayerBindings(owner, gpu.uniform_buffer);
    if (gpu.shader_resources == nullptr) {
      releaseLayerGpu(gpu);
      return false;
    }
  }
  return true;
}

void PoseRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.instance_buffer;
  delete gpu.uniform_buffer;
  gpu = {};
}

void PoseRhiPass::release() {
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  releaseLayerGpu(tf_triad_gpu_);
  tf_triad_instance_count_ = 0;
  tf_triads_uploaded_revision_ = ~std::uint64_t{0};
  admitted_.clear();
  draws_.clear();
  delete layout_shader_resources_;
  delete layout_uniform_buffer_;
  pipeline_ = nullptr;
  layout_shader_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  render_mode_uniforms_ = nullptr;
  geometry_ = nullptr;
}

}  // namespace pj::scene3d
