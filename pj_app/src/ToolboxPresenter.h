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

// One live toolbox: the wrapped panel, its PanelEngine, the plugin session that keeps the host services and
// plugin instance alive, and its presentation. MainWindow owns one per plugin id; the container moves between
// presentations without teardown, and teardown() is the single way out. Callbacks handed to Qt look the
// presenter up by plugin id, never capture it: one arriving after teardown must find nothing.
class ToolboxPresenter {
 public:
  enum class Presentation { kDetached, kTakeover, kTab, kFloating };
  enum class Teardown { kDeferred, kImmediate };

  struct Spec {
    QWidget* container = nullptr;   // the wrapped panel; parentless when handed over
    PanelEngine* engine = nullptr;  // null for engine-less panels (tests drive the chrome alone)
    // Dies with the presenter, after the engine stopped touching the plugin's dialog. Its address is the
    // identity ingest callbacks report, which keys the takeover fold.
    std::shared_ptr<ToolboxPanelSession> session;
    ToolboxRuntimeHost* host = nullptr;  // owned by `session`, valid for the presenter's whole life
    QString title;
    std::function<QString()> save_config;  // the toolbox's saveConfig JSON for layout save
    // Re-shows the banner in tab (pinned) or docked chrome; the floating window hides it.
    std::function<void(bool pinned)> apply_chrome;
  };

  ToolboxPresenter(MainWindow& shell, QString plugin_id, Spec spec);
  ~ToolboxPresenter();
  ToolboxPresenter(const ToolboxPresenter&) = delete;
  ToolboxPresenter& operator=(const ToolboxPresenter&) = delete;

  // The session, drawer and actions ride along; a no-op without a container. `transient` marks a host-chosen
  // fold, excluded from layout save.
  void moveToDock();
  void moveToTab(bool transient);
  void moveToFloating();

  // Detaches the container without teardown and returns it hidden, taking title() from the tab strip. It is
  // NOT reparented (every reparent re-polishes the tree; see MainWindow::releaseCentralPanel): it stays where it
  // was until its destination reparents it or teardown deletes it. Null when the presentation no longer holds
  // this container.
  QWidget* release();

  // Stops the engine and destroys container, window and engine: deferred, or immediate for bulk closes (layout
  // replace, shutdown), where the engine must be gone before a relaunch binds a fresh plugin instance. Called
  // once, by MainWindow::teardownToolbox after it extracted the registry entry.
  void teardown(Teardown mode);

  // The chrome's close button. A tab closes through the strip's vetoable path (busy confirmation), a floating
  // window through its own close. A docked takeover with work in flight folds into a transient tab, since
  // teardown would cancel the job; idle, it tears down.
  void requestClose();

  // A plugin-initiated close; returns whether the engine may tear itself down. A tab ignores "import_complete"
  // (a folded panel outlives its batch) and answers false so it keeps exchanging widget data; any other reason
  // closes the tab, unless a busy confirmation is declined.
  bool onEngineCloseRequested(const std::string& reason);

  [[nodiscard]] const QString& pluginId() const {
    return plugin_id_;
  }
  // kTakeover/kDetached are derived from MainWindow's current panel, the only record of the takeover.
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
  // Read live from the strip while kTab (the only store of a user rename), else the last pinned/released label.
  [[nodiscard]] QString title() const;
  [[nodiscard]] bool transient() const {
    return transient_;
  }
  [[nodiscard]] QString saveConfig() const {
    return save_config_ ? save_config_() : QString();
  }
  [[nodiscard]] bool hasWorkInFlight() const;
  // The identity ingest callbacks report: the session's address (the presenter's own without one). Compared,
  // never dereferenced.
  [[nodiscard]] const void* sessionOwner() const;

 private:
  friend class ToolboxPanelFoldTestPeer;

  // What release() gives back, or the container itself on a fresh launch.
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
  // While kFloating. release() disconnects finished() -> teardown, so closing the emptied window tears down
  // nothing.
  QPointer<QWidget> floating_window_;
  QMetaObject::Connection on_window_finished_;
};

}  // namespace PJ
