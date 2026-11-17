#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

namespace PJ {

// qApp event filter that adjusts widgets at Polish where QSS cannot: every unnamed QMenu (context menus too)
// gets objectName "PJMenu" for the QMenu#PJMenu rules; a QMessageBox loses its native frame; combo popups (the
// view and Qt's private QComboBoxPrivateContainer) get the popup palette directly, because Fusion paints them
// from palette(Window) / palette(Base) whatever the QSS says, and lose their frame and shadow.
class WidgetTuner : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override;
};

}  // namespace PJ
