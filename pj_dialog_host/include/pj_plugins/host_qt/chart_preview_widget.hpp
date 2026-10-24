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

/// Lightweight chart widget that renders named XY line series inside a QFrame.
/// Created and managed by the widget binding layer — plugin authors never touch
/// this directly. Built on Qwt (not Qt Charts), so the dialog host
/// carries no Qt6::Charts dependency; the dialog-protocol chart API
/// (setChartSeries / onChartViewChanged) is unaffected.
class ChartPreviewWidget : public QwtPlot {
  Q_OBJECT

 public:
  explicit ChartPreviewWidget(QWidget* parent = nullptr);

  struct Series {
    std::string label;
    std::vector<std::pair<double, double>> points;
    std::string color;  // optional hex "#rrggbb"; empty means use the built-in palette
  };

  void setSeries(const std::vector<Series>& series);
  void clearSeries();

  /// One marker overlaid on top of the series. Interpret by `kind`:
  /// "event" → a hollow point at (x0,y0) when has_value, else a vertical line at x0;
  /// "region" → a shaded vertical band over x ∈ [x0,x1];
  /// "value_band" → a shaded horizontal band over y ∈ [y0,y1] (a line when y0==y1);
  /// "label" → a vertical line at x0 carrying `label`. Coordinates are in chart units.
  struct Marker {
    std::string kind;
    double x0 = 0.0;
    double x1 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    bool has_value = false;
    std::string color;  // hex "#rrggbb"; empty → a default
    std::string label;
  };

  /// Overlay markers on top of the series. Replaces any previously-set markers;
  /// pass an empty vector to clear them. Markers do not affect the auto-fit range.
  void setMarkers(const std::vector<Marker>& markers);

  /// Enable or disable interactive navigation: zoom (rubber band + mouse wheel), pan
  /// (middle-drag or Ctrl+left-drag), and a background grid. All are off until enabled.
  /// Wheel zoom-out stops at the data extent (the fitted view). Panning redraws live.
  /// When enabled, viewChanged() is emitted whenever the user zooms or pans.
  void setZoomEnabled(bool enabled);

 signals:
  /// Emitted when the visible axes range changes due to user zoom or pan.
  /// Only emitted when zoom is enabled via setZoomEnabled(true).
  void viewChanged(double x_min, double x_max, double y_min, double y_max);

 protected:
  /// Wheel zoom and panning are implemented here because the plot *canvas* (a child
  /// widget), not this widget, receives the mouse events — overrides here never fire.
  /// Panning moves the axes on every mouse move and replots: QwtPlotPanner instead drags
  /// a snapshot of the canvas, which leaves the axes behind and smears until release.
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  QwtPlotZoomer* zoomer_ = nullptr;
  QwtPlotGrid* grid_ = nullptr;  // background grid, shown only while interactive
  bool zoom_enabled_ = false;
  bool panning_ = false;
  QPoint pan_last_pos_;                     // canvas coordinates of the previous pan step
  std::vector<QwtPlotItem*> marker_items_;  // overlay items owned here (detached+deleted on replace)

  void emitViewChanged();
  /// Ends a pan in progress and restores the canvas cursor; no-op when not panning.
  void endPan();
  /// Moves the view by a canvas-pixel delta on both axes (a positive dx shows earlier x).
  void panBy(int dx, int dy);
};

}  // namespace PJ
