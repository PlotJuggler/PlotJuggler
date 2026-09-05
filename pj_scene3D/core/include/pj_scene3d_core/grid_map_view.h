// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/grid_map.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

// Upper bound on cell count the pack functions allocate for (64 MiB per R32F
// texture). The wire dims are validated against `data.size()` by validateGridMap,
// but a huge yet self-consistent grid would still exhaust the GPU texture path,
// so the layer refuses it with a status warning instead of uploading.
inline constexpr uint64_t kMaxRenderableGridMapCells = 16ull * 1024 * 1024;

// Total cell count (column * row). 0 if either dimension is 0.
[[nodiscard]] uint64_t gridMapCellCount(const PJ::sdk::GridMap& grid);

// Finite min/max of a float array; `valid` is false when no element is finite.
struct FiniteRange {
  float lo = 0.0f;
  float hi = 0.0f;
  bool valid = false;
};
[[nodiscard]] FiniteRange finiteRange(const std::vector<float>& values);

// De-interleave `field` into a dense row-major float array (index = r*column_count + c,
// the layout the render pass uploads as an R32F texture). Integer datatypes widen to
// float; a cell whose bytes fall outside `data` packs as NaN (a hole), the same
// "no data" sentinel the SDK defines for float channels. Empty on a degenerate grid,
// a sizeless datatype, or a cell count past kMaxRenderableGridMapCells. `range`, when
// given, receives finiteRange() of the result accumulated in the same pass.
[[nodiscard]] std::vector<float> packGridMapField(
    const PJ::sdk::GridMap& grid, const PJ::sdk::PointField& field, FiniteRange* range = nullptr);

// Fields the selectors offer: single-element (`count == 1`) channels with a sized
// datatype. Multi-element fields cannot drive a heightfield and are not listed.
[[nodiscard]] bool isScalarGridMapField(const PJ::sdk::PointField& field);

// The elevation channel to display when the user has not picked one: the field
// named `elevation` (SDK convention) if scalar, else the first scalar float field,
// else nullptr (a flat plane).
[[nodiscard]] const PJ::sdk::PointField* defaultElevationField(const std::vector<PJ::sdk::PointField>& fields);

// SOURCE-FRAME bounds: the 8 corners of the local box (x: 0..cols*cell_size.x,
// y: 0..rows*cell_size.y, z: elevation lo..hi) transformed by the origin pose.
// `elevation` is nullopt for the flat "None" mode (z = 0); an elevation channel
// with no finite sample renders nothing and yields an invalid box, as does an
// empty grid. Never applies the fixed-frame TF — the dock resolves that itself.
[[nodiscard]] AABB gridMapBounds(const PJ::sdk::GridMap& grid, std::optional<FiniteRange> elevation);

}  // namespace pj::scene3d
