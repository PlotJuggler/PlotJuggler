// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
#include "../../shaders/cube/cube_lighting.glslinc"
#include "../../shaders/cube/cube_edge.glslinc"

layout(std140, binding = 0) uniform VoxelUniforms {
    mat4 view_projection;
    mat4 fixed_from_grid;
    mat4 view;
    vec4 cell_size_opacity;
    vec4 dims_kind;
    vec4 range_params;
    vec4 mode_params;
};

layout(binding = 2) uniform sampler2D color_lut;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec3 local_position;
layout(location = 1) in vec3 view_normal;
layout(location = 2) in float normalized_value;
layout(location = 3) in vec3 voxel_color;
layout(location = 0) out vec4 fragment_color;

void main() {
    if (cell_size_opacity.w <= 0.003) {
        discard;
    }
    vec3 base;
    if (int(dims_kind.w) == 1) {
        base = voxel_color;
    } else {
        float row = (mode_params.z + 0.5) / 4.0;
        base = texture(color_lut, vec2(normalized_value, row)).rgb;
    }
    vec3 normal = normalize(view_normal);
    float shading = cubeLambert(normal);
    vec3 outlined = base * (1.0 - kCubeEdgeDarken * cubeEdgeFactor(local_position));
    vec3 color = render_mode.x != 0 ? pow(max(outlined, vec3(0.0)), vec3(2.2)) : outlined;
    fragment_color = vec4(color * shading, cell_size_opacity.w);
}
