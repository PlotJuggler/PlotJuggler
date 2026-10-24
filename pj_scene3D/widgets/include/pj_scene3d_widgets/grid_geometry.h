#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <vector>

namespace pj::scene3d {

// Which ground-grid geometry is built and drawn (the persisted grid_style).
enum class GridStyle { kLines, kFilledCells };

// One ground-grid vertex on the z=0 plane plus a checkerboard parity flag
// (0.0 or 1.0). The grid shader mixes between two tile tones by this flag, so
// cells with parity 0 get tone A and parity 1 get tone B. Grid-line vertices
// always carry parity 0 (the line color is set independently via the uniforms).
struct GridVertex {
  glm::vec3 pos;
  float parity;
};

// Line grid: (divisions+1) lines per axis, 2 vertices per line (drawn as
// a line list). extent_m is the full side length; the grid is centered on the
// origin. Every vertex carries parity 0.
std::vector<GridVertex> buildGridLines(float extent_m, int divisions);

// Full checkerboard: EVERY cell is emitted as a solid quad (two triangles,
// 6 vertices, drawn as a triangle list), tagged parity = (i + j) & 1 so the two
// tile tones alternate. Unlike a half-checkerboard, no cell is skipped — the
// total is exactly divisions*divisions*6 vertices.
std::vector<GridVertex> buildCheckerboardCells(float extent_m, int divisions);

}  // namespace pj::scene3d
