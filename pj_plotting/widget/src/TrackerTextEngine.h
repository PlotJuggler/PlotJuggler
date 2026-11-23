#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_text.h>

#include <vector>

#include "pj_plotting/CurveTracker.h"

namespace PJ {

/// Separates the cells of a row given to valueBoxLabel(). Every cell but a row's last is a column, right-aligned
/// across the rows that have it: numbers printed with one precision keep their decimal points in one line in any font
/// whose digits share an advance (the app font's do), whatever the width of a minus sign. The last cell follows.
inline constexpr QChar kValueBoxCellSeparator = u'\t';

/// The tracker's value box as a QwtText: rows of one colour each, laid out with font metrics alone, because the box
/// is measured and drawn several times per plot and playback tick (docs/TECHNICAL_NOTES.md, "Tracker value box text").
[[nodiscard]] QwtText valueBoxLabel(const std::vector<ValueBoxRow>& rows);

}  // namespace PJ
