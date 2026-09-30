// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "grid_rhi_pass.h"

#include <cstddef>
#include <utility>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kInitialVertexBufferBytes = 64U * 1024U;

}  // namespace

QRhiVertexInputLayout GridRhiPass::vertexLayout() {
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(Vertex, r)),
  });
  return layout;
}

bool GridRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.view_uniforms == nullptr || setup.annotation_render_mode_uniforms == nullptr) {
    return false;
  }
  setup_ = setup;
  shader_resources_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.render_mode_uniforms),
           });
  annotation_shader_resources_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.annotation_render_mode_uniforms),
           });
  PipelineDesc desc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/scene.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/scene.frag.qsb")),
      .input_layout = vertexLayout(),
      .bindings = shader_resources_,
  };
  triangle_pipeline_ = createScenePipeline(setup, desc);
  desc.topology = QRhiGraphicsPipeline::Lines;
  line_pipeline_ = createScenePipeline(setup, desc);
  desc.blend = annotationAlphaBlend();
  connection_line_pipeline_ = createScenePipeline(setup, desc);
  return ready();
}

void GridRhiPass::registerPipelines(ScenePipelineRegistry& registry) const {
  registry.add(line_pipeline_);
  registry.add(triangle_pipeline_);
  registry.add(connection_line_pipeline_);
}

bool GridRhiPass::createShadowReceiver(
    QRhiBuffer* lighting_uniforms, QRhiTexture* shadow_map, QRhiSampler* shadow_sampler,
    ScenePipelineRegistry& registry) {
  if (shadow_pipeline_ != nullptr) {
    return true;
  }
  if (setup_.rhi == nullptr || lighting_uniforms == nullptr || shadow_map == nullptr || shadow_sampler == nullptr) {
    return false;
  }
  shadow_shader_resources_ = createBindings(
      setup_.rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup_.view_uniforms),
          QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage, lighting_uniforms),
          QRhiShaderResourceBinding::sampledTexture(
              2, QRhiShaderResourceBinding::FragmentStage, shadow_map, shadow_sampler),
          QRhiShaderResourceBinding::uniformBuffer(
              8, QRhiShaderResourceBinding::FragmentStage, setup_.render_mode_uniforms),
      });
  shadow_pipeline_ = createScenePipeline(
      setup_, {
                  .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_shadow.vert.qsb")),
                  .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_shadow.frag.qsb")),
                  .input_layout = vertexLayout(),
                  .bindings = shadow_shader_resources_,
              });
  if (shadow_pipeline_ == nullptr) {
    releaseShadowReceiver(registry);
    return false;
  }
  registry.add(shadow_pipeline_);
  return true;
}

void GridRhiPass::releaseShadowReceiver(ScenePipelineRegistry& registry) {
  registry.remove(shadow_pipeline_);
  delete shadow_pipeline_;
  delete shadow_shader_resources_;
  shadow_pipeline_ = nullptr;
  shadow_shader_resources_ = nullptr;
}

void GridRhiPass::appendVertex(std::vector<Vertex>& out, const glm::dvec3& point, const glm::vec4& color) {
  out.push_back(
      Vertex{
          static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z), color.r, color.g,
          color.b, color.a});
}

void GridRhiPass::build(
    const Settings& settings, const TransformBuffer* tf, const std::string& fixed_frame, PJ::Timepoint time,
    const glm::dvec3& render_origin) {
  BuildKey key{settings, tf, tf != nullptr ? tf->revision() : 0U, fixed_frame, time, render_origin};
  if (built_key_ == key) {
    return;
  }
  built_key_ = std::move(key);
  line_vertices_.clear();
  triangle_vertices_.clear();
  if (settings.grid_visible) {
    if (settings.style == GridStyle::kFilledCells) {
      for (const GridVertex& vertex : buildCheckerboardCells(settings.extent_m, settings.divisions)) {
        const glm::vec4 color =
            vertex.parity < 0.5F ? glm::vec4(look::kGridCellToneA, 1.0F) : glm::vec4(look::kGridCellToneB, 1.0F);
        appendVertex(triangle_vertices_, glm::dvec3(vertex.pos) - render_origin, color);
      }
    }
    for (const GridVertex& vertex : buildGridLines(settings.extent_m, settings.divisions)) {
      appendVertex(line_vertices_, glm::dvec3(vertex.pos) - render_origin, glm::vec4(look::kGridLineColor, 1.0F));
    }
  }
  grid_line_vertex_count_ = static_cast<quint32>(line_vertices_.size());

  if (tf != nullptr && !fixed_frame.empty() && settings.connections_visible) {
    buildTfConnectionSegments(*tf, fixed_frame, time, segments_, render_origin);
    const glm::vec4 color(look::kTfConnectionColor, 1.0F);
    for (const TfConnectionSegment& segment : segments_) {
      appendVertex(line_vertices_, glm::dvec3(segment.child), color);
      appendVertex(line_vertices_, glm::dvec3(segment.parent), color);
    }
  }
  upload_pending_ = true;
}

void GridRhiPass::prepare(const RhiFrameContext& frame, bool shadow_active) {
  receive_shadow_ = shadow_active && shadow_pipeline_ != nullptr;
  if (!upload_pending_) {
    return;
  }
  const auto line_bytes = static_cast<quint32>(line_vertices_.size() * sizeof(Vertex));
  const auto triangle_bytes = static_cast<quint32>(triangle_vertices_.size() * sizeof(Vertex));
  buffers_ready_ =
      ensureVertexBuffer(frame.rhi, line_buffer_, line_capacity_, line_bytes, kInitialVertexBufferBytes) &&
      ensureVertexBuffer(frame.rhi, triangle_buffer_, triangle_capacity_, triangle_bytes, kInitialVertexBufferBytes);
  if (!buffers_ready_) {
    return;  // retried next frame
  }
  if (line_bytes != 0) {
    frame.updates->updateDynamicBuffer(line_buffer_, 0, line_bytes, line_vertices_.data());
  }
  if (triangle_bytes != 0) {
    frame.updates->updateDynamicBuffer(triangle_buffer_, 0, triangle_bytes, triangle_vertices_.data());
  }
  upload_pending_ = false;
}

void GridRhiPass::submit(
    QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  if (!buffers_ready_ || !ready()) {
    return;
  }
  const auto draw = [&](QRhiGraphicsPipeline* direct, QRhiShaderResourceBindings* resources, QRhiBuffer* buffer,
                        quint32 first, quint32 count) {
    QRhiGraphicsPipeline* pipeline = registry.resolve(direct, pass);
    if (pipeline == nullptr || count == 0U) {
      return;
    }
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(resources);
    const QRhiCommandBuffer::VertexInput input(buffer, first * static_cast<quint32>(sizeof(Vertex)));
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(count);
  };
  draw(
      receive_shadow_ ? shadow_pipeline_ : triangle_pipeline_,
      receive_shadow_ ? shadow_shader_resources_ : shader_resources_, triangle_buffer_, 0U,
      static_cast<quint32>(triangle_vertices_.size()));
  draw(line_pipeline_, shader_resources_, line_buffer_, 0U, grid_line_vertex_count_);
  // Only the HDR target carries the data/annotation split in its alpha: the
  // direct pass and the depth replay's coverage draw the connections like grid lines.
  draw(
      pass == ScenePass::kHdrColor ? connection_line_pipeline_ : line_pipeline_, annotation_shader_resources_,
      line_buffer_, grid_line_vertex_count_, static_cast<quint32>(line_vertices_.size()) - grid_line_vertex_count_);
}

void GridRhiPass::release() {
  delete shadow_pipeline_;
  delete shadow_shader_resources_;
  delete connection_line_pipeline_;
  delete line_pipeline_;
  delete triangle_pipeline_;
  delete annotation_shader_resources_;
  delete shader_resources_;
  delete line_buffer_;
  delete triangle_buffer_;
  shadow_pipeline_ = nullptr;
  shadow_shader_resources_ = nullptr;
  connection_line_pipeline_ = nullptr;
  line_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  annotation_shader_resources_ = nullptr;
  shader_resources_ = nullptr;
  line_buffer_ = nullptr;
  triangle_buffer_ = nullptr;
  line_capacity_ = 0;
  triangle_capacity_ = 0;
  buffers_ready_ = false;
  upload_pending_ = true;
  built_key_.reset();
  receive_shadow_ = false;
  setup_ = {};
}

}  // namespace pj::scene3d
