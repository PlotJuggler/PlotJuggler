// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "image_ops.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace PJ::scripting::native {
namespace {

bool cancelled(const NativeLimits& limits) {
  return limits.cancelled && limits.cancelled();
}

/// Samples of `count` floats, charged to `limits.budget` before they are allocated.
Expected<DecodedPixels> allocateLike(const DecodedPixels& like, std::size_t count, const NativeLimits& limits) {
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return unexpected("image size overflow");
  }
  auto charge = std::make_shared<MemoryReservation>(limits.budget);
  if (Status grown = charge->grow(count * sizeof(float)); !grown) {
    return unexpected(grown.error());
  }
  DecodedPixels out = like;
  out.samples = std::make_shared<const std::vector<float>>(count);
  out.charge = std::move(charge);
  return out;
}

float* mutableSamples(DecodedPixels& pixels) {
  return const_cast<float*>(pixels.samples->data());  // freshly allocated by allocateLike, not yet shared
}

/// Two passes over `channel`: count the matches, charge them, then collect them at their source coordinates. The
/// predicate is one comparison, so evaluating it twice costs less than a match bitmap would. A non-finite sample never
/// matches; `value` stays a double (a Luau number), because rounded to float 1.00000001 would equal a sample of 1.
template <class Compare>
Expected<Charged<sdk::ImageAnnotations>> thresholdKernel(
    const DecodedPixels& image, double value, uint32_t channel, const NativeLimits& limits) {
  const auto matches = [value](float sample) { return std::isfinite(sample) && Compare{}(sample, value); };
  const std::size_t stride = image.channels;
  const std::size_t row_samples = std::size_t{image.width} * stride;
  const float* base = image.samples->data() + channel;
  std::size_t matched = 0;
  for (uint32_t y = 0; y < image.height; ++y) {
    if (cancelled(limits)) {
      return unexpected(std::string(kCancelledError));
    }
    const float* row = base + y * row_samples;
    for (uint32_t x = 0; x < image.width; ++x) {
      matched += matches(row[x * stride]) ? 1 : 0;
    }
  }
  Charged<sdk::ImageAnnotations> result{{}, MemoryReservation(limits.budget)};
  const std::size_t header = sizeof(sdk::ImageAnnotations) + sizeof(sdk::PointsAnnotation) + image.image_topic.size();
  if (Status grown = result.charge.grow(header + matched * sizeof(sdk::Point2)); !grown) {
    return unexpected(grown.error());
  }
  result.value.image_topic = image.image_topic;
  result.value.timestamp = image.timestamp_ns;
  if (matched == 0) {
    return result;
  }
  sdk::PointsAnnotation points;
  points.topology = sdk::AnnotationTopology::kPoints;
  points.points.reserve(matched);
  for (uint32_t y = 0; y < image.height; ++y) {
    if (cancelled(limits)) {
      return unexpected(std::string(kCancelledError));
    }
    const float* row = base + y * row_samples;
    const double source_y = double(image.origin_y) + y;
    for (uint32_t x = 0; x < image.width; ++x) {
      if (matches(row[x * stride])) {
        points.points.push_back({double(image.origin_x) + x, source_y});
      }
    }
  }
  result.value.points.push_back(std::move(points));
  return result;
}

}  // namespace

std::string imageRepresentation(const sdk::Image& image) {
  const std::string& encoding = image.encoding;
  if (encoding == "jpeg" || encoding == "png") {
    return encoding;
  }
  constexpr std::string_view kRaw[] = {"mono8", "8UC1", "mono16", "16UC1", "32FC1", "rgb8", "rgba8", "bgr8", "bgra8"};
  if (std::find(std::begin(kRaw), std::end(kRaw), encoding) != std::end(kRaw)) {
    return "raw:" + encoding;
  }
  return "compressed:" + encoding;
}

std::string imageRepresentation(const sdk::DepthImage& depth) {
  return "depth:" + depth.encoding;
}

Status validatePixels(const DecodedPixels& image) {
  if (image.width == 0 || image.height == 0 || (image.channels != 1 && image.channels != 3 && image.channels != 4)) {
    return unexpected("invalid image dimensions or channels");
  }
  const uint64_t pixels = uint64_t{image.width} * image.height;
  if (image.samples == nullptr || pixels > std::numeric_limits<std::size_t>::max() / sizeof(float) / image.channels ||
      image.samples->size() != pixels * image.channels) {
    return unexpected("invalid image sample buffer");
  }
  return okStatus();
}

Expected<std::vector<float>> imagePixel(const DecodedPixels& image, uint32_t x, uint32_t y) {
  if (Status valid = validatePixels(image); !valid) {
    return unexpected(valid.error());
  }
  if (x >= image.width || y >= image.height) {
    return unexpected("pixel is outside the image");
  }
  const float* pixel = image.samples->data() + (std::size_t{y} * image.width + x) * image.channels;
  return std::vector<float>(pixel, pixel + image.channels);
}

Expected<DecodedPixels> imageToGray(const DecodedPixels& image, const NativeLimits& limits) {
  if (Status valid = validatePixels(image); !valid) {
    return unexpected(valid.error());
  }
  if (image.channels == 1) {
    return image;
  }
  auto out = allocateLike(image, std::size_t{image.width} * image.height, limits);
  if (!out) {
    return out;
  }
  out->channels = 1;
  const std::size_t stride = image.channels;
  const float* source = image.samples->data();
  float* target = mutableSamples(*out);
  for (uint32_t y = 0; y < image.height; ++y) {
    if (cancelled(limits)) {
      return unexpected(std::string(kCancelledError));
    }
    for (uint32_t x = 0; x < image.width; ++x, source += stride) {
      *target++ = 0.299f * source[0] + 0.587f * source[1] + 0.114f * source[2];
    }
  }
  return out;
}

Expected<DecodedPixels> imageCrop(
    const DecodedPixels& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const NativeLimits& limits) {
  if (Status valid = validatePixels(image); !valid) {
    return unexpected(valid.error());
  }
  if (width == 0 || height == 0 || x >= image.width || y >= image.height || width > image.width - x ||
      height > image.height - y) {
    return unexpected("crop is outside the image");
  }
  if (x > UINT32_MAX - image.origin_x || y > UINT32_MAX - image.origin_y) {
    return unexpected("crop origin overflow");
  }
  const std::size_t row = std::size_t{width} * image.channels;
  auto out = allocateLike(image, row * height, limits);
  if (!out) {
    return out;
  }
  out->width = width;
  out->height = height;
  out->origin_x += x;
  out->origin_y += y;
  const float* source = image.samples->data() + (std::size_t{y} * image.width + x) * image.channels;
  float* target = mutableSamples(*out);
  for (uint32_t line = 0; line < height; ++line) {
    if (cancelled(limits)) {
      return unexpected(std::string(kCancelledError));
    }
    std::copy_n(source, row, target);
    source += std::size_t{image.width} * image.channels;
    target += row;
  }
  return out;
}

Expected<Charged<sdk::ImageAnnotations>> imageThreshold(
    const DecodedPixels& image, CompareOp op, double value, uint32_t channel, const NativeLimits& limits) {
  if (Status valid = validatePixels(image); !valid) {
    return unexpected(valid.error());
  }
  if (channel >= image.channels || !std::isfinite(value)) {
    return unexpected("invalid threshold channel or value");
  }
  return dispatchCompare(
      op, [&](auto compare) { return thresholdKernel<decltype(compare)>(image, value, channel, limits); });
}

Expected<ImageStats> imageStats(const DecodedPixels& image, const NativeLimits& limits) {
  if (Status valid = validatePixels(image); !valid) {
    return unexpected(valid.error());
  }
  ImageStats stats;
  double sum = 0.0;
  double low = std::numeric_limits<double>::infinity();
  double high = -std::numeric_limits<double>::infinity();
  const std::size_t row = std::size_t{image.width} * image.channels;
  const float* sample = image.samples->data();
  for (uint32_t y = 0; y < image.height; ++y) {
    if (cancelled(limits)) {
      return unexpected(std::string(kCancelledError));
    }
    for (const float* end = sample + row; sample != end; ++sample) {
      if (std::isfinite(*sample)) {
        sum += *sample;
        low = std::min<double>(low, *sample);
        high = std::max<double>(high, *sample);
        ++stats.count;
      }
    }
  }
  if (stats.count == 0) {
    return unexpected("image has no finite samples");
  }
  stats.min = low;
  stats.max = high;
  stats.mean = sum / double(stats.count);
  return stats;
}

std::size_t countAnnotationPoints(const sdk::ImageAnnotations& annotations) noexcept {
  std::size_t total = 0;
  for (const sdk::PointsAnnotation& entry : annotations.points) {
    total += entry.points.size();
  }
  return total;
}

}  // namespace PJ::scripting::native
