#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>

namespace PJ {

/// Stand-in shown by a Custom Widget panel whose plugin widget cannot be created (library not
/// installed, widget type not in the library, Qt build mismatch, creation failure). It only states
/// the reason; the panel keeps the saved layout element so the widget returns once the cause is
/// fixed.
class MissingWidgetPlaceholder : public QWidget {
  Q_OBJECT
 public:
  explicit MissingWidgetPlaceholder(const QString& message, QWidget* parent = nullptr);
};

}  // namespace PJ
