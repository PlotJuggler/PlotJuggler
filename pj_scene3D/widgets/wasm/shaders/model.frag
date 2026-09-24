#version 440

layout(std140, binding = 6) uniform ModelLightingUniforms {
    vec4 camera_position;
    vec4 lighting;
    vec4 environment;
    mat4 light_view_projection;  // world -> shadow-map texture space (u, v, stored depth)
    vec4 shadow_params;
};

layout(binding = 1) uniform sampler2D base_color_map;
layout(binding = 2) uniform sampler2D metallic_roughness_map;
layout(binding = 3) uniform sampler2D normal_map;
layout(binding = 4) uniform sampler2D occlusion_map;
layout(binding = 5) uniform sampler2D emissive_map;
layout(binding = 7) uniform sampler2D shadow_map;

layout(std140, binding = 8) uniform RenderMode {
    ivec4 render_mode;  // x: 1 when writing the linear-light HDR target
};

layout(location = 0) in vec4 model_color;
layout(location = 1) in vec3 model_normal;
layout(location = 2) in vec2 model_uv;
layout(location = 3) flat in vec4 model_params;
layout(location = 4) in vec3 model_position;
layout(location = 5) in vec3 model_tangent;
layout(location = 6) in vec3 model_bitangent;
layout(location = 7) flat in vec4 model_tint;
layout(location = 8) flat in vec4 model_texture_flags;
layout(location = 9) flat in vec4 model_pbr_factors;
layout(location = 10) flat in vec4 model_emissive_factor;
layout(location = 11) in vec4 model_vertex_color;

layout(location = 0) out vec4 fragment_color;

const float PI = 3.14159265;

float distributionGgx(float normal_half, float alpha) {
    float alpha_squared = alpha * alpha;
    float denominator = normal_half * normal_half * (alpha_squared - 1.0) + 1.0;
    return alpha_squared / max(PI * denominator * denominator, 1e-5);
}

float visibilitySmithGgx(float normal_view, float normal_light, float alpha) {
    float alpha_squared = alpha * alpha;
    float view = normal_light * sqrt(normal_view * normal_view * (1.0 - alpha_squared) + alpha_squared);
    float light = normal_view * sqrt(normal_light * normal_light * (1.0 - alpha_squared) + alpha_squared);
    return 0.5 / max(view + light, 1e-5);
}

vec3 fresnelSchlick(vec3 f0, float view_half) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - view_half, 0.0, 1.0), 5.0);
}

vec3 shadeLight(
    vec3 normal, vec3 view, vec3 light, vec3 diffuse_color, vec3 f0, float alpha) {
    vec3 half_vector = normalize(view + light);
    float normal_light = max(dot(normal, light), 0.0);
    float normal_view = max(dot(normal, view), 0.0);
    float normal_half = max(dot(normal, half_vector), 0.0);
    float view_half = max(dot(view, half_vector), 0.0);
    vec3 fresnel = fresnelSchlick(f0, view_half);
    float specular = distributionGgx(normal_half, alpha) *
                     visibilitySmithGgx(normal_view, normal_light, alpha);
    return (diffuse_color / PI * (1.0 - fresnel) + fresnel * specular) * normal_light;
}

vec2 environmentBrdf(float normal_view, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * normal_view)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 environmentRadiance(vec3 direction) {
    const vec3 ground = vec3(0.28, 0.27, 0.25);
    const vec3 sky = vec3(0.50, 0.52, 0.55);
    return mix(ground, sky, clamp(direction.z * 0.5 + 0.5, 0.0, 1.0));
}

float shadowFactor(vec3 position, vec3 normal, vec3 light) {
    if (shadow_params.z < 0.5) {
        return 1.0;
    }
    float normal_light = max(dot(normal, light), 0.0);
    vec3 biased = position + normal *
        (shadow_params.x * (2.0 - normal_light) * max(shadow_params.y, 1.0));
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

vec3 linearToSrgb(vec3 color) {
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

void main() {
    // Preserve W19g/W19h's exact simple-lighting path for genuinely legacy
    // materials. PBR factors or any texture select the parity path below.
    if (model_pbr_factors.z < 0.5) {
        vec4 color = model_color;
        if (model_params.x > 0.5) {
            color *= texture(base_color_map, model_uv);
        }
        if (model_params.y > 0.5 && model_params.y < 1.5 && color.a < model_params.z) {
            discard;
        }
        vec3 normal = normalize(model_normal);
        vec3 light = normalize(vec3(0.35, -0.45, 0.82));
        float diffuse = max(dot(normal, light), 0.0) * shadowFactor(model_position, normal, light);
        if (render_mode.x != 0) {
            color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(2.2));
        }
        color.rgb *= 0.35 + 0.65 * diffuse;
        fragment_color = color;
        return;
    }

    vec3 base_rgb;
    float base_alpha;
    if (model_params.w > 0.5) {
        base_rgb = model_tint.rgb * pow(max(model_vertex_color.rgb, vec3(0.0)), vec3(2.2));
        base_alpha = model_tint.a * model_vertex_color.a;
    } else {
        base_rgb = pow(max(model_tint.rgb, vec3(0.0)), vec3(2.2));
        base_alpha = model_tint.a;
    }
    if (model_params.x > 0.5) {
        vec4 sampled_base = texture(base_color_map, model_uv);
        base_rgb *= sampled_base.rgb;
        base_alpha *= sampled_base.a;
    }
    if (model_params.y > 0.5 && model_params.y < 1.5 && base_alpha < model_params.z) {
        discard;
    }

    float metallic = clamp(model_pbr_factors.x, 0.0, 1.0);
    float roughness = model_pbr_factors.w > 0.5 ? 0.85 : model_pbr_factors.y;
    if (model_texture_flags.x > 0.5) {
        vec3 sampled_mr = texture(metallic_roughness_map, model_uv).rgb;
        roughness *= sampled_mr.g;
        metallic *= sampled_mr.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);
    float alpha = roughness * roughness;

    vec3 normal = normalize(model_normal);
    if (model_texture_flags.y > 0.5) {
        vec3 tangent_normal = texture(normal_map, model_uv).xyz * 2.0 - 1.0;
        mat3 tangent_basis = mat3(normalize(model_tangent), normalize(model_bitangent), normal);
        normal = normalize(tangent_basis * tangent_normal);
    }
    vec3 view = normalize(camera_position.xyz - model_position);
    float normal_view = max(dot(normal, view), 0.0);
    vec3 base = max(base_rgb, vec3(0.0));
    vec3 f0 = mix(vec3(lighting.x), base, metallic);
    vec3 diffuse_color = base * (1.0 - metallic);

    vec3 key_light = normalize(environment.xyz);
    vec3 fill_light = normalize(view + vec3(0.0, 0.0, 0.25));
    float key_shadow = shadowFactor(model_position, normal, key_light);
    vec3 direct = shadeLight(normal, view, key_light, diffuse_color, f0, alpha) * (lighting.z * key_shadow) +
                  shadeLight(normal, view, fill_light, diffuse_color, f0, alpha) * lighting.w;

    float occlusion = model_texture_flags.z > 0.5 ? texture(occlusion_map, model_uv).r : 1.0;
    vec3 diffuse_ibl = environmentRadiance(normal) * diffuse_color;
    vec3 reflection = reflect(-view, normal);
    vec3 prefiltered = environmentRadiance(mix(reflection, normal, roughness));
    vec2 dfg = environmentBrdf(normal_view, roughness);
    vec3 specular_ibl = prefiltered * (f0 * dfg.x + dfg.y) * environment.w;
    vec3 color = (diffuse_ibl + specular_ibl) * occlusion * lighting.y + direct;
    if (model_pbr_factors.w > 0.5) {
        color = mix(color, base, 0.35);
    }

    vec3 emissive = model_emissive_factor.rgb;
    if (model_texture_flags.w > 0.5) {
        emissive *= texture(emissive_map, model_uv).rgb;
    }
    color += emissive;
    vec3 output_color = max(color, vec3(0.0));
    if (render_mode.x == 0) {
        output_color = linearToSrgb(output_color);
    }
    fragment_color = vec4(output_color, clamp(base_alpha, 0.0, 1.0));
}
