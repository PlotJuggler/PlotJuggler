// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/RenderingWarningLabel.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPalette>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

QLabel* createRenderingWarningLabel(QWidget* parent, const QString& object_name) {
  auto* label = new QLabel(parent);
  label->setObjectName(object_name);
  label->setAttribute(Qt::WA_TransparentForMouseEvents);
  setRenderingWarningSubtle(label, false);
  label->hide();
  return label;
}

void setRenderingWarningSubtle(QLabel* label, bool subtle) {
  const auto theme = theme::themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
  const auto css_color = [](const QColor& color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
  };
  label->setWordWrap(!subtle);
  label->setAlignment(subtle ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
  if (subtle) {
    label->setStyleSheet(
        QStringLiteral("QLabel { color: %1; background: %2; border-radius: %3px; padding: %4px; font-size: %5pt; }")
            .arg(css_color(theme::onOverlayHud(theme)), css_color(theme::overlay(theme::Overlay::Hud, theme)))
            .arg(theme::radius(theme::Radius::Input, theme))
            .arg(theme::space(theme::Space::Snug, theme))
            .arg(theme::type(theme::TextRole::Caption, theme).size));
    return;
  }
  label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border-radius: %3px; padding: %4px; }")
                           .arg(
                               theme::onStatus(theme::Status::Warning, theme).name(),
                               theme::status(theme::Status::Warning, theme).name())
                           .arg(theme::radius(theme::Radius::Input, theme))
                           .arg(theme::space(theme::Space::Comfortable, theme)));
}

}  // namespace PJ
