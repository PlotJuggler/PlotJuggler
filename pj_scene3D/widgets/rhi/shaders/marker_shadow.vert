// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// Depth-only shadow caster for the instanced marker primitives: marker.vert's
// per-instance layout (model + cone taper) projected by the light instead of
// the camera. Pairs with shadow.frag.

layout(std140, binding = 0) uniform ShadowUniforms {
    mat4 light_view_projection;
};

layout(location = 0) in vec3 mesh_position;
layout(location = 2) in mat4 instance_model;
layout(location = 7) in vec4 instance_params;

void main() {
    vec3 tapered = mesh_position;
    float axial = clamp(mesh_position.z + 0.5, 0.0, 1.0);
    tapered.xy *= mix(instance_params.x, instance_params.y, axial);
    gl_Position = light_view_projection * instance_model * vec4(tapered, 1.0);
}
