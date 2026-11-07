// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Shared unit-cube geometry + the C++ mirrors of the GLSL helpers (shaders/cube/*.glslinc,
// which the shaders #include; scene3d_shader_constants_test checks the mirrors against
// them) for every instanced-cube pass (the point-cloud "cube" shape and the voxel grid).
// Keeping the mesh and its vertex-attrib contract (location 0 = position, location 1 =
// outward normal) in one place means the two passes can't drift, and both draw an
// identically-edged cube ("unify the cube").
//
// Two geometries live here, for two different cost profiles:
//
//   kCubeVertices/kCubeIndices — the full 24-vertex solid. Use it when a pass must
//     survive the camera being INSIDE a cube, or when instances are few enough
//     that the geometry cost is irrelevant (voxel grids: one instance per voxel,
//     but the shader discards most of them before the corner math).
//
//   kCubeFanIndices + cube_fan.glslinc — an attributeless 7-vertex hexagon fan
//     covering only the 3 camera-facing faces. A closed opaque cube never shows
//     the other three, so this is pixel-equivalent at 1/3.4 the vertex
//     invocations and half the triangles. Use it for high-instance-count passes
//     (point clouds).
//
// Below a couple of pixels even the fan is more geometry than the result can show,
// so cube_sprite.glslinc degrades a cube to a single area- and brightness-matched point
// sprite. Its constants are tied to cube_lighting.glslinc / cube_edge.glslinc on purpose —
// the whole point is that crossing the threshold changes nothing on screen.

namespace pj::scene3d {

// One vertex of the unit cube: position (each axis +/-0.5) + outward face normal.
struct CubeVertex {
  float px, py, pz;
  float nx, ny, nz;
};

// 24-vertex unit cube (4 verts per face x 6 faces), each carrying its outward face
// normal.
//
// WINDING INVARIANT: each face is CCW viewed from OUTSIDE, so GL's default front
// face (GL_CCW) selects the outward side and GL_BACK culling keeps what the camera
// can see. Passes that enable culling — and the QRhi pipelines that set
// CullMode::Back — depend on this; verify any edit with the cross product of the
// first two edges, not by eye.
inline constexpr std::array<CubeVertex, 24> kCubeVertices = {{
    // +X face, normal (1, 0, 0)
    {0.5f, -0.5f, -0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 0.5f, -0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    {0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
    // -X face, normal (-1, 0, 0)
    {-0.5f, -0.5f, 0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, 0.5f, -0.5f, -1.0f, 0.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, -1.0f, 0.0f, 0.0f},
    // +Y face, normal (0, 1, 0)
    {-0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    {-0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f},
    {0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f},
    {0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    // -Y face, normal (0, -1, 0)
    {-0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f},
    {0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f},
    {0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f},
    // +Z face, normal (0, 0, 1)
    {-0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    {-0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
    // -Z face, normal (0, 0, -1)
    {0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {-0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {-0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
    {0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f},
}};

// uint16 rather than uint8: several drivers have no native 8-bit index path and
// silently convert the buffer on every draw.
inline constexpr std::array<uint16_t, 36> kCubeIndices = {{
    0,  1,  2,  0,  2,  3,   // +X
    4,  5,  6,  4,  6,  7,   // -X
    8,  9,  10, 8,  10, 11,  // +Y
    12, 13, 14, 12, 14, 15,  // -Y
    16, 17, 18, 16, 18, 19,  // +Z
    20, 21, 22, 20, 22, 23,  // -Z
}};

// Hexagon fan over the 3 camera-facing faces: 6 triangles sharing the near corner
// (logical vertex 0) around the 6-vertex silhouette ring (1..6). No vertex buffer
// — the corner positions come from cube_fan.glslinc's cubeFanCorner(gl_VertexID, s).
// Draw as 18 instanced indexed triangles-list vertices.
// Consecutive pairs of triangles tile one face each; which face a pair lands on
// depends on the per-instance sign vector, so don't annotate them by axis.
inline constexpr std::array<uint16_t, 18> kCubeFanIndices = {{
    0,
    1,
    2,
    0,
    2,
    3,  // first visible face
    0,
    3,
    4,
    0,
    4,
    5,  // second visible face
    0,
    5,
    6,
    0,
    6,
    1,  // third visible face
}};

// C++ mirrors of the numbers the edge band is built from, so kCubeEdgeMeanScale can be
// RE-DERIVED in a headless test instead of trusted, on every platform.
inline constexpr float kCubeEdgeWidth = 0.06f;
inline constexpr float kCubeEdgeDarken = 0.6f;
inline constexpr float kCubeSrgbGamma = 2.2f;

// Mean of pow(1 - kCubeEdgeDarken * cubeEdgeFactor(local), kCubeSrgbGamma) over one
// face: how much darker the edge band makes a cube on average, in the LINEAR space the
// LOD sprite writes. Integrated in closed-ish form over the face's distance-to-edge
// distribution — for a unit face the distance t = min(0.5-|u|, 0.5-|v|) has density
// 4(1-2t) on [0, 0.5], and the band only bites below kCubeEdgeWidth.
[[nodiscard]] inline double cubeEdgeMeanScale(int samples = 100000) {
  double total = 0.0;
  for (int i = 0; i < samples; ++i) {
    const double t = 0.5 * (static_cast<double>(i) + 0.5) / samples;
    const double s = std::clamp(t / kCubeEdgeWidth, 0.0, 1.0);
    const double smoothstep = s * s * (3.0 - 2.0 * s);
    const double edge_factor = 1.0 - smoothstep;
    const double density = 4.0 * (1.0 - 2.0 * t);
    total += std::pow(1.0 - kCubeEdgeDarken * edge_factor, kCubeSrgbGamma) * density * (0.5 / samples);
  }
  return total;
}

// The value cube_sprite.glslinc hard-codes, mirrored for the test that checks the two agree.
inline constexpr float kCubeEdgeMeanScale = 0.887f;

// kSphereDiscShading from cube_point_color.glslinc, mirrored for the test that re-derives it.
inline constexpr float kSphereDiscShading = 0.702f;

// Mean of the shared Lambert (ambient 0.35, diffuse 0.65, key light (0.4, 0.5, 0.8))
// over a sphere imposter's unit disc, whose normal is (x, y, sqrt(1 - x^2 - y^2)).
[[nodiscard]] inline double sphereDiscMeanShading(int samples = 1000) {
  const double norm = std::sqrt(0.4 * 0.4 + 0.5 * 0.5 + 0.8 * 0.8);
  double total = 0.0;
  int inside = 0;
  for (int j = 0; j < samples; ++j) {
    for (int i = 0; i < samples; ++i) {
      const double x = 2.0 * (i + 0.5) / samples - 1.0;
      const double y = 2.0 * (j + 0.5) / samples - 1.0;
      const double r2 = x * x + y * y;
      if (r2 > 1.0) {
        continue;
      }
      const double lit = (0.4 * x + 0.5 * y + 0.8 * std::sqrt(1.0 - r2)) / norm;
      total += 0.35 + 0.65 * std::max(lit, 0.0);
      ++inside;
    }
  }
  return total / inside;
}

}  // namespace pj::scene3d
