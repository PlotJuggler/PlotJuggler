// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(binding = 1) uniform sampler2D overlay_image;  // premultiplied, display-referred

layout(location = 0) in vec2 texture_uv;
layout(location = 0) out vec4 fragment_color;

void main() {
    fragment_color = texture(overlay_image, texture_uv);
}
