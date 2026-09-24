#version 440

layout(std140, binding = 1) uniform ModelLightingUniforms {
    vec4 camera_position;
    vec4 lighting;
    vec4 environment;
    mat4 light_view_projection;  // world -> shadow-map texture space (u, v, stored depth)
    vec4 shadow_params;
};

layout(binding = 2) uniform sampler2D shadow_map;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec3 render_position;
layout(location = 0) out vec4 fragment_color;

float floorShadow(vec3 position) {
    vec3 biased = position + vec3(0.0, 0.0, shadow_params.x * max(shadow_params.y, 1.0));
    vec4 light_clip = light_view_projection * vec4(biased, 1.0);
    vec3 projected = light_clip.xyz / light_clip.w;
    if (projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0 ||
        projected.z > 1.0) {
        return 1.0;
    }
    const vec2 poisson[16] = vec2[](
        vec2(-0.942, -0.399), vec2(0.946, -0.769), vec2(-0.094, -0.929), vec2(0.345, 0.294),
        vec2(-0.916, 0.458), vec2(-0.815, -0.879), vec2(-0.383, 0.277), vec2(0.975, 0.756),
        vec2(0.443, -0.975), vec2(0.537, -0.474), vec2(-0.265, -0.419), vec2(0.792, 0.191),
        vec2(-0.242, 0.997), vec2(-0.814, 0.914), vec2(0.200, 0.786), vec2(0.144, -0.141));
    vec2 texel = (1.0 / vec2(textureSize(shadow_map, 0))) * max(shadow_params.y, 1.0);
    float lit = 0.0;
    for (int index = 0; index < 16; ++index) {
        lit += projected.z <= texture(shadow_map, projected.xy + poisson[index] * texel).r ? 1.0 : 0.0;
    }
    return lit / 16.0;
}

void main() {
    float shade = mix(0.45, 1.0, floorShadow(render_position));
    vec3 color = render_mode.x != 0 ? pow(max(vertex_color.rgb, vec3(0.0)), vec3(2.2)) : vertex_color.rgb;
    fragment_color = vec4(color * shade, vertex_color.a);
}
