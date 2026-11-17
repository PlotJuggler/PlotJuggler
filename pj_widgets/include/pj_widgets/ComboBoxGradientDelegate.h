#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QStyledItemDelegate>

class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace PJ {

// Item delegate that paints a horizontal light_purple to light_blue gradient on selected and hovered rows. Workaround
// for Qt QSS: QComboBox popups paint items through QStyle::drawPrimitive(PE_PanelItemViewItem), which uses
// QPalette::Highlight (a solid colour) and ignores a `background: qlineargradient(...)` rule on `::item:selected`;
// overriding paint() avoids the QSS path.
//
// Apply with: combo->setItemDelegate(new PJ::ComboBoxGradientDelegate(combo));
class ComboBoxGradientDelegate : public QStyledItemDelegate {
  Q_OBJECT
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

}  // namespace PJ
