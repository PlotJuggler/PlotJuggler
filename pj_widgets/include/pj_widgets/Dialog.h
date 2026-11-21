#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <Qt>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"

class QAbstractButton;
class QEvent;
class QLayout;
class QMouseEvent;
class QShowEvent;
class QSplitter;

namespace Ui {
class Dialog;
}

namespace PJ {

// Base class of app-styled dialogs: a frameless window with a custom title bar (drag to move, close button) above an
// empty content area. Subclasses set the title with setDialogTitle() and populate contentWidget() / contentLayout().
//
//   class FooDialog : public Dialog {
//    public:
//     FooDialog(...) : Dialog(parent) {
//       setDialogTitle(tr("Foo"));
//       ui_->setupUi(contentWidget());  // your .ui's root is a QWidget
//     }
//   };
class Dialog : public QDialog {
  Q_OBJECT
 public:
  explicit Dialog(QWidget* parent = nullptr);
  ~Dialog() override;

  void setDialogTitle(const QString& title);
  [[nodiscard]] QString dialogTitle() const;

  // Shows or hides the title-bar close button. Hide it for dialogs whose only sanctioned exits are their own action
  // buttons (ProgressDialog).
  void setCloseButtonVisible(bool visible);

  // Where a title-bar action goes: Trailing sits left of the close button (window-level actions); Leading sits at the
  // far left, before the title, which then centres itself between the two groups: the place for a panel's own controls
  // (a drawer toggle, a settings gear), so they read as window chrome and not a second header inside the content.
  enum class TitleBarSlot { Trailing, Leading };

  // Inserts a host-supplied action button into the title bar, so a dialog carries window-level actions in its one
  // chrome row. The dialog reparents the button and keeps it sized with the chrome (the close button's extent and icon
  // size, re-applied on every setChromeMetrics). The caller keeps the button's signal wiring.
  void addTitleBarAction(QAbstractButton* button, TitleBarSlot slot = TitleBarSlot::Trailing);

  // Docks a full-height, draggable column at the left of the whole chrome, beside the title bar and not under it,
  // through makeSideDrawerSplitter (QSplitter hides the handle and collapses the column whenever `widget` hides
  // itself): a panel's side drawer reads as its own section, like a sidebar. The dialog reparents `widget`; the caller
  // keeps ownership of its contents and drives its visibility. With a non-empty `settings_key` the column's width is
  // remembered under it, shared with any other presentation of the same drawer that uses the key (a tab). nullptr
  // detaches the current one without deleting it and puts dialogBody straight back into rootLayout.
  void setSideWidget(QWidget* widget, const QString& settings_key = {});
  [[nodiscard]] QWidget* sideWidget() const;

  // Sizes the title-bar chrome (height, close-button extent, icon size, padding) from the shared ChromeMetrics, so it
  // matches the main window. The constructor applies the defaults; hosts that know the live metrics call it again.
  void setChromeMetrics(const ChromeMetrics& metrics);

  // The body subclasses fill, already in the chrome's vertical layout under the title bar.
  [[nodiscard]] QWidget* contentWidget() const;
  [[nodiscard]] QLayout* contentLayout() const;

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  // On first show the content's scroll areas get the overlay pill scrollbars (attachPillScrollbars), so every
  // app-styled dialog scrolls with app-styled bars without per-dialog wiring. First show, not construction: subclasses
  // populate contentWidget() in their own constructor body, after Dialog's.
  void showEvent(QShowEvent* event) override;
  // A live theme switch arrives as a palette or style change: the title-bar icons are re-tinted in the new ink.
  void changeEvent(QEvent* event) override;

 private:
  void applyIcons();
  // The edges (Qt::LeftEdge, RightEdge, TopEdge, BottomEdge, or a corner combination) whose kResizeMargin band the
  // point lies in, or 0 in the interior.
  [[nodiscard]] Qt::Edges edgesAtPoint(const QPoint& pos) const;
  // Applies one manual-drag step: recomputes the geometry from the armed edge set (or move) and the cursor's travel
  // since the press, clamped to the effective min and max sizes.
  void applyManualDrag(const QPoint& global_pos);

  Ui::Dialog* ui_;
  // Host-added title-bar action buttons (addTitleBarAction), kept so setChromeMetrics can re-size them. QPointer: a
  // caller may delete its button; sizing skips the dead entry.
  std::vector<QPointer<QAbstractButton>> title_bar_actions_;
  // Leading buttons sit ahead of a stretch that centres the title; it is inserted with the first of them, so dialogs
  // without leading actions keep a left-aligned title.
  int leading_actions_ = 0;
  // The docked side column (setSideWidget) and the splitter holding it beside dialogBody, so the column is draggable.
  // side_splitter_ is null until the first setSideWidget(non-null); a dialog that never docks one keeps dialogBody in
  // rootLayout. QPointer: the caller may take its widget back or delete it.
  QPointer<QWidget> side_widget_;
  QPointer<QSplitter> side_splitter_;
  // The last metrics applied, so a button added after setChromeMetrics is sized consistently at insert.
  ChromeMetrics chrome_metrics_{};
  // Runs the first-show pill attach once (attach is idempotent, but this avoids re-walking the tree on every show).
  bool scroll_pills_attached_ = false;
  // Manual drag fallback for platforms whose QPA implements neither startSystemResize nor startSystemMove (Qt-wasm):
  // the edge set being resized (0: none), whether a title-bar move drag is active, and the press-time cursor and
  // geometry the drag is computed against.
  Qt::Edges manual_resize_edges_ = {};
  bool manual_move_active_ = false;
  QPoint manual_press_global_;
  QRect manual_press_geometry_;
};

}  // namespace PJ
