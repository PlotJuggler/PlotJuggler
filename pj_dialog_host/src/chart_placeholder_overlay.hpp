#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QEvent>
#include <QLabel>
#include <QString>
#include <QWidget>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

// Centered translucent hint banner over an empty chart frame (a drag-and-drop prompt): transparent to the mouse,
// sized to its text, re-centered when the host frame resizes. The host shows it while the chart has no series.
class ChartPlaceholderOverlay : public QLabel {
  Q_OBJECT
 public:
  explicit ChartPlaceholderOverlay(QWidget* host) : QLabel(host) {
    const auto fw_theme = theme::appTheme();
    setObjectName(QStringLiteral("chartPlaceholderOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAlignment(Qt::AlignCenter);
    setWordWrap(true);
    setStyleSheet(
        QStringLiteral(
            "QLabel#chartPlaceholderOverlay { background-color: %1; color: %2; "
            "border: 1px solid %3; border-radius: %4px; padding: %5px %6px; "
            "font-size: 13px; font-weight: 600; }")
            .arg(
                theme::overlay(theme::Overlay::Hud, fw_theme).name(QColor::HexArgb),
                theme::onOverlayHud(fw_theme).name(QColor::HexArgb),
                theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, fw_theme).name(QColor::HexArgb))
            .arg(theme::radius(theme::Radius::Dialog))
            .arg(theme::space(theme::Space::Comfortable, fw_theme))
            .arg(theme::space(theme::Space::Section, fw_theme)));
    if (host != nullptr) {
      host->installEventFilter(this);
    }
  }

  void setMessage(const QString& text) {
    setText(text);
    // Text can change the banner geometry while the host stays fixed.
    last_host_size_ = QSize();
    recenter();
  }

  // Centers over the host frame, sized to the text (capped to the frame width).
  void recenter() {
    QWidget* host = parentWidget();
    if (host == nullptr) {
      return;
    }
    // Recomputes only when the host size changed, so the Paint filter can call it on every repaint without
    // re-firing the wrap toggle and resize.
    if (host->size() == last_host_size_) {
      return;
    }
    last_host_size_ = host->size();
    // Wraps only when the single-line text would overflow the host width.
    const int avail_w = std::max(80, host->width() - 48);
    setWordWrap(false);
    int w = sizeHint().width();
    if (w > avail_w) {
      setWordWrap(true);
      w = avail_w;
    }
    // heightForWidth() is the wrapped height: QLabel::sizeHint() does not track a constrained width, so
    // adjustSize() would clip a wrapped banner and center it off. resize(), not setFixed*, keeps it re-measurable.
    const int h = wordWrap() ? heightForWidth(w) : sizeHint().height();
    resize(w, std::max(h, 1));
    move(std::max(0, (host->width() - width()) / 2), std::max(0, (host->height() - height()) / 2));
    raise();
  }

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override {
    // Resize and Show cover the common cases; Paint fires after the final layout settles, so a host sized before
    // this overlay existed (during dialog construction) is still centered on the first paint.
    if (obj == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Paint)) {
      recenter();
    }
    return QLabel::eventFilter(obj, event);
  }

 private:
  QSize last_host_size_;
};

}  // namespace PJ
