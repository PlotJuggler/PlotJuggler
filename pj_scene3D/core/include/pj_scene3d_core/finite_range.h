// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace pj::scene3d {

// Finite min/max of a set of floats; `valid` is false when no element is finite.
struct FiniteRange {
  float lo = 0.0f;
  float hi = 0.0f;
  bool valid = false;
};

// Grows `range` to include `value`; a non-finite value is ignored.
inline void accumulate(FiniteRange& range, float value) {
  if (!std::isfinite(value)) {
    return;
  }
  if (!range.valid) {
    range = {.lo = value, .hi = value, .valid = true};
  } else {
    range.lo = std::min(range.lo, value);
    range.hi = std::max(range.hi, value);
  }
}

// Finite min/max of `values`.
[[nodiscard]] inline FiniteRange finiteRange(const std::vector<float>& values) {
  FiniteRange range;
  for (const float value : values) {
    accumulate(range, value);
  }
  return range;
}

}  // namespace pj::scene3d
