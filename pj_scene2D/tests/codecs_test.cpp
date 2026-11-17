// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/codecs.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "test_png_io.h"

namespace {

PJ::DecodedFrame rawBytesFrame(const std::vector<uint8_t>& bytes) {
  PJ::DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(bytes);
  return frame;
}

PJ::DecodedFrame mono16Frame(int width, int height, const std::vector<uint16_t>& values) {
  PJ::DecodedFrame frame;
  frame.width = width;
  frame.height = height;
  frame.format = PJ::PixelFormat::kMono16;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(values.size() * sizeof(uint16_t));
  std::memcpy(frame.pixels->data(), values.data(), frame.pixels->size());
  return frame;
}

PJ::DecodedFrame rgbFrame(int width, int height, const std::vector<uint8_t>& values) {
  PJ::DecodedFrame frame;
  frame.width = width;
  frame.height = height;
  frame.format = PJ::PixelFormat::kRGB888;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(values);
  return frame;
}

// CRC-32 (PNG/zlib polynomial) over a byte range — used to re-stamp a patched
// IHDR so png_read_info accepts our forged dimensions.
uint32_t crc32Png(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// Rewrite a valid PNG's IHDR width/height to forge a giant declared size, fixing
// the IHDR CRC so png_read_info still parses it. Layout: [8 sig][4 len][4 "IHDR"]
// [4 width][4 height]...[4 CRC]; the CRC covers "IHDR" + the 13 data bytes.
std::vector<uint8_t> pngWithPatchedDimensions(std::vector<uint8_t> png, uint32_t width, uint32_t height) {
  auto put_be = [&](size_t off, uint32_t v) {
    png[off + 0] = static_cast<uint8_t>(v >> 24);
    png[off + 1] = static_cast<uint8_t>(v >> 16);
    png[off + 2] = static_cast<uint8_t>(v >> 8);
    png[off + 3] = static_cast<uint8_t>(v);
  };
  put_be(16, width);
  put_be(20, height);
  put_be(29, crc32Png(png.data() + 12, 17));
  return png;
}

PJ::DecodedFrame mono8MosaicFrame(int width, int height, const std::vector<uint8_t>& mosaic) {
  PJ::DecodedFrame frame;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(mosaic);
  frame.width = width;
  frame.height = height;
  frame.format = PJ::PixelFormat::kMono8;
  return frame;
}

// Build a width*height mosaic where each site holds the value mapped to the
// channel it samples under the given top-left 2x2 CFA pattern. site_for(r,c)
// returns 'R','G','B'. R=255, G=128, B=0 — a uniform per-channel field, so a
// correct demosaic reconstructs (255,128,0) at every interior pixel.
template <typename SiteFn>
std::vector<uint8_t> channelEncodedMosaic(int width, int height, SiteFn site_for) {
  std::vector<uint8_t> m(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      const char ch = site_for(r, c);
      m[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)] = (ch == 'R')   ? 255
                                                                                        : (ch == 'G') ? 128
                                                                                                      : 0;
    }
  }
  return m;
}

void expectRgb(const PJ::DecodedFrame& f, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(f.width) + static_cast<size_t>(x)) * 3;
  ASSERT_LT(i + 2, f.pixels->size());
  EXPECT_EQ((*f.pixels)[i + 0], r) << "R @ (" << x << "," << y << ")";
  EXPECT_EQ((*f.pixels)[i + 1], g) << "G @ (" << x << "," << y << ")";
  EXPECT_EQ((*f.pixels)[i + 2], b) << "B @ (" << x << "," << y << ")";
}

}  // namespace

TEST(PngCodecTest, RejectsAbsurdDimensionsBeforeAllocating) {
  // A real 2x2 PNG with its IHDR forged to 11000x11000 (121 MP > the 100 MP cap).
  // Without the cap the decoder value-initializes a ~240 MB buffer straight from
  // these header bytes; the cap must reject up front with a dimension error.
  auto small = PJ::test::makeGrayPng(2, 2, 16, std::vector<uint16_t>{0, 1000, 2000, 3000});
  ASSERT_GT(small.size(), 33u);
  auto huge = pngWithPatchedDimensions(small, 11000, 11000);

  PJ::PngCodec codec;
  auto result = codec.decode(rawBytesFrame(huge));
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("dimension"), std::string::npos) << "got: " << result.error();
}

TEST(AutoImageCodecTest, NormalizesMono16PngToRgbGrayscale) {
  const std::vector<uint16_t> values = {0, 1000, 2000, 3000};
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(2, 2, 16, values);
  ASSERT_FALSE(png.empty());

  PJ::AutoImageCodec codec;
  auto decoded = codec.decode(rawBytesFrame(png));
  ASSERT_TRUE(decoded.has_value()) << decoded.error();

  EXPECT_EQ(decoded->width, 2);
  EXPECT_EQ(decoded->height, 2);
  EXPECT_EQ(decoded->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(decoded->pixels, nullptr);
  ASSERT_EQ(decoded->pixels->size(), 12U);

  const auto& pixels = *decoded->pixels;
  EXPECT_EQ(pixels[0], 0);  // invalid zero mono16 stays black
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[2], 0);
  EXPECT_EQ(pixels[3], 0);  // min non-zero maps to black
  EXPECT_EQ(pixels[4], 0);
  EXPECT_EQ(pixels[5], 0);
  EXPECT_EQ(pixels[6], 127);  // midpoint maps near half intensity
  EXPECT_EQ(pixels[7], 127);
  EXPECT_EQ(pixels[8], 127);
  EXPECT_EQ(pixels[9], 255);  // max maps to white
  EXPECT_EQ(pixels[10], 255);
  EXPECT_EQ(pixels[11], 255);
}

TEST(Mono16ToGrayscaleTest, FlatImageClampsMaxLessEqualMinToBlack) {
  PJ::Mono16ToGrayscale codec;
  auto decoded = codec.decode(mono16Frame(2, 1, {1234, 1234}));
  ASSERT_TRUE(decoded.has_value()) << decoded.error();

  EXPECT_EQ(decoded->width, 2);
  EXPECT_EQ(decoded->height, 1);
  EXPECT_EQ(decoded->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(decoded->pixels, nullptr);
  EXPECT_EQ(*decoded->pixels, (std::vector<uint8_t>{0, 0, 0, 0, 0, 0}));
}

TEST(Mono16ToGrayscaleTest, RejectsShortBuffer) {
  PJ::DecodedFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.format = PJ::PixelFormat::kMono16;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(7, 0);

  PJ::Mono16ToGrayscale codec;
  auto decoded = codec.decode(frame);
  EXPECT_FALSE(decoded.has_value());
}

TEST(Mono16ToGrayscaleTest, ZeroPixelsStayBlackWhileValidPixelsNormalize) {
  PJ::Mono16ToGrayscale codec;
  auto decoded = codec.decode(mono16Frame(3, 1, {0, 1000, 2000}));
  ASSERT_TRUE(decoded.has_value()) << decoded.error();

  ASSERT_NE(decoded->pixels, nullptr);
  ASSERT_EQ(decoded->pixels->size(), 9u);
  const auto& pixels = *decoded->pixels;
  EXPECT_EQ(pixels[0], 0);
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[2], 0);
  EXPECT_EQ(pixels[3], 0);
  EXPECT_EQ(pixels[4], 0);
  EXPECT_EQ(pixels[5], 0);
  EXPECT_EQ(pixels[6], 255);
  EXPECT_EQ(pixels[7], 255);
  EXPECT_EQ(pixels[8], 255);
}

TEST(NormalizeMono16Test, PassesNonMono16ThroughUnchanged) {
  const auto input = rgbFrame(1, 1, {1, 2, 3});

  PJ::NormalizeMono16 codec;
  auto decoded = codec.decode(input);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();

  EXPECT_EQ(decoded->width, input.width);
  EXPECT_EQ(decoded->height, input.height);
  EXPECT_EQ(decoded->format, input.format);
  EXPECT_EQ(decoded->pixels, input.pixels);
  EXPECT_EQ(*decoded->pixels, (std::vector<uint8_t>{1, 2, 3}));
}

TEST(BayerDecodeTest, RggbInteriorPixelsReconstructChannels) {
  // RGGB top-left tile:  R G / G B
  auto site = [](int r, int c) -> char {
    if (r % 2 == 0 && c % 2 == 0) {
      return 'R';
    }
    if (r % 2 == 1 && c % 2 == 1) {
      return 'B';
    }
    return 'G';
  };
  const std::vector<uint8_t> mosaic = channelEncodedMosaic(4, 4, site);

  PJ::BayerDecode codec(PJ::BayerPattern::kRGGB);
  auto out = codec.decode(mono8MosaicFrame(4, 4, mosaic));
  ASSERT_TRUE(out.has_value()) << out.error();

  EXPECT_EQ(out->width, 4);
  EXPECT_EQ(out->height, 4);
  EXPECT_EQ(out->format, PJ::PixelFormat::kRGB888);
  ASSERT_NE(out->pixels, nullptr);
  EXPECT_EQ(out->pixels->size(), 4u * 4u * 3u);

  // Interior pixels have a full neighbourhood, so the uniform-per-channel
  // field must round-trip exactly.
  expectRgb(*out, 1, 1, 255, 128, 0);
  expectRgb(*out, 2, 1, 255, 128, 0);
  expectRgb(*out, 1, 2, 255, 128, 0);
  expectRgb(*out, 2, 2, 255, 128, 0);
}

TEST(BayerDecodeTest, BggrPlacesRedAndBlueOppositeToRggb) {
  // BGGR top-left tile:  B G / G R  — same uniform-per-channel field, so a
  // pattern-aware decode must still yield (255,128,0) at interior pixels.
  auto site = [](int r, int c) -> char {
    if (r % 2 == 0 && c % 2 == 0) {
      return 'B';
    }
    if (r % 2 == 1 && c % 2 == 1) {
      return 'R';
    }
    return 'G';
  };
  const std::vector<uint8_t> mosaic = channelEncodedMosaic(4, 4, site);

  PJ::BayerDecode codec(PJ::BayerPattern::kBGGR);
  auto out = codec.decode(mono8MosaicFrame(4, 4, mosaic));
  ASSERT_TRUE(out.has_value()) << out.error();
  expectRgb(*out, 1, 1, 255, 128, 0);
  expectRgb(*out, 2, 2, 255, 128, 0);
}

// Guard against an out-of-bounds read in the 3x3 neighbourhood loop: a buffer
// smaller than width*height must be rejected, not demosaiced.
TEST(BayerDecodeTest, RejectsBufferTooSmallForDimensions) {
  auto out = PJ::BayerDecode(PJ::BayerPattern::kRGGB).decode(mono8MosaicFrame(4, 4, std::vector<uint8_t>(8, 0)));
  EXPECT_FALSE(out.has_value());
}

// A transport that losslessly PNG-wraps a raw buffer (Mosaico stores a 16UC1
// depth frame as an 8-bit grayscale PNG of width=stride) must round-trip back to
// the exact flat bytes, so the depth path reads them as raw 16UC1 instead of
// mis-reading the compressed PNG as depth (an all-black image).
TEST(RecoverContainerRawSamplesTest, EightBitGrayPngWrappedDepthRoundTrips) {
  // A 2x2 16UC1 frame (4 px) = 8 little-endian bytes, stored as an 8-bit
  // grayscale PNG of width=stride(4) x height(2) = 8 samples.
  const std::vector<uint16_t> depth_mm = {1000, 2000, 3000, 40000};
  std::vector<uint8_t> raw;
  for (uint16_t v : depth_mm) {
    raw.push_back(static_cast<uint8_t>(v & 0xFF));
    raw.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  }
  const std::vector<uint8_t> png = PJ::test::makeGrayPng(/*width=*/4, /*height=*/2, /*bit_depth=*/8, raw);
  ASSERT_FALSE(png.empty());

  auto recovered = PJ::recoverContainerRawSamples(png.data(), png.size());
  ASSERT_TRUE(recovered.has_value());
  ASSERT_NE(*recovered, nullptr);
  EXPECT_EQ(**recovered, raw);  // exact flat bytes, ready to reinterpret as 16UC1
}

// Genuinely raw bytes (no container signature) are left to the caller's raw path.
TEST(RecoverContainerRawSamplesTest, RawBytesAreNotAContainer) {
  const std::vector<uint8_t> raw = {0xE8, 0x03, 0xD0, 0x07, 0xB8, 0x0B};
  EXPECT_FALSE(PJ::recoverContainerRawSamples(raw.data(), raw.size()).has_value());
}
