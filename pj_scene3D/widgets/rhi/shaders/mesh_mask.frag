// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(binding = 1) uniform sampler2D base_color_map;

layout(location = 0) in vec4 model_color;
// Unused, but declared: D3D11 links the stages by register, so skipping
// model.vert's location 1 would read the UV from the normal's register.
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) flat in vec4 model_params;

layout(location = 0) out vec4 fragment_color;

void main() {
    // Match model.frag's alpha-mask discard before declaring the fragment a
    // mesh pixel. Blended models still write a full binary mask, matching the
    // native mesh pass's independent location-1 output.
    float alpha = model_color.a;
    if (model_params.x > 0.5) {
        alpha *= texture(base_color_map, model_uv).a;
    }
    if (model_params.y > 0.5 && model_params.y < 1.5 && alpha < model_params.z) {
        discard;
    }
    fragment_color = vec4(1.0, 0.0, 0.0, 1.0);
}
