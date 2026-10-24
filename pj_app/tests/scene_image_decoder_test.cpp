// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SceneImageDecoder, the codec on-demand scripts decode through (docs/archive/derived_objects/SCENARIOS.md, rows
// MED-11, MED-12, BUD-12, GAP-26): the raw encodings, Scene2D's JPEG and PNG codecs, and the charges taken before
// either runs.

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "SceneImageDecoder.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/object_script.h"
#include "test_png_io.h"

namespace PJ {
namespace {

/// A `width` x `height` JPEG of a gradient, encoded directly with turbojpeg.
std::vector<uint8_t> makeJpeg(int width, int height, bool progressive = false) {
  std::vector<uint8_t> rgb(std::size_t(width) * std::size_t(height) * 3);
  for (std::size_t index = 0; index < rgb.size(); ++index) {
    rgb[index] = static_cast<uint8_t>(index * 7);
  }
  tjhandle compressor = tjInitCompress();
  unsigned char* buffer = nullptr;
  unsigned long size = 0;  // NOLINT(google-runtime-int): turbojpeg's type
  const int status = tjCompress2(
      compressor, rgb.data(), width, width * 3, height, TJPF_RGB, &buffer, &size, TJSAMP_420, 80,
      TJFLAG_FASTDCT | (progressive ? TJFLAG_PROGRESSIVE : 0));
  std::vector<uint8_t> jpeg;
  if (status == 0) {
    jpeg.assign(buffer, buffer + size);
  }
  tjFree(buffer);
  tjDestroy(compressor);
  return jpeg;
}

constexpr std::array<uint8_t, 2> kSof0{0xff, 0xc0};

sdk::Image compressed(const char* encoding, const std::vector<uint8_t>& bytes) {
  sdk::Image image;
  image.encoding = encoding;
  image.data = {bytes.data(), bytes.size()};
  return image;
}

TEST(SceneImageDecoder, HonorsRawStrideChannelOrderAndBounds) {
  std::array<uint8_t, 8> bytes{3, 2, 1, 99, 6, 5, 4, 99};
  sdk::Image image;
  image.width = 1;
  image.height = 2;
  image.row_step = 4;
  image.encoding = "bgr8";
  image.data = {bytes.data(), bytes.size()};
  const SceneImageDecoder decoder;
  auto decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->channels, 3u);
  EXPECT_EQ(*decoded->samples, (std::vector<float>{1, 2, 3, 4, 5, 6}));
  image.encoding = "rgba8";
  image.row_step = 0;
  image.height = 1;
  decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(*decoded->samples, (std::vector<float>{3, 2, 1, 99}));
  image.encoding = "bgr8";
  image.height = 2;
  image.row_step = 2;  // shorter than a row
  EXPECT_FALSE(decoder.decode(image, nullptr));
  image.row_step = 4;
  image.height = 3;  // past the buffer
  EXPECT_FALSE(decoder.decode(image, nullptr));
  image.height = UINT32_MAX;
  EXPECT_FALSE(decoder.decode(image, nullptr));
}

TEST(SceneImageDecoder, PreservesUint16AndFloat32Samples) {
  std::array<uint8_t, 4> bytes{0x12, 0x34, 0xff, 0xff};
  sdk::Image image;
  image.width = 2;
  image.height = 1;
  image.encoding = "mono16";
  image.is_bigendian = true;
  image.data = {bytes.data(), bytes.size()};
  const SceneImageDecoder decoder;
  auto decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(*decoded->samples, (std::vector<float>{4660, 65535}));
  image.is_bigendian = false;
  decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded->samples, (std::vector<float>{13330, 65535}));
  const auto bits = std::bit_cast<uint32_t>(1.25f);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<uint8_t>(bits >> (8 * index));
  }
  image.width = 1;
  image.encoding = "32FC1";
  decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded);
  EXPECT_FLOAT_EQ((*decoded->samples)[0], 1.25f);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<uint8_t>(bits >> (8 * (3 - index)));
  }
  image.is_bigendian = true;
  decoded = decoder.decode(image, nullptr);
  ASSERT_TRUE(decoded);
  EXPECT_FLOAT_EQ((*decoded->samples)[0], 1.25f);
}

TEST(SceneImageDecoder, DecodesKnownPngWithoutDisplayNormalization) {
  // One red RGB pixel, a lossless PNG.
  const std::vector<uint8_t> bytes = {137, 80,  78,  71, 13, 10,  26,  10, 0,   0,   0,   13,  73,  72, 68,  82, 0, 0,
                                      0,   1,   0,   0,  0,  1,   8,   2,  0,   0,   0,   144, 119, 83, 222, 0,  0, 0,
                                      12,  73,  68,  65, 84, 120, 156, 99, 248, 207, 192, 0,   0,   3,  1,   1,  0, 201,
                                      254, 146, 239, 0,  0,  0,   0,   73, 69,  78,  68,  174, 66,  96, 130};
  sdk::Image image;
  image.encoding = "png";
  image.data = {bytes.data(), bytes.size()};
  auto budget = scripting::makeNativeBudget(std::size_t{1} << 20);
  auto decoded = SceneImageDecoder{}.decode(image, budget);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->width, 1u);
  EXPECT_EQ(decoded->height, 1u);
  EXPECT_EQ(*decoded->samples, (std::vector<float>{255, 0, 0}));
  EXPECT_EQ(budget->used(), 3 * sizeof(float));  // the samples stay charged, the codec's buffers do not
  decoded = {unexpected("dropped")};
  EXPECT_EQ(budget->used(), 0u);
}

TEST(SceneImageDecoder, RejectsUnsupportedAndCorruptPayloads) {
  const SceneImageDecoder decoder;
  sdk::Image image;
  image.encoding = "qoi";
  auto decoded = decoder.decode(image, nullptr);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "unsupported image encoding: qoi");
  image.encoding = "png";
  EXPECT_FALSE(decoder.decode(image, nullptr));
  const std::array<uint8_t, 30> corrupt{137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13,
                                        73,  72, 68, 82, 0,  0,  0,  1,  0, 0, 0, 1};
  image.data = {corrupt.data(), corrupt.size()};
  EXPECT_FALSE(decoder.decode(image, nullptr));
  image.encoding = "jpeg";
  EXPECT_FALSE(decoder.decode(image, nullptr));
}

TEST(SceneImageDecoder, RejectsFloatExpansionBeforeReadingUnbackedPixels) {
  sdk::Image image;  // declares 2048 x 2048 and holds no bytes: reading a pixel would be out of bounds
  image.width = 2048;
  image.height = 2048;
  image.encoding = "mono8";
  auto budget = scripting::makeNativeBudget(std::size_t{1} << 20);
  auto decoded = SceneImageDecoder{}.decode(image, budget);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(budget->used(), 0u);
  // A JPEG whose header declares more than the budget allows is refused before the codec runs.
  const std::vector<uint8_t> jpeg = makeJpeg(64, 64);
  ASSERT_FALSE(jpeg.empty());
  image = compressed("jpeg", jpeg);
  // Room for the samples, not for them and the codec's buffers at once.
  budget = scripting::makeNativeBudget(64 * 64 * 3 * sizeof(float) + 1024);
  decoded = SceneImageDecoder{}.decode(image, budget);
  ASSERT_FALSE(decoded);
  EXPECT_TRUE(decoded.error().starts_with(scripting::native::kNativeLimitError)) << decoded.error();
  EXPECT_EQ(budget->used(), 0u);
  budget = scripting::makeNativeBudget(std::size_t{1} << 20);
  decoded = SceneImageDecoder{}.decode(image, budget);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->width, 64u);
  EXPECT_EQ(decoded->channels, 3u);
}

// The header that sizes the charge is the container's own: a JPEG labelled png, whose APP1 segment holds a fake 1 x 1
// IHDR where a PNG keeps its size, is refused before any codec reads it, and so is a PNG labelled jpeg.
TEST(SceneImageDecoder, AMislabelledPayloadIsRefusedBeforeItsHeaderSizesTheCharge) {
  const std::vector<uint8_t> jpeg = makeJpeg(64, 64);
  ASSERT_GT(jpeg.size(), 2u);
  // SOI, then an APP1 of 20 bytes whose payload puts big-endian 1 and 1 at offsets 16 and 20, then the rest.
  std::vector<uint8_t> crafted = {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x14, 0, 0, 0, 0, 0, 0,
                                  0,    0,    0,    0,    0,    0,    0, 1, 0, 0, 0, 1};
  crafted.insert(crafted.end(), jpeg.begin() + 2, jpeg.end());
  auto budget = scripting::makeNativeBudget(4 + crafted.size() + 4096);
  auto decoded = SceneImageDecoder{}.decode(compressed("png", crafted), budget);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "the payload is not a png container");
  EXPECT_EQ(budget->used(), 0u);
  // Labelled for what it is, the same bytes decode at the size their own header declares.
  decoded = SceneImageDecoder{}.decode(compressed("jpeg", crafted), scripting::makeNativeBudget(std::size_t{1} << 20));
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->width, 64u);
  const std::vector<uint8_t> png = test::makeRgbPng(2, 2, false);
  decoded = SceneImageDecoder{}.decode(compressed("jpeg", png), nullptr);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "the payload is not a jpeg container");
}

// What a codec allocates beside its output is charged before it runs: a 1 x 100000 PNG's row pointers (800 KB on a
// 64-bit host) outweigh its pixels, and a progressive JPEG keeps a coefficient buffer for the whole image, which a
// baseline one does not.
TEST(SceneImageDecoder, CodecWorkspaceIsChargedBeforeTheCodecRuns) {
  const std::vector<uint8_t> tall = test::makeRgbPng(1, 100000, false);
  ASSERT_FALSE(tall.empty());
  // Room for the output (4 bytes per pixel), the input copy, the float samples and 128 KiB of library state and row
  // buffers; not for the row pointers too.
  const std::size_t without_rows = 100000 * 4 + tall.size() + 100000 * 3 * sizeof(float) + (std::size_t{128} << 10);
  auto budget = scripting::makeNativeBudget(without_rows);
  auto decoded = SceneImageDecoder{}.decode(compressed("png", tall), budget);
  ASSERT_FALSE(decoded);
  EXPECT_TRUE(decoded.error().starts_with(scripting::native::kNativeLimitError)) << decoded.error();
  EXPECT_EQ(budget->used(), 0u);
  budget = scripting::makeNativeBudget(without_rows + 100000 * sizeof(void*));
  decoded = SceneImageDecoder{}.decode(compressed("png", tall), budget);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->height, 100000u);

  for (const bool progressive : {false, true}) {
    SCOPED_TRACE(progressive);
    const std::vector<uint8_t> jpeg = makeJpeg(1024, 1024, progressive);
    ASSERT_FALSE(jpeg.empty());
    // The output, the input copy, the samples and 2 MiB for row pointers, row buffers and state; not the 6 MiB of
    // coefficients a progressive file holds.
    budget = scripting::makeNativeBudget(
        1024 * 1024 * 4 + jpeg.size() + 1024 * 1024 * 3 * sizeof(float) + (std::size_t{2} << 20));
    decoded = SceneImageDecoder{}.decode(compressed("jpeg", jpeg), budget);
    EXPECT_EQ(decoded.has_value(), !progressive) << (decoded ? "" : decoded.error());
    if (!decoded) {
      EXPECT_TRUE(decoded.error().starts_with(scripting::native::kNativeLimitError)) << decoded.error();
      EXPECT_EQ(budget->used(), 0u);
    }
  }
}

// A JPEG frame of more than 4 components is no image the codec outputs, and its workspace bound would not fit a
// 32-bit size: refused before the charge.
TEST(SceneImageDecoder, RefusesAJpegOfMoreThanFourComponents) {
  std::vector<uint8_t> jpeg = makeJpeg(16, 16);
  const auto sof = std::search(jpeg.begin(), jpeg.end(), kSof0.begin(), kSof0.end());
  ASSERT_NE(sof, jpeg.end());
  sof[9] = 5;  // FF C0, length, precision, height, width, then the component count
  const auto budget = scripting::makeNativeBudget(std::size_t{1} << 20);
  const auto decoded = SceneImageDecoder{}.decode(compressed("jpeg", jpeg), budget);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), "invalid or missing jpeg header");
  EXPECT_EQ(budget->used(), 0u);
}

// ---- benchmarks -----------------------------------------------------------------------------------------------

// PJ_BENCH=1: SceneImageDecoder at 1080p on the calling thread (ms, median of 21): a JPEG, a PNG and raw rgb8.
TEST(SceneImageDecoderBench, Decode1080p) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  const auto median_ms = [](auto&& work) {
    std::vector<double> ms;
    for (int run = 0; run < 21; ++run) {
      const auto begin = std::chrono::steady_clock::now();
      work();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[10];
  };
  const SceneImageDecoder decoder;
  const auto budget = scripting::makeNativeBudget(std::size_t{256} << 20);
  const std::vector<uint8_t> jpeg = makeJpeg(1920, 1080);
  const std::vector<uint8_t> png = test::makeRgbPng(1920, 1080, false);
  std::vector<uint8_t> raw(std::size_t{1920} * 1080 * 3, 7);
  sdk::Image rgb;
  rgb.encoding = "rgb8";
  rgb.width = 1920;
  rgb.height = 1080;
  rgb.data = {raw.data(), raw.size()};
  const double jpeg_ms = median_ms([&] { EXPECT_TRUE(decoder.decode(compressed("jpeg", jpeg), budget)); });
  const double png_ms = median_ms([&] { EXPECT_TRUE(decoder.decode(compressed("png", png), budget)); });
  const double raw_ms = median_ms([&] { EXPECT_TRUE(decoder.decode(rgb, budget)); });
  std::printf(
      "[bench] decode 1080p: jpeg %.2f ms, png %.2f ms, rgb8 %.2f ms (%.2f ns/pixel)\n", jpeg_ms, png_ms, raw_ms,
      raw_ms * 1e6 / (1920.0 * 1080.0));
}

}  // namespace
}  // namespace PJ
