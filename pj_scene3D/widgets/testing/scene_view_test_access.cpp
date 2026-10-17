// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "scene_view_test_access.h"

#include <rhi/qrhi.h>

#include "../rhi/passes/grid_rhi_pass.h"
#include "../rhi/passes/gridmap_rhi_pass.h"
#include "../rhi/passes/marker_rhi_pass.h"
#include "../rhi/passes/model_rhi_pass.h"
#include "../rhi/passes/occupancy_rhi_pass.h"
#include "../rhi/passes/overlay_rhi_pass.h"
#include "../rhi/passes/point_rhi_pass.h"
#include "../rhi/passes/pose_rhi_pass.h"
#include "../rhi/passes/trail_rhi_pass.h"
#include "../rhi/passes/voxel_rhi_pass.h"
#include "../rhi/rhi_scene_pass.h"
#include "../rhi/scene_view_widget_rhi_quality_p.h"

namespace pj::scene3d {

void SceneViewTestAccess::admitFrame(SceneViewWidget& view) {
  SceneViewWidget::FrameAdmission admission;
  view.admitFrame(nullptr, admission);
  view.buildTfTriads(glm::dvec3(0.0), admission.fixed_frame);
}

bool SceneViewTestAccess::pipelinesBuiltForTarget(const SceneViewWidget& view) {
  const QRhiRenderTarget* target = view.renderTarget();
  if (target == nullptr) {
    return false;
  }
  const QRhiRenderPassDescriptor* render_pass = target->renderPassDescriptor();
  const int samples = view.sampleCount();
  return view.pipelines_->directsBuiltFor(render_pass, samples) && view.overlay_pass_->builtFor(render_pass, samples) &&
         pipelineBuiltFor(view.hdr_->present_pipeline.get(), render_pass, samples);
}

SceneViewTestAccess::PipelineCounts SceneViewTestAccess::pipelineCounts(const SceneViewWidget& view) {
  const ScenePipelineRegistry::Counts counts = view.pipelines_->counts();
  return {counts.registered, counts.directs, counts.hdr_variants};
}

QStringList SceneViewTestAccess::builtFamilies(const SceneViewWidget& view) {
  QStringList families;
  const auto note = [&families](bool ready, const char* name) {
    if (ready) {
      families << QString::fromLatin1(name);
    }
  };
  note(view.point_pass_->ready(), "points");
  note(view.trail_pass_->ready(), "trails");
  note(view.occupancy_pass_->ready(), "occupancy");
  note(view.voxel_pass_->ready(), "voxels");
  note(view.marker_pass_->ready(), "markers");
  note(view.model_pass_->ready(), "models");
  note(view.gridmap_pass_->ready(), "grid maps");
  return families;
}

bool SceneViewTestAccess::edlResourcesReady(const SceneViewWidget& view) {
  return view.edl_->target != nullptr && view.model_pass_->meshMaskReady();
}

bool SceneViewTestAccess::shadowResourcesReady(const SceneViewWidget& view) {
  return view.shadow_->texture != nullptr && view.shadow_->target != nullptr;
}

int SceneViewTestAccess::shadowMapSize(const SceneViewWidget& view) {
  return view.shadow_->mapSize();
}

bool SceneViewTestAccess::hdrResourcesReady(const SceneViewWidget& view) {
  const HdrResources& hdr = *view.hdr_;
  return hdr.target != nullptr && hdr.depth_target != nullptr && hdr.resolve_color != nullptr &&
         hdr.resolve_depth != nullptr && hdr.resolve_coverage != nullptr;
}

QSize SceneViewTestAccess::hdrRenderSize(const SceneViewWidget& view) {
  return view.hdr_->size;
}

std::uint64_t SceneViewTestAccess::hdrAllocatedBytes(const SceneViewWidget& view) {
  return view.hdr_->allocated_bytes;
}

bool SceneViewTestAccess::depthResolveActive(const SceneViewWidget& view) {
  return view.hdr_->depth_resolve && view.hdr_->state.last_active;
}

bool SceneViewTestAccess::ssaoResourcesReady(const SceneViewWidget& view) {
  const SsaoResources& ssao = *view.ssao_;
  return ssao.target != nullptr && ssao.pipeline != nullptr && ssao.bindings != nullptr &&
         view.ssao_uniform_buffer_ != nullptr;
}

void SceneViewTestAccess::setForceDepthReplay(SceneViewWidget& view, bool force) {
  if (view.force_depth_replay_for_test_ == force) {
    return;
  }
  view.force_depth_replay_for_test_ = force;
  view.releaseHdrResources();  // the chains differ in their render-pass descriptors
  view.invalidateContent();
}

void SceneViewTestAccess::discardShadowMap(SceneViewWidget& view) {
  view.discardShadowMap();
  view.invalidateContent();
}

}  // namespace pj::scene3d
