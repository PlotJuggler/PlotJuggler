// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame cost of plot canvases under the plot workloads that matter:
//
//   plot_canvas_benchmark --canvas rhi|raster --case NAME [--frames N]
//       [--samples N] [--plots K] [--grid] [--legend] [--msaa N] [--gpu]
//       [--generic-series]
//
// Cases: static_1m, static_10m (one curve, replotted every frame), streaming
// (10 curves x 100k samples, 2 samples appended per curve per frame with a
// sliding 30 s window: 100 Hz at a 50 Hz frame rate), zoom_pan (1M samples, the
// view moves every frame), many_curves (50 x 100k), dashed (5 x 100k,
// Qt::DashLine), tracker_only (10 x 100k, static; only a CurveTracker moves and
// the plot replots its overlays, as PlotWidget::setTrackerPosition does during
// playback). Every frame replots and waits for every canvas to finish a frame.
// --samples overrides the samples per curve; --plots K lays out K such plots in
// the 1600x900 window; --grid and --legend add PlotWidgetBase's grid (dashed
// major, dotted minor, cosmetic pens) and an in-canvas legend. --msaa N sets
// the RHI canvas's sample count (default: the canvas's own, 1). The series are
// RevisionedSeries like the app's datastore adapters (an unchanged replot reuses
// the RHI canvas's curve layer); --generic-series makes them plain Qwt data.
//
// Reported per run (medians after a time-based warm-up, pj_bench::WarmUp, whose
// first frame is cold_ms; summed over the plots): canvas CPU
// time (PlotRhiCanvas's render() or QwtPlotCanvas::paintEvent), GPU time (--gpu
// only: QRhi timestamps through QSG_RHI_PROFILE; costs CPU), the
// wall time from replot to the finished frames (includes any compositor vsync
// wait), the RHI canvases' vertices of the last build, plus the GPU memory the
// DRM driver charges to the process at the end of the run (Linux fdinfo
// drm-total, any canvas and API; n/a on software rasterizers); pass
// QT_SCALE_FACTOR=2 for DPR 2. The QRhi backend follows PJ_GRAPHICS_API. Needs a
// display; run without LIBGL_ALWAYS_SOFTWARE for the real GPU. Built with
// PJ_BUILD_DEMOS.

#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_series_data.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QPen>
#include <QSurfaceFormat>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "pj_bench_probes.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotRhiCanvas.h"
#include "pj_plotting/RevisionedSeries.h"

using namespace Qt::StringLiterals;

namespace {

// Growable series; the bounding rect is not used (the axes are fixed). A
// RevisionedSeries, as the app's datastore adapters are: an append bumps its
// revision. With --generic-series the canvas sees plain Qwt data instead.
class VectorSeries : public QwtSeriesData<QPointF> {
 public:
  explicit VectorSeries(std::vector<QPointF> points) : points_(std::move(points)) {}
  [[nodiscard]] size_t size() const override {
    return points_.size();
  }
  [[nodiscard]] QPointF sample(size_t index) const override {
    return points_[index];
  }
  [[nodiscard]] QRectF boundingRect() const override {
    return {0.0, -2.0, points_.empty() ? 1.0 : points_.back().x(), 4.0};
  }
  virtual void append(QPointF point) {
    points_.push_back(point);
  }

 private:
  std::vector<QPointF> points_;
};

class RevisionedVectorSeries final : public VectorSeries, public PJ::RevisionedSeries {
 public:
  using VectorSeries::VectorSeries;
  void append(QPointF point) override {
    VectorSeries::append(point);
    bumpContentRevision();
  }
};

// Noisy sine at `rate` Hz starting at t = 0.
std::vector<QPointF> makeSignal(std::size_t count, double rate, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, 0.1);
  std::vector<QPointF> points(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / rate;
    points[i] = QPointF(t, std::sin(t * 0.7 + seed) + noise(rng));
  }
  return points;
}

class TimedRasterCanvas final : public QwtPlotCanvas {
 public:
  using QwtPlotCanvas::QwtPlotCanvas;
  double cpu_ms = 0.0;

 protected:
  void paintEvent(QPaintEvent* event) override {
    QElapsedTimer timer;
    timer.start();
    QwtPlotCanvas::paintEvent(event);
    cpu_ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    ++painted;
  }

 public:
  int painted = 0;
};

}  // namespace

// One benchmarked plot: its canvas (exactly one of the two is set), series
// and optional tracker.
struct BenchPlot {
  QwtPlot* plot = nullptr;
  PJ::PlotRhiCanvas* rhi = nullptr;
  TimedRasterCanvas* raster = nullptr;
  std::vector<VectorSeries*> series;
  std::unique_ptr<PJ::CurveTracker> tracker;
  int presented = 0;

  [[nodiscard]] int framesDone() const {
    return raster != nullptr ? raster->painted : presented;
  }
  [[nodiscard]] double cpuMs() const {
    return rhi != nullptr ? rhi->lastRenderCpuMs() : raster->cpu_ms;
  }
  [[nodiscard]] double gpuMs() const {
    return rhi != nullptr ? rhi->lastGpuFrameMs() : 0.0;
  }
};

int main(int argc, char** argv) {
  const bool gpu_timing =
      std::any_of(argv + 1, argv + argc, [](const char* arg) { return qstrcmp(arg, "--gpu") == 0; });
  if (gpu_timing) {
    qputenv("QSG_RHI_PROFILE", "1");  // QRhi timestamps for the widget compositor
  }
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setSwapInterval(0);
  QSurfaceFormat::setDefaultFormat(format);
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({u"canvas"_s, u"rhi or raster"_s, u"canvas"_s, u"rhi"_s});
  parser.addOption({u"case"_s, u"workload"_s, u"case"_s, u"static_1m"_s});
  parser.addOption({u"frames"_s, u"measured frames"_s, u"frames"_s, u"60"_s});
  parser.addOption({u"samples"_s, u"samples per curve (default: the case's)"_s, u"samples"_s});
  parser.addOption({u"plots"_s, u"plots in the window"_s, u"plots"_s, u"1"_s});
  parser.addOption({u"grid"_s, u"draw PlotWidgetBase's grid"_s});
  parser.addOption({u"legend"_s, u"draw an in-canvas legend"_s});
  parser.addOption({u"msaa"_s, u"RHI canvas sample count"_s, u"samples"_s});
  parser.addOption({u"gpu"_s, u"measure GPU time (timestamp queries)"_s});
  parser.addOption({u"generic-series"_s, u"plain Qwt series data (no content revision)"_s});
  parser.process(app);
  const QString canvas_kind = parser.value(u"canvas"_s);
  if (canvas_kind != u"rhi"_s && canvas_kind != u"raster"_s) {
    std::fprintf(stderr, "unknown --canvas %s (rhi or raster)\n", qPrintable(canvas_kind));
    return 2;
  }
  const QString workload = parser.value(u"case"_s);
  const int frames = parser.value(u"frames"_s).toInt();
  const int plot_count = std::max(1, parser.value(u"plots"_s).toInt());
  const bool generic_series = parser.isSet(u"generic-series"_s);

  std::size_t curve_count = 1;
  std::size_t samples = 1'000'000;
  bool dashed = false;
  bool streaming = false;
  bool zoom_pan = false;
  bool tracker_only = false;
  if (workload == u"static_10m"_s) {
    samples = 10'000'000;
  } else if (workload == u"streaming"_s) {
    curve_count = 10;
    samples = 100'000;
    streaming = true;
  } else if (workload == u"zoom_pan"_s) {
    zoom_pan = true;
  } else if (workload == u"many_curves"_s) {
    curve_count = 50;
    samples = 100'000;
  } else if (workload == u"dashed"_s) {
    curve_count = 5;
    samples = 100'000;
    dashed = true;
  } else if (workload == u"tracker_only"_s) {
    curve_count = 10;
    samples = 100'000;
    tracker_only = true;
  } else if (workload != u"static_1m"_s) {
    std::fprintf(stderr, "unknown case %s\n", qPrintable(workload));
    return 2;
  }
  if (parser.isSet(u"samples"_s)) {
    samples = parser.value(u"samples"_s).toULongLong();
  }
  constexpr double kRate = 100.0;  // samples per second of plot time
  const double duration = static_cast<double>(samples) / kRate;

  QWidget window;
  auto* layout = new QGridLayout(&window);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(plot_count))));
  std::vector<std::unique_ptr<BenchPlot>> plots;
  for (int plot_index = 0; plot_index < plot_count; ++plot_index) {
    auto bench = std::make_unique<BenchPlot>();
    bench->plot = new QwtPlot();
    QwtPlot& plot = *bench->plot;
    plot.setAutoReplot(false);
    if (canvas_kind == u"rhi"_s) {
      bench->rhi = new PJ::PlotRhiCanvas();
      if (parser.isSet(u"msaa"_s)) {
        bench->rhi->setSampleCount(parser.value(u"msaa"_s).toInt());
      }
      plot.setCanvas(bench->rhi);
      bench->rhi->setPlot(&plot);
    } else {
      bench->raster = new TimedRasterCanvas();
      bench->raster->setPaintAttribute(QwtPlotCanvas::BackingStore, false);
      plot.setCanvas(bench->raster);
    }
    plot.setCanvasBackground(QColor(30, 30, 34));
    if (parser.isSet(u"grid"_s)) {
      auto* grid = new QwtPlotGrid();
      grid->setMajorPen(QPen(QColor(90, 90, 100), 0.0, Qt::DashLine));
      grid->setMinorPen(QPen(QColor(90, 90, 100), 0.0, Qt::DotLine));
      grid->enableXMin(true);
      grid->enableYMin(true);
      grid->attach(&plot);
    }
    if (parser.isSet(u"legend"_s)) {
      auto* legend = new PJ::PlotLegend(&plot);
      legend->setVisible(true);
    }
    for (std::size_t index = 0; index < curve_count; ++index) {
      auto* curve = new PJ::PlotCurve(QString::number(index));
      curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
      curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
      curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);
      std::vector<QPointF> signal = makeSignal(samples, kRate, static_cast<unsigned>(index));
      VectorSeries* data =
          generic_series ? new VectorSeries(std::move(signal)) : new RevisionedVectorSeries(std::move(signal));
      bench->series.push_back(data);
      curve->setData(data);
      curve->setPen(
          QColor::fromHsv(static_cast<int>(index * 47 % 360), 200, 230), 1.4, dashed ? Qt::DashLine : Qt::SolidLine);
      curve->attach(&plot);
    }
    plot.setAxisScale(QwtPlot::yLeft, -2.5, 2.5);
    plot.setAxisScale(QwtPlot::xBottom, 0.0, duration);
    if (tracker_only) {
      bench->tracker = std::make_unique<PJ::CurveTracker>(&plot, QColor(255, 180, 0));
    }
    // Frames the canvas actually finished (submitted or painted).
    BenchPlot* raw = bench.get();
    if (bench->rhi != nullptr) {
      QObject::connect(bench->rhi, &QRhiWidget::frameSubmitted, [raw]() { ++raw->presented; });
    }
    layout->addWidget(bench->plot, plot_index / columns, plot_index % columns);
    plots.push_back(std::move(bench));
  }
  window.resize(1600, 900);
  window.show();
  if (!QTest::qWaitForWindowExposed(&window)) {
    std::fprintf(stderr, "window not exposed\n");
    return 1;
  }

  std::vector<double> cpu;
  std::vector<double> gpu;
  std::vector<double> wall;
  double now = duration;
  // A time-based warm-up (pj_bench::WarmUp); its first frame, after the idle
  // since the window was exposed, is the cold-after-idle latency (cold_ms).
  pj_bench::WarmUp warm_up;
  bool warming = true;
  int measured = 0;
  for (int frame = 0; measured < frames; ++frame) {
    QElapsedTimer timer;
    timer.start();
    std::vector<int> before;
    for (auto& bench : plots) {
      QwtPlot& plot = *bench->plot;
      before.push_back(bench->framesDone());
      if (streaming) {
        for (VectorSeries* data : bench->series) {
          for (int k = 0; k < 2; ++k) {
            data->append(QPointF(now + k / kRate, std::sin(now)));
          }
        }
        plot.setAxisScale(QwtPlot::xBottom, now - 30.0, now);
      } else if (zoom_pan) {
        const double span = duration * (0.2 + 0.15 * std::sin(frame * 0.1));
        const double left = (duration - span) * (0.5 + 0.5 * std::sin(frame * 0.05));
        plot.setAxisScale(QwtPlot::xBottom, left, left + span);
      }
      if (tracker_only) {
        // As PlotWidget::setTrackerPosition: move the tracker, then replot the
        // overlays (PlotWidgetBase::replotOverlays()).
        bench->tracker->setPosition(QPointF(duration * (0.1 + 0.8 * (frame % 100) / 100.0), 0.0));
        if (bench->rhi != nullptr) {
          bench->rhi->setOverlayOnlyReplot(true);
        }
        plot.replot();
        if (bench->rhi != nullptr) {
          bench->rhi->setOverlayOnlyReplot(false);
        }
      } else {
        plot.replot();
      }
      plot.canvas()->update();
    }
    if (streaming) {
      now += 2.0 / kRate;
    }
    QElapsedTimer deadline;
    deadline.start();
    const auto all_done = [&]() {
      for (std::size_t index = 0; index < plots.size(); ++index) {
        if (plots[index]->framesDone() == before[index]) {
          return false;
        }
      }
      return true;
    };
    while (!all_done() && deadline.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    const double wall_ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (warming) {
      warming = !warm_up.done(wall_ms);
      continue;
    }
    ++measured;
    wall.push_back(wall_ms);
    double frame_cpu = 0.0;
    double frame_gpu = 0.0;
    for (const auto& bench : plots) {
      frame_cpu += bench->cpuMs();
      frame_gpu += bench->gpuMs();
    }
    cpu.push_back(frame_cpu);
    gpu.push_back(frame_gpu);
  }
  BenchPlot& first = *plots.front();
  const QString backend = first.rhi != nullptr ? first.rhi->rendererBackend() : u"QPainter raster"_s;
  std::size_t vertices = 0;
  for (const auto& bench : plots) {
    vertices += bench->rhi != nullptr ? bench->rhi->lastVertexCount() : 0;
  }
  std::printf(
      "canvas=%s case=%s backend=\"%s\" plots=%d frames=%d cpu_ms=%.2f cpu_p95_ms=%.2f gpu_ms=%s wall_ms=%.2f "
      "samples=%d dpr=%.1f vertices=%zu gpu_drm_mib=%s cold_ms=%.2f warmup_frames=%d gpu_mhz=%s\n",
      qPrintable(canvas_kind), qPrintable(workload), qPrintable(backend), plot_count, frames,
      pj_bench::stats(cpu).median, pj_bench::stats(cpu).p95,
      gpu_timing ? QByteArray::number(pj_bench::stats(gpu).median, 'f', 2).constData() : "n/a",
      pj_bench::stats(wall).median, first.rhi != nullptr ? first.rhi->sampleCount() : 0,
      first.plot->devicePixelRatioF(), vertices, pj_bench::fmt(pj_bench::drmTotalMib()).constData(), warm_up.coldMs(),
      warm_up.frames(), pj_bench::fmt(warm_up.gpuMhzAtEnd(), 0).constData());
  return 0;
}
