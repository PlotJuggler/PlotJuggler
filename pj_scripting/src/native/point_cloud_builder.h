// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"

// Native PointCloud builder behind a script's `pj.cloud` (D14); `downsample` (point_cloud_ops.h) builds its output
// with it. Luau-free, so the Luau and Python binders share it, like scene_entities_ops.h.
namespace PJ::scripting::native {

/// The familiar PCL point types, by their field names and types (PCL is not a dependency). Every record is 16 bytes,
/// PCL's PointXYZ: float32 x/y/z at 0/4/8, then float32 `intensity` (xyzi) or a uint32 `rgb` (xyzrgb) at 12, whose
/// bytes are R, G, B, 255 by address, PJ4's canonical packed colour (PCL itself packs BGRA and pads XYZI/XYZRGB to 32
/// bytes, which would only double the memory here).
enum class CloudPreset : uint8_t { kXyz, kXyzi, kXyzrgb };

/// Bytes of one point record, whatever the preset.
inline constexpr uint32_t kCloudRecordBytes = 16;

/// The most points one cloud holds: its byte size (`row_step`) is a uint32, which is also the whole of a 32-bit size_t.
inline constexpr std::size_t kMaxCloudPoints = UINT32_MAX / kCloudRecordBytes;

/// The preset `name` names: "xyz", "xyzi" or "xyzrgb"; nullopt for any other text.
[[nodiscard]] std::optional<CloudPreset> parseCloudPreset(std::string_view name);

/// Accumulates the points of one cloud. Its buffer's capacity is what is charged to the evaluation's budget, before
/// the buffer grows (doubling, unless `reserve` sized it), so a builder grown in a loop stops at the limit. A growth
/// charges the new buffer beside the old one (up to three times the points' bytes), then only the new one. The output
/// is flat (height 1), dense, in `frame_id`, unstamped: the binder stamps it with the result's stamp.
class PointCloudBuilder {
 public:
  /// `budget` null: unbounded. Charges the header (frame, fields).
  [[nodiscard]] static Expected<PointCloudBuilder> create(
      CloudPreset preset, std::string frame_id, std::shared_ptr<MemoryBudget> budget);

  /// The values one point takes: x, y, z, then intensity (xyzi) or r, g, b (xyzrgb).
  [[nodiscard]] std::size_t arity() const noexcept;

  /// Charges and allocates room for `points` more points, so a binder that knows a list's length grows once.
  [[nodiscard]] Status reserve(std::size_t points);

  /// Whether a point of `count` values fits the preset; the error names the values it takes.
  [[nodiscard]] Status acceptsArity(std::size_t count) const;

  /// One point: exactly `arity()` values, every one finite and within float32's range; r, g, b from 0 to 255
  /// (truncated, as a scene colour is).
  [[nodiscard]] Status add(std::span<const double> values);

  /// The cloud, whose buffer and charge move into the result. Consumes the builder: a binder must not use it
  /// afterwards.
  [[nodiscard]] Charged<sdk::PointCloud> finish() &&;

 private:
  PointCloudBuilder(CloudPreset preset, sdk::PointCloud header, MemoryReservation charge)
      : preset_(preset), header_(std::move(header)), charge_(std::move(charge)) {}

  /// Charges, then allocates, a buffer capacity of at least `capacity` bytes.
  [[nodiscard]] Status growTo(std::size_t capacity);

  CloudPreset preset_;
  sdk::PointCloud header_;
  std::vector<uint8_t> bytes_;
  MemoryReservation charge_;
};

}  // namespace PJ::scripting::native
