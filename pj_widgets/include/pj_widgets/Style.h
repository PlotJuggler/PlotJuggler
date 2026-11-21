#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QIcon>
#include <QPainter>
#include <QPen>
#include <QProxyStyle>
#include <QPushButton>
#include <QSize>
#include <QStyleOption>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"  // loadSvg, currentTheme

namespace PJ {

// QProxyStyle layered over Fusion that adjusts Qt defaults that clash with the app's chrome:
//   - generatedIconPixmap: the disabled look of icons without a Disabled pixmap of their own (below).
//   - SH_DialogButtonBox_ButtonsHaveIcons: stops the platform from stamping its theme glyphs onto Ok, Cancel,
//     Save, Yes and No inside the QDialogButtonBoxes of plugin dialogs loaded through pj_dialog_host.
//   - SH_UnderlineShortcut: turns off underlining the mnemonic letter ("&Save"), which bleeds through on menus
//     and dialog buttons.
//   - SP_LineEditClearButton: swaps Qt's clear-field glyph (a dark disc with a white X on Fusion) for the app's
//     close X, so a QLineEdit's clear affordance matches the close X of tabs and dialogs.
//   - PE_FrameFocusRect on a QPushButton: a ring just inside the button's edge, after keyboard navigation only.
class Style : public QProxyStyle {
  Q_OBJECT
 public:
  using QProxyStyle::QProxyStyle;

  int styleHint(
      StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
      QStyleHintReturn* return_data = nullptr) const override {
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons || hint == SH_UnderlineShortcut) {
      return 0;
    }
    return QProxyStyle::styleHint(hint, option, widget, return_data);
  }

  // Replaces Qt's QLineEdit clear button with the app's close X (resources/svg/close-button.svg, as tab and dialog
  // close). Honoured while the app stylesheet is active: QStyleSheetStyle forwards standardIcon() to this base style
  // when no QSS rule supplies the icon. loadSvg tints the glyph to the active theme ink and caches it; QLineEdit caches
  // the returned icon, so a live theme switch keeps the prior tint until the field rebuilds the button.
  QIcon standardIcon(
      StandardPixmap standard_icon, const QStyleOption* option = nullptr,
      const QWidget* widget = nullptr) const override {
    if (standard_icon == SP_LineEditClearButton) {
      return QIcon(loadSvg(QStringLiteral(":/resources/svg/close-button.svg"), currentTheme()));
    }
    return QProxyStyle::standardIcon(standard_icon, option, widget);
  }

  // Keyboard focus on a push button. Under the app stylesheet Qt hands this the label's contents rect, so a ring there
  // cuts the label of a button that hugs its text: the ring is the button's own edge instead (same corner radius), in
  // the fill mixed half and half with the theme's text ink, so it moves away from the fill toward the ink that reads
  // on the backdrop next to it (lighter in dark, darker in light). A flat button's fill is the backdrop behind it
  // (`QPalette::Window`). The fill is the polished rest-state palette: a `:hover` rule that swaps the fill leaves the
  // ring in the rest tint while the pointer is on the button.
  // Drawn only after keyboard navigation (State_KeyboardFocusChange), so a mouse click leaves no ring behind. Reached
  // only while the QSS rule for the button declares no `outline`: QStyleSheetStyle draws a declared outline itself.
  void drawPrimitive(
      PrimitiveElement element, const QStyleOption* option, QPainter* painter,
      const QWidget* widget = nullptr) const override {
    if (element == PE_FrameFocusRect && qobject_cast<const QPushButton*>(widget) != nullptr) {
      if ((option->state & State_KeyboardFocusChange) != 0) {
        const qreal width = theme::stroke(theme::Stroke::Emphasis);
        const qreal radius = theme::radius(theme::Radius::Input);
        QColor fill = option->palette.color(QPalette::Button);
        if (fill.alpha() == 0) {
          fill = option->palette.color(QPalette::Window);
        }
        const QColor ink = theme::text(theme::appTheme());
        const QColor ring(
            (fill.red() + ink.red()) / 2, (fill.green() + ink.green()) / 2, (fill.blue() + ink.blue()) / 2);
        const qreal inset = width / 2;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(ring, width));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(
            QRectF(widget->rect()).adjusted(inset, inset, -inset, -inset), radius - inset, radius - inset);
        painter->restore();
      }
      return;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
  }

  // The disabled look of every icon that has no QIcon::Disabled pixmap of its own (all loadSvg / renderSvgPixmap icons,
  // plugin .ui iconsets, actions): Qt asks the application style for it, also under the app stylesheet. Fusion's greyed
  // copy of a light-on-dark glyph stays nearly as bright as the enabled one, so the glyph is painted flat in the
  // icon_ink_disabled token instead. A multi-colour icon becomes a one-ink silhouette too.
  QPixmap generatedIconPixmap(QIcon::Mode mode, const QPixmap& pixmap, const QStyleOption* option) const override {
    if (mode == QIcon::Disabled) {
      return tintedPixmap(pixmap, theme::iconInkDisabled(theme::appTheme()));
    }
    return QProxyStyle::generatedIconPixmap(mode, pixmap, option);
  }

  // Fallback only: the compact input height when no stylesheet is active (a unit test without the app QSS). In the
  // running app the QSS governs input height through the `input_outer_height` / `input_min_height` tokens in
  // resources/stylesheet_*.qss, once qApp->setStyleSheet() wraps this style in a QStyleSheetStyle, and this override is
  // bypassed. It mirrors the QSS outer height, so the two never disagree.
  QSize sizeFromContents(
      ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override {
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_LineEdit || type == CT_ComboBox || type == CT_SpinBox) {
      s.setHeight(kInputHeight);
    }
    return s;
  }

  // The compact input outer height. The primary knob is the QSS token `input_outer_height`; keep this C++ mirror in
  // lockstep with it, so the no-stylesheet fallback matches what the stylesheet produces.
  static constexpr int kInputHeight = 20;
};

}  // namespace PJ
