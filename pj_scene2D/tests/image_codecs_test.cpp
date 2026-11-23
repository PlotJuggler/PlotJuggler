// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "pj_scene2d_core/codecs.h"
#include "test_png_io.h"

namespace PJ {
namespace {

std::vector<uint8_t> createTestJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width * height * 3));
  for (size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = 255;
    rgb[i + 1] = 0;
    rgb[i + 2] = 0;
  }

  tjhandle compressor = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;  // NOLINT(google-runtime-int)
  tjCompress2(
      compressor, rgb.data(), width, width * 3, height, TJPF_RGB, &jpeg_buf, &jpeg_size, TJSAMP_420, 80,
      TJFLAG_FASTUPSAMPLE);
  std::vector<uint8_t> result(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  tjDestroy(compressor);
  return result;
}

DecodedFrame compressedFrame(const std::vector<uint8_t>& bytes) {
  DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(bytes);
  return frame;
}

// --- JPEG tests ---

TEST(ImageCodecTest, DecodeValidJpeg) {
  ImageDecodeCascade decoder;
  auto jpeg = createTestJpeg(64, 48);
  auto result = decoder.decode(compressedFrame(jpeg));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->width, 64);
  EXPECT_EQ(result->height, 48);
  EXPECT_EQ(result->format, PixelFormat::kRGB888);
  EXPECT_FALSE(result->isNull());
  EXPECT_GT((*result->pixels)[0], 200);
  EXPECT_LT((*result->pixels)[1], 50);
}

// The RGBA output is the RGB one with an opaque alpha: the viewer uploads it as it is.
TEST(ImageCodecTest, DecodeJpegToRgbaMatchesRgb) {
  const auto jpeg = createTestJpeg(64, 48);
  const auto rgb = JpegCodec{}.decode(compressedFrame(jpeg));
  const auto rgba = JpegCodec{ColorOutput::kRgba}.decode(compressedFrame(jpeg));
  ASSERT_TRUE(rgb.has_value()) << rgb.error();
  ASSERT_TRUE(rgba.has_value()) << rgba.error();
  EXPECT_EQ(rgba->format, PixelFormat::kRGBA8888);
  EXPECT_EQ(rgba->width, 64);
  EXPECT_EQ(rgba->height, 48);
  ASSERT_EQ(rgba->pixels->size(), 64u * 48u * 4u);
  for (size_t pixel = 0; pixel < 64u * 48u; ++pixel) {
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 0], (*rgb->pixels)[pixel * 3 + 0]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 1], (*rgb->pixels)[pixel * 3 + 1]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 2], (*rgb->pixels)[pixel * 3 + 2]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 3], 255) << pixel;
  }
}

TEST(ImageCodecTest, DecodeEmptyInputFails) {
  JpegCodec decoder;
  auto result = decoder.decode({});
  EXPECT_FALSE(result.has_value());
}

TEST(ImageCodecTest, DecodeCorruptInputFails) {
  JpegCodec decoder;
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xD8, 0xFF, 0xE0};
  auto result = decoder.decode(compressedFrame(garbage));
  EXPECT_FALSE(result.has_value());
}

// --- PNG tests ---

// An opaque PNG gains an opaque alpha; one with its own alpha keeps it; 16-bit grayscale stays Mono16.
TEST(ImageCodecTest, DecodePngToRgba) {
  const PngCodec rgb_decoder;
  const PngCodec rgba_decoder{ColorOutput::kRgba};

  const auto opaque = PJ::test::makeRgbPng(32, 24, false);
  const auto rgb = rgb_decoder.decode(compressedFrame(opaque));
  const auto rgba = rgba_decoder.decode(compressedFrame(opaque));
  ASSERT_TRUE(rgb.has_value()) << rgb.error();
  ASSERT_TRUE(rgba.has_value()) << rgba.error();
  ASSERT_EQ(rgb->format, PixelFormat::kRGB888);
  ASSERT_EQ(rgba->format, PixelFormat::kRGBA8888);
  ASSERT_EQ(rgba->pixels->size(), 32u * 24u * 4u);
  for (size_t pixel = 0; pixel < 32u * 24u; ++pixel) {
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 0], (*rgb->pixels)[pixel * 3 + 0]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 1], (*rgb->pixels)[pixel * 3 + 1]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 2], (*rgb->pixels)[pixel * 3 + 2]) << pixel;
    ASSERT_EQ((*rgba->pixels)[pixel * 4 + 3], 255) << pixel;
  }

  const auto translucent = PJ::test::makeRgbPng(32, 24, true);
  const auto kept = rgba_decoder.decode(compressedFrame(translucent));
  const auto reference = rgb_decoder.decode(compressedFrame(translucent));
  ASSERT_TRUE(kept.has_value()) << kept.error();
  ASSERT_TRUE(reference.has_value()) << reference.error();
  EXPECT_EQ(kept->format, PixelFormat::kRGBA8888);
  EXPECT_EQ(*kept->pixels, *reference->pixels);
}

TEST(ImageCodecTest, DecodePngRgb) {
  PngCodec decoder;
  auto png = PJ::test::makeRgbPng(32, 24, false);
  auto result = decoder.decode(compressedFrame(png));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->width, 32);
  EXPECT_EQ(result->height, 24);
  EXPECT_EQ(result->format, PixelFormat::kRGB888);
  EXPECT_LT((*result->pixels)[0], 10);
  EXPECT_GT((*result->pixels)[1], 240);
  EXPECT_LT((*result->pixels)[2], 10);
}

TEST(ImageCodecTest, DecodePngRgba) {
  PngCodec decoder;
  auto png = PJ::test::makeRgbPng(16, 16, true);
  auto result = decoder.decode(compressedFrame(png));
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->format, PixelFormat::kRGBA8888);
  EXPECT_EQ((*result->pixels)[3], 128);
}

TEST(ImageCodecTest, DecodePngEmptyFails) {
  PngCodec decoder;
  auto result = decoder.decode({});
  EXPECT_FALSE(result.has_value());
}

TEST(ImageCodecTest, DecodePngCorruptFails) {
  PngCodec decoder;
  std::vector<uint8_t> garbage = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00};
  auto result = decoder.decode(compressedFrame(garbage));
  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace PJ
