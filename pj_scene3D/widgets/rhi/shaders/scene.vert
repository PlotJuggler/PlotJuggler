// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// PJ_LINE_PARTNER (scene_line.vert.qsb, the marker stream lines): each vertex also carries its segment's other end,
// for clipBehindEye.
#ifdef PJ_LINE_PARTNER
#extension GL_GOOGLE_include_directive : require
#include "line_clip.glslinc"
#endif

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
#ifdef PJ_LINE_PARTNER
layout(location = 2) in vec3 other_end;
#endif
layout(location = 0) out vec4 vertex_color;

void main() {
#ifdef PJ_LINE_PARTNER
    gl_Position = clipBehindEye(view_projection * vec4(position, 1.0), view_projection * vec4(other_end, 1.0));
#else
    gl_Position = view_projection * vec4(position, 1.0);
#endif
    vertex_color = color;
}
