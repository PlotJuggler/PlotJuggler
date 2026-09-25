// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform OverlayUniforms {
    vec4 rect;  // clip-space x/y of the image's top-left (xy) and bottom-right (zw) corners
};

layout(location = 0) out vec2 texture_uv;

// A four-vertex triangle strip; texture row 0 is the image's top row.
void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
    texture_uv = corner;
}
