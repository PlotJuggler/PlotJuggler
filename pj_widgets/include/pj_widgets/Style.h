#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QIcon>
#include <QProxyStyle>
#include <QSize>
#include <QStyleOption>
#include <algorithm>

#include "pj_widgets/SvgUtil.h"  // loadSvg, currentTheme

namespace PJ {

// QProxyStyle layered over Fusion that adjusts Qt defaults that clash with the app's chrome:
//   - SH_DialogButtonBox_ButtonsHaveIcons: stops the platform from stamping its theme glyphs onto Ok, Cancel,
//     Save, Yes and No inside the QDialogButtonBoxes of plugin dialogs loaded through pj_dialog_host.
//   - SH_UnderlineShortcut: turns off underlining the mnemonic letter ("&Save"), which bleeds through on menus
//     and dialog buttons.
//   - SP_LineEditClearButton: swaps Qt's clear-field glyph (a dark disc with a white X on Fusion) for the app's
//     close X, so a QLineEdit's clear affordance matches the close X of tabs and dialogs.
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
