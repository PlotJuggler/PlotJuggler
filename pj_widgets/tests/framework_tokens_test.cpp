// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Completeness guard for the semantic UI framework (resources/ui_framework.md).
//
// The QSS palette in resources/stylesheet_{dark,light}.qss is the single ground
// truth for framework values. FrameworkTokens is a typed C++ reader over it.
// This test drives the real reader against the real (qrc-linked) stylesheets and
// asserts that every role the framework exposes resolves in BOTH themes, plus a
// few relationship/contrast invariants. Adding an enum value without its QSS
// token, deleting a token, or a typo'd value all fail here.

#include <gtest/gtest.h>

#include <QColor>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <array>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"

namespace {

using namespace PJ::theme;

constexpr std::array<Theme, 2> kThemes = {Theme::Dark, Theme::Light};
constexpr std::array<Surface, 7> kSurfaces = {Surface::Banner,      Surface::Backdrop,   Surface::DataBackdrop,
                                              Surface::Input,       Surface::Separation, Surface::BannerInput,
                                              Surface::ScrollHandle};
constexpr std::array<Variant, 4> kVariants = {Variant::Neutral, Variant::Accent, Variant::Highlight, Variant::Emphasis};
// The interaction FILL states (no Focused column — focus is an outline ring).
constexpr std::array<State, 7> kStates = {State::Nominal,        State::Hovered,        State::Checked, State::Pressed,
                                          State::CheckedHovered, State::CheckedPressed, State::Disabled};
constexpr std::array<Emphasis, 3> kEmphases = {Emphasis::Default, Emphasis::Muted, Emphasis::Disabled};
constexpr std::array<OutlineRole, 4> kOutlineRoles = {
    OutlineRole::Default, OutlineRole::Interactive, OutlineRole::Divider, OutlineRole::Gridline};
constexpr std::array<OutlineState, 5> kOutlineStates = {
    OutlineState::Rest, OutlineState::Hovered, OutlineState::Focused, OutlineState::Checked, OutlineState::Disabled};
constexpr std::array<Overlay, 5> kOverlays = {
    Overlay::Hover, Overlay::Pressed, Overlay::Selected, Overlay::Scrim, Overlay::Hud};
constexpr std::array<Selection, 2> kSelections = {Selection::Item, Selection::TextEdit};
constexpr std::array<ElevatedSurface, 5> kElevatedSurfaces = {
    ElevatedSurface::Dialog, ElevatedSurface::Card, ElevatedSurface::CardHover, ElevatedSurface::Toast,
    ElevatedSurface::Disabled};
constexpr std::array<Gradient, 2> kGradients = {Gradient::Accent, Gradient::Brand};
constexpr std::array<Space, 5> kSpaces = {Space::None, Space::Tight, Space::Snug, Space::Comfortable, Space::Section};
constexpr std::array<Metric, 7> kMetrics = {Metric::InputMinHeight, Metric::InputOuterHeight};
constexpr std::array<Radius, 5> kRadii = {Radius::Square, Radius::Input, Radius::Card, Radius::Dialog, Radius::Pill};
constexpr std::array<Stroke, 2> kStrokes = {Stroke::Hairline, Stroke::Emphasis};
constexpr std::array<Motion, 4> kMotions = {Motion::Fast, Motion::Base, Motion::Slow, Motion::RevealHold};
constexpr std::array<TextRole, 4> kTextRoles = {TextRole::Caption, TextRole::Body, TextRole::Title, TextRole::Heading};

const char* themeName(Theme t) {
  return t == Theme::Dark ? "dark" : "light";
}

// ---- Surfaces & interaction fills ----

TEST(FrameworkTokens, EverySurfaceResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (Surface s : kSurfaces) {
      EXPECT_TRUE(surface(s, t).isValid()) << "surface '" << key(s).toStdString() << "' invalid in " << themeName(t);
    }
  }
}

TEST(FrameworkTokens, EveryInteractionCellResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (Variant v : kVariants) {
      for (State st : kStates) {
        EXPECT_TRUE(interaction(v, st, t).isValid())
            << "interaction '" << key(v, st).toStdString() << "' invalid in " << themeName(t);
      }
    }
  }
}

TEST(FrameworkTokens, IconInkResolvesInBothThemes) {
  for (Theme t : kThemes) {
    EXPECT_TRUE(iconInk(t).isValid()) << "icon_ink invalid in " << themeName(t);
    EXPECT_TRUE(iconInkDisabled(t).isValid()) << "icon_ink_disabled invalid in " << themeName(t);
  }
}

// Pin the retuned text/icon ink values so a stray palette edit can't silently
// drift them (these are what the user sees as "text" and icon color).
TEST(FrameworkTokens, TextAndIconInkValues) {
  EXPECT_EQ(text(Theme::Dark).name(QColor::HexRgb), QStringLiteral("#ffffff"));
  EXPECT_EQ(text(Theme::Light).name(QColor::HexRgb), QStringLiteral("#333333"));
  EXPECT_EQ(iconInk(Theme::Dark).name(QColor::HexRgb), QStringLiteral("#ffffff"));
  EXPECT_EQ(iconInk(Theme::Light).name(QColor::HexRgb), QStringLiteral("#333333"));
  EXPECT_EQ(iconInkDisabled(Theme::Dark).name(QColor::HexRgb), QStringLiteral("#8f8fa3"));
  EXPECT_EQ(iconInkDisabled(Theme::Light).name(QColor::HexRgb), QStringLiteral("#a3a3a3"));
}

// End-to-end proof that the app's #3D3D3D chrome-ink icons actually fold onto the
// active icon ink through the real recolor path (SvgUtil::recolorSvgInk).
TEST(SvgRecolor, ChromeInkFoldsOntoIconInk) {
  for (Theme t : kThemes) {
    const bool light = (t == Theme::Light);
    QByteArray svg = R"(<svg viewBox="0 0 24 24"><path fill="#3D3D3D" d="M0 0h24v24H0z"/></svg>)";
    PJ::recolorSvgInk(svg, light);
    const QByteArray ink = iconInk(t).name(QColor::HexRgb).toUtf8();
    EXPECT_FALSE(svg.contains("#3D3D3D")) << "authoring ink survived recolor in " << themeName(t);
    EXPECT_TRUE(svg.contains(ink)) << "expected fold to " << ink.toStdString() << " in " << themeName(t);
  }
}

TEST(FrameworkTokens, SpecialEndpointsAreAccentAndHighlight) {
  for (Theme t : kThemes) {
    for (State st : kStates) {
      const auto [lo, hi] = special(st, t);
      EXPECT_EQ(lo.rgb(), interaction(Variant::Accent, st, t).rgb());
      EXPECT_EQ(hi.rgb(), interaction(Variant::Highlight, st, t).rgb());
    }
  }
}

// ---- Foreground / content ink ----

TEST(FrameworkTokens, EverySurfaceInkResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (Surface s : kSurfaces) {
      for (Emphasis e : kEmphases) {
        EXPECT_TRUE(onSurface(s, e, t).isValid())
            << "surface ink '" << onSurfaceKey(s, e).toStdString() << "' invalid in " << themeName(t);
      }
    }
  }
}

TEST(FrameworkTokens, EveryFillInkCellResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (Variant v : kVariants) {
      for (State st : kStates) {
        EXPECT_TRUE(onFill(v, st, t).isValid())
            << "fill ink '" << onFillKey(v, st).toStdString() << "' invalid in " << themeName(t);
      }
    }
  }
}

TEST(FrameworkTokens, EverySpecialInkResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (State st : kStates) {
      EXPECT_TRUE(onSpecial(st, t).isValid())
          << "special ink '" << onSpecialKey(st).toStdString() << "' invalid in " << themeName(t);
    }
  }
}

TEST(FrameworkTokens, TextAliasesBackdropDefaultSurfaceInk) {
  for (Theme t : kThemes) {
    EXPECT_EQ(text(t).rgb(), onSurface(Surface::Backdrop, Emphasis::Default, t).rgb());
  }
}

TEST(FrameworkTokens, FillInkCanFlipByState) {
  EXPECT_NE(
      onFill(Variant::Highlight, State::Nominal, Theme::Dark).rgb(),
      onFill(Variant::Highlight, State::Checked, Theme::Dark).rgb());
}

// ---- Outline / focus ----

TEST(FrameworkTokens, EveryOutlineCellResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (OutlineRole r : kOutlineRoles) {
      for (OutlineState st : kOutlineStates) {
        EXPECT_TRUE(outline(r, st, t).isValid())
            << "outline '" << key(r, st).toStdString() << "' invalid in " << themeName(t);
      }
    }
  }
}

TEST(FrameworkTokens, OutlineFocusIsDistinctFromHover) {
  for (Theme t : kThemes) {
    EXPECT_NE(
        outline(OutlineRole::Interactive, OutlineState::Focused, t).rgb(),
        outline(OutlineRole::Interactive, OutlineState::Hovered, t).rgb())
        << "focus outline still collapsed into hover in " << themeName(t);
  }
}

TEST(FrameworkTokens, InteractionFocusedUsesNominalFill) {
  for (Theme t : kThemes) {
    for (Variant v : kVariants) {
      EXPECT_EQ(key(v, State::Focused), key(v, State::Nominal));
      EXPECT_EQ(interaction(v, State::Focused, t).rgb(), interaction(v, State::Nominal, t).rgb());
    }
  }
}

// ---- Overlay / selection ----

TEST(FrameworkTokens, EveryOverlayIsTranslucentInBothThemes) {
  for (Theme t : kThemes) {
    for (Overlay o : kOverlays) {
      const QColor c = overlay(o, t);
      EXPECT_TRUE(c.isValid()) << "overlay '" << key(o).toStdString() << "' invalid in " << themeName(t);
      EXPECT_GT(c.alpha(), 0) << key(o).toStdString() << " has no alpha in " << themeName(t);
      EXPECT_LT(c.alpha(), 255) << key(o).toStdString() << " is opaque in " << themeName(t);
    }
  }
}

TEST(FrameworkTokens, SelectionPairsResolveAndStayDistinct) {
  for (Theme t : kThemes) {
    for (Selection s : kSelections) {
      EXPECT_TRUE(selectionFill(s, t).isValid());
      EXPECT_TRUE(onSelection(s, t).isValid());
    }
    EXPECT_EQ(selectionFill(Selection::Item, t).alpha(), 255) << "item selection must be opaque in " << themeName(t);
    EXPECT_LT(selectionFill(Selection::TextEdit, t).alpha(), 255)
        << "text-edit selection must be translucent in " << themeName(t);
    EXPECT_NE(selectionFill(Selection::Item, t).rgb(), interaction(Variant::Neutral, State::Checked, t).rgb())
        << "selection_fill must stay distinct from Primary Checked in " << themeName(t);
  }
}

// ---- Elevated surfaces & gradients ----

TEST(FrameworkTokens, EveryElevatedSurfaceResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (ElevatedSurface s : kElevatedSurfaces) {
      EXPECT_TRUE(elevatedSurface(s, t).isValid())
          << "elevated surface '" << key(s).toStdString() << "' invalid in " << themeName(t);
    }
    EXPECT_NE(elevatedSurface(ElevatedSurface::CardHover, t).rgb(), elevatedSurface(ElevatedSurface::Card, t).rgb());
  }
}

TEST(FrameworkTokens, EveryGradientEndpointResolvesAndDiffers) {
  for (Theme t : kThemes) {
    for (Gradient g : kGradients) {
      const auto [start, end] = gradient(g, t);
      EXPECT_TRUE(start.isValid()) << key(g, GradientEndpoint::Start).toStdString();
      EXPECT_TRUE(end.isValid()) << key(g, GradientEndpoint::End).toStdString();
      EXPECT_NE(start.rgb(), end.rgb()) << "gradient endpoints identical in " << themeName(t);
    }
  }
}

// ---- Extended semantic families ----

TEST(FrameworkTokens, EveryExtendedFamilyTokenResolves) {
  constexpr std::array<Status, 5> kStatuses = {
      Status::Success, Status::Warning, Status::Error, Status::Info, Status::Neutral};
  constexpr std::array<Destructive, 4> kDestructives = {
      Destructive::Ink, Destructive::Nominal, Destructive::Hovered, Destructive::Pressed};
  constexpr std::array<ProgressRole, 3> kProgress = {
      ProgressRole::Outline, ProgressRole::Track, ProgressRole::Indicator};
  constexpr std::array<SyntaxRole, 5> kSyntax = {
      SyntaxRole::Keyword, SyntaxRole::Number, SyntaxRole::String, SyntaxRole::Comment, SyntaxRole::Builtin};
  for (Theme t : kThemes) {
    for (Status s : kStatuses) {
      EXPECT_TRUE(status(s, t).isValid());
      EXPECT_TRUE(statusHovered(s, t).isValid());
    }
    for (Status s : {Status::Success, Status::Warning, Status::Error, Status::Info, Status::Neutral}) {
      EXPECT_TRUE(onStatus(s, t).isValid());
    }
    EXPECT_TRUE(statusErrorSurface(t).isValid());
    EXPECT_TRUE(onStatusErrorSurface(t).isValid());
    for (Destructive d : kDestructives) {
      EXPECT_TRUE(destructive(d, t).isValid());
    }
    for (Destructive d : {Destructive::Ink, Destructive::Nominal, Destructive::Hovered, Destructive::Pressed}) {
      EXPECT_TRUE(onDestructive(d, t).isValid());
    }
    for (ProgressRole r : kProgress) {
      EXPECT_TRUE(progress(r, t).isValid());
    }
    EXPECT_TRUE(onProgress(t).isValid());
    EXPECT_TRUE(sliderHandle(false, t).isValid());
    EXPECT_TRUE(sliderHandle(true, t).isValid());
    EXPECT_TRUE(scrollHandleHovered(t).isValid());
    EXPECT_TRUE(onOverlayHud(t).isValid());
    EXPECT_TRUE(onCodeEditor(t).isValid());
    EXPECT_TRUE(iconInkMuted(t).isValid());
    for (SyntaxRole r : kSyntax) {
      EXPECT_TRUE(syntaxInk(r, t).isValid());
    }
  }
  EXPECT_TRUE(contrastInk(Contrast::Dark).isValid());
  EXPECT_TRUE(contrastInk(Contrast::Light).isValid());
  // Every diagnostic enumerator (Fallback..TitleBarButton) resolves to a palette hue.
  for (int i = 0; i <= static_cast<int>(Diagnostic::TitleBarButton); ++i) {
    EXPECT_TRUE(diagnostic(static_cast<Diagnostic>(i)).isValid()) << "diagnostic index " << i;
  }
}

// ---- Non-color axes ----

TEST(FrameworkTokens, EveryNonColorTokenResolvesInBothThemes) {
  for (Theme t : kThemes) {
    for (Space s : kSpaces) {
      EXPECT_GE(space(s, t), 0) << key(s).toStdString() << " invalid in " << themeName(t);
    }
    for (Metric m : kMetrics) {
      EXPECT_GT(metric(m, t), 0) << key(m).toStdString() << " invalid in " << themeName(t);
    }
    for (Radius r : kRadii) {
      EXPECT_GE(radius(r, t), 0) << key(r).toStdString() << " invalid in " << themeName(t);
    }
    for (Stroke s : kStrokes) {
      EXPECT_GT(stroke(s, t), 0) << key(s).toStdString() << " invalid in " << themeName(t);
    }
    for (Motion m : kMotions) {
      EXPECT_GT(duration(m, t), 0) << key(m).toStdString() << " invalid in " << themeName(t);
    }
    for (TextRole role : kTextRoles) {
      const TypeSpec spec = type(role, t);
      EXPECT_FALSE(spec.family.isEmpty()) << key(role, TypeFacet::Family).toStdString();
      EXPECT_GT(spec.size, 0) << key(role, TypeFacet::Size).toStdString();
      EXPECT_GT(spec.weight, 0) << key(role, TypeFacet::Weight).toStdString();
    }
  }
}

TEST(FrameworkTokens, NonColorTokensAreThemeAgnostic) {
  for (Space s : kSpaces) {
    EXPECT_EQ(space(s, Theme::Dark), space(s, Theme::Light)) << key(s).toStdString();
  }
  for (Metric m : kMetrics) {
    EXPECT_EQ(metric(m, Theme::Dark), metric(m, Theme::Light)) << key(m).toStdString();
  }
  for (Radius r : kRadii) {
    EXPECT_EQ(radius(r, Theme::Dark), radius(r, Theme::Light)) << key(r).toStdString();
  }
  for (Stroke s : kStrokes) {
    EXPECT_EQ(stroke(s, Theme::Dark), stroke(s, Theme::Light)) << key(s).toStdString();
  }
  for (Motion m : kMotions) {
    EXPECT_EQ(duration(m, Theme::Dark), duration(m, Theme::Light)) << key(m).toStdString();
  }
}

TEST(FrameworkTokens, NonColorRelationshipsHold) {
  for (Theme t : kThemes) {
    EXPECT_EQ(space(Space::None, t), 0);
    EXPECT_LT(space(Space::Tight, t), space(Space::Snug, t));
    EXPECT_LT(space(Space::Snug, t), space(Space::Comfortable, t));
    EXPECT_LT(space(Space::Comfortable, t), space(Space::Section, t));
    EXPECT_EQ(radius(Radius::Input, t), space(Space::Snug, t));
    EXPECT_LT(radius(Radius::Input, t), radius(Radius::Card, t));
    EXPECT_LT(stroke(Stroke::Hairline, t), stroke(Stroke::Emphasis, t));
    EXPECT_LT(duration(Motion::Fast, t), duration(Motion::Base, t));
    EXPECT_LT(duration(Motion::Base, t), duration(Motion::Slow, t));
    const TypeSpec caption = type(TextRole::Caption, t);
    const TypeSpec body = type(TextRole::Body, t);
    const TypeSpec heading = type(TextRole::Heading, t);
    EXPECT_LT(caption.size, body.size);
    EXPECT_LT(body.size, heading.size);
  }
}

}  // namespace

// Every ${token} referenced in a stylesheet BODY must be defined in that
// stylesheet's palette block. A missing key survives Theme.cpp's expansion as a
// literal "${...}", which is a Qt CSS parse error — and one parse error makes
// Qt drop the ENTIRE application stylesheet (this exact failure shipped once,
// via ${input_background}). The enum-driven tests above cannot catch body-only
// tokens, so this walks the raw QSS text.
TEST(FrameworkTokens, EveryBodyPlaceholderIsDefinedInThePalette) {
  const std::array<QString, 2> sheets = {
      QStringLiteral(":/resources/stylesheet_dark.qss"), QStringLiteral(":/resources/stylesheet_light.qss")};
  for (const QString& path : sheets) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text)) << path.toStdString();
    const QString qss = QString::fromUtf8(file.readAll());

    const qsizetype start = qss.indexOf(QStringLiteral("PALETTE START"));
    const qsizetype end = qss.indexOf(QStringLiteral("PALETTE END"));
    ASSERT_GE(start, 0) << path.toStdString();
    ASSERT_GT(end, start) << path.toStdString();

    // Palette keys: "key: value" lines between the markers, // comments skipped.
    QSet<QString> keys;
    const QStringList palette_lines = qss.mid(start, end - start).split(QLatin1Char('\n'));
    for (const QString& raw : palette_lines) {
      const QString line = raw.trimmed();
      if (line.isEmpty() || line.startsWith(QStringLiteral("//"))) {
        continue;
      }
      const qsizetype colon = line.indexOf(QLatin1Char(':'));
      if (colon > 0) {
        keys.insert(line.left(colon).trimmed());
      }
    }
    ASSERT_FALSE(keys.isEmpty()) << path.toStdString();

    // Body placeholders.
    const QString body = qss.mid(end);
    static const QRegularExpression re(QStringLiteral("\\$\\{([^}]+)\\}"));
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
      const QString key = it.next().captured(1);
      EXPECT_TRUE(keys.contains(key)) << "unresolved body token ${" << key.toStdString() << "} in "
                                      << path.toStdString();
    }
  }
}
