// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/hud_overlay.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QRectF>
#include <QSize>
#include <algorithm>
#include <cmath>

#include "pj_widgets/FrameworkTokens.h"

namespace pj::scene3d {

namespace {

PJ::theme::Theme appTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return PJ::theme::themeFor(window.lightnessF() > 0.5);
}

}  // namespace

QImage renderHudPanel(
    const QStringList& lines, const QFont& font, qreal device_pixel_ratio, int padding, int panel_alpha,
    const QColor& text_color) {
  (void)panel_alpha;
  // Nothing to draw: no lines, or every line empty. A null image lets the caller
  // skip the blit entirely.
  const bool has_text = std::any_of(lines.cbegin(), lines.cend(), [](const QString& line) { return !line.isEmpty(); });
  if (!has_text) {
    return QImage{};
  }

  const qreal dpr = device_pixel_ratio > 0.0 ? device_pixel_ratio : 1.0;
  const QFontMetrics metrics(font);
  const int line_h = metrics.height();

  // Logical (device-independent) box: widest line advance + symmetric padding.
  int text_w = 0;
  for (const QString& line : lines) {
    text_w = std::max(text_w, metrics.horizontalAdvance(line));
  }
  const int box_w = text_w + 2 * padding;
  const int box_h = line_h * static_cast<int>(lines.size()) + 2 * padding;

  // Allocate at PHYSICAL resolution (logical * dpr) and tag the image with the
  // ratio, so painting below — and the caller's drawImage() — work in logical
  // coordinates and stay crisp at any DPR, which a GPU glyph cache would lose
  // across a QRhi recreation (see header).
  const int phys_w = static_cast<int>(std::lround(box_w * dpr));
  const int phys_h = static_cast<int>(std::lround(box_h * dpr));
  QImage image(QSize(phys_w, phys_h), QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(dpr);
  image.fill(Qt::transparent);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setFont(font);

  // Translucent rounded panel.
  painter.setPen(Qt::NoPen);
  const auto active_theme = appTheme();
  painter.setBrush(PJ::theme::overlay(PJ::theme::Overlay::Hud, active_theme));
  const int radius = PJ::theme::radius(PJ::theme::Radius::Input, active_theme);
  painter.drawRoundedRect(QRectF(0, 0, box_w, box_h), radius, radius);

  // Lines left-aligned, stacked top to bottom on successive baselines.
  painter.setPen(text_color);
  int baseline = padding + metrics.ascent();
  for (const QString& line : lines) {
    painter.drawText(padding, baseline, line);
    baseline += line_h;
  }
  painter.end();
  return image;
}

}  // namespace pj::scene3d
