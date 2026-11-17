#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PJ {

struct UndistortMap;

enum class PixelFormat : uint8_t {
  kRGB888,
  kRGBA8888,
  kBGR888,
  kBGRA8888,
  kMono8,
  kMono16,
  kYUV420P,
  kNV12,
  kDepthR32F,  // float32 metres; 0 = no data
};

/// The YUV→RGB coefficients a YUV frame was encoded with (the wrong one shifts saturated hues).
enum class YuvColorSpace : uint8_t {
  kBt601,  ///< SMPTE 170M / BT.470BG
  kBt709,
};

/// Limited: luma 16..235, chroma 16..240. Full: 0..255 (JPEG range).
enum class YuvColorRange : uint8_t {
  kLimited,
  kFull,
};

/// Display filter, set by the producing source: images use kNearest, video kLinear. Depth is always sampled
/// nearest, whatever this says: blending would mix the no-data 0 into valid depths.
enum class MagFilter : uint8_t {
  kLinear,
  kNearest,
};

/// Buffer size in bytes; chroma planes are ceil(w/2) x ceil(h/2). Size every YUV/NV12 allocation with it, or an odd
/// dimension overflows the buffer.
[[nodiscard]] inline size_t expectedBufferSize(int width, int height, PixelFormat format) noexcept {
  auto w = static_cast<size_t>(width);
  auto h = static_cast<size_t>(height);
  switch (format) {
    case PixelFormat::kRGB888:
    case PixelFormat::kBGR888:
      return w * h * 3;
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGRA8888:
      return w * h * 4;
    case PixelFormat::kMono8:
      return w * h;
    case PixelFormat::kMono16:
      return w * h * 2;
    case PixelFormat::kYUV420P: {
      size_t uv_w = (w + 1) / 2;
      size_t uv_h = (h + 1) / 2;
      return w * h + 2 * uv_w * uv_h;
    }
    case PixelFormat::kNV12: {
      size_t uv_w = (w + 1) / 2;
      size_t uv_h = (h + 1) / 2;
      // Y plane (w*h) + interleaved UV plane: uv_w UV-pairs per row (2 bytes each),
      // uv_h rows. Equals w*h + w*uv_h only for even widths.
      return w * h + 2 * uv_w * uv_h;
    }
    case PixelFormat::kDepthR32F:
      return w * h * 4;
  }
  return 0;
}

/// How the media shader colours a kDepthR32F frame: normalize by [near_m, far_m], optionally invert, look up the
/// colormap row. `active` only on depth frames.
struct DepthColorParams {
  float near_m = 0.0f;
  float far_m = 1.0f;
  bool invert = false;
  uint8_t colormap = 0;  ///< pj_widgets Colormap value == LUT row
  bool active = false;
};

/// A decoded image. `pixels` is shared and never mutated after delivery: the viewer skips the upload when a frame
/// brings the buffer it already holds, so a producer hands out a fresh buffer per decoded frame. YUV420P is the Y
/// plane, then U, then V, contiguous.
struct DecodedFrame {
  std::shared_ptr<std::vector<uint8_t>> pixels;
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::kRGB888;
  int64_t pts = -1;  ///< ns; -1 = unknown
  /// The source image's own stamp (its payload's `timestamp_ns`) on the dataset clock: what annotations made from the
  /// image carry, which may differ from `pts`, the store entry's time. Unset for a frame with no payload stamp (video).
  std::optional<int64_t> stamp;
  /// The time of the store entry the frame was decoded from, which a lookup among the topic's entries takes; `pts` can
  /// be a parser's record stamp or the image's own. Unset for a frame that came from no single entry (video).
  std::optional<int64_t> entry_ts;
  std::string frame_id;  ///< From sdk::Image; keys the CameraInfo lookup.

  /// YUV formats only; the video decoder sets them from the codec's colorimetry.
  YuvColorSpace color_space = YuvColorSpace::kBt709;
  YuvColorRange color_range = YuvColorRange::kFull;

  MagFilter mag_filter = MagFilter::kLinear;

  /// Non-null: the frame is raw (`width`/`height` = source size) and the consumer rectifies it through this map; the
  /// map's `out_width`/`out_height` is the logical size annotations, aspect and inspector use. Null: display-ready.
  std::shared_ptr<const UndistortMap> rectify_map;

  DepthColorParams depth;

  [[nodiscard]] bool isNull() const noexcept {
    return pixels == nullptr || pixels->empty();
  }

  [[nodiscard]] bool isValid() const noexcept {
    return !isNull() && width > 0 && height > 0 && pixels->size() == expectedBufferSize(width, height, format);
  }
};

/// Keeps every metadata field. For consumers of planar YUV only (the thumbnail encoder, a backend without RG8
/// textures). A null frame when `nv12` is not well-formed NV12.
[[nodiscard]] inline DecodedFrame nv12ToYuv420p(const DecodedFrame& nv12) {
  const int w = nv12.width;
  const int h = nv12.height;
  if (nv12.format != PixelFormat::kNV12 || nv12.pixels == nullptr ||
      nv12.pixels->size() < expectedBufferSize(w, h, PixelFormat::kNV12)) {
    return {};
  }
  const int uv_w = (w + 1) / 2;
  const int uv_h = (h + 1) / 2;
  const int y_size = w * h;
  const int uv_size = uv_w * uv_h;
  const uint8_t* src = nv12.pixels->data();

  auto pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(w, h, PixelFormat::kYUV420P));
  uint8_t* dst = pixels->data();
  std::memcpy(dst, src, static_cast<size_t>(y_size));
  const uint8_t* uv = src + y_size;
  uint8_t* u_out = dst + y_size;
  uint8_t* v_out = dst + y_size + uv_size;
  for (int i = 0; i < uv_size; ++i) {
    u_out[i] = uv[i * 2 + 0];
    v_out[i] = uv[i * 2 + 1];
  }

  DecodedFrame out = nv12;
  out.pixels = std::move(pixels);
  out.format = PixelFormat::kYUV420P;
  return out;
}

}  // namespace PJ
