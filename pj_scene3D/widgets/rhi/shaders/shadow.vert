// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform ShadowUniforms {
    mat4 light_view_projection;
};

layout(location = 0) in vec3 mesh_position;
layout(location = 5) in mat4 instance_model;

void main() {
    gl_Position = light_view_projection * instance_model * vec4(mesh_position, 1.0);
}
