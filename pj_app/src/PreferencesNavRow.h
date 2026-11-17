#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFrame>
#include <QString>

class QLabel;

namespace PJ {

// A PreferencesDialog nav row; the QSS keys on its dynamic "selected" property.
class PreferencesNavRow : public QFrame {
  Q_OBJECT
 public:
  explicit PreferencesNavRow(const QString& text, QWidget* parent = nullptr);

  void setSelected(bool selected);
  [[nodiscard]] bool isSelected() const {
    return selected_;
  }

 signals:
  void clicked();

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  QLabel* label_;
  bool selected_ = false;
};

}  // namespace PJ
