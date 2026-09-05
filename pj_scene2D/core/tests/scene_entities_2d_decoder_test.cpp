// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "pj_base/builtin/image_annotations.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_scene2d_core/scene_decoder.h"

namespace PJ {
namespace {

sdk::Pose poseAt(double x, double y, double z = 0.0) {
  sdk::Pose pose;
  pose.position = {.x = x, .y = y, .z = z};
  return pose;
}

Expected<SceneFrame> decodeSerialized(const sdk::SceneEntities& entities) {
  const auto bytes = serializeSceneEntities(entities);
  EXPECT_FALSE(bytes.empty());
  SceneEntities2DDecoder decoder;
  return decoder.decode(bytes.data(), bytes.size());
}

TEST(SceneEntities2DDecoderTest, FactoryCreatesDecoderForSchema) {
  auto decoder = makeSceneDecoder(kSchemaSceneEntities);
  EXPECT_NE(decoder, nullptr);
}

TEST(SceneEntities2DDecoderTest, ProjectsRenderablePrimitivesToImageAnnotations) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 42;
  entity.frame_id = "camera";
  entity.id = "overlay";

  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.pose = poseAt(10.0, 20.0);
  line.thickness = 3.0;
  line.points = {{0.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, {5.0, 5.0, 0.0}};
  line.color = {255, 0, 0, 255};
  entity.lines.push_back(std::move(line));

  sdk::TextPrimitive text;
  text.pose = poseAt(30.0, 40.0);
  text.font_size = 12.0;
  text.color = {0, 255, 0, 255};
  text.text = "label";
  entity.texts.push_back(std::move(text));

  sdk::SpherePrimitive sphere;
  sphere.pose = poseAt(50.0, 60.0);
  sphere.size = {.x = 8.0, .y = 6.0, .z = 2.0};
  sphere.color = {0, 0, 255, 255};
  entity.spheres.push_back(std::move(sphere));

  entities.entities.push_back(std::move(entity));
  const auto bytes = serializeSceneEntities(entities);
  ASSERT_FALSE(bytes.empty());

  SceneEntities2DDecoder decoder;
  auto frame = decoder.decode(bytes.data(), bytes.size());
  ASSERT_TRUE(frame.has_value()) << frame.error();

  EXPECT_EQ(frame->timestamp, 42);
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& annotation = frame->annotations.front();
  EXPECT_EQ(annotation.timestamp, 42);

  ASSERT_EQ(annotation.points.size(), 1u);
  const auto& projected_line = annotation.points.front();
  EXPECT_EQ(projected_line.topology, AnnotationTopology::kLineStrip);
  EXPECT_EQ(projected_line.thickness, 3.0);
  ASSERT_EQ(projected_line.points.size(), 3u);
  EXPECT_DOUBLE_EQ(projected_line.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(projected_line.points[0].y, 20.0);
  EXPECT_DOUBLE_EQ(projected_line.points[2].x, 15.0);
  EXPECT_DOUBLE_EQ(projected_line.points[2].y, 25.0);

  ASSERT_EQ(annotation.texts.size(), 1u);
  EXPECT_EQ(annotation.texts.front().text, "label");
  EXPECT_DOUBLE_EQ(annotation.texts.front().position.x, 30.0);
  EXPECT_DOUBLE_EQ(annotation.texts.front().position.y, 40.0);

  ASSERT_EQ(annotation.circles.size(), 1u);
  EXPECT_DOUBLE_EQ(annotation.circles.front().center.x, 50.0);
  EXPECT_DOUBLE_EQ(annotation.circles.front().center.y, 60.0);
  EXPECT_DOUBLE_EQ(annotation.circles.front().radius, 4.0);
}

TEST(SceneEntities2DDecoderTest, ProjectsCubeFootprintCorners) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 11;

  sdk::CubePrimitive cube;
  cube.pose = poseAt(10.0, 20.0);
  cube.size = {.x = 4.0, .y = 6.0, .z = 8.0};
  cube.color = {10, 20, 30, 255};
  entity.cubes.push_back(std::move(cube));
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& annotation = frame->annotations.front();
  ASSERT_EQ(annotation.points.size(), 1u);

  const auto& footprint = annotation.points.front();
  EXPECT_EQ(footprint.topology, AnnotationTopology::kLineLoop);
  EXPECT_EQ(footprint.color, (ColorRGBA{10, 20, 30, 255}));
  ASSERT_EQ(footprint.points.size(), 4u);
  EXPECT_DOUBLE_EQ(footprint.points[0].x, 8.0);
  EXPECT_DOUBLE_EQ(footprint.points[0].y, 17.0);
  EXPECT_DOUBLE_EQ(footprint.points[1].x, 12.0);
  EXPECT_DOUBLE_EQ(footprint.points[1].y, 17.0);
  EXPECT_DOUBLE_EQ(footprint.points[2].x, 12.0);
  EXPECT_DOUBLE_EQ(footprint.points[2].y, 23.0);
  EXPECT_DOUBLE_EQ(footprint.points[3].x, 8.0);
  EXPECT_DOUBLE_EQ(footprint.points[3].y, 23.0);
}

TEST(SceneEntities2DDecoderTest, ProjectsTriangleEdgesFromExplicitAndSynthesizedIndices) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 12;

  sdk::TrianglePrimitive explicit_indices;
  explicit_indices.pose = poseAt(10.0, 20.0);
  explicit_indices.color = {200, 10, 20, 255};
  explicit_indices.points = {
      {0.0, 0.0, 0.0},
      {2.0, 0.0, 0.0},
      {0.0, 2.0, 0.0},
      {4.0, 4.0, 0.0},
  };
  explicit_indices.indices = {0, 1, 2, 0, 99, 2, 2, 1, 3};
  entity.triangles.push_back(std::move(explicit_indices));

  sdk::TrianglePrimitive synthesized_indices;
  synthesized_indices.pose = poseAt(100.0, 200.0);
  synthesized_indices.color = {20, 200, 10, 255};
  synthesized_indices.points = {
      {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {2.0, 2.0, 0.0}, {3.0, 2.0, 0.0}, {2.0, 3.0, 0.0},
  };
  entity.triangles.push_back(std::move(synthesized_indices));
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& points = frame->annotations.front().points;
  ASSERT_EQ(points.size(), 4u);

  EXPECT_EQ(points[0].topology, AnnotationTopology::kLineLoop);
  ASSERT_EQ(points[0].points.size(), 3u);
  EXPECT_DOUBLE_EQ(points[0].points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(points[0].points[1].x, 12.0);
  EXPECT_DOUBLE_EQ(points[0].points[2].y, 22.0);

  EXPECT_EQ(points[1].topology, AnnotationTopology::kLineLoop);
  ASSERT_EQ(points[1].points.size(), 3u);
  EXPECT_DOUBLE_EQ(points[1].points[0].x, 10.0 + 0.0);
  EXPECT_DOUBLE_EQ(points[1].points[0].y, 20.0 + 2.0);
  EXPECT_DOUBLE_EQ(points[1].points[2].x, 10.0 + 4.0);
  EXPECT_DOUBLE_EQ(points[1].points[2].y, 20.0 + 4.0);

  EXPECT_EQ(points[2].topology, AnnotationTopology::kLineLoop);
  ASSERT_EQ(points[2].points.size(), 3u);
  EXPECT_DOUBLE_EQ(points[2].points[0].x, 100.0);
  EXPECT_DOUBLE_EQ(points[2].points[0].y, 200.0);
  EXPECT_DOUBLE_EQ(points[2].points[2].x, 100.0);
  EXPECT_DOUBLE_EQ(points[2].points[2].y, 201.0);

  EXPECT_EQ(points[3].topology, AnnotationTopology::kLineLoop);
  ASSERT_EQ(points[3].points.size(), 3u);
  EXPECT_DOUBLE_EQ(points[3].points[0].x, 102.0);
  EXPECT_DOUBLE_EQ(points[3].points[0].y, 202.0);
  EXPECT_DOUBLE_EQ(points[3].points[2].x, 102.0);
  EXPECT_DOUBLE_EQ(points[3].points[2].y, 203.0);
}

TEST(SceneEntities2DDecoderTest, ProjectsArrowAsLineSegment) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 13;

  sdk::ArrowPrimitive arrow;
  arrow.pose = poseAt(2.0, 3.0);
  arrow.shaft_length = 4.0;
  arrow.head_length = 1.0;
  arrow.shaft_diameter = 0.5;
  arrow.color = {1, 2, 3, 255};
  entity.arrows.push_back(std::move(arrow));
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& points = frame->annotations.front().points;
  ASSERT_EQ(points.size(), 1u);

  const auto& segment = points.front();
  EXPECT_EQ(segment.topology, AnnotationTopology::kLineList);
  EXPECT_DOUBLE_EQ(segment.thickness, 0.5);
  EXPECT_EQ(segment.color, (ColorRGBA{1, 2, 3, 255}));
  ASSERT_EQ(segment.points.size(), 2u);
  EXPECT_DOUBLE_EQ(segment.points[0].x, 2.0);
  EXPECT_DOUBLE_EQ(segment.points[0].y, 3.0);
  EXPECT_DOUBLE_EQ(segment.points[1].x, 7.0);
  EXPECT_DOUBLE_EQ(segment.points[1].y, 3.0);
}

TEST(SceneEntities2DDecoderTest, ProjectsAxesAsTwoFixedColorSegments) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 14;

  sdk::AxesPrimitive axes;
  axes.pose = poseAt(10.0, 20.0);
  axes.length = 5.0;
  axes.thickness = 1.5;
  entity.axes.push_back(std::move(axes));
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& points = frame->annotations.front().points;
  ASSERT_EQ(points.size(), 1u);

  const auto& segments = points.front();
  EXPECT_EQ(segments.topology, AnnotationTopology::kLineList);
  EXPECT_DOUBLE_EQ(segments.thickness, 1.5);
  ASSERT_EQ(segments.points.size(), 4u);
  EXPECT_DOUBLE_EQ(segments.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(segments.points[0].y, 20.0);
  EXPECT_DOUBLE_EQ(segments.points[1].x, 15.0);
  EXPECT_DOUBLE_EQ(segments.points[1].y, 20.0);
  EXPECT_DOUBLE_EQ(segments.points[2].x, 10.0);
  EXPECT_DOUBLE_EQ(segments.points[2].y, 20.0);
  EXPECT_DOUBLE_EQ(segments.points[3].x, 10.0);
  EXPECT_DOUBLE_EQ(segments.points[3].y, 25.0);
  ASSERT_EQ(segments.colors.size(), 4u);
  EXPECT_EQ(segments.colors[0], (ColorRGBA{255, 0, 0, 255}));
  EXPECT_EQ(segments.colors[1], (ColorRGBA{255, 0, 0, 255}));
  EXPECT_EQ(segments.colors[2], (ColorRGBA{0, 255, 0, 255}));
  EXPECT_EQ(segments.colors[3], (ColorRGBA{0, 255, 0, 255}));
}

TEST(SceneEntities2DDecoderTest, IndexedLineReindexesPointsAndColors) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 15;

  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.pose = poseAt(100.0, 200.0);
  line.thickness = 4.0;
  line.points = {
      {0.0, 0.0, 0.0},
      {10.0, 0.0, 0.0},
      {20.0, 5.0, 0.0},
  };
  line.colors = {
      {10, 0, 0, 255},
      {0, 20, 0, 255},
      {0, 0, 30, 255},
  };
  line.indices = {2, 0, 1};
  entity.lines.push_back(std::move(line));
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  ASSERT_EQ(frame->annotations.size(), 1u);
  const auto& points = frame->annotations.front().points;
  ASSERT_EQ(points.size(), 1u);

  const auto& indexed = points.front();
  EXPECT_EQ(indexed.topology, AnnotationTopology::kLineStrip);
  EXPECT_DOUBLE_EQ(indexed.thickness, 4.0);
  ASSERT_EQ(indexed.points.size(), 3u);
  EXPECT_DOUBLE_EQ(indexed.points[0].x, 120.0);
  EXPECT_DOUBLE_EQ(indexed.points[0].y, 205.0);
  EXPECT_DOUBLE_EQ(indexed.points[1].x, 100.0);
  EXPECT_DOUBLE_EQ(indexed.points[1].y, 200.0);
  EXPECT_DOUBLE_EQ(indexed.points[2].x, 110.0);
  EXPECT_DOUBLE_EQ(indexed.points[2].y, 200.0);
  ASSERT_EQ(indexed.colors.size(), 3u);
  EXPECT_EQ(indexed.colors[0], (ColorRGBA{0, 0, 30, 255}));
  EXPECT_EQ(indexed.colors[1], (ColorRGBA{10, 0, 0, 255}));
  EXPECT_EQ(indexed.colors[2], (ColorRGBA{0, 20, 0, 255}));
}

TEST(SceneEntities2DDecoderTest, EntityWithoutPrimitivesDecodesToEmptyFrame) {
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 16;
  entities.entities.push_back(std::move(entity));

  auto frame = decodeSerialized(entities);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  EXPECT_EQ(frame->timestamp, 16);
  EXPECT_TRUE(frame->annotations.empty());
  EXPECT_TRUE(frame->empty());
}

TEST(SceneEntities2DDecoderTest, ObjectRouteMatchesByteRoute) {
  // A-2: parser-backed topics decode the canonical object directly instead of
  // re-serializing it to bytes and re-parsing. Both routes must agree exactly.
  sdk::SceneEntities entities;
  sdk::SceneEntity entity;
  entity.timestamp = 7;
  sdk::LinePrimitive line;
  line.type = sdk::LineType::kLineStrip;
  line.pose = poseAt(1.0, 2.0);
  line.thickness = 2.0;
  line.points = {{0.0, 0.0, 0.0}, {4.0, 0.0, 0.0}};
  line.color = {10, 20, 30, 255};
  entity.lines.push_back(std::move(line));
  entities.entities.push_back(std::move(entity));

  SceneEntities2DDecoder decoder;
  const auto bytes = serializeSceneEntities(entities);
  auto from_bytes = decoder.decode(bytes.data(), bytes.size());
  ASSERT_TRUE(from_bytes.has_value()) << from_bytes.error();

  const sdk::BuiltinObject object = entities;  // BuiltinObject == std::any
  auto from_object = decoder.decode(object);
  ASSERT_TRUE(from_object.has_value()) << from_object.error();

  EXPECT_EQ(from_object->timestamp, from_bytes->timestamp);
  ASSERT_EQ(from_object->annotations.size(), 1u);
  ASSERT_EQ(from_bytes->annotations.size(), 1u);
  const auto& obj_pts = from_object->annotations.front().points;
  const auto& byte_pts = from_bytes->annotations.front().points;
  ASSERT_EQ(obj_pts.size(), byte_pts.size());
  ASSERT_EQ(obj_pts.size(), 1u);
  EXPECT_EQ(obj_pts.front().topology, byte_pts.front().topology);
  ASSERT_EQ(obj_pts.front().points.size(), byte_pts.front().points.size());
  EXPECT_DOUBLE_EQ(obj_pts.front().points[0].x, byte_pts.front().points[0].x);
  EXPECT_DOUBLE_EQ(obj_pts.front().points[1].x, byte_pts.front().points[1].x);
}

TEST(SceneEntities2DDecoderTest, ObjectRouteRejectsWrongType) {
  // The object route downcasts: a BuiltinObject of the wrong kind must error,
  // not silently drop (this is what surfaces a parser contract violation).
  SceneEntities2DDecoder decoder;
  const sdk::BuiltinObject not_entities = sdk::ImageAnnotations{};
  EXPECT_FALSE(decoder.decode(not_entities).has_value());
}

}  // namespace
}  // namespace PJ
