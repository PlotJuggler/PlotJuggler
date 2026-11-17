#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QAbstractButton>
#include <QColor>

namespace PJ {

// A fixed-size colour swatch: a rounded square in the current colour that opens a ColorPickerPopup when clicked. The
// one swatch control: do not hand-roll per-layer swatch buttons.
//
// colorChanged() fires only for user picks in the popup; setColor() is programmatic and silent, so a host can seed or
// restore the swatch without feedback.
class ColorPickerWidget : public QAbstractButton {
  Q_OBJECT
 public:
  explicit ColorPickerWidget(QWidget* parent = nullptr);
  ~ColorPickerWidget() override;

  [[nodiscard]] QColor color() const {
    return color_;
  }
  // Updates the swatch; does not emit colorChanged().
  void setColor(const QColor& color);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

 signals:
  // Emitted only when the user picks a colour in the popup.
  void colorChanged(QColor color);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  void openPopup();

  QColor color_{Qt::white};
};

}  // namespace PJ
