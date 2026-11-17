#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string_view>
#include <vector>

#include "pj_base/builtin/camera_info.hpp"

namespace PJ {

/// Reverse rectification map: for every output (rectified) pixel, the source (raw) pixel coordinates, in the
/// decoded source's pixel space, to sample bilinearly. Built once per camera from CameraInfo K/D/R/P.
struct UndistortMap {
  int out_width = 0;
  int out_height = 0;
  int src_width = 0;  ///< A frame of another size needs a rebuilt map.
  int src_height = 0;
  std::vector<float> src_x;
  std::vector<float> src_y;

  [[nodiscard]] bool valid() const noexcept {
    const auto n = static_cast<size_t>(out_width) * static_cast<size_t>(out_height);
    return out_width > 0 && out_height > 0 && src_x.size() == n && src_y.size() == n;
  }
};

/// Nonzero fx, fy and native resolution. With no distortion the map is a pure rescale to the calibrated resolution,
/// which still lines up annotations authored there. A capability check, not a "needs rectifying" decision.
[[nodiscard]] bool isRectifiable(const sdk::CameraInfo& ci) noexcept;

/// `src_w`/`src_h` is the decoded image's size, which may be smaller than `ci`'s (Waymo ships 480-wide JPEGs against
/// a 1920-wide calibration); `out_w`/`out_h` = `ci.width`/`ci.height` for native output. Invalid when `ci` is not
/// rectifiable.
[[nodiscard]] UndistortMap computeUndistortMap(const sdk::CameraInfo& ci, int src_w, int src_h, int out_w, int out_h);

/// The CPU rectifier's table: per output pixel, the top-left source tap's linear index and the two bilinear
/// fractions, so the per-frame loop does no floor or bounds math. A frame of another size needs a rebuilt table.
struct UndistortMapFast {
  int out_width = 0;
  int out_height = 0;
  int src_width = 0;
  int src_height = 0;
  std::vector<int32_t> src_p0;  ///< Linear source pixel index of the top-left bilinear tap; -1 = out of bounds (black).
  std::vector<float> frac_x;    ///< Horizontal bilinear fraction in [0,1).
  std::vector<float> frac_y;    ///< Vertical bilinear fraction in [0,1).

  [[nodiscard]] bool valid() const noexcept {
    const auto n = static_cast<size_t>(out_width) * static_cast<size_t>(out_height);
    return out_width > 0 && out_height > 0 && src_p0.size() == n && frac_x.size() == n && frac_y.size() == n;
  }
};

/// Bit-for-bit equivalent to `rectifyFrame`; invalid for an invalid or zero-source map.
[[nodiscard]] UndistortMapFast buildFastRectifyMap(const UndistortMap& map);

/// The GPU remap LUT: per output pixel (row-major), the source sample point in [0,1] texture space with the +0.5
/// half-texel offset that makes a linear sampler reproduce the CPU bilinear; `(-1,-1)` marks out of bounds (drawn
/// black). Empty for an invalid map.
[[nodiscard]] std::vector<float> undistortMapToNormalizedRG(const UndistortMap& map);

}  // namespace PJ
