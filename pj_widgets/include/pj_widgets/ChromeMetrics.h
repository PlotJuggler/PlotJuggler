#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// Global icon and layout dimensions broadcast from MainWindow to every chrome-aware widget, in pixels. Defaults match
// the first-launch baseline MainWindow uses before QSettings is read.
struct ChromeMetrics {
  int icon_size = 24;
  int icon_padding = 4;
  int layout_padding = 2;
  int layout_spacing = 2;

  // The one titlebar-tone band height: every header band in the app (the left-panel and timeline headers, the curve
  // Width and Style headers, the toolbox panel banner, every plugin SectionHeaderBand) is this tall, so they read as
  // one strip. The button extent (icon_size + icon_padding) grown by the layout padding on each side, as the panel
  // headers' contentsMargins apply it. The only place for that formula: re-deriving it at a call site is how bands
  // drifted out of alignment.
  [[nodiscard]] int bandHeight() const {
    return icon_size + icon_padding + (2 * layout_padding);
  }

  // The window-chrome (title-bar) height: the main-window TitleBar and every framed Dialog chrome are exactly this
  // tall. bandHeight() plus 1px for the QSS bottom border, which draws inside the bar's geometry. The only place for
  // that formula: never hardcode a title-bar height.
  [[nodiscard]] int titleBarHeight() const {
    return bandHeight() + 1;
  }
};

}  // namespace PJ
