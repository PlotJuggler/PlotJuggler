// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

layout(location = 0) in vec3 mesh_position;
layout(location = 1) in vec3 mesh_normal;
layout(location = 2) in mat4 instance_model;
layout(location = 6) in vec4 instance_color;
layout(location = 7) in vec4 instance_params;

layout(location = 0) out vec4 marker_color;

void main() {
    vec3 tapered = mesh_position;
    float axial = clamp(mesh_position.z + 0.5, 0.0, 1.0);
    tapered.xy *= mix(instance_params.x, instance_params.y, axial);
    gl_Position = view_projection * instance_model * vec4(tapered, 1.0);
    marker_color = instance_color;
}
