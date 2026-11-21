#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>
#include <map>

#include "pj_widgets/AppSettings.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

constexpr const char* kThemeSettingsKey = "StyleSheet::theme";

inline QString currentTheme() {
  return appSetting(QString::fromLatin1(kThemeSettingsKey), QStringLiteral("light")).toString();
}

// Asset-encoding contract for monochrome SVGs (the "find" side of recolorSvgInk). These are not theme values: they are
// the literal colors the .svg source files are painted with on disk. recolorSvgInk() scans the raw bytes for them and
// folds them onto the active framework ink (theme::iconInk, the "replace" side). An icon that should follow the theme
// must be authored with one of these inks. (Authoring with `fill="currentColor"` and setting the colour at render time
// would remove the string replacement.)
namespace svg_ink {
// Canonical PJ4 icon ink: every theme-agnostic app icon is drawn with it. Authored uppercase; the lowercase form is
// covered too, since the swap is case-sensitive.
inline constexpr const char* kChrome = "#3D3D3D";
inline constexpr const char* kChromeLower = "#3d3d3d";
// Legacy monochrome inks (hand-edited snippets, the app logo): pure black is dark ink, pure white the opposite (light)
// ink.
inline constexpr const char* kLegacyDark = "#000000";
inline constexpr const char* kLegacyLight = "#ffffff";
}  // namespace svg_ink

// Recolours every visible stroke and fill in an SVG document to the active theme's framework ink, recognising the
// svg_ink authoring contract above. Three shapes:
//   1. Palette swap: an SVG already carrying the opposite theme's ink is flipped to the active one.
//   2. Authoring inks (svg_ink::*) fold onto the active ink: the PJ4 chrome ink maps to `ink`, legacy black to `ink`,
//      white to `opposite`. #E0E0E0 (the light-on-dark ink) is deliberately not folded: it lives only in static
//      *_dark.svg variants used raw, and folding it would invert them.
//   3. Material Symbols SVGs without any explicit fill get a `fill="..."` on the root `<svg>`, so paths inherit the
//      theme ink.
inline void recolorSvgInk(QByteArray& svg_data, bool light_theme) {
  const QByteArray ink = theme::iconInk(theme::themeFor(light_theme)).name(QColor::HexRgb).toUtf8();
  const QByteArray opposite = theme::iconInk(theme::themeFor(!light_theme)).name(QColor::HexRgb).toUtf8();

  // All source-to-target mappings go through unique placeholders and apply in one logical pass: sequential in-place
  // replaces cascade whenever a target ink equals a later rule's source (dark theme black to white, then white to
  // dark-ink, collapsed black-and-white artwork onto one color). Authoring inks are matched before the palette swap, so
  // a legacy #ffffff is the legacy light ink, not "the opposite theme's ink", when they coincide.
  constexpr const char* kInkPh = "\x01PJ_INK\x01";
  constexpr const char* kOppPh = "\x01PJ_OPP\x01";
  svg_data.replace(svg_ink::kLegacyDark, kInkPh);
  svg_data.replace(svg_ink::kLegacyLight, kOppPh);
  svg_data.replace(svg_ink::kChrome, kInkPh).replace(svg_ink::kChromeLower, kInkPh);
  // Palette swap: an SVG already carrying the opposite theme's ink flips to the active one (no-op if that byte pattern
  // was consumed as a legacy ink).
  svg_data.replace(opposite, kInkPh);
  svg_data.replace(kInkPh, ink);
  svg_data.replace(kOppPh, opposite);

  // (3) Root-tag fill injection: an SVG with no `fill` on its root element gets one, so children without a per-path
  // fill inherit it.
  const int svg_open = static_cast<int>(svg_data.indexOf("<svg"));
  if (svg_open < 0) {
    return;
  }
  const int tag_end = static_cast<int>(svg_data.indexOf('>', svg_open));
  if (tag_end <= svg_open) {
    return;
  }
  const QByteArray open_tag = svg_data.mid(svg_open, tag_end - svg_open);
  if (open_tag.contains("fill=\"")) {
    return;
  }
  const QByteArray fill_attr = " fill=\"" + ink + "\"";
  svg_data.insert(tag_end, fill_attr);
}

// True when `style_name` denotes the light theme: the one definition of theme polarity, shared by the recolor pipeline
// and by callers that tint their own composites to match the recolored glyphs.
inline bool isLightTheme(const QString& style_name) {
  return style_name.contains(QLatin1String("light"));
}

// A copy of `pixmap` with every visible pixel painted `ink`, keeping its alpha (and its devicePixelRatio): one ink for
// a glyph whose role needs another ink than the theme's icon ink.
inline QPixmap tintedPixmap(const QPixmap& pixmap, const QColor& ink) {
  QPixmap out = pixmap;
  if (!out.isNull()) {
    QPainter painter(&out);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRectF(QPointF(), out.deviceIndependentSize()), ink);
  }
  return out;
}

// Loads an SVG from a resource path, recoloring monochrome content (#000000, #ffffff) for the requested theme. Cached
// per (path, theme). GUI thread only: the cache maps are not locked.
inline const QPixmap& loadSvg(const QString& filename, const QString& style_name = "light") {
  static std::map<QString, QPixmap> light_images;
  static std::map<QString, QPixmap> dark_images;
  const bool light_theme = isLightTheme(style_name);

  auto* stored_images = light_theme ? &light_images : &dark_images;

  auto it = stored_images->find(filename);
  if (it != stored_images->end()) {
    return it->second;
  }

  QFile file(filename);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    qWarning("PJ::LoadSvg: cannot open %s", qPrintable(filename));
    static const QPixmap fallback;
    return fallback;
  }
  QByteArray svg_data = file.readAll();
  file.close();

  recolorSvgInk(svg_data, light_theme);

  QSvgRenderer renderer(svg_data);
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();

  it = stored_images->insert({filename, QPixmap::fromImage(image)}).first;
  return it->second;
}

// Rasterises a monochrome SVG at the requested logical size, honouring the caller's devicePixelRatio so
// QLabel::setPixmap stays crisp on HiDPI. Use it instead of `loadSvg(...).scaled(w, h, ...)` when the rendered size
// differs from loadSvg's 64x64 cache: scaling the cached pixmap blurs, this renders fresh at logical_size * dpr pixels
// and stamps the DPR on the result. Not cached: call sparingly (once per widget construction). renderSvgDataPixmap
// takes the SVG markup itself (an icon embedded in a manifest) and returns a null pixmap when it does not parse. A
// distinct name, not an overload: a string literal converts to QString and QByteArray alike.
inline QPixmap renderSvgDataPixmap(
    QByteArray svg_data, const QString& style_name, const QSize& logical_size, qreal dpr) {
  recolorSvgInk(svg_data, isLightTheme(style_name));
  QSvgRenderer renderer(svg_data);
  if (!renderer.isValid()) {
    return {};
  }
  const QSize physical = logical_size * dpr;
  QImage image(physical, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  QPixmap pm = QPixmap::fromImage(image);
  pm.setDevicePixelRatio(dpr);
  return pm;
}

inline QPixmap renderSvgPixmap(
    const QString& filename, const QString& style_name, const QSize& logical_size, qreal dpr) {
  QFile file(filename);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    qWarning("PJ::RenderSvgPixmap: cannot open %s", qPrintable(filename));
    return {};
  }
  return renderSvgDataPixmap(file.readAll(), style_name, logical_size, dpr);
}

}  // namespace PJ
