/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "curvelist_panel.h"
#include "ui_curvelist_panel.h"
#include "PlotJuggler/alphanum.hpp"
#include <QDebug>
#include <QLayoutItem>
#include <QMenu>
#include <QSettings>
#include <QDrag>
#include <QMimeData>
#include <QHeaderView>
#include <QFontDatabase>
#include <QMessageBox>
#include <QApplication>
#include <QPainter>
#include <QCompleter>
#include <QStandardItem>
#include <QWheelEvent>
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QTreeWidget>

#include "PlotJuggler/svg_util.h"

namespace
{
bool isCurveTreeItem(const QTreeWidgetItem* item)
{
  return item && !item->data(0, CustomRoles::Name).toString().isEmpty() &&
         item->flags().testFlag(Qt::ItemIsSelectable);
}
}  // namespace

//-------------------------------------------------

CurveListPanel::CurveListPanel(PlotDataMapRef& mapped_plot_data,
                               const TransformsMap& mapped_math_plots, QWidget* parent)
  : QWidget(parent)
  , ui(new Ui::CurveListPanel)
  , _plot_data(mapped_plot_data)
  , _custom_view(new CurveTreeView(this))
  , _tree_view(new CurveTreeView(this))
  , _transforms_map(mapped_math_plots)
  , _column_width_dirty(true)
{
  ui->setupUi(this);

  setFocusPolicy(Qt::ClickFocus);

  _tree_view->setObjectName("curveTreeView");
  _custom_view->setObjectName("curveCustomView");

  auto layout1 = new QHBoxLayout();
  ui->listPlaceholder1->setLayout(layout1);
  layout1->addWidget(_tree_view, 1);
  layout1->setMargin(0);

  auto layout2 = new QHBoxLayout();
  ui->listPlaceholder2->setLayout(layout2);
  layout2->addWidget(_custom_view, 1);
  layout2->setMargin(0);

  QSettings settings;

  int point_size = settings.value("FilterableListWidget/table_point_size", 9).toInt();
  changeFontSize(point_size);

  ui->splitter->setStretchFactor(0, 5);
  ui->splitter->setStretchFactor(1, 1);

  connect(_custom_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &CurveListPanel::onCustomSelectionChanged);

  connect(_custom_view->verticalScrollBar(), &QScrollBar::valueChanged, this,
          &CurveListPanel::refreshValues);

  connect(_tree_view->verticalScrollBar(), &QScrollBar::valueChanged, this,
          &CurveListPanel::refreshValues);

  connect(_tree_view, &QTreeWidget::itemExpanded, this, &CurveListPanel::refreshValues);
}

CurveListPanel::~CurveListPanel()
{
  delete ui;
}

void CurveListPanel::clear()
{
  _custom_view->clear();
  _tree_view->clear();
  _tree_view_items.clear();
  ui->labelNumberDisplayed->setText("0 of 0");
}

bool CurveListPanel::addCurve(const std::string& plot_name)
{
  if (_transforms_map.count(plot_name) > 0)
  {
    return false;
  }

  QString plot_id = QString::fromStdString(plot_name);
  if (_tree_view_items.count(plot_name) > 0)
  {
    return false;
  }

  QString group_name;

  auto FindInPlotData = [&](auto& plot_data, const std::string& plot_name) {
    auto it = plot_data.find(plot_name);
    if (it != plot_data.end())
    {
      auto& plot = it->second;
      if (plot.group())
      {
        group_name = QString::fromStdString(plot.group()->name());
      }
      return true;
    }
    return false;
  };

  bool found = FindInPlotData(_plot_data.numeric, plot_name) ||
               FindInPlotData(_plot_data.scatter_xy, plot_name) ||
               FindInPlotData(_plot_data.strings, plot_name);

  if (!found)
  {
    return false;
  }

  _tree_view->addItem(group_name, getTreeName(plot_id), plot_id);
  _tree_view_items.insert(plot_name);

  _column_width_dirty = true;
  return true;
}

void CurveListPanel::addCustom(const QString& item_name)
{
  // Remove a same-name placeholder from the main list if one exists.
  std::string name_str = item_name.toStdString();
  if (_tree_view_items.count(name_str) > 0)
  {
    _tree_view->removeCurve(item_name);
    _tree_view_items.erase(name_str);
  }
  _custom_view->addItem({}, item_name, item_name);
  _column_width_dirty = true;
}

void CurveListPanel::updateAppearance()
{
  for (CurveTreeView* view : { _tree_view, _custom_view })
  {
    QColor default_color = view->palette().color(QPalette::Text);
    //------------------------------------------
    // Propagate change in color and style to the children of a group
    std::function<void(QTreeWidgetItem*, QColor, bool)> ChangeColorAndStyle;
    ChangeColorAndStyle = [&](QTreeWidgetItem* cell, QColor color, bool italic) {
      cell->setForeground(0, color);
      auto font = cell->font(0);
      font.setItalic(italic);
      cell->setFont(0, font);
      for (int c = 0; c < cell->childCount(); c++)
      {
        ChangeColorAndStyle(cell->child(c), color, italic);
      };
    };

    // set everything to default first
    for (int c = 0; c < view->invisibleRootItem()->childCount(); c++)
    {
      ChangeColorAndStyle(view->invisibleRootItem()->child(c), default_color, false);
    }
    //------------- Change groups first ---------------------

    auto ChangeGroupVisitor = [&](QTreeWidgetItem* cell) {
      if (cell->data(0, CustomRoles::IsGroupName).toBool())
      {
        auto group_name = cell->data(0, CustomRoles::Name).toString();
        auto it = _plot_data.groups.find(group_name.toStdString());
        if (it != _plot_data.groups.end())
        {
          QVariant color_var = it->second->attribute(PJ::TEXT_COLOR);
          QColor text_color = color_var.isValid() ? color_var.value<QColor>() : default_color;

          QVariant style_var = it->second->attribute(PJ::ITALIC_FONTS);
          bool italic = (style_var.isValid() && style_var.value<bool>());

          ChangeColorAndStyle(cell, text_color, italic);

          // tooltip doesn't propagate
          QVariant tooltip = it->second->attribute(TOOL_TIP);
          cell->setData(0, CustomRoles::ToolTip, tooltip);
        }
      }
    };

    view->treeVisitor(ChangeGroupVisitor);

    //------------- Change leaves ---------------------

    auto ChangeLeavesVisitor = [&](QTreeWidgetItem* cell) {
      if (isCurveTreeItem(cell))
      {
        const std::string& curve_name = cell->data(0, CustomRoles::Name).toString().toStdString();

        QVariant text_color;

        auto GetTextColor = [&](auto& plot_data, const std::string& curve_name) {
          auto it = plot_data.find(curve_name);
          if (it != plot_data.end())
          {
            auto& series = it->second;
            QVariant color_var = series.attribute(PJ::TEXT_COLOR);
            if (color_var.isValid())
            {
              cell->setForeground(0, color_var.value<QColor>());
            }

            QVariant tooltip_var = series.attribute(PJ::TOOL_TIP);
            cell->setData(0, CustomRoles::ToolTip, tooltip_var);

            QVariant style_var = series.attribute(PJ::ITALIC_FONTS);
            bool italic = (style_var.isValid() && style_var.value<bool>());
            if (italic)
            {
              QFont font = cell->font(0);
              font.setItalic(italic);
              cell->setFont(0, font);
            }
            if (series.isTimeseries() == false)
            {
              cell->setIcon(0, LoadSvg("://resources/svg/xy.svg", _style_dir));
            }
            return true;
          }
          return false;
        };

        bool valid = (GetTextColor(_plot_data.numeric, curve_name) ||
                      GetTextColor(_plot_data.scatter_xy, curve_name) ||
                      GetTextColor(_plot_data.strings, curve_name));
      }
    };

    view->treeVisitor(ChangeLeavesVisitor);
  }
}

void CurveListPanel::refreshColumns()
{
  _tree_view->refreshColumns();
  _custom_view->refreshColumns();
  _column_width_dirty = false;

  updateFilter();
  updateAppearance();
}

void CurveListPanel::updateFilter()
{
  on_lineEditFilter_textChanged(ui->lineEditFilter->text());
  on_lineEditCustomFilter_textChanged(ui->lineEditCustomFilter->text());
}

void CurveListPanel::keyPressEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_Delete)
  {
    removeSelectedCurves();
  }
}

void CurveListPanel::changeFontSize(int point_size)
{
  _tree_view->setFontSize(point_size);
  _custom_view->setFontSize(point_size);

  QSettings settings;
  settings.setValue("FilterableListWidget/table_point_size", point_size);
}

bool CurveListPanel::is2ndColumnHidden() const
{
  // return ui->checkBoxHideSecondColumn->isChecked();
  return false;
}

void CurveListPanel::update2ndColumnValues(double tracker_time)
{
  _tracker_time = tracker_time;
  refreshValues();
}

void CurveListPanel::refreshValues()
{
  auto default_foreground = _custom_view->palette().foreground();

  auto FormattedNumber = [](double value) {
    QSettings settings;
    int prec = settings.value("Preferences::precision", 3).toInt();
    QString num_text = QString::number(value, 'f', prec);
    if (num_text.contains('.'))
    {
      int idx = num_text.length() - 1;
      while (num_text[idx] == '0')
      {
        num_text[idx] = ' ';
        idx--;
      }
      if (num_text[idx] == '.')
      {
        num_text[idx] = ' ';
      }
    }
    return num_text + " ";
  };

  auto GetValue = [&](const std::string& name) -> QString {
    {
      auto it = _plot_data.numeric.find(name);
      if (it != _plot_data.numeric.end())
      {
        auto& plot_data = it->second;
        auto val = plot_data.getYfromX(_tracker_time);
        if (val)
        {
          return FormattedNumber(val.value());
        }
      }
    }

    {
      auto it = _plot_data.strings.find(name);
      if (it != _plot_data.strings.end())
      {
        auto& plot_data = it->second;
        auto str = plot_data.getStringFromX(_tracker_time);
        if (str)
        {
          char last_byte = str->data()[str->size() - 1];
          if (last_byte == '\0')
          {
            return QString::fromLocal8Bit(str->data(), str->size() - 1);
          }
          else
          {
            return QString::fromLocal8Bit(str->data(), str->size());
          }
        }
      }
    }
    return "-";
  };

  for (CurveTreeView* tree_view : { _tree_view, _custom_view })
  {
    const int vertical_height = tree_view->visibleRegion().boundingRect().height();

    auto DisplayValue = [&](QTreeWidgetItem* cell) {
      if (!isCurveTreeItem(cell))
      {
        return;
      }

      QString curve_name = cell->data(0, CustomRoles::Name).toString();

      if (!curve_name.isEmpty())
      {
        auto rect = cell->treeWidget()->visualItemRect(cell);

        if (rect.bottom() < 0 || cell->isHidden() || rect.top() > vertical_height)
        {
          return;
        }

        if (!is2ndColumnHidden())
        {
          QString str_value = GetValue(curve_name.toStdString());
          cell->setText(1, str_value);
        }
      }
    };

    tree_view->setViewResizeEnabled(false);
    tree_view->treeVisitor(DisplayValue);
    // tree_view->setViewResizeEnabled(true);
  }
}

QString StringifyArray(QString str)
{
  static const QRegExp rx("(\\[\\d+\\])");
  int pos = 0;
  std::vector<std::pair<int, int>> index_positions;

  while ((pos = rx.indexIn(str, pos)) != -1)
  {
    QString array_index = rx.cap(1);

    std::pair<int, int> index = { pos + 1, array_index.size() - 2 };
    index_positions.push_back(index);
    pos += rx.matchedLength();
  }
  if (index_positions.empty())
  {
    return str;
  }

  QStringList out_list;
  out_list.push_back(str);

  for (int i = index_positions.size() - 1; i >= 0; i--)
  {
    std::pair<int, int> index = index_positions[i];
    str.remove(index.first, index.second);
    out_list.push_front(str);
  }

  return out_list.join("/");
}

QString CurveListPanel::getTreeName(QString name)
{
  auto parts = name.split('/', PJ::SkipEmptyParts);

  QString out;
  for (int i = 0; i < parts.size(); i++)
  {
    out += StringifyArray(parts[i]);
    if (i + 1 < parts.size())
    {
      out += "/";
    }
  }
  return out;
}

void CurveListPanel::on_lineEditFilter_textChanged(const QString& search_string)
{
  bool updated = _tree_view->applyVisibilityFilter(search_string);

  const auto& [hidden_count, item_count] = _tree_view->hiddenItemsCount();
  const int visible_count = item_count - hidden_count;

  ui->labelNumberDisplayed->setText(QString::number(visible_count) + QString(" of ") +
                                    QString::number(item_count));
  if (updated)
  {
    emit hiddenItemsChanged();
  }
}

void CurveListPanel::on_lineEditCustomFilter_textChanged(const QString& search_string)
{
  if (_custom_view->applyVisibilityFilter(search_string))
  {
    emit hiddenItemsChanged();
  }
}

void CurveListPanel::removeSelectedCurves()
{
  QMessageBox::StandardButton reply;
  reply = QMessageBox::question(nullptr, tr("Warning"),
                                tr("Do you really want to remove these data?\n"),
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

  if (reply == QMessageBox::Yes)
  {
    emit deleteCurves(_tree_view->getSelectedNames());
    emit deleteCurves(_custom_view->getSelectedNames());
  }

  updateFilter();
}

void CurveListPanel::removeCurve(const std::string& name)
{
  QString curve_name = QString::fromStdString(name);
  _tree_view->removeCurve(curve_name);
  _tree_view_items.erase(name);
  _custom_view->removeCurve(curve_name);
}

void CurveListPanel::on_buttonAddCustom_clicked()
{
  std::array<CurvesView*, 2> views = { _tree_view, _custom_view };

  std::string suggested_name;
  for (CurvesView* view : views)
  {
    auto curve_names = view->getSelectedNames();
    if (curve_names.size() > 0)
    {
      suggested_name = (curve_names.front());
      break;
    }
  }

  emit createMathPlot(suggested_name);
  updateFilter();
}

void CurveListPanel::onCustomSelectionChanged(const QItemSelection&, const QItemSelection&)
{
  auto selected = _custom_view->getSelectedNames();

  bool enabled = (selected.size() == 1);
  ui->buttonEditCustom->setEnabled(enabled);
  ui->buttonEditCustom->setToolTip(enabled ? "Edit the selected custom timeserie" :
                                             "Select a single custom Timeserie to Edit "
                                             "it");

  enabled = (selected.size() > 0);
  ui->buttonDeleteCustom->setEnabled(enabled);
  ui->buttonDeleteCustom->setToolTip(enabled ? "Delete the selected custom timeseries" :
                                               "Select one or more custom timeseries to"
                                               " delete them");
}

void CurveListPanel::on_buttonEditCustom_clicked()
{
  auto selected = _custom_view->getSelectedNames();
  if (selected.size() == 1)
  {
    editMathPlot(selected.front());
  }
}

void CurveListPanel::on_buttonDeleteCustom_clicked()
{
  auto selected = _custom_view->getSelectedNames();
  if (selected.size() >= 1)
  {
    for (const auto& curve_name : selected)
    {
      removeCurve(curve_name);
    }
  }
}

std::vector<std::string> CurveListPanel::getSelectedNames() const
{
  auto selected = _tree_view->getSelectedNames();
  auto custom_select = _custom_view->getSelectedNames();
  selected.insert(selected.end(), custom_select.begin(), custom_select.end());
  return selected;
}

void CurveListPanel::clearSelections()
{
  _custom_view->clearSelection();
  _tree_view->clearSelection();
}

void CurveListPanel::on_stylesheetChanged(QString theme)
{
  _style_dir = theme;
  ui->buttonAddCustom->setIcon(LoadSvg(":/resources/svg/add_tab.svg", theme));
  ui->buttonEditCustom->setIcon(LoadSvg(":/resources/svg/pencil-edit.svg", theme));
  ui->buttonDeleteCustom->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));
  ui->pushButtonTrash->setIcon(LoadSvg(":/resources/svg/trash.svg", theme));

  auto ChangeIconVisitor = [&](QTreeWidgetItem* cell) {
    if (!isCurveTreeItem(cell))
    {
      return;
    }

    const auto& curve_name = cell->data(0, CustomRoles::Name).toString().toStdString();

    auto it = _plot_data.scatter_xy.find(curve_name);
    if (it != _plot_data.scatter_xy.end())
    {
      auto& series = it->second;
      if (series.isTimeseries() == false)
      {
        cell->setIcon(0, LoadSvg("://resources/svg/xy.svg", _style_dir));
      }
    }
  };

  _tree_view->treeVisitor(ChangeIconVisitor);
}

void CurveListPanel::on_checkBoxShowValues_toggled(bool show)
{
  _tree_view->hideValuesColumn(!show);
  _custom_view->hideValuesColumn(!show);
  emit hiddenItemsChanged();
}

void CurveListPanel::on_pushButtonTrash_clicked(bool)
{
  QMessageBox msgBox(this);
  msgBox.setWindowTitle("Warning. Can't be undone.");
  msgBox.setText(tr("Delete data:\n\n"
                    "[Delete All]: remove timeseries and plots.\n"
                    "[Delete Points]: reset data points, but keep plots and "
                    "timeseries.\n"));

  QSettings settings;
  QString theme = settings.value("StyleSheet::theme", "light").toString();

  QPushButton* buttonAll = msgBox.addButton(tr("Delete All"), QMessageBox::DestructiveRole);
  buttonAll->setIcon(LoadSvg(":/resources/svg/clear.svg"));

  QPushButton* buttonPoints = msgBox.addButton(tr("Delete Points"), QMessageBox::DestructiveRole);
  buttonPoints->setIcon(LoadSvg(":/resources/svg/point_chart.svg"));

  msgBox.addButton(QMessageBox::Cancel);
  msgBox.setDefaultButton(QMessageBox::Cancel);

  msgBox.exec();

  if (msgBox.clickedButton() == buttonAll)
  {
    emit requestDeleteAll(1);
  }
  else if (msgBox.clickedButton() == buttonPoints)
  {
    emit requestDeleteAll(2);
  }
}
