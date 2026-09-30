// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// PlotRhiCanvas on a real display, per QRhi backend (PJ_GRAPHICS_API picks it;
// ctest registers each backend of the platform). Each case renders one kind of plot
// item and compares the presented frame with Qwt's own QPainter rendering of the
// same canvas (its items drawn into a QImage as QwtPlot::drawItems does), so pixel positions are
// checked against Qwt's scale maps, and a y-flipped or offset frame fails.
// Needs an RHI-capable QPA (wayland/xcb/windows/cocoa); on `offscreen` every case skips, or
// fails under PJ_REQUIRE_GPU_TESTS=1. The failure cases use the
// pjTestFailPipeline hook (PJ_RHI_TEST_HOOKS, test-enabled builds only).

#include <gtest/gtest.h>
#include <qwt_painter.h>
#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_div.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_map.h>
#include <qwt_series_data.h>
#include <qwt_symbol.h>
#include <qwt_text.h>
#include <qwt_transform.h>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <numeric>
#include <string_view>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotPanner.h"
#include "pj_plotting/PlotRhiCanvas.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/RevisionedSeries.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/GraphicsApi.h"

using namespace Qt::StringLiterals;

namespace {

// Runs before main(); see pjPreferRealDisplayQpa().
const bool kRealDisplayPlatform = pjPreferRealDisplayQpa();

PJ::PlotRhiCanvas* rhiCanvas(QwtPlot* plot) {
  return qobject_cast<PJ::PlotRhiCanvas*>(plot->canvas());
}

// Records on the plot the backend its GPU canvas had initialized when it
// reported a failure. The canvas is deleted by the raster fallback, so the plot
// keeps the record: a failure after initialize() is a pipeline failure, which
// presentedFrame() turns into a test failure instead of a skip.
constexpr const char* kFailedBackend = "pjTestFailedBackend";
void watchRenderFailures(QwtPlot* plot) {
  PJ::PlotRhiCanvas* canvas = rhiCanvas(plot);
  if (canvas == nullptr) {
    return;
  }
  QObject::connect(canvas, &QRhiWidget::renderFailed, plot, [plot, canvas] {
    if (!canvas->rendererBackend().isEmpty() && plot->property(kFailedBackend).isNull()) {
      plot->setProperty(kFailedBackend, canvas->rendererBackend());
    }
  });
}

class TestPlot : public PJ::PlotWidgetBase {
 public:
  explicit TestPlot(QWidget* parent = nullptr) : PJ::PlotWidgetBase(parent) {
    watchRenderFailures(qwtPlot());
  }
  using PJ::PlotWidgetBase::qwtPlot;
  using PJ::PlotWidgetBase::setStyle;
};

// Points with a content revision the test bumps by hand: an unchanged revision
// lets a full replot reuse the curve layer.
class Revisioned : public QwtPointSeriesData, public PJ::RevisionedSeries {
 public:
  using QwtPointSeriesData::QwtPointSeriesData;
  void bump() {
    bumpContentRevision();
  }
};

// Revisioned points always allocated at one address, as a series reallocated
// by a transform applied in place can be. One alive at a time.
class SameAddressRevisioned final : public Revisioned {
 public:
  using Revisioned::Revisioned;
  static void* operator new(std::size_t size) {
    if (size > sizeof(slot_)) {
      throw std::bad_alloc();
    }
    return slot_;
  }
  static void operator delete(void* /*slot*/) noexcept {}

 private:
  alignas(std::max_align_t) static inline std::byte slot_[512];
};

class TestPlotWidget : public PJ::PlotWidget {
 public:
  TestPlotWidget(PJ::SessionManager* session, PJ::CatalogModel* catalog) : PJ::PlotWidget(session, catalog) {
    watchRenderFailures(qwtPlot());
  }
  using PJ::PlotWidgetBase::qwtPlot;
};

class GrabbingPanner : public PJ::PlotPanner {
 public:
  using PJ::PlotPanner::PlotPanner;
  [[nodiscard]] QPixmap grabCanvas() const {
    return grab();
  }
};

QwtSeriesData<QPointF>* series(std::vector<QPointF> points) {
  return new QwtPointSeriesData(QVector<QPointF>(points.begin(), points.end()));
}

// One presented frame after the queued replot work has run; null when nothing
// was presented within the canvas's timeout, or the canvas was replaced by the
// raster fallback meanwhile.
QImage grabPresented(PJ::PlotRhiCanvas& canvas) {
  return pjWaitForGrab([&canvas](auto done) { canvas.grabPresentedFrame(std::move(done)); }, &canvas);
}

// Qwt's own QPainter rendering of the canvas items, at the frame's resolution
// (QwtPlot::drawItems, item by item). Qwt snaps mapped coordinates to whole
// pixels on screen devices (QwtPainter::roundingAlignment()). The RHI canvas
// snaps the grid and marker lines the same way, and keeps curves exact, so its
// references snap only those items: a 1 px curve half a pixel off would split
// into two half-covered rows and no colour would match. `snapped` snaps every
// item: the on-screen rendering, for the raster canvas.
// The image format follows the canvas's text path. With ClearType on (Windows),
// Qt's raster engine draws subpixel (coloured-fringe) glyphs only into opaque
// ARGB32_Premultiplied/RGB32 targets: the on-screen raster canvas gets them, the
// RHI canvas's translucent RGBA8888 text atlas gets grayscale antialiasing, so
// its reference is drawn into that format too. Elsewhere both formats are equal.
QImage referenceCanvas(QwtPlot& plot, QSize device_size, bool snapped = false) {
  QWidget* canvas = plot.canvas();
  QImage image(device_size, snapped ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
  image.setDevicePixelRatio(static_cast<qreal>(device_size.width()) / canvas->width());
  image.fill(plot.canvasBackground().color());
  QPainter painter(&image);
  std::array<QwtScaleMap, QwtAxis::AxisPositions> maps;
  for (int axis = 0; axis < QwtAxis::AxisPositions; ++axis) {
    maps[static_cast<std::size_t>(axis)] = plot.canvasMap(axis);
  }
  const QRectF canvas_rect = canvas->contentsRect();
  const bool was_snapping = QwtPainter::roundingAlignment();
  for (QwtPlotItem* item : plot.itemList()) {
    if (!item->isVisible()) {
      continue;
    }
    const bool axis_line = item->rtti() == QwtPlotItem::Rtti_PlotGrid || item->rtti() == QwtPlotItem::Rtti_PlotMarker;
    QwtPainter::setRoundingAlignment(snapped || axis_line);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, item->testRenderHint(QwtPlotItem::RenderAntialiased));
    item->draw(
        &painter, maps[static_cast<std::size_t>(item->xAxis())], maps[static_cast<std::size_t>(item->yAxis())],
        canvas_rect);
    painter.restore();
  }
  QwtPainter::setRoundingAlignment(was_snapping);
  painter.end();
  return image.convertToFormat(QImage::Format_RGB32);
}

int channelDelta(QRgb a, QRgb b) {
  return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}

// Pixels that visibly differ from the canvas background, border excluded (see
// compareWithReference()).
std::vector<QPoint> contentPixels(const QImage& image, QRgb background) {
  std::vector<QPoint> pixels;
  for (int y = 2; y < image.height() - 2; ++y) {
    for (int x = 2; x < image.width() - 2; ++x) {
      if (channelDelta(image.pixel(x, y), background) > 60) {
        pixels.emplace_back(x, y);
      }
    }
  }
  return pixels;
}

int maxChannelDelta(QRgb a, QRgb b) {
  return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

// How the RHI frame compares with Qwt's raster of the same canvas. "Foreground"
// is a pixel that visibly differs from the background (contentPixels()).
struct ReferenceComparison {
  // Foreground of either image, and those whose frame colour is within
  // kColourTolerance of a reference pixel in their 3x3 neighbourhood: the
  // colours match, background pixels do not dilute the ratio.
  std::size_t foreground = 0;
  std::size_t colour_matched = 0;
  // Reference foreground with no frame foreground within `radius`.
  std::size_t reference_pixels = 0;
  std::size_t missing = 0;
  // Frame foreground farther than `radius` from any reference foreground.
  std::size_t frame_pixels = 0;
  std::size_t extra = 0;
  // Pixels not exactly the clear colour farther than `radius` from any pixel
  // the reference drew (any colour but the clear one).
  std::size_t background_strays = 0;
};

constexpr int kColourTolerance = 48;

// The canvas frame (1 logical px) and one device pixel beyond it are excluded:
// Qwt draws items in the canvas contents rect, the RHI canvas across the whole
// widget, and the frame's antialiasing fringe reaches past it on some rasterizers
// (Apple's software OpenGL at DPR 2).
ReferenceComparison compareWithReference(const QImage& frame, const QImage& reference, QRgb background, int radius) {
  const int width = frame.width();
  const int height = frame.height();
  const int border =
      1 + std::max(1, static_cast<int>(std::lround(frame.width() / reference.deviceIndependentSize().width())));
  const auto is_foreground = [background](const QImage& image, int x, int y) {
    return channelDelta(image.pixel(x, y), background) > 60;
  };
  std::vector<char> reference_mask(static_cast<std::size_t>(width * height), 0);
  std::vector<char> frame_mask(reference_mask.size(), 0);
  std::vector<char> reference_drawn(reference_mask.size(), 0);
  for (int y = border; y < height - border; ++y) {
    for (int x = border; x < width - border; ++x) {
      const auto index = static_cast<std::size_t>(y * width + x);
      reference_mask[index] = is_foreground(reference, x, y) ? 1 : 0;
      frame_mask[index] = is_foreground(frame, x, y) ? 1 : 0;
      reference_drawn[index] = reference.pixel(x, y) != background ? 1 : 0;
    }
  }
  const auto any_within = [&](const std::vector<char>& mask, int x, int y, int r) {
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        const int px = x + dx;
        const int py = y + dy;
        if (px >= 0 && py >= 0 && px < width && py < height && mask[static_cast<std::size_t>(py * width + px)] != 0) {
          return true;
        }
      }
    }
    return false;
  };
  ReferenceComparison result;
  for (int y = border; y < height - border; ++y) {
    for (int x = border; x < width - border; ++x) {
      const bool in_reference = reference_mask[static_cast<std::size_t>(y * width + x)] != 0;
      const bool in_frame = frame_mask[static_cast<std::size_t>(y * width + x)] != 0;
      if (in_reference || in_frame) {
        ++result.foreground;
        bool matched = false;
        for (int dy = -1; dy <= 1 && !matched; ++dy) {
          for (int dx = -1; dx <= 1 && !matched; ++dx) {
            matched = maxChannelDelta(frame.pixel(x, y), reference.pixel(x + dx, y + dy)) <= kColourTolerance;
          }
        }
        result.colour_matched += matched ? 1U : 0U;
      }
      if (in_reference) {
        ++result.reference_pixels;
        result.missing += any_within(frame_mask, x, y, radius) ? 0U : 1U;
      }
      if (in_frame) {
        ++result.frame_pixels;
        result.extra += any_within(reference_mask, x, y, radius) ? 0U : 1U;
      }
      if (frame.pixel(x, y) != background && !any_within(reference_drawn, x, y, radius)) {
        ++result.background_strays;
      }
    }
  }
  return result;
}

// A pixel's ink between the clear colour (0) and `ink` (1), read from the
// channel where the two differ most.
double inkCoverage(QRgb pixel, QRgb clear, QRgb ink) {
  const std::array<int, 3> pixel_channels = {qRed(pixel), qGreen(pixel), qBlue(pixel)};
  const std::array<int, 3> clear_channels = {qRed(clear), qGreen(clear), qBlue(clear)};
  const std::array<int, 3> ink_channels = {qRed(ink), qGreen(ink), qBlue(ink)};
  std::size_t best = 0;
  for (std::size_t channel = 1; channel < 3; ++channel) {
    if (std::abs(ink_channels[channel] - clear_channels[channel]) >
        std::abs(ink_channels[best] - clear_channels[best])) {
      best = channel;
    }
  }
  const double span = ink_channels[best] - clear_channels[best];
  return std::clamp((pixel_channels[best] - clear_channels[best]) / span, 0.0, 1.0);
}

// Share of `inked` (true where ink was found, one entry per device pixel along
// a line) that follows a dash pattern of `on` pixels out of `period`, at the
// best phase. Entries of -1 (crossing lines) are not compared.
double cadenceMatch(const std::vector<int>& inked, int period, int on) {
  double best = 0.0;
  for (int phase = 0; phase < period; ++phase) {
    int compared = 0;
    int matched = 0;
    for (std::size_t index = 0; index < inked.size(); ++index) {
      if (inked[index] < 0) {
        continue;
      }
      const bool expected = ((static_cast<int>(index) + phase) % period) < on;
      ++compared;
      matched += (inked[index] == 1) == expected ? 1 : 0;
    }
    best = std::max(best, compared == 0 ? 0.0 : static_cast<double>(matched) / compared);
  }
  return best;
}

// The RHI frame and Qwt's raster agree: nearly every foreground pixel of either
// (`min_colour_match`) has the reference's colour close by, neither misses nor
// adds geometry, the background is exactly the clear colour, and both drew a
// meaningful amount.
void expectMatchesReference(
    const QImage& frame, const QImage& reference, QRgb background, std::size_t min_pixels,
    double min_colour_match = 0.98, std::size_t max_strays = 2) {
  ASSERT_EQ(frame.size(), reference.size());
  // PJ_RHI_TEST_DUMP_DIR=<dir> saves both images (CI uploads the dir on failure).
  if (const QString dir = qEnvironmentVariable("PJ_RHI_TEST_DUMP_DIR"); !dir.isEmpty()) {
    const QString base =
        dir + u"/plot_"_s + QString::fromUtf8(::testing::UnitTest::GetInstance()->current_test_info()->name()) + u'_' +
        qEnvironmentVariable("PJ_GRAPHICS_API", u"default"_s) +
        (qEnvironmentVariableIsSet("QT_SCALE_FACTOR") ? u"_dpr"_s + qEnvironmentVariable("QT_SCALE_FACTOR")
                                                      : QString());
    QDir().mkpath(dir);
    frame.save(base + u"_frame.png"_s);
    reference.save(base + u"_ref.png"_s);
  }
  const int radius =
      std::max(2, static_cast<int>(std::lround(2.0 * frame.width() / reference.deviceIndependentSize().width())));
  const ReferenceComparison c = compareWithReference(frame, reference, background, radius);
  const auto fraction = [](std::size_t part, std::size_t whole) {
    return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
  };
  const double colour_match = fraction(c.colour_matched, c.foreground);
  const double missing = fraction(c.missing, c.reference_pixels);
  const double extra = fraction(c.extra, c.frame_pixels);
  std::printf(
      "[parity] %s colour_match=%.4f missing=%.4f extra=%.4f strays=%zu foreground=%zu\n",
      ::testing::UnitTest::GetInstance()->current_test_info()->name(), colour_match, missing, extra,
      c.background_strays, c.foreground);
  EXPECT_GE(c.reference_pixels, min_pixels) << "the reference drew too little to compare";
  EXPECT_GE(c.frame_pixels, min_pixels);
  // Measured on llvmpipe/lavapipe at DPR 1 and 2 (one sample per pixel, shader
  // edge antialiasing): colour match >= 0.986, 1.000 for curves, grid and
  // dashes; missing <= 0.014 (PlotMarkersItem text), no extra pixel and no
  // stray. A red/blue swap in the vertex colour drops the colour match to 0.00-0.68.
  EXPECT_GE(colour_match, min_colour_match) << "foreground colours differ from Qwt's";
  EXPECT_LE(missing, 0.03) << "reference pixels missing in frame";
  EXPECT_LE(extra, 0.02) << "frame pixels absent from reference";
  EXPECT_LE(c.background_strays, max_strays) << "the background is not exactly the clear colour";
}

// A top-level window hosting one plot, shown and exposed.
class PlotWindow {
 public:
  explicit PlotWindow(QWidget* plot, QSize size = {640, 400}) {
    auto* layout = new QVBoxLayout(&window_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(plot);
    window_.resize(size);
    window_.show();
    exposed_ = QTest::qWaitForWindowExposed(&window_);
  }
  [[nodiscard]] bool exposed() const {
    return exposed_;
  }
  QWidget& window() {
    return window_;
  }

 private:
  QWidget window_;
  bool exposed_ = false;
};

class PlotRhiRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(false);
  }

  // Common plot setup: no grid, no legend, fixed 0..10 axes.
  static void prepare(TestPlot& plot) {
    plot.setGridVisible(false);
    plot.setLegendVisible(false);
    plot.qwtPlot()->setAxisScale(QwtPlot::xBottom, 0.0, 10.0);
    plot.qwtPlot()->setAxisScale(QwtPlot::yLeft, 0.0, 10.0);
    plot.qwtPlot()->replot();
  }

  // Grabs the presented frame. Null when the canvas never got a QRhi (the
  // caller skips) or on a test failure: a canvas that initialized a backend and
  // then failed, or presented nothing, is a broken pipeline, not a missing GPU.
  static QImage presentedFrame(QwtPlot& plot) {
    const QPointer<PJ::PlotRhiCanvas> canvas = rhiCanvas(&plot);
    const QImage frame = canvas != nullptr ? grabPresented(*canvas) : QImage();
    const QString failed = plot.property(kFailedBackend).toString();
    if (!failed.isEmpty()) {
      ADD_FAILURE() << "the plot pipeline failed after initializing " << failed.toStdString();
      return {};
    }
    const QString backend = canvas != nullptr ? canvas->rendererBackend() : QString();
    if (backend.isEmpty()) {
      return {};
    }
    if (frame.isNull()) {
      ADD_FAILURE() << "the canvas initialized " << backend.toStdString() << " but presented no frame";
      return {};
    }
    const QString expected = pjRhiBackendName(PJ::preferredGraphicsApi());
    EXPECT_TRUE(backend.startsWith(expected))
        << "backend " << backend.toStdString() << ", requested " << expected.toStdString();
    return frame;
  }

  static QRgb background(QwtPlot& plot) {
    return plot.canvasBackground().color().rgb();
  }
};

#define PJ_GRAB_OR_SKIP(frame, plot)                                             \
  const QImage frame = presentedFrame(plot);                                     \
  if (frame.isNull()) {                                                          \
    if (HasFailure()) {                                                          \
      return;                                                                    \
    }                                                                            \
    PJ_SKIP_WITHOUT_GPU("the plot canvas presented no frame (no QRhi backend)"); \
  }

TEST_F(PlotRhiRenderTest, CurvesMatchQwtTransforms) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  // Asymmetric: a steep ramp confined to the top-left quadrant plus a flat
  // line near the bottom, so a y-flip or x-mirror cannot match the reference.
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}}), QColor(230, 40, 40));
  plot->addCurve(u"floor"_s, series({{1.0, 1.0}, {9.0, 2.0}}), QColor(40, 90, 230));
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 200);

  // The ramp's first sample lands where Qwt's maps put it.
  const QwtScaleMap x_map = plot->qwtPlot()->canvasMap(QwtPlot::xBottom);
  const QwtScaleMap y_map = plot->qwtPlot()->canvasMap(QwtPlot::yLeft);
  const qreal dpr = static_cast<qreal>(frame.width()) / plot->qwtPlot()->canvas()->width();
  const QPoint sample(
      static_cast<int>(std::lround(x_map.transform(2.0) * dpr)),
      static_cast<int>(std::lround(y_map.transform(9.5) * dpr)));
  bool red_near_sample = false;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const QRgb pixel = frame.pixel(sample + QPoint(dx, dy));
      red_near_sample = red_near_sample || (qRed(pixel) > 150 && qBlue(pixel) < 120);
    }
  }
  EXPECT_TRUE(red_near_sample) << "no ramp pixel at the mapped sample " << sample.x() << "," << sample.y();
}

// Wide translucent curves crossing each other and an opaque one: the colours
// where they blend (over the background and over each other) must match Qwt's,
// not only their coverage.
TEST_F(PlotRhiRenderTest, OverlappingTranslucentCurvesBlendLikeQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  const auto add = [plot](const QString& name, std::vector<QPointF> points, QColor colour) {
    auto* info = plot->addCurve(name, series(std::move(points)), colour);
    ASSERT_NE(info, nullptr);
    QPen pen = info->curve->pen();
    pen.setColor(colour);
    pen.setWidthF(10.0);
    info->curve->setPen(pen);
  };
  add(u"opaque"_s, {{0.5, 5.0}, {9.5, 5.0}}, QColor(40, 200, 90));
  add(u"rising"_s, {{0.5, 1.5}, {9.5, 8.5}}, QColor(230, 40, 40, 128));
  add(u"falling"_s, {{0.5, 8.5}, {9.5, 1.5}}, QColor(40, 90, 230, 150));
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 2000);
}

TEST_F(PlotRhiRenderTest, DashedLinesMatchQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  auto* info = plot->addCurve(u"dashed"_s, series({{0.5, 5.0}, {9.5, 5.0}}), QColor(230, 40, 40));
  ASSERT_NE(info, nullptr);
  QPen pen = info->curve->pen();
  pen.setStyle(Qt::DashLine);
  info->curve->setPen(pen);
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 100);
  // Gaps exist: along the line the ink dips once per dash period (at most 12
  // device pixels). An antialiased gap edge can straddle two pixels, so a dip is
  // any run below 3/4 of the line's full ink, not a background pixel.
  const qreal dpr = static_cast<qreal>(frame.width()) / plot->qwtPlot()->canvas()->width();
  const int y = static_cast<int>(std::lround(plot->qwtPlot()->canvasMap(QwtPlot::yLeft).transform(5.0) * dpr));
  const QRgb clear = background(*plot->qwtPlot());
  int full_ink = 0;
  for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
    full_ink = std::max(full_ink, channelDelta(frame.pixel(x, y), clear));
  }
  int dips = 0;
  bool in_dip = false;
  for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
    const bool low = channelDelta(frame.pixel(x, y), clear) * 4 < full_ink * 3;
    dips += low && !in_dip ? 1 : 0;
    in_dip = low;
  }
  EXPECT_GT(full_ink, 60) << "no line on the sampled row";
  EXPECT_GT(dips, frame.width() / 2 / 24) << "the dashed line has no gaps";
}

TEST_F(PlotRhiRenderTest, SymbolsOfEveryStyleMatchQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot, {720, 480});
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  const std::vector<QwtSymbol::Style> styles = {
      QwtSymbol::Ellipse,   QwtSymbol::Rect,      QwtSymbol::Diamond, QwtSymbol::Triangle, QwtSymbol::DTriangle,
      QwtSymbol::LTriangle, QwtSymbol::RTriangle, QwtSymbol::Cross,   QwtSymbol::XCross,   QwtSymbol::HLine,
      QwtSymbol::VLine,     QwtSymbol::Star1,     QwtSymbol::Hexagon};
  for (std::size_t index = 0; index < styles.size(); ++index) {
    const double x = 0.7 + static_cast<double>(index % 7) * 1.4;
    const double y = index < 7 ? 7.0 : 3.0;
    auto* info = plot->addCurve(u"symbol%1"_s.arg(index), series({{x, y}}), QColor(230, 40, 40));
    ASSERT_NE(info, nullptr);
    info->curve->setStyle(QwtPlotCurve::NoCurve);
    auto* symbol =
        new QwtSymbol(styles[index], QBrush(QColor(40, 90, 230)), QPen(QColor(230, 40, 40), 2.0), QSize(18, 18));
    symbol->setCachePolicy(QwtSymbol::NoCache);  // the pixmap cache snaps symbols to whole pixels
    info->curve->setSymbol(symbol);
  }
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  // Qwt draws Rect, Cross, HLine and VLine symbols aliased on whole pixels
  // (qwt_symbol.cpp); the canvas places them on whole pixels too, antialiased
  // edges and all: 0.989 at DPR 1, 0.992 at DPR 2.
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 1000);
}

TEST_F(PlotRhiRenderTest, LinesAndDotsDrawSquareDots) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->setDefaultStyle(PJ::PlotWidgetBase::kLinesAndDots);
  auto* info = plot->addCurve(
      u"dots"_s, series({{1.0, 2.0}, {3.0, 8.0}, {5.0, 3.0}, {7.0, 7.0}, {9.0, 5.0}}), QColor(230, 40, 40));
  ASSERT_NE(info, nullptr);
  const auto* curve = dynamic_cast<const PJ::PlotCurve*>(info->curve);
  ASSERT_NE(curve, nullptr);
  ASSERT_GT(curve->dotWidth(), 0.0);
  ASSERT_EQ(curve->symbol(), nullptr) << "Lines and Dots is PlotCurve's dot pass, not a per-point symbol";
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  const QImage reference = referenceCanvas(*plot->qwtPlot(), frame.size());
  expectMatchesReference(frame, reference, background(*plot->qwtPlot()), 200);
  // The dot at the (3, 8) peak is wider than the line through it.
  const qreal dpr = static_cast<qreal>(frame.width()) / plot->qwtPlot()->canvas()->width();
  const QPoint peak(
      static_cast<int>(std::lround(plot->qwtPlot()->canvasMap(QwtPlot::xBottom).transform(3.0) * dpr)),
      static_cast<int>(std::lround(plot->qwtPlot()->canvasMap(QwtPlot::yLeft).transform(8.0) * dpr)));
  const int half = static_cast<int>(std::floor(curve->dotWidth() * dpr * 0.5)) - 1;
  int covered = 0;
  for (int dx = -half; dx <= half; ++dx) {
    covered += channelDelta(frame.pixel(peak + QPoint(dx, half)), background(*plot->qwtPlot())) > 60 ? 1 : 0;
  }
  EXPECT_GE(covered, 2 * half) << "the dot's bottom row is not filled";
}

TEST_F(PlotRhiRenderTest, MarkerLineSymbolAndLabelMatchQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  auto* marker = new QwtPlotMarker();
  marker->setLineStyle(QwtPlotMarker::VLine);
  marker->setLinePen(QColor(40, 200, 90), 2.0);
  marker->setValue(3.0, 6.0);
  auto* symbol = new QwtSymbol(QwtSymbol::Ellipse, QBrush(QColor(230, 40, 40)), QPen(Qt::NoPen), QSize(12, 12));
  symbol->setCachePolicy(QwtSymbol::NoCache);  // the pixmap cache snaps symbols to whole pixels
  marker->setSymbol(symbol);
  // The RHI canvas antialiases every edge; so does this reference.
  marker->setRenderHint(QwtPlotItem::RenderAntialiased, true);
  QwtText label(u"Marker label"_s);
  label.setColor(QColor(240, 240, 60));
  marker->setLabel(label);
  marker->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
  marker->attach(plot->qwtPlot());
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 300);
}

// A labelled marker replaced by an identical one (the old one deleted after the
// new one is attached, so the two never share an address): the reused text
// atlas follows the new item, whose label still draws.
TEST_F(PlotRhiRenderTest, IdenticalMarkerReplacementKeepsItsLabel) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  QwtPlot& qwt = *plot->qwtPlot();
  const auto make_marker = [&qwt] {
    auto* marker = new QwtPlotMarker();
    marker->setLineStyle(QwtPlotMarker::VLine);
    marker->setLinePen(QColor(40, 200, 90), 2.0);
    marker->setValue(3.0, 6.0);
    marker->setRenderHint(QwtPlotItem::RenderAntialiased, true);
    QwtText label(u"Marker label"_s);
    label.setColor(QColor(240, 240, 60));
    marker->setLabel(label);
    marker->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    marker->attach(&qwt);
    return marker;
  };
  QwtPlotMarker* first = make_marker();
  qwt.replot();
  PJ_GRAB_OR_SKIP(before, qwt);
  expectMatchesReference(before, referenceCanvas(qwt, before.size()), background(qwt), 300);

  make_marker();
  delete first;
  qwt.replot();
  const QImage after = presentedFrame(qwt);
  ASSERT_FALSE(after.isNull());
  expectMatchesReference(after, referenceCanvas(qwt, after.size()), background(qwt), 300);
}

// An XY curve doubling back along itself, past its first point: the returning
// segment keeps the full stroke width instead of reusing the incoming corner
// pair (a self-crossing quad that pinches to nothing halfway).
TEST_F(PlotRhiRenderTest, CollinearReversalKeepsTheStrokeWidth) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  auto* info = plot->addCurve(u"reversal"_s, series({{4.0, 5.0}, {5.0, 5.0}, {0.5, 5.0}}), QColor(230, 40, 40));
  ASSERT_NE(info, nullptr);
  QPen pen = info->curve->pen();
  pen.setWidthF(8.0);
  info->curve->setPen(pen);
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  // Where the path doubles back over itself, its antialiased edge pixels blend
  // twice (Qwt fills the stroke once): 5% of the foreground on llvmpipe/lavapipe.
  expectMatchesReference(
      frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 1500,
      /*min_colour_match=*/0.9);
}

// A solid segment between a sample in view and one mapped far off the canvas:
// 1e41 on a 0..10 axis lands about 4e42 px away, past the float range of the
// clip coordinates. The stroke still reaches the canvas edge, whichever end is
// the far one.
TEST_F(PlotRhiRenderTest, SegmentToAFarOffCanvasSampleReachesTheEdge) {
  PJ_SKIP_ON_HEADLESS_QPA();
  for (const bool far_last : {true, false}) {
    SCOPED_TRACE(far_last ? "far final sample" : "far first sample");
    auto* plot = new TestPlot();
    PlotWindow window(plot);
    ASSERT_TRUE(window.exposed());
    prepare(*plot);
    std::vector<QPointF> points = {{3.0, 2.0}, {3.0, 1e41}};
    if (!far_last) {
      std::reverse(points.begin(), points.end());
    }
    auto* info = plot->addCurve(u"spike"_s, series(points), QColor(230, 40, 40));
    ASSERT_NE(info, nullptr);
    QPen pen = info->curve->pen();
    pen.setWidthF(3.0);
    info->curve->setPen(pen);
    plot->qwtPlot()->replot();
    PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
    const QRgb clear = background(*plot->qwtPlot());
    expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), clear, 200);

    // Every row from the in-view sample up to the canvas edge is inked.
    const qreal dpr = static_cast<qreal>(frame.width()) / plot->qwtPlot()->canvas()->width();
    const int x = static_cast<int>(std::lround(plot->qwtPlot()->canvasMap(QwtPlot::xBottom).transform(3.0) * dpr));
    const int sample_y = static_cast<int>(std::lround(plot->qwtPlot()->canvasMap(QwtPlot::yLeft).transform(2.0) * dpr));
    const int edge = 2 + static_cast<int>(std::lround(dpr));  // past the 1 px canvas frame
    int blank_rows = 0;
    for (int y = edge; y < sample_y; ++y) {
      bool inked = false;
      for (int dx = -2; dx <= 2 && !inked; ++dx) {
        inked = channelDelta(frame.pixel(x + dx, y), clear) > 60;
      }
      blank_rows += inked ? 0 : 1;
    }
    EXPECT_EQ(blank_rows, 0) << "of " << (sample_y - edge) << " rows between the edge and the sample";
  }
}

TEST_F(PlotRhiRenderTest, LegendTextMatchesQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->setLegendVisible(true);
  // Out of view: only the legend entry draws.
  plot->addCurve(u"legend entry"_s, series({{-5.0, -5.0}, {-4.0, -4.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 50);
}

// PlotMarkersItem (regions/events/labels from the ObjectStore) is rasterised
// through its own QPainter drawing into the canvas's texture layer.
TEST_F(PlotRhiRenderTest, PlotMarkersItemRegionMatchesQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/imu/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  for (int i = 0; i < 20; ++i) {
    writer.appendScalar(*handle, 1'000'000'000 + i * 100'000'000LL, static_cast<double>(i % 5));
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  QString key;
  for (const auto& curve : catalog.curves()) {
    key = curve.name;
  }
  ASSERT_FALSE(key.isEmpty());

  PJ::sdk::PlotMarkers markers;
  PJ::sdk::PlotMarker region;
  region.kind = PJ::sdk::MarkerKind::kRegion;
  region.t_start = 1'400'000'000;
  region.t_end = 1'800'000'000;
  region.label = "region";
  region.color = {255, 40, 40, 255};
  markers.markers.push_back(region);
  PJ::sdk::PlotMarker event;
  event.kind = PJ::sdk::MarkerKind::kEvent;
  event.t_start = 2'300'000'000;
  event.label = "event";
  markers.markers.push_back(event);
  // A text callout: not vertically symmetric, unlike the full-height bands.
  PJ::sdk::PlotMarker callout;
  callout.kind = PJ::sdk::MarkerKind::kLabel;
  callout.t_start = 2'600'000'000;
  callout.label = "callout text";
  markers.markers.push_back(callout);
  const std::string object_topic = PJ::sdk::markerObjectTopicName(PJ::markerScopeTopic(PJ::MarkerScope::kDataset));
  const auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = PJ::markerScopeDataset(PJ::MarkerScope::kDataset, *dataset),
          .topic_name = object_topic,
          .metadata_json = {}});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(session.objectStore().pushOwned(*topic, PJ::Timestamp{0}, PJ::serializePlotMarkers(markers)).has_value());
  session.notifyMarkersChanged();

  auto* plot = new TestPlotWidget(&session, &catalog);
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  auto* info = plot->addCurve(key);
  ASSERT_NE(info, nullptr);
  plot->zoomOut(/*emit_signal=*/false);
  info->curve->setVisible(false);  // only the markers draw
  plot->setGridVisible(false);
  plot->setLegendVisible(false);
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  const QImage reference = referenceCanvas(*plot->qwtPlot(), frame.size());
  expectMatchesReference(frame, reference, background(*plot->qwtPlot()), 2000);
  // The bands are vertically symmetric; the callout text is not. Its half of
  // the canvas must carry the same extra ink in both images.
  const auto top_share = [&](const QImage& image) {
    const auto pixels = contentPixels(image, background(*plot->qwtPlot()));
    const auto top =
        std::count_if(pixels.begin(), pixels.end(), [&](const QPoint& p) { return p.y() < image.height() / 2; });
    return static_cast<double>(top) / static_cast<double>(std::max<std::size_t>(1, pixels.size()));
  };
  EXPECT_NEAR(top_share(frame), top_share(reference), 0.01);
  EXPECT_GT(std::abs(top_share(reference) - 0.5), 0.02) << "the fixture lost its vertical asymmetry";
}

// A canvas keeps no mapped copy of a long series between frames: an x-sorted
// solid curve maps only its visible range, and the whole-series map of a
// zoomed-out view is released after the rebuild that needed it.
TEST_F(PlotRhiRenderTest, LongSeriesLeavesNoMappedCopyBehind) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  constexpr int kSamples = 1'000'000;
  std::vector<QPointF> points;
  points.reserve(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    points.emplace_back(i, 5.0 + 4.0 * std::sin(i * 0.01));
  }
  plot->addCurve(u"long"_s, series(std::move(points)), QColor(230, 40, 40));
  plot->qwtPlot()->setAxisScale(QwtPlot::xBottom, 500'000.0, 501'000.0);
  plot->qwtPlot()->replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(plot->qwtPlot());
  {
    PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
    // ~1000 visible samples plus the cull margins: tens of KiB, not the 16 MB series.
    EXPECT_LT(canvas->retainedSampleScratchBytes(), std::size_t{64} << 10);
  }
  plot->qwtPlot()->setAxisScale(QwtPlot::xBottom, 0.0, kSamples);
  plot->qwtPlot()->replot();
  {
    PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
    EXPECT_LE(canvas->retainedSampleScratchBytes(), std::size_t{1} << 20);
    EXPECT_GT(canvas->lastVertexCount(), 0U) << "the zoomed-out curve drew nothing";
  }
}

// A new top-level (new QRhi), a resize and a sample-count change (new render
// pass format) each rebuild the pipelines and still draw where Qwt maps.
TEST_F(PlotRhiRenderTest, ReparentResizeAndFormatChangeKeepRendering) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  auto first = std::make_unique<PlotWindow>(plot);
  ASSERT_TRUE(first->exposed());
  prepare(*plot);
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  {
    PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
    expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 200);
  }

  QWidget second;
  auto* layout = new QVBoxLayout(&second);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot);
  second.resize(500, 300);
  second.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&second));
  first.reset();
  plot->qwtPlot()->replot();
  {
    const QImage frame = presentedFrame(*plot->qwtPlot());
    ASSERT_FALSE(frame.isNull()) << "no frame after the reparent";
    expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 150);
  }

  second.resize(760, 520);
  QCoreApplication::processEvents();
  plot->qwtPlot()->replot();
  {
    const QImage frame = presentedFrame(*plot->qwtPlot());
    ASSERT_FALSE(frame.isNull()) << "no frame after the resize";
    expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 200);
  }

  rhiCanvas(plot->qwtPlot())->setSampleCount(1);
  plot->qwtPlot()->replot();
  {
    const QImage frame = presentedFrame(*plot->qwtPlot());
    ASSERT_FALSE(frame.isNull()) << "no frame after the sample-count change";
    expectMatchesReference(frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 200);
  }
}

// The panner's drag image comes from the QRhi framebuffer (QWidget::grab sees
// no QRhi content), repeatedly.
TEST_F(PlotRhiRenderTest, PannerGrabReadsTheFramebuffer) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  GrabbingPanner panner(plot->qwtPlot()->canvas());
  for (int attempt = 0; attempt < 2; ++attempt) {
    const QImage grabbed = panner.grabCanvas().toImage().convertToFormat(QImage::Format_RGB32);
    ASSERT_EQ(grabbed.size(), frame.size()) << "attempt " << attempt;
    expectMatchesReference(
        grabbed, referenceCanvas(*plot->qwtPlot(), grabbed.size()), background(*plot->qwtPlot()), 200);
  }
}

// The raster canvas's own painting, at the widget's device resolution.
QImage rasterFrame(QWidget& canvas) {
  QCoreApplication::processEvents();
  return canvas.grab().toImage().convertToFormat(QImage::Format_RGB32);
}

void sendMouse(QWidget& target, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent event(type, pos, target.mapToGlobal(pos), button, buttons, Qt::NoModifier);
  QApplication::sendEvent(&target, &event);
}

// Waits for PlotWidgetBase's queued swap to the raster canvas.
QwtPlotCanvas* waitForRasterCanvas(QwtPlot& plot) {
  static_cast<void>(QTest::qWaitFor([&plot] { return qobject_cast<QwtPlotCanvas*>(plot.canvas()) != nullptr; }, 5000));
  return qobject_cast<QwtPlotCanvas*>(plot.canvas());
}

// A GPU canvas whose pipelines cannot be built: renderFailed() once, then
// PlotWidgetBase swaps in the raster canvas, which draws the curve and keeps
// zoom and pan, while a sibling QRhi plot in the same window keeps rendering.
TEST_F(PlotRhiRenderTest, PipelineFailureFallsBackToTheRasterCanvas) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* failing = new TestPlot();
  auto* sibling = new TestPlot();
  const QPointer<PJ::PlotRhiCanvas> failed_canvas = rhiCanvas(failing->qwtPlot());
  ASSERT_NE(failed_canvas, nullptr);
  failed_canvas->setProperty("pjTestFailPipeline", u"color"_s);
  QSignalSpy failures(failed_canvas.data(), &QRhiWidget::renderFailed);
  auto* pair = new QWidget();
  auto* layout = new QHBoxLayout(pair);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(failing);
  layout->addWidget(sibling);
  PlotWindow window(pair, {1000, 400});
  ASSERT_TRUE(window.exposed());
  for (TestPlot* plot : {failing, sibling}) {
    prepare(*plot);
    plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
    plot->qwtPlot()->replot();
  }
  { PJ_GRAB_OR_SKIP(frame, *sibling->qwtPlot()); }

  QwtPlot& qwt = *failing->qwtPlot();
  QwtPlotCanvas* raster = waitForRasterCanvas(qwt);
  ASSERT_NE(raster, nullptr) << "the failed GPU canvas was not replaced";
  EXPECT_TRUE(failed_canvas.isNull());
  EXPECT_EQ(failures.count(), 1);
  EXPECT_EQ(raster->objectName(), u"qwtCanvas"_s);
  EXPECT_TRUE(raster->testPaintAttribute(QwtPlotCanvas::BackingStore));
  EXPECT_TRUE(raster->hasMouseTracking());
  EXPECT_TRUE(raster->isVisible());

  {
    PJ_GRAB_OR_SKIP(frame, *sibling->qwtPlot());
    const QImage reference = referenceCanvas(*sibling->qwtPlot(), frame.size());
    expectMatchesReference(frame, reference, background(*sibling->qwtPlot()), 200);
  }
  QImage drawn = rasterFrame(*raster);
  expectMatchesReference(drawn, referenceCanvas(qwt, drawn.size(), /*snapped=*/true), background(qwt), 200);

  // The wheel reaches the magnifier and a middle-button drag the panner.
  const QPointF center = QRectF(raster->rect()).center();
  const double width_before = qwt.axisScaleDiv(QwtPlot::xBottom).range();
  QWheelEvent wheel(
      center, raster->mapToGlobal(center), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
      false);
  QApplication::sendEvent(raster, &wheel);
  EXPECT_NE(qwt.axisScaleDiv(QwtPlot::xBottom).range(), width_before) << "the wheel did not zoom";
  const double lower_before = qwt.axisScaleDiv(QwtPlot::xBottom).lowerBound();
  const QPointF end = center + QPointF(80.0, 0.0);
  sendMouse(*raster, QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
  sendMouse(*raster, QEvent::MouseMove, end, Qt::NoButton, Qt::MiddleButton);
  sendMouse(*raster, QEvent::MouseButtonRelease, end, Qt::MiddleButton, Qt::NoButton);
  EXPECT_LT(qwt.axisScaleDiv(QwtPlot::xBottom).lowerBound(), lower_before) << "the drag did not pan";
  drawn = rasterFrame(*raster);
  expectMatchesReference(drawn, referenceCanvas(qwt, drawn.size(), /*snapped=*/true), background(qwt), 100);
}

// A bare PlotRhiCanvas (no PlotWidgetBase to swap it) keeps its failed state:
// clear-only frames, the banner, renderFailed() once, and no pipeline retry
// until the next initialize(); once the cause is gone, that initialize() recovers.
TEST_F(PlotRhiRenderTest, PipelineFailureShowsTheBannerAndRecoversOnInitialize) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new QwtPlot();
  auto* canvas = new PJ::PlotRhiCanvas();
  plot->setCanvas(canvas);
  canvas->setPlot(plot);
  plot->setCanvasBackground(QColor(20, 20, 20));
  plot->setAxisScale(QwtPlot::xBottom, 0.0, 10.0);
  plot->setAxisScale(QwtPlot::yLeft, 0.0, 10.0);
  auto* curve = new QwtPlotCurve();
  curve->setSamples(QVector<QPointF>{{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}});
  curve->setPen(QColor(230, 40, 40), 2.0);
  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);
  curve->attach(plot);
  canvas->setProperty("pjTestFailPipeline", u"color"_s);
  QSignalSpy failures(canvas, &QRhiWidget::renderFailed);
  const auto attempts = [canvas] { return canvas->property("pjTestPipelineAttempts").toInt(); };
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  plot->replot();
  static_cast<void>(QTest::qWaitFor([&failures] { return failures.count() > 0; }, 5000));
  if (canvas->rendererBackend().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("the plot canvas never initialized (no QRhi backend)");
  }
  const QRgb clear = plot->canvasBackground().color().rgb();
  const int attempts_after_failure = attempts();
  EXPECT_GE(attempts_after_failure, 1);
  for (int frame = 0; frame < 3; ++frame) {
    plot->replot();
    const QImage image = grabPresented(*canvas);
    ASSERT_FALSE(image.isNull());
    EXPECT_TRUE(contentPixels(image, clear).empty()) << "a failed canvas must only clear";
  }
  EXPECT_EQ(attempts(), attempts_after_failure) << "a failed canvas retried its pipelines on a frame";
  EXPECT_EQ(failures.count(), 1);
  const auto* banner = canvas->findChild<QLabel*>();
  ASSERT_NE(banner, nullptr);
  EXPECT_TRUE(banner->isVisible());

  // A new render target (a resize: QRhiWidget re-runs initialize()) while the
  // cause persists: one retry, no new signal. Wait for the layout to apply each
  // size, or two resizes can merge into none.
  const auto resize_canvas = [&window, canvas](QSize size) {
    const QSize before = canvas->size();
    window.window().resize(size);
    ASSERT_TRUE(QTest::qWaitFor([canvas, before] { return canvas->size() != before; }, 5000));
  };
  resize_canvas({700, 450});
  plot->replot();
  ASSERT_FALSE(grabPresented(*canvas).isNull());
  EXPECT_GT(attempts(), attempts_after_failure) << "the new render target did not retry";
  const int attempts_after_retry = attempts();
  plot->replot();
  ASSERT_FALSE(grabPresented(*canvas).isNull());
  EXPECT_EQ(attempts(), attempts_after_retry);
  EXPECT_EQ(failures.count(), 1);
  EXPECT_TRUE(banner->isVisible());

  canvas->setProperty("pjTestFailPipeline", QVariant());
  resize_canvas({760, 480});
  plot->replot();
  const QImage recovered = grabPresented(*canvas);
  ASSERT_FALSE(recovered.isNull());
  expectMatchesReference(recovered, referenceCanvas(*plot, recovered.size()), clear, 200);
  EXPECT_FALSE(banner->isVisible());
  EXPECT_EQ(failures.count(), 1);
}

// A text pipeline failure only drops the text: the curves still draw, with no
// banner and no renderFailed(), so the browser (no raster fallback) keeps its
// plots. The frame matches Qwt's rendering of the same plot without a legend.
TEST_F(PlotRhiRenderTest, TextPipelineFailureDropsOnlyTheText) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(plot->qwtPlot());
  ASSERT_NE(canvas, nullptr);
  canvas->setProperty("pjTestFailPipeline", u"text"_s);
  QSignalSpy failures(canvas, &QRhiWidget::renderFailed);
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->setLegendVisible(true);
  plot->addCurve(u"legend entry"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot());
  EXPECT_EQ(failures.count(), 0);
  const auto* banner = canvas->findChild<QLabel*>();
  EXPECT_TRUE(banner == nullptr || !banner->isVisible());
  const QImage with_legend = referenceCanvas(*plot->qwtPlot(), frame.size());
  plot->setLegendVisible(false);
  const QImage without_legend = referenceCanvas(*plot->qwtPlot(), frame.size());
  const QRgb clear = background(*plot->qwtPlot());
  ASSERT_GT(contentPixels(with_legend, clear).size(), contentPixels(without_legend, clear).size() + 50)
      << "the fixture's legend drew too little to tell apart";
  expectMatchesReference(frame, without_legend, clear, 200);
}

// The fallback deletes the failed canvas, so it waits while a popup or modal
// window is open (a context menu's or dialog's nested loop may own a canvas
// event). A modal dialog here: a Wayland popup needs prior input to show.
TEST_F(PlotRhiRenderTest, FallbackWaitsForAnOpenModalWindow) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  { PJ_GRAB_OR_SKIP(frame, *plot->qwtPlot()); }

  QDialog dialog(&window.window());
  dialog.open();
  ASSERT_TRUE(QTest::qWaitFor([] { return QApplication::activeModalWidget() != nullptr; }, 5000));
  const QPointer<PJ::PlotRhiCanvas> canvas = rhiCanvas(plot->qwtPlot());
  ASSERT_NE(canvas, nullptr);
  QSignalSpy failures(canvas.data(), &QRhiWidget::renderFailed);
  canvas->setProperty("pjTestFailPipeline", u"color"_s);
  canvas->setSampleCount(canvas->sampleCount() == 1 ? 4 : 1);  // a new target format re-runs createPipeline
  plot->qwtPlot()->replot();
  ASSERT_TRUE(QTest::qWaitFor([&failures] { return failures.count() > 0; }, 5000));
  QTest::qWait(400);
  EXPECT_FALSE(canvas.isNull()) << "the canvas was swapped while the dialog was open";

  dialog.close();
  QwtPlotCanvas* raster = waitForRasterCanvas(*plot->qwtPlot());
  ASSERT_NE(raster, nullptr) << "the swap did not happen after the dialog closed";
  EXPECT_TRUE(canvas.isNull());
}

// A canvas that gets no QRhi at all (its API differs from the one an OpenGL
// widget already pinned the window to) also ends up on the raster canvas.
TEST_F(PlotRhiRenderTest, NoQRhiFallsBackToTheRasterCanvas) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* host = new QWidget();
  auto* layout = new QHBoxLayout(host);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(new QOpenGLWidget());
  PlotWindow window(host, {900, 400});
  ASSERT_TRUE(window.exposed());
  auto* plot = new TestPlot();
  rhiCanvas(plot->qwtPlot())->setApi(QRhiWidget::Api::Null);
  layout->addWidget(plot);
  prepare(*plot);
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  plot->qwtPlot()->replot();
  QwtPlotCanvas* raster = waitForRasterCanvas(*plot->qwtPlot());
  ASSERT_NE(raster, nullptr) << "the canvas without a QRhi was not replaced";
  const QImage drawn = rasterFrame(*raster);
  expectMatchesReference(
      drawn, referenceCanvas(*plot->qwtPlot(), drawn.size(), /*snapped=*/true), background(*plot->qwtPlot()), 200);
}

// The grid and marker lines sit on whole device pixels as Qwt puts them, not
// blurred across two rows, and the cosmetic grid pens keep Qt's cadence: dotted
// 1 on / 2 off, dashed 4 on / 2 off, in device pixels. The 1 px canvas frame is
// drawn in the palette's WindowText, as the Qwt canvases draw it.
TEST_F(PlotRhiRenderTest, GridAndMarkerLinesMatchQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->setGridVisible(true);
  QwtPlot& qwt = *plot->qwtPlot();
  const QwtPlotItemList grids = qwt.itemList(QwtPlotItem::Rtti_PlotGrid);
  ASSERT_EQ(grids.size(), 1);
  auto* grid = static_cast<QwtPlotGrid*>(grids.front());
  ASSERT_EQ(grid->majorPen().widthF(), 0.0) << "PlotWidgetBase's grid pens are cosmetic";
  ASSERT_EQ(grid->majorPen().style(), Qt::DashLine);
  ASSERT_EQ(grid->minorPen().style(), Qt::DotLine);
  // A colour the checks can tell from the background; same pens otherwise.
  const QColor grid_ink(40, 90, 230);
  grid->setMajorPen(QPen(grid_ink, 0.0, Qt::DashLine));
  grid->setMinorPen(QPen(grid_ink, 0.0, Qt::DotLine));
  auto* marker = new QwtPlotMarker();
  marker->setLineStyle(QwtPlotMarker::VLine);
  const QColor marker_ink(230, 40, 40);
  marker->setLinePen(marker_ink, 0.0);
  marker->setValue(3.37, 5.0);
  marker->attach(&qwt);
  qwt.replot();
  PJ_GRAB_OR_SKIP(frame, qwt);
  const QRgb clear = background(qwt);
  expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), clear, 1000, /*min_colour_match=*/0.99);

  // Device rows and columns of every grid line and of the marker, where Qwt
  // puts them (the logical pixel rounded).
  const qreal dpr = static_cast<qreal>(frame.width()) / qwt.canvas()->width();
  const auto device = [dpr](qreal logical) { return static_cast<int>(std::lround(std::round(logical) * dpr)); };
  const auto lines = [&](QwtAxisId axis) {
    std::vector<int> positions;
    const QwtScaleMap map = qwt.canvasMap(axis);
    for (int type : {QwtScaleDiv::MajorTick, QwtScaleDiv::MediumTick, QwtScaleDiv::MinorTick}) {
      for (double tick : qwt.axisScaleDiv(axis).ticks(type)) {
        positions.push_back(device(map.transform(tick)));
      }
    }
    return positions;
  };
  std::vector<int> columns = lines(QwtPlot::xBottom);
  const int marker_column = device(qwt.canvasMap(QwtPlot::xBottom).transform(3.37));
  columns.push_back(marker_column);
  const std::vector<int> rows = lines(QwtPlot::yLeft);
  const auto crossing = [](const std::vector<int>& positions, int at) {
    return std::find(positions.begin(), positions.end(), at) != positions.end();
  };
  const int border = static_cast<int>(std::ceil(2.0 * dpr));
  const auto cadence_along_row = [&](double tick) {
    const int row = device(qwt.canvasMap(QwtPlot::yLeft).transform(tick));
    std::vector<int> inked;
    for (int x = border; x < frame.width() - border; ++x) {
      inked.push_back(
          crossing(columns, x) ? -1 : (inkCoverage(frame.pixel(x, row), clear, grid_ink.rgb()) > 0.5 ? 1 : 0));
    }
    return inked;
  };
  const QwtScaleDiv& y_division = qwt.axisScaleDiv(QwtPlot::yLeft);
  ASSERT_FALSE(y_division.ticks(QwtScaleDiv::MinorTick).isEmpty());
  ASSERT_GE(y_division.ticks(QwtScaleDiv::MajorTick).size(), 3);
  const double minor_tick = y_division.ticks(QwtScaleDiv::MinorTick).at(1);
  const double major_tick = y_division.ticks(QwtScaleDiv::MajorTick).at(1);
  const double minor_match = cadenceMatch(cadence_along_row(minor_tick), 3, 1);
  const double major_match = cadenceMatch(cadence_along_row(major_tick), 6, 4);
  std::printf("[grid] dpr=%.1f minor 1/2 match=%.3f major 4/2 match=%.3f\n", dpr, minor_match, major_match);
  EXPECT_GE(minor_match, 0.95) << "the dotted minor grid is not 1 on / 2 off device pixels";
  EXPECT_GE(major_match, 0.95) << "the dashed major grid is not 4 on / 2 off device pixels";

  // The marker line: its ink in one device column, not split across two.
  std::array<double, 7> column_ink{};
  for (int y = border; y < frame.height() - border; ++y) {
    if (crossing(rows, y)) {
      continue;
    }
    for (int dx = -3; dx <= 3; ++dx) {
      column_ink[static_cast<std::size_t>(dx + 3)] +=
          inkCoverage(frame.pixel(marker_column + dx, y), clear, marker_ink.rgb());
    }
  }
  const double total = std::accumulate(column_ink.begin(), column_ink.end(), 0.0);
  ASSERT_GT(total, 10.0) << "no marker line near column " << marker_column;
  EXPECT_GE(column_ink[3] / total, 0.9) << "the marker line is blurred across columns";

  // The frame: the outermost logical pixel ring.
  const QRgb frame_ink = qwt.canvas()->palette().color(QPalette::WindowText).rgb();
  for (const QPoint& point :
       {QPoint(0, frame.height() / 2), QPoint(frame.width() / 2, 0), QPoint(frame.width() - 1, frame.height() / 3),
        QPoint(frame.width() / 3, frame.height() - 1)}) {
    EXPECT_LE(maxChannelDelta(frame.pixel(point), frame_ink), 8) << "no frame at " << point.x() << "," << point.y();
  }
}

// Every edge is antialiased over about one device pixel: across a wide,
// gently sloped line each device column holds the line's exact coverage
// (width / cos(slope)), with partial pixels only at the two edges, one or two
// per edge, and an aliased edge (no partial pixel) in almost no column.
TEST_F(PlotRhiRenderTest, EdgeAntialiasingRampsCoverageAcrossOneDevicePixel) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  const QColor ink(20, 20, 20);
  auto* info = plot->addCurve(u"slope"_s, series({{0.5, 4.0}, {9.5, 5.0}}), ink);
  ASSERT_NE(info, nullptr);
  QPen pen = info->curve->pen();
  pen.setColor(ink);
  // Not a whole number of device pixels at DPR 1 or 2: the two edges never
  // both land on a pixel boundary, where no pixel is partial however smooth.
  pen.setWidthF(5.3);
  info->curve->setPen(pen);
  QwtPlot& qwt = *plot->qwtPlot();
  qwt.replot();
  PJ_GRAB_OR_SKIP(frame, qwt);
  const QRgb clear = background(qwt);
  expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), clear, 1000);

  const qreal dpr = static_cast<qreal>(frame.width()) / qwt.canvas()->width();
  const QwtScaleMap x_map = qwt.canvasMap(QwtPlot::xBottom);
  const QwtScaleMap y_map = qwt.canvasMap(QwtPlot::yLeft);
  const QLineF line(x_map.transform(0.5), y_map.transform(4.0), x_map.transform(9.5), y_map.transform(5.0));
  const double expected_ink = 5.3 * dpr * line.length() / std::abs(line.dx());
  int columns = 0;
  int aliased_columns = 0;
  int wide_ramps = 0;
  double worst_error = 0.0;
  for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
    double column_ink = 0.0;
    int partial = 0;
    for (int y = 2; y < frame.height() - 2; ++y) {
      const double coverage = inkCoverage(frame.pixel(x, y), clear, ink.rgb());
      column_ink += coverage;
      partial += coverage > 0.03 && coverage < 0.97 ? 1 : 0;
    }
    ++columns;
    aliased_columns += partial == 0 ? 1 : 0;
    wide_ramps += partial > 4 ? 1 : 0;
    worst_error = std::max(worst_error, std::abs(column_ink - expected_ink));
  }
  std::printf(
      "[edge-aa] dpr=%.1f expected=%.3f worst_error=%.3f aliased=%d wide=%d of %d\n", dpr, expected_ink, worst_error,
      aliased_columns, wide_ramps, columns);
  EXPECT_LE(worst_error, 0.25) << "a column's coverage differs from the line's width";
  EXPECT_LE(aliased_columns, columns / 20) << "the edges are aliased";
  EXPECT_EQ(wide_ramps, 0) << "an edge ramp spans more than two device pixels";
}

// GPU dashes follow Qt's patterns: each dashed style, cosmetic (per-pixel
// pattern, no caps) and 2 px wide (square caps lengthen each dash), puts its
// dashes where Qwt's raster does.
TEST_F(PlotRhiRenderTest, DashPatternsMatchQwt) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot, {720, 480});
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  const std::array<Qt::PenStyle, 4> styles = {Qt::DashLine, Qt::DotLine, Qt::DashDotLine, Qt::DashDotDotLine};
  const QColor ink(230, 40, 40);
  std::vector<double> levels;
  for (std::size_t index = 0; index < styles.size() * 2; ++index) {
    const double y = 1.0 + static_cast<double>(index);
    levels.push_back(y);
    auto* info = plot->addCurve(u"dash%1"_s.arg(index), series({{0.5, y}, {9.5, y}}), ink);
    ASSERT_NE(info, nullptr);
    info->curve->setPen(QPen(ink, index < styles.size() ? 0.0 : 2.0, styles[index % styles.size()]));
  }
  QwtPlot& qwt = *plot->qwtPlot();
  qwt.replot();
  PJ_GRAB_OR_SKIP(frame, qwt);
  const QRgb clear = background(qwt);
  const QImage reference = referenceCanvas(qwt, frame.size());
#ifdef Q_OS_WIN
  // D3D11 (WARP) ends three of the 2 px dashed lines one 2-3 px dash after Qwt's
  // last one (9 strays); llvmpipe, lavapipe and Metal match exactly.
  constexpr std::size_t kMaxStrays = 12;
#else
  constexpr std::size_t kMaxStrays = 2;
#endif
  expectMatchesReference(frame, reference, clear, 1000, 0.98, kMaxStrays);

  // Along each line: the columns whose ink (summed across the line) is at least
  // half the solid width, in the frame and in Qwt's raster.
  const qreal dpr = static_cast<qreal>(frame.width()) / qwt.canvas()->width();
  const QwtScaleMap x_map = qwt.canvasMap(QwtPlot::xBottom);
  const QwtScaleMap y_map = qwt.canvasMap(QwtPlot::yLeft);
  const int first = static_cast<int>(std::ceil(x_map.transform(1.0) * dpr));
  const int last = static_cast<int>(std::floor(x_map.transform(9.0) * dpr));
  for (std::size_t index = 0; index < levels.size(); ++index) {
    const int row = static_cast<int>(std::lround(y_map.transform(levels[index]) * dpr));
    const auto dashes = [&](const QImage& image) {
      std::vector<double> ink_per_column;
      for (int x = first; x < last; ++x) {
        double column = 0.0;
        for (int y = row - 4; y <= row + 4; ++y) {
          column += inkCoverage(image.pixel(x, y), clear, ink.rgb());
        }
        ink_per_column.push_back(column);
      }
      const double solid = *std::max_element(ink_per_column.begin(), ink_per_column.end());
      std::vector<bool> on;
      for (double column : ink_per_column) {
        on.push_back(column >= 0.5 * solid);
      }
      return on;
    };
    const std::vector<bool> drawn = dashes(frame);
    const std::vector<bool> expected = dashes(reference);
    // The best of three phases: Apple's software OpenGL at DPR 2 cuts the
    // cosmetic patterns one device pixel later (a tie at the pixel centre).
    double agreement = 0.0;
    for (int shift = -1; shift <= 1; ++shift) {
      std::size_t agree = 0;
      std::size_t compared = 0;
      for (std::size_t x = 1; x + 1 < drawn.size(); ++x) {
        agree += drawn[x] == expected[static_cast<std::size_t>(static_cast<int>(x) + shift)] ? 1U : 0U;
        ++compared;
      }
      agreement = std::max(agreement, static_cast<double>(agree) / static_cast<double>(compared));
    }
    std::printf("[dash] line=%zu agreement=%.3f\n", index, agreement);
    EXPECT_GE(agreement, 0.9) << "line " << index << " (style " << static_cast<int>(styles[index % styles.size()])
                              << (index < styles.size() ? ", cosmetic" : ", 2 px") << ") dashes differ from Qwt's";
  }
}

// Moving the playback tracker (PlotWidget::setTrackerPosition) rebuilds only
// the markers and the changed text: the curve layer stays, and the legend's
// raster is reused. A full replot rebuilds the curves.
TEST_F(PlotRhiRenderTest, TrackerOnlyReplotKeepsTheCurveLayer) {
  PJ_SKIP_ON_HEADLESS_QPA();
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/imu/x", PJ::NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  for (int i = 0; i < 200; ++i) {
    writer.appendScalar(*handle, 1'000'000'000 + i * 10'000'000LL, std::sin(i * 0.1));
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  QString key;
  for (const auto& curve : catalog.curves()) {
    key = curve.name;
  }
  ASSERT_FALSE(key.isEmpty());

  auto* plot = new TestPlotWidget(&session, &catalog);
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  ASSERT_NE(plot->addCurve(key), nullptr);
  plot->zoomOut(/*emit_signal=*/false);
  plot->setGridVisible(true);
  plot->setLegendVisible(true);
  plot->setTrackerParameter(PJ::CurveTracker::kLineOnly);
  QwtPlot& qwt = *plot->qwtPlot();
  const QwtScaleDiv& x_axis = qwt.axisScaleDiv(QwtPlot::xBottom);
  const auto at = [&x_axis](double share) { return x_axis.lowerBound() + share * x_axis.range(); };
  plot->setTrackerPosition(at(0.3));
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  const std::size_t curve_builds = canvas->curveLayerBuildsForTest();
  const std::size_t rasterized = canvas->textLayerRasterizationsForTest();

  for (double share : {0.45, 0.6}) {
    plot->setTrackerPosition(at(share));
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->curveLayerBuildsForTest(), curve_builds) << "a tracker move rebuilt the curves";
    EXPECT_EQ(canvas->textLayerRasterizationsForTest(), rasterized) << "a tracker move re-rasterised the legend";
    // The tracker line moved: the frame matches Qwt's rendering at the new position.
    expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
  }

  // With the value box on, a move re-rasterises that box only.
  plot->setTrackerParameter(PJ::CurveTracker::kValue);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  const std::size_t builds_with_value = canvas->curveLayerBuildsForTest();
  const std::size_t rasterized_with_value = canvas->textLayerRasterizationsForTest();
  plot->setTrackerPosition(at(0.7));
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds_with_value);
  EXPECT_LE(canvas->textLayerRasterizationsForTest(), rasterized_with_value + 1);

  // The adapter's content revision is unchanged: a full replot keeps the curves.
  qwt.replot();
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds_with_value) << "an unchanged full replot rebuilt the curves";
  // New samples reach the adapter (samplesIngested) and rebuild them.
  writer.appendScalar(*handle, 1'000'000'000 + 200 * 10'000'000LL, 5.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  qwt.replot();
  {
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds_with_value + 1) << "new samples kept stale curves";
    expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
  }
}

// A revisioned series (RevisionedSeries) keeps its geometry across an unchanged
// full replot, and every in-place change a replot carries still rebuilds: a
// revision bump, a grid flag, a symbol edited in place, a log axis. Plain Qwt data
// has no revision and rebuilds on every full replot.
TEST_F(PlotRhiRenderTest, FullReplotRebuildsOnlyChangedContent) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  auto* data = new Revisioned(QVector<QPointF>{{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}});
  PJ::PlotWidgetBase::CurveInfo* info = plot->addCurve(u"ramp"_s, data, QColor(230, 40, 40));
  ASSERT_NE(info, nullptr);
  QwtPlot& qwt = *plot->qwtPlot();
  qwt.replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  std::size_t builds = canvas->curveLayerBuildsForTest();
  const auto expect_after_replot = [&](const char* what, bool rebuilt) {
    qwt.replot();
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds + (rebuilt ? 1 : 0)) << what;
    builds = canvas->curveLayerBuildsForTest();
    expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
  };
  expect_after_replot("an unchanged replot rebuilt the curves", false);

  data->setSamples(QVector<QPointF>{{0.5, 1.0}, {2.0, 2.0}, {4.0, 8.0}, {9.0, 9.0}});
  data->bump();
  expect_after_replot("a new revision kept stale curves", true);

  plot->setGridVisible(true);
  expect_after_replot("showing the grid kept the curves", true);
  auto* grid = [&qwt]() -> QwtPlotGrid* {
    for (QwtPlotItem* item : qwt.itemList(QwtPlotItem::Rtti_PlotGrid)) {
      return static_cast<QwtPlotGrid*>(item);
    }
    return nullptr;
  }();
  ASSERT_NE(grid, nullptr);
  grid->enableXMin(!grid->xMinEnabled());
  expect_after_replot("a grid flag changed in place kept the grid", true);

  auto* symbol = new QwtSymbol(QwtSymbol::Rect, QBrush(Qt::blue), QPen(Qt::black), QSize(5, 5));
  info->curve->setSymbol(symbol);
  expect_after_replot("a new symbol kept the curves", true);
  symbol->setSize(QSize(9, 9));
  expect_after_replot("a symbol resized in place kept the curves", true);

  // The range reaches the axis widget first: the engine switch lays the axis
  // out at once, and a log transformation of the old ticks (<= 0) feeds a NaN to
  // an int cast inside Qwt (UBSan).
  qwt.setAxisScale(QwtPlot::yLeft, 0.5, 10.0);
  qwt.updateAxes();
  qwt.setAxisScaleEngine(QwtPlot::yLeft, new QwtLogScaleEngine());
  expect_after_replot("a log axis kept the linear geometry", true);

  // The canvas does not draw area fills, so only the rebuild is checked.
  info->curve->setBrush(QBrush(Qt::green));
  qwt.replot();
  {
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds + 1) << "a new brush kept the curves";
  }
  info->curve->setBrush(Qt::NoBrush);
  qwt.replot();
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  builds = canvas->curveLayerBuildsForTest();

  // Plain Qwt data: every full replot rebuilds.
  plot->addCurve(u"floor"_s, series({{1.0, 1.0}, {9.0, 2.0}}), QColor(40, 90, 230));
  expect_after_replot("a new curve kept the curves", true);
  expect_after_replot("a replot of unrevisioned data kept the curves", true);
}

// Scales that differ only in which list holds a tick, or only in a transformation's
// parameter, are different views: a full replot of unchanged content rebuilds.
TEST_F(PlotRhiRenderTest, FullReplotSeesTickTypesAndTransformations) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->addCurve(
      u"ramp"_s, new Revisioned(QVector<QPointF>{{0.5, -6.0}, {2.0, 9.5}, {4.0, -3.0}, {9.0, 5.0}}),
      QColor(230, 40, 40));
  plot->setGridVisible(true);
  QwtPlot& qwt = *plot->qwtPlot();
  auto* grid = static_cast<QwtPlotGrid*>(qwt.itemList(QwtPlotItem::Rtti_PlotGrid).value(0));
  ASSERT_NE(grid, nullptr);
  grid->enableXMin(false);  // medium ticks draw no line; major ones do
  grid->enableYMin(false);
  const QwtScaleDiv y_div(-10.0, 10.0, {}, {}, {-10.0, 0.0, 10.0});
  qwt.setAxisScaleDiv(QwtPlot::xBottom, QwtScaleDiv(-1.0, 10.0, {}, {}, {0.0, 1.0, 2.0}));
  qwt.setAxisScaleDiv(QwtPlot::yLeft, y_div);
  qwt.replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  std::size_t builds = canvas->curveLayerBuildsForTest();
  const auto expect_rebuilt_after_replot = [&](const char* what) {
    qwt.replot();
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds + 1) << what;
    builds = canvas->curveLayerBuildsForTest();
    expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
  };

  // The same tick values in the same order: the major line at 0 disappears.
  qwt.setAxisScaleDiv(QwtPlot::xBottom, QwtScaleDiv(-1.0, 10.0, {}, {0.0}, {1.0, 2.0}));
  expect_rebuilt_after_replot("a major tick turned medium kept its grid line");

  // Power transformations map the midpoint of symmetric bounds alike and every
  // other value differently.
  for (const double exponent : {2.0, 3.0}) {
    auto* engine = new QwtLinearScaleEngine();
    engine->setTransformation(new QwtPowerTransform(exponent));
    qwt.setAxisScaleEngine(QwtPlot::yLeft, engine);
    qwt.setAxisScaleDiv(QwtPlot::yLeft, y_div);
    expect_rebuilt_after_replot("a new power exponent kept the old mapping");
  }
}

// A curve removed and re-added with new data whose series lands at the freed
// one's address (a transform applied in place) rebuilds: revisions are unique
// for the process, not per series.
TEST_F(PlotRhiRenderTest, ReplacedSeriesAtOneAddressRebuilds) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  QwtPlot& qwt = *plot->qwtPlot();
  auto* first = new SameAddressRevisioned(QVector<QPointF>{{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}});
  ASSERT_NE(plot->addCurve(u"ramp"_s, first, QColor(230, 40, 40)), nullptr);
  qwt.replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  const std::size_t builds = canvas->curveLayerBuildsForTest();

  plot->removeCurve(u"ramp"_s);
  auto* second = new SameAddressRevisioned(QVector<QPointF>{{0.5, 1.0}, {2.0, 2.0}, {4.0, 8.0}, {9.0, 9.0}});
  ASSERT_EQ(static_cast<void*>(second), static_cast<void*>(first));
  ASSERT_NE(plot->addCurve(u"ramp"_s, second, QColor(230, 40, 40)), nullptr);
  qwt.replot();
  PJ_GRAB_OR_SKIP(frame, qwt);
  EXPECT_GT(canvas->curveLayerBuildsForTest(), builds) << "the replaced series kept the old geometry";
  expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
}

// A curve subclass with its own RTTI is drawn like any curve, so its content is
// tracked like one: plain Qwt data changed in place, same count, rebuilds on replot.
TEST_F(PlotRhiRenderTest, CustomRttiCurveRebuildsOnReplot) {
  PJ_SKIP_ON_HEADLESS_QPA();
  class CustomRttiCurve : public QwtPlotCurve {
   public:
    [[nodiscard]] int rtti() const override {
      return QwtPlotItem::Rtti_PlotUserItem + 1;
    }
  };
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  QwtPlot& qwt = *plot->qwtPlot();
  auto* data = new QwtPointSeriesData(QVector<QPointF>{{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}});
  auto* curve = new CustomRttiCurve();  // owned by the plot once attached
  curve->setPen(QColor(230, 40, 40), 2.0);
  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);  // as the canvas draws, for the reference
  curve->setData(data);
  curve->attach(&qwt);
  qwt.replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  { PJ_GRAB_OR_SKIP(frame, qwt); }
  const std::size_t builds = canvas->curveLayerBuildsForTest();

  data->setSamples(QVector<QPointF>{{0.5, 1.0}, {2.0, 2.0}, {4.0, 8.0}, {9.0, 9.0}});
  qwt.replot();
  PJ_GRAB_OR_SKIP(frame, qwt);
  EXPECT_EQ(canvas->curveLayerBuildsForTest(), builds + 1) << "a custom-RTTI curve kept stale geometry";
  expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
}

// The canvas renders at one sample per pixel into a colour-only target: 4 bytes
// per device pixel, no depth-stencil. A sample count set by hand gets an MSAA
// colour buffer and its resolve, still without depth-stencil.
TEST_F(PlotRhiRenderTest, RendersIntoOneColourOnlyTarget) {
  PJ_SKIP_ON_HEADLESS_QPA();
  auto* plot = new TestPlot();
  PlotWindow window(plot);
  ASSERT_TRUE(window.exposed());
  prepare(*plot);
  plot->addCurve(u"ramp"_s, series({{0.5, 6.0}, {2.0, 9.5}, {4.0, 7.0}, {9.0, 1.0}}), QColor(230, 40, 40));
  QwtPlot& qwt = *plot->qwtPlot();
  qwt.replot();
  PJ::PlotRhiCanvas* canvas = rhiCanvas(&qwt);
  {
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->sampleCount(), 1);
    EXPECT_EQ(canvas->renderTargetBytesPerPixel(), 4);
  }
  canvas->setSampleCount(4);
  qwt.replot();
  {
    PJ_GRAB_OR_SKIP(frame, qwt);
    EXPECT_EQ(canvas->renderTargetBytesPerPixel(), 4 * 4 + 4) << "MSAA colour + resolve, no depth-stencil";
    expectMatchesReference(frame, referenceCanvas(qwt, frame.size()), background(qwt), 200);
  }
}

}  // namespace
