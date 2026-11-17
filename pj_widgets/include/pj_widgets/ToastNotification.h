#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QString>
#include <QTimer>

class QHBoxLayout;

namespace PJ {

// A single transient toast: a rounded frame that slides in from the right, shows an optional left icon and a rich-text
// message, and auto-dismisses after `timeout_ms` (or when the user clicks the close button). Positioning and stacking
// are ToastManager's: a ToastNotification never places itself.
//
// The message label is rich text with external links enabled, so an `<a href>` in the message opens in the system
// browser with no extra wiring.
class ToastNotification : public QFrame {
  Q_OBJECT

 public:
  // `icon` is optional; when non-null it is scaled to kIconSize and shown on the left. `timeout_ms == 0` disables
  // auto-dismiss (the toast stays until closed). The countdown starts once the slide-in finishes.
  // ToastManager::showToast always supplies the timeout; the default applies only to a directly constructed toast.
  explicit ToastNotification(
      const QString& message, const QPixmap& icon = QPixmap(), int timeout_ms = 8000, QWidget* parent = nullptr);

  ~ToastNotification() override;

  // Slides in from the right to the position ToastManager already moved it to.
  void showAnimated();

  // Slides out to the right, then emits closed() and deleteLater().
  void hideAnimated();

  QString message() const;
  void setMessage(const QString& message);

  // Scales to kIconSize with the left corners rounded; a null pixmap hides it.
  void setIcon(const QPixmap& icon);

  // Retargets the slide destination, re-aiming an in-flight animation if needed.
  void updateTargetPosition(const QPoint& target);

 signals:
  // Emitted once the close animation has finished.
  void closed();

 private slots:
  void onCloseClicked();
  void onAnimationFinished();
  void onTimeout();

 private:
  void setupUI();
  void setupAnimation();
  // Clips `source` with only its left corners rounded (the right side abuts the message area), returning a
  // transparent-padded pixmap.
  QPixmap createRoundedPixmap(const QPixmap& source, int radius);

  QHBoxLayout* layout_ = nullptr;
  QLabel* icon_label_ = nullptr;
  QLabel* message_label_ = nullptr;
  QPushButton* close_button_ = nullptr;
  QPropertyAnimation* slide_animation_ = nullptr;
  QTimer* timeout_timer_ = nullptr;

  int timeout_ms_ = 0;
  bool is_closing_ = false;

  static constexpr int kBorderRadius = 6;
  static constexpr int kIconSize = 128;
  static constexpr int kAnimationDurationMs = 300;
};

}  // namespace PJ
