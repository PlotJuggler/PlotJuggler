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

struct UndistortMap;  // pj_scene2d_core/undistort_remap.h — only held by shared_ptr here.

/// Pixel format tag for decoded image data.
enum class PixelFormat : uint8_t {
  kRGB888,
  kRGBA8888,
  kBGR888,
  kBGRA8888,
  kMono8,
  kMono16,
  kYUV420P,
  kNV12,
  kDepthR32F,  // single-channel float32 metric depth; GPU-colormapped in the media shader
};

/// YUV→RGB luma-coefficient set for the GPU color-conversion matrix. Carried on a
/// decoded YUV frame so the shader applies the matrix the source was actually
/// encoded with instead of a hardcoded default (BT.601 for SD, BT.709 for HD —
/// applying the wrong one shifts saturated hues). Ignored by non-YUV formats.
enum class YuvColorSpace : uint8_t {
  kBt601,  ///< SMPTE 170M / BT.470BG — standard-definition video
  kBt709,  ///< BT.709 — high-definition video (the common default)
};

/// YUV sample range. Most camera/file H.264 is "limited" (luma 16..235, chroma
/// 16..240); "full" (0..255, a.k.a. JPEG/PC range) needs a different scale+offset.
/// Decoding limited-range pixels with a full-range matrix washes out blacks/whites.
enum class YuvColorRange : uint8_t {
  kLimited,  ///< studio / TV range (the common default)
  kFull,     ///< full / JPEG / PC range
};

/// Magnification (texture-min/mag) filter for displaying a frame zoomed past 1:1.
/// Set by the producing source, not user-selectable: image frames stamp kNearest
/// (crisp pixels for inspection), video uses the kLinear default (smooth), and depth
/// always samples nearest regardless (see media_viewer_widget): blending the no-data
/// sentinel is wrong.
enum class MagFilter : uint8_t {
  kLinear,   ///< bilinear interpolation (default)
  kNearest,  ///< nearest-neighbour ("pixelated")
};

/// Compute the expected pixel buffer size in bytes for a given format and dimensions.
/// Uses ceil(w/2), ceil(h/2) for chroma planes (correct for odd dimensions).
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

/// Per-frame parameters for GPU depth colormapping (PixelFormat::kDepthR32F). The
/// media shader maps raw metric depth (metres) through a colormap LUT: normalize
/// by [near_m, far_m], optionally invert, then look up colormap row. `active` is
/// true only for depth frames.
struct DepthColorParams {
  float near_m = 0.0f;
  float far_m = 1.0f;
  bool invert = false;
  uint8_t colormap = 0;  ///< colormap id (pj_widgets Colormap) == LUT row
  bool active = false;
};

/// Decoded pixel buffer produced by decoders and codec stages
/// and consumed by MediaViewerWidget for GPU upload.
///
/// Ownership: `pixels` is shared via shared_ptr, enabling zero-copy
/// handoff between pipeline stages and MediaSource delivery paths.
///
/// For YUV420P: pixels contains Y plane (w*h), then U plane
/// ((w+1)/2 * (h+1)/2), then V plane (same size) — contiguous.
/// Use expectedBufferSize() for correct allocation.
struct DecodedFrame {
  std::shared_ptr<std::vector<uint8_t>> pixels;  ///< Pixel data (contiguous, layout depends on format)
  int width = 0;                                 ///< Image width in pixels
  int height = 0;                                ///< Image height in pixels
  PixelFormat format = PixelFormat::kRGB888;     ///< Pixel layout in the buffer
  int64_t pts = -1;                              ///< Presentation timestamp (-1 if unknown)
  /// The source image's own stamp (its payload's `timestamp_ns`) on the dataset clock: what annotations made from the
  /// image carry, which may differ from `pts`, the store entry's time. Unset for a frame with no payload stamp (video).
  std::optional<int64_t> stamp;
  /// The time of the store entry the frame was decoded from, which a lookup among the topic's entries takes; `pts` can
  /// be a parser's record stamp or the image's own. Unset for a frame that came from no single entry (video).
  std::optional<int64_t> entry_ts;
  std::string frame_id;  ///< Source frame (from sdk::Image); lets a consumer find the CameraInfo.

  /// YUV→RGB conversion parameters (only meaningful for kYUV420P / kNV12). Set by
  /// the video decoder from the codec's signalled colorimetry; the GPU shader's
  /// color matrix is built from these. Defaults match the historical assumption
  /// (full-range BT.709) so non-decoder producers need not set them.
  YuvColorSpace color_space = YuvColorSpace::kBt709;
  YuvColorRange color_range = YuvColorRange::kFull;

  /// Display magnification filter (see MagFilter). A display hint, not a decode
  /// property; set by the layer that owns this stream.
  MagFilter mag_filter = MagFilter::kLinear;

  /// Deferred-rectification handle. When non-null this frame is RAW (still in
  /// source pixel space, `width`/`height` = source size) and the consumer must
  /// rectify it through this map before display — the GPU path, where the widget
  /// undistorts at draw time. The map's `out_width`/`out_height` is the logical
  /// (rectified) display size that annotation/aspect/inspector coordinate spaces
  /// use. Null means the frame is already display-ready (CPU path or no
  /// calibration). Shared + immutable: many frames of one camera reuse it.
  std::shared_ptr<const UndistortMap> rectify_map;

  /// GPU depth-colormap parameters; `depth.active` is set only for kDepthR32F frames.
  DepthColorParams depth;

  /// True if no pixel data is present (null or empty buffer).
  [[nodiscard]] bool isNull() const noexcept {
    return pixels == nullptr || pixels->empty();
  }

  /// True if pixels, dimensions, and format are mutually consistent.
  [[nodiscard]] bool isValid() const noexcept {
    return !isNull() && width > 0 && height > 0 && pixels->size() == expectedBufferSize(width, height, format);
  }
};

/// Deinterleave an NV12 frame into a planar YUV420P frame, preserving all metadata
/// (colorimetry, pts, frame_id, mag filter, rectify map). NV12 is the native
/// hardware-decode download format; the GPU display path uploads it directly, but
/// CPU consumers that only understand planar YUV420P (e.g. the thumbnail encoder, or
/// a backend lacking RG8 textures) call this first. Returns a null frame if `nv12`
/// isn't a well-formed NV12 frame.
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
  std::memcpy(dst, src, static_cast<size_t>(y_size));  // Y plane is identical
  const uint8_t* uv = src + y_size;
  uint8_t* u_out = dst + y_size;
  uint8_t* v_out = dst + y_size + uv_size;
  for (int i = 0; i < uv_size; ++i) {
    u_out[i] = uv[i * 2 + 0];
    v_out[i] = uv[i * 2 + 1];
  }

  DecodedFrame out = nv12;  // copy metadata (colour space/range, pts, frame_id, mag, rectify_map)
  out.pixels = std::move(pixels);
  out.format = PixelFormat::kYUV420P;
  return out;
}

}  // namespace PJ
