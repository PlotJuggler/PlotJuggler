// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
// Point-cloud cubes as the hexagon fan over the three camera-facing faces: 7
// logical vertices / 18 indices per instance, corners from gl_VertexIndex, no
// vertex buffer. The colour is constant over a cube, so it is resolved here.
#include "point_uniforms.glslinc"
#include "colormap.glslinc"
#include "../../shaders/cube/cube_point_color.glslinc"
#include "../../shaders/cube/cube_fan.glslinc"

layout(location = 0) in vec3 instance_position;
layout(location = 1) in uint instance_attribute;  // decodePointAttribute()

layout(location = 0) flat out vec4 v_color;  // rgb: target space; a: alpha
// Centroid: the fragment stage picks the face from the component that reached the
// surface; pixel-centre sampling extrapolates outside the triangle on partially
// covered MSAA pixels and can pick the wrong face.
layout(location = 1) centroid out vec3 v_local;

void main() {
    float instance_scalar;
    vec4 instance_color;
    decodePointAttribute(instance_attribute, instance_scalar, instance_color);
    vec3 in_render = (fixed_from_source * vec4(instance_position, 1.0)).xyz;
    float scalar = modes.w < 0 ? instance_scalar : in_render[modes.w] + color_axis_offset[modes.w];
    bool outside;
    // Culling here keeps the fragment stage discard-free (early depth).
    if (pointcloudCulled(scalar, range_params.x, range_params.y, range_params.z, int(range_params.w), outside)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec3 to_camera = face_pick_eye.xyz - face_pick_eye.w * in_render;
    v_local = cubeFanCorner(gl_VertexIndex, cubeFaceSigns(to_camera));
    gl_Position = view_projection * vec4(in_render + v_local * size_params.x, 1.0);
    vec3 base = resolveBaseColor(modes.x, solid_color.rgb, instance_color.rgb, modes.y, modes.z != 0, scalar,
                                 range_params.x, range_params.y);
    v_color = vec4(toTargetSpace(base), outside ? range_params.z : 1.0);
}
