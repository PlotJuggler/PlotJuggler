// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "line_clip.glslinc"

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

layout(location = 0) out vec4 vertex_color;

#ifdef PJ_LINE_SEGMENT
// grid_segment.vert.qsb, on a software rasterizer (RhiPassSetup::line_segments): one instance per segment (LineSegment,
// both ends); gl_VertexIndex (0 or 1) is the end this vertex draws.
layout(location = 0) in vec3 position_a;
layout(location = 1) in vec4 color_a;
layout(location = 2) in vec3 position_b;
layout(location = 3) in vec4 color_b;

void main() {
    bool end_b = gl_VertexIndex != 0;
    float moved;
    gl_Position = clipBehindEye(view_projection * vec4((end_b ? position_b : position_a) + origin_offset.xyz, 1.0),
                                view_projection * vec4((end_b ? position_a : position_b) + origin_offset.xyz, 1.0),
                                moved);
    vertex_color = mix(end_b ? color_b : color_a, end_b ? color_a : color_b, moved);
}
#else
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;

void main() {
    gl_Position = view_projection * vec4(position + origin_offset.xyz, 1.0);
    vertex_color = color;
}
#endif
