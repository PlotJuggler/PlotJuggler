// SPDX-License-Identifier: MPL-2.0
#include "pj_plotting/PlotCurve.h"

#include <qwt_legend_data.h>
#include <qwt_painter.h>
#include <qwt_point_mapper.h>
#include <qwt_text.h>

#include <QPainter>

namespace PJ {

PlotCurve::PlotCurve(const QString& title) : QwtPlotCurve(title) {}

void PlotCurve::setDotWidth(double width) {
  if (dot_width_ != width) {
    dot_width_ = width;
    itemChanged();
  }
}

double PlotCurve::dotWidth() const {
  return dot_width_;
}

bool PlotCurve::setLegendSuffix(const QString& suffix) {
  if (legend_suffix_ == suffix) {
    return false;
  }
  legend_suffix_ = suffix;
  legendChanged();
  return true;
}

QList<QwtLegendData> PlotCurve::legendData() const {
  QList<QwtLegendData> data = QwtPlotCurve::legendData();
  if (!legend_suffix_.isEmpty()) {
    for (QwtLegendData& entry : data) {
      QwtText title = entry.title();
      title.setText(QStringLiteral("%1 (%2)").arg(title.text(), legend_suffix_));
      entry.setValue(QwtLegendData::TitleRole, QVariant::fromValue(title));
    }
  }
  return data;
}

void PlotCurve::drawCurve(
    QPainter* painter, int style, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect,
    int from, int to) const {
  QwtPlotCurve::drawCurve(painter, style, x_map, y_map, canvas_rect, from, to);
  if (style != QwtPlotCurve::Lines || dot_width_ <= 0.0) {
    return;
  }
  // SquareCap keeps the batched points on the cheap quad-stroking path; RoundCap
  // would turn every zero-length segment back into per-point arc geometry, and
  // FlatCap draws nothing at all for zero-length segments.
  const QPen line_pen = painter->pen();
  QPen dot_pen = line_pen;
  dot_pen.setWidthF(dot_width_);
  dot_pen.setCapStyle(Qt::SquareCap);
  painter->setPen(dot_pen);
  // One drawPoints() batch over pixel-weeded points, not per-sample symbols.
  // QwtPlotCurve::drawDots would not weed: it skips weeding whenever
  // antialiasing is on, as it is on every PJ4 curve. It would then paint one
  // antialiased square per sample, and samples sharing a pixel overdraw the
  // partially covered edge pixels of the same square, darkening the antialiased
  // edges. RoundPoints + WeedOutPoints with a valid bounding rect selects the
  // pixel-matrix path: one dot per covered pixel, which also paints fewer squares.
  QwtPointMapper mapper;
  mapper.setBoundingRect(canvas_rect);
  mapper.setFlag(QwtPointMapper::RoundPoints, true);
  mapper.setFlag(QwtPointMapper::WeedOutPoints, true);
  const QPolygonF points = mapper.toPointsF(x_map, y_map, data(), from, to);
  QwtPainter::drawPoints(painter, points);
  painter->setPen(line_pen);
}

}  // namespace PJ
