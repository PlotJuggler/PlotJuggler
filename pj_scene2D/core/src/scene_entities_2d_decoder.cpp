// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <any>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_scene2d_core/scene_decoder.h"

namespace PJ {

namespace {

// Translation-only orthographic XY projection. Rotation and Z are intentionally
// dropped to keep SceneEntities usable as lightweight pixel-space overlays.
[[nodiscard]] Point2 project(const sdk::Point3& point, const sdk::Pose& pose) noexcept {
  return Point2{pose.position.x + point.x, pose.position.y + point.y};
}

[[nodiscard]] Point2 projectPosition(const sdk::Pose& pose) noexcept {
  return Point2{pose.position.x, pose.position.y};
}

[[nodiscard]] AnnotationTopology topologyFor(sdk::LineType type) noexcept {
  switch (type) {
    case sdk::LineType::kLineStrip:
      return AnnotationTopology::kLineStrip;
    case sdk::LineType::kLineLoop:
      return AnnotationTopology::kLineLoop;
    case sdk::LineType::kLineList:
      return AnnotationTopology::kLineList;
  }
  return AnnotationTopology::kLineStrip;
}

[[nodiscard]] double positiveOrDefault(double value, double fallback) noexcept {
  return value > 0.0 ? value : fallback;
}

[[nodiscard]] std::vector<Point2> projectedLinePoints(const sdk::LinePrimitive& line) {
  std::vector<Point2> out;
  if (!line.indices.empty()) {
    out.reserve(line.indices.size());
    for (uint32_t index : line.indices) {
      if (index < line.points.size()) {
        out.push_back(project(line.points[index], line.pose));
      }
    }
    return out;
  }

  out.reserve(line.points.size());
  for (const auto& point : line.points) {
    out.push_back(project(point, line.pose));
  }
  return out;
}

[[nodiscard]] std::vector<ColorRGBA> projectedLineColors(const sdk::LinePrimitive& line) {
  if (line.colors.empty()) {
    return {};
  }
  if (!line.indices.empty()) {
    std::vector<ColorRGBA> out;
    out.reserve(line.indices.size());
    for (uint32_t index : line.indices) {
      if (index < line.colors.size()) {
        out.push_back(line.colors[index]);
      }
    }
    return out;
  }
  return line.colors;
}

void appendLine(const sdk::LinePrimitive& line, ImageAnnotation& annotation) {
  auto points = projectedLinePoints(line);
  if (points.empty()) {
    return;
  }

  PointsAnnotation out;
  out.topology = topologyFor(line.type);
  out.points = std::move(points);
  out.thickness = positiveOrDefault(line.thickness, 2.0);
  out.color = line.color;
  out.colors = projectedLineColors(line);
  annotation.points.push_back(std::move(out));
}

void appendSphere(const sdk::SpherePrimitive& sphere, ImageAnnotation& annotation) {
  const double radius = std::max(std::abs(sphere.size.x), std::abs(sphere.size.y)) * 0.5;
  if (radius <= 0.0) {
    return;
  }
  CircleAnnotation out;
  out.center = projectPosition(sphere.pose);
  out.radius = radius;
  out.color = sphere.color;
  annotation.circles.push_back(out);
}

void appendCubeFootprint(const sdk::CubePrimitive& cube, ImageAnnotation& annotation) {
  const double half_x = std::abs(cube.size.x) * 0.5;
  const double half_y = std::abs(cube.size.y) * 0.5;
  if (half_x <= 0.0 || half_y <= 0.0) {
    return;
  }
  const auto center = projectPosition(cube.pose);
  PointsAnnotation out;
  out.topology = AnnotationTopology::kLineLoop;
  out.points = {
      {center.x - half_x, center.y - half_y},
      {center.x + half_x, center.y - half_y},
      {center.x + half_x, center.y + half_y},
      {center.x - half_x, center.y + half_y},
  };
  out.color = cube.color;
  annotation.points.push_back(std::move(out));
}

void appendTriangleEdges(const sdk::TrianglePrimitive& triangle, ImageAnnotation& annotation) {
  std::vector<uint32_t> indices;
  if (!triangle.indices.empty()) {
    indices = triangle.indices;
  } else {
    indices.resize(triangle.points.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = static_cast<uint32_t>(i);
    }
  }

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t a = indices[i + 0];
    const uint32_t b = indices[i + 1];
    const uint32_t c = indices[i + 2];
    if (a >= triangle.points.size() || b >= triangle.points.size() || c >= triangle.points.size()) {
      continue;
    }
    PointsAnnotation out;
    out.topology = AnnotationTopology::kLineLoop;
    out.points = {
        project(triangle.points[a], triangle.pose),
        project(triangle.points[b], triangle.pose),
        project(triangle.points[c], triangle.pose),
    };
    out.color = triangle.color;
    annotation.points.push_back(std::move(out));
  }
}

void appendArrow(const sdk::ArrowPrimitive& arrow, ImageAnnotation& annotation) {
  const double length = positiveOrDefault(arrow.shaft_length + arrow.head_length, arrow.shaft_length);
  if (length <= 0.0) {
    return;
  }
  const auto start = projectPosition(arrow.pose);
  PointsAnnotation out;
  out.topology = AnnotationTopology::kLineList;
  out.points = {start, {start.x + length, start.y}};
  out.thickness = positiveOrDefault(arrow.shaft_diameter, 2.0);
  out.color = arrow.color;
  annotation.points.push_back(std::move(out));
}

void appendAxes(const sdk::AxesPrimitive& axes, ImageAnnotation& annotation) {
  const double length = positiveOrDefault(axes.length, 0.0);
  if (length <= 0.0) {
    return;
  }
  const auto origin = projectPosition(axes.pose);
  PointsAnnotation out;
  out.topology = AnnotationTopology::kLineList;
  out.points = {
      origin,
      {origin.x + length, origin.y},
      origin,
      {origin.x, origin.y + length},
  };
  out.colors = {
      {255, 0, 0, 255},
      {255, 0, 0, 255},
      {0, 255, 0, 255},
      {0, 255, 0, 255},
  };
  out.thickness = positiveOrDefault(axes.thickness, 2.0);
  annotation.points.push_back(std::move(out));
}

void appendText(const sdk::TextPrimitive& text, ImageAnnotation& annotation) {
  if (text.text.empty()) {
    return;
  }
  TextAnnotation out;
  out.position = projectPosition(text.pose);
  out.font_size = positiveOrDefault(text.font_size, 14.0);
  out.color = text.color;
  out.text = text.text;
  annotation.texts.push_back(std::move(out));
}

void appendEntity(const sdk::SceneEntity& entity, ImageAnnotation& annotation) {
  for (const auto& arrow : entity.arrows) {
    appendArrow(arrow, annotation);
  }
  for (const auto& cube : entity.cubes) {
    appendCubeFootprint(cube, annotation);
  }
  for (const auto& sphere : entity.spheres) {
    appendSphere(sphere, annotation);
  }
  for (const auto& line : entity.lines) {
    appendLine(line, annotation);
  }
  for (const auto& triangle : entity.triangles) {
    appendTriangleEdges(triangle, annotation);
  }
  for (const auto& text : entity.texts) {
    appendText(text, annotation);
  }
  for (const auto& axes : entity.axes) {
    appendAxes(axes, annotation);
  }
  // TODO: Cylinders could reuse the sphere top-down circle projection.
  // Models are intentionally skipped: mesh assets need a loader before 2D projection.
}

// Project a canonical SceneEntities batch onto a 2D SceneFrame (the primitives'
// xy footprints as annotations). Object-based: shared by both decode routes.
[[nodiscard]] SceneFrame sceneEntitiesToFrame(const sdk::SceneEntities& entities) {
  SceneFrame frame;
  ImageAnnotation annotation;
  if (!entities.entities.empty()) {
    frame.timestamp = entities.entities.front().timestamp;
    annotation.timestamp = frame.timestamp;
  }

  for (const auto& entity : entities.entities) {
    appendEntity(entity, annotation);
  }

  if (!annotation.empty()) {
    frame.annotations.push_back(std::move(annotation));
  }
  return frame;
}

}  // namespace

Expected<SceneFrame> SceneEntities2DDecoder::decode(const uint8_t* data, size_t size) {
  auto entities = deserializeSceneEntities(data, size);
  if (!entities.has_value()) {
    return unexpected(std::move(entities).error());
  }
  return sceneEntitiesToFrame(*entities);
}

Expected<SceneFrame> SceneEntities2DDecoder::decode(const sdk::BuiltinObject& object) {
  const auto* entities = object.get<sdk::SceneEntities>();
  if (entities == nullptr) {
    return unexpected(std::string("SceneEntities2DDecoder: object is not SceneEntities"));
  }
  return sceneEntitiesToFrame(*entities);
}

}  // namespace PJ
