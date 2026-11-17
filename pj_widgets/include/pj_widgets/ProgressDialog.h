#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_widgets/Dialog.h"

class QKeyEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

namespace PJ {

// App-styled modal progress dialog with up to two "stop" buttons, on the frameless Dialog chrome so it matches
// MessageBox and FileDialog instead of the native QProgressDialog look.
//
// Domain-neutral: it reports which button was pressed (Action::Primary / Secondary) and leaves the meaning to the
// caller (one caller maps Primary to keep-partial and Secondary to discard); the dialog knows nothing about datasets,
// flushing or feature policy.
//
// Driving model: the owner runs its work synchronously on the GUI thread and pumps the event loop
// (QCoreApplication::processEvents) between progress steps, polling action() to learn whether the user asked to stop;
// stopRequested() is emitted too. The chosen action is sticky until resetAction(); completion paths just hide() the
// dialog (there is no QProgressDialog-style reset()).
//
// No close affordance: the base close button is hidden and Esc is swallowed, so the two action buttons are the only
// exits: a progress prompt must not be dismissed mid-operation.
class ProgressDialog : public Dialog {
  Q_OBJECT
 public:
  // Which stop button the user pressed, or None (the operation ran to completion).
  enum class Action { kNone, kPrimary, kSecondary };

  explicit ProgressDialog(QWidget* parent = nullptr);
  ~ProgressDialog() override;

  // Configures a stop button; an empty `label` hides it. `icon_path` is an SVG resource path, recolored for the active
  // theme (empty: no icon). Primary sits left of Secondary.
  void setPrimaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});
  void setSecondaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});

  // Progress-bar range and value; setRange(0, 0) is an indeterminate (busy) bar, as QProgressBar.
  void setRange(int minimum, int maximum);
  void setValue(int value);
  [[nodiscard]] int maximum() const;

  // Body message above the bar (the title-bar text is Dialog::setDialogTitle()); an empty string collapses the row.
  void setMessage(const QString& text);

  // Shows or hides both stop buttons together (a non-cancellable phase); a button with no configured label stays
  // hidden.
  void setStopButtonsVisible(bool visible);

  // The button pressed so far this run; sticky until resetAction().
  [[nodiscard]] Action action() const {
    return action_;
  }
  void resetAction() {
    action_ = Action::kNone;
  }

 signals:
  // Emitted once when the user presses a stop button.
  void stopRequested(Action action);

 protected:
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void configureButton(QPushButton* button, const QString& label, const QString& icon_path, const QString& tooltip);

  QLabel* message_ = nullptr;
  QProgressBar* bar_ = nullptr;
  QPushButton* primary_ = nullptr;
  QPushButton* secondary_ = nullptr;
  Action action_ = Action::kNone;
};

}  // namespace PJ
