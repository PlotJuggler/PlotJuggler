#version 440

layout(location = 0) out vec2 texture_uv;

// Baked twice: plain, and with -DPJ_FLIP_Y for backends whose NDC and
// framebuffer disagree on Y (D3D, Metal), where texture row 0 is the top row.
void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
#ifdef PJ_FLIP_Y
    texture_uv = vec2(corner.x, 1.0 - corner.y);
#else
    texture_uv = corner;
#endif
}
