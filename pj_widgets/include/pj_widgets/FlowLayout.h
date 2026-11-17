#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QStyle>

namespace PJ {

// QLayout that wraps items horizontally and breaks to a new row when the next item would overflow the available width
// (the Qt FlowLayout example). Used for tool strips that stack vertically when the host column narrows until one item
// fits per row.
class FlowLayout : public QLayout {
 public:
  explicit FlowLayout(QWidget* parent, int margin = -1, int h_spacing = -1, int v_spacing = -1);
  ~FlowLayout() override;

  void addItem(QLayoutItem* item) override;
  [[nodiscard]] int horizontalSpacing() const;
  [[nodiscard]] int verticalSpacing() const;
  [[nodiscard]] Qt::Orientations expandingDirections() const override;
  [[nodiscard]] bool hasHeightForWidth() const override;
  [[nodiscard]] int heightForWidth(int width) const override;
  [[nodiscard]] int count() const override;
  [[nodiscard]] QLayoutItem* itemAt(int index) const override;
  [[nodiscard]] QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  [[nodiscard]] QSize sizeHint() const override;
  QLayoutItem* takeAt(int index) override;

 private:
  int doLayout(const QRect& rect, bool test_only) const;
  [[nodiscard]] int smartSpacing(QStyle::PixelMetric pm) const;

  QList<QLayoutItem*> items_;
  int h_space_;
  int v_space_;
};

}  // namespace PJ
