#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <algorithm>
#include <cmath>

namespace PJ {

/// WCAG 2.x contrast ratio of two sRGB colours, 1 (identical) to 21 (black on white). Alpha is ignored: pass the
/// effective, composited background.
[[nodiscard]] inline double contrastRatio(const QColor& a, const QColor& b) {
  const auto luminance = [](const QColor& c) {
    const auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
  };
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// `ink`, or the closest lightening or darkening of it that has at least `min_ratio`:1 against `background`: the ink
/// is mixed toward white on a dark background and toward black on a light one, in 1% steps, so its hue is kept and a
/// colour that already reads is returned untouched. Falls back to the pure extreme when no mix reaches the ratio.
[[nodiscard]] inline QColor inkWithContrast(const QColor& ink, const QColor& background, double min_ratio) {
  if (contrastRatio(ink, background) >= min_ratio) {
    return ink;
  }
  const QColor extreme = contrastRatio(Qt::white, background) >= contrastRatio(Qt::black, background)
                             ? QColor(Qt::white)
                             : QColor(Qt::black);
  const auto mix = [](int from, int to, double t) { return static_cast<int>(std::lround(from + (to - from) * t)); };
  QColor mixed = extreme;
  for (int percent = 1; percent <= 100; ++percent) {
    const double t = percent / 100.0;
    mixed = QColor(
        mix(ink.red(), extreme.red(), t), mix(ink.green(), extreme.green(), t), mix(ink.blue(), extreme.blue(), t));
    if (contrastRatio(mixed, background) >= min_ratio) {
      break;
    }
  }
  return mixed;
}

}  // namespace PJ
