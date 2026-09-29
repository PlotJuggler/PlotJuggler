// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <rhi/qrhi.h>

#include <QSize>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <random>

#include "pj_scene3d_core/shadow_camera.h"
#include "pj_scene3d_widgets/rhi_conventions.h"
#include "pj_scene3d_widgets/ssao_kernel.h"
#include "rhi_scene_pass.h"

namespace pj::scene3d {

/// The fullscreen-triangle vertex shader for `rhi`: the -DPJ_FLIP_Y bake where
/// texture row 0 is not at NDC y = -1.
inline QString presentVertexShaderPath(const QRhi& rhi) {
  return fullscreenNeedsYFlip(rhiConventions(rhi)) ? QStringLiteral(":/scene3d_rhi/present_flip_y.vert.qsb")
                                                   : QStringLiteral(":/scene3d_rhi/present.vert.qsb");
}

/// Samples of the HDR chain's colour and depth targets.
constexpr int kHdrSampleCount = 4;
constexpr quint32 kShadowUniformBytes = 64U;
constexpr quint32 kCompositeUniformBytes = 128U;
constexpr quint32 kSsaoUniformBytes = 656U;
constexpr std::size_t kSsaoKernelSize = 32U;

// The depth replay clears RGBA8 to zero and writes every surviving fragment's
// post-discard alpha. Half an 8-bit step separates untouched background from
// visible geometry, including source-over and antialiased coverage.
constexpr float kBackgroundCoverageThreshold = 0.5F / 255.0F;

struct alignas(16) CompositeUniforms {
  std::array<float, 4> params{};
  std::array<float, 16> inverse_projection{};
  std::array<float, 4> edl_params{};
  std::array<float, 4> ssao_params{};   // AO strength, enabled, coverage from depth, unused
  std::array<float, 4> grade_params{};  // tonemap mode, unused x3
};
static_assert(sizeof(CompositeUniforms) == kCompositeUniformBytes);

struct alignas(16) SsaoUniforms {
  std::array<float, 16> projection{};
  std::array<float, 16> inverse_projection{};
  std::array<float, 4> params{};
  std::array<std::array<float, 4>, kSsaoKernelSize> kernel{};
};
static_assert(sizeof(SsaoUniforms) == kSsaoUniformBytes);

/// Bookkeeping every optional quality stage carries, in its resource owner:
/// whether it ran last frame (cleared by the owner's release), the test override
/// holding it down, the size-keyed failure latch (a resize re-probes: a failure
/// never blocks the stage for the QRhi's whole lifetime), and the resource
/// generation the probes report.
struct QualityStageState {
  bool last_active = false;
  bool forced_off = false;
  QSize rejected_size;
  QString capability_error;
  std::uint64_t resource_generation = 0;
  /// True while the stage must stay down for this output size: a failure is
  /// latched only against the size it happened at.
  [[nodiscard]] bool blockedFor(const QSize& size) const {
    return !capability_error.isEmpty() && rejected_size == size;
  }
};

// The quality stages' GPU resources, one owner per stage. Each release() deletes
// a resource only after everything of its own that binds it (bindings before
// targets before attachments, pipelines and targets before their render-pass
// descriptor); the members are declared in the reverse of that order, so a plain
// destruction keeps it too. The view releases them explicitly, while their QRhi
// lives, after the resources of other owners that bind them (the model pass's
// caster and mask pipelines, the grid's shadow receiver, the registry's HDR variants).

/// The shadow map: a kShadowMapSize² D32F depth texture, its target and the
/// casters' light bindings. The model pass's caster pipeline and materials and
/// the grid's receiver bind them.
struct ShadowResources {
  QualityStageState state;
  std::unique_ptr<QRhiTexture> texture;
  std::unique_ptr<QRhiSampler> sampler;
  std::unique_ptr<QRhiBuffer> uniforms;  ///< the casters' binding 0: the light view-projection
  std::unique_ptr<QRhiRenderPassDescriptor> render_pass;
  std::unique_ptr<QRhiTextureRenderTarget> target;
  std::unique_ptr<QRhiShaderResourceBindings> bindings;  ///< the casters' layout

  /// The map's edge in texels; 0 without a map.
  [[nodiscard]] int mapSize() const {
    return texture != nullptr ? kShadowMapSize : 0;
  }
  void release() {
    bindings.reset();
    target.reset();
    render_pass.reset();
    uniforms.reset();
    sampler.reset();
    texture.reset();
    state.last_active = false;
  }
};

/// The HDR chain. Kept per QRhi: the render-pass descriptors (the registry's
/// variants target them), samplers, composite uniforms and the present pipeline
/// (on the widget's target). Rebuilt per render size: the attachments, their
/// targets and the present bindings (releaseTargets()).
struct HdrResources {
  QualityStageState state;
  std::unique_ptr<QRhiRenderPassDescriptor> render_pass;        ///< the HDR colour pass's
  std::unique_ptr<QRhiRenderPassDescriptor> depth_render_pass;  ///< the coverage pass's
  std::unique_ptr<QRhiSampler> color_sampler;
  std::unique_ptr<QRhiSampler> coverage_sampler;
  std::unique_ptr<QRhiBuffer> composite_uniforms;
  std::unique_ptr<QRhiTexture> resolve_coverage;
  std::unique_ptr<QRhiTexture> resolve_depth;
  std::unique_ptr<QRhiTexture> resolve_color;
  std::unique_ptr<QRhiTexture> msaa_depth_texture;  ///< the depth-resolve chain's
  std::unique_ptr<QRhiRenderBuffer> msaa_depth;     ///< the depth replay's chain
  std::unique_ptr<QRhiRenderBuffer> msaa_color;
  std::unique_ptr<QRhiTextureRenderTarget> target;
  /// Fills resolve_coverage before SSAO and the EDL mask: the depth replay
  /// (coverage + depth), or on the depth-resolve chain a clear of the coverage
  /// alone (the HDR pass resolved the depth, present reads coverage from it).
  std::unique_ptr<QRhiTextureRenderTarget> depth_target;
  /// Samples both resolved textures: released before them. The present
  /// pipeline keeps only its layout and survives a size-only rebuild, which
  /// binds a new layout-compatible one.
  std::unique_ptr<QRhiShaderResourceBindings> present_bindings;
  std::unique_ptr<QRhiGraphicsPipeline> present_pipeline;
  QSize size;                         ///< the render size the targets exist for; empty: none
  QRhi* charged_rhi = nullptr;        ///< the QRhi whose shared byte cap carries allocated_bytes
  std::uint64_t allocated_bytes = 0;  ///< charged against the byte caps (charge())
  bool depth_resolve = false;         ///< the chain resolves depth (QRhi::ResolveDepthStencil)

  /// The bytes every live chain has charged to `rhi` (the views sharing it).
  [[nodiscard]] static std::uint64_t chargedBytes(QRhi* rhi);
  /// Charges new targets' `bytes` to `rhi`'s shared cap; releaseTargets() gives
  /// them back, and must have given back the previous charge.
  void charge(QRhi* rhi, std::uint64_t bytes);
  /// The per-size part: bindings, then targets, then attachments; gives their
  /// byte charge back. SSAO's and EDL's targets reference it: released first.
  void releaseTargets();
  void release() {
    present_pipeline.reset();
    releaseTargets();
    composite_uniforms.reset();
    coverage_sampler.reset();
    color_sampler.reset();
    depth_render_pass.reset();
    render_pass.reset();
  }
};

/// SSAO: raw AO written into the HDR coverage's green channel through a
/// colour-only target, sampling the HDR depth. Its uniforms (a constant kernel)
/// are a base resource of the view.
struct SsaoResources {
  QualityStageState state;
  std::unique_ptr<QRhiRenderPassDescriptor> render_pass;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline;
  std::unique_ptr<QRhiTextureRenderTarget> target;
  std::unique_ptr<QRhiShaderResourceBindings> bindings;  ///< samples the HDR depth

  /// The per-size part, which references the HDR attachments.
  void releaseTargets() {
    bindings.reset();
    target.reset();
    state.last_active = false;
  }
  void release() {
    releaseTargets();
    pipeline.reset();
    render_pass.reset();
  }
};

/// The EDL mesh mask's target over the HDR coverage and depth; the model pass's
/// mask pipelines target its render-pass descriptor.
struct EdlResources {
  QualityStageState state;
  std::unique_ptr<QRhiRenderPassDescriptor> render_pass;
  std::unique_ptr<QRhiTextureRenderTarget> target;

  /// The per-size part, which references the HDR attachments.
  void releaseTargets() {
    target.reset();
    state.last_active = false;
  }
  void release() {
    releaseTargets();
    render_pass.reset();
  }
};

inline const std::array<std::array<float, 4>, kSsaoKernelSize>& ssaoKernel() {
  static const auto kernel = [] {
    std::array<std::array<float, 4>, kSsaoKernelSize> result{};
    const std::vector<glm::vec3> samples = ssaoHemisphereKernel(static_cast<int>(kSsaoKernelSize));
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] = {samples[index].x, samples[index].y, samples[index].z, 0.0F};
    }
    return result;
  }();
  return kernel;
}

}  // namespace pj::scene3d
