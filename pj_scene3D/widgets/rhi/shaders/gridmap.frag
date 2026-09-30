// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "colormap.glslinc"

layout(std140, binding = 0) uniform GridMapUniforms {
    mat4 view_projection;
    mat4 model;
    mat4 normal_to_world;
    vec4 eye;
    vec4 view_z;
    vec4 key_dir;
    vec4 cell_dims;
    vec4 color_params;
    vec4 light_params;
    vec4 flags;
} gm;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
} rm;

layout(location = 0) flat in float valid_flag;
layout(location = 1) in float colormap_t;
layout(location = 2) in vec3 normal_world;
layout(location = 3) in vec3 scene_position;
layout(location = 0) out vec4 fragment_color;

void main() {
    if (valid_flag < 0.5) {
        discard;
    }
    // In-shader polynomials, not a LUT.
    vec3 base = sampleColormap(int(gm.color_params.z), colormap_t);
    if (rm.render_mode.x != 0) {
        base = pow(max(base, vec3(0.0)), vec3(2.2));
    }
    float shade = 1.0;
    if (gm.light_params.w > 0.5) {
        // Two-sided (a heightfield has no inside): face the normal towards the
        // eye. Geometric, so it holds whatever winding a backend's clip-space
        // correction produces.
        vec3 normal = normalize(normal_world);
        if (dot(normal, gm.eye.xyz - scene_position) < 0.0) {
            normal = -normal;
        }
        shade = gm.light_params.x + gm.light_params.y * max(dot(normal, gm.key_dir.xyz), 0.0) +
                gm.light_params.z * max(dot(normal, gm.view_z.xyz), 0.0);
    }
    fragment_color = vec4(base * shade, gm.color_params.w);
}
