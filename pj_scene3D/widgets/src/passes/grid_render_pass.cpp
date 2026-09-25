// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/grid_render_pass.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"

namespace pj::scene3d {
namespace {

// One program serves both styles. The vertex stage forwards the checkerboard
// parity flag and the world-space XY (model is identity, so in_pos IS the world
// position) so the fragment stage can draw the grid lines *procedurally*.
constexpr std::string_view kGridVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in float in_parity;
uniform mat4 u_mvp;
uniform mat4 u_model;  // render-space placement (subtracts render_origin), for the shadow lookup
flat out float v_parity;
out vec2 v_world_xy;
out vec3 v_render_pos;
void main() {
  v_parity = in_parity;
  v_world_xy = in_pos.xy;  // WORLD xy: keeps the procedural grid lines fixed in world space
  // Render-space floor position so floorShadow projects with the render-space
  // u_light_vp (fitted to render-space caster bounds). Computed via u_model like the
  // mesh receiver's v_world_pos — same precision as this grid's own gl_Position.
  v_render_pos = (u_model * vec4(in_pos, 1.0)).xyz;
  gl_Position = u_mvp * vec4(in_pos, 1.0);
}
)";

// u_mode 0 = plain line grid (GL_LINES geometry, flat line color).
// u_mode 1 = checkerboard fill with the grid lines baked in: the cell tone comes
// from the parity flag; the lines are drawn analytically from the distance to the
// nearest cell edge, anti-aliased with screen-space derivatives (fwidth). Drawing
// the lines in the SAME surface as the fill — instead of as a second coplanar
// pass — is what eliminates the grazing-angle z-fighting (no separate primitive
// to win/lose the depth test in patches), and the fwidth term keeps them ~1px
// wide in screen space so they fade smoothly into the distance instead of
// breaking into dashes.
constexpr std::string_view kGridFragSrc = R"(#version 450 core
flat in float v_parity;
in vec2 v_world_xy;
in vec3 v_render_pos;
uniform int u_mode;
uniform vec3 u_color;       // line color
uniform vec3 u_color_a;     // checkerboard tone A
uniform vec3 u_color_b;     // checkerboard tone B
uniform float u_cell_m;     // cell size in metres
uniform float u_cell_phase; // the cells start at -extent/2: half a cell for an odd division count, else 0
// Shadow receive on the SOLID floor only (the User's goal: meshes cast onto the
// ground). The floor sits at world z=0 with normal +Z; the shadow lookup uses
// v_render_pos (render-space, render_origin already subtracted) so it matches the
// render-space u_light_vp. Same plain-sampler2D PCF as the mesh receiver;
// u_has_shadow gates it off (lit) when no map is bound this frame.
uniform bool u_has_shadow;
uniform mat4 u_light_vp;
uniform sampler2D u_shadow_map;
uniform float u_shadow_normal_offset;
uniform float u_shadow_softness;  // PCF penumbra radius in shadow-map texels
out vec4 frag_color;

float floorShadow(vec3 world_pos) {
  if (!u_has_shadow) {
    return 1.0;
  }
  // Bias scales with the PCF radius (u_shadow_softness), like the mesh receiver.
  vec3 biased = world_pos + vec3(0.0, 0.0, u_shadow_normal_offset * max(u_shadow_softness, 1.0));
  vec4 lc = u_light_vp * vec4(biased, 1.0);
  vec3 proj = (lc.xyz / lc.w) * 0.5 + 0.5;
  if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0) {
    return 1.0;
  }
  // 16-tap Poisson-disk PCF (matches the mesh receiver) for a smooth soft edge.
  const vec2 kPoisson[16] = vec2[](
      vec2(-0.942, -0.399), vec2(0.946, -0.769), vec2(-0.094, -0.929), vec2(0.345, 0.294),
      vec2(-0.916, 0.458), vec2(-0.815, -0.879), vec2(-0.383, 0.277), vec2(0.975, 0.756),
      vec2(0.443, -0.975), vec2(0.537, -0.474), vec2(-0.265, -0.419), vec2(0.792, 0.191),
      vec2(-0.242, 0.997), vec2(-0.814, 0.914), vec2(0.200, 0.786), vec2(0.144, -0.141));
  vec2 texel = (1.0 / vec2(textureSize(u_shadow_map, 0))) * max(u_shadow_softness, 1.0);
  float lit = 0.0;
  for (int i = 0; i < 16; ++i) {
    lit += proj.z <= texture(u_shadow_map, proj.xy + kPoisson[i] * texel).r ? 1.0 : 0.0;
  }
  return lit / 16.0;
}

void main() {
  if (u_mode == 0) {
    frag_color = vec4(u_color, 1.0);  // plain line grid: not a shadow receiver
    return;
  }
  vec3 tone = mix(u_color_a, u_color_b, v_parity);
  vec2 coord = (v_world_xy + u_cell_phase) / u_cell_m;
  vec2 grid = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
  float line = 1.0 - min(min(grid.x, grid.y), 1.0);
  vec3 base = mix(tone, u_color, line);
  // Darken the floor toward a floor value (not pure black) so the grid lines stay
  // legible inside the shadow.
  float shade = mix(0.45, 1.0, floorShadow(v_render_pos));
  frag_color = vec4(base * shade, 1.0);
}
)";

// Upload `verts` into (vao, vbo) and bind the (vec3 pos, float parity) layout.
// Requires a current GL context. Empty `verts` is fine (count 0, nothing drawn).
void uploadVertices(gl::VertexArray& vao, gl::Buffer& vbo, const std::vector<GridVertex>& verts) {
  vao.bind();
  vbo.uploadStatic(GL_ARRAY_BUFFER, verts.data(), static_cast<GLsizeiptr>(sizeof(GridVertex) * verts.size()));
  withGlFunctions([](auto& functions) {
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GridVertex)), nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 1, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GridVertex)),
        reinterpret_cast<const void*>(offsetof(GridVertex, parity)));
  });
  vao.unbind();
}

}  // namespace

void GridRenderPass::initializeGL() {
  initialized_ = false;
  auto result = gl::Program::fromSources(kGridVertSrc, kGridFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "GridRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }
  geometry_dirty_ = true;
  rebuildGeometry();
  initialized_ = true;
}

void GridRenderPass::rebuildGeometry() {
  if (!geometry_dirty_) {
    return;
  }
  // Each style needs exactly one buffer: kLines draws line primitives; kFilledCells
  // draws the cell surface and synthesizes its lines in the fragment shader.
  if (style_ == Style::kLines) {
    const std::vector<GridVertex> lines = buildGridLines(extent_m_, divisions_);
    line_vertex_count_ = static_cast<int>(lines.size());
    uploadVertices(line_vao_, line_vbo_, lines);
    cell_vertex_count_ = 0;
  } else {
    const std::vector<GridVertex> cells = buildCheckerboardCells(extent_m_, divisions_);
    cell_vertex_count_ = static_cast<int>(cells.size());
    uploadVertices(cell_vao_, cell_vbo_, cells);
    line_vertex_count_ = 0;
  }
  geometry_dirty_ = false;
}

void GridRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!initialized_ || program_ == nullptr) {
    return;
  }
  rebuildGeometry();  // lazy: extent/divisions/style changed since last frame

  // The grid lives at the WORLD origin (its vertices are world XY at z=0). Under
  // camera-relative rendering the view is built against frame_ctx.render_origin, so
  // place the grid with a model matrix that subtracts that origin — keeping it at
  // the true world origin (render_origin == {0,0,0} ⇒ identity, unchanged).
  glm::mat4 model{1.0f};
  model[3] = glm::vec4(glm::vec3(-frame_ctx.render_origin), 1.0f);
  const glm::mat4 mvp = view_params.proj * view_params.view * model;
  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setMat4("u_model", model);  // render-space floor position for the shadow lookup
  program_->setVec3("u_color", color_);

  if (style_ == Style::kFilledCells && cell_vertex_count_ > 0) {
    program_->setInt("u_mode", 1);
    program_->setVec3("u_color_a", cell_color_a_);
    program_->setVec3("u_color_b", cell_color_b_);
    const float cell = extent_m_ / static_cast<float>(divisions_);
    program_->setFloat("u_cell_m", cell);
    program_->setFloat("u_cell_phase", divisions_ % 2 == 1 ? 0.5F * cell : 0.0F);
    // Solid floor receives mesh shadows (User goal). The map + light matrix come from
    // ViewParams (set by the shadow pre-pass); shadow_map_id == 0 leaves u_has_shadow
    // 0 so the floor renders fully lit. Bind the depth map on unit 0 (the grid uses no
    // other texture).
    const bool shadows_on = view_params.shadow_map_id != 0U && view_params.shading.shadows_enabled;
    program_->setInt("u_has_shadow", shadows_on ? 1 : 0);
    if (shadows_on) {
      program_->setMat4("u_light_vp", view_params.light_view_proj);
      program_->setInt("u_shadow_map", 0);
      program_->setFloat(
          "u_shadow_normal_offset", view_params.shadow_world_units_per_texel * look::kShadowNormalOffsetTexels);
      program_->setFloat("u_shadow_softness", look::kShadowSoftnessTexels);
      withGlFunctions([&view_params](auto& functions) {
        functions.glActiveTexture(GL_TEXTURE0);
        functions.glBindTexture(GL_TEXTURE_2D, view_params.shadow_map_id);
      });
    }
    cell_vao_.bind();
    const int cell_count = cell_vertex_count_;
    withGlFunctions([cell_count](auto& functions) { functions.glDrawArrays(GL_TRIANGLES, 0, cell_count); });
    cell_vao_.unbind();
  } else if (style_ == Style::kLines && line_vertex_count_ > 0) {
    program_->setInt("u_mode", 0);
    line_vao_.bind();
    const int line_count = line_vertex_count_;
    withGlFunctions([line_count](auto& functions) { functions.glDrawArrays(GL_LINES, 0, line_count); });
    line_vao_.unbind();
  }
  unuseProgram();
}

void GridRenderPass::releaseGL() {
  // Forget the old context's GL objects and re-arm initializeGL(), which
  // rebuilds the program + re-uploads the static grid VBOs from scratch.
  program_.reset();
  line_vao_ = gl::VertexArray{};
  line_vbo_ = gl::Buffer{};
  cell_vao_ = gl::VertexArray{};
  cell_vbo_ = gl::Buffer{};
  initialized_ = false;
}

void GridRenderPass::setColor(const glm::vec3& color) {
  color_ = color;
}

void GridRenderPass::setCellColors(const glm::vec3& tone_a, const glm::vec3& tone_b) {
  cell_color_a_ = tone_a;
  cell_color_b_ = tone_b;
}

void GridRenderPass::setExtentMetres(float extent_m) {
  if (extent_m_ != extent_m) {
    extent_m_ = extent_m;
    geometry_dirty_ = true;
  }
}

void GridRenderPass::setDivisions(int divisions) {
  divisions = std::clamp(divisions, 1, 200);
  if (divisions_ != divisions) {
    divisions_ = divisions;
    geometry_dirty_ = true;
  }
}

void GridRenderPass::setStyle(Style style) {
  if (style_ != style) {
    style_ = style;
    geometry_dirty_ = true;
  }
}

}  // namespace pj::scene3d
