// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "model_rhi_pass.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImageIOHandler>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
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

bool materialUsesPbrPath(const Material& material) {
  const bool has_emissive =
      material.emissive_factor.r != 0.0F || material.emissive_factor.g != 0.0F || material.emissive_factor.b != 0.0F;
  const auto textures = material.textureSlots();
  return material.has_pbr || has_emissive ||
         std::any_of(textures.cbegin(), textures.cend(), [](const TextureSource* texture) {
           return texture != nullptr && !texture->empty();
         });
}

}  // namespace

bool ModelRhiPass::textureIsSrgb(std::size_t slot) {
  return slot == kBaseColorSlot || slot == kEmissiveSlot;
}

// Unkeyed sources share by address: an entry exists only while a mesh holding
// the source's MeshData references it, so the address cannot be reused meanwhile.
ModelRhiPass::TextureKey ModelRhiPass::textureKey(const TextureSource& texture, std::size_t slot) {
  std::string key =
      !texture.key.empty() ? texture.key : "@" + std::to_string(reinterpret_cast<std::uintptr_t>(&texture));
  return {std::move(key), textureIsSrgb(slot)};
}

QSize ModelRhiPass::decodedTextureSize(QSize source, int max_texture_size, const SceneLimits& limits) {
  const int extent = static_cast<int>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(std::max(max_texture_size, 1)), limits.max_model_texture_dimension));
  if (source.width() <= extent && source.height() <= extent) {
    return source;
  }
  return source.scaled(extent, extent, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

bool ModelRhiPass::decodeFitsTransientLimit(
    QSize source, QSize decoded, bool scales_while_decoding, const SceneLimits& limits) {
  const QSize held = scales_while_decoding ? decoded : source;
  return static_cast<std::uint64_t>(held.width()) * static_cast<std::uint64_t>(held.height()) <=
         limits.max_model_texture_pixels_per_mesh;
}

AABB ModelRhiPass::meshBounds(const MeshData& mesh) {
  AABB output;
  for (const auto& vertex : mesh.vertices) {
    if (std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z)) {
      expandAABB(output, vertex.position);
    }
  }
  return output;
}

bool ModelRhiPass::drawsInColorPass(ModelDrawGroup group, const MeshShadingParams& shading) {
  return (group != ModelDrawGroup::kVisual || (shading.meshes_visible && shading.mesh_opacity > 0.0F)) &&
         (group != ModelDrawGroup::kCollision || (shading.collisions_visible && shading.collision_opacity > 0.0F));
}

bool ModelRhiPass::castsShadow(ModelDrawGroup group, bool shadows_requested) {
  return shadows_requested && group != ModelDrawGroup::kCollision;
}

float ModelRhiPass::drawOpacity(float layer_opacity, ModelDrawGroup group, const MeshShadingParams& shading) {
  if (group == ModelDrawGroup::kVisual) {
    return layer_opacity * shading.mesh_opacity;
  }
  if (group == ModelDrawGroup::kCollision) {
    return layer_opacity * shading.collision_opacity;
  }
  return layer_opacity;
}

float ModelRhiPass::shaderAlphaMode(const Material& material, bool material_applies) {
  if (!material_applies) {
    return 0.0F;
  }
  if (material.alpha_mode == AlphaMode::kMask) {
    return 1.0F;
  }
  return material.alpha_mode == AlphaMode::kBlend ? 2.0F : 0.0F;
}

bool ModelRhiPass::drawIsTranslucent(ModelDrawGroup group, float alpha, float alpha_mode) {
  return group == ModelDrawGroup::kCollision || alpha < 0.999F || alpha_mode > 1.5F;
}

PipelineDesc ModelRhiPass::meshDesc() const {
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
  return PipelineDesc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/model.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/model.frag.qsb")),
      .input_layout = layout,
      .bindings = layout_bindings_,
  };
}

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
  static constexpr std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
  QRhiTextureSubresourceUploadDescription white_texel(white.data(), static_cast<quint32>(white.size()));
  white_texel.setSourceSize(QSize(1, 1));
  setup.updates->uploadTexture(white_texture_, QRhiTextureUploadDescription({0, 0, white_texel}));

  PipelineDesc desc = meshDesc();
  triangle_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.topology = QRhiGraphicsPipeline::Lines;
  line_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.depth_write = false;
  line_no_depth_pipeline_ = setup.pipelines->add(desc, setup.part);
  desc.topology = QRhiGraphicsPipeline::Triangles;
  triangle_no_depth_pipeline_ = setup.pipelines->add(desc, setup.part);
  return ready();
}

bool ModelRhiPass::ready() const {
  return triangle_pipeline_ != nullptr && triangle_no_depth_pipeline_ != nullptr && line_pipeline_ != nullptr &&
         line_no_depth_pipeline_ != nullptr && layout_bindings_ != nullptr;
}

void ModelRhiPass::setLayers(const std::vector<ModelRenderable*>& layers) {
  adoptLayerList(layers, layers_, layer_gpu_, [this](LayerGpu& gpu) { releaseLayerGpu(gpu); });
}

void ModelRhiPass::admit(const PlaceFn& place, const MeshShadingParams& shading, bool shadows_requested) {
  if (admitted_prepared_) {
    prepared_admitted_.swap(admitted_);
    admitted_prepared_ = false;
  }
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
    prepared.revision = layer->modelRevision();
    prepared.opacity = layer->opacity();
    prepared.color_override = layer->colorOverrideEnabled();
    if (prepared.color_override) {
      prepared.override_color = layer->overrideColor();
    }
    prepared.wireframe = layer->wireframe();
    prepared.calls.reserve(layer->modelDrawCalls().size());
    bool has_color = false;
    AABB layer_bounds;
    const std::vector<ModelDrawCall>& draw_calls = layer->modelDrawCalls();
    for (std::size_t draw_index = 0; draw_index < draw_calls.size(); ++draw_index) {
      const ModelDrawCall& draw = draw_calls[draw_index];
      const bool color_requested = drawsInColorPass(draw.group, shading);
      const bool shadow_requested = castsShadow(draw.group, shadows_requested);
      if (!color_requested && !shadow_requested) {
        continue;
      }
      const auto mesh = layer->modelMeshes().find(draw.mesh_key);
      if (mesh == layer->modelMeshes().end() || mesh->second == nullptr || !mesh->second->ok || draw.frame_id.empty()) {
        continue;
      }
      // Cursor-placed (entry clock): a robot model's links ARE TF frames, and
      // scene-entity models follow the marker rule (a finding at its stamp).
      const Scene3DLayer& owner = layer->modelLayer();
      const auto placed = place(draw.frame_id, owner.renderStamp(), owner.placementHoldFor(draw.frame_id));
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
      if (color_requested) {
        layer_bounds = unionAABB(layer_bounds, transformedBounds(cached->second, fixed_from_model));
      }
      prepared.calls.push_back(
          {static_cast<std::uint32_t>(draw_index), fixed_from_model, color_requested, shadow_requested});
      has_color = has_color || color_requested;
      prepared.casts = prepared.casts || shadow_requested;
    }
    if (prepared.calls.empty()) {
      continue;
    }
    if (prepared.draws > kSceneLimits.max_model_draws_per_layer ||
        prepared.triangles > kSceneLimits.max_model_triangles_per_layer) {
      layer->modelLayer().noteRenderFailure(tr("Models need %1 draws and %2 triangles; the layer limits are %3 and %4")
                                                .arg(
                                                    QString::number(prepared.draws),
                                                    QString::number(prepared.triangles),
                                                    QString::number(kSceneLimits.max_model_draws_per_layer),
                                                    QString::number(kSceneLimits.max_model_triangles_per_layer)));
      continue;
    }
    if (has_color && !tryConsumeModels(prepared.draws, prepared.triangles, remaining_draws, remaining_triangles)) {
      layer->modelLayer().noteRenderFailure(
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
      for (Call& call : prepared.calls) {
        call.shadow = false;
      }
      std::erase_if(prepared.calls, [](const Call& call) { return !call.color; });
      prepared.casts = false;
      prepared.shadow_draws = 0;
      prepared.shadow_triangles = 0;
    }
    if (layer->contributesToSceneBounds()) {
      family_bounds = unionAABB(family_bounds, layer_bounds);
    }
    if (shadow_warning.isEmpty()) {
      layer->modelLayer().noteRenderSuccess();
    } else {
      layer->modelLayer().noteRenderFailure(shadow_warning);
    }
    admitted_.push_back(std::move(prepared));
  }
  last_bounds_ = family_bounds;
}

void ModelRhiPass::prepare(const RhiFrameContext& frame, const MeshShadingParams& shading) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  QRhi* rhi = frame.rhi;
  admitted_prepared_ = true;
  bool gpu_changed = false;
  for (const Admitted& prepared : admitted_) {
    bool changed = false;
    if (!ensureLayerGpu(rhi, prepared.layer, layer_gpu_[prepared.layer], changed)) {
      changed = true;
    }
    gpu_changed = gpu_changed || changed;
  }
  // The instances are a pure function of the admitted draws (placements
  // included), the render origin and these shading inputs; kept GPU meshes keep
  // the draws' mesh/material pointers valid.
  const std::array<float, 3> shading_key{shading.mesh_opacity, shading.collision_opacity, shading.roughness};
  const bool reuse = caches_enabled_ && instances_valid_ && !gpu_changed && frame.render_origin == prepared_origin_ &&
                     shading_key == prepared_shading_ && admitted_ == prepared_admitted_;
  if (!reuse) {
    buildInstances(rhi, frame.render_origin, shading);
    prepared_origin_ = frame.render_origin;
    prepared_shading_ = shading_key;
    shadow_upload_pending_ = true;
  }
  QRhiResourceUpdateBatch* updates = frame.updates;
  const quint32 instance_bytes = static_cast<quint32>(instances_.size() * sizeof(Instance));
  const quint32 instance_capacity = instance_capacity_;
  if (!ensureVertexBuffer(rhi, instance_buffer_, instance_capacity_, instance_bytes, kInitialBufferBytes)) {
    draws_.clear();
    instances_valid_ = false;
  } else if (instance_bytes != 0U && (!reuse || instance_capacity_ != instance_capacity)) {
    // A recreated buffer (new capacity) starts empty. A skipped Dynamic update
    // leaves every frame slot holding the last write.
    updates->updateDynamicBuffer(instance_buffer_, 0, instance_bytes, instances_.data());
    ++counters_.instance_uploads;
  }
  // The batch copies the bytes: the edge list's CPU copy can go right after.
  for (MeshGpu* mesh : pending_uploads_) {
    if (mesh->source == nullptr) {
      continue;
    }
    if (mesh->upload_pending) {
      // One copy each: the buffers are Immutable, whose every copy QRhi's Vulkan
      // backend stages in a full-size buffer of its own.
      updates->uploadStaticBuffer(mesh->vertex_buffer, mesh->source->vertices.data());
      updates->uploadStaticBuffer(mesh->triangle_index_buffer, mesh->source->indices.data());
      mesh->upload_pending = false;
    }
    if (mesh->edge_upload_pending && mesh->edge_index_buffer != nullptr) {
      updates->uploadStaticBuffer(mesh->edge_index_buffer, mesh->edge_indices.data());
      mesh->edge_indices = {};
      mesh->edge_upload_pending = false;
    }
  }
  pending_uploads_.clear();
  for (CachedTexture* texture : pending_textures_) {
    const QRhiTextureSubresourceUploadDescription description(texture->pending_image);
    updates->uploadTexture(texture->texture, QRhiTextureUploadDescription({0, 0, description}));
    texture->pending_image = {};
  }
  pending_textures_.clear();
}

void ModelRhiPass::buildInstances(QRhi* rhi, const glm::dvec3& render_origin, const MeshShadingParams& shading) {
  ++shadow_revision_;
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
  bool edges_failed = false;
  for (const Admitted& prepared : admitted_) {
    ModelRenderable* layer = prepared.layer;
    const auto found = layer_gpu_.find(layer);
    if (found == layer_gpu_.end() || found->second.uploaded_revision != prepared.revision) {
      continue;  // its GPU state could not be built this frame
    }
    LayerGpu& gpu = found->second;
    const std::vector<ModelDrawCall>& draw_calls = layer->modelDrawCalls();
    // The admitted call's mesh on the GPU (its source is the layer's MeshData).
    const auto gpu_mesh_of = [&](const Call& call) {
      const auto gpu_mesh = gpu.meshes.find(draw_calls[call.draw_index].mesh_key);
      return gpu_mesh == gpu.meshes.end() || gpu_mesh->second.source == nullptr ? nullptr : &gpu_mesh->second;
    };
    const std::size_t first_draw = draws_.size();
    for (const Call& call : prepared.calls) {
      MeshGpu* gpu_mesh = call.color ? gpu_mesh_of(call) : nullptr;
      if (gpu_mesh == nullptr) {
        continue;
      }
      const ModelDrawCall& draw = draw_calls[call.draw_index];
      const MeshData& source = *gpu_mesh->source;
      MeshGpu& mesh = *gpu_mesh;
      const bool lines = prepared.wireframe;
      if (lines && !ensureEdges(rhi, mesh)) {
        layer->modelLayer().noteRenderFailure(tr("Could not allocate a model wireframe buffer"));
        edges_failed = true;
        continue;
      }
      const glm::mat4 narrowed = relativeModel(call.fixed_from_model, render_origin);
      for (std::size_t index = 0; index < source.submeshes.size() && index < mesh.materials.size(); ++index) {
        const SubMesh& submesh = source.submeshes[index];
        if (submesh.index_count == 0U) {
          continue;
        }
        const Material fallback;
        const Material& material = submesh.material != nullptr ? *submesh.material : fallback;
        MaterialGpu& material_gpu = mesh.materials[index];
        const bool pbr_active = materialUsesPbrPath(material);
        bool use_material = draw.use_material;
        glm::vec4 tint = use_material ? material.base_color_factor : draw.override_color;
        if (prepared.color_override) {
          const QColor& override = prepared.override_color;
          tint.r = static_cast<float>(override.redF());
          tint.g = static_cast<float>(override.greenF());
          tint.b = static_cast<float>(override.blueF());
          use_material = false;
        }
        tint.a *= drawOpacity(prepared.opacity, draw.group, shading);
        const bool use_texture = material_gpu.textures[kBaseColorSlot].present && (pbr_active || use_material);
        const float alpha_mode = shaderAlphaMode(material, pbr_active || use_material);
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
            draw.group == ModelDrawGroup::kCollision ? 1.0F : 0.0F};
        instance.emissive_factor = {
            pbr_active ? material.emissive_factor.r : 0.0F, pbr_active ? material.emissive_factor.g : 0.0F,
            pbr_active ? material.emissive_factor.b : 0.0F, 0.0F};
        for (std::size_t slot = 0; slot < material_gpu.textures.size(); ++slot) {
          last_texture_slot_counts_[slot] += material_gpu.textures[slot].present ? 1 : 0;
        }
        const quint32 instance_offset = static_cast<quint32>(instances_.size());
        instances_.push_back(instance);
        const quint32 index_offset = lines ? mesh.edge_offsets[index] : static_cast<quint32>(submesh.index_offset);
        const quint32 index_count = lines ? mesh.edge_counts[index] : static_cast<quint32>(submesh.index_count);
        const bool translucent = drawIsTranslucent(draw.group, tint.a, alpha_mode);
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
    for (const Call& call : prepared.calls) {
      MeshGpu* gpu_mesh = call.shadow ? gpu_mesh_of(call) : nullptr;
      if (gpu_mesh == nullptr || gpu_mesh->triangle_index_buffer == nullptr || gpu_mesh->source->indices.empty()) {
        continue;
      }
      const glm::mat4 narrowed = relativeModel(call.fixed_from_model, render_origin);
      ShadowInstance instance;
      copyMatrix(narrowed, instance.model);
      const quint32 instance_offset = static_cast<quint32>(shadow_instances_.size());
      shadow_instances_.push_back(instance);
      const auto cached = bounds_cache_.find(gpu_mesh->source);
      if (cached != bounds_cache_.end()) {
        shadow_bounds_ =
            unionAABB(shadow_bounds_, transformedBounds(cached->second, call.fixed_from_model, render_origin));
      }
      const quint32 index_count = static_cast<quint32>(gpu_mesh->source->indices.size());
      shadow_draws_.push_back(ShadowDraw{gpu_mesh, instance_offset, index_count});
      last_shadow_triangle_count_ += static_cast<int>(index_count / 3U);
    }
  }
  // A failed edge list retries on the next frame instead of being skipped.
  instances_valid_ = !edges_failed;
}

bool ModelRhiPass::uploadShadowCasters(const RhiFrameContext& frame) {
  const quint32 shadow_bytes = static_cast<quint32>(shadow_instances_.size() * sizeof(ShadowInstance));
  const quint32 capacity = shadow_instance_capacity_;
  if (!ensureVertexBuffer(
          frame.rhi, shadow_instance_buffer_, shadow_instance_capacity_, shadow_bytes, kInitialBufferBytes)) {
    return false;
  }
  if (shadow_bytes != 0U && (shadow_upload_pending_ || shadow_instance_capacity_ != capacity)) {
    frame.updates->updateDynamicBuffer(shadow_instance_buffer_, 0, shadow_bytes, shadow_instances_.data());
    ++counters_.shadow_instance_uploads;
  }
  shadow_upload_pending_ = false;
  return true;
}

bool ModelRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  if (draw.mesh == nullptr || draw.material == nullptr) {
    return false;
  }
  const bool depth_write = draw.draw_class == DrawClass::kOpaque;
  const ScenePipeline* direct = draw.lines ? (depth_write ? line_pipeline_ : line_no_depth_pipeline_)
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
  shadow_pipeline_ = createPipeline(
      rhi,
      {
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/shadow.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/shadow.frag.qsb")),
          .input_layout = layout,
          .bindings = bindings,
          .blend = std::nullopt,
          .depth_bias = static_cast<int>(look::kShadowDepthBiasUnits),
          .slope_scaled_depth_bias = look::kShadowSlopeScaledDepthBias,
      },
      render_pass, 1);
  if (shadow_pipeline_ == nullptr) {
    return false;
  }
  shadow_bindings_ = bindings;
  return true;
}

void ModelRhiPass::releaseShadowResources() {
  shadow_pipeline_.reset();
  delete shadow_instance_buffer_;
  shadow_bindings_ = nullptr;
  shadow_instance_buffer_ = nullptr;
  shadow_instance_capacity_ = 0;
}

void ModelRhiPass::submitShadowCasters(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const {
  if (shadow_pipeline_ == nullptr || shadow_instance_buffer_ == nullptr) {
    return;
  }
  command_buffer->setGraphicsPipeline(shadow_pipeline_.get());
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
    if (prepared.layer != nullptr && prepared.casts) {
      prepared.layer->modelLayer().noteRenderFailure(error);
    }
  }
}

bool ModelRhiPass::ensureMeshMaskPipelines(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, bool depth_resolve) {
  if (meshMaskReady()) {
    return true;
  }
  if (!ready()) {
    return false;
  }
  // The colour pipelines' topology, vertex input, bindings and culling, drawn
  // red-only without blending, depth tested against the scene depth without
  // writing it.
  PipelineDesc desc = meshDesc();
  desc.fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/mesh_mask.frag.qsb"));
  QRhiGraphicsPipeline::TargetBlend mask_blend;
  mask_blend.enable = false;
  mask_blend.colorWrite = QRhiGraphicsPipeline::R;
  desc.blend = mask_blend;
  desc.depth_test = true;
  desc.depth_write = false;
  desc.depth_op = QRhiGraphicsPipeline::LessOrEqual;
  // A resolved depth holds one sample of the 4x pattern, off the pixel centre
  // where the mask rasterizes: pull the mask up to a pixel's slope closer so a
  // sloped mesh still passes against its own depth.
  const int resolve_bias = depth_resolve ? -1 : 0;
  desc.depth_bias += resolve_bias;
  desc.slope_scaled_depth_bias += static_cast<float>(resolve_bias);
  if (mask_triangle_pipeline_ == nullptr) {
    mask_triangle_pipeline_ = createPipeline(rhi, desc, render_pass, 1);
  }
  desc.topology = QRhiGraphicsPipeline::Lines;
  if (mask_line_pipeline_ == nullptr) {
    mask_line_pipeline_ = createPipeline(rhi, desc, render_pass, 1);
  }
  return meshMaskReady();
}

void ModelRhiPass::releaseMeshMaskPipelines() {
  mask_line_pipeline_.reset();
  mask_triangle_pipeline_.reset();
}

void ModelRhiPass::submitMeshMask(QRhiCommandBuffer* command_buffer, const QRhiViewport& viewport) const {
  for (const Draw& draw : draws_) {
    if (draw.mesh == nullptr || draw.material == nullptr || draw.index_count == 0U) {
      continue;
    }
    QRhiGraphicsPipeline* pipeline = draw.lines ? mask_line_pipeline_.get() : mask_triangle_pipeline_.get();
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

bool ModelRhiPass::ensureLayerGpu(QRhi* rhi, ModelRenderable* layer, LayerGpu& gpu, bool& changed) {
  if (rhi == nullptr || layer == nullptr || white_texture_ == nullptr || sampler_ == nullptr ||
      view_uniforms_ == nullptr || lighting_uniforms_ == nullptr) {
    return false;
  }
  if (gpu.uploaded_revision == layer->modelRevision()) {
    return true;
  }
  changed = true;
  const auto& sources = layer->modelMeshes();
  if (!caches_enabled_) {
    releaseLayerGpu(gpu);
  }
  // MeshData is immutable: a key still holding the same pointer is unchanged.
  std::erase_if(gpu.meshes, [&](auto& entry) {
    const auto source = sources.find(entry.first);
    if (source != sources.end() && source->second == entry.second.source) {
      return false;
    }
    releaseMeshGpu(entry.second);
    return true;
  });
  // Decoded textures and their QRhi textures are retained across the whole
  // layer, not just one mesh: kept meshes keep their charge.
  std::uint64_t remaining_layer_pixels = kSceneLimits.max_model_texture_pixels_per_layer;
  for (const auto& [_, mesh] : gpu.meshes) {
    remaining_layer_pixels -= std::min(remaining_layer_pixels, mesh.texture_pixels);
  }
  const auto fail = [&](const char* reason) {
    layer->modelLayer().noteRenderFailure(tr(reason));
    releaseLayerGpu(gpu);
    return false;
  };
  for (const auto& [key, source] : sources) {
    if (source == nullptr || !source->ok || source->vertices.empty() || source->indices.empty()) {
      continue;
    }
    if (const auto kept = gpu.meshes.find(key); kept != gpu.meshes.end()) {
      // clearShadowMap() may have dropped its bindings.
      for (MaterialGpu& material : kept->second.materials) {
        if (material.shader_resources == nullptr && !rebuildMaterialBindings(rhi, material)) {
          return fail("Could not create model material bindings");
        }
      }
      continue;
    }
    MeshGpu mesh;
    const char* error = nullptr;
    if (!buildMesh(rhi, source, remaining_layer_pixels, mesh, error)) {
      releaseMeshGpu(mesh);
      return fail(error);
    }
    ++counters_.mesh_builds;
    mesh.upload_pending = true;
    pending_uploads_.push_back(&gpu.meshes.emplace(key, std::move(mesh)).first->second);
  }
  gpu.uploaded_revision = layer->modelRevision();
  return true;
}

bool ModelRhiPass::buildMesh(
    QRhi* rhi, const std::shared_ptr<const MeshData>& source, std::uint64_t& remaining_layer_pixels, MeshGpu& mesh,
    const char*& error) {
  const auto fail = [&error](const char* reason) {
    error = reason;
    return false;
  };
  mesh.source = source;
  const std::uint64_t vertex_bytes = source->vertices.size() * sizeof(pj::scene3d::Vertex);
  const std::uint64_t index_bytes = source->indices.size() * sizeof(std::uint32_t);
  if (vertex_bytes > std::numeric_limits<quint32>::max() || index_bytes > std::numeric_limits<quint32>::max()) {
    return fail("A model GPU buffer exceeds the 4 GiB buffer limit");
  }
  mesh.vertex_buffer =
      rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(vertex_bytes));
  mesh.triangle_index_buffer =
      rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(index_bytes));
  if (!mesh.vertex_buffer->create() || !mesh.triangle_index_buffer->create()) {
    return fail("Could not allocate model GPU buffers");
  }

  // Charge every distinct texture against the mesh/layer budgets, decoding the
  // cache misses, before taking any cache reference: a late fifth-slot
  // rejection cannot leave a partially committed material or consume the layer
  // budget. Sizes are checked before a decode allocates.
  const int max_texture_size = rhi->resourceLimit(QRhi::TextureSizeMax);
  std::vector<TextureKey> keys;
  std::map<TextureKey, QImage> decoded;
  std::uint64_t remaining_mesh_pixels = kSceneLimits.max_model_texture_pixels_per_mesh;
  std::uint64_t candidate_layer_pixels = remaining_layer_pixels;
  for (const SubMesh& submesh : source->submeshes) {
    if (submesh.material == nullptr) {
      continue;
    }
    const auto textures = submesh.material->textureSlots();
    for (std::size_t slot = 0; slot < textures.size(); ++slot) {
      const TextureSource* texture = textures[slot];
      if (texture == nullptr || texture->empty()) {
        continue;
      }
      TextureKey key = textureKey(*texture, slot);
      if (std::find(keys.cbegin(), keys.cend(), key) != keys.cend()) {
        continue;
      }
      if (const auto cached = textures_.find(key); cached != textures_.end()) {
        if (!tryConsumeModelTexturePixels(cached->second.pixels, remaining_mesh_pixels, candidate_layer_pixels)) {
          return fail("A model exceeds the decoded-texture limit");
        }
        keys.push_back(std::move(key));
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
      const QSize source_size = reader.size();
      const QSize size = decodedTextureSize(source_size, max_texture_size);
      QImage image;
      if (size.isValid()) {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
        if (!tryConsumeModelTexturePixels(pixels, remaining_mesh_pixels, candidate_layer_pixels)) {
          return fail("A model exceeds the decoded-texture limit");
        }
        if (size != source_size) {
          if (!decodeFitsTransientLimit(
                  source_size, size, reader.supportsOption(QImageIOHandler::ScaledSize), kSceneLimits)) {
            return fail("A model exceeds the decoded-texture limit");
          }
          reader.setScaledSize(size);
        }
        image = reader.read();
      }
      if (!size.isValid() || image.isNull()) {
        return fail("Could not decode a model material texture");
      }
      ++counters_.texture_decodes;
      decoded.emplace(key, image.convertToFormat(QImage::Format_RGBA8888));
      keys.push_back(std::move(key));
    }
  }

  // Commit: every reference taken is in texture_refs, which releaseMeshGpu() drops.
  for (const TextureKey& key : keys) {
    const auto image = decoded.find(key);
    if (image == decoded.end()) {
      ++textures_.at(key).references;
      mesh.texture_refs.push_back(key);
      continue;
    }
    const QRhiTexture::Flags flags = key.second ? QRhiTexture::sRGB : QRhiTexture::Flags{};
    QRhiTexture* texture = rhi->newTexture(QRhiTexture::RGBA8, image->second.size(), 1, flags);
    if (texture == nullptr || !texture->create()) {
      delete texture;
      return fail("Could not allocate a model material texture");
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(image->second.width()) * static_cast<std::uint64_t>(image->second.height());
    CachedTexture& cached = textures_[key];
    cached = CachedTexture{texture, std::move(image->second), pixels, 1};
    pending_textures_.push_back(&cached);
    mesh.texture_refs.push_back(key);
  }

  mesh.materials.resize(source->submeshes.size());
  for (std::size_t index = 0; index < source->submeshes.size(); ++index) {
    const std::shared_ptr<const Material>& material = source->submeshes[index].material;
    MaterialGpu& material_gpu = mesh.materials[index];
    const Material fallback;
    const auto textures = (material != nullptr ? *material : fallback).textureSlots();
    for (std::size_t slot = 0; slot < textures.size(); ++slot) {
      TextureGpu& texture_gpu = material_gpu.textures[slot];
      texture_gpu.texture = white_texture_;
      if (textures[slot] != nullptr && !textures[slot]->empty()) {
        texture_gpu.texture = textures_.at(textureKey(*textures[slot], slot)).texture;
        texture_gpu.present = true;
      }
    }
    if (!rebuildMaterialBindings(rhi, material_gpu)) {
      return fail("Could not create model material bindings");
    }
  }
  mesh.texture_pixels = kSceneLimits.max_model_texture_pixels_per_mesh - remaining_mesh_pixels;
  remaining_layer_pixels = candidate_layer_pixels;
  return true;
}

bool ModelRhiPass::ensureEdges(QRhi* rhi, MeshGpu& mesh) {
  if (mesh.edges_built) {
    return true;
  }
  const MeshData& source = *mesh.source;
  mesh.edge_indices.clear();
  mesh.edge_offsets.clear();
  mesh.edge_counts.clear();
  mesh.edge_indices.reserve(source.indices.size() * 2U);
  for (const SubMesh& submesh : source.submeshes) {
    const quint32 first = static_cast<quint32>(mesh.edge_indices.size());
    const std::size_t end = std::min(source.indices.size(), submesh.index_offset + submesh.index_count);
    for (std::size_t index = submesh.index_offset; index + 2U < end; index += 3U) {
      const quint32 a = source.indices[index];
      const quint32 b = source.indices[index + 1U];
      const quint32 c = source.indices[index + 2U];
      mesh.edge_indices.insert(mesh.edge_indices.end(), {a, b, b, c, c, a});
    }
    mesh.edge_offsets.push_back(first);
    mesh.edge_counts.push_back(static_cast<quint32>(mesh.edge_indices.size()) - first);
  }
  if (!mesh.edge_indices.empty()) {
    const std::uint64_t edge_bytes = mesh.edge_indices.size() * sizeof(quint32);
    if (edge_bytes <= std::numeric_limits<quint32>::max()) {
      mesh.edge_index_buffer =
          rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(edge_bytes));
      if (!mesh.edge_index_buffer->create()) {
        delete mesh.edge_index_buffer;
        mesh.edge_index_buffer = nullptr;
      }
    }
    if (mesh.edge_index_buffer == nullptr) {
      mesh.edge_indices = {};
      mesh.edge_offsets.clear();
      mesh.edge_counts.clear();
      return false;
    }
    mesh.edge_upload_pending = true;
    if (std::find(pending_uploads_.cbegin(), pending_uploads_.cend(), &mesh) == pending_uploads_.cend()) {
      pending_uploads_.push_back(&mesh);
    }
  }
  mesh.edges_built = true;
  ++counters_.edge_builds;
  return true;
}

void ModelRhiPass::releaseTexture(const TextureKey& key) {
  const auto found = textures_.find(key);
  if (found == textures_.end() || --found->second.references > 0) {
    return;
  }
  std::erase(pending_textures_, &found->second);
  delete found->second.texture;
  textures_.erase(found);
}

void ModelRhiPass::releaseMeshGpu(MeshGpu& gpu) {
  std::erase(pending_uploads_, &gpu);
  // Destroy every binding before any texture it may reference.
  for (MaterialGpu& material : gpu.materials) {
    delete material.shader_resources;
    material.shader_resources = nullptr;
  }
  for (const TextureKey& key : gpu.texture_refs) {
    releaseTexture(key);
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
  for (auto& [_, texture] : textures_) {
    delete texture.texture;  // none left once every layer is released
  }
  textures_.clear();
  pending_textures_.clear();
  prepared_admitted_.clear();
  admitted_prepared_ = false;
  instances_valid_ = false;
  shadow_upload_pending_ = false;
  releaseMeshMaskPipelines();
  line_no_depth_pipeline_ = nullptr;
  line_pipeline_ = nullptr;
  triangle_no_depth_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  delete layout_bindings_;
  delete sampler_;
  delete white_texture_;
  delete instance_buffer_;
  layout_bindings_ = nullptr;
  sampler_ = nullptr;
  white_texture_ = nullptr;
  instance_buffer_ = nullptr;
  instance_capacity_ = 0;
  shadow_texture_ = nullptr;
  shadow_sampler_ = nullptr;
  view_uniforms_ = nullptr;
  lighting_uniforms_ = nullptr;
  render_mode_uniforms_ = nullptr;
  last_shadow_triangle_count_ = 0;
}

}  // namespace pj::scene3d
