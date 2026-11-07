// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform OccupancyUniforms {
    mat4 view_projection;
    mat4 fixed_from_grid;
    vec4 display_params;  // opacity, color scheme (0=map, 1=costmap), unused, unused
};

layout(location = 0) in vec2 grid_uv;
layout(location = 0) out vec2 cell_uv;

void main() {
    cell_uv = grid_uv;
    gl_Position = view_projection * fixed_from_grid * vec4(grid_uv, 0.0, 1.0);
}
