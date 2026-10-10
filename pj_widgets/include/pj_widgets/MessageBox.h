#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <initializer_list>
#include <vector>

class QCheckBox;
class QEvent;
class QKeyEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QVBoxLayout;

namespace PJ {

// Frameless modal dialog that replaces QMessageBox with the app's visual
// language: heading + body + optional "Don't show again" checkbox +
// vertical stack of N labelled buttons. The primary button paints the
// light_purple → light_blue gradient at rest; other buttons take a solid
// surface tone keyed off their ButtonRole.
//
// Static convenience methods cover the common single-OK cases
// (information / warning / critical) plus the multi-button question.
// The instance API is for callers that need full control over labels and
// roles (matches QMessageBox::addButton).
//
// A button label too wide for the dialog's bounded width is word-wrapped
// onto multiple lines (the button grows taller) rather than clipped.
class MessageBox : public QDialog {
  Q_OBJECT
 public:
  // Visual role applied to each button. The dialog sets this via the
  // `msgbox_role` dynamic property; the QSS keys off it.
  enum ButtonRole {
    kPrimaryRole,      // Gradient at rest. The recommended affirmative action.
    kNeutralRole,      // Solid neutral surface.
    kDestructiveRole,  // Neutral surface with purple text; light_purple hover.
    kCancelRole,       // Subtler surface. Esc / window-close return this button's index.
  };

  struct ButtonSpec {
    QString label;
    ButtonRole role;
  };

  explicit MessageBox(QWidget* parent = nullptr);
  ~MessageBox() override;

  void setTitle(const QString& title);
  void setText(const QString& text);

  // Show or hide the "Don't show again" checkbox between body and buttons.
  // Default label is "Don't show again." — override via `label` if a more
  // specific phrase is wanted.
  void setShowDontShowAgain(bool show, const QString& label = {});
  [[nodiscard]] bool dontShowAgainChecked() const;

  // Appends a button to the bottom of the vertical column. The dialog owns the
  // returned pointer. Buttons close the dialog by default; an asynchronous
  // action may opt out so it can keep a retry surface visible until its callback
  // arrives.
  QPushButton* addButton(const QString& label, ButtonRole role);
  QPushButton* addButton(const QString& label, ButtonRole role, bool close_on_click);

  // Index of the button the user clicked (in addButton() order), or -1
  // if the dialog was rejected without picking one (Esc when no Cancel
  // role exists, or the window manager closing the dialog).
  [[nodiscard]] int clickedIndex() const;

  // --- Static convenience helpers ---
  // Single-OK informational variants. Modal on desktop; in the browser they
  // open non-blocking and return at once, since nothing waits on the answer.
  static void information(QWidget* parent, const QString& title, const QString& text);
  static void warning(QWidget* parent, const QString& title, const QString& text);
  static void critical(QWidget* parent, const QString& title, const QString& text);

  // Multi-button question. Returns the index of the clicked button (or -1
  // on reject). If `dont_show_again` is non-null, the checkbox is shown
  // above the buttons and its checked state is written back through it.
  static int question(
      QWidget* parent, const QString& title, const QString& text, std::initializer_list<ButtonSpec> buttons,
      bool* dont_show_again = nullptr);

  // Continuation form of question(): `on_done(choice, dont_show_again)` receives
  // the clicked index (or -1 on reject) and the checkbox state, which is only
  // shown when `show_dont_show_again` is set. On desktop the dialog is modal and
  // `on_done` runs before this returns; in the browser, whose Asyncify-free
  // build cannot run exec()'s nested event loop, the dialog opens
  // non-blocking and `on_done` runs once the user answers. Callers must not
  // rely on either ordering, and anything `on_done` captures must outlive the
  // dialog (it is parented to `parent`, so capture `parent` itself safely).
  static void questionAsync(
      QWidget* parent, const QString& title, const QString& text, std::vector<ButtonSpec> buttons,
      std::function<void(int choice, bool dont_show_again)> on_done, bool show_dont_show_again = false);

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  // Wraps over-long button labels at QEvent::Polish — after the QSS font is
  // applied but before the dialog auto-sizes — so a long label breaks across
  // lines instead of being clipped against the dialog's bounded width.
  bool event(QEvent* event) override;
  // Two first-show corrections (see the .cpp): (1) grow to fit the word-wrapped
  // body, whose wrapped height is not reflected in sizeHint and is hidden by the
  // styled card's broken heightForWidth propagation, then re-center; (2) pin the
  // dialog to the exact height its content needs at the shown width so the
  // QVBoxLayout never compresses the gaps between stacked buttons.
  void showEvent(QShowEvent* event) override;

 private:
  // Re-wrap every button's label to fit the dialog's max content width, given
  // the now-polished button font. Short labels that already fit are left
  // untouched (rendered exactly as before).
  void rewrapButtonLabels();

  bool size_finalized_ = false;  // showEvent body-grow + re-center runs once
  QLabel* title_label_;
  QLabel* body_label_;
  QCheckBox* dont_show_again_;
  QVBoxLayout* button_column_;
  QList<QPushButton*> buttons_;
  QList<ButtonRole> button_roles_;
  // Original (un-wrapped) button labels, parallel to buttons_. The source of
  // truth for re-wrapping; the QPushButton's own text may carry soft '\n'.
  QStringList button_labels_;
  int clicked_index_ = -1;
};

}  // namespace PJ
