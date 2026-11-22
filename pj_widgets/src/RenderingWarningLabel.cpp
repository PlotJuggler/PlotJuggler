// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/RenderingWarningLabel.h"

#include <QLabel>

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
  const theme::Theme theme = theme::appTheme();
  label->setWordWrap(!subtle);
  label->setAlignment(subtle ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
  const QColor ink = subtle ? theme::onOverlayHud(theme) : theme::onStatus(theme::Status::Warning, theme);
  const QColor fill =
      subtle ? theme::overlay(theme::Overlay::Hud, theme) : theme::status(theme::Status::Warning, theme);
  const QString font_size =
      subtle ? QStringLiteral("font-size: %1pt;").arg(theme::type(theme::TextRole::Caption, theme).size) : QString{};
  label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border-radius: %3px; padding: %4px; %5 }")
                           .arg(ink.name(QColor::HexArgb), fill.name(QColor::HexArgb))
                           .arg(theme::radius(theme::Radius::Input, theme))
                           .arg(theme::space(subtle ? theme::Space::Snug : theme::Space::Comfortable, theme))
                           .arg(font_size));
}

}  // namespace PJ
