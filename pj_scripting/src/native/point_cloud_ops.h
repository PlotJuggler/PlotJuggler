// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"

// Native operations over a canonical `sdk::PointCloud`, the heavy half of the object-script contract
// (docs/derived_objects/ARCHITECTURE.md §6): a script composes these instead of looping over points in the
// interpreter. Luau-free, so the Luau and Python binders share them.
//
// Layout contract: little-endian only; x/y/z are float32 or float64 scalar channels; an organized cloud
// (height > 1) honours `row_step`, and rows may not overlap; the declared extent must fit `data`. A record
// with a non-finite coordinate is skipped and keeps its index. Every output keeps the input's
// `timestamp_ns`, because the scene places an object at its own stamp.
namespace PJ::scripting::native {

/// The error of an allocating operation that stopped because its run was cancelled.
inline constexpr std::string_view kCancelledError = "cancelled";

/// What an allocating operation runs under. `budget` (null: unbounded) is charged for the output, its
/// bytes plus its header, before it is allocated, and stays charged until the output's buffer dies.
/// `cancelled` (empty: never) is the run's cancel check, the one `runMarkerScript` takes: polled every
/// 65536 records on the calling thread; once it returns true the operation fails with "cancelled".
struct NativeLimits {
  std::shared_ptr<MemoryBudget> budget;
  std::function<bool()> cancelled;
};

/// Records `cloud` declares: `width * height`, height 0 read as 1; 0 when `point_step` is 0. Not checked
/// against `data`: an operation fails on a cloud whose declared extent does not fit.
[[nodiscard]] uint64_t countPoints(const sdk::PointCloud& cloud) noexcept;

/// One scalar channel inside a record.
struct ChannelRef {
  uint32_t offset = 0;
  sdk::PointField::Datatype datatype = sdk::PointField::Datatype::kUnknown;
};

/// A validated cloud: where x/y/z live, and how many records `data` holds (`records * point_step` never
/// passes its size).
struct XyzAccess {
  ChannelRef x;
  ChannelRef y;
  ChannelRef z;
  std::size_t records = 0;
};

/// Validates the layout (endianness, extent, rows, x/y/z channels); the error names what is wrong.
[[nodiscard]] Expected<XyzAccess> resolveXyz(const sdk::PointCloud& cloud);

/// One record's coordinates; `index` is 0-based, counted over every record (row_step padding excluded).
struct Point {
  std::size_t index = 0;
  sdk::Vector3 position;
};

/// The first record at or after `index` with finite x/y/z, advancing `index` past it; nullopt once the
/// records run out, or once it has read `max_records` records (`index` past them). `access` must come from
/// `resolveXyz(cloud)`. Backs a binder's point iterator.
[[nodiscard]] std::optional<Point> nextFinitePoint(
    const sdk::PointCloud& cloud, const XyzAccess& access, std::size_t& index, std::size_t max_records = SIZE_MAX);

/// Axis-aligned bounds of the finite points. `min`/`max` mean something only when `count > 0`; the caller
/// decides what an empty cloud means (D25).
struct Bounds {
  sdk::Vector3 min;
  sdk::Vector3 max;
  std::size_t count = 0;
};
[[nodiscard]] Expected<Bounds> bounds(const sdk::PointCloud& cloud);

/// The finite point with the smallest (kMin) or largest (kMax) finite `channel` value; ties keep the first
/// index.
struct ExtremePoint {
  Point point;
  double value = 0.0;
};
enum class Extreme : uint8_t { kMin, kMax };
/// nullopt when no finite point has a finite `channel` value.
[[nodiscard]] Expected<std::optional<ExtremePoint>> extremePoint(
    const sdk::PointCloud& cloud, std::string_view channel, Extreme which);

/// Records whose finite coordinates lie inside `[min, max]` on every axis. The output is compacted
/// (height 1, dense) and copies whole records, so every extra channel survives (D15); it keeps the input's
/// frame. It is charged exactly what it keeps, plus one bit per input record while it runs.
[[nodiscard]] Expected<sdk::PointCloud> cropBox(
    const sdk::PointCloud& cloud, const sdk::Vector3& min, const sdk::Vector3& max, const NativeLimits& limits);

enum class CompareOp : uint8_t { kLt, kLe, kGt, kGe, kEq, kNe };

/// Calls `visit` with the `std::less<>`-style functor for `op`, so a caller's per-element loop holds no switch.
template <class Visit>
decltype(auto) dispatchCompare(CompareOp op, Visit&& visit) {
  switch (op) {
    case CompareOp::kLt:
      return visit(std::less<>{});
    case CompareOp::kLe:
      return visit(std::less_equal<>{});
    case CompareOp::kGt:
      return visit(std::greater<>{});
    case CompareOp::kGe:
      return visit(std::greater_equal<>{});
    case CompareOp::kEq:
      return visit(std::equal_to<>{});
    case CompareOp::kNe:
      break;
  }
  return visit(std::not_equal_to<>{});
}

/// Records whose finite `channel` value satisfies `value <op> threshold`; any numeric datatype. Compacted
/// like `cropBox`, but it keeps the input's `is_dense`: a kept record may still have a NaN coordinate.
[[nodiscard]] Expected<sdk::PointCloud> filterByField(
    const sdk::PointCloud& cloud, std::string_view channel, CompareOp op, double threshold, const NativeLimits& limits);

/// Re-expresses every record in `tf.parent_frame_id`, the output's frame (an empty parent keeps the input's
/// frame): `p' = rotation * p + translation` on x/y/z, with the rotation normalized; every other channel is
/// copied verbatim and a non-finite record is left untouched. A non-finite transform or a zero rotation is
/// an error. Never compacts: an organized cloud keeps its shape. `tf.child_frame_id` and `tf.timestamp` are
/// not read. The new frame name is charged with the output.
[[nodiscard]] Expected<sdk::PointCloud> transformXyz(
    const sdk::PointCloud& cloud, const sdk::FrameTransform& tf, const NativeLimits& limits);

/// One centroid per occupied cube of side `voxel_size` (finite, > 0): the mean of the finite points inside it, as an
/// xyz-preset cloud (point_cloud_builder.h) in the input's frame at its stamp, ordered by cube (z, then y, then x). The
/// cubes are counted from the finite points' minimum corner; a cloud spanning more than 2^21 cubes along an axis is an
/// error. Charged while it runs: 24 bytes per finite point; the output as its builder charges it.
[[nodiscard]] Expected<Charged<sdk::PointCloud>> downsample(
    const sdk::PointCloud& cloud, double voxel_size, const NativeLimits& limits);

}  // namespace PJ::scripting::native
