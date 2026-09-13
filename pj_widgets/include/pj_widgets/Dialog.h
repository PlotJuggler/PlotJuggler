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

// Base class for app-styled dialogs: frameless window with a custom
// title bar (drag-to-move + close button) on top and an empty content
// area below. Subclasses set their title via setDialogTitle() and
// populate contentWidget() / contentLayout().
//
// Pattern:
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

  // Show or hide the title-bar close (✕) button. Hide it for dialogs whose
  // only sanctioned exits are their own action buttons (e.g. ProgressDialog).
  void setCloseButtonVisible(bool visible);

  // Where a title-bar action goes: Trailing sits left of the ✕ (window-level
  // actions); Leading sits at the far left, before the title, which then
  // centres itself between the two groups — the place for a panel's own
  // controls (a drawer toggle, a settings gear) so they read as part of the
  // window chrome rather than as a second header inside the content.
  enum class TitleBarSlot { Trailing, Leading };

  // Insert a host-supplied action button into the title bar, so a dialog can
  // carry window-level actions in its one chrome row instead of stacking a
  // second header inside the content. The dialog reparents the button and
  // keeps it sized with the chrome (same extent/icon size as the close
  // button, re-applied on every setChromeMetrics). The caller keeps the
  // button's signal wiring.
  void addTitleBarAction(QAbstractButton* button, TitleBarSlot slot = TitleBarSlot::Trailing);

  // Dock a full-height, draggable column at the LEFT of the whole chrome —
  // beside the title bar, not under it — via makeSideDrawerSplitter
  // (QSplitter auto-hides the handle and collapses the column's space
  // whenever `widget` hides itself). A panel's side drawer goes here so it
  // reads as its own section of the window, the way a sidebar does. The
  // dialog reparents `widget`; the caller keeps ownership of its contents and
  // drives visibility. With a non-empty `settings_key` the column's width is
  // remembered under it (shared with whatever other presentation of the same
  // drawer uses the same key, e.g. a tab). Passing nullptr detaches the
  // current one (without deleting it) and restores dialogBody straight into
  // rootLayout, exactly as if setSideWidget had never been called.
  void setSideWidget(QWidget* widget, const QString& settings_key = {});
  [[nodiscard]] QWidget* sideWidget() const;

  // Size the title-bar chrome (height, close-button extent, icon size, padding)
  // from the app's shared ChromeMetrics, so this dialog's chrome matches the main
  // window exactly. Called with defaults in the constructor; hosts that know the
  // live metrics (the dialog host, app) call it again with the current values.
  void setChromeMetrics(const ChromeMetrics& metrics);

  // The body widget subclasses fill. Already in the chrome's vertical
  // layout under the title bar.
  [[nodiscard]] QWidget* contentWidget() const;
  [[nodiscard]] QLayout* contentLayout() const;

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  // On first show, give the content's scroll areas the canonical overlay pill
  // scrollbars (via attachPillScrollbars) so every app-styled dialog scrolls
  // with app-styled bars — no per-dialog wiring. First show, not construction,
  // because subclasses populate contentWidget() in their own constructor body
  // after Dialog's runs.
  void showEvent(QShowEvent* event) override;

 private:
  void applyIcons();
  // Returns the edges (Qt::LeftEdge / RightEdge / TopEdge / BottomEdge,
  // or a corner combination) the point lies inside the kResizeMargin
  // band of, or 0 when the point is in the interior.
  [[nodiscard]] Qt::Edges edgesAtPoint(const QPoint& pos) const;
  // Apply one manual-drag step: recompute geometry from the armed edge set
  // (or move) and the cursor's travel since the press, clamped to the
  // effective min/max sizes.
  void applyManualDrag(const QPoint& global_pos);

  Ui::Dialog* ui_;
  // Host-added title-bar action buttons (addTitleBarAction), kept so
  // setChromeMetrics can re-size them with the rest of the chrome. QPointer:
  // a caller may delete its button; sizing just skips the dead entry.
  std::vector<QPointer<QAbstractButton>> title_bar_actions_;
  // Leading buttons sit ahead of a stretch that centres the title; the stretch
  // is inserted with the first of them, so dialogs without leading actions
  // keep their left-aligned title.
  int leading_actions_ = 0;
  // The docked side column (setSideWidget) and the splitter that holds it
  // beside dialogBody, so the column is draggable instead of a fixed width.
  // side_splitter_ is null until the first setSideWidget(widget!=nullptr) —
  // a dialog that never docks a side widget keeps dialogBody straight in
  // rootLayout, exactly as before. QPointer: the caller may take its widget
  // back or delete it.
  QPointer<QWidget> side_widget_;
  QPointer<QSplitter> side_splitter_;
  // Last metrics applied, so a button added after setChromeMetrics is sized
  // consistently at insert time.
  ChromeMetrics chrome_metrics_{};
  // One-shot guard so the first-show pill attach runs once (attach itself is
  // idempotent, but this avoids re-walking the tree on every show).
  bool scroll_pills_attached_ = false;
  // Manual drag fallback for platforms whose QPA implements neither
  // startSystemResize nor startSystemMove (Qt-wasm): the edge set being
  // resized (0 = none), whether a title-bar move drag is active, and the
  // press-time cursor/geometry the drag is computed against.
  Qt::Edges manual_resize_edges_ = {};
  bool manual_move_active_ = false;
  QPoint manual_press_global_;
  QRect manual_press_geometry_;
};

}  // namespace PJ
