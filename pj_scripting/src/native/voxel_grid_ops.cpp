// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "voxel_grid_ops.h"

#include <utility>

#include "field_checks.h"
#include "pj_base/buffer_anchor.hpp"

namespace PJ::scripting::native {
namespace {

constexpr const char* kValueChannel = "value";

}  // namespace

Expected<VoxelGridBuilder> VoxelGridBuilder::create(
    std::string frame_id, sdk::Vector3 origin, double cell_size, std::array<int64_t, 3> dims,
    std::shared_ptr<MemoryBudget> budget) {
  if (Status valid = firstFailure(
          {checkFinite("voxels: 'origin'", {origin.x, origin.y, origin.z}),
           checkPositive("voxels: 'cell_size'", cell_size)});
      !valid) {
    return unexpected(valid.error());
  }
  uint64_t cells = 1;
  for (const int64_t count : dims) {
    if (count <= 0) {
      return unexpected(std::string("voxels: 'dims' must be 3 integers greater than 0"));
    }
    if (static_cast<uint64_t>(count) > kMaxVoxelCells / cells) {  // cells * count would pass the cap, or overflow
      return unexpected("voxels: 'dims' asks for more than " + std::to_string(kMaxVoxelCells) + " cells");
    }
    cells *= static_cast<uint64_t>(count);
  }
  MemoryReservation charge(std::move(budget));
  if (Status reserved = charge.grow(
          sizeof(sdk::VoxelGrid) + frame_id.size() + sizeof(sdk::PointField) + std::string_view(kValueChannel).size() +
          cells * sizeof(float));
      !reserved) {
    return unexpected(reserved.error());
  }
  sdk::VoxelGrid header;
  header.frame_id = std::move(frame_id);
  header.origin.position = origin;
  header.cell_size = sdk::Vector3{cell_size, cell_size, cell_size};
  header.column_count = static_cast<uint32_t>(dims[0]);
  header.row_count = static_cast<uint32_t>(dims[1]);
  header.slice_count = static_cast<uint32_t>(dims[2]);
  header.cell_stride = sizeof(float);
  header.row_stride = header.cell_stride * header.column_count;
  header.slice_stride = header.row_stride * header.row_count;
  header.fields = {
      sdk::PointField{.name = kValueChannel, .offset = 0, .datatype = sdk::PointField::Datatype::kFloat32, .count = 1}};
  return VoxelGridBuilder(std::move(header), std::vector<float>(cells, 0.0f), std::move(charge));
}

Status VoxelGridBuilder::set(int64_t i, int64_t j, int64_t k, double value) {
  if (i < 0 || j < 0 || k < 0 || i >= header_.column_count || j >= header_.row_count || k >= header_.slice_count) {
    return unexpected(
        "voxels: the cell is outside the grid (" + std::to_string(header_.column_count) + " x " +
        std::to_string(header_.row_count) + " x " + std::to_string(header_.slice_count) + ")");
  }
  if (Status valid = checkFloat32("voxels: 'value'", value); !valid) {
    return valid;
  }
  const auto index =
      (static_cast<std::size_t>(k) * header_.row_count + static_cast<std::size_t>(j)) * header_.column_count +
      static_cast<std::size_t>(i);
  cells_[index] = static_cast<float>(value);
  return okStatus();
}

Charged<sdk::VoxelGrid> VoxelGridBuilder::finish() && {
  sdk::VoxelGrid grid = std::move(header_);
  auto cells = std::make_shared<const std::vector<float>>(std::move(cells_));
  grid.data = Span<const uint8_t>(reinterpret_cast<const uint8_t*>(cells->data()), cells->size() * sizeof(float));
  grid.anchor = std::move(cells);
  return {std::move(grid), std::move(charge_)};
}

}  // namespace PJ::scripting::native
