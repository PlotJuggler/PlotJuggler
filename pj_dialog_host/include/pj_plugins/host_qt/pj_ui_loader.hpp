#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// QUiLoader that instantiates the host-provided widgets (see createWidget in pj_ui_loader.cpp) that plugin .ui
// files declare by class name; plain Qt classes fall through to QUiLoader. Both engines use it, so every dialog
// and panel sees the same widget vocabulary.

#include <QUiLoader>

namespace PJ {

class PjUiLoader : public QUiLoader {
 public:
  explicit PjUiLoader(QObject* parent = nullptr);

  QWidget* createWidget(const QString& class_name, QWidget* parent, const QString& name) override;
};

}  // namespace PJ
