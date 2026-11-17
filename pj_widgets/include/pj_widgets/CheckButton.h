#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QAbstractButton>
#include <QColor>

namespace PJ {

// A checkable rounded-rectangle toggle: the label sits inside a 4px-radius rectangle with a hairline border; unchecked
// is a plain button, checked fills with the accent selection colour. A labelled, click-the-whole-control alternative to
// QCheckBox, distinct from ToggleSwitch (a sliding switch with no inline text).
//
// Checkable by default, so a drop-in for QCheckBox: connect toggled(). Self-painted; every colour is a Qt stylesheet
// property, so the app theme drives them from the QSS in light and dark:
//   PJ--CheckButton {
//     qproperty-accentColor:      ${border_checked};            // checked + hover border
//     qproperty-borderColor:      ${border_default};            // unchecked border
//     qproperty-checkedFillColor: ${item_selection_background}; // checked fill
//     qproperty-textColor:        ${default_text};              // label (both states)
//   }
class CheckButton : public QAbstractButton {
  Q_OBJECT
  // Border when checked or hovered (the active accent).
  Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
  // Border when unchecked and at rest.
  Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor)
  // Fill when checked.
  Q_PROPERTY(QColor checkedFillColor READ checkedFillColor WRITE setCheckedFillColor)
  // Label colour (both states).
  Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
 public:
  explicit CheckButton(QWidget* parent = nullptr);
  explicit CheckButton(const QString& text, QWidget* parent = nullptr);
  ~CheckButton() override;

  [[nodiscard]] QColor accentColor() const {
    return accent_color_;
  }
  void setAccentColor(const QColor& color);

  [[nodiscard]] QColor borderColor() const {
    return border_color_;
  }
  void setBorderColor(const QColor& color);

  [[nodiscard]] QColor checkedFillColor() const {
    return checked_fill_color_;
  }
  void setCheckedFillColor(const QColor& color);

  [[nodiscard]] QColor textColor() const {
    return text_color_;
  }
  void setTextColor(const QColor& color);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QColor accent_color_;
  QColor border_color_;
  QColor checked_fill_color_;
  QColor text_color_;
};

}  // namespace PJ
