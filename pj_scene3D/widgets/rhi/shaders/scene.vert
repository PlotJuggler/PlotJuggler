// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vertex_color;

void main() {
    gl_Position = view_projection * vec4(position, 1.0);
    vertex_color = color;
}
