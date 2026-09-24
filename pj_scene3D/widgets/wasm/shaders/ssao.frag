#version 440

layout(std140, binding = 0) uniform SsaoUniforms {
    mat4 projection;          // view space -> texture space (u, v, stored depth)
    mat4 inverse_projection;  // texture space -> view space
    vec4 ssao_params;  // radius in metres, AO power, depth bias, texture-v sign (see textureVSign)
    vec4 kernel[32];
};

layout(binding = 1) uniform sampler2D scene_depth;

layout(location = 0) in vec2 texture_uv;
layout(location = 0) out vec4 fragment_color;

vec3 viewPosition(vec2 uv) {
    float depth = texture(scene_depth, uv).r;
    vec4 view = inverse_projection * vec4(uv, depth, 1.0);
    return view.xyz / view.w;
}

vec3 normalFromDepth(vec2 uv) {
    vec2 texel = 1.0 / vec2(textureSize(scene_depth, 0));
    vec3 center = viewPosition(uv);
    vec3 left = viewPosition(uv - vec2(texel.x, 0.0));
    vec3 right = viewPosition(uv + vec2(texel.x, 0.0));
    vec3 down = viewPosition(uv - vec2(0.0, texel.y));
    vec3 up = viewPosition(uv + vec2(0.0, texel.y));
    vec3 horizontal = abs(left.z - center.z) < abs(right.z - center.z)
        ? center - left
        : right - center;
    vec3 vertical = abs(down.z - center.z) < abs(up.z - center.z)
        ? center - down
        : up - center;
    return normalize(cross(horizontal, vertical)) * ssao_params.w;
}

void main() {
    float depth = texture(scene_depth, texture_uv).r;
    if (depth >= 0.9999) {
        fragment_color = vec4(0.0, 1.0, 0.0, 0.0);
        return;
    }

    vec3 position = viewPosition(texture_uv);
    vec3 normal = normalFromDepth(texture_uv);
    vec2 tile = vec2(ivec2(gl_FragCoord.xy) & 3);
    float angle = fract(sin(dot(tile, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    vec3 random_vector = vec3(cos(angle), sin(angle), 0.0);
    vec3 tangent = normalize(random_vector - normal * dot(random_vector, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tangent_basis = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int index = 0; index < 32; ++index) {
        vec3 sample_position = position + (tangent_basis * kernel[index].xyz) * ssao_params.x;
        vec4 projected = projection * vec4(sample_position, 1.0);
        projected.xyz /= projected.w;
        if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0) {
            continue;
        }
        float sample_depth = viewPosition(projected.xy).z;
        float range = smoothstep(
            0.0,
            1.0,
            ssao_params.x / max(abs(position.z - sample_depth), 1.0e-4));
        occlusion += (sample_depth >= sample_position.z + ssao_params.z ? 1.0 : 0.0) * range;
    }

    float ao = pow(clamp(1.0 - occlusion / 32.0, 0.0, 1.0), ssao_params.y);
    fragment_color = vec4(0.0, ao, 0.0, 0.0);
}
