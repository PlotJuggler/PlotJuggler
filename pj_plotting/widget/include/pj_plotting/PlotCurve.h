// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <qwt_plot_curve.h>

namespace PJ {

/// QwtPlotCurve with an optional batched dots pass over the Lines style — the
/// PJ4 home of PJ3's patched-Qwt "LinesAndDots" drawing. Dots are one widened-pen
/// drawDots() batch, not per-point symbols: on the OpenGL paint engine every
/// symbol blit costs a QPixmap-to-QImage conversion plus a texture upload
/// (fresh cacheKey each call), and every symbol path costs CPU triangulation,
/// so per-point QPainter calls collapse the replot rate on dense curves.
class PlotCurve : public QwtPlotCurve {
 public:
  explicit PlotCurve(const QString& title);

  /// Pen width of the dots pass, in the same units as the curve pen width.
  /// <= 0 disables the pass (plain Lines). Only drawn when style() is Lines.
  void setDotWidth(double width);
  [[nodiscard]] double dotWidth() const;

 protected:
  void drawCurve(
      QPainter* painter, int style, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect,
      int from, int to) const override;

 private:
  double dot_width_ = 0.0;
};

}  // namespace PJ
