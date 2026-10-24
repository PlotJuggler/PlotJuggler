// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "point_uniforms.glslinc"
#include "../../shaders/cube/cube_face_normal.glslinc"
#include "../../shaders/cube/cube_lighting.glslinc"
#include "../../shaders/cube/cube_edge.glslinc"

layout(location = 0) flat in vec4 v_color;
layout(location = 1) centroid in vec3 v_local;
layout(location = 0) out vec4 fragment_color;

// No discard, by design: the vertex stage culls.
void main() {
    float shading = cubeLambert(cubeFaceNormalView(v_local, mat3(view)));
    // The outline scales the colour, so it commutes with the linearization done in
    // the vertex stage: pow(b * k, 2.2) == pow(b, 2.2) * pow(k, 2.2).
    float edge_scale = 1.0 - kCubeEdgeDarken * cubeEdgeFactor(v_local);
    fragment_color = vec4(v_color.rgb * toTargetSpace(vec3(edge_scale)).x * shading, v_color.a);
}
