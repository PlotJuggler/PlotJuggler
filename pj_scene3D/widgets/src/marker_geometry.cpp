// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/marker_geometry.h"

#include <QObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>

#include "pj_scene3d_core/scene_entities_budget.h"

namespace pj::scene3d {
namespace {

bool finite(const glm::dmat4& matrix) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

bool finite(const glm::dvec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::optional<glm::dmat4> poseMatrix(const PJ::sdk::Pose& pose) {
  const glm::dvec3 translation{pose.position.x, pose.position.y, pose.position.z};
  const glm::dquat orientation{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z};
  const double norm = glm::length(orientation);
  if (!finite(translation) || !std::isfinite(norm) || norm < 1e-15) {
    return std::nullopt;
  }
  const glm::dmat4 matrix = glm::translate(glm::dmat4(1.0), translation) * glm::mat4_cast(orientation / norm);
  return finite(matrix) ? std::optional<glm::dmat4>{matrix} : std::nullopt;
}

glm::vec4 colorVector(const PJ::sdk::ColorRGBA& color) {
  return {
      color.r / 255.0F,
      color.g / 255.0F,
      color.b / 255.0F,
      color.a / 255.0F,
  };
}

glm::dvec3 pointVector(const PJ::sdk::Point3& point) {
  return {point.x, point.y, point.z};
}

glm::dvec3 sizeVector(const PJ::sdk::Vector3& size) {
  return {size.x, size.y, size.z};
}

void expandBounds(MarkerBounds& bounds, const glm::dvec3& point) {
  if (!finite(point)) {
    return;
  }
  if (!bounds.valid) {
    bounds.min = point;
    bounds.max = point;
    bounds.valid = true;
    return;
  }
  bounds.min = glm::min(bounds.min, point);
  bounds.max = glm::max(bounds.max, point);
}

void expandUnitBounds(
    MarkerBounds& bounds, const glm::dmat4& model, const glm::dvec3& minimum, const glm::dvec3& maximum) {
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? maximum.x : minimum.x,
        (corner & 2) != 0 ? maximum.y : minimum.y,
        (corner & 4) != 0 ? maximum.z : minimum.z,
    };
    expandBounds(bounds, glm::dvec3(model * glm::dvec4(point, 1.0)));
  }
}

struct Preflight {
  std::uint64_t instances = 0;
  std::uint64_t stream_vertices = 0;
  std::uint64_t frames = 0;
  std::uint64_t retained_bytes = 0;
  std::uint64_t cubes = 0;
  std::uint64_t spheres = 0;
  std::uint64_t cylinders = 0;
  std::uint64_t arrows = 0;
  std::uint64_t axes_arms = 0;
  std::uint64_t line_batches = 0;
  std::uint64_t triangle_batches = 0;
  std::uint64_t frame_string_bytes = 0;
  QString error;
};

std::uint64_t lineExpandedVertexCount(const PJ::sdk::LinePrimitive& line) {
  const std::uint64_t count = line.indices.empty() ? line.points.size() : line.indices.size();
  switch (line.type) {
    case PJ::sdk::LineType::kLineList:
      return count - count % 2U;
    case PJ::sdk::LineType::kLineStrip:
      return count >= 2U ? saturatedMultiply(count - 1U, 2U) : 0U;
    case PJ::sdk::LineType::kLineLoop:
      return count >= 2U ? saturatedMultiply(count, 2U) : 0U;
  }
  return 0U;
}

Preflight preflight(const PJ::sdk::SceneEntities& scene) {
  Preflight output;
  std::unordered_map<std::string, std::uint32_t> frames;
  for (const PJ::sdk::SceneEntity& entity : scene.entities) {
    const bool inserted = frames.try_emplace(entity.frame_id, static_cast<std::uint32_t>(frames.size())).second;
    if (inserted) {
      output.frame_string_bytes = saturatedAdd(output.frame_string_bytes, saturatedAdd(entity.frame_id.size(), 1U));
    }
    if (frames.size() > kSceneLimits.max_marker_frames_per_layer) {
      output.frames = frames.size();
      output.error = QObject::tr("SceneEntities exceeds the limit of %1 distinct frames")
                         .arg(kSceneLimits.max_marker_frames_per_layer);
      return output;
    }
    output.cubes = saturatedAdd(output.cubes, entity.cubes.size());
    output.spheres = saturatedAdd(output.spheres, entity.spheres.size());
    output.cylinders = saturatedAdd(output.cylinders, entity.cylinders.size());
    output.arrows = saturatedAdd(output.arrows, entity.arrows.size());
    output.axes_arms = saturatedAdd(output.axes_arms, saturatedMultiply(entity.axes.size(), 3U));

    for (const PJ::sdk::LinePrimitive& line : entity.lines) {
      const std::uint64_t vertices = lineExpandedVertexCount(line);
      output.stream_vertices = saturatedAdd(output.stream_vertices, vertices);
      output.line_batches = saturatedAdd(output.line_batches, vertices == 0U ? 0U : 1U);
    }
    for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
      const std::uint64_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
      const std::uint64_t triangle_count = count / 3U;
      // Reserve for the larger WebGL wireframe representation: six line
      // vertices for every three triangle vertices.
      output.stream_vertices = saturatedAdd(output.stream_vertices, saturatedMultiply(triangle_count, 6U));
      output.triangle_batches = saturatedAdd(output.triangle_batches, triangle_count == 0U ? 0U : 1U);
    }
  }
  output.instances = saturatedAdd(
      saturatedAdd(output.cubes, output.spheres),
      saturatedAdd(saturatedAdd(output.cylinders, output.arrows), output.axes_arms));
  output.frames = frames.size();
  output.retained_bytes = saturatedMultiply(output.instances, sizeof(MarkerInstanceSource));
  output.retained_bytes =
      saturatedAdd(output.retained_bytes, saturatedMultiply(output.stream_vertices, sizeof(MarkerStreamVertex)));
  output.retained_bytes =
      saturatedAdd(output.retained_bytes, saturatedMultiply(output.frames, sizeof(MarkerBounds) + sizeof(std::string)));
  output.retained_bytes = saturatedAdd(output.retained_bytes, output.frame_string_bytes);
  output.retained_bytes = saturatedAdd(
      output.retained_bytes,
      saturatedMultiply(saturatedAdd(output.line_batches, output.triangle_batches), sizeof(MarkerStreamBatch)));
  if (!markerLayerCountsFit(output.instances, output.stream_vertices, output.frames)) {
    output.error = QObject::tr("SceneEntities exceeds the layer limits (%1 instances, %2 expanded vertices, %3 frames)")
                       .arg(
                           QString::number(output.instances), QString::number(output.stream_vertices),
                           QString::number(output.frames));
  } else if (!markerRetainedBytesFit(output.retained_bytes)) {
    output.error = QObject::tr("SceneEntities would retain more than %1 MiB")
                       .arg(kSceneLimits.max_marker_retained_bytes_per_layer / (1024ULL * 1024ULL));
  }
  return output;
}

std::uint32_t internFrame(MarkerGeometry& geometry, const std::string& frame) {
  for (std::uint32_t index = 0; index < geometry.frames.size(); ++index) {
    if (geometry.frames[index] == frame) {
      return index;
    }
  }
  geometry.frames.push_back(frame);
  geometry.frame_bounds.emplace_back();
  return static_cast<std::uint32_t>(geometry.frames.size() - 1U);
}

template <typename Primitive>
void appendSolid(
    const Primitive& primitive, std::uint32_t frame_index, std::vector<MarkerInstanceSource>& destination,
    MarkerBounds& bounds, std::uint64_t& skipped_invalid) {
  const auto pose = poseMatrix(primitive.pose);
  const glm::dvec3 size = sizeVector(primitive.size);
  if (!pose.has_value() || !finite(size)) {
    ++skipped_invalid;
    return;
  }
  const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), size);
  if (!finite(model)) {
    ++skipped_invalid;
    return;
  }
  destination.push_back({model, colorVector(primitive.color), frame_index, 1.0F, 1.0F});
  expandUnitBounds(bounds, model, glm::dvec3(-0.5), glm::dvec3(0.5));
}

}  // namespace

MarkerGeometryResult buildMarkerGeometry(const PJ::sdk::SceneEntities& scene) {
  MarkerGeometryResult result;
  try {
    const Preflight counts = preflight(scene);
    if (!counts.error.isEmpty()) {
      result.error = counts.error;
      return result;
    }
    MarkerGeometry& geometry = result.geometry;
    geometry.frames.reserve(static_cast<std::size_t>(counts.frames));
    geometry.frame_bounds.reserve(static_cast<std::size_t>(counts.frames));
    geometry.cubes.reserve(static_cast<std::size_t>(counts.cubes));
    geometry.spheres.reserve(static_cast<std::size_t>(counts.spheres));
    geometry.cylinders.reserve(static_cast<std::size_t>(counts.cylinders));
    geometry.arrows.reserve(static_cast<std::size_t>(counts.arrows));
    geometry.axes.reserve(static_cast<std::size_t>(counts.axes_arms));
    geometry.lines.reserve(static_cast<std::size_t>(counts.line_batches));
    geometry.triangles.reserve(static_cast<std::size_t>(counts.triangle_batches));

    for (const PJ::sdk::SceneEntity& entity : scene.entities) {
      const std::uint32_t frame_index = internFrame(geometry, entity.frame_id);
      MarkerBounds& bounds = geometry.frame_bounds[frame_index];

      for (const PJ::sdk::CubePrimitive& cube : entity.cubes) {
        appendSolid(cube, frame_index, geometry.cubes, bounds, geometry.skipped_invalid);
      }
      for (const PJ::sdk::SpherePrimitive& sphere : entity.spheres) {
        appendSolid(sphere, frame_index, geometry.spheres, bounds, geometry.skipped_invalid);
      }
      for (const PJ::sdk::CylinderPrimitive& cylinder : entity.cylinders) {
        const auto pose = poseMatrix(cylinder.pose);
        const glm::dvec3 size = sizeVector(cylinder.size);
        if (!pose.has_value() || !finite(size) || !std::isfinite(cylinder.bottom_scale) ||
            !std::isfinite(cylinder.top_scale)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), size);
        const float bottom_scale = static_cast<float>(cylinder.bottom_scale);
        const float top_scale = static_cast<float>(cylinder.top_scale);
        const double radius = 0.5 * std::max(std::abs(cylinder.bottom_scale), std::abs(cylinder.top_scale));
        if (!finite(model) || !std::isfinite(bottom_scale) || !std::isfinite(top_scale) || !std::isfinite(radius)) {
          ++geometry.skipped_invalid;
          continue;
        }
        geometry.cylinders.push_back({model, colorVector(cylinder.color), frame_index, bottom_scale, top_scale});
        expandUnitBounds(bounds, model, {-radius, -radius, -0.5}, {radius, radius, 0.5});
      }
      for (const PJ::sdk::ArrowPrimitive& arrow : entity.arrows) {
        const auto pose = poseMatrix(arrow.pose);
        const double total = std::max(arrow.shaft_length + arrow.head_length, 1e-4);
        const double diameter = std::max({arrow.shaft_diameter, arrow.head_diameter, 1e-4});
        if (!pose.has_value() || !std::isfinite(total) || !std::isfinite(diameter)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 model = *pose * glm::scale(glm::dmat4(1.0), glm::dvec3(total, diameter, diameter));
        if (!finite(model)) {
          ++geometry.skipped_invalid;
          continue;
        }
        geometry.arrows.push_back({model, colorVector(arrow.color), frame_index, 1.0F, 1.0F});
        expandUnitBounds(bounds, model, {0.0, -0.5, -0.5}, {1.0, 0.5, 0.5});
      }
      for (const PJ::sdk::AxesPrimitive& axes : entity.axes) {
        const auto pose = poseMatrix(axes.pose);
        const double length = std::max(axes.length, 1e-3);
        const double thickness = std::max(axes.thickness, 1e-3);
        if (!pose.has_value() || !std::isfinite(length) || !std::isfinite(thickness)) {
          ++geometry.skipped_invalid;
          continue;
        }
        const glm::dmat4 scale = glm::scale(glm::dmat4(1.0), glm::dvec3(length, thickness, thickness));
        const std::array<std::pair<glm::dmat4, glm::vec4>, 3> arms{{
            {glm::dmat4(1.0), {1.0F, 0.0F, 0.0F, 1.0F}},
            {glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0.0, 0.0, 1.0)), {0.0F, 1.0F, 0.0F, 1.0F}},
            {glm::rotate(glm::dmat4(1.0), glm::radians(-90.0), glm::dvec3(0.0, 1.0, 0.0)), {0.0F, 0.0F, 1.0F, 1.0F}},
        }};
        for (const auto& [rotation, color] : arms) {
          const glm::dmat4 model = *pose * rotation * scale;
          if (!finite(model)) {
            ++geometry.skipped_invalid;
            continue;
          }
          geometry.axes.push_back({model, color, frame_index, 1.0F, 1.0F});
          expandUnitBounds(bounds, model, {0.0, -0.5, -0.5}, {1.0, 0.5, 0.5});
        }
      }

      for (const PJ::sdk::LinePrimitive& line : entity.lines) {
        const auto model = poseMatrix(line.pose);
        if (!model.has_value()) {
          ++geometry.skipped_invalid;
          continue;
        }
        MarkerStreamBatch batch{.model = *model, .vertices = {}, .frame_index = frame_index};
        batch.vertices.reserve(static_cast<std::size_t>(lineExpandedVertexCount(line)));
        const bool per_vertex_color = line.colors.size() == line.points.size();
        const std::size_t count = line.indices.empty() ? line.points.size() : line.indices.size();
        const auto resolve = [&line](std::size_t index) {
          return line.indices.empty() ? index : static_cast<std::size_t>(line.indices[index]);
        };
        const auto emit_segment = [&](std::size_t left, std::size_t right) {
          const std::size_t a = resolve(left);
          const std::size_t b = resolve(right);
          if (a >= line.points.size() || b >= line.points.size()) {
            ++geometry.skipped_invalid;
            return;
          }
          const glm::dvec3 pa = pointVector(line.points[a]);
          const glm::dvec3 pb = pointVector(line.points[b]);
          if (!finite(pa) || !finite(pb)) {
            ++geometry.skipped_invalid;
            return;
          }
          batch.vertices.push_back({pa, colorVector(per_vertex_color ? line.colors[a] : line.color)});
          batch.vertices.push_back({pb, colorVector(per_vertex_color ? line.colors[b] : line.color)});
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(pa, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(pb, 1.0)));
        };
        switch (line.type) {
          case PJ::sdk::LineType::kLineList:
            for (std::size_t index = 0; index + 1U < count; index += 2U) {
              emit_segment(index, index + 1U);
            }
            break;
          case PJ::sdk::LineType::kLineStrip:
            for (std::size_t index = 0; index + 1U < count; ++index) {
              emit_segment(index, index + 1U);
            }
            break;
          case PJ::sdk::LineType::kLineLoop:
            for (std::size_t index = 0; index + 1U < count; ++index) {
              emit_segment(index, index + 1U);
            }
            if (count >= 2U) {
              emit_segment(count - 1U, 0U);
            }
            break;
        }
        if (!batch.vertices.empty()) {
          geometry.line_vertices += batch.vertices.size();
          geometry.lines.push_back(std::move(batch));
        }
      }

      for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
        const auto model = poseMatrix(triangles.pose);
        if (!model.has_value()) {
          ++geometry.skipped_invalid;
          continue;
        }
        MarkerStreamBatch batch{.model = *model, .vertices = {}, .frame_index = frame_index};
        const bool per_vertex_color = triangles.colors.size() == triangles.points.size();
        const std::size_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
        const auto resolve = [&triangles](std::size_t index) {
          return triangles.indices.empty() ? index : static_cast<std::size_t>(triangles.indices[index]);
        };
        batch.vertices.reserve(count - count % 3U);
        for (std::size_t index = 0; index + 2U < count; index += 3U) {
          const std::size_t ia = resolve(index);
          const std::size_t ib = resolve(index + 1U);
          const std::size_t ic = resolve(index + 2U);
          if (ia >= triangles.points.size() || ib >= triangles.points.size() || ic >= triangles.points.size()) {
            ++geometry.skipped_invalid;
            continue;
          }
          const glm::dvec3 a = pointVector(triangles.points[ia]);
          const glm::dvec3 b = pointVector(triangles.points[ib]);
          const glm::dvec3 c = pointVector(triangles.points[ic]);
          const glm::dvec3 cross = glm::cross(b - a, c - a);
          if (!finite(a) || !finite(b) || !finite(c) || !finite(cross) || glm::length(cross) < 1e-15) {
            ++geometry.skipped_invalid;
            continue;
          }
          batch.vertices.push_back({a, colorVector(per_vertex_color ? triangles.colors[ia] : triangles.color)});
          batch.vertices.push_back({b, colorVector(per_vertex_color ? triangles.colors[ib] : triangles.color)});
          batch.vertices.push_back({c, colorVector(per_vertex_color ? triangles.colors[ic] : triangles.color)});
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(a, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(b, 1.0)));
          expandBounds(bounds, glm::dvec3(*model * glm::dvec4(c, 1.0)));
        }
        if (!batch.vertices.empty()) {
          geometry.triangle_vertices += batch.vertices.size();
          geometry.triangles.push_back(std::move(batch));
        }
      }

      geometry.skipped_texts += entity.texts.size();
      geometry.skipped_models += entity.models.size();
    }
  } catch (const std::bad_alloc&) {
    result.geometry = {};
    result.error = QObject::tr("Not enough memory to build these SceneEntities markers");
  } catch (const std::exception& error) {
    result.geometry = {};
    result.error = QObject::tr("SceneEntities conversion failed: %1").arg(QString::fromUtf8(error.what()));
  }
  return result;
}

}  // namespace pj::scene3d
