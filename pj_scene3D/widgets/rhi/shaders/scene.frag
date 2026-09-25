// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: linear HDR target, y: color is already in native target space
};

layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    vec3 color = render_mode.x != 0 && render_mode.y == 0
                     ? pow(max(vertex_color.rgb, vec3(0.0)), vec3(2.2))
                     : vertex_color.rgb;
    fragment_color = vec4(color, vertex_color.a);
}
