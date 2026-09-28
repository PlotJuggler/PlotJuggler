#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

class QLabel;
class QString;
class QWidget;

namespace PJ {

/// A hidden, word-wrapped label in the application theme's warning colours, for
/// the reason a GPU view cannot render. The caller places it and toggles it. The
/// theme is read once, here.
[[nodiscard]] QLabel* createRenderingWarningLabel(QWidget* parent, const QString& object_name);

}  // namespace PJ
