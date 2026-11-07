#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <png.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace PJ::test {
namespace detail {

struct PngWriteCtx {
  std::vector<uint8_t> bytes;
};

inline void pngWriteCallback(png_structp png, png_bytep data, png_size_t length) {
  auto* ctx = static_cast<PngWriteCtx*>(png_get_io_ptr(png));
  const auto* first = reinterpret_cast<const uint8_t*>(data);
  ctx->bytes.insert(ctx->bytes.end(), first, first + length);
}

inline void pngFlushCallback(png_structp /*png*/) {}

}  // namespace detail

template <typename Sample>
std::vector<uint8_t> makeGrayPng(int width, int height, int bit_depth, const std::vector<Sample>& values) {
  static_assert(std::is_same_v<Sample, uint8_t> || std::is_same_v<Sample, uint16_t>);
  if ((bit_depth != 8 && bit_depth != 16) || width <= 0 || height <= 0) {
    ADD_FAILURE() << "unsupported gray PNG geometry/bit depth";
    return {};
  }
  if (bit_depth == 16 && !std::is_same_v<Sample, uint16_t>) {
    ADD_FAILURE() << "16-bit gray PNG samples must be uint16_t";
    return {};
  }

  const auto pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (values.size() < pixel_count) {
    ADD_FAILURE() << "not enough gray PNG samples";
    return {};
  }

  detail::PngWriteCtx ctx;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  EXPECT_NE(png, nullptr);
  if (png == nullptr) {
    return {};
  }
  png_infop info = png_create_info_struct(png);
  EXPECT_NE(info, nullptr);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return {};
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }

  png_set_write_fn(png, &ctx, detail::pngWriteCallback, detail::pngFlushCallback);
  png_set_IHDR(
      png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), bit_depth, PNG_COLOR_TYPE_GRAY,
      PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png, info);
  std::vector<uint8_t> samples;
  if constexpr (std::is_same_v<Sample, uint16_t>) {
    if (bit_depth == 16) {
      samples.resize(pixel_count * 2);
      for (size_t i = 0; i < pixel_count; ++i) {
        samples[i * 2 + 0] = static_cast<uint8_t>((values[i] >> 8) & 0xFF);
        samples[i * 2 + 1] = static_cast<uint8_t>(values[i] & 0xFF);
      }
    } else {
      samples.resize(pixel_count);
      for (size_t i = 0; i < pixel_count; ++i) {
        samples[i] = static_cast<uint8_t>(values[i]);
      }
    }
  } else {
    samples.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(pixel_count));
  }

  const size_t row_bytes = static_cast<size_t>(width) * static_cast<size_t>(bit_depth / 8);
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] = samples.data() + static_cast<size_t>(y) * row_bytes;
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return ctx.bytes;
}

inline std::vector<uint8_t> makeRgbPng(int width, int height, bool with_alpha) {
  if (width <= 0 || height <= 0) {
    ADD_FAILURE() << "invalid RGB PNG geometry";
    return {};
  }

  const int channels = with_alpha ? 4 : 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels));
  for (size_t i = 0; i < pixels.size(); i += static_cast<size_t>(channels)) {
    pixels[i + 0] = 0;
    pixels[i + 1] = 255;
    pixels[i + 2] = 0;
    if (with_alpha) {
      pixels[i + 3] = 128;
    }
  }

  detail::PngWriteCtx ctx;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  EXPECT_NE(png, nullptr);
  if (png == nullptr) {
    return {};
  }
  png_infop info = png_create_info_struct(png);
  EXPECT_NE(info, nullptr);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return {};
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }

  png_set_write_fn(png, &ctx, detail::pngWriteCallback, detail::pngFlushCallback);
  png_set_IHDR(
      png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
      with_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
      PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(y)] =
        pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * static_cast<size_t>(channels);
  }
  png_write_image(png, rows.data());
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return ctx.bytes;
}

}  // namespace PJ::test
