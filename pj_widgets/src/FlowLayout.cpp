// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/FlowLayout.h"

#include <QStyle>
#include <QWidget>

namespace PJ {

FlowLayout::FlowLayout(QWidget* parent, int margin, int h_spacing, int v_spacing)
    : QLayout(parent), h_space_(h_spacing), v_space_(v_spacing) {
  setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
  while (QLayoutItem* item = takeAt(0)) {
    delete item;
  }
}

void FlowLayout::addItem(QLayoutItem* item) {
  items_.append(item);
}

int FlowLayout::horizontalSpacing() const {
  if (h_space_ >= 0) {
    return h_space_;
  }
  return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const {
  if (v_space_ >= 0) {
    return v_space_;
  }
  return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const {
  return static_cast<int>(items_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const {
  return items_.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index) {
  if (index >= 0 && index < items_.size()) {
    return items_.takeAt(index);
  }
  return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
  return {};
}

bool FlowLayout::hasHeightForWidth() const {
  return true;
}

int FlowLayout::heightForWidth(int width) const {
  return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const {
  return minimumSize();
}

QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem* item : items_) {
    size = size.expandedTo(item->minimumSize());
  }
  const QMargins margins = contentsMargins();
  return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool test_only) const {
  const QMargins margins = contentsMargins();
  const QRect effective = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
  int x = effective.x();
  int y = effective.y();
  int line_height = 0;

  for (QLayoutItem* item : items_) {
    QWidget* widget = item->widget();
    int space_x = horizontalSpacing();
    if (space_x == -1 && widget != nullptr) {
      space_x = widget->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Horizontal);
    }
    int space_y = verticalSpacing();
    if (space_y == -1 && widget != nullptr) {
      space_y = widget->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Vertical);
    }

    int next_x = x + item->sizeHint().width() + space_x;
    // Off-by-one trap: `effective.right()` is the inclusive last pixel (rect.x() + rect.width() - 1), so the layout
    // wraps iff the item's right edge would land past that pixel: `x + width > rect.x() + rect.width()`. The naive
    // `next_x - space_x > effective.right()` wraps unnecessarily when an item ends exactly on the rect's right edge
    // (two 24-px icons with 0 spacing in a 48-px rect), forcing a single column.
    if (x + item->sizeHint().width() > effective.x() + effective.width() && line_height > 0) {
      x = effective.x();
      y += line_height + space_y;
      next_x = x + item->sizeHint().width() + space_x;
      line_height = 0;
    }
    if (!test_only) {
      item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
    }
    x = next_x;
    line_height = qMax(line_height, item->sizeHint().height());
  }
  return y + line_height + margins.bottom() - rect.y();
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
  QObject* parent = this->parent();
  if (parent == nullptr) {
    return -1;
  }
  if (parent->isWidgetType()) {
    auto* w = qobject_cast<QWidget*>(parent);
    return w->style()->pixelMetric(pm, nullptr, w);
  }
  return qobject_cast<QLayout*>(parent)->spacing();
}

}  // namespace PJ
