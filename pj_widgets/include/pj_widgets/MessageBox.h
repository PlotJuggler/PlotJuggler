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

// Frameless modal dialog that replaces QMessageBox in the app's visual language: heading, body, an optional "Don't show
// again" checkbox and a vertical stack of N labelled buttons. The primary button paints the light_purple to light_blue
// gradient at rest; the others take a solid surface tone keyed off their ButtonRole.
//
// Static helpers cover the common single-OK cases (information, warning, critical) and the multi-button question; the
// instance API gives full control over labels and roles (as QMessageBox::addButton).
//
// A button label too wide for the dialog's bounded width is word-wrapped onto several lines (the button grows taller)
// instead of clipped.
class MessageBox : public QDialog {
  Q_OBJECT
 public:
  // Visual role of each button, set through the `msgbox_role` dynamic property that the QSS keys off.
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

  // Shows or hides the "Don't show again" checkbox between body and buttons. The default label is "Don't show again.";
  // `label` overrides it.
  void setShowDontShowAgain(bool show, const QString& label = {});
  [[nodiscard]] bool dontShowAgainChecked() const;

  // Appends a button to the bottom of the vertical column; the dialog owns the returned pointer. Buttons close the
  // dialog by default; an asynchronous action may opt out, to keep a retry surface visible until its callback arrives.
  QPushButton* addButton(const QString& label, ButtonRole role);
  QPushButton* addButton(const QString& label, ButtonRole role, bool close_on_click);

  // Index of the button the user clicked (in addButton() order), or -1 if the dialog was rejected without one (Esc when
  // no Cancel role exists, or the window manager closing it).
  [[nodiscard]] int clickedIndex() const;

  // --- Static convenience helpers ---
  // Single-OK informational variants: modal on desktop; in the browser they open non-blocking and return at once, since
  // nothing waits on the answer.
  static void information(QWidget* parent, const QString& title, const QString& text);
  static void warning(QWidget* parent, const QString& title, const QString& text);
  static void critical(QWidget* parent, const QString& title, const QString& text);

  // Multi-button question: the index of the clicked button (-1 on reject). A non-null `dont_show_again` shows the
  // checkbox above the buttons and receives its checked state.
  static int question(
      QWidget* parent, const QString& title, const QString& text, std::initializer_list<ButtonSpec> buttons,
      bool* dont_show_again = nullptr);

  // Continuation form of question(): `on_done(choice, dont_show_again)` receives the clicked index (-1 on reject) and
  // the checkbox state, shown only when `show_dont_show_again` is set. On desktop the dialog is modal and `on_done`
  // runs before this returns; in the browser, whose Asyncify-free build cannot run exec()'s nested event loop, the
  // dialog opens non-blocking and `on_done` runs once the user answers. Do not rely on either ordering, and anything
  // `on_done` captures must outlive the dialog (parented to `parent`, so capturing `parent` is safe). `on_shown`, when
  // given, receives the dialog just before it is shown (on desktop, before the modal loop starts): a caller that may
  // have to withdraw the question keeps a QPointer to it and reject()s, which `on_done` reports as -1.
  static void questionAsync(
      QWidget* parent, const QString& title, const QString& text, std::vector<ButtonSpec> buttons,
      std::function<void(int choice, bool dont_show_again)> on_done, bool show_dont_show_again = false,
      const std::function<void(QDialog& dialog)>& on_shown = {});

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  // Wraps over-long button labels at QEvent::Polish, after the QSS font is applied and before the dialog auto-sizes, so
  // a long label breaks across lines instead of clipping against the bounded width.
  bool event(QEvent* event) override;
  // Two first-show corrections (see the .cpp): grow to fit the word-wrapped body, whose wrapped height is missing from
  // sizeHint and hidden by the styled card's broken heightForWidth propagation, then re-center; and pin the dialog to
  // the exact height its content needs at the shown width, so the QVBoxLayout never compresses the gaps between
  // buttons.
  void showEvent(QShowEvent* event) override;

 private:
  // Re-wraps every button's label to the dialog's max content width, given the polished button font; labels that
  // already fit are left untouched.
  void rewrapButtonLabels();

  bool size_finalized_ = false;  // showEvent body-grow + re-center runs once
  QLabel* title_label_;
  QLabel* body_label_;
  QCheckBox* dont_show_again_;
  QVBoxLayout* button_column_;
  QList<QPushButton*> buttons_;
  QList<ButtonRole> button_roles_;
  // The original (unwrapped) button labels, parallel to buttons_: the source for re-wrapping, since a QPushButton's own
  // text may carry soft '\n'.
  QStringList button_labels_;
  int clicked_index_ = -1;
};

}  // namespace PJ
