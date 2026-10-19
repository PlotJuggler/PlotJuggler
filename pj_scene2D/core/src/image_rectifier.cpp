// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/image_rectifier.h"

#include <cmath>
#include <cstdint>

namespace PJ {
namespace {

// Bytes-per-pixel for the interleaved 8-bit formats we resample; 0 = unsupported
// (planar or multi-byte — the caller keeps the original frame).
[[nodiscard]] int interleavedChannels(PixelFormat fmt) noexcept {
  switch (fmt) {
    case PixelFormat::kRGB888:
    case PixelFormat::kBGR888:
      return 3;
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGRA8888:
      return 4;
    case PixelFormat::kMono8:
      return 1;
    case PixelFormat::kMono16:
    case PixelFormat::kYUV420P:
    case PixelFormat::kNV12:
    case PixelFormat::kDepthR32F:
      return 0;
  }
  return 0;
}

}  // namespace

std::optional<DecodedFrame> rectifyFrame(const DecodedFrame& src, const UndistortMap& map) {
  if (!map.valid() || !src.isValid()) {
    return std::nullopt;
  }
  const int ch = interleavedChannels(src.format);
  if (ch == 0) {
    return std::nullopt;  // unsupported format -> caller keeps original.
  }

  const int sw = src.width;
  const int sh = src.height;
  const std::vector<uint8_t>& sp = *src.pixels;
  const auto src_stride = static_cast<size_t>(sw) * static_cast<size_t>(ch);

  DecodedFrame out;
  out.width = map.out_width;
  out.height = map.out_height;
  out.format = src.format;
  out.pts = src.pts;
  out.stamp = src.stamp;
  out.entry_ts = src.entry_ts;
  out.frame_id = src.frame_id;
  out.pixels = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(map.out_width) * static_cast<size_t>(map.out_height) * static_cast<size_t>(ch), 0);
  std::vector<uint8_t>& op = *out.pixels;
  const auto out_stride = static_cast<size_t>(map.out_width) * static_cast<size_t>(ch);

  for (int v = 0; v < map.out_height; ++v) {
    for (int u = 0; u < map.out_width; ++u) {
      const size_t midx = static_cast<size_t>(v) * static_cast<size_t>(map.out_width) + static_cast<size_t>(u);
      const float fx = map.src_x[midx];
      const float fy = map.src_y[midx];

      // Bilinear: the 2x2 neighborhood must be fully inside the source.
      const float x0f = std::floor(fx);
      const float y0f = std::floor(fy);
      const int x0 = static_cast<int>(x0f);
      const int y0 = static_cast<int>(y0f);
      if (x0 < 0 || y0 < 0 || x0 + 1 >= sw || y0 + 1 >= sh) {
        continue;  // out of bounds -> leave black (buffer is zero-filled).
      }

      const float ax = fx - x0f;
      const float ay = fy - y0f;
      const float w00 = (1.0F - ax) * (1.0F - ay);
      const float w10 = ax * (1.0F - ay);
      const float w01 = (1.0F - ax) * ay;
      const float w11 = ax * ay;

      const uint8_t* p00 =
          &sp[static_cast<size_t>(y0) * src_stride + static_cast<size_t>(x0) * static_cast<size_t>(ch)];
      const uint8_t* p10 = p00 + ch;
      const uint8_t* p01 = p00 + src_stride;
      const uint8_t* p11 = p01 + ch;
      uint8_t* dst = &op[static_cast<size_t>(v) * out_stride + static_cast<size_t>(u) * static_cast<size_t>(ch)];
      for (int c = 0; c < ch; ++c) {
        const float val = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
        dst[c] = static_cast<uint8_t>(val + 0.5F);
      }
    }
  }
  return out;
}

bool rectifyFrameFast(const DecodedFrame& src, const UndistortMapFast& fast, DecodedFrame& out) {
  if (!fast.valid() || !src.isValid()) {
    return false;
  }
  const int ch = interleavedChannels(src.format);
  if (ch == 0) {
    return false;  // planar / 16-bit -> caller keeps the original frame.
  }
  // The table indexes a source of fast.src_width x fast.src_height; a frame of a
  // different size would read the wrong pixels (the caller rebuilds on size change).
  if (src.width != fast.src_width || src.height != fast.src_height) {
    return false;
  }

  const std::vector<uint8_t>& sp = *src.pixels;
  const auto src_stride = static_cast<size_t>(src.width) * static_cast<size_t>(ch);

  out.width = fast.out_width;
  out.height = fast.out_height;
  out.format = src.format;
  out.pts = src.pts;
  out.stamp = src.stamp;
  out.entry_ts = src.entry_ts;
  out.frame_id = src.frame_id;
  const size_t out_bytes =
      static_cast<size_t>(fast.out_width) * static_cast<size_t>(fast.out_height) * static_cast<size_t>(ch);
  if (out.pixels == nullptr) {
    out.pixels = std::make_shared<std::vector<uint8_t>>(out_bytes);
  } else {
    out.pixels->resize(out_bytes);  // reused buffer: no per-frame allocation on the steady state.
  }
  std::vector<uint8_t>& op = *out.pixels;

  const size_t n = static_cast<size_t>(fast.out_width) * static_cast<size_t>(fast.out_height);
  for (size_t i = 0; i < n; ++i) {
    uint8_t* dst = &op[i * static_cast<size_t>(ch)];
    const int32_t p0 = fast.src_p0[i];
    if (p0 < 0) {
      for (int c = 0; c < ch; ++c) {
        dst[c] = 0;  // out of bounds -> black (write explicitly; the buffer is reused).
      }
      continue;
    }
    const float ax = fast.frac_x[i];
    const float ay = fast.frac_y[i];
    const float w00 = (1.0F - ax) * (1.0F - ay);
    const float w10 = ax * (1.0F - ay);
    const float w01 = (1.0F - ax) * ay;
    const float w11 = ax * ay;
    const uint8_t* p00 = &sp[static_cast<size_t>(p0) * static_cast<size_t>(ch)];
    const uint8_t* p10 = p00 + ch;
    const uint8_t* p01 = p00 + src_stride;
    const uint8_t* p11 = p01 + ch;
    for (int c = 0; c < ch; ++c) {
      const float val = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
      dst[c] = static_cast<uint8_t>(val + 0.5F);
    }
  }
  return true;
}

}  // namespace PJ
