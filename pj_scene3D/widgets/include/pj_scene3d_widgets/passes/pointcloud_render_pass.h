#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_scene3d_core/camera/camera.h"     // AABB
#include "pj_scene3d_core/cube_draw_policy.h"  // CubeDrawMode
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_scene3d_widgets/passes/pointcloud_aabb_reducer.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_widgets/Colormap.h"  // shared Colormap enum + colormapGlsl()

namespace pj::scene3d {

struct DecodedPointCloud;  // forward-declare — defined in pj_scene3d_core/pointcloud.h

// Fast-path retained state: the verbatim wire cloud (its anchor keeps bytes alive
// across GL-context recreation) + the precomputed bind layout. Hold wire BY VALUE
// (copies the BufferAnchor) — never reduce to a Span, or recreation re-uploads a
// dangling view.
struct FastCloudData {
  PJ::sdk::PointCloud wire;    // frame_id / data / anchor all live here — read wire.frame_id
  std::size_t point_count{0};  // == wire.width * wire.height (size_t: no uint32 overflow)
  AttribLayout layout;
};

class PointcloudRenderPass : public IRenderPass {
 public:
  // Shape / colour source per point (layer_style.h). kCube is drawn as the
  // 3-camera-facing-faces hexagon fan (cube_mesh.h), 7 vertices and 6 triangles
  // per point rather than 24 and 12, and falls back to kSphere if the cube program
  // fails to compile. kRgb needs `DecodedPointCloud::rgba`.
  using Shape = PointShape;
  using ColorType = PointColorType;

  // Colormap selector (used only in ColorType::kField mode). The colormap set +
  // its math (CPU LUT and the in-shader GLSL) is shared with the 2D depth view
  // via pj_widgets/Colormap.h, so a scalar maps to the same colour in both views.
  using Colormap = ::PJ::Colormap;

  PointcloudRenderPass();
  ~PointcloudRenderPass() override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Replaces the cloud being rendered. Triggers VBO re-upload on next render.
  // If cloud is non-null and cloud->scalar.size() == cloud->positions.size(),
  // the scalar attribute is uploaded; otherwise scalars default to 0.
  void setActiveCloud(std::shared_ptr<const DecodedPointCloud> cloud);
  void setActiveFastCloud(FastCloudData cloud);

  // GPU AABB reduction (fast path only). When enabled, render() dispatches a
  // compute reduction of the fast-path VBO's geometric bounds after each upload
  // and, every frame, polls the prior dispatch; a completed result is delivered
  // through setBoundsCallback(). The caller (PointCloudLayer) only enables this
  // once the layout is GPU-eligible (4-byte-aligned float32 xyz) and keeps a CPU
  // scan running until gpuAabbAvailable() confirms the compute path works.
  void setGpuAabbEnabled(bool enabled) {
    gpu_aabb_enabled_ = enabled;
  }
  // Invoked from render() (GL thread) with the freshly read-back source-frame
  // AABB whenever an async reduction completes. An invalid AABB means the cloud
  // had no finite points.
  void setBoundsCallback(std::function<void(std::optional<AABB>)> callback) {
    bounds_callback_ = std::move(callback);
  }
  // True once the compute reduction has compiled+linked on this context (known
  // only after the first dispatch). Lets the layer drop its CPU scan.
  [[nodiscard]] bool gpuAabbAvailable() const {
    return aabb_reducer_.available();
  }
  // True once a dispatch has been attempted, so gpuAabbAvailable() is
  // authoritative (distinguishes "not yet probed" from "probed, unsupported").
  [[nodiscard]] bool gpuAabbProbed() const {
    return aabb_reducer_.probed();
  }

  // Range used to normalize the scalar field to [0,1] for the colormap.
  void setColormapRange(float min_value, float max_value);

  // FIXED-FRAME axis colouring. -1 (default) → colour by the uploaded per-point
  // scalar attribute. 0/1/2 → ignore the attribute and colour by the x/y/z
  // coordinate of the point AFTER the source→fixed model transform, computed on
  // the GPU in the vertex shader. This keeps colour-by-height consistent across
  // sensors at different mounts (the raw x/y/z field is sensor-local).
  void setScalarAxis(int axis);

  // Source-frame bounds that drive the auto colormap range when setScalarAxis()
  // selected a spatial axis. When engaged, render() derives the [min,max] for
  // that axis from these bounds transformed by the SAME per-frame model used to
  // place the geometry — so colour and range never disagree as the TF moves.
  // Pass std::nullopt to fall back to the explicit setColormapRange() values
  // (manual range, or a non-spatial field's scalar range).
  void setSpatialAutoBounds(std::optional<AABB> source_bounds);

  // SOURCE-frame bounds of the active cloud, used only to skip the whole draw when
  // the cloud falls outside the view frustum. Distinct from setSpatialAutoBounds()
  // (which is about colour), because the frustum test must stay available in every
  // colour mode. Both setActiveCloud overloads CLEAR this: bounds that describe the
  // previous cloud could cull the new one for the frame or two an async GPU
  // reduction takes to land, so an unknown extent must mean "draw it".
  void setGeometryBounds(std::optional<AABB> source_bounds);

  // World-coordinate size: sphere DIAMETER, or cube side length. Default 0.01 m.
  void setSizeMeters(float meters);

  // Pixel size for kPoint shape (ignored in sphere/cube). Fractional sizes are
  // honoured (gl_PointSize is a float). Clamped to >= 1 px. Default 2 px.
  void setSizePixels(float pixels);

  // Shape selector — see enum above. Default kSphere.
  void setShape(Shape shape);

  // kCube only: projected size, in device pixels, below which a cube is drawn as one
  // area- and brightness-matched point sprite instead of the 7-vertex fan. The default
  // is deliberately small — a cube that covers a couple of pixels has no visible
  // structure left to lose, and the common wide shot puts an ENTIRE cloud under the
  // threshold, where the fan draw is then skipped outright and a cube costs one vertex.
  // 0 disables the LOD and forces the fan at every size (what the A/B benchmark and the
  // equivalence tests use).
  void setCubeLodThresholdPx(float pixels);

  // Color-sourcing selector — see enum above. Default kField.
  void setColorType(ColorType type);

  // Uniform color used when ColorType == kSolid. Components in [0,1].
  // Default = white.
  void setSolidColor(glm::vec3 rgb);

  // Colormap used when ColorType == kField. Default kTurbo.
  void setColormap(Colormap cm);

  // When true, the LUT is sampled with t replaced by (1 - t) — same min/max
  // mapping, reversed color progression. Default false.
  void setInvertLut(bool invert);

  // Effective opacity (0..1) for points whose colour scalar leaves [range_min,
  // range_max], instead of clamping them opaque to the colormap ends. 1 = no
  // change; values in (0,1) blend them; 0 culls them in the vertex shader. Only
  // bites in ColorType::kField (solid/rgb have no scalar). The layer folds its
  // opacity scrubber + visibility eye into this single value. Default 1.
  void setOutsideRangeAlpha(float alpha);

  // Per-pass visibility — when false, render() is a no-op. Used by
  // SceneViewWidget to hide individual pointcloud topics without
  // destroying their GL state. Default true.
  void setVisible(bool visible) {
    visible_ = visible;
  }
  [[nodiscard]] bool isVisible() const {
    return visible_;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
 public:
  [[nodiscard]] bool cloudDirtyForTest() const {
    return cloud_dirty_;
  }
  [[nodiscard]] std::size_t vboPointCountForTest() const {
    return vbo_point_count_;
  }
  [[nodiscard]] bool cubeInstanceBindingsDirtyForTest() const {
    return cube_instance_bindings_dirty_;
  }
  [[nodiscard]] bool activeCloudIsFastForTest() const {
    return std::holds_alternative<FastCloudData>(cloud_);
  }
#endif

 private:
  // What render() derives once per frame and both shape paths then consume. Grouping it
  // keeps those paths to two arguments instead of eight, and makes it obvious that
  // nothing below recomputes any of it.
  struct FrameDraw {
    glm::mat4 model{1.0f};              // source frame -> fixed frame, in RENDER space
    glm::mat4 view_proj{1.0f};          // proj * view, folded on the CPU
    glm::vec3 color_axis_offset{0.0f};  // render origin; lifts axis colour to absolute world
    float range_min{0.0f};              // colormap range AFTER the spatial-axis auto-range
    float range_max{1.0f};
    float outside_alpha{1.0f};  // opacity for out-of-range geometry; 1 = everything opaque
    bool blend_outside{false};  // out-of-range geometry needs a second, blended, depth-read-only pass
    // What render()'s shared chooseCubeDrawMode() decided. Never kSkip — render() returns
    // on that before dispatching to a shape.
    CubeDrawMode mode{CubeDrawMode::kFan};
  };

  [[nodiscard]] const std::string& activeFrameId() const;
  [[nodiscard]] bool hasRetainedCloud() const;

  // The two shape families. Each owns its programs, VAO and draw schedule; render()
  // owns everything before the branch (upload, frustum reject, colour range).
  void renderCubeShape(const ViewParams& view_params, const FrameDraw& frame_draw);
  void renderSpriteShape(const ViewParams& view_params, const FrameDraw& frame_draw);

  // The colour/range/partition uniforms every one of the three programs declares
  // identically — set them in one place so a new program cannot silently miss one.
  void setSharedColorUniforms(gl::Program& program, const FrameDraw& frame_draw) const;

  std::variant<std::monostate, FastCloudData, std::shared_ptr<const DecodedPointCloud>> cloud_;
  bool cloud_dirty_{false};
  float range_min_{0.0f};
  float range_max_{1.0f};
  // -1 → colour by uploaded scalar; 0/1/2 → colour by fixed-frame x/y/z (GPU).
  int scalar_axis_{-1};
  // Engaged only with scalar_axis_ >= 0: source-frame bounds whose transformed
  // axis extent becomes the auto colormap range, recomputed per frame.
  std::optional<AABB> spatial_auto_bounds_;
  // Source-frame bounds for the whole-cloud frustum early-out; see setGeometryBounds.
  std::optional<AABB> geometry_bounds_;
  float size_meters_{0.01f};
  float size_pixels_{2.0f};
  bool initialized_{false};
  bool visible_{true};

  Shape shape_{Shape::kSphere};
  ColorType color_type_{ColorType::kField};
  glm::vec3 solid_color_{1.0f, 1.0f, 1.0f};
  Colormap colormap_{Colormap::kTurbo};
  bool invert_lut_{false};
  float outside_range_alpha_{1.0f};

  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  std::size_t vbo_point_count_{0};

  // GPU AABB reduction over vbo_ (fast path only) — see setGpuAabbEnabled().
  PointcloudAabbReducer aabb_reducer_;
  bool gpu_aabb_enabled_{false};
  // Set when a reduction was dispatched for the cloud now in the VBO, so a completed
  // result may be adopted as geometry_bounds_. Cleared by every cloud swap.
  bool gpu_bounds_current_{false};
  std::function<void(std::optional<AABB>)> bounds_callback_;

  // Cube path — separate program, no vertex buffer of its own (the hexagon-fan
  // corners come from gl_VertexID), just the shared index buffer. The same cloud
  // VBO (vbo_) is bound as a per-instance attribute buffer; no per-frame upload
  // changes versus the points/sphere path.
  std::unique_ptr<gl::Program> cube_program_;
  gl::VertexArray cube_vao_;
  gl::Buffer cube_ebo_;
  // The per-instance attribs in cube_vao_ reference vbo_'s stable buffer ID,
  // but the active cloud can change stride/offset/type, so re-specify the VAO
  // format whenever the retained cloud variant is swapped.
  bool cube_instance_bindings_dirty_{true};

  // Cube LOD — the sub-threshold half of the cube shape, drawn over the point VAO
  // (divisor 0, one sprite per point) so it needs no buffers of its own. The two
  // programs partition the cloud by projected size and never draw the same point.
  std::unique_ptr<gl::Program> cube_sprite_program_;
  float cube_lod_threshold_px_{kDefaultCubeLodThresholdPx};
};

}  // namespace pj::scene3d
