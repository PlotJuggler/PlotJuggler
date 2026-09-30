// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// Screen-space ribbon: each polyline point arrives as two vertices carrying the
// point's tangent and a +/-1 side; the offset is half the width in pixels,
// perpendicular to the tangent's screen projection.
layout(std140, binding = 0) uniform TrailUniforms {
    mat4 view_projection;  // clip-corrected, camera-relative
    vec4 color;            // rgb: the half's colour
    vec4 params;           // xy: render-target size in pixels, z: half width in pixels
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 direction;
layout(location = 2) in float side;

void main() {
    vec4 clip = view_projection * vec4(position, 1.0);
    vec4 clip_ahead = view_projection * vec4(position + direction, 1.0);
    vec2 ndc0 = clip.xy / max(abs(clip.w), 1e-6);
    vec2 ndc1 = clip_ahead.xy / max(abs(clip_ahead.w), 1e-6);
    vec2 screen_dir = (ndc1 - ndc0) * params.xy;
    float len = length(screen_dir);
    vec2 normal = len > 1e-6 ? vec2(-screen_dir.y, screen_dir.x) / len : vec2(0.0);
    clip.xy += normal * side * params.z * (2.0 * clip.w / params.xy);
    gl_Position = clip;
}
