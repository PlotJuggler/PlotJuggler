#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_scene3d_core/scene_entities_render.h"
#include "pj_scene3d_widgets/gizmos/arrow_gizmo.h"
#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace pj::scene3d {

// Renders a DecodedSceneEntities batch as real 3D geometry placed by TF.
//
// Per-frame TF resolution: each interned frame in the batch is resolved ONCE via
// FrameContext::lookup (O(frames), not O(primitives)); each primitive's world
// matrix is `fixed<-frame[frame_index] * model`. Primitives whose frame can't
// resolve are skipped (renders the resolvable subset).
//
// Stateless snapshot: whatever setActive() last received is drawn; no
// accumulation / deletions / lifetime (a future consumer layer).
//
// v1 slice: CUBE only. Sphere/cylinder reuse the same instanced-solid path;
// arrow/axes/lines/triangles land next. Text is deferred (v2).
class MarkerRenderPass : public IRenderPass {
 public:
  MarkerRenderPass();
  ~MarkerRenderPass() override;

  MarkerRenderPass(const MarkerRenderPass&) = delete;
  MarkerRenderPass& operator=(const MarkerRenderPass&) = delete;

  // Swap the batch to draw. Cheap: just stores the shared_ptr; GL upload happens
  // lazily in render() (instance data depends on the live TF, so it can't be
  // baked here).
  void setActive(std::shared_ptr<const DecodedSceneEntities> markers);

  // Viewer-side display overrides; wireframe uses glPolygonMode here.
  using DisplayOverrides = MarkerDisplayOverrides;
  void setOverrides(const DisplayOverrides& overrides) {
    overrides_ = overrides;
  }
  [[nodiscard]] const DisplayOverrides& overrides() const {
    return overrides_;
  }

  // Per-topic visibility (matches PointcloudRenderPass/OccupancyGridRenderPass):
  // when false, render() is a no-op but the active batch is retained.
  void setVisible(bool visible) {
    visible_ = visible;
  }

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

 private:
  std::shared_ptr<const DecodedSceneEntities> markers_;

  // Instanced solid path, shared by cube and sphere: one Lambert program, one
  // per-instance VBO (mat4 world + vec4 color) rebuilt every render from the live
  // TF, and one static unit mesh per shape. Non-uniform scale in `world` turns the
  // unit cube into a box and the unit sphere into an ellipsoid.
  std::unique_ptr<gl::Program> solid_program_;
  gl::Buffer instance_vbo_;
  gl::VertexArray cube_vao_;
  gl::Buffer cube_vbo_;
  gl::Buffer cube_ebo_;
  int cube_index_count_ = 0;
  gl::VertexArray sphere_vao_;
  gl::Buffer sphere_vbo_;
  gl::Buffer sphere_ebo_;
  int sphere_index_count_ = 0;

  // Cube edge overlay: every box also gets its 12 edges as opaque 1px lines.
  // The shader colors front edges dark and self-occluded/rear edges with a
  // softer contrast between the face and front-edge colors. Instanced over the
  // SAME instance_vbo_ records the cube fill consumed, so the edge draw must run
  // after the cube fill and before the sphere draw re-uploads that buffer.
  std::unique_ptr<gl::Program> edge_program_;
  gl::VertexArray edge_vao_;
  gl::Buffer edge_vbo_;  // 24 GL_LINES endpoints: pos + adjacent face normals

  // Cylinder / cone / truncated-cone: its own program — the taper (per-instance
  // bottom/top radius scale) deforms the unit mesh in the vertex shader, so it
  // can't share the rigid solid path. Unit mesh: axis +Z, radius 0.5, height 1.
  std::unique_ptr<gl::Program> cyl_program_;
  gl::VertexArray cyl_vao_;
  gl::Buffer cyl_vbo_;
  gl::Buffer cyl_ebo_;
  gl::Buffer cyl_instance_vbo_;
  int cyl_index_count_ = 0;

  // Arrows + axes: ONE unit ArrowGizmo (init once); per-marker dims are baked
  // into the model matrix — never rebuild() per arrow/frame.
  ArrowGizmo marker_arrow_;

  // Lines (unlit) + triangles (Lambert): one draw per batch, not instanced —
  // the per-batch VBO is re-streamed each frame (world changes with TF).
  std::unique_ptr<gl::Program> line_program_;
  gl::VertexArray line_vao_;
  gl::Buffer line_vbo_;
  std::unique_ptr<gl::Program> tri_program_;
  gl::VertexArray tri_vao_;
  gl::Buffer tri_vbo_;

  DisplayOverrides overrides_;
  bool visible_ = true;
  bool initialized_ = false;
};

}  // namespace pj::scene3d
