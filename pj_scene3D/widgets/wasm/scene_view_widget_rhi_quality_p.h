// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <rhi/qrhi.h>

#include <QString>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <random>

#include "pj_scene3d_widgets/rhi_conventions.h"
#include "pj_scene3d_widgets/ssao_kernel.h"

namespace pj::scene3d {

inline RhiConventions rhiConventions(const QRhi& rhi) {
  return {rhi.isYUpInNDC(), rhi.isYUpInFramebuffer(), rhi.isClipDepthZeroToOne()};
}

/// The fullscreen-triangle vertex shader for `rhi`: the -DPJ_FLIP_Y bake where
/// texture row 0 is not at NDC y = -1.
inline QString presentVertexShaderPath(const QRhi& rhi) {
  return fullscreenNeedsYFlip(rhiConventions(rhi)) ? QStringLiteral(":/scene3d_wasm/present_flip_y.vert.qsb")
                                                   : QStringLiteral(":/scene3d_wasm/present.vert.qsb");
}

constexpr quint32 kShadowUniformBytes = 64U;
constexpr quint32 kCompositeUniformBytes = 112U;
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
  std::array<float, 4> ssao_params{};
};
static_assert(sizeof(CompositeUniforms) == kCompositeUniformBytes);

struct alignas(16) SsaoUniforms {
  std::array<float, 16> projection{};
  std::array<float, 16> inverse_projection{};
  std::array<float, 4> params{};
  std::array<std::array<float, 4>, kSsaoKernelSize> kernel{};
};
static_assert(sizeof(SsaoUniforms) == kSsaoUniformBytes);

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
