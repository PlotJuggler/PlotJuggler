// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ToolboxPresenter.h"

#include <QAbstractButton>
#include <QDialog>
#include <QLayout>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <utility>

#include "MainWindow.h"
#include "ToolboxChrome.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_plugins/host_qt/panel_engine.hpp"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/SvgButton.h"
#include "ui_MainWindow.h"

using namespace Qt::StringLiterals;

namespace PJ {

ToolboxPresenter::ToolboxPresenter(MainWindow& shell, QString plugin_id, Spec spec)
    : shell_(shell),
      plugin_id_(std::move(plugin_id)),
      container_(spec.container),
      engine_(spec.engine),
      session_(std::move(spec.session)),
      host_(spec.host),
      title_(std::move(spec.title)),
      save_config_(std::move(spec.save_config)),
      apply_chrome_(std::move(spec.apply_chrome)) {
  // One close routing for every presentation, resolved at request time.
  if (!engine_.isNull()) {
    engine_->onCloseRequested([&shell, id = plugin_id_](const std::string& reason) {
      ToolboxPresenter* live = shell.findToolbox(id);
      return live == nullptr || live->onEngineCloseRequested(reason);
    });
  }
}

ToolboxPresenter::~ToolboxPresenter() = default;

ToolboxPresenter::Presentation ToolboxPresenter::presentation() const {
  if (presentation_ == Presentation::kTab || presentation_ == Presentation::kFloating) {
    return presentation_;
  }
  const bool is_takeover = !container_.isNull() && shell_.current_panel_ == container_.data();
  return is_takeover ? Presentation::kTakeover : Presentation::kDetached;
}

bool ToolboxPresenter::hasWorkInFlight() const {
  return shell_.hostHasWorkInFlight(host_);
}

const void* ToolboxPresenter::sessionOwner() const {
  return session_ != nullptr ? static_cast<const void*>(session_.get()) : this;
}

QWidget* ToolboxPresenter::releasableContent() {
  if (QWidget* released = release(); released != nullptr) {
    return released;
  }
  const bool never_presented =
      presentation() == Presentation::kDetached && !container_.isNull() && container_->parentWidget() == nullptr;
  return never_presented ? container_.data() : nullptr;
}

void ToolboxPresenter::moveToDock() {
  QWidget* content = releasableContent();
  if (content == nullptr) {
    return;
  }
  shell_.presentPanel(content);
  shell_.current_panel_engine_ = engine_.data();
  if (apply_chrome_) {
    apply_chrome_(false);
  }
}

void ToolboxPresenter::moveToTab(bool transient) {
  if (QWidget* content = releasableContent()) {
    attachAsTab(content, transient);
  }
}

void ToolboxPresenter::moveToFloating() {
  if (QWidget* content = releasableContent()) {
    attachAsFloating(content);
  }
}

QWidget* ToolboxPresenter::release() {
  if (container_.isNull()) {
    return nullptr;
  }
  TabbedPlotWidget& tabs = *shell_.ui_->tabbedPlotWidget;
  switch (presentation()) {
    case Presentation::kTab: {
      // The strip's label is the sole store of a user rename.
      title_ = tabs.widgetTabName(container_);
      QWidget* released = tabs.takeWidgetTab(container_);
      if (released != nullptr) {
        presentation_ = Presentation::kDetached;
      }
      return released;
    }
    case Presentation::kFloating: {
      auto* window = static_cast<PJ::Dialog*>(floating_window_.data());
      if (window == nullptr) {
        return nullptr;
      }
      // The side column goes back to the panel's own splitter, so nothing is
      // stranded in the dying window. The panel itself stays in it until its
      // destination reparents it (the window is only deleted later).
      QWidget* side = window->sideWidget();
      QSplitter* splitter = side != nullptr ? toolboxDrawerSplitter(container_) : nullptr;
      if (side != nullptr && splitter == nullptr) {
        return nullptr;
      }
      QObject::disconnect(on_window_finished_);
      if (side != nullptr) {
        window->setSideWidget(nullptr);
        splitter->insertWidget(0, side);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
      }
      window->close();
      window->deleteLater();
      floating_window_ = nullptr;
      presentation_ = Presentation::kDetached;
      return container_.data();
    }
    case Presentation::kTakeover:
      return shell_.releaseCentralPanel();
    case Presentation::kDetached:
      return nullptr;
  }
  return nullptr;
}

QString ToolboxPresenter::title() const {
  if (presentation() == Presentation::kTab) {
    return shell_.ui_->tabbedPlotWidget->widgetTabName(container_);
  }
  return title_;
}

void ToolboxPresenter::attachAsTab(QWidget* content, bool transient) {
  transient_ = transient;
  presentation_ = Presentation::kTab;
  if (apply_chrome_) {
    apply_chrome_(true);
  }
  MainWindow& shell = shell_;
  const QString id = plugin_id_;
  TabbedPlotWidget& tabs = *shell_.ui_->tabbedPlotWidget;
  tabs.addWidgetTab(title_, content, [&shell, id]() { shell.teardownToolbox(id, Teardown::kDeferred); });
  // A panel with a job in flight confirms first, because the teardown cancels that job; stopping through its
  // own host makes the cancellation land on this panel only.
#ifdef PJ_TARGET_WASM
  tabs.setWidgetTabPreClose(content, [&shell, id, tab_content = QPointer<QWidget>(content)]() {
#else
  tabs.setWidgetTabPreClose(content, [&shell, id]() {
#endif
    ToolboxPresenter* live = shell.findToolbox(id);
    if (live == nullptr || !live->hasWorkInFlight()) {
      return true;
    }
#ifdef PJ_TARGET_WASM
    // The browser cannot hold this close open on a modal answer: veto it, ask,
    // and close the tab ourselves once the user confirms.
    shell.confirmCancelRunningJobAsync(live->title(), [&shell, id, tab_content]() {
      if (ToolboxPresenter* confirmed = shell.findToolbox(id)) {
        shell.stopHostWork(confirmed->host());
      }
      if (tab_content != nullptr) {
        shell.ui_->tabbedPlotWidget->closeWidgetTabForced(tab_content);
      }
    });
    return false;
#else
    if (!shell.confirmCancelRunningJob(live->title())) {
      return false;
    }
#endif
    // The confirmation ran a modal event loop, so `live` may be gone: re-find it.
    if (ToolboxPresenter* confirmed = shell.findToolbox(id)) {
      shell.stopHostWork(confirmed->host());
    }
    return true;
  });
}

void ToolboxPresenter::attachAsFloating(QWidget* content) {
  MainWindow& shell = shell_;
  const QString id = plugin_id_;
  // A PJ::Dialog: the app QSS paints QWidget transparent (a top-level one renders black), and only the dialog
  // family paints the themed frameless chrome.
  auto* window = new PJ::Dialog(&shell_);
  window->setDialogTitle(title_);
  window->setWindowTitle(title_);  // the WM/taskbar entry
  window->setChromeMetrics(shell_.chrome_metrics_);
  // Into the content area: PJ::Dialog owns the window's own layout.
  if (auto* layout = window->contentLayout(); layout != nullptr) {
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
  }
  // The side drawer becomes the window's side column and the tagged actions get title-bar proxies (the banner
  // is hidden while floating); both go back when the panel returns to a tab.
  if (QSplitter* splitter = toolboxDrawerSplitter(content); splitter != nullptr && splitter->count() > 0) {
    QWidget* first = splitter->widget(0);
    if (first != nullptr && first->property(kToolboxSideDrawerProperty).toBool()) {
      // The tab splitter's key: both presentations share the width.
      window->setSideWidget(first, toolboxDrawerWidthSettingsKey(plugin_id_));
    }
  }
  for (QAbstractButton* src : taggedChromeActions(content)) {
    window->addTitleBarAction(
        makeChromeActionProxy(src, window),
        isLeadingChromeAction(src) ? PJ::Dialog::TitleBarSlot::Leading : PJ::Dialog::TitleBarSlot::Trailing);
  }
  // releaseCentralPanel() hid it explicitly, which a layout does not undo.
  content->show();
  // Enter in a QDialog goes to Qt's default button (the dialog host's returnPressed wiring stands down there).
  // autoDefault would make Enter click the focused button, and Qt moves focus on to the next one (e.g. Cancel)
  // when a focused button is disabled; the declared default is toggled so the new dialog ancestor picks it up.
  for (auto* btn : window->findChildren<QPushButton*>()) {
    btn->setAutoDefault(false);
    if (btn->isDefault()) {
      btn->setDefault(false);
      btn->setDefault(true);
    }
  }
  // Starting size only; the width floor leaves room for a side drawer beside the content.
  window->resize(qMax(720, window->sizeHint().width()), 620);
  // Every close (✕, Escape, closeEvent, a plugin's close()) goes through QDialog::done(), which emits finished()
  // while the panel is alive, so the engine is torn down before the deferred deletes run.
  on_window_finished_ = QObject::connect(
      window, &QDialog::finished, &shell_, [&shell, id](int) { shell.teardownToolbox(id, Teardown::kDeferred); },
      Qt::SingleShotConnection);
  floating_window_ = window;
  presentation_ = Presentation::kFloating;
  SvgButton* to_tab_button = makeChromeButton(u":/resources/svg/tab_move.svg"_s, QObject::tr("Move to a tab"), window);
  to_tab_button->setObjectName(u"buttonFloatingMigrateTab"_s);
  window->addTitleBarAction(to_tab_button);
  QObject::connect(to_tab_button, &QAbstractButton::clicked, &shell_, [&shell, id]() {
    shell.withToolbox(id, [](ToolboxPresenter& live) { live.moveToTab(/*transient=*/false); });
  });
  SvgButton* to_dock_button =
      makeChromeButton(u":/resources/svg/dock_window.svg"_s, QObject::tr("Move to docked panel"), window);
  to_dock_button->setObjectName(u"buttonFloatingMigrateDock"_s);
  window->addTitleBarAction(to_dock_button);
  QObject::connect(to_dock_button, &QAbstractButton::clicked, &shell_, [&shell, id]() {
    shell.withToolbox(id, [](ToolboxPresenter& live) { live.moveToDock(); });
  });
  window->show();
  shell_.schedulePanelPreviewSync();
}

void ToolboxPresenter::requestClose() {
  switch (presentation()) {
    case Presentation::kTab:
      shell_.ui_->tabbedPlotWidget->closeWidgetTab(container_);
      return;
    case Presentation::kFloating:
      if (!floating_window_.isNull()) {
        floating_window_->close();  // finished() runs the teardown
      }
      return;
    case Presentation::kTakeover:
    case Presentation::kDetached:
      if (hasWorkInFlight()) {
        moveToTab(/*transient=*/true);
        return;
      }
      shell_.teardownToolbox(plugin_id_, Teardown::kDeferred);
      return;
  }
}

bool ToolboxPresenter::onEngineCloseRequested(const std::string& reason) {
  switch (presentation()) {
    case Presentation::kTab: {
      if (reason == "import_complete") {
        return false;
      }
      // closeWidgetTab may tear this presenter down: only locals past it.
      MainWindow& shell = shell_;
      const QString id = plugin_id_;
      shell.ui_->tabbedPlotWidget->closeWidgetTab(container_);
      return shell.findToolbox(id) == nullptr;
    }
    case Presentation::kFloating:
      if (!floating_window_.isNull()) {
        floating_window_->close();  // finished() runs the teardown
      }
      return true;
    case Presentation::kTakeover:
    case Presentation::kDetached:
      shell_.teardownToolbox(plugin_id_, Teardown::kDeferred);
      return true;
  }
  return true;
}

void ToolboxPresenter::teardown(Teardown mode) {
  // First, while the session that owns the plugin is alive: close() reaches plugin code through its dialog.
  if (!engine_.isNull()) {
    engine_->close();
  }
  QObject::disconnect(on_window_finished_);
  QWidget* owned = container_.data();
  switch (presentation()) {
    case Presentation::kTakeover:
    case Presentation::kTab:
      // Safe inside the strip's own on_close: the strip re-derives its entry afterwards.
      static_cast<void>(release());
      break;
    case Presentation::kFloating:
      if (!floating_window_.isNull()) {
        floating_window_->close();
        owned = floating_window_.data();  // the container rides inside it
      }
      break;
    case Presentation::kDetached:
      break;
  }
  presentation_ = Presentation::kDetached;
  if (mode == Teardown::kImmediate) {
    // Direct deletes cancel any deleteLater already queued for these.
    delete engine_.data();
    delete owned;
    return;
  }
  if (!engine_.isNull()) {
    engine_->deleteLater();
  }
  if (owned != nullptr) {
    owned->deleteLater();
  }
}

}  // namespace PJ
