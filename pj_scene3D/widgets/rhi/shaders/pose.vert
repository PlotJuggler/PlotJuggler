// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform PoseUniforms {
    mat4 view_projection;
    mat4 fixed_from_source;
    mat4 view;
};

layout(location = 0) in vec3 arrow_position;
layout(location = 1) in vec3 arrow_normal;
layout(location = 2) in mat4 instance_model;
layout(location = 6) in vec4 instance_color;

layout(location = 0) out vec3 view_normal;
layout(location = 1) out vec4 arrow_color;

void main() {
    vec4 source_position = instance_model * vec4(arrow_position, 1.0);
    vec4 fixed_position = fixed_from_source * source_position;
    gl_Position = view_projection * fixed_position;
    view_normal = mat3(view * fixed_from_source * instance_model) * arrow_normal;
    arrow_color = instance_color;
}
