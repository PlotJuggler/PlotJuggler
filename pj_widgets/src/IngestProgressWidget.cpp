// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/IngestProgressWidget.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QToolButton>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ProgressBar.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

IngestProgressWidget::IngestProgressWidget(QWidget* parent) : QWidget(parent) {
  // Hugs the content (bar and buttons). Otherwise the widget expands to fill its container (the title-bar centre region
  // between its spacers), and since the bar is fixed-width with no stretch item the surplus becomes dead space
  // stretching the strip far past its ~200px of content. Maximum still lets it shrink in a narrow window.
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

  auto* layout = new QHBoxLayout(this);
  // The strip is the bar and buttons, with only the canonical inter-control gap.
  layout->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  layout->setSpacing(theme::space(theme::Space::Comfortable));

  // The bar carries the status text in its centred caption (no separate label).
  bar_ = new ProgressBar(this);
  bar_->setObjectName(u"ingestProgressBar"_s);
  bar_->setFixedWidth(180);  // fixed strip width (no QSS width rule, so this holds)

  // QToolButtons (not QPushButtons): they sit naturally in a chrome/toolbar row.
  primary_ = new QToolButton(this);
  primary_->setObjectName(u"ingestPrimaryButton"_s);
  primary_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  primary_->setFocusPolicy(Qt::NoFocus);
  primary_->hide();  // hidden until setPrimaryButton() configures a label

  secondary_ = new QToolButton(this);
  secondary_->setObjectName(u"ingestSecondaryButton"_s);
  secondary_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  secondary_->setFocusPolicy(Qt::NoFocus);
  secondary_->hide();

  layout->addWidget(bar_);  // fixed width — no stretch
  layout->addWidget(primary_);
  layout->addWidget(secondary_);

  connect(primary_, &QToolButton::clicked, this, [this]() {
    last_action_ = Action::kPrimary;
    emit actionRequested(Action::kPrimary);
  });
  connect(secondary_, &QToolButton::clicked, this, [this]() {
    last_action_ = Action::kSecondary;
    emit actionRequested(Action::kSecondary);
  });

  hide();  // idle until setActive(true)
}

void IngestProgressWidget::setTitle(const QString& label) {
  title_text_ = label;
  updateCaption();
}

void IngestProgressWidget::setCounterText(const QString& text) {
  counter_text_ = text;
  updateCaption();
}

void IngestProgressWidget::updateCaption() {
  QString caption = title_text_;
  if (!counter_text_.isEmpty()) {
    caption += caption.isEmpty() ? counter_text_ : u"  %1"_s.arg(counter_text_);
  }
  // The caption is the bar's QProgressBar format() string (drawn centred); a plain
  // literal shows as-is (no %p/%v/%m token → no percentage substitution).
  bar_->setFormat(caption);
}

void IngestProgressWidget::setRange(int minimum, int maximum) {
  bar_->setRange(minimum, maximum);
}

void IngestProgressWidget::setValue(int value) {
  bar_->setValue(value);
}

void IngestProgressWidget::configureButton(
    QToolButton* button, const QString& label, const QString& icon_path, const QString& tooltip) {
  button->setText(label);
  button->setIcon(icon_path.isEmpty() ? QIcon() : QIcon(loadSvg(icon_path, currentTheme())));
  // Icon-only when there's no label (the title-bar buttons rely on the tooltip
  // for meaning); text-beside-icon otherwise.
  button->setToolButtonStyle(label.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
  button->setToolTip(tooltip);
  button->setVisible(!(label.isEmpty() && icon_path.isEmpty()));
}

void IngestProgressWidget::setPrimaryButton(const QString& label, const QString& icon_path, const QString& tooltip) {
  configureButton(primary_, label, icon_path, tooltip);
}

void IngestProgressWidget::setSecondaryButton(const QString& label, const QString& icon_path, const QString& tooltip) {
  configureButton(secondary_, label, icon_path, tooltip);
}

void IngestProgressWidget::setActive(bool active) {
  setVisible(active);
  if (!active) {
    last_action_ = Action::kNone;
  }
}

}  // namespace PJ
