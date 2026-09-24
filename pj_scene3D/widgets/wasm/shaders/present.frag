#version 440

layout(std140, binding = 0) uniform CompositeUniforms {
    vec4 composite_params;  // exposure, saturation, background coverage threshold, EDL floor
    mat4 inverse_projection;  // texture space (u, v, stored depth) -> view space
    vec4 edl_params;        // strength, radius in pixels, maximum neighbour gap, enabled
    vec4 ssao_params;       // AO strength, enabled, unused, unused
};

layout(binding = 1) uniform sampler2D scene_color;
layout(binding = 2) uniform sampler2D scene_coverage;
layout(binding = 3) uniform sampler2D scene_depth;

layout(location = 0) in vec2 texture_uv;
layout(location = 0) out vec4 fragment_color;

vec3 linearToSrgb(vec3 color) {
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

vec3 aces(vec3 color) {
    return clamp(
        (color * (2.51 * color + 0.03)) /
            (color * (2.43 * color + 0.59) + 0.14),
        0.0,
        1.0);
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
    float coverage = texture(scene_coverage, texture_uv).a;
    vec3 hdr = scene.rgb * composite_params.x;
    hdr *= mix(1.0, ssaoShade(texture_uv), ssao_params.x);
    hdr *= mix(composite_params.w, 1.0, edlShade(texture_uv));
    vec3 graded = aces(hdr);
    float luminance = dot(graded, vec3(0.2126, 0.7152, 0.0722));
    graded = clamp(mix(vec3(luminance), graded, composite_params.y), vec3(0.0), vec3(1.0));
    if (coverage <= composite_params.z) {
        graded = clamp(scene.rgb, vec3(0.0), vec3(1.0));
    }
    vec3 display_linear = mix(clamp(scene.rgb, vec3(0.0), vec3(1.0)), graded, scene.a);
    fragment_color = vec4(linearToSrgb(display_linear), 1.0);
}
