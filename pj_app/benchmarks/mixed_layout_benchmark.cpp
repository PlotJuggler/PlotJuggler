// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame cost of plots beside a Scene3D view in one dock manager, in both builds
// (P5 budgets, Gate A "mixed"): the layout of mixed_layout_fixture.h during
// playback, where every frame moves the tracker of every plot (a replot each)
// and orbits the Scene3D camera, then waits until all of them finished a frame.
//
//   mixed_layout_benchmark [--plots 6] [--curves 4] [--samples 100000]
//                          [--cloud-side 100] [--frames 60] [--dump scene.png]
//
// Prints one MIXED_BENCH line with the same metrics as scene3d_frame_benchmark
// (median/min/max wall per frame, GUI-thread CPU per frame, RSS and its peak,
// GPU memory, idle frames of the settled layout). The QRhi backend follows
// PJ_GRAPHICS_API. Needs a real display. Both builds draw the same scene through
// the same camera; --dump saves the Scene3D view's last frame to check that.

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QImage>
#include <QWindow>
#include <cstdio>
#include <memory>
#include <vector>

#include "mixed_layout_fixture.h"
#include "pj_bench_probes.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"

#ifndef PJ_SCENE3D_RHI
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#endif

using namespace Qt::StringLiterals;

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
  parser.process(app);
  const int plot_count = parser.value(u"plots"_s).toInt();
  const int curves = parser.value(u"curves"_s).toInt();
  const int samples = parser.value(u"samples"_s).toInt();
  const int cloud_side = parser.value(u"cloud-side"_s).toInt();
  const int frames = parser.value(u"frames"_s).toInt();

  pj_mixed_layout::MixedLayout layout;
  std::vector<PJ::PlotWidget*> plots;
  for (int i = 0; i < plot_count; ++i) {
    plots.push_back(layout.createPlot(curves, samples));
    layout.addDock(u"plot_%1"_s.arg(i), plots.back(), i % 2 == 0 ? ads::LeftDockWidgetArea : ads::TopDockWidgetArea);
  }
  PJ::Scene3DDockWidget* scene = layout.createScene3D(cloud_side);
  layout.addDock(u"scene3d"_s, scene, ads::RightDockWidgetArea);
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
  const auto all_finished_since = [&counters](const std::vector<std::uint64_t>& before) {
    for (std::size_t i = 0; i < counters.size(); ++i) {
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
  (void)pj_bench::idleFrames(total, 1000, 15000, 0);  // decodes, uploads, the view's one-shot fit

  // One camera for both builds, replacing the QRhi view's one-shot fit (the GL
  // view does not fit): the [-0.5, 0.5]^3 cloud fills about half the view height.
  pj::scene3d::CameraState camera = view->camera().state();
  camera.focal = {0.0F, 0.0F, 0.0F};
  camera.radius = 3.0F;
  camera.azimuth = glm::radians(-60.0F);
  camera.elevation = glm::radians(35.0F);
  constexpr int kWarmup = 3;
  std::vector<double> wall;
  double cpu_start = 0.0;
  for (int k = 0; k < kWarmup + frames; ++k) {
    if (k == kWarmup) {
      cpu_start = pj_bench::threadCpuMs();
    }
    const std::vector<std::uint64_t> before = snapshot();
    QElapsedTimer timer;
    timer.start();
    const double tracker_s = 1.0 + 0.1 * static_cast<double>((k * 3) % samples);  // 30 Hz playback at 10x
    for (PJ::PlotWidget* plot : plots) {
      plot->setTrackerPosition(tracker_s);
    }
    camera.azimuth += 0.02F;
    view->camera().adoptState(camera);
    view->update();
    if (!pj_bench::waitUntil([&] { return all_finished_since(before); })) {
      std::fprintf(stderr, "frame %d never finished in every widget\n", k);
      return 1;
    }
    if (k >= kWarmup) {
      wall.push_back(static_cast<double>(timer.nsecsElapsed()) / 1.0e6);
    }
  }
  const double cpu_ms = frames > 0 ? (pj_bench::threadCpuMs() - cpu_start) / frames : 0.0;
  const std::uint64_t idle_frames = pj_bench::idleFrames(total);
  if (const QString dump = parser.value(u"dump"_s); !dump.isEmpty() && !view->grabFramebuffer().save(dump)) {
    std::fprintf(stderr, "could not save %s\n", qPrintable(dump));
    return 1;
  }

#ifdef PJ_SCENE3D_RHI
  const char* build = "rhi";
  QRhi* rhi = pj_bench::rhiOf(*view);
  const QString backend = view->rhiBackendNameForTest();
  const QString device = rhi != nullptr ? QString::fromUtf8(rhi->driverInfo().deviceName) : QString();
  const double vma = pj_bench::rhiAllocatorMib(rhi);
#else
  const char* build = "gl";
  const QString backend = u"OpenGL"_s;
  QString device;
  view->makeCurrent();
  if (QOpenGLContext* context = view->context(); context != nullptr) {
    device = QString::fromLatin1(reinterpret_cast<const char*>(context->functions()->glGetString(GL_RENDERER)));
  }
  view->doneCurrent();
  const double vma = -1.0;
#endif
  const pj_bench::Stats stats = pj_bench::stats(wall);
  std::printf(
      "MIXED_BENCH build=%s backend=%s device=\"%s\" plots=%d curves=%d samples=%d cloud_points=%d frames=%d "
      "wall_ms=%.2f wall_min=%.2f wall_max=%.2f cpu_ms=%.2f rss_mib=%s hwm_mib=%s gpu_drm_mib=%s gpu_vma_mib=%s "
      "idle_frames=%llu first_show_ms=%.1f\n",
      build, qPrintable(backend), qPrintable(device), plot_count, curves, samples, cloud_side * cloud_side * cloud_side,
      frames, stats.median, stats.min, stats.max, cpu_ms, pj_bench::fmt(pj_bench::rssMib()).constData(),
      pj_bench::fmt(pj_bench::peakRssMib()).constData(), pj_bench::fmt(pj_bench::drmTotalMib()).constData(),
      pj_bench::fmt(vma).constData(), static_cast<unsigned long long>(idle_frames), first_show_ms);
  return 0;
}
