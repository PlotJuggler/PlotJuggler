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
// every user gesture off it.
class LockedRowDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

 protected:
  void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
    QStyledItemDelegate::initStyleOption(option, index);
    if (!index.data(kLockedRowRole).toBool()) {
      return;
    }
    option->state |= QStyle::State_Selected;
    if (option->widget == nullptr || option->widget->isEnabled()) {
      option->state |= QStyle::State_Enabled;
    }
  }
};

}  // namespace PJ
