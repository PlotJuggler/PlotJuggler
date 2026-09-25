// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "model_rhi_pass.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kInitialBufferBytes = 64U * 1024U;
constexpr std::size_t kBaseColorSlot = 0U;
constexpr std::size_t kMetallicRoughnessSlot = 1U;
constexpr std::size_t kNormalSlot = 2U;
constexpr std::size_t kOcclusionSlot = 3U;
constexpr std::size_t kEmissiveSlot = 4U;

static_assert(sizeof(ModelRhiPass::Instance) == 144U);
static_assert(offsetof(ModelRhiPass::Instance, tint) == 64U);
static_assert(offsetof(ModelRhiPass::Instance, params) == 80U);
static_assert(offsetof(ModelRhiPass::Instance, texture_flags) == 96U);
static_assert(offsetof(ModelRhiPass::Instance, pbr_factors) == 112U);
static_assert(offsetof(ModelRhiPass::Instance, emissive_factor) == 128U);
static_assert(sizeof(ModelRhiPass::ShadowInstance) == 64U);
// Every per-view budget fills one quint32-sized buffer.
constexpr bool fitsBuffers(const SceneLimits& limits) {
  constexpr std::uint64_t kMaxBytes = std::numeric_limits<quint32>::max();
  return limits.max_model_draws_per_view * sizeof(ModelRhiPass::Instance) <= kMaxBytes &&
         limits.max_model_draws_per_view * sizeof(ModelRhiPass::ShadowInstance) <= kMaxBytes;
}
static_assert(fitsBuffers(kBrowserSceneLimits));
static_assert(fitsBuffers(kDesktopSceneLimits));

QString tr(const char* text) {
  return QCoreApplication::translate("pj::scene3d::SceneViewWidget", text);
}

std::array<const TextureSource*, 5> textureSources(const Material& material) {
  return {
      &material.base_color, &material.metallic_roughness, &material.normal, &material.occlusion, &material.emissive};
}

constexpr bool textureIsSrgb(std::size_t slot) {
  return slot == kBaseColorSlot || slot == kEmissiveSlot;
}

bool materialUsesPbrPath(const Material& material) {
  const bool has_emissive =
      material.emissive_factor.r != 0.0F || material.emissive_factor.g != 0.0F || material.emissive_factor.b != 0.0F;
  const auto textures = textureSources(material);
  return material.has_pbr || has_emissive ||
         std::any_of(textures.cbegin(), textures.cend(), [](const TextureSource* texture) {
           return texture != nullptr && !texture->empty();
         });
}

// The layer behind a ModelRenderable (every implementer is a Scene3DLayer),
// which carries the renderer part of its row warning.
Scene3DLayer& asLayer(ModelRenderable* model) {
  return *dynamic_cast<Scene3DLayer*>(model);
}

AABB meshBounds(const MeshData& mesh) {
  AABB output;
  for (const auto& vertex : mesh.vertices) {
    if (std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z)) {
      expandAABB(output, vertex.position);
    }
  }
  return output;
}

}  // namespace

bool ModelRhiPass::initialize(const RhiPassSetup& setup, QRhiBuffer* lighting_uniforms) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.view_uniforms == nullptr || lighting_uniforms == nullptr) {
    return false;
  }
  view_uniforms_ = setup.view_uniforms;
  lighting_uniforms_ = lighting_uniforms;
  render_mode_uniforms_ = setup.render_mode_uniforms;
  white_texture_ = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
  sampler_ = rhi->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::Repeat, QRhiSampler::Repeat);
  if (!white_texture_->create() || !sampler_->create()) {
    return false;
  }
  // Every texture slot bound to the white texture: the layout of each material SRB.
  layout_bindings_ = createMaterialBindings(rhi, MaterialGpu{});
  if (layout_bindings_ == nullptr) {
    return false;
  }
  white_upload_pending_ = true;

  QRhiVertexInputLayout layout;
  layout.setBindings({
      QRhiVertexInputBinding(sizeof(pj::scene3d::Vertex)),
      QRhiVertexInputBinding(sizeof(Instance), QRhiVertexInputBinding::PerInstance),
  });
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, position)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, normal)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, offsetof(pj::scene3d::Vertex, color)),
      QRhiVertexInputAttribute(0, 3, QRhiVertexInputAttribute::Float2, offsetof(pj::scene3d::Vertex, uv)),
      QRhiVertexInputAttribute(0, 4, QRhiVertexInputAttribute::Float4, offsetof(pj::scene3d::Vertex, tangent)),
      // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 0U, 0),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 16U, 1),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 32U, 2),
      QRhiVertexInputAttribute(1, 8, QRhiVertexInputAttribute::Float4, 48U, 3),
      QRhiVertexInputAttribute(1, 9, QRhiVertexInputAttribute::Float4, 64U),
      QRhiVertexInputAttribute(1, 10, QRhiVertexInputAttribute::Float4, 80U),
      QRhiVertexInputAttribute(1, 11, QRhiVertexInputAttribute::Float4, 96U),
      QRhiVertexInputAttribute(1, 12, QRhiVertexInputAttribute::Float4, 112U),
      QRhiVertexInputAttribute(1, 13, QRhiVertexInputAttribute::Float4, 128U),
  });
  PipelineDesc desc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/model.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/model.frag.qsb")),
      .input_layout = layout,
      .bindings = layout_bindings_,
  };
  triangle_pipeline_ = createScenePipeline(setup, desc);
  desc.topology = QRhiGraphicsPipeline::Lines;
  line_pipeline_ = createScenePipeline(setup, desc);
  desc.depth_write = false;
  line_no_depth_pipeline_ = createScenePipeline(setup, desc);
  desc.topology = QRhiGraphicsPipeline::Triangles;
  triangle_no_depth_pipeline_ = createScenePipeline(setup, desc);
  return ready();
}

bool ModelRhiPass::ready() const {
  return triangle_pipeline_ != nullptr && triangle_no_depth_pipeline_ != nullptr && line_pipeline_ != nullptr &&
         line_no_depth_pipeline_ != nullptr && layout_bindings_ != nullptr;
}

void ModelRhiPass::registerPipelines(ScenePipelineRegistry& registry) const {
  for (QRhiGraphicsPipeline* pipeline :
       {triangle_pipeline_, triangle_no_depth_pipeline_, line_pipeline_, line_no_depth_pipeline_}) {
    registry.add(pipeline);
  }
}

void ModelRhiPass::setLayers(const std::vector<ModelRenderable*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, [this](LayerGpu& gpu) { releaseLayerGpu(gpu); });
}

void ModelRhiPass::admit(
    const PlaceFn& place, const MeshShadingParams& shading, bool shadows_requested, SceneBoundsCollector& bounds) {
  admitted_.clear();
  admitted_.reserve(layers_.size());
  std::erase_if(bounds_cache_, [](const auto& entry) { return entry.first.use_count() == 1; });
  AABB family_bounds;
  std::uint64_t remaining_draws = kSceneLimits.max_model_draws_per_view;
  std::uint64_t remaining_triangles = kSceneLimits.max_model_triangles_per_view;
  std::uint64_t remaining_shadow_draws = kSceneLimits.max_model_draws_per_view;
  std::uint64_t remaining_shadow_triangles = kSceneLimits.max_model_triangles_per_view;
  for (ModelRenderable* layer : layers_) {
    if (layer == nullptr || !layer->visible() || layer->modelDrawCalls().empty()) {
      continue;
    }
    Admitted prepared;
    prepared.layer = layer;
    prepared.color_calls.reserve(layer->modelDrawCalls().size());
    prepared.shadow_calls.reserve(layer->modelDrawCalls().size());
    AABB layer_bounds;
    for (const ModelDrawCall& draw : layer->modelDrawCalls()) {
      const bool color_requested =
          (draw.group != ModelDrawGroup::kVisual || (shading.meshes_visible && shading.mesh_opacity > 0.0F)) &&
          (draw.group != ModelDrawGroup::kCollision ||
           (shading.collisions_visible && shading.collision_opacity > 0.0F));
      // Native casts SceneEntities model primitives and RobotModel visuals. A
      // hidden Robot visual still casts there; collisions never do.
      const bool shadow_requested = shadows_requested && draw.group != ModelDrawGroup::kCollision;
      if (!color_requested && !shadow_requested) {
        continue;
      }
      const auto mesh = layer->modelMeshes().find(draw.mesh_key);
      if (mesh == layer->modelMeshes().end() || mesh->second == nullptr || !mesh->second->ok || draw.frame_id.empty()) {
        continue;
      }
      const auto placed = place(draw.frame_id);
      if (!placed.has_value()) {
        continue;
      }
      const std::uint64_t submesh_draws = mesh->second->submeshes.size();
      const std::uint64_t triangles = mesh->second->indices.size() / 3U;
      if (color_requested) {
        if (submesh_draws > std::numeric_limits<std::uint64_t>::max() - prepared.draws ||
            triangles > std::numeric_limits<std::uint64_t>::max() - prepared.triangles) {
          prepared.draws = std::numeric_limits<std::uint64_t>::max();
          prepared.triangles = std::numeric_limits<std::uint64_t>::max();
          break;
        }
        prepared.draws += submesh_draws;
        prepared.triangles += triangles;
      }
      if (shadow_requested) {
        if (prepared.shadow_draws == std::numeric_limits<std::uint64_t>::max() ||
            triangles > std::numeric_limits<std::uint64_t>::max() - prepared.shadow_triangles) {
          prepared.shadow_draws = std::numeric_limits<std::uint64_t>::max();
          prepared.shadow_triangles = std::numeric_limits<std::uint64_t>::max();
          break;
        }
        ++prepared.shadow_draws;
        prepared.shadow_triangles += triangles;
      }
      const glm::dmat4 fixed_from_model = placed->matrix() * draw.model;
      const auto [cached, inserted] = bounds_cache_.try_emplace(mesh->second);
      if (inserted) {
        cached->second = meshBounds(*mesh->second);
      }
      const Call call{draw.mesh_key,       mesh->second,      fixed_from_model,
                      draw.override_color, draw.use_material, draw.group};
      if (color_requested) {
        layer_bounds = unionAABB(layer_bounds, transformedBounds(cached->second, fixed_from_model));
        prepared.color_calls.push_back(call);
      }
      if (shadow_requested) {
        prepared.shadow_calls.push_back(call);
      }
    }
    if (prepared.color_calls.empty() && prepared.shadow_calls.empty()) {
      continue;
    }
    if (prepared.draws > kSceneLimits.max_model_draws_per_layer ||
        prepared.triangles > kSceneLimits.max_model_triangles_per_layer) {
      asLayer(layer).noteRenderFailure(tr("Models need %1 draws and %2 triangles; the layer limits are %3 and %4")
                                           .arg(
                                               QString::number(prepared.draws), QString::number(prepared.triangles),
                                               QString::number(kSceneLimits.max_model_draws_per_layer),
                                               QString::number(kSceneLimits.max_model_triangles_per_layer)));
      continue;
    }
    if (!prepared.color_calls.empty() &&
        !tryConsumeModels(prepared.draws, prepared.triangles, remaining_draws, remaining_triangles)) {
      asLayer(layer).noteRenderFailure(
          tr("Models need %1 draws and %2 triangles; the remaining view budget is %3 and %4")
              .arg(
                  QString::number(prepared.draws), QString::number(prepared.triangles),
                  QString::number(remaining_draws), QString::number(remaining_triangles)));
      continue;
    }
    QString shadow_warning;
    if (prepared.shadow_draws > kSceneLimits.max_model_draws_per_layer ||
        prepared.shadow_triangles > kSceneLimits.max_model_triangles_per_layer ||
        !tryConsumeModels(
            prepared.shadow_draws, prepared.shadow_triangles, remaining_shadow_draws, remaining_shadow_triangles)) {
      shadow_warning = tr("Model shadows exceed this view's draw/triangle budget");
      prepared.shadow_calls.clear();
      prepared.shadow_draws = 0;
      prepared.shadow_triangles = 0;
    }
    if (layer->contributesToSceneBounds()) {
      family_bounds = unionAABB(family_bounds, layer_bounds);
      bounds.add(dynamic_cast<const void*>(layer), layer_bounds);
    }
    if (shadow_warning.isEmpty()) {
      asLayer(layer).noteRenderSuccess();
    } else {
      asLayer(layer).noteRenderFailure(shadow_warning);
    }
    admitted_.push_back(std::move(prepared));
  }
  last_bounds_ = family_bounds;
}

void ModelRhiPass::prepare(const RhiFrameContext& frame, const MeshShadingParams& shading) {
  QRhi* rhi = frame.rhi;
  const glm::dvec3& render_origin = frame.render_origin;
  draws_.clear();
  shadow_draws_.clear();
  instances_.clear();
  shadow_instances_.clear();
  shadow_bounds_ = {};
  last_layer_count_ = 0;
  last_draw_count_ = 0;
  last_triangle_count_ = 0;
  last_shadow_triangle_count_ = 0;
  last_texture_slot_counts_.fill(0);
  for (const Admitted& prepared : admitted_) {
    ModelRenderable* layer = prepared.layer;
    LayerGpu& gpu = layer_gpu_[layer];
    if (!ensureLayerGpu(rhi, layer, gpu)) {
      continue;
    }
    const std::size_t first_draw = draws_.size();
    for (const Call& call : prepared.color_calls) {
      const auto gpu_mesh = gpu.meshes.find(call.mesh_key);
      if (gpu_mesh == gpu.meshes.end() || gpu_mesh->second.source == nullptr) {
        continue;
      }
      MeshGpu& mesh = gpu_mesh->second;
      const glm::mat4 narrowed = relativeModel(call.fixed_from_model, render_origin);
      for (std::size_t index = 0; index < call.mesh->submeshes.size() && index < mesh.materials.size(); ++index) {
        const SubMesh& submesh = call.mesh->submeshes[index];
        if (submesh.index_count == 0U) {
          continue;
        }
        const Material fallback;
        const Material& material = submesh.material != nullptr ? *submesh.material : fallback;
        MaterialGpu& material_gpu = mesh.materials[index];
        const bool pbr_active = materialUsesPbrPath(material);
        bool use_material = call.use_material;
        glm::vec4 tint = use_material ? material.base_color_factor : call.override_color;
        if (layer->colorOverrideEnabled()) {
          const QColor override = layer->overrideColor();
          tint.r = static_cast<float>(override.redF());
          tint.g = static_cast<float>(override.greenF());
          tint.b = static_cast<float>(override.blueF());
          use_material = false;
        }
        float opacity = layer->opacity();
        if (call.group == ModelDrawGroup::kVisual) {
          opacity *= shading.mesh_opacity;
        } else if (call.group == ModelDrawGroup::kCollision) {
          opacity *= shading.collision_opacity;
        }
        tint.a *= opacity;
        const bool use_texture = material_gpu.textures[kBaseColorSlot].present && (pbr_active || use_material);
        float alpha_mode = 0.0F;
        if ((pbr_active || use_material) && material.alpha_mode == AlphaMode::kMask) {
          alpha_mode = 1.0F;
        } else if ((pbr_active || use_material) && material.alpha_mode == AlphaMode::kBlend) {
          alpha_mode = 2.0F;
        }
        Instance instance;
        copyMatrix(narrowed, instance.model);
        instance.tint = {tint.r, tint.g, tint.b, tint.a};
        instance.params = {use_texture ? 1.0F : 0.0F, alpha_mode, material.alpha_cutoff, use_material ? 1.0F : 0.0F};
        instance.texture_flags = {
            pbr_active && material_gpu.textures[kMetallicRoughnessSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kNormalSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kOcclusionSlot].present ? 1.0F : 0.0F,
            pbr_active && material_gpu.textures[kEmissiveSlot].present ? 1.0F : 0.0F};
        instance.pbr_factors = {
            material.has_pbr ? material.metallic_factor : 0.0F,
            material.has_pbr ? material.roughness_factor : shading.roughness, pbr_active ? 1.0F : 0.0F,
            call.group == ModelDrawGroup::kCollision ? 1.0F : 0.0F};
        instance.emissive_factor = {
            pbr_active ? material.emissive_factor.r : 0.0F, pbr_active ? material.emissive_factor.g : 0.0F,
            pbr_active ? material.emissive_factor.b : 0.0F, 0.0F};
        for (std::size_t slot = 0; slot < material_gpu.textures.size(); ++slot) {
          last_texture_slot_counts_[slot] += material_gpu.textures[slot].present ? 1 : 0;
        }
        const quint32 instance_offset = static_cast<quint32>(instances_.size());
        instances_.push_back(instance);
        const bool lines = layer->wireframe();
        const quint32 index_offset = lines ? mesh.edge_offsets[index] : static_cast<quint32>(submesh.index_offset);
        const quint32 index_count = lines ? mesh.edge_counts[index] : static_cast<quint32>(submesh.index_count);
        const bool translucent = call.group == ModelDrawGroup::kCollision || tint.a < 0.999F || alpha_mode > 1.5F;
        draws_.push_back(
            Draw{
                layer, &mesh, &material_gpu, instance_offset, index_offset, index_count, lines,
                translucent ? DrawClass::kTranslucent : DrawClass::kOpaque});
      }
    }
    if (draws_.size() != first_draw) {
      ++last_layer_count_;
      last_draw_count_ += static_cast<int>(draws_.size() - first_draw);
      last_triangle_count_ += static_cast<int>(prepared.triangles);
    }
    for (const Call& call : prepared.shadow_calls) {
      const auto gpu_mesh = gpu.meshes.find(call.mesh_key);
      if (gpu_mesh == gpu.meshes.end() || gpu_mesh->second.source == nullptr ||
          gpu_mesh->second.triangle_index_buffer == nullptr || call.mesh->indices.empty()) {
        continue;
      }
      const glm::mat4 narrowed = relativeModel(call.fixed_from_model, render_origin);
      ShadowInstance instance;
      copyMatrix(narrowed, instance.model);
      const quint32 instance_offset = static_cast<quint32>(shadow_instances_.size());
      shadow_instances_.push_back(instance);
      const auto cached = bounds_cache_.find(call.mesh);
      if (cached != bounds_cache_.end()) {
        shadow_bounds_ =
            unionAABB(shadow_bounds_, transformedBounds(cached->second, call.fixed_from_model, render_origin));
      }
      const quint32 index_count = static_cast<quint32>(call.mesh->indices.size());
      shadow_draws_.push_back(ShadowDraw{&gpu_mesh->second, instance_offset, index_count});
      last_shadow_triangle_count_ += static_cast<int>(index_count / 3U);
    }
  }
  QRhiResourceUpdateBatch* updates = frame.updates;
  const quint32 instance_bytes = static_cast<quint32>(instances_.size() * sizeof(Instance));
  if (!ensureVertexBuffer(rhi, instance_buffer_, instance_capacity_, instance_bytes, kInitialBufferBytes)) {
    draws_.clear();
  } else if (instance_bytes != 0U) {
    updates->updateDynamicBuffer(instance_buffer_, 0, instance_bytes, instances_.data());
  }
  if (white_upload_pending_ && white_texture_ != nullptr) {
    static constexpr std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
    QRhiTextureSubresourceUploadDescription description(white.data(), static_cast<quint32>(white.size()));
    description.setSourceSize(QSize(1, 1));
    updates->uploadTexture(white_texture_, QRhiTextureUploadDescription({0, 0, description}));
    white_upload_pending_ = false;
  }
  for (MeshGpu* mesh : pending_uploads_) {
    if (mesh->upload_pending && mesh->source != nullptr) {
      updates->uploadStaticBuffer(mesh->vertex_buffer, mesh->source->vertices.data());
      updates->uploadStaticBuffer(mesh->triangle_index_buffer, mesh->source->indices.data());
      if (mesh->edge_index_buffer != nullptr && !mesh->edge_indices.empty()) {
        updates->uploadStaticBuffer(mesh->edge_index_buffer, mesh->edge_indices.data());
      }
      mesh->upload_pending = false;
    }
    for (MaterialGpu& material : mesh->materials) {
      for (TextureGpu& texture : material.textures) {
        if (texture.texture_upload_pending && texture.texture != nullptr && !texture.pending_image.isNull()) {
          const QRhiTextureSubresourceUploadDescription description(texture.pending_image);
          updates->uploadTexture(texture.texture, QRhiTextureUploadDescription({0, 0, description}));
          texture.pending_image = {};
          texture.texture_upload_pending = false;
        }
      }
    }
  }
  pending_uploads_.clear();
}

bool ModelRhiPass::uploadShadowCasters(const RhiFrameContext& frame) {
  const quint32 shadow_bytes = static_cast<quint32>(shadow_instances_.size() * sizeof(ShadowInstance));
  if (!ensureVertexBuffer(
          frame.rhi, shadow_instance_buffer_, shadow_instance_capacity_, shadow_bytes, kInitialBufferBytes)) {
    return false;
  }
  if (shadow_bytes != 0U) {
    frame.updates->updateDynamicBuffer(shadow_instance_buffer_, 0, shadow_bytes, shadow_instances_.data());
  }
  return true;
}

bool ModelRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  if (draw.mesh == nullptr || draw.material == nullptr) {
    return false;
  }
  const bool depth_write = draw.draw_class == DrawClass::kOpaque;
  QRhiGraphicsPipeline* direct = draw.lines ? (depth_write ? line_pipeline_ : line_no_depth_pipeline_)
                                            : (depth_write ? triangle_pipeline_ : triangle_no_depth_pipeline_);
  QRhiGraphicsPipeline* pipeline = registry.resolve(direct, pass);
  QRhiBuffer* index_buffer = draw.lines ? draw.mesh->edge_index_buffer : draw.mesh->triangle_index_buffer;
  if (pipeline == nullptr || draw.mesh->vertex_buffer == nullptr || index_buffer == nullptr ||
      draw.material->shader_resources == nullptr || instance_buffer_ == nullptr || draw.index_count == 0U) {
    return false;
  }
  command_buffer->setGraphicsPipeline(pipeline);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(draw.material->shader_resources);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(draw.mesh->vertex_buffer, 0),
      QRhiCommandBuffer::VertexInput(instance_buffer_, draw.instance_offset * static_cast<quint32>(sizeof(Instance))),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), index_buffer,
      draw.index_offset * static_cast<quint32>(sizeof(quint32)), QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(draw.index_count, 1U);
  return true;
}

bool ModelRhiPass::ensureShadowPipeline(
    QRhi* rhi, QRhiRenderPassDescriptor* render_pass, QRhiShaderResourceBindings* bindings) {
  if (shadow_pipeline_ != nullptr) {
    return true;
  }
  if (render_pass == nullptr) {
    return false;
  }
  QRhiVertexInputLayout layout;
  layout.setBindings({
      QRhiVertexInputBinding(sizeof(pj::scene3d::Vertex)),
      QRhiVertexInputBinding(sizeof(ShadowInstance), QRhiVertexInputBinding::PerInstance),
  });
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, position)),
      // mat4 rows carry their slice: D3D matches them as TEXCOORD<first>_<slice>.
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 0U, 0),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 16U, 1),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 32U, 2),
      QRhiVertexInputAttribute(1, 8, QRhiVertexInputAttribute::Float4, 48U, 3),
  });
  shadow_pipeline_ = createScenePipeline(
      {rhi, render_pass, 1}, {
                                 .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/shadow.vert.qsb")),
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

void ModelRhiPass::releaseShadowResources() {
  delete shadow_pipeline_;
  delete shadow_instance_buffer_;
  shadow_pipeline_ = nullptr;
  shadow_bindings_ = nullptr;
  shadow_instance_buffer_ = nullptr;
  shadow_instance_capacity_ = 0;
}

void ModelRhiPass::submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const {
  if (shadow_pipeline_ == nullptr || shadow_instance_buffer_ == nullptr) {
    return;
  }
  command_buffer->setGraphicsPipeline(shadow_pipeline_);
  command_buffer->setViewport(viewport);
  command_buffer->setShaderResources(shadow_bindings_);
  for (const ShadowDraw& draw : shadow_draws_) {
    if (draw.mesh == nullptr || draw.mesh->vertex_buffer == nullptr || draw.mesh->triangle_index_buffer == nullptr ||
        draw.index_count == 0U) {
      continue;
    }
    const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
        QRhiCommandBuffer::VertexInput(draw.mesh->vertex_buffer, 0),
        QRhiCommandBuffer::VertexInput(
            shadow_instance_buffer_, draw.instance_offset * static_cast<quint32>(sizeof(ShadowInstance))),
    };
    command_buffer->setVertexInput(
        0, static_cast<int>(inputs.size()), inputs.data(), draw.mesh->triangle_index_buffer, 0,
        QRhiCommandBuffer::IndexUInt32);
    command_buffer->drawIndexed(draw.index_count, 1U);
  }
}

bool ModelRhiPass::setShadowMap(QRhi* rhi, QRhiTexture* texture, QRhiSampler* sampler) {
  shadow_texture_ = texture;
  shadow_sampler_ = sampler;
  return rebuildAllMaterialBindings(rhi);
}

void ModelRhiPass::clearShadowMap(QRhi* rhi, bool restore_bindings) {
  shadow_texture_ = nullptr;
  shadow_sampler_ = nullptr;
  if (!restore_bindings) {
    dropMaterialBindings(false);
    return;
  }
  if (rhi == nullptr || white_texture_ == nullptr) {
    return;
  }
  if (!rebuildAllMaterialBindings(rhi)) {
    // Keep this frame's mesh/range pointers valid, but remove every binding
    // that still references the soon-to-be-destroyed depth texture. The
    // revision mismatch rebuilds the complete layer on the next frame.
    dropMaterialBindings(true);
  }
}

void ModelRhiPass::noteShadowFailure(const QString& error) {
  for (const Admitted& prepared : admitted_) {
    if (prepared.layer != nullptr && !prepared.shadow_calls.empty()) {
      asLayer(prepared.layer).noteRenderFailure(error);
    }
  }
}

void ModelRhiPass::submitMeshMask(
    QRhiCommandBuffer* command_buffer, QRhiGraphicsPipeline* triangle_pipeline, QRhiGraphicsPipeline* line_pipeline,
    const QRhiViewport& viewport) const {
  for (const Draw& draw : draws_) {
    if (draw.mesh == nullptr || draw.material == nullptr || draw.index_count == 0U) {
      continue;
    }
    QRhiGraphicsPipeline* pipeline = draw.lines ? line_pipeline : triangle_pipeline;
    QRhiBuffer* index_buffer = draw.lines ? draw.mesh->edge_index_buffer : draw.mesh->triangle_index_buffer;
    if (pipeline == nullptr || draw.mesh->vertex_buffer == nullptr || index_buffer == nullptr ||
        draw.material->shader_resources == nullptr || instance_buffer_ == nullptr) {
      continue;
    }
    const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
        QRhiCommandBuffer::VertexInput(draw.mesh->vertex_buffer, 0),
        QRhiCommandBuffer::VertexInput(instance_buffer_, draw.instance_offset * static_cast<quint32>(sizeof(Instance))),
    };
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(draw.material->shader_resources);
    command_buffer->setVertexInput(
        0, static_cast<int>(inputs.size()), inputs.data(), index_buffer,
        draw.index_offset * static_cast<quint32>(sizeof(quint32)), QRhiCommandBuffer::IndexUInt32);
    command_buffer->drawIndexed(draw.index_count, 1U);
  }
}

QRhiShaderResourceBindings* ModelRhiPass::createMaterialBindings(QRhi* rhi, const MaterialGpu& material) const {
  if (rhi == nullptr || view_uniforms_ == nullptr || lighting_uniforms_ == nullptr || white_texture_ == nullptr ||
      sampler_ == nullptr) {
    return nullptr;
  }
  QRhiTexture* shadow_texture = shadow_texture_ != nullptr ? shadow_texture_ : white_texture_;
  QRhiSampler* shadow_sampler = shadow_sampler_ != nullptr ? shadow_sampler_ : sampler_;
  const auto texture = [&](std::size_t slot) {
    return material.textures[slot].texture != nullptr ? material.textures[slot].texture : white_texture_;
  };
  return createBindings(
      rhi,
      {
          QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, view_uniforms_),
          QRhiShaderResourceBinding::sampledTexture(
              1, QRhiShaderResourceBinding::FragmentStage, texture(kBaseColorSlot), sampler_),
          QRhiShaderResourceBinding::sampledTexture(
              2, QRhiShaderResourceBinding::FragmentStage, texture(kMetallicRoughnessSlot), sampler_),
          QRhiShaderResourceBinding::sampledTexture(
              3, QRhiShaderResourceBinding::FragmentStage, texture(kNormalSlot), sampler_),
          QRhiShaderResourceBinding::sampledTexture(
              4, QRhiShaderResourceBinding::FragmentStage, texture(kOcclusionSlot), sampler_),
          QRhiShaderResourceBinding::sampledTexture(
              5, QRhiShaderResourceBinding::FragmentStage, texture(kEmissiveSlot), sampler_),
          QRhiShaderResourceBinding::uniformBuffer(6, QRhiShaderResourceBinding::FragmentStage, lighting_uniforms_),
          QRhiShaderResourceBinding::sampledTexture(
              7, QRhiShaderResourceBinding::FragmentStage, shadow_texture, shadow_sampler),
          QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniforms_),
      });
}

bool ModelRhiPass::rebuildMaterialBindings(QRhi* rhi, MaterialGpu& material) const {
  QRhiShaderResourceBindings* replacement = createMaterialBindings(rhi, material);
  if (replacement == nullptr) {
    return false;
  }
  delete material.shader_resources;
  material.shader_resources = replacement;
  return true;
}

bool ModelRhiPass::rebuildAllMaterialBindings(QRhi* rhi) {
  std::vector<std::pair<MaterialGpu*, QRhiShaderResourceBindings*>> replacements;
  for (auto& [_, layer] : layer_gpu_) {
    for (auto& [__, mesh] : layer.meshes) {
      for (MaterialGpu& material : mesh.materials) {
        QRhiShaderResourceBindings* replacement = createMaterialBindings(rhi, material);
        if (replacement == nullptr) {
          for (const auto& [___, pending] : replacements) {
            delete pending;
          }
          return false;
        }
        replacements.emplace_back(&material, replacement);
      }
    }
  }
  // Commit only after every replacement exists. A failed shadow enable/disable
  // must not leave half the materials bound to the old depth texture.
  for (const auto& [material, replacement] : replacements) {
    delete material->shader_resources;
    material->shader_resources = replacement;
  }
  return true;
}

void ModelRhiPass::dropMaterialBindings(bool invalidate_layers) {
  for (auto& [_, layer] : layer_gpu_) {
    for (auto& [__, mesh] : layer.meshes) {
      for (MaterialGpu& material : mesh.materials) {
        delete material.shader_resources;
        material.shader_resources = nullptr;
      }
    }
    if (invalidate_layers) {
      layer.uploaded_revision = ~std::uint64_t{0};
    }
  }
}

bool ModelRhiPass::ensureLayerGpu(QRhi* rhi, ModelRenderable* layer, LayerGpu& gpu) {
  if (rhi == nullptr || layer == nullptr || white_texture_ == nullptr || sampler_ == nullptr ||
      view_uniforms_ == nullptr || lighting_uniforms_ == nullptr) {
    return false;
  }
  if (gpu.uploaded_revision == layer->modelRevision()) {
    return true;
  }
  releaseLayerGpu(gpu);
  const int max_texture_size = rhi->resourceLimit(QRhi::TextureSizeMax);
  // Decoded QImages and their QRhi textures are retained across the whole
  // layer, not just one mesh. Enforce both envelopes before QImage allocation.
  std::uint64_t remaining_layer_texture_pixels = kSceneLimits.max_model_texture_pixels_per_layer;
  const auto fail = [&](MeshGpu& mesh, const char* reason) {
    asLayer(layer).noteRenderFailure(tr(reason));
    releaseMeshGpu(mesh);
    releaseLayerGpu(gpu);
    return false;
  };
  for (const auto& [key, source] : layer->modelMeshes()) {
    if (source == nullptr || !source->ok || source->vertices.empty() || source->indices.empty()) {
      continue;
    }
    MeshGpu mesh;
    mesh.source = source;
    const std::uint64_t vertex_bytes = source->vertices.size() * sizeof(pj::scene3d::Vertex);
    const std::uint64_t index_bytes = source->indices.size() * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<quint32>::max() || index_bytes > std::numeric_limits<quint32>::max()) {
      return fail(mesh, "A model GPU buffer exceeds the 4 GiB buffer limit");
    }
    mesh.vertex_buffer =
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(vertex_bytes));
    mesh.triangle_index_buffer =
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(index_bytes));

    mesh.edge_indices.reserve(source->indices.size() * 2U);
    mesh.edge_offsets.reserve(source->submeshes.size());
    mesh.edge_counts.reserve(source->submeshes.size());
    for (const SubMesh& submesh : source->submeshes) {
      const quint32 first = static_cast<quint32>(mesh.edge_indices.size());
      const std::size_t end = std::min(source->indices.size(), submesh.index_offset + submesh.index_count);
      for (std::size_t index = submesh.index_offset; index + 2U < end; index += 3U) {
        const quint32 a = source->indices[index];
        const quint32 b = source->indices[index + 1U];
        const quint32 c = source->indices[index + 2U];
        mesh.edge_indices.insert(mesh.edge_indices.end(), {a, b, b, c, c, a});
      }
      mesh.edge_offsets.push_back(first);
      mesh.edge_counts.push_back(static_cast<quint32>(mesh.edge_indices.size()) - first);
    }
    if (!mesh.edge_indices.empty()) {
      const std::uint64_t edge_bytes = mesh.edge_indices.size() * sizeof(quint32);
      if (edge_bytes > std::numeric_limits<quint32>::max()) {
        return fail(mesh, "A model wireframe buffer exceeds the 4 GiB buffer limit");
      }
      mesh.edge_index_buffer =
          rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(edge_bytes));
    }
    if (!mesh.vertex_buffer->create() || !mesh.triangle_index_buffer->create() ||
        (mesh.edge_index_buffer != nullptr && !mesh.edge_index_buffer->create())) {
      return fail(mesh, "Could not allocate model GPU buffers");
    }

    // Decode every distinct material slot against temporary mesh/layer budgets
    // before allocating any texture. A late fifth-slot rejection therefore
    // cannot leave a partially committed material or consume the layer budget.
    struct DecodedTexture {
      QImage image;
      std::size_t slot = 0U;
    };
    std::unordered_map<const TextureSource*, DecodedTexture> decoded_textures;
    std::uint64_t remaining_texture_pixels = kSceneLimits.max_model_texture_pixels_per_mesh;
    std::uint64_t candidate_layer_texture_pixels = remaining_layer_texture_pixels;
    for (const SubMesh& submesh : source->submeshes) {
      if (submesh.material == nullptr) {
        continue;
      }
      const auto textures = textureSources(*submesh.material);
      for (std::size_t slot = 0; slot < textures.size(); ++slot) {
        const TextureSource* texture = textures[slot];
        if (texture == nullptr || texture->empty() || decoded_textures.contains(texture)) {
          continue;
        }
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QImageReader reader;
        if (!texture->bytes.empty()) {
          encoded = QByteArray(
              reinterpret_cast<const char*>(texture->bytes.data()), static_cast<qsizetype>(texture->bytes.size()));
          buffer.open(QIODevice::ReadOnly);
          reader.setDevice(&buffer);
        } else {
          reader.setFileName(texture->path);
        }
        const QSize size = reader.size();
        QImage image;
        if (size.isValid()) {
          const std::uint64_t pixels =
              static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
          if (size.width() > max_texture_size || size.height() > max_texture_size ||
              !tryConsumeModelTexturePixels(pixels, remaining_texture_pixels, candidate_layer_texture_pixels)) {
            return fail(mesh, "A model exceeds the decoded-texture limit");
          }
          image = reader.read();
        }
        if (!size.isValid() || image.isNull()) {
          return fail(mesh, "Could not decode a model material texture");
        }
        decoded_textures.emplace(texture, DecodedTexture{image.convertToFormat(QImage::Format_RGBA8888), slot});
      }
    }
    remaining_layer_texture_pixels = candidate_layer_texture_pixels;

    mesh.materials.resize(source->submeshes.size());
    std::unordered_map<const TextureSource*, QRhiTexture*> shared_textures;
    for (std::size_t index = 0; index < source->submeshes.size(); ++index) {
      const std::shared_ptr<const Material>& material = source->submeshes[index].material;
      MaterialGpu& material_gpu = mesh.materials[index];
      const Material fallback;
      const Material& resolved_material = material != nullptr ? *material : fallback;
      const auto textures = textureSources(resolved_material);
      for (std::size_t slot = 0; slot < textures.size(); ++slot) {
        const TextureSource* texture = textures[slot];
        TextureGpu& texture_gpu = material_gpu.textures[slot];
        texture_gpu.texture = white_texture_;
        if (texture == nullptr || texture->empty()) {
          continue;
        }
        texture_gpu.present = true;
        if (const auto shared = shared_textures.find(texture); shared != shared_textures.end()) {
          texture_gpu.texture = shared->second;
          continue;
        }
        auto decoded = decoded_textures.find(texture);
        if (decoded == decoded_textures.end() || decoded->second.image.isNull() || decoded->second.slot != slot) {
          return fail(mesh, "A model material texture lost its decoded upload state");
        }
        const QRhiTexture::Flags flags = textureIsSrgb(slot) ? QRhiTexture::sRGB : QRhiTexture::Flags{};
        texture_gpu.texture = rhi->newTexture(QRhiTexture::RGBA8, decoded->second.image.size(), 1, flags);
        texture_gpu.owns_texture = true;
        if (texture_gpu.texture == nullptr || !texture_gpu.texture->create()) {
          return fail(mesh, "Could not allocate a model material texture");
        }
        texture_gpu.pending_image = std::move(decoded->second.image);
        texture_gpu.texture_upload_pending = true;
        shared_textures.emplace(texture, texture_gpu.texture);
      }
      if (!rebuildMaterialBindings(rhi, material_gpu)) {
        return fail(mesh, "Could not create model material bindings");
      }
    }
    mesh.upload_pending = true;
    pending_uploads_.push_back(&gpu.meshes.emplace(key, std::move(mesh)).first->second);
  }
  gpu.uploaded_revision = layer->modelRevision();
  return true;
}

void ModelRhiPass::releaseMeshGpu(MeshGpu& gpu) {
  std::erase(pending_uploads_, &gpu);
  // Bindings may reference textures owned by another submesh entry representing
  // the same shared material. Destroy every binding before any owned texture.
  for (MaterialGpu& material : gpu.materials) {
    delete material.shader_resources;
    material.shader_resources = nullptr;
  }
  for (MaterialGpu& material : gpu.materials) {
    for (TextureGpu& texture : material.textures) {
      if (texture.owns_texture) {
        delete texture.texture;
      }
    }
  }
  delete gpu.edge_index_buffer;
  delete gpu.triangle_index_buffer;
  delete gpu.vertex_buffer;
  gpu = {};
}

void ModelRhiPass::releaseLayerGpu(LayerGpu& gpu) {
  for (auto& [_, mesh] : gpu.meshes) {
    releaseMeshGpu(mesh);
  }
  gpu = {};
}

void ModelRhiPass::release() {
  releaseShadowResources();
  for (auto& [layer, gpu] : layer_gpu_) {
    (void)layer;
    releaseLayerGpu(gpu);
  }
  layer_gpu_.clear();
  admitted_.clear();
  draws_.clear();
  shadow_draws_.clear();
  shadow_instances_.clear();
  instances_.clear();
  bounds_cache_.clear();
  pending_uploads_.clear();
  for (QRhiGraphicsPipeline** pipeline :
       {&line_no_depth_pipeline_, &line_pipeline_, &triangle_no_depth_pipeline_, &triangle_pipeline_}) {
    delete *pipeline;
    *pipeline = nullptr;
  }
  delete layout_bindings_;
  delete sampler_;
  delete white_texture_;
  delete instance_buffer_;
  layout_bindings_ = nullptr;
  sampler_ = nullptr;
  white_texture_ = nullptr;
  instance_buffer_ = nullptr;
  instance_capacity_ = 0;
  white_upload_pending_ = false;
  shadow_texture_ = nullptr;
  shadow_sampler_ = nullptr;
  view_uniforms_ = nullptr;
  lighting_uniforms_ = nullptr;
  render_mode_uniforms_ = nullptr;
  last_shadow_triangle_count_ = 0;
}

}  // namespace pj::scene3d
