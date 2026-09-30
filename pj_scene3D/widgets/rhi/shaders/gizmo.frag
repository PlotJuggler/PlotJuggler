// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// The corner axis gizmo, drawn on the display-referred output after
// presentation (pose.vert supplies the view-space normal). The desktop GL
// renderer writes the same lit colour to its linear HDR target as
// pow(lit, 2.2) and presents it ungraded through the sRGB encode; this shader
// reproduces that encode directly.
layout(location = 0) in vec3 view_normal;
layout(location = 1) in vec4 arrow_color;
layout(location = 0) out vec4 fragment_color;

vec3 linearToSrgb(vec3 color) {
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

void main() {
    const vec3 light_direction = normalize(vec3(0.30, 0.55, 0.80));
    const float ambient = 0.35;
    float lambert = max(dot(normalize(view_normal), light_direction), 0.0);
    vec3 lit = arrow_color.rgb * (ambient + ((1.0 - ambient) * lambert));
    vec3 linear = clamp(pow(max(lit, vec3(0.0)), vec3(2.2)), vec3(0.0), vec3(1.0));
    fragment_color = vec4(linearToSrgb(linear), arrow_color.a);
}
