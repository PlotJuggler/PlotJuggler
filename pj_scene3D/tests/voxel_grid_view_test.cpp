// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/voxel_grid_view.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"  // PointField
#include "pj_base/builtin/voxel_grid.hpp"

namespace pj::scene3d {
namespace {

using PJ::sdk::PointField;
using PJ::sdk::VoxelGrid;

VoxelGrid makeGrid(
    uint32_t cols, uint32_t rows, uint32_t slices, uint32_t cell_stride, std::vector<PointField> fields,
    const std::vector<uint8_t>& bytes) {
  VoxelGrid grid;
  grid.column_count = cols;
  grid.row_count = rows;
  grid.slice_count = slices;
  grid.cell_stride = cell_stride;
  grid.row_stride = cell_stride * cols;
  grid.slice_stride = grid.row_stride * rows;
  grid.cell_size = {1.0, 1.0, 1.0};
  grid.fields = std::move(fields);
  grid.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  return grid;
}

PointField scalarField(const std::string& name, uint32_t offset, PointField::Datatype dt) {
  PointField f;
  f.name = name;
  f.offset = offset;
  f.datatype = dt;
  f.count = 1;
  return f;
}

TEST(VoxelGridView, CountAndOffset) {
  std::vector<uint8_t> bytes(2 * 2 * 2, 0);
  const VoxelGrid grid = makeGrid(2, 2, 2, 1, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  EXPECT_EQ(voxelCount(grid), 8U);
  // offset = sz*slice_stride + ry*row_stride + cx*cell_stride; strides {1,2,4}.
  EXPECT_EQ(voxelByteOffset(grid, 0, 0, 0), 0U);
  EXPECT_EQ(voxelByteOffset(grid, 1, 0, 0), 1U);
  EXPECT_EQ(voxelByteOffset(grid, 0, 1, 0), 2U);
  EXPECT_EQ(voxelByteOffset(grid, 1, 1, 1), 7U);
}

TEST(VoxelGridView, OffsetHonoursStridePadding) {
  // row_stride > cols*cell_stride (trailing per-row padding); slice padding too.
  std::vector<uint8_t> bytes(64, 0);
  VoxelGrid grid = makeGrid(2, 2, 1, 1, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  grid.row_stride = 8;     // 6 bytes padding after the 2 cells
  grid.slice_stride = 32;  // padded slice
  EXPECT_EQ(voxelByteOffset(grid, 1, 0, 0), 1U);
  EXPECT_EQ(voxelByteOffset(grid, 0, 1, 0), 8U);
  EXPECT_EQ(voxelByteOffset(grid, 1, 1, 0), 9U);
}

TEST(VoxelGridView, CountZeroWhenAnyDimZero) {
  std::vector<uint8_t> bytes;
  EXPECT_EQ(voxelCount(makeGrid(0, 4, 4, 1, {}, bytes)), 0U);
  EXPECT_EQ(voxelCount(makeGrid(4, 0, 4, 1, {}, bytes)), 0U);
  EXPECT_EQ(voxelCount(makeGrid(4, 4, 0, 1, {}, bytes)), 0U);
}

TEST(VoxelGridView, PackScalarFieldUint8InZyxOrder) {
  std::vector<uint8_t> bytes = {10, 20, 30, 40};
  const VoxelGrid grid = makeGrid(2, 2, 1, 1, {scalarField("occ", 0, PointField::Datatype::kUint8)}, bytes);
  const std::vector<float> packed = packScalarField(grid, grid.fields[0]);
  ASSERT_EQ(packed.size(), 4U);
  EXPECT_FLOAT_EQ(packed[0], 10.0f);  // (cx0,ry0)
  EXPECT_FLOAT_EQ(packed[1], 20.0f);  // (cx1,ry0)
  EXPECT_FLOAT_EQ(packed[2], 30.0f);  // (cx0,ry1)
  EXPECT_FLOAT_EQ(packed[3], 40.0f);  // (cx1,ry1)
}

TEST(VoxelGridView, PackScalarFieldFloatAtNonZeroOffsetWithCellStride) {
  // Two-field voxel: occupancy(uint8 @0) + cost(float32 @4), cell_stride 8.
  std::vector<uint8_t> bytes(2 * 8, 0);
  const float costs[2] = {1.5f, -2.25f};
  std::memcpy(bytes.data() + 4, &costs[0], 4);      // voxel 0 cost at byte 4
  std::memcpy(bytes.data() + 8 + 4, &costs[1], 4);  // voxel 1 cost at byte 12
  const VoxelGrid grid = makeGrid(
      2, 1, 1, 8,
      {scalarField("occ", 0, PointField::Datatype::kUint8), scalarField("cost", 4, PointField::Datatype::kFloat32)},
      bytes);
  const std::vector<float> packed = packScalarField(grid, grid.fields[1]);
  ASSERT_EQ(packed.size(), 2U);
  EXPECT_FLOAT_EQ(packed[0], 1.5f);
  EXPECT_FLOAT_EQ(packed[1], -2.25f);
}

TEST(VoxelGridView, PackScalarFieldShortDataPacksZero) {
  std::vector<uint8_t> bytes = {7};  // only one byte for a 2-voxel grid
  const VoxelGrid grid = makeGrid(2, 1, 1, 1, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  const std::vector<float> packed = packScalarField(grid, grid.fields[0]);
  ASSERT_EQ(packed.size(), 2U);
  EXPECT_FLOAT_EQ(packed[0], 7.0f);
  EXPECT_FLOAT_EQ(packed[1], 0.0f);  // out-of-bounds voxel → 0
}

// column/row/slice_count are unbounded uint32 off the wire (no codec cross-checks them against data.size()) and pack*
// size their output from their product. A corrupt grid declaring 2^32-1 x 2^32-1 x 1 (past std::vector::max_size())
// would make out.reserve(count) throw std::length_error, and packing runs on the Qt render thread with no try/catch
// (std::terminate). packScalarField refuses any count past kMaxRenderableVoxels and returns empty (the layer checks the
// same cap and warns before packing).
TEST(VoxelGridView, PackScalarFieldHugeDimsRefused) {
  std::vector<uint8_t> bytes(16, 0);  // tiny payload; the dims lie about the real size
  const VoxelGrid grid =
      makeGrid(4294967295U, 4294967295U, 1U, 1U, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  ASSERT_GT(voxelCount(grid), (uint64_t{1} << 62));  // product did not overflow to something small
  std::vector<float> packed;
  EXPECT_NO_THROW(packed = packScalarField(grid, grid.fields[0]));
  EXPECT_TRUE(packed.empty());  // over-cap dims → refused, not a bogus allocation
}

// Same untrusted-dims guard on the RGBA path (a uint8x4 colour field).
TEST(VoxelGridView, PackRgbaHugeDimsRefused) {
  std::vector<uint8_t> bytes(16, 0);
  PointField rgba;
  rgba.name = "rgba";
  rgba.offset = 0;
  rgba.datatype = PointField::Datatype::kUint8;
  rgba.count = 4;
  const VoxelGrid grid = makeGrid(4294967295U, 4294967295U, 1U, 4U, {rgba}, bytes);
  std::vector<uint8_t> packed;
  EXPECT_NO_THROW(packed = packRgbaField(grid, grid.fields[0]));
  EXPECT_TRUE(packed.empty());
}

TEST(VoxelGridView, PackRgbaUint8Count4) {
  std::vector<uint8_t> bytes = {10, 20, 30, 40};
  PointField f;
  f.name = "rgba";
  f.offset = 0;
  f.datatype = PointField::Datatype::kUint8;
  f.count = 4;
  const VoxelGrid grid = makeGrid(1, 1, 1, 4, {f}, bytes);
  const std::vector<uint8_t> packed = packRgbaField(grid, grid.fields[0]);
  ASSERT_EQ(packed.size(), 4U);
  EXPECT_EQ(packed[0], 10);
  EXPECT_EQ(packed[1], 20);
  EXPECT_EQ(packed[2], 30);
  EXPECT_EQ(packed[3], 40);
}

TEST(VoxelGridView, PackRgbUint8Count3GetsOpaqueAlpha) {
  std::vector<uint8_t> bytes = {10, 20, 30};
  PointField f;
  f.name = "rgb";
  f.offset = 0;
  f.datatype = PointField::Datatype::kUint8;
  f.count = 3;
  const VoxelGrid grid = makeGrid(1, 1, 1, 3, {f}, bytes);
  const std::vector<uint8_t> packed = packRgbaField(grid, grid.fields[0]);
  ASSERT_EQ(packed.size(), 4U);
  EXPECT_EQ(packed[3], 255);  // alpha defaulted opaque
}

TEST(VoxelGridView, ChooseDefaultField) {
  EXPECT_EQ(chooseDefaultField({}).index, -1);

  const VoxelFieldSelection scalar = chooseDefaultField({scalarField("cost", 0, PointField::Datatype::kFloat32)});
  EXPECT_EQ(scalar.index, 0);
  EXPECT_EQ(scalar.kind, VoxelValueKind::kScalar);

  PointField rgba;
  rgba.name = "RGBA";  // case-insensitive
  rgba.datatype = PointField::Datatype::kUint8;
  rgba.count = 4;
  const VoxelFieldSelection color = chooseDefaultField({scalarField("x", 0, PointField::Datatype::kFloat32), rgba});
  EXPECT_EQ(color.index, 1);
  EXPECT_EQ(color.kind, VoxelValueKind::kRgba);
}

TEST(VoxelGridBounds, IdentityOriginUnitCells) {
  std::vector<uint8_t> bytes(8, 0);
  const VoxelGrid grid = makeGrid(2, 2, 2, 1, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  const AABB box = voxelGridBounds(grid);
  ASSERT_TRUE(box.valid);
  EXPECT_FLOAT_EQ(box.min.x, 0.0f);
  EXPECT_FLOAT_EQ(box.min.y, 0.0f);
  EXPECT_FLOAT_EQ(box.min.z, 0.0f);
  EXPECT_FLOAT_EQ(box.max.x, 2.0f);
  EXPECT_FLOAT_EQ(box.max.y, 2.0f);
  EXPECT_FLOAT_EQ(box.max.z, 2.0f);
}

TEST(VoxelGridBounds, OriginTranslationShiftsBox) {
  std::vector<uint8_t> bytes(8, 0);
  VoxelGrid grid = makeGrid(2, 2, 2, 1, {scalarField("v", 0, PointField::Datatype::kUint8)}, bytes);
  grid.origin.position = {10.0, -5.0, 1.0};
  const AABB box = voxelGridBounds(grid);
  ASSERT_TRUE(box.valid);
  EXPECT_FLOAT_EQ(box.min.x, 10.0f);
  EXPECT_FLOAT_EQ(box.max.x, 12.0f);
  EXPECT_FLOAT_EQ(box.min.y, -5.0f);
  EXPECT_FLOAT_EQ(box.max.z, 3.0f);
}

TEST(VoxelGridBounds, EmptyGridInvalid) {
  std::vector<uint8_t> bytes;
  EXPECT_FALSE(voxelGridBounds(makeGrid(0, 0, 0, 1, {}, bytes)).valid);
}

}  // namespace
}  // namespace pj::scene3d
