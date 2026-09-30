// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <QFile>
#include <QOpenGLContext>
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "passes/grid_rhi_pass.h"
#include "passes/marker_rhi_pass.h"
#include "passes/model_rhi_pass.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_core/shadow_camera.h"
#include "pj_scene3d_widgets/grid_geometry.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "rhi_scene_pass.h"
#include "scene_view_widget_rhi_quality_p.h"

namespace pj::scene3d {
namespace {

// Where QRhi resolves multisample depth (ResolveDepthStencil with multisample
// textures: OpenGL 4.3+, Vulkan, Metal), the HDR pass resolves its D32F into the
// single-sample depth texture and present derives coverage from it. Elsewhere
// (D3D11, WebGL2: no depth resolve, and WebGL's GLES QRhi does not resolve a
// second multisample color attachment reliably either) the bounded scene is
// replayed once into single-sample RGBA8 coverage + D32F depth.
//
// Bytes per pixel of a chain whose depth takes `depth_bytes` per sample:
// 4x (RGBA16F 8 + depth) + RGBA16F 8 + RGBA8 4 + depth. D32F (4 B): 64 B/px;
// OpenGL's resolve uses D32FS8 (8 B): 84 B/px.
constexpr std::uint64_t hdrBytesPerPixel(std::uint64_t depth_bytes) {
  return kHdrSampleCount * (8U + depth_bytes) + 8U + 4U + depth_bytes;
}
static_assert(hdrBytesPerPixel(4U) == 64U && hdrBytesPerPixel(8U) == 84U);
constexpr std::uint64_t kMaxHdrBytesPerView = kSceneLimits.max_hdr_bytes_per_view;
constexpr std::uint64_t kMaxHdrBytesPerRhi = kSceneLimits.max_hdr_bytes_per_rhi;

// Live Scene3D docks share one QRhi (every dock in the browser, every dock of a
// top-level window on desktop). Keep the retained offscreen chains below both a
// per-view and an aggregate per-QRhi ceiling.
std::unordered_map<QRhi*, std::uint64_t> hdr_bytes_by_rhi;

// Qt 6.11's OpenGL backend reports QRhi::MultisampleTexture from GL 3.0 (GLES
// 3.1) but allocates the texture with glTexStorage2DMultisample, which is core
// only from GL 4.3: on macOS's 4.1 core context that entry point is null and
// create() crashes. The OpenGL context is current inside render().
bool canCreateMultisampleTextures(QRhi& rhi) {
  if (!rhi.isFeatureSupported(QRhi::MultisampleTexture)) {
    return false;
  }
  if (rhi.backend() != QRhi::OpenGLES2) {
    return true;
  }
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return false;
  }
  const std::pair<int, int> version = context->format().version();
  return context->isOpenGLES()
             ? version >= std::pair{3, 1}
             : version >= std::pair{4, 3} || context->hasExtension("GL_ARB_texture_storage_multisample");
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

void SceneViewWidget::setForceDepthReplayForTest(bool force) {
  if (force_depth_replay_for_test_ == force) {
    return;
  }
  force_depth_replay_for_test_ = force;
  releaseHdrResources();  // the chains differ in their render-pass descriptors
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
  if (shadow_texture_ != nullptr && shadow_render_target_ != nullptr && grid_pass_->shadowReceiverReady() &&
      shadow_shader_resources_ != nullptr) {
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
    qWarning("SceneViewWidget QRhi shadows unavailable: %s", qPrintable(shadow_stage_.capability_error));
    releaseShadowResources(true);
    return false;
  };
  if (shadow_stage_.force_fallback_for_test) {
    return fail(tr("Shadow rendering disabled by the browser acceptance override"));
  }
  const int maximum_texture_size = owner->resourceLimit(QRhi::TextureSizeMax);
  if (maximum_texture_size < kShadowMapSize) {
    return fail(tr("A %1 x %1 shadow map exceeds this GPU's %2-pixel texture limit")
                    .arg(QString::number(kShadowMapSize), QString::number(maximum_texture_size)));
  }
  if (!owner->isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget)) {
    return fail(tr("This GPU does not support sampled D32F render targets"));
  }

  shadow_texture_ =
      owner->newTexture(QRhiTexture::D32F, QSize(kShadowMapSize, kShadowMapSize), 1, QRhiTexture::RenderTarget);
  if (shadow_texture_ == nullptr || !shadow_texture_->create()) {
    return fail(tr("Could not allocate the shadow depth texture"));
  }
  QRhiTextureRenderTargetDescription target_description;
  target_description.setDepthTexture(shadow_texture_);
  shadow_render_target_ = owner->newTextureRenderTarget(target_description);
  shadow_render_pass_descriptor_ = shadow_render_target_->newCompatibleRenderPassDescriptor();
  shadow_render_target_->setRenderPassDescriptor(shadow_render_pass_descriptor_);
  if (!shadow_render_target_->create()) {
    return fail(tr("Could not create the shadow render target"));
  }

  shadow_sampler_ = owner->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  shadow_uniform_buffer_ = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kShadowUniformBytes);
  if (shadow_sampler_ == nullptr || shadow_uniform_buffer_ == nullptr || !shadow_sampler_->create() ||
      !shadow_uniform_buffer_->create()) {
    return fail(tr("Could not allocate the shadow bindings"));
  }
  shadow_shader_resources_ = owner->newShaderResourceBindings();
  shadow_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, shadow_uniform_buffer_),
  });
  if (!shadow_shader_resources_->create()) {
    return fail(tr("Could not create the shadow resource bindings"));
  }

  // The caster pipeline of the model family, the one family that casts, once
  // built (createSceneResources() drops these resources when it is built later).
  if (model_pass_->ready() &&
      !model_pass_->ensureShadowPipeline(owner, shadow_render_pass_descriptor_, shadow_shader_resources_)) {
    return fail(tr("Could not create the shadow depth pipeline"));
  }

  if (!grid_pass_->createShadowReceiver(model_uniform_buffer_, shadow_texture_, shadow_sampler_, *pipelines_) ||
      (model_pass_->ready() && !model_pass_->setShadowMap(owner, shadow_texture_, shadow_sampler_))) {
    return fail(tr("Could not create the shadow receiver pipeline"));
  }
  shadow_map_size_ = kShadowMapSize;
  shadow_stage_.capability_error.clear();
  ++shadow_stage_.resource_generation;
  return true;
}

void SceneViewWidget::releaseShadowResources(bool restore_model_bindings) {
  grid_pass_->releaseShadowReceiver(*pipelines_);
  model_pass_->releaseShadowResources();
  delete shadow_shader_resources_;
  shadow_shader_resources_ = nullptr;

  QRhiTexture* released_texture = shadow_texture_;
  QRhiSampler* released_sampler = shadow_sampler_;
  shadow_texture_ = nullptr;
  shadow_sampler_ = nullptr;
  model_pass_->clearShadowMap(resource_rhi_, restore_model_bindings);

  delete shadow_render_target_;
  delete shadow_render_pass_descriptor_;
  delete shadow_uniform_buffer_;
  delete released_sampler;
  delete released_texture;
  shadow_render_target_ = nullptr;
  shadow_render_pass_descriptor_ = nullptr;
  shadow_uniform_buffer_ = nullptr;
  shadow_map_size_ = 0;
  shadow_stage_.last_active = false;
}

void SceneViewWidget::releaseHdrTargetResources() {
  // The present SRB owns references to both resolved textures. Destroy it
  // before the target and attachments. Pipelines (present included) only
  // retain the compatible render-pass descriptor and survive a size-only
  // rebuild, which binds a new layout-compatible SRB.
  releaseSsaoTargetResources();
  releaseEdlTargetResources();
  delete present_shader_resources_;
  delete hdr_depth_render_target_;
  delete hdr_render_target_;
  delete hdr_msaa_color_;
  delete hdr_msaa_depth_;
  delete hdr_msaa_depth_texture_;
  delete hdr_resolve_color_;
  delete hdr_resolve_depth_;
  delete hdr_resolve_coverage_;
  present_shader_resources_ = nullptr;
  hdr_depth_render_target_ = nullptr;
  hdr_render_target_ = nullptr;
  hdr_msaa_color_ = nullptr;
  hdr_msaa_depth_ = nullptr;
  hdr_msaa_depth_texture_ = nullptr;
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
  // The SRB retains replay depth: destroy every size-bound reference before the
  // HDR attachments. The pipeline only retains the SRB layout and the view-owned
  // descriptor, so it survives until releaseSsaoResources().
  delete ssao_shader_resources_;
  delete ssao_raw_render_target_;
  ssao_shader_resources_ = nullptr;
  ssao_raw_render_target_ = nullptr;
  ssao_stage_.last_active = false;
}

void SceneViewWidget::releaseSsaoResources() {
  releaseSsaoTargetResources();
  delete ssao_pipeline_;
  ssao_pipeline_ = nullptr;
  delete ssao_render_pass_descriptor_;
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
  const QString warning = render_failure_.failed()                  ? render_failure_.reason()
                          : !hdr_stage_.capability_error.isEmpty()  ? hdr_stage_.capability_error
                          : !overlay_failure_.isEmpty()             ? overlay_failure_
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
  pipelines_->releaseClones();
  delete present_pipeline_;
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
      qWarning("SceneViewWidget QRhi SSAO unavailable: %s", qPrintable(ssao_stage_.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (ssao_stage_.force_fallback_for_test) {
    return fail(tr("SSAO rendering disabled by the browser acceptance override"));
  }
  if (hdr_resolve_coverage_ == nullptr || hdr_resolve_depth_ == nullptr || hdr_coverage_sampler_ == nullptr) {
    return fail(tr("The HDR depth replay is not ready for SSAO"));
  }

  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({QRhiColorAttachment(hdr_resolve_coverage_)});
  ssao_raw_render_target_ = owner->newTextureRenderTarget(description, QRhiTextureRenderTarget::PreserveColorContents);
  if (ssao_render_pass_descriptor_ == nullptr) {
    ssao_render_pass_descriptor_ = ssao_raw_render_target_->newCompatibleRenderPassDescriptor();
  }
  ssao_raw_render_target_->setRenderPassDescriptor(ssao_render_pass_descriptor_);
  if (!ssao_raw_render_target_->create()) {
    return fail(tr("Could not preserve the replay coverage for SSAO"));
  }

  ssao_shader_resources_ = owner->newShaderResourceBindings();
  ssao_shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, ssao_uniform_buffer_),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, hdr_resolve_depth_, hdr_coverage_sampler_),
  });
  if (!ssao_shader_resources_->create()) {
    return fail(tr("Could not create the SSAO bindings"));
  }

  if (ssao_pipeline_ != nullptr) {
    ssao_pipeline_->setShaderResourceBindings(ssao_shader_resources_);
  } else {
    QRhiGraphicsPipeline::TargetBlend raw_ao_write;
    raw_ao_write.colorWrite = QRhiGraphicsPipeline::G;
    ssao_pipeline_ = createScenePipeline(
        {owner, ssao_render_pass_descriptor_, 1},
        {
            .vertex = loadRhiShader(presentVertexShaderPath(*owner)),
            .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/ssao.frag.qsb")),
            .bindings = ssao_shader_resources_,
            .blend = raw_ao_write,
            .depth_test = false,
            .depth_write = false,
        });
    if (ssao_pipeline_ == nullptr) {
      return fail(tr("Could not create the SSAO pipeline"));
    }
  }

  ssao_stage_.rejected_size = {};
  if (!ssao_stage_.capability_error.isEmpty()) {
    ssao_stage_.capability_error.clear();
    updateRenderingWarning();
  }
  ++ssao_stage_.resource_generation;
  return true;
}

bool SceneViewWidget::createEdlMaskPipelines(QRhi* owner) {
  if (!model_pass_->ready()) {
    return true;
  }
  const QShader vertex_shader = loadRhiShader(QStringLiteral(":/scene3d_rhi/model.vert.qsb"));
  const QShader fragment_shader = loadRhiShader(QStringLiteral(":/scene3d_rhi/mesh_mask.frag.qsb"));
  if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
    return false;
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
    destination->setShaderResourceBindings(model_pass_->layoutBindings());
    QRhiGraphicsPipeline::TargetBlend mask_blend;
    mask_blend.enable = false;
    mask_blend.colorWrite = QRhiGraphicsPipeline::R;
    destination->setTargetBlends({mask_blend});
    destination->setCullMode(source->cullMode());
    destination->setFrontFace(source->frontFace());
    destination->setDepthTest(true);
    destination->setDepthWrite(false);
    destination->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    // A resolved depth holds one sample of the 4x pattern, off the pixel centre
    // where the mask rasterizes: pull the mask up to a pixel's slope closer so a
    // sloped mesh still passes against its own depth.
    const int resolve_bias = hdr_depth_resolve_ ? -1 : 0;
    destination->setDepthBias(source->depthBias() + resolve_bias);
    destination->setSlopeScaledDepthBias(source->slopeScaledDepthBias() + static_cast<float>(resolve_bias));
    destination->setSampleCount(1);
    destination->setRenderPassDescriptor(edl_mesh_mask_render_pass_descriptor_);
    return createGraphicsPipeline(destination);
  };
  return create_pipeline(edl_mesh_mask_triangle_pipeline_, model_pass_->trianglePipeline()) &&
         create_pipeline(edl_mesh_mask_line_pipeline_, model_pass_->linePipeline());
}

bool SceneViewWidget::ensureEdlResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  // The mesh mask needs the model family's pipelines; without it (no model
  // layer) the mask pass would record nothing, and is skipped.
  const bool masks_meshes = model_pass_->ready();
  if (edl_mesh_mask_render_target_ != nullptr &&
      (!masks_meshes || (edl_mesh_mask_triangle_pipeline_ != nullptr && edl_mesh_mask_line_pipeline_ != nullptr))) {
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
      qWarning("SceneViewWidget QRhi EDL unavailable: %s", qPrintable(edl_stage_.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (edl_stage_.force_fallback_for_test) {
    return fail(tr("EDL rendering disabled by the browser acceptance override"));
  }
  if (hdr_resolve_coverage_ == nullptr || hdr_resolve_depth_ == nullptr ||
      (masks_meshes && (model_pass_->trianglePipeline() == nullptr || model_pass_->linePipeline() == nullptr ||
                        model_pass_->layoutBindings() == nullptr))) {
    return fail(tr("The HDR depth replay is not ready for EDL"));
  }
  if (edl_mesh_mask_render_target_ != nullptr) {
    // Built before the model family was: only the mask pipelines are missing.
    return createEdlMaskPipelines(owner) || fail(tr("Could not create the EDL mesh-mask pipelines"));
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
    return fail(tr("Could not preserve the depth replay for the EDL mesh mask"));
  }

  if (!createEdlMaskPipelines(owner)) {
    return fail(tr("Could not create the EDL mesh-mask pipelines"));
  }

  edl_stage_.rejected_size = {};
  if (!edl_stage_.capability_error.isEmpty()) {
    edl_stage_.capability_error.clear();
    updateRenderingWarning();
  }
  ++edl_stage_.resource_generation;
  return true;
}

bool SceneViewWidget::rejectHdr(const QSize& size, QString reason) {
  releaseHdrResources();
  hdr_stage_.rejected_size = size;
  const bool changed = hdr_stage_.capability_error != reason;
  hdr_stage_.capability_error = std::move(reason);
  if (changed) {
    qWarning("SceneViewWidget QRhi HDR unavailable: %s", qPrintable(hdr_stage_.capability_error));
  }
  updateRenderingWarning();
  return false;
}

QString SceneViewWidget::hdrUnsupportedReason(QRhi& rhi) {
  if (!rhi.supportedSampleCounts().contains(kHdrSampleCount) ||
      !rhi.isFeatureSupported(QRhi::MultisampleRenderBuffer)) {
    return tr("This GPU does not support 4x multisample renderbuffers");
  }
  if (!rhi.isTextureFormatSupported(QRhiTexture::RGBA16F, QRhiTexture::RenderTarget) ||
      !rhi.isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget) ||
      !rhi.isTextureFormatSupported(QRhiTexture::RGBA8, QRhiTexture::RenderTarget)) {
    return tr("This GPU does not support the RGBA16F/D32F/RGBA8 HDR attachments");
  }
  return {};
}

bool SceneViewWidget::ensureHdrResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  if (hdr_render_target_ != nullptr && hdr_depth_render_target_ != nullptr && hdr_render_size_ == size &&
      present_shader_resources_ != nullptr && present_pipeline_ != nullptr) {
    return true;
  }
  if (hdr_stage_.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) { return rejectHdr(size, std::move(reason)); };

  if (hdr_stage_.force_fallback_for_test) {
    return fail(tr("HDR rendering disabled by the browser acceptance override"));
  }
  if (QString unsupported = hdrUnsupportedReason(*owner); !unsupported.isEmpty()) {
    return fail(std::move(unsupported));
  }
  const int texture_limit = owner->resourceLimit(QRhi::TextureSizeMax);
  if (texture_limit <= 0 || size.width() > texture_limit || size.height() > texture_limit) {
    return fail(tr("The %1 x %2 Scene3D view exceeds this GPU's %3-pixel texture limit")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(texture_limit));
  }

  // The depth chain decides the charge, so it is picked before admission.
  // Qt 6.11's OpenGL resolve blits depth AND stencil (it attaches the textures
  // as both), which leaves a stencil-less D32F framebuffer incomplete and the
  // resolved depth unwritten: OpenGL resolves D32FS8. The replay uses D32F.
  const QRhiTexture::Format resolve_format =
      owner->backend() == QRhi::OpenGLES2 ? QRhiTexture::D32FS8 : QRhiTexture::D32F;
  const bool depth_resolve = !force_depth_replay_for_test_ && owner->isFeatureSupported(QRhi::ResolveDepthStencil) &&
                             canCreateMultisampleTextures(*owner) &&
                             owner->isTextureFormatSupported(resolve_format, QRhiTexture::RenderTarget);
  const QRhiTexture::Format depth_format = depth_resolve ? resolve_format : QRhiTexture::D32F;
  const std::uint64_t pixels = static_cast<std::uint64_t>(size.width()) * static_cast<std::uint64_t>(size.height());
  const std::uint64_t bytes = pixels * hdrBytesPerPixel(depth_format == QRhiTexture::D32FS8 ? 8U : 4U);
  if (bytes > kMaxHdrBytesPerView) {
    return fail(tr("The %1 x %2 Scene3D HDR chain needs %3 MiB; this view's limit is %4 MiB")
                    .arg(size.width())
                    .arg(size.height())
                    .arg((bytes + 1024U * 1024U - 1U) / (1024U * 1024U))
                    .arg(kMaxHdrBytesPerView / (1024U * 1024U)));
  }
  const auto aggregate_iterator = hdr_bytes_by_rhi.find(owner);
  const std::uint64_t aggregate = aggregate_iterator != hdr_bytes_by_rhi.end() ? aggregate_iterator->second : 0U;
  const std::uint64_t aggregate_without_this = aggregate > hdr_allocated_bytes_ ? aggregate - hdr_allocated_bytes_ : 0U;
  if (bytes > kMaxHdrBytesPerRhi - std::min(aggregate_without_this, kMaxHdrBytesPerRhi)) {
    return fail(tr("Live Scene3D HDR views would exceed their shared %1 MiB GPU limit")
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
      return fail(tr("Could not allocate the HDR presentation bindings"));
    }
  }

  // Qt recommends multisample renderbuffers over multisample textures on GLES:
  // WebGL2 exposes the former in configurations where multisample textures are
  // unavailable. The backing-format hints are required so the resolves have
  // matching RGBA16F/D32F storage rather than QRhi's usual RGBA8/depth default.
  hdr_msaa_color_ = owner->newRenderBuffer(QRhiRenderBuffer::Color, size, kHdrSampleCount, {}, QRhiTexture::RGBA16F);
  // The depth resolve needs a multisample depth texture; the replay a renderbuffer.
  hdr_depth_resolve_ = depth_resolve;
  hdr_resolve_color_ = owner->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
  hdr_resolve_depth_ = owner->newTexture(depth_format, size, 1, QRhiTexture::RenderTarget);
  hdr_resolve_coverage_ = owner->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
  if (!hdr_msaa_color_->create() || !hdr_resolve_color_->create() || !hdr_resolve_depth_->create() ||
      !hdr_resolve_coverage_->create()) {
    return fail(tr("Could not allocate the HDR color and depth-replay chain"));
  }
  if (hdr_depth_resolve_) {
    hdr_msaa_depth_texture_ = owner->newTexture(depth_format, size, kHdrSampleCount, QRhiTexture::RenderTarget);
  } else {
    hdr_msaa_depth_ =
        owner->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, kHdrSampleCount, {}, QRhiTexture::D32F);
  }
  if (hdr_depth_resolve_ ? !hdr_msaa_depth_texture_->create() : !hdr_msaa_depth_->create()) {
    return fail(tr("Could not allocate the HDR color and depth-replay chain"));
  }

  QRhiColorAttachment color_attachment(hdr_msaa_color_);
  color_attachment.setResolveTexture(hdr_resolve_color_);
  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({color_attachment});
  if (hdr_depth_resolve_) {
    description.setDepthTexture(hdr_msaa_depth_texture_);
    description.setDepthResolveTexture(hdr_resolve_depth_);
  } else {
    description.setDepthStencilBuffer(hdr_msaa_depth_);
  }
  hdr_render_target_ = owner->newTextureRenderTarget(description);
  if (hdr_render_pass_descriptor_ == nullptr) {
    hdr_render_pass_descriptor_ = hdr_render_target_->newCompatibleRenderPassDescriptor();
  }
  hdr_render_target_->setRenderPassDescriptor(hdr_render_pass_descriptor_);
  if (!hdr_render_target_->create()) {
    return fail(tr("Could not create the multisample HDR color target and resolve"));
  }

  QRhiTextureRenderTargetDescription depth_description;
  depth_description.setColorAttachments({QRhiColorAttachment(hdr_resolve_coverage_)});
  if (!hdr_depth_resolve_) {
    depth_description.setDepthTexture(hdr_resolve_depth_);
  }
  hdr_depth_render_target_ = owner->newTextureRenderTarget(depth_description);
  if (hdr_depth_render_pass_descriptor_ == nullptr) {
    hdr_depth_render_pass_descriptor_ = hdr_depth_render_target_->newCompatibleRenderPassDescriptor();
  }
  hdr_depth_render_target_->setRenderPassDescriptor(hdr_depth_render_pass_descriptor_);
  if (!hdr_depth_render_target_->create()) {
    return fail(tr("Could not create the single-sample coverage/depth replay target"));
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
    return fail(tr("Could not create the HDR presentation bindings"));
  }

  // QRhiWidget keeps its render-pass descriptor across resizes; a new one
  // (format/sample-count change) needs a new pipeline.
  if (present_pipeline_ != nullptr &&
      present_pipeline_->renderPassDescriptor() != renderTarget()->renderPassDescriptor()) {
    delete present_pipeline_;
    present_pipeline_ = nullptr;
  }
  if (present_pipeline_ != nullptr) {
    present_pipeline_->setShaderResourceBindings(present_shader_resources_);
  } else {
    present_pipeline_ = createScenePipeline(
        {owner, renderTarget()->renderPassDescriptor(), sampleCount()},
        {
            .vertex = loadRhiShader(presentVertexShaderPath(*owner)),
            .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/present.frag.qsb")),
            .bindings = present_shader_resources_,
            .blend = std::nullopt,
            .depth_test = false,
            .depth_write = false,
        });
    if (present_pipeline_ == nullptr) {
      return fail(tr("Could not create the HDR presentation pipeline"));
    }
  }
  // The scene pipelines' HDR and depth-replay clones are built on first use.
  pipelines_->setCloneTargets(owner, hdr_render_pass_descriptor_, kHdrSampleCount, hdr_depth_render_pass_descriptor_);

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
