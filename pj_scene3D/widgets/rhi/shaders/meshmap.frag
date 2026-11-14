// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "colormap.glslinc"
#include "srgb.glslinc"

layout(std140, binding = 0) uniform MeshMapUniforms {
    mat4 view_projection;
    mat4 model;
    vec4 eye;
    vec4 view_z;
    vec4 key_dir;
    vec4 color_params;
    vec4 light_params;
    vec4 flags;
} mm;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
} rm;

layout(location = 0) in float channel_value;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec3 scene_position;
layout(location = 0) out vec4 fragment_color;

void main() {
    int mode = int(mm.flags.x + 0.5);
    vec3 base;
    if (mode == 0) {
        // A sentinel, not NaN, marks "no data": NaN need not survive attribute
        // interpolation on every driver. kMeshMapNoDataValue (1e30) leaves
        // headroom, so perspective-correct interpolation stays finite; data is
        // clamped to 1e28.
        if (abs(channel_value) > 1.0e29) {
            base = vec3(0.5);
        } else {
            float span = mm.color_params.y - mm.color_params.x;
            float t = span > 0.0 ? clamp((channel_value - mm.color_params.x) / span, 0.0, 1.0) : 0.5;
            base = sampleColormap(int(mm.color_params.z), t);
        }
    } else if (mode == 2) {
        base = vec3(0.7);
    } else {
        base = vertex_color.rgb;
    }
    if (rm.render_mode.x != 0) {
        base = srgbToLinear(base);
    }
    float shade = 1.0;
    if (mode != 3 && mm.light_params.w > 0.5) {
        // Flat normal from the screen-space derivatives of the position, faced
        // towards the eye: a navigation surface has no inside, and this holds
        // whatever winding the producer used.
        // A degenerate or sub-pixel triangle has no derivative normal: face the
        // viewer instead of normalizing zero into NaN.
        vec3 face = cross(dFdx(scene_position), dFdy(scene_position));
        float face_length = length(face);
        vec3 normal = face_length > 1.0e-12 ? face / face_length : mm.view_z.xyz;
        if (dot(normal, mm.eye.xyz - scene_position) < 0.0) {
            normal = -normal;
        }
        shade = mm.light_params.x + mm.light_params.y * max(dot(normal, mm.key_dir.xyz), 0.0) +
                mm.light_params.z * max(dot(normal, mm.view_z.xyz), 0.0);
    }
    float alpha = mm.color_params.w * (mode == 1 || mode == 3 ? vertex_color.a : 1.0);
    fragment_color = vec4(base * shade, alpha);
}
