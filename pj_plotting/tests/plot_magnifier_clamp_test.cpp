// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The PlotMagnifier enforces a minimum on-screen X width of 2 ns on a TIME axis.
// Saved viewports persist X as rounded integer nanoseconds, so a window narrower
// than 2 ns would round to a degenerate (equal-edge) range that cannot survive
// save/load; the clamp keeps every reachable interactive zoom above that floor.
// An XY plot's X is a data value with no such quantization, so it is not clamped.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_scale_div.h>

#include <QApplication>
#include <QRectF>
#include <QWheelEvent>
#include <QtGlobal>
#include <cmath>

#include "pj_plotting/PlotMagnifier.h"

namespace {

// A QwtPlot whose canvas carries a PlotMagnifier, framed on a realistic ~1 s
// window at epoch scale (so the clamp's ns-domain floor is the interesting bound,
// not a coordinate-precision artifact).
struct MagnifierFixture {
  QwtPlot plot;
  PJ::PlotMagnifier magnifier{plot.canvas()};

  MagnifierFixture() {
    plot.resize(800, 400);
    plot.setAxisScale(QwtPlot::xBottom, 0.0, 1.0);  // 1 s window
    plot.setAxisScale(QwtPlot::yLeft, -1.0, 1.0);
    plot.updateAxes();
    plot.replot();
  }

  // In the live widget the magnifier's rescaled() signal drives a replot, which is
  // what makes canvasMap (the input to the NEXT rescale) reflect the applied scale.
  // Mirror that here: rescale, then updateAxes+replot so a subsequent rescale reads
  // the just-zoomed window rather than a stale map.
  void zoomX(double factor) {
    magnifier.rescale(factor, PJ::PlotMagnifier::kXAxis);
    plot.updateAxes();
    plot.replot();
  }

  [[nodiscard]] double xWidth() {
    plot.updateAxes();
    const QwtScaleDiv& div = plot.axisScaleDiv(QwtPlot::xBottom);
    return std::abs(div.upperBound() - div.lowerBound());
  }

  // Plant the zoom center at the axis midpoint by sending one wheel notch at the
  // canvas center (widgetWheelEvent sets mouse_position_ from that pixel).
  void plantMouseAtCenter() {
    QWidget* canvas = plot.canvas();
    const QPointF pos(canvas->width() / 2.0, canvas->height() / 2.0);
    QWheelEvent event(
        pos, canvas->mapToGlobal(pos.toPoint()), QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &event);
    plot.updateAxes();
    plot.replot();
  }
};

}  // namespace

// A single huge zoom-in on a time axis stops at the 2 ns floor, never below it.
TEST(PlotMagnifierClamp, TimeAxisCannotZoomBelowTwoNanoseconds) {
  MagnifierFixture fixture;
  fixture.magnifier.setTimeXAxis(true);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);

  // Zoom in by 1e10x in one step: 1 s -> would be 1e-10 s (0.1 ns) without clamp.
  fixture.zoomX(1.0e10);

  const double width = fixture.xWidth();
  EXPECT_GE(width, 2.0e-9) << "time-axis X window must not shrink below 2 ns";
  // The clamp targets exactly the floor (not far above it) for this deep zoom.
  EXPECT_NEAR(width, 2.0e-9, 1e-12);
}

// Repeated aggressive zoom-ins stay pinned at the floor rather than accumulating
// below it -- the guard is a hard floor, not a one-shot.
TEST(PlotMagnifierClamp, RepeatedZoomStaysAtFloor) {
  MagnifierFixture fixture;
  fixture.magnifier.setTimeXAxis(true);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);

  for (int i = 0; i < 20; ++i) {
    fixture.zoomX(1000.0);
  }
  EXPECT_GE(fixture.xWidth(), 2.0e-9);
  EXPECT_NEAR(fixture.xWidth(), 2.0e-9, 1e-12);
}

// An XY plot's X is a value, not time: the floor does not apply, so a deep zoom
// takes the window well below 2 ns.
TEST(PlotMagnifierClamp, ValueAxisIsNotClamped) {
  MagnifierFixture fixture;
  fixture.magnifier.setTimeXAxis(false);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);

  fixture.zoomX(1.0e10);
  EXPECT_LT(fixture.xWidth(), 2.0e-9) << "a value X axis has no ns floor";
}

// Zooming OUT on a time axis is never affected by the floor.
TEST(PlotMagnifierClamp, ZoomOutUnaffectedByFloor) {
  MagnifierFixture fixture;
  fixture.magnifier.setTimeXAxis(true);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);

  fixture.zoomX(0.1);  // zoom out 10x
  EXPECT_GT(fixture.xWidth(), 1.0) << "zoom-out widens past the original 1 s window";
}

// The "Use time offset" toggle can be OFF, putting the time axis at wall-clock
// epoch scale (~1.6e9 s). A fixed 2 ns floor is not representable there -- a
// double's ULP at 1.6e9 s is ~2.4e-7 s -- so a fixed-floor clamp collapses the
// window to a single value. The ULP-aware floor must keep the edges distinct.
//
// qwt's axisScaleDiv masks a degenerate (zero-width) scale, so the faithful
// observable is the rescaled() signal's rect (raw clamped v1/v2). The zoom
// re-centers on the magnifier's mouse_position_, which defaults to (0,0); to keep
// the zoomed window AT epoch scale we drive a real wheel event whose canvas
// position maps to the axis midpoint (~1.6e9 s) before reading the emitted rect.
struct EpochMagnifierFixture {
  QwtPlot plot;
  PJ::PlotMagnifier magnifier{plot.canvas()};
  QRectF last_rect;

  EpochMagnifierFixture() {
    plot.resize(800, 400);
    plot.setAxisScale(QwtPlot::xBottom, 1.6e9, 1.6e9 + 1.0);  // [1.6e9, 1.6e9+1] s
    plot.setAxisScale(QwtPlot::yLeft, -1.0, 1.0);
    plot.updateAxes();
    plot.replot();
    QObject::connect(&magnifier, &PJ::PlotMagnifier::rescaled, &plot, [this](const QRectF& rect) { last_rect = rect; });
  }

  // Plant the magnifier's zoom center at the axis midpoint (~1.6e9 s) by sending
  // one wheel notch at the canvas center -- widgetWheelEvent sets mouse_position_
  // from the invTransform of that pixel. A subsequent direct rescale() then zooms
  // AROUND epoch scale, which is what makes the ns floor's ULP-awareness matter.
  void zoomX(double factor) {
    magnifier.rescale(factor, PJ::PlotMagnifier::kXAxis);
    plot.updateAxes();
    plot.replot();
  }

  void plantMouseAtCenter() {
    QWidget* canvas = plot.canvas();
    const QPointF pos(canvas->width() / 2.0, canvas->height() / 2.0);
    QWheelEvent event(
        pos, canvas->mapToGlobal(pos.toPoint()), QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &event);
    plot.updateAxes();
    plot.replot();
  }
};

// A deep zoom at epoch scale must leave the two X edges as DISTINCT doubles. A
// fixed 2 ns floor collapses to a single value at 1.6e9 s (2 ns << ULP ~2.4e-7 s);
// the ULP-aware floor keeps them apart, so a saved viewport can round-trip.
TEST(PlotMagnifierClamp, EpochScaleZoomKeepsEdgesDistinct) {
  EpochMagnifierFixture fixture;
  fixture.magnifier.setTimeXAxis(true);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);

  fixture.plantMouseAtCenter();
  fixture.zoomX(1.0e12);  // zoom AROUND epoch scale; sub-ns window without a floor

  ASSERT_FALSE(fixture.last_rect.isNull()) << "the magnifier must have emitted at least one rescale";
  EXPECT_GT(fixture.last_rect.left(), 1.0e9) << "sanity: the window stayed at epoch scale";
  EXPECT_NE(fixture.last_rect.left(), fixture.last_rect.right())
      << "epoch-scale zoom must not collapse the clamped X edges to a single double";
  EXPECT_GT(fixture.last_rect.right() - fixture.last_rect.left(), 0.0);
}

// Axis limits are applied INSIDE the clamp, before the time-axis floor re-expands.
// A tight upper bound just above the zoom center must not be able to shave the
// window back below the floor: the clamped width still clears the ULP-aware floor.
TEST(PlotMagnifierClamp, BoundsCannotUndercutFloor) {
  MagnifierFixture fixture;
  QRectF last_rect;
  QObject::connect(&fixture.magnifier, &PJ::PlotMagnifier::rescaled, &fixture.plot, [&last_rect](const QRectF& rect) {
    last_rect = rect;
  });
  fixture.magnifier.setTimeXAxis(true);
  fixture.magnifier.setDefaultMode(PJ::PlotMagnifier::kXAxis);
  // Zoom center at the axis midpoint (0.5), then cap the upper bound a hair ABOVE it
  // (0.5 + 1 ns). A naive clamp-then-bound ordering would widen the window to the 2 ns
  // floor around 0.5, then the min(v2, upper) would trim the right edge back to
  // 0.5 + 1 ns -- half the floor. Bounds-first keeps the clamped width intact.
  fixture.plantMouseAtCenter();
  fixture.magnifier.setAxisLimits(QwtPlot::xBottom, -1.0, 0.5 + 1.0e-9);

  fixture.zoomX(1.0e10);

  // The clamp re-expands to ~2 ns; allow a hair of float noise from computing the
  // edges at the 0.5 s offset scale. The pre-fix clamp-then-bound ordering trimmed
  // this to ~1 ns (half the floor), which this bound comfortably rejects.
  EXPECT_GT(last_rect.right() - last_rect.left(), 1.9e-9)
      << "a tight bound must not shave the clamped window below the 2 ns floor";
}
