// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/video_color.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace PJ {
namespace {

// Apply the column-major YUV→RGB matrix exactly as the media shader does:
// rgb = (M * vec4(y, u - 0.5, v - 0.5, 1)).rgb. Element (row r, col c) = M[c*4 + r].
std::array<float, 3> applyMatrix(const std::array<float, 16>& m, float y, float u, float v) {
  const float a[4] = {y, u - 0.5f, v - 0.5f, 1.0f};
  std::array<float, 3> rgb{};
  for (int r = 0; r < 3; ++r) {
    float acc = 0.0f;
    for (int c = 0; c < 4; ++c) {
      acc += m[c * 4 + r] * a[c];
    }
    rgb[r] = acc;
  }
  return rgb;
}

constexpr float kTol = 2e-3f;

TEST(VideoColorMatrix, Bt709FullMatchesHistoricalConstant) {
  // Pins the full-range BT.709 matrix, which untagged content gets.
  // clang-format off
  const std::array<float, 16> kHistorical = {
      1.0f,    1.0f,      1.0f,    0.0f,
      0.0f,   -0.18732f,  1.8556f, 0.0f,
      1.5748f, -0.46812f,  0.0f,   0.0f,
      0.0f,    0.0f,      0.0f,    1.0f};
  // clang-format on
  const auto m = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(m[i], kHistorical[i], kTol) << "element " << i;
  }
}

TEST(VideoColorMatrix, FullRangeGrayMapsToGray) {
  const auto m = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
  const auto rgb = applyMatrix(m, 0.5f, 0.5f, 0.5f);  // mid luma, neutral chroma
  EXPECT_NEAR(rgb[0], 0.5f, kTol);
  EXPECT_NEAR(rgb[1], 0.5f, kTol);
  EXPECT_NEAR(rgb[2], 0.5f, kTol);
}

TEST(VideoColorMatrix, LimitedRangeBlackAndWhite) {
  const auto m = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kLimited);
  // Studio black: Y=16/255, neutral chroma 128/255 → RGB ~0.
  const auto black = applyMatrix(m, 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
  EXPECT_NEAR(black[0], 0.0f, kTol);
  EXPECT_NEAR(black[1], 0.0f, kTol);
  EXPECT_NEAR(black[2], 0.0f, kTol);
  // Studio white: Y=235/255 → RGB ~1.
  const auto white = applyMatrix(m, 235.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
  EXPECT_NEAR(white[0], 1.0f, kTol);
  EXPECT_NEAR(white[1], 1.0f, kTol);
  EXPECT_NEAR(white[2], 1.0f, kTol);
}

TEST(VideoColorMatrix, FullRangeBlackAndWhite) {
  const auto m = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
  const auto black = applyMatrix(m, 0.0f, 0.5f, 0.5f);
  EXPECT_NEAR(black[0], 0.0f, kTol);
  const auto white = applyMatrix(m, 1.0f, 0.5f, 0.5f);
  EXPECT_NEAR(white[0], 1.0f, kTol);
  EXPECT_NEAR(white[1], 1.0f, kTol);
  EXPECT_NEAR(white[2], 1.0f, kTol);
}

TEST(VideoColorMatrix, Bt601DiffersFromBt709) {
  const auto m601 = buildYuvMatrix(YuvColorSpace::kBt601, YuvColorRange::kFull);
  const auto m709 = buildYuvMatrix(YuvColorSpace::kBt709, YuvColorRange::kFull);
  // A saturated chroma sample must land on different RGB under the two matrices,
  // otherwise the space selection is a no-op.
  const auto a = applyMatrix(m601, 0.5f, 0.9f, 0.1f);
  const auto b = applyMatrix(m709, 0.5f, 0.9f, 0.1f);
  const float d = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
  EXPECT_GT(d, 0.02f);
}

}  // namespace
}  // namespace PJ
