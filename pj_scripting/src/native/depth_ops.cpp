// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "depth_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "pj_base/builtin/depth_image_utils.hpp"

namespace PJ::scripting::native {
namespace {

constexpr uint32_t kPointStep = 3 * sizeof(float);

/// The cloud's bytes and their charge: its anchor owns both, so a retained cloud keeps counting.
using ChargedBytes = Charged<std::vector<uint8_t>>;

}  // namespace

sdk::Image depthAsImage(const sdk::DepthImage& depth) {
  sdk::Image image;
  image.width = depth.width;
  image.height = depth.height;
  image.encoding = depth.encoding;
  image.data = depth.data;
  image.anchor = depth.anchor;
  image.timestamp_ns = depth.timestamp_ns;
  return image;
}

Expected<sdk::PointCloud> depthToPointCloud(
    const DecodedPixels& pixels, const std::string& encoding, Timestamp stamp, const sdk::CameraInfo& camera,
    const DepthOptions& options, const NativeLimits& limits) {
  if (Status valid = validatePixels(pixels); !valid) {
    return unexpected(valid.error());
  }
  if (pixels.channels != 1) {
    return unexpected("a depth image has one channel");
  }
  if (options.step == 0 || !(options.max_depth > 0)) {
    return unexpected("invalid depth options");
  }
  if ((camera.width != 0 && camera.width != pixels.width) || (camera.height != 0 && camera.height != pixels.height)) {
    return unexpected("camera calibration dimensions do not match the depth image");
  }
  if (std::any_of(camera.D.begin(), camera.D.end(), [](double coefficient) { return coefficient != 0; })) {
    return unexpected("depth must be rectified before unprojection");
  }
  const std::optional<sdk::PinholeIntrinsics> intrinsics = sdk::PinholeIntrinsics::fromK(camera.K);
  if (!intrinsics) {
    return unexpected("invalid camera intrinsics");
  }
  double scale = 1.0;
  if (encoding == "mono16" || encoding == "16UC1") {
    scale = 0.001;
  } else if (encoding != "32FC1") {
    return unexpected("unsupported depth encoding: " + encoding);
  }
  const uint32_t width = 1 + (pixels.width - 1) / options.step;
  const uint32_t height = 1 + (pixels.height - 1) / options.step;
  const std::size_t bytes = std::size_t{width} * height * kPointStep;
  MemoryReservation charge(limits.budget);
  const std::size_t header = sizeof(sdk::PointCloud) + 3 * (sizeof(sdk::PointField) + 1) + camera.frame_id.size();
  if (Status grown = charge.grow(bytes + header); !grown) {
    return unexpected(grown.error());
  }
  auto owner = std::make_shared<ChargedBytes>(ChargedBytes{std::vector<uint8_t>(bytes), std::move(charge)});
  uint8_t* out = owner->value.data();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  bool dense = true;
  for (uint32_t row = 0; row < height; ++row) {
    if (limits.cancelled && limits.cancelled()) {
      return unexpected(std::string(kCancelledError));
    }
    const uint32_t v = row * options.step;
    const float* line = pixels.samples->data() + std::size_t{v} * pixels.width;
    for (uint32_t u = 0; u < pixels.width; u += options.step, out += kPointStep) {
      const double depth_m = line[u] * scale;
      const auto point = depth_m <= options.max_depth ? intrinsics->unproject(u, v, depth_m) : std::nullopt;
      const float xyz[3] = {
          point ? static_cast<float>((*point)[0]) : nan, point ? static_cast<float>((*point)[1]) : nan,
          point ? static_cast<float>((*point)[2]) : nan};
      dense = dense && point && std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]);
      std::memcpy(out, xyz, sizeof(xyz));
    }
  }
  sdk::PointCloud cloud;
  cloud.width = width;
  cloud.height = height;
  cloud.point_step = kPointStep;
  cloud.row_step = width * kPointStep;
  cloud.frame_id = camera.frame_id;
  cloud.timestamp_ns = stamp;
  cloud.is_dense = dense;
  cloud.fields = {
      {"x", 0, sdk::PointField::Datatype::kFloat32, 1},
      {"y", 4, sdk::PointField::Datatype::kFloat32, 1},
      {"z", 8, sdk::PointField::Datatype::kFloat32, 1}};
  cloud.data = Span<const uint8_t>(owner->value.data(), owner->value.size());
  cloud.anchor = std::move(owner);
  return cloud;
}

}  // namespace PJ::scripting::native
