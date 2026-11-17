// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesNavRow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>

#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace PJ {

PreferencesNavRow::PreferencesNavRow(const QString& text, QWidget* parent)
    : QFrame(parent), label_(new QLabel(text, this)) {
  setObjectName(u"navRow"_s);
  setProperty("selected", false);
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);

  auto* layout = new QHBoxLayout(this);
  // Matches the accent edge, so the label does not shift when selected.
  layout->setContentsMargins(
      PJ::theme::space(theme::Space::Section), PJ::theme::space(theme::Space::Comfortable),
      PJ::theme::space(theme::Space::Section), PJ::theme::space(theme::Space::Comfortable));
  layout->setSpacing(PJ::theme::space(theme::Space::None));
  label_->setObjectName(u"navRowLabel"_s);
  layout->addWidget(label_);
  layout->addStretch(1);
}

void PreferencesNavRow::setSelected(bool selected) {
  if (selected_ == selected) {
    return;
  }
  selected_ = selected;
  setProperty("selected", selected);
  // A dynamic-property change does not re-apply QSS without unpolish/polish.
  style()->unpolish(this);
  style()->polish(this);
  update();
}

void PreferencesNavRow::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit clicked();
    event->accept();
    return;
  }
  QFrame::mousePressEvent(event);
}

}  // namespace PJ
