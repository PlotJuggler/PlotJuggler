// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Tests for ChartPreviewWidget navigation: wheel zoom-out stops at the fitted data
// extent, panning moves the axes during the drag (and survives its release), and
// constant data still gets a fitted view.

#include <gtest/gtest.h>
#include <qwt_scale_div.h>

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>

namespace {

PJ::ChartPreviewWidget::Series ramp() {
  PJ::ChartPreviewWidget::Series s;
  s.label = "ramp";
  for (int i = 0; i <= 20; ++i) {
    s.points.emplace_back(static_cast<double>(i), static_cast<double>(i));
  }
  return s;
}

void wheel(QWidget* canvas, int delta) {
  const QPointF pos(canvas->width() / 2.0, canvas->height() / 2.0);
  QWheelEvent event(
      pos, canvas->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
      false);
  QApplication::sendEvent(canvas, &event);
}

void mouse(
    QWidget* canvas, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers mods) {
  QMouseEvent event(type, QPointF(pos), canvas->mapToGlobal(QPointF(pos)), button, buttons, mods);
  QApplication::sendEvent(canvas, &event);
}

struct Range {
  double lo;
  double hi;
};

Range xRange(const PJ::ChartPreviewWidget& chart) {
  const QwtScaleDiv& div = chart.axisScaleDiv(QwtPlot::xBottom);
  return {div.lowerBound(), div.upperBound()};
}

Range yRange(const PJ::ChartPreviewWidget& chart) {
  const QwtScaleDiv& div = chart.axisScaleDiv(QwtPlot::yLeft);
  return {div.lowerBound(), div.upperBound()};
}

PJ::ChartPreviewWidget::Series constantSeries(double y) {
  PJ::ChartPreviewWidget::Series s;
  s.label = "constant";
  for (int i = 0; i <= 20; i += 4) {
    s.points.emplace_back(static_cast<double>(i), y);
  }
  return s;
}

class ChartPreviewNavigation : public ::testing::Test {
 protected:
  void SetUp() override {
    chart_.resize(400, 300);
    chart_.setZoomEnabled(true);
    chart_.setSeries({ramp()});
    chart_.show();
    QApplication::processEvents();
  }
  PJ::ChartPreviewWidget chart_;
};

TEST_F(ChartPreviewNavigation, WheelZoomOutNeverExceedsTheFittedExtent) {
  const Range fit = xRange(chart_);
  for (int i = 0; i < 30; ++i) {
    wheel(chart_.canvas(), -120);
  }
  const Range out = xRange(chart_);
  EXPECT_DOUBLE_EQ(out.lo, fit.lo);
  EXPECT_DOUBLE_EQ(out.hi, fit.hi);
}

TEST_F(ChartPreviewNavigation, WheelZoomInStillNarrowsTheView) {
  const Range fit = xRange(chart_);
  wheel(chart_.canvas(), 120);
  const Range in = xRange(chart_);
  EXPECT_LT(in.hi - in.lo, fit.hi - fit.lo);
  EXPECT_GE(in.lo, fit.lo);
  EXPECT_LE(in.hi, fit.hi);
}

TEST_F(ChartPreviewNavigation, CtrlDragMovesTheAxesDuringTheDrag) {
  wheel(chart_.canvas(), 120);  // zoom in so there is room to pan
  const Range before = xRange(chart_);
  QWidget* canvas = chart_.canvas();
  const QPoint start(200, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
  mouse(canvas, QEvent::MouseMove, start + QPoint(-60, 0), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
  const Range during = xRange(chart_);
  EXPECT_GT(during.lo, before.lo) << "dragging left must show later x while the button is still held";
  EXPECT_NEAR(during.hi - during.lo, before.hi - before.lo, 1e-9) << "panning keeps the zoom level";
  mouse(canvas, QEvent::MouseButtonRelease, start + QPoint(-60, 0), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
}

TEST_F(ChartPreviewNavigation, MiddleDragAlsoPansLive) {
  wheel(chart_.canvas(), 120);
  const Range before = xRange(chart_);
  QWidget* canvas = chart_.canvas();
  const QPoint start(200, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseMove, start + QPoint(40, 0), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
  EXPECT_LT(xRange(chart_).lo, before.lo);
  mouse(canvas, QEvent::MouseButtonRelease, start + QPoint(40, 0), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
}

// Rubber-band zoom with the left button, as QwtPicker needs it: press, drag, release.
void rubberBandZoom(QWidget* canvas) {
  const QPoint from(100, 80);
  const QPoint to(260, 200);
  mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseMove, QPoint(180, 140), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
}

TEST_F(ChartPreviewNavigation, MiddleDragAfterRubberBandZoomKeepsThePannedViewOnRelease) {
  const Range fit = xRange(chart_);
  QWidget* canvas = chart_.canvas();
  rubberBandZoom(canvas);
  const Range zoomed = xRange(chart_);
  ASSERT_LT(zoomed.hi - zoomed.lo, fit.hi - fit.lo) << "the rubber band must have zoomed in";

  const QPoint start(200, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseMove, start + QPoint(40, 0), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
  const Range panned = xRange(chart_);
  ASSERT_LT(panned.lo, zoomed.lo);
  mouse(canvas, QEvent::MouseButtonRelease, start + QPoint(40, 0), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);

  const Range after = xRange(chart_);
  EXPECT_DOUBLE_EQ(after.lo, panned.lo) << "releasing the middle button must not step the zoom back";
  EXPECT_DOUBLE_EQ(after.hi, panned.hi);
}

TEST_F(ChartPreviewNavigation, CtrlDragAfterRubberBandZoomKeepsThePannedViewOnRelease) {
  const Range fit = xRange(chart_);
  QWidget* canvas = chart_.canvas();
  rubberBandZoom(canvas);
  const Range zoomed = xRange(chart_);
  ASSERT_LT(zoomed.hi - zoomed.lo, fit.hi - fit.lo) << "the rubber band must have zoomed in";

  const QPoint start(200, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
  mouse(canvas, QEvent::MouseMove, start + QPoint(-40, 0), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
  const Range panned = xRange(chart_);
  ASSERT_GT(panned.lo, zoomed.lo);
  mouse(canvas, QEvent::MouseButtonRelease, start + QPoint(-40, 0), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);

  const Range after = xRange(chart_);
  EXPECT_DOUBLE_EQ(after.lo, panned.lo);
  EXPECT_DOUBLE_EQ(after.hi, panned.hi);
}

TEST_F(ChartPreviewNavigation, WheelZoomInKeepsAViewPannedPastTheFittedExtent) {
  const Range fit = xRange(chart_);
  QWidget* canvas = chart_.canvas();
  wheel(canvas, 120);
  const QPoint start(300, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
  mouse(
      canvas, QEvent::MouseMove, start - QPoint(canvas->width() / 2, 0), Qt::NoButton, Qt::LeftButton,
      Qt::ControlModifier);
  mouse(
      canvas, QEvent::MouseButtonRelease, start - QPoint(canvas->width() / 2, 0), Qt::LeftButton, Qt::NoButton,
      Qt::ControlModifier);
  const Range before = xRange(chart_);
  ASSERT_GT(before.hi, fit.hi) << "the view must extend past the fitted extent";

  wheel(canvas, 120);
  const Range after = xRange(chart_);
  EXPECT_LT(after.hi - after.lo, before.hi - before.lo);
  const double center_before = (before.lo + before.hi) / 2.0;
  const double center_after = (after.lo + after.hi) / 2.0;
  EXPECT_NEAR(center_after, center_before, std::abs(center_before) * 1e-9)
      << "zooming in must not pull the view back inside the fitted extent";
}

TEST_F(ChartPreviewNavigation, ConstantSeriesGetsAFittedViewAndZoomOutLimit) {
  chart_.setSeries({constantSeries(100.0)});
  Range y = yRange(chart_);
  EXPECT_LT(y.lo, 100.0);
  EXPECT_GT(y.hi, 100.0);
  for (int i = 0; i < 30; ++i) {
    wheel(chart_.canvas(), -120);
  }
  y = yRange(chart_);
  EXPECT_LT(y.lo, 100.0);
  EXPECT_GT(y.hi, 100.0);
  EXPECT_GT(y.lo, 50.0) << "the zoom-out limit must be the constant's own extent, not the previous data's";
}

TEST_F(ChartPreviewNavigation, SinglePointSeriesGetsANonDegenerateView) {
  PJ::ChartPreviewWidget::Series point;
  point.label = "point";
  point.points.emplace_back(500.0, 700.0);
  chart_.setSeries({point});
  const Range x = xRange(chart_);
  const Range y = yRange(chart_);
  EXPECT_LT(x.lo, 500.0);
  EXPECT_GT(x.hi, 500.0);
  EXPECT_LT(y.lo, 700.0);
  EXPECT_GT(y.hi, 700.0);
}

TEST_F(ChartPreviewNavigation, SinglePointWithHugeXStillGetsAWidthAboveDoubleResolution) {
  // 1e18 (nanosecond timestamps): a fixed +-1 pad is below double resolution there.
  PJ::ChartPreviewWidget::Series point;
  point.label = "ns";
  point.points.emplace_back(1e18, 0.0);
  chart_.setSeries({point});
  const Range x = xRange(chart_);
  EXPECT_LT(x.lo, 1e18);
  EXPECT_GT(x.hi, 1e18);
}

TEST_F(ChartPreviewNavigation, DisablingNavigationMidPanRestoresTheCursor) {
  QWidget* canvas = chart_.canvas();
  const QPoint start(200, 150);
  mouse(canvas, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
  mouse(canvas, QEvent::MouseMove, start + QPoint(20, 0), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
  ASSERT_EQ(canvas->cursor().shape(), Qt::ClosedHandCursor);
  chart_.setZoomEnabled(false);
  EXPECT_NE(canvas->cursor().shape(), Qt::ClosedHandCursor);
}

TEST(ChartPreviewNavigationDisabled, WithoutZoomTheWheelDoesNothing) {
  PJ::ChartPreviewWidget chart;
  chart.resize(400, 300);
  chart.setSeries({ramp()});
  chart.show();
  QApplication::processEvents();
  const Range fit = xRange(chart);
  wheel(chart.canvas(), 120);
  const Range after = xRange(chart);
  EXPECT_DOUBLE_EQ(after.lo, fit.lo);
  EXPECT_DOUBLE_EQ(after.hi, fit.hi);
}

}  // namespace
