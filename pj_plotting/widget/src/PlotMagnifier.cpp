// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotMagnifier.h"

#include <qwt_scale_map.h>
#include <qwt_scale_widget.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>
#include <algorithm>
#include <limits>

#include "TimeAxisWidth.h"

namespace PJ {

// Key events go to the focused canvas while wheel events go to the hovered one.
// Observe keys across plots in this window, without consuming them or watching
// wheel events here (Qwt must receive each wheel exactly once).
class PlotAxisKeyState final : public QObject {
 public:
  PlotAxisKeyState(QWidget* canvas, QObject* owner) : QObject(owner), canvas_(canvas) {
    qApp->installEventFilter(this);
  }

  PlotMagnifier::AxisMode mode() const {
    if (x_ == y_) {
      return PlotMagnifier::kBothAxes;
    }
    return x_ ? PlotMagnifier::kXAxis : PlotMagnifier::kYAxis;
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!canvas_) {
      return false;
    }
    if (event->type() == QEvent::ApplicationDeactivate ||
        (event->type() == QEvent::WindowDeactivate && watched == canvas_->window())) {
      x_ = y_ = false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) {
      return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
      return false;
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->isAutoRepeat() || (key->key() != Qt::Key_X && key->key() != Qt::Key_Y)) {
      return false;
    }
    bool& held = key->key() == Qt::Key_X ? x_ : y_;
    if (event->type() == QEvent::KeyRelease) {
      held = false;
      return false;
    }
    if (widget->window() != canvas_->window() || widget->testAttribute(Qt::WA_InputMethodEnabled) ||
        (key->modifiers() != Qt::NoModifier && key->modifiers() != Qt::ShiftModifier)) {
      return false;
    }
    // Only a plot surface can start the gesture, never a text editor or toolbar.
    for (QWidget* parent = widget; parent != nullptr; parent = parent->parentWidget()) {
      if (auto* source_plot = qobject_cast<QwtPlot*>(parent)) {
        // Do not arm on a key propagated from an editor into its parent plot.
        if (widget == source_plot->canvas() || qobject_cast<QwtScaleWidget*>(widget) != nullptr) {
          held = true;
        }
        break;
      }
    }
    return false;
  }

 private:
  QPointer<QWidget> canvas_;
  bool x_ = false;
  bool y_ = false;
};

PlotMagnifier::PlotMagnifier(QWidget* canvas)
    : QwtPlotMagnifier(canvas), axis_keys_(new PlotAxisKeyState(canvas, this)) {
  for (int axis_id = 0; axis_id < QwtPlot::axisCnt; ++axis_id) {
    lower_bounds_[axis_id] = std::numeric_limits<double>::lowest();
    upper_bounds_[axis_id] = std::numeric_limits<double>::max();
  }
}

void PlotMagnifier::setAxisLimits(int axis, double lower, double upper) {
  if (axis >= 0 && axis < QwtPlot::axisCnt) {
    lower_bounds_[axis] = lower;
    upper_bounds_[axis] = upper;
  }
}

void PlotMagnifier::rescale(double factor, AxisMode axis) {
  factor = qAbs(1.0 / factor);

  QwtPlot* qwt_plot = plot();
  if (qwt_plot == nullptr || factor == 1.0) {
    return;
  }

  bool do_replot = false;
  const bool auto_replot = qwt_plot->autoReplot();
  qwt_plot->setAutoReplot(false);

  const int axis_list[2] = {QwtPlot::xBottom, QwtPlot::yLeft};
  QRectF new_rect;

  for (int index = 0; index < 2; ++index) {
    double temp_factor = factor;
    if (index == 1 && axis == kXAxis) {
      temp_factor = 1.0;
    }
    if (index == 0 && axis == kYAxis) {
      temp_factor = 1.0;
    }

    const int axis_id = axis_list[index];
    if (!isAxisEnabled(axis_id)) {
      continue;
    }

    const QwtScaleMap scale_map = qwt_plot->canvasMap(axis_id);
    double v1 = scale_map.s1();
    double v2 = scale_map.s2();
    double center = axis_id == QwtPlot::yLeft ? mouse_position_.y() : mouse_position_.x();

    if (scale_map.transformation()) {
      v1 = scale_map.transform(v1);
      v2 = scale_map.transform(v2);
    }

    const double width = v2 - v1;
    const double ratio = (v2 - center) / width;
    v1 = center - width * temp_factor * (1 - ratio);
    v2 = center + width * temp_factor * ratio;

    bool reversed_axis = false;
    if (v1 > v2) {
      reversed_axis = true;
      std::swap(v1, v2);
    }

    if (scale_map.transformation()) {
      v1 = scale_map.invTransform(v1);
      v2 = scale_map.invTransform(v2);
    }

    // Apply the caller's axis bounds FIRST, so the time-axis floor below sees the
    // window a bound-clamped edge zoom would actually produce and can re-expand it
    // past the floor. Doing the floor first and the bounds after would let a bound
    // shave the window back below the floor and quietly undo the clamp.
    v1 = std::max(v1, lower_bounds_[axis_id]);
    v2 = std::min(v2, upper_bounds_[axis_id]);

    // Time-axis zoom floor: never let the X window shrink below the minimum a
    // rounded-integer-nanosecond saved viewport can represent as a non-degenerate
    // range. v1/v2 are in data (seconds) coordinates here; re-expand symmetrically
    // around the midpoint so the cursor stays roughly centered. The floor is
    // ULP-aware at the window center: the nominal 2 ns is sub-ULP near epoch scale
    // (axis at ~1.6e9 s with "Use time offset" off), where a fixed 2 ns window
    // collapses back to a single double -- the ULP term keeps the edges distinct.
    // Applied only when zooming IN (new width smaller than the floor) so a
    // legitimate zoom-out is untouched.
    if (axis_id == QwtPlot::xBottom && x_is_time_ && !reversed_axis) {
      const double center = 0.5 * (v1 + v2);
      const double min_width = plotting_detail::ulpAwareMinTimeXWidthSec(center);
      if ((v2 - v1) < min_width) {
        v1 = center - 0.5 * min_width;
        v2 = center + 0.5 * min_width;
      }
    }
    qwt_plot->setAxisScale(axis_id, reversed_axis ? v2 : v1, reversed_axis ? v1 : v2);

    if (axis_id == QwtPlot::xBottom) {
      new_rect.setLeft(v1);
      new_rect.setRight(v2);
    } else {
      new_rect.setBottom(v1);
      new_rect.setTop(v2);
    }
    do_replot = true;
  }

  qwt_plot->setAutoReplot(auto_replot);
  if (do_replot) {
    // Keep this signal as the gesture commit point: synchronous history
    // snapshots must observe the new canvas maps, not the preceding viewport.
    qwt_plot->replot();
    emit rescaled(new_rect);
  }
}

QPointF PlotMagnifier::invTransform(QPoint pos) {
  const QwtScaleMap x_map = plot()->canvasMap(QwtPlot::xBottom);
  const QwtScaleMap y_map = plot()->canvasMap(QwtPlot::yLeft);
  return QPointF(x_map.invTransform(pos.x()), y_map.invTransform(pos.y()));
}

void PlotMagnifier::widgetWheelEvent(QWheelEvent* event) {
  mouse_position_ = invTransform(event->position().toPoint());
  const AxisMode previous = default_mode_;
  if (axis_key_zoom_enabled_ && default_mode_ == kBothAxes) {
    default_mode_ = axis_keys_->mode();
  }
  QwtPlotMagnifier::widgetWheelEvent(event);
  default_mode_ = previous;
}

void PlotMagnifier::widgetMousePressEvent(QMouseEvent* event) {
  mouse_position_ = invTransform(event->pos());
  QwtPlotMagnifier::widgetMousePressEvent(event);
}

}  // namespace PJ
