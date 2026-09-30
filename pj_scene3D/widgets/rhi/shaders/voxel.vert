// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

layout(std140, binding = 0) uniform VoxelUniforms {
    mat4 view_projection;
    mat4 fixed_from_grid;
    mat4 view;
    vec4 cell_size_opacity;  // xyz cell dimensions, w layer opacity
    vec4 dims_kind;          // xyz lattice dimensions, w 0 scalar / 1 rgba
    vec4 range_params;       // threshold, predicate min, predicate max, color min
    vec4 mode_params;        // color max, draw mode, colormap id, unused
};

layout(binding = 1) uniform sampler3D voxel_volume;

layout(location = 0) in vec3 corner_position;
layout(location = 1) in vec3 corner_normal;

layout(location = 0) out vec3 local_position;
layout(location = 1) out vec3 view_normal;
layout(location = 2) out float normalized_value;
layout(location = 3) out vec3 voxel_color;

bool scalarPredicate(int mode, float value) {
    if (mode == 0) {
        return true;
    }
    if (mode == 1) {
        return value != 0.0;
    }
    if (mode == 2) {
        return value >= range_params.x;
    }
    return value >= range_params.y && value <= range_params.z;
}

void main() {
    int columns = int(dims_kind.x);
    int rows = int(dims_kind.y);
    int instance = gl_InstanceIndex;
    int column = instance % columns;
    int row = (instance / columns) % rows;
    int slice = instance / (columns * rows);
    vec4 texel = texelFetch(voxel_volume, ivec3(column, row, slice), 0);
    int kind = int(dims_kind.w);
    int draw_mode = int(mode_params.y);
    bool draw;
    if (kind == 1) {
        voxel_color = texel.rgb;
        normalized_value = 0.0;
        draw = draw_mode == 0 || texel.a > 0.001;
    } else {
        voxel_color = vec3(0.0);
        float span = max(mode_params.x - range_params.w, 1.0e-9);
        normalized_value = clamp((texel.r - range_params.w) / span, 0.0, 1.0);
        draw = scalarPredicate(draw_mode, texel.r);
    }

    local_position = corner_position;
    if (!draw) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        view_normal = vec3(0.0, 0.0, 1.0);
        return;
    }

    vec3 center = (vec3(column, row, slice) + 0.5) * cell_size_opacity.xyz;
    vec3 grid_position = center + corner_position * cell_size_opacity.xyz;
    vec4 fixed_position = fixed_from_grid * vec4(grid_position, 1.0);
    gl_Position = view_projection * fixed_position;
    view_normal = mat3(view * fixed_from_grid) * corner_normal;
}
