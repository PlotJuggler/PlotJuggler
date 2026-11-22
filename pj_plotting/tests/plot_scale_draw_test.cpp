// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_text.h>

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QTimeZone>
#include <QVector>
#include <algorithm>
#include <cmath>

#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotScaleDraw.h"
#include "pj_plotting/PlotWidgetBase.h"

using namespace Qt::StringLiterals;

namespace {

QString labelText(double value) {
  const PJ::PlotScaleDraw draw;
  return draw.label(value).text();
}

// Locale-sensitive formatting: pin the C locale so expectations are stable
// regardless of the host's decimal separator.
class PlotScaleDrawTest : public ::testing::Test {
 protected:
  void SetUp() override {
    QLocale::setDefault(QLocale::c());
  }
  void TearDown() override {
    QLocale::setDefault(QLocale::system());
  }
};

TEST_F(PlotScaleDrawTest, IntegersHaveNoDecimals) {
  EXPECT_EQ(labelText(0.0), "0");
  EXPECT_EQ(labelText(100.0), "100");
  EXPECT_EQ(labelText(-42.0), "-42");
}

TEST_F(PlotScaleDrawTest, TrailingZerosStripped) {
  EXPECT_EQ(labelText(1.5), "1.5");
  EXPECT_EQ(labelText(0.25), "0.25");
  EXPECT_EQ(labelText(-1.5), "-1.5");
}

// Zeros that are part of the integer portion must survive the trailing-zero
// strip (the fractional zeros and the dot go, "200" stays intact).
TEST_F(PlotScaleDrawTest, IntegerZerosPreserved) {
  EXPECT_EQ(labelText(20.0), "20");
  EXPECT_EQ(labelText(200.0), "200");
  EXPECT_EQ(labelText(1.05), "1.05");
}

TEST_F(PlotScaleDrawTest, RoundsToSixDecimals) {
  EXPECT_EQ(labelText(0.123456789), "0.123457");
  EXPECT_EQ(labelText(1e-9), "0");
}

// Locales with a decimal comma must trim like the C locale does: fractional
// zeros and a bare separator go, group separators and integer digits stay.
TEST_F(PlotScaleDrawTest, DecimalCommaLocale) {
  QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
  EXPECT_EQ(labelText(1.5), "1,5");
  EXPECT_EQ(labelText(2.0), "2");
  EXPECT_EQ(labelText(20.0), "20");
  EXPECT_EQ(labelText(1000.0), "1.000");
}

// Exposes the protected accessors needed by the wiring assertions below.
class TestPlot : public PJ::PlotWidgetBase {
 public:
  using PJ::PlotWidgetBase::applyStyleToCurve;
  using PJ::PlotWidgetBase::qwtPlot;
};

// The formatting only takes effect if QwtPlotPimpl installs PlotScaleDraw on
// every axis; a plain PlotWidgetBase must never fall back to QwtScaleDraw.
TEST_F(PlotScaleDrawTest, InstalledOnAllPlotAxes) {
  TestPlot plot;
  for (const int axis : {QwtPlot::yLeft, QwtPlot::yRight, QwtPlot::xBottom, QwtPlot::xTop}) {
    EXPECT_NE(dynamic_cast<const PJ::PlotScaleDraw*>(plot.qwtPlot()->axisScaleDraw(axis)), nullptr) << axis;
  }
}

// The clock engine works in seconds while QwtDateScaleEngine works in ms: the
// ticks must come back in seconds, on whole-minute boundaries for an hour span,
// inside the caller's exact bounds.
TEST_F(PlotScaleDrawTest, TimeEngineAlignsTicksToClockBoundaries) {
  const PJ::PlotTimeScaleEngine engine(PJ::TimeLabelFormat::kUtcClock);
  const double x1 = 1794066683.25;  // 2026-11-07 15:51:23.25 UTC
  const double x2 = x1 + 3600.0;
  const QwtScaleDiv div = engine.divideScale(x1, x2, 8, 5);
  EXPECT_EQ(div.lowerBound(), x1);
  EXPECT_EQ(div.upperBound(), x2);
  const QList<double> ticks = div.ticks(QwtScaleDiv::MajorTick);
  ASSERT_GE(ticks.size(), 3);
  for (const double tick : ticks) {
    EXPECT_GE(tick, x1);
    EXPECT_LE(tick, x2);
    EXPECT_EQ(std::fmod(tick, 60.0), 0.0) << tick;
  }
}

// Below a second the engine falls back to decimal steps, still in seconds.
TEST_F(PlotScaleDrawTest, TimeEngineSubSecondTicksStayInSeconds) {
  const PJ::PlotTimeScaleEngine engine(PJ::TimeLabelFormat::kUtcClock);
  const QwtScaleDiv div = engine.divideScale(10.0, 10.5, 5, 5);
  const QList<double> ticks = div.ticks(QwtScaleDiv::MajorTick);
  ASSERT_GE(ticks.size(), 2);
  EXPECT_NEAR(ticks[1] - ticks[0], 0.1, 1e-9);
}

// Local time shifts by the zone offset instead of running Qwt in local mode:
// the ticks must still land on whole local minutes.
TEST_F(PlotScaleDrawTest, LocalTimeEngineAlignsToLocalMinutes) {
  const PJ::PlotTimeScaleEngine engine(PJ::TimeLabelFormat::kLocalClock);
  const double x1 = 1794066683.25;
  const QwtScaleDiv div = engine.divideScale(x1, x1 + 3600.0, 8, 5);
  const QList<double> ticks = div.ticks(QwtScaleDiv::MajorTick);
  ASSERT_GE(ticks.size(), 3);
  for (const double tick : ticks) {
    const QDateTime local = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(tick), QTimeZone(QTimeZone::LocalTime));
    EXPECT_EQ(local.time().second(), 0) << tick;
    EXPECT_EQ(std::fmod(tick, 1.0), 0.0) << tick;
  }
}

// The date-time toggle swaps only the time axis, and an XY plot keeps numbers.
TEST_F(PlotScaleDrawTest, TimeLabelFormatSwapsOnlyTheTimeAxis) {
  TestPlot plot;
  auto x_draw = [&plot]() { return plot.qwtPlot()->axisScaleDraw(QwtPlot::xBottom); };
  plot.setTimeLabelFormat(PJ::TimeLabelFormat::kLocalClock);
  EXPECT_NE(dynamic_cast<const PJ::PlotTimeScaleDraw*>(x_draw()), nullptr);
  EXPECT_NE(dynamic_cast<const PJ::PlotTimeScaleEngine*>(plot.qwtPlot()->axisScaleEngine(QwtPlot::xBottom)), nullptr);
  EXPECT_NE(dynamic_cast<const PJ::PlotScaleDraw*>(plot.qwtPlot()->axisScaleDraw(QwtPlot::yLeft)), nullptr);

  plot.setModeXY(true);
  EXPECT_NE(dynamic_cast<const PJ::PlotScaleDraw*>(x_draw()), nullptr);
  plot.setModeXY(false);
  EXPECT_NE(dynamic_cast<const PJ::PlotTimeScaleDraw*>(x_draw()), nullptr);

  plot.setTimeLabelFormat(PJ::TimeLabelFormat::kSeconds);
  EXPECT_NE(dynamic_cast<const PJ::PlotScaleDraw*>(x_draw()), nullptr);
}

// The tracker's time line reads like the X axis: clock time while the date-time toggle is on, seconds otherwise.
TEST_F(PlotScaleDrawTest, TrackerTimeFollowsTheAxisFormat) {
  TestPlot plot;
  PJ::CurveTracker tracker(plot.qwtPlot(), Qt::red);
  const double instant = 1700000000.5;  // 2023-11-14 22:13:20.5 UTC

  tracker.setPosition(QPointF(instant, 0.0));
  EXPECT_TRUE(tracker.valueBoxText().contains(u"time : 1700000000.5"_s)) << tracker.valueBoxText().toStdString();

  plot.setTimeLabelFormat(PJ::TimeLabelFormat::kUtcClock);
  tracker.redraw();
  EXPECT_TRUE(tracker.valueBoxText().contains(u"time : 22:13:20.5"_s)) << tracker.valueBoxText().toStdString();

  plot.setModeXY(true);
  tracker.redraw();
  EXPECT_TRUE(tracker.valueBoxText().contains(u"time : 1700000000.5"_s)) << "an XY plot keeps seconds";
}

// The value box glides toward the curves' value at a capped speed instead of jumping with every sample, and keeps
// moving after positions stop until it arrives; a box shown for the first time, or after a seek, snaps.
TEST_F(PlotScaleDrawTest, TrackerValueBoxGlidesToTheValue) {
  TestPlot plot;
  plot.resize(400, 300);
  plot.show();
  plot.qwtPlot()->setAxisScale(QwtPlot::xBottom, 0.0, 10.0);
  plot.qwtPlot()->setAxisScale(QwtPlot::yLeft, 0.0, 100.0);
  plot.qwtPlot()->replot();
  QApplication::processEvents();
  auto* curve = new QwtPlotCurve(u"step"_s);
  curve->setSamples(QVector<QPointF>{{0.0, 10.0}, {5.0, 10.0}, {5.1, 90.0}, {10.0, 90.0}});
  curve->attach(plot.qwtPlot());
  PJ::CurveTracker tracker(plot.qwtPlot(), Qt::red);
  int moves = 0;
  int moves_during_playback = 0;
  QElapsedTimer since_position;
  since_position.start();
  QObject::connect(&tracker, &PJ::CurveTracker::valueBoxMoved, [&]() {
    ++moves;
    // The glide timer must not repaint between positions that keep arriving at PlaybackEngine's ~33 ms tick.
    if (since_position.elapsed() < 40) {
      ++moves_during_playback;
    }
  });

  tracker.setPosition(QPointF(4.95, 0.0));
  EXPECT_DOUBLE_EQ(tracker.valueBoxY(), 10.0);

  // Playback-sized steps (a few pixels) across the curve's jump glide.
  for (int tick = 0; tick < 8; ++tick) {
    tracker.setPosition(QPointF(5.15 + 0.005 * tick, 0.0));
    since_position.restart();
    while (since_position.elapsed() < 33) {
      QApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
    }
  }
  EXPECT_LT(tracker.valueBoxY(), 90.0) << "jumped instead of gliding";
  EXPECT_GT(tracker.valueBoxY(), 10.0) << "positions did not advance the glide";
  EXPECT_EQ(moves_during_playback, 0);

  QElapsedTimer clock;
  clock.start();
  while (tracker.valueBoxY() != 90.0 && clock.elapsed() < 10000) {
    QApplication::processEvents(QEventLoop::WaitForMoreEvents, 20);
  }
  EXPECT_DOUBLE_EQ(tracker.valueBoxY(), 90.0);
  EXPECT_GT(clock.elapsed(), 200) << "arrived faster than the speed cap allows";
  EXPECT_GT(moves, 0);

  // A seek (a click far along the time axis) moves the box at once.
  tracker.setPosition(QPointF(1.0, 0.0));
  EXPECT_DOUBLE_EQ(tracker.valueBoxY(), 10.0);
}

// The value box is centred on the sampled value: at a sample on the top or bottom edge of the view, half of it would
// fall outside the canvas and be clipped.
TEST_F(PlotScaleDrawTest, TrackerValueBoxStaysInsideTheCanvas) {
  TestPlot plot;
  plot.resize(600, 200);
  plot.show();
  QApplication::processEvents();
  QwtPlot* qwt = plot.qwtPlot();
  auto* curve = new QwtPlotCurve(u"speed"_s);
  curve->setSamples(QVector<QPointF>{{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  curve->attach(qwt);
  qwt->setAxisScale(QwtPlot::xBottom, 0.0, 10.0);
  qwt->setAxisScale(QwtPlot::yLeft, 0.0, 10.0);
  qwt->replot();
  const QwtScaleMap y_map = qwt->canvasMap(QwtPlot::yLeft);
  const double canvas_top = std::min(y_map.p1(), y_map.p2());
  const double canvas_bottom = std::max(y_map.p1(), y_map.p2());
  ASSERT_GT(canvas_bottom - canvas_top, 100.0) << "the plot was not laid out";

  PJ::CurveTracker tracker(qwt, Qt::red);
  for (const double x : {5.0, 0.0}) {  // the sample on the top edge, then the one on the bottom edge
    tracker.setPosition(QPointF(x, 0.0));
    ASSERT_TRUE(tracker.valueBoxVisible());
    const QwtPlotMarker* box = nullptr;
    for (const QwtPlotItem* item : qwt->itemList(QwtPlotItem::Rtti_PlotMarker)) {
      const auto* marker = dynamic_cast<const QwtPlotMarker*>(item);
      if (marker != nullptr && !marker->label().isEmpty()) {
        box = marker;
      }
    }
    ASSERT_NE(box, nullptr);
    const double centre = y_map.transform(box->yValue());
    const double half_height = 0.5 * box->label().textSize().height();
    EXPECT_GE(centre - half_height, canvas_top - 0.5) << "x=" << x;
    EXPECT_LE(centre + half_height, canvas_bottom + 0.5) << "x=" << x;
  }
}

// A plot is laid out at a small size first (a new dock) and grows with no new tracker position and no change of view:
// the box kept inside the small canvas must follow the canvas edge, not stay at the value the small canvas gave it.
TEST_F(PlotScaleDrawTest, TrackerValueBoxFollowsACanvasResize) {
  TestPlot plot;
  plot.resize(600, 120);
  plot.show();
  QApplication::processEvents();
  QwtPlot* qwt = plot.qwtPlot();
  auto* curve = new QwtPlotCurve(u"speed"_s);
  curve->setSamples(QVector<QPointF>{{0.0, 0.0}, {10.0, 10.0}});
  curve->attach(qwt);
  qwt->setAxisScale(QwtPlot::xBottom, 0.0, 10.0);
  qwt->setAxisScale(QwtPlot::yLeft, 0.0, 10.0);
  qwt->replot();
  PJ::CurveTracker tracker(qwt, Qt::red);
  tracker.setPosition(QPointF(0.0, 0.0));  // the sample on the bottom edge
  ASSERT_TRUE(tracker.valueBoxVisible());

  plot.resize(600, 500);
  QApplication::processEvents();
  const QwtScaleMap y_map = qwt->canvasMap(QwtPlot::yLeft);
  const double canvas_bottom = std::max(y_map.p1(), y_map.p2());
  const QwtPlotMarker* box = nullptr;
  for (const QwtPlotItem* item : qwt->itemList(QwtPlotItem::Rtti_PlotMarker)) {
    const auto* marker = dynamic_cast<const QwtPlotMarker*>(item);
    if (marker != nullptr && !marker->label().isEmpty()) {
      box = marker;
    }
  }
  ASSERT_NE(box, nullptr);
  const double box_bottom = y_map.transform(box->yValue()) + 0.5 * box->label().textSize().height();
  EXPECT_NEAR(box_bottom, canvas_bottom, 1.0);
}

// "Lines and Dots" renders as plain Lines plus PlotCurve's batched dots pass
// (stock Qwt has no such style, and no per-sample symbol stands in for it);
// toggling back to Lines must disable the pass.
TEST_F(PlotScaleDrawTest, LinesAndDotsMapsToLinesPlusBatchedDots) {
  TestPlot plot;
  PJ::PlotCurve curve(QStringLiteral("curve"));
  plot.applyStyleToCurve(&curve, PJ::PlotWidgetBase::kLinesAndDots);
  EXPECT_EQ(curve.style(), QwtPlotCurve::Lines);
  EXPECT_EQ(curve.symbol(), nullptr);
  EXPECT_GT(curve.dotWidth(), 0.0);
  plot.applyStyleToCurve(&curve, PJ::PlotWidgetBase::kLines);
  EXPECT_EQ(curve.style(), QwtPlotCurve::Lines);
  EXPECT_EQ(curve.symbol(), nullptr);
  EXPECT_EQ(curve.dotWidth(), 0.0);
}

}  // namespace
