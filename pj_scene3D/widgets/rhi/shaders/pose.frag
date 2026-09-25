// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec3 view_normal;
layout(location = 1) in vec4 arrow_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    const vec3 light_direction = normalize(vec3(0.30, 0.55, 0.80));
    const float ambient = 0.35;
    float lambert = max(dot(normalize(view_normal), light_direction), 0.0);
    float shading = ambient + ((1.0 - ambient) * lambert);
    vec3 lit = arrow_color.rgb * shading;
    vec3 color = render_mode.x != 0 ? pow(max(lit, vec3(0.0)), vec3(2.2)) : lit;
    fragment_color = vec4(color, arrow_color.a);
}
