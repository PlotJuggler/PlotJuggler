// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// GridMap heightfield: 6 * (cols-1) * (rows-1) procedural vertices, two
// triangles per quad between adjacent cell centres, elevation fetched per
// corner. A triangle touching a NaN corner is dropped through a flat validity.
layout(std140, binding = 0) uniform GridMapUniforms {
    mat4 view_projection;  // clip-corrected, camera-relative
    mat4 model;            // grid-local -> camera-relative scene
    mat4 normal_to_world;  // upper 3x3: normal matrix of model
    vec4 eye;              // xyz: camera eye, camera-relative
    vec4 view_z;           // xyz: world direction of view-space +z (towards the viewer)
    vec4 key_dir;          // xyz: world direction TO the key light
    vec4 cell_dims;        // xy: cell size, z: column count, w: row count
    vec4 color_params;     // x: range lo, y: range hi, z: colormap id, w: opacity
    vec4 light_params;     // x: ambient, y: direct, z: fill, w: lighting on
    vec4 flags;            // x: has elevation
} gm;

layout(binding = 1) uniform sampler2D elevation_texture;  // R32F, NaN = hole
layout(binding = 2) uniform sampler2D color_texture;      // R32F

layout(location = 0) flat out float valid_flag;
layout(location = 1) out float colormap_t;
layout(location = 2) out vec3 normal_world;
layout(location = 3) out vec3 scene_position;

bool isFinite(float v) {
    return !isnan(v) && !isinf(v);
}

float elevationAt(ivec2 cell) {
    return gm.flags.x > 0.5 ? texelFetch(elevation_texture, cell, 0).r : 0.0;
}

// Slope along one axis: central difference when both neighbours are finite and
// inside the grid, one-sided when only one is, flat when neither.
float slope(float here, ivec2 cell, ivec2 step_dir, int limit, float spacing) {
    int index = cell.x * step_dir.x + cell.y * step_dir.y;
    bool has_prev = index > 0;
    bool has_next = index < limit - 1;
    float prev = has_prev ? elevationAt(cell - step_dir) : 0.0;
    float next = has_next ? elevationAt(cell + step_dir) : 0.0;
    has_prev = has_prev && isFinite(prev);
    has_next = has_next && isFinite(next);
    if (has_prev && has_next) {
        return (next - prev) / (2.0 * spacing);
    }
    if (has_next) {
        return (next - here) / spacing;
    }
    if (has_prev) {
        return (here - prev) / spacing;
    }
    return 0.0;
}

ivec2 cornerOffset(int corner) {
    // (0,0)(1,0)(1,1) and (0,0)(1,1)(0,1): two CCW triangles seen from local +z.
    if (corner == 1) {
        return ivec2(1, 0);
    }
    if (corner == 2 || corner == 4) {
        return ivec2(1, 1);
    }
    if (corner == 5) {
        return ivec2(0, 1);
    }
    return ivec2(0, 0);
}

void main() {
    ivec2 dims = ivec2(int(gm.cell_dims.z), int(gm.cell_dims.w));
    int quads_x = dims.x - 1;
    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex - quad * 6;
    ivec2 quad_cell = ivec2(quad % quads_x, quad / quads_x);
    int tri_base = (corner / 3) * 3;

    // Every vertex tests all three corners of its triangle, so the flat
    // validity is the same whichever vertex is provoking.
    bool valid = true;
    for (int i = 0; i < 3; ++i) {
        valid = valid && isFinite(elevationAt(quad_cell + cornerOffset(tri_base + i)));
    }
    valid_flag = valid ? 1.0 : 0.0;

    ivec2 cell = quad_cell + cornerOffset(corner);
    float h = valid ? elevationAt(cell) : 0.0;

    vec3 normal_local = vec3(0.0, 0.0, 1.0);
    if (gm.light_params.w > 0.5 && gm.flags.x > 0.5 && valid) {
        float dzdx = slope(h, cell, ivec2(1, 0), dims.x, gm.cell_dims.x);
        float dzdy = slope(h, cell, ivec2(0, 1), dims.y, gm.cell_dims.y);
        normal_local = normalize(vec3(-dzdx, -dzdy, 1.0));
    }
    normal_world = normalize(mat3(gm.normal_to_world) * normal_local);

    float value = texelFetch(color_texture, cell, 0).r;
    float span = gm.color_params.y - gm.color_params.x;
    if (!isFinite(value)) {
        colormap_t = 0.0;  // a hole in the colour channel: the LUT low end
    } else if (!(span > 0.0)) {
        colormap_t = 0.5;  // degenerate range: the LUT midpoint
    } else {
        colormap_t = clamp((value - gm.color_params.x) / span, 0.0, 1.0);
    }

    vec4 scene = gm.model * vec4((vec2(cell) + 0.5) * gm.cell_dims.xy, h, 1.0);
    scene_position = scene.xyz;
    gl_Position = gm.view_projection * scene;
}
