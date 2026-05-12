/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLOT_DOCKER_H
#define PLOT_DOCKER_H

#include <QDomElement>
#include <QXmlStreamReader>
#include "PlotJuggler/plotdata.h"
#include "plotwidget.h"
#include "plot_docker_toolbar.h"

class DockWidget : public ads::CDockWidget
{
  Q_OBJECT

public:
  DockWidget(PlotDataMapRef& datamap, QWidget* parent = nullptr);

  ~DockWidget() override;

  PlotWidget* plotWidget();

  DockToolbar* toolBar();

  QString name() const;

public slots:
  DockWidget* splitHorizontal();

  DockWidget* splitVertical();

private:
  PlotWidget* _plot_widget = nullptr;

  DockToolbar* _toolbar;

  PlotDataMapRef& _datamap;

signals:
  void undoableChange();
};

class PlotDocker : public ads::CDockManager
{
  Q_OBJECT

public:
  PlotDocker(QString name, PlotDataMapRef& datamap, QWidget* parent = nullptr);

  ~PlotDocker();

  QString name() const;

  void setName(QString name);

  QDomElement xmlSaveState(QDomDocument& doc) const;

  bool xmlLoadState(QDomElement& tab_element);

  int plotCount() const;

  PlotWidget* plotAt(int index);

  void setHorizontalLink(bool enabled);

  void zoomOut();

  void replot();
  void invalidateAxisAlignment();

public slots:

  void on_stylesheetChanged(QString theme);

  void savePlotsToFile();

private:
  struct AxisAlignmentCache
  {
    bool valid = false;
    int plot_count = 0;
    int left_width = 0;
    int bottom_height = 0;
    double left_extent = 0.0;
    double bottom_extent = 0.0;
  };

  void updateAxisAlignmentCache(bool force_recompute);
  void restoreSplitter(QDomElement elem, DockWidget* widget);

  QRect plotRelativeFootprint(int index, QSize plot_size) const;

  QString _name;

  PlotDataMapRef& _datamap;
  AxisAlignmentCache _axis_alignment_cache;

signals:

  void plotWidgetAdded(PlotWidget*);
  void plotWidgetRemoved();

  void undoableChange();
};

#endif  // PLOT_DOCKER_H
