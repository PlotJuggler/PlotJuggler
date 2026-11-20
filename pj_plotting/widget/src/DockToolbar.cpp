// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DockToolbar.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <algorithm>

#include "pj_widgets/SvgUtil.h"
#include "ui_DockToolbar.h"

namespace PJ {

namespace {
void setButtonIcon(QPushButton* button, const QIcon& icon) {
  button->setIcon(icon);
  button->setText("");
}
}  // namespace

DockToolbar::DockToolbar(ads::CDockWidget* parent) : QWidget(parent), parent_dock_(parent), ui_(new Ui::DockToolbar) {
  ui_->setupUi(this);
  // A QWidget subclass without its own paintEvent paints a QSS background only with this attribute.
  setAttribute(Qt::WA_StyledBackground, true);

  onStylesheetChanged(currentTheme());

  ui_->buttonFullscreen->setVisible(false);
  ui_->buttonSplitHorizontal->setVisible(false);
  ui_->buttonSplitVertical->setVisible(false);

  setMouseTracking(true);
  ui_->widgetButtons->setMouseTracking(true);

  ui_->label->installEventFilter(this);

  // Inline rename editor: hidden until the label is double-clicked. Enter
  // commits via returnPressed; Escape / focus-out revert (handled in
  // eventFilter, since the line edit is the watched object). The editor grows
  // with its text, so keep its width in sync as the user types.
  ui_->lineEditRename->installEventFilter(this);
  connect(ui_->lineEditRename, &QLineEdit::returnPressed, this, &DockToolbar::commitRename);
  connect(ui_->lineEditRename, &QLineEdit::textChanged, this, &DockToolbar::updateRenameEditWidth);

  // Hint that the title bar is draggable: an open-hand "grab" cursor over the
  // bar. Children inherit it, so override the clickable buttons (pointing hand)
  // and the rename editor (text I-beam) to keep their own affordances.
  setCursor(Qt::OpenHandCursor);
  ui_->lineEditRename->setCursor(Qt::IBeamCursor);
  for (QPushButton* button :
       {ui_->buttonSplitHorizontal, ui_->buttonSplitVertical, ui_->buttonFullscreen, ui_->buttonClose}) {
    button->setCursor(Qt::PointingHandCursor);
  }
}

DockToolbar::~DockToolbar() {
  delete ui_;
}

QLabel* DockToolbar::label() {
  return ui_->label;
}
QPushButton* DockToolbar::buttonFullscreen() {
  return ui_->buttonFullscreen;
}
QPushButton* DockToolbar::buttonClose() {
  return ui_->buttonClose;
}
QPushButton* DockToolbar::buttonSplitHorizontal() {
  return ui_->buttonSplitHorizontal;
}
QPushButton* DockToolbar::buttonSplitVertical() {
  return ui_->buttonSplitVertical;
}

void DockToolbar::setFullscreen(bool on) {
  fullscreen_mode_ = on;
  setButtonIcon(ui_->buttonFullscreen, fullscreen_mode_ ? collapse_icon_ : expand_icon_);
  ui_->buttonClose->setHidden(fullscreen_mode_);
  if (fullscreen_mode_) {
    ui_->buttonSplitHorizontal->setVisible(false);
    ui_->buttonSplitVertical->setVisible(false);
  }
}

void DockToolbar::mousePressEvent(QMouseEvent* ev) {
  if (auto* area = parent_dock_->dockAreaWidget()) {
    // Forward with synthetic pos (0, 0). CFloatingDragPreview positions its
    // top-left at QCursor::pos() - DragStartMousePosition, so a (0, 0) start
    // makes the preview track the cursor directly instead of being offset by
    // wherever the user happened to click along the wide toolbar.
    QMouseEvent fwd(
        QEvent::MouseButtonPress, QPointF(0, 0), ev->globalPosition(), ev->button(), ev->buttons(), ev->modifiers());
    QCoreApplication::sendEvent(area->titleBar(), &fwd);
    ev->setAccepted(fwd.isAccepted());
  }
}

void DockToolbar::mouseReleaseEvent(QMouseEvent* ev) {
  if (auto* area = parent_dock_->dockAreaWidget()) {
    QCoreApplication::sendEvent(area->titleBar(), ev);
  }
}

void DockToolbar::mouseMoveEvent(QMouseEvent* ev) {
  ui_->buttonFullscreen->setVisible(true);
  ui_->buttonSplitHorizontal->setVisible(!fullscreen_mode_);
  ui_->buttonSplitVertical->setVisible(!fullscreen_mode_);
  if (auto* area = parent_dock_->dockAreaWidget()) {
    QCoreApplication::sendEvent(area->titleBar(), ev);
  }
  ev->accept();
  QWidget::mouseMoveEvent(ev);
}

void DockToolbar::enterEvent(QEnterEvent* ev) {
  ui_->buttonFullscreen->setVisible(true);
  ui_->buttonSplitHorizontal->setVisible(!fullscreen_mode_);
  ui_->buttonSplitVertical->setVisible(!fullscreen_mode_);
  ev->accept();
  QWidget::enterEvent(ev);
}

void DockToolbar::leaveEvent(QEvent* ev) {
  ui_->buttonFullscreen->setVisible(fullscreen_mode_);
  ui_->buttonSplitHorizontal->setVisible(false);
  ui_->buttonSplitVertical->setVisible(false);
  QWidget::leaveEvent(ev);
}

bool DockToolbar::eventFilter(QObject* object, QEvent* event) {
  if (object == ui_->label && event->type() == QEvent::MouseButtonDblClick) {
    enterRenameMode();
    return true;
  }
  if (object == ui_->lineEditRename) {
    // Match the tab-rename UX: focus loss reverts; only Enter (returnPressed)
    // commits. Escape reverts explicitly.
    if (event->type() == QEvent::FocusOut) {
      cancelRename();
    } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
      cancelRename();
      return true;
    }
  }
  return QObject::eventFilter(object, event);
}

void DockToolbar::enterRenameMode() {
  if (rename_active_) {
    return;
  }
  rename_active_ = true;
  ui_->lineEditRename->setText(ui_->label->text());
  updateRenameEditWidth();  // size to the seeded text (>= the 300 px default)
  ui_->label->hide();
  ui_->lineEditRename->show();
  ui_->lineEditRename->setFocus(Qt::OtherFocusReason);
  ui_->lineEditRename->setCursorPosition(ui_->lineEditRename->text().length());
}

void DockToolbar::commitRename() {
  if (!rename_active_) {
    return;
  }
  // Clear the flag first: hiding the focused line edit fires FocusOut, which
  // would otherwise re-enter cancelRename() and double-process the swap.
  rename_active_ = false;
  const QString new_name = ui_->lineEditRename->text();
  ui_->lineEditRename->hide();
  ui_->label->setText(new_name);
  ui_->label->show();
  emit titleChanged(new_name);
}

void DockToolbar::cancelRename() {
  if (!rename_active_) {
    return;
  }
  rename_active_ = false;
  ui_->lineEditRename->hide();
  ui_->label->show();
}

void DockToolbar::updateRenameEditWidth() {
  // Start at a compact 300 px and grow only when the text would not fit, so the
  // editor doesn't span the whole (often very wide) title bar. Cap the growth at
  // the space left after the fixed chrome — left spacer (40) + buttons (~90) +
  // close (24) + margins ≈ 160 px — so a long name can't push under the buttons.
  constexpr int kDefaultWidth = 300;
  constexpr int kChromeReserve = 160;
  constexpr int kTextPadding = 24;  // frame borders, text margins, cursor room

  auto* edit = ui_->lineEditRename;
  const int text_width = edit->fontMetrics().horizontalAdvance(edit->text()) + kTextPadding;
  const int max_width = std::max(kDefaultWidth, width() - kChromeReserve);
  edit->setFixedWidth(std::clamp(text_width, kDefaultWidth, max_width));
}

void DockToolbar::onStylesheetChanged(QString theme) {
  expand_icon_ = loadSvg(":/resources/svg/expand.svg", theme);
  collapse_icon_ = loadSvg(":/resources/svg/collapse.svg", theme);
  setButtonIcon(ui_->buttonFullscreen, fullscreen_mode_ ? collapse_icon_ : expand_icon_);
  setButtonIcon(ui_->buttonClose, loadSvg(":/resources/svg/close-button.svg", theme));
  setButtonIcon(ui_->buttonSplitHorizontal, loadSvg(":/resources/svg/add_column.svg", theme));
  setButtonIcon(ui_->buttonSplitVertical, loadSvg(":/resources/svg/add_row.svg", theme));
}

}  // namespace PJ
