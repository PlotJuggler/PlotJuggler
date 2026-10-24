// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/image_decoder.h"
#include "point_cloud_ops.h"

// Native operations over decoded pixels (pj_scripting/image_decoder.h), the heavy half of a script's image methods
// (docs/derived_objects/ARCHITECTURE.md §6). Luau-free, so the Luau and Python binders share them. Every operation
// first checks the pixels' shape (`validatePixels`); an allocating one charges its output to `limits.budget` before
// allocating it and polls `limits.cancelled` once per row.
namespace PJ::scripting::native {

/// How the source is stored, independent of how it decodes: "jpeg", "png", "raw:<encoding>" for the raw encodings
/// the decoders know, else "compressed:<encoding>".
[[nodiscard]] std::string imageRepresentation(const sdk::Image& image);
/// "depth:<encoding>".
[[nodiscard]] std::string imageRepresentation(const sdk::DepthImage& depth);

/// Rejects a zero size, a channel count other than 1, 3 or 4, and samples whose count is not
/// `width * height * channels` (checked without overflow).
[[nodiscard]] Status validatePixels(const DecodedPixels& image);

/// The `channels` samples of pixel (x, y), in channel order.
[[nodiscard]] Expected<std::vector<float>> imagePixel(const DecodedPixels& image, uint32_t x, uint32_t y);

/// Rec. 601 luma, in the samples' own range; a mono image is returned as is, sharing its samples and its charge.
[[nodiscard]] Expected<DecodedPixels> imageToGray(const DecodedPixels& image, const NativeLimits& limits);

/// The rectangle at (x, y), which must be non-empty and lie wholly inside the image. It keeps the source's pairing,
/// and its origin moves by (x, y), so what is found in it is reported at its coordinates in the source.
[[nodiscard]] Expected<DecodedPixels> imageCrop(
    const DecodedPixels& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const NativeLimits& limits);

/// One points annotation holding every pixel whose `channel` sample satisfies `sample <op> value` (a non-finite
/// sample never does), at its coordinates in the source image, paired as the pixels are (`image_topic`,
/// `timestamp_ns`); no annotation when nothing matches. All or nothing: a result over the budget fails whole.
[[nodiscard]] Expected<Charged<sdk::ImageAnnotations>> imageThreshold(
    const DecodedPixels& image, CompareOp op, double value, uint32_t channel, const NativeLimits& limits);

/// Over every finite sample of every channel; an image with none has no statistics (an error).
struct ImageStats {
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  std::size_t count = 0;
};
[[nodiscard]] Expected<ImageStats> imageStats(const DecodedPixels& image, const NativeLimits& limits);

/// The points of every points annotation, summed; a read, nothing copied.
[[nodiscard]] std::size_t countAnnotationPoints(const sdk::ImageAnnotations& annotations) noexcept;

}  // namespace PJ::scripting::native
