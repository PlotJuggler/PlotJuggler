// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame cost of plots beside a Scene3D view in one dock manager (Gate A
// "mixed", pj_scene3D/docs/TECHNICAL_NOTES.md §9): the layout of mixed_layout_fixture.h during
// playback, where every frame moves the tracker of every plot (a replot each)
// and orbits the Scene3D camera, then waits until all of them finished a frame.
//
//   mixed_layout_benchmark [--plots 6] [--curves 4] [--samples 100000]
//                          [--cloud-side 100] [--frames 60] [--dump scene.png]
//                          [--tab-switch N]
//
// Prints one MIXED_BENCH line with the same metrics as scene3d_frame_benchmark
// (median/min/max wall per frame after a time-based warm-up, the first warm-up
// frame after idle as cold_ms, GUI-thread CPU per frame, RSS and its peak, GPU
// memory, idle frames of the settled layout: the Scene3D view's full renders
// where the build counts them, else its finished frames, plus the plot canvases'
// finished frames). The QRhi backend follows PJ_GRAPHICS_API. Needs a real
// display. Every run draws the same scene through the same camera; --dump saves
// the Scene3D view's last frame to check that.
//
// --tab-switch N instead tabs the Scene3D dock with every plot in one ADS dock
// area, shows each plot tab once, then switches between the Scene3D tab and the
// first plot's N times (setAsCurrentTab(): a real dock-area tab switch, not a
// hide/show). --plots K > 1 measures what the visited hidden tabs keep: the GPU
// and resident memory at the end (gpu_drm_mib, rss_mib) are with K + 1 tabs
// visited. It prints one MIXED_TAB line: the medians of show-Scene3D to its
// first finished frame (tab_ms, with [min, max]) and to the plot's (plot_tab_ms),
// and per switch back to Scene3D its QRhi adoptions, base builds (init_ms),
// upload work, paint events, render() calls, skipped paints and full renders, and
// the time of those renders (render_ms; builds with PJ_RHI_TEST_HOOKS; n/a
// otherwise), and the
// switches' span in CLOCK_MONOTONIC ns (window_ns; MIXED_TAB_SPANS lists each
// switch's, plot and Scene3D alternating) to cut a perf profile.

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QImage>
#include <QWindow>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "mixed_layout_fixture.h"
#include "pj_bench_probes.h"
#include "pj_bench_scene_work.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "scene_view_test_access.h"

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewTestAccess;

using pj_bench::SceneViewWork;
using pj_bench::viewWork;

// --tab-switch: shows every plot tab once, then `switches` rounds of showing
// the first plot's tab, then the Scene3D tab of one dock area, each to its
// widget's first finished frame (after the layout settled); prints MIXED_TAB.
// counters[0] is the Scene3D view's, counters[1 + i] plot_docks[i]'s.
int runTabSwitches(
    int switches, int cloud_points, ads::CDockWidget& scene_dock, pj::scene3d::SceneViewWidget& view,
    const std::vector<ads::CDockWidget*>& plot_docks,
    const std::vector<std::unique_ptr<pj_bench::FrameCounter>>& counters, const std::function<std::uint64_t()>& total) {
  ads::CDockWidget& plot_dock = *plot_docks.front();
  QByteArray spans;  // "<begin>-<end>,..." of every switch, steadyNs()
  const auto switch_to = [&](ads::CDockWidget& dock, const pj_bench::FrameCounter& counter) {
    const std::uint64_t before = counter.count();
    const std::int64_t begin_ns = pj_bench::steadyNs();
    dock.setAsCurrentTab();
    if (!pj_bench::waitUntil([&] { return counter.count() > before; })) {
      return -1.0;
    }
    const std::int64_t end_ns = pj_bench::steadyNs();
    spans += (spans.isEmpty() ? "" : ",") + QByteArray::number(begin_ns) + '-' + QByteArray::number(end_ns);
    const double ms = static_cast<double>(end_ns - begin_ns) / 1.0e6;
    (void)pj_bench::idleFrames(total, 300, 15000, 0);
    return ms;
  };
  for (std::size_t i = 0; i < plot_docks.size(); ++i) {
    if (switch_to(*plot_docks[i], *counters[1 + i]) < 0.0 || switch_to(scene_dock, *counters[0]) < 0.0) {
      std::fprintf(stderr, "a first tab visit never presented a frame\n");
      return 1;
    }
  }
  spans.clear();
  std::vector<double> scene_ms;
  std::vector<double> plot_ms;
  std::vector<double> init_ms;
  const SceneViewWork start = viewWork(view);
  SceneViewWork to_scene;  // summed over the switches back to Scene3D only
  double to_scene_render_ms = -1.0;
  const auto add = [](double& sum, double from, double to) {
    sum = from < 0.0 ? -1.0 : std::max(sum, 0.0) + to - from;
  };
  const std::int64_t window_begin_ns = pj_bench::steadyNs();
  for (int i = 0; i < switches; ++i) {
    plot_ms.push_back(switch_to(plot_dock, *counters[1]));
    const SceneViewWork before = viewWork(view);
    scene_ms.push_back(switch_to(scene_dock, *counters[0]));
    const SceneViewWork after = viewWork(view);
    add(to_scene.paints, before.paints, after.paints);
    add(to_scene.renders, before.renders, after.renders);
    add(to_scene.skips, before.skips, after.skips);
    add(to_scene.full_renders, before.full_renders, after.full_renders);
    add(to_scene_render_ms, before.renderMs(), after.renderMs());
#ifdef PJ_RHI_TEST_HOOKS
    if (viewWork(view).inits > before.inits) {
      init_ms.push_back(SceneViewTestAccess::stageTimes(view).last_base_build_ms);
    }
#else
    (void)before;
#endif
    if (scene_ms.back() < 0.0 || plot_ms.back() < 0.0) {
      std::fprintf(stderr, "switch %d never presented a frame\n", i);
      return 1;
    }
  }
  const std::int64_t window_end_ns = pj_bench::steadyNs();
  const SceneViewWork end = viewWork(view);
  const auto per_switch = [switches](double from, double to) {
    return from < 0.0 ? QByteArray("n/a") : QByteArray::number((to - from) / switches, 'f', 1);
  };
  const auto per_scene_switch = [&per_switch](double sum) { return per_switch(sum < 0.0 ? -1.0 : 0.0, sum); };
  const pj_bench::Stats scene = pj_bench::stats(scene_ms);
  std::printf(
      "MIXED_TAB build=%s backend=%s switches=%d tabs=%zu cloud_points=%d tab_ms=%.2f tab_min=%.2f tab_max=%.2f "
      "plot_tab_ms=%.2f adoptions=%s inits=%s init_ms=%s uploads=%s upload_mib=%s paints=%s renders=%s skips=%s "
      "full_renders=%s render_ms=%s rss_mib=%s gpu_drm_mib=%s window_ns=%lld-%lld\n",
      "rhi", qPrintable(view.rhiBackendName()), switches, plot_docks.size() + 1, cloud_points, scene.median, scene.min,
      scene.max, pj_bench::stats(plot_ms).median, per_switch(start.adoptions, end.adoptions).constData(),
      per_switch(start.inits, end.inits).constData(),
      init_ms.empty() ? "n/a" : QByteArray::number(pj_bench::stats(init_ms).median, 'f', 2).constData(),
      per_switch(start.uploads, end.uploads).constData(), per_switch(start.upload_mib, end.upload_mib).constData(),
      per_scene_switch(to_scene.paints).constData(), per_scene_switch(to_scene.renders).constData(),
      per_scene_switch(to_scene.skips).constData(), per_scene_switch(to_scene.full_renders).constData(),
      per_scene_switch(to_scene_render_ms).constData(), pj_bench::fmt(pj_bench::rssMib()).constData(),
      pj_bench::fmt(pj_bench::drmTotalMib()).constData(), static_cast<long long>(window_begin_ns),
      static_cast<long long>(window_end_ns));
  std::printf("MIXED_TAB_SPANS %s\n", spans.constData());  // plot and Scene3D switches, alternating
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({u"plots"_s, u"plot docks"_s, u"n"_s, u"6"_s});
  parser.addOption({u"curves"_s, u"curves per plot"_s, u"n"_s, u"4"_s});
  parser.addOption({u"samples"_s, u"samples per curve"_s, u"n"_s, u"100000"_s});
  parser.addOption({u"cloud-side"_s, u"Scene3D cloud: side^3 points"_s, u"n"_s, u"100"_s});
  parser.addOption({u"frames"_s, u"measured frames"_s, u"n"_s, u"60"_s});
  parser.addOption({u"dump"_s, u"save the Scene3D view's last frame"_s, u"png"_s});
  parser.addOption({u"tab-switch"_s, u"switch a Scene3D/plot tab pair N times"_s, u"n"_s, u"0"_s});
  parser.process(app);
  const int plot_count = parser.value(u"plots"_s).toInt();
  const int curves = parser.value(u"curves"_s).toInt();
  const int samples = parser.value(u"samples"_s).toInt();
  const int cloud_side = parser.value(u"cloud-side"_s).toInt();
  const int frames = parser.value(u"frames"_s).toInt();
  const int tab_switches = parser.value(u"tab-switch"_s).toInt();

  pj_mixed_layout::MixedLayout layout;
  std::vector<PJ::PlotWidget*> plots;
  std::vector<ads::CDockWidget*> plot_docks;
  for (int i = 0; i < std::max(plot_count, tab_switches > 0 ? 1 : 0); ++i) {
    plots.push_back(layout.createPlot(curves, samples));
    plot_docks.push_back(layout.addDock(
        u"plot_%1"_s.arg(i), plots.back(), i % 2 == 0 ? ads::LeftDockWidgetArea : ads::TopDockWidgetArea));
  }
  PJ::Scene3DDockWidget* scene = layout.createScene3D(cloud_side);
  ads::CDockWidget* scene_dock = layout.addDock(u"scene3d"_s, scene, ads::RightDockWidgetArea);
  if (tab_switches > 0) {
    for (std::size_t i = 1; i < plot_docks.size(); ++i) {
      layout.manager().addDockWidgetTabToArea(plot_docks[i], plot_docks.front()->dockAreaWidget());
    }
    layout.manager().addDockWidgetTabToArea(scene_dock, plot_docks.front()->dockAreaWidget());
    scene_dock->setAsCurrentTab();
  }
  pj::scene3d::SceneViewWidget* view = scene->sceneView();

  std::vector<std::unique_ptr<pj_bench::FrameCounter>> counters;
  counters.push_back(std::make_unique<pj_bench::FrameCounter>(view));
  for (PJ::PlotWidget* plot : plots) {
    std::unique_ptr<pj_bench::FrameCounter> counter;
    for (QWidget* child : plot->findChildren<QWidget*>()) {  // the canvas: the plot's one GPU widget
      counter = std::make_unique<pj_bench::FrameCounter>(child);
      if (counter->valid()) {
        break;
      }
    }
    if (counter == nullptr || !counter->valid()) {
      std::fprintf(stderr, "a plot has no GPU canvas\n");
      return 1;
    }
    counters.push_back(std::move(counter));
  }
  // --tab-switch: every plot (counters[1..]) starts hidden behind the Scene3D tab.
  const std::size_t shown_counters = tab_switches > 0 ? 1 : counters.size();
  const auto all_finished_since = [&counters, shown_counters](const std::vector<std::uint64_t>& before) {
    for (std::size_t i = 0; i < shown_counters; ++i) {
      if (counters[i]->count() <= before[i]) {
        return false;
      }
    }
    return true;
  };
  const auto snapshot = [&counters] {
    std::vector<std::uint64_t> counts;
    for (const auto& counter : counters) {
      counts.push_back(counter->count());
    }
    return counts;
  };
  const auto total = [&counters] {
    std::uint64_t sum = 0;
    for (const auto& counter : counters) {
      sum += counter->count();
    }
    return sum;
  };

  // The playhead on the cloud's sample (t = 100 ns), as a loaded file would have it.
  scene->onTrackerTime(100.0e-9);
  layout.window().resize(1260, 780);
  pj_bench::resetPeakRss();
  QElapsedTimer show_timer;
  show_timer.start();
  layout.window().show();
  const std::vector<std::uint64_t> none(counters.size(), 0);
  if (!pj_bench::waitUntil([&] { return all_finished_since(none); })) {
    std::fprintf(stderr, "the layout never presented a frame in every widget\n");
    return 1;
  }
  const double first_show_ms = static_cast<double>(show_timer.nsecsElapsed()) / 1.0e6;
  (void)pj_bench::idleFrames(total, 1000, 15000, 0);  // decodes, uploads

  // One fixed camera: the [-0.5, 0.5]^3 cloud fills about half the view height.
  pj::scene3d::CameraState camera = view->camera().state();
  camera.focal = {0.0F, 0.0F, 0.0F};
  camera.radius = 3.0F;
  camera.azimuth = glm::radians(-60.0F);
  camera.elevation = glm::radians(35.0F);
  const float azimuth0 = camera.azimuth;
  if (tab_switches > 0) {
    // adoptState() does not notify the view: render the adopted camera before
    // settling, or the QRhi view's Shows keep presenting the fitted one.
    const std::uint64_t renders = pj_bench::viewRenders(*view, *counters.front());
    view->camera().adoptState(camera);
    view->invalidateContent();
    if (!pj_bench::waitUntil([&] { return pj_bench::viewRenders(*view, *counters.front()) > renders; })) {
      std::fprintf(stderr, "the adopted camera was never rendered\n");
      return 1;
    }
    (void)pj_bench::idleFrames(total, 300, 15000, 0);
    return runTabSwitches(
        tab_switches, cloud_side * cloud_side * cloud_side, *scene_dock, *view, plot_docks, counters, total);
  }

  // Frame k: the tracker and camera at step k; the warm-up (pj_bench::WarmUp)
  // alternates k = -1 and -2, so the last frame does not depend on its length.
  const auto drive_frame = [&](int k) {
    const std::vector<std::uint64_t> before = snapshot();
    QElapsedTimer timer;
    timer.start();
    const double tracker_s = 1.0 + 0.1 * static_cast<double>((k * 3) % samples);  // 30 Hz playback at 10x
    for (PJ::PlotWidget* plot : plots) {
      plot->setTrackerPosition(tracker_s);
    }
    camera.azimuth = azimuth0 + 0.02F * static_cast<float>(k + 1);
    view->camera().adoptState(camera);
    view->invalidateContent();
    if (!pj_bench::waitUntil([&] { return all_finished_since(before); })) {
      std::fprintf(stderr, "frame %d never finished in every widget\n", k);
      return -1.0;
    }
    return static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
  };
  pj_bench::WarmUp warm_up;
  for (int w = 0;; ++w) {
    const double ms = drive_frame(-1 - w % 2);
    if (ms < 0.0) {
      return 1;
    }
    if (warm_up.done(ms)) {
      break;
    }
  }
  std::vector<double> wall;
  const double cpu_start = pj_bench::threadCpuMs();
  for (int k = 0; k < frames; ++k) {
    const double ms = drive_frame(k);
    if (ms < 0.0) {
      return 1;
    }
    wall.push_back(ms);
  }
  const double cpu_ms = frames > 0 ? (pj_bench::threadCpuMs() - cpu_start) / frames : 0.0;
  // The plots' finished frames and the Scene3D view's own renders.
  const auto rendered = [&] {
    return total() - counters.front()->count() + pj_bench::viewRenders(*view, *counters.front());
  };
  const std::uint64_t idle_frames = pj_bench::idleFrames(rendered);
  if (const QString dump = parser.value(u"dump"_s); !dump.isEmpty() && !view->grabFramebuffer().save(dump)) {
    std::fprintf(stderr, "could not save %s\n", qPrintable(dump));
    return 1;
  }

  const char* build = "rhi";
  QRhi* rhi = pj_bench::rhiOf(*view);
  const QString backend = view->rhiBackendName();
  const QString device = rhi != nullptr ? QString::fromUtf8(rhi->driverInfo().deviceName) : QString();
  const double vma = pj_bench::rhiAllocatorMib(rhi);
  const pj_bench::Stats stats = pj_bench::stats(wall);
  std::printf(
      "MIXED_BENCH build=%s backend=%s device=\"%s\" plots=%d curves=%d samples=%d cloud_points=%d frames=%d "
      "wall_ms=%.2f wall_min=%.2f wall_max=%.2f cpu_ms=%.2f rss_mib=%s hwm_mib=%s gpu_drm_mib=%s gpu_vma_mib=%s "
      "idle_frames=%llu first_show_ms=%.1f cold_ms=%.2f warmup_frames=%d gpu_mhz=%s\n",
      build, qPrintable(backend), qPrintable(device), plot_count, curves, samples, cloud_side * cloud_side * cloud_side,
      frames, stats.median, stats.min, stats.max, cpu_ms, pj_bench::fmt(pj_bench::rssMib()).constData(),
      pj_bench::fmt(pj_bench::peakRssMib()).constData(), pj_bench::fmt(pj_bench::drmTotalMib()).constData(),
      pj_bench::fmt(vma).constData(), static_cast<unsigned long long>(idle_frames), first_show_ms, warm_up.coldMs(),
      warm_up.frames(), pj_bench::fmt(warm_up.gpuMhzAtEnd(), 0).constData());
  return 0;
}
