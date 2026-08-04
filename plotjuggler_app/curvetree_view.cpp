/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "curvetree_view.h"
#include "curvelist_panel.h"
#include <QTimer>
#include <QFontDatabase>
#include <QObject>
#include <QDebug>
#include <QToolTip>
#include <QKeySequence>
#include <QClipboard>

namespace
{
bool isCurveItem(const QTreeWidgetItem* item)
{
  return item && !item->data(0, CustomRoles::Name).toString().isEmpty() &&
         item->flags().testFlag(Qt::ItemIsSelectable);
}
}  // namespace

class TreeWidgetItem : public QTreeWidgetItem
{
public:
  TreeWidgetItem(QTreeWidgetItem* parent) : QTreeWidgetItem(parent)
  {
  }

  bool operator<(const QTreeWidgetItem& other) const
  {
    return doj::alphanum_impl(this->text(0).toLocal8Bit(), other.text(0).toLocal8Bit()) < 0;
  }
};

CurveTreeView::CurveTreeView(CurveListPanel* parent) : QTreeWidget(parent), CurvesView(parent)
{
  setColumnCount(2);
  setEditTriggers(NoEditTriggers);
  setDragEnabled(false);
  setDefaultDropAction(Qt::IgnoreAction);
  setDragDropOverwriteMode(false);
  setMouseTracking(true);
  setDragDropMode(NoDragDrop);
  viewport()->installEventFilter(this);
  setSelectionMode(ExtendedSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setFocusPolicy(Qt::ClickFocus);

  header()->setVisible(false);
  header()->setStretchLastSection(true);
  header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

  connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
    if (column == 0)
    {
      expandChildren(!item->isExpanded(), item);
    }
  });

  connect(selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
    if (getSelectedNames().empty())
    {
      // this looks nicer
      clearFocus();
      setFocusPolicy(Qt::NoFocus);
    }
    else
    {
      // this focus policy is needed to allow CurveListPanel::keyPressEvent to be called
      setFocusPolicy(Qt::ClickFocus);
    }
  });
  _tooltip_timer = new QTimer(this);
  connect(_tooltip_timer, &QTimer::timeout, this, [this]() {
    if (_tooltip_item)
    {
      auto tooltip = _tooltip_item->data(0, CustomRoles::ToolTip);
      if (tooltip.isValid())
      {
        QToolTip::showText(_tooltip_pos, tooltip.toString(), this, QRect(), 10000);
      }
    }
  });
  _tooltip_timer->start(100);
}

void CurveTreeView::clear()
{
  _tooltip_item = nullptr;
  _tooltip_timer->stop();
  QTreeWidget::clear();
  _leaf_count = 0;
  _hidden_count = 0;
}

void CurveTreeView::addItem(const QString& group_name, const QString& tree_name,
                            const QString& plot_ID)
{
  QSettings settings; /*
                       * This Source Code Form is subject to the terms of the Mozilla
                       * Public License, v. 2.0. If a copy of the MPL was not distributed
                       * with this file, You can obtain one at
                       * https://mozilla.org/MPL/2.0/.
                       */

  bool use_separator = settings.value("Preferences::use_separator", true).toBool();

  QStringList parts;
  if (use_separator)
  {
    parts = tree_name.split('/', PJ::SkipEmptyParts);
  }
  else
  {
    parts.push_back(tree_name);
  }

  if (parts.size() == 0)
  {
    return;
  }

  bool hasGroup = !group_name.isEmpty();
  auto group_parts = group_name.split('/', PJ::SkipEmptyParts);

  // Check if tree_name already starts with the group prefix by comparing
  // the split parts (avoids leading-slash mismatch between the two strings).
  bool prefix_is_group = false;
  if (hasGroup && group_parts.size() <= parts.size())
  {
    prefix_is_group = true;
    for (int i = 0; i < group_parts.size(); i++)
    {
      if (parts[i] != group_parts[i])
      {
        prefix_is_group = false;
        break;
      }
    }
  }

  if (hasGroup && !prefix_is_group)
  {
    parts = group_parts + parts;
  }

  QTreeWidgetItem* tree_parent = this->invisibleRootItem();

  for (int i = 0; i < parts.size(); i++)
  {
    bool is_leaf = (i == parts.size() - 1);
    const auto& part = parts[i];

    QTreeWidgetItem* matching_child = nullptr;

    for (int c = 0; c < tree_parent->childCount(); c++)
    {
      QTreeWidgetItem* tree_child = tree_parent->child(c);
      if (tree_child->text(0) == part)
      {
        matching_child = tree_child;
        break;
      }
    }

    if (matching_child)
    {
      if (is_leaf)
      {
        matching_child->setText(1, "-");
        matching_child->setFlags(matching_child->flags() | Qt::ItemIsSelectable);
        matching_child->setData(0, Name, plot_ID);
      }
      tree_parent = matching_child;
    }
    else
    {
      QTreeWidgetItem* child_item = new TreeWidgetItem(tree_parent);
      child_item->setText(0, part);
      child_item->setText(1, is_leaf ? "-" : "");

      bool isGroupCell = (i < group_parts.size());

      QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
      font.setPointSize(_point_size);
      // font.setBold(isGroupCell);
      child_item->setFont(0, font);

      font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
      font.setPointSize(_point_size - 2);
      child_item->setFont(1, font);
      child_item->setTextAlignment(1, Qt::AlignRight);

      tree_parent = child_item;

      auto current_flag = child_item->flags();

      if (isGroupCell)
      {
        child_item->setData(0, Name, group_name);
        child_item->setData(0, IsGroupName, (i + 1) == group_parts.size());
      }

      if (is_leaf)
      {
        child_item->setFlags(current_flag | Qt::ItemIsSelectable);
        child_item->setData(0, Name, plot_ID);
      }
      else
      {
        child_item->setFlags(current_flag & (~Qt::ItemIsSelectable));
      }
    }
  }
  _leaf_count++;
}

void CurveTreeView::refreshColumns()
{
  invisibleRootItem()->sortChildren(0, Qt::AscendingOrder);
  treeVisitor([&](QTreeWidgetItem* item) { item->sortChildren(0, Qt::AscendingOrder); });
  header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}

std::vector<std::string> CurveTreeView::getSelectedNames()
{
  std::vector<std::string> non_hidden_list;

  for (const auto& item : selectedItems())
  {
    if (isCurveItem(item))
    {
      non_hidden_list.push_back(item->data(0, Qt::UserRole).toString().toStdString());
    }
  }
  return non_hidden_list;
}

void CurveTreeView::refreshFontSize()
{
  header()->setSectionResizeMode(0, QHeaderView::Fixed);
  header()->setSectionResizeMode(1, QHeaderView::Fixed);

  treeVisitor([this](QTreeWidgetItem* item) {
    auto font = item->font(0);
    font.setPointSize(_point_size);
    item->setFont(0, font);
    font = item->font(1);
    font.setPointSize(_point_size - 2);
    item->setFont(1, font);
  });

  header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  header()->setSectionResizeMode(1, QHeaderView::Stretch);
}

bool CurveTreeView::applyVisibilityFilter(const QString& search_string)
{
  bool updated = false;
  _hidden_count = 0;

  QStringList spaced_items = search_string.split(' ', PJ::SkipEmptyParts);

  auto hideFunc = [&](QTreeWidgetItem* item) {
    if (!isCurveItem(item))
    {
      return;
    }

    QString name = item->data(0, Qt::UserRole).toString();
    bool toHide = false;

    if (search_string.isEmpty() == false)
    {
      for (const auto& spaced_item : spaced_items)
      {
        if (name.contains(spaced_item, Qt::CaseInsensitive) == false)
        {
          toHide = true;
          break;
        }
      }
    }
    if (toHide)
    {
      _hidden_count++;
    }

    if (toHide != item->isHidden())
    {
      updated = true;
    }

    item->setHidden(toHide);

    // hide the parent if necessary
    auto parent = item->parent();
    while (parent)
    {
      bool all_children_hidden = true;
      for (int c = 0; c < parent->childCount(); c++)
      {
        if (!parent->child(c)->isHidden())
        {
          all_children_hidden = false;
          break;
        }
      }
      auto parent_hidden = parent->isHidden();
      if (all_children_hidden != parent_hidden)
      {
        parent->setHidden(all_children_hidden);
        parent = parent->parent();
      }
      else
      {
        break;
      }
    }
  };

  treeVisitor(hideFunc);
  //-------------

  return updated;
}

bool CurveTreeView::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::MouseMove)
  {
    auto mouse_event = static_cast<QMouseEvent*>(event);
    auto* item = itemAt(mouse_event->pos());
    if (item)
    {
      _tooltip_pos = mapToGlobal(mouse_event->pos());
    }
    _tooltip_item = item;
  }

  if (event->type() == QEvent::Leave)
  {
    _tooltip_item = nullptr;
  }

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

void CurveTreeView::removeCurve(const QString& to_be_deleted)
{
  auto removeFunc = [&](QTreeWidgetItem* item) {
    QString curve_name = item->data(0, Qt::UserRole).toString();
    if (curve_name == to_be_deleted)
    {
      _leaf_count--;

      if (item->childCount() > 0)
      {
        item->setFlags(item->flags() & (~Qt::ItemIsSelectable));
        item->setData(0, Qt::UserRole, {});
        item->setText(1, "");
        return;
      }

      auto parent_item = item->parent();
      if (!parent_item)
      {
        parent_item = invisibleRootItem();
      }
      parent_item->removeChild(item);

      while (parent_item->childCount() == 0 && parent_item != invisibleRootItem())
      {
        auto prev_item = parent_item;
        parent_item = parent_item->parent();
        if (!parent_item)
        {
          parent_item = invisibleRootItem();
        }
        parent_item->removeChild(prev_item);
      }
    }
  };

  // just in case
  _tooltip_item = nullptr;
  treeVisitor(removeFunc);
}

void CurveTreeView::hideValuesColumn(bool hide)
{
  setViewResizeEnabled(true);
  setColumnHidden(1, hide);
}

void CurveTreeView::treeVisitor(std::function<void(QTreeWidgetItem*)> visitor)
{
  std::function<void(QTreeWidgetItem*)> recursiveFunction;
  recursiveFunction = [&](QTreeWidgetItem* item) {
    visitor(item);
    for (int c = 0; c < item->childCount(); c++)
    {
      recursiveFunction(item->child(c));
    }
  };

  for (int c = 0; c < invisibleRootItem()->childCount(); c++)
  {
    recursiveFunction(invisibleRootItem()->child(c));
  }
}

void CurveTreeView::keyPressEvent(QKeyEvent* event)
{
  //Standary copy, just path
  if (event->matches(QKeySequence::Copy))
  {
    auto selected = selectedItems();
    if (selected.size() > 0)
    {
      QClipboard* clipboard = QApplication::clipboard();
      clipboard->setText(selected.front()->data(0, Name).toString());
    }
  }

  //Alternate copy - Copy each name and value of selected to clipoard. Useful for debugging, or copying a lat/lon, quaternion, or some other vector for example.
  const auto key = event->key();
  if(event->modifiers().testFlag(Qt::AltModifier) && key == Qt::Key_C )
  {
    auto selected = selectedItems();
    if (selected.size() > 0)
    {
      QStringList name_value_list;
      for (auto* item : selected)
      {
        QString name = item->data(0, Name).toString();
        QVariant raw_value = item->data(1, RawValue);

        Q_ASSERT_X(raw_value.isValid(), __FUNCTION__, "Empty QVariant in RawValue role. Check CurveListPanel::refreshValues().");

        QString string_raw_value;
        if (raw_value.type() == QVariant::Double)                                                                                                                                               
        {               
          string_raw_value = QString::number(raw_value.toDouble(), 'g', 17);
        }
        else
        {
          string_raw_value = raw_value.toString();
        }
   
        name_value_list.append(name + ": " + string_raw_value);  
      }

      QString clipboardText = name_value_list.join('\n');
      QClipboard* clipboard = QApplication::clipboard();
      clipboard->setText(clipboardText);
    }
  }
}

void CurveTreeView::expandChildren(bool expanded, QTreeWidgetItem* item)
{
  int childCount = item->childCount();
  for (int i = 0; i < childCount; i++)
  {
    const auto child = item->child(i);
    // Recursively call the function for each child node.
    if (child->childCount() > 0)
    {
      child->setExpanded(expanded);
      expandChildren(expanded, child);
    }
  }
}
