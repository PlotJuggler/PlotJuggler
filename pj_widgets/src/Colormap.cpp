// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Colormap.h"

#include <algorithm>
#include <cstddef>

namespace PJ {
namespace {

[[nodiscard]] uint8_t toByte(float value) noexcept {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Google's "Turbo" colormap, degree-5 polynomial form (same coefficients as the
// GLSL turbo() in colormapGlsl()).
[[nodiscard]] ColormapRgb turbo(float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float t4 = t2 * t2;
  const float t5 = t4 * t;
  const auto eval = [&](float a, float b, float c, float d, float e, float f) {
    return a + b * t + c * t2 + d * t3 + e * t4 + f * t5;
  };
  return {
      eval(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f, -152.94239396f, 59.28637943f),
      eval(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f, 4.27729857f, 2.82956604f),
      eval(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f, -89.90310912f, 27.34824973f),
  };
}

// Degree-6 RGB polynomial (Matt Zucker's matplotlib fits), Horner-evaluated.
// viridis and plasma differ only by this coefficient table (c[0]..c[6]).
[[nodiscard]] ColormapRgb evalPoly(const ColormapRgb (&c)[7], float t) noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  ColormapRgb acc = c[6];
  for (int i = 5; i >= 0; --i) {
    acc = {c[i].r + t * acc.r, c[i].g + t * acc.g, c[i].b + t * acc.b};
  }
  return acc;
}

[[nodiscard]] ColormapRgb viridis(float t) noexcept {
  static constexpr ColormapRgb kCoeffs[7] = {
      {0.2777273272234177f, 0.005407344544966578f, 0.3340998053353061f},
      {0.1050930431085774f, 1.404613529898575f, 1.384590162594685f},
      {-0.3308618287255563f, 0.214847559468213f, 0.09509516302823659f},
      {-4.634230498983486f, -5.799100973351585f, -19.33244095627987f},
      {6.228269936347081f, 14.17993336680509f, 56.69055260068105f},
      {4.776384997670288f, -13.74514537774601f, -65.35303263337234f},
      {-5.435455855934631f, 4.645852612178535f, 26.3124352495832f},
  };
  return evalPoly(kCoeffs, t);
}

[[nodiscard]] ColormapRgb plasma(float t) noexcept {
  static constexpr ColormapRgb kCoeffs[7] = {
      {0.05873234392399702f, 0.02333670892565664f, 0.5433401826748754f},
      {2.176514634195958f, 0.2383834171260182f, 0.7539604599784036f},
      {-2.689460476458034f, -7.455851135738909f, 3.110799939717086f},
      {6.130348345893603f, 42.3461881477227f, -28.51885465332158f},
      {-11.10743619062271f, -82.66631109428045f, 60.13984767418263f},
      {10.02306557647065f, 71.41361770095349f, -54.07218655560067f},
      {-3.658713842777788f, -22.93153465461149f, 18.19190778539828f},
  };
  return evalPoly(kCoeffs, t);
}

}  // namespace

ColormapRgb colorFor(Colormap colormap, float t) noexcept {
  switch (colormap) {
    case Colormap::kTurbo:
      return turbo(t);
    case Colormap::kViridis:
      return viridis(t);
    case Colormap::kPlasma:
      return plasma(t);
    case Colormap::kGrayscale: {
      const float v = std::clamp(t, 0.0f, 1.0f);
      return {v, v, v};
    }
  }
  return turbo(t);
}

std::vector<uint8_t> buildColormapLut(int width) {
  width = std::max(width, 1);
  std::vector<uint8_t> lut(static_cast<size_t>(width) * kColormapCount * 4U, 0);
  for (int row = 0; row < kColormapCount; ++row) {
    const auto colormap = static_cast<Colormap>(row);
    for (int x = 0; x < width; ++x) {
      const float t = width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0f;
      const ColormapRgb c = colorFor(colormap, t);
      uint8_t* pixel = &lut[(static_cast<size_t>(row) * width + x) * 4U];
      pixel[0] = toByte(c.r);
      pixel[1] = toByte(c.g);
      pixel[2] = toByte(c.b);
      pixel[3] = 255;
    }
  }
  return lut;
}

std::string_view colormapGlsl() noexcept {
  return R"(
vec3 turbo(float t) {
  const vec4 kRedVec4   = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
  const vec4 kGreenVec4 = vec4(0.09140261, 2.19418839,   4.84296658, -14.18503333);
  const vec4 kBlueVec4  = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
  const vec2 kRedVec2   = vec2(-152.94239396,  59.28637943);
  const vec2 kGreenVec2 = vec2(  4.27729857,   2.82956604);
  const vec2 kBlueVec2  = vec2(-89.90310912,  27.34824973);

  t = clamp(t, 0.0, 1.0);
  vec4 v4 = vec4(1.0, t, t * t, t * t * t);
  vec2 v2 = v4.zw * v4.z;
  return vec3(
    dot(v4, kRedVec4)   + dot(v2, kRedVec2),
    dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
    dot(v4, kBlueVec4)  + dot(v2, kBlueVec2)
  );
}

// Matt Zucker's polynomial approximation of matplotlib's viridis.
vec3 viridis(float t) {
  const vec3 c0 = vec3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
  const vec3 c1 = vec3(0.1050930431085774, 1.404613529898575,    1.384590162594685);
  const vec3 c2 = vec3(-0.3308618287255563, 0.214847559468213,   0.09509516302823659);
  const vec3 c3 = vec3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
  const vec3 c4 = vec3(6.228269936347081,  14.17993336680509,   56.69055260068105);
  const vec3 c5 = vec3(4.776384997670288, -13.74514537774601,  -65.35303263337234);
  const vec3 c6 = vec3(-5.435455855934631,  4.645852612178535,  26.3124352495832);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

// Matt Zucker's polynomial approximation of matplotlib's plasma.
vec3 plasma(float t) {
  const vec3 c0 = vec3(0.05873234392399702, 0.02333670892565664, 0.5433401826748754);
  const vec3 c1 = vec3(2.176514634195958,   0.2383834171260182,  0.7539604599784036);
  const vec3 c2 = vec3(-2.689460476458034, -7.455851135738909,   3.110799939717086);
  const vec3 c3 = vec3(6.130348345893603,  42.3461881477227,   -28.51885465332158);
  const vec3 c4 = vec3(-11.10743619062271, -82.66631109428045,  60.13984767418263);
  const vec3 c5 = vec3(10.02306557647065,  71.41361770095349, -54.07218655560067);
  const vec3 c6 = vec3(-3.658713842777788, -22.93153465461149, 18.19190778539828);
  return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

vec3 sampleColormap(int id, float t) {
  t = clamp(t, 0.0, 1.0);
  if (id == 0) return turbo(t);
  if (id == 1) return viridis(t);
  if (id == 2) return plasma(t);
  return vec3(t);  // grayscale
}
)";
}

const char* colormapName(Colormap colormap) noexcept {
  switch (colormap) {
    case Colormap::kTurbo:
      return "Turbo";
    case Colormap::kViridis:
      return "Viridis";
    case Colormap::kPlasma:
      return "Plasma";
    case Colormap::kGrayscale:
      return "Grayscale";
  }
  return "Turbo";
}

}  // namespace PJ
