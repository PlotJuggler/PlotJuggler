// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "line_clip.glslinc"

// MeshMap: de-indexed triangles (three vertices per face) or link line segments.
// Positions are mesh-local; the colour source is a scalar channel through the
// colormap, an RGBA8 vertex colour, or one solid colour (meshmap.frag).
layout(std140, binding = 0) uniform MeshMapUniforms {
    mat4 view_projection;  // clip-corrected, camera-relative
    mat4 model;            // mesh-local -> camera-relative scene
    vec4 eye;              // xyz: camera eye, camera-relative
    vec4 view_z;           // xyz: world direction of view-space +z (towards the viewer)
    vec4 key_dir;          // xyz: world direction TO the key light
    vec4 color_params;     // x: range lo, y: range hi, z: colormap id, w: opacity
    vec4 light_params;     // x: ambient, y: direct, z: fill, w: lighting on
    vec4 flags;            // x: colour mode (0 channel, 1 vertex colour, 2 solid, 3 unlit line)
} mm;

layout(location = 0) out float channel_value;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec3 scene_position;

#ifdef PJ_LINE_SEGMENT
// meshmap_segment.vert.qsb, the links on a software rasterizer (RhiPassSetup::line_segments): one instance per link,
// both ends read from the link position and colour buffers; gl_VertexIndex (0 or 1) is the end this vertex draws.
layout(location = 0) in vec3 position_a;
layout(location = 1) in vec3 position_b;
layout(location = 2) in vec4 color_a;  // RGBA8
layout(location = 3) in vec4 color_b;

void main() {
    bool end_b = gl_VertexIndex != 0;
    channel_value = 0.0;
    vec4 scene = mm.model * vec4(end_b ? position_b : position_a, 1.0);
    scene_position = scene.xyz;
    float moved;
    gl_Position = clipBehindEye(mm.view_projection * scene,
                                mm.view_projection * (mm.model * vec4(end_b ? position_a : position_b, 1.0)), moved);
    vertex_color = mix(end_b ? color_b : color_a, end_b ? color_a : color_b, moved);
}
#else
layout(location = 0) in vec3 position;
layout(location = 1) in float value;  // channel value; above 1e29 in magnitude = "no data"
layout(location = 2) in vec4 color;   // RGBA8 vertex colour

void main() {
    channel_value = value;
    vertex_color = color;
    vec4 scene = mm.model * vec4(position, 1.0);
    scene_position = scene.xyz;
    gl_Position = mm.view_projection * scene;
}
#endif
