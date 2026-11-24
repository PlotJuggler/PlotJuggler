#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QWidget>
#include <pj_plugins/host_qt/widget_binding.hpp>

namespace PJ {

// Base of every host delegate on a plugin table: paints an item tagged kLockedRowRole as selected and enabled (while
// the view is enabled), so the QSS `::item:selected` accent applies to it. The item itself stays disabled, which keeps
// clicks, keyboard and selection off it. A cell without an item (a radio column, a ragged row) takes the tag of its
// row's first item, or the locked row would paint with a hole.
class LockedRowDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

 protected:
  void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
    QStyledItemDelegate::initStyleOption(option, index);
    QVariant locked = index.data(kLockedRowRole);
    for (int c = 0; !locked.isValid() && c < index.model()->columnCount(index.parent()); ++c) {
      locked = index.siblingAtColumn(c).data(kLockedRowRole);
    }
    if (!locked.toBool()) {
      return;
    }
    option->state |= QStyle::State_Selected;
    if (option->widget == nullptr || option->widget->isEnabled()) {
      option->state |= QStyle::State_Enabled;
      // The text, a native style and a deletable table's trash icon then draw in the normal colours, whichever group
      // the view picked for the disabled item.
      option->palette.setCurrentColorGroup(
          option->widget != nullptr && !option->widget->isActiveWindow() ? QPalette::Inactive : QPalette::Active);
    }
  }
};

}  // namespace PJ
