#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <utility>

namespace PJ::theme {

// Typed C++ accessor over the semantic UI framework's palette. The one source of the values is the QSS palette in
// resources/stylesheet_{dark,light}.qss (spec: resources/ui_framework.md); QSS-driven widgets reach the tokens as
// `${token}`. Widgets that paint themselves with QPainter cannot reach the QSS engine, so they read the same tokens by
// name through these functions.
//
// This header holds no color or size numbers. Each call parses the compiled stylesheet resource for the requested theme
// (cached; the .qss files are build-time qrc resources) and returns the token's value, so editing the QSS palette moves
// both render paths and nothing needs keeping in sync.
//
// If the stylesheet resource is not linked (a headless unit test that does not pull in resources.qrc), values resolve
// to an invalid QColor or -1 and a warning is logged: link the qrc, as QSS-styled widgets need.
//
// The one colour vocabulary of the app: every colour resolves from here. Beyond the base surface, interaction, outline
// and overlay grid it covers the extended semantic families: status, destructive, progress, slider and scroll, syntax
// highlighting, contrast ink, and the dev-only diagnostic palette.

// Which stylesheet a value is resolved for. Use themeFor(bool) where only a light/dark flag exists (QPalette::Window
// lightness).
enum class Theme { Dark, Light };

// The seven background surface roles (resources/ui_framework.md "Surfaces"). A region's fill is chosen by its role,
// never by a raw literal.
enum class Surface {
  Banner,        // titlebar / section header bands
  Backdrop,      // default window / dialog / popup background
  DataBackdrop,  // interactable data containers: plots, trees, tables, scrubbers
  Input,         // editable text entry (line/text edits, filter fields)
  Separation,    // dividers, splitter handles, borders
  BannerInput,   // an input embedded inside a Banner
  ScrollHandle,  // grabbable scroll handles (not timeline/playback sliders)
};

// The solid interaction variants (resources/ui_framework.md "Interactions"): accent colour families named by visual
// emphasis, not by state: Neutral (grey default), Accent (blue), Highlight (magenta), Emphasis (yellow). "Special" is a
// per-state Accent to Highlight gradient, built with special().
enum class Variant { Neutral, Accent, Highlight, Emphasis };

// The interaction state ramp of an enabled, interactive element. Focus is an outline over the nominal fill, so
// interaction() and onFill() map State::Focused to Nominal; the visible focus color is outline(role,
// OutlineState::Focused, theme).
enum class State { Nominal, Hovered, Focused, Checked, Pressed, CheckedHovered, CheckedPressed, Disabled };

// Content-ink emphasis levels for surface foreground (resources/ui_framework.md "Foreground").
enum class Emphasis { Default, Muted, Disabled };

// Stroke-ink roles for borders, dividers, gridlines, focus rings and checked outlines (resources/ui_framework.md
// "Outline / Stroke + Focus"): strokes, not fills; Surface::Separation remains the fill role of separator surfaces.
enum class OutlineRole { Default, Interactive, Divider, Gridline };

// The outline state ramp; focus is a distinct ring color, not collapsed into hover.
enum class OutlineState { Rest, Hovered, Focused, Checked, Disabled };

// Alpha-bearing overlay roles (resources/ui_framework.md "Overlays and Selection"): Hover, Pressed and Selected are
// state layers; Scrim and Hud are content veils.
enum class Overlay { Hover, Pressed, Selected, Scrim, Hud };

// Selection fill and ink pairs, distinct from interaction Checked: Item is the solid view, list and tab selection;
// TextEdit is the translucent selection inside editors.
enum class Selection { Item, TextEdit };

// Raised and floating surface roles (resources/ui_framework.md "Elevation Surfaces and Gradients"), kept apart from
// Surface so they do not churn the base seven.
enum class ElevatedSurface { Dialog, Card, CardHover, Toast, Disabled };

// Named two-stop framework gradients. QSS composes the endpoint tokens as qlineargradient(... stop:0
// ${gradient_*_start}, stop:1 ${gradient_*_end}).
enum class Gradient { Accent, Brand };
enum class GradientEndpoint { Start, End };

// Extended semantic families the base interaction grid cannot express: status hues (warning, success, error, info,
// neutral), destructive (close, remove) actions, progress-bar parts, editor syntax ink, theme-independent contrast ink,
// and the dev-only diagnostic categorical palette.
enum class Status { Success, Warning, Error, Info, Neutral };
enum class Destructive { Ink, Nominal, Hovered, Pressed };
enum class ProgressRole { Outline, Track, Indicator };
enum class SyntaxRole { Keyword, Number, String, Comment, Builtin };
enum class Contrast { Dark, Light };
enum class Diagnostic {
  Fallback,
  MainWindow,
  Dialog,
  Widget,
  Frame,
  Splitter,
  SplitterHandle,
  TabWidget,
  TabPane,
  Tab,
  MenuBar,
  MenuBarItem,
  Menu,
  MenuItem,
  ToolTip,
  LineEdit,
  PlainTextEdit,
  TextBrowser,
  ComboBox,
  SpinBox,
  CheckBox,
  RadioButton,
  GroupBox,
  Label,
  PushButton,
  ListView,
  TreeView,
  HeaderSection,
  ScrollBar,
  ScrollHandle,
  SliderGroove,
  SliderHandle,
  SliderSubPage,
  PlotWidget,
  QwtPlot,
  TitleBar,
  TitleBarButton
};

// Non-color axes (resources/ui_framework.md "Size / Non-Color Axes"): theme-agnostic; the Theme parameter only selects
// which palette copy is read.
enum class Space { None, Tight, Snug, Comfortable, Section };
enum class Metric {
  InputMinHeight,
  InputOuterHeight,
};
enum class Radius { Square, Input, Card, Dialog, Pill };
enum class Stroke { Hairline, Emphasis };
enum class Motion { Fast, Base, Slow, RevealHold };
enum class TextRole { Caption, Body, Title, Heading };
enum class TypeFacet { Family, Size, Weight };
struct TypeSpec {
  int size;
  int weight;
  QString family;
};

// Framework color of a surface role in the given theme.
QColor surface(Surface s, Theme t);

// Framework color of a solid interaction variant in a state and theme.
QColor interaction(Variant v, State st, Theme t);

// The "Special" variant for a state: the gradient endpoints {Accent, Highlight} of that state. Callers build the
// QLinearGradient; QSS composes the same gradient from the two ${accent_*} / ${highlight_*} tokens.
std::pair<QColor, QColor> special(State st, Theme t);

// Default foreground and text ink: an alias for onSurface(Backdrop, Default).
QColor text(Theme t);

// Content ink on a background surface role at the requested emphasis.
QColor onSurface(Surface s, Emphasis emphasis, Theme t);

// Content ink on a solid interaction fill for the variant and state cell. The legible ink flips by state (WCAG), so it
// is per cell, not per variant.
QColor onFill(Variant v, State st, Theme t);

// Content ink over the Special gradient for the state.
QColor onSpecial(State st, Theme t);

// Icon ink: nominal icons match the body-text ink, disabled icons drop to a muted mid-grey.
QColor iconInk(Theme t);
QColor iconInkDisabled(Theme t);

// Framework stroke ink of an outline role in a state and theme.
QColor outline(OutlineRole r, OutlineState st, Theme t);

// Framework alpha overlay of a role in a theme.
QColor overlay(Overlay o, Theme t);

// Selection fill and ink of a selection role in a theme.
QColor selectionFill(Selection s, Theme t);
QColor onSelection(Selection s, Theme t);

// Framework color of a raised or floating surface role in a theme.
QColor elevatedSurface(ElevatedSurface s, Theme t);

// Named gradient endpoints {start, end}; callers build the QLinearGradient.
std::pair<QColor, QColor> gradient(Gradient g, Theme t);

// Semantic status hue (theme-agnostic warning, success, error, info, neutral).
QColor status(Status s, Theme t);
// Hovered status hue. Only Warning and Info define a hover; the rest return the nominal status(s, t).
QColor statusHovered(Status s, Theme t);
// Ink over a saturated status fill, per status: the legible ink flips with the hue's luminance (dark on the warm, light
// hues, white on error).
QColor onStatus(Status s, Theme t);
// Soft invalid-field error surface and its ink (validation backgrounds).
QColor statusErrorSurface(Theme t);
QColor onStatusErrorSurface(Theme t);

// Destructive (close, remove) fills and the ink over them, per fill state: the pale Hovered fill needs dark ink,
// Nominal and Pressed need white.
QColor destructive(Destructive d, Theme t);
QColor onDestructive(Destructive d, Theme t);

// Progress-bar parts (outline, track, indicator) and the caption ink.
QColor progress(ProgressRole r, Theme t);
QColor onProgress(Theme t);

// Slider handle (nominal, hovered) and the scrollbar-handle hover tint.
QColor sliderHandle(bool hovered, Theme t);
QColor scrollHandleHovered(Theme t);

// Foreground ink over the Overlay::Hud veil (HUD, tracker and hint text).
QColor onOverlayHud(Theme t);

// Code-editor body ink (the editor background is Surface::DataBackdrop).
QColor onCodeEditor(Theme t);

// Editor syntax-highlighting ink of a token role.
QColor syntaxInk(SyntaxRole r, Theme t);

// Theme-independent guaranteed-contrast ink, for a cursor or ring drawn over an arbitrary user-picked colour, where
// theme ink cannot guarantee contrast.
QColor contrastInk(Contrast c);

// Dev-only categorical diagnostic hue (DebugUi widget-class outlines), distinct per widget class; not for product
// chrome.
QColor diagnostic(Diagnostic d);

// Muted tree-branch line ink (also baked into the *_light/_dark.svg branch assets).
QColor iconInkMuted(Theme t);

int space(Space s, Theme t = Theme::Dark);
int metric(Metric m, Theme t = Theme::Dark);
int radius(Radius r, Theme t = Theme::Dark);
int stroke(Stroke s, Theme t = Theme::Dark);
int duration(Motion m, Theme t = Theme::Dark);
TypeSpec type(TextRole role, Theme t = Theme::Dark);

QString key(Surface s);
QString key(Variant v, State st);
QString key(Emphasis emphasis);
QString key(OutlineRole r, OutlineState st);
QString key(Overlay o);
QString key(ElevatedSurface s);
QString key(Gradient g, GradientEndpoint endpoint);
QString key(Space s);
QString key(Metric m);
QString key(Radius r);
QString key(Stroke s);
QString key(Motion m);
QString key(TextRole role, TypeFacet facet);
QString onSurfaceKey(Surface s, Emphasis emphasis);
QString onFillKey(Variant v, State st);
QString onSpecialKey(State st);
QString selectionFillKey(Selection s);
QString onSelectionKey(Selection s);
QString iconInkKey();
QString iconInkDisabledKey();
QString textKey();

// Resolves a Theme from a light/dark flag (a widget's palette lightness).
inline Theme themeFor(bool light) {
  return light ? Theme::Light : Theme::Dark;
}

// The active theme, from the application palette's Window lightness (Theme.cpp keeps QGuiApplication::palette() in
// lockstep with the loaded stylesheet). Always use this, never a widget-local palette: QStyleSheetStyle rewrites widget
// palettes under QSS (a styled ancestor can resolve Window to #000000), which mis-detects "dark" inside styled
// containers. Light when no QGuiApplication exists (headless tests).
[[nodiscard]] Theme appTheme();

}  // namespace PJ::theme
