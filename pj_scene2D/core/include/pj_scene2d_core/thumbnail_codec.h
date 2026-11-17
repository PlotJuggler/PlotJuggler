#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

// Stateless JPEG thumbnail codec. Thumbnails stay YUV420P end to end (an NV12 input is deinterleaved first), so the
// media shader draws them like live frames.

inline constexpr int kThumbnailMaxWidth = 1280;
inline constexpr int kThumbnailJpegQuality = 80;

struct JpegThumbnail {
  std::vector<uint8_t> jpeg;  ///< Compressed JPEG bytes (empty on failure).
  int width = 0;              ///< Decoded thumbnail width (<= kThumbnailMaxWidth).
  int height = 0;             ///< Decoded thumbnail height.
};

/// Downscales to <= `max_width` (aspect kept, even dimensions) and JPEG-encodes. Empty `.jpeg` when `src` is not a
/// valid YUV420P or NV12 frame or the codec fails.
[[nodiscard]] JpegThumbnail encodeThumbnailJpeg(const DecodedFrame& src, int max_width, int quality);

/// Decompress a JPEG thumbnail (encoded by encodeThumbnailJpeg) back to a
/// YUV420P DecodedFrame of the given dimensions. Returns nullopt on failure.
[[nodiscard]] std::optional<DecodedFrame> decodeThumbnailJpeg(
    const uint8_t* jpeg, size_t jpeg_size, int width, int height);

}  // namespace PJ
