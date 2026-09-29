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

#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_core/scene_entities_budget.h"

namespace pj::scene3d {
namespace {

using pj::scene3d::finite;  // the dmat4 overload, beside the dvec3 one below

bool finite(const glm::dvec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
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
  std::uint64_t stream_bytes = 0;  ///< the batches' points, per-point colours and indices
  std::uint64_t frame_string_bytes = 0;
  // Each distinct frame_id's index, numbered in order of first appearance, and
  // the frame_ids in that order.
  std::unordered_map<std::string, std::uint32_t> frame_index;
  std::vector<std::string> frame_ids;
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
  auto& frames = output.frame_index;
  for (const PJ::sdk::SceneEntity& entity : scene.entities) {
    const bool inserted = frames.try_emplace(entity.frame_id, static_cast<std::uint32_t>(frames.size())).second;
    if (inserted) {
      output.frame_ids.push_back(entity.frame_id);
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

    // An upper bound of a batch's retained arrays: every point, its colour when
    // the primitive has per-point colours, one index per expanded vertex.
    const auto batch_bytes = [](std::uint64_t points, bool per_point_colors, std::uint64_t vertices) {
      const std::uint64_t point_bytes =
          sizeof(glm::dvec3) + (per_point_colors ? sizeof(PJ::sdk::ColorRGBA) : std::uint64_t{0});
      return saturatedAdd(saturatedMultiply(points, point_bytes), saturatedMultiply(vertices, sizeof(std::uint32_t)));
    };
    for (const PJ::sdk::LinePrimitive& line : entity.lines) {
      const std::uint64_t vertices = lineExpandedVertexCount(line);
      output.stream_vertices = saturatedAdd(output.stream_vertices, vertices);
      output.line_batches = saturatedAdd(output.line_batches, vertices == 0U ? 0U : 1U);
      if (vertices != 0U) {
        output.stream_bytes = saturatedAdd(
            output.stream_bytes, batch_bytes(line.points.size(), line.colors.size() == line.points.size(), vertices));
      }
    }
    for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
      const std::uint64_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
      const std::uint64_t triangle_count = count / 3U;
      // Reserve for the larger WebGL wireframe representation: six line
      // vertices for every three triangle vertices.
      output.stream_vertices = saturatedAdd(output.stream_vertices, saturatedMultiply(triangle_count, 6U));
      output.triangle_batches = saturatedAdd(output.triangle_batches, triangle_count == 0U ? 0U : 1U);
      if (triangle_count != 0U) {
        output.stream_bytes = saturatedAdd(
            output.stream_bytes, batch_bytes(
                                     triangles.points.size(), triangles.colors.size() == triangles.points.size(),
                                     saturatedMultiply(triangle_count, 3U)));
      }
    }
  }
  output.instances = saturatedAdd(
      saturatedAdd(output.cubes, output.spheres),
      saturatedAdd(saturatedAdd(output.cylinders, output.arrows), output.axes_arms));
  output.frames = frames.size();
  output.retained_bytes = saturatedMultiply(output.instances, sizeof(MarkerInstanceSource));
  output.retained_bytes = saturatedAdd(output.retained_bytes, output.stream_bytes);
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
  } else if (output.retained_bytes > kSceneLimits.max_marker_retained_bytes_per_layer) {
    output.error = QObject::tr("SceneEntities would retain more than %1 MiB")
                       .arg(kSceneLimits.max_marker_retained_bytes_per_layer / (1024ULL * 1024ULL));
  }
  return output;
}

// A batch holding every point of a primitive (as doubles) and its colours:
// per-point only when the primitive has per-point colours that differ.
MarkerStreamBatch streamBatch(
    const glm::dmat4& model, std::uint32_t frame_index, const std::vector<PJ::sdk::Point3>& points,
    const std::vector<PJ::sdk::ColorRGBA>& colors, const PJ::sdk::ColorRGBA& color) {
  MarkerStreamBatch batch;
  batch.model = model;
  batch.frame_index = frame_index;
  batch.points.reserve(points.size());
  for (const PJ::sdk::Point3& point : points) {
    batch.points.push_back(pointVector(point));
  }
  const bool per_point = !points.empty() && colors.size() == points.size();
  const bool uniform = !per_point || std::all_of(colors.cbegin(), colors.cend(), [&colors](const auto& c) {
    return c == colors.front();
  });
  batch.color = markerColor(per_point ? colors.front() : color);
  if (!uniform) {
    batch.colors = colors;
  }
  return batch;
}

// Adds each point of `batch` to `bounds` the first time an accepted segment or
// triangle references it: a strip or an indexed mesh transforms every point once.
// Min/max keep the earlier of equal values, so this is the bounds of every
// referenced vertex in index order.
class StreamBounds {
 public:
  StreamBounds(MarkerBounds& bounds, const MarkerStreamBatch& batch)
      : bounds_(bounds), batch_(batch), seen_(batch.points.size(), false) {}
  void add(std::size_t point) {
    if (!seen_[point]) {
      seen_[point] = true;
      expandBounds(bounds_, glm::dvec3(batch_.model * glm::dvec4(batch_.points[point], 1.0)));
    }
  }

 private:
  MarkerBounds& bounds_;
  const MarkerStreamBatch& batch_;
  std::vector<bool> seen_;
};

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
  destination.push_back({model, markerColor(primitive.color), frame_index, 1.0F, 1.0F});
  expandUnitBounds(bounds, model, glm::dvec3(-0.5), glm::dvec3(0.5));
}

}  // namespace

MarkerGeometryResult buildMarkerGeometry(const PJ::sdk::SceneEntities& scene) {
  MarkerGeometryResult result;
  try {
    Preflight counts = preflight(scene);
    if (!counts.error.isEmpty()) {
      result.error = counts.error;
      return result;
    }
    MarkerGeometry& geometry = result.geometry;
    geometry.frames = std::move(counts.frame_ids);
    geometry.frame_bounds.resize(geometry.frames.size());
    geometry.cubes.reserve(static_cast<std::size_t>(counts.cubes));
    geometry.spheres.reserve(static_cast<std::size_t>(counts.spheres));
    geometry.cylinders.reserve(static_cast<std::size_t>(counts.cylinders));
    geometry.arrows.reserve(static_cast<std::size_t>(counts.arrows));
    geometry.axes.reserve(static_cast<std::size_t>(counts.axes_arms));
    geometry.lines.reserve(static_cast<std::size_t>(counts.line_batches));
    geometry.triangles.reserve(static_cast<std::size_t>(counts.triangle_batches));

    for (const PJ::sdk::SceneEntity& entity : scene.entities) {
      const std::uint32_t frame_index = counts.frame_index.find(entity.frame_id)->second;
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
        geometry.cylinders.push_back({model, markerColor(cylinder.color), frame_index, bottom_scale, top_scale});
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
        geometry.arrows.push_back({model, markerColor(arrow.color), frame_index, 1.0F, 1.0F});
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
        MarkerStreamBatch batch = streamBatch(*model, frame_index, line.points, line.colors, line.color);
        batch.indices.reserve(static_cast<std::size_t>(lineExpandedVertexCount(line)));
        StreamBounds batch_bounds(bounds, batch);
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
          const glm::dvec3& pa = batch.points[a];
          const glm::dvec3& pb = batch.points[b];
          if (!finite(pa) || !finite(pb)) {
            ++geometry.skipped_invalid;
            return;
          }
          batch.indices.insert(batch.indices.end(), {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)});
          batch_bounds.add(a);
          batch_bounds.add(b);
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
        if (!batch.indices.empty()) {
          geometry.line_vertices += batch.indices.size();
          geometry.lines.push_back(std::move(batch));
        }
      }

      for (const PJ::sdk::TrianglePrimitive& triangles : entity.triangles) {
        const auto model = poseMatrix(triangles.pose);
        if (!model.has_value()) {
          ++geometry.skipped_invalid;
          continue;
        }
        MarkerStreamBatch batch = streamBatch(*model, frame_index, triangles.points, triangles.colors, triangles.color);
        const std::size_t count = triangles.indices.empty() ? triangles.points.size() : triangles.indices.size();
        const auto resolve = [&triangles](std::size_t index) {
          return triangles.indices.empty() ? index : static_cast<std::size_t>(triangles.indices[index]);
        };
        batch.indices.reserve(count - count % 3U);
        StreamBounds batch_bounds(bounds, batch);
        for (std::size_t index = 0; index + 2U < count; index += 3U) {
          const std::size_t ia = resolve(index);
          const std::size_t ib = resolve(index + 1U);
          const std::size_t ic = resolve(index + 2U);
          if (ia >= triangles.points.size() || ib >= triangles.points.size() || ic >= triangles.points.size()) {
            ++geometry.skipped_invalid;
            continue;
          }
          const glm::dvec3& a = batch.points[ia];
          const glm::dvec3& b = batch.points[ib];
          const glm::dvec3& c = batch.points[ic];
          const glm::dvec3 cross = glm::cross(b - a, c - a);
          if (!finite(a) || !finite(b) || !finite(c) || !finite(cross) || glm::length(cross) < 1e-15) {
            ++geometry.skipped_invalid;
            continue;
          }
          batch.indices.insert(
              batch.indices.end(),
              {static_cast<std::uint32_t>(ia), static_cast<std::uint32_t>(ib), static_cast<std::uint32_t>(ic)});
          batch_bounds.add(ia);
          batch_bounds.add(ib);
          batch_bounds.add(ic);
        }
        if (!batch.indices.empty()) {
          geometry.triangle_vertices += batch.indices.size();
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
