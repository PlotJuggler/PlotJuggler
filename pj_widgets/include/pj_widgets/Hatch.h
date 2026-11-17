#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// App-wide "no data" diagonal hatch: one shared pattern, so every widget that paints it (the Timeline empty span, the
// RangeSlider unselected track) reads as a window into one continuous hatch layer, with the diagonal lines
// phase-aligned across widgets and not merely equally spaced. The pattern is phased to a shared grid, `X + Y = 0 (mod
// kHatchSpacing)`, in a common coordinate frame (each widget passes its global origin), so two widgets at different
// screen positions draw collinear lines.

#include <QColor>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <cmath>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

// The one hatch line spacing (logical px), shared by every widget so the density cannot drift.
inline constexpr double kHatchSpacing = 14.0;

// Theme ink of the hatch. The app syncs QPalette::Window per theme; its lightness only chooses the framework
// data-backdrop and text tokens.
inline QColor appHatchColor(bool enabled = true) {
  const QPalette pal = QGuiApplication::palette();
  const auto fw_theme = theme::themeFor(pal.color(QPalette::Window).lightness() >= 128);
  return theme::onSurface(
      theme::Surface::DataBackdrop, enabled ? theme::Emphasis::Muted : theme::Emphasis::Disabled, fw_theme);
}

// The shared data backdrop the hatch is composited over, the other half of "one continuous layer": every caller must
// paint it behind the hatch so contrast matches too, not just the ink (appHatchColor).
inline QColor appHatchBackground() {
  const QPalette pal = QGuiApplication::palette();
  const auto fw_theme = theme::themeFor(pal.color(QPalette::Window).lightness() >= 128);
  return theme::surface(theme::Surface::DataBackdrop, fw_theme);
}

// Paints the shared hatch into `painter`, filling `rect` and intersecting any clip the caller set (so a widget can
// restrict it to a region, such as the RangeSlider's unselected stripes, by setting a clip first). `global_origin` is
// the origin of `rect`'s coordinate frame in the shared screen space: `widget->mapToGlobal({0,0})` for a QWidget, the
// item origin mapped to global for a QGraphicsItem. Lines are 45-degree "/", length rect.height(), 1px cosmetic.
inline void drawHatch(QPainter& painter, const QRectF& rect, const QPointF& global_origin, const QColor& color) {
  if (rect.width() <= 0.0 || rect.height() <= 0.0) {
    return;
  }
  const auto posmod = [](double a, double m) {
    const double r = std::fmod(a, m);
    return r < 0.0 ? r + m : r;
  };
  painter.save();
  painter.setClipRect(rect, Qt::IntersectClip);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(color, 1.0));
  const double h = rect.height();
  // A "/" line through local (xs, rect.bottom()) lies on global X+Y = xs + global_origin.x() + rect.bottom() +
  // global_origin.y(); xs is chosen so that is a multiple of kHatchSpacing, then the pattern tiles by kHatchSpacing
  // across rect (clipping the spill).
  const double base_phase = posmod(global_origin.x() + global_origin.y() + rect.bottom(), kHatchSpacing);
  double start_x = rect.left() - h;
  start_x -= posmod(start_x + base_phase, kHatchSpacing);
  const int count = static_cast<int>((rect.width() + 2.0 * h) / kHatchSpacing) + 2;
  for (int i = 0; i < count; ++i) {
    const double xs = start_x + i * kHatchSpacing;
    painter.drawLine(QLineF(xs, rect.bottom(), xs + h, rect.top()));
  }
  painter.restore();
}

// Paints the complete shared "no data" fill into `rect`: the theme backdrop (appHatchBackground) plus the phase-aligned
// hatch (appHatchColor), so every widget composites the same ink over the same backdrop. Prefer it to a bare
// drawHatch() wherever the region is not already filled with QPalette::Window (the RangeSlider's transparent groove,
// where a bare hatch would sit on white and read busier). Respects any clip the caller set.
inline void drawNoDataHatch(QPainter& painter, const QRectF& rect, const QPointF& global_origin, bool enabled = true) {
  painter.fillRect(rect, appHatchBackground());
  drawHatch(painter, rect, global_origin, appHatchColor(enabled));
}

}  // namespace PJ
