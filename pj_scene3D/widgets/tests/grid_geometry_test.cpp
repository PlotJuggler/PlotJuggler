// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/grid_geometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <utility>

namespace pj::scene3d {
namespace {

constexpr float kExtent = 10.0f;
constexpr int kDivisions = 10;

// Recover the integer cell index (i, j) that a quad vertex belongs to, from its
// position on the centered z=0 plane. half = extent/2, cell = extent/divisions.
std::pair<int, int> cellIndexOf(const glm::vec3& pos, float extent_m, int divisions) {
  const float half = extent_m * 0.5f;
  const float cell = extent_m / static_cast<float>(divisions);
  return {static_cast<int>(std::lround((pos.x + half) / cell)), static_cast<int>(std::lround((pos.y + half) / cell))};
}

// The whole point of the change: a *full* checkerboard emits every cell, not
// just the even ones. Six vertices (two triangles) per cell, no cell skipped.
TEST(GridGeometryTest, CheckerboardEmitsEveryCell) {
  const auto verts = buildCheckerboardCells(kExtent, kDivisions);
  EXPECT_EQ(verts.size(), static_cast<std::size_t>(kDivisions) * kDivisions * 6U);

  // Every (i, j) cell in [0, divisions) appears, and each appears exactly once.
  std::set<std::pair<int, int>> covered;
  for (std::size_t base = 0; base < verts.size(); base += 6U) {
    // The first vertex of each quad is the (x0, y0) corner of its cell.
    covered.insert(cellIndexOf(verts[base].pos, kExtent, kDivisions));
  }
  EXPECT_EQ(covered.size(), static_cast<std::size_t>(kDivisions) * kDivisions);
}

// Each cell's six vertices share parity == (i + j) & 1, and both tones appear.
TEST(GridGeometryTest, CheckerboardParityAlternates) {
  const auto verts = buildCheckerboardCells(kExtent, kDivisions);
  ASSERT_EQ(verts.size(), static_cast<std::size_t>(kDivisions) * kDivisions * 6U);

  bool saw_zero = false;
  bool saw_one = false;
  for (std::size_t base = 0; base < verts.size(); base += 6U) {
    const auto [i, j] = cellIndexOf(verts[base].pos, kExtent, kDivisions);
    const float expected = static_cast<float>((i + j) & 1);
    for (std::size_t k = 0; k < 6U; ++k) {
      EXPECT_FLOAT_EQ(verts[base + k].parity, expected) << "cell (" << i << "," << j << ")";
    }
    saw_zero = saw_zero || (expected == 0.0f);
    saw_one = saw_one || (expected == 1.0f);
  }
  EXPECT_TRUE(saw_zero);
  EXPECT_TRUE(saw_one);
}

// Lines are unchanged: (divisions+1) per axis, 2 verts each, all parity 0.
TEST(GridGeometryTest, LinesCountAndParity) {
  const auto verts = buildGridLines(kExtent, kDivisions);
  EXPECT_EQ(verts.size(), static_cast<std::size_t>(kDivisions + 1) * 4U);
  for (const auto& vertex : verts) {
    EXPECT_FLOAT_EQ(vertex.parity, 0.0f);
  }
}

}  // namespace
}  // namespace pj::scene3d
