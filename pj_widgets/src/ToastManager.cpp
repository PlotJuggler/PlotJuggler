// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ToastManager.h"

#include <QEvent>
#include <QLayout>
#include <QRegion>

#include "pj_widgets/ToastNotification.h"

namespace PJ {

ToastManager::ToastManager(QWidget* parent_widget) : QObject(parent_widget), parent_widget_(parent_widget) {
  container_ = new QWidget(parent_widget_);
  container_->setObjectName("toastManagerContainer");
  container_->setAttribute(Qt::WA_TranslucentBackground);
  container_->hide();
}

ToastManager::~ToastManager() = default;

void ToastManager::showToast(const QString& message, const QPixmap& icon, int timeout_ms) {
  auto* toast = new ToastNotification(message, icon, timeout_ms, container_);
  toast->setMaximumWidth(max_width_);
  toast->installEventFilter(this);

  toasts_.append(toast);
  connect(toast, &ToastNotification::closed, this, &ToastManager::onToastClosed);

  updatePosition();
  // Lay out (and mask) before showing: the container must never be visible
  // with a stale input region, or it would swallow clicks it should let
  // through to the host.
  repositionToasts();

  container_->show();
  container_->raise();

  toast->showAnimated();
}

bool ToastManager::eventFilter(QObject* watched, QEvent* event) {
  switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
      updateInputRegion();
      break;
    default:
      break;
  }
  return QObject::eventFilter(watched, event);
}

void ToastManager::updatePosition() {
  // Nothing to place when there are no toasts (the container is hidden), so a
  // host resize is a true no-op until the first toast appears.
  if (!parent_widget_ || toasts_.isEmpty()) {
    return;
  }

  const int container_width = max_width_ + margin_right_ * 2;
  const int container_height = parent_widget_->height();

  container_->setFixedSize(container_width, container_height);
  container_->move(parent_widget_->width() - container_width, 0);

  if (container_->isVisible()) {
    repositionToasts();
  }
}

void ToastManager::onToastClosed() {
  if (auto* toast = qobject_cast<ToastNotification*>(sender())) {
    toasts_.removeOne(toast);
  }

  if (toasts_.isEmpty()) {
    container_->hide();
  } else {
    repositionToasts();
  }
}

void ToastManager::repositionToasts() {
  if (toasts_.isEmpty()) {
    return;
  }

  const int container_width = container_->width();
  const int container_height = container_->height();
  const int toast_width = max_width_;
  const int x = container_width - toast_width - margin_right_;

  // Newest toast (last in the list) sits at the bottom; stack upward from there.
  int current_y = container_height - margin_bottom_;

  for (int i = toasts_.size() - 1; i >= 0; --i) {
    ToastNotification* toast = toasts_[i];

    toast->setFixedWidth(toast_width);
    toast->layout()->activate();  // Recompute height for the fixed width.

    int toast_height = toast->heightForWidth(toast_width);
    if (toast_height < 0) {
      toast_height = toast->sizeHint().height();
    }
    toast_height = qMax(toast_height, toast->minimumHeight());
    toast->setFixedHeight(toast_height);

    current_y -= toast_height;
    toast->updateTargetPosition(QPoint(x, current_y));
    toast->show();

    current_y -= spacing_;
  }

  updateInputRegion();
}

void ToastManager::updateInputRegion() {
  // Constrain hit-testing (and painting) to the toasts themselves so the
  // full-height overlay never blocks interaction with the host widgets beneath
  // it. An empty QRegion CLEARS the mask (full-rect input again) rather than
  // masking everything out — and the empty case does run on every last-toast
  // close, via the QEvent::Hide the container's hide() delivers to the closing
  // toast. That is safe only because the container is hidden at that point and
  // showToast() re-masks (through repositionToasts()) before every show();
  // keep that ordering if you add another path that shows the container.
  QRegion region;
  for (const ToastNotification* toast : toasts_) {
    region += toast->geometry();
  }
  container_->setMask(region);
}

}  // namespace PJ
