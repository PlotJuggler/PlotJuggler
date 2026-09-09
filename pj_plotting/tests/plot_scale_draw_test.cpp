// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QLocale>

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

// "Lines and Dots" renders as plain Lines plus PlotCurve's batched dots pass
// (stock Qwt has no such style; per-sample symbols are pathological on the GL
// paint engine); toggling back to Lines must disable the pass.
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
