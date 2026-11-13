// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

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

layout(location = 0) in vec3 position;
layout(location = 1) in float value;  // channel value; above 1e29 in magnitude = "no data"
layout(location = 2) in vec4 color;   // RGBA8 vertex / line colour

layout(location = 0) out float channel_value;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec3 scene_position;

void main() {
    channel_value = value;
    vertex_color = color;
    vec4 scene = mm.model * vec4(position, 1.0);
    scene_position = scene.xyz;
    gl_Position = mm.view_projection * scene;
}
