// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>
#include <string>

namespace PJ {

class MainWindow;
class PanelEngine;
class ToolboxRuntimeHost;
struct ToolboxPanelSession;

// One live toolbox: the wrapped panel (from MainWindow::wrapToolboxPanel), its
// PanelEngine, the plugin session that keeps the host services and plugin
// instance alive, and the presentation it currently has. MainWindow owns one
// per plugin id in its registry; the same container moves between
// presentations without teardown, and teardown() is the single way out.
//
// Callbacks handed to Qt (tab close, window finished, engine close requests)
// look the presenter up by plugin id through MainWindow rather than capturing
// it, so a callback arriving after teardown finds nothing instead of a
// dangling pointer.
class ToolboxPresenter {
 public:
  enum class Presentation { kDetached, kTakeover, kTab, kFloating };
  enum class Teardown { kDeferred, kImmediate };

  struct Spec {
    QWidget* container = nullptr;   // the wrapped panel; parentless when handed over
    PanelEngine* engine = nullptr;  // null for engine-less panels (tests drive the chrome alone)
    // The launch's session, owned here so it dies with the presenter (after
    // the engine stopped touching the plugin's dialog). Its address is the
    // identity ingest callbacks report, so it also keys the takeover fold.
    std::shared_ptr<ToolboxPanelSession> session;
    ToolboxRuntimeHost* host = nullptr;  // owned by `session`, valid for the presenter's whole life
    QString title;
    std::function<QString()> save_config;  // the toolbox's saveConfig JSON for layout save
    // Re-shows the banner in tab (pinned=true) or docked chrome; the floating
    // window hides it (MainWindow::wrapToolboxPanel).
    std::function<void(bool pinned)> apply_chrome;
  };

  ToolboxPresenter(MainWindow& shell, QString plugin_id, Spec spec);
  ~ToolboxPresenter();
  ToolboxPresenter(const ToolboxPresenter&) = delete;
  ToolboxPresenter& operator=(const ToolboxPresenter&) = delete;

  // Presentation changes. Each releases the container from where it is and
  // attaches it elsewhere; the plugin session, drawer and actions ride along.
  // moveToDock returns false when the chart slot could not be claimed, leaving
  // the container released (detached) for the caller to place or tear down.
  // moveToTab's `transient` marks a host-chosen fold, excluded from layout save.
  bool moveToDock();
  void moveToTab(bool transient);
  void moveToFloating();
  // The "dock" gesture: a missing chart slot must not strand a live session
  // offscreen, so it falls back to a (user-chosen, persisted) tab.
  void moveToDockOrTab();

  // Detaches the container from its presentation without teardown and returns
  // it parentless, updating title() from the tab strip's (possibly renamed)
  // label. Null when nothing is presented or the presentation no longer holds
  // this container, which leaves it untouched.
  QWidget* release();

  // Stops the engine and destroys container, window and engine — deferred via
  // deleteLater, or immediately for bulk closes (layout replace, shutdown),
  // where the engine must be gone before a relaunch binds a fresh plugin
  // instance. Once only: MainWindow::teardownToolbox extracts the registry
  // entry first, so nothing can reach the presenter again. The session is
  // released when the presenter dies, after the engine stopped using the
  // plugin's dialog.
  void teardown(Teardown mode);

  // The chrome's close button. A tab closes through the strip's vetoable path
  // (busy confirmation); a floating window through its own close; a docked
  // takeover with work in flight FOLDS into a transient tab instead of tearing
  // down (rule 2: teardown is the job's kill switch, and a user who pressed
  // close chose a pin even less than an auto-fold did), idle it tears down.
  void requestClose();

  // Routes a plugin-initiated requestClose for the current presentation;
  // returns whether the engine may tear itself down. A tab ignores
  // "import_complete" (a folded panel is the user's surface once pinned and
  // outlives its own batch) and answers false so the kept-open tab keeps
  // exchanging widget data instead of becoming an inert shell; every other
  // reason closes the tab, and a declined busy confirmation also keeps the
  // engine.
  bool onEngineCloseRequested(const std::string& reason);

  [[nodiscard]] const QString& pluginId() const {
    return plugin_id_;
  }
  // The chart-area takeover has no registry of its own: MainWindow's current
  // panel is the ground truth, whichever path presented the container, so
  // kTakeover/kDetached are derived from it rather than recorded.
  [[nodiscard]] Presentation presentation() const;
  [[nodiscard]] QWidget* container() const {
    return container_.data();
  }
  [[nodiscard]] QWidget* floatingWindow() const {
    return floating_window_.data();
  }
  [[nodiscard]] ToolboxRuntimeHost* host() const {
    return host_;
  }
  // The tab label: read live from the strip while presentation() == kTab (the
  // sole store of a user rename), otherwise the one it was pinned or last
  // released with.
  [[nodiscard]] QString title() const;
  [[nodiscard]] bool transient() const {
    return transient_;
  }
  [[nodiscard]] QString saveConfig() const {
    return save_config_ ? save_config_() : QString();
  }
  // Work in flight on this toolbox's host (through the shell's test seam).
  [[nodiscard]] bool hasWorkInFlight() const;
  // The identity ingest callbacks report for this toolbox: its session's
  // address (the presenter's own for an engine-less one). Compared, never
  // dereferenced.
  [[nodiscard]] const void* sessionOwner() const;

 private:
  friend class ToolboxPanelFoldTestPeer;

  // The container to attach next: what release() gives back, or the container
  // itself when it was never presented (a fresh launch).
  QWidget* releasableContent();
  void attachAsTab(QWidget* content, bool transient);
  void attachAsFloating(QWidget* content);

  MainWindow& shell_;
  const QString plugin_id_;
  QPointer<QWidget> container_;
  QPointer<PanelEngine> engine_;
  std::shared_ptr<ToolboxPanelSession> session_;
  ToolboxRuntimeHost* host_ = nullptr;
  QString title_;
  std::function<QString()> save_config_;
  std::function<void(bool pinned)> apply_chrome_;
  Presentation presentation_ = Presentation::kDetached;
  bool transient_ = false;
  // The floating PJ::Dialog while presentation_ == kFloating, and its
  // finished() -> teardown connection (disconnected by release(), so closing
  // the emptied window tears down nothing).
  QPointer<QWidget> floating_window_;
  QMetaObject::Connection on_window_finished_;
};

}  // namespace PJ
