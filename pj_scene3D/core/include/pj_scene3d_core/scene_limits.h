// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace pj::scene3d {

// Admission limits of the QRhi Scene3D renderer and its layers. One policy is
// selected at compile time (kSceneLimits); every *Fits / tryConsume* helper in
// the family *_budget.h headers defaults to it. The limits bound decoded CPU
// memory and GPU buffers so hostile or oversized samples fail with a warning
// instead of an allocation failure or an integer wrap. Per-view values bound a
// single SceneViewWidget; per-layer values bound one layer inside it.
struct SceneLimits {
  // Point clouds (DepthCloud shares these: one candidate point per pixel).
  std::uint64_t max_points_per_cloud;
  std::uint64_t max_point_wire_bytes_per_cloud;
  std::uint64_t max_point_vertices_per_view;
  // PosesInFrame: every pose expands to three 80-byte triad arms, so 300k arms are
  // about 24 MiB of CPU staging plus 24 MiB of QRhi instance buffer.
  std::uint64_t max_poses_per_layer;
  std::uint64_t max_pose_wire_bytes_per_layer;
  std::uint64_t max_pose_arms_per_view;
  // OccupancyGrid: one R8 texel per cell.
  std::uint64_t max_occupancy_cells_per_layer;
  std::uint64_t max_occupancy_wire_bytes;
  std::uint64_t max_occupancy_update_window_bytes;
  std::uint64_t occupancy_snapshot_budget_bytes;
  std::uint64_t max_occupancy_texture_dimension;
  std::uint64_t max_occupancy_cells_per_view;
  // VoxelGrid: four bytes per voxel on CPU and GPU.
  std::uint64_t max_voxels_per_layer;
  std::uint64_t max_voxel_wire_bytes;
  std::uint64_t max_voxels_per_view;
  // SceneEntities markers.
  std::uint64_t max_marker_wire_bytes;
  std::uint64_t max_marker_instances_per_layer;
  std::uint64_t max_marker_instances_per_view;
  std::uint64_t max_marker_stream_vertices_per_layer;
  std::uint64_t max_marker_stream_vertices_per_view;
  std::uint64_t max_marker_frames_per_layer;
  std::uint64_t max_marker_retained_bytes_per_layer;
  // Meshes (URDF, SceneEntities model primitives).
  std::uint64_t max_model_source_bytes;
  std::uint64_t max_model_vertices_per_mesh;
  std::uint64_t max_model_indices_per_mesh;
  std::uint64_t max_model_submeshes_per_mesh;
  std::uint64_t max_model_materials_per_mesh;
  std::uint64_t max_model_nodes_per_mesh;
  std::uint64_t max_model_node_depth;
  std::uint64_t max_model_encoded_texture_bytes_per_mesh;
  std::uint64_t max_model_texture_pixels_per_mesh;
  std::uint64_t max_model_texture_pixels_per_layer;
  // Longest side a model texture decodes at; larger maps are scaled down (aspect
  // kept) instead of failing, so a robot's textures fit the pixel budgets.
  std::uint64_t max_model_texture_dimension;
  std::uint64_t max_model_state_bytes_per_layer;
  std::uint64_t max_model_retained_bytes_per_mesh;
  std::uint64_t max_model_retained_bytes_per_layer;
  std::uint64_t max_model_draws_per_layer;
  std::uint64_t max_model_draws_per_view;
  std::uint64_t max_model_triangles_per_layer;
  std::uint64_t max_model_triangles_per_view;
  std::uint64_t max_model_snapshot_cache_bytes;
  // URDF structural envelope (see UrdfParseLimits).
  std::uint64_t max_urdf_links;
  std::uint64_t max_urdf_joints;
  std::uint64_t max_urdf_geometries;
  std::uint64_t max_urdf_materials;
  std::uint64_t max_urdf_string_bytes;
  // Retained HDR/SSAO/EDL offscreen chain (64 B/px), per view and summed over
  // every view sharing one QRhi.
  std::uint64_t max_hdr_bytes_per_view;
  std::uint64_t max_hdr_bytes_per_rhi;

  friend constexpr bool operator==(const SceneLimits&, const SceneLimits&) = default;
};

inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

// Browser: one wasm32 heap (at most 4 GiB) and one WebGL2 context shared by
// every dock, so limits stay within a few hundred MiB in total.
inline constexpr SceneLimits kBrowserSceneLimits{
    .max_points_per_cloud = 1'000'000,
    .max_point_wire_bytes_per_cloud = 64 * kMiB,
    .max_point_vertices_per_view = 1'000'000,
    .max_poses_per_layer = 100'000,
    .max_pose_wire_bytes_per_layer = 32 * kMiB,
    .max_pose_arms_per_view = 300'000,
    .max_occupancy_cells_per_layer = 8 * kMiB,
    .max_occupancy_wire_bytes = 32 * kMiB,
    .max_occupancy_update_window_bytes = 32 * kMiB,
    .occupancy_snapshot_budget_bytes = 16 * kMiB,
    .max_occupancy_texture_dimension = 8192,
    .max_occupancy_cells_per_view = 16 * kMiB,
    .max_voxels_per_layer = 4 * kMiB,
    .max_voxel_wire_bytes = 32 * kMiB,
    .max_voxels_per_view = 8 * kMiB,
    .max_marker_wire_bytes = 32 * kMiB,
    .max_marker_instances_per_layer = 50'000,
    .max_marker_instances_per_view = 100'000,
    .max_marker_stream_vertices_per_layer = 500'000,
    .max_marker_stream_vertices_per_view = 1'000'000,
    .max_marker_frames_per_layer = 4'096,
    .max_marker_retained_bytes_per_layer = 64 * kMiB,
    .max_model_source_bytes = 32 * kMiB,
    .max_model_vertices_per_mesh = 1'000'000,
    .max_model_indices_per_mesh = 3'000'000,
    .max_model_submeshes_per_mesh = 4'096,
    .max_model_materials_per_mesh = 4'096,
    .max_model_nodes_per_mesh = 16'384,
    .max_model_node_depth = 256,
    .max_model_encoded_texture_bytes_per_mesh = 32 * kMiB,
    .max_model_texture_pixels_per_mesh = 16 * kMiB,
    .max_model_texture_pixels_per_layer = 32 * kMiB,
    .max_model_texture_dimension = 1024,
    .max_model_state_bytes_per_layer = 64 * kMiB,
    .max_model_retained_bytes_per_mesh = 128 * kMiB,
    .max_model_retained_bytes_per_layer = 256 * kMiB,
    .max_model_draws_per_layer = 4'096,
    .max_model_draws_per_view = 8'192,
    .max_model_triangles_per_layer = 2'000'000,
    .max_model_triangles_per_view = 4'000'000,
    .max_model_snapshot_cache_bytes = 64 * kMiB,
    .max_urdf_links = 4'096,
    .max_urdf_joints = 8'192,
    .max_urdf_geometries = 16'384,
    .max_urdf_materials = 4'096,
    .max_urdf_string_bytes = 8'192,
    .max_hdr_bytes_per_view = 256 * kMiB,
    .max_hdr_bytes_per_rhi = 512 * kMiB,
};

// Desktop: sized for real robotics data on a discrete GPU (aggregated lidar
// maps of tens of millions of points, 16k occupancy maps, multi-million
// triangle robots) but not unlimited: every per-view total stays below the
// renderer's quint32 buffer sizes and int statistics counters (static_asserts
// below and in scene_view_widget_rhi.cpp). Largest single buffers: 100M points x
// 16 B = 1.6 GB, 20M marker vertices x 28 B = 560 MB. HDR: 3840x2160 x 64 B =
// 506 MiB, so 1 GiB per view fits one 5K view and 4 GiB per QRhi eight 4K
// docks. Model node depth stays at 256: it bounds recursion, not memory.
inline constexpr SceneLimits kDesktopSceneLimits{
    .max_points_per_cloud = 50'000'000,
    .max_point_wire_bytes_per_cloud = 2048 * kMiB,
    .max_point_vertices_per_view = 100'000'000,
    .max_poses_per_layer = 1'000'000,
    .max_pose_wire_bytes_per_layer = 256 * kMiB,
    .max_pose_arms_per_view = 6'000'000,
    .max_occupancy_cells_per_layer = 64 * kMiB,
    .max_occupancy_wire_bytes = 256 * kMiB,
    .max_occupancy_update_window_bytes = 256 * kMiB,
    .occupancy_snapshot_budget_bytes = 256 * kMiB,
    .max_occupancy_texture_dimension = 16384,
    .max_occupancy_cells_per_view = 128 * kMiB,
    .max_voxels_per_layer = 64 * kMiB,
    .max_voxel_wire_bytes = 512 * kMiB,
    .max_voxels_per_view = 128 * kMiB,
    .max_marker_wire_bytes = 512 * kMiB,
    .max_marker_instances_per_layer = 1'000'000,
    .max_marker_instances_per_view = 2'000'000,
    .max_marker_stream_vertices_per_layer = 10'000'000,
    .max_marker_stream_vertices_per_view = 20'000'000,
    .max_marker_frames_per_layer = 65'536,
    .max_marker_retained_bytes_per_layer = 1024 * kMiB,
    .max_model_source_bytes = 512 * kMiB,
    .max_model_vertices_per_mesh = 20'000'000,
    .max_model_indices_per_mesh = 60'000'000,
    .max_model_submeshes_per_mesh = 65'536,
    .max_model_materials_per_mesh = 65'536,
    .max_model_nodes_per_mesh = 262'144,
    .max_model_node_depth = 256,
    .max_model_encoded_texture_bytes_per_mesh = 512 * kMiB,
    .max_model_texture_pixels_per_mesh = 64 * kMiB,
    .max_model_texture_pixels_per_layer = 256 * kMiB,
    .max_model_texture_dimension = 16384,
    .max_model_state_bytes_per_layer = 1024 * kMiB,
    .max_model_retained_bytes_per_mesh = 2048 * kMiB,
    .max_model_retained_bytes_per_layer = 4096 * kMiB,
    .max_model_draws_per_layer = 65'536,
    .max_model_draws_per_view = 131'072,
    .max_model_triangles_per_layer = 50'000'000,
    .max_model_triangles_per_view = 100'000'000,
    .max_model_snapshot_cache_bytes = 1024 * kMiB,
    .max_urdf_links = 65'536,
    .max_urdf_joints = 131'072,
    .max_urdf_geometries = 262'144,
    .max_urdf_materials = 65'536,
    .max_urdf_string_bytes = 65'536,
    .max_hdr_bytes_per_view = 1024 * kMiB,
    .max_hdr_bytes_per_rhi = 4096 * kMiB,
};

#ifdef PJ_TARGET_WASM
inline constexpr SceneLimits kSceneLimits = kBrowserSceneLimits;
#else
inline constexpr SceneLimits kSceneLimits = kDesktopSceneLimits;
#endif

// Type bounds the renderer relies on, independent of vertex layouts: per-view
// totals feed int statistics counters, per-layer texel/index counts feed
// quint32 upload sizes and QSize ints. Layout-dependent byte bounds are checked
// in the SceneViewWidget constructor (scene_view_widget_rhi.cpp).
[[nodiscard]] constexpr bool sceneLimitsFitRendererTypes(const SceneLimits& limits) noexcept {
  constexpr auto kIntMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
  constexpr std::uint64_t kU32Max = std::numeric_limits<std::uint32_t>::max();
  return limits.max_point_vertices_per_view <= kIntMax && limits.max_pose_arms_per_view <= kIntMax &&
         limits.max_occupancy_cells_per_view <= kIntMax && limits.max_voxels_per_view <= kIntMax &&
         limits.max_marker_instances_per_view <= kIntMax && limits.max_marker_stream_vertices_per_view <= kIntMax &&
         limits.max_model_draws_per_view <= kIntMax && limits.max_model_triangles_per_view <= kIntMax &&
         limits.max_occupancy_texture_dimension <= kIntMax && limits.max_model_texture_dimension <= kIntMax &&
         limits.max_occupancy_cells_per_layer <= kU32Max && limits.max_voxels_per_layer <= kU32Max / 4U &&
         limits.max_model_vertices_per_mesh <= kU32Max && limits.max_model_indices_per_mesh <= kU32Max;
}
static_assert(sceneLimitsFitRendererTypes(kBrowserSceneLimits));
static_assert(sceneLimitsFitRendererTypes(kDesktopSceneLimits));

// The one per-view budget primitive behind every family's tryConsume* wrapper:
// subtract `requested` from `remaining` when it fits, refuse (leaving
// `remaining` untouched) when it does not. Never refund within a frame —
// callers check placement before consuming.
[[nodiscard]] constexpr bool tryConsumeBudget(std::uint64_t requested, std::uint64_t& remaining) noexcept {
  if (requested > remaining) {
    return false;
  }
  remaining -= requested;
  return true;
}

// Saturating byte/count arithmetic for budget estimates: an overflow pins at
// kSaturated, which every limit rejects.
inline constexpr std::uint64_t kSaturated = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] constexpr std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right) noexcept {
  return right > kSaturated - left ? kSaturated : left + right;
}

[[nodiscard]] constexpr std::uint64_t saturatedMultiply(std::uint64_t left, std::uint64_t right) noexcept {
  return left != 0U && right > kSaturated / left ? kSaturated : left * right;
}

// Per-layer memory of a per-view budget rejection. A layer that did not fit the
// view's remaining budget drops its data and records its size and its candidate:
// what it measured, as a key of its sample and of the settings that size it. The
// same candidate is retried only once that much fits again, instead of
// re-decoding every frame; any other candidate (a tracker move to another
// sample, a settings change) is examined on the next frame.
class BudgetLatch {
 public:
  // True while the rejected size still exceeds `remaining` and `candidate()`
  // (what the layer would draw now) is still the rejected one: the layer stays
  // dropped this frame. Otherwise the latch clears and returns false with
  // `released` set: the caller must re-decode what it dropped. `candidate` runs
  // only while a rejection is latched.
  template <typename CandidateFn>
  [[nodiscard]] constexpr bool blocked(std::uint64_t remaining, CandidateFn&& candidate, bool& released) {
    released = false;
    if (!rejected_.has_value()) {
      return false;
    }
    if (rejected_->count > remaining && rejected_->candidate == candidate()) {
      return true;
    }
    rejected_.reset();
    released = true;
    return false;
  }
  constexpr void reject(std::uint64_t count, std::uint64_t candidate) noexcept {
    rejected_ = Rejection{count, candidate};
  }
  constexpr void reset() noexcept {
    rejected_.reset();
  }

 private:
  struct Rejection {
    std::uint64_t count = 0;
    std::uint64_t candidate = 0;
  };
  std::optional<Rejection> rejected_;
};

}  // namespace pj::scene3d
