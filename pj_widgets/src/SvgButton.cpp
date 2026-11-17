// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SvgButton.h"

#include <QEvent>
#include <QSize>

#include "pj_widgets/SvgUtil.h"

namespace PJ {

SvgButton::SvgButton(QWidget* parent) : QToolButton(parent) {
  setAutoRaise(true);
  setFocusPolicy(Qt::NoFocus);
  setSize(Size::kDefault);
}

SvgButton::SvgButton(const QString& icon_path, Size size, QWidget* parent) : SvgButton(parent) {
  setSize(size);
  setIconPath(icon_path);
}

void SvgButton::setIconPath(const QString& icon_path) {
  icon_path_ = icon_path;
  reloadIcon();
}

void SvgButton::setSize(Size size) {
  const int extent = size == Size::kSmaller ? kSmallerExtent : kDefaultExtent;
  setExtent(extent, extent);
}

void SvgButton::setExtent(int button_px, int icon_px) {
  setFixedSize(button_px, button_px);
  setIconSize(QSize(icon_px, icon_px));
}

void SvgButton::changeEvent(QEvent* event) {
  QToolButton::changeEvent(event);
  // Qt 6 has no QApplication::paletteChanged signal; a theme switch arrives as these events (the host repaints qApp's
  // palette and stylesheet), so this is the re-tint hook, with no per-button connection.
  switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
      reloadIcon();
      break;
    default:
      break;
  }
}

void SvgButton::reloadIcon() {
  if (!icon_path_.isEmpty()) {
    setIcon(loadSvg(icon_path_, currentTheme()));
  }
}

}  // namespace PJ
