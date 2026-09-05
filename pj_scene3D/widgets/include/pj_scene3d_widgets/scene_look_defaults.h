// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Single source of truth for the 3D scene's default LOOK parameters — the values
// that MeshShadingParams, CompositeParams, SsaoPass and EdlPass bake as their
// defaults and that the scene3d_mesh_viewer demo seeds its look-dev controls
// from. Keeping them here (instead of duplicated literals) means the application
// and the demo can never drift apart: change a default once and both follow.
//
// These are the User's 2026-06-13 look-dev picks. Pure C++ (glm + <cmath>), no
// Qt, so it is includable from every layer that needs a default.
namespace pj::scene3d::look {

// World-space direction TO the key ("sun") light from azimuth/elevation in
// degrees (Z-up). Shared by MeshShadingParams' key_light_dir default and the
// demo's key-light sliders / CLI overrides, so the angle defaults live in ONE
// place. Not constexpr: std::cos/std::sin are not constexpr in C++20.
inline glm::vec3 keyDirFromAzEl(float az_deg, float el_deg) {
  const float az = glm::radians(az_deg);
  const float el = glm::radians(el_deg);
  const float ce = std::cos(el);
  return glm::vec3(ce * std::cos(az), ce * std::sin(az), std::sin(el));
}

// ---- Mesh shading (MeshShadingParams) ----
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
// Tint for collision geometry that has no <material> (RViz convention) — keeps
// collision hulls visually distinct from the visual meshes.
inline constexpr glm::vec4 kCollisionDefaultColor{1.0f, 0.5f, 0.1f, 1.0f};

// ---- GridMap heightfield shading (GridMapRenderPass) ----
// Lambert weights scaled by the view's ambient / direct / fill knobs, tuned so a
// flat, top-lit map sits near the cube passes' brightness. The ambient weight is
// doubled relative to the cubes' because the heightfield has no AO term.
inline constexpr float kGridMapAmbient = 0.7f;
inline constexpr float kGridMapDirect = 0.45f;
inline constexpr float kGridMapFill = 0.4f;

// ---- Composite / post (CompositeParams) ----
inline constexpr int kTonemapMode = 1;  // 0 None, 1 ACES, 2 AgX, 3 Neutral
inline constexpr float kExposure = 1.3f;
inline constexpr float kSaturation = 1.3f;  // post-tonemap saturation boost
inline constexpr float kAoStrength = 1.0f;  // SSAO blend into the composite
inline constexpr float kEdlFloor = 0.3f;    // EDL darkens toward floor*color

// ---- Screen-space passes (SsaoPass / EdlPass) ----
inline constexpr float kSsaoRadiusM = 0.4f;  // SSAO sample radius (metres)
inline constexpr float kSsaoPower = 1.0f;    // SSAO contrast exponent
inline constexpr float kEdlStrength = 1.0f;  // EDL response strength
inline constexpr float kEdlRadiusPx = 0.6f;  // EDL neighbour radius (pixels; plan spec was 1.4)
// Per-neighbour clamp on the log-depth gap (EDL is mesh-only). Surface creases
// produce gaps far below this, so they are unaffected; only a mesh pixel's
// silhouette — against a farther mesh, a non-mesh pixel (point cloud / grid /
// axes), or the empty background — exceeds it and gets bounded, turning a solid
// black band into a graded outline. Raising it darkens/widens that outline.
inline constexpr float kEdlMaxGap = 0.02f;

// ---- Mesh shadows ----
// PCF penumbra radius in shadow-map texels (16-tap Poisson disk). Larger = softer
// edge; the world-space penumbra is this times world_units_per_texel, so it tracks
// the fitted frustum. ~4 texels reads as a soft contact shadow without the banding a
// sparse box kernel gives.
inline constexpr float kShadowSoftnessTexels = 4.0f;
// Slope-scaled depth bias applied while rendering casters so their stored depth
// is pushed away from the light — kills self-shadowing acne. Paired with the
// receiver-side world normal offset below; conservative defaults shared by the
// native GL pass and the browser QRhi pipeline.
inline constexpr float kShadowSlopeScaledDepthBias = 2.0f;
inline constexpr float kShadowDepthBiasUnits = 4.0f;
// Receiver-side normal offset in shadow-texel units (scaled by
// world_units_per_texel at the call sites).
inline constexpr float kShadowNormalOffsetTexels = 1.5f;
// SSAO depth-compare bias (view-space metres).
inline constexpr float kSsaoBias = 0.025f;

// ---- Scene geometry colors (grid / frame axes / TF connections) ----
// Shared by the native GL passes and the WASM QRhi geometry builder so the two
// backends cannot drift apart.
inline constexpr glm::vec3 kGridLineColor{0.35f, 0.35f, 0.35f};
inline constexpr glm::vec3 kGridCellToneA{0.30f, 0.30f, 0.30f};
inline constexpr glm::vec3 kGridCellToneB{0.42f, 0.42f, 0.42f};
// Slightly desaturated R/G/B so adjacent frames don't clash visually with the
// HUD overlay (which uses the saturated triplets).
inline constexpr glm::vec3 kAxisTriadX{0.95f, 0.30f, 0.30f};
inline constexpr glm::vec3 kAxisTriadY{0.30f, 0.85f, 0.30f};
inline constexpr glm::vec3 kAxisTriadZ{0.35f, 0.50f, 1.00f};
inline constexpr glm::vec3 kTfConnectionColor{1.0f, 0.0f, 1.0f};  // magenta

}  // namespace pj::scene3d::look
