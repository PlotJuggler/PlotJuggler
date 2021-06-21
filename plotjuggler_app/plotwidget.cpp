#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDebug>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QWheelEvent>
#include <QSettings>
#include <QSvgGenerator>
#include <QClipboard>
#include <iostream>
#include <limits>
#include <set>
#include <memory>
#include <QtXml/QDomElement>
#include "qwt_scale_widget.h"
#include "qwt_plot_canvas.h"
#include "qwt_plot_opengl_canvas.h"
#include "qwt_scale_engine.h"
#include "qwt_scale_map.h"
#include "qwt_plot_layout.h"
#include "qwt_scale_draw.h"
#include "qwt_text.h"
#include "plotwidget.h"
#include "qwt_plot_renderer.h"
#include "qwt_series_data.h"
#include "qwt_date_scale_draw.h"
#include "point_series_xy.h"
#include "suggest_dialog.h"
#include "transforms/custom_function.h"
#include "transforms/custom_timeseries.h"
#include "PlotJuggler/svg_util.h"
#include "plotwidget_editor.h"
#include "plotwidget_transforms.h"

int PlotWidget::global_color_index = 0;


QColor PlotWidget::getColorHint(PlotData* data)
{
  QSettings settings;
  bool remember_color = settings.value("Preferences::remember_color", true).toBool();

  if (data )
  {
    auto colorHint = data->attribute("color_hint");
    if( remember_color && colorHint.isValid())
    {
      return colorHint.value<QColor>();
    }
  }
  QColor color;
  bool use_plot_color_index = settings.value("Preferences::use_plot_color_index", false).toBool();
  int index = _curve_list.size();

  if (!use_plot_color_index)
  {
    index = (PlotWidget::global_color_index++);
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
    data->setAttribute( "color_hint", color );
  }

  return color;
}

class TimeScaleDraw : public QwtScaleDraw
{
  virtual QwtText label(double v) const
  {
    QDateTime dt = QDateTime::fromMSecsSinceEpoch((qint64)(v * 1000));
    if (dt.date().year() == 1970 && dt.date().month() == 1 && dt.date().day() == 1)
    {
      return dt.toString("hh:mm:ss.z");
    }
    return dt.toString("hh:mm:ss.z\nyyyy MMM dd");
  }
};

const double MAX_DOUBLE = std::numeric_limits<double>::max() / 2;

static const char* noTransform = "noTransform";
static const char* Derivative1st = "1st Derivative";
static const char* Derivative2nd = "2nd Derivative";
static bool if_xy_plot_failed_show_dialog = true;

static QStringList builtin_trans = { noTransform, Derivative1st, Derivative2nd };

PlotWidget::PlotWidget(PlotDataMapRef& datamap, QWidget* parent)
  : QwtPlot(parent)
  , _zoomer(nullptr)
  , _magnifier(nullptr)
  , _panner1(nullptr)
  , _panner2(nullptr)
  , _tracker(nullptr)
  , _legend(nullptr)
  , _use_date_time_scale(false)
  , _color_index(0)
  , _mapped_data(datamap)
  , _dragging({ DragInfo::NONE, {}, nullptr })
  , _curve_style(QwtPlotCurve::Lines)
  , _time_offset(0.0)
  , _xy_mode(false)
  , _transform_select_dialog(nullptr)
  , _zoom_enabled(true)
  , _keep_aspect_ratio(true)
  ,_context_menu_enabled(true)
{
  connect(this, &PlotWidget::curveListChanged, this, [this]() { this->updateMaximumZoomArea(); });

  this->setAcceptDrops(true);

  this->setMinimumWidth(100);
  this->setMinimumHeight(100);

  this->sizePolicy().setHorizontalPolicy(QSizePolicy::Expanding);
  this->sizePolicy().setVerticalPolicy(QSizePolicy::Expanding);

  QSettings settings;

  bool use_opengl = settings.value("Preferences::use_opengl", true).toBool();
  if( use_opengl )
  {
    auto canvas = new QwtPlotOpenGLCanvas();
    canvas->setFrameStyle(QFrame::NoFrame);
    canvas->setFrameStyle( QFrame::Box | QFrame::Plain );
    canvas->setLineWidth( 1 );
    canvas->setPalette( Qt::white );
    this->setCanvas(canvas);
  }
  else{
    auto canvas = new QwtPlotCanvas();
    canvas->setFrameStyle(QFrame::NoFrame);
    canvas->setFrameStyle( QFrame::Box | QFrame::Plain );
    canvas->setLineWidth( 1 );
    canvas->setPalette( Qt::white );
    canvas->setPaintAttribute(QwtPlotCanvas::BackingStore, true);
    this->setCanvas(canvas);
  }

  this->setCanvasBackground(Qt::white);

  this->setAxisAutoScale(QwtPlot::yLeft, true);
  this->setAxisAutoScale(QwtPlot::xBottom, true);

  this->axisScaleEngine(QwtPlot::xBottom)->setAttribute(QwtScaleEngine::Floating, true);
  this->plotLayout()->setAlignCanvasToScales(true);

  //--------------------------
  _zoomer = (new PlotZoomer(this->canvas()));
  _magnifier = (new PlotMagnifier(this->canvas()));
  _panner1 = (new QwtPlotPanner(this->canvas()));
  _panner2 = (new QwtPlotPanner(this->canvas()));
  _tracker = (new CurveTracker(this));

  _grid = new QwtPlotGrid();
  _grid->setPen(QPen(Qt::gray, 0.0, Qt::DotLine));

  _zoomer->setRubberBandPen(QColor(Qt::red, 1, Qt::DotLine));
  _zoomer->setTrackerPen(QColor(Qt::green, 1, Qt::DotLine));
  _zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);
  connect(_zoomer, &PlotZoomer::zoomed, this, &PlotWidget::on_externallyResized);

  _magnifier->setAxisEnabled(xTop, false);
  _magnifier->setAxisEnabled(yRight, false);

  _magnifier->setZoomInKey(Qt::Key_Plus, Qt::ControlModifier);
  _magnifier->setZoomOutKey(Qt::Key_Minus, Qt::ControlModifier);

  // disable right button. keep mouse wheel
  _magnifier->setMouseButton(Qt::NoButton);
  connect(_magnifier, &PlotMagnifier::rescaled, this, [this](QRectF rect) {
    on_externallyResized(rect);
    replot();
  });

  _panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
  _panner2->setMouseButton(Qt::MiddleButton, Qt::NoModifier);

  connect(_panner1, &QwtPlotPanner::panned, this, &PlotWidget::on_panned);
  connect(_panner2, &QwtPlotPanner::panned, this, &PlotWidget::on_panned);

  //-------------------------

  buildActions();

  _legend = new PlotLegend(this);

  this->canvas()->setMouseTracking(true);

  setAxisScale(xBottom, 0.0, 1.0);
  setAxisScale(yLeft, 0.0, 1.0);

  _custom_Y_limits.min = (-MAX_DOUBLE);
  _custom_Y_limits.max = (MAX_DOUBLE);

  QwtScaleWidget* bottomAxis = this->axisWidget(xBottom);
  QwtScaleWidget* leftAxis = this->axisWidget(yLeft);

  bottomAxis->installEventFilter(this);
  leftAxis->installEventFilter(this);
}

void PlotWidget::setContextMenuEnabled(bool enabled)
{
  _context_menu_enabled = enabled;
}

void PlotWidget::buildActions()
{
  QIcon iconDeleteList;

  _action_edit  = new QAction("&Edit curves...", this);
  connect(_action_edit, &QAction::triggered, this,
          [=]()
          {
            auto editor_dialog = new PlotwidgetEditor(this, this);
            editor_dialog->exec();
            editor_dialog->deleteLater();
          } );

  _action_formula  = new QAction("&Apply filter to data...", this);
  connect(_action_formula, &QAction::triggered, this,
          [=]()
          {
            auto editor_dialog = new DialogTransformEditor(this);
            int res = editor_dialog->exec();
            editor_dialog->deleteLater();
            if( res == QDialog::Accepted){
              emit undoableChange();
            }
          } );

  _action_split_horizontal = new  QAction("&Split Horizontally", this);
  connect( _action_split_horizontal, &QAction::triggered, this, &PlotWidget::splitHorizontal);

  _action_split_vertical = new  QAction("&Split Vertically", this);
  connect( _action_split_vertical, &QAction::triggered, this, &PlotWidget::splitVertical);

  _action_removeAllCurves = new QAction("&Remove ALL curves", this);
  connect(_action_removeAllCurves, &QAction::triggered, this, &PlotWidget::removeAllCurves);
  connect(_action_removeAllCurves, &QAction::triggered, this, &PlotWidget::undoableChange);

  _action_zoomOutMaximum = new QAction("&Zoom Out", this);
  connect(_action_zoomOutMaximum, &QAction::triggered, this, [this]() {
    zoomOut(true);
    replot();
    emit undoableChange();
  });

  _action_zoomOutHorizontally = new QAction("&Zoom Out Horizontally", this);
  connect(_action_zoomOutHorizontally, &QAction::triggered, this, [this]() {
    on_zoomOutHorizontal_triggered(true);
    replot();
    emit undoableChange();
  });

  _action_zoomOutVertically = new QAction("&Zoom Out Vertically", this);
  connect(_action_zoomOutVertically, &QAction::triggered, this, [this]() {
    on_zoomOutVertical_triggered(true);
    replot();
    emit undoableChange();
  });

  QFont font;
  font.setPointSize(10);

  _action_saveToFile = new QAction("&Save plot to file", this);
  connect(_action_saveToFile, &QAction::triggered, this, &PlotWidget::on_savePlotToFile);

  _action_copy = new QAction("&Copy", this);
  connect(_action_copy, &QAction::triggered, this, &PlotWidget::on_copyAction_triggered);

  _action_paste = new QAction("&Paste", this);
  connect(_action_paste, &QAction::triggered, this, &PlotWidget::on_pasteAction_triggered);

  _action_image_to_clipboard = new QAction("&Copy image to clipboard", this);
  connect(_action_image_to_clipboard, &QAction::triggered, this, &PlotWidget::on_copyToClipboard);

}

void PlotWidget::canvasContextMenuTriggered(const QPoint& pos)
{
  if( _context_menu_enabled == false )
  {
    return;
  }

  QSettings settings;
  QString theme = settings.value("StyleSheet::theme", "light").toString();

  _action_removeAllCurves->setIcon( LoadSvgIcon(":/resources/svg/remove_red.svg", theme) );
  _action_edit->setIcon( LoadSvgIcon(":/resources/svg/pencil-edit.svg", theme) );
  _action_formula->setIcon( LoadSvgIcon(":/resources/svg/Fx.svg", theme) );
  _action_split_horizontal->setIcon( LoadSvgIcon(":/resources/svg/add_column.svg", theme) );
  _action_split_vertical->setIcon( LoadSvgIcon(":/resources/svg/add_row.svg", theme) );
  _action_zoomOutMaximum->setIcon( LoadSvgIcon(":/resources/svg/zoom_max.svg", theme) );
  _action_zoomOutHorizontally->setIcon( LoadSvgIcon(":/resources/svg/zoom_horizontal.svg", theme) );
  _action_zoomOutVertically->setIcon( LoadSvgIcon(":/resources/svg/zoom_vertical.svg", theme) );
  _action_copy->setIcon( LoadSvgIcon(":/resources/svg/copy.svg", theme) );
  _action_paste->setIcon( LoadSvgIcon(":/resources/svg/paste.svg", theme) );
  _action_saveToFile->setIcon( LoadSvgIcon(":/resources/svg/save.svg", theme) );
  _action_image_to_clipboard->setIcon( LoadSvgIcon(":/resources/svg/plot_image.svg", theme) );

  QMenu menu(this);

  menu.addAction(_action_edit);
  menu.addAction(_action_formula);
  menu.addSeparator();
  menu.addAction(_action_split_horizontal);
  menu.addAction(_action_split_vertical);
  menu.addSeparator();
  menu.addAction(_action_zoomOutMaximum);
  menu.addAction(_action_zoomOutHorizontally);
  menu.addAction(_action_zoomOutVertically);
  menu.addSeparator();
  menu.addAction(_action_removeAllCurves);
  menu.addSeparator();
  menu.addAction(_action_copy);
  menu.addAction(_action_paste);
  menu.addAction(_action_image_to_clipboard);
  menu.addAction(_action_saveToFile);

  // check the clipboard
  QClipboard *clipboard = QGuiApplication::clipboard();
  QString clipboard_text = clipboard->text();
  QDomDocument doc;
  bool valid_clipbaord =
      ( !clipboard_text.isEmpty() && // not empty
      doc.setContent(clipboard_text) && // valid xml
      doc.firstChildElement().tagName() == "PlotWidgetClipBoard");

  _action_paste->setEnabled(valid_clipbaord);

  _action_removeAllCurves->setEnabled(!_curve_list.empty());
  _action_formula->setEnabled(!_curve_list.empty() && !isXYPlot());

  menu.exec(canvas()->mapToGlobal(pos));
}

PlotWidget::~PlotWidget()
{
}


PlotWidget::CurveInfo* PlotWidget::addCurve(const std::string& name, QColor color )
{
  auto it = _mapped_data.numeric.find(name);
  if (it == _mapped_data.numeric.end())
  {
    return nullptr;
  }

  const auto qname = QString::fromStdString(name);

  // title is the same of src_name, unless a transform was applied
  auto curve_it = curveFromTitle(qname);
  if (curve_it)
  {
    return nullptr; //TODO FIXME
  }

  PlotData& data = it->second;

  auto curve = new QwtPlotCurve(qname);
  try
  {
    auto plot_qwt = createTimeSeries("", &data);

    curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
    curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
    curve->setData(plot_qwt);
  }
  catch (std::exception& ex)
  {
    QMessageBox::warning(this, "Exception!", ex.what());
    return nullptr;
  }

  if( color == Qt::transparent ){
    color = getColorHint(&data);
  }
  curve->setPen(color, (_curve_style == QwtPlotCurve::Dots) ? 4.0 : 1.3);
  curve->setStyle(_curve_style);

  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

  curve->attach(this);

  auto marker = new QwtPlotMarker;
  marker->attach(this);
  marker->setVisible(isXYPlot());

  QwtSymbol* sym = new QwtSymbol(QwtSymbol::Ellipse, Qt::red, QPen(Qt::black), QSize(8, 8));
  marker->setSymbol(sym);

  CurveInfo curve_info;
  curve_info.curve = curve;
  curve_info.marker = marker;
  curve_info.src_name = name;
  _curve_list.push_back( curve_info );

  return &(_curve_list.back());
}

PlotWidget::CurveInfo *PlotWidget::addCurveXY(std::string name_x, std::string name_y, QString curve_name)
{
  std::string name = curve_name.toStdString();

  while (name.empty())
  {
    SuggestDialog dialog(name_x, name_y, this);

    bool ok = (dialog.exec() == QDialog::Accepted);
    curve_name = dialog.suggestedName();
    name = curve_name.toStdString();
    name_x = dialog.nameX().toStdString();
    name_y = dialog.nameY().toStdString();

    if (!ok)
    {
      return nullptr;
    }

    auto curve_it = curveFromTitle(curve_name);

    if (name.empty() || curve_it )
    {
      int ret = QMessageBox::warning(this, "Missing name",
                                     "The name of the curve is missing or exist already. Try again or abort.",
                                     QMessageBox::Abort | QMessageBox::Retry, QMessageBox::Retry);
      if (ret == QMessageBox::Abort)
      {
        return nullptr;
      }
      name.clear();
    }
  }

  auto it = _mapped_data.numeric.find(name_x);
  if (it == _mapped_data.numeric.end())
  {
    throw std::runtime_error("Creation of XY plot failed");
  }
  PlotData& data_x = it->second;

  it = _mapped_data.numeric.find(name_y);
  if (it == _mapped_data.numeric.end())
  {
    throw std::runtime_error("Creation of XY plot failed");
  }
  PlotData& data_y = it->second;

  auto curve_it = curveFromTitle(curve_name);
  if (curve_it)
  {
    return nullptr;
  }

  const auto qname = QString::fromStdString(name);
  auto curve = new QwtPlotCurve(qname);

  try
  {
    auto plot_qwt = createCurveXY(&data_x, &data_y);

    curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
    curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
    curve->setData(plot_qwt);
  }
  catch (std::exception& ex)
  {
    QMessageBox::warning(this, "Exception!", ex.what());
    return nullptr;
  }

  QColor color = getColorHint(nullptr);

  curve->setPen(color, (_curve_style == QwtPlotCurve::Dots) ? 4.0 : 1.3);
  curve->setStyle(_curve_style);

  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

  curve->attach(this);

  auto marker = new QwtPlotMarker;
  marker->attach(this);
  marker->setVisible(isXYPlot());
  QwtSymbol* sym = new QwtSymbol(QwtSymbol::Ellipse, color, QPen(Qt::black), QSize(8, 8));
  marker->setSymbol(sym);

  CurveInfo curve_info;
  curve_info.curve = curve;
  curve_info.marker = marker;
  curve_info.src_name = name;
  _curve_list.push_back( curve_info );

  return &(_curve_list.back());
}

void PlotWidget::removeCurve(const QString &title)
{
  auto it = std::find_if(_curve_list.begin(), _curve_list.end(),
                         [&title](const PlotWidget::CurveInfo& info)
  {
    return info.curve->title() == title;
  });

  if (it != _curve_list.end())
  {
    it->curve->detach();
    it->marker->detach();

    _curve_list.erase(it);

    _tracker->redraw();
    emit curveListChanged();
  }
}

void PlotWidget::onSourceDataRemoved(const std::string& src_name)
{
  bool deleted = false;

  for (auto it = _curve_list.begin(); it != _curve_list.end();)
  {
    PointSeriesXY* curve_xy = dynamic_cast<PointSeriesXY*>(it->curve->data());
    bool remove_curve_xy =
        curve_xy && (curve_xy->dataX()->plotName() == src_name ||
                     curve_xy->dataY()->plotName() == src_name);

    if (it->src_name == src_name || remove_curve_xy)
    {
      deleted = true;

      it->curve->detach();
      it->marker->detach();

      it = _curve_list.erase(it);
    }
    else {
      it++;
    }
  }

  if (deleted)
  {
    _tracker->redraw();
    emit curveListChanged();
  }
}

bool PlotWidget::isEmpty() const
{
  return _curve_list.empty();
}

const std::list<PlotWidget::CurveInfo>& PlotWidget::curveList() const
{
  return _curve_list;
}

std::list<PlotWidget::CurveInfo>& PlotWidget::curveList()
{
  return _curve_list;
}

PlotWidget::CurveInfo *PlotWidget::curveFromTitle(const QString &title)
{
  for(auto& info: _curve_list )
  {
    if( info.curve->title() == title )
    {
      return &info;
    }
    if( info.src_name == title.toStdString() )
    {
      return &info;
    }
  }
  return nullptr;
}

const PlotWidget::CurveInfo *PlotWidget::curveFromTitle(const QString &title) const
{
  for(const auto& info: _curve_list )
  {
    if( info.curve->title() == title )
    {
      return &info;
    }
    if( info.src_name == title.toStdString() )
    {
      return &info;
    }
  }

  return nullptr;
}

void PlotWidget::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls())
  {
    event->ignore();
  }
  const QMimeData* mimeData = event->mimeData();
  QStringList mimeFormats = mimeData->formats();
  _dragging.curves.clear();
  _dragging.source = event->source();

  for (const QString& format : mimeFormats)
  {
    QByteArray encoded = mimeData->data(format);
    QDataStream stream(&encoded, QIODevice::ReadOnly);

    while (!stream.atEnd())
    {
      QString curve_name;
      stream >> curve_name;
      if (!curve_name.isEmpty())
      {
        _dragging.curves.push_back(curve_name);
      }
    }

    if (format == "curveslist/add_curve")
    {
      _dragging.mode = DragInfo::CURVES;
      event->acceptProposedAction();
    }
    if (format == "curveslist/new_XY_axis")
    {
      if (_dragging.curves.size() != 2)
      {
        qDebug() << "FATAL: Dragging " << _dragging.curves.size() << " curves";
        return;
      }
      if( _curve_list.empty() || isXYPlot())
      {
        _dragging.mode = DragInfo::NEW_XY;
        event->acceptProposedAction();
      }
      else{
        event->ignore();
      }
    }
  }
}

void PlotWidget::dragLeaveEvent(QDragLeaveEvent*)
{
  _dragging.mode = DragInfo::NONE;
  _dragging.curves.clear();
}

void PlotWidget::dropEvent(QDropEvent*)
{
  bool curves_changed = false;

  if (_dragging.mode == DragInfo::CURVES)
  {
    if ( isXYPlot() && !_curve_list.empty())
    {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
      QMessageBox::warning(this, "Warning",
                           tr("This is a XY plot, you can not drop normal time series here.\n"
                              "Clear all curves to reset it to normal mode."));
      return;
    }
    else if ( isXYPlot() && _curve_list.empty())
    {
      setModeXY(false);
    }

    for (const auto& curve_name : _dragging.curves)
    {
      bool added = addCurve(curve_name.toStdString()) != nullptr;
      curves_changed = curves_changed || added;
    }
  }
  else if (_dragging.mode == DragInfo::NEW_XY && _dragging.curves.size() == 2)
  {
    if (!_curve_list.empty() && !_xy_mode)
    {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
      QMessageBox::warning(this, "Warning",
                           tr("To convert this widget into a XY plot, "
                              "you must first remove all the time series."));
      return;
    }

    setModeXY(true);
    addCurveXY(_dragging.curves[0].toStdString(),
               _dragging.curves[1].toStdString());

    curves_changed = true;
  }

  if (curves_changed)
  {
    emit curvesDropped();
    emit curveListChanged();
    zoomOut(true);
  }
  _dragging.mode = DragInfo::NONE;
  _dragging.curves.clear();
}

void PlotWidget::removeAllCurves()
{
  for (auto& it : _curve_list)
  {
    it.curve->detach();
    it.marker->detach();
  }

  setModeXY(false);
  _curve_list.clear();
  _tracker->redraw();

  emit curveListChanged();

  replot();
}

void PlotWidget::on_panned(int, int)
{
  on_externallyResized(canvasBoundingRect());
}

QDomElement PlotWidget::xmlSaveState(QDomDocument& doc) const
{
  QDomElement plot_el = doc.createElement("plot");

  QDomElement range_el = doc.createElement("range");
  QRectF rect = this->canvasBoundingRect();
  range_el.setAttribute("bottom", QString::number(rect.bottom(), 'f', 6));
  range_el.setAttribute("top", QString::number(rect.top(), 'f', 6));
  range_el.setAttribute("left", QString::number(rect.left(), 'f', 6));
  range_el.setAttribute("right", QString::number(rect.right(), 'f', 6));
  plot_el.appendChild(range_el);

  QDomElement limitY_el = doc.createElement("limitY");
  if (_custom_Y_limits.min > -MAX_DOUBLE)
  {
    limitY_el.setAttribute("min", QString::number(_custom_Y_limits.min));
  }
  if (_custom_Y_limits.max < MAX_DOUBLE)
  {
    limitY_el.setAttribute("max", QString::number(_custom_Y_limits.max));
  }
  plot_el.appendChild(limitY_el);

  if (_curve_style == QwtPlotCurve::Lines)
  {
    plot_el.setAttribute("style", "Lines");
  }
  else if (_curve_style == QwtPlotCurve::LinesAndDots)
  {
    plot_el.setAttribute("style", "LinesAndDots");
  }
  else if (_curve_style == QwtPlotCurve::Dots)
  {
    plot_el.setAttribute("style", "Dots");
  }

  for (auto& it : _curve_list)
  {
    auto& name = it.src_name;
    QwtPlotCurve* curve = it.curve;
    QDomElement curve_el = doc.createElement("curve");
    curve_el.setAttribute("name", QString::fromStdString(name));
    curve_el.setAttribute("color", curve->pen().color().name());

    plot_el.appendChild(curve_el);

    if (isXYPlot())
    {
      PointSeriesXY* curve_xy = dynamic_cast<PointSeriesXY*>(curve->data());
      curve_el.setAttribute("curve_x", QString::fromStdString(curve_xy->dataX()->plotName()));
      curve_el.setAttribute("curve_y", QString::fromStdString(curve_xy->dataY()->plotName()));
    }
    else{
      auto ts = dynamic_cast<TransformedTimeseries*>(curve->data());
      if(ts && ts->transform())
      {
        QDomElement transform_el = doc.createElement("transform");
        transform_el.setAttribute("name", ts->transformName() );
        transform_el.setAttribute("alias", ts->transform()->alias() );
        ts->transform()->xmlSaveState(doc, transform_el);
        curve_el.appendChild(transform_el);
      }
    }
  }

  plot_el.setAttribute("mode", isXYPlot() ? "XYPlot" : "TimeSeries");

  return plot_el;
}

bool PlotWidget::xmlLoadState(QDomElement& plot_widget)
{
  std::set<std::string> added_curve_names;

  QString mode = plot_widget.attribute("mode");
  setModeXY(mode == "XYPlot");

  QDomElement limitY_el = plot_widget.firstChildElement("limitY");

  _custom_Y_limits.min = -MAX_DOUBLE;
  _custom_Y_limits.max = +MAX_DOUBLE;

  if (!limitY_el.isNull())
  {
    if (limitY_el.hasAttribute("min"))
    {
      _custom_Y_limits.min = limitY_el.attribute("min").toDouble();
    }
    if (limitY_el.hasAttribute("max"))
    {
      _custom_Y_limits.max = limitY_el.attribute("max").toDouble();
    }
  }

  static bool warning_message_shown = false;

  // removeAllCurves simplified
  for (auto& it : _curve_list)
  {
    it.curve->detach();
    it.marker->detach();
  }
  _curve_list.clear();

  // insert curves
  for (QDomElement curve_element = plot_widget.firstChildElement("curve"); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement("curve"))
  {
    QString curve_name = curve_element.attribute("name");
    std::string curve_name_std = curve_name.toStdString();
    QColor color( curve_element.attribute("color"));

    bool error = false;
    if (!isXYPlot())
    {
      if (_mapped_data.numeric.find(curve_name_std) == _mapped_data.numeric.end())
      {
        error = true;
      }
      else
      {
        auto curve_it = addCurve(curve_name_std, color);
        if( ! curve_it )
        {
          continue;
        }
        auto &curve = curve_it->curve;
        curve->setPen(color, 1.3);
        added_curve_names.insert(curve_name_std);

        auto ts = dynamic_cast<TransformedTimeseries*>(curve->data());
        QDomElement transform_el = curve_element.firstChildElement("transform");
        if( transform_el.isNull() == false )
        {
          ts->setTransform( transform_el.attribute("name") );
          ts->transform()->xmlLoadState(transform_el);
          ts->updateCache(true);
          auto alias = transform_el.attribute("alias");
          ts->transform()->setAlias( alias );
          curve->setTitle( alias );
        }
      }
    }
    else
    {
      std::string curve_x = curve_element.attribute("curve_x").toStdString();
      std::string curve_y = curve_element.attribute("curve_y").toStdString();

      if (_mapped_data.numeric.find(curve_x) == _mapped_data.numeric.end() ||
          _mapped_data.numeric.find(curve_y) == _mapped_data.numeric.end())
      {
        error = true;
      }
      else
      {
        auto curve_it = addCurveXY(curve_x, curve_y, curve_name);
        if( ! curve_it )
        {
          continue;
        }
        curve_it->curve->setPen(color, 1.3);
        curve_it->marker->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, color, QPen(Qt::black), QSize(8, 8)));
        added_curve_names.insert(curve_name_std);
      }
    }

    if (error && !warning_message_shown)
    {
      QMessageBox::warning(this, "Warning",
                           tr("Can't find one or more curves.\n"
                              "This message will be shown only once."));
      warning_message_shown = true;
    }
  }

  emit curveListChanged();

  //-----------------------------------------

  QDomElement rectangle = plot_widget.firstChildElement("range");

  if (!rectangle.isNull())
  {
    QRectF rect;
    rect.setBottom(rectangle.attribute("bottom").toDouble());
    rect.setTop(rectangle.attribute("top").toDouble());
    rect.setLeft(rectangle.attribute("left").toDouble());
    rect.setRight(rectangle.attribute("right").toDouble());
    this->setZoomRectangle(rect, false);
  }

  if (plot_widget.hasAttribute("style"))
  {
    QString style = plot_widget.attribute("style");
    if (style == "Lines")
    {
      _curve_style = QwtPlotCurve::Lines;
    }
    else if (style == "LinesAndDots")
    {
      _curve_style = QwtPlotCurve::LinesAndDots;
    }
    else if (style == "Dots")
    {
      _curve_style = QwtPlotCurve::Dots;
    }
    changeCurveStyle(_curve_style);
  }

  updateMaximumZoomArea();
  replot();
  return true;
}

QRectF PlotWidget::canvasBoundingRect() const
{
  QRectF rect;
  rect.setBottom(this->canvasMap(yLeft).s1());
  rect.setTop(this->canvasMap(yLeft).s2());
  rect.setLeft(this->canvasMap(xBottom).s1());
  rect.setRight(this->canvasMap(xBottom).s2());
  return rect;
}

void PlotWidget::updateMaximumZoomArea()
{
  QRectF max_rect;
  auto rangeX = getMaximumRangeX();
  max_rect.setLeft(rangeX.min);
  max_rect.setRight(rangeX.max);

  auto rangeY = getMaximumRangeY(rangeX);
  max_rect.setBottom(rangeY.min);
  max_rect.setTop(rangeY.max);

  if (isXYPlot() && _keep_aspect_ratio)
  {
    const QRectF canvas_rect = canvas()->contentsRect();
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
    _magnifier->setAxisLimits(xBottom, max_rect.left(), max_rect.right());
    _magnifier->setAxisLimits(yLeft, max_rect.bottom(), max_rect.top());
    _zoomer->keepAspectRatio(true);
  }
  else
  {
    _magnifier->setAxisLimits(xBottom, max_rect.left(), max_rect.right());
    _magnifier->setAxisLimits(yLeft, max_rect.bottom(), max_rect.top());
    _zoomer->keepAspectRatio(false);
  }
  _max_zoom_rect = max_rect;
}

void PlotWidget::rescaleEqualAxisScaling()
{
  const QwtScaleMap xMap = canvasMap(QwtPlot::xBottom);
  const QwtScaleMap yMap = canvasMap(QwtPlot::yLeft);

  QRectF canvas_rect = canvas()->contentsRect();
  canvas_rect = canvas_rect.normalized();
  const double x1 = xMap.invTransform(canvas_rect.left());
  const double x2 = xMap.invTransform(canvas_rect.right());
  const double y1 = yMap.invTransform(canvas_rect.bottom());
  const double y2 = yMap.invTransform(canvas_rect.top());

  const double data_ratio = (x2 - x1) / (y2 - y1);
  const double canvas_ratio = canvas_rect.width() / canvas_rect.height();

  QRectF rect(QPointF(x1, y2), QPointF(x2, y1));

  if (data_ratio < canvas_ratio)
  {
    double new_width = fabs(rect.height() * canvas_ratio);
    double increment = new_width - rect.width();
    rect.setWidth(new_width);
    rect.moveLeft(rect.left() - 0.5 * increment);
  }
  else
  {
    double new_height = -(rect.width() / canvas_ratio);
    double increment = fabs(new_height - rect.height());
    rect.setHeight(new_height);
    rect.moveTop(rect.top() + 0.5 * increment);
  }
  if (rect.contains(_max_zoom_rect))
  {
    rect = _max_zoom_rect;
  }

  this->setAxisScale(yLeft, std::min(rect.bottom(), rect.top()), std::max(rect.bottom(), rect.top()));
  this->setAxisScale(xBottom, std::min(rect.left(), rect.right()), std::max(rect.left(), rect.right()));
  this->updateAxes();
}

void PlotWidget::resizeEvent(QResizeEvent* ev)
{
  QwtPlot::resizeEvent(ev);
  updateMaximumZoomArea();

  if (isXYPlot() && _keep_aspect_ratio)
  {
    rescaleEqualAxisScaling();
  }
}

void PlotWidget::updateLayout()
{
  QwtPlot::updateLayout();
  // qDebug() << canvasBoundingRect();
}

void PlotWidget::setConstantRatioXY(bool active)
{
  _keep_aspect_ratio = active;
  if (isXYPlot() && active)
  {
    _zoomer->keepAspectRatio(true);
  }
  else
  {
    _zoomer->keepAspectRatio(false);
  }
  zoomOut(false);
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal)
{
  QRectF current_rect = canvasBoundingRect();
  if (current_rect == rect)
  {
    return;
  }
  this->setAxisScale(yLeft, std::min(rect.bottom(), rect.top()), std::max(rect.bottom(), rect.top()));
  this->setAxisScale(xBottom, std::min(rect.left(), rect.right()), std::max(rect.left(), rect.right()));
  this->updateAxes();

  if (isXYPlot() && _keep_aspect_ratio)
  {
    rescaleEqualAxisScaling();
  }

  if (emit_signal)
  {
    if (isXYPlot())
    {
      emit undoableChange();
    }
    else
    {
      emit rectChanged(this, rect);
    }
  }
}

void PlotWidget::reloadPlotData()
{
  // TODO: this needs MUCH more testing

  int visible = 0;

  for (auto& it : _curve_list)
  {
    if (it.curve->isVisible()){
      visible++;
    }

    const auto& curve_name = it.src_name;

    auto data_it = _mapped_data.numeric.find(curve_name);
    if (data_it != _mapped_data.numeric.end())
    {
      auto ts = dynamic_cast<TransformedTimeseries*>(it.curve->data());
      if( ts )
      {
        ts->updateCache(true);
      }
    }
  }

  if (_curve_list.size() == 0 || visible == 0)
  {
    setDefaultRangeX();
  }
}

void PlotWidget::activateLegend(bool activate)
{
  _legend->setVisible(activate);
}

void PlotWidget::activateGrid(bool activate)
{
  _grid->enableX(activate);
  _grid->enableXMin(activate);
  _grid->enableY(activate);
  _grid->enableYMin(activate);
  _grid->attach(this);
}

void PlotWidget::configureTracker(CurveTracker::Parameter val)
{
  _tracker->setParameter(val);
}

void PlotWidget::enableTracker(bool enable)
{
  _tracker->setEnabled(enable && !isXYPlot());
}

bool PlotWidget::isTrackerEnabled() const
{
  return _tracker->isEnabled();
}

void PlotWidget::setTrackerPosition(double abs_time)
{
  if (isXYPlot())
  {
    for (auto& it : _curve_list)
    {
      auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
      auto pointXY = series->sampleFromTime(abs_time);
      if (pointXY)
      {
        it.marker->setValue(pointXY.value());
      }
    }
  }
  else
  {
    double relative_time = abs_time - _time_offset;
    _tracker->setPosition(QPointF(relative_time, 0.0));
  }
}

void PlotWidget::on_changeTimeOffset(double offset)
{
  auto prev_offset = _time_offset;
  _time_offset = offset;

  if (fabs(prev_offset - offset) > std::numeric_limits<double>::epsilon())
  {
    for (auto& it : _curve_list)
    {
      auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
      series->setTimeOffset(_time_offset);
    }
    if (!isXYPlot() && !_curve_list.empty())
    {
      QRectF rect = canvasBoundingRect();
      double delta = prev_offset - offset;
      rect.moveLeft(rect.left() + delta);
      setZoomRectangle(rect, false);
    }
  }
}

void PlotWidget::on_changeDateTimeScale(bool enable)
{
  _use_date_time_scale = enable;
  bool is_timescale = dynamic_cast<TimeScaleDraw*>(axisScaleDraw(QwtPlot::xBottom)) != nullptr;

  if (enable && !isXYPlot())
  {
    if (!is_timescale)
    {
      setAxisScaleDraw(QwtPlot::xBottom, new TimeScaleDraw());
    }
  }
  else
  {
    if (is_timescale)
    {
      setAxisScaleDraw(QwtPlot::xBottom, new QwtScaleDraw);
    }
  }
}

Range PlotWidget::getMaximumRangeX() const
{
  double left = std::numeric_limits<double>::max();
  double right = -std::numeric_limits<double>::max();

  for (auto& it : _curve_list)
  {
    if (!it.curve->isVisible())
      continue;

    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
    const auto max_range_X = series->getVisualizationRangeX();
    if (!max_range_X)
      continue;

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

// TODO report failure for empty dataset
Range PlotWidget::getMaximumRangeY(Range range_X) const
{
  double top = -std::numeric_limits<double>::max();
  double bottom = std::numeric_limits<double>::max();

  for (auto& it : _curve_list)
  {
    if (!it.curve->isVisible())
      continue;

    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());

    const auto max_range_X = series->getVisualizationRangeX();
    if (!max_range_X){
      continue;
    }

    double left = std::max(max_range_X->min, range_X.min);
    double right = std::min(max_range_X->max, range_X.max);

    left += _time_offset;
    right += _time_offset;
    left = std::nextafter(left, right);
    right = std::nextafter(right, left);

    auto range_Y = series->getVisualizationRangeY({ left, right });
    if (!range_Y)
    {
      qDebug() << " invalid range_Y in PlotWidget::maximumRangeY";
      continue;
    }
    if (top < range_Y->max){
      top = range_Y->max;
    }
    if (bottom > range_Y->min){
      bottom = range_Y->min;
    }
  }

  double margin = 0.1;

  if (bottom > top)
  {
    bottom = 0;
    top = 0;
  }

  if (top - bottom > std::numeric_limits<double>::epsilon())
  {
    margin = (top - bottom) * 0.025;
  }

  const bool lower_limit = _custom_Y_limits.min > -MAX_DOUBLE;
  const bool upper_limit = _custom_Y_limits.max < MAX_DOUBLE;

  if (lower_limit)
  {
    bottom = _custom_Y_limits.min;
    if (top < bottom){
      top = bottom + margin;
    }
  }

  if (upper_limit)
  {
    top = _custom_Y_limits.max;
    if (top < bottom){
      bottom = top - margin;
    }
  }

  if (!lower_limit && !upper_limit)
  {
    top += margin;
    bottom -= margin;
  }

  return Range({ bottom, top });
}

void PlotWidget::updateCurves()
{
  for (auto& it : _curve_list)
  {
    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
    series->updateCache(false);
    // TODO check res and do something if false.
  }
  updateMaximumZoomArea();
}

std::map<QString, QColor> PlotWidget::getCurveColors() const
{
  std::map<QString, QColor> color_by_name;

  for (auto& it : _curve_list)
  {
    color_by_name.insert( {it.curve->title().text(), it.curve->pen().color()} );
  }
  return color_by_name;
}

void PlotWidget::on_changeCurveColor(const QString& curve_name, QColor new_color)
{
  for(auto& it: _curve_list)
  {
    if (it.curve->title() == curve_name)
    {
      auto& curve = it.curve;
      if (curve->pen().color() != new_color)
      {
        curve->setPen(new_color, 1.3);
      }
      replot();
      break;
    }
  }
}

void PlotWidget::changeCurveStyle(QwtPlotCurve::CurveStyle style)
{
  _curve_style = style;
  for (auto& it : _curve_list)
  {
    auto& curve = it.curve;
    curve->setPen(curve->pen().color(), (_curve_style == QwtPlotCurve::Dots) ? 4.0 : 1.3);
    curve->setStyle(_curve_style);
  }
  replot();
}

void PlotWidget::on_showPoints_triggered()
{
  if (_curve_style == QwtPlotCurve::Lines)
  {
    _curve_style = QwtPlotCurve::LinesAndDots;
  }
  else if (_curve_style == QwtPlotCurve::LinesAndDots)
  {
    _curve_style = QwtPlotCurve::Dots;
  }
  else if (_curve_style == QwtPlotCurve::Dots)
  {
    _curve_style = QwtPlotCurve::Lines;
  }

  changeCurveStyle(_curve_style);
}

void PlotWidget::on_externallyResized(const QRectF& rect)
{
  QRectF current_rect = canvasBoundingRect();
  if (current_rect == rect)
  {
    return;
  }

  if (isXYPlot())
  {
    emit undoableChange();
  }
  else
  {
    emit rectChanged(this, rect);
  }
}

void PlotWidget::zoomOut(bool emit_signal)
{
  if (_curve_list.size() == 0)
  {
    QRectF rect(0, 1, 1, -1);
    this->setZoomRectangle(rect, false);
    return;
  }
  updateMaximumZoomArea();
  setZoomRectangle(_max_zoom_rect, emit_signal);
  replot();
}

void PlotWidget::on_zoomOutHorizontal_triggered(bool emit_signal)
{
  updateMaximumZoomArea();
  QRectF act = canvasBoundingRect();
  auto rangeX = getMaximumRangeX();

  act.setLeft(rangeX.min);
  act.setRight(rangeX.max);
  this->setZoomRectangle(act, emit_signal);
}

void PlotWidget::on_zoomOutVertical_triggered(bool emit_signal)
{
  updateMaximumZoomArea();
  QRectF rect = canvasBoundingRect();
  auto rangeY = getMaximumRangeY({ rect.left(), rect.right() });

  rect.setBottom(rangeY.min);
  rect.setTop(rangeY.max);
  this->setZoomRectangle(rect, emit_signal);
}

//void PlotWidget::on_changeToBuiltinTransforms(QString new_transform)
//{
//  _xy_mode = false;

//  enableTracker(true);

//  for (auto& it : _curve_list)
//  {
//    const auto& curve_name = it.first;
//    auto& curve = it.second;

//    _point_marker[curve_name]->setVisible(false);
//    curve->setTitle(QString::fromStdString(curve_name));
//    _curves_transform[curve_name] = new_transform;

//    auto data_it = _mapped_data.numeric.find(curve_name);
//    if (data_it != _mapped_data.numeric.end())
//    {
//      const auto& data = data_it->second;
//      auto data_series = createTimeSeries(new_transform, &data);
//      curve->setData(data_series);
//    }
//  }

//  _default_transform = new_transform;
//  zoomOut(true);
//  on_changeDateTimeScale(_use_date_time_scale);
//  replot();
//}

bool PlotWidget::isXYPlot() const
{
  return _xy_mode;
}

void PlotWidget::setModeXY(bool enable)
{
  if( enable == _xy_mode)
  {
    return;
  }
  _xy_mode = enable;

  enableTracker(!enable);

  if( enable ){
    QFont font_footer;
    font_footer.setPointSize(10);
    QwtText text("XY Plot");
    text.setFont(font_footer);
    this->setFooter(text);
  }
  else{
    this->setFooter("");
  }

  zoomOut(true);
  on_changeDateTimeScale(_use_date_time_scale);
  replot();
}

void PlotWidget::updateAvailableTransformers()
{
  QSettings settings;
  QByteArray xml_text = settings.value("AddCustomPlotDialog.savedXML", QByteArray()).toByteArray();
  if (!xml_text.isEmpty())
  {
    _snippets = GetSnippetsFromXML(xml_text);
  }
}

void PlotWidget::transformCustomCurves()
{
  std::string error_message;

  /*for (auto& curve_it : _curve_list)
  {
    auto& curve = curve_it.second;
    const auto& curve_name = curve_it.first;
    const auto& transform = _curves_transform.at(curve_name);

    auto data_it = _mapped_data.numeric.find(curve_name);
    if (data_it != _mapped_data.numeric.end())
    {
      auto& data = data_it->second;
      try
      {
        auto data_series = createTimeSeries(transform, &data);
        curve->setData(data_series);

        if (transform == noTransform || transform.isEmpty())
        {
          curve->setTitle(QString::fromStdString(curve_name));
        }
        else
        {
          curve->setTitle(QString::fromStdString(curve_name) + tr(" [") + transform + tr("]"));
        }
      }
      catch (std::runtime_error& err)
      {
        auto data_series = createTimeSeries(noTransform, &data);
        curve->setData(data_series);

        error_message += curve_name + (" [") + transform.toStdString() + ("]: ");
        error_message += err.what();

        curve->setTitle(QString::fromStdString(curve_name));
      }
    }
  }
  if (error_message.size() > 0)
  {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Warnings");
    msgBox.setText(tr("Something wrong happened while creating the following curves. "
                      "Please check that the transform equation is correct.\n\n") +
                   QString::fromStdString(error_message));
    msgBox.exec();
  }*/
}

void PlotWidget::on_savePlotToFile()
{
  QString fileName;

  QFileDialog saveDialog(this);
  saveDialog.setAcceptMode(QFileDialog::AcceptSave);

  QStringList filters;
  filters << "png (*.png)"
          << "jpg (*.jpg *.jpeg)"
          << "svg (*.svg)";

  saveDialog.setNameFilters(filters);
  saveDialog.exec();

  if (saveDialog.result() == QDialog::Accepted && !saveDialog.selectedFiles().empty())
  {
    fileName = saveDialog.selectedFiles().first();

    if (fileName.isEmpty())
    {
      return;
    }

    bool is_svg = false;
    QFileInfo fileinfo(fileName);
    if (fileinfo.suffix().isEmpty())
    {
      auto filter = saveDialog.selectedNameFilter();
      if (filter == filters[0])
      {
        fileName.append(".png");
      }
      else if (filter == filters[1])
      {
        fileName.append(".jpg");
      }
      else if (filter == filters[2])
      {
        fileName.append(".svg");
        is_svg = true;
      }
    }

    bool tracker_enabled = _tracker->isEnabled();
    if (tracker_enabled)
    {
      this->enableTracker(false);
      replot();
    }

    QRect documentRect(0, 0, 1200, 900);
    QwtPlotRenderer rend;

    if (is_svg)
    {
      QSvgGenerator generator;
      generator.setFileName(fileName);
      generator.setResolution(80);
      generator.setViewBox(documentRect);
      QPainter painter(&generator);
      rend.render(this, &painter, documentRect);
    }
    else
    {
      QPixmap pixmap(1200, 900);
      QPainter painter(&pixmap);
      rend.render(this, &painter, documentRect);
      pixmap.save(fileName);
    }

    if (tracker_enabled)
    {
      this->enableTracker(true);
      replot();
    }
  }
}

void PlotWidget::setCustomAxisLimits(Range range)
{
  _custom_Y_limits = range;
  on_zoomOutVertical_triggered(false);
  replot();
}

Range PlotWidget::customAxisLimit() const
{
  return _custom_Y_limits;
}

void PlotWidget::on_copyToClipboard()
{
  bool tracker_enabled = _tracker->isEnabled();
  if (tracker_enabled)
  {
    this->enableTracker(false);
    replot();
  }

  auto documentRect = this->canvas()->rect();
  qDebug() << documentRect;

  QwtPlotRenderer rend;
  QPixmap pixmap(documentRect.width(), documentRect.height());
  QPainter painter(&pixmap);
  rend.render(this, &painter, documentRect);

  QClipboard* clipboard = QGuiApplication::clipboard();
  clipboard->setPixmap(pixmap);

  if (tracker_enabled)
  {
    this->enableTracker(true);
    replot();
  }
}

void PlotWidget::on_copyAction_triggered()
{
  QDomDocument doc;
  auto root = doc.createElement("PlotWidgetClipBoard");
  auto el = xmlSaveState(doc);
  doc.appendChild(root);
  root.appendChild(el);

  QClipboard *clipboard = QGuiApplication::clipboard();
  clipboard->setText( doc.toString() );
}

void PlotWidget::on_pasteAction_triggered()
{
  QClipboard *clipboard = QGuiApplication::clipboard();
  QString clipboard_text = clipboard->text();

  QDomDocument doc;
  bool valid = doc.setContent(clipboard_text);
  if( !valid ){
    return;
  }
  auto root = doc.firstChildElement();
  if( root.tagName() !=  "PlotWidgetClipBoard")
  {
    return;
  }
  else{
    auto el = root.firstChildElement();
    xmlLoadState(el);
    clipboard->setText("");
    emit undoableChange();
  }
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* event)
{
  QwtScaleWidget* bottomAxis = this->axisWidget(xBottom);
  QwtScaleWidget* leftAxis = this->axisWidget(yLeft);

  if (_magnifier && (obj == bottomAxis || obj == leftAxis) && !(isXYPlot() && _keep_aspect_ratio))
  {
    if (event->type() == QEvent::Wheel)
    {
      auto wheel_event = dynamic_cast<QWheelEvent*>(event);
      if (obj == bottomAxis)
      {
        _magnifier->setDefaultMode(PlotMagnifier::X_AXIS);
      }
      else
      {
        _magnifier->setDefaultMode(PlotMagnifier::Y_AXIS);
      }
      _magnifier->widgetWheelEvent(wheel_event);
    }
  }

  if (obj == canvas())
  {
    if (_magnifier)
    {
      _magnifier->setDefaultMode(PlotMagnifier::BOTH_AXES);
    }
    return canvasEventFilter(event);
  }

  return false;
}

void PlotWidget::overrideCursonMove()
{
  QSettings settings;
  QString theme = settings.value("Preferences::theme", "light").toString();
  QPixmap pixmap(tr(":/style_%1/move.png").arg(theme));
  QApplication::setOverrideCursor(QCursor(pixmap.scaled(24, 24)));
}

bool PlotWidget::canvasEventFilter(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::Wheel: {
      auto mouse_event = dynamic_cast<QWheelEvent*>(event);

      bool ctrl_modifier = mouse_event->modifiers() == Qt::ControlModifier;
      auto legend_rect = _legend->geometry(canvas()->rect());

      if (ctrl_modifier)
      {
        if (legend_rect.contains(mouse_event->pos()) && _legend->isVisible())
        {
          int prev_size = _legend->font().pointSize();
          int new_size = prev_size;
          if (mouse_event->angleDelta().y() > 0)
          {
            new_size = std::min(13, prev_size+1);
          }
          if (mouse_event->angleDelta().y() < 0)
          {
            new_size = std::max(7, prev_size-1);
          }
          if( new_size != prev_size)
          {
            setLegendSize(new_size);
            emit legendSizeChanged(new_size);
          }
          return true;
        }
      }
      return false;
    }

    case QEvent::MouseButtonPress: {
      if (_dragging.mode != DragInfo::NONE)
      {
        return true;  // don't pass to canvas().
      }

      QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);

      if (mouse_event->button() == Qt::LeftButton)
      {
        const QPoint press_point = mouse_event->pos();
        if (mouse_event->modifiers() == Qt::ShiftModifier)  // time tracker
        {
          QPointF pointF(invTransform(xBottom, press_point.x()), invTransform(yLeft, press_point.y()));
          emit trackerMoved(pointF);
          return true;  // don't pass to canvas().
        }
        else if (mouse_event->modifiers() == Qt::ControlModifier)  // panner
        {
          overrideCursonMove();
        }
        else
        {
          auto clicked_item = _legend->processMousePressEvent(mouse_event);
          if (clicked_item)
          {
            for (auto& it : _curve_list)
            {
              if (clicked_item == it.curve)
              {
                it.curve->setVisible(!it.curve->isVisible());
                _tracker->redraw();
                on_zoomOutVertical_triggered();
                replot();
                return true;
              }
            }
          }
        }
        return false;  // send to canvas()
      }
      else if (mouse_event->buttons() == Qt::MiddleButton && mouse_event->modifiers() == Qt::NoModifier)
      {
        overrideCursonMove();
        return false;
      }
      else if (mouse_event->button() == Qt::RightButton)
      {
        if (mouse_event->modifiers() == Qt::NoModifier)  // show menu
        {
          canvasContextMenuTriggered(mouse_event->pos());
          return true;  // don't pass to canvas().
        }
      }
    }
    break;
      //---------------------------------
    case QEvent::MouseMove: {
      if (_dragging.mode != DragInfo::NONE)
      {
        return true;  // don't pass to canvas().
      }

      QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);

      if (mouse_event->buttons() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier)
      {
        const QPoint point = mouse_event->pos();
        QPointF pointF(invTransform(xBottom, point.x()), invTransform(yLeft, point.y()));
        emit trackerMoved(pointF);
        return true;
      }
    }
    break;

    case QEvent::Leave: {
      if (_dragging.mode == DragInfo::NONE)
      {
        return false;
      }
    }
    break;
    case QEvent::MouseButtonRelease: {
      if (_dragging.mode == DragInfo::NONE)
      {
        QApplication::restoreOverrideCursor();
        return false;
      }
    }
    break;

    case QEvent::Enter: {
      // If you think that this code doesn't make sense, you are right.
      // This is the workaround I have eventually found to avoid the problem with spurious
      // QEvent::DragLeave (I have never found the origin of the bug).
      if (_dragging.mode != DragInfo::NONE)
      {
        dropEvent(nullptr);
      }
      return true;
    }

    default: {
    }

  }  // end switch

  return false;
}

void PlotWidget::setDefaultRangeX()
{
  if (!_curve_list.empty())
  {
    double min = std::numeric_limits<double>::max();
    double max = -std::numeric_limits<double>::max();
    for (auto& it : _mapped_data.numeric)
    {
      const PlotData& data = it.second;
      if (data.size() > 0)
      {
        double A = data.front().x;
        double B = data.back().x;
        min = std::min(A, min);
        max = std::max(B, max);
      }
    }
    setAxisScale(xBottom, min - _time_offset, max - _time_offset);
  }
  else{
    setAxisScale(xBottom, 0.0, 1.0);
  }
}

QwtSeriesWrapper* PlotWidget::createCurveXY(const PlotData* data_x, const PlotData* data_y)
{
  QwtSeriesWrapper* output = nullptr;

  try
  {
    output = new PointSeriesXY(data_x, data_y);
  }
  catch (std::runtime_error& ex)
  {
    if (if_xy_plot_failed_show_dialog)
    {
      QMessageBox msgBox(this);
      msgBox.setWindowTitle("Warnings");
      msgBox.setText(tr("The creation of the XY plot failed with the following message:\n %1").arg(ex.what()));
      msgBox.addButton("Continue", QMessageBox::AcceptRole);
      msgBox.exec();
    }
    throw std::runtime_error("Creation of XY plot failed");
  }

  output->setTimeOffset(_time_offset);
  return output;
}

QwtSeriesWrapper* PlotWidget::createTimeSeries(const QString& transform_ID, const PlotData* data)
{
  TransformedTimeseries* output = new TransformedTimeseries(data);
  output->setTransform(transform_ID);
  output->setTimeOffset(_time_offset);
  output->updateCache(true);
  return output;
}

void PlotWidget::changeBackgroundColor(QColor color)
{
  if (canvasBackground().color() != color)
  {
    setCanvasBackground(color);
    replot();
  }
}

void PlotWidget::setLegendSize(int size)
{
  auto font = _legend->font();
  font.setPointSize(size);
  _legend->setFont(font);
  replot();
}

bool PlotWidget::isLegendVisible() const
{
  return _legend && _legend->isVisible();
}

void PlotWidget::setLegendAlignment(Qt::Alignment alignment)
{
  _legend->setAlignmentInCanvas(Qt::Alignment(Qt::AlignTop | alignment));
}

void PlotWidget::setZoomEnabled(bool enabled)
{
  _zoom_enabled = enabled;
  _zoomer->setEnabled(enabled);
  _magnifier->setEnabled(enabled);
  _panner1->setEnabled(enabled);
  _panner2->setEnabled(enabled);
}

bool PlotWidget::isZoomEnabled() const
{
  return _zoom_enabled;
}

void PlotWidget::replot()
{ 
  if (_zoomer)
  {
    _zoomer->setZoomBase(false);
  }

 // static int replot_count = 0;
  QwtPlot::replot();
  //qDebug() << replot_count++;
}
