// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/TailFollowOverlay.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QScrollBar>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgButton.h"

namespace PJ {

TailFollowOverlay::TailFollowOverlay(QAbstractScrollArea* view) : QWidget(view->viewport()), view_(view) {
  setObjectName(QStringLiteral("tailFollowOverlay"));
  const auto fw_theme = theme::appTheme();
  auto* button = new SvgButton(QStringLiteral(":/resources/svg/expand_more.svg"), SvgButton::Size::kDefault, this);
  button->setObjectName(QStringLiteral("tailFollowButton"));
  button->setToolTip(tr("Jump to the latest"));
  button->setCursor(Qt::PointingHandCursor);
  // A HUD-toned disc under the glyph so it reads over any line of text.
  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet(QStringLiteral("QWidget#tailFollowOverlay { background-color: %1; border-radius: %2px; }")
                    .arg(theme::overlay(theme::Overlay::Hud, fw_theme).name(QColor::HexArgb))
                    .arg(button->sizeHint().height() / 2));
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(button);

  QScrollBar* bar = view->verticalScrollBar();
  connect(button, &QAbstractButton::clicked, this, [this]() {
    if (!view_.isNull()) {
      QScrollBar* sb = view_->verticalScrollBar();
      sb->setValue(sb->maximum());
    }
  });
  connect(bar, &QScrollBar::valueChanged, this, [this](int) { sync(); });
  connect(bar, &QScrollBar::rangeChanged, this, [this](int, int) { sync(); });
  view->viewport()->installEventFilter(this);
  sync();
}

void TailFollowOverlay::sync() {
  if (view_.isNull()) {
    return;
  }
  QScrollBar* sb = view_->verticalScrollBar();
  const bool show = sb->maximum() > 0 && sb->value() < sb->maximum();
  // Placement depends on the viewport, not the scroll offset: lay out once
  // when the button appears (and on viewport resize), not on every scroll.
  if (show && isHidden()) {
    place();
  }
  setVisible(show);
}

bool TailFollowOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Resize) {
    place();
  }
  return QWidget::eventFilter(watched, event);
}

void TailFollowOverlay::place() {
  QWidget* viewport = parentWidget();
  if (viewport == nullptr) {
    return;
  }
  adjustSize();
  const int margin = theme::space(theme::Space::Comfortable, theme::appTheme());
  move(viewport->width() - width() - margin, viewport->height() - height() - margin);
  raise();
}

}  // namespace PJ
