// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "grid_rhi_pass.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kMinimumGridBufferBytes = 1024U;
constexpr quint32 kInitialConnectionBufferBytes = 4U * 1024U;
constexpr quint32 kOffsetUniformBytes = 4U * sizeof(float);
constexpr quint32 kGridUniformBytes = 8U * sizeof(float);
// Binding 3 (origin offset and line parameters) of every grid SRB: pipelines built
// on one SRB draw with another only when their layouts match.
constexpr auto kGridStages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;

glm::vec4 opaque(const glm::dvec3& color) {
  return {static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), 1.0F};
}

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
  if (rhi == nullptr || setup.view_uniforms == nullptr || setup.annotation_render_mode_uniforms == nullptr ||
      !ensureUniformBuffer(rhi, origin_offset_uniforms_, kGridUniformBytes) ||
      !ensureUniformBuffer(rhi, zero_offset_uniforms_, kOffsetUniformBytes)) {
    return false;
  }
  const std::array<float, 4> zero{};
  setup.updates->updateDynamicBuffer(zero_offset_uniforms_, 0, kOffsetUniformBytes, zero.data());
  setup_ = setup;
  setup_.updates = nullptr;  // the batch is valid only during initialize()
  shader_resources_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               QRhiShaderResourceBinding::uniformBuffer(3, kGridStages, origin_offset_uniforms_),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.render_mode_uniforms),
           });
  annotation_shader_resources_ = createBindings(
      rhi, {
               QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, setup.view_uniforms),
               // The connections bind line_pipeline_'s layout: binding 3 has the same stages.
               QRhiShaderResourceBinding::uniformBuffer(3, kGridStages, zero_offset_uniforms_),
               QRhiShaderResourceBinding::uniformBuffer(
                   8, QRhiShaderResourceBinding::FragmentStage, setup.annotation_render_mode_uniforms),
           });
  triangle_pipeline_ = setup.pipelines->add(
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_cell.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_cell.frag.qsb")),
          .input_layout = vertexLayout(),
          .bindings = shader_resources_,
      },
      setup.part);
  PipelineDesc desc{
      .topology = QRhiGraphicsPipeline::Lines,
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/scene.frag.qsb")),
      .input_layout = vertexLayout(),
      .bindings = shader_resources_,
  };
  line_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.blend = annotationAlphaBlend();
  connection_line_pipeline_ = setup.pipelines->add(desc, setup.part);
  return ready();
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
          QRhiShaderResourceBinding::uniformBuffer(3, kGridStages, origin_offset_uniforms_),
          QRhiShaderResourceBinding::uniformBuffer(
              8, QRhiShaderResourceBinding::FragmentStage, setup_.render_mode_uniforms),
      });
  // Optional: a GPU that cannot build it draws the cells unshadowed, and the
  // view turns shadows off (shadowReceiverFailed()), instead of failing the scene.
  shadow_pipeline_ = registry.add(
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_cell.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/grid_cell_shadow.frag.qsb")),
          .input_layout = vertexLayout(),
          .bindings = shadow_shader_resources_,
      },
      setup_.part, /*optional=*/true);
  if (shadow_pipeline_ == nullptr) {
    releaseShadowReceiver(registry);
    return false;
  }
  return true;
}

void GridRhiPass::releaseShadowReceiver(ScenePipelineRegistry& registry) {
  registry.remove(shadow_pipeline_);
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
  const int divisions = std::max(settings.divisions, 1);
  const float cell = settings.extent_m / static_cast<float>(divisions);
  grid_uniforms_ = {static_cast<float>(-render_origin.x),       static_cast<float>(-render_origin.y),
                    static_cast<float>(-render_origin.z),       divisions % 2 == 1 ? 0.5F * cell : 0.0F,
                    static_cast<float>(settings.colors.line.r), static_cast<float>(settings.colors.line.g),
                    static_cast<float>(settings.colors.line.b), cell};

  if (grid_key_ != settings) {
    grid_key_ = settings;
    grid_vertices_.clear();
    grid_cell_vertex_count_ = 0;
    grid_line_vertex_count_ = 0;
    // Each style is one buffer range: line primitives, or cells whose shader
    // draws the lines.
    if (settings.grid_visible && settings.style == GridStyle::kFilledCells) {
      const glm::vec4 tone_a = opaque(settings.colors.tone_a);
      const glm::vec4 tone_b = opaque(settings.colors.tone_b);
      for (const GridVertex& vertex : buildCheckerboardCells(settings.extent_m, settings.divisions)) {
        appendVertex(grid_vertices_, glm::dvec3(vertex.pos), vertex.parity < 0.5F ? tone_a : tone_b);
      }
      grid_cell_vertex_count_ = static_cast<quint32>(grid_vertices_.size());
    } else if (settings.grid_visible) {
      const glm::vec4 line = opaque(settings.colors.line);
      for (const GridVertex& vertex : buildGridLines(settings.extent_m, settings.divisions)) {
        appendVertex(grid_vertices_, glm::dvec3(vertex.pos), line);
      }
      grid_line_vertex_count_ = static_cast<quint32>(grid_vertices_.size());
    }
    grid_upload_pending_ = true;
    ++grid_build_count_;
  }

  ConnectionKey connection_key{
      settings.connections_visible, tf, tf != nullptr ? tf->revision() : 0U, fixed_frame, time, render_origin};
  if (connection_key_ != connection_key) {
    connection_key_ = std::move(connection_key);
    connection_vertices_.clear();
    if (tf != nullptr && !fixed_frame.empty() && settings.connections_visible) {
      buildTfConnectionSegments(*tf, fixed_frame, time, segments_, render_origin);
      const glm::vec4 color(look::kTfConnectionColor, 1.0F);
      for (const TfConnectionSegment& segment : segments_) {
        appendVertex(connection_vertices_, glm::dvec3(segment.child), color);
        appendVertex(connection_vertices_, glm::dvec3(segment.parent), color);
      }
    }
    connection_upload_pending_ = true;
    ++connection_build_count_;
  }
}

void GridRhiPass::prepare(const RhiFrameContext& frame, bool shadow_active) {
  receive_shadow_ = shadow_active && shadow_pipeline_ != nullptr;
  frame.updates->updateDynamicBuffer(origin_offset_uniforms_, 0, kGridUniformBytes, grid_uniforms_.data());
  // A failed allocation retries next frame.
  if (grid_upload_pending_) {
    const auto bytes = static_cast<quint32>(grid_vertices_.size() * sizeof(Vertex));
    grid_ready_ = ensureStaticVertexBuffer(frame.rhi, grid_buffer_, grid_capacity_, bytes, kMinimumGridBufferBytes);
    if (grid_ready_) {
      if (bytes != 0) {
        PJ::uploadStaticBufferInPieces(frame.updates, grid_buffer_, 0, bytes, grid_vertices_.data());
      }
      grid_upload_pending_ = false;
    }
  }
  if (connection_upload_pending_) {
    const auto bytes = static_cast<quint32>(connection_vertices_.size() * sizeof(Vertex));
    connections_ready_ =
        ensureVertexBuffer(frame.rhi, connection_buffer_, connection_capacity_, bytes, kInitialConnectionBufferBytes);
    if (connections_ready_) {
      if (bytes != 0) {
        frame.updates->updateDynamicBuffer(connection_buffer_, 0, bytes, connection_vertices_.data());
      }
      connection_upload_pending_ = false;
    }
  }
}

void GridRhiPass::submit(
    QRhiCommandBuffer* command_buffer, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  if (!ready()) {
    return;
  }
  const auto draw = [&](const ScenePipeline* registered, QRhiShaderResourceBindings* resources, QRhiBuffer* buffer,
                        quint32 first, quint32 count) {
    if (buffer == nullptr || count == 0U) {
      return;  // before resolve(): an empty part (the cells of the line style) builds no variant
    }
    QRhiGraphicsPipeline* pipeline = registry.resolve(registered, pass);
    if (pipeline == nullptr) {
      return;
    }
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(resources);
    const QRhiCommandBuffer::VertexInput input(buffer, first * static_cast<quint32>(sizeof(Vertex)));
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(count);
  };
  if (grid_ready_) {
    const bool shadowed =
        receive_shadow_ && grid_cell_vertex_count_ != 0U && registry.resolve(shadow_pipeline_, pass) != nullptr;
    draw(
        shadowed ? shadow_pipeline_ : triangle_pipeline_, shadowed ? shadow_shader_resources_ : shader_resources_,
        grid_buffer_, 0U, grid_cell_vertex_count_);
    draw(line_pipeline_, shader_resources_, grid_buffer_, grid_cell_vertex_count_, grid_line_vertex_count_);
  }
  // Only the HDR target carries the data/annotation split in its alpha: the
  // direct pass and the depth replay's coverage draw the connections like grid lines.
  if (connections_ready_) {
    draw(
        pass == ScenePass::kHdrColor ? connection_line_pipeline_ : line_pipeline_, annotation_shader_resources_,
        connection_buffer_, 0U, static_cast<quint32>(connection_vertices_.size()));
  }
}

void GridRhiPass::release() {
  delete shadow_shader_resources_;
  delete annotation_shader_resources_;
  delete shader_resources_;
  delete grid_buffer_;
  delete connection_buffer_;
  delete origin_offset_uniforms_;
  delete zero_offset_uniforms_;
  shadow_pipeline_ = nullptr;
  shadow_shader_resources_ = nullptr;
  connection_line_pipeline_ = nullptr;
  line_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  annotation_shader_resources_ = nullptr;
  shader_resources_ = nullptr;
  grid_buffer_ = nullptr;
  connection_buffer_ = nullptr;
  origin_offset_uniforms_ = nullptr;
  zero_offset_uniforms_ = nullptr;
  grid_capacity_ = 0;
  connection_capacity_ = 0;
  grid_ready_ = false;
  connections_ready_ = false;
  grid_upload_pending_ = true;
  connection_upload_pending_ = true;
  grid_key_.reset();
  connection_key_.reset();
  receive_shadow_ = false;
  setup_ = {};
}

}  // namespace pj::scene3d
