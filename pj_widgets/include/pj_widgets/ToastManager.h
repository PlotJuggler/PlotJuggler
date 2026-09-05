#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QObject>
#include <QPixmap>
#include <QWidget>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

class ToastNotification;

// Owns and lays out a bottom-right stack of ToastNotification widgets over a
// host widget. Toasts are children of an internal transparent container sized
// to the host; the newest toast sits at the bottom and older ones stack upward.
//
// The container spans the host's full height only to give the slide/stack
// animations a stable coordinate system: its input (and paint) region is
// masked down to the union of the toast rectangles, so everywhere outside the
// toasts clicks fall through to whatever the host shows beneath the overlay.
//
// The host must forward its resize (and reparent, if any) to updatePosition()
// so the stack keeps hugging the corner.
class ToastManager : public QObject {
  Q_OBJECT

 public:
  explicit ToastManager(QWidget* parent_widget);
  ~ToastManager() override;

  // Tracks toast Move/Resize/Show/Hide to keep the container's input mask
  // glued to the toasts while the slide animations move them.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Auto-dismiss default for showToast() (0 disables it).
  static constexpr int kDefaultTimeoutMs = 8000;

  // Show a toast. `icon` is optional; `timeout_ms == 0` disables auto-dismiss.
  // The message may contain rich text, including clickable `<a href>` links.
  void showToast(const QString& message, const QPixmap& icon = QPixmap(), int timeout_ms = kDefaultTimeoutMs);

  // Re-fit the container to the host size; call from the host's resizeEvent.
  void updatePosition();

 private slots:
  void onToastClosed();

 private:
  void repositionToasts();
  void updateInputRegion();

  QWidget* parent_widget_ = nullptr;
  QWidget* container_ = nullptr;
  QList<ToastNotification*> toasts_;

  int margin_right_ = theme::space(theme::Space::Section);
  int margin_bottom_ = theme::space(theme::Space::Section);
  int spacing_ = theme::space(theme::Space::Comfortable);
  int max_width_ = 400;
};

}  // namespace PJ
