/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef CURVELIST_VIEW_H
#define CURVELIST_VIEW_H

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableWidget>
#include <vector>
#include <QSet>

#include "PlotJuggler/alphanum.hpp"

class CurveListPanel;

template <typename Item>
class SortedTableItem : public Item
{
public:
  SortedTableItem(const QString& name) : Item(name), str(name.toStdString())
  {
  }

  bool operator<(const SortedTableItem& other) const
  {
    return doj::alphanum_impl(this->str.c_str(), other.str.c_str()) < 0;
  }

protected:
  std::string str;
};

enum CustomRoles
{
  Name = Qt::UserRole,
  IsGroupName = Qt::UserRole + 1,
  ToolTip = Qt::UserRole + 2,
  RawValue = Qt::UserRole + 3
};

class CurvesView
{
public:
  CurvesView(CurveListPanel* parent);

  virtual void clear() = 0;

  virtual void addItem(const QString& prefix, const QString& tree_name, const QString& plot_ID) = 0;

  virtual std::vector<std::string> getSelectedNames() = 0;

  virtual bool applyVisibilityFilter(const QString& filter_string) = 0;

  virtual void refreshFontSize() = 0;

  virtual void refreshColumns() = 0;

  virtual void hideValuesColumn(bool hide) = 0;

  virtual void setViewResizeEnabled(bool enable) = 0;

  bool eventFilterBase(QObject* object, QEvent* event);

  virtual std::pair<int, int> hiddenItemsCount() = 0;

  virtual void removeCurve(const QString& name) = 0;

  void setFontSize(int size)
  {
    _point_size = size;
    refreshFontSize();
  }

protected:
  int _point_size = 9;
  QPoint _drag_start_pos;
  bool _newX_modifier = false;
  bool _dragging = false;
  CurveListPanel* _parent_panel;
};

class CurveTableView : public QTableWidget, public CurvesView
{
public:
  CurveTableView(CurveListPanel* parent);

  void clear() override
  {
    setRowCount(0);
    _inserted_curves.clear();
  }

  void addItem(const QString& prefix, const QString& tree_name, const QString& plot_ID) override;

  void refreshColumns() override;

  std::vector<std::string> getSelectedNames() override;

  void refreshFontSize() override;

  void removeCurve(const QString& name) override;

  bool applyVisibilityFilter(const QString& filter_string) override;

  bool eventFilter(QObject* object, QEvent* event) override
  {
    bool ret = CurvesView::eventFilterBase(object, event);
    if (!ret)
    {
      return QWidget::eventFilter(object, event);
    }
    else
    {
      return true;
    }
  }

  void setViewResizeEnabled(bool enable) override;

  virtual std::pair<int, int> hiddenItemsCount() override
  {
    return { _hidden_count, model()->rowCount() };
  }

  virtual void hideValuesColumn(bool hide) override;

private:
  int _hidden_count = 0;
  QSet<QString> _inserted_curves;
};

#endif  // CURVELIST_VIEW_H
