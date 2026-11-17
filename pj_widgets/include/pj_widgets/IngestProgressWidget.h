#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

class QToolButton;

namespace PJ {

class ProgressBar;

// Non-modal inline progress strip sized for a title bar: a PJ::ProgressBar whose centred caption carries the status
// text (a filename, plus an optional "N/M" counter), followed by up to two action buttons. The title-bar counterpart of
// ProgressDialog: the app stays interactive while it shows.
//
// Domain-neutral: it knows nothing about files or loading. It reports which button was pressed (Action::kPrimary /
// kSecondary, as ProgressDialog) through actionRequested() and a latched lastAction(), and leaves their meaning to the
// owner. Auto-hide timing (show delay, linger after completion) is owner policy: the widget shows and hides only on
// command, through setActive().
class IngestProgressWidget : public QWidget {
  Q_OBJECT
 public:
  // Which action button was pressed since the last setActive(true), or kNone.
  enum class Action { kNone, kPrimary, kSecondary };

  explicit IngestProgressWidget(QWidget* parent = nullptr);

  // Leading status text (the current filename), shown in the bar's caption.
  void setTitle(const QString& label);
  // Short progress counter such as "2/5", appended to the caption; empty clears it.
  void setCounterText(const QString& text);
  // Progress-bar bounds and value; setRange(0, 0) is an indeterminate (busy) bar, as QProgressBar.
  void setRange(int minimum, int maximum);
  void setValue(int value);
  // Configures an action button (Primary sits left of Secondary). An empty `label` and empty `icon_path` hide it; an
  // icon with an empty label is an icon-only button: pass `tooltip` to explain it. `icon_path` is an SVG resource path,
  // recolored for the active theme.
  void setPrimaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});
  void setSecondaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});

  // Shows or hides the strip; hiding also resets the action latch, so the next run starts at Action::kNone.
  void setActive(bool active);

  // The button pressed since the last setActive(true); latched, reset by setActive(false).
  [[nodiscard]] Action lastAction() const {
    return last_action_;
  }

 signals:
  // Emitted once per click of a configured action button.
  void actionRequested(Action action);

 private:
  void configureButton(QToolButton* button, const QString& label, const QString& icon_path, const QString& tooltip);
  // Recomposes the bar's caption from the title and counter text.
  void updateCaption();

  ProgressBar* bar_ = nullptr;
  QToolButton* primary_ = nullptr;
  QToolButton* secondary_ = nullptr;
  QString title_text_;
  QString counter_text_;
  Action last_action_ = Action::kNone;
};

}  // namespace PJ
