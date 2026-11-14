// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "meshmap_rhi_pass.h"

#include <QCoreApplication>
#include <array>
#include <cmath>
#include <limits>

#include "lit_surface_uniforms.h"
#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_widgets/layers/mesh_map_layer.h"
#include "pj_widgets/GraphicsApi.h"  // uploadStaticBufferInPieces

namespace pj::scene3d {
namespace {

struct alignas(16) MeshMapUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> model{};
  std::array<float, 4> eye{};
  std::array<float, 4> view_z{};
  std::array<float, 4> key_dir{};
  std::array<float, 4> color_params{};
  std::array<float, 4> light_params{};
  std::array<float, 4> flags{};
};
constexpr quint32 kUniformBytes = sizeof(MeshMapUniforms);
static_assert(kUniformBytes == 224U);

// The shader's colour modes (meshmap.frag `flags.x`).
constexpr float kModeChannel = 0.0F;
constexpr float kModeVertexColor = 1.0F;  // RGBA8 colours: vertex colour or a categorical palette
constexpr float kModeSolid = 2.0F;
constexpr float kModeLine = 3.0F;

constexpr quint32 kMinimumBufferBytes = 64U * 1024U;

bool drawable(const MeshMapUpload& mesh) {
  return !mesh.positions.empty() && mesh.positions.size() % 9 == 0;
}

QRhiVertexInputLayout inputLayout() {
  // binding 0: position, 1: channel value, 2: RGBA8 colour. A draw that does not
  // read the value or the colour binds another buffer of at least 4 bytes per
  // vertex there; the shader ignores it in that mode.
  QRhiVertexInputLayout layout;
  layout.setBindings({
      QRhiVertexInputBinding(3 * sizeof(float)),
      QRhiVertexInputBinding(sizeof(float)),
      QRhiVertexInputBinding(sizeof(std::uint32_t)),
  });
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
      QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float, 0),
      QRhiVertexInputAttribute(2, 2, QRhiVertexInputAttribute::UNormByte4, 0),
  });
  return layout;
}

// Grows `buffer` to hold `bytes`; false when it cannot be allocated.
bool ensureBuffer(QRhi* rhi, QRhiBuffer*& buffer, quint32& capacity, std::size_t bytes) {
  return bytes <= std::numeric_limits<quint32>::max() &&
         ensureStaticVertexBuffer(rhi, buffer, capacity, static_cast<quint32>(bytes), kMinimumBufferBytes);
}

// Records the upload of `bytes` of `data` into `buffer`, already large enough.
void recordUpload(QRhiResourceUpdateBatch* updates, QRhiBuffer* buffer, const void* data, std::size_t bytes) {
  if (bytes != 0) {
    PJ::uploadStaticBufferInPieces(updates, buffer, 0, static_cast<quint32>(bytes), data);
  }
}

}  // namespace

QRhiShaderResourceBindings* MeshMapRhiPass::createLayerBindings(QRhi* rhi, QRhiBuffer* uniforms) const {
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

bool MeshMapRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  render_mode_uniforms_ = setup.render_mode_uniforms;
  if (!ensureUniformBuffer(rhi, layout_uniform_buffer_, kUniformBytes)) {
    return false;
  }
  layout_shader_resources_ = createLayerBindings(rhi, layout_uniform_buffer_);
  PipelineDesc desc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/meshmap.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/meshmap.frag.qsb")),
      .input_layout = inputLayout(),
      .bindings = layout_shader_resources_,
  };
  opaque_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.depth_write = false;  // depth test only
  translucent_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.topology = QRhiGraphicsPipeline::Lines;
  translucent_line_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.depth_write = true;
  line_pipeline_ = setup.pipelines->add(desc, setup.part);
  return ready();
}

void MeshMapRhiPass::setLayers(const std::vector<MeshMapLayer*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, releaseLayerGpu);
}

void MeshMapRhiPass::admit(const PlaceFn& place) {
  admitted_.clear();
  for (MeshMapLayer* layer : layers_) {
    if (layer == nullptr || !layer->visible()) {
      continue;
    }
    layer->prepareForRender();
    const std::optional<MeshMapUpload>& mesh = layer->stagedMesh();
    if (!mesh.has_value() || !drawable(*mesh)) {
      pruneLayerGpu(layer_gpu_, layer, releaseLayerGpu);
      continue;
    }
    const auto placed = place(mesh->frame_id, layer->renderStamp(), layer->placementHold());
    if (!placed.has_value()) {
      continue;
    }
    admitted_.push_back({layer, *placed});
  }
}

bool MeshMapRhiPass::upload(QRhiResourceUpdateBatch* updates, QRhi* rhi, MeshMapLayer* layer, LayerGpu& gpu) {
  const MeshMapUpload& mesh = *layer->stagedMesh();
  const bool geometry = gpu.uploaded_geometry != mesh.geometry_revision;
  const bool colors = geometry || gpu.uploaded_revision != layer->stagedRevision();  // colours follow the geometry
  if (!colors) {
    return true;
  }
  // One revision behind, with only a span of corners rewritten since: the buffer
  // already holds the rest. Both colour attributes are four bytes per corner.
  const std::optional<ElementRange>& delta = layer->stagedColorDelta();
  if (!geometry && delta.has_value() && gpu.uploaded_revision + 1 == layer->stagedRevision()) {
    const void* corners = mesh.color_mode == MeshMapColorMode::kChannel ? static_cast<const void*>(mesh.values.data())
                                                                        : static_cast<const void*>(mesh.colors.data());
    const quint32 offset = delta->begin * 4U;
    const quint32 bytes = delta->empty() ? 0U : (delta->end - delta->begin) * 4U;
    if (bytes != 0 && gpu.attributes != nullptr && offset + bytes <= gpu.attributes_capacity) {
      PJ::uploadStaticBufferInPieces(
          updates, gpu.attributes, offset, bytes, static_cast<const char*>(corners) + offset);
      color_upload_bytes_ += bytes;
    }
    gpu.uploaded_revision = layer->stagedRevision();
    ++color_upload_count_;
    return true;
  }
  const void* attribute_data = nullptr;
  std::size_t attribute_bytes = 0;
  if (mesh.color_mode == MeshMapColorMode::kChannel) {
    attribute_data = mesh.values.data();  // "no data" already kMeshMapNoDataValue
    attribute_bytes = mesh.values.size() * sizeof(float);
  } else if (mesh.color_mode == MeshMapColorMode::kRgba) {
    attribute_data = mesh.colors.data();
    attribute_bytes = mesh.colors.size() * sizeof(std::uint32_t);
  }

  // Every buffer is allocated before any upload is recorded: the frame's batch
  // keeps raw buffer pointers, so a failure must not delete one it references.
  const std::size_t link_position_bytes = mesh.link_positions.size() * sizeof(float);
  const std::size_t link_color_bytes = mesh.link_colors.size() * sizeof(std::uint32_t);
  if (geometry && (!ensureBuffer(rhi, gpu.positions, gpu.positions_capacity, mesh.positions.size() * sizeof(float)) ||
                   !ensureBuffer(rhi, gpu.link_positions, gpu.link_positions_capacity, link_position_bytes) ||
                   !ensureBuffer(rhi, gpu.link_colors, gpu.link_colors_capacity, link_color_bytes))) {
    layer->noteRenderFailure(tr("Could not allocate the mesh-map vertex buffers"));
    releaseLayerGpu(gpu);
    return false;
  }
  if (!ensureBuffer(rhi, gpu.attributes, gpu.attributes_capacity, attribute_bytes)) {
    layer->noteRenderFailure(tr("Could not allocate the mesh-map colour buffer"));
    releaseLayerGpu(gpu);
    return false;
  }

  if (geometry) {
    recordUpload(updates, gpu.positions, mesh.positions.data(), mesh.positions.size() * sizeof(float));
    recordUpload(updates, gpu.link_positions, mesh.link_positions.data(), link_position_bytes);
    recordUpload(updates, gpu.link_colors, mesh.link_colors.data(), link_color_bytes);
    gpu.surface_vertices = static_cast<quint32>(mesh.positions.size() / 3);
    gpu.link_vertices = static_cast<quint32>(mesh.link_positions.size() / 3);
    gpu.uploaded_geometry = mesh.geometry_revision;
    ++geometry_upload_count_;
  }
  recordUpload(updates, gpu.attributes, attribute_data, attribute_bytes);
  color_upload_bytes_ += attribute_bytes;
  gpu.attributes_in_use = mesh.color_mode != MeshMapColorMode::kSolid;
  gpu.uploaded_revision = layer->stagedRevision();
  ++color_upload_count_;
  return true;
}

void MeshMapRhiPass::prepare(const RhiFrameContext& frame, const MeshShadingParams& shading) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  last_layer_count_ = 0;
  last_vertex_count_ = 0;
  const LitSurfaceFrame lit = litSurfaceFrame(frame, shading);
  for (const auto& admitted : admitted_) {
    MeshMapLayer* layer = admitted.layer;
    const MeshMapUpload& mesh = *layer->stagedMesh();
    LayerGpu& gpu = layer_gpu_[layer];
    if (!ensureLayerGpu(frame.rhi, layer, gpu)) {
      continue;
    }
    if (!upload(frame.updates, frame.rhi, layer, gpu)) {
      continue;  // the upload failed and released the layer's buffers
    }
    layer->noteRenderSuccess();

    const glm::mat4 model_f =
        relativeModel(admitted.fixed_from_source.matrix() * poseTransform(mesh.origin).matrix(), frame.render_origin);
    const auto opacity = static_cast<float>(layer->opacity());
    // Blending surfaces draw after the opaque ones without writing depth: below
    // full layer opacity, or with a colour whose own alpha is below 255. The links
    // carry the layer opacity alone.
    const DrawClass layer_class = opacity < 0.999F ? DrawClass::kTranslucent : DrawClass::kOpaque;
    const DrawClass surface_class = mesh.translucent ? DrawClass::kTranslucent : layer_class;
    float mode = kModeSolid;
    if (mesh.color_mode == MeshMapColorMode::kChannel) {
      mode = kModeChannel;
    } else if (mesh.color_mode == MeshMapColorMode::kRgba) {
      mode = kModeVertexColor;
    }

    MeshMapUniforms uniforms;
    copyMatrix(frame.view_projection, uniforms.view_projection);
    copyMatrix(model_f, uniforms.model);
    fillLitSurfaceUniforms(uniforms, lit, *layer, mesh.color_range);
    uniforms.flags = {mode, 0.0F, 0.0F, 0.0F};
    frame.updates->updateDynamicBuffer(gpu.surface_uniforms, 0, kUniformBytes, &uniforms);
    draws_.push_back(
        Draw{.layer = layer, .lines = false, .vertex_count = gpu.surface_vertices, .draw_class = surface_class});
    last_vertex_count_ += static_cast<int>(gpu.surface_vertices);

    if (layer->showLinks() && gpu.link_vertices > 0) {
      uniforms.flags = {kModeLine, 0.0F, 0.0F, 0.0F};
      frame.updates->updateDynamicBuffer(gpu.line_uniforms, 0, kUniformBytes, &uniforms);
      draws_.push_back(
          Draw{.layer = layer, .lines = true, .vertex_count = gpu.link_vertices, .draw_class = layer_class});
      last_vertex_count_ += static_cast<int>(gpu.link_vertices);
    }
    ++last_layer_count_;
  }
}

bool MeshMapRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  const auto found = layer_gpu_.find(draw.layer);
  if (found == layer_gpu_.end()) {
    return false;
  }
  const LayerGpu& gpu = found->second;
  const bool translucent = draw.draw_class == DrawClass::kTranslucent;
  const ScenePipeline* registered = draw.lines ? (translucent ? translucent_line_pipeline_ : line_pipeline_)
                                               : (translucent ? translucent_pipeline_ : opaque_pipeline_);
  QRhiGraphicsPipeline* pipeline = registry.resolve(registered, pass);
  if (pipeline == nullptr) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  if (draw.lines) {
    command_buffer->setShaderResources(gpu.line_bindings);
    const std::array<QRhiCommandBuffer::VertexInput, 3> inputs{{
        {gpu.link_positions, 0},
        {gpu.link_colors, 0},  // value slot: ignored by the line mode
        {gpu.link_colors, 0},
    }};
    command_buffer->setVertexInput(0, static_cast<int>(inputs.size()), inputs.data());
  } else {
    command_buffer->setShaderResources(gpu.surface_bindings);
    // Solid mode reads neither attribute: bind the positions (12 bytes per vertex,
    // so always long enough) rather than a colour buffer left from an earlier,
    // possibly smaller mesh.
    QRhiBuffer* attributes = gpu.attributes_in_use ? gpu.attributes : gpu.positions;
    const std::array<QRhiCommandBuffer::VertexInput, 3> inputs{{
        {gpu.positions, 0},
        {attributes, 0},
        {attributes, 0},
    }};
    command_buffer->setVertexInput(0, static_cast<int>(inputs.size()), inputs.data());
  }
  command_buffer->draw(draw.vertex_count);
  return true;
}

bool MeshMapRhiPass::ensureLayerGpu(QRhi* rhi, MeshMapLayer* layer, LayerGpu& gpu) {
  if (!ensureUniformBuffer(rhi, gpu.surface_uniforms, kUniformBytes) ||
      !ensureUniformBuffer(rhi, gpu.line_uniforms, kUniformBytes)) {
    layer->noteRenderFailure(tr("Could not create the mesh-map uniform buffers"));
    releaseLayerGpu(gpu);
    return false;
  }
  if (gpu.surface_bindings == nullptr) {
    gpu.surface_bindings = createLayerBindings(rhi, gpu.surface_uniforms);
  }
  if (gpu.line_bindings == nullptr) {
    gpu.line_bindings = createLayerBindings(rhi, gpu.line_uniforms);
  }
  if (gpu.surface_bindings == nullptr || gpu.line_bindings == nullptr) {
    layer->noteRenderFailure(tr("Could not create mesh-map shader bindings"));
    releaseLayerGpu(gpu);
    return false;
  }
  return true;
}

void MeshMapRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  delete gpu.surface_bindings;
  delete gpu.line_bindings;
  delete gpu.surface_uniforms;
  delete gpu.line_uniforms;
  delete gpu.positions;
  delete gpu.attributes;
  delete gpu.link_positions;
  delete gpu.link_colors;
  gpu = {};
}

void MeshMapRhiPass::release() {
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  delete layout_shader_resources_;
  delete layout_uniform_buffer_;
  opaque_pipeline_ = nullptr;
  translucent_pipeline_ = nullptr;
  line_pipeline_ = nullptr;
  translucent_line_pipeline_ = nullptr;
  layout_shader_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  render_mode_uniforms_ = nullptr;
}

}  // namespace pj::scene3d
