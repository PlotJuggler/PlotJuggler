#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot.h>

#include <QPoint>
#include <string>
#include <utility>
#include <vector>

class QEvent;
class QObject;
class QwtPlotGrid;
class QwtPlotItem;
class QwtPlotZoomer;

namespace PJ {

/// Lightweight Qwt chart that renders named XY line series inside a QFrame (no Qt6::Charts dependency). Created
/// by the widget binding layer for the dialog-protocol chart API (setChartSeries / onChartViewChanged); plugins do
/// not touch it.
class ChartPreviewWidget : public QwtPlot {
  Q_OBJECT

 public:
  explicit ChartPreviewWidget(QWidget* parent = nullptr);

  struct Series {
    std::string label;
    std::vector<std::pair<double, double>> points;
    std::string color;  // hex "#rrggbb"; empty: the built-in palette
  };

  void setSeries(const std::vector<Series>& series);
  void clearSeries();

  /// One marker overlaid on the series, by `kind`, in chart units:
  /// "event": a hollow point at (x0,y0) when has_value, else a vertical line at x0;
  /// "region": a shaded vertical band over x in [x0,x1];
  /// "value_band": a shaded horizontal band over y in [y0,y1] (a line when y0 == y1);
  /// "label": a vertical line at x0 carrying `label`.
  struct Marker {
    std::string kind;
    double x0 = 0.0;
    double x1 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    bool has_value = false;
    std::string color;  // hex "#rrggbb"; empty: a default
    std::string label;
  };

  /// Replaces the markers (empty clears them). They do not affect the auto-fit range.
  void setMarkers(const std::vector<Marker>& markers);

  /// Interactive navigation: zoom (rubber band, mouse wheel), pan (middle-drag or Ctrl+left-drag) and a background
  /// grid; all off until enabled. Wheel zoom-out stops at the data extent (the fitted view). Enabled, it emits
  /// viewChanged() on every zoom or pan.
  void setZoomEnabled(bool enabled);

 signals:
  /// The visible axes range changed by user zoom or pan; emitted only while zoom is enabled.
  void viewChanged(double x_min, double x_max, double y_min, double y_max);

 protected:
  /// Wheel zoom and panning live here because the plot canvas (a child widget), not this widget, receives the
  /// mouse events. Panning moves the axes and replots on every move: QwtPlotPanner drags a snapshot of the
  /// canvas instead, which leaves the axes behind and smears until release.
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  QwtPlotZoomer* zoomer_ = nullptr;
  QwtPlotGrid* grid_ = nullptr;  // shown only while interactive
  bool zoom_enabled_ = false;
  bool panning_ = false;
  QPoint pan_last_pos_;                     // canvas coordinates of the previous pan step
  std::vector<QwtPlotItem*> marker_items_;  // owned here; detached and deleted on replace

  void emitViewChanged();
  /// Ends a pan and restores the canvas cursor; no-op when not panning.
  void endPan();
  /// Moves the view by a canvas-pixel delta (a positive dx shows earlier x).
  void panBy(int dx, int dy);
};

}  // namespace PJ
