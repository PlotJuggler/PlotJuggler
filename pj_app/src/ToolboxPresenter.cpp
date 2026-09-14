// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ToolboxPresenter.h"

#include <QAbstractButton>
#include <QDialog>
#include <QLayout>
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

bool ToolboxPresenter::moveToDock() {
  QWidget* content = releasableContent();
  if (content == nullptr) {
    return false;
  }
  if (!shell_.presentPanel(content)) {
    return false;
  }
  shell_.current_panel_engine_ = engine_.data();
  if (apply_chrome_) {
    apply_chrome_(false);
  }
  return true;
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

void ToolboxPresenter::moveToDockOrTab() {
  if (!moveToDock()) {
    moveToTab(/*transient=*/false);
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
      // The side column goes back to the panel's own splitter before the
      // panel leaves the window, so nothing is stranded in the dying window.
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
      container_->setParent(nullptr);
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
  // Rules 3-4: an idle folded panel closes like any tab; one with a job in
  // flight confirms first, because the teardown above cancels that job. The
  // presenter's own host is what makes the cancellation land on THIS panel.
  tabs.setWidgetTabPreClose(content, [&shell, id]() {
    ToolboxPresenter* live = shell.findToolbox(id);
    if (live == nullptr || !live->hasWorkInFlight()) {
      return true;
    }
    if (!shell.confirmCancelRunningJob(live->title())) {
      return false;
    }
    // The confirmation ran a modal event loop, so `live` may be gone: re-find
    // rather than stopping through a dangling host.
    if (ToolboxPresenter* confirmed = shell.findToolbox(id)) {
      shell.stopHostWork(confirmed->host());
    }
    return true;
  });
}

void ToolboxPresenter::attachAsFloating(QWidget* content) {
  MainWindow& shell = shell_;
  const QString id = plugin_id_;
  // A PJ::Dialog, not a bare QWidget or QDialog: the app QSS paints QWidget
  // transparent (a top-level one renders black), and only the canonical dialog
  // family paints the app's own themed, frameless chrome.
  auto* window = new PJ::Dialog(&shell_);
  window->setDialogTitle(title_);  // the chrome's own title bar
  window->setWindowTitle(title_);  // and the WM/taskbar entry
  window->setChromeMetrics(shell_.chrome_metrics_);
  // The panel goes into the chrome's content area, NOT onto the dialog itself:
  // PJ::Dialog already owns the dialog's layout (title bar above, body below),
  // so parenting a second layout to the window would fight it.
  if (auto* layout = window->contentLayout(); layout != nullptr) {
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
  }
  // The panel's chrome pieces follow it into the window: its side drawer
  // becomes the window's side column, and its tagged actions get proxies in
  // the title bar (the banner, with its own proxies, is hidden while
  // floating). Both go back when the panel returns to a tab.
  if (QSplitter* splitter = toolboxDrawerSplitter(content); splitter != nullptr && splitter->count() > 0) {
    QWidget* first = splitter->widget(0);
    if (first != nullptr && first->property(kToolboxSideDrawerProperty).toBool()) {
      // Same key the tab presentation's splitter uses, so a width set in
      // one presentation is the width the other opens with.
      window->setSideWidget(first, toolboxDrawerWidthSettingsKey(plugin_id_));
    }
  }
  for (QAbstractButton* src : taggedChromeActions(content)) {
    window->addTitleBarAction(
        makeChromeActionProxy(src, window),
        isLeadingChromeAction(src) ? PJ::Dialog::TitleBarSlot::Leading : PJ::Dialog::TitleBarSlot::Trailing);
  }
  // releaseCentralPanel() hid the container EXPLICITLY, and a layout does not
  // undo an explicit hide — without this the window opens with an empty body.
  content->show();
  // Enter inside the floating QDialog is handled by Qt's native default-button
  // interception (the dialog-host's returnPressed wiring stands down inside
  // QDialogs). Two adjustments make that native path work:
  //  - strip autoDefault: it makes Enter click whichever button happens to
  //    HOLD FOCUS — and Qt silently moves focus to the next button (e.g.
  //    Cancel) when a focused button is disabled mid-turn, turning Enter into
  //    an accidental cancel;
  //  - re-assert the panel's declared default: the .ui declared it long
  //    before this reparent gave the button a QDialog ancestor, so toggle it
  //    for the dialog to pick it up.
  for (auto* btn : window->findChildren<QPushButton*>()) {
    btn->setAutoDefault(false);
    if (btn->isDefault()) {
      btn->setDefault(false);
      btn->setDefault(true);
    }
  }
  // Starting size only (user-resizable). A panel that declares a side
  // drawer needs room for the drawer PLUS the content beside it, not just
  // the content — e.g. a ~160px drawer floor next to a ~380px content
  // minimum, plus margins — so 720 is the floor, comfortably over that sum.
  // Prefer the window's own projected size (drawer included, via
  // setSideWidget above) when it asks for more than that floor.
  window->resize(qMax(720, window->sizeHint().width()), 620);
  // Single teardown point. Every way the window can close funnels through
  // QDialog::done() — the chrome's ✕ and Escape call reject(), and so does
  // closeEvent, so a plugin-initiated close() lands here too — and done()
  // emits finished() while the panel is still alive, so the engine is torn
  // down before the deferred deletes run. Single-shot: a second close is a
  // no-op, and release() disconnects it first so closing the emptied window
  // then tears down nothing.
  on_window_finished_ = QObject::connect(
      window, &QDialog::finished, &shell_, [&shell, id](int) { shell.teardownToolbox(id, Teardown::kDeferred); },
      Qt::SingleShotConnection);
  floating_window_ = window;
  presentation_ = Presentation::kFloating;
  // The ways back, in the window's own title bar next to its ✕.
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
    shell.withToolbox(id, [](ToolboxPresenter& live) { live.moveToDockOrTab(); });
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
  // Quiesce the plugin FIRST, while the session that owns it is still alive:
  // close() reaches plugin code through the borrowed dialog.
  if (!engine_.isNull()) {
    engine_->close();
  }
  QObject::disconnect(on_window_finished_);
  QWidget* owned = container_.data();
  switch (presentation()) {
    case Presentation::kTakeover:
    case Presentation::kTab:
      // Safe from inside the strip's own on_close too: the strip re-derives
      // its entry afterwards and finds this tab already gone.
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
