// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Colormap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ {
namespace {

// turbo runs blue -> green -> red. The polynomial approximation is rough exactly
// at the endpoints, so probe slightly inside [0,1] where the ordering is robust.
TEST(ColormapTest, TurboGoesBlueToRedWithGreenMidpoint) {
  const ColormapRgb low = colorFor(Colormap::kTurbo, 0.1f);
  const ColormapRgb mid = colorFor(Colormap::kTurbo, 0.5f);
  const ColormapRgb high = colorFor(Colormap::kTurbo, 0.9f);

  EXPECT_GT(low.b, low.r);    // blue dominates the near end
  EXPECT_GT(high.r, high.b);  // red dominates the far end
  EXPECT_GT(mid.g, low.g);    // green peaks in the middle
  EXPECT_GT(mid.g, high.g);
}

TEST(ColormapTest, GrayscaleIsIdentityRamp) {
  for (float t : {0.0f, 0.25f, 0.5f, 1.0f}) {
    const ColormapRgb c = colorFor(Colormap::kGrayscale, t);
    EXPECT_FLOAT_EQ(c.r, t);
    EXPECT_FLOAT_EQ(c.g, t);
    EXPECT_FLOAT_EQ(c.b, t);
  }
}

TEST(ColormapTest, ViridisAndPlasmaEndpointsDiffer) {
  for (Colormap cm : {Colormap::kViridis, Colormap::kPlasma}) {
    const ColormapRgb a = colorFor(cm, 0.0f);
    const ColormapRgb b = colorFor(cm, 1.0f);
    EXPECT_FALSE(a.r == b.r && a.g == b.g && a.b == b.b);
  }
}

TEST(ColormapTest, ColorForClampsOutOfRangeT) {
  EXPECT_FLOAT_EQ(colorFor(Colormap::kGrayscale, -1.0f).r, 0.0f);
  EXPECT_FLOAT_EQ(colorFor(Colormap::kGrayscale, 2.0f).r, 1.0f);
}

TEST(ColormapTest, BuildLutShapeAlphaAndDeterminism) {
  constexpr int kWidth = 256;
  const std::vector<uint8_t> lut = buildColormapLut(kWidth);
  ASSERT_EQ(lut.size(), static_cast<size_t>(kWidth) * kColormapCount * 4U);

  // Opaque everywhere; alpha is the 4th byte of every texel.
  for (size_t i = 3; i < lut.size(); i += 4) {
    EXPECT_EQ(lut[i], 255) << "alpha at texel " << (i / 4);
  }

  // Same input -> identical table (no Math.random/time dependence).
  EXPECT_EQ(lut, buildColormapLut(kWidth));
}

TEST(ColormapTest, LutMatchesColorForAndGrayscaleRowRamps) {
  constexpr int kWidth = 256;
  const std::vector<uint8_t> lut = buildColormapLut(kWidth);
  const auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  const auto texel = [&](int row, int x) { return &lut[(static_cast<size_t>(row) * kWidth + x) * 4U]; };

  // The turbo row (row 0) must equal colorFor(kTurbo, t) sampled across the row.
  for (int x : {0, 64, 128, 200, 255}) {
    const float t = static_cast<float>(x) / static_cast<float>(kWidth - 1);
    const ColormapRgb c = colorFor(Colormap::kTurbo, t);
    const uint8_t* p = texel(0, x);
    EXPECT_EQ(p[0], toByte(c.r));
    EXPECT_EQ(p[1], toByte(c.g));
    EXPECT_EQ(p[2], toByte(c.b));
  }

  // The grayscale row (row 3) is a monotone gray ramp.
  const int gray_row = static_cast<int>(Colormap::kGrayscale);
  EXPECT_EQ(texel(gray_row, 0)[0], 0);
  EXPECT_EQ(texel(gray_row, kWidth - 1)[0], 255);
  for (int x = 0; x < kWidth; ++x) {
    const uint8_t* p = texel(gray_row, x);
    EXPECT_EQ(p[0], p[1]);
    EXPECT_EQ(p[1], p[2]);
  }
}

// The persisted layout-XML tokens: saved layouts depend on these exact spellings.
TEST(ColormapTest, TokensRoundTripEveryColormap) {
  const std::pair<Colormap, const char*> kPairs[] = {
      {Colormap::kTurbo, "turbo"},
      {Colormap::kViridis, "viridis"},
      {Colormap::kPlasma, "plasma"},
      {Colormap::kGrayscale, "grayscale"},
  };
  static_assert(std::size(kPairs) == kColormapCount);
  for (const auto& [colormap, token] : kPairs) {
    EXPECT_EQ(colormapToken(colormap), QString::fromLatin1(token));
    EXPECT_EQ(parseColormap(QString::fromLatin1(token)), colormap) << token;
  }
}

// Matching is exact and case-sensitive; anything else is nullopt, and each caller
// picks its own fallback (Scene3D rejects the element, the 2D depth layer uses turbo).
TEST(ColormapTest, UnknownOrMiscasedTokensDoNotParse) {
  for (const char* token : {"", "jet", "rainbow", "Turbo", "VIRIDIS", "Grayscale", " plasma", "gray", "0"}) {
    EXPECT_EQ(parseColormap(QString::fromLatin1(token)), std::nullopt) << token;
  }
}

TEST(ColormapTest, GlslExposesDispatcher) {
  const std::string_view glsl = colormapGlsl();
  EXPECT_NE(glsl.find("sampleColormap"), std::string_view::npos);
  EXPECT_NE(glsl.find("vec3 turbo"), std::string_view::npos);
}

}  // namespace
}  // namespace PJ
