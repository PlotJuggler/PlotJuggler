// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/codecs.h"

#include <png.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace PJ {

// ---------------------------------------------------------------------------
// JpegCodec
// ---------------------------------------------------------------------------

JpegCodec::JpegCodec() : tj_(tjInitDecompress()) {}

JpegCodec::~JpegCodec() {
  if (tj_ != nullptr) {
    tjDestroy(tj_);
  }
}

Expected<DecodedFrame> JpegCodec::decode(const DecodedFrame& input) const {
  if (tj_ == nullptr) {
    return unexpected("turbojpeg not initialized");
  }
  if (input.isNull()) {
    return unexpected("empty input");
  }
  const auto* data = input.pixels->data();
  auto size = static_cast<unsigned long>(input.pixels->size());

  int width = 0;
  int height = 0;
  int subsamp = 0;
  if (tjDecompressHeader2(static_cast<tjhandle>(tj_), const_cast<uint8_t*>(data), size, &width, &height, &subsamp) !=
      0) {
    return unexpected(std::string("JPEG header failed: ") + tjGetErrorStr());
  }

  // Reject crafted/corrupt geometry before allocating the pixel buffer from it.
  if (!imageDimensionsWithinLimit(width, height)) {
    return unexpected("JPEG image dimensions exceed limit: " + std::to_string(width) + "x" + std::to_string(height));
  }

  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
  if (tjDecompress2(
          static_cast<tjhandle>(tj_), const_cast<uint8_t*>(data), size, pixels->data(), width, width * 3, height,
          TJPF_RGB, TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT) != 0) {
    return unexpected(std::string("JPEG decode failed: ") + tjGetErrorStr());
  }

  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = width;
  frame.height = height;
  frame.format = PixelFormat::kRGB888;
  return frame;
}

// ---------------------------------------------------------------------------
// PngCodec
// ---------------------------------------------------------------------------

namespace {

struct PngReadCtx {
  const uint8_t* data;
  size_t size;
  size_t offset;
};

void pngReadCb(png_structp png, png_bytep out, png_size_t count) {
  auto* ctx = static_cast<PngReadCtx*>(png_get_io_ptr(png));
  if (ctx->offset + count > ctx->size) {
    png_error(png, "read past end");
    return;
  }
  std::memcpy(out, ctx->data + ctx->offset, count);
  ctx->offset += count;
}

// Runs png_read_image under libpng's setjmp inside ITS OWN frame, so no caller
// local (the pixel buffer, the row-pointer vector, the format enum) lives across
// the longjmp. That both (a) lets the caller's RAII objects unwind normally on a
// corrupt/truncated IDAT — fixing the historical leak of placing setjmp before the
// allocations — and (b) avoids GCC's -Wclobbered on those locals. Returns false if
// libpng raised an error while reading the image.
[[nodiscard]] bool pngReadImageGuarded(png_structp png, png_bytep* rows) {
  // NOLINTNEXTLINE(cert-err52-cpp) — libpng's only error channel is setjmp/longjmp.
  if (setjmp(png_jmpbuf(png))) {
    return false;
  }
  png_read_image(png, rows);
  return true;
}

}  // namespace

Expected<DecodedFrame> PngCodec::decode(const DecodedFrame& input) const {
  if (input.isNull() || input.pixels->size() < 8) {
    return unexpected("empty input");
  }
  const auto* data = input.pixels->data();
  auto size = input.pixels->size();

  if (png_sig_cmp(data, 0, 8) != 0) {
    return unexpected("not a PNG");
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    return unexpected("png_create_read_struct failed");
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    return unexpected("png_create_info_struct failed");
  }

  // Region 1 — header. Only fixed-size libpng state exists here, so a longjmp out
  // of png_read_info/png_read_update_info leaks nothing.
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    return unexpected("PNG header read failed");
  }

  PngReadCtx ctx{data, size, 0};
  png_set_read_fn(png, &ctx, pngReadCb);
  png_read_info(png, info);

  auto width = static_cast<int>(png_get_image_width(png, info));
  auto height = static_cast<int>(png_get_image_height(png, info));

  // Reject crafted/corrupt geometry BEFORE allocating the pixel buffer from it.
  if (!imageDimensionsWithinLimit(width, height)) {
    png_destroy_read_struct(&png, &info, nullptr);
    return unexpected("PNG image dimensions exceed limit: " + std::to_string(width) + "x" + std::to_string(height));
  }

  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth = png_get_bit_depth(png, info);

  bool is_mono16 = (color_type == PNG_COLOR_TYPE_GRAY && bit_depth == 16);

  if (is_mono16) {
    if constexpr (std::endian::native == std::endian::little) {
      png_set_swap(png);
    }
    png_read_update_info(png, info);
  } else {
    if (bit_depth == 16) {
      png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
      png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
      png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
      png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);
  }

  PixelFormat fmt;
  int channels;
  if (is_mono16) {
    fmt = PixelFormat::kMono16;
    channels = 2;
  } else {
    bool has_alpha = (png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA) != 0;
    channels = has_alpha ? 4 : 3;
    fmt = has_alpha ? PixelFormat::kRGBA8888 : PixelFormat::kRGB888;
  }

  auto row_bytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(height) * row_bytes);
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] = pixels->data() + static_cast<size_t>(y) * row_bytes;
  }

  // The pixel read runs its setjmp in pngReadImageGuarded's own frame, so a
  // corrupt/truncated IDAT unwinds `pixels`/`rows` here normally (no leak).
  if (!pngReadImageGuarded(png, rows.data())) {
    png_destroy_read_struct(&png, &info, nullptr);
    return unexpected("PNG image read failed");
  }
  png_destroy_read_struct(&png, &info, nullptr);

  DecodedFrame frame;
  frame.pixels = std::move(pixels);
  frame.width = width;
  frame.height = height;
  frame.format = fmt;
  return frame;
}

// ---------------------------------------------------------------------------
// NormalizeMono16
// ---------------------------------------------------------------------------

Expected<DecodedFrame> NormalizeMono16::decode(const DecodedFrame& input) const {
  if (input.isNull()) {
    return unexpected("empty image data");
  }
  if (input.format == PixelFormat::kMono16 && input.width > 0 && input.height > 0) {
    return mono16_to_grayscale_.decode(input);
  }
  return input;
}

// ---------------------------------------------------------------------------
// ImageDecodeCascade
// ---------------------------------------------------------------------------

Expected<DecodedFrame> ImageDecodeCascade::decode(const DecodedFrame& input) const {
  if (input.isNull()) {
    return unexpected("empty image data");
  }

  const auto* data = input.pixels->data();
  const auto size = input.pixels->size();
  switch (sniffImageContainer(data, size)) {
    case ImageContainer::kJpeg:
      return jpeg_.decode(input);
    case ImageContainer::kPng:
      return png_.decode(input);
    case ImageContainer::kUnknown:
      break;
  }

  return unexpected("unsupported image payload: expected JPEG or PNG");
}

// ---------------------------------------------------------------------------
// AutoImageCodec
// ---------------------------------------------------------------------------

Expected<DecodedFrame> AutoImageCodec::decode(const DecodedFrame& input) const {
  if (input.isNull()) {
    return unexpected("empty image data");
  }

  if (input.format == PixelFormat::kMono16 && input.width > 0 && input.height > 0) {
    return normalize_.decode(input);
  }

  auto decoded = decode_.decode(input);
  if (!decoded.has_value()) {
    return decoded;
  }
  return normalize_.decode(*decoded);
}

// ---------------------------------------------------------------------------
// Mono16ToGrayscale
// ---------------------------------------------------------------------------

Expected<DecodedFrame> Mono16ToGrayscale::decode(const DecodedFrame& input) const {
  // Generic uint16 grayscale for Image/PNG mono16 topics; DepthImage keeps a separate float/colormap path.
  if (input.isNull()) {
    return unexpected("empty mono16 data");
  }
  if (input.format != PixelFormat::kMono16 || input.width == 0 || input.height == 0) {
    return unexpected("expected mono16 input with valid dimensions");
  }

  auto pixel_count = static_cast<size_t>(input.width) * static_cast<size_t>(input.height);
  if (input.pixels->size() < pixel_count * 2) {
    return unexpected("mono16 buffer too small for dimensions");
  }
  const auto* src = reinterpret_cast<const uint16_t*>(input.pixels->data());

  uint16_t min_val = 65535;
  uint16_t max_val = 0;
  for (size_t i = 0; i < pixel_count; ++i) {
    if (src[i] > 0) {
      min_val = std::min(min_val, src[i]);
      max_val = std::max(max_val, src[i]);
    }
  }
  if (max_val <= min_val) {
    max_val = min_val + 1;
  }

  auto out = std::make_shared<std::vector<uint8_t>>(pixel_count * 3);
  float scale = 255.0f / static_cast<float>(max_val - min_val);
  for (size_t i = 0; i < pixel_count; ++i) {
    uint8_t val = 0;
    if (src[i] > 0) {
      val = static_cast<uint8_t>(std::clamp(static_cast<float>(src[i] - min_val) * scale, 0.0f, 255.0f));
    }
    (*out)[i * 3 + 0] = val;
    (*out)[i * 3 + 1] = val;
    (*out)[i * 3 + 2] = val;
  }

  DecodedFrame frame;
  frame.pixels = std::move(out);
  frame.width = input.width;
  frame.height = input.height;
  frame.format = PixelFormat::kRGB888;
  return frame;
}

// ---------------------------------------------------------------------------
// SegmentationPalette
// ---------------------------------------------------------------------------

Expected<DecodedFrame> SegmentationPalette::decode(const DecodedFrame& input) const {
  if (input.isNull() || input.width == 0 || input.height == 0) {
    return unexpected("empty segmentation data");
  }
  if (input.format != PixelFormat::kMono8) {
    return unexpected("expected mono8 input for segmentation palette");
  }
  auto pixel_count_check = static_cast<size_t>(input.width) * static_cast<size_t>(input.height);
  if (input.pixels->size() < pixel_count_check) {
    return unexpected("segmentation buffer too small for dimensions");
  }

  static const auto palette = []() {
    std::array<std::array<uint8_t, 3>, 256> pal{};
    pal[0] = {0, 0, 0};
    for (int i = 1; i < 256; ++i) {
      int hue = (i * 37) % 256;
      int sector = hue / 43;
      int frac = (hue % 43) * 6;
      int r = 0;
      int g = 0;
      int b = 0;
      switch (sector) {
        case 0:
          r = 255;
          g = frac;
          break;
        case 1:
          r = 255 - frac;
          g = 255;
          break;
        case 2:
          g = 255;
          b = frac;
          break;
        case 3:
          g = 255 - frac;
          b = 255;
          break;
        case 4:
          r = frac;
          b = 255;
          break;
        default:
          r = 255;
          b = 255 - frac;
          break;
      }
      pal[static_cast<size_t>(i)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
    }
    return pal;
  }();

  auto pixel_count = static_cast<size_t>(input.width) * static_cast<size_t>(input.height);
  auto out = std::make_shared<std::vector<uint8_t>>(pixel_count * 3);
  for (size_t i = 0; i < pixel_count; ++i) {
    auto id = (*input.pixels)[i];
    (*out)[i * 3 + 0] = palette[id][0];
    (*out)[i * 3 + 1] = palette[id][1];
    (*out)[i * 3 + 2] = palette[id][2];
  }

  DecodedFrame frame;
  frame.pixels = std::move(out);
  frame.width = input.width;
  frame.height = input.height;
  frame.format = PixelFormat::kRGB888;
  return frame;
}

// ---------------------------------------------------------------------------
// BayerDecode
// ---------------------------------------------------------------------------

namespace {

// Channel sampled by the CFA at (row, col): 0=R, 1=G, 2=B. Indexed by the
// top-left 2x2 tile of each pattern.
int cfaChannel(BayerPattern pattern, int row, int col) noexcept {
  static constexpr int kTiles[4][2][2] = {
      {{0, 1}, {1, 2}},  // RGGB
      {{1, 0}, {2, 1}},  // GRBG
      {{1, 2}, {0, 1}},  // GBRG
      {{2, 1}, {1, 0}},  // BGGR
  };
  return kTiles[static_cast<int>(pattern)][row & 1][col & 1];
}

}  // namespace

Expected<DecodedFrame> BayerDecode::decode(const DecodedFrame& input) const {
  if (input.isNull()) {
    return unexpected("empty bayer input");
  }
  if (input.format != PixelFormat::kMono8) {
    return unexpected("BayerDecode expects a kMono8 mosaic");
  }
  const int w = input.width;
  const int h = input.height;
  if (w <= 0 || h <= 0) {
    return unexpected("invalid bayer dimensions");
  }
  const auto& src = *input.pixels;
  if (src.size() < static_cast<size_t>(w) * static_cast<size_t>(h)) {
    return unexpected("bayer buffer too small for dimensions");
  }

  auto out = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int sum[3] = {0, 0, 0};
      int cnt[3] = {0, 0, 0};
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = std::clamp(x + dx, 0, w - 1);
          const int ny = std::clamp(y + dy, 0, h - 1);
          const int ch = cfaChannel(pattern_, ny, nx);
          sum[ch] += src[static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx)];
          ++cnt[ch];
        }
      }
      // The channel sampled at this pixel is known exactly, so pass it through
      // instead of averaging it with same-channel neighbours (which would soften
      // the measured channel). Only the two missing channels are interpolated.
      const int center_ch = cfaChannel(pattern_, y, x);
      const uint8_t center_val = src[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
      const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
      for (int ch = 0; ch < 3; ++ch) {
        (*out)[o + static_cast<size_t>(ch)] =
            ch == center_ch ? center_val : (cnt[ch] > 0 ? static_cast<uint8_t>(sum[ch] / cnt[ch]) : 0);
      }
    }
  }

  DecodedFrame frame;
  frame.pixels = std::move(out);
  frame.width = w;
  frame.height = h;
  frame.format = PixelFormat::kRGB888;
  frame.pts = input.pts;
  return frame;
}

// ---------------------------------------------------------------------------
// Pipeline builders
// ---------------------------------------------------------------------------

std::unique_ptr<CodecPipeline> makeJpegPipeline() {
  auto p = std::make_unique<CodecPipeline>();
  p->addStage(std::make_unique<JpegCodec>());
  return p;
}

std::optional<std::shared_ptr<std::vector<uint8_t>>> recoverContainerRawSamples(const uint8_t* data, size_t size) {
  const ImageContainer container = sniffImageContainer(data, size);
  if (container == ImageContainer::kUnknown) {
    return std::nullopt;  // not a container — caller treats the bytes as already raw
  }
  DecodedFrame staged;
  staged.pixels = std::make_shared<std::vector<uint8_t>>(data, data + size);
  const Expected<DecodedFrame> decoded =
      (container == ImageContainer::kJpeg) ? JpegCodec{}.decode(staged) : PngCodec{}.decode(staged);
  if (!decoded.has_value() || decoded->isNull() || decoded->width <= 0 || decoded->height <= 0) {
    return std::nullopt;
  }
  const DecodedFrame& frame = *decoded;
  const size_t samples = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
  auto out = std::make_shared<std::vector<uint8_t>>();
  switch (frame.format) {
    case PixelFormat::kMono16:
      // 16-bit grayscale already IS the flat 2-byte-per-sample buffer.
    case PixelFormat::kMono8:
      *out = *frame.pixels;
      break;
    case PixelFormat::kRGB888:
    case PixelFormat::kBGR888: {
      // PngCodec expands 8-bit grayscale to RGB (R==G==B); collapse to the luma byte.
      if (frame.pixels->size() < samples * 3) {
        return std::nullopt;
      }
      out->resize(samples);
      for (size_t i = 0; i < samples; ++i) {
        (*out)[i] = (*frame.pixels)[i * 3];
      }
      break;
    }
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGRA8888: {
      if (frame.pixels->size() < samples * 4) {
        return std::nullopt;
      }
      out->resize(samples);
      for (size_t i = 0; i < samples; ++i) {
        (*out)[i] = (*frame.pixels)[i * 4];
      }
      break;
    }
    default:
      return std::nullopt;  // YUV/NV12/DepthR32F aren't container-wrapped raw frames
  }
  return out;
}

// ---------------------------------------------------------------------------
// Container headers
// ---------------------------------------------------------------------------

ImageHeader peekImageHeader(const uint8_t* data, size_t size, ImageContainer container) noexcept {
  if (container == ImageContainer::kPng) {
    if (size < 24) {
      return {};
    }
    const auto be32 = [](const uint8_t* bytes) {
      return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | bytes[3];
    };
    return {be32(data + 16), be32(data + 20)};
  }
  if (container != ImageContainer::kJpeg) {
    return {};
  }
  ImageHeader header;
  bool progressive = false;
  for (size_t at = 2; at + 3 < size;) {
    if (data[at++] != 0xff) {
      break;
    }
    while (at < size && data[at] == 0xff) {
      ++at;
    }
    if (at >= size) {
      break;
    }
    const uint8_t marker = data[at++];
    if (marker == 0xd9) {  // end of image
      break;
    }
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {  // markers without a length
      continue;
    }
    if (at + 1 >= size) {
      break;
    }
    const size_t length = (size_t{data[at]} << 8) | data[at + 1];
    if (length < 2 || length > size - at) {
      break;
    }
    if (marker == 0xda) {  // the first scan: does it hold every component?
      if (header.width == 0 || length < 3) {
        return {};
      }
      header.multi_scan = progressive || data[at + 2] < header.components;
      return header;
    }
    const bool sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                     (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
    if (sof && header.width == 0) {
      if (length < 8) {
        return {};
      }
      header.width = (uint32_t{data[at + 5]} << 8) | data[at + 6];
      header.height = (uint32_t{data[at + 3]} << 8) | data[at + 4];
      header.components = data[at + 7];
      header.multi_scan = true;  // until the first scan says otherwise
      progressive = marker == 0xc2 || marker == 0xc6 || marker == 0xca || marker == 0xce;
    }
    at += length;
  }
  return header;  // a frame with no scan found stays multi-scan: the bound covers it
}

size_t codecWorkspaceBound(const ImageHeader& header, ImageContainer container) noexcept {
  constexpr size_t kLibraryState = size_t{64} << 10;  // zlib's window and inflate state, libjpeg's tables
  const size_t row_pointers = size_t{header.height} * sizeof(void*);
  if (container == ImageContainer::kPng) {
    return row_pointers + 2 * (size_t{header.width} * 8 + 1) + kLibraryState;  // a row and the previous one
  }
  // The coefficient buffer is 2 bytes per sample, blocks padded to 16.
  const size_t padded_width = (size_t{header.width} + 15) / 16 * 16;
  const size_t padded_height = (size_t{header.height} + 15) / 16 * 16;
  const size_t coefficients =
      header.multi_scan ? padded_width * padded_height * header.components * sizeof(int16_t) : 0;
  return row_pointers + coefficients + padded_width * header.components * 64 + kLibraryState;
}

}  // namespace PJ
