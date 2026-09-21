#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QMap>
#include <QPointer>
#include <QUrl>

#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/installed_extension.hpp"
#include "pj_widgets/Dialog.h"

namespace Ui {
class MarketplaceWindow;
}

namespace PJ {

class DownloadManager;
class ExtensionManager;
class RegistryManager;

// Marketplace window on the canonical app chrome (PJ::Dialog): frameless title
// bar + close, no system (window-manager) decorations.
class MarketplaceWindow : public Dialog {
  Q_OBJECT

 public:
  explicit MarketplaceWindow(const QUrl& registry_url, QWidget* parent = nullptr);

  // Uses an externally owned ExtensionManager, mainly for tests and embedding.
  explicit MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent = nullptr);

  // Uses an externally owned ExtensionManager and a caller-provided installed snapshot.
  explicit MarketplaceWindow(
      ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
      QWidget* parent = nullptr);

  ~MarketplaceWindow() override;

  // Returns true when install state changed while the dialog was open.
  bool installationsChanged() const {
    return installations_changed_;
  }

  // The marketplace content widget (the whole UI body). Lets a host embed the
  // marketplace as a central-area panel/tab instead of a modal window: reparent
  // this into the host container and keep the MarketplaceWindow alive as the
  // controller (it owns the UI and receives all the child-widget signals).
  QWidget* contentWidget() const {
    return content_widget_;
  }

  // Refreshes installed state and repaints — the embedded equivalent of what
  // showEvent() does for the modal path. Call after embedding contentWidget().
  void activateEmbedded();

  // Points the window at a different registry and re-fetches it immediately.
  // The URL is captured once at construction and read nowhere else, so a panel
  // the user leaves open keeps serving the registry it opened with until the
  // host pushes the new one here. Registry-URL policy stays with the host (the
  // Preferences setting); this window never second-guesses it. A no-op when the
  // URL is unchanged, so an unrelated Preferences edit costs nothing.
  void setRegistryUrl(const QUrl& registry_url);

 signals:
  // Emitted when the user clicks the settings (gear) button. The host (MainWindow)
  // responds by opening Preferences ▸ Plugins; the marketplace itself has no
  // dependency on the preferences UI.
  void pluginPreferencesRequested();

 protected:
  // Refreshes installed state before the table is repainted.
  void showEvent(QShowEvent* event) override;

 private slots:
  // Updates the search filter.
  void onSearchChanged(const QString& text);

  // Re-applies the filter row after any one of its toggles flips. Shared by all
  // of them: the toggles are read as a set, so which one changed is irrelevant.
  void onFilterToggled();

  // Picks a local ZIP and sideloads it via ExtensionManager::installFromLocalZip.
  void onInstallLocalClicked();

  // Queues updates for every installed extension with a newer registry version.
  void onUpdateAllClicked();

  // Opens a read-only view of recent marketplace diagnostics.
  void onDiagnosticsClicked();

  // Runs the primary install/update action for one extension card.
  void onActionButtonClicked(const QString& ext_id);

  // Confirms and uninstalls one installed extension.
  void onUninstallButtonClicked(const QString& ext_id);

 private:
  // Shared constructor tail: UI + signals, optional installed-state snapshot,
  // initial diagnostics, and the first registry fetch. The caller owns
  // registry-URL policy (the PJ4 host resolves it from its Preferences-managed
  // setting); the window never second-guesses registry_url_.
  void finishConstruction(const QMap<QString, InstalledExtension>* installed);

  // Creates widgets from the .ui file and configures fixed UI affordances.
  void setupUi();

  // Connects registry, extension-manager, and widget signals.
  void setupSignals();

  // Rebuilds every table row from filtered_. preserve_scroll keeps the vertical
  // scroll offset across the rebuild (true for install/update/uninstall repaints,
  // so the list doesn't jump to the top mid-session); pass false when the row set
  // changes meaning — filter/search/registry reload — where returning to the top
  // is the expected behaviour.
  void rebuildTable(bool preserve_scroll = true);

  // Recomposes extensions_ as registry_extensions_ plus one synthesized row per
  // installed extension the registry does not list, so a sideloaded plugin is
  // still visible and manageable. Recomputed rather than appended to, so a
  // repeated install can never duplicate a row. Returns whether the resulting id
  // set differs from the previous one.
  bool rebuildExtensionList();

  // Repaints after the installed set may have changed. Re-filters only when a row
  // actually appeared or disappeared (a local-only extension installed or
  // removed) — otherwise the cheaper repaint keeps the scroll position, since the
  // same rows still mean the same thing.
  void refreshAfterInstalledChange();

  // Applies search and category filters to the registry list.
  void applyFilters();

  // Updates the status label; error statuses remain sticky until a user action clears them.
  void setStatus(const QString& msg, bool is_error = false);

  // Allows the next non-error status update to replace an error.
  void clearStickyStatus();

  // Shows the newest diagnostic in the status bar, if one exists.
  void showLatestDiagnostic();

  // Shows or hides the diagnostics button based on diagnostic history.
  void updateDiagnosticsButton();

  // Rebuilds the bottom "Details" footer (description + changelog + metadata)
  // for the currently selected table row. No selection / empty list clears it.
  void updateDetailFooter();

  // Processes one pending bulk-update item at a time.
  void processInstallQueue();

  // Dispatches a local-ZIP sideload and marks it in flight. Takes the path BY
  // VALUE: a synchronous failure inside installFromLocalZip() reaches
  // installFinished, which clears local_install_path_ while the callee is still
  // reading its argument.
  void startLocalInstall(QString zip_path);

  // True while ExtensionManager is busy with any install/update — a registry
  // op (active_install_id_ set) OR a local-ZIP sideload. The dispatch gates use
  // this so a new action is queued rather than dispatched into the manager's
  // single-install-at-a-time guard (which would reject it as "already in
  // progress"). A local install sets no active_install_id_, so checking that
  // alone missed it.
  [[nodiscard]] bool isInstallBusy() const {
    return !active_install_id_.isEmpty() || !local_install_path_.isEmpty();
  }

  // Shows the status of the in-flight install together with the queue depth,
  // e.g. "Installing mcap…  ·  2 queued". `verb` is the current phase word
  // ("Installing", "Verifying", "Extracting"). No-op when nothing is active, so
  // it never clobbers a terminal "Installed"/"Failed" message. Called on every
  // event that changes the active id or the queue, so the count stays live and
  // an enqueue no longer hides what is currently installing. A sideload has no
  // active_install_id_ until its manifest is read, so it is named by its file
  // name instead — otherwise the whole download/extract window is silent.
  void showInstallProgress(const QString& verb = QStringLiteral("Installing"));

  // "  ·  N queued" for the combined depth of every queue (card clicks, local
  // ZIPs, the Update All batch), followed by "  ·  M failed" once this run has
  // failures to report. Empty when nothing is waiting and nothing has failed.
  QString queueSuffix() const;

  // Success message for a finished install. `from_file` selects the sideload
  // phrasing, which reads the version from the installed snapshot rather than the
  // registry list. Neither phrasing mentions a restart: this is the immediate
  // install path. Staged outcomes report through installPendingRestart instead.
  QString installedStatusText(const QString& id, bool from_file) const;

  // Shows an informational (non-error) status, UNLESS an install/update is in
  // flight — then the status line belongs to that operation, so re-assert its
  // progress instead of clobbering it with unrelated text (filter count,
  // "Refreshing", "Ready", "Loading registry"). Errors still go via setStatus().
  void setInfoStatus(const QString& msg);

  // Pops the canonical restart-required MessageBox once the install queue has
  // fully settled and at least one operation staged for restart. No-op while a
  // batch is still in flight, so an Update All run yields ONE dialog at the
  // end, not one per staged item.
  void maybeShowRestartRequiredDialog();

  // Pops a MessageBox explaining that the registry could not be loaded, so only
  // already-installed extensions are shown and no discovery or updates are
  // available until it loads correctly. The wording states only the consequence
  // (it fires for both a parse error and a network failure); the concrete cause
  // goes in the Details line from last_registry_error_. The red status line alone
  // is easy to miss. Guarded by registry_error_dialog_open_ so a burst of failed
  // refreshes shows it once, not stacked.
  void showInvalidRegistryDialog();

  // Once all install activity has settled (nothing active or queued), if any
  // items failed during this run, replace the last item's success status with a
  // summary ("Finished with N failures — see Diagnostics") so a mid-batch failure is
  // not buried under a later success. No-op while anything is still in flight or
  // when nothing failed. Resets batch_failed_count_.
  void maybeShowBatchSummary();

  // Total depth of every queue: card clicks, local ZIPs and the Update All
  // batch. The one place that enumerates them, so adding a fourth queue cannot
  // silently leave the settled gate or the status suffix counting the old three.
  [[nodiscard]] int queuedCount() const {
    return static_cast<int>(pending_clicks_.size() + pending_local_zips_.size() + update_queue_.size());
  }

  // True when nothing is installing or queued — the shared "batch settled" gate
  // used by both maybeShowRestartRequiredDialog and maybeShowBatchSummary. Every
  // queue counts, sideloads included: a summary or a restart dialog fired while
  // a queued ZIP is still waiting would report on a batch that is not over.
  [[nodiscard]] bool installBatchSettled() const {
    return !isInstallBusy() && queuedCount() == 0;
  }

  Ui::MarketplaceWindow* ui_ = nullptr;
  QWidget* content_widget_ = nullptr;  ///< the UI body; exposed for embedding
  DownloadManager* download_mgr_ = nullptr;
  RegistryManager* registry_mgr_ = nullptr;
  // Guarded: the manager is owned by a service that MainWindow holds as a member,
  // and members die before ~QWidget deletes the child widgets — so by the time
  // this window is destroyed at shutdown, the manager may already be gone.
  QPointer<ExtensionManager> ext_mgr_;
  QUrl registry_url_;

  // Registry rows exactly as fetched, kept apart from extensions_ so the local-only
  // rows can be recomposed on top of them without ever accumulating duplicates.
  QList<Extension> registry_extensions_;
  QList<Extension> extensions_;  // registry_extensions_ + synthesized local-only rows
  QList<Extension> filtered_;
  QList<Extension> update_queue_;
  // Individual Install/Update button clicks that arrive while another install
  // is already running. Drained by processInstallQueue() in FIFO order once
  // active_install_id_ clears.
  QList<QString> pending_clicks_;
  // Local-ZIP install paths that arrive while another install is already
  // running. "Install local…" is queued like a card click instead of being
  // rejected by the manager's single-install-at-a-time guard; drained by
  // processInstallQueue() after pending_clicks_. Canonical paths, so the same
  // file reached by a different spelling (relative, symlink) is one entry.
  QList<QString> pending_local_zips_;
  // Id of the extension currently being installed or updated by the manager,
  // set from installStarted and cleared from installFinished. Empty means
  // idle — the UI-side guard uses this to decide whether to enqueue a click
  // instead of dispatching it straight to ExtensionManager::install().
  QString active_install_id_;
  // Registry id of the row whose details the footer shows; preserved across
  // table rebuilds so an install/update repaint keeps the same row selected.
  QString footer_ext_id_;
  // Canonical path of the local-ZIP sideload in flight, empty when none. The
  // outcome handlers phrase their message from the installed snapshot instead of
  // the registry list, which a sideloaded id is by definition absent from; the
  // path also dedupes a re-pick of the same file and names the status line for
  // the window before installStarted supplies an id.
  QString local_install_path_;
  // Staged operations (updates, sideload replacements, staged uninstalls and
  // downgrades) whose effect waits for an app restart, accumulated until
  // maybeShowRestartRequiredDialog() surfaces them and resets the count.
  int restart_pending_count_ = 0;
  // Failures accumulated across the current run of install activity (any mix of
  // Update All, queued card clicks and queued sideloads), so the summary shown
  // when everything settles reports them instead of ending on the last item's
  // "Installed X" — a failure must not be buried under a later success. Reset
  // once the batch settles (same gate as restart_pending_count_).
  int batch_failed_count_ = 0;
  // True while the restart-required MessageBox is up: its exec() spins a nested
  // event loop, so a completion arriving inside it must not open a second
  // dialog on top; the count it accumulates is surfaced right after.
  bool restart_dialog_open_ = false;
  // True while the invalid-registry MessageBox is up (its exec() spins a nested
  // event loop), so a burst of failed refreshes shows it once, not stacked.
  bool registry_error_dialog_open_ = false;
  // The concrete reason from the last RegistryManager::fetchError (a parse error
  // such as which entry is malformed, or a network error such as "Connection
  // refused"), shown as the Details line of the registry-load-error dialog.
  QString last_registry_error_;
  bool installations_changed_ = false;
  bool status_error_sticky_ = false;
  bool initial_snapshot_provided_ = false;
  // When this panel started. Diagnostics outlive it — they belong to the
  // ExtensionManager — so the Details button counts only the ones recorded since,
  // and a panel reopened after an install failure does not advertise it again.
  // The dialog behind the button still lists the full history.
  QDateTime opened_at_;
  // Last user-chosen sort state on one of the sortable columns (Name, Category,
  // Installed, Marketplace) — the header intercepts clicks on Description and
  // reverts to this. Default: Category ascending, which via CategoryItem's
  // compound comparator reads as "grouped by category, alphabetical within".
  int last_sort_column_ = 1;  // kColCategory
  Qt::SortOrder last_sort_order_ = Qt::AscendingOrder;
};

}  // namespace PJ
