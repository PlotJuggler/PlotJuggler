#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

namespace pj_widgets_demos {

// Mirrors pj_app::Theme's QSS preprocessor (palette block and ${token} substitution); pj_widgets demos cannot depend on
// pj_app, so this is the demo-private copy. Loads stylesheet_<theme>.qss from PJ_QSS_DIR, expands ${token} placeholders
// against the palette block, and returns the body ready for setStyleSheet.
QString loadAndExpandQss(const QString& theme);

// Loads and applies it through qApp->setStyleSheet, and writes QSettings("StyleSheet::theme") so pj_widgets controls
// that consult currentTheme() (DoubleScrubber, the SvgUtil icon palette) pick the matching per-theme colours.
void applyTheme(const QString& theme);

}  // namespace pj_widgets_demos
