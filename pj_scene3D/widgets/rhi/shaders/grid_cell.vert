// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// The filled-cell grid (GridRhiPass): cells built once at the fixed-frame
// origin, moved by the origin offset; grid_cell.frag draws the lines analytically.

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

// Declared identically in grid_cell.frag (one GL program sees both).
layout(std140, binding = 3) uniform GridUniforms {
    // xyz: fixed-frame origin - render origin. w: the cells start at -extent/2,
    // so their edges sit at multiples of the cell side once shifted by w (half a
    // cell for an odd division count, else zero).
    vec4 origin_offset;
    vec4 line_params;  // rgb: display-referred line colour, w: cell side in metres
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec3 render_position;
layout(location = 2) out vec2 grid_position;

void main() {
    render_position = position + origin_offset.xyz;
    grid_position = position.xy;  // fixed-frame XY: keeps the lines fixed in the world
    gl_Position = view_projection * vec4(render_position, 1.0);
    vertex_color = color;
}
