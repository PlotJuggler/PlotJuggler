#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringView>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace PJ {

/// Scientific colormaps shared by the visualization views (the 3D pointcloud field colouring and the 2D depth-image
/// LUT), so one scalar maps to one colour everywhere. The order is the LUT row index and the persisted id: append only,
/// reordering recolours saved layouts.
enum class Colormap : uint8_t {
  kTurbo,
  kViridis,
  kPlasma,
  kGrayscale,
};

/// Number of colormaps (rows in the LUT texture); keep in sync with the enum.
inline constexpr int kColormapCount = 4;

/// Untranslated display name ("Turbo", "Viridis", ...), a tr()-ready literal so every view lists the same names in enum
/// order.
[[nodiscard]] const char* colormapName(Colormap colormap) noexcept;

/// Persisted layout-XML spelling ("turbo", "viridis", "plasma", "grayscale"): colormapName() lowercased, so renaming a
/// label changes saved layouts too.
[[nodiscard]] QString colormapToken(Colormap colormap);

/// The colormap `token` spells: an exact, case-sensitive match of a colormapToken(); nullopt otherwise (the caller
/// picks the fallback).
[[nodiscard]] std::optional<Colormap> parseColormap(QStringView token);

/// Default LUT width (samples of t per colormap row): the resolution of the GPU LUT texture, 8 bits per entry.
inline constexpr int kColormapLutWidth = 256;

/// Linear RGB in [0, 1]: a minimal colour type, so this helper depends only on Qt and the standard library (not
/// pj_base, hence not sdk::ColorRGBA).
struct ColormapRgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

/// Maps t in [0, 1] to a colour; out-of-range t is clamped. `turbo` is Google's degree-5 polynomial, `viridis` and
/// `plasma` are Matt Zucker's degree-6 fits of matplotlib. Same coefficients as colormapGlsl(), so the 2D LUT path and
/// the 3D in-shader path agree to within the LUT's 8-bit quantization.
[[nodiscard]] ColormapRgb colorFor(Colormap colormap, float t) noexcept;

/// Builds a `width` x kColormapCount RGBA8 lookup table, row-major: row `c` is colormap `c` sampled across t in [0, 1].
/// Uploaded once as a GPU LUT texture by the LUT-sampling backends (the 2D depth shader). `width` < 1 is 1.
[[nodiscard]] std::vector<uint8_t> buildColormapLut(int width = kColormapLutWidth);

/// GLSL source defining `turbo`, `viridis`, `plasma` and a `vec3 sampleColormap(int id, float t)` dispatcher (id is the
/// Colormap value), for backends that colour in-shader instead of through a LUT (the 3D pointcloud pass). Same
/// coefficients as colorFor().
[[nodiscard]] std::string_view colormapGlsl() noexcept;

}  // namespace PJ
