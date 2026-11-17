// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/depth_backproject.h"

#include <cmath>
#include <cstring>

namespace pj::scene3d {

DepthIntrinsics intrinsicsFromK(const std::array<double, 9>& k, uint32_t width, uint32_t height) {
  // Row-major K: [fx 0 cx; 0 fy cy; 0 0 1] -> indices 0, 4, 2, 5.
  return DepthIntrinsics{
      .fx = k[0],
      .fy = k[4],
      .cx = k[2],
      .cy = k[5],
      .width = width,
      .height = height,
  };
}

std::vector<glm::vec3> depthToPoints(
    const PJ::sdk::DepthImage& depth, const DepthIntrinsics& intr, const BackprojectOptions& opts,
    std::vector<float>* scalar_out, AABB* bounds_out, PJ::Range<float>* scalar_range_out) {
  std::vector<glm::vec3> points;
  if (scalar_out != nullptr) {
    scalar_out->clear();
  }
  AABB bounds;
  if (bounds_out != nullptr) {
    *bounds_out = AABB{};
  }
  if (scalar_range_out != nullptr) {
    *scalar_range_out = {0.0f, 1.0f};
  }
  if (!intr.valid()) {
    return points;
  }

  int bytes_per_sample = 0;
  float to_metres = 1.0f;
  bool is_uint16 = false;
  if (depth.encoding == "16UC1") {
    bytes_per_sample = 2;
    to_metres = 0.001f;  // millimetres -> metres
    is_uint16 = true;
  } else if (depth.encoding == "32FC1") {
    bytes_per_sample = 4;  // float32, already metres
  } else {
    return points;  // unsupported encoding
  }

  const uint32_t w = depth.width;
  const uint32_t h = depth.height;
  if (w == 0 || h == 0) {
    return points;
  }
  const size_t needed_bytes = static_cast<size_t>(w) * h * static_cast<size_t>(bytes_per_sample);
  if (depth.data.size() < needed_bytes) {
    return points;  // truncated/inconsistent buffer
  }

  // Rescale intrinsics to the depth image's own resolution when calibrated at a different one (CameraInfo at color
  // resolution + aligned, downsampled depth).
  double fx = intr.fx;
  double fy = intr.fy;
  double cx = intr.cx;
  double cy = intr.cy;
  if (intr.width > 0 && intr.height > 0 && (intr.width != w || intr.height != h)) {
    const double sx = static_cast<double>(w) / static_cast<double>(intr.width);
    const double sy = static_cast<double>(h) / static_cast<double>(intr.height);
    fx *= sx;
    cx *= sx;
    fy *= sy;
    cy *= sy;
  }

  const uint32_t stride = opts.stride == 0U ? 1U : opts.stride;
  const uint8_t* base = depth.data.data();

  const size_t cols = (w + stride - 1U) / stride;
  const size_t rows = (h + stride - 1U) / stride;
  points.reserve(cols * rows);
  if (scalar_out != nullptr) {
    scalar_out->reserve(cols * rows);
  }

  for (uint32_t v = 0; v < h; v += stride) {
    for (uint32_t u = 0; u < w; u += stride) {
      const size_t idx = static_cast<size_t>(v) * w + u;
      float z = 0.0f;
      if (is_uint16) {
        const uint8_t* p = base + idx * 2U;  // little-endian uint16
        const uint16_t raw = static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U));
        z = static_cast<float>(raw) * to_metres;
      } else {
        std::memcpy(&z, base + idx * 4U, sizeof(float));  // host is little-endian
      }
      if (!std::isfinite(z) || z <= opts.min_depth_m) {
        continue;  // drop "no return" (z<=0/min) and non-finite samples
      }
      if (opts.max_depth_m > 0.0f && z > opts.max_depth_m) {
        continue;
      }
      const float x = static_cast<float>((static_cast<double>(u) - cx) * static_cast<double>(z) / fx);
      const float y = static_cast<float>((static_cast<double>(v) - cy) * static_cast<double>(z) / fy);
      const glm::vec3 point(x, y, z);
      points.push_back(point);
      expandAABB(bounds, point);
      if (scalar_out != nullptr) {
        scalar_out->push_back(z);
      }
    }
  }

  if (bounds_out != nullptr) {
    *bounds_out = bounds;
  }
  if (scalar_range_out != nullptr && bounds.valid) {
    // scalar == point.z, so the depth range is exactly the AABB's z extent; widen a flat range so the colormap still
    // spans.
    float lo = bounds.min.z;
    float hi = bounds.max.z;
    if (hi - lo < 1e-6f) {
      hi = lo + 1.0f;
    }
    *scalar_range_out = {lo, hi};
  }
  return points;
}

}  // namespace pj::scene3d
