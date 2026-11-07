// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_limits.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/occupancy_grid_budget.h"
#include "pj_scene3d_core/pointcloud_budget.h"
#include "pj_scene3d_core/voxel_grid_budget.h"

namespace pj::scene3d {
namespace {

using Field = std::uint64_t SceneLimits::*;

constexpr std::array<Field, 48> kAllFields{
    &SceneLimits::max_points_per_cloud,
    &SceneLimits::max_point_wire_bytes_per_cloud,
    &SceneLimits::max_point_vertices_per_view,
    &SceneLimits::max_poses_per_layer,
    &SceneLimits::max_pose_wire_bytes_per_layer,
    &SceneLimits::max_pose_arms_per_view,
    &SceneLimits::max_occupancy_cells_per_layer,
    &SceneLimits::max_occupancy_wire_bytes,
    &SceneLimits::max_occupancy_update_window_bytes,
    &SceneLimits::occupancy_snapshot_budget_bytes,
    &SceneLimits::max_occupancy_texture_dimension,
    &SceneLimits::max_occupancy_cells_per_view,
    &SceneLimits::max_voxels_per_layer,
    &SceneLimits::max_voxel_wire_bytes,
    &SceneLimits::max_voxels_per_view,
    &SceneLimits::max_marker_wire_bytes,
    &SceneLimits::max_marker_instances_per_layer,
    &SceneLimits::max_marker_instances_per_view,
    &SceneLimits::max_marker_stream_vertices_per_layer,
    &SceneLimits::max_marker_stream_vertices_per_view,
    &SceneLimits::max_marker_frames_per_layer,
    &SceneLimits::max_marker_retained_bytes_per_layer,
    &SceneLimits::max_model_source_bytes,
    &SceneLimits::max_model_vertices_per_mesh,
    &SceneLimits::max_model_indices_per_mesh,
    &SceneLimits::max_model_submeshes_per_mesh,
    &SceneLimits::max_model_materials_per_mesh,
    &SceneLimits::max_model_nodes_per_mesh,
    &SceneLimits::max_model_node_depth,
    &SceneLimits::max_model_encoded_texture_bytes_per_mesh,
    &SceneLimits::max_model_texture_pixels_per_mesh,
    &SceneLimits::max_model_texture_pixels_per_layer,
    &SceneLimits::max_model_texture_dimension,
    &SceneLimits::max_model_state_bytes_per_layer,
    &SceneLimits::max_model_retained_bytes_per_mesh,
    &SceneLimits::max_model_retained_bytes_per_layer,
    &SceneLimits::max_model_draws_per_layer,
    &SceneLimits::max_model_draws_per_view,
    &SceneLimits::max_model_triangles_per_layer,
    &SceneLimits::max_model_triangles_per_view,
    &SceneLimits::max_model_snapshot_cache_bytes,
    &SceneLimits::max_urdf_links,
    &SceneLimits::max_urdf_joints,
    &SceneLimits::max_urdf_geometries,
    &SceneLimits::max_urdf_materials,
    &SceneLimits::max_urdf_string_bytes,
    &SceneLimits::max_hdr_bytes_per_view,
    &SceneLimits::max_hdr_bytes_per_rhi,
};
static_assert(sizeof(SceneLimits) == kAllFields.size() * sizeof(std::uint64_t), "list every SceneLimits field");

#ifndef PJ_TARGET_WASM
static_assert(kSceneLimits == kDesktopSceneLimits, "desktop builds use the desktop policy");
#endif

TEST(SceneLimits, DesktopIsNeverTighterThanBrowserAndNeverUnlimited) {
  for (const Field field : kAllFields) {
    EXPECT_GE(kDesktopSceneLimits.*field, kBrowserSceneLimits.*field);
    EXPECT_GT(kDesktopSceneLimits.*field, 0U);
    EXPECT_LT(kDesktopSceneLimits.*field, std::uint64_t{1} << 40U);
  }
}

// Both real policies are static_asserted in scene_limits.h; this checks the check.
TEST(SceneLimits, TypeBoundCheckRejectsOverflow) {
  SceneLimits overflowing = kDesktopSceneLimits;
  overflowing.max_point_vertices_per_view = std::uint64_t{1} << 31U;
  EXPECT_FALSE(sceneLimitsFitRendererTypes(overflowing));
}

TEST(SceneLimits, DesktopAdmitsLargeRoboticsData) {
  // Aggregated lidar map of tens of millions of points.
  EXPECT_LE(40'000'000U, kDesktopSceneLimits.max_points_per_cloud);
  EXPECT_GT(40'000'000U, kBrowserSceneLimits.max_points_per_cloud);
  std::uint64_t pixels = 0;
  EXPECT_TRUE(depthDimensionsFit(3840U, 2160U, pixels, kDesktopSceneLimits));
  EXPECT_TRUE(occupancyDimensionsFit(8192U, 8192U, kDesktopSceneLimits));
  EXPECT_FALSE(occupancyDimensionsFit(16385U, 1U, kDesktopSceneLimits));
  EXPECT_TRUE(voxelDimensionsFit(400U, 400U, 400U, kDesktopSceneLimits));
  EXPECT_TRUE(modelMeshCountsFit(10'000'000U, 30'000'000U, 1'000U, kDesktopSceneLimits));
}

TEST(SceneLimits, DesktopHdrFitsSeveral4kDocks) {
  constexpr std::uint64_t kHdrBytesPerPixel = 64U;
  constexpr std::uint64_t k4k = 3840ULL * 2160ULL * kHdrBytesPerPixel;
  EXPECT_LE(k4k, kDesktopSceneLimits.max_hdr_bytes_per_view);
  EXPECT_GT(k4k, kBrowserSceneLimits.max_hdr_bytes_per_view);
  EXPECT_GE(kDesktopSceneLimits.max_hdr_bytes_per_rhi / k4k, 4U);
}

TEST(SceneLimits, BudgetLatchHoldsUntilTheRejectedSizeFits) {
  BudgetLatch latch;
  const auto same = [] { return std::uint64_t{7}; };
  bool released = true;
  EXPECT_FALSE(latch.blocked(0U, same, released));
  EXPECT_FALSE(released);
  latch.reject(100U, 7U);
  EXPECT_TRUE(latch.blocked(99U, same, released));
  EXPECT_FALSE(released);
  EXPECT_FALSE(latch.blocked(100U, same, released));
  EXPECT_TRUE(released);
  EXPECT_FALSE(latch.blocked(0U, same, released));  // cleared by the release
  EXPECT_FALSE(released);
  latch.reject(5U, 7U);
  latch.reset();
  EXPECT_FALSE(latch.blocked(0U, same, released));
  EXPECT_FALSE(released);
  EXPECT_EQ(saturatedAdd(kSaturated, 1U), kSaturated);
  EXPECT_EQ(saturatedMultiply(kSaturated, 2U), kSaturated);
}

TEST(SceneLimits, BudgetLatchReleasesForAnotherCandidate) {
  BudgetLatch latch;
  std::uint64_t candidate = 7U;
  int evaluations = 0;
  const auto current = [&] {
    ++evaluations;
    return candidate;
  };
  bool released = false;
  EXPECT_FALSE(latch.blocked(0U, current, released));
  EXPECT_EQ(evaluations, 0) << "the candidate was computed with nothing latched";
  latch.reject(100U, 7U);
  EXPECT_TRUE(latch.blocked(10U, current, released));
  candidate = 8U;  // the tracker moved to another sample, or a sizing setting changed
  EXPECT_FALSE(latch.blocked(10U, current, released));
  EXPECT_TRUE(released);
  EXPECT_FALSE(latch.blocked(10U, current, released));
  EXPECT_FALSE(released);
}

}  // namespace
}  // namespace pj::scene3d
