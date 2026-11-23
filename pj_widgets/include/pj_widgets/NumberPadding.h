#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QChar>

namespace PJ {

/// Spaces that keep numbers in columns in the app font (PlotJuggler Sans) without a fixed-pitch font: in both of its
/// faces every digit has one advance, kFigureSpace has the same, and kPunctuationSpace has the period's. Pad a number
/// with kFigureSpace where a digit would be and with kPunctuationSpace where a decimal point would be;
/// `BundledFont.DigitsAndTheirSpacesShareAnAdvance` pins those advances.
inline constexpr QChar kFigureSpace{0x2007};
inline constexpr QChar kPunctuationSpace{0x2008};

}  // namespace PJ
