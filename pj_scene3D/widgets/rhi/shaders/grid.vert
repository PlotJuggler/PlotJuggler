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
layout(location = 2) in vec3 other_end;  // the segment's other vertex
layout(location = 0) out vec4 vertex_color;

// Below every perspective near plane (camera_math adaptiveNearFar floors it at 1e-3), so the rasterizer still clips
// the moved endpoint at the near plane and the visible segment is unchanged.
const float kMinClipW = 1e-4;

void main() {
    vec4 clip = view_projection * vec4(position + origin_offset.xyz, 1.0);
    vec4 other = view_projection * vec4(other_end + origin_offset.xyz, 1.0);
    // Chromium's SwiftShader (WebGL2 through ANGLE/Vulkan) drops a whole line with one vertex behind the eye
    // (w < 0) instead of clipping it (TECHNICAL_NOTES "SwiftShader drops lines that cross behind the eye"): move
    // that vertex along the segment to just in front of the eye. Clip space is linear along the segment.
    if (clip.w < kMinClipW && other.w > kMinClipW) {
        clip = mix(clip, other, (kMinClipW - clip.w) / (other.w - clip.w));
    }
    gl_Position = clip;
    vertex_color = color;
}
