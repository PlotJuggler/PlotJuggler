// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440

// The twelve box edges of a cube marker (MarkerRhiPass), replaying the cube
// fill's instances. An edge with a
// camera-facing adjacent face is the fill colour darkened, a self-occluded one
// blends halfway back to the fill colour; both opaque.

layout(std140, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
};

layout(std140, binding = 4) uniform CameraView {
    mat4 view;  // the camera's view matrix, relative to the render origin
};

layout(location = 0) in vec3 mesh_position;
layout(location = 1) in vec3 edge_center;  // the edge's midpoint on the unit cube (two coordinates are +-0.5)
layout(location = 2) in mat4 instance_model;
layout(location = 6) in vec4 instance_color;

layout(location = 0) out vec4 marker_color;

const float kEdgeDarken = 0.6;
const float kHiddenEdgeMix = 0.45;
const float kEdgeVisibleEpsilon = 1e-4;

void main() {
    mat4 view_world = view * instance_model;
    mat3 normal_matrix = transpose(inverse(mat3(view_world)));
    vec3 center_view = (view_world * vec4(edge_center, 1.0)).xyz;
    float center_length = length(center_view);
    vec3 to_camera = center_length > 1e-6 ? -center_view / center_length : vec3(0.0, 0.0, 1.0);
    // The two faces an edge joins are the axes its midpoint sits out on.
    float facing = -1.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(edge_center[axis]) > 0.25) {
            vec3 normal = vec3(0.0);
            normal[axis] = sign(edge_center[axis]);
            facing = max(facing, dot(normalize(normal_matrix * normal), to_camera));
        }
    }
    vec3 front_color = instance_color.rgb * kEdgeDarken;
    vec3 hidden_color = mix(instance_color.rgb, front_color, kHiddenEdgeMix);
    gl_Position = view_projection * instance_model * vec4(mesh_position, 1.0);
    marker_color = vec4(facing > kEdgeVisibleEpsilon ? front_color : hidden_color, 1.0);
}
