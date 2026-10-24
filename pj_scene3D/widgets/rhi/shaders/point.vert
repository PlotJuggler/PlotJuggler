// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require
// Points, sphere imposters and cube LOD sprites as instanced camera-facing quads
// (gl_PointSize does not exist on D3D): one instance per point, four strip
// vertices from gl_VertexIndex. Everything constant over a point (colour,
// flat shading, fade) is resolved here once.
#include "point_uniforms.glslinc"
#include "colormap.glslinc"
#include "../../shaders/cube/cube_point_color.glslinc"
#include "../../shaders/cube/cube_lighting.glslinc"
#include "../../shaders/cube/cube_fan.glslinc"
#include "../../shaders/cube/cube_sprite.glslinc"

layout(location = 0) in vec3 instance_position;
layout(location = 1) in uint instance_attribute;  // decodePointAttribute()

layout(location = 0) flat out vec4 v_color;   // rgb: target space, shaded unless an imposter; a: alpha
layout(location = 1) out vec2 v_corner;       // [-1, 1] across the quad, +y screen-up
layout(location = 2) flat out float v_disc;   // 0: square, 1: shaded sphere imposter, 2: flat disc

void cull() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}

void main() {
    float instance_scalar;
    vec4 instance_color;
    decodePointAttribute(instance_attribute, instance_scalar, instance_color);
    vec3 in_render = (fixed_from_source * vec4(instance_position, 1.0)).xyz;
    vec4 clip = view_projection * vec4(in_render, 1.0);
    float scalar = modes.w < 0 ? instance_scalar : in_render[modes.w] + color_axis_offset[modes.w];
    bool outside;
    if (pointcloudCulled(scalar, range_params.x, range_params.y, range_params.z, int(range_params.w), outside)) {
        cull();
        return;
    }
    vec3 base = resolveBaseColor(modes.x, solid_color.rgb, instance_color.rgb, modes.y, modes.z != 0, scalar,
                                 range_params.x, range_params.y);
    float shading = 1.0;
    float size_px;
    v_disc = 0.0;
    if (shape.x == 2) {
        // Cube LOD: a square of the cube's projected area, lit as its visible faces.
        vec3 to_camera = face_pick_eye.xyz - face_pick_eye.w * in_render;
        vec3 weights = cubeProjectedFaceWeights(to_camera);
        float cos_view_angle =
            face_pick_eye.w > 0.5 ? clamp(clip.w / max(length(to_camera), 1e-6), 1e-3, 1.0) : 1.0;
        float size_px_scale = size_params.x * size_params.w * size_params.z * 0.5;
        size_px = cubeSpriteSidePx(weights, size_px_scale / max(clip.w, 1e-4), cos_view_angle);
        if (size_px <= 0.0) {
            cull();  // a zero-size cube draws nothing, not a min-size dot
            return;
        }
        // Floor at one output pixel, at full colour, like every other shape.
        size_px = max(size_px, pixel_scale);
        shading = cubeSpriteShading(cubeFaceSigns(to_camera), weights, view);
    } else if (shape.x == 1) {
        // Project the sphere diameter; beyond the depth threshold the perspective
        // falloff softens to 1/sqrt(depth * threshold) so far spheres stay visible.
        size_px = size_params.x * 0.5 * size_params.w * size_params.z;
        if (clamp_params.w > 0.5) {
            float depth = max(clip.w, 0.01);
            size_px /= depth <= clamp_params.z ? depth : sqrt(depth * clamp_params.z);
        }
        size_px = clamp(size_px, clamp_params.x, clamp_params.y);
        if (size_px < kSphereDiscMaxPx * pixel_scale) {
            v_disc = 2.0;
            shading = kSphereDiscShading;
        } else {
            v_disc = 1.0;
        }
    } else {
        size_px = clamp(size_params.y, clamp_params.x, clamp_params.y);
    }
    v_color = vec4(toTargetSpace(base) * shading, outside ? range_params.z : 1.0);

#ifdef PJ_POINT_TOPOLOGY
    // Point-list fast path for backends with QRhi::VertexShaderPointSize.
    v_corner = vec2(0.0);
    gl_PointSize = size_px;
    gl_Position = clip;
#else
    vec2 corner = vec2((gl_VertexIndex & 1) != 0 ? 1.0 : -1.0, (gl_VertexIndex & 2) != 0 ? 1.0 : -1.0);
    v_corner = corner;
    vec2 offset = corner * (0.5 * size_px) * clip_per_px * vec2(1.0, clip_y_up);
    gl_Position = clip + vec4(offset * clip.w, 0.0, 0.0);
#endif
}
