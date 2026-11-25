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

// The marketplace UI on the app's own chrome (PJ::Dialog: frameless, no window-manager decorations).
class MarketplaceWindow : public Dialog {
  Q_OBJECT

 public:
  explicit MarketplaceWindow(const QUrl& registry_url, QWidget* parent = nullptr);

  // Uses an externally owned ExtensionManager (the host's own).
  explicit MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent = nullptr);

  // As above, with an installed snapshot that seeds the manager before the first render; the first activation then
  // skips the disk rescan.
  explicit MarketplaceWindow(
      ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
      QWidget* parent = nullptr);

  ~MarketplaceWindow() override;

  // True when install state changed while the window was open (the host reloads its plugin catalog then).
  bool installationsChanged() const {
    return installations_changed_;
  }

  // The whole UI body, for a host that embeds the marketplace as a panel or tab instead of showing this window:
  // reparent it and keep this object alive as the controller (it owns the UI and receives the child-widget signals).
  QWidget* contentWidget() const {
    return content_widget_;
  }

  // The manager behind every fetch this window makes. Its first fetch starts on the next event-loop turn, so a host
  // that connects right after construction hears about it.
  RegistryManager* registryManager() const {
    return registry_mgr_;
  }

  // Refreshes installed state and repaints: the embedded equivalent of showEvent(). Call after embedding
  // contentWidget().
  void activateEmbedded();

  // Points the window at a different registry and re-fetches it. The URL is captured once at construction, so a panel
  // left open keeps its registry until the host pushes a new one: registry-URL policy stays with the host. No-op when
  // the URL is unchanged.
  void setRegistryUrl(const QUrl& registry_url);

 signals:
  // Emitted by the settings (gear) button; the host opens Preferences ▸ Plugins. The marketplace has no dependency on
  // the preferences UI.
  void pluginPreferencesRequested();

 protected:
  // Refreshes installed state before the table is repainted.
  void showEvent(QShowEvent* event) override;

 private slots:
  void onSearchChanged(const QString& text);

  // Re-applies the filter row after any toggle flips: the toggles are read as a set, so which one is irrelevant.
  void onFilterToggled();

  // Picks a local ZIP and sideloads it via ExtensionManager::installFromLocalZip.
  void onInstallLocalClicked();

  // Queues updates for every installed extension with a newer, compatible registry version.
  void onUpdateAllClicked();

  // Opens a read-only view of the diagnostics history.
  void onDiagnosticsClicked();

  // Installs or updates one extension.
  void onActionButtonClicked(const QString& ext_id);

  // Stages the uninstall of one installed extension.
  void onUninstallButtonClicked(const QString& ext_id);

 private:
  // Shared constructor tail: UI, signals, optional installed snapshot, initial diagnostics, first registry fetch.
  void finishConstruction(const QMap<QString, InstalledExtension>* installed);

  void setupUi();
  void setupSignals();

  // Rebuilds every table row from filtered_. preserve_scroll keeps the scroll offset (install/update/uninstall
  // repaints); pass false when the row set changes meaning (filter, search, registry reload).
  void rebuildTable(bool preserve_scroll = true);

  // Recomposes extensions_ as registry_extensions_ plus one synthesized row per installed, or staged-for-removal,
  // extension the registry does not list, so a sideloaded plugin stays visible and manageable. Recomputed, not
  // appended to, so a repeated install cannot duplicate a row. Returns whether the id set changed.
  bool rebuildExtensionList();

  // Repaints after the installed set may have changed; re-filters only when a row appeared or disappeared, otherwise
  // the repaint keeps the scroll position.
  void refreshAfterInstalledChange();

  // Applies the search and the filter row to extensions_ and rebuilds the table.
  void applyFilters();

  // Updates the status label; error statuses remain sticky until a user action clears them.
  void setStatus(const QString& msg, bool is_error = false);

  // Allows the next non-error status update to replace an error.
  void clearStickyStatus();

  // Shows the newest diagnostic in the status bar if it has not been surfaced yet.
  void showLatestDiagnostic();

  // Shows the diagnostics button when diagnostics were recorded since this panel opened.
  void updateDiagnosticsButton();

  // Rebuilds the bottom Details footer (metadata, description, changelog, action buttons) for the selected row; no
  // selection clears it.
  void updateDetailFooter();

  // Dispatches the next queued install: button clicks first, then local ZIPs, then the Update All batch.
  void processInstallQueue();

  // Dispatches a local-ZIP sideload and marks it in flight. Takes the path BY VALUE: a synchronous failure inside
  // installFromLocalZip() reaches installFinished, which clears local_install_path_ while the callee still reads its
  // argument.
  void startLocalInstall(QString zip_path);

  // True while ExtensionManager is busy with any install/update: a registry op (active_install_id_) or a local-ZIP
  // sideload, which sets no active_install_id_. New actions queue instead of hitting the manager's
  // one-install-at-a-time guard.
  [[nodiscard]] bool isInstallBusy() const {
    return !active_install_id_.isEmpty() || !local_install_path_.isEmpty();
  }

  // Shows the in-flight install with the queue depth ("Installing mcap…  ·  2 queued"); `verb` is the phase word.
  // No-op when nothing is active, so it never clobbers a terminal "Installed"/"Failed" message. A sideload is named
  // by its file name until its manifest gives an id.
  void showInstallProgress(const QString& verb = tr("Installing"));

  // "  ·  N queued" for the combined depth of every queue, then "  ·  M failed" once this run has failures. Empty
  // when nothing is waiting and nothing has failed.
  QString queueSuffix() const;

  // Success message for a finished install; `from_file` selects the sideload phrasing (version from the installed
  // snapshot, the id being absent from the registry list). Neither mentions a restart: staged outcomes report through
  // installPendingRestart.
  QString installedStatusText(const QString& id, bool from_file) const;

  // Shows an informational status, unless an install is in flight: the status line then belongs to that operation, so
  // its progress is re-asserted. Errors go through setStatus().
  void setInfoStatus(const QString& msg);

  // Shows the restart-required dialog once the queue has settled and at least one operation was staged. An Update All
  // run yields one dialog, not one per staged item.
  void maybeShowRestartRequiredDialog();

  // Explains that the registry could not be loaded (only installed extensions are shown). Consequence-only wording,
  // since it fires for parse and network failures alike; the cause goes in the Details line (last_registry_error_).
  // The red status line alone is easy to miss. registry_error_dialog_open_ stops a burst of failed refreshes from
  // stacking dialogs.
  void showInvalidRegistryDialog();

  // Once all install activity has settled, replaces the last item's success status with a failure summary so a
  // mid-batch failure is not buried under a later success. No-op while anything is in flight or nothing failed.
  // Resets batch_failed_count_.
  void maybeShowBatchSummary();

  // Total depth of every queue (button clicks, local ZIPs, Update All): the one place that enumerates them.
  [[nodiscard]] int queuedCount() const {
    return static_cast<int>(pending_clicks_.size() + pending_local_zips_.size() + update_queue_.size());
  }

  // True when nothing is installing or queued, sideloads included: the gate for the restart dialog and the batch
  // summary.
  [[nodiscard]] bool installBatchSettled() const {
    return !isInstallBusy() && queuedCount() == 0;
  }

  Ui::MarketplaceWindow* ui_ = nullptr;
  QWidget* content_widget_ = nullptr;  ///< the UI body; exposed for embedding
  DownloadManager* download_mgr_ = nullptr;
  RegistryManager* registry_mgr_ = nullptr;
  // Guarded: the manager belongs to a service MainWindow holds as a member, and members die before ~QWidget deletes
  // this child, so at shutdown it may already be gone.
  QPointer<ExtensionManager> ext_mgr_;
  QUrl registry_url_;

  // Registry rows as fetched, apart from extensions_ so the local-only rows can be recomposed without duplicates.
  QList<Extension> registry_extensions_;
  QList<Extension> extensions_;  // registry_extensions_ + synthesized local-only rows
  QList<Extension> filtered_;
  QList<Extension> update_queue_;
  // Install/Update clicks that arrive while another install runs; drained FIFO by processInstallQueue().
  QList<QString> pending_clicks_;
  // Local-ZIP paths that arrive while another install runs; drained after pending_clicks_. Canonical, so one file
  // reached by different spellings is one entry.
  QList<QString> pending_local_zips_;
  // Id being installed or updated by the manager (from installStarted until installFinished); empty means idle.
  QString active_install_id_;
  // Id of the row the footer shows; kept across table rebuilds so a repaint keeps the same row selected.
  QString footer_ext_id_;
  // Canonical path of the local-ZIP sideload in flight, empty when none. Outcome handlers phrase their message from
  // the installed snapshot (the id is absent from the registry list); it also dedupes a re-pick and names the status
  // line before installStarted supplies an id.
  QString local_install_path_;
  // Staged operations (updates, sideload replacements, staged uninstalls and downgrades) waiting for a restart, until
  // maybeShowRestartRequiredDialog() surfaces and resets them.
  int restart_pending_count_ = 0;
  // Failures accumulated across the current run of install activity, so the summary shown when everything settles
  // does not end on the last item's "Installed X". Reset with restart_pending_count_.
  int batch_failed_count_ = 0;
  // True while the restart-required MessageBox is up: its exec() spins a nested event loop, so a completion arriving
  // inside it must not open a second dialog; the accumulated count is surfaced right after.
  bool restart_dialog_open_ = false;
  // True while the invalid-registry MessageBox is up (nested event loop), so failed refreshes do not stack dialogs.
  bool registry_error_dialog_open_ = false;
  // The reason from the last RegistryManager::fetchError, shown as the Details line of the registry-load-error dialog.
  QString last_registry_error_;
  bool installations_changed_ = false;
  bool status_error_sticky_ = false;
  bool initial_snapshot_provided_ = false;
  // When this panel was opened. Diagnostics outlive it (they belong to the ExtensionManager), so the Details button
  // counts only those recorded since; the dialog behind it lists the full history.
  QDateTime opened_at_;
  // Last user-chosen sort of a sortable column (Name, Category, Installed, Marketplace); a header click on Description
  // reverts to it. Default: Category ascending (grouped by category, alphabetical within).
  int last_sort_column_ = 1;  // kColCategory
  Qt::SortOrder last_sort_order_ = Qt::AscendingOrder;
};

}  // namespace PJ
