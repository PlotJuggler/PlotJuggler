/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

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
#include "suggest_dialog.h"
#include "transforms/custom_function.h"
#include "plotwidget_editor.h"
#include "plotwidget_transforms.h"

#include "plotzoomer.h"
#include "plotmagnifier.h"
#include "plotlegend.h"

#include "PlotJuggler/save_plot.h"
#include "PlotJuggler/svg_util.h"
#include "point_series_xy.h"
#include "colormap_selector.h"

#include "statistics_dialog.h"

class TimeScaleDraw : public QwtScaleDraw
{
  virtual QwtText label(double v) const
  {
    const auto tz = QTimeZone(qEnvironmentVariable("TZ", "UTC").toUtf8());
    QDateTime dt = QDateTime::fromMSecsSinceEpoch((qint64)(v * 1000), tz);
    if (dt.date().year() == 1970 && dt.date().month() == 1 && dt.date().day() == 1)
    {
      return dt.toString("hh:mm:ss.z");
    }
    return dt.toString("hh:mm:ss.z\nyyyy MMM dd");
  }
};

const double MAX_DOUBLE = std::numeric_limits<double>::max() / 2;

static bool if_xy_plot_failed_show_dialog = true;

PlotWidget::PlotWidget(PlotDataMapRef& datamap, QWidget* parent)
  : PlotWidgetBase(parent)
  , _mapped_data(datamap)
  , _tracker(nullptr)
  , _use_date_time_scale(false)
  , _dragging({ DragInfo::NONE, {}, nullptr })
  , _time_offset(0.0)
  , _transform_select_dialog(nullptr)
  , _context_menu_enabled(true)
{
  connect(this, &PlotWidget::curveListChanged, this, [this]() { this->updateMaximumZoomArea(); });

  qwtPlot()->setAcceptDrops(true);

  //--------------------------
  _tracker = (new CurveTracker(qwtPlot(), Qt::red));
  _reference_tracker = (new CurveTracker(qwtPlot(), Qt::blue));
  _reference_tracker->setParameter(CurveTracker::LINE_ONLY);

  _grid = new QwtPlotGrid();
  _grid->setPen(QPen(Qt::gray, 0.0, Qt::DotLine));

  connect(this, &PlotWidgetBase::viewResized, this, &PlotWidget::on_externallyResized);

  connect(this, &PlotWidgetBase::dragEnterSignal, this, &PlotWidget::onDragEnterEvent);
  connect(this, &PlotWidgetBase::dragLeaveSignal, this, &PlotWidget::onDragLeaveEvent);

  connect(this, &PlotWidgetBase::dropSignal, this, &PlotWidget::onDropEvent);

  connect(this, &PlotWidgetBase::widgetResized, this, [this]() {
    if (isXYPlot() && keepRatioXY())
    {
      rescaleEqualAxisScaling();
    }
  });

  //-------------------------

  buildActions();

  _custom_Y_limits.min = (-MAX_DOUBLE);
  _custom_Y_limits.max = (MAX_DOUBLE);

  QwtSymbol* sym = new QwtSymbol(QwtSymbol::Ellipse, Qt::black, QPen(Qt::black), QSize(5, 5));

  _show_point_marker = (new QwtPlotMarker);
  _show_point_marker->attach(qwtPlot());
  _show_point_marker->setSymbol(sym);

  _show_point_text = new QwtPlotMarker();
  _show_point_text->attach(qwtPlot());
}

PlotWidget::~PlotWidget()
{
  delete _action_split_horizontal;
  delete _action_split_vertical;
  delete _action_removeAllCurves;
  delete _action_zoomOutMaximum;
  delete _action_zoomOutHorizontally;
  delete _action_zoomOutVertically;
  delete _action_saveToFile;
  delete _action_copy;
  delete _action_paste;
  delete _action_image_to_clipboard;
  delete _action_data_statistics;
}

void PlotWidget::setContextMenuEnabled(bool enabled)
{
  _context_menu_enabled = enabled;
}

void PlotWidget::buildActions()
{
  QIcon iconDeleteList;

  _action_edit = new QAction("&Edit curves...", this);
  connect(_action_edit, &QAction::triggered, this, [this]() {
    auto editor_dialog = new PlotwidgetEditor(this, qwtPlot());
    editor_dialog->exec();
    editor_dialog->deleteLater();
  });

  _action_formula = new QAction("&Apply filter to data...", this);
  connect(_action_formula, &QAction::triggered, this, [this]() {
    auto editor_dialog = new DialogTransformEditor(this);
    int res = editor_dialog->exec();
    editor_dialog->deleteLater();
    if (res == QDialog::Accepted)
    {
      emit undoableChange();
    }
  });

  _action_split_horizontal = new QAction("&Split Horizontally", this);
  connect(_action_split_horizontal, &QAction::triggered, this, &PlotWidget::splitHorizontal);

  _action_split_vertical = new QAction("&Split Vertically", this);
  connect(_action_split_vertical, &QAction::triggered, this, &PlotWidget::splitVertical);

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

  _flip_x = new QAction("&Flip Horizontal Axis", this);
  _flip_x->setCheckable(true);
  connect(_flip_x, &QAction::changed, this, &PlotWidget::onFlipAxis);

  _flip_y = new QAction("&Flip Vertical Axis", this);
  _flip_y->setCheckable(true);
  connect(_flip_y, &QAction::changed, this, &PlotWidget::onFlipAxis);

  _action_data_statistics = new QAction("&Show data statistics", this);
  connect(_action_data_statistics, &QAction::triggered, this, &PlotWidget::onShowDataStatistics);
}

void PlotWidget::canvasContextMenuTriggered(const QPoint& pos)
{
  if (_context_menu_enabled == false)
  {
    return;
  }

  QSettings settings;
  QString theme = settings.value("StyleSheet::theme", "light").toString();

  _action_removeAllCurves->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  _action_edit->setIcon(LoadSvg(":/resources/svg/pencil-edit.svg", theme));
  _action_formula->setIcon(LoadSvg(":/resources/svg/Fx.svg", theme));
  _action_split_horizontal->setIcon(LoadSvg(":/resources/svg/add_column.svg", theme));
  _action_split_vertical->setIcon(LoadSvg(":/resources/svg/add_row.svg", theme));
  _action_zoomOutMaximum->setIcon(LoadSvg(":/resources/svg/zoom_max.svg", theme));
  _action_zoomOutHorizontally->setIcon(LoadSvg(":/resources/svg/zoom_horizontal.svg", theme));
  _action_zoomOutVertically->setIcon(LoadSvg(":/resources/svg/zoom_vertical.svg", theme));
  _action_copy->setIcon(LoadSvg(":/resources/svg/copy.svg", theme));
  _action_paste->setIcon(LoadSvg(":/resources/svg/paste.svg", theme));
  _action_saveToFile->setIcon(LoadSvg(":/resources/svg/save.svg", theme));
  _action_image_to_clipboard->setIcon(LoadSvg(":/resources/svg/plot_image.svg", theme));

  QMenu menu(qwtPlot());

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
  if (isXYPlot())
  {
    menu.addAction(_flip_x);
  }
  menu.addAction(_flip_y);
  menu.addSeparator();
  menu.addAction(_action_copy);
  menu.addAction(_action_paste);
  menu.addAction(_action_image_to_clipboard);
  menu.addAction(_action_saveToFile);
  menu.addAction(_action_data_statistics);

  // check the clipboard
  QClipboard* clipboard = QGuiApplication::clipboard();
  QString clipboard_text = clipboard->text();
  QDomDocument doc;
  bool valid_clipbaord = (!clipboard_text.isEmpty() &&       // not empty
                          doc.setContent(clipboard_text) &&  // valid xml
                          doc.firstChildElement().tagName() == "PlotWidgetClipBoard");

  _action_paste->setEnabled(valid_clipbaord);

  _action_removeAllCurves->setEnabled(!curveList().empty());
  _action_formula->setEnabled(!curveList().empty() && !isXYPlot());

  menu.exec(qwtPlot()->canvas()->mapToGlobal(pos));
}

PlotWidget::CurveInfo* PlotWidget::addCurveXY(std::string name_x, std::string name_y,
                                              QString curve_name)
{
  std::string name = curve_name.toStdString();

  while (name.empty())
  {
    SuggestDialog dialog(name_x, name_y, qwtPlot());
    auto ret = dialog.exec();

    curve_name = dialog.suggestedName();
    name = curve_name.toStdString();
    name_x = dialog.nameX().toStdString();
    name_y = dialog.nameY().toStdString();

    if (ret != QDialog::Accepted)
    {
      return nullptr;
    }

    auto curve_it = curveFromTitle(curve_name);

    if (name.empty() || curve_it)
    {
      int ret =
          QMessageBox::warning(qwtPlot(), "Missing name",
                               "The name of the curve is missing or exist already. "
                               "Try again or abort.",
                               QMessageBox::Abort | QMessageBox::Retry, QMessageBox::NoButton);
      if (ret == QMessageBox::Abort || ret == QMessageBox::NoButton)
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
    QMessageBox::warning(qwtPlot(), "Exception!", ex.what());
    return nullptr;
  }

  QColor color = getColorHint(nullptr);
  curve->setPen(color);
  setStyle(curve, curveStyle());

  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

  curve->attach(qwtPlot());

  auto marker = new QwtPlotMarker;
  marker->attach(qwtPlot());
  marker->setVisible(isXYPlot());
  QwtSymbol* sym = new QwtSymbol(QwtSymbol::Ellipse, color, QPen(Qt::black), QSize(8, 8));
  marker->setSymbol(sym);

  CurveInfo curve_info;
  curve_info.curve = curve;
  curve_info.marker = marker;
  curve_info.src_name = name;
  curveList().push_back(curve_info);

  return &(curveList().back());
}

PlotWidgetBase::CurveInfo* PlotWidget::addCurve(const std::string& name, QColor color)
{
  PlotWidgetBase::CurveInfo* info = nullptr;

  auto it1 = _mapped_data.numeric.find(name);
  if (it1 != _mapped_data.numeric.end())
  {
    info = PlotWidgetBase::addCurve(name, it1->second, color);
  }

  auto it2 = _mapped_data.scatter_xy.find(name);
  if (it2 != _mapped_data.scatter_xy.end())
  {
    info = PlotWidgetBase::addCurve(name, it2->second, color);
  }

  if (info && info->curve)
  {
    if (auto timeseries = dynamic_cast<QwtTimeseries*>(info->curve->data()))
    {
      timeseries->setTimeOffset(_time_offset);
    }
  }
  _tracker->redraw();
  _reference_tracker->redraw();
  return info;
}

void PlotWidget::removeCurve(const QString& title)
{
  PlotWidgetBase::removeCurve(title);
  _tracker->redraw();
  _reference_tracker->redraw();
}

void PlotWidget::onDataSourceRemoved(const std::string& src_name)
{
  bool deleted = false;

  for (auto it = curveList().begin(); it != curveList().end();)
  {
    PointSeriesXY* curve_xy = dynamic_cast<PointSeriesXY*>(it->curve->data());
    bool remove_curve_xy = curve_xy && (curve_xy->dataX()->plotName() == src_name ||
                                        curve_xy->dataY()->plotName() == src_name);

    if (it->src_name == src_name || remove_curve_xy)
    {
      deleted = true;
      it->curve->detach();
      it->marker->detach();
      it = curveList().erase(it);
    }
    else
    {
      it++;
    }
  }

  if (deleted)
  {
    _tracker->redraw();
    _reference_tracker->redraw();
    emit curveListChanged();
  }
  if (_background_item && _background_item->dataName() == QString::fromStdString(src_name))
  {
    _background_item->detach();
    _background_item.reset();
  }
}

void PlotWidget::removeAllCurves()
{
  PlotWidgetBase::removeAllCurves();
  setModeXY(false);
  _tracker->redraw();
  _reference_tracker->redraw();
  _flip_x->setChecked(false);
  _flip_y->setChecked(false);
}

void PlotWidget::onDragEnterEvent(QDragEnterEvent* event)
{
  const QMimeData* mimeData = event->mimeData();
  QStringList mimeFormats = mimeData->formats();

  _dragging.mode = DragInfo::NONE;
  _dragging.curves.clear();
  _dragging.source = event->source();

  auto& format = mimeFormats.first();
  QByteArray encoded = mimeData->data(format);
  QDataStream stream(&encoded, QIODevice::ReadOnly);

  while (!stream.atEnd())
  {
    QString curve_name;
    stream >> curve_name;
    auto name = curve_name.toStdString();
    if (!curve_name.isEmpty())
    {
      _dragging.curves.push_back(curve_name);
    }
    if (_mapped_data.numeric.count(name) == 0 && _mapped_data.scatter_xy.count(name) == 0)
    {
      event->ignore();
      return;
    }
  }

  if (_dragging.curves.empty())
  {
    event->ignore();
    return;
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
    if (curveList().empty() || isXYPlot())
    {
      _dragging.mode = DragInfo::NEW_XY;
      event->acceptProposedAction();
    }
  }
}

void PlotWidget::onDragLeaveEvent(QDragLeaveEvent* event)
{
  _dragging.mode = DragInfo::NONE;
  _dragging.curves.clear();
}

void PlotWidget::onDropEvent(QDropEvent*)
{
  bool curves_changed = false;

  bool noCurves = curveList().empty();

  if (_dragging.mode == DragInfo::CURVES)
  {
    size_t scatter_count = 0;
    for (const auto& curve_name : _dragging.curves)
    {
      scatter_count += _mapped_data.scatter_xy.count(curve_name.toStdString());
    }
    bool scatter_curves = (scatter_count == _dragging.curves.size());
    if (scatter_count > 0 && !scatter_curves)
    {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
      QMessageBox::warning(qwtPlot(), "Warning",
                           "You can not drag XY (scatter) data and timeseries into the "
                           "same plot");
      return;
    }

    // if there aren't other curves, you can change the mode
    if (curveList().empty())
    {
      setModeXY(scatter_curves);
    }

    if (isXYPlot() && !scatter_curves)
    {
      QMessageBox::warning(qwtPlot(), "Warning",
                           tr("This is a [XY plot], you can not drop a timeseries here.\n"
                              "To convert this widget into a [timeseries plot], "
                              "you must first remove all its curves."));
    }
    if (!isXYPlot() && scatter_curves)
    {
      QMessageBox::warning(qwtPlot(), "Warning",
                           tr("This is a [timeseries plot], you can not "
                              "drop XY scatter data here.\n"
                              "To convert this widget into a [XY plot], "
                              "you must first remove all its curves."));
    }

    if (isXYPlot() != scatter_curves)
    {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
      return;
    }

    for (const auto& curve_name : _dragging.curves)
    {
      bool added = addCurve(curve_name.toStdString()) != nullptr;
      curves_changed = curves_changed || added;
    }
  }
  else if (_dragging.mode == DragInfo::NEW_XY && _dragging.curves.size() == 2)
  {
    if (!curveList().empty() && !isXYPlot())
    {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
      QMessageBox::warning(qwtPlot(), "Warning",
                           tr("This is a [timeseries plot], you can not "
                              "drop XY scatter data here.\n"
                              "To convert this widget into a [XY plot], "
                              "you must first remove all its curves."));
      return;
    }

    setModeXY(true);
    addCurveXY(_dragging.curves[0].toStdString(), _dragging.curves[1].toStdString());

    curves_changed = true;
  }

  if (curves_changed)
  {
    emit curvesDropped();
    emit curveListChanged();

    QSettings settings;
    bool autozoom_curve_added = settings.value("Preferences::autozoom_curve_added", true).toBool();
    if (autozoom_curve_added || noCurves)
    {
      zoomOut(autozoom_curve_added);
    }
    else
    {
      replot();
    }
  }
  _dragging.mode = DragInfo::NONE;
  _dragging.curves.clear();
}

void PlotWidget::on_panned(int, int)
{
  on_externallyResized(currentBoundingRect());
}

QDomElement PlotWidget::xmlSaveState(QDomDocument& doc) const
{
  QDomElement plot_el = doc.createElement("plot");

  QDomElement range_el = doc.createElement("range");
  QRectF rect = currentBoundingRect();
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

  if (overriddenCurvesStyle().has_value())
  {
    switch (overriddenCurvesStyle().value())
    {
      case PlotWidgetBase::LINES:
        plot_el.setAttribute("style", "Lines");
        break;
      case PlotWidgetBase::DOTS:
        plot_el.setAttribute("style", "Dots");
        break;
      case PlotWidgetBase::LINES_AND_DOTS:
        plot_el.setAttribute("style", "LinesAndDots");
        break;
      case PlotWidgetBase::STICKS:
        plot_el.setAttribute("style", "Sticks");
        break;
      case PlotWidgetBase::STEPS:
        plot_el.setAttribute("style", "Steps");
        break;
      case PlotWidgetBase::STEPSINV:
        plot_el.setAttribute("style", "StepsInv");
        break;
    }
  }

  for (auto& it : curveList())
  {
    auto& name = it.src_name;
    QwtPlotCurve* curve = it.curve;
    QDomElement curve_el = doc.createElement("curve");
    curve_el.setAttribute("name", QString::fromStdString(name));
    curve_el.setAttribute("color", curve->pen().color().name());

    plot_el.appendChild(curve_el);

    if (isXYPlot())
    {
      if (auto xy = dynamic_cast<PointSeriesXY*>(curve->data()))
      {
        curve_el.setAttribute("curve_x", QString::fromStdString(xy->dataX()->plotName()));
        curve_el.setAttribute("curve_y", QString::fromStdString(xy->dataY()->plotName()));
      }
    }
    else
    {
      auto ts = dynamic_cast<TransformedTimeseries*>(curve->data());
      if (ts && ts->transform())
      {
        QDomElement transform_el = doc.createElement("transform");
        transform_el.setAttribute("name", ts->transformName());
        transform_el.setAttribute("alias", ts->alias());
        ts->transform()->xmlSaveState(doc, transform_el);
        curve_el.appendChild(transform_el);
      }
    }
  }

  plot_el.setAttribute("mode", isXYPlot() ? "XYPlot" : "TimeSeries");

  plot_el.setAttribute("flip_x", isXYPlot() && _flip_x->isChecked() ? "true" : "false");
  plot_el.setAttribute("flip_y", _flip_y->isChecked() ? "true" : "false");

  switch (lineWidth())
  {
    case POINTS_1_0:
      plot_el.setAttribute("line_width", "1.0");
      break;
    case POINTS_1_5:
      plot_el.setAttribute("line_width", "1.5");
      break;
    case POINTS_2_0:
      plot_el.setAttribute("line_width", "2.0");
      break;
    case POINTS_3_0:
      plot_el.setAttribute("line_width", "3.0");
      break;
  }

  if (_background_item)
  {
    plot_el.setAttribute("background_data", _background_item->dataName());
    plot_el.setAttribute("background_colormap", _background_item->colormapName());
  }

  return plot_el;
}

bool PlotWidget::xmlLoadState(QDomElement& plot_widget, bool autozoom)
{
  std::set<std::string> added_curve_names;

  QString mode = plot_widget.attribute("mode");
  setModeXY(mode == "XYPlot");

  _flip_x->setChecked(plot_widget.attribute("flip_x") == "true");
  _flip_y->setChecked(plot_widget.attribute("flip_y") == "true");

  auto line_width_str = plot_widget.attribute("line_width", QString("1.0"));
  if (line_width_str == "1.0")
  {
    setLineWidth(LineWidth::POINTS_1_0);
  }
  else if (line_width_str == "1.5")
  {
    setLineWidth(LineWidth::POINTS_1_5);
  }
  else if (line_width_str == "2.0")
  {
    setLineWidth(LineWidth::POINTS_2_0);
  }
  else if (line_width_str == "3.0")
  {
    setLineWidth(LineWidth::POINTS_3_0);
  }

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
  for (auto& it : curveList())
  {
    it.curve->detach();
    it.marker->detach();
  }
  curveList().clear();

  // insert curves
  QStringList missing_curves;
  for (QDomElement curve_element = plot_widget.firstChildElement("curve"); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement("curve"))
  {
    bool is_merged_xy =
        curve_element.hasAttribute("curve_x") && curve_element.hasAttribute("curve_y");
    bool is_timeseries = !isXYPlot();
    bool is_scatter_xy = !is_timeseries && !is_merged_xy;

    QString curve_name = curve_element.attribute("name");
    std::string curve_name_std = curve_name.toStdString();
    QColor color(curve_element.attribute("color"));

    //-----------------
    if (is_timeseries || is_scatter_xy)
    {
      if ((is_timeseries && _mapped_data.numeric.count(curve_name_std) == 0) ||
          (!is_timeseries && _mapped_data.scatter_xy.count(curve_name_std) == 0))
      {
        missing_curves.append(curve_name);
      }
      else
      {
        auto curve_info = addCurve(curve_name_std, color);
        if (!curve_info)
        {
          continue;
        }
        auto& curve = curve_info->curve;
        added_curve_names.insert(curve_name_std);

        auto ts = dynamic_cast<TransformedTimeseries*>(curve->data());
        QDomElement transform_el = curve_element.firstChildElement("transform");
        if (ts && transform_el.isNull() == false)
        {
          if (!ts->setTransform(transform_el.attribute("name")) || !ts->transform())
          {
            QMessageBox::warning(
                qwtPlot(), "Warning",
                tr("Can't restore the transform for curve [%1].\n"
                   "Transform [%2] not found.\nAre you using an old configuration file?")
                    .arg(curve_name, transform_el.attribute("name")));
            continue;
          }
          ts->transform()->xmlLoadState(transform_el);
          ts->updateCache(true);
          auto alias = transform_el.attribute("alias");
          ts->setAlias(alias);
          curve->setTitle(alias);
        }
      }
    }
    //-----------------
    if (is_merged_xy)
    {
      std::string curve_x = curve_element.attribute("curve_x").toStdString();
      std::string curve_y = curve_element.attribute("curve_y").toStdString();
      if (_mapped_data.numeric.find(curve_x) == _mapped_data.numeric.end() ||
          _mapped_data.numeric.find(curve_y) == _mapped_data.numeric.end())
      {
        missing_curves.append(curve_name);
      }
      else
      {
        auto curve_it = addCurveXY(curve_x, curve_y, curve_name);
        if (!curve_it)
        {
          continue;
        }
        curve_it->curve->setPen(color, 1.3);
        curve_it->marker->setSymbol(
            new QwtSymbol(QwtSymbol::Ellipse, color, QPen(Qt::black), QSize(8, 8)));
        added_curve_names.insert(curve_name_std);
      }
    }
  }

  if (missing_curves.size() > 0 && !warning_message_shown)
  {
    QMessageBox::warning(qwtPlot(), "Warning",
                         tr("Can't find one or more curves.\n"
                            "This message will be shown only once.\n%1")
                             .arg(missing_curves.join(",\n")));
    warning_message_shown = true;
  }

  emit curveListChanged();

  //-----------------------------------------

  QDomElement rectangle = plot_widget.firstChildElement("range");

  if (!rectangle.isNull() && autozoom)
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
      overrideCurvesStyle(std::nullopt);
    }
    else if (style == "LinesAndDots")
    {
      overrideCurvesStyle(PlotWidgetBase::LINES_AND_DOTS);
    }
    else if (style == "Dots")
    {
      overrideCurvesStyle(PlotWidgetBase::DOTS);
    }
    else if (style == "Sticks")
    {
      overrideCurvesStyle(PlotWidgetBase::STICKS);
    }
    else if (style == "Steps")
    {
      overrideCurvesStyle(PlotWidgetBase::STEPS);
    }
    else if (style == "StepsInv")
    {
      overrideCurvesStyle(PlotWidgetBase::STEPSINV);
    }
  }
  else
  {
    overrideCurvesStyle(std::nullopt);
  }
  updateCurvesStyle();

  QString bg_data = plot_widget.attribute("background_data");
  QString bg_colormap = plot_widget.attribute("background_colormap");

  if (!bg_data.isEmpty() && !bg_colormap.isEmpty())
  {
    auto plot_it = datamap().numeric.find(bg_data.toStdString());
    if (plot_it == datamap().numeric.end())
    {
      QMessageBox::warning(qwtPlot(), "Warning",
                           tr("Can't restore the background color.\n"
                              "Series [%1] not found.")
                               .arg(bg_data));
    }
    else
    {
      auto color_it = ColorMapLibrary().find(bg_colormap);
      if (color_it == ColorMapLibrary().end())
      {
        QMessageBox::warning(qwtPlot(), "Warning",
                             tr("Can't restore the background color.\n"
                                "ColorMap [%1] not found.")
                                 .arg(bg_colormap));
      }
      else
      {
        // everything fine.
        _background_item = std::make_unique<BackgroundColorItem>(plot_it->second, bg_colormap);
        _background_item->setTimeOffset(&_time_offset);
        _background_item->attach(qwtPlot());
      }
    }
  }

  if (autozoom)
  {
    updateMaximumZoomArea();
  }
  replot();
  return true;
}

void PlotWidget::rescaleEqualAxisScaling()
{
  QRectF canvas_rect = qwtPlot()->canvas()->contentsRect();

  auto max_rect = maxZoomRect();
  const double canvas_ratio = std::abs(canvas_rect.width() / canvas_rect.height());
  const double max_ratio = std::abs(max_rect.width() / max_rect.height());

  QRectF rect = max_rect;

  if (max_ratio < canvas_ratio)
  {
    double new_width = (-max_rect.height() * canvas_ratio);
    rect.setWidth(new_width);
  }
  else
  {
    double new_height = (-max_rect.width() / canvas_ratio);
    rect.setHeight(new_height);
  }

  rect.moveCenter(max_rect.center());

  setAxisScale(QwtPlot::yLeft, rect.bottom(), rect.top());
  setAxisScale(QwtPlot::xBottom, rect.left(), rect.right());
  qwtPlot()->updateAxes();
  replot();
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal)
{
  if (isXYPlot() && keepRatioXY())
  {
    rescaleEqualAxisScaling();
  }
  else
  {
    setAxisScale(QwtPlot::yLeft, rect.bottom(), rect.top());
    setAxisScale(QwtPlot::xBottom, rect.left(), rect.right());
    qwtPlot()->updateAxes();
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
  updateStatistics();
}

void PlotWidget::reloadPlotData()
{
  // TODO: this needs MUCH more testing

  int visible = 0;

  for (auto& it : curveList())
  {
    if (it.curve->isVisible())
    {
      visible++;
    }

    const auto& curve_name = it.src_name;

    auto data_it = _mapped_data.numeric.find(curve_name);
    if (data_it != _mapped_data.numeric.end())
    {
      if (auto ts = dynamic_cast<TransformedTimeseries*>(it.curve->data()))
      {
        ts->updateCache(true);
      }
    }
  }

  if (curveList().size() == 0 || visible == 0)
  {
    setDefaultRangeX();
  }
}

void PlotWidget::activateLegend(bool activate)
{
  legend()->setVisible(activate);
}

void PlotWidget::activateGrid(bool activate)
{
  _grid->enableX(activate);
  _grid->enableXMin(activate);
  _grid->enableY(activate);
  _grid->enableYMin(activate);
  _grid->attach(qwtPlot());
}

void PlotWidget::configureTracker(CurveTracker::Parameter val)
{
  _tracker->setParameter(val);
}

void PlotWidget::enableTracker(bool enable)
{
  _tracker->setEnabled(enable && !isXYPlot());
  _reference_tracker->setEnabled(enable && !isXYPlot());
}

bool PlotWidget::isTrackerEnabled() const
{
  return _tracker->isEnabled();
}

void PlotWidget::setTrackerPosition(double abs_time)
{
  if (isXYPlot())
  {
    for (auto& it : curveList())
    {
      if (auto series = dynamic_cast<QwtTimeseries*>(it.curve->data()))
      {
        auto pointXY = series->sampleFromTime(abs_time);
        if (pointXY)
        {
          it.marker->setValue(pointXY.value());
        }
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
  // move the trackers
  if (!isXYPlot())
  {
    double prev_tracker = _tracker->actualPosition().x();
    double new_tracker = prev_tracker + (prev_offset - offset);
    _tracker->setPosition(QPointF(new_tracker, 0.0));
  }

  if (fabs(prev_offset - offset) > std::numeric_limits<double>::epsilon())
  {
    for (auto& it : curveList())
    {
      if (auto series = dynamic_cast<QwtTimeseries*>(it.curve->data()))
      {
        series->setTimeOffset(_time_offset);
      }
    }
    if (!isXYPlot() && !curveList().empty())
    {
      QRectF rect = currentBoundingRect();
      double delta = prev_offset - offset;
      rect.moveLeft(rect.left() + delta);
      setZoomRectangle(rect, false);
    }
  }
  updateMaximumZoomArea();
}

void PlotWidget::on_changeDateTimeScale(bool enable)
{
  _use_date_time_scale = enable;
  bool is_timescale =
      dynamic_cast<TimeScaleDraw*>(qwtPlot()->axisScaleDraw(QwtPlot::xBottom)) != nullptr;

  if (enable && !isXYPlot())
  {
    if (!is_timescale)
    {
      qwtPlot()->setAxisScaleDraw(QwtPlot::xBottom, new TimeScaleDraw());
    }
  }
  else
  {
    if (is_timescale)
    {
      qwtPlot()->setAxisScaleDraw(QwtPlot::xBottom, new QwtScaleDraw);
    }
  }
}

// TODO report failure for empty dataset
Range PlotWidget::getVisualizationRangeY(Range range_X) const
{
  auto [bottom, top] = PlotWidgetBase::getVisualizationRangeY(range_X);

  const bool lower_limit = _custom_Y_limits.min > -MAX_DOUBLE;
  const bool upper_limit = _custom_Y_limits.max < MAX_DOUBLE;

  if (lower_limit)
  {
    bottom = _custom_Y_limits.min;
    if (top < bottom)
    {
      top = bottom;
    }
  }

  if (upper_limit)
  {
    top = _custom_Y_limits.max;
    if (top < bottom)
    {
      bottom = top;
    }
  }

  return Range({ bottom, top });
}

void PlotWidget::updateCurves(bool reset_older_data)
{
  for (auto& it : curveList())
  {
    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
    series->updateCache(reset_older_data);
  }
  updateMaximumZoomArea();

  updateStatistics(true);
}

void PlotWidget::updateStatistics(bool forceUpdate)
{
  if (_statistics_dialog)
  {
    if (_statistics_dialog->calcVisibleRange() || forceUpdate)
    {
      auto rect = currentBoundingRect();
      _statistics_dialog->update({ rect.left(), rect.right() });
    }
  }
}

void PlotWidget::changeDots(bool force_dots)
{
  setDefaultStyle(force_dots ? CurveStyle::LINES_AND_DOTS : CurveStyle::LINES);
  updateCurvesStyle();
}

void PlotWidget::on_changeCurveColor(const QString& curve_name, QColor new_color)
{
  for (auto& it : curveList())
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

void PlotWidget::onFlipAxis()
{
  if (isXYPlot())
  {
    rescaleEqualAxisScaling();
  }
  else
  {
    QRectF canvas_rect = qwtPlot()->canvas()->contentsRect();
    const QwtScaleMap xMap = qwtPlot()->canvasMap(QwtPlot::xBottom);
    const QwtScaleMap yMap = qwtPlot()->canvasMap(QwtPlot::yLeft);
    canvas_rect = canvas_rect.normalized();
    double x1 = xMap.invTransform(canvas_rect.left());
    double x2 = xMap.invTransform(canvas_rect.right());
    double y1 = yMap.invTransform(canvas_rect.bottom());
    double y2 = yMap.invTransform(canvas_rect.top());
    // flip will be done inside the function setAxisScale()
    setAxisScale(QwtPlot::yLeft, y1, y2);
    setAxisScale(QwtPlot::xBottom, x1, x2);
    qwtPlot()->updateAxes();
    replot();
  }
  emit undoableChange();
}

void PlotWidget::onBackgroundColorRequest(QString name)
{
  QString prev_colormap;
  if (name.isEmpty())
  {
    if (_background_item)
    {
      name = _background_item->dataName();
      prev_colormap = _background_item->colormapName();
    }
    else
    {
      return;
    }
  }

  auto plot_it = datamap().numeric.find(name.toStdString());
  if (plot_it == datamap().numeric.end())
  {
    if (_background_item)
    {
      _background_item->detach();
      _background_item.reset();
      replot();
    }
    return;
  }

  ColormapSelectorDialog dialog(name, prev_colormap, this);
  auto ret = dialog.exec();
  if (ret == QDialog::Accepted)
  {
    if (_background_item)
    {
      _background_item->detach();
      _background_item.reset();
    }

    QString colormap = dialog.selectedColorMap();
    if (!colormap.isEmpty() && ColorMapLibrary().count(colormap) != 0)
    {
      _background_item = std::make_unique<BackgroundColorItem>(plot_it->second, colormap);
      _background_item->setTimeOffset(&_time_offset);
      _background_item->attach(qwtPlot());
    }
    replot();
  }
}

void PlotWidget::onReferenceLineChecked(bool checked, double reference_value)
{
  if (isXYPlot())
  {
    return;
  }

  if (checked)
  {
    QPointF reference_point(reference_value - _time_offset, 0);
    _reference_tracker->setEnabled(true);
    _reference_tracker->setPosition(reference_point);
    _tracker->setReferencePosition(reference_point);
  }
  if (!checked)
  {
    _reference_tracker->setEnabled(false);
    _tracker->setReferencePosition(std::nullopt);
  }
  qwtPlot()->replot();
}

void PlotWidget::onShowPlot(bool checked)
{
  _show_point_enabled = checked;
}

void PlotWidget::setStatisticsTitle(QString title)
{
  _statistics_window_title = title;

  if (_statistics_dialog)
  {
    _statistics_dialog->setTitle(_statistics_window_title);
  }
}

void PlotWidget::onShowDataStatistics()
{
  if (!_statistics_dialog)
  {
    _statistics_dialog = new StatisticsDialog(this);
  }

  setStatisticsTitle(_statistics_window_title);

  auto rect = currentBoundingRect();
  _statistics_dialog->update({ rect.left(), rect.right() });
  _statistics_dialog->show();
  _statistics_dialog->raise();
  _statistics_dialog->activateWindow();

  _statistics_dialog->setAttribute(Qt::WA_DeleteOnClose);

  auto setToNull = [this]() { _statistics_dialog = nullptr; };

  connect(this, &PlotWidget::rectChanged, _statistics_dialog, [this](PlotWidget*, QRectF rect) {
    _statistics_dialog->update({ rect.left(), rect.right() });
  });

  connect(_statistics_dialog, &QDialog::rejected, this, setToNull);

  connect(this, &PlotWidgetBase::curveListChanged, this, [this]() { updateStatistics(); });
}

void PlotWidget::on_externallyResized(const QRectF& rect)
{
  QRectF current_rect = currentBoundingRect();
  if (current_rect == rect)
  {
    return;
  }

  if (!isXYPlot() && isZoomLinkEnabled())
  {
    emit rectChanged(this, rect);
  }
}

void PlotWidget::zoomOut(bool emit_signal)
{
  if (curveList().size() == 0)
  {
    QRectF rect(0, 1, 1, -1);
    this->setZoomRectangle(rect, false);
    return;
  }
  updateMaximumZoomArea();

  setZoomRectangle(maxZoomRect(), emit_signal);
  replot();
}

void PlotWidget::on_zoomOutHorizontal_triggered(bool emit_signal)
{
  updateMaximumZoomArea();
  QRectF act = currentBoundingRect();
  auto rangeX = getVisualizationRangeX();

  act.setLeft(rangeX.min);
  act.setRight(rangeX.max);
  setZoomRectangle(act, emit_signal);
}

void PlotWidget::on_zoomOutVertical_triggered(bool emit_signal)
{
  updateMaximumZoomArea();
  QRectF rect = currentBoundingRect();
  auto rangeY = getVisualizationRangeY({ rect.left(), rect.right() });

  rect.setBottom(rangeY.min);
  rect.setTop(rangeY.max);
  this->setZoomRectangle(rect, emit_signal);
}

void PlotWidget::setModeXY(bool enable)
{
  if (enable == isXYPlot())
  {
    return;
  }
  PlotWidgetBase::setModeXY(enable);

  enableTracker(!enable);

  if (enable)
  {
    QFont font_footer;
    font_footer.setPointSize(10);
    QwtText text("XY Plot");
    text.setFont(font_footer);
    qwtPlot()->setFooter(text);
  }
  else
  {
    qwtPlot()->setFooter("");
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

void PlotWidget::on_savePlotToFile()
{
  const auto plot_size = plotSize();
  PlotSaveHelper save_plots_helper(plot_size, this);
  plotOn(save_plots_helper, { 0, 0, plot_size.width(), plot_size.height() });
}

void PlotWidget::plotOn(const PlotSaveHelper& plot_save_helper, QRect paint_at)
{
  bool tracker_enabled = _tracker->isEnabled();
  if (tracker_enabled)
  {
    this->enableTracker(false);
    replot();
  }

  plot_save_helper.paint(qwtPlot(), paint_at);

  if (tracker_enabled)
  {
    this->enableTracker(true);
    replot();
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

  auto documentRect = qwtPlot()->canvas()->rect();
  qDebug() << documentRect;

  QwtPlotRenderer rend;
  QPixmap pixmap(documentRect.width(), documentRect.height());
  QPainter painter(&pixmap);
  rend.render(qwtPlot(), &painter, documentRect);

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

  QClipboard* clipboard = QGuiApplication::clipboard();
  clipboard->setText(doc.toString());
}

void PlotWidget::on_pasteAction_triggered()
{
  QClipboard* clipboard = QGuiApplication::clipboard();
  QString clipboard_text = clipboard->text();

  QDomDocument doc;
  bool valid = doc.setContent(clipboard_text);
  if (!valid)
  {
    return;
  }
  auto root = doc.firstChildElement();
  if (root.tagName() != "PlotWidgetClipBoard")
  {
    return;
  }
  else
  {
    auto el = root.firstChildElement();
    xmlLoadState(el);
    emit undoableChange();
  }
}

void PlotWidget::showPointValues(QPoint point)
{
  if (!_show_point_enabled)
  {
    return;
  }
  const QwtPlotItemList curves = qwtPlot()->itemList(QwtPlotItem::Rtti_PlotCurve);

  auto paint_to_plot = [this](QPoint p) {
    return QPointF(qwtPlot()->invTransform(QwtPlot::xBottom, p.x()),
                   qwtPlot()->invTransform(QwtPlot::yLeft, p.y()));
  };
  auto plot_to_paint = [this](QPointF p) {
    return QPoint(qwtPlot()->transform(QwtPlot::xBottom, p.x()),
                  qwtPlot()->transform(QwtPlot::yLeft, p.y()));
  };

  const QPointF pointF = paint_to_plot(point);

  QSettings settings;
  const int prec = settings.value("Preferences::precision", 3).toInt();

  QString text;
  int min_distance_sqr = 40 * 40;
  bool updated = false;
  QPointF marker_point;
  for (int i = 0; i < curves.size(); i++)
  {
    QwtPlotCurve* curve = static_cast<QwtPlotCurve*>(curves[i]);
    auto maybe_point = curvePointAt(curve, pointF.x());
    if (maybe_point)
    {
      QPoint p(qwtPlot()->transform(QwtPlot::xBottom, maybe_point->x()),
               qwtPlot()->transform(QwtPlot::yLeft, maybe_point->y()));
      QPoint diff = p - point;
      int dist_sqr = diff.x() * diff.x() + diff.y() * diff.y();
      if (dist_sqr < min_distance_sqr)
      {
        updated = true;
        min_distance_sqr = dist_sqr;
        _show_point_marker->setValue(maybe_point.value());
        marker_point = maybe_point.value();

        text = QString("<font color=%1>name: %2<br>time:%3<br>value: %4</font>")
                   .arg(curve->pen().color().name())
                   .arg(curve->title().text())
                   .arg(QString::number(maybe_point->x(), 'f', prec))
                   .arg(QString::number(maybe_point->y(), 'f', prec));
      }
    }
  }
  bool disappeared = !_show_point_marker->isVisible() && !updated;
  _show_point_marker->setVisible(updated);
  _show_point_text->setVisible(updated);

  if (updated)
  {
    const QPoint marker_pos_paint = plot_to_paint(marker_point);
    const QPointF offset_point = paint_to_plot(marker_pos_paint + QPoint(15, -20));

    QwtText mark_text;
    mark_text.setText(text);
    mark_text.setBorderPen(QColor(Qt::transparent));
    QColor background_color = qwtPlot()->palette().background().color();
    background_color.setAlpha(220);
    mark_text.setBackgroundBrush(background_color);

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(9);
    mark_text.setFont(font);
    mark_text.setRenderFlags(Qt::AlignLeft);
    _show_point_text->setLabel(mark_text);
    _show_point_text->setLabelAlignment(Qt::AlignRight);
    _show_point_text->setValue(offset_point);

    const double canvas_width = qwtPlot()->canvas()->width();
    const double text_width = mark_text.textSize().width();
    const double text_right_edge = marker_pos_paint.x() + 20 + text_width;

    const bool flip_horizontally =
        (marker_pos_paint.x() > (canvas_width * 0.5)) && text_right_edge > canvas_width;

    if (flip_horizontally)
    {
      _show_point_text->setValue(paint_to_plot(marker_pos_paint + QPoint(-15 - text_width, -20)));
    }
  }

  if (updated || disappeared)
  {
    replot();
  }
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* event)
{
  if (PlotWidgetBase::eventFilter(obj, event))
  {
    return true;
  }

  if (event->type() == QEvent::Destroy)
  {
    return false;
  }

  if (obj == qwtPlot()->canvas())
  {
    return canvasEventFilter(event);
  }
  return false;
}

void PlotWidget::setAxisScale(QwtAxisId axisId, double min, double max)
{
  if (min > max)
  {
    std::swap(min, max);
  }
  if (axisId == QwtPlot::xBottom && _flip_x->isChecked())
  {
    qwtPlot()->setAxisScale(QwtPlot::xBottom, max, min);
  }
  else if (axisId == QwtPlot::yLeft && _flip_y->isChecked())
  {
    qwtPlot()->setAxisScale(QwtPlot::yLeft, max, min);
  }
  else
  {
    qwtPlot()->setAxisScale(axisId, min, max);
  }
}

bool PlotWidget::isZoomLinkEnabled() const
{
  //  for (const auto& it : curveList())
  //  {
  //    auto series = dynamic_cast<QwtSeriesWrapper*>(it.curve->data());
  //    if (series->plotData()->attribute(PJ::DISABLE_LINKED_ZOOM).toBool())
  //    {
  //      return false;
  //    }
  //  }
  return true;
}

bool PlotWidget::canvasEventFilter(QEvent* event)
{
  switch (event->type())
  {
    default:
      break;
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
          QPointF pointF(qwtPlot()->invTransform(QwtPlot::xBottom, press_point.x()),
                         qwtPlot()->invTransform(QwtPlot::yLeft, press_point.y()));
          emit trackerMoved(pointF);
          return true;  // don't pass to canvas().
        }
        return false;  // send to canvas()
      }
      else if (mouse_event->buttons() == Qt::MiddleButton &&
               mouse_event->modifiers() == Qt::NoModifier)
      {
        return false;  // send to canvas()
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
      const QPoint point = mouse_event->pos();
      QPointF pointF(qwtPlot()->invTransform(QwtPlot::xBottom, point.x()),
                     qwtPlot()->invTransform(QwtPlot::yLeft, point.y()));

      if (mouse_event->buttons() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier)
      {
        emit trackerMoved(pointF);
        return true;
      }
      showPointValues(point);
    }
    break;

    case QEvent::Leave: {
      _dragging.mode = DragInfo::NONE;
      _dragging.curves.clear();
    }
    break;
    case QEvent::MouseButtonRelease: {
      if (_dragging.mode == DragInfo::NONE)
      {
        return false;
      }
    }
    break;

  }  // end switch

  return false;
}

void PlotWidget::setDefaultRangeX()
{
  if (!curveList().empty())
  {
    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::lowest();
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
    setAxisScale(QwtPlot::xBottom, min - _time_offset, max - _time_offset);
  }
  else
  {
    setAxisScale(QwtPlot::xBottom, 0.0, 1.0);
  }
}

QwtSeriesWrapper* PlotWidget::createCurveXY(const PlotData* data_x, const PlotData* data_y)
{
  PointSeriesXY* output = nullptr;

  try
  {
    output = new PointSeriesXY(data_x, data_y);
  }
  catch (std::runtime_error& ex)
  {
    if (if_xy_plot_failed_show_dialog)
    {
      QMessageBox msgBox(qwtPlot());
      msgBox.setWindowTitle("Warnings");
      msgBox.setText(tr("The creation of the XY plot failed with the following "
                        "message:\n %1")
                         .arg(ex.what()));
      msgBox.addButton("Continue", QMessageBox::AcceptRole);
      msgBox.exec();
    }
    throw std::runtime_error("Creation of XY plot failed");
  }

  output->setTimeOffset(_time_offset);
  return output;
}

QwtSeriesWrapper* PlotWidget::createTimeSeries(const PlotData* data, const QString& transform_ID)
{
  TransformedTimeseries* output = new TransformedTimeseries(data);
  output->setTransform(transform_ID);
  output->setTimeOffset(_time_offset);
  output->updateCache(true);
  return output;
}
