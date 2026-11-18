// SPDX-License-Identifier: MPL-2.0

// The raster canvas draws a curve through QwtPlotCurve, which maps every sample in the range: a NaN or infinite sample
// became a line to the canvas edge. It must leave a gap, as PlotRhiCanvas does (PlotSampleReducer drops non-finite
// samples), with the finite runs on either side still drawn.

#include <gtest/gtest.h>
#include <qwt_scale_map.h>
#include <qwt_series_data.h>

#include <QImage>
#include <QPainter>
#include <cmath>
#include <limits>

#include "pj_plotting/PlotCurve.h"

namespace {

constexpr int kSize = 101;

// Five samples at x = 0..4 over a 101 px square; the finite ones sit at y = 0, the middle of the canvas.
QImage renderWithMiddleSample(double middle, int style) {
  PJ::PlotCurve curve(QStringLiteral("c"));
  curve.setStyle(static_cast<QwtPlotCurve::CurveStyle>(style));
  curve.setPen(Qt::black, 1.0);
  curve.setSamples(QVector<QPointF>{{0.0, 0.0}, {1.0, 0.0}, {2.0, middle}, {3.0, 0.0}, {4.0, 0.0}});

  QwtScaleMap x_map;
  x_map.setScaleInterval(0.0, 4.0);
  x_map.setPaintInterval(0.0, kSize - 1);
  QwtScaleMap y_map;
  y_map.setScaleInterval(-1.0, 1.0);
  y_map.setPaintInterval(kSize - 1, 0.0);

  QImage image(kSize, kSize, QImage::Format_ARGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  curve.draw(&painter, x_map, y_map, QRectF(0, 0, kSize, kSize));
  painter.end();
  return image;
}

[[nodiscard]] bool dark(const QImage& image, int x, int y) {
  return qGray(image.pixel(x, y)) < 128;
}

// Rows away from the y = 0 line hold no ink, and both finite runs are drawn.
void expectGapOnly(const QImage& image) {
  int stray = 0;
  for (int y = 0; y < kSize; ++y) {
    if (std::abs(y - kSize / 2) <= 3) {
      continue;
    }
    for (int x = 0; x < kSize; ++x) {
      stray += dark(image, x, y) ? 1 : 0;
    }
  }
  EXPECT_EQ(stray, 0) << "a non-finite sample was drawn as a line off the curve";
  EXPECT_TRUE(dark(image, 12, kSize / 2)) << "the finite run before the gap is missing";
  EXPECT_TRUE(dark(image, 88, kSize / 2)) << "the finite run after the gap is missing";
}

}  // namespace

TEST(PlotCurveNonFinite, NaNSampleLeavesAGap) {
  expectGapOnly(renderWithMiddleSample(std::numeric_limits<double>::quiet_NaN(), QwtPlotCurve::Lines));
}

TEST(PlotCurveNonFinite, InfiniteSamplesLeaveAGap) {
  expectGapOnly(renderWithMiddleSample(-std::numeric_limits<double>::infinity(), QwtPlotCurve::Lines));
  expectGapOnly(renderWithMiddleSample(std::numeric_limits<double>::infinity(), QwtPlotCurve::Lines));
}

TEST(PlotCurveNonFinite, StepsLeaveAGap) {
  expectGapOnly(renderWithMiddleSample(std::numeric_limits<double>::quiet_NaN(), QwtPlotCurve::Steps));
}
