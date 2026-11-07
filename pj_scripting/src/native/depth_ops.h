// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>

#include "image_ops.h"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/point_cloud.hpp"

// Depth unprojection: a decoded depth image and its CameraInfo into a metric point cloud. Luau-free, like
// image_ops.h.
namespace PJ::scripting::native {

/// `step` keeps every step-th pixel of every step-th row (an organized grid); a pixel farther than `max_depth`
/// metres, or with no valid depth, becomes a NaN point that keeps its place.
struct DepthOptions {
  uint32_t step = 1;
  double max_depth = std::numeric_limits<double>::infinity();
};

/// `depth` as an `sdk::Image` the decoder reads: its bytes, tightly packed and little-endian (the SDK depth schema
/// has neither a stride nor an endianness), shared, not copied.
[[nodiscard]] sdk::Image depthAsImage(const sdk::DepthImage& depth);

/// Unprojects a rectified depth image through `camera.K` into an organized x/y/z float32 cloud in `camera.frame_id`
/// (a DepthImage has no frame of its own), stamped with the image's stamp. `encoding` is the source's: "16UC1" and
/// "mono16" are millimetres, "32FC1" metres; another is an error, and so is a nonzero distortion on the camera, a K
/// that does not invert, a camera whose size differs from the pixels', and pixels with more than one channel. The
/// cloud is charged to `limits.budget` before it is allocated; a DepthImage's own distortion is the caller's check.
[[nodiscard]] Expected<sdk::PointCloud> depthToPointCloud(
    const DecodedPixels& pixels, const std::string& encoding, Timestamp stamp, const sdk::CameraInfo& camera,
    const DepthOptions& options, const NativeLimits& limits);

}  // namespace PJ::scripting::native
