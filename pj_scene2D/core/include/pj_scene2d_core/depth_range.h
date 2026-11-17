#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"

namespace PJ {

/// Finite and > 0; 0 is the no-data sentinel. The rule of the decode, Auto-fit and Point Inspector; it must match
/// the media shader's `!(d > 0)` test, or the inspector disagrees with the transparent pixels on screen.
[[nodiscard]] inline bool isValidDepth(float value) noexcept {
  return std::isfinite(value) && value > 0.0f;
}

/// The `lo_frac`/`hi_frac` percentiles of the valid samples (percentiles, not min/max, so speckle outliers do not
/// move it), as `min` = near, `max` = far. Equal percentiles nudge far up: the shader divides by far - near. nullopt
/// without valid samples.
[[nodiscard]] inline std::optional<Range<float>> depthPercentileRange(
    const float* data, std::size_t count, float lo_frac = 0.02f, float hi_frac = 0.98f) {
  if (data == nullptr || count == 0) {
    return std::nullopt;
  }
  std::vector<float> valid;
  valid.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float v = data[i];
    if (isValidDepth(v)) {
      valid.push_back(v);
    }
  }
  if (valid.empty()) {
    return std::nullopt;
  }

  float lo = std::clamp(lo_frac, 0.0f, 1.0f);
  float hi = std::clamp(hi_frac, 0.0f, 1.0f);
  if (hi < lo) {
    std::swap(lo, hi);
  }

  const auto n = valid.size();
  const auto pick = [&](float frac) -> float {
    const auto idx = static_cast<std::size_t>(std::llround(frac * static_cast<double>(n - 1)));
    auto it = valid.begin() + static_cast<std::ptrdiff_t>(std::min(idx, n - 1));
    std::nth_element(valid.begin(), it, valid.end());
    return *it;
  };

  float near_m = pick(lo);
  float far_m = pick(hi);
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  if (!(far_m > near_m)) {
    far_m = near_m + std::max(1e-3f, std::abs(near_m) * 0.01f);
  }
  return Range<float>{near_m, far_m};
}

}  // namespace PJ
