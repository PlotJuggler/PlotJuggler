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

/// Restyles a label from createRenderingWarningLabel(): `subtle` is a small, muted, single-line note on the HUD veil
/// for a reduced-quality mode the user can keep working in (software rendering); otherwise the word-wrapped banner.
void setRenderingWarningSubtle(QLabel* label, bool subtle);

}  // namespace PJ
