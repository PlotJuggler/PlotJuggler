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
  // Without it QSS `background:` does not paint a QWidget subclass.
  setAttribute(Qt::WA_StyledBackground, true);

  ui_->setupUi(this);
  // centerContainer must stay opaque to mouse events: WA_TransparentForMouseEvents would make childAt() skip
  // the center widget's buttons and route their clicks to the drag handler. The height is pinned so
  // QMainWindow::setMenuWidget cannot size the bar by sizeHint (a gray strip under the buttons); MainWindow
  // pushes the saved metrics later through chromeMetricsChanged.
  applyIconMetrics();

  // objectName "PJMenu": the QMenu#PJMenu selector outranks the app-wide `QWidget { background: transparent }`
  // rule, which would otherwise win for the popups. No native menu bar: the menus are part of the in-window
  // chrome even on desktops with a global menu bar.
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

  setSystemFrame(false);
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
  // Into the bell's group, sharing its spacing. Re-applies the metrics: callers add widgets after the first
  // broadcast.
  ui_->rightClusterLayout->addWidget(widget);
  applyIconMetrics();
}

void TitleBar::setCenterWidget(QWidget* widget) {
  if (widget == center_widget_) {
    return;
  }
  // Not deleted: the caller owns it.
  if (center_widget_ != nullptr) {
    ui_->centerLayout->removeWidget(center_widget_);
    center_widget_->setParent(nullptr);
  }
  center_widget_ = widget;
  if (widget != nullptr) {
    // Between the two spacers, stretch 0, so the spacers center it.
    ui_->centerLayout->insertWidget(1, widget, /*stretch=*/0);
    applyIconMetrics();
  }
}

void TitleBar::setSystemFrame(bool system_frame) {
  system_frame_ = system_frame;
  ui_->appIcon->setHidden(system_frame);
#ifdef PJ_TARGET_WASM
  constexpr bool kBrowserTab = true;  // the tab's own chrome owns minimize/maximize/close
#else
  constexpr bool kBrowserTab = false;
#endif
  for (QAbstractButton* button : {ui_->buttonMinimize, ui_->buttonMaximize, ui_->buttonClose}) {
    button->setHidden(system_frame || kBrowserTab);
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
  // Includes the QSS bottom border, drawn inside the geometry. Dialog chrome uses the same call.
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

  // The right cluster (bell and addRightClusterWidget widgets) shares the bell's square extent.
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
  // The "Update" button's height is pinned here, never in QSS: a QSS max-height would cap it below
  // button_extent.
  ui_->buttonExtensionUpdate->setMinimumHeight(button_extent);
  ui_->buttonExtensionUpdate->setMaximumHeight(button_extent);
  ui_->appIcon->setMinimumHeight(button_extent);
  ui_->appIcon->setMaximumHeight(button_extent);
  ui_->appIcon->setIconSize(icon_sz);
  // Natural height, capped and centered: stretched to the row, its item highlights hang from the top edge.
  ui_->menuBar->setMaximumHeight(button_extent);

  // Center tool buttons hug their icon (no chrome padding), flush against the neighbouring progress bar.
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
  if (system_frame_) {
    return false;  // the window manager's own title bar moves and maximizes the window
  }
  // The bar background, the app icon and the empty center area; an interactive child gets its own click.
  QWidget* hit = childAt(pos);
  return hit == nullptr || hit == ui_->appIcon || hit == ui_->centerContainer || hit == center_widget_;
}

void TitleBar::applyIcons(const QString& theme) {
  ui_->appIcon->setIcon(loadSvg(":/resources/svg/plotjuggler.svg", theme));
  if (unseen_error_) {
    // SourceIn keeps the glyph's alpha and swaps its ink.
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
