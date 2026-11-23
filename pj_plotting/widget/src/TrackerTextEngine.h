#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_text.h>

#include <vector>

#include "pj_plotting/CurveTracker.h"

namespace PJ {

/// The tracker's value box as a QwtText: rows of one colour each, laid out with font metrics alone, because the box
/// is measured and drawn several times per plot and playback tick (docs/TECHNICAL_NOTES.md, "Tracker value box text").
[[nodiscard]] QwtText valueBoxLabel(const std::vector<ValueBoxRow>& rows);

}  // namespace PJ
