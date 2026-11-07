// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "../../shaders/cube/cube_lighting.glslinc"

layout(location = 0) flat in vec4 v_color;
layout(location = 1) in vec2 v_corner;
layout(location = 2) flat in float v_disc;
layout(location = 0) out vec4 fragment_color;

void main() {
    float shading = 1.0;
    if (v_disc > 0.5) {
#ifdef PJ_POINT_TOPOLOGY
        vec2 corner = vec2(2.0 * gl_PointCoord.x - 1.0, 1.0 - 2.0 * gl_PointCoord.y);
#else
        vec2 corner = v_corner;
#endif
        float radius_squared = dot(corner, corner);
        if (radius_squared > 1.0) {
            discard;
        }
        if (v_disc < 1.5) {
            // Sphere imposter: the view-space normal of the sphere under this fragment.
            shading = cubeLambert(vec3(corner, sqrt(max(0.0, 1.0 - radius_squared))));
        }
    }
    fragment_color = vec4(v_color.rgb * shading, v_color.a);
}
