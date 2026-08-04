/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLOTWIDGET_BASE_H
#define PLOTWIDGET_BASE_H

#include <array>
#include <QWidget>
#include "plotdata.h"
#include "timeseries_qwt.h"

class QwtPlot;
class QwtPlotCurve;
class QwtPlotMarker;

class PlotPanner;
class PlotZoomer;
class PlotMagnifier;
class PlotLegend;

namespace PJ
{

enum LineWidth
{
  POINTS_1_0 = 0,
  POINTS_1_5 = 1,
  POINTS_2_0 = 2,
  POINTS_3_0 = 3
};

inline double lineWidthValue(LineWidth line_width)
{
  constexpr std::array<double, 4> line_widths = { 1.0, 1.5, 2.0, 3.0 };
  return 1.4 * line_widths[static_cast<size_t>(line_width)];
}

inline double dotWidthValue(LineWidth line_width)
{
  return (lineWidthValue(line_width) * 1.5) + 2.0;
}

class PlotWidgetBase : public QWidget
{
  Q_OBJECT

public:
  enum CurveStyle
  {
    LINES,
    DOTS,
    LINES_AND_DOTS,
    STICKS,
    STEPS,
    STEPSINV
  };

  struct CurveInfo
  {
    std::string src_name;
    QwtPlotCurve* curve;
    QwtPlotMarker* marker;
  };

  PlotWidgetBase(QWidget* parent);

  virtual ~PlotWidgetBase();

  virtual CurveInfo* addCurve(const std::string& name, PlotDataXY& src_data,
                              QColor color = Qt::transparent);

  virtual void removeCurve(const QString& title);

  const std::list<CurveInfo>& curveList() const;
  std::list<CurveInfo>& curveList();

  bool isEmpty() const;

  QColor getColorHint(PlotDataXY* data);

  std::map<QString, QColor> getCurveColors() const;

  CurveInfo* curveFromTitle(const QString& title);

  virtual QwtSeriesWrapper* createTimeSeries(const PlotData* data,
                                             const QString& transform_ID = {});

  virtual void resetZoom();

  virtual PJ::Range getVisualizationRangeX() const;

  virtual PJ::Range getVisualizationRangeY(PJ::Range range_X) const;

  virtual void setModeXY(bool enable);

  void setLegendSize(int size);

  void setLegendAlignment(Qt::Alignment alignment);

  void setZoomEnabled(bool enabled);

  bool isZoomEnabled() const;

  void setSwapZoomPan(bool swapped);

  bool isXYPlot() const;

  QRectF currentBoundingRect() const;

  QRectF maxZoomRect() const;

  bool keepRatioXY() const;

  void setKeepRatioXY(bool active);

  void setAcceptDrops(bool accept);

  int leftAxisWidth();
  int bottomAxisHeight();
  double leftAxisExtent() const;
  double bottomAxisExtent() const;
  void setLeftAxisMinimumWidth(int width);
  void setBottomAxisMinimumHeight(int height);
  void setLeftAxisMinimumExtent(double extent);
  void setBottomAxisMinimumExtent(double extent);

  void overrideCurvesStyle(std::optional<CurveStyle> style);
  std::optional<CurveStyle> overriddenCurvesStyle() const;

  void setDefaultStyle(CurveStyle default_style);
  CurveStyle defaultCurveStyle() const;

  CurveStyle curveStyle() const;

  void updateCurvesStyle();

  void setLineWidth(LineWidth width);

  LineWidth lineWidth() const
  {
    return _line_width;
  }

public slots:

  void replot();

  virtual void removeAllCurves();

signals:

  void curveListChanged();

  void viewResized(const QRectF&);

  void dragEnterSignal(QDragEnterEvent* event);
  void dragLeaveSignal(QDragLeaveEvent* event);

  void dropSignal(QDropEvent* event);

  void legendSizeChanged(int new_size);

  void widgetResized();

protected:
  class QwtPlotPimpl;
  QwtPlotPimpl* p = nullptr;

  void setStyle(QwtPlotCurve* curve, CurveStyle style);

  QwtPlot* qwtPlot();
  const QwtPlot* qwtPlot() const;

  PlotLegend* legend();
  PlotZoomer* zoomer();
  PlotMagnifier* magnifier();
  PlotPanner* panner1();
  PlotPanner* panner2();

  void updateMaximumZoomArea();

  bool eventFilter(QObject* obj, QEvent* event);

private:
  bool _xy_mode;

  QRectF _max_zoom_rect;

  bool _keep_aspect_ratio;

  LineWidth _line_width = LineWidth::POINTS_1_0;
};

}  // namespace PJ

#endif  // PLOTWIDGET_PROXY_H
