// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Scene3D view counters shared by the frame benchmarks that drive one
// (scene3d_frame_benchmark, pj_app's mixed_layout_benchmark). Apart from
// pj_bench_probes.h, whose users need not link pj_scene3d_widgets.

#include <cstdint>

#include "pj_bench_probes.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "scene_view_test_access.h"

namespace pj_bench {

/// Cumulative work of a Scene3D view: QRhi adoptions, base builds, uploads,
/// callbacks and its full renders' stage sums (ms). Every field is negative where
/// the build has no counters (no PJ_RHI_TEST_HOOKS), so a
/// difference of two snapshots is 0 there.
struct SceneViewWork {
  double adoptions = -1.0;
  double inits = -1.0;
  double uploads = -1.0;  // point-cloud uploads + marker layer rebuilds + model mesh builds
  double upload_mib = -1.0;
  double paints = -1.0;
  double renders = -1.0;  // render() calls
  double skips = -1.0;    // paints the view skipped
  double full_renders = -1.0;
  double targets_ms = -1.0;
  double admit_ms = -1.0;
  double prepare_ms = -1.0;
  double submit_ms = -1.0;
  double init_ms = -1.0;  // the base builds'

  /// The full renders' targets + admit + prepare + submit; negative without counters.
  [[nodiscard]] double renderMs() const {
    return targets_ms < 0.0 ? -1.0 : targets_ms + admit_ms + prepare_ms + submit_ms;
  }
};

inline SceneViewWork viewWork([[maybe_unused]] const pj::scene3d::SceneViewWidget& view) {
  SceneViewWork work;
#ifdef PJ_RHI_TEST_HOOKS
  const auto& t = pj::scene3d::SceneViewTestAccess::stageTimes(view);
  const auto ms = [](std::int64_t ns) { return static_cast<double>(ns) / 1.0e6; };
  work.adoptions = view.property("pjTestRhiAdoptions").toDouble();
  work.inits = static_cast<double>(t.base_builds);
  work.uploads = static_cast<double>(t.point_uploads + t.marker_rebuilds + t.model_mesh_builds);
  work.upload_mib = static_cast<double>(t.point_upload_bytes) / (1024.0 * 1024.0);
  work.paints = static_cast<double>(t.paint_events);
  work.renders = static_cast<double>(t.render_calls);
  work.skips = static_cast<double>(t.paints_skipped);
  work.full_renders = static_cast<double>(t.full_renders);
  work.targets_ms = ms(t.targets_ns);
  work.admit_ms = ms(t.admit_ns);
  work.prepare_ms = ms(t.prepare_ns);
  work.submit_ms = ms(t.submit_ns);
  work.init_ms = ms(t.base_build_ns);
#endif
  return work;
}

/// The frames `view` drew: its full renders where the build counts them, else
/// the frames `counter` (on `view`) saw finish. Qt also finishes a frame when it
/// only composites the retained texture, which the full renders leave out.
inline std::uint64_t viewRenders(
    [[maybe_unused]] const pj::scene3d::SceneViewWidget& view, [[maybe_unused]] const FrameCounter& counter) {
#ifdef PJ_RHI_TEST_HOOKS
  return static_cast<std::uint64_t>(pj::scene3d::SceneViewTestAccess::stageTimes(view).full_renders);
#else
  return counter.count();
#endif
}

}  // namespace pj_bench
