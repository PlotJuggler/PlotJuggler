// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
// plot_rhi::Vertex::aux: across (0..1 for -1..1), style code, second coordinate
// high and low byte, each normalized from 0..255.
layout(location = 2) in vec4 a_aux;

layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_across;
layout(location = 2) out float v_code;
layout(location = 3) out float v_second;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = a_color;
    v_across = a_aux.x * 2.0 - 1.0;
    v_code = a_aux.y * 255.0;
    // The 16-bit value; the sum of the two interpolated bytes stays linear.
    v_second = a_aux.z * 65280.0 + a_aux.w * 255.0;
}
