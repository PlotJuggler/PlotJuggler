/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "PlotJuggler/plotwidget_base.h"
#include "timeseries_qwt.h"

#include "plotmagnifier.h"
#include "plotzoomer.h"
#include "plotlegend.h"

#include "qwt_axis.h"
#include "qwt_legend.h"
#include "qwt_plot.h"
#include "qwt_plot_curve.h"
#include "qwt_plot_grid.h"
#include "qwt_plot_canvas.h"
#include "qwt_plot_curve.h"
#include "qwt_plot_opengl_canvas.h"
#include "qwt_plot_rescaler.h"
#include "qwt_plot_legenditem.h"
#include "qwt_plot_marker.h"
#include "qwt_plot_layout.h"
#include "qwt_scale_engine.h"
#include "qwt_scale_map.h"
#include "qwt_scale_draw.h"
#include "qwt_scale_widget.h"
#include "qwt_symbol.h"
#include "qwt_text.h"

#include <array>
#include <QBoxLayout>
#include <QMessageBox>
#include <QSettings>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>

#include "plotpanner.h"

static int _global_color_index_ = 0;

class PlotWidgetBase::QwtPlotPimpl : public QwtPlot
{
public:
  PlotLegend* legend;
  PlotMagnifier* magnifier;
  PlotPanner* panner1;
  PlotPanner* panner2;
  PlotZoomer* zoomer;
  std::function<void(const QRectF&)> resized_callback;
  std::function<void(QEvent*)> event_callback;
  PlotWidgetBase* parent;

  QwtPlotPimpl(PlotWidgetBase* parentObject, QWidget* canvas,
               std::function<void(const QRectF&)> resizedViewCallback,
               std::function<void(QEvent*)> eventCallback)
    : QwtPlot(nullptr)
    , resized_callback(resizedViewCallback)
    , event_callback(eventCallback)
    , parent(parentObject)
  {
    this->setCanvas(canvas);

    legend = new PlotLegend(this);
    magnifier = new PlotMagnifier(this->canvas());
    panner1 = new PlotPanner(this->canvas());
    panner2 = new PlotPanner(this->canvas());
    zoomer = new PlotZoomer(this->canvas());

    zoomer->setRubberBandPen(QColor(Qt::red, 1, Qt::DotLine));
    zoomer->setTrackerPen(QColor(Qt::green, 1, Qt::DotLine));
    zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);

    magnifier->setAxisEnabled(QwtPlot::xTop, false);
    magnifier->setAxisEnabled(QwtPlot::yRight, false);

    magnifier->setZoomInKey(Qt::Key_Plus, Qt::ControlModifier);
    magnifier->setZoomOutKey(Qt::Key_Minus, Qt::ControlModifier);

    // disable right button. keep mouse wheel
    magnifier->setMouseButton(Qt::NoButton);

    panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
    panner2->setMouseButton(Qt::MiddleButton, Qt::NoModifier);

    connect(zoomer, &PlotZoomer::zoomed, this, [this](const QRectF& r) { resized_callback(r); });

    connect(magnifier, &PlotMagnifier::rescaled, this, [this](const QRectF& r) {
      resized_callback(r);
      replot();
    });

    connect(panner1, &PlotPanner::rescaled, this, [this](QRectF r) { resized_callback(r); });

    connect(panner2, &PlotPanner::rescaled, this, [this](QRectF r) { resized_callback(r); });

    QwtScaleWidget* bottomAxis = axisWidget(QwtPlot::xBottom);
    QwtScaleWidget* leftAxis = axisWidget(QwtPlot::yLeft);

    bottomAxis->installEventFilter(parent);
    leftAxis->installEventFilter(parent);
    this->canvas()->installEventFilter(parent);
  }

  ~QwtPlotPimpl() override
  {
    QwtScaleWidget* bottomAxis = axisWidget(QwtPlot::xBottom);
    QwtScaleWidget* leftAxis = axisWidget(QwtPlot::yLeft);

    bottomAxis->installEventFilter(parent);
    leftAxis->removeEventFilter(parent);
    canvas()->removeEventFilter(parent);

    setCanvas(nullptr);
  }

  QRectF canvasBoundingRect() const
  {
    QRectF rect;
    rect.setBottom(canvasMap(QwtPlot::yLeft).s1());
    rect.setTop(canvasMap(QwtPlot::yLeft).s2());
    rect.setLeft(canvasMap(QwtPlot::xBottom).s1());
    rect.setRight(canvasMap(QwtPlot::xBottom).s2());
    return rect;
  }

  virtual void resizeEvent(QResizeEvent* ev) override
  {
    QwtPlot::resizeEvent(ev);
    resized_callback(canvasBoundingRect());
    emit parent->widgetResized();
  }

  std::list<CurveInfo> curve_list;

  std::optional<CurveStyle> overridden_curve_style;
  CurveStyle default_curve_style = LINES;

  bool zoom_enabled = true;

  void dragEnterEvent(QDragEnterEvent* event) override
  {
    event_callback(event);
  }

  void dragLeaveEvent(QDragLeaveEvent* event) override
  {
    event_callback(event);
  }

  void dropEvent(QDropEvent* event) override
  {
    event_callback(event);
  }
};

namespace PJ
{
QwtPlot* PlotWidgetBase::qwtPlot()
{
  return p;
}

const QwtPlot* PlotWidgetBase::qwtPlot() const
{
  return p;
}

void PlotWidgetBase::resetZoom()
{
  updateMaximumZoomArea();
  QRectF rect = maxZoomRect();

  qwtPlot()->setAxisScale(QwtPlot::yLeft, std::min(rect.bottom(), rect.top()),
                          std::max(rect.bottom(), rect.top()));
  qwtPlot()->setAxisScale(QwtPlot::xBottom, std::min(rect.left(), rect.right()),
                          std::max(rect.left(), rect.right()));
  qwtPlot()->updateAxes();

  replot();
}

Range PlotWidgetBase::getVisualizationRangeX() const
{
  double left = std::numeric_limits<double>::max();
  double right = std::numeric_limits<double>::lowest();

  for (auto& it : curveList())
  {
    if (!it.curve->isVisible())
    {
      continue;
    }

    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
    const auto max_range_X = series->getVisualizationRangeX();
    if (!max_range_X)
    {
      continue;
    }

    left = std::min(max_range_X->min, left);
    right = std::max(max_range_X->max, right);
  }

  if (left > right)
  {
    left = 0;
    right = 0;
  }

  double margin = 0.0;
  if (fabs(right - left) > std::numeric_limits<double>::epsilon())
  {
    margin = isXYPlot() ? ((right - left) * 0.025) : 0.0;
  }
  right = right + margin;
  left = left - margin;

  return Range({ left, right });
}

Range PlotWidgetBase::getVisualizationRangeY(Range range_X) const
{
  double top = std::numeric_limits<double>::lowest();
  double bottom = std::numeric_limits<double>::max();

  for (auto& it : curveList())
  {
    if (!it.curve->isVisible())
    {
      continue;
    }

    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());

    auto max_range_X = series->getVisualizationRangeX();
    if (!max_range_X)
    {
      continue;
    }

    double left = std::max(max_range_X->min, range_X.min);
    double right = std::min(max_range_X->max, range_X.max);
    left = std::nextafter(left, right);
    right = std::nextafter(right, left);

    auto range_Y = series->getVisualizationRangeY({ left, right });
    if (!range_Y)
    {
      qDebug() << " invalid range_Y in PlotWidget::maximumRangeY";
      continue;
    }
    if (top < range_Y->max)
    {
      top = range_Y->max;
    }
    if (bottom > range_Y->min)
    {
      bottom = range_Y->min;
    }
  }

  if (bottom > top)
  {
    bottom = -1.0;
    top = 1.0;
  }

  double margin = (top - bottom) * 0.025;

  top += margin;
  bottom -= margin;

  return Range({ bottom, top });
}

bool PlotWidgetBase::isXYPlot() const
{
  return _xy_mode;
}

QRectF PlotWidgetBase::currentBoundingRect() const
{
  return p->canvasBoundingRect();
}

QRectF PlotWidgetBase::maxZoomRect() const
{
  return _max_zoom_rect;
}

void PlotWidgetBase::setModeXY(bool enable)
{
  _xy_mode = enable;
}

PlotWidgetBase::PlotWidgetBase(QWidget* parent) : _xy_mode(false), _keep_aspect_ratio(false)
{
  auto onViewResized = [this](const QRectF& r) { emit viewResized(r); };

  auto onEvent = [this](QEvent* event) {
    if (auto ev = dynamic_cast<QDragEnterEvent*>(event))
    {
      emit dragEnterSignal(ev);
    }
    else if (auto ev = dynamic_cast<QDragLeaveEvent*>(event))
    {
      emit dragLeaveSignal(ev);
    }
    else if (auto ev = dynamic_cast<QDropEvent*>(event))
    {
      emit dropSignal(ev);
    }
  };

  QSettings settings;
  bool use_opengl = settings.value("Preferences::use_opengl", true).toBool();

  QWidget* abs_canvas;
  if (use_opengl)
  {
    auto canvas = new QwtPlotOpenGLCanvas();
    canvas->setFrameStyle(QFrame::NoFrame);
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(Qt::white);
    abs_canvas = canvas;
  }
  else
  {
    auto canvas = new QwtPlotCanvas();
    canvas->setFrameStyle(QFrame::NoFrame);
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(Qt::white);
    canvas->setPaintAttribute(QwtPlotCanvas::BackingStore, true);
    abs_canvas = canvas;
  }
  abs_canvas->setObjectName("qwtCanvas");

  p = new QwtPlotPimpl(this, abs_canvas, onViewResized, onEvent);

  auto layout = new QHBoxLayout(this);
  layout->setMargin(0);
  this->setLayout(layout);
  layout->addWidget(p);

  qwtPlot()->setMinimumWidth(100);
  qwtPlot()->setMinimumHeight(100);

  qwtPlot()->sizePolicy().setHorizontalPolicy(QSizePolicy::Expanding);
  qwtPlot()->sizePolicy().setVerticalPolicy(QSizePolicy::Expanding);

  qwtPlot()->canvas()->setMouseTracking(true);

  qwtPlot()->setCanvasBackground(Qt::white);
  qwtPlot()->setAxisAutoScale(QwtPlot::yLeft, true);
  qwtPlot()->setAxisAutoScale(QwtPlot::xBottom, true);

  qwtPlot()->axisScaleEngine(QwtPlot::xBottom)->setAttribute(QwtScaleEngine::Floating, true);
  qwtPlot()->plotLayout()->setAlignCanvasToScales(true);

  qwtPlot()->setAxisScale(QwtPlot::xBottom, 0.0, 1.0);
  qwtPlot()->setAxisScale(QwtPlot::yLeft, 0.0, 1.0);
}

PlotWidgetBase::~PlotWidgetBase()
{
  if (p)
  {
    delete p;
    p = nullptr;
  }
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::addCurve(const std::string& name, PlotDataXY& data,
                                                    QColor color)
{
  const auto qname = QString::fromStdString(name);

  // title is the same of src_name, unless a transform was applied
  auto curve_it = curveFromTitle(qname);
  if (curve_it)
  {
    return nullptr;  // TODO FIXME
  }

  auto curve = new QwtPlotCurve(qname);
  try
  {
    QwtSeriesWrapper* plot_qwt = nullptr;
    if (auto ts_data = dynamic_cast<const PlotData*>(&data))
    {
      plot_qwt = createTimeSeries(ts_data);
    }
    else
    {
      plot_qwt = new QwtSeriesWrapper(&data);
    }

    curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
    curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
    curve->setData(plot_qwt);
  }
  catch (std::exception& ex)
  {
    QMessageBox::warning(qwtPlot(), "Exception!", ex.what());
    return nullptr;
  }

  if (color == Qt::transparent)
  {
    color = getColorHint(&data);
  }

  curve->setPen(color);
  setStyle(curve, p->overridden_curve_style.value_or(p->default_curve_style));

  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

  curve->attach(qwtPlot());

  auto marker = new QwtPlotMarker;
  marker->attach(qwtPlot());
  marker->setVisible(false);

  QwtSymbol* sym = new QwtSymbol(QwtSymbol::Ellipse, Qt::red, QPen(Qt::black), QSize(8, 8));
  marker->setSymbol(sym);

  CurveInfo curve_info;
  curve_info.curve = curve;
  curve_info.marker = marker;
  curve_info.src_name = name;

  p->curve_list.push_back(curve_info);

  return &(p->curve_list.back());
}

bool PlotWidgetBase::isEmpty() const
{
  return p->curve_list.empty();
}

void PlotWidgetBase::removeCurve(const QString& title)
{
  auto it = std::find_if(
      p->curve_list.begin(), p->curve_list.end(),
      [&title](const PlotWidgetBase::CurveInfo& info) { return info.curve->title() == title; });

  if (it != p->curve_list.end())
  {
    it->curve->detach();
    delete it->curve;
    it->marker->detach();
    delete it->marker;
    p->curve_list.erase(it);

    emit curveListChanged();
  }
}

const std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList() const
{
  return p->curve_list;
}

std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList()
{
  return p->curve_list;
}

QwtSeriesWrapper* PlotWidgetBase::createTimeSeries(const PlotData* data,
                                                   const QString& transform_ID)
{
  TransformedTimeseries* output = new TransformedTimeseries(data);
  output->setTransform(transform_ID);
  output->updateCache(true);
  return output;
}

bool PlotWidgetBase::keepRatioXY() const
{
  return _keep_aspect_ratio;
}

int PlotWidgetBase::leftAxisWidth()
{
  auto* axis = qwtPlot()->axisWidget(QwtPlot::yLeft);
  const int previous_minimum = axis->minimumWidth();
  axis->setMinimumWidth(0);
  qwtPlot()->updateAxes();
  const int width = axis->sizeHint().width();
  axis->setMinimumWidth(previous_minimum);
  return width;
}

int PlotWidgetBase::bottomAxisHeight()
{
  auto* axis = qwtPlot()->axisWidget(QwtPlot::xBottom);
  const int previous_minimum = axis->minimumHeight();
  axis->setMinimumHeight(0);
  qwtPlot()->updateAxes();
  const int height = axis->sizeHint().height();
  axis->setMinimumHeight(previous_minimum);
  return height;
}

double PlotWidgetBase::leftAxisExtent() const
{
  auto* axis = qwtPlot()->axisWidget(QwtPlot::yLeft);
  return axis->scaleDraw()->extent(axis->font());
}

double PlotWidgetBase::bottomAxisExtent() const
{
  auto* axis = qwtPlot()->axisWidget(QwtPlot::xBottom);
  return axis->scaleDraw()->extent(axis->font());
}

void PlotWidgetBase::setLeftAxisMinimumWidth(int width)
{
  qwtPlot()->axisWidget(QwtPlot::yLeft)->setMinimumWidth(width);
  qwtPlot()->updateAxes();
}

void PlotWidgetBase::setBottomAxisMinimumHeight(int height)
{
  qwtPlot()->axisWidget(QwtPlot::xBottom)->setMinimumHeight(height);
  qwtPlot()->updateAxes();
}

void PlotWidgetBase::setLeftAxisMinimumExtent(double extent)
{
  qwtPlot()->axisWidget(QwtPlot::yLeft)->scaleDraw()->setMinimumExtent(extent);
  qwtPlot()->updateAxes();
}

void PlotWidgetBase::setBottomAxisMinimumExtent(double extent)
{
  qwtPlot()->axisWidget(QwtPlot::xBottom)->scaleDraw()->setMinimumExtent(extent);
  qwtPlot()->updateAxes();
}

void PlotWidgetBase::setKeepRatioXY(bool active)
{
  _keep_aspect_ratio = active;
  if (isXYPlot() && active)
  {
    zoomer()->keepAspectRatio(true);
  }
  else
  {
    zoomer()->keepAspectRatio(false);
  }
}

void PlotWidgetBase::setAcceptDrops(bool accept)
{
  qwtPlot()->setAcceptDrops(accept);
}

void PlotWidgetBase::setDefaultStyle(CurveStyle default_style)
{
  p->default_curve_style = default_style;
  updateCurvesStyle();
}

PlotWidgetBase::CurveStyle PlotWidgetBase::defaultCurveStyle() const
{
  return p->default_curve_style;
}

PlotWidgetBase::CurveStyle PlotWidgetBase::curveStyle() const
{
  return p->overridden_curve_style.value_or(p->default_curve_style);
}

bool PlotWidgetBase::eventFilter(QObject* obj, QEvent* event)
{
  if (event->type() == QEvent::Destroy)
  {
    return false;
  }

  QwtScaleWidget* bottomAxis = qwtPlot()->axisWidget(QwtPlot::xBottom);
  QwtScaleWidget* leftAxis = qwtPlot()->axisWidget(QwtPlot::yLeft);

  if (magnifier() && (obj == bottomAxis || obj == leftAxis) && !(isXYPlot() && keepRatioXY()))
  {
    if (event->type() == QEvent::Wheel)
    {
      auto wheel_event = dynamic_cast<QWheelEvent*>(event);
      if (obj == bottomAxis)
      {
        magnifier()->setDefaultMode(PlotMagnifier::X_AXIS);
      }
      else
      {
        magnifier()->setDefaultMode(PlotMagnifier::Y_AXIS);
      }
      magnifier()->widgetWheelEvent(wheel_event);
    }
  }
  if (obj == qwtPlot()->canvas())
  {
    if (magnifier())
    {
      magnifier()->setDefaultMode(PlotMagnifier::BOTH_AXES);
    }
    switch (event->type())
    {
      case QEvent::Wheel: {
        auto mouse_event = dynamic_cast<QWheelEvent*>(event);

        bool ctrl_modifier = mouse_event->modifiers() == Qt::ControlModifier;
        auto legend_rect = legend()->geometry(qwtPlot()->canvas()->rect());

        if (ctrl_modifier)
        {
          if (legend_rect.contains(mouse_event->pos()) && legend()->isVisible())
          {
            int prev_size = legend()->font().pointSize();
            int new_size = prev_size;
            if (mouse_event->angleDelta().y() > 0)
            {
              new_size = std::min(13, prev_size + 1);
            }
            if (mouse_event->angleDelta().y() < 0)
            {
              new_size = std::max(7, prev_size - 1);
            }
            if (new_size != prev_size)
            {
              setLegendSize(new_size);
              emit legendSizeChanged(new_size);
            }
            return true;
          }
        }
        return false;
      }
      //-------------------
      case QEvent::MouseButtonPress: {
        QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);
        if (mouse_event->button() == Qt::LeftButton && mouse_event->modifiers() == Qt::NoModifier)
        {
          auto clicked_item = legend()->processMousePressEvent(mouse_event);
          if (clicked_item)
          {
            for (auto& it : curveList())
            {
              QSettings settings;
              bool autozoom_visibility =
                  settings.value("Preferences::autozoom_visibility", true).toBool();
              if (clicked_item == it.curve)
              {
                it.curve->setVisible(!it.curve->isVisible());
                //_tracker->redraw();

                if (autozoom_visibility)
                {
                  resetZoom();
                }
                replot();
                return true;
              }
            }
          }
        }
      }
    }
  }
  return false;
}

QColor PlotWidgetBase::getColorHint(PlotDataXY* data)
{
  QSettings settings;
  bool remember_color = settings.value("Preferences::remember_color", true).toBool();

  if (data)
  {
    auto colorHint = data->attribute(COLOR_HINT);
    if (remember_color && colorHint.isValid())
    {
      return colorHint.value<QColor>();
    }
  }
  QColor color;
  bool use_plot_color_index = settings.value("Preferences::use_plot_color_index", false).toBool();
  int index = p->curve_list.size();

  if (!use_plot_color_index)
  {
    index = (_global_color_index_++);
  }

  // https://matplotlib.org/3.1.1/users/dflt_style_changes.html
  switch (index % 8)
  {
    case 0:
      color = QColor("#1f77b4");
      break;
    case 1:
      color = QColor("#d62728");
      break;
    case 2:
      color = QColor("#1ac938");
      break;
    case 3:
      color = QColor("#ff7f0e");
      break;

    case 4:
      color = QColor("#f14cc1");
      break;
    case 5:
      color = QColor("#9467bd");
      break;
    case 6:
      color = QColor("#17becf");
      break;
    case 7:
      color = QColor("#bcbd22");
      break;
  }
  if (data)
  {
    data->setAttribute(COLOR_HINT, color);
  }

  return color;
}

std::map<QString, QColor> PlotWidgetBase::getCurveColors() const
{
  std::map<QString, QColor> color_by_name;

  for (auto& it : p->curve_list)
  {
    color_by_name.insert({ it.curve->title().text(), it.curve->pen().color() });
  }
  return color_by_name;
}

void PlotWidgetBase::setStyle(QwtPlotCurve* curve, CurveStyle style)
{
  const auto line_width =
      (style == DOTS) ? dotWidthValue(lineWidth()) : lineWidthValue(lineWidth());
  curve->setPen(curve->pen().color(), line_width);

  switch (style)
  {
    case LINES:
      curve->setStyle(QwtPlotCurve::Lines);
      break;
    case LINES_AND_DOTS:
      curve->setStyle(QwtPlotCurve::LinesAndDots);
      break;
    case DOTS:
      curve->setStyle(QwtPlotCurve::Dots);
      break;
    case STICKS:
      curve->setStyle(QwtPlotCurve::Sticks);
      break;
    case STEPS:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, false);
      break;
    case STEPSINV:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, true);
      break;
  }
}

void PlotWidgetBase::updateCurvesStyle()
{
  const auto style = p->overridden_curve_style.value_or(p->default_curve_style);
  for (auto& it : p->curve_list)
  {
    setStyle(it.curve, style);
  }
  replot();
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::curveFromTitle(const QString& title)
{
  for (auto& info : p->curve_list)
  {
    if (info.curve->title() == title)
    {
      return &info;
    }
    if (info.src_name == title.toStdString())
    {
      return &info;
    }
  }
  return nullptr;
}

void PlotWidgetBase::setLegendSize(int size)
{
  auto font = p->legend->font();
  font.setPointSize(size);
  p->legend->setFont(font);
  replot();
}

void PlotWidgetBase::setLegendAlignment(Qt::Alignment alignment)
{
  p->legend->setAlignmentInCanvas(Qt::Alignment(Qt::AlignTop | alignment));
}

void PlotWidgetBase::setZoomEnabled(bool enabled)
{
  p->zoom_enabled = enabled;
  p->zoomer->setEnabled(enabled);
  p->magnifier->setEnabled(enabled);
  p->panner1->setEnabled(enabled);
  p->panner2->setEnabled(enabled);
}

bool PlotWidgetBase::isZoomEnabled() const
{
  return p->zoom_enabled;
}

void PlotWidgetBase::setSwapZoomPan(bool swapped)
{
  if (swapped)
  {
    // Swap: Left button for pan, Ctrl+Left for zoom
    p->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::ControlModifier);
    p->panner1->setMouseButton(Qt::LeftButton, Qt::NoModifier);
  }
  else
  {
    // Default: Left button for zoom, Ctrl+Left for pan
    p->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);
    p->panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
  }
}

void PlotWidgetBase::overrideCurvesStyle(std::optional<CurveStyle> style)
{
  p->overridden_curve_style = style;
}

std::optional<PlotWidgetBase::CurveStyle> PlotWidgetBase::overriddenCurvesStyle() const
{
  return p->overridden_curve_style;
}

void PlotWidgetBase::replot()
{
  if (p->zoomer)
  {
    p->zoomer->setZoomBase(false);
  }
  qwtPlot()->replot();
}

void PlotWidgetBase::removeAllCurves()
{
  for (auto& it : curveList())
  {
    it.curve->detach();
    delete it.curve;
    it.marker->detach();
    delete it.marker;
  }

  curveList().clear();
  emit curveListChanged();
  replot();
}

PlotLegend* PlotWidgetBase::legend()
{
  return p->legend;
}

PlotZoomer* PlotWidgetBase::zoomer()
{
  return p->zoomer;
}

PlotMagnifier* PlotWidgetBase::magnifier()
{
  return p->magnifier;
}

PlotPanner* PlotWidgetBase::panner1()
{
  return p->panner1;
}

PlotPanner* PlotWidgetBase::panner2()
{
  return p->panner2;
}

void PlotWidgetBase::updateMaximumZoomArea()
{
  QRectF max_rect;
  auto rangeX = getVisualizationRangeX();
  max_rect.setLeft(rangeX.min);
  max_rect.setRight(rangeX.max);

  rangeX.min = std::numeric_limits<double>::lowest();
  rangeX.max = std::numeric_limits<double>::max();
  auto rangeY = getVisualizationRangeY(rangeX);
  max_rect.setBottom(rangeY.min);
  max_rect.setTop(rangeY.max);

  if (isXYPlot() && _keep_aspect_ratio)
  {
    const QRectF canvas_rect = p->canvas()->contentsRect();
    const double canvas_ratio = fabs(canvas_rect.width() / canvas_rect.height());
    const double data_ratio = fabs(max_rect.width() / max_rect.height());
    if (data_ratio < canvas_ratio)
    {
      // height is negative!!!!
      double new_width = fabs(max_rect.height() * canvas_ratio);
      double increment = new_width - max_rect.width();
      max_rect.setWidth(new_width);
      max_rect.moveLeft(max_rect.left() - 0.5 * increment);
    }
    else
    {
      // height must be negative!!!!
      double new_height = -(max_rect.width() / canvas_ratio);
      double increment = fabs(new_height - max_rect.height());
      max_rect.setHeight(new_height);
      max_rect.moveTop(max_rect.top() + 0.5 * increment);
    }
    magnifier()->setAxisLimits(QwtPlot::xBottom, max_rect.left(), max_rect.right());
    magnifier()->setAxisLimits(QwtPlot::yLeft, max_rect.bottom(), max_rect.top());
    zoomer()->keepAspectRatio(true);
  }
  else
  {
    magnifier()->setAxisLimits(QwtPlot::xBottom, max_rect.left(), max_rect.right());
    magnifier()->setAxisLimits(QwtPlot::yLeft, max_rect.bottom(), max_rect.top());
    zoomer()->keepAspectRatio(false);
  }
  _max_zoom_rect = max_rect;
}

void PlotWidgetBase::setLineWidth(LineWidth width)
{
  _line_width = width;
  const double width_value = lineWidthValue(width);

  for (auto& it : p->curve_list)
  {
    it.curve->setPen(it.curve->pen().color(), width_value);
  }
  replot();
}

}  // namespace PJ
