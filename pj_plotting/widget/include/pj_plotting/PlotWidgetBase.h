#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QColor>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QStringList>
#include <QWidget>
#include <functional>
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
    // Whether this curve contributes its own per-series marker set to the overlay. A dataset's Dataset and Global
    // marker scopes are separate, gated only by PlotWidget::setDatasetMarkerScopeVisible (the CurveEditor's
    // pseudo-rows), never by a curve's flag. Toggled per row in CurveEditor.
    bool show_markers = true;
  };

  explicit PlotWidgetBase(QWidget* parent = nullptr);
  ~PlotWidgetBase() override;

  virtual CurveInfo* addCurve(
      const QString& name, QwtSeriesData<QPointF>* series, QColor color = Qt::transparent,
      const QString& display_name = {});
  virtual void removeCurve(const QString& title);
  // Moves the curves `source_names` names to the front, in that order; the others follow in their current order and
  // names not plotted are skipped. The curves and their markers are re-attached in the new order, since Qwt draws and
  // lists the legend in attach order. Emits curveListChanged once, only when the order changed, and does not replot.
  bool reorderCurves(const QStringList& source_names);
  // Shows or hides the curve `curve_name` names (source_name or title, as curveFromTitle) and replots. The one path of
  // every visibility change, the legend click included: PlotWidget extends it with what depends on the visible set
  // (trackers, undo, curve editor).
  virtual void setCurveVisible(const QString& curve_name, bool visible);

  [[nodiscard]] const std::list<CurveInfo>& curveList() const noexcept;
  [[nodiscard]] std::list<CurveInfo>& curveList() noexcept;
  [[nodiscard]] bool isEmpty() const noexcept;
  // Keyed by each curve's stable source_name (the catalog key, == CurveDescriptor::name), not its display title, so a
  // caller can match a plotted curve's colour to a descriptor (the Filter Editor matches its sources to plot colours).
  [[nodiscard]] std::map<QString, QColor> curveColors() const;
  // The curve whose source_name is `title`, else the first whose title is: a source_name outranks another curve's
  // alias.
  [[nodiscard]] CurveInfo* curveFromTitle(const QString& title);

  // plot_xml::paletteColor, for callers that own a shared colour counter (a session-wide CurveColorRegistry) and must
  // not depend on this widget's nextColor().
  [[nodiscard]] static QColor paletteColor(int index);

  // Session-wide override of the GPU-canvas choice: when true (the --disable-opengl flag in main.cpp), every plot
  // constructed afterwards uses the software raster canvas whatever Preferences::use_opengl says, without changing the
  // saved preference. Inactive by default; set once at startup, before any plot exists.
  static void setGpuCanvasDisabledOverride(bool disabled);

  /// Whether a plot constructed now gets the QRhi canvas: always in the browser; on desktop Preferences::use_opengl
  /// unless the override above is set.
  [[nodiscard]] static bool gpuCanvasEnabled();

  virtual void resetZoom();
  [[nodiscard]] virtual Range<double> getVisualizationRangeX() const;
  [[nodiscard]] virtual Range<double> getVisualizationRangeY(Range<double> range_x) const;

  virtual void setModeXY(bool enable);
  [[nodiscard]] bool isXYPlot() const noexcept;

  /// Labeling of the time (X) axis: kSeconds keeps PlotScaleDraw; a clock format installs PlotTimeScaleEngine and
  /// PlotTimeScaleDraw (clock-aligned ticks). An XY plot's X is a data value and keeps plain numbers until it leaves XY
  /// mode.
  void setTimeLabelFormat(TimeLabelFormat format);

  void setLegendSize(int size);
  void setLegendAlignment(Qt::Alignment alignment);
  void setLegendVisible(bool visible);
  [[nodiscard]] bool legendVisible() const noexcept;

  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const noexcept;

  // When true (the default), Qwt aligns the canvas to the axis scales, reserving a small margin above it for the top
  // axis label, and the chart keeps its internal padding. False makes the canvas fill to the widget's top edge with no
  // padding, for embedded charts that sit flush against surrounding chrome (a toolbox banner).
  void setCanvasAlignedToScales(bool aligned);

  void setZoomEnabled(bool enabled);
  [[nodiscard]] bool isZoomEnabled() const noexcept;
  void setSwapZoomPan(bool swapped);

  [[nodiscard]] QRectF currentBoundingRect() const;
  // The view to persist: for an XY plot keeping its ratio, the range last asked for, before the fit widened one axis
  // to the canvas shape (saving the widened axes would widen again at every load under another window shape);
  // otherwise currentBoundingRect().
  [[nodiscard]] QRectF requestedView() const;
  [[nodiscard]] QRectF maxZoomRect() const noexcept;

  [[nodiscard]] bool keepRatioXY() const noexcept;
  void setKeepRatioXY(bool active);

  void setAcceptDrops(bool accept);
  void overrideCurvesStyle(std::optional<CurveStyle> style);
  [[nodiscard]] std::optional<CurveStyle> overriddenCurvesStyle() const noexcept;
  // Sets the plot-level curve style: restyles every curve and is what new curves inherit (addCurve() applies
  // curveStyle()). Style belongs to the plot, not to a curve.
  void setDefaultStyle(CurveStyle default_style);
  [[nodiscard]] CurveStyle defaultCurveStyle() const noexcept;
  [[nodiscard]] CurveStyle curveStyle() const noexcept;
  void updateCurvesStyle();

  // Sets the plot-level line width: re-pens every curve and is what new curves inherit. Width belongs to the plot, not
  // to a curve.
  void setLineWidth(LineWidth width);
  [[nodiscard]] LineWidth lineWidth() const noexcept {
    return line_width_;
  }

  // Installs `filter` on the canvas and every visible axis scale widget so the caller sees Enter/Leave across the whole
  // chart (Qt does not bubble those events to ancestors). A replacement canvas (the raster fallback after a GPU
  // failure) gets it too.
  void installHoverFilter(QObject* filter);

  // The Qwt canvas widget: the plot area proper, without axes and legend. Overlays that must stay inside the plotted
  // region (the model-tab watermark) anchor to its geometry. Not stable: a failed GPU canvas is replaced by the raster
  // one, so do not cache it.
  [[nodiscard]] QWidget* canvasWidget() const;

 public slots:
  void replot();
  // replot() after a change to markers only (the tracker, ride-along markers): the GPU canvas keeps its grid and curve
  // geometry. Any other change needs replot().
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
  // Runs inside every replot, after the curves hold the window of the new axis scales and before the canvas paints:
  // the place to re-sample overlays that read the curves.
  virtual void beforeCanvasReplot() {}
  class QwtPlotPimpl;

  void setStyle(QwtPlotCurve* curve, CurveStyle style);
  // Maps a CurveStyle onto the Qwt style/symbol/attribute triple without touching the pen, so a caller that manages pen
  // width separately (per-curve width) can restyle a curve non-destructively.
  void applyStyleToCurve(QwtPlotCurve* curve, CurveStyle style);
  QColor nextColor();
  // plot_xml::freePaletteIndex over this plot's curves: their own pens decide, so a colour restored from a layout or
  // picked by the user counts like an auto-assigned one. Each draw advances the caller's counter, so a session-wide
  // counter skips the colours this plot holds.
  [[nodiscard]] int freePaletteIndex(const std::function<int()>& next_index) const;
  // Fills `info`'s ride-along marker with its curve's current colour; no-op if it already has it. The marker is not a
  // tracker: nothing else follows a recolour, so a pass that shows it calls this.
  void syncMarkerColor(const CurveInfo& info);

  [[nodiscard]] QwtPlot* qwtPlot();
  [[nodiscard]] const QwtPlot* qwtPlot() const;
  [[nodiscard]] PlotLegend* legend();
  [[nodiscard]] PlotZoomer* zoomer();
  [[nodiscard]] PlotMagnifier* magnifier();
  [[nodiscard]] PlotPanner* panner1();
  [[nodiscard]] PlotPanner* panner2();

  void updateMaximumZoomArea();
  // Whether any visible curve has samples to frame: the test the X-range fit applies per curve, which frames nothing
  // when it fails for all of them.
  [[nodiscard]] bool hasUsableCurveData() const;
  bool eventFilter(QObject* obj, QEvent* event) override;

  // Re-reads the Data Backdrop surface for the active theme and re-applies it to the Qwt canvas. QwtPlotCanvas paints
  // through a backing store that ignores QSS, so the canvas background is a solid palette colour resolved from the
  // framework; reacting to ApplicationPaletteChange keeps the empty-plot backdrop right when the theme is applied after
  // construction or toggled at run time.
  void changeEvent(QEvent* event) override;

  // Corrects `rect` to the canvas aspect ratio (XY with keepRatioXY) and applies it to the axes; the caller decides
  // whether to replot. Pass the rect explicitly: after a magnifier or panner change currentBoundingRect() is stale
  // (they emit before they replot). `rect` stays the requested view: a resize re-fits it, not the widened axes.
  void applyRectKeepingRatio(QRectF rect);

  // Applies a rect to the axes (min/max-safe, so it tolerates applyKeepAspectRatio's normalized rect); the caller
  // decides whether to replot. A zero step lets the scale engine pick the major tick step.
  void applyRectToAxes(const QRectF& rect, double x_step = 0.0, double y_step = 0.0);

 private:
  void refreshCanvasBackground();
  // Replaces a failed GPU canvas with the raster QwtPlotCanvas and moves the magnifier, panners, zoomer and every
  // canvas event filter over, in their original order. No-op once the canvas is the raster one. Retries later while a
  // popup or modal widget is open, since the swap deletes the canvas.
  void fallBackToRasterCanvas();
  // Swaps the X axis engine and draw to match time_label_format_ and the XY mode; no-op when the installed pair
  // matches.
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
