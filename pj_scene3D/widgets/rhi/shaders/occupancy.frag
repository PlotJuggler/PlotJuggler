// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform OccupancyUniforms {
    mat4 view_projection;
    mat4 fixed_from_grid;
    vec4 display_params;
};

layout(binding = 1) uniform sampler2D grid_texture;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec2 cell_uv;
layout(location = 0) out vec4 fragment_color;

void main() {
    float raw = floor((texture(grid_texture, cell_uv).r * 255.0) + 0.5);
    if (raw > 100.5) {
        discard;
    }
    float occupancy = clamp(raw / 100.0, 0.0, 1.0);
    vec3 color;
    if (display_params.y > 0.5) {
        color = mix(vec3(0.0, 0.4, 1.0), vec3(1.0, 0.0, 0.0), occupancy);
    } else {
        float grayscale = 1.0 - occupancy;
        color = vec3(grayscale);
    }
    if (render_mode.x != 0) {
        color = pow(max(color, vec3(0.0)), vec3(2.2));
    }
    fragment_color = vec4(color, display_params.x);
}
