// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/FrameworkTokens.h"

#include <QFile>
#include <QGuiApplication>
#include <QLatin1Char>
#include <QLatin1String>
#include <QLoggingCategory>
#include <QPalette>
#include <QStringList>
#include <array>
#include <map>

namespace PJ::theme {

namespace {

Q_LOGGING_CATEGORY(lcTokens, "pj.widgets.framework_tokens")

// Parses a stylesheet's `PALETTE START`..`PALETTE END` block into key to value, by PJ::Theme's loader rules: skip `//`
// and blank lines, split on the first `:`, drop a trailing `;`. The first occurrence of a key wins.
std::map<QString, QString> parsePalette(const QString& qss) {
  std::map<QString, QString> palette;
  const QStringList lines = qss.split(QLatin1Char('\n'));
  int i = 0;
  while (i < lines.size() && !lines[i].contains(QLatin1String("PALETTE START"))) {
    ++i;
  }
  ++i;  // skip START marker (or run off the end -> empty palette)
  for (; i < lines.size() && !lines[i].contains(QLatin1String("PALETTE END")); ++i) {
    const QString trimmed = lines[i].trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("//"))) {
      continue;
    }
    const qsizetype colon = trimmed.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
      continue;
    }
    QString token = trimmed.left(colon).trimmed();
    QString value = trimmed.mid(colon + 1).trimmed();
    if (value.endsWith(QLatin1Char(';'))) {
      value.chop(1);
      value = value.trimmed();
    }
    palette.emplace(std::move(token), std::move(value));
  }
  return palette;
}

QString stylesheetResource(Theme t) {
  return QStringLiteral(":/resources/stylesheet_%1.qss")
      .arg(t == Theme::Dark ? QLatin1String("dark") : QLatin1String("light"));
}

std::map<QString, QString> loadPalette(Theme t) {
  QFile file(stylesheetResource(t));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qCWarning(lcTokens) << "Framework palette resource unavailable:" << stylesheetResource(t)
                        << "- self-painted widgets will get invalid values; link resources.qrc into the target.";
    return {};
  }
  return parsePalette(QString::fromUtf8(file.readAll()));
}

// Per-theme palette cache. The .qss files are compiled-in qrc resources, so their palette is fixed for the process
// lifetime: each theme is parsed once (magic statics are thread-safe) and a theme switch selects the other map.
const std::map<QString, QString>& paletteFor(Theme t) {
  if (t == Theme::Dark) {
    static const std::map<QString, QString> dark = loadPalette(Theme::Dark);
    return dark;
  }
  static const std::map<QString, QString> light = loadPalette(Theme::Light);
  return light;
}

// Parses a palette color value: `#rgb`, `#rrggbb`, `#aarrggbb`, named colors (through QColor) and CSS `rgb(...)` /
// `rgba(...)`: the alpha overlay tokens are stored in QSS-friendly rgba() form, which QColor's string constructor
// cannot parse.
QColor parseCssColor(const QString& value) {
  const QString v = value.trimmed();
  if (v.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
    const qsizetype lp = v.indexOf(QLatin1Char('('));
    const qsizetype rp = v.lastIndexOf(QLatin1Char(')'));
    if (lp < 0 || rp <= lp) {
      return {};
    }
    const QStringList parts = v.mid(lp + 1, rp - lp - 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
    auto num = [](const QString& s, bool* ok) { return s.trimmed().toInt(ok); };
    bool ok = true, part_ok = false;
    const int r = parts.size() > 0 ? num(parts[0], &part_ok) : 0;
    ok = ok && part_ok;
    const int g = parts.size() > 1 ? num(parts[1], &part_ok) : 0;
    ok = ok && part_ok;
    const int b = parts.size() > 2 ? num(parts[2], &part_ok) : 0;
    ok = ok && part_ok;
    int a = 255;
    if (parts.size() > 3) {
      a = num(parts[3], &part_ok);
      ok = ok && part_ok;
    }
    if (!ok || parts.size() < 3) {
      return {};
    }
    return QColor(r, g, b, a);
  }
  return QColor(v);
}

// Resolves a palette key to a color, warning (not crashing) on a missing or invalid token: a missing token is a
// build-time bug that should be visible, as PJ::Theme leaves an unknown `${...}` as a literal placeholder.
QColor resolve(const QString& token_key, Theme t) {
  const auto& palette = paletteFor(t);
  const auto it = palette.find(token_key);
  if (it == palette.end()) {
    qCWarning(lcTokens) << "Framework token missing from QSS palette:" << token_key;
    return {};
  }
  const QColor color = parseCssColor(it->second);
  if (!color.isValid()) {
    qCWarning(lcTokens) << "Framework token is not a color:" << token_key << '=' << it->second;
  }
  return color;
}

int resolveInt(const QString& token_key, Theme t) {
  const auto& palette = paletteFor(t);
  const auto it = palette.find(token_key);
  if (it == palette.end()) {
    qCWarning(lcTokens) << "Framework token missing from QSS palette:" << token_key;
    return -1;
  }
  bool ok = false;
  const int value = it->second.toInt(&ok);
  if (!ok) {
    qCWarning(lcTokens) << "Framework token is not an integer:" << token_key << '=' << it->second;
    return -1;
  }
  return value;
}

QString resolveStr(const QString& token_key, Theme t) {
  const auto& palette = paletteFor(t);
  const auto it = palette.find(token_key);
  if (it == palette.end()) {
    qCWarning(lcTokens) << "Framework token missing from QSS palette:" << token_key;
    return {};
  }
  return it->second;
}

}  // namespace

// -------- Color: surfaces & interaction fills --------

QColor surface(Surface s, Theme t) {
  return resolve(key(s), t);
}

QColor interaction(Variant v, State st, Theme t) {
  return resolve(key(v, st), t);
}

std::pair<QColor, QColor> special(State st, Theme t) {
  return {interaction(Variant::Accent, st, t), interaction(Variant::Highlight, st, t)};
}

// -------- Color: foreground / content ink --------

QColor text(Theme t) {
  return onSurface(Surface::Backdrop, Emphasis::Default, t);
}

QColor onSurface(Surface s, Emphasis emphasis, Theme t) {
  return resolve(onSurfaceKey(s, emphasis), t);
}

QColor onFill(Variant v, State st, Theme t) {
  return resolve(onFillKey(v, st), t);
}

QColor onSpecial(State st, Theme t) {
  return resolve(onSpecialKey(st), t);
}

QColor iconInk(Theme t) {
  return resolve(iconInkKey(), t);
}

QColor iconInkDisabled(Theme t) {
  return resolve(iconInkDisabledKey(), t);
}

// -------- Color: outline / overlay / selection --------

QColor outline(OutlineRole r, OutlineState st, Theme t) {
  return resolve(key(r, st), t);
}

QColor overlay(Overlay o, Theme t) {
  return resolve(key(o), t);
}

QColor selectionFill(Selection s, Theme t) {
  return resolve(selectionFillKey(s), t);
}

QColor onSelection(Selection s, Theme t) {
  return resolve(onSelectionKey(s), t);
}

// -------- Color: elevated surfaces & gradients --------

QColor elevatedSurface(ElevatedSurface s, Theme t) {
  return resolve(key(s), t);
}

std::pair<QColor, QColor> gradient(Gradient g, Theme t) {
  return {resolve(key(g, GradientEndpoint::Start), t), resolve(key(g, GradientEndpoint::End), t)};
}

// -------- Extended semantic families --------

QColor status(Status s, Theme t) {
  switch (s) {
    case Status::Success:
      return resolve(QStringLiteral("status_success"), t);
    case Status::Warning:
      return resolve(QStringLiteral("status_warning"), t);
    case Status::Error:
      return resolve(QStringLiteral("status_error"), t);
    case Status::Info:
      return resolve(QStringLiteral("status_info"), t);
    case Status::Neutral:
      return resolve(QStringLiteral("status_neutral"), t);
  }
  return {};
}

QColor statusHovered(Status s, Theme t) {
  switch (s) {
    case Status::Warning:
      return resolve(QStringLiteral("status_warning_hovered"), t);
    case Status::Info:
      return resolve(QStringLiteral("status_info_hovered"), t);
    default:
      return status(s, t);
  }
}

QColor onStatus(Status s, Theme t) {
  switch (s) {
    case Status::Success:
      return resolve(QStringLiteral("on_status_success"), t);
    case Status::Warning:
      return resolve(QStringLiteral("on_status_warning"), t);
    case Status::Error:
      return resolve(QStringLiteral("on_status_error"), t);
    case Status::Info:
      return resolve(QStringLiteral("on_status_info"), t);
    case Status::Neutral:
      return resolve(QStringLiteral("on_status_neutral"), t);
  }
  return {};
}
QColor statusErrorSurface(Theme t) {
  return resolve(QStringLiteral("status_error_surface"), t);
}
QColor onStatusErrorSurface(Theme t) {
  return resolve(QStringLiteral("on_status_error_surface"), t);
}

QColor destructive(Destructive d, Theme t) {
  switch (d) {
    case Destructive::Ink:
      return resolve(QStringLiteral("destructive_ink"), t);
    case Destructive::Nominal:
      return resolve(QStringLiteral("destructive_nominal"), t);
    case Destructive::Hovered:
      return resolve(QStringLiteral("destructive_hovered"), t);
    case Destructive::Pressed:
      return resolve(QStringLiteral("destructive_pressed"), t);
  }
  return {};
}
QColor onDestructive(Destructive d, Theme t) {
  switch (d) {
    case Destructive::Hovered:
      return resolve(QStringLiteral("on_destructive_hovered"), t);
    case Destructive::Pressed:
      return resolve(QStringLiteral("on_destructive_pressed"), t);
    case Destructive::Ink:
    case Destructive::Nominal:
      return resolve(QStringLiteral("on_destructive_nominal"), t);
  }
  return {};
}

QColor progress(ProgressRole r, Theme t) {
  switch (r) {
    case ProgressRole::Outline:
      return resolve(QStringLiteral("progress_outline"), t);
    case ProgressRole::Track:
      return resolve(QStringLiteral("progress_track"), t);
    case ProgressRole::Indicator:
      return resolve(QStringLiteral("progress_indicator"), t);
  }
  return {};
}
QColor onProgress(Theme t) {
  return resolve(QStringLiteral("on_progress"), t);
}

QColor sliderHandle(bool hovered, Theme t) {
  return resolve(hovered ? QStringLiteral("slider_handle_hovered") : QStringLiteral("slider_handle_nominal"), t);
}
QColor scrollHandleHovered(Theme t) {
  return resolve(QStringLiteral("scroll_handle_hovered"), t);
}

QColor onOverlayHud(Theme t) {
  return resolve(QStringLiteral("on_overlay_hud"), t);
}
QColor onCodeEditor(Theme t) {
  return resolve(QStringLiteral("on_code_editor"), t);
}

QColor syntaxInk(SyntaxRole r, Theme t) {
  switch (r) {
    case SyntaxRole::Keyword:
      return resolve(QStringLiteral("syntax_keyword"), t);
    case SyntaxRole::Number:
      return resolve(QStringLiteral("syntax_number"), t);
    case SyntaxRole::String:
      return resolve(QStringLiteral("syntax_string"), t);
    case SyntaxRole::Comment:
      return resolve(QStringLiteral("syntax_comment"), t);
    case SyntaxRole::Builtin:
      return resolve(QStringLiteral("syntax_builtin"), t);
  }
  return {};
}

QColor contrastInk(Contrast c) {
  // Theme-independent; both palettes hold the same value, so read either.
  return resolve(c == Contrast::Dark ? QStringLiteral("contrast_dark") : QStringLiteral("contrast_light"), Theme::Dark);
}

QColor iconInkMuted(Theme t) {
  return resolve(QStringLiteral("icon_ink_muted"), t);
}

QColor diagnostic(Diagnostic d) {
  // Dev-only categorical palette; theme-agnostic (same value in both stylesheets).
  static constexpr std::array<const char*, 37> kKeys = {
      "diag_fallback",      "diag_main_window",     "diag_dialog",         "diag_widget",      "diag_frame",
      "diag_splitter",      "diag_splitter_handle", "diag_tab_widget",     "diag_tab_pane",    "diag_tab",
      "diag_menu_bar",      "diag_menu_bar_item",   "diag_menu",           "diag_menu_item",   "diag_tooltip",
      "diag_line_edit",     "diag_plain_text_edit", "diag_text_browser",   "diag_combo_box",   "diag_spin_box",
      "diag_check_box",     "diag_radio_button",    "diag_group_box",      "diag_label",       "diag_push_button",
      "diag_list_view",     "diag_tree_view",       "diag_header_section", "diag_scroll_bar",  "diag_scroll_handle",
      "diag_slider_groove", "diag_slider_handle",   "diag_slider_subpage", "diag_plot_widget", "diag_qwt_plot",
      "diag_title_bar",     "diag_title_bar_button"};
  return resolve(QString::fromLatin1(kKeys[static_cast<size_t>(d)]), Theme::Dark);
}

// -------- Non-color axes --------

int space(Space s, Theme t) {
  return resolveInt(key(s), t);
}
int metric(Metric m, Theme t) {
  return resolveInt(key(m), t);
}
int radius(Radius r, Theme t) {
  return resolveInt(key(r), t);
}
int stroke(Stroke s, Theme t) {
  return resolveInt(key(s), t);
}
int duration(Motion m, Theme t) {
  return resolveInt(key(m), t);
}

TypeSpec type(TextRole role, Theme t) {
  return {
      resolveInt(key(role, TypeFacet::Size), t),
      resolveInt(key(role, TypeFacet::Weight), t),
      resolveStr(key(role, TypeFacet::Family), t),
  };
}

// -------- QSS palette keys --------

QString key(Surface s) {
  switch (s) {
    case Surface::Banner:
      return QStringLiteral("banner");
    case Surface::Backdrop:
      return QStringLiteral("backdrop");
    case Surface::DataBackdrop:
      return QStringLiteral("data_backdrop");
    case Surface::Input:
      return QStringLiteral("input");
    case Surface::Separation:
      return QStringLiteral("separation");
    case Surface::BannerInput:
      return QStringLiteral("banner_input");
    case Surface::ScrollHandle:
      return QStringLiteral("scroll_handle");
  }
  return {};
}

QString key(Variant v, State st) {
  QString variant;
  switch (v) {
    case Variant::Neutral:
      variant = QStringLiteral("neutral");
      break;
    case Variant::Accent:
      variant = QStringLiteral("accent");
      break;
    case Variant::Highlight:
      variant = QStringLiteral("highlight");
      break;
    case Variant::Emphasis:
      variant = QStringLiteral("emphasis");
      break;
  }
  QString state;
  switch (st) {
    case State::Nominal:
      state = QStringLiteral("nominal");
      break;
    case State::Hovered:
      state = QStringLiteral("hovered");
      break;
    // Focus is a ring over the nominal fill; the fill grid has no focused column.
    case State::Focused:
      state = QStringLiteral("nominal");
      break;
    case State::Checked:
      state = QStringLiteral("checked");
      break;
    case State::Pressed:
      state = QStringLiteral("pressed");
      break;
    case State::CheckedHovered:
      state = QStringLiteral("checked_hovered");
      break;
    case State::CheckedPressed:
      state = QStringLiteral("checked_pressed");
      break;
    case State::Disabled:
      state = QStringLiteral("disabled");
      break;
  }
  return variant + QLatin1Char('_') + state;
}

QString key(Emphasis emphasis) {
  switch (emphasis) {
    case Emphasis::Default:
      return QStringLiteral("default");
    case Emphasis::Muted:
      return QStringLiteral("muted");
    case Emphasis::Disabled:
      return QStringLiteral("disabled");
  }
  return {};
}

QString key(OutlineRole r, OutlineState st) {
  QString role;
  switch (r) {
    case OutlineRole::Default:
      role = QStringLiteral("outline_default");
      break;
    case OutlineRole::Interactive:
      role = QStringLiteral("outline_interactive");
      break;
    case OutlineRole::Divider:
      role = QStringLiteral("outline_divider");
      break;
    case OutlineRole::Gridline:
      role = QStringLiteral("outline_gridline");
      break;
  }
  QString state;
  switch (st) {
    case OutlineState::Rest:
      state = QStringLiteral("rest");
      break;
    case OutlineState::Hovered:
      state = QStringLiteral("hovered");
      break;
    case OutlineState::Focused:
      state = QStringLiteral("focused");
      break;
    case OutlineState::Checked:
      state = QStringLiteral("checked");
      break;
    case OutlineState::Disabled:
      state = QStringLiteral("disabled");
      break;
  }
  return role + QLatin1Char('_') + state;
}

QString key(Overlay o) {
  switch (o) {
    case Overlay::Hover:
      return QStringLiteral("overlay_hover");
    case Overlay::Pressed:
      return QStringLiteral("overlay_pressed");
    case Overlay::Selected:
      return QStringLiteral("overlay_selected");
    case Overlay::Scrim:
      return QStringLiteral("overlay_scrim");
    case Overlay::Hud:
      return QStringLiteral("overlay_hud");
  }
  return {};
}

QString key(ElevatedSurface s) {
  switch (s) {
    case ElevatedSurface::Dialog:
      return QStringLiteral("surface_dialog");
    case ElevatedSurface::Card:
      return QStringLiteral("surface_card");
    case ElevatedSurface::CardHover:
      return QStringLiteral("surface_card_hover");
    case ElevatedSurface::Toast:
      return QStringLiteral("surface_toast");
    case ElevatedSurface::Disabled:
      return QStringLiteral("surface_disabled");
  }
  return {};
}

QString key(Gradient g, GradientEndpoint endpoint) {
  QString name;
  switch (g) {
    case Gradient::Accent:
      name = QStringLiteral("gradient_accent");
      break;
    case Gradient::Brand:
      name = QStringLiteral("gradient_brand");
      break;
  }
  return name + QLatin1Char('_') +
         (endpoint == GradientEndpoint::Start ? QStringLiteral("start") : QStringLiteral("end"));
}

QString key(Space s) {
  switch (s) {
    case Space::None:
      return QStringLiteral("space_none");
    case Space::Tight:
      return QStringLiteral("space_tight");
    case Space::Snug:
      return QStringLiteral("space_snug");
    case Space::Comfortable:
      return QStringLiteral("space_comfortable");
    case Space::Section:
      return QStringLiteral("space_section");
  }
  return {};
}

QString key(Metric m) {
  switch (m) {
    case Metric::InputMinHeight:
      return QStringLiteral("size_input_min_height");
    case Metric::InputOuterHeight:
      return QStringLiteral("size_input_outer_height");
  }
  return {};
}

QString key(Radius r) {
  switch (r) {
    case Radius::Square:
      return QStringLiteral("radius_square");
    case Radius::Input:
      return QStringLiteral("radius_input");
    case Radius::Card:
      return QStringLiteral("radius_card");
    case Radius::Dialog:
      return QStringLiteral("radius_dialog");
    case Radius::Pill:
      return QStringLiteral("radius_pill");
  }
  return {};
}

QString key(Stroke s) {
  switch (s) {
    case Stroke::Hairline:
      return QStringLiteral("stroke_hairline");
    case Stroke::Emphasis:
      return QStringLiteral("stroke_emphasis");
  }
  return {};
}

QString key(Motion m) {
  switch (m) {
    case Motion::Fast:
      return QStringLiteral("motion_fast");
    case Motion::Base:
      return QStringLiteral("motion_base");
    case Motion::Slow:
      return QStringLiteral("motion_slow");
    case Motion::RevealHold:
      return QStringLiteral("motion_reveal_hold");
  }
  return {};
}

QString key(TextRole role, TypeFacet facet) {
  QString role_name;
  switch (role) {
    case TextRole::Caption:
      role_name = QStringLiteral("caption");
      break;
    case TextRole::Body:
      role_name = QStringLiteral("body");
      break;
    case TextRole::Title:
      role_name = QStringLiteral("title");
      break;
    case TextRole::Heading:
      role_name = QStringLiteral("heading");
      break;
  }
  QString facet_name;
  switch (facet) {
    case TypeFacet::Family:
      facet_name = QStringLiteral("family");
      break;
    case TypeFacet::Size:
      facet_name = QStringLiteral("size");
      break;
    case TypeFacet::Weight:
      facet_name = QStringLiteral("weight");
      break;
  }
  return QStringLiteral("type_") + role_name + QLatin1Char('_') + facet_name;
}

QString onSurfaceKey(Surface s, Emphasis emphasis) {
  return QStringLiteral("on_") + key(s) + QLatin1Char('_') + key(emphasis);
}

QString onFillKey(Variant v, State st) {
  return QStringLiteral("on_") + key(v, st);
}

QString onSpecialKey(State st) {
  const QString suffix = key(Variant::Neutral, st).mid(QStringLiteral("neutral").size());
  return QStringLiteral("on_special") + suffix;
}

QString selectionFillKey(Selection s) {
  return s == Selection::Item ? QStringLiteral("selection_fill") : QStringLiteral("text_selection_fill");
}

QString onSelectionKey(Selection s) {
  return s == Selection::Item ? QStringLiteral("on_selection") : QStringLiteral("on_text_selection");
}

QString iconInkKey() {
  return QStringLiteral("icon_ink");
}

QString iconInkDisabledKey() {
  return QStringLiteral("icon_ink_disabled");
}

QString textKey() {
  return QStringLiteral("text");
}

Theme appTheme() {
  if (qGuiApp == nullptr) {
    return Theme::Light;
  }
  return themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
}

}  // namespace PJ::theme
