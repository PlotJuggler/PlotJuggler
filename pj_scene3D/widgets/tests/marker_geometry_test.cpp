// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// buildMarkerGeometry()'s stream batches (the QRhi marker pass's input): lines
// and triangles keep their points once, with an index per expanded vertex, so a
// strip is not stored as a line list. Each segment and triangle is still
// validated on its own (an out-of-range index or a non-finite point drops only
// it), and colours are per batch when uniform and per point otherwise.

#include "pj_scene3d_widgets/marker_geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "pj_scene3d_core/scene_limits.h"

namespace {

using pj::scene3d::buildMarkerGeometry;
using pj::scene3d::markerColor;
using pj::scene3d::MarkerStreamBatch;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

PJ::sdk::SceneEntities scene(PJ::sdk::SceneEntity entity) {
  entity.frame_id = "map";
  PJ::sdk::SceneEntities output;
  output.entities.push_back(std::move(entity));
  return output;
}

PJ::sdk::LinePrimitive line(PJ::sdk::LineType type, std::vector<PJ::sdk::Point3> points) {
  PJ::sdk::LinePrimitive output;
  output.type = type;
  output.points = std::move(points);
  output.color = {10, 20, 30, 255};
  return output;
}

// The expanded vertex positions of a batch, as the renderer reads them.
std::vector<glm::dvec3> expanded(const MarkerStreamBatch& batch) {
  std::vector<glm::dvec3> output;
  for (const std::uint32_t index : batch.indices) {
    output.push_back(batch.points.at(index));
  }
  return output;
}

TEST(MarkerGeometryTest, StripKeepsItsPointsOnceWithSegmentIndices) {
  PJ::sdk::SceneEntity entity;
  entity.lines.push_back(line(PJ::sdk::LineType::kLineStrip, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}));
  const auto result = buildMarkerGeometry(scene(entity));
  ASSERT_TRUE(result.error.isEmpty());
  ASSERT_EQ(result.geometry.lines.size(), 1U);
  const MarkerStreamBatch& batch = result.geometry.lines[0];
  EXPECT_EQ(batch.points.size(), 4U);
  EXPECT_EQ(batch.indices, (std::vector<std::uint32_t>{0, 1, 1, 2, 2, 3}));
  EXPECT_EQ(result.geometry.line_vertices, 6U);
  EXPECT_TRUE(batch.colors.empty());
  EXPECT_EQ(batch.colorOf(2), markerColor({10, 20, 30, 255}));
  ASSERT_TRUE(result.geometry.frame_bounds[0].valid);
  EXPECT_EQ(result.geometry.frame_bounds[0].max, glm::dvec3(1, 1, 0));
}

TEST(MarkerGeometryTest, EachSegmentIsValidatedOnItsOwn) {
  PJ::sdk::SceneEntity entity;
  // Loop: (0,1) and (1,2) touch the NaN point; only the closing (2,0) survives.
  entity.lines.push_back(line(PJ::sdk::LineType::kLineLoop, {{0, 0, 0}, {kNaN, 0, 0}, {2, 0, 0}}));
  // Indexed list: the second segment's index is out of range.
  PJ::sdk::LinePrimitive list = line(PJ::sdk::LineType::kLineList, {{0, 0, 1}, {0, 0, 2}});
  list.indices = {0, 1, 5, 0, 1, 0};
  entity.lines.push_back(list);
  const auto result = buildMarkerGeometry(scene(entity));
  ASSERT_TRUE(result.error.isEmpty());
  ASSERT_EQ(result.geometry.lines.size(), 2U);
  EXPECT_EQ(expanded(result.geometry.lines[0]), (std::vector<glm::dvec3>{{2, 0, 0}, {0, 0, 0}}));
  EXPECT_EQ(expanded(result.geometry.lines[1]), (std::vector<glm::dvec3>{{0, 0, 1}, {0, 0, 2}, {0, 0, 2}, {0, 0, 1}}));
  EXPECT_EQ(result.geometry.skipped_invalid, 3U);
  EXPECT_EQ(result.geometry.line_vertices, 6U);
}

TEST(MarkerGeometryTest, ColoursArePerBatchWhenUniformAndPerPointOtherwise) {
  PJ::sdk::SceneEntity entity;
  PJ::sdk::LinePrimitive uniform = line(PJ::sdk::LineType::kLineStrip, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}});
  uniform.colors.assign(3, {200, 100, 50, 128});
  PJ::sdk::LinePrimitive varied = uniform;
  varied.colors[1] = {1, 2, 3, 4};
  entity.lines = {uniform, varied};
  const auto result = buildMarkerGeometry(scene(entity));
  ASSERT_TRUE(result.error.isEmpty());
  ASSERT_EQ(result.geometry.lines.size(), 2U);
  const MarkerStreamBatch& same = result.geometry.lines[0];
  EXPECT_TRUE(same.colors.empty());
  EXPECT_EQ(same.colorOf(1), markerColor({200, 100, 50, 128}));
  const MarkerStreamBatch& mixed = result.geometry.lines[1];
  ASSERT_EQ(mixed.colors.size(), 3U);
  EXPECT_EQ(mixed.colorOf(0), markerColor({200, 100, 50, 128}));
  EXPECT_EQ(mixed.colorOf(1), markerColor({1, 2, 3, 4}));
}

TEST(MarkerGeometryTest, TrianglesIndexTheirValidTriangles) {
  PJ::sdk::SceneEntity entity;
  PJ::sdk::TrianglePrimitive triangles;
  triangles.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
  triangles.indices = {0, 1, 2, 3, 4, 5, 2, 1, 0, 0, 1, 9};  // collinear, reversed, out of range
  triangles.colors = {{1, 0, 0, 255}, {2, 0, 0, 255}, {3, 0, 0, 255}, {4, 0, 0, 255}, {5, 0, 0, 255}, {6, 0, 0, 255}};
  entity.triangles.push_back(triangles);
  const auto result = buildMarkerGeometry(scene(entity));
  ASSERT_TRUE(result.error.isEmpty());
  ASSERT_EQ(result.geometry.triangles.size(), 1U);
  const MarkerStreamBatch& batch = result.geometry.triangles[0];
  EXPECT_EQ(batch.indices, (std::vector<std::uint32_t>{0, 1, 2, 2, 1, 0}));
  EXPECT_EQ(result.geometry.triangle_vertices, 6U);
  EXPECT_EQ(result.geometry.skipped_invalid, 2U);
  EXPECT_EQ(batch.colorOf(2), markerColor({3, 0, 0, 255}));
}

// Frames are numbered in order of first appearance, and every primitive points
// at its entity's frame, with thousands of frames each repeated. The build time
// is printed for the interning cost (not asserted: sanitizer and debug lanes run
// this too).
TEST(MarkerGeometryTest, ThousandsOfFramesKeepFirstAppearanceOrder) {
  constexpr std::uint32_t kFrames =
      std::min<std::uint32_t>(4096U, pj::scene3d::kSceneLimits.max_marker_frames_per_layer);
  constexpr std::uint32_t kEntities = 5U * kFrames;
  PJ::sdk::SceneEntities batch;
  for (std::uint32_t index = 0; index < kEntities; ++index) {
    PJ::sdk::SceneEntity entity;
    entity.frame_id = "f" + std::to_string((index * 7U) % kFrames);  // every frame, first seen in a shuffled order
    PJ::sdk::CubePrimitive cube;
    cube.pose.orientation.w = 1.0;
    cube.size = {1.0, 1.0, 1.0};
    entity.cubes.push_back(cube);
    batch.entities.push_back(std::move(entity));
  }
  const auto start = std::chrono::steady_clock::now();
  const auto result = buildMarkerGeometry(batch);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::cout << "buildMarkerGeometry, " << kEntities << " entities in " << kFrames
            << " frames: " << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";

  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  const auto& geometry = result.geometry;
  ASSERT_EQ(geometry.frames.size(), kFrames);
  ASSERT_EQ(geometry.frame_bounds.size(), kFrames);
  for (std::uint32_t index = 0; index < kFrames; ++index) {
    ASSERT_EQ(geometry.frames[index], "f" + std::to_string((index * 7U) % kFrames)) << "frame " << index;
  }
  ASSERT_EQ(geometry.cubes.size(), kEntities);
  for (std::uint32_t index = 0; index < kEntities; ++index) {
    ASSERT_EQ(geometry.frames[geometry.cubes[index].frame_index], batch.entities[index].frame_id) << "cube " << index;
  }
}
}  // namespace
