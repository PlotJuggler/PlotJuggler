// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Qt-free coverage of the GridMap CPU pack: de-interleave of one field out of a
// multi-field record, NaN holes preserved, short payloads pack as holes, the
// finite-only range, the default elevation pick, and the source-frame bounds with
// a rotated origin.

#include "pj_scene3d_core/grid_map_view.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace {

using namespace pj::scene3d;
using PJ::sdk::PointField;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// 3 columns x 2 rows, two float32 fields interleaved per cell: elevation then cost.
struct Fixture {
  std::vector<uint8_t> bytes;
  PJ::sdk::GridMap grid;

  Fixture() {
    const float elevation[6] = {0.0f, 1.0f, 2.0f, 3.0f, kNaN, 5.0f};
    const float cost[6] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f};
    bytes.resize(6 * 8);
    for (int i = 0; i < 6; ++i) {
      std::memcpy(bytes.data() + i * 8, &elevation[i], 4);
      std::memcpy(bytes.data() + i * 8 + 4, &cost[i], 4);
    }
    grid.column_count = 3;
    grid.row_count = 2;
    grid.cell_stride = 8;
    grid.row_stride = 24;
    grid.cell_size = {0.5, 0.25};
    grid.fields = {
        PointField{.name = "elevation", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
        PointField{.name = "cost", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1}};
    grid.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  }
};

TEST(GridMapView, PackDeinterleavesOneFieldRowMajor) {
  const Fixture f;
  const auto elevation = packGridMapField(f.grid, f.grid.fields[0]);
  const auto cost = packGridMapField(f.grid, f.grid.fields[1]);
  ASSERT_EQ(elevation.size(), 6u);
  ASSERT_EQ(cost.size(), 6u);
  EXPECT_FLOAT_EQ(elevation[0], 0.0f);
  EXPECT_FLOAT_EQ(elevation[3], 3.0f);  // row 1, column 0
  EXPECT_TRUE(std::isnan(elevation[4]));
  EXPECT_FLOAT_EQ(cost[4], 14.0f);  // the hole is per-field, not per-cell
}

TEST(GridMapView, ShortPayloadPacksHoles) {
  Fixture f;
  f.grid.data = PJ::Span<const uint8_t>(f.bytes.data(), 8 * 4);  // only 4 of 6 cells present
  FiniteRange range;
  const auto cost = packGridMapField(f.grid, f.grid.fields[1], &range);
  ASSERT_EQ(cost.size(), 6u);
  EXPECT_FLOAT_EQ(cost[3], 13.0f);
  EXPECT_TRUE(std::isnan(cost[4]));
  EXPECT_TRUE(std::isnan(cost[5]));
  EXPECT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.lo, 10.0f);
  EXPECT_FLOAT_EQ(range.hi, 13.0f);
}

TEST(GridMapView, PackRangeMatchesFiniteRangeAndSkipsHoles) {
  const Fixture f;
  FiniteRange range;
  const auto elevation = packGridMapField(f.grid, f.grid.fields[0], &range);
  const FiniteRange expected = finiteRange(elevation);
  EXPECT_EQ(range.valid, expected.valid);
  EXPECT_FLOAT_EQ(range.lo, expected.lo);
  EXPECT_FLOAT_EQ(range.hi, expected.hi);
  EXPECT_FLOAT_EQ(range.hi, 5.0f);
}

TEST(GridMapView, PackRefusesDegenerateAndOversized) {
  Fixture f;
  f.grid.row_count = 0;
  EXPECT_TRUE(packGridMapField(f.grid, f.grid.fields[0]).empty());
  f.grid.row_count = 0x7fffffff;
  f.grid.column_count = 0x7fffffff;
  EXPECT_TRUE(packGridMapField(f.grid, f.grid.fields[0]).empty());
}

TEST(GridMapView, FiniteRangeSkipsNaN) {
  const auto range = finiteRange({kNaN, 2.0f, -1.0f, std::numeric_limits<float>::infinity()});
  EXPECT_TRUE(range.valid);
  EXPECT_FLOAT_EQ(range.lo, -1.0f);
  EXPECT_FLOAT_EQ(range.hi, 2.0f);
  EXPECT_FALSE(finiteRange({kNaN, kNaN}).valid);
  EXPECT_FALSE(finiteRange({}).valid);
}

TEST(GridMapView, DefaultElevationPrefersNamedThenFirstFloat) {
  const Fixture f;
  ASSERT_NE(defaultElevationField(f.grid.fields), nullptr);
  EXPECT_EQ(defaultElevationField(f.grid.fields)->name, "elevation");

  std::vector<PointField> fields = {
      PointField{.name = "id", .offset = 0, .datatype = PointField::Datatype::kUint16, .count = 1},
      PointField{.name = "height", .offset = 2, .datatype = PointField::Datatype::kFloat32, .count = 1}};
  EXPECT_EQ(defaultElevationField(fields)->name, "height");
  fields.pop_back();
  EXPECT_EQ(defaultElevationField(fields), nullptr);
  EXPECT_TRUE(isScalarGridMapField(fields[0]));
  fields[0].count = 3;
  EXPECT_FALSE(isScalarGridMapField(fields[0]));
}

TEST(GridMapView, BoundsFollowRotatedOriginAndElevationRange) {
  Fixture f;
  // Origin rotated 90 degrees about z at (10, 20, 1): local +x maps to world +y.
  f.grid.origin.position = {10.0, 20.0, 1.0};
  f.grid.origin.orientation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
  const AABB box = gridMapBounds(f.grid, FiniteRange{-0.5f, 2.0f, true});
  ASSERT_TRUE(box.valid);
  // Local extent is 1.5 (x) by 0.5 (y): rotated, x spans [10 - 0.5, 10], y spans [20, 21.5].
  EXPECT_NEAR(box.min.x, 9.5f, 1e-4f);
  EXPECT_NEAR(box.max.x, 10.0f, 1e-4f);
  EXPECT_NEAR(box.min.y, 20.0f, 1e-4f);
  EXPECT_NEAR(box.max.y, 21.5f, 1e-4f);
  EXPECT_NEAR(box.min.z, 0.5f, 1e-4f);
  EXPECT_NEAR(box.max.z, 3.0f, 1e-4f);

  // No elevation channel (the "None" flat mode): the plane at z = 0.
  const AABB flat = gridMapBounds(f.grid, std::nullopt);
  EXPECT_NEAR(flat.min.z, 1.0f, 1e-4f);
  EXPECT_NEAR(flat.max.z, 1.0f, 1e-4f);
  // An elevation channel with no finite sample renders nothing: no bounds.
  EXPECT_FALSE(gridMapBounds(f.grid, FiniteRange{}).valid);
  f.grid.column_count = 0;
  EXPECT_FALSE(gridMapBounds(f.grid, std::nullopt).valid);
}

}  // namespace
