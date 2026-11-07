#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QColor>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QWidget>
#include <list>
#include <map>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/TimeFormat.h"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QwtPlot;
class QwtPlotCurve;
class QwtPlotMarker;

namespace PJ {

class PlotLegend;
class PlotMagnifier;
class PlotPanner;
class PlotZoomer;

enum class LineWidth { kPoints10 = 0, kPoints15 = 1, kPoints20 = 2, kPoints30 = 3 };

[[nodiscard]] double lineWidthValue(LineWidth line_width) noexcept;
[[nodiscard]] double dotWidthValue(LineWidth line_width) noexcept;

class PlotWidgetBase : public QWidget {
  Q_OBJECT
 public:
  enum CurveStyle { kLines, kDots, kLinesAndDots, kSticks, kSteps, kStepsInverted };

  struct CurveInfo {
    QString source_name;
    QwtPlotCurve* curve = nullptr;
    QwtPlotMarker* marker = nullptr;
    // Whether this curve contributes its OWN per-series marker set to the
    // overlay. Each dataset's Dataset/Global marker scopes are a separate
    // concern, gated solely by PlotWidget::setDatasetMarkerScopeVisible (the
    // CurveEditor's "Dataset markers" / "Global markers" pseudo-rows) — never
    // by any curve's flag. Toggled per-row in CurveEditor.
    bool show_markers = true;
  };

  explicit PlotWidgetBase(QWidget* parent = nullptr);
  ~PlotWidgetBase() override;

  virtual CurveInfo* addCurve(
      const QString& name, QwtSeriesData<QPointF>* series, QColor color = Qt::transparent,
      const QString& display_name = {});
  virtual void removeCurve(const QString& title);

  [[nodiscard]] const std::list<CurveInfo>& curveList() const noexcept;
  [[nodiscard]] std::list<CurveInfo>& curveList() noexcept;
  [[nodiscard]] bool isEmpty() const noexcept;
  // Keyed by each curve's stable source_name (catalog key, == CurveDescriptor::name),
  // NOT its display title — so callers can correlate a plotted curve's colour with a
  // descriptor (e.g. the Filter Editor matching its source list to plot colours).
  [[nodiscard]] std::map<QString, QColor> curveColors() const;
  [[nodiscard]] CurveInfo* curveFromTitle(const QString& title);

  // The color at `index` in the built-in 8-color palette (index wraps modulo
  // the palette size). Lets callers that own a shared color counter (e.g. a
  // session-wide CurveColorRegistry) map an index to a color without depending
  // on this widget's per-instance nextColor() counter.
  [[nodiscard]] static QColor paletteColor(int index);

  // Session-wide override for the GPU-canvas choice. When set true (from the
  // --disable-opengl CLI flag in main.cpp), every plot constructed afterwards
  // uses the software raster canvas regardless of the Preferences::use_opengl
  // setting — without modifying that saved preference. The override is inactive
  // by default (so the saved preference decides); intended to be set once at
  // startup, before any plot is constructed.
  static void setGpuCanvasDisabledOverride(bool disabled);

  /// Whether a plot constructed now gets the QRhi canvas: always in the browser;
  /// on desktop the Preferences::use_opengl setting unless the override above is set.
  [[nodiscard]] static bool gpuCanvasEnabled();

  virtual void resetZoom();
  [[nodiscard]] virtual Range<double> getVisualizationRangeX() const;
  [[nodiscard]] virtual Range<double> getVisualizationRangeY(Range<double> range_x) const;

  virtual void setModeXY(bool enable);
  [[nodiscard]] bool isXYPlot() const noexcept;

  /// Labeling of the time (X) axis: kSeconds keeps PlotScaleDraw; a clock format
  /// installs PlotTimeScaleEngine + PlotTimeScaleDraw (clock-aligned ticks). An
  /// XY plot's X is a data value, so it keeps plain numbers until it leaves XY mode.
  void setTimeLabelFormat(TimeLabelFormat format);

  void setLegendSize(int size);
  void setLegendAlignment(Qt::Alignment alignment);
  void setLegendVisible(bool visible);
  [[nodiscard]] bool legendVisible() const noexcept;

  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const noexcept;

  // When true (the default), Qwt aligns the canvas to the axis scales, reserving
  // a small margin above the canvas for the top axis label, and the chart keeps
  // its internal breathing-room padding. Set false so the canvas fills to the
  // widget's top edge with zero internal padding — used by embedded charts that
  // must sit flush against surrounding chrome (e.g. a toolbox banner).
  void setCanvasAlignedToScales(bool aligned);

  void setZoomEnabled(bool enabled);
  [[nodiscard]] bool isZoomEnabled() const noexcept;
  void setSwapZoomPan(bool swapped);

  [[nodiscard]] QRectF currentBoundingRect() const;
  [[nodiscard]] QRectF maxZoomRect() const noexcept;

  [[nodiscard]] bool keepRatioXY() const noexcept;
  void setKeepRatioXY(bool active);

  void setAcceptDrops(bool accept);
  void overrideCurvesStyle(std::optional<CurveStyle> style);
  [[nodiscard]] std::optional<CurveStyle> overriddenCurvesStyle() const noexcept;
  // Sets the plot-level curve style: restyles every existing curve and is the
  // style new curves inherit (addCurve() applies curveStyle()). Style is a
  // property of the plot, not of an individual curve.
  void setDefaultStyle(CurveStyle default_style);
  [[nodiscard]] CurveStyle defaultCurveStyle() const noexcept;
  [[nodiscard]] CurveStyle curveStyle() const noexcept;
  void updateCurvesStyle();

  // Sets the plot-level line width: re-pens every existing curve and is the
  // width new curves inherit. Width is a property of the plot, not of an
  // individual curve.
  void setLineWidth(LineWidth width);
  [[nodiscard]] LineWidth lineWidth() const noexcept {
    return line_width_;
  }

  // Installs `filter` on the canvas and every visible axis scale widget,
  // so the caller sees Enter/Leave across the whole chart surface (Qt
  // does not bubble those events to ancestors). A replacement canvas (the
  // raster fallback after a GPU failure) gets it too.
  void installHoverFilter(QObject* filter);

  // The Qwt canvas widget: the plot area proper, excluding axes and legend.
  // Overlays that must stay inside the plotted region (the model-tab
  // watermark) anchor to its geometry. Not stable: a failed GPU canvas is
  // replaced by the raster one, so do not cache it.
  [[nodiscard]] QWidget* canvasWidget() const;

 public slots:
  void replot();
  // replot() after a change to markers only (the tracker, ride-along markers):
  // the GPU canvas keeps its grid and curve geometry. Any other change needs
  // replot().
  void replotOverlays();
  virtual void removeAllCurves();

 signals:
  void curveListChanged();
  void viewResized(const QRectF& rect);
  void dragEnterSignal(QDragEnterEvent* event);
  void dragLeaveSignal(QDragLeaveEvent* event);
  void dropSignal(QDropEvent* event);
  void legendSizeChanged(int new_size);

 protected:
  class QwtPlotPimpl;

  void setStyle(QwtPlotCurve* curve, CurveStyle style);
  // Maps a CurveStyle onto the Qwt style/symbol/attribute triple without
  // touching the pen, so callers that manage pen width separately (per-curve
  // width) can restyle a curve non-destructively.
  void applyStyleToCurve(QwtPlotCurve* curve, CurveStyle style);
  QColor nextColor();

  [[nodiscard]] QwtPlot* qwtPlot();
  [[nodiscard]] const QwtPlot* qwtPlot() const;
  [[nodiscard]] PlotLegend* legend();
  [[nodiscard]] PlotZoomer* zoomer();
  [[nodiscard]] PlotMagnifier* magnifier();
  [[nodiscard]] PlotPanner* panner1();
  [[nodiscard]] PlotPanner* panner2();

  void updateMaximumZoomArea();
  // Whether any visible curve has samples to frame: the same test the X-range fit
  // applies per curve, which frames nothing when it fails for all of them.
  [[nodiscard]] bool hasUsableCurveData() const;
  bool eventFilter(QObject* obj, QEvent* event) override;

  // Re-reads the Data Backdrop surface for the active theme and re-applies it to
  // the Qwt canvas. QwtPlotCanvas paints via a backing store that ignores QSS, so
  // the canvas background is a solid palette colour resolved from the framework;
  // reacting to ApplicationPaletteChange keeps the empty-plot backdrop correct
  // when the theme is applied after construction or toggled at runtime.
  void changeEvent(QEvent* event) override;

  // Corrects rect to the canvas aspect ratio (when XY + keepRatioXY) and
  // applies it to the axes. Caller decides whether to replot. Pass the rect
  // explicitly: after a magnifier/panner change, currentBoundingRect() is
  // still stale (those emit before they replot).
  void applyRectKeepingRatio(QRectF rect);

  // Applies a rect to the axes (min/max-safe, so it tolerates the normalized
  // rect that applyKeepAspectRatio produces). Caller decides whether to replot.
  void applyRectToAxes(const QRectF& rect);

 private:
  void refreshCanvasBackground();
  // Replaces a failed GPU canvas with the raster QwtPlotCanvas and moves the
  // magnifier, panners, zoomer and every canvas event filter over, in their
  // original order. No-op once the canvas is the raster one. Retries later
  // while a popup or modal widget is open, since the swap deletes the canvas.
  void fallBackToRasterCanvas();
  // Swaps the X axis engine + draw to match time_label_format_ and the XY mode;
  // no-op when the installed pair already matches.
  void applyTimeAxisFormat();

  QwtPlotPimpl* plot_ = nullptr;
  bool xy_mode_ = false;
  TimeLabelFormat time_label_format_ = TimeLabelFormat::kSeconds;    // requested by the host
  TimeLabelFormat applied_time_format_ = TimeLabelFormat::kSeconds;  // installed on the X axis
  QRectF max_zoom_rect_;
  bool keep_aspect_ratio_ = false;
  LineWidth line_width_ = LineWidth::kPoints10;
  int next_color_index_ = 0;
  // installHoverFilter() filters, re-installed on a replacement canvas.
  std::vector<QPointer<QObject>> hover_filters_;
  // fallBackToRasterCanvas() waits for a popup or modal window to close.
  bool fallback_retry_pending_ = false;
};

}  // namespace PJ
