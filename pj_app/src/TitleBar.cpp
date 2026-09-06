// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TitleBar.h"

#include <QAbstractButton>
#include <QAction>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWindow>
#include <vector>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_TitleBar.h"
using namespace Qt::StringLiterals;

namespace PJ {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent), ui_(new Ui::TitleBar) {
  // QSS `background:` only paints on a custom QWidget subclass when
  // WA_StyledBackground is set. Built-in widgets (QPushButton etc.) do
  // this internally; bare QWidget subclasses must opt in or the rule is
  // a no-op and the title bar shows whatever is painted behind it.
  setAttribute(Qt::WA_StyledBackground, true);

  ui_->setupUi(this);
  // The center region replaces the old horizontalSpacer; its empty area is a
  // window-drag handle (see isOnMoveHandle). The container stays OPAQUE so an
  // interactive center widget (setCenterWidget — e.g. the ingest stop buttons)
  // receives its own clicks: WA_TransparentForMouseEvents here would make
  // childAt() skip the whole subtree, routing button clicks to the drag handler.
  // Hard-pin so QMainWindow::setMenuWidget can't size us via sizeHint
  // and leave a ghost strip of titlebar-background gray below the
  // buttons.
  //
  // Height tracks the global icon-metrics setting: button height equals
  // icon_size + icon_padding, and the title-bar itself is +1 to leave
  // room for the QSS `border-bottom: 1px` that draws inside our
  // geometry. Initial values are the defaults; MainWindow re-pushes the
  // saved metrics via iconMetricsChanged after the connection is wired.
  applyIconMetrics();

  // Traditional menus, owned here so MainWindow can populate them.
  // objectName "PJMenu" keeps the existing QMenu#PJMenu QSS applying to
  // the popups (the id+type selector outranks the cascading
  // `QWidget { background: transparent }` rule that otherwise wins for
  // popups when QSS is delivered via qApp->setStyleSheet).
  // setNativeMenuBar(false) forces in-window rendering even on desktops
  // with a global menu bar — the menubar is part of our custom chrome,
  // not the platform's.
  file_menu_ = new QMenu(tr("&File"), this);
  toolbox_menu_ = new QMenu(tr("&Toolbox"), this);
  help_menu_ = new QMenu(tr("&Help"), this);
  ui_->menuBar->setNativeMenuBar(false);
  for (QMenu* menu : {file_menu_, toolbox_menu_, help_menu_}) {
    menu->setObjectName(u"PJMenu"_s);
    ui_->menuBar->addMenu(menu);
  }
  applyIcons(currentTheme());
  connect(ui_->buttonNotifications, &QToolButton::clicked, this, &TitleBar::notificationsClicked);
  connect(ui_->buttonExtensionUpdate, &QToolButton::clicked, this, &TitleBar::extensionUpdateRequested);
  connect(ui_->buttonMinimize, &QToolButton::clicked, this, [this]() {
    if (auto* w = window()) {
      w->showMinimized();
    }
  });
  connect(ui_->buttonMaximize, &QToolButton::clicked, this, &TitleBar::onMaximizeClicked);
  connect(ui_->buttonClose, &QToolButton::clicked, this, [this]() {
    if (auto* w = window()) {
      w->close();
    }
  });

#ifdef PJ_TARGET_WASM
  // A browser tab has no host window to minimize/maximize/close — the tab's own
  // chrome owns that — so the window controls are meaningless here. Hide them.
  ui_->buttonMinimize->hide();
  ui_->buttonMaximize->hide();
  ui_->buttonClose->hide();
#endif
}

TitleBar::~TitleBar() {
  delete ui_;
}

QMenu* TitleBar::fileMenu() const {
  return file_menu_;
}

QMenu* TitleBar::toolboxMenu() const {
  return toolbox_menu_;
}

QMenu* TitleBar::helpMenu() const {
  return help_menu_;
}

void TitleBar::addRightClusterWidget(QWidget* widget) {
  if (widget == nullptr) {
    return;
  }
  // Appends to the bell's group so the added widgets share its tight
  // intra-group spacing; the spacer + outer layout spacing keep the
  // group visually separate from the window controls. Re-apply the
  // metrics so the new widget is sized to the chrome extent immediately
  // (callers add widgets after the initial metrics broadcast).
  ui_->rightClusterLayout->addWidget(widget);
  applyIconMetrics();
}

void TitleBar::setCenterWidget(QWidget* widget) {
  if (widget == center_widget_) {
    return;
  }
  // Drop the prior widget without deleting it — the caller owns its lifetime.
  if (center_widget_ != nullptr) {
    ui_->centerLayout->removeWidget(center_widget_);
    center_widget_->setParent(nullptr);
  }
  center_widget_ = widget;
  if (widget != nullptr) {
    // Insert between centerLeftSpacer (index 0) and centerRightSpacer with
    // stretch 0, so the widget keeps its compact size and the spacers center it.
    ui_->centerLayout->insertWidget(1, widget, /*stretch=*/0);
    applyIconMetrics();  // size the widget's tool buttons to match the chrome icons
  }
}

void TitleBar::setExtensionUpdateCount(int count) {
  if (count <= 0) {
    ui_->buttonExtensionUpdate->hide();
    return;
  }
  ui_->buttonExtensionUpdate->setToolTip(tr("%n extension update(s) available", nullptr, count));
  ui_->buttonExtensionUpdate->show();
}

void TitleBar::setUnseenError(bool unseen) {
  if (unseen_error_ == unseen) {
    return;
  }
  unseen_error_ = unseen;
  applyIcons(currentTheme());
}

void TitleBar::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void TitleBar::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIconMetrics();
}

void TitleBar::applyIconMetrics() {
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  // Canonical chrome height (the QSS bottom border draws inside our geometry, so
  // the inner content rect is bar_height - 1). Dialog chrome uses the same call.
  const int bar_height = chrome_metrics_.titleBarHeight();
  setMinimumHeight(bar_height);
  setMaximumHeight(bar_height);
  setFixedHeight(bar_height);
  if (auto* layout = ui_->horizontalLayout) {
    layout->setContentsMargins(
        chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
        chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }

  // Square chrome buttons — fixed extent on both axes. The right cluster
  // is sized as a group (bell + every widget added via
  // addRightClusterWidget, e.g. the panel-toggle buttons) so all of them
  // share the bell's extent and sit centered in the bar like it does.
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  std::vector<QAbstractButton*> square_buttons{ui_->buttonMinimize, ui_->buttonMaximize, ui_->buttonClose};
  for (int i = 0; i < ui_->rightClusterLayout->count(); ++i) {
    if (auto* btn = qobject_cast<QAbstractButton*>(ui_->rightClusterLayout->itemAt(i)->widget())) {
      square_buttons.push_back(btn);
    }
  }
  for (QAbstractButton* btn : square_buttons) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  // The "Update" button keeps its natural (text) width but matches the square
  // chrome buttons' height, so it fills the bar like its icon neighbours. The
  // height is pinned here — not in QSS — because the two must not both constrain
  // it (a QSS max-height would cap it below button_extent).
  ui_->buttonExtensionUpdate->setMinimumHeight(button_extent);
  ui_->buttonExtensionUpdate->setMaximumHeight(button_extent);
  ui_->appIcon->setMinimumHeight(button_extent);
  ui_->appIcon->setMaximumHeight(button_extent);
  ui_->appIcon->setIconSize(icon_sz);
  // The menubar keeps its natural (content) height, capped at the chrome
  // extent, and is vertically centered by the layout — stretching it to
  // the row height left its item highlights hanging from the top edge.
  ui_->menuBar->setMaximumHeight(button_extent);

  // Tool buttons hosted in the center widget (the ingest stop button) hug their
  // icon exactly — no surrounding chrome padding — so they sit flush against the
  // neighbouring progress bar instead of floating inside an oversized box. They
  // are intentionally tighter than the square chrome buttons on the right, whose
  // extra padding gives the window controls a larger hit target.
  if (center_widget_ != nullptr) {
    for (QToolButton* btn : center_widget_->findChildren<QToolButton*>()) {
      btn->setMinimumSize(icon_sz);
      btn->setMaximumSize(icon_sz);
      btn->setIconSize(icon_sz);
    }
  }
}

void TitleBar::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) {
    ui_->buttonMaximize->setToolTip(window()->isMaximized() ? tr("Restore") : tr("Maximize"));
  }
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !isOnMoveHandle(event->position().toPoint())) {
    QWidget::mousePressEvent(event);
    return;
  }
  if (auto* handle = window()->windowHandle()) {
    handle->startSystemMove();
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !isOnMoveHandle(event->position().toPoint())) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }
  onMaximizeClicked();
  event->accept();
}

void TitleBar::onMaximizeClicked() {
  auto* w = window();
  if (w == nullptr) {
    return;
  }
  if (w->isMaximized()) {
    w->showNormal();
  } else {
    w->showMaximized();
  }
}

bool TitleBar::isOnMoveHandle(const QPoint& pos) const {
  // Drag on raw bar background, the non-interactive app icon, and the empty
  // center region (the opaque centerContainer or the center widget's own area).
  // A click landing on an interactive child — a tool button, the menubar, the
  // ingest progress bar — goes to that child instead.
  QWidget* hit = childAt(pos);
  return hit == nullptr || hit == ui_->appIcon || hit == ui_->centerContainer || hit == center_widget_;
}

void TitleBar::applyIcons(const QString& theme) {
  ui_->appIcon->setIcon(loadSvg(":/resources/svg/plotjuggler.svg", theme));
  if (unseen_error_) {
    // SourceIn keeps the glyph's alpha and swaps its ink for the status-error
    // hue, which the token table already resolves per theme.
    QPixmap bell = loadSvg(":/resources/svg/alarm-bell-active.svg", theme);
    QPainter tint(&bell);
    tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tint.fillRect(bell.rect(), theme::status(theme::Status::Error, theme::themeFor(isLightTheme(theme))));
    ui_->buttonNotifications->setIcon(bell);
    ui_->buttonNotifications->setToolTip(tr("New errors"));
  } else {
    ui_->buttonNotifications->setIcon(loadSvg(":/resources/svg/alarm-bell.svg", theme));
    ui_->buttonNotifications->setToolTip(tr("Diagnostics"));
  }
  ui_->buttonMinimize->setIcon(loadSvg(":/resources/svg/minimize.svg", theme));
  ui_->buttonMaximize->setIcon(loadSvg(":/resources/svg/maximize.svg", theme));
  ui_->buttonClose->setIcon(loadSvg(":/resources/svg/close_windows_light.svg", theme));
}

}  // namespace PJ
