#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QWidget>

class QVBoxLayout;

namespace PJ {

class ConfigPanelHost : public QWidget {
  Q_OBJECT
 public:
  explicit ConfigPanelHost(QWidget* parent = nullptr);

  // Shows `widget` as the sole hosted panel and takes ownership: the host reparents it and deleteLater()s the previous
  // one. Callers must not delete a widget after handing it over. nullptr is clear().
  void setConfigWidget(QWidget* widget);

  // Removes the hosted widget, if any, with deleteLater().
  void clear();

 private:
  QVBoxLayout* layout_ = nullptr;
  QPointer<QWidget> current_widget_;
};

}  // namespace PJ
