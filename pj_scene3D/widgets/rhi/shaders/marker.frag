// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec4 marker_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    // Decoded marker colors are written directly into the scene target;
    // unlike display-referred point/voxel colors, they are not
    // linearized by the shader before the composite.
    fragment_color = marker_color;
}
