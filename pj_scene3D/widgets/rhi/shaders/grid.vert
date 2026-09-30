// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// scene.vert for the scene prelude (GridRhiPass): the grid is built once at the
// fixed-frame origin and moved by the origin offset, so panning uploads nothing.

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

// Fixed-frame origin - render origin (GridRhiPass::originOffset()); zero for
// the TF connection lines, which are built relative to the render origin.
layout(std140, binding = 3) uniform OriginOffset {
    vec4 origin_offset;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vertex_color;

void main() {
    gl_Position = view_projection * vec4(position + origin_offset.xyz, 1.0);
    vertex_color = color;
}
