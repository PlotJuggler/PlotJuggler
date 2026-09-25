#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Timestamp arithmetic that clamps at the int64 limits instead of overflowing
// (signed overflow is UB). Timestamps near the limits are not meaningful, but
// merge shifts and retention windows must not turn them into UB or wrap them
// around to the other end of the timeline.

#include <limits>

#include "pj_base/types.hpp"

namespace PJ {

[[nodiscard]] constexpr Timestamp saturatingAdd(Timestamp a, Timestamp b) noexcept {
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
  if (b > 0 && a > kMax - b) {
    return kMax;
  }
  if (b < 0 && a < kMin - b) {
    return kMin;
  }
  return a + b;
}

[[nodiscard]] constexpr Timestamp saturatingSub(Timestamp a, Timestamp b) noexcept {
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
  if (b > 0 && a < kMin + b) {
    return kMin;
  }
  if (b < 0 && a > kMax + b) {
    return kMax;
  }
  return a - b;
}

}  // namespace PJ
