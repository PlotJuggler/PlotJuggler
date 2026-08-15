// SPDX-License-Identifier: MPL-2.0
#include "pj_plotting/PlotCurve.h"

#include <qwt_painter.h>
#include <qwt_point_mapper.h>

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
  // Map and weed the points here instead of delegating to drawDots():
  // QwtPlotCurve::drawDots skips weeding whenever antialiasing is on, and the
  // OpenGL paint engine reports no rounding alignment — together that emits one
  // quad per SAMPLE. RoundPoints + WeedOutPoints with a valid bounding rect
  // selects the pixel-matrix path: one dot per covered pixel.
  QwtPointMapper mapper;
  mapper.setBoundingRect(canvas_rect);
  mapper.setFlag(QwtPointMapper::RoundPoints, true);
  mapper.setFlag(QwtPointMapper::WeedOutPoints, true);
  const QPolygonF points = mapper.toPointsF(x_map, y_map, data(), from, to);
  QwtPainter::drawPoints(painter, points);
  painter->setPen(line_pen);
}

}  // namespace PJ
