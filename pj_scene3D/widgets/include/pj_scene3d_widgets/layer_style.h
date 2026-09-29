#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

namespace pj::scene3d {

// Style enums shared by the layers and the view's passes, so a layer header needs
// no pass to spell its settings.

// Per-point shape.
//   kSphere — world-radius sphere imposter; foreshortens with depth under a
//             perspective camera, fixed on-screen size under an orthographic one.
//   kPoint  — flat sprite of a fixed pixel size (no perspective).
//   kCube   — instanced fixed-frame-axis-aligned cube.
enum class PointShape : std::int32_t { kSphere, kPoint, kCube };

// Per-point colour source.
//   kField — per-point scalar -> colormap.
//   kSolid — one uniform colour, scalar ignored.
//   kRgb   — per-point packed RGBA used directly (white when the cloud has none).
enum class PointColorType : std::int32_t { kField, kSolid, kRgb };

// OccupancyGrid cell palette: ROS "map" (grey) or "costmap".
enum class OccupancyColorScheme : std::int32_t { kMap, kCostmap };

}  // namespace pj::scene3d
