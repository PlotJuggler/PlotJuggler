#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// Y plane (or RGBA / BGRA / R8-mono texture, depending on pixelFormat)
layout(binding = 1) uniform sampler2D y_tex;
// U plane (or packed UV for NV12)
layout(binding = 2) uniform sampler2D u_tex;
// V plane (unused for NV12/RGBA)
layout(binding = 3) uniform sampler2D v_tex;
// Rectification lookup: per output pixel, the SOURCE sample point in [0,1]
// texture space, as the bits of two floats (RG32UI: QRhi has no RG32F); .r < 0
// marks an out-of-bounds pixel (render black). Fetched texel-exact so each
// output pixel gets its precomputed source coord.
layout(binding = 4) uniform usampler2D remap_tex;

layout(std140, binding = 0) uniform Uniforms {
    mat4 viewTransform;
    mat4 colorMatrix;
    int pixelFormat;  // 0 = YUV420P, 1 = NV12, 2 = RGBA, 3 = Mono8, 4 = BGRA, 5 = Depth
    float opacity;
    int rectify;      // 1 = remap v_uv through remap_tex before sampling
    int invert;       // depth: 1 = mirror the colormap (t -> 1 - t)
    float near_m;     // depth: range start (metres)
    float far_m;      // depth: range end (metres)
    int colormap_id;  // depth: LUT row (DepthColormap id)
};

void main()
{
    // The quad spans the (rectified) image; clip anything outside it to black.
    if (v_uv.x < 0.0 || v_uv.x > 1.0 || v_uv.y < 0.0 || v_uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 uv = v_uv;
    if (rectify == 1) {
        // Look up the source sample point for this output pixel.
        ivec2 remap_size = textureSize(remap_tex, 0);
        ivec2 texel = clamp(ivec2(v_uv * vec2(remap_size)), ivec2(0), remap_size - 1);
        vec2 s = uintBitsToFloat(texelFetch(remap_tex, texel, 0).rg);
        if (s.x < 0.0) {  // out-of-bounds sentinel -> black border
            fragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        uv = s;
    }

    // Depth (R32F in y_tex): normalize metric depth by [near,far] and map through
    // the colormap LUT (u_tex: 256 wide x 4 colormap rows). 0 == no-data.
    if (pixelFormat == 5) {
        float d = texture(y_tex, uv).r;
        if (!(d > 0.0)) {
            fragColor = vec4(0.0, 0.0, 0.0, 0.0);
            return;
        }
        float t = clamp((d - near_m) / max(far_m - near_m, 1e-6), 0.0, 1.0);
        if (invert == 1) {
            t = 1.0 - t;
        }
        // 4.0 is the LUT row count; MUST equal PJ::kColormapCount (pj_widgets/Colormap.h).
        // If a colormap is appended there, bump this divisor and recompile the .qsb.
        float row = (float(colormap_id) + 0.5) / 4.0;
        vec3 c = texture(u_tex, vec2(t, row)).rgb;
        fragColor = vec4(c, opacity);
        return;
    }

    // RGBA passthrough
    if (pixelFormat == 2) {
        fragColor = texture(y_tex, uv);
        fragColor.a *= opacity;
        return;
    }

    // BGRA: swizzle to RGBA (uploaded verbatim, no CPU repack)
    if (pixelFormat == 4) {
        fragColor = texture(y_tex, uv).bgra;
        fragColor.a *= opacity;
        return;
    }

    // Mono8 (single R8 texture): expand gray to RGB
    if (pixelFormat == 3) {
        float g = texture(y_tex, uv).r;
        fragColor = vec4(g, g, g, opacity);
        return;
    }

    // Sample Y (full resolution)
    float y = texture(y_tex, uv).r;
    float u, v;

    // 4:2:0 chroma sample (x / 2, y / 2) belongs to luma pixel (x, y). At an odd
    // size the (n + 1) / 2-sample chroma plane ends in a sample covering one luma
    // pixel, so scale uv by n / (2 * chroma_n) instead of stretching the plane over
    // the frame. The select keeps even sizes exactly at uv (no rounded n / n).
    ivec2 luma_size = textureSize(y_tex, 0);
    ivec2 chroma_span = 2 * textureSize(u_tex, 0);
    vec2 chroma_uv = mix(uv * vec2(luma_size) / vec2(chroma_span), uv, equal(luma_size, chroma_span));

    if (pixelFormat == 1) {
        // NV12: UV interleaved in a single RG texture
        vec2 uv_val = texture(u_tex, chroma_uv).rg;
        u = uv_val.r;
        v = uv_val.g;
    } else {
        // YUV420P: separate U and V planes
        u = texture(u_tex, chroma_uv).r;
        v = texture(v_tex, chroma_uv).r;
    }

    // YUV → RGB via color matrix (BT.709 or BT.601)
    vec3 yuv = vec3(y, u - 0.5, v - 0.5);
    vec3 rgb = (colorMatrix * vec4(yuv, 1.0)).rgb;
    fragColor = vec4(clamp(rgb, 0.0, 1.0), opacity);
}
