#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

class QLabel;
class QString;
class QWidget;

namespace PJ {

/// A hidden, word-wrapped label in the theme's warning colours, for the reason a GPU view cannot render. The caller
/// places and toggles it; the theme is read once, at construction.
[[nodiscard]] QLabel* createRenderingWarningLabel(QWidget* parent, const QString& object_name);

}  // namespace PJ
