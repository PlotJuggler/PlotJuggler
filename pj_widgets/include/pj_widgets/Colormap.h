#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string_view>
#include <vector>

namespace PJ {

/// Scientific colormaps shared by the visualization views — the 3D pointcloud
/// field colouring (`PointcloudRenderPass`) and the 2D depth-image LUT
/// (`DepthImageLayer`) — so the same scalar maps to the same colour in every
/// view. The order is the LUT row index AND the persisted id: append only,
/// never reorder.
enum class Colormap : uint8_t {
  kTurbo,
  kViridis,
  kPlasma,
  kGrayscale,
};

/// Number of colormaps (= rows in the LUT texture). Keep in sync with the enum.
inline constexpr int kColormapCount = 4;

/// Untranslated display name ("Turbo", "Viridis", ...), a `tr()`-ready literal for
/// the colormap selectors so every view lists the same names in the enum order.
[[nodiscard]] const char* colormapName(Colormap colormap) noexcept;

/// Default LUT width (samples of t per colormap row). 256 8-bit entries is the
/// resolution the GPU LUT texture is built and uploaded at.
inline constexpr int kColormapLutWidth = 256;

/// Linear RGB in [0, 1]. A minimal colour type so this helper depends on nothing
/// beyond Qt + the standard library (pj_widgets' dependency contract — notably
/// no pj_base, hence not sdk::ColorRGBA).
struct ColormapRgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

/// Map t in [0, 1] to a colour. `turbo` is Google's degree-5 polynomial; `viridis`
/// and `plasma` are Matt Zucker's degree-6 fits of matplotlib. Out-of-range t is
/// clamped. These use the same coefficients as colormapGlsl(), so the 2D LUT path
/// and the 3D in-shader path agree to within the LUT's 256-entry 8-bit quantization.
[[nodiscard]] ColormapRgb colorFor(Colormap colormap, float t) noexcept;

/// Build a `width` x kColormapCount RGBA8 lookup table, row-major: row `c` holds
/// colormap `c` sampled across t in [0, 1]. Uploaded once as a GPU LUT texture
/// (row = colormap id, column = t) by the LUT-sampling backends (the 2D depth
/// shader). `width` < 1 is treated as 1.
[[nodiscard]] std::vector<uint8_t> buildColormapLut(int width = kColormapLutWidth);

/// GLSL source defining `turbo`/`viridis`/`plasma` and a
/// `vec3 sampleColormap(int id, float t)` dispatcher (id == Colormap value).
/// Injected into a fragment shader by backends that colour in-shader instead of
/// via a LUT (the 3D pointcloud pass). Same coefficients as colorFor().
[[nodiscard]] std::string_view colormapGlsl() noexcept;

}  // namespace PJ
