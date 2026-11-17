// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ProgressDialog.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

ProgressDialog::ProgressDialog(QWidget* parent) : Dialog(parent) {
  setWindowModality(Qt::WindowModal);
  setMinimumWidth(400);
  // The two action buttons are the only sanctioned exits (see header).
  setCloseButtonVisible(false);

  message_ = new QLabel(contentWidget());
  message_->setAlignment(Qt::AlignCenter);
  message_->hide();

  bar_ = new QProgressBar(contentWidget());
  bar_->setAlignment(Qt::AlignCenter);
  bar_->setTextVisible(true);

  primary_ = new QPushButton(contentWidget());
  secondary_ = new QPushButton(contentWidget());
  primary_->hide();
  secondary_->hide();

  connect(primary_, &QPushButton::clicked, this, [this]() {
    action_ = Action::kPrimary;
    hide();
    emit stopRequested(action_);
  });
  connect(secondary_, &QPushButton::clicked, this, [this]() {
    action_ = Action::kSecondary;
    hide();
    emit stopRequested(action_);
  });

  auto* button_row = new QHBoxLayout();
  button_row->addStretch();
  button_row->addWidget(primary_);
  button_row->addWidget(secondary_);

  // contentLayout() is the base chrome's vertical layout under the titlebar.
  auto* layout = qobject_cast<QVBoxLayout*>(contentLayout());
  layout->setContentsMargins(
      theme::space(theme::Space::Section), theme::space(theme::Space::Section), theme::space(theme::Space::Section),
      theme::space(theme::Space::Section));
  layout->setSpacing(theme::space(theme::Space::Comfortable));
  layout->addWidget(message_);
  layout->addWidget(bar_);
  layout->addLayout(button_row);
}

ProgressDialog::~ProgressDialog() = default;

void ProgressDialog::configureButton(
    QPushButton* button, const QString& label, const QString& icon_path, const QString& tooltip) {
  button->setText(label);
  button->setToolTip(tooltip);
  button->setIcon(icon_path.isEmpty() ? QIcon() : QIcon(loadSvg(icon_path, currentTheme())));
  button->setVisible(!label.isEmpty());
}

void ProgressDialog::setPrimaryButton(const QString& label, const QString& icon_path, const QString& tooltip) {
  configureButton(primary_, label, icon_path, tooltip);
}

void ProgressDialog::setSecondaryButton(const QString& label, const QString& icon_path, const QString& tooltip) {
  configureButton(secondary_, label, icon_path, tooltip);
}

void ProgressDialog::setRange(int minimum, int maximum) {
  bar_->setRange(minimum, maximum);
}

void ProgressDialog::setValue(int value) {
  bar_->setValue(value);
}

int ProgressDialog::maximum() const {
  return bar_->maximum();
}

void ProgressDialog::setMessage(const QString& text) {
  message_->setText(text);
  message_->setVisible(!text.isEmpty());
}

void ProgressDialog::setStopButtonsVisible(bool visible) {
  // A button with no label was never configured: it stays hidden either way.
  primary_->setVisible(visible && !primary_->text().isEmpty());
  secondary_->setVisible(visible && !secondary_->text().isEmpty());
}

void ProgressDialog::keyPressEvent(QKeyEvent* event) {
  // Swallow Esc: the default QDialog handling would reject() and hide the
  // dialog while the work behind it keeps running with no visible prompt.
  if (event->key() == Qt::Key_Escape) {
    event->ignore();
    return;
  }
  Dialog::keyPressEvent(event);
}

}  // namespace PJ
