// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_custom_widgets/MissingWidgetPlaceholder.h"

#include <QLabel>
#include <QVBoxLayout>

namespace PJ {

MissingWidgetPlaceholder::MissingWidgetPlaceholder(const QString& message, QWidget* parent) : QWidget(parent) {
  auto* label = new QLabel(message, this);
  // The message quotes the layout's plugin id and type and the plugin's own error text: never
  // let QLabel's rich-text auto-detection render any of it as markup.
  label->setTextFormat(Qt::PlainText);
  label->setAlignment(Qt::AlignCenter);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // The panel stays usable as a drop target and resizes freely, so the label never sets a minimum.
  label->setMinimumSize(0, 0);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->addWidget(label);
}

}  // namespace PJ
