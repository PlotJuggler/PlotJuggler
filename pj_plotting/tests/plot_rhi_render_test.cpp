// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// PlotRhiCanvas on a real display, per QRhi backend (PJ_GRAPHICS_API picks it;
// ctest registers each backend of the platform). Each case renders one kind of plot
// item and compares the presented frame with Qwt's own QPainter rendering of the
// same canvas (QwtPlot::drawCanvas into a QImage), so pixel positions are
// checked against Qwt's scale maps, and a y-flipped or offset frame fails.
// Needs an RHI-capable QPA (wayland/xcb/windows/cocoa); on `offscreen` every case skips, or
// fails under PJ_REQUIRE_GPU_TESTS=1. The failure cases use the
// pjTestFailPipeline hook (PJ_RHI_TEST_HOOKS, test-enabled builds only).

#include <gtest/gtest.h>
#include <qwt_painter.h>
#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_div.h>
#include <qwt_scale_map.h>
#include <qwt_series_data.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

// Qwt's own QPainter rendering of the canvas items, at the frame's resolution.
// Qwt snaps mapped coordinates to whole pixels on screen devices
// (QwtPainter::roundingAlignment()); the RHI canvas keeps them exact, so its
// references are drawn unsnapped: a 1 px line half a pixel off would split
// into two half-covered rows and no colour would match. `snapped` gives the
// on-screen rendering, for the raster canvas.
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
  painter.setRenderHint(QPainter::Antialiasing, true);
  const bool was_snapping = QwtPainter::roundingAlignment();
  QwtPainter::setRoundingAlignment(snapped);
  plot.drawCanvas(&painter);
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

// The 1-device-pixel border is excluded: Qwt draws items in the canvas contents
// rect, the RHI canvas across the whole widget.
ReferenceComparison compareWithReference(const QImage& frame, const QImage& reference, QRgb background, int radius) {
  const int width = frame.width();
  const int height = frame.height();
  const auto is_foreground = [background](const QImage& image, int x, int y) {
    return channelDelta(image.pixel(x, y), background) > 60;
  };
  std::vector<char> reference_mask(static_cast<std::size_t>(width * height), 0);
  std::vector<char> frame_mask(reference_mask.size(), 0);
  std::vector<char> reference_drawn(reference_mask.size(), 0);
  for (int y = 2; y < height - 2; ++y) {
    for (int x = 2; x < width - 2; ++x) {
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
  for (int y = 2; y < height - 2; ++y) {
    for (int x = 2; x < width - 2; ++x) {
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

// The RHI frame and Qwt's raster agree: nearly every foreground pixel of either
// (`min_colour_match`) has the reference's colour close by, neither misses nor
// adds geometry, the background is exactly the clear colour, and both drew a
// meaningful amount.
void expectMatchesReference(
    const QImage& frame, const QImage& reference, QRgb background, std::size_t min_pixels,
    double min_colour_match = 0.95) {
  ASSERT_EQ(frame.size(), reference.size());
  // PJ_RHI_TEST_DUMP_DIR=<dir> saves both images (CI uploads the dir on failure).
  if (const QString dir = qEnvironmentVariable("PJ_RHI_TEST_DUMP_DIR"); !dir.isEmpty()) {
    const QString base = dir + u"/plot_"_s +
                         QString::fromUtf8(::testing::UnitTest::GetInstance()->current_test_info()->name()) + u'_' +
                         qEnvironmentVariable("PJ_GRAPHICS_API", u"default"_s);
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
  // Measured on llvmpipe/lavapipe at DPR 1 and 2: colour match >= 0.985 (0.90
  // for the aliased symbols, see SymbolsOfEveryStyleMatchQwt), missing <= 0.014 (PlotMarkersItem text), no extra pixel
  // and no stray. A red/blue swap in the vertex colour drops the colour match to 0.00-0.68.
  EXPECT_GE(colour_match, min_colour_match) << "foreground colours differ from Qwt's";
  EXPECT_LE(missing, 0.03) << "reference pixels missing in frame";
  EXPECT_LE(extra, 0.02) << "frame pixels absent from reference";
  EXPECT_LE(c.background_strays, 2U) << "the background is not exactly the clear colour";
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
    PJ::PlotWidgetBase::setOpenGlDisabledOverride(false);
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
  // device pixels). A 1 px gap can straddle two pixels under MSAA, so a dip is
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
  // Qwt draws Rect, Cross, HLine and VLine symbols aliased (qwt_symbol.cpp turns
  // antialiasing off for them); the MSAA canvas does not snap their 2 px strokes
  // to whole pixels, so those edges differ: 0.90 at DPR 1, 0.94 at DPR 2.
  expectMatchesReference(
      frame, referenceCanvas(*plot->qwtPlot(), frame.size()), background(*plot->qwtPlot()), 1000,
      /*min_colour_match=*/0.85);
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
  // The RHI canvas antialiases everything (MSAA); so does this reference.
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

}  // namespace
