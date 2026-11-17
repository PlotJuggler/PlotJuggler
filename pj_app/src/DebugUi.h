#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

class QMainWindow;
class QWidget;

namespace PJ {

class Theme;

// Dev-time UI inspector, owned by the host window. Ctrl+Shift+D outlines every visible descendant widget
// (mouse-transparent, layout-neutral); Ctrl+Shift+Q appends a debug QSS layer drawing 1px dashed borders, which
// shifts layout by 1px. The QSS toggle persists in QSettings.
class DebugUi : public QObject {
  Q_OBJECT
 public:
  // Hooks Theme::qssChanged so the debug layer survives theme switches.
  static void installInto(QMainWindow* host, Theme* theme);

  explicit DebugUi(QMainWindow* host, Theme* theme);

 private:
  void toggleOverlay();
  void toggleQssLayer();
  void applyQss();

  QMainWindow* host_;
  Theme* theme_;
  QWidget* overlay_ = nullptr;
  bool qss_enabled_ = false;
};

}  // namespace PJ
