// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "WidgetTuner.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QString>
#include <QWidget>
using namespace Qt::StringLiterals;

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {

theme::Theme popupTheme() {
  return theme::themeFor(currentTheme().contains(QStringLiteral("light")));
}

// Combo popup palettes use the same framework input surface as the closed combo.
QColor popupBgColor() {
  return theme::surface(theme::Surface::Input, popupTheme());
}

QColor popupTextColor() {
  return theme::onSurface(theme::Surface::Input, theme::Emphasis::Default, popupTheme());
}

// Every role Fusion reads when painting a popup surface (Button included).
void applyPopupPalette(QWidget* w) {
  if (w == nullptr) {
    return;
  }
  const QColor bg = popupBgColor();
  const QColor fg = popupTextColor();
  QPalette p = w->palette();
  p.setColor(QPalette::Window, bg);
  p.setColor(QPalette::Base, bg);
  p.setColor(QPalette::AlternateBase, bg);
  p.setColor(QPalette::Button, bg);
  p.setColor(QPalette::WindowText, fg);
  p.setColor(QPalette::Text, fg);
  p.setColor(QPalette::ButtonText, fg);
  w->setPalette(p);
  w->setAutoFillBackground(true);
}

}  // namespace

bool WidgetTuner::eventFilter(QObject* watched, QEvent* event) {
  // A rich-text label fixes its links' colour from its palette when its text is parsed, so after a live theme switch a
  // label already shown kept the old link colour (the light theme's dark blue on the dark dialog). Its palette is the
  // one the stylesheet sets, new only once the new stylesheet has re-polished it (StyleChange), not at the
  // application palette change before it. QLabel::setText ignores the same text, hence the clear.
  if (event->type() == QEvent::StyleChange) {
    if (auto* label = qobject_cast<QLabel*>(watched); label != nullptr && label->text().contains(u"<a "_s)) {
      const QString text = label->text();
      label->clear();
      label->setText(text);
    }
    return QObject::eventFilter(watched, event);
  }
  if (event->type() != QEvent::Polish) {
    return QObject::eventFilter(watched, event);
  }

  if (auto* menu = qobject_cast<QMenu*>(watched); menu != nullptr && menu->objectName().isEmpty()) {
    menu->setObjectName(u"PJMenu"_s);
  }

  if (auto* msg = qobject_cast<QMessageBox*>(watched)) {
    msg->setWindowFlag(Qt::FramelessWindowHint, true);
  }

  if (auto* combo = qobject_cast<QComboBox*>(watched)) {
    if (auto* view = combo->view(); view != nullptr) {
      applyPopupPalette(view);
      if (auto* viewport = view->viewport(); viewport != nullptr) {
        applyPopupPalette(viewport);
      }
      if (auto* frame = qobject_cast<QFrame*>(view)) {
        frame->setFrameShape(QFrame::NoFrame);
      }
    }
  }

  if (auto* w = qobject_cast<QWidget*>(watched);
      w != nullptr && QString::fromUtf8(w->metaObject()->className()) == "QComboBoxPrivateContainer"_L1) {
    w->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    if (auto* frame = qobject_cast<QFrame*>(w)) {
      frame->setFrameShape(QFrame::NoFrame);
      frame->setLineWidth(0);
      frame->setMidLineWidth(0);
    }
    applyPopupPalette(w);
  }

  return QObject::eventFilter(watched, event);
}

}  // namespace PJ
