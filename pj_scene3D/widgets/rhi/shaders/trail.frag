// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform TrailUniforms {
    mat4 view_projection;
    vec4 color;
    vec4 params;
};

layout(location = 0) out vec4 fragment_color;

void main() {
    // Written as-is (no linearisation).
    fragment_color = vec4(color.rgb, 1.0);
}
