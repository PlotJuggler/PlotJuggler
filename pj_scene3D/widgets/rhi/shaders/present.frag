// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#version 440
#extension GL_GOOGLE_include_directive : require

layout(std140, binding = 0) uniform CompositeUniforms {
    vec4 composite_params;  // exposure, saturation, background coverage threshold, EDL floor
    mat4 inverse_projection;  // texture space (u, v, stored depth) -> view space
    vec4 edl_params;        // strength, radius in pixels, maximum neighbour gap, enabled
    vec4 ssao_params;       // AO strength, enabled, coverage from depth (the depth-resolve chain), unused
    vec4 grade_params;      // tonemap mode (0 None, 1 ACES, 2 AgX, 3 Neutral), unused x3
};

layout(binding = 1) uniform sampler2D scene_color;
layout(binding = 2) uniform sampler2D scene_coverage;
layout(binding = 3) uniform sampler2D scene_depth;

layout(location = 0) in vec2 texture_uv;
layout(location = 0) out vec4 fragment_color;

#include "srgb.glslinc"

vec3 aces(vec3 color) {
    return clamp(
        (color * (2.51 * color + 0.03)) /
            (color * (2.43 * color + 0.59) + 0.14),
        0.0,
        1.0);
}

// AgX: adapted from three.js tonemapping_pars_fragment (MIT; Filament/Sobotka
// derived). Neutral: Khronos PBR Neutral (Apache-2.0, via three.js). Full
// license texts: pj_scene3D/THIRDPARTY.md.
vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// GLSL mat3 constructors are COLUMN-major; these are three.js's AgXInset/Outset
// columns (transposing them tints greys blue).
const mat3 AGX_IN = mat3(0.856627, 0.137319, 0.111898, 0.0951212, 0.761242, 0.0767994, 0.0482516, 0.101439, 0.811302);
const mat3 AGX_OUT = mat3(1.127101, -0.141330, -0.141330, -0.110607, 1.157824, -0.110607, -0.016494, -0.016494, 1.251936);
const mat3 SRGB_TO_REC2020 = mat3(0.627404, 0.069097, 0.016392, 0.329282, 0.919540, 0.088013, 0.043314, 0.011361, 0.895595);
const mat3 REC2020_TO_SRGB = mat3(1.660500, -0.124551, -0.018151, -0.587641, 1.132900, -0.100579, -0.072850, -0.008349, 1.118730);

vec3 agx(vec3 color) {
    color = SRGB_TO_REC2020 * color;
    color = AGX_IN * color;
    color = max(color, vec3(1e-10));
    color = log2(color);
    color = clamp((color + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);
    color = agxContrast(color);
    color = AGX_OUT * color;
    color = pow(max(color, vec3(0.0)), vec3(2.2));
    color = REC2020_TO_SRGB * color;
    return clamp(color, 0.0, 1.0);
}

vec3 pbrNeutral(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) {
        return color;
    }
    float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

vec3 tonemap(vec3 color) {
    int mode = int(grade_params.x + 0.5);
    return mode == 1   ? aces(color)
           : mode == 2 ? agx(color)
           : mode == 3 ? pbrNeutral(color)
                       : clamp(color, vec3(0.0), vec3(1.0));
}

const float FAR_SENTINEL = 1.0e6;

bool isMesh(vec2 uv) {
    return texture(scene_coverage, uv).r > 0.01;
}

float meshDepth(vec2 uv) {
    if (!isMesh(uv)) {
        return FAR_SENTINEL;
    }
    float depth = texture(scene_depth, uv).r;
    vec4 view = inverse_projection * vec4(uv, depth, 1.0);
    return log2(max(abs(view.z / view.w), 1.0e-6));
}

float edlShade(vec2 uv) {
    if (edl_params.w < 0.5 || !isMesh(uv)) {
        return 1.0;
    }
    const vec2 offsets[8] = vec2[](
        vec2(1.0, 0.0), vec2(0.70710678, 0.70710678), vec2(0.0, 1.0),
        vec2(-0.70710678, 0.70710678), vec2(-1.0, 0.0), vec2(-0.70710678, -0.70710678),
        vec2(0.0, -1.0), vec2(0.70710678, -0.70710678));
    float center = meshDepth(uv);
    vec2 texel = edl_params.y / vec2(textureSize(scene_depth, 0));
    float response = 0.0;
    for (int index = 0; index < 8; ++index) {
        float neighbour = meshDepth(uv + offsets[index] * texel);
        response += min(max(0.0, neighbour - center), edl_params.z);
    }
    response /= 8.0;
    return exp(-response * 300.0 * edl_params.x);
}

float ssaoShade(vec2 uv) {
    if (ssao_params.y < 0.5) {
        return 1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(scene_coverage, 0));
    float sum = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            sum += texture(scene_coverage, uv + vec2(float(x), float(y)) * texel).g;
        }
    }
    return sum / 16.0;
}

void main() {
    vec4 scene = texture(scene_color, texture_uv);
    // The depth-resolve chain has no replay coverage: a covered pixel is one a
    // depth-writing draw reached, as the replay's alpha records.
    float coverage = ssao_params.z > 0.5 ? (texture(scene_depth, texture_uv).r < 1.0 ? 1.0 : 0.0)
                                         : texture(scene_coverage, texture_uv).a;
    vec3 hdr = scene.rgb * composite_params.x;
    hdr *= mix(1.0, ssaoShade(texture_uv), ssao_params.x);
    hdr *= mix(composite_params.w, 1.0, edlShade(texture_uv));
    vec3 graded = tonemap(hdr);
    float luminance = dot(graded, vec3(0.2126, 0.7152, 0.0722));
    graded = clamp(mix(vec3(luminance), graded, composite_params.y), vec3(0.0), vec3(1.0));
    if (coverage <= composite_params.z) {
        graded = clamp(scene.rgb, vec3(0.0), vec3(1.0));
    }
    vec3 display_linear = mix(clamp(scene.rgb, vec3(0.0), vec3(1.0)), graded, scene.a);
    fragment_color = vec4(linearToSrgb(display_linear), 1.0);
}
