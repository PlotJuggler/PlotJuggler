// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <cmath>

#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotScaleDraw.h"
#include "pj_plotting/PlotWidgetBase.h"

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
