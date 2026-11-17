// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/decoded_frame.h"

#include <gtest/gtest.h>

using PJ::DecodedFrame;
using PJ::expectedBufferSize;
using PJ::PixelFormat;

// --- expectedBufferSize: packed formats ---

TEST(ExpectedBufferSize, RGB888) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kRGB888), size_t{640} * 480 * 3);
  EXPECT_EQ(expectedBufferSize(1920, 1080, PixelFormat::kRGB888), size_t{1920} * 1080 * 3);
}

TEST(ExpectedBufferSize, RGBA8888) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kRGBA8888), size_t{640} * 480 * 4);
}

TEST(ExpectedBufferSize, BGR888) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kBGR888), size_t{640} * 480 * 3);
}

TEST(ExpectedBufferSize, BGRA8888) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kBGRA8888), size_t{640} * 480 * 4);
}

TEST(ExpectedBufferSize, Mono8) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kMono8), size_t{640} * 480);
}

TEST(ExpectedBufferSize, Mono16) {
  EXPECT_EQ(expectedBufferSize(640, 480, PixelFormat::kMono16), size_t{640} * 480 * 2);
}

// --- expectedBufferSize: YUV420P (the critical cases) ---

TEST(ExpectedBufferSize, YUV420PEvenDimensions) {
  size_t y = size_t{1920} * 1080;
  size_t uv = size_t{960} * 540;
  EXPECT_EQ(expectedBufferSize(1920, 1080, PixelFormat::kYUV420P), y + 2 * uv);
}

TEST(ExpectedBufferSize, YUV420POddWidthAndHeight) {
  // Chroma planes are ceil(w/2) x ceil(h/2): w/2 truncates and overflows the buffer.
  size_t y = size_t{1921} * 1081;
  size_t uv = size_t{961} * 541;  // (1921+1)/2 * (1081+1)/2
  EXPECT_EQ(expectedBufferSize(1921, 1081, PixelFormat::kYUV420P), y + 2 * uv);

  // Verify it does NOT equal the truncated (buggy) formula
  size_t wrong_uv = size_t{960} * 540;
  EXPECT_NE(expectedBufferSize(1921, 1081, PixelFormat::kYUV420P), size_t{1921} * 1081 + 2 * wrong_uv);
}

TEST(ExpectedBufferSize, YUV420POddWidthOnly) {
  size_t y = size_t{641} * 480;
  size_t uv = size_t{321} * 240;  // (641+1)/2 = 321, 480/2 = 240
  EXPECT_EQ(expectedBufferSize(641, 480, PixelFormat::kYUV420P), y + 2 * uv);
}

TEST(ExpectedBufferSize, YUV420POddHeightOnly) {
  size_t y = size_t{640} * 481;
  size_t uv = size_t{320} * 241;  // 640/2 = 320, (481+1)/2 = 241
  EXPECT_EQ(expectedBufferSize(640, 481, PixelFormat::kYUV420P), y + 2 * uv);
}

TEST(ExpectedBufferSize, YUV420PMinimal1x1) {
  // Smallest valid YUV420P: Y=1, U=1, V=1
  EXPECT_EQ(expectedBufferSize(1, 1, PixelFormat::kYUV420P), size_t{3});
}

TEST(ExpectedBufferSize, YUV420P2x2) {
  // 2x2: Y=4, U=1, V=1
  EXPECT_EQ(expectedBufferSize(2, 2, PixelFormat::kYUV420P), size_t{6});
}

// --- expectedBufferSize: NV12 ---

TEST(ExpectedBufferSize, NV12EvenDimensions) {
  // NV12: Y plane (w*h) + interleaved UV plane (w * ceil(h/2))
  size_t y = size_t{1920} * 1080;
  size_t uv = size_t{1920} * 540;
  EXPECT_EQ(expectedBufferSize(1920, 1080, PixelFormat::kNV12), y + uv);
}

TEST(ExpectedBufferSize, NV12OddHeight) {
  size_t y = size_t{1920} * 1081;
  size_t uv = size_t{1920} * 541;  // ceil(1081/2) = 541
  EXPECT_EQ(expectedBufferSize(1920, 1081, PixelFormat::kNV12), y + uv);
}

TEST(ExpectedBufferSize, NV12OddWidth) {
  // NV12 chroma row = ceil(w/2) interleaved U,V pairs = 2*ceil(w/2) bytes
  // (642 for w=641), matching FFmpeg's chroma linesize — NOT w bytes.
  size_t y = size_t{641} * 480;
  size_t uv = size_t{2} * 321 * 240;  // 2 * ceil(641/2) * ceil(480/2)
  EXPECT_EQ(expectedBufferSize(641, 480, PixelFormat::kNV12), y + uv);
}

// --- DecodedFrame::isValid ---

TEST(DecodedFrame, IsValidRGB) {
  DecodedFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.format = PixelFormat::kRGB888;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(2 * 2 * 3);
  EXPECT_TRUE(frame.isValid());
  EXPECT_FALSE(frame.isNull());
}

TEST(DecodedFrame, IsValidYUV420P) {
  DecodedFrame frame;
  frame.width = 3;
  frame.height = 3;
  frame.format = PixelFormat::kYUV420P;
  // 3x3 Y=9, U=2*2=4, V=4, total=17
  frame.pixels = std::make_shared<std::vector<uint8_t>>(expectedBufferSize(3, 3, PixelFormat::kYUV420P));
  EXPECT_TRUE(frame.isValid());
}

TEST(DecodedFrame, IsValidWrongBufferSize) {
  DecodedFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.format = PixelFormat::kRGB888;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(10);  // should be 12
  EXPECT_FALSE(frame.isValid());
}

TEST(DecodedFrame, IsValidNullPixels) {
  DecodedFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.format = PixelFormat::kRGB888;
  frame.pixels = nullptr;
  EXPECT_FALSE(frame.isValid());
  EXPECT_TRUE(frame.isNull());
}

TEST(DecodedFrame, IsValidZeroDimensions) {
  DecodedFrame frame;
  frame.width = 0;
  frame.height = 2;
  frame.format = PixelFormat::kRGB888;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(6);
  EXPECT_FALSE(frame.isValid());
}

TEST(DecodedFrame, DefaultConstructedIsNullAndInvalid) {
  DecodedFrame frame;
  EXPECT_TRUE(frame.isNull());
  EXPECT_FALSE(frame.isValid());
}
