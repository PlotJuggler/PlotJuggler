// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_scene3d_core/cube_draw_policy.h"
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/cube_mesh.h"  // kCubeFanIndices + the fan / face-normal / edge GLSL
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/point_shading.h"

namespace pj::scene3d {

static_assert(
    kGlFloat == GL_FLOAT && kGlUnsignedInt == GL_UNSIGNED_INT && kGlShort == GL_SHORT &&
        kGlUnsignedShort == GL_UNSIGNED_SHORT && kGlByte == GL_BYTE && kGlUnsignedByte == GL_UNSIGNED_BYTE &&
        kGlInt == GL_INT,
    "core GL enum mirror drift");

namespace {

constexpr std::string_view kPointcloudVertHead = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
// Integer attribs bound with normalized=GL_FALSE are widened to float by GL,
// matching readScalarAt (signed types sign-extend).
layout(location = 1) in float in_scalar;
layout(location = 2) in vec4 in_color;  // per-point RGBA in [0,1] (kRgb mode)
uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid, 2 = per-point rgb
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;
// Colour is constant over a point, so the colormap and the sRGB->linear conversion run in
// the VERTEX stage instead of once per covered fragment. A point sprite has a single
// vertex, so here that really is once per point (the cube fan runs its vertex stage 7
// times, so there it is 7x — still far below the fragment count it replaces).
flat out vec3 v_base_linear;
uniform mat4 u_view_model;
uniform mat4 u_proj;
uniform mat4 u_model;          // source-frame -> fixed-frame (RENDER space); only for fixed-frame axis colour
uniform int u_scalar_axis;     // -1 = colour by in_scalar; 0/1/2 = fixed-frame x/y/z
// Camera-relative render origin: u_model places points in render space (origin
// subtracted for float precision), so add it back along the coloured axis to recover
// the ABSOLUTE world coordinate the colormap range is expressed in.
uniform vec3 u_color_axis_offset;
uniform float u_range_min;
uniform float u_range_max;
uniform float u_outside_range_alpha;  // opacity for points whose scalar leaves [min,max]; 0 = culled
uniform int u_partition;  // 0=all, 1=in-range only, 2=outside only (opaque/translucent two-pass)
uniform float u_world_radius;     // metres — used when u_use_perspective_size
uniform float u_pixel_size;       // px    — used otherwise
uniform float u_viewport_height;  // pixels
uniform float u_min_size_px;
uniform float u_max_size_px;
uniform bool u_use_perspective_size;
// Piecewise perceptual decay applied ONLY under a perspective projection: depths
// <= u_depth_threshold use the physically-correct 1/depth scaling so near-field
// perspective cues remain intact; beyond the threshold, scaling falls off as
// 1/sqrt(depth * u_depth_threshold) so distant spheres stay perceptible instead of
// vanishing into a 1-pixel dot. The two branches meet continuously at
// depth = u_depth_threshold. An orthographic projection has no perspective divide.
uniform float u_depth_threshold;  // metres
out float v_outside;  // 1.0 when the point's scalar is outside [range_min, range_max]
// Spheres below kSphereDiscMaxPx draw as flat discs at this area-matched shade instead
// of per-fragment imposter shading, which is driver-dependent at that size; 0 = shade
// per fragment.
flat out float v_flat_shading;
)";

constexpr std::string_view kPointcloudVertTail = R"(
void main() {
  vec4 view_pos = u_view_model * vec4(in_pos, 1.0);
  gl_Position = u_proj * view_pos;
  if (u_use_perspective_size) {
    // Project the world-space radius to a pixel diameter:
    //   gl_PointSize == 2 * R * focal_pixels / d,
    // with focal_pixels = viewport_height * proj[1][1] / 2,
    // which simplifies to R * proj[1][1] * viewport_height / d.
    float size_px = u_world_radius * u_proj[1][1] * u_viewport_height;
    // The 1/d perspective foreshortening is correct ONLY for a perspective camera;
    // an orthographic camera has a constant world->pixel scale (d == 1). proj[3][3]
    // is 0 for perspective and 1 for orthographic — a cheap, uniform-free
    // discriminator (the same matrix-derived idiom the SSAO pass uses).
    if (u_proj[3][3] == 0.0) {
      // OpenGL view-space looks down -Z, so depth is -view_pos.z (positive forward).
      float depth = max(-view_pos.z, 0.01);
      float depth_eff = depth <= u_depth_threshold ? depth : sqrt(depth * u_depth_threshold);
      size_px /= depth_eff;
    }
    gl_PointSize = clamp(size_px, u_min_size_px, u_max_size_px);
  } else {
    gl_PointSize = clamp(u_pixel_size, u_min_size_px, u_max_size_px);
  }
  // Fixed-frame axis colouring: derive the colormap input from the GPU-transformed
  // position (u_model * in_pos) so sensors at different mounts agree on world height.
  // u_model is render-relative, so add the origin back along the axis to colour by the
  // absolute world coordinate (stable as the camera moves; matches the colormap range).
  float scalar = u_scalar_axis < 0 ? in_scalar
                                   : (u_model * vec4(in_pos, 1.0))[u_scalar_axis] + u_color_axis_offset[u_scalar_axis];
  // Cull BEFORE the colormap: a culled point should not pay for a LUT and three pow()s.
  bool outside;
  if (pointcloudCulled(scalar, u_range_min, u_range_max, u_outside_range_alpha, u_partition, outside)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 0.0;
    return;
  }
  v_outside = outside ? 1.0 : 0.0;  // drives the fragment stage's fade
  v_flat_shading = u_use_perspective_size && gl_PointSize < kSphereDiscMaxPx ? kSphereDiscShading : 0.0;
  v_base_linear = resolveBaseLinear(
      u_color_mode, u_solid_color, in_color.rgb, u_colormap_id, u_invert, scalar, u_range_min, u_range_max);
}
)";

// The colour and outside-range blocks all three programs need come from the shared
// cube_point_color.glslinc (kCubePointColorGlsl), which the QRhi point shaders include too.

// Shared colormap GLSL (turbo / viridis / plasma + sampleColormap dispatcher)
// comes from pj_widgets/Colormap.h::colormapGlsl(), so the hand-tuned polynomials
// live in ONE place shared with the 2D depth LUT (PJ::buildColormapLut). It is
// concatenated into BOTH the point/sphere and cube fragment sources at the
// makeXxxFragSrc() call sites below.

// The point/sphere fragment shader split around the colormap GLSL: head declares the
// inputs/uniforms, the LUTs slot in, then this tail's main() consumes them.
constexpr std::string_view kPointcloudFragHead = R"(#version 450 core
flat in vec3 v_base_linear;    // per-point colour, colormapped and linearized already
in float v_outside;            // 1.0 when the point's scalar is outside [min,max]
flat in float v_flat_shading;  // > 0: a sub-2-px sphere drawn as a flat disc at this shade
out vec4 frag_color;

uniform bool u_shape_is_sphere;  // sphere imposter shading vs flat point
uniform float u_outside_range_alpha;  // opacity applied where v_outside == 1.0
)";

constexpr std::string_view kPointcloudFragTail = R"(
void main() {
  // Sphere-imposter alpha discard. In kPoint mode (u_shape_is_sphere == false)
  // the whole sprite is opaque and unlit — what the user asked for.
  float shading = 1.0;
  if (u_shape_is_sphere) {
    // Treat the quad as the silhouette of a sphere and compute a per-fragment
    // normal so Lambert shading gives a 3D look. coord is the fragment
    // position within the sprite, remapped from gl_PointCoord ([0,1]) to
    // [-1,1] with the origin at the sprite center.
    vec2 coord = 2.0 * gl_PointCoord - 1.0;
    float r2 = dot(coord, coord);
    if (r2 > 1.0) {
      discard;
    }
    // Sphere normal in eye space. gl_PointCoord.y runs top-to-bottom by
    // default, so we negate the y component.
    vec3 normal = vec3(coord.x, -coord.y, sqrt(1.0 - r2));
    // Same key light as every other point-cloud solid (cube_mesh.h), so a topic keeps
    // its apparent lighting when the user switches shape.
    shading = v_flat_shading > 0.0 ? v_flat_shading : cubeLambert(normal);
  }

  // Points outside [min,max] fade to u_outside_range_alpha; drop the fully
  // transparent ones so they neither shade nor write depth.
  float alpha = v_outside > 0.5 ? u_outside_range_alpha : 1.0;
  if (alpha <= 0.003) {
    discard;
  }
  frag_color = vec4(v_base_linear * shading, alpha);
}
)";

// Cube vertex shader — the hexagon-fan geometry from cube_mesh.h. The cloud VBO is
// bound with divisor 1 at the SAME attribute locations the point shader uses, so
// both VAOs share one wiring routine; the cube corners are generated from
// gl_VertexID instead of read from a vertex buffer.
constexpr std::string_view kCubeVertHead = R"(#version 450 core
layout(location = 0) in vec3 in_instance_pos;     // per-instance, divisor=1
// Integer attribs bound with normalized=GL_FALSE are widened to float by GL,
// matching readScalarAt (signed types sign-extend).
layout(location = 1) in float in_instance_scalar; // per-instance, divisor=1
layout(location = 2) in vec4 in_instance_color;   // per-instance RGBA, divisor=1 (kRgb mode)

uniform mat4 u_model;          // source-frame -> fixed-frame (RENDER space)
uniform mat4 u_viewproj;       // proj * view, folded on the CPU
// Camera reference for choosing the 3 visible faces, in homogeneous form so one
// expression covers both projections: a perspective camera passes its eye POSITION
// in render space (w = 1) and each cube compares its own centre against it; an
// orthographic camera has no meaningful eye position — the visible faces depend on
// the view DIRECTION alone — so it passes the scene->camera direction as a point at
// infinity (w = 0), which drops the per-instance term below.
uniform vec4 u_face_pick_eye;
uniform float u_size_meters;
uniform float u_range_min;
uniform float u_range_max;
uniform float u_outside_range_alpha;  // opacity for cubes whose scalar leaves [min,max]; 0 = culled
uniform int u_partition;  // 0=all, 1=in-range only, 2=outside only (opaque/translucent two-pass)
uniform int u_scalar_axis;     // -1 = colour by in_instance_scalar; 0/1/2 = fixed-frame x/y/z
// Render origin added back along the coloured axis to recover the ABSOLUTE world
// coordinate (u_model is render-relative for float precision); see point shader.
uniform vec3 u_color_axis_offset;
uniform int  u_color_mode;     // 0 = field-from-LUT, 1 = solid, 2 = per-point rgb
uniform vec3 u_solid_color;
uniform int  u_colormap_id;    // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;

// The cube's colour is constant over the whole cube, so the colormap and the sRGB->linear
// conversion move to the vertex stage. That is 7 evaluations per cube, not one — the fan
// runs every vertex — but a covered cube spans far more fragments than that. flat:
// every vertex of an instance computes the same value, so the provoking vertex
// cannot matter (unlike the face normal, which is why THAT one is derived from
// v_local instead).
flat out vec3 v_base_linear;
// CENTROID, and it matters: the fragment stage derives the face normal from this by
// asking which component reached the cube surface. Default (pixel-centre) sampling
// extrapolates outside the triangle on partially covered MSAA pixels, which can push
// a NON-face component past 0.5 and pick the wrong face — invisible on a large cube,
// but on cubes a few pixels across nearly every pixel is a silhouette pixel, so whole
// clouds shade wrong. Centroid sampling keeps the sample inside the primitive.
centroid out vec3 v_local;  // unit-cube corner: drives both the face normal and the edge outline
out float v_outside;  // 1.0 when the cube's scalar is outside [range_min, range_max]
)";

constexpr std::string_view kCubeVertTail = R"(
void main() {
  // Cubes are axis-aligned in the fixed frame: transform the instance
  // position into fixed-frame coordinates, then add the corner offset
  // unchanged. No per-instance rotation matrix is needed.
  vec3 instance_in_fixed = (u_model * vec4(in_instance_pos, 1.0)).xyz;
  // Fixed-frame axis colouring reuses instance_in_fixed. It is render-relative, so add
  // the origin back to colour by the absolute world coordinate (stable as the camera
  // moves; matches the range).
  float scalar = u_scalar_axis < 0 ? in_instance_scalar
                                   : instance_in_fixed[u_scalar_axis] + u_color_axis_offset[u_scalar_axis];
  // Culling HERE rather than discarding per fragment is what keeps the cube FRAGMENT
  // shader discard-free, and with it the GPU's early-Z depth writes: cube clouds overdraw
  // heavily, and late-Z would shade every hidden fragment. Doing it before the corner and
  // colour work below also means a culled cube costs almost nothing — and it runs 7 times
  // per cube, so that is 7x the saving of the same move in the point shader.
  bool outside;
  if (pointcloudCulled(scalar, u_range_min, u_range_max, u_outside_range_alpha, u_partition, outside)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }
  v_outside = outside ? 1.0 : 0.0;
  vec3 to_camera = u_face_pick_eye.xyz - u_face_pick_eye.w * instance_in_fixed;
  v_local = cubeFanCorner(gl_VertexID, cubeFaceSigns(to_camera));
  gl_Position = u_viewproj * vec4(instance_in_fixed + v_local * u_size_meters, 1.0);
  v_base_linear = resolveBaseLinear(
      u_color_mode, u_solid_color, in_instance_color.rgb, u_colormap_id, u_invert, scalar, u_range_min, u_range_max);
}
)";

// The cube fragment shader split around the colormap GLSL (same shared source as
// the point shader). Head declares inputs/uniforms; tail's main() shades.
constexpr std::string_view kCubeFragHead = R"(#version 450 core
flat in vec3 v_base_linear;    // per-cube colour, colormapped and linearized already
centroid in vec3 v_local;      // must match the vertex stage — see the note there
in float v_outside;            // 1.0 when the cube's scalar is outside [min,max]
out vec4 frag_color;

uniform mat4 u_view;           // only its rotation is used, to orient the face normal
uniform float u_outside_range_alpha;  // opacity applied where v_outside == 1.0
)";

// No `discard` anywhere in this shader, by design — see the vertex-stage cull.
constexpr std::string_view kCubeFragTail = R"(
void main() {
  // View-space Lambertian against the shared key light (cube_mesh.h), which the LOD
  // sprite averages — keep them reading the same constants. The normal is recovered
  // from the interpolated corner rather than interpolated itself, which is what lets
  // the vertex stage drop its normal attribute.
  float shading = cubeLambert(cubeFaceNormalView(v_local, mat3(u_view)));

  // Darker outline on each cube's faces (same hue), so adjacent cubes stay legible.
  // The outline SCALES the colour — mix(b, b*0.4, e) == b * (1 - 0.6*e) — so it
  // commutes with the sRGB->linear conversion the vertex stage already applied:
  // pow(b*k, 2.2) == pow(b, 2.2) * pow(k, 2.2). Hoisting the colour left exactly this
  // one scalar pow per fragment in place of a colormap plus three vector pows.
  float edge_scale = 1.0 - kCubeEdgeDarken * cubeEdgeFactor(v_local);
  vec3 base = v_base_linear * pow(edge_scale, 2.2);
  // Cubes outside [min,max] fade to u_outside_range_alpha; the vertex stage already
  // culled the ones that would fade to nothing.
  float alpha = v_outside > 0.5 ? u_outside_range_alpha : 1.0;
  frag_color = vec4(base * shading, alpha);
}
)";

// Cube LOD sprite vertex shader — the cube shape's far-field form, drawn INSTEAD of the
// fan (never alongside it) once render() has established that no cube in the cloud
// reaches the threshold. It runs over the SAME cloud VBO through the point VAO, so it
// costs one vertex per point and needs no buffers, and it draws unconditionally: the
// pass either owns the whole cloud or does not run. Everything the fragment stage needs
// is constant over a sprite, so the shading collapses into one flat varying here and the
// fragment shader is a single store — no colormap, no normal, no discard.
constexpr std::string_view kCubeSpriteVertHead = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in float in_scalar;
layout(location = 2) in vec4 in_color;

uniform mat4 u_model;          // source-frame -> fixed-frame (RENDER space)
uniform mat4 u_view;           // supplies the face foreshortening AND the key-light dots
uniform mat4 u_clip_from_source;   // proj * view * model, folded on the CPU
uniform vec4 u_face_pick_eye;  // as the fan: eye position (w=1) or view direction (w=0)
uniform float u_size_px_scale; // projected pixels per cube at clip w == 1
uniform float u_range_min;
uniform float u_range_max;
uniform float u_outside_range_alpha;
uniform int u_partition;       // 0=all, 1=in-range only, 2=outside only
uniform int u_scalar_axis;
uniform vec3 u_color_axis_offset;
uniform int  u_color_mode;
uniform vec3 u_solid_color;
uniform int  u_colormap_id;
uniform bool u_invert;

flat out vec4 v_color_linear;  // rgb already shaded and linearized, a = coverage-scaled alpha
)";

constexpr std::string_view kCubeSpriteVertTail = R"(
void main() {
  gl_Position = u_clip_from_source * vec4(in_pos, 1.0);
  gl_PointSize = 0.0;
  vec3 instance_in_fixed = (u_model * vec4(in_pos, 1.0)).xyz;

  float scalar = u_scalar_axis < 0 ? in_scalar
                                   : instance_in_fixed[u_scalar_axis] + u_color_axis_offset[u_scalar_axis];
  bool outside;
  if (pointcloudCulled(scalar, u_range_min, u_range_max, u_outside_range_alpha, u_partition, outside)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }

  // A square of the cube's projected AREA. Where the driver refuses to go that small,
  // widen it and dim it by the area we overshot, so a cloud receding into the distance
  // loses brightness at the rate its cubes lose area instead of plateauing at one pixel.
  vec3 to_camera = u_face_pick_eye.xyz - u_face_pick_eye.w * instance_in_fixed;
  vec3 face_weights = cubeProjectedFaceWeights(to_camera);
  // Under perspective (w == 1) to_camera is the eye ray, so its length against the
  // view-axis depth gives the off-axis cosine. Ortho passes a direction and has no
  // off-axis tilt at all, hence the 1.
  float cos_view_angle =
      u_face_pick_eye.w > 0.5 ? clamp(gl_Position.w / max(length(to_camera), 1e-6), 1e-3, 1.0) : 1.0;
  float side_px = cubeSpriteSidePx(face_weights, u_size_px_scale / max(gl_Position.w, 1e-4), cos_view_angle);
  if (side_px <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // a zero-size cube must draw nothing, not a min-size dot
    return;
  }
  // Floor at ONE DEVICE PIXEL and draw at FULL colour, however far out the camera goes.
  // That is the same contract kPoint and kSphere already have, and what a viewer expects:
  // a distant point stays visible, in its own colour, instead of fading out or vanishing.
  // The floor is an explicit 1.0 rather than the driver's GL_POINT_SIZE_RANGE minimum,
  // which some drivers report below 1 — the guarantee here is a product requirement, not
  // whatever the hardware happens to permit.
  //
  // Dimming by the area overshoot instead would conserve the cube's total light, which is
  // exact only against a BLACK background: this pass writes alpha 1 with blending off, so
  // a heavily dimmed sprite replaces the background rather than blending into it, and a
  // zoomed-out cloud turns black on a light one. Flooring costs exactness in the other
  // direction — a clamped sprite is brighter than the cube it stands in for — but that is
  // the same, expected, floor every other shape here has, and it only begins once the
  // sprite is already sub-pixel. At the LOD threshold itself side_px is ~1.2 px, above the
  // floor, so the switch is still an exact area match and does not pop.
  gl_PointSize = max(side_px, 1.0);

  float shading = cubeSpriteShading(cubeFaceSigns(to_camera), face_weights, u_view);

  vec3 base_linear = resolveBaseLinear(
      u_color_mode, u_solid_color, in_color.rgb, u_colormap_id, u_invert, scalar, u_range_min, u_range_max);
  v_color_linear = vec4(base_linear * shading, outside ? u_outside_range_alpha : 1.0);
}
)";

constexpr std::string_view kCubeSpriteFragSrc = R"(#version 450 core
flat in vec4 v_color_linear;
out vec4 frag_color;

void main() {
  frag_color = v_color_linear;
}
)";

// Compose each fragment source once (head + shared LUTs + tail). string_view
// concatenation needs std::string; fromSources takes string_view so the static
// strings convert implicitly. function-local statics build them on first use.
// Every vertex program is head + the shared colormap LUTs + the shared colour/cull
// helpers (which call into those LUTs, so order matters) + its own main().
std::string makePointcloudVertSrc() {
  return std::string(kPointcloudVertHead) + std::string(PJ::colormapGlsl()) + std::string(kCubePointColorGlsl) +
         std::string(kPointcloudVertTail);
}

std::string makePointcloudFragSrc() {
  return std::string(kPointcloudFragHead) + std::string(kCubeLightingGlsl) + std::string(kPointcloudFragTail);
}

// The colormap LUTs now live in the VERTEX stage of both programs — the colour is
// per point, so evaluating it per fragment was work proportional to coverage.
std::string makeCubeVertSrc() {
  return std::string(kCubeVertHead) + std::string(PJ::colormapGlsl()) + std::string(kCubePointColorGlsl) +
         std::string(kCubeFanGlsl) + std::string(kCubeVertTail);
}

std::string makeCubeFragSrc() {
  return std::string(kCubeFragHead) + std::string(kCubeFaceNormalGlsl) + std::string(kCubeLightingGlsl) +
         std::string(kCubeEdgeGlsl) + std::string(kCubeFragTail);
}

std::string makeCubeSpriteVertSrc() {
  return std::string(kCubeSpriteVertHead) + std::string(PJ::colormapGlsl()) + std::string(kCubePointColorGlsl) +
         std::string(kCubeFanGlsl) + std::string(kCubeLightingGlsl) + std::string(kCubeSpriteGlsl) +
         std::string(kCubeSpriteVertTail);
}

struct CloudVertex {
  float x;
  float y;
  float z;
  float scalar;
  uint32_t rgba;  // packed per-point color (byte0=R..byte3=A); uploaded as 4 normalized bytes
};

// The CloudVertex fallback described as an AttribLayout, so the wiring below has a
// single code path for it and for the zero-copy wire buffer.
constexpr AttribLayout kFallbackLayout{/*stride=*/static_cast<uint32_t>(sizeof(CloudVertex)),
                                       /*xyz_offset=*/0U,
                                       /*scalar_offset=*/12U,
                                       /*scalar_gl_type=*/kGlFloat,
                                       /*has_scalar=*/true,
                                       /*color_offset=*/16U,
                                       /*has_color=*/true};

// Wire the currently bound cloud VBO at locations 0/1/2 (position, scalar, packed
// RGBA) into the currently bound VAO. The point and cube programs declare exactly
// these three inputs, so their VAOs differ only in `divisor`: 0 draws one vertex
// per point, 1 makes each point one instance of the cube fan.
template <typename Functions>
void bindCloudAttribs(Functions& functions, const AttribLayout& layout, GLuint divisor) {
  const auto offset_ptr = [](uint32_t offset) {
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
  };
  const auto stride = static_cast<GLsizei>(layout.stride);
  functions.glEnableVertexAttribArray(0U);
  functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, offset_ptr(layout.xyz_offset));
  functions.glVertexAttribDivisor(0U, divisor);
  if (layout.has_scalar) {
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 1, static_cast<GLenum>(layout.scalar_gl_type), GL_FALSE, stride, offset_ptr(layout.scalar_offset));
    functions.glVertexAttribDivisor(1U, divisor);
  } else {
    functions.glDisableVertexAttribArray(1U);
    functions.glVertexAttrib1f(1U, 0.0f);
  }
  if (layout.has_color) {
    // Packed RGBA straight from the buffer: 4 normalized bytes -> vec4 in [0,1]
    // (the shader paints .rgb), pixel-identical to the CPU rgba extraction.
    functions.glEnableVertexAttribArray(2U);
    functions.glVertexAttribPointer(2U, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, offset_ptr(layout.color_offset));
    functions.glVertexAttribDivisor(2U, divisor);
  } else {
    functions.glDisableVertexAttribArray(2U);
  }
}

// The layout the active cloud is bound with — the wire layout on the fast path,
// the converted CloudVertex layout otherwise.
const AttribLayout& activeLayout(
    const std::variant<std::monostate, FastCloudData, std::shared_ptr<const DecodedPointCloud>>& cloud) {
  if (const auto* fast = std::get_if<FastCloudData>(&cloud); fast != nullptr) {
    return fast->layout;
  }
  return kFallbackLayout;
}

// Out-of-range geometry draws in a SECOND pass, blended and with depth writes off, so it
// tints without occluding the crisp in-range data behind it (the opaque/translucent split
// the mesh passes use).
void beginFadedPass() {
  withGlFunctions([](auto& functions) {
    functions.glEnable(GL_BLEND);
    functions.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    functions.glDepthMask(GL_FALSE);
  });
}

void endFadedPass() {
  withGlFunctions([](auto& functions) { functions.glDepthMask(GL_TRUE); });
}

// The partition schedule every program in this pass shares: one opaque draw, then the
// faded out-of-range draw when there is one. `u_partition` selects which half the vertex
// stage keeps (0 = all, 1 = in-range only, 2 = outside only).
template <typename Draw>
void drawInPartitions(gl::Program& program, bool blend_outside, const Draw& draw) {
  program.setInt("u_partition", blend_outside ? 1 : 0);
  draw();
  if (blend_outside) {
    program.setInt("u_partition", 2);
    beginFadedPass();
    draw();
    endFadedPass();
  }
}

}  // namespace

PointcloudRenderPass::PointcloudRenderPass() = default;

PointcloudRenderPass::~PointcloudRenderPass() = default;

const std::string& PointcloudRenderPass::activeFrameId() const {
  if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
    return fast->wire.frame_id;
  }
  if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
      fallback != nullptr && *fallback) {
    return (*fallback)->frame_id;
  }
  static const std::string kEmpty;
  return kEmpty;
}

bool PointcloudRenderPass::hasRetainedCloud() const {
  if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
    return fast->point_count > 0U;
  }
  if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
      fallback != nullptr && *fallback) {
    return !(*fallback)->positions.empty();
  }
  return false;
}

void PointcloudRenderPass::releaseGL() {
  // Drop both programs and every buffer from the dying context. The CPU-side
  // retained cloud variant (fast wire anchor or fallback shared_ptr) survives;
  // initializeGL() re-sets cloud_dirty_ so render() re-uploads the point VBO
  // and re-wires the attribs in the new context. cube_instance_bindings_dirty_
  // forces the cube path to rebind too.
  program_.reset();
  cube_program_.reset();
  cube_sprite_program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  cube_vao_ = gl::VertexArray{};
  cube_ebo_ = gl::Buffer{};
  aabb_reducer_.releaseGL();  // re-probes lazily on the next dispatch in the new context
  vbo_point_count_ = 0U;
  cube_instance_bindings_dirty_ = true;
  cloud_dirty_ = hasRetainedCloud();
  initialized_ = false;
}

void PointcloudRenderPass::initializeGL() {
  // Idempotent: SceneViewWidget::paintGL calls the owning layer's
  // initializeGL() every frame (so a layer added after the widget is
  // realised initialises on its first paint). Without this guard we
  // recompiled both shader programs, re-uploaded the static cube mesh, and
  // re-flagged cloud_dirty_ (forcing a full cloud VBO re-upload) on EVERY
  // frame — an apitrace-confirmed perf bug (~3 compiles + ~3.4 buffer
  // uploads per frame, 6.9 GB trace). Mirror the guard AxisOverlayPass uses.
  if (initialized_) {
    return;
  }
  static const std::string k_pointcloud_vert_src = makePointcloudVertSrc();
  static const std::string k_pointcloud_frag_src = makePointcloudFragSrc();
  auto result = gl::Program::fromSources(k_pointcloud_vert_src, k_pointcloud_frag_src);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }

  static const std::string k_cube_vert_src = makeCubeVertSrc();
  static const std::string k_cube_frag_src = makeCubeFragSrc();
  auto cube_result = gl::Program::fromSources(k_cube_vert_src, k_cube_frag_src);
  if (auto* program = std::get_if<gl::Program>(&cube_result); program != nullptr) {
    cube_program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass cube shader error: {}\n", std::get<std::string>(cube_result));
    cube_program_.reset();
    // Sphere/point still works; cube draws fall back to sphere via shape_
    // check in render().
  }

  static const std::string k_cube_sprite_vert_src = makeCubeSpriteVertSrc();
  auto sprite_result = gl::Program::fromSources(k_cube_sprite_vert_src, kCubeSpriteFragSrc);
  if (auto* program = std::get_if<gl::Program>(&sprite_result); program != nullptr) {
    cube_sprite_program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "PointcloudRenderPass cube sprite shader error: {}\n", std::get<std::string>(sprite_result));
    // The fan alone still draws every cube correctly, just without the LOD — render()
    // withholds the split when this program is missing.
    cube_sprite_program_.reset();
  }

  // The fan cube has no vertex buffer — the corners come out of gl_VertexID — so
  // only its index buffer is uploaded, inside cube_vao_ (an element-buffer binding
  // is captured by the bound VAO). The per-instance attribs are wired lazily on the
  // first cube draw, once vbo_ has a real name.
  cube_vao_.bind();
  cube_ebo_.uploadStatic(
      GL_ELEMENT_ARRAY_BUFFER, kCubeFanIndices.data(),
      static_cast<GLsizeiptr>(kCubeFanIndices.size() * sizeof(uint16_t)));
  cube_vao_.unbind();
  cube_instance_bindings_dirty_ = true;

  withGlFunctions([](auto& functions) {
    functions.glEnable(GL_PROGRAM_POINT_SIZE);

    // A COMPATIBILITY-profile context rasterizes multisampled points as CIRCLES unless
    // point sprites are on, and a circle covers pi/4 of the square everything here
    // assumes: the LOD sprite lands 21% dim (measured 0.773 against the fan), and
    // gl_PointCoord — which the sphere imposter needs — is not even defined. Enabling
    // GL_POINT_SPRITE restores the core-profile semantics both rely on.
    //
    // The profile has to come from GL. Qt reports CoreProfile in QSurfaceFormat on
    // contexts whose GL_VERSION says Compatibility (reproducible under
    // QT_QPA_PLATFORM=offscreen), so trusting the format silently misses this. Enabling
    // it on a real core context would be GL_INVALID_ENUM, hence the gate.
    constexpr GLenum kPointSprite = 0x8861;  // compatibility-only, absent from core headers
    GLint profile_mask = 0;
    functions.glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile_mask);
    if ((static_cast<GLuint>(profile_mask) & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0U) {
      functions.glEnable(kPointSprite);
    }
  });

  initialized_ = true;
  cloud_dirty_ = true;
}

void PointcloudRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  if (!initialized_ || program_ == nullptr) {
    return;
  }

  // Drain a completed GPU AABB from a PRIOR frame's dispatch before this frame's
  // draw, so spatial_auto_bounds_ (set by the callback in spatial-axis modes) and
  // the camera see the fresh extent this frame. Non-blocking — nullopt until the
  // fence signals. Always polled (even if disabled now) so a late result drains.
  if (auto box = aabb_reducer_.poll(); box.has_value()) {
    // The reducer keeps one fence, and a dispatch always immediately follows an upload,
    // so a completed result describes the cloud currently in the VBO — as long as THIS
    // cloud dispatched one. A cloud swapped in while a PRIOR cloud's reduction was still
    // in flight leaves the flag clear, and the result that lands then belongs to a cloud
    // nobody is drawing: publishing it would hand the layer an extent for the wrong data,
    // which now feeds the frustum reject and the LOD gate as well as the camera fit. Drop
    // it and wait for this cloud's own reduction.
    if (gpu_bounds_current_) {
      geometry_bounds_ = box->valid ? box : std::nullopt;
      if (bounds_callback_) {
        bounds_callback_(*box);
      }
    }
  }

  if (cloud_dirty_) {
    if (std::holds_alternative<std::monostate>(cloud_)) {
      vbo_point_count_ = 0U;
      cloud_dirty_ = false;
      return;
    }
    if (const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
        fallback != nullptr && (!*fallback || (*fallback)->positions.empty())) {
      vbo_point_count_ = 0U;
      cloud_dirty_ = false;
      return;
    }

    if (const auto* fast = std::get_if<FastCloudData>(&cloud_); fast != nullptr) {
      const GLsizeiptr bytes = static_cast<GLsizeiptr>(fast->point_count) * fast->layout.stride;
      vbo_.uploadStatic(GL_ARRAY_BUFFER, fast->wire.data.data(), bytes);
      vao_.bind();
      withGlFunctions([&](auto& functions) { bindCloudAttribs(functions, fast->layout, 0U); });
      vao_.unbind();

      vbo_point_count_ = fast->point_count;

      // Reduce the just-uploaded geometry on the GPU (async). The result lands in
      // a later frame's poll() above. Eligibility (4-byte-aligned float32 xyz) is
      // guaranteed by the layer before it enables this.
      if (gpu_aabb_enabled_ && fast->point_count > 0U) {
        aabb_reducer_.dispatch(vbo_.id(), fast->point_count, fast->layout.stride, fast->layout.xyz_offset);
        gpu_bounds_current_ = true;
      }
    } else if (
        const auto* fallback = std::get_if<std::shared_ptr<const DecodedPointCloud>>(&cloud_);
        fallback != nullptr && *fallback) {
      const auto& decoded = **fallback;
      std::vector<CloudVertex> vertices;
      vertices.reserve(decoded.positions.size());
      const bool has_scalars = decoded.scalar.size() == decoded.positions.size();
      const bool has_colors = decoded.rgba.size() == decoded.positions.size();
      for (std::size_t i = 0U; i < decoded.positions.size(); ++i) {
        const glm::vec3& p = decoded.positions[i];
        const float scalar = has_scalars ? decoded.scalar[i] : 0.0f;
        const uint32_t color = has_colors ? decoded.rgba[i] : 0xFFFFFFFFu;  // opaque white when no color
        vertices.push_back(CloudVertex{p.x, p.y, p.z, scalar, color});
      }

      vbo_.uploadStatic(
          GL_ARRAY_BUFFER, vertices.data(), static_cast<GLsizeiptr>(vertices.size() * sizeof(CloudVertex)));
      vao_.bind();
      withGlFunctions([](auto& functions) { bindCloudAttribs(functions, kFallbackLayout, 0U); });
      vao_.unbind();

      vbo_point_count_ = decoded.positions.size();
    }
    cloud_dirty_ = false;
  }

  if (vbo_point_count_ == 0U) {
    return;
  }

  const auto transform = frame_ctx.lookup(activeFrameId());
  if (!transform.has_value()) {
    return;
  }

  // Camera-relative model (frame_ctx.lookup already subtracted render_origin in
  // double): geometry is placed in render space, so the eye→point delta survives
  // float32 even when following a frame at large world coordinates. The colour axis
  // is lifted back to absolute world below; positions stay relative.
  const glm::mat4 model = glm::mat4(transform->matrix());
  const glm::mat4 view_proj = view_params.proj * view_params.view;

  // One shared decision for every shape: skip / sprite / fan. chooseCubeDrawMode lives in
  // pj_scene3d_core so the voxel-grid pass reaches the same verdict from the same code —
  // it owns the frustum reject (against the box inflated to cover the GEOMETRY, not just
  // the point centres), the camera-inside fallback, and the LOD threshold.
  const int policy_device_height =
      view_params.device_height_px > 0 ? view_params.device_height_px : view_params.viewport_height_px;
  const float pixels_per_metre = view_params.proj[1][1] * static_cast<float>(std::max(policy_device_height, 1)) * 0.5f;
  const bool cube_shape = shape_ == Shape::kCube && cube_program_ != nullptr;

  CubeCloudView cloud_view;
  cloud_view.centre_bounds = geometry_bounds_.value_or(AABB{});
  cloud_view.clip_from_cloud = view_proj * model;
  // The policy wants the eye in the CLOUD's frame; model is render-relative, so invert it.
  if (const auto eye_render = eyeInRenderSpace(view_params); eye_render.has_value()) {
    cloud_view.eye_in_cloud = glm::vec3(glm::inverse(model) * glm::vec4(*eye_render, 1.0f));
  }
  // kPoint is sized in PIXELS, so it adds no world extent to inflate by. Its screen
  // reach stays 0 too: GL_POINTS are clipped whole at their centre, so a wider reject
  // would draw nothing more.
  cloud_view.cube_size_m = shape_ == Shape::kPoint ? 0.0f : size_meters_;
  cloud_view.pixels_per_metre = pixels_per_metre;
  cloud_view.lod_threshold_px = cube_shape ? cube_lod_threshold_px_ : 0.0f;

  // No 24-vertex solid on this path: at point-cloud cube sizes the camera cannot get
  // inside one, so kSolid degrades to the fan rather than earning a third geometry. The
  // voxel pass, whose cubes can swallow the viewpoint, is the one that owns that case.
  const CubeDrawCapabilities capabilities{/*sprite=*/cube_shape && cube_sprite_program_ != nullptr, /*fan=*/true,
                                          /*solid=*/false};
  const CubeDrawMode draw_mode = chooseCubeDrawMode(cloud_view, capabilities);
  if (draw_mode == CubeDrawMode::kSkip) {
    return;
  }

  // Outside-range opacity: points/cubes whose scalar leaves [range_min, range_max]
  // render at outside_alpha instead of clamped-opaque. Only in kField mode (solid/
  // rgb have no scalar to test); 0 fully hides them (the vertex shader culls them),
  // 1.0 (default) leaves everything opaque.
  const float outside_alpha = color_type_ == ColorType::kField ? outside_range_alpha_ : 1.0f;
  // When faded (0 < alpha < 1), the outside-range geometry draws in a SECOND pass
  // with depth writes OFF, so it blends without occluding the crisp in-range data
  // behind it (the opaque/translucent split the mesh passes use). The opaque
  // default keeps BLEND off so covered samples reset the scene FBO alpha marker to
  // "grade me" instead of inheriting alpha=0 from TF/HUD annotations drawn earlier.
  const bool blend_outside = outside_alpha > 0.0f && outside_alpha < 1.0f;
  withGlFunctions([](auto& functions) { functions.glDisable(GL_BLEND); });

  const glm::vec3 color_axis_offset(frame_ctx.render_origin);

  // Fixed-frame axis auto-range: derive the colormap [min,max] from the source
  // bounds transformed by THIS frame's model, so colour (computed per-point in the
  // shader from the same model) and range stay consistent as the TF moves. The model
  // is render-relative but the shader colours by the ABSOLUTE coordinate (it adds the
  // axis offset back), so lift the range by the same offset. Falls back to the
  // explicit range (manual, or a non-spatial field's scalar range).
  float effective_range_min = range_min_;
  float effective_range_max = range_max_;
  if (scalar_axis_ >= 0 && spatial_auto_bounds_.has_value()) {
    const auto [axis_min, axis_max] = transformedAabbAxisRange(*spatial_auto_bounds_, model, scalar_axis_);
    const float axis_offset = color_axis_offset[scalar_axis_];
    effective_range_min = axis_min + axis_offset;
    effective_range_max = axis_max + axis_offset;
  }

  const FrameDraw frame_draw{model,         view_proj,     color_axis_offset, effective_range_min, effective_range_max,
                             outside_alpha, blend_outside, draw_mode};
  if (shape_ == Shape::kCube && cube_program_ != nullptr) {
    renderCubeShape(view_params, frame_draw);
  } else {
    // The cube setter falls back here while cube_program_ is unavailable (shader
    // compile failure), which is why this is an else and not a kCube-excluding test.
    renderSpriteShape(view_params, frame_draw);
  }
  unuseProgram();
  withGlFunctions([](auto& functions) { functions.glEnable(GL_BLEND); });
}

void PointcloudRenderPass::setSharedColorUniforms(gl::Program& program, const FrameDraw& frame_draw) const {
  program.setMat4("u_model", frame_draw.model);
  program.setFloat("u_range_min", frame_draw.range_min);
  program.setFloat("u_range_max", frame_draw.range_max);
  program.setInt("u_scalar_axis", scalar_axis_);
  program.setVec3("u_color_axis_offset", frame_draw.color_axis_offset);
  program.setInt("u_color_mode", pointColorMode(color_type_));
  program.setVec3("u_solid_color", solid_color_);
  program.setInt("u_colormap_id", static_cast<int>(colormap_));
  program.setInt("u_invert", invert_lut_ ? 1 : 0);
  program.setFloat("u_outside_range_alpha", frame_draw.outside_alpha);
}

void PointcloudRenderPass::renderCubeShape(const ViewParams& view_params, const FrameDraw& frame_draw) {
  // Screen-space LOD. A cube's projected size in device pixels is
  //   size_px = size_px_scale / clip_w,
  // because clip w is the view-axis depth under perspective and 1 under ortho, which is
  // exactly the difference between the two projections' world->pixel scale. So one scale
  // factor covers both cameras with no branch.
  const int device_height =
      view_params.device_height_px > 0 ? view_params.device_height_px : view_params.viewport_height_px;
  const float size_px_scale =
      size_meters_ * view_params.proj[1][1] * static_cast<float>(std::max(device_height, 1)) * 0.5f;
  const glm::mat4 clip_from_source = frame_draw.view_proj * frame_draw.model;
  const bool draw_sprites = frame_draw.mode == CubeDrawMode::kSprite;

  if (draw_sprites) {
    cube_sprite_program_->use();
    setSharedColorUniforms(*cube_sprite_program_, frame_draw);
    cube_sprite_program_->setMat4("u_view", view_params.view);
    cube_sprite_program_->setMat4("u_clip_from_source", clip_from_source);
    cube_sprite_program_->setVec4("u_face_pick_eye", facePickReference(view_params.view, view_params.proj));
    cube_sprite_program_->setFloat("u_size_px_scale", size_px_scale);
    // The point VAO already binds this cloud at divisor 0 — one sprite per point.
    vao_.bind();
    drawInPartitions(*cube_sprite_program_, frame_draw.blend_outside, [this]() {
      withGlFunctions(
          [this](auto& functions) { functions.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vbo_point_count_)); });
    });
    vao_.unbind();
    return;
  }

  // Lazy re-wiring of the cube VAO's per-instance attribs to vbo_. The buffer ID is
  // stable across cloud swaps, but stride/offset/type can vary. Same locations and same
  // routine as the point VAO — only the divisor differs.
  if (cube_instance_bindings_dirty_) {
    cube_vao_.bind();
    vbo_.bind(GL_ARRAY_BUFFER);
    const AttribLayout& layout = activeLayout(cloud_);
    withGlFunctions([&layout](auto& functions) { bindCloudAttribs(functions, layout, 1U); });
    cube_vao_.unbind();
    cube_instance_bindings_dirty_ = false;
  }

  cube_program_->use();
  setSharedColorUniforms(*cube_program_, frame_draw);
  cube_program_->setMat4("u_view", view_params.view);
  // proj*view folded here rather than multiplied per vertex: written as `u_proj * u_view
  // * pos` in GLSL it is a mat4xmat4 the driver is not obliged to hoist, and the fan
  // still runs it 7 times per cube.
  cube_program_->setMat4("u_viewproj", frame_draw.view_proj);
  cube_program_->setVec4("u_face_pick_eye", facePickReference(view_params.view, view_params.proj));
  cube_program_->setFloat("u_size_meters", size_meters_);

  cube_vao_.bind();
  drawInPartitions(*cube_program_, frame_draw.blend_outside, [this]() {
    withGlFunctions([this](auto& functions) {
      functions.glDrawElementsInstanced(
          GL_TRIANGLES, static_cast<GLsizei>(kCubeFanIndices.size()), GL_UNSIGNED_SHORT, nullptr,
          static_cast<GLsizei>(vbo_point_count_));
    });
  });
  cube_vao_.unbind();
}

void PointcloudRenderPass::renderSpriteShape(const ViewParams& view_params, const FrameDraw& frame_draw) {
  const bool use_perspective_size = shape_ != Shape::kPoint;
  program_->use();
  setSharedColorUniforms(*program_, frame_draw);
  program_->setMat4("u_view_model", view_params.view * frame_draw.model);
  program_->setMat4("u_proj", view_params.proj);
  // size_meters_ is user-facing as the sphere DIAMETER; the shader formula is
  // parameterised on radius (gl_PointSize ~= 2*R*focal/depth). Halve here so a "0.01 m"
  // input renders as a 1 cm sphere, not a 2 cm one.
  program_->setFloat("u_world_radius", size_meters_ * 0.5f);
  program_->setFloat("u_pixel_size", size_pixels_);
  // gl_PointSize rasterizes in DEVICE (framebuffer) pixels, so the world-radius formula
  // needs the device viewport height — on HiDPI the logical height would shrink every
  // perspective-sized point by 1/DPR (M.32). Fall back to the logical height when
  // ViewParams carries no device size (hand-built callers).
  const int device_height =
      view_params.device_height_px > 0 ? view_params.device_height_px : view_params.viewport_height_px;
  program_->setFloat("u_viewport_height", static_cast<float>(std::max(device_height, 1)));
  // The min/max clamps are physical pixel sizes; scale them by DPR so 0.5/32 keep the
  // same on-screen meaning at any DPR. Derive DPR from device/logical height.
  const float dpr = std::max(
      static_cast<float>(device_height) / static_cast<float>(std::max(view_params.viewport_height_px, 1)), 1.0f);
  program_->setFloat("u_min_size_px", kMinPointPx * dpr);
  program_->setFloat("u_max_size_px", kMaxPointPx * dpr);
  program_->setFloat("u_depth_threshold", kSphereDepthThreshold);
  program_->setInt("u_use_perspective_size", use_perspective_size ? 1 : 0);
  program_->setInt("u_shape_is_sphere", shape_ != Shape::kPoint ? 1 : 0);

  vao_.bind();
  drawInPartitions(*program_, frame_draw.blend_outside, [this]() {
    withGlFunctions(
        [this](auto& functions) { functions.glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vbo_point_count_)); });
  });
  vao_.unbind();
}

void PointcloudRenderPass::setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud) {
  if (cloud) {
    cloud_ = std::move(cloud);
  } else {
    cloud_ = std::monostate{};
  }
  cloud_dirty_ = true;
  cube_instance_bindings_dirty_ = true;
  geometry_bounds_.reset();
  gpu_bounds_current_ = false;
}

void PointcloudRenderPass::setActiveFastCloud(FastCloudData cloud) {
  cloud_ = std::move(cloud);
  cloud_dirty_ = true;
  cube_instance_bindings_dirty_ = true;
  geometry_bounds_.reset();
  gpu_bounds_current_ = false;
}

void PointcloudRenderPass::setColormapRange(float min_value, float max_value) {
  range_min_ = min_value;
  range_max_ = max_value;
}

void PointcloudRenderPass::setScalarAxis(int axis) {
  scalar_axis_ = (axis >= 0 && axis <= 2) ? axis : -1;
}

void PointcloudRenderPass::setSpatialAutoBounds(std::optional<AABB> source_bounds) {
  spatial_auto_bounds_ = std::move(source_bounds);
}

void PointcloudRenderPass::setGeometryBounds(std::optional<AABB> source_bounds) {
  geometry_bounds_ = std::move(source_bounds);
}

void PointcloudRenderPass::setSizeMeters(float meters) {
  size_meters_ = std::max(0.0f, meters);
}

void PointcloudRenderPass::setSizePixels(float pixels) {
  size_pixels_ = std::max(1.0f, pixels);
}

void PointcloudRenderPass::setShape(Shape shape) {
  shape_ = shape;
}

void PointcloudRenderPass::setCubeLodThresholdPx(float pixels) {
  cube_lod_threshold_px_ = std::max(0.0f, pixels);
}

void PointcloudRenderPass::setColorType(ColorType type) {
  color_type_ = type;
}

void PointcloudRenderPass::setSolidColor(glm::vec3 rgb) {
  solid_color_ = glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f));
}

void PointcloudRenderPass::setColormap(Colormap cm) {
  colormap_ = cm;
}

void PointcloudRenderPass::setInvertLut(bool invert) {
  invert_lut_ = invert;
}

void PointcloudRenderPass::setOutsideRangeAlpha(float alpha) {
  outside_range_alpha_ = std::clamp(alpha, 0.0f, 1.0f);
}

}  // namespace pj::scene3d
