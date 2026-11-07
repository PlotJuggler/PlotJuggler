// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"

// Native VoxelGrid builder behind a script's `pj.voxels` (D16). Luau-free, so the Luau and Python binders share it.
namespace PJ::scripting::native {

/// The most cells one grid may have (64 MiB of float32), checked before anything is charged or allocated, whatever the
/// budget: dimensions a script computes could otherwise overflow the cell count or ask for terabytes.
inline constexpr uint64_t kMaxVoxelCells = uint64_t{1} << 24;

/// A dense grid of one float32 channel, `value`, every cell 0 until set; cells laid out x fastest, then y, then z, the
/// SDK's order. The output is unstamped: the binder stamps it with the result's stamp.
class VoxelGridBuilder {
 public:
  /// `origin` is the grid's (0, 0, 0) corner in `frame_id`, identity orientation; `cell_size` (finite, > 0) the side of
  /// a cubic cell; `dims` the cells along x, y and z, each > 0, at most `kMaxVoxelCells` together. The whole grid and
  /// its header are charged to `budget` (null: unbounded) before the cells are allocated.
  [[nodiscard]] static Expected<VoxelGridBuilder> create(
      std::string frame_id, sdk::Vector3 origin, double cell_size, std::array<int64_t, 3> dims,
      std::shared_ptr<MemoryBudget> budget);

  /// Sets cell (i, j, k), each 0-based and inside the grid, to `value`, which is finite and fits a float32.
  [[nodiscard]] Status set(int64_t i, int64_t j, int64_t k, double value);

  /// The grid, whose cells and charge move into the result. Consumes the builder: a binder must not use it afterwards.
  [[nodiscard]] Charged<sdk::VoxelGrid> finish() &&;

 private:
  VoxelGridBuilder(sdk::VoxelGrid header, std::vector<float> cells, MemoryReservation charge)
      : header_(std::move(header)), cells_(std::move(cells)), charge_(std::move(charge)) {}

  sdk::VoxelGrid header_;
  std::vector<float> cells_;
  MemoryReservation charge_;
};

}  // namespace PJ::scripting::native
