// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame cost of one plot canvas under the plot workloads that matter:
//
//   plot_canvas_benchmark --canvas rhi|gl|raster --case NAME [--frames N]
//
// Cases: static_1m, static_10m (one curve, replotted every frame), streaming
// (10 curves x 100k samples, 2 samples appended per curve per frame with a
// sliding 30 s window: 100 Hz at a 50 Hz frame rate), zoom_pan (1M samples, the
// view moves every frame), many_curves (50 x 100k), dashed (5 x 100k,
// Qt::DashLine). Every frame replots and waits for the canvas to finish a frame.
//
// Reported per run (medians after warm-up): canvas CPU time (PlotRhiCanvas's
// render(), or QwtPlotOpenGLCanvas::paintGL / QwtPlotCanvas::paintEvent), GPU
// time (QRhi timestamps, enabled here through QSG_RHI_PROFILE; a GL_TIME_ELAPSED
// query around paintGL), and the wall time from replot to the finished frame (includes any
// compositor vsync wait). The QRhi
// backend follows PJ_GRAPHICS_API. Needs a display; run without
// LIBGL_ALWAYS_SOFTWARE for the real GPU. Built with PJ_BUILD_DEMOS in the
// QRhi build (PJ_SCENE3D_RHI=ON), the only build that has both canvases.

#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_opengl_canvas.h>
#include <qwt_series_data.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotRhiCanvas.h"

using namespace Qt::StringLiterals;

namespace {

// Growable series; the bounding rect is not used (the axes are fixed).
class VectorSeries final : public QwtSeriesData<QPointF> {
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
  void append(QPointF point) {
    points_.push_back(point);
  }

 private:
  std::vector<QPointF> points_;
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

// QwtPlotOpenGLCanvas with its paintGL CPU time and a GL_TIME_ELAPSED query.
class TimedGlCanvas final : public QwtPlotOpenGLCanvas {
 public:
  using QwtPlotOpenGLCanvas::QwtPlotOpenGLCanvas;
  double cpu_ms = 0.0;
  double gpu_ms = 0.0;

 protected:
  void paintGL() override {
    constexpr GLenum kTimeElapsed = 0x88BF;
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    using GetQueryUi64 = void (*)(GLuint, GLenum, quint64*);
    static const auto get_result =
        reinterpret_cast<GetQueryUi64>(QOpenGLContext::currentContext()->getProcAddress("glGetQueryObjectui64v"));
    if (query_ == 0) {
      gl->glGenQueries(1, &query_);
    }
    QElapsedTimer timer;
    timer.start();
    gl->glBeginQuery(kTimeElapsed, query_);
    QwtPlotOpenGLCanvas::paintGL();
    gl->glEndQuery(kTimeElapsed);
    cpu_ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (get_result != nullptr) {
      quint64 nanoseconds = 0;
      get_result(query_, GL_QUERY_RESULT, &nanoseconds);  // waits for the GPU
      gpu_ms = static_cast<double>(nanoseconds) / 1e6;
    }
  }

 private:
  GLuint query_ = 0;
};

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

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

double percentile95(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[std::min(values.size() - 1, values.size() * 95 / 100)];
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QSG_RHI_PROFILE", "1");  // QRhi timestamps for the widget compositor
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setSwapInterval(0);
  QSurfaceFormat::setDefaultFormat(format);
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({u"canvas"_s, u"rhi, gl or raster"_s, u"canvas"_s, u"rhi"_s});
  parser.addOption({u"case"_s, u"workload"_s, u"case"_s, u"static_1m"_s});
  parser.addOption({u"frames"_s, u"measured frames"_s, u"frames"_s, u"60"_s});
  parser.process(app);
  const QString canvas_kind = parser.value(u"canvas"_s);
  const QString workload = parser.value(u"case"_s);
  const int frames = parser.value(u"frames"_s).toInt();

  QwtPlot plot;
  plot.setAutoReplot(false);
  plot.resize(1600, 900);
  PJ::PlotRhiCanvas* rhi_canvas = nullptr;
  TimedGlCanvas* gl_canvas = nullptr;
  TimedRasterCanvas* raster_canvas = nullptr;
  if (canvas_kind == u"rhi"_s) {
    rhi_canvas = new PJ::PlotRhiCanvas();
    plot.setCanvas(rhi_canvas);
    rhi_canvas->setPlot(&plot);
  } else if (canvas_kind == u"gl"_s) {
    gl_canvas = new TimedGlCanvas();
    gl_canvas->setPaintAttribute(QwtPlotOpenGLCanvas::BackingStore, false);
    plot.setCanvas(gl_canvas);
  } else {
    raster_canvas = new TimedRasterCanvas();
    raster_canvas->setPaintAttribute(QwtPlotCanvas::BackingStore, false);
    plot.setCanvas(raster_canvas);
  }
  plot.setCanvasBackground(QColor(30, 30, 34));

  std::size_t curve_count = 1;
  std::size_t samples = 1'000'000;
  bool dashed = false;
  bool streaming = false;
  bool zoom_pan = false;
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
  } else if (workload != u"static_1m"_s) {
    std::fprintf(stderr, "unknown case %s\n", qPrintable(workload));
    return 2;
  }
  constexpr double kRate = 100.0;  // samples per second of plot time
  std::vector<VectorSeries*> series;
  for (std::size_t index = 0; index < curve_count; ++index) {
    auto* curve = new PJ::PlotCurve(QString::number(index));
    curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
    curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
    curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);
    auto* data = new VectorSeries(makeSignal(samples, kRate, static_cast<unsigned>(index)));
    series.push_back(data);
    curve->setData(data);
    curve->setPen(
        QColor::fromHsv(static_cast<int>(index * 47 % 360), 200, 230), 1.4, dashed ? Qt::DashLine : Qt::SolidLine);
    curve->attach(&plot);
  }
  const double duration = static_cast<double>(samples) / kRate;
  plot.setAxisScale(QwtPlot::yLeft, -2.5, 2.5);
  plot.setAxisScale(QwtPlot::xBottom, 0.0, duration);
  plot.show();
  if (!QTest::qWaitForWindowExposed(&plot)) {
    std::fprintf(stderr, "window not exposed\n");
    return 1;
  }

  // Frames the canvas actually finished (submitted, swapped or painted).
  int presented = 0;
  if (rhi_canvas != nullptr) {
    QObject::connect(rhi_canvas, &QRhiWidget::frameSubmitted, [&presented]() { ++presented; });
  } else if (gl_canvas != nullptr) {
    QObject::connect(gl_canvas, &QOpenGLWidget::frameSwapped, [&presented]() { ++presented; });
  }
  const auto frames_done = [&]() { return raster_canvas != nullptr ? raster_canvas->painted : presented; };

  std::vector<double> cpu;
  std::vector<double> gpu;
  std::vector<double> wall;
  double now = duration;
  constexpr int kWarmup = 5;
  for (int frame = 0; frame < frames + kWarmup; ++frame) {
    if (streaming) {
      for (VectorSeries* data : series) {
        for (int k = 0; k < 2; ++k) {
          data->append(QPointF(now + k / kRate, std::sin(now)));
        }
      }
      now += 2.0 / kRate;
      plot.setAxisScale(QwtPlot::xBottom, now - 30.0, now);
    } else if (zoom_pan) {
      const double span = duration * (0.2 + 0.15 * std::sin(frame * 0.1));
      const double left = (duration - span) * (0.5 + 0.5 * std::sin(frame * 0.05));
      plot.setAxisScale(QwtPlot::xBottom, left, left + span);
    }
    QElapsedTimer timer;
    timer.start();
    const int before = frames_done();
    plot.replot();
    plot.canvas()->update();
    QElapsedTimer deadline;
    deadline.start();
    while (frames_done() == before && deadline.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    const double wall_ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (frame < kWarmup) {
      continue;
    }
    wall.push_back(wall_ms);
    if (rhi_canvas != nullptr) {
      cpu.push_back(rhi_canvas->lastRenderCpuMs());
      gpu.push_back(rhi_canvas->lastGpuFrameMs());
    } else if (gl_canvas != nullptr) {
      cpu.push_back(gl_canvas->cpu_ms);
      gpu.push_back(gl_canvas->gpu_ms);
    } else {
      cpu.push_back(raster_canvas->cpu_ms);
    }
  }
  if (gl_canvas != nullptr) {
    gl_canvas->makeCurrent();
  }
  const QString backend =
      rhi_canvas != nullptr ? rhi_canvas->rendererBackend()
      : gl_canvas != nullptr
          ? QString::fromLatin1(
                reinterpret_cast<const char*>(gl_canvas->context()->functions()->glGetString(GL_RENDERER)))
          : u"QPainter raster"_s;
  std::printf(
      "canvas=%s case=%s backend=\"%s\" frames=%d cpu_ms=%.2f cpu_p95_ms=%.2f gpu_ms=%.2f wall_ms=%.2f\n",
      qPrintable(canvas_kind), qPrintable(workload), qPrintable(backend), frames, median(cpu), percentile95(cpu),
      median(gpu), median(wall));
  return 0;
}
