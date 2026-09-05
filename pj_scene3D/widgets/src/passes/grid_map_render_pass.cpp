// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/passes/grid_map_render_pass.h"

#include <QLoggingCategory>
#include <QString>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcGridMapPass, "pj.scene3d.grid_map_pass")

// Vertex shader: gl_VertexID -> (quad, triangle, corner). Each quad is two CCW
// triangles (seen from local +z): (0,0)(1,0)(1,1) and (0,0)(1,1)(0,1). Every vertex
// fetches all three corners of ITS triangle so the `flat` validity is identical for
// the whole primitive whichever vertex is provoking.
constexpr std::string_view kVertSrc = R"(#version 450 core
uniform mat4 u_model;             // grid-local -> render space (TF * origin pose)
uniform mat4 u_viewproj;          // proj * view
uniform mat3 u_normal_to_world;   // normal matrix of u_model
uniform mat3 u_world_to_view;     // mat3(view), for the camera-locked fill
uniform ivec2 u_dims;             // column_count, row_count
uniform vec2 u_cell_size;
uniform sampler2D u_elevation;    // R32F, NaN = hole
uniform sampler2D u_color;        // R32F
uniform bool u_has_elevation;     // false -> flat plane, up normal
uniform bool u_lighting;          // false -> skip the normal estimate entirely
uniform float u_color_lo;
uniform float u_color_hi;

flat out float v_valid;
out float v_t;
out vec3 v_normal_world;
out vec3 v_normal_view;

const ivec2 kCorner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));

bool finite(float v) { return !isnan(v) && !isinf(v); }

float elevationAt(ivec2 cell) {
  return u_has_elevation ? texelFetch(u_elevation, cell, 0).r : 0.0;
}

// Slope along one axis from the two neighbours: central difference when both are
// finite and inside the grid, one-sided when only one is, flat when neither.
float slope(float here, ivec2 cell, ivec2 step, int limit, float spacing) {
  bool has_prev = cell.x * step.x + cell.y * step.y > 0;
  bool has_next = cell.x * step.x + cell.y * step.y < limit - 1;
  float prev = has_prev ? elevationAt(cell - step) : 0.0;
  float next = has_next ? elevationAt(cell + step) : 0.0;
  has_prev = has_prev && finite(prev);
  has_next = has_next && finite(next);
  if (has_prev && has_next) return (next - prev) / (2.0 * spacing);
  if (has_next) return (next - here) / spacing;
  if (has_prev) return (here - prev) / spacing;
  return 0.0;
}

void main() {
  int quads_x = u_dims.x - 1;
  int quad = gl_VertexID / 6;
  int corner = gl_VertexID - quad * 6;
  ivec2 quad_cell = ivec2(quad % quads_x, quad / quads_x);
  int tri_base = (corner / 3) * 3;

  bool valid = true;
  for (int i = 0; i < 3; ++i) {
    valid = valid && finite(elevationAt(quad_cell + kCorner[tri_base + i]));
  }
  v_valid = valid ? 1.0 : 0.0;

  ivec2 cell = quad_cell + kCorner[corner];
  float h = elevationAt(cell);
  if (!valid) h = 0.0;

  vec3 normal_local = vec3(0.0, 0.0, 1.0);
  if (u_lighting && u_has_elevation && valid) {
    float dzdx = slope(h, cell, ivec2(1, 0), u_dims.x, u_cell_size.x);
    float dzdy = slope(h, cell, ivec2(0, 1), u_dims.y, u_cell_size.y);
    normal_local = normalize(vec3(-dzdx, -dzdy, 1.0));
  }
  v_normal_world = normalize(u_normal_to_world * normal_local);
  v_normal_view = normalize(u_world_to_view * v_normal_world);

  float value = texelFetch(u_color, cell, 0).r;
  float span = u_color_hi - u_color_lo;
  if (!finite(value)) {
    v_t = 0.0;  // a hole in the colour channel: the LUT low end
  } else if (!(span > 0.0)) {
    v_t = 0.5;  // degenerate range: the LUT midpoint
  } else {
    v_t = clamp((value - u_color_lo) / span, 0.0, 1.0);
  }

  vec3 local = vec3((vec2(cell) + 0.5) * u_cell_size, h);
  gl_Position = u_viewproj * (u_model * vec4(local, 1.0));
}
)";

constexpr std::string_view kFragHead = R"(#version 450 core
flat in float v_valid;
in float v_t;
in vec3 v_normal_world;
in vec3 v_normal_view;
out vec4 frag_color;

uniform int u_colormap_id;
uniform float u_opacity;
uniform bool u_lighting;
uniform vec3 u_key_dir;        // world-space direction TO the key light
uniform float u_ambient;
uniform float u_direct_scale;
uniform float u_fill_scale;
)";

// Lambert key + camera fill, two-sided so a map seen from below is still shaded
// (a heightfield has no inside). The scene FBO is linear: linearize the LUT color.
constexpr std::string_view kFragTail = R"(
void main() {
  if (v_valid < 0.5) discard;
  vec3 base = pow(max(sampleColormap(u_colormap_id, v_t), vec3(0.0)), vec3(2.2));
  float shade = 1.0;
  if (u_lighting) {
    float side = gl_FrontFacing ? 1.0 : -1.0;
    vec3 n_world = normalize(v_normal_world) * side;
    vec3 n_view = normalize(v_normal_view) * side;
    shade = u_ambient + u_direct_scale * max(dot(n_world, u_key_dir), 0.0) + u_fill_scale * max(n_view.z, 0.0);
  }
  frag_color = vec4(base * shade, u_opacity);
}
)";

std::string makeFragSrc() {
  return std::string(kFragHead) + std::string(PJ::colormapGlsl()) + std::string(kFragTail);
}

uint64_t cellCount(const GridMapUpload& grid) {
  return static_cast<uint64_t>(grid.column_count) * grid.row_count;
}

// 6 vertices (two triangles) per quad between adjacent cell centres.
uint64_t vertexCount(const GridMapUpload& grid) {
  return 6ull * (grid.column_count - 1) * (grid.row_count - 1);
}

}  // namespace

GridMapRenderPass::GridMapRenderPass() = default;
GridMapRenderPass::~GridMapRenderPass() = default;

void GridMapRenderPass::initializeGL() {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  static const std::string frag_src = makeFragSrc();
  auto result = gl::Program::fromSources(kVertSrc, frag_src);
  if (auto* program = std::get_if<gl::Program>(&result)) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    qCCritical(lcGridMapPass) << "GridMap shader build failed:" << QString::fromStdString(std::get<std::string>(result))
                              << "- grid maps will not render on this GL context.";
    return;  // render() no-ops (program_ stays null)
  }
  vao_.bind();
  vao_.unbind();
  withGlFunctions([this](auto& functions) { functions.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_); });
}

void GridMapRenderPass::setGrid(GridMapUpload upload) {
  grid_ = std::move(upload);
  const uint64_t count = cellCount(grid_);
  const bool have_elevation = grid_.elevation.size() == count;
  const bool have_color = grid_.color_is_elevation ? have_elevation : grid_.color.size() == count;
  const bool have_payload = have_color && (grid_.elevation.empty() || have_elevation);
  has_grid_ = grid_.column_count >= 2 && grid_.row_count >= 2 && have_payload;
  pending_full_ = has_grid_;
  rejection_.reset();
}

void GridMapRenderPass::clearGrid() {
  grid_ = {};  // the staged channels can be 2 x 64 MiB: release them, not just the flag
  has_grid_ = false;
  pending_full_ = false;
  rejection_.reset();
}

void GridMapRenderPass::releaseGL() {
  program_.reset();
  vao_ = gl::VertexArray{};
  elevation_tex_ = gl::Texture{};
  color_tex_ = gl::Texture{};
  elevation_dims_ = glm::uvec2(0);
  color_dims_ = glm::uvec2(0);
  initialized_ = false;
  rejection_.reset();
  pending_full_ = has_grid_;  // CPU payload survives; re-upload (or re-reject) under the new context
}

void GridMapRenderPass::uploadPending() {
  const uint64_t cells = cellCount(grid_);
  const uint64_t vertices = vertexCount(grid_);
  const auto max_dim = static_cast<uint32_t>(max_texture_size_);
  const bool over_texture = max_texture_size_ > 0 && (grid_.column_count > max_dim || grid_.row_count > max_dim);
  if (over_texture || vertices > static_cast<uint64_t>(std::numeric_limits<GLsizei>::max()) ||
      cells > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    rejection_ = QStringLiteral("Grid map %1 x %2 exceeds this GPU's texture or draw limits")
                     .arg(grid_.column_count)
                     .arg(grid_.row_count);
    qCWarning(lcGridMapPass) << *rejection_;
    pending_full_ = false;  // has_grid_ stays: the payload is retried on a recreated context
    return;
  }
  if (!grid_.elevation.empty()) {
    uploadChannel(elevation_tex_, elevation_dims_, grid_.elevation.data());
  }
  if (!grid_.color_is_elevation) {
    uploadChannel(color_tex_, color_dims_, grid_.color.data());
  }
  pending_full_ = false;
}

void GridMapRenderPass::uploadChannel(gl::Texture& texture, glm::uvec2& dims, const float* data) {
  const glm::uvec2 next(grid_.column_count, grid_.row_count);
  if (dims == next) {
    texture.overwrite(GL_RED, GL_FLOAT, next.x, next.y, data);
    return;
  }
  texture.upload(GL_R32F, GL_RED, GL_FLOAT, next.x, next.y, data);
  dims = next;
}

void GridMapRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !has_grid_) {
    return;
  }
  initializeGL();
  if (program_ == nullptr) {
    return;
  }
  if (pending_full_) {
    uploadPending();
  }
  if (rejection_.has_value()) {
    return;
  }
  const auto transform = frame_ctx.lookup(grid_.frame_id);
  if (!transform) {
    return;  // the map's frame can't resolve to the fixed frame: orphan, skip
  }
  const glm::mat4 model = glm::mat4(transform->matrix()) * poseToMat4(grid_.origin);
  const bool has_elevation = !grid_.elevation.empty();
  // An auto range with no finite sample collapses to lo == hi: the shader then
  // paints the LUT midpoint instead of dividing by zero.
  float color_lo = manual_lo_;
  float color_hi = manual_hi_;
  if (auto_range_) {
    color_lo = grid_.color_range.valid ? grid_.color_range.lo : 0.0f;
    color_hi = grid_.color_range.valid ? grid_.color_range.hi : 0.0f;
  }
  const bool opaque = opacity_ >= 0.999f;

  const MeshShadingParams& shading = view_params.shading;
  glm::vec3 key_dir = shading.key_light_dir;
  const float key_len = glm::length(key_dir);
  key_dir = key_len > 1e-6f ? key_dir / key_len : glm::vec3(0.0f, 0.0f, 1.0f);

  program_->use();
  program_->setMat4("u_model", model);
  program_->setMat4("u_viewproj", view_params.proj * view_params.view);
  program_->setMat3("u_normal_to_world", glm::transpose(glm::inverse(glm::mat3(model))));
  program_->setMat3("u_world_to_view", glm::mat3(view_params.view));
  program_->setVec2("u_cell_size", grid_.cell_size);
  program_->setInt("u_has_elevation", has_elevation ? 1 : 0);
  program_->setFloat("u_color_lo", color_lo);
  program_->setFloat("u_color_hi", color_hi);
  program_->setInt("u_colormap_id", static_cast<int>(colormap_));
  program_->setFloat("u_opacity", opacity_);
  program_->setInt("u_lighting", lighting_ ? 1 : 0);
  program_->setVec3("u_key_dir", key_dir);
  program_->setFloat("u_ambient", look::kGridMapAmbient * shading.ambient_scale);
  program_->setFloat("u_direct_scale", look::kGridMapDirect * shading.direct_scale);
  program_->setFloat("u_fill_scale", look::kGridMapFill * shading.fill_light_scale);
  program_->setInt("u_elevation", 0);
  program_->setInt("u_color", 1);
  if (has_elevation) {
    elevation_tex_.bind(0);
  }
  (grid_.color_is_elevation ? elevation_tex_ : color_tex_).bind(1);
  vao_.bind();

  const auto vertex_count = static_cast<GLsizei>(vertexCount(grid_));
  withGlFunctions([this, opaque, vertex_count](auto& functions) {
    functions.glUniform2i(  // no ivec2 setter on gl::Program
        program_->uniformLocation("u_dims"), static_cast<GLint>(grid_.column_count),
        static_cast<GLint>(grid_.row_count));
    // Opaque surfaces draw unblended so covered samples reset the scene FBO's
    // tonemap-coverage alpha; translucent ones use the scene's coverage-union
    // blend, which is also restored afterwards, and keep the depth TEST but skip
    // depth WRITES so what is drawn behind them still shows through (the
    // OccupancyGrid pass contract).
    auto set_coverage_blend = [&functions]() {
      functions.glEnable(GL_BLEND);
      functions.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    };
    if (opaque) {
      functions.glDisable(GL_BLEND);
    } else {
      set_coverage_blend();
      functions.glDepthMask(GL_FALSE);
    }
    functions.glDrawArrays(GL_TRIANGLES, 0, vertex_count);
    functions.glDepthMask(GL_TRUE);
    set_coverage_blend();
  });
  vao_.unbind();
  unuseProgram();
}

}  // namespace pj::scene3d
