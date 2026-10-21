// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <qwt_plot_curve.h>

namespace PJ {

/// QwtPlotCurve with an optional dots pass over the Lines style — the PJ4 home of
/// PJ3's patched-Qwt "LinesAndDots" drawing. drawCurve() paints it only on the
/// QPainter canvases (the raster QwtPlotCanvas: GPU canvas disabled, or the
/// fallback after a GPU failure); PlotRhiCanvas never calls drawCurve(), it
/// reads dotWidth() and builds the same square dots as geometry.
class PlotCurve : public QwtPlotCurve {
 public:
  explicit PlotCurve(const QString& title);

  /// Pen width of the dots pass, in the same units as the curve pen width.
  /// <= 0 disables the pass (plain Lines). Only drawn when style() is Lines.
  void setDotWidth(double width);
  [[nodiscard]] double dotWidth() const;

  /// Text shown in parentheses after the title in the legend only: the title stays the curve's identity. Empty: none.
  /// Returns whether it changed.
  bool setLegendSuffix(const QString& suffix);
  [[nodiscard]] QList<QwtLegendData> legendData() const override;

 protected:
  void drawCurve(
      QPainter* painter, int style, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect,
      int from, int to) const override;

 private:
  double dot_width_ = 0.0;
  QString legend_suffix_;
};

}  // namespace PJ
