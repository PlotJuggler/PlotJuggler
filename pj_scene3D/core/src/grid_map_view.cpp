// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/grid_map_view.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>

#include "pj_scene3d_core/pointcloud_convert.h"     // readScalarAt, findField
#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4

namespace pj::scene3d {

namespace {

void accumulate(FiniteRange& range, float value) {
  if (!std::isfinite(value)) {
    return;
  }
  if (!range.valid) {
    range.lo = value;
    range.hi = value;
    range.valid = true;
  } else {
    range.lo = std::min(range.lo, value);
    range.hi = std::max(range.hi, value);
  }
}

// One element widened to float: a memcpy load on a little-endian host (every PJ
// target), the byte-assembling readScalarAt otherwise.
template <typename T>
float readAs(const uint8_t* data, PJ::sdk::PointField::Datatype datatype) {
  if constexpr (std::endian::native == std::endian::little) {
    T value;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<float>(value);
  } else {
    return readScalarAt(data, datatype);
  }
}

}  // namespace

uint64_t gridMapCellCount(const PJ::sdk::GridMap& grid) {
  return static_cast<uint64_t>(grid.column_count) * grid.row_count;
}

FiniteRange finiteRange(const std::vector<float>& values) {
  FiniteRange range;
  for (const float value : values) {
    accumulate(range, value);
  }
  return range;
}

std::vector<float> packGridMapField(
    const PJ::sdk::GridMap& grid, const PJ::sdk::PointField& field, FiniteRange* range) {
  FiniteRange local_range;
  FiniteRange& acc = range != nullptr ? *range : local_range;
  acc = {};
  const uint64_t count = gridMapCellCount(grid);
  const uint32_t elem = PJ::sdk::bytesPerElement(field.datatype);
  if (count == 0 || elem == 0 || count > kMaxRenderableGridMapCells) {
    return {};
  }
  std::vector<float> out(count, std::numeric_limits<float>::quiet_NaN());
  const uint8_t* base = grid.data.data();
  const size_t size = grid.data.size();
  // One bounds check for the whole grid; only a short payload pays per cell.
  const uint64_t last_cell_end = static_cast<uint64_t>(grid.row_count - 1) * grid.row_stride +
                                 static_cast<uint64_t>(grid.column_count - 1) * grid.cell_stride + field.offset + elem;
  const bool whole_grid_in_bounds = base != nullptr && last_cell_end <= size;
  const auto fill = [&](auto read) {
    size_t index = 0;
    for (uint32_t r = 0; r < grid.row_count; ++r) {
      for (uint32_t c = 0; c < grid.column_count; ++c, ++index) {
        const uint64_t off =
            static_cast<uint64_t>(r) * grid.row_stride + static_cast<uint64_t>(c) * grid.cell_stride + field.offset;
        if (whole_grid_in_bounds || (base != nullptr && off + elem <= size)) {
          out[index] = read(base + off);
          accumulate(acc, out[index]);
        }
      }
    }
  };
  using DT = PJ::sdk::PointField::Datatype;
  const DT datatype = field.datatype;
  switch (datatype) {
    case DT::kInt8:
      fill([datatype](const uint8_t* p) { return readAs<int8_t>(p, datatype); });
      break;
    case DT::kUint8:
      fill([datatype](const uint8_t* p) { return readAs<uint8_t>(p, datatype); });
      break;
    case DT::kInt16:
      fill([datatype](const uint8_t* p) { return readAs<int16_t>(p, datatype); });
      break;
    case DT::kUint16:
      fill([datatype](const uint8_t* p) { return readAs<uint16_t>(p, datatype); });
      break;
    case DT::kInt32:
      fill([datatype](const uint8_t* p) { return readAs<int32_t>(p, datatype); });
      break;
    case DT::kUint32:
      fill([datatype](const uint8_t* p) { return readAs<uint32_t>(p, datatype); });
      break;
    case DT::kFloat32:
      fill([datatype](const uint8_t* p) { return readAs<float>(p, datatype); });
      break;
    case DT::kFloat64:
      fill([datatype](const uint8_t* p) { return readAs<double>(p, datatype); });
      break;
    default:
      fill([datatype](const uint8_t* p) { return readScalarAt(p, datatype); });
      break;
  }
  return out;
}

bool isScalarGridMapField(const PJ::sdk::PointField& field) {
  return field.count == 1 && PJ::sdk::bytesPerElement(field.datatype) > 0;
}

const PJ::sdk::PointField* defaultElevationField(const std::vector<PJ::sdk::PointField>& fields) {
  if (const auto* named = findField(fields, "elevation"); named != nullptr && isScalarGridMapField(*named)) {
    return named;
  }
  for (const auto& field : fields) {
    if (isScalarGridMapField(field) && (field.datatype == PJ::sdk::PointField::Datatype::kFloat32 ||
                                        field.datatype == PJ::sdk::PointField::Datatype::kFloat64)) {
      return &field;
    }
  }
  return nullptr;
}

AABB gridMapBounds(const PJ::sdk::GridMap& grid, std::optional<FiniteRange> elevation) {
  if (gridMapCellCount(grid) == 0 || (elevation.has_value() && !elevation->valid)) {
    return {};  // no cells, or a heightfield with no finite sample: nothing is drawn
  }
  const float z_lo = elevation.has_value() ? elevation->lo : 0.0f;
  const float z_hi = elevation.has_value() ? elevation->hi : 0.0f;
  const AABB local{
      .min = glm::vec3(0.0f, 0.0f, z_lo),
      .max = glm::vec3(
          static_cast<float>(grid.column_count) * static_cast<float>(grid.cell_size.x),
          static_cast<float>(grid.row_count) * static_cast<float>(grid.cell_size.y), z_hi),
      .valid = true};
  return transformedAABB(poseToMat4(grid.origin), local);
}

}  // namespace pj::scene3d
