// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <glm/ext/vector_double3.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Single source of truth for the 3D scene's default LOOK parameters: the values MeshShadingParams, CompositeParams and
// the SSAO/EDL stages bake as defaults. Pure C++ (glm + <cmath>), no Qt, so every layer can include it.
namespace pj::scene3d::look {

// The exact sRGB decode of one display-referred channel (0..1) to linear light: the C++ twin of srgbToLinear() in
// rhi/shaders/srgb.glslinc, for colours the CPU linearizes (the HDR clear) and mirrors of shader maths (cube_mesh.h).
inline double srgbToLinear(double channel) {
  const double c = std::max(channel, 0.0);
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// World-space direction TO the key ("sun") light from azimuth/elevation in degrees (Z-up), shared by
// MeshShadingParams' key_light_dir default and the demo's key-light controls. Not constexpr: std::cos/std::sin are not
// constexpr in C++20.
inline glm::vec3 keyDirFromAzEl(float az_deg, float el_deg) {
  const float az = glm::radians(az_deg);
  const float el = glm::radians(el_deg);
  const float ce = std::cos(el);
  return glm::vec3(ce * std::cos(az), ce * std::sin(az), std::sin(el));
}

inline constexpr float kRoughness = 0.6f;              // visual-mesh GGX roughness
inline constexpr float kReflectivity = 0.06f;          // dielectric f0
inline constexpr float kAmbientScale = 0.5f;           // image-based ambient weight
inline constexpr float kKeyLightScale = 1.6f;          // fixed world "sun" weight
inline constexpr float kFillLightScale = 0.5f;         // camera-locked fill weight
inline constexpr float kEnvIntensity = 1.0f;           // analytic specular IBL weight
inline constexpr float kKeyLightAzimuthDeg = 40.0f;    // key-light azimuth (Z-up)
inline constexpr float kKeyLightElevationDeg = 55.0f;  // key-light elevation
inline constexpr float kMeshOpacity = 1.0f;
inline constexpr float kCollisionOpacity = 0.4f;
// Tint for collision geometry that has no <material> (RViz convention).
inline constexpr glm::vec4 kCollisionDefaultColor{1.0f, 0.5f, 0.1f, 1.0f};

// Lit colormapped surfaces (GridMapRhiPass, MeshMapRhiPass): Lambert weights scaled by the view's ambient / direct /
// fill knobs, tuned so a flat, top-lit map sits near the cube passes' brightness. The ambient weight is doubled
// relative to the cubes' because these surfaces have no AO term.
inline constexpr float kLitSurfaceAmbient = 0.7f;
inline constexpr float kLitSurfaceDirect = 0.45f;
inline constexpr float kLitSurfaceFill = 0.4f;

inline constexpr int kTonemapMode = 1;  // 0 None, 1 ACES, 2 AgX, 3 Neutral
inline constexpr float kExposure = 1.0f;
inline constexpr float kSaturation = 1.15f;  // post-tonemap saturation boost
inline constexpr float kAoStrength = 1.0f;   // SSAO blend into the composite
inline constexpr float kEdlFloor = 0.3f;     // EDL darkens toward floor*color

inline constexpr float kSsaoRadiusM = 0.4f;  // SSAO sample radius (metres)
inline constexpr float kSsaoPower = 1.0f;    // SSAO contrast exponent
inline constexpr float kEdlStrength = 1.0f;  // EDL response strength
inline constexpr float kEdlRadiusPx = 0.6f;  // EDL neighbour radius (pixels)
// Per-neighbour clamp on the log-depth gap (EDL is mesh-only). Surface creases produce gaps far below it; only a mesh
// pixel's silhouette (against a farther mesh, a non-mesh pixel or the empty background) exceeds it and gets bounded,
// turning a solid black band into a graded outline. Raising it darkens and widens that outline.
inline constexpr float kEdlMaxGap = 0.02f;

// Mesh shadows: PCF penumbra radius in shadow-map texels (16-tap Poisson disk). Larger = softer edge; the world-space
// penumbra is this times world_units_per_texel, so it tracks the fitted frustum. ~4 texels reads as a soft contact
// shadow without the banding a sparse box kernel gives.
inline constexpr float kShadowSoftnessTexels = 4.0f;
// Slope-scaled depth bias applied while rendering casters, pushing their stored depth away from the light to kill
// self-shadowing acne. Paired with the receiver-side normal offset below; conservative defaults.
inline constexpr float kShadowSlopeScaledDepthBias = 2.0f;
inline constexpr float kShadowDepthBiasUnits = 4.0f;
// Receiver-side normal offset in shadow-texel units (scaled by world_units_per_texel at the call sites).
inline constexpr float kShadowNormalOffsetTexels = 1.5f;
inline constexpr float kSsaoBias = 0.025f;

/// The ground grid's colours, display-referred (sRGB-encoded like the theme tokens they come from): blends of the
/// scene backdrop toward its foreground.
struct GridColors {
  glm::dvec3 line{0.0};    ///< high-contrast grid lines
  glm::dvec3 tone_a{0.0};  ///< checkerboard tile tone A, just above the backdrop
  glm::dvec3 tone_b{0.0};  ///< checkerboard tile tone B

  bool operator==(const GridColors&) const = default;
};

/// Grid colours for a theme's backdrop and foreground (display-referred 0..1): lines at 0.35 of the way to the
/// foreground, tiles at 0.10 and 0.24, so the lines read clearly on top of the fill.
inline GridColors gridColors(const glm::dvec3& backdrop, const glm::dvec3& foreground) {
  const auto blend = [&](double fraction) { return backdrop * (1.0 - fraction) + foreground * fraction; };
  return {blend(0.35), blend(0.10), blend(0.24)};
}
// Luminance gain on the hovered frame's triad colours, so the highlighted gizmo
// reads brighter than its neighbours (clamped to 1; it feeds the HDR pipeline).
inline constexpr float kHighlightGain = 1.8f;
// Slightly desaturated R/G/B so adjacent frames do not clash with the HUD overlay (which uses the saturated triplets).
inline constexpr glm::vec3 kAxisTriadX{0.95f, 0.30f, 0.30f};
inline constexpr glm::vec3 kAxisTriadY{0.30f, 0.85f, 0.30f};
inline constexpr glm::vec3 kAxisTriadZ{0.35f, 0.50f, 1.00f};
inline constexpr glm::vec3 kTfConnectionColor{1.0f, 0.0f, 1.0f};  // magenta
inline constexpr glm::vec3 kHudTriadX{1.00f, 0.30f, 0.30f};
inline constexpr glm::vec3 kHudTriadY{0.35f, 0.85f, 0.35f};
inline constexpr glm::vec3 kHudTriadZ{0.40f, 0.55f, 1.00f};

}  // namespace pj::scene3d::look

namespace pj::scene3d {

// Look knobs of a view's present pass; read every frame, so call the view's invalidateContent() after changing them.
struct CompositeParams {
  int tonemap_mode = look::kTonemapMode;  // 0 None, 1 ACES, 2 AgX, 3 Khronos PBR Neutral
  float exposure = look::kExposure;
  float saturation = look::kSaturation;  // post-tonemap; data pixels only
  float ao_strength = look::kAoStrength;
  bool ssao_enabled = true;
  bool edl_enabled = true;
  float edl_floor = look::kEdlFloor;  // EDL darkens toward floor*color (0 = old black; 1 = off)
};

}  // namespace pj::scene3d
