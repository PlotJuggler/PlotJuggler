// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"

namespace pj::scene3d {

// CPU marker geometry the QRhi renderer uploads for one SceneEntities batch.
// Every primitive stays in its entity's frame: `model` is frame-local and
// `frame_index` indexes `frames`, so the renderer applies TF once per frame.

struct MarkerInstanceSource {
  glm::dmat4 model{1.0};
  glm::vec4 color{1.0F};
  std::uint32_t frame_index = 0;
  float bottom_scale = 1.0F;
  float top_scale = 1.0F;
};

// A marker colour as the renderer's float RGBA: each channel / 255.
[[nodiscard]] inline glm::vec4 markerColor(const PJ::sdk::ColorRGBA& color) {
  return {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
}

// Line-list or triangle-list vertices sharing one frame-local pose, kept
// unexpanded: expanded vertex i is points[indices[i]] (two per segment, three
// per triangle, only those that passed validation), so a strip or an indexed
// mesh stores each point once.
struct MarkerStreamBatch {
  glm::dmat4 model{1.0};
  std::vector<glm::dvec3> points;
  std::vector<std::uint32_t> indices;
  // Per-point colours, parallel to `points`; empty when every vertex is `color`.
  std::vector<PJ::sdk::ColorRGBA> colors;
  glm::vec4 color{1.0F};
  std::uint32_t frame_index = 0;

  [[nodiscard]] std::size_t vertexCount() const {
    return indices.size();
  }
  [[nodiscard]] glm::vec4 colorOf(std::uint32_t point) const {
    return colors.empty() ? color : markerColor(colors[point]);
  }
};

struct MarkerBounds {
  glm::dvec3 min{0.0};
  glm::dvec3 max{0.0};
  bool valid = false;
};

struct MarkerGeometry {
  std::vector<std::string> frames;
  // Frame-local bounds, parallel to `frames`.
  std::vector<MarkerBounds> frame_bounds;
  std::vector<MarkerInstanceSource> cubes;
  std::vector<MarkerInstanceSource> spheres;
  std::vector<MarkerInstanceSource> cylinders;
  std::vector<MarkerInstanceSource> arrows;
  std::vector<MarkerInstanceSource> axes;
  std::vector<MarkerStreamBatch> lines;
  std::vector<MarkerStreamBatch> triangles;
  // Expanded vertices: the sums of the batches' vertexCount().
  std::uint64_t line_vertices = 0;
  std::uint64_t triangle_vertices = 0;
  std::uint64_t skipped_texts = 0;
  std::uint64_t skipped_models = 0;
  std::uint64_t skipped_invalid = 0;

  [[nodiscard]] std::uint64_t instanceCount() const {
    return cubes.size() + spheres.size() + cylinders.size() + arrows.size() + axes.size();
  }
  [[nodiscard]] std::uint64_t maximumStreamVertexCount() const {
    // Wireframe triangle edges expand each triangle's three vertices to six.
    return line_vertices + triangle_vertices * 2U;
  }
  [[nodiscard]] bool empty() const {
    return instanceCount() == 0U && line_vertices == 0U && triangle_vertices == 0U;
  }
};

struct MarkerGeometryResult {
  MarkerGeometry geometry;
  // Non-empty when the batch was rejected (SceneLimits) or expansion failed;
  // `geometry` is then empty.
  QString error;
};

// Expands a batch into renderer geometry. Counts are checked against
// kSceneLimits BEFORE any allocation, so an oversized batch fails closed with
// an error instead of exhausting memory. Pure and thread-safe: layers run it on
// a worker thread. Texts and models are counted as skipped (models render
// through ModelRenderable).
[[nodiscard]] MarkerGeometryResult buildMarkerGeometry(const PJ::sdk::SceneEntities& scene);

}  // namespace pj::scene3d
