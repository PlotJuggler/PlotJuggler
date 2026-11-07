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
#include "pj_widgets/RhiTestHooks.h"
#include "rhi_scene_pass.h"
#include "scene_view_widget_rhi_quality_p.h"

namespace pj::scene3d {
namespace {

// The capability error of a stage held down by SceneViewTestAccess::forceStageFallback():
// a test override, never shown to a user, so untranslated. The browser
// acceptance specs match its wording.
QString forcedOffReason(const char* stage) {
  return QStringLiteral("%1 rendering disabled by the browser acceptance override").arg(QLatin1StringView(stage));
}

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

std::uint64_t HdrResources::chargedBytes(QRhi* rhi) {
  const auto iterator = hdr_bytes_by_rhi.find(rhi);
  return iterator != hdr_bytes_by_rhi.end() ? iterator->second : 0U;
}

void HdrResources::charge(QRhi* rhi, std::uint64_t bytes) {
  Q_ASSERT(allocated_bytes == 0U);  // releaseTargets() gave the previous charge back
  if (hdr_bytes_by_rhi.find(rhi) == hdr_bytes_by_rhi.end()) {
    // The aggregate is keyed by a raw QRhi pointer that the allocator can
    // reuse: purge the entry when the QRhi itself dies so a widget whose
    // releaseResources() was skipped cannot poison a successor at the same
    // address with phantom bytes.
    rhi->addCleanupCallback(&hdr_bytes_by_rhi, [](QRhi* dying) { hdr_bytes_by_rhi.erase(dying); });
  }
  hdr_bytes_by_rhi[rhi] += bytes;
  charged_rhi = rhi;
  allocated_bytes = bytes;
}

void HdrResources::releaseTargets() {
  present_bindings.reset();
  depth_target.reset();
  target.reset();
  msaa_color.reset();
  msaa_depth.reset();
  msaa_depth_texture.reset();
  resolve_color.reset();
  resolve_depth.reset();
  resolve_coverage.reset();
  size = {};
  state.last_active = false;
  if (charged_rhi != nullptr && allocated_bytes != 0U) {
    if (auto iterator = hdr_bytes_by_rhi.find(charged_rhi); iterator != hdr_bytes_by_rhi.end()) {
      iterator->second = iterator->second > allocated_bytes ? iterator->second - allocated_bytes : 0U;
      if (iterator->second == 0U) {
        hdr_bytes_by_rhi.erase(iterator);
      }
    }
  }
  charged_rhi = nullptr;
  allocated_bytes = 0U;
}

QualityStageState& SceneViewWidget::stageState(QualityStage stage) const {
  switch (stage) {
    case QualityStage::kHdr:
      return hdr_->state;
    case QualityStage::kSsao:
      return ssao_->state;
    case QualityStage::kEdl:
      return edl_->state;
    case QualityStage::kShadow:
      break;
  }
  return shadow_->state;
}

void SceneViewWidget::forceStageFallback(QualityStage stage, bool force) {
  QualityStageState& state = stageState(stage);
  if (state.forced_off == force) {
    return;
  }
  state.forced_off = force;
  state.rejected_size = {};
  if (stage == QualityStage::kShadow) {
    state.capability_error.clear();
  }
  if (force) {
    switch (stage) {
      case QualityStage::kHdr:
        releaseHdrResources();
        break;
      case QualityStage::kSsao:
        ssao_->release();
        break;
      case QualityStage::kEdl:
        releaseEdlResources();
        break;
      case QualityStage::kShadow:
        releaseShadowResources(true);
        break;
    }
  }
  invalidateContent();
}

bool SceneViewWidget::ensureShadowResources(QRhi* owner) {
  ShadowResources& shadow = *shadow_;
  const auto fail = [this](QString reason) {
    shadow_->state.capability_error = std::move(reason);
    shadow_->state.rejected_size = renderTarget() != nullptr ? renderTarget()->pixelSize() : QSize{};
    qWarning("SceneViewWidget QRhi shadows unavailable: %s", qPrintable(shadow_->state.capability_error));
    releaseShadowResources(true);
    return false;
  };
  // Known once a frame bound it: that frame drew the cells unshadowed.
  if (grid_pass_->shadowReceiverFailed(*pipelines_)) {
    return fail(tr("Could not create the shadow receiver pipeline"));
  }
  if (shadow.texture != nullptr && shadow.target != nullptr && grid_pass_->shadowReceiverReady() &&
      shadow.bindings != nullptr) {
    return true;
  }
  if (owner == nullptr || renderTarget() == nullptr || uniform_buffer_ == nullptr || model_uniform_buffer_ == nullptr) {
    return false;
  }
  // A failure blocks retries only while the output size is unchanged — a
  // resize re-probes, mirroring the HDR/SSAO/EDL rejected-size recovery.
  if (shadow_->state.blockedFor(renderTarget()->pixelSize())) {
    return false;
  }
  if (shadow_->state.forced_off) {
    return fail(forcedOffReason("Shadow"));
  }
  const int maximum_texture_size = owner->resourceLimit(QRhi::TextureSizeMax);
  if (maximum_texture_size < kShadowMapSize) {
    return fail(tr("A %1 x %1 shadow map exceeds this GPU's %2-pixel texture limit")
                    .arg(QString::number(kShadowMapSize), QString::number(maximum_texture_size)));
  }
  if (!owner->isTextureFormatSupported(QRhiTexture::D32F, QRhiTexture::RenderTarget)) {
    return fail(tr("This GPU does not support sampled D32F render targets"));
  }

  shadow.texture.reset(
      owner->newTexture(QRhiTexture::D32F, QSize(kShadowMapSize, kShadowMapSize), 1, QRhiTexture::RenderTarget));
  if (shadow.texture == nullptr || !shadow.texture->create()) {
    return fail(tr("Could not allocate the shadow depth texture"));
  }
  QRhiTextureRenderTargetDescription target_description;
  target_description.setDepthTexture(shadow.texture.get());
  shadow.target.reset(owner->newTextureRenderTarget(target_description));
  shadow.render_pass.reset(shadow.target->newCompatibleRenderPassDescriptor());
  shadow.target->setRenderPassDescriptor(shadow.render_pass.get());
  if (PJ::rhiTestFault(*this, "shadow target") || !shadow.target->create()) {
    return fail(tr("Could not create the shadow render target"));
  }

  shadow.sampler.reset(owner->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge));
  shadow.uniforms.reset(owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kShadowUniformBytes));
  if (shadow.sampler == nullptr || shadow.uniforms == nullptr || !shadow.sampler->create() ||
      !shadow.uniforms->create()) {
    return fail(tr("Could not allocate the shadow bindings"));
  }
  shadow.bindings.reset(createBindings(
      owner,
      {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, shadow.uniforms.get())}));
  if (shadow.bindings == nullptr) {
    return fail(tr("Could not create the shadow resource bindings"));
  }

  // The caster pipeline of the model family, the one family that casts, once
  // built (createSceneResources() drops these resources when it is built later).
  if (model_pass_->ready() &&
      !model_pass_->ensureShadowPipeline(owner, shadow.render_pass.get(), shadow.bindings.get())) {
    return fail(tr("Could not create the shadow depth pipeline"));
  }

  if (!grid_pass_->createShadowReceiver(
          model_uniform_buffer_, shadow.texture.get(), shadow.sampler.get(), *pipelines_) ||
      (model_pass_->ready() && !model_pass_->setShadowMap(owner, shadow.texture.get(), shadow.sampler.get()))) {
    return fail(tr("Could not create the shadow receiver pipeline"));
  }
  shadow_->state.capability_error.clear();
  ++shadow_->state.resource_generation;
  return true;
}

void SceneViewWidget::releaseShadowResources(bool restore_model_bindings) {
  // What binds the map first: the grid's receiver, the model pass's caster
  // pipeline and its materials (rebound to white, or dropped).
  grid_pass_->releaseShadowReceiver(*pipelines_);
  model_pass_->releaseShadowResources();
  model_pass_->clearShadowMap(resource_rhi_, restore_model_bindings);
  shadow_->release();
}

void SceneViewWidget::releaseHdrTargetResources() {
  // SSAO's bindings and target and EDL's target reference the HDR attachments.
  ssao_->releaseTargets();
  edl_->releaseTargets();
  hdr_->releaseTargets();
}

void SceneViewWidget::releaseEdlResources() {
  // The model pass's mask pipelines target the mask's render-pass descriptor.
  model_pass_->releaseMeshMaskPipelines();
  edl_->release();
}

void SceneViewWidget::updateRenderingWarning() {
  // SSAO/EDL are probed only while the HDR chain runs, so their stored errors
  // go stale the moment HDR is down — suppress them until HDR is live again.
  const QString warning = render_failure_.failed()                   ? render_failure_.reason()
                          : !hdr_->state.capability_error.isEmpty()  ? hdr_->state.capability_error
                          : !overlay_failure_.isEmpty()              ? overlay_failure_
                          : !hdr_->state.last_active                 ? QString{}
                          : !ssao_->state.capability_error.isEmpty() ? ssao_->state.capability_error
                                                                     : edl_->state.capability_error;
  if (warning == rendering_warning_) {
    return;
  }
  rendering_warning_ = warning;
  emit renderingWarningChanged(rendering_warning_);
}

void SceneViewWidget::releaseHdrResources() {
  // SSAO, EDL and the registry's HDR variants bind the chain's render passes
  // and attachments: released first.
  ssao_->release();
  releaseEdlResources();
  pipelines_->releaseVariants();
  hdr_->release();
}

bool SceneViewWidget::ensureSsaoResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  SsaoResources& ssao = *ssao_;
  if (ssao.target != nullptr && ssao.pipeline != nullptr && ssao.bindings != nullptr &&
      ssao_uniform_buffer_ != nullptr) {
    return true;
  }
  if (ssao_->state.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) {
    ssao_->release();
    ssao_->state.rejected_size = size;
    const bool changed = ssao_->state.capability_error != reason;
    ssao_->state.capability_error = std::move(reason);
    if (changed) {
      qWarning("SceneViewWidget QRhi SSAO unavailable: %s", qPrintable(ssao_->state.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (ssao_->state.forced_off) {
    return fail(forcedOffReason("SSAO"));
  }
  if (hdr_->resolve_coverage == nullptr || hdr_->resolve_depth == nullptr || hdr_->coverage_sampler == nullptr) {
    return fail(tr("The HDR depth replay is not ready for SSAO"));
  }

  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({QRhiColorAttachment(hdr_->resolve_coverage.get())});
  ssao.target.reset(owner->newTextureRenderTarget(description, QRhiTextureRenderTarget::PreserveColorContents));
  if (ssao.render_pass == nullptr) {
    ssao.render_pass.reset(ssao.target->newCompatibleRenderPassDescriptor());
  }
  ssao.target->setRenderPassDescriptor(ssao.render_pass.get());
  if (PJ::rhiTestFault(*this, "ssao target") || !ssao.target->create()) {
    return fail(tr("Could not preserve the replay coverage for SSAO"));
  }

  ssao.bindings.reset(createBindings(
      owner,
      {
          QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, ssao_uniform_buffer_),
          QRhiShaderResourceBinding::sampledTexture(
              1, QRhiShaderResourceBinding::FragmentStage, hdr_->resolve_depth.get(), hdr_->coverage_sampler.get()),
      }));
  if (ssao.bindings == nullptr) {
    return fail(tr("Could not create the SSAO bindings"));
  }

  if (ssao.pipeline != nullptr) {
    ssao.pipeline->setShaderResourceBindings(ssao.bindings.get());
  } else {
    QRhiGraphicsPipeline::TargetBlend raw_ao_write;
    raw_ao_write.colorWrite = QRhiGraphicsPipeline::G;
    ssao.pipeline = createPipeline(
        owner,
        {
            .vertex = loadRhiShader(presentVertexShaderPath(*owner)),
            .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/ssao.frag.qsb")),
            .bindings = ssao.bindings.get(),
            .blend = raw_ao_write,
            .depth_test = false,
            .depth_write = false,
        },
        ssao.render_pass.get(), 1);
    if (ssao.pipeline == nullptr) {
      return fail(tr("Could not create the SSAO pipeline"));
    }
  }

  ssao_->state.rejected_size = {};
  if (!ssao_->state.capability_error.isEmpty()) {
    ssao_->state.capability_error.clear();
    updateRenderingWarning();
  }
  ++ssao_->state.resource_generation;
  return true;
}

bool SceneViewWidget::ensureEdlResources(QRhi* owner, const QSize& size) {
  if (owner == nullptr || !size.isValid() || size.isEmpty()) {
    return false;
  }
  EdlResources& edl = *edl_;
  // The mesh mask needs the model family; without it (no model layer) the mask
  // pass would record nothing, and is skipped.
  const bool masks_meshes = model_pass_->ready();
  if (edl.target != nullptr && (!masks_meshes || model_pass_->meshMaskReady())) {
    return true;
  }
  if (edl_->state.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) {
    releaseEdlResources();
    edl_->state.rejected_size = size;
    const bool changed = edl_->state.capability_error != reason;
    edl_->state.capability_error = std::move(reason);
    if (changed) {
      qWarning("SceneViewWidget QRhi EDL unavailable: %s", qPrintable(edl_->state.capability_error));
    }
    updateRenderingWarning();
    return false;
  };

  if (edl_->state.forced_off) {
    return fail(forcedOffReason("EDL"));
  }
  if (hdr_->resolve_coverage == nullptr || hdr_->resolve_depth == nullptr) {
    return fail(tr("The HDR depth replay is not ready for EDL"));
  }
  // The view supplies the mask's target and depth chain; the model pass builds
  // its own mask pipelines for them.
  const auto create_mask_pipelines = [&] {
    return !masks_meshes || model_pass_->ensureMeshMaskPipelines(owner, edl.render_pass.get(), hdr_->depth_resolve);
  };
  if (edl.target != nullptr) {
    // Built before the model family was: only the mask pipelines are missing.
    return create_mask_pipelines() || fail(tr("Could not create the EDL mesh-mask pipelines"));
  }

  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({QRhiColorAttachment(hdr_->resolve_coverage.get())});
  description.setDepthTexture(hdr_->resolve_depth.get());
  edl.target.reset(owner->newTextureRenderTarget(
      description,
      QRhiTextureRenderTarget::PreserveColorContents | QRhiTextureRenderTarget::PreserveDepthStencilContents));
  if (edl.render_pass == nullptr) {
    edl.render_pass.reset(edl.target->newCompatibleRenderPassDescriptor());
  }
  edl.target->setRenderPassDescriptor(edl.render_pass.get());
  if (PJ::rhiTestFault(*this, "edl target") || !edl.target->create()) {
    return fail(tr("Could not preserve the depth replay for the EDL mesh mask"));
  }

  if (!create_mask_pipelines()) {
    return fail(tr("Could not create the EDL mesh-mask pipelines"));
  }

  edl_->state.rejected_size = {};
  if (!edl_->state.capability_error.isEmpty()) {
    edl_->state.capability_error.clear();
    updateRenderingWarning();
  }
  ++edl_->state.resource_generation;
  return true;
}

bool SceneViewWidget::rejectHdr(const QSize& size, QString reason) {
  releaseHdrResources();
  hdr_->state.rejected_size = size;
  const bool changed = hdr_->state.capability_error != reason;
  hdr_->state.capability_error = std::move(reason);
  if (changed) {
    qWarning("SceneViewWidget QRhi HDR unavailable: %s", qPrintable(hdr_->state.capability_error));
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
  HdrResources& hdr = *hdr_;
  if (hdr.target != nullptr && hdr.depth_target != nullptr && hdr.size == size && hdr.present_bindings != nullptr &&
      hdr.present_pipeline != nullptr) {
    return true;
  }
  if (hdr_->state.blockedFor(size)) {
    return false;
  }

  const auto fail = [this, &size](QString reason) { return rejectHdr(size, std::move(reason)); };

  if (hdr_->state.forced_off) {
    return fail(forcedOffReason("HDR"));
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
  const std::uint64_t aggregate = HdrResources::chargedBytes(owner);
  const std::uint64_t aggregate_without_this = aggregate > hdr.allocated_bytes ? aggregate - hdr.allocated_bytes : 0U;
  if (bytes > kMaxHdrBytesPerRhi - std::min(aggregate_without_this, kMaxHdrBytesPerRhi)) {
    return fail(tr("Live Scene3D HDR views would exceed their shared %1 MiB GPU limit")
                    .arg(kMaxHdrBytesPerRhi / (1024U * 1024U)));
  }

  releaseHdrTargetResources();
  if (hdr.color_sampler == nullptr) {
    hdr.color_sampler.reset(owner->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge));
    hdr.coverage_sampler.reset(owner->newSampler(
        QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge));
    hdr.composite_uniforms.reset(
        owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kCompositeUniformBytes));
    if (!hdr.color_sampler->create() || !hdr.coverage_sampler->create() || !hdr.composite_uniforms->create()) {
      return fail(tr("Could not allocate the HDR presentation bindings"));
    }
  }

  // Qt recommends multisample renderbuffers over multisample textures on GLES:
  // WebGL2 exposes the former in configurations where multisample textures are
  // unavailable. The backing-format hints are required so the resolves have
  // matching RGBA16F/D32F storage rather than QRhi's usual RGBA8/depth default.
  hdr.msaa_color.reset(
      owner->newRenderBuffer(QRhiRenderBuffer::Color, size, kHdrSampleCount, {}, QRhiTexture::RGBA16F));
  // The depth resolve needs a multisample depth texture; the replay a renderbuffer.
  hdr.depth_resolve = depth_resolve;
  hdr.resolve_color.reset(owner->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget));
  hdr.resolve_depth.reset(owner->newTexture(depth_format, size, 1, QRhiTexture::RenderTarget));
  hdr.resolve_coverage.reset(owner->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget));
  if (!hdr.msaa_color->create() || !hdr.resolve_color->create() || !hdr.resolve_depth->create() ||
      !hdr.resolve_coverage->create()) {
    return fail(tr("Could not allocate the HDR color and depth-replay chain"));
  }
  if (hdr.depth_resolve) {
    hdr.msaa_depth_texture.reset(owner->newTexture(depth_format, size, kHdrSampleCount, QRhiTexture::RenderTarget));
  } else {
    hdr.msaa_depth.reset(
        owner->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, kHdrSampleCount, {}, QRhiTexture::D32F));
  }
  if (PJ::rhiTestFault(*this, "hdr attachments") ||
      (hdr.depth_resolve ? !hdr.msaa_depth_texture->create() : !hdr.msaa_depth->create())) {
    return fail(tr("Could not allocate the HDR color and depth-replay chain"));
  }

  QRhiColorAttachment color_attachment(hdr.msaa_color.get());
  color_attachment.setResolveTexture(hdr.resolve_color.get());
  QRhiTextureRenderTargetDescription description;
  description.setColorAttachments({color_attachment});
  if (hdr.depth_resolve) {
    description.setDepthTexture(hdr.msaa_depth_texture.get());
    description.setDepthResolveTexture(hdr.resolve_depth.get());
  } else {
    description.setDepthStencilBuffer(hdr.msaa_depth.get());
  }
  hdr.target.reset(owner->newTextureRenderTarget(description));
  if (hdr.render_pass == nullptr) {
    hdr.render_pass.reset(hdr.target->newCompatibleRenderPassDescriptor());
  }
  hdr.target->setRenderPassDescriptor(hdr.render_pass.get());
  if (!hdr.target->create()) {
    return fail(tr("Could not create the multisample HDR color target and resolve"));
  }

  QRhiTextureRenderTargetDescription depth_description;
  depth_description.setColorAttachments({QRhiColorAttachment(hdr.resolve_coverage.get())});
  if (!hdr.depth_resolve) {
    depth_description.setDepthTexture(hdr.resolve_depth.get());
  }
  hdr.depth_target.reset(owner->newTextureRenderTarget(depth_description));
  if (hdr.depth_render_pass == nullptr) {
    hdr.depth_render_pass.reset(hdr.depth_target->newCompatibleRenderPassDescriptor());
  }
  hdr.depth_target->setRenderPassDescriptor(hdr.depth_render_pass.get());
  if (!hdr.depth_target->create()) {
    return fail(tr("Could not create the single-sample coverage/depth replay target"));
  }

  hdr.present_bindings.reset(createBindings(
      owner,
      {
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::FragmentStage, hdr.composite_uniforms.get()),
          QRhiShaderResourceBinding::sampledTexture(
              1, QRhiShaderResourceBinding::FragmentStage, hdr.resolve_color.get(), hdr.color_sampler.get()),
          QRhiShaderResourceBinding::sampledTexture(
              2, QRhiShaderResourceBinding::FragmentStage, hdr.resolve_coverage.get(), hdr.coverage_sampler.get()),
          QRhiShaderResourceBinding::sampledTexture(
              3, QRhiShaderResourceBinding::FragmentStage, hdr.resolve_depth.get(), hdr.coverage_sampler.get()),
      }));
  if (hdr.present_bindings == nullptr) {
    return fail(tr("Could not create the HDR presentation bindings"));
  }

  // QRhiWidget keeps its render-pass descriptor across resizes; a new one
  // (format/sample-count change) needs a new pipeline.
  if (hdr.present_pipeline != nullptr &&
      hdr.present_pipeline->renderPassDescriptor() != renderTarget()->renderPassDescriptor()) {
    hdr.present_pipeline.reset();
  }
  if (hdr.present_pipeline != nullptr) {
    hdr.present_pipeline->setShaderResourceBindings(hdr.present_bindings.get());
  } else {
    hdr.present_pipeline = createPipeline(
        owner,
        {
            .vertex = loadRhiShader(presentVertexShaderPath(*owner)),
            .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/present.frag.qsb")),
            .bindings = hdr.present_bindings.get(),
            .blend = std::nullopt,
            .depth_test = false,
            .depth_write = false,
        },
        renderTarget()->renderPassDescriptor(), sampleCount());
    if (hdr.present_pipeline == nullptr) {
      return fail(tr("Could not create the HDR presentation pipeline"));
    }
  }
  // The scene pipelines' HDR and depth-replay variants are built on first use.
  pipelines_->setVariantTargets(hdr.render_pass.get(), kHdrSampleCount, hdr.depth_render_pass.get());

  hdr.size = size;
  hdr.charge(owner, bytes);
  hdr_->state.rejected_size = {};
  if (!hdr_->state.capability_error.isEmpty()) {
    hdr_->state.capability_error.clear();
    updateRenderingWarning();
  }
  ++hdr_->state.resource_generation;
  return true;
}

}  // namespace pj::scene3d
