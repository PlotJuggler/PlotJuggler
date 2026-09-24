// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <QFile>
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_scene3d_core/shadow_camera.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_view_widget_rhi.h"
#include "scene_view_widget_rhi_quality_p.h"

namespace pj::scene3d {
namespace {

constexpr int kHdrSampleCount = 4;
// WebGL2 cannot resolve depth from Qt's multisample renderbuffer, and its GLES
// QRhi does not resolve a second multisample color attachment reliably either.
// Replay the bounded scene once into single-sample RGBA8 coverage + D32F depth:
// 4x (RGBA16F 8 + D32F 4) + RGBA16F 8 + RGBA8 4 + D32F 4 = 64 B/px.
constexpr std::uint64_t kHdrBytesPerPixel = 64U;
constexpr std::uint64_t kMaxHdrBytesPerView = 256U * 1024U * 1024U;
constexpr std::uint64_t kMaxHdrBytesPerRhi = 512U * 1024U * 1024U;

// Every live Scene3D dock shares one QRhi in the browser. Keep the retained
// offscreen chains below both a per-view and an aggregate per-engine ceiling.
std::unordered_map<QRhi*, std::uint64_t> hdr_bytes_by_rhi;

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning("SceneViewWidget WASM: failed to open shader %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

QRhiGraphicsPipeline* clonePipelineForRenderPass(
    QRhi* owner, const QRhiGraphicsPipeline* source, QRhiRenderPassDescriptor* render_pass, int sample_count,
    bool annotation_alpha = false, bool coverage_alpha_only = false) {
  if (owner == nullptr || source == nullptr || render_pass == nullptr) {
    return nullptr;
  }
  QRhiGraphicsPipeline* pipeline = owner->newGraphicsPipeline();
  pipeline->setTopology(source->topology());
  pipeline->setShaderStages(source->cbeginShaderStages(), source->cendShaderStages());
  pipeline->setVertexInputLayout(source->vertexInputLayout());
  pipeline->setShaderResourceBindings(source->shaderResourceBindings());
  std::vector<QRhiGraphicsPipeline::TargetBlend> target_blends(
      source->cbeginTargetBlends(), source->cendTargetBlends());
  if (target_blends.empty()) {
    target_blends.emplace_back();
  }
  if (annotation_alpha) {
    target_blends.front().srcAlpha = QRhiGraphicsPipeline::Zero;
    target_blends.front().dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  }
  if (coverage_alpha_only) {
    target_blends.front().colorWrite = QRhiGraphicsPipeline::A;
  }
  pipeline->setTargetBlends(target_blends.cbegin(), target_blends.cend());
  pipeline->setCullMode(source->cullMode());
  pipeline->setFrontFace(source->frontFace());
  pipeline->setDepthTest(source->hasDepthTest());
  pipeline->setDepthWrite(source->hasDepthWrite());
  pipeline->setDepthOp(source->depthOp());
  pipeline->setDepthBias(source->depthBias());
  pipeline->setSlopeScaledDepthBias(source->slopeScaledDepthBias());
  pipeline->setSampleCount(sample_count);
  pipeline->setRenderPassDescriptor(render_pass);
  if (!pipeline->create()) {
    delete pipeline;
    return nullptr;
  }
  return pipeline;
}

}  // namespace

void SceneViewWidget::setForceHdrFallbackForTest(bool force) {
  if (hdr_stage_.force_fallback_for_test == force) {
    return;
  }
  hdr_stage_.force_fallback_for_test = force;
  hdr_stage_.rejected_size = {};
  if (force) {
    releaseHdrResources();
  }
  update();
}

void SceneViewWidget::setForceShadowFallbackForTest(bool force) {
  if (shadow_stage_.force_fallback_for_test == force) {
    return;
  }
  shadow_stage_.force_fallback_for_test = force;
  shadow_stage_.capability_error.clear();
  shadow_stage_.rejected_size = {};
  if (force) {
    releaseShadowResources(true);
  }
  update();
}

void SceneViewWidget::setForceSsaoFallbackForTest(bool force) {
  if (ssao_stage_.force_fallback_for_test == force) {
    return;
  }
  ssao_stage_.force_fallback_for_test = force;
  ssao_stage_.rejected_size = {};
  if (force) {
    releaseSsaoResources();
  }
  update();
}

void SceneViewWidget::setForceEdlFallbackForTest(bool force) {
  if (edl_stage_.force_fallback_for_test == force) {
    return;
  }
  edl_stage_.force_fallback_for_test = force;
  edl_stage_.rejected_size = {};
  if (force) {
    releaseEdlResources();
  }
  update();
}

bool SceneViewWidget::ensureShadowResources(QRhi* owner) {
  if (shadow_texture_ != nullptr && shadow_render_target_ != nullptr && shadow_pipeline_ != nullptr &&
      grid_shadow_pipeline_ != nullptr && shadow_shader_resources_ != nullptr &&
      grid_shadow_shader_resources_ != nullptr) {
    return true;
  }
  if (owner == nullptr || renderTarget() == nullptr || uniform_buffer_ == nullptr || model_uniform_buffer_ == nullptr) {
    return false;
  }
  // A failure blocks retries only while the output size is unchanged — a
  // resize re-probes, mirroring the HDR/SSAO/EDL rejected-size recovery.
  if (shadow_stage_.blockedFor(renderTarget()->pixelSize())) {
    return false;
  }
  const auto fail = [this](QString reason) {
    shadow_stage_.capability_error = std::move(reason);
    shadow_stage_.rejected_size = renderTarget() != nullptr ? renderTarget()->pixelSize() : QSize{};
    qWarning("SceneViewWidget WASM shadows unavailable: %s", qPrintable(shadow_stage_.capability_error));
    releaseShadowResources(true);
    return false;
  };
  if (shadow_stage_.force_fallback_for_test) {
    return fail(tr("Shadow rendering disabled by the browser acceptance override"));
  }
  const int maximum_texture_size = owner->resourceLimit(QRhi::TextureSizeMax);
  if (maximum_texture_size < kShadowMapSize) {
    return fail(tr("A %1 x %1 shadow map exceeds this browser GPU's %2-pixel texture limit")
                    .arg(QString::number(kShadowMapSize), QString::number(maximum_texture_size)));
  }
  if (!owner->isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget)) {
    return fail(tr("This browser GPU does not support sampled D32F render targets"));
  }

  const QShader shadow_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/shadow.vert.qsb"));
  const QShader shadow_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/shadow.frag.qsb"));
  const QShader grid_vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/grid_shadow.vert.qsb"));
  const QShader grid_fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/grid_shadow.frag.qsb"));
  if (!shadow_vertex_shader.isValid() || !shadow_fragment_shader.isValid() || !grid_vertex_shader.isValid() ||
      !grid_fragment_shader.isValid()) {
    return fail(tr("The browser shadow shaders are unavailable"));
  }

  shadow_texture_ =
      owner->newTexture(QRhiTexture::D32F, QSize(kShadowMapSize, kShadowMapSize), 1, QRhiTexture::RenderTarget);
  if (shadow_texture_ == nullptr || !shadow_texture_->create()) {
    return fail(tr("Could not allocate the browser shadow depth texture"));
  }
  QRhiTextureRenderTargetDescription target_description;
  target_description.setDepthTexture(shadow_texture_);
  shadow_render_target_ = owner->newTextureRenderTarget(target_description);
  shadow_render_pass_descriptor_ = shadow_render_target_->newCompatibleRenderPassDescriptor();
  shadow_render_target_->setRenderPassDescriptor(shadow_render_pass_descriptor_);
  if (!shadow_render_target_->create()) {
    return fail(tr("Could not create the browser shadow render target"));
  }

  shadow_sampler_ = owner->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  shadow_uniform_buffer_ = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kShadowUniformBytes);
  if (shadow_sampler_ == nullptr || shadow_uniform_buffer_ == nullptr || !shadow_sampler_->create() ||
      !shadow_uniform_buffer_->create()) {
    return fail(tr("Could not allocate the browser shadow bindings"));
  }
  shadow_shader_resources_ = owner->newShaderResourceBindings();
  shadow_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, shadow_uniform_buffer_),
  });
  grid_shadow_shader_resources_ = owner->newShaderResourceBindings();
  grid_shadow_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
      QRhiShaderResourceBinding::uniformBuffer(1, QRhiShaderResourceBinding::FragmentStage, model_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          2, QRhiShaderResourceBinding::FragmentStage, shadow_texture_, shadow_sampler_),
      QRhiShaderResourceBinding::uniformBuffer(
          8, QRhiShaderResourceBinding::FragmentStage, render_mode_uniform_buffer_),
  });
  if (!shadow_shader_resources_->create() || !grid_shadow_shader_resources_->create()) {
    return fail(tr("Could not create the browser shadow resource bindings"));
  }

  QRhiVertexInputLayout shadow_layout;
  shadow_layout.setBindings({
      QRhiVertexInputBinding(sizeof(pj::scene3d::Vertex)),
      QRhiVertexInputBinding(sizeof(ShadowInstance), QRhiVertexInputBinding::PerInstance),
  });
  shadow_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(pj::scene3d::Vertex, position)),
      QRhiVertexInputAttribute(1, 5, QRhiVertexInputAttribute::Float4, 0U),
      QRhiVertexInputAttribute(1, 6, QRhiVertexInputAttribute::Float4, 16U),
      QRhiVertexInputAttribute(1, 7, QRhiVertexInputAttribute::Float4, 32U),
      QRhiVertexInputAttribute(1, 8, QRhiVertexInputAttribute::Float4, 48U),
  });
  shadow_pipeline_ = owner->newGraphicsPipeline();
  shadow_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  shadow_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, shadow_vertex_shader},
      {QRhiShaderStage::Fragment, shadow_fragment_shader},
  });
  shadow_pipeline_->setVertexInputLayout(shadow_layout);
  shadow_pipeline_->setShaderResourceBindings(shadow_shader_resources_);
  shadow_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  shadow_pipeline_->setDepthTest(true);
  shadow_pipeline_->setDepthWrite(true);
  shadow_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  shadow_pipeline_->setDepthBias(static_cast<int>(look::kShadowDepthBiasUnits));
  shadow_pipeline_->setSlopeScaledDepthBias(look::kShadowSlopeScaledDepthBias);
  shadow_pipeline_->setSampleCount(1);
  shadow_pipeline_->setRenderPassDescriptor(shadow_render_pass_descriptor_);
  if (!shadow_pipeline_->create()) {
    return fail(tr("Could not create the browser shadow depth pipeline"));
  }

  QRhiVertexInputLayout grid_layout;
  grid_layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  grid_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(Vertex, r)),
  });
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  grid_shadow_pipeline_ = owner->newGraphicsPipeline();
  grid_shadow_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  grid_shadow_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, grid_vertex_shader},
      {QRhiShaderStage::Fragment, grid_fragment_shader},
  });
  grid_shadow_pipeline_->setVertexInputLayout(grid_layout);
  grid_shadow_pipeline_->setShaderResourceBindings(grid_shadow_shader_resources_);
  grid_shadow_pipeline_->setTargetBlends({blend});
  grid_shadow_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  grid_shadow_pipeline_->setDepthTest(true);
  grid_shadow_pipeline_->setDepthWrite(true);
  grid_shadow_pipeline_->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  grid_shadow_pipeline_->setSampleCount(sampleCount());
  grid_shadow_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!grid_shadow_pipeline_->create() || !rebuildAllModelMaterialBindings(owner)) {
    return fail(tr("Could not create the browser shadow receiver pipeline"));
  }
  if (hdr_render_pass_descriptor_ != nullptr) {
    hdr_grid_shadow_pipeline_ =
        clonePipelineForRenderPass(owner, grid_shadow_pipeline_, hdr_render_pass_descriptor_, kHdrSampleCount);
    if (hdr_grid_shadow_pipeline_ == nullptr) {
      return fail(tr("Could not create the HDR shadow receiver pipeline"));
    }
  }
  if (hdr_depth_render_pass_descriptor_ != nullptr) {
    QRhiGraphicsPipeline*& depth_pipeline = hdr_depth_pipelines_[grid_shadow_pipeline_];
    depth_pipeline =
        clonePipelineForRenderPass(owner, grid_shadow_pipeline_, hdr_depth_render_pass_descriptor_, 1, false, true);
    if (depth_pipeline == nullptr) {
      return fail(tr("Could not create the HDR shadow receiver replay pipeline"));
    }
  }

  shadow_map_size_ = kShadowMapSize;
  shadow_stage_.capability_error.clear();
  ++shadow_stage_.resource_generation;
  return true;
}

void SceneViewWidget::releaseShadowResources(bool restore_model_bindings) {
  if (auto iterator = hdr_depth_pipelines_.find(grid_shadow_pipeline_); iterator != hdr_depth_pipelines_.end()) {
    delete iterator->second;
    hdr_depth_pipelines_.erase(iterator);
  }
  delete hdr_grid_shadow_pipeline_;
  delete grid_shadow_pipeline_;
  hdr_grid_shadow_pipeline_ = nullptr;
  grid_shadow_pipeline_ = nullptr;
  delete shadow_pipeline_;
  shadow_pipeline_ = nullptr;
  delete grid_shadow_shader_resources_;
  grid_shadow_shader_resources_ = nullptr;
  delete shadow_shader_resources_;
  shadow_shader_resources_ = nullptr;

  QRhiTexture* released_texture = shadow_texture_;
  QRhiSampler* released_sampler = shadow_sampler_;
  shadow_texture_ = nullptr;
  shadow_sampler_ = nullptr;
  if (restore_model_bindings && resource_rhi_ != nullptr && model_white_texture_ != nullptr) {
    if (!rebuildAllModelMaterialBindings(resource_rhi_)) {
      // Keep this frame's mesh/range pointers valid, but remove every binding
      // that still references the soon-to-be-destroyed depth texture. The
      // revision mismatch rebuilds the complete layer on the next frame.
      for (auto& [_, layer] : model_layer_gpu_) {
        for (auto& [__, mesh] : layer.meshes) {
          for (ModelMaterialGpu& material : mesh.materials) {
            delete material.shader_resources;
            material.shader_resources = nullptr;
          }
        }
        layer.uploaded_revision = ~std::uint64_t{0};
      }
    }
  } else if (!restore_model_bindings) {
    for (auto& [_, layer] : model_layer_gpu_) {
      for (auto& [__, mesh] : layer.meshes) {
        for (ModelMaterialGpu& material : mesh.materials) {
          delete material.shader_resources;
          material.shader_resources = nullptr;
        }
      }
    }
  }

  delete shadow_render_target_;
  delete shadow_render_pass_descriptor_;
  delete shadow_instance_buffer_;
  delete shadow_uniform_buffer_;
  delete released_sampler;
  delete released_texture;
  shadow_render_target_ = nullptr;
  shadow_render_pass_descriptor_ = nullptr;
  shadow_instance_buffer_ = nullptr;
  shadow_uniform_buffer_ = nullptr;
  shadow_instance_buffer_capacity_ = 0;
  shadow_map_size_ = 0;
  shadow_stage_.last_active = false;
}

void SceneViewWidget::releaseHdrTargetResources() {
  // The present SRB owns references to both resolved textures. Destroy it
  // before the target and attachments; geometry pipelines only retain the
  // compatible render-pass descriptor and can survive a size-only rebuild.
  releaseSsaoTargetResources();
  releaseEdlTargetResources();
  delete present_pipeline_;
  delete present_shader_resources_;
  delete hdr_depth_render_target_;
  delete hdr_render_target_;
  delete hdr_msaa_color_;
  delete hdr_msaa_depth_;
  delete hdr_resolve_color_;
  delete hdr_resolve_depth_;
  delete hdr_resolve_coverage_;
  present_shader_resources_ = nullptr;
  present_pipeline_ = nullptr;
  hdr_depth_render_target_ = nullptr;
  hdr_render_target_ = nullptr;
  hdr_msaa_color_ = nullptr;
  hdr_msaa_depth_ = nullptr;
  hdr_resolve_color_ = nullptr;
  hdr_resolve_depth_ = nullptr;
  hdr_resolve_coverage_ = nullptr;
  hdr_render_size_ = {};
  hdr_stage_.last_active = false;

  if (resource_rhi_ != nullptr && hdr_allocated_bytes_ != 0U) {
    auto iterator = hdr_bytes_by_rhi.find(resource_rhi_);
    if (iterator != hdr_bytes_by_rhi.end()) {
      iterator->second = iterator->second > hdr_allocated_bytes_ ? iterator->second - hdr_allocated_bytes_ : 0U;
      if (iterator->second == 0U) {
        hdr_bytes_by_rhi.erase(iterator);
      }
    }
  }
  hdr_allocated_bytes_ = 0U;
}

void SceneViewWidget::releaseSsaoTargetResources() {
  // The pipeline retains the SRB layout and target descriptor; the SRB retains
  // replay depth. Destroy all size-bound references before HDR attachments.
  delete ssao_pipeline_;
  delete ssao_shader_resources_;
  delete ssao_raw_render_target_;
  ssao_pipeline_ = nullptr;
  ssao_shader_resources_ = nullptr;
  ssao_raw_render_target_ = nullptr;
  ssao_stage_.last_active = false;
}

void SceneViewWidget::releaseSsaoResources() {
  releaseSsaoTargetResources();
  delete ssao_uniform_buffer_;
  delete ssao_render_pass_descriptor_;
  ssao_uniform_buffer_ = nullptr;
  ssao_render_pass_descriptor_ = nullptr;
}

void SceneViewWidget::releaseEdlTargetResources() {
  delete edl_mesh_mask_render_target_;
  edl_mesh_mask_render_target_ = nullptr;
  edl_stage_.last_active = false;
}

void SceneViewWidget::releaseEdlResources() {
  // The pipelines retain the compatible render-pass descriptor and the target
  // retains the shared HDR replay attachments. Destroy those references before
  // the core HDR chain can release either dependency.
  delete edl_mesh_mask_triangle_pipeline_;
  delete edl_mesh_mask_line_pipeline_;
  edl_mesh_mask_triangle_pipeline_ = nullptr;
  edl_mesh_mask_line_pipeline_ = nullptr;
  releaseEdlTargetResources();
  delete edl_mesh_mask_render_pass_descriptor_;
  edl_mesh_mask_render_pass_descriptor_ = nullptr;
}

void SceneViewWidget::updateRenderingWarning() {
  // SSAO/EDL are probed only while the HDR chain runs, so their stored errors
  // go stale the moment HDR is down — suppress them until HDR is live again.
  const QString warning = !hdr_stage_.capability_error.isEmpty()    ? hdr_stage_.capability_error
                          : !hdr_stage_.last_active                 ? QString{}
                          : !ssao_stage_.capability_error.isEmpty() ? ssao_stage_.capability_error
                                                                    : edl_stage_.capability_error;
  if (warning == rendering_warning_) {
    return;
  }
  rendering_warning_ = warning;
  emit renderingWarningChanged(rendering_warning_);
}

void SceneViewWidget::releaseHdrResources() {
  // Pipelines reference the HDR render-pass descriptor; release all of them
  // before the descriptor and all SRBs before their textures.
  releaseSsaoResources();
  releaseEdlResources();
  for (auto& [_, pipeline] : hdr_depth_pipelines_) {
    delete pipeline;
  }
  hdr_depth_pipelines_.clear();
  delete hdr_grid_shadow_pipeline_;
  delete hdr_model_line_no_depth_pipeline_;
  delete hdr_model_line_pipeline_;
  delete hdr_model_triangle_no_depth_pipeline_;
  delete hdr_model_triangle_pipeline_;
  delete hdr_marker_line_no_depth_pipeline_;
  delete hdr_marker_line_pipeline_;
  delete hdr_marker_triangle_cull_no_depth_pipeline_;
  delete hdr_marker_triangle_cull_pipeline_;
  delete hdr_marker_triangle_no_depth_pipeline_;
  delete hdr_marker_triangle_pipeline_;
  delete hdr_voxel_pipeline_;
  delete hdr_occupancy_pipeline_;
  delete hdr_pose_pipeline_;
  delete hdr_cube_pipeline_;
  delete hdr_point_pipeline_;
  delete hdr_triangle_no_depth_pipeline_;
  delete hdr_line_no_depth_pipeline_;
  delete hdr_triangle_pipeline_;
  delete hdr_annotation_line_pipeline_;
  delete hdr_line_pipeline_;
  delete present_pipeline_;
  hdr_grid_shadow_pipeline_ = nullptr;
  hdr_model_line_no_depth_pipeline_ = nullptr;
  hdr_model_line_pipeline_ = nullptr;
  hdr_model_triangle_no_depth_pipeline_ = nullptr;
  hdr_model_triangle_pipeline_ = nullptr;
  hdr_marker_line_no_depth_pipeline_ = nullptr;
  hdr_marker_line_pipeline_ = nullptr;
  hdr_marker_triangle_cull_no_depth_pipeline_ = nullptr;
  hdr_marker_triangle_cull_pipeline_ = nullptr;
  hdr_marker_triangle_no_depth_pipeline_ = nullptr;
  hdr_marker_triangle_pipeline_ = nullptr;
  hdr_voxel_pipeline_ = nullptr;
  hdr_occupancy_pipeline_ = nullptr;
  hdr_pose_pipeline_ = nullptr;
  hdr_cube_pipeline_ = nullptr;
  hdr_point_pipeline_ = nullptr;
  hdr_triangle_no_depth_pipeline_ = nullptr;
  hdr_line_no_depth_pipeline_ = nullptr;
  hdr_triangle_pipeline_ = nullptr;
  hdr_annotation_line_pipeline_ = nullptr;
  hdr_line_pipeline_ = nullptr;
  present_pipeline_ = nullptr;
  releaseHdrTargetResources();
  delete composite_uniform_buffer_;
  delete hdr_coverage_sampler_;
  delete hdr_color_sampler_;
  delete hdr_depth_render_pass_descriptor_;
  delete hdr_render_pass_descriptor_;
  composite_uniform_buffer_ = nullptr;
  hdr_coverage_sampler_ = nullptr;
  hdr_color_sampler_ = nullptr;
  hdr_depth_render_pass_descriptor_ = nullptr;
  hdr_render_pass_descriptor_ = nullptr;
}

bool SceneViewWidget::ensureSsaoResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  if (ssao_raw_render_target_ != nullptr && ssao_pipeline_ != nullptr && ssao_shader_resources_ != nullptr &&
      ssao_uniform_buffer_ != nullptr) {
    return true;
  }
  if (ssao_stage_.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) {
    releaseSsaoResources();
    ssao_stage_.rejected_size = size;
    const bool changed = ssao_stage_.capability_error != reason;
    ssao_stage_.capability_error = std::move(reason);
    if (changed) {
      qWarning("SceneViewWidget WASM SSAO unavailable: %s", qPrintable(ssao_stage_.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (ssao_stage_.force_fallback_for_test) {
    return fail(tr("SSAO rendering disabled by the browser acceptance override"));
  }
  if (hdr_resolve_coverage_ == nullptr || hdr_resolve_depth_ == nullptr || hdr_coverage_sampler_ == nullptr) {
    return fail(tr("The browser HDR depth replay is not ready for SSAO"));
  }

  if (ssao_uniform_buffer_ == nullptr) {
    ssao_uniform_buffer_ = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kSsaoUniformBytes);
    if (!ssao_uniform_buffer_->create()) {
      return fail(tr("Could not allocate the browser SSAO kernel buffer"));
    }
  }

  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({QRhiColorAttachment(hdr_resolve_coverage_)});
  ssao_raw_render_target_ = owner->newTextureRenderTarget(description, QRhiTextureRenderTarget::PreserveColorContents);
  if (ssao_render_pass_descriptor_ == nullptr) {
    ssao_render_pass_descriptor_ = ssao_raw_render_target_->newCompatibleRenderPassDescriptor();
  }
  ssao_raw_render_target_->setRenderPassDescriptor(ssao_render_pass_descriptor_);
  if (!ssao_raw_render_target_->create()) {
    return fail(tr("Could not preserve the browser replay coverage for SSAO"));
  }

  ssao_shader_resources_ = owner->newShaderResourceBindings();
  ssao_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, ssao_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, hdr_resolve_depth_, hdr_coverage_sampler_),
  });
  if (!ssao_shader_resources_->create()) {
    return fail(tr("Could not create the browser SSAO bindings"));
  }

  const QShader vertex_shader = loadShader(presentVertexShaderPath(*owner));
  const QShader fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/ssao.frag.qsb"));
  if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
    return fail(tr("Could not load the browser SSAO shaders"));
  }
  ssao_pipeline_ = owner->newGraphicsPipeline();
  ssao_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  ssao_pipeline_->setShaderStages({
      {QRhiShaderStage::Vertex, vertex_shader},
      {QRhiShaderStage::Fragment, fragment_shader},
  });
  ssao_pipeline_->setShaderResourceBindings(ssao_shader_resources_);
  QRhiGraphicsPipeline::TargetBlend raw_ao_write;
  raw_ao_write.enable = false;
  raw_ao_write.colorWrite = QRhiGraphicsPipeline::G;
  ssao_pipeline_->setTargetBlends({raw_ao_write});
  ssao_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  ssao_pipeline_->setDepthTest(false);
  ssao_pipeline_->setDepthWrite(false);
  ssao_pipeline_->setSampleCount(1);
  ssao_pipeline_->setRenderPassDescriptor(ssao_render_pass_descriptor_);
  if (!ssao_pipeline_->create()) {
    return fail(tr("Could not create the browser SSAO pipeline"));
  }

  ssao_stage_.rejected_size = {};
  if (!ssao_stage_.capability_error.isEmpty()) {
    ssao_stage_.capability_error.clear();
    updateRenderingWarning();
  }
  ++ssao_stage_.resource_generation;
  return true;
}

bool SceneViewWidget::ensureEdlResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  if (edl_mesh_mask_render_target_ != nullptr && edl_mesh_mask_triangle_pipeline_ != nullptr &&
      edl_mesh_mask_line_pipeline_ != nullptr) {
    return true;
  }
  if (edl_stage_.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) {
    releaseEdlResources();
    edl_stage_.rejected_size = size;
    const bool changed = edl_stage_.capability_error != reason;
    edl_stage_.capability_error = std::move(reason);
    if (changed) {
      qWarning("SceneViewWidget WASM EDL unavailable: %s", qPrintable(edl_stage_.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (edl_stage_.force_fallback_for_test) {
    return fail(tr("EDL rendering disabled by the browser acceptance override"));
  }
  if (hdr_resolve_coverage_ == nullptr || hdr_resolve_depth_ == nullptr || model_triangle_pipeline_ == nullptr ||
      model_line_pipeline_ == nullptr || model_layout_shader_resources_ == nullptr) {
    return fail(tr("The browser HDR depth replay is not ready for EDL"));
  }

  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({QRhiColorAttachment(hdr_resolve_coverage_)});
  description.setDepthTexture(hdr_resolve_depth_);
  edl_mesh_mask_render_target_ = owner->newTextureRenderTarget(
      description,
      QRhiTextureRenderTarget::PreserveColorContents | QRhiTextureRenderTarget::PreserveDepthStencilContents);
  if (edl_mesh_mask_render_pass_descriptor_ == nullptr) {
    edl_mesh_mask_render_pass_descriptor_ = edl_mesh_mask_render_target_->newCompatibleRenderPassDescriptor();
  }
  edl_mesh_mask_render_target_->setRenderPassDescriptor(edl_mesh_mask_render_pass_descriptor_);
  if (!edl_mesh_mask_render_target_->create()) {
    return fail(tr("Could not preserve the browser depth replay for the EDL mesh mask"));
  }

  const QShader vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/model.vert.qsb"));
  const QShader fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/mesh_mask.frag.qsb"));
  if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
    return fail(tr("Could not load the browser EDL mesh-mask shaders"));
  }
  const auto create_pipeline = [owner, this, &vertex_shader, &fragment_shader](
                                   QRhiGraphicsPipeline*& destination, const QRhiGraphicsPipeline* source) {
    if (destination != nullptr) {
      return true;
    }
    destination = owner->newGraphicsPipeline();
    destination->setTopology(source->topology());
    destination->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader},
        {QRhiShaderStage::Fragment, fragment_shader},
    });
    destination->setVertexInputLayout(source->vertexInputLayout());
    destination->setShaderResourceBindings(model_layout_shader_resources_);
    QRhiGraphicsPipeline::TargetBlend mask_blend;
    mask_blend.enable = false;
    mask_blend.colorWrite = QRhiGraphicsPipeline::R;
    destination->setTargetBlends({mask_blend});
    destination->setCullMode(source->cullMode());
    destination->setFrontFace(source->frontFace());
    destination->setDepthTest(true);
    destination->setDepthWrite(false);
    destination->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    destination->setDepthBias(source->depthBias());
    destination->setSlopeScaledDepthBias(source->slopeScaledDepthBias());
    destination->setSampleCount(1);
    destination->setRenderPassDescriptor(edl_mesh_mask_render_pass_descriptor_);
    return destination->create();
  };
  if (!create_pipeline(edl_mesh_mask_triangle_pipeline_, model_triangle_pipeline_) ||
      !create_pipeline(edl_mesh_mask_line_pipeline_, model_line_pipeline_)) {
    return fail(tr("Could not create the browser EDL mesh-mask pipelines"));
  }

  edl_stage_.rejected_size = {};
  if (!edl_stage_.capability_error.isEmpty()) {
    edl_stage_.capability_error.clear();
    updateRenderingWarning();
  }
  ++edl_stage_.resource_generation;
  return true;
}

bool SceneViewWidget::ensureHdrPipelines(QRhi* owner) {
  if (owner == nullptr || hdr_render_pass_descriptor_ == nullptr || hdr_depth_render_pass_descriptor_ == nullptr) {
    return false;
  }
  const auto ensure_clone = [owner, this](QRhiGraphicsPipeline*& destination, QRhiGraphicsPipeline* source) {
    if (source == nullptr) {
      return true;
    }
    if (destination == nullptr) {
      destination = clonePipelineForRenderPass(owner, source, hdr_render_pass_descriptor_, kHdrSampleCount);
    }
    return destination != nullptr;
  };
  const bool color_ready =
      ensure_clone(hdr_line_pipeline_, line_pipeline_) &&
      (hdr_annotation_line_pipeline_ != nullptr ||
       (hdr_annotation_line_pipeline_ = clonePipelineForRenderPass(
            owner, line_pipeline_, hdr_render_pass_descriptor_, kHdrSampleCount, true)) != nullptr) &&
      ensure_clone(hdr_triangle_pipeline_, triangle_pipeline_) &&
      ensure_clone(hdr_line_no_depth_pipeline_, line_no_depth_pipeline_) &&
      ensure_clone(hdr_triangle_no_depth_pipeline_, triangle_no_depth_pipeline_) &&
      ensure_clone(hdr_point_pipeline_, point_pipeline_) && ensure_clone(hdr_cube_pipeline_, cube_pipeline_) &&
      ensure_clone(hdr_pose_pipeline_, pose_pipeline_) && ensure_clone(hdr_occupancy_pipeline_, occupancy_pipeline_) &&
      ensure_clone(hdr_voxel_pipeline_, voxel_pipeline_) &&
      ensure_clone(hdr_marker_triangle_pipeline_, marker_triangle_pipeline_) &&
      ensure_clone(hdr_marker_triangle_no_depth_pipeline_, marker_triangle_no_depth_pipeline_) &&
      ensure_clone(hdr_marker_triangle_cull_pipeline_, marker_triangle_cull_pipeline_) &&
      ensure_clone(hdr_marker_triangle_cull_no_depth_pipeline_, marker_triangle_cull_no_depth_pipeline_) &&
      ensure_clone(hdr_marker_line_pipeline_, marker_line_pipeline_) &&
      ensure_clone(hdr_marker_line_no_depth_pipeline_, marker_line_no_depth_pipeline_) &&
      ensure_clone(hdr_model_triangle_pipeline_, model_triangle_pipeline_) &&
      ensure_clone(hdr_model_triangle_no_depth_pipeline_, model_triangle_no_depth_pipeline_) &&
      ensure_clone(hdr_model_line_pipeline_, model_line_pipeline_) &&
      ensure_clone(hdr_model_line_no_depth_pipeline_, model_line_no_depth_pipeline_) &&
      ensure_clone(hdr_grid_shadow_pipeline_, grid_shadow_pipeline_);
  if (!color_ready) {
    return false;
  }
  const std::array<QRhiGraphicsPipeline*, 20> direct_pipelines = {
      line_pipeline_,
      triangle_pipeline_,
      line_no_depth_pipeline_,
      triangle_no_depth_pipeline_,
      point_pipeline_,
      cube_pipeline_,
      pose_pipeline_,
      occupancy_pipeline_,
      voxel_pipeline_,
      marker_triangle_pipeline_,
      marker_triangle_no_depth_pipeline_,
      marker_triangle_cull_pipeline_,
      marker_triangle_cull_no_depth_pipeline_,
      marker_line_pipeline_,
      marker_line_no_depth_pipeline_,
      model_triangle_pipeline_,
      model_triangle_no_depth_pipeline_,
      model_line_pipeline_,
      model_line_no_depth_pipeline_,
      grid_shadow_pipeline_,
  };
  for (QRhiGraphicsPipeline* source : direct_pipelines) {
    if (source == nullptr) {
      continue;
    }
    auto [iterator, inserted] = hdr_depth_pipelines_.try_emplace(source, nullptr);
    if (inserted || iterator->second == nullptr) {
      iterator->second = clonePipelineForRenderPass(owner, source, hdr_depth_render_pass_descriptor_, 1, false, true);
    }
    if (iterator->second == nullptr) {
      return false;
    }
  }
  return true;
}

bool SceneViewWidget::ensureHdrResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  if (hdr_render_target_ != nullptr && hdr_depth_render_target_ != nullptr && hdr_render_size_ == size &&
      present_shader_resources_ != nullptr && present_pipeline_ != nullptr && ensureHdrPipelines(owner)) {
    return true;
  }
  if (hdr_stage_.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) {
    releaseHdrResources();
    hdr_stage_.rejected_size = size;
    const bool changed = hdr_stage_.capability_error != reason;
    hdr_stage_.capability_error = std::move(reason);
    if (changed) {
      qWarning("SceneViewWidget WASM HDR unavailable: %s", qPrintable(hdr_stage_.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (hdr_stage_.force_fallback_for_test) {
    return fail(tr("HDR rendering disabled by the browser acceptance override"));
  }
  if (!owner->supportedSampleCounts().contains(kHdrSampleCount) ||
      !owner->isFeatureSupported(QRhi::MultisampleRenderBuffer)) {
    return fail(tr("This browser GPU does not support 4x multisample renderbuffers"));
  }
  if (!owner->isTextureFormatSupported(QRhiTexture::RGBA16F, QRhiTexture::RenderTarget) ||
      !owner->isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget) ||
      !owner->isTextureFormatSupported(QRhiTexture::RGBA8, QRhiTexture::RenderTarget)) {
    return fail(tr("This browser GPU does not support the RGBA16F/D32F/RGBA8 HDR attachments"));
  }
  const int texture_limit = owner->resourceLimit(QRhi::TextureSizeMax);
  if (texture_limit <= 0 || size.width() > texture_limit || size.height() > texture_limit) {
    return fail(tr("The %1 x %2 Scene3D view exceeds this browser GPU's %3-pixel texture limit")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(texture_limit));
  }

  const std::uint64_t pixels = static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
  const std::uint64_t bytes = pixels * kHdrBytesPerPixel;
  if (bytes > kMaxHdrBytesPerView) {
    return fail(tr("The %1 x %2 Scene3D HDR chain needs %3 MiB; the browser per-view limit is %4 MiB")
                    .arg(size.width())
                    .arg(size.height())
                    .arg((bytes + 1024U * 1024U - 1U) / (1024U * 1024U))
                    .arg(kMaxHdrBytesPerView / (1024U * 1024U)));
  }
  const auto aggregate_iterator = hdr_bytes_by_rhi.find(owner);
  const std::uint64_t aggregate = aggregate_iterator != hdr_bytes_by_rhi.end() ? aggregate_iterator->second : 0U;
  const std::uint64_t aggregate_without_this = aggregate > hdr_allocated_bytes_ ? aggregate - hdr_allocated_bytes_ : 0U;
  if (bytes > kMaxHdrBytesPerRhi - std::min(aggregate_without_this, kMaxHdrBytesPerRhi)) {
    return fail(tr("Live Scene3D HDR views would exceed the browser engine's %1 MiB limit")
                    .arg(kMaxHdrBytesPerRhi / (1024U * 1024U)));
  }

  releaseHdrTargetResources();
  if (hdr_color_sampler_ == nullptr) {
    hdr_color_sampler_ = owner->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);
    hdr_coverage_sampler_ = owner->newSampler(
        QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);
    composite_uniform_buffer_ =
        owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kCompositeUniformBytes);
    if (!hdr_color_sampler_->create() || !hdr_coverage_sampler_->create() || !composite_uniform_buffer_->create()) {
      return fail(tr("Could not allocate the browser HDR presentation bindings"));
    }
  }

  // Qt recommends multisample renderbuffers over multisample textures on GLES:
  // WebGL2 exposes the former in configurations where multisample textures are
  // unavailable. The backing-format hints are required so the resolves have
  // matching RGBA16F/D32F storage rather than QRhi's usual RGBA8/depth default.
  hdr_msaa_color_ = owner->newRenderBuffer(QRhiRenderBuffer::Color, size, kHdrSampleCount, {}, QRhiTexture::RGBA16F);
  hdr_msaa_depth_ =
      owner->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, kHdrSampleCount, {}, QRhiTexture::D32F);
  hdr_resolve_color_ = owner->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
  hdr_resolve_depth_ = owner->newTexture(QRhiTexture::D32F, size, 1, QRhiTexture::RenderTarget);
  hdr_resolve_coverage_ = owner->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
  if (!hdr_msaa_color_->create() || !hdr_msaa_depth_->create() || !hdr_resolve_color_->create() ||
      !hdr_resolve_depth_->create() || !hdr_resolve_coverage_->create()) {
    return fail(tr("Could not allocate the browser HDR color and depth-replay chain"));
  }

  QRhiColorAttachment color_attachment(hdr_msaa_color_);
  color_attachment.setResolveTexture(hdr_resolve_color_);
  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({color_attachment});
  description.setDepthStencilBuffer(hdr_msaa_depth_);
  hdr_render_target_ = owner->newTextureRenderTarget(description);
  if (hdr_render_pass_descriptor_ == nullptr) {
    hdr_render_pass_descriptor_ = hdr_render_target_->newCompatibleRenderPassDescriptor();
  }
  hdr_render_target_->setRenderPassDescriptor(hdr_render_pass_descriptor_);
  if (!hdr_render_target_->create()) {
    return fail(tr("Could not create the multisample browser HDR color target and resolve"));
  }

  QRhiTextureRenderTargetDescription depth_description;
  depth_description.setColorAttachments({QRhiColorAttachment(hdr_resolve_coverage_)});
  depth_description.setDepthTexture(hdr_resolve_depth_);
  hdr_depth_render_target_ = owner->newTextureRenderTarget(depth_description);
  if (hdr_depth_render_pass_descriptor_ == nullptr) {
    hdr_depth_render_pass_descriptor_ = hdr_depth_render_target_->newCompatibleRenderPassDescriptor();
  }
  hdr_depth_render_target_->setRenderPassDescriptor(hdr_depth_render_pass_descriptor_);
  if (!hdr_depth_render_target_->create()) {
    return fail(tr("Could not create the browser single-sample coverage/depth replay target"));
  }

  present_shader_resources_ = owner->newShaderResourceBindings();
  present_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, composite_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, hdr_resolve_color_, hdr_color_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          2, QRhiShaderResourceBinding::FragmentStage, hdr_resolve_coverage_, hdr_coverage_sampler_),
      QRhiShaderResourceBinding::sampledTexture(
          3, QRhiShaderResourceBinding::FragmentStage, hdr_resolve_depth_, hdr_coverage_sampler_),
  });
  if (!present_shader_resources_->create()) {
    return fail(tr("Could not create the browser HDR presentation bindings"));
  }

  if (present_pipeline_ == nullptr) {
    const QShader vertex_shader = loadShader(presentVertexShaderPath(*owner));
    const QShader fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/present.frag.qsb"));
    if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
      return fail(tr("Could not load the browser HDR presentation shaders"));
    }
    present_pipeline_ = owner->newGraphicsPipeline();
    present_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    present_pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader},
        {QRhiShaderStage::Fragment, fragment_shader},
    });
    present_pipeline_->setShaderResourceBindings(present_shader_resources_);
    present_pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    present_pipeline_->setDepthTest(false);
    present_pipeline_->setDepthWrite(false);
    present_pipeline_->setSampleCount(sampleCount());
    present_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!present_pipeline_->create()) {
      return fail(tr("Could not create the browser HDR presentation pipeline"));
    }
  }
  if (!ensureHdrPipelines(owner)) {
    return fail(tr("Could not create the browser HDR geometry pipelines"));
  }

  hdr_render_size_ = size;
  hdr_allocated_bytes_ = bytes;
  if (hdr_bytes_by_rhi.find(owner) == hdr_bytes_by_rhi.end()) {
    // The aggregate is keyed by a raw QRhi pointer that the allocator can
    // reuse: purge the entry when the QRhi itself dies so a widget whose
    // releaseResources() was skipped cannot poison a successor at the same
    // address with phantom bytes.
    owner->addCleanupCallback(&hdr_bytes_by_rhi, [](QRhi* dying) { hdr_bytes_by_rhi.erase(dying); });
  }
  hdr_bytes_by_rhi[owner] = aggregate_without_this + bytes;
  hdr_stage_.rejected_size = {};
  if (!hdr_stage_.capability_error.isEmpty()) {
    hdr_stage_.capability_error.clear();
    updateRenderingWarning();
  }
  ++hdr_stage_.resource_generation;
  return true;
}

}  // namespace pj::scene3d
