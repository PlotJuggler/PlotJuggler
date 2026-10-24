// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

layout(location = 0) in vec3 mesh_position;
layout(location = 1) in vec3 mesh_normal;
layout(location = 2) in vec4 mesh_color;
layout(location = 3) in vec2 mesh_uv;
layout(location = 4) in vec4 mesh_tangent;
layout(location = 5) in mat4 instance_model;
layout(location = 9) in vec4 instance_tint;
layout(location = 10) in vec4 instance_params;
layout(location = 11) in vec4 instance_texture_flags;
layout(location = 12) in vec4 instance_pbr_factors;
layout(location = 13) in vec4 instance_emissive_factor;

layout(location = 0) out vec4 model_color;
layout(location = 1) out vec3 model_normal;
layout(location = 2) out vec2 model_uv;
layout(location = 3) flat out vec4 model_params;
layout(location = 4) out vec3 model_position;
layout(location = 5) out vec3 model_tangent;
layout(location = 6) out vec3 model_bitangent;
layout(location = 7) flat out vec4 model_tint;
layout(location = 8) flat out vec4 model_texture_flags;
layout(location = 9) flat out vec4 model_pbr_factors;
layout(location = 10) flat out vec4 model_emissive_factor;
layout(location = 11) out vec4 model_vertex_color;

void main() {
    vec4 world = instance_model * vec4(mesh_position, 1.0);
    mat3 normal_matrix = transpose(inverse(mat3(instance_model)));
    vec3 normal = normalize(normal_matrix * mesh_normal);
    vec3 tangent = normal_matrix * mesh_tangent.xyz;
    tangent = normalize(tangent - normal * dot(normal, tangent));

    gl_Position = view_projection * world;
    model_color = (instance_params.w > 0.5 ? mesh_color : vec4(1.0)) * instance_tint;
    model_normal = normal;
    model_uv = mesh_uv;
    model_params = instance_params;
    model_position = world.xyz;
    model_tangent = tangent;
    model_bitangent = cross(normal, tangent) * mesh_tangent.w;
    model_tint = instance_tint;
    model_texture_flags = instance_texture_flags;
    model_pbr_factors = instance_pbr_factors;
    model_emissive_factor = instance_emissive_factor;
    model_vertex_color = mesh_color;
}
