// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/grid_geometry.h"

namespace pj::scene3d {

std::vector<GridVertex> buildGridLines(float extent_m, int divisions) {
  std::vector<GridVertex> vertices;
  vertices.reserve(static_cast<std::size_t>(divisions + 1) * 4U);
  const float half = extent_m * 0.5f;
  const float cell = extent_m / static_cast<float>(divisions);
  for (int i = 0; i <= divisions; ++i) {
    const float offset = -half + static_cast<float>(i) * cell;
    vertices.push_back({{-half, offset, 0.0f}, 0.0f});
    vertices.push_back({{half, offset, 0.0f}, 0.0f});
    vertices.push_back({{offset, -half, 0.0f}, 0.0f});
    vertices.push_back({{offset, half, 0.0f}, 0.0f});
  }
  return vertices;
}

std::vector<GridVertex> buildCheckerboardCells(float extent_m, int divisions) {
  std::vector<GridVertex> vertices;
  const float half = extent_m * 0.5f;
  const float cell = extent_m / static_cast<float>(divisions);
  vertices.reserve(static_cast<std::size_t>(divisions) * divisions * 6U);
  for (int i = 0; i < divisions; ++i) {
    for (int j = 0; j < divisions; ++j) {
      // Every cell is emitted; its parity selects one of the two tile tones,
      // which the grid pass resolves on the CPU while appending vertices.
      const float x0 = -half + static_cast<float>(i) * cell;
      const float y0 = -half + static_cast<float>(j) * cell;
      const float x1 = x0 + cell;
      const float y1 = y0 + cell;
      const float parity = static_cast<float>((i + j) & 1);
      vertices.push_back({{x0, y0, 0.0f}, parity});
      vertices.push_back({{x1, y0, 0.0f}, parity});
      vertices.push_back({{x1, y1, 0.0f}, parity});
      vertices.push_back({{x0, y0, 0.0f}, parity});
      vertices.push_back({{x1, y1, 0.0f}, parity});
      vertices.push_back({{x0, y1, 0.0f}, parity});
    }
  }
  return vertices;
}

}  // namespace pj::scene3d
