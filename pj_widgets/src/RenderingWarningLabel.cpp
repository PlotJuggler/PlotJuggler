// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/RenderingWarningLabel.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPalette>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

QLabel* createRenderingWarningLabel(QWidget* parent, const QString& object_name) {
  const auto theme = theme::themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
  auto* label = new QLabel(parent);
  label->setObjectName(object_name);
  label->setWordWrap(true);
  label->setAlignment(Qt::AlignCenter);
  label->setAttribute(Qt::WA_TransparentForMouseEvents);
  label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border-radius: %3px; padding: %4px; }")
                           .arg(
                               theme::onStatus(theme::Status::Warning, theme).name(),
                               theme::status(theme::Status::Warning, theme).name())
                           .arg(theme::radius(theme::Radius::Input, theme))
                           .arg(theme::space(theme::Space::Comfortable, theme)));
  label->hide();
  return label;
}

}  // namespace PJ
