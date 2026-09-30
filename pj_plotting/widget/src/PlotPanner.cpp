// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotPanner.h"

#include <qwt_axis.h>
#include <qwt_plot.h>
#include <qwt_scale_div.h>
#include <qwt_scale_map.h>

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>

#include "pj_plotting/PlotRhiCanvas.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

QPixmap PlotPanner::grab() const {
  const auto* rhi_canvas = qobject_cast<const PlotRhiCanvas*>(canvas());
  if (rhi_canvas != nullptr) {
    const QImage image = const_cast<PlotRhiCanvas*>(rhi_canvas)->grabFramebuffer();
    if (!image.isNull()) {
      QPixmap pixmap = QPixmap::fromImage(image);
      pixmap.setDevicePixelRatio(image.devicePixelRatio());
      return pixmap;
    }
  }
  return QwtPlotPanner::grab();
}

void PlotPanner::moveCanvas(int dx, int dy) {
  if (dx == 0 && dy == 0) {
    return;
  }

  QwtPlot* qwt_plot = plot();
  if (qwt_plot == nullptr) {
    return;
  }

  const bool auto_replot = qwt_plot->autoReplot();
  qwt_plot->setAutoReplot(false);

  QRectF new_rect;
  for (int axis_pos = 0; axis_pos < QwtAxis::AxisPositions; ++axis_pos) {
    const QwtAxisId axis_id(axis_pos);
    if (!isAxisEnabled(axis_id)) {
      continue;
    }

    const QwtScaleMap map = qwt_plot->canvasMap(axis_id);
    const double p1 = map.transform(qwt_plot->axisScaleDiv(axis_id).lowerBound());
    const double p2 = map.transform(qwt_plot->axisScaleDiv(axis_id).upperBound());

    double d1 = 0.0;
    double d2 = 0.0;
    if (QwtAxis::isXAxis(axis_pos)) {
      d1 = map.invTransform(p1 - dx);
      d2 = map.invTransform(p2 - dx);
    } else {
      d1 = map.invTransform(p1 - dy);
      d2 = map.invTransform(p2 - dy);
    }

    qwt_plot->setAxisScale(axis_id, d1, d2);
    if (axis_id == QwtPlot::yLeft) {
      new_rect.setBottom(d1);
      new_rect.setTop(d2);
    } else if (axis_id == QwtPlot::xBottom) {
      new_rect.setLeft(d1);
      new_rect.setRight(d2);
    }
  }

  qwt_plot->setAutoReplot(auto_replot);
  qwt_plot->replot();
  // Synchronous history consumers serialize from this signal. Refresh the
  // canvas maps first so they capture the gesture that just committed.
  emit rescaled(new_rect);
}

void PlotPanner::widgetMousePressEvent(QMouseEvent* event) {
  Qt::MouseButton button;
  Qt::KeyboardModifiers modifiers;
  getMouseButton(button, modifiers);

  if (event->button() == button && event->modifiers() == modifiers) {
    const QPixmap& pixmap = loadSvg(":/resources/svg/move_view.svg", currentTheme());
    QApplication::setOverrideCursor(QCursor(pixmap.scaled(24, 24)));
    cursor_overridden_ = true;
  }
  QwtPlotPanner::widgetMousePressEvent(event);
}

void PlotPanner::widgetMouseReleaseEvent(QMouseEvent* event) {
  if (cursor_overridden_) {
    QApplication::restoreOverrideCursor();
    cursor_overridden_ = false;
  }
  QwtPlotPanner::widgetMouseReleaseEvent(event);
}

}  // namespace PJ
