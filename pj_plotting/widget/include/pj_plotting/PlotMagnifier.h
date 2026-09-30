#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot.h>
#include <qwt_plot_magnifier.h>

#include <QPointF>

namespace PJ {

class PlotAxisKeyState;

class PlotMagnifier : public QwtPlotMagnifier {
  Q_OBJECT
 public:
  enum AxisMode { kXAxis, kYAxis, kBothAxes };

  explicit PlotMagnifier(QWidget* canvas);
  ~PlotMagnifier() override = default;

  void setAxisLimits(int axis, double lower, double upper);
  // When true, the X axis carries absolute time (seconds) and magnification is
  // clamped so the window never shrinks below the smallest non-degenerate window
  // an integer-nanosecond saved viewport can represent (plotting_detail::
  // ulpAwareMinTimeXWidthSec, ~2 ns, widened near epoch scale). Saved viewports
  // persist X as rounded integer nanoseconds, so a sub-floor window would round to
  // a degenerate (equal-edge) range and could not survive save/load; the clamp
  // keeps every reachable zoom above that floor. XY plots' X is a data value with
  // no such quantization, so leave this false for them.
  void setTimeXAxis(bool is_time) {
    x_is_time_ = is_time;
  }
  // Axis keys constrain canvas wheel zoom; disable when XY aspect ratio is locked.
  void setAxisKeyZoomEnabled(bool enabled) {
    axis_key_zoom_enabled_ = enabled;
  }
  void widgetWheelEvent(QWheelEvent* event) override;
  void rescale(double factor) override {
    rescale(factor, default_mode_);
  }
  void setDefaultMode(AxisMode mode) {
    default_mode_ = mode;
  }
  void rescale(double factor, AxisMode axis);

 signals:
  void rescaled(QRectF rect);

 protected:
  void widgetMousePressEvent(QMouseEvent* event) override;

 private:
  QPointF invTransform(QPoint pos);

  double lower_bounds_[QwtPlot::axisCnt];
  double upper_bounds_[QwtPlot::axisCnt];
  QPointF mouse_position_;
  AxisMode default_mode_ = kBothAxes;
  bool x_is_time_ = false;
  bool axis_key_zoom_enabled_ = true;
  PlotAxisKeyState* axis_keys_ = nullptr;
};

}  // namespace PJ
