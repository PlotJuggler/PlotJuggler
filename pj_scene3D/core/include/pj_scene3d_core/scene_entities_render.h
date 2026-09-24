#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace pj::scene3d {

// Pure render structs: the decoded marker geometry a render pass consumes,
// produced from a canonical sdk::SceneEntities. No Qt, no GL, no wire shapes.
//
// Two-layer placement: every primitive carries a message-fixed LOCAL `model`
// matrix (its pose, in its entity's frame_id) plus a `frame_index` into the
// batch's interned `frames` table. The render pass composes the world matrix as
//   world = (fixed_frame <- frames[frame_index]) * model
// resolving TF once per unique frame per paint (O(frames), not O(primitives)).
//
// This is a STATELESS snapshot of one batch at the tracker time: no (ns,id)
// dedup, no deletions, no lifetime. Those are a future consumer-side layer.

// One placement of a unit mesh. Non-uniform scale baked into `model` turns the
// unit cube into a box and the unit sphere into an ellipsoid, so a single mesh
// serves every CUBE / SPHERE marker.
struct MarkerSolid {
  glm::mat4 model{1.0F};
  glm::vec4 color{1.0F};  // rgba, 0..1
  std::uint32_t frame_index = 0;
};

// Cylinder / cone / truncated-cone: a unit cylinder scaled by `model`, with the
// bottom and top face radii independently collapsed (0..1) toward the axis.
struct MarkerCylinder {
  glm::mat4 model{1.0F};
  glm::vec4 color{1.0F};
  float bottom_scale = 1.0F;
  float top_scale = 1.0F;
  std::uint32_t frame_index = 0;
};

// Arrow: tail at the model origin, pointing +x in local space (matches
// sdk::ArrowPrimitive's identity orientation). Dimensions in metres.
struct MarkerArrow {
  glm::mat4 model{1.0F};
  glm::vec4 color{1.0F};
  float shaft_length = 0.0F;
  float shaft_diameter = 0.0F;
  float head_length = 0.0F;
  float head_diameter = 0.0F;
  std::uint32_t frame_index = 0;
};

// Coordinate-axes glyph (X red, Y green, Z blue) of side `length`.
struct MarkerAxes {
  glm::mat4 model{1.0F};
  float length = 0.0F;
  float thickness = 0.0F;
  std::uint32_t frame_index = 0;
};

// A flat line batch, pre-expanded to GL_LINES vertex pairs: every two
// consecutive vertices form one segment. Vertices are local to `model`.
struct MarkerLineBatch {
  glm::mat4 model{1.0F};
  std::vector<glm::vec3> vertices;  // pairs: (0,1),(2,3),...
  std::vector<glm::vec4> colors;    // per-vertex; size == vertices.size(), or empty => solid `color`
  glm::vec4 color{1.0F};
  float thickness = 1.0F;
  std::uint32_t frame_index = 0;
};

// Triangle batch: vertices in triples, with CPU-computed flat normals.
struct MarkerTriangleBatch {
  glm::mat4 model{1.0F};
  std::vector<glm::vec3> vertices;  // triples: (0,1,2),(3,4,5),...
  std::vector<glm::vec3> normals;   // per-vertex (flat: equal within a triangle)
  std::vector<glm::vec4> colors;    // per-vertex, or empty => solid `color`
  glm::vec4 color{1.0F};
  std::uint32_t frame_index = 0;
};

// Text label anchored at the model origin.
struct MarkerText {
  glm::mat4 model{1.0F};
  glm::vec4 color{1.0F};
  std::string text;
  float font_size = 0.0F;  // world-space metres (always; the SDK's scale_invariant flag is not propagated here)
  bool billboard = false;  // true = text plane always faces the camera (screen-aligned)
  std::uint32_t frame_index = 0;
};

// The full decoded batch. `frames` is the interned frame_id table; every
// primitive's `frame_index` indexes it.
struct DecodedSceneEntities {
  std::vector<std::string> frames;
  std::vector<MarkerSolid> cubes;
  std::vector<MarkerSolid> spheres;
  std::vector<MarkerCylinder> cylinders;
  std::vector<MarkerArrow> arrows;
  std::vector<MarkerAxes> axes;
  std::vector<MarkerLineBatch> lines;
  std::vector<MarkerTriangleBatch> triangles;
  std::vector<MarkerText> texts;

  [[nodiscard]] bool empty() const {
    return cubes.empty() && spheres.empty() && cylinders.empty() && arrows.empty() && axes.empty() && lines.empty() &&
           triangles.empty() && texts.empty();
  }
};

// Viewer-side display overrides of one SceneEntities layer (a per-topic
// preference, NOT marker data: the marker protocol has no such fields). Both
// renderers apply them at draw time:
//  - opacity   : multiplies every primitive's alpha (transparency slider).
//  - color_*   : when set, replaces every primitive's rgb with override_color.
//  - wireframe : draws marker primitives as edges. View-only.
struct MarkerDisplayOverrides {
  float opacity = 1.0F;
  bool color_override = false;
  glm::vec4 override_color{1.0F};
  bool wireframe = false;
};

}  // namespace pj::scene3d
