#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/installed_extension.hpp"
#include "pj_marketplace/platform_utils.hpp"

namespace PJ {

class ArtifactStore;
class StoreSession;

// One user-visible diagnostic emitted by marketplace lifecycle operations.
struct ExtensionDiagnostic {
  QString id;       ///< Registry or embedded plugin id, when known.
  QString message;  ///< Human-readable diagnostic.
  bool is_error = false;
  QDateTime timestamp;  ///< UTC timestamp.
};

// Marketplace extension installs, updates, uninstalls and startup cleanup. Installed metadata comes from embedded DSO
// manifests, not sidecars.
//
// A managed store has ONE writer at a time across processes: construction takes an interprocess lock, and an instance
// that does not get it runs read-only: it still scans and reports installed state, but every mutation refuses with a
// diagnostic. See hasStoreWriteAccess().
class ExtensionManager : public QObject {
  Q_OBJECT

 public:
  // Creates an owned DownloadManager and uses the standard user directories.
  ExtensionManager();

  // Uses the supplied downloader and directories (tests pass isolated temp paths). The optional sink receives the same
  // diagnostics as diagnosticReported(), so a host can merge them with non-marketplace events into one stream.
  explicit ExtensionManager(
      DownloadManager* downloader, const QString& extensions_dir = PlatformUtils::extensionsDir(),
      const QString& pending_dir = PlatformUtils::pendingDir(), DiagnosticSink sink = {}, QObject* parent = nullptr);

  // Cancels all in-flight work, THEN releases the single-writer lock: an extraction worker writes into the store, so
  // releasing the lock first would hand the store to the next process mid-write. Blocks for as long as cancelling the
  // running extraction takes (the worker checks for cancellation at every archive entry and data block).
  // A downloader passed to the constructor stays the caller's and must outlive this object; only an owned one is
  // deleted here.
  ~ExtensionManager() override;

  // Starts an async install for the current platform.
  void install(const Extension& ext);

  // Host compatibility of a registry extension: the current platform must be listed in ext.platforms (when the record
  // declares any) AND this build's SDK must satisfy ext.min_sdk_required. min_plotjuggler_version never gates.
  // `reason` explains !ok for the UI. install() refuses an incompatible extension.
  struct HostCompatibility {
    bool ok = true;
    QString reason;
    /// Non-empty when compatible but suggested_sdk_version is newer than this build's SDK. Display only, never part of
    /// the gate.
    QString completeness_note;
  };
  HostCompatibility hostCompatibility(const Extension& ext) const;

  // The same gate for an extension known from its embedded manifest (a sideload or an extracted registry payload): ABI
  // major and min_sdk_required together. There is no platform half: a foreign binary fails discovery first. Local ZIP
  // admission, post-extraction validation, staged promotion, bundled seeding and runtime loading share one evaluator.
  HostCompatibility hostCompatibility(const InstalledExtension& installed) const;

  // Asked when a local archive carries an id that is already installed: return true to replace it, false to abort.
  // Invoked on the GUI thread from the handler that runs once extraction finishes, so it may block on a modal dialog,
  // but not while installFromLocalZip() is on the stack. The decision is UI policy and this module links no widgets,
  // so the host injects it. Left unset, a conflict is refused.
  using ReplaceConfirmation =
      std::function<bool(const QString& id, const QString& installed_version, const QString& archive_version)>;

  // Registers the decision on behalf of `context`, the object whose lifetime the callback's captures depend on. Only a
  // QPointer to it is kept: the question is asked once extraction finishes, possibly after the host window closed, and
  // calling a lambda that captured it would be a use-after-free. A confirmation whose context is gone resolves as
  // DECLINED and is dropped, so nothing is replaced unapproved and nothing waits for an answer that cannot come.
  // A null `context` or an empty `confirm` clears the registration. Only one is stored: a second registration displaces
  // the first, so with two marketplace windows every pending decision resolves against the last registrant.
  void setReplaceConfirmation(QObject* context, ReplaceConfirmation confirm);

  // Drops the registration only when `context` made it, so a window closing after another took over cannot disarm the
  // live one. Call it from the registrant's destructor.
  void clearReplaceConfirmation(QObject* context);

  // Sideloads a plugin from a local ZIP that the registry does not list. There is no registry entry: id and version
  // come from the archive's embedded manifest, and there is no checksum (a local file has no provenance). The gate
  // is the manifest scan (the DSO must load and describe itself coherently) plus the embedded ABI and
  // min_sdk_required, enforced before anything is written.
  //
  // The archive must hold exactly one top-level directory containing the plugin. On success it lands in extensionsDir()
  // keyed by the discovered id, the root every install writes to, so a sideload follows `--plugin-dir` like a registry
  // install. This manager never loads it: the host reloads its catalog after a fresh install.
  //
  // If the id is ALREADY installed, setReplaceConfirmation() decides. A confirmed replacement is staged, not written
  // over the existing directory (a loaded DSO cannot be swapped: Windows locks the file, and dlopen keys its cache by
  // path); applyPendingInstalls() promotes it at the next launch, backing up the displaced version, and
  // installPendingRestart() is emitted instead of installFinished(). update()'s version comparison is skipped:
  // rebuilding without a version bump is normal. An archive byte-identical to the installed tree is reported through
  // installUnchanged() and changes nothing.
  //
  // Reports through installStarted / installFinished / installError like install(), except that installStarted comes
  // only once the id is known (after the manifest is read).
  void installFromLocalZip(const QString& zip_path);

  // Stages the removal: journalled outside the payload, not performed now, since the DSO is still loaded. Emits
  // uninstallPendingRestart(), NOT uninstallFinished().
  //
  // Disables the id first, persisted with the sync CHECKED, before the journal record is written: the loader decides
  // what to load from the disabled list alone, so an enabled id would load again from a directory that survived the
  // drain. A step that cannot be persisted rolls the enable state back and fails. The disabled entry stays after the
  // files are gone, as a tombstone: a reinstall clears it (registerInstalledExtension), and keeping it means journal
  // processing never needs authority to enable an id.
  void uninstall(const QString& extension_id);

  // Reverts a core extension updated ABOVE its bundled version to the shipped one. Staged: the updated copy is
  // journalled for removal (the row shows "Needs Restart") and downgradePendingRestart() is emitted; at the next
  // launch applyPendingUninstalls() removes it and the host seed restores the bundled version. Reports failure through
  // uninstallError() when `id` is not installed, not core, or not above its bundled version.
  void downgradeToBundled(const QString& extension_id);

  // Replaces an installed extension: stages the new version in pending_dir_; applyPendingInstalls() promotes it
  // (backing up the old one) at the next startup, so a loaded DSO is never hot-swapped.
  void update(const Extension& ext);

  // Promotes validated staged installs from the pending directory.
  void applyPendingInstalls();

  // Drains the removal journal: deletes each recorded directory and drops the record only once the directory is gone.
  // One that resists keeps its record (retried next launch) and reports a diagnostic, partial deletion included: the
  // record lives outside the payload. Every record is validated first (schema, exact operation, id matching its file,
  // target a direct child of the canonical store root); a failing record is quarantined and reported, never acted on.
  // Draining never enables an id, so no record can make a plugin loadable. Requires the store write lease; a read-only
  // instance leaves the work alone.
  void applyPendingUninstalls();

  // Returns true when the latest disk scan found this extension id.
  bool isInstalled(const QString& id) const;

  // id -> version of the plugins that ship with the application ("core"), computed by the host from the bundled
  // directory. Membership gates uninstall (so it survives updates and needs no on-disk flag); the version enables
  // "downgrade to bundled".
  void setBundledVersions(const QMap<QString, QString>& id_to_version);

  // id -> version of the copies the startup seed WROTE into the managed dir this session, so installedVersion() can
  // report them: the scan snapshot predates the write and cannot be refreshed (see installedVersion). Distinct from
  // setBundledVersions(), which names every shipped plugin whether or not the seed touched it. Only ids whose payload
  // was promoted belong here. The host reports it in every mode: it states a fact about the managed dir, not policy.
  void setSeededVersions(const QMap<QString, QString>& id_to_version);

  // True when `id` ships with the application ("core"). At its bundled version it cannot be uninstalled (uninstall()
  // refuses, the UI disables the action); above it, it can be reverted with downgradeToBundled(). False when no
  // bundled set was provided (standalone app, --plugin-dir).
  bool isBundled(const QString& id) const;

  // The version `id` ships with, or empty if `id` is not core.
  QString bundledVersion(const QString& id) const;

  // ── Enable/disable (installed but not loaded) ──────────────────────────────
  // A disabled extension stays on disk but the host skips loading it. The state persists globally in QSettings, read by
  // both the marketplace and the runtime catalog, and applies on the next launch (a loaded DSO cannot be unloaded).

  // True when `id` is NOT in the disabled list (independent of installed state).
  bool isEnabled(const QString& id) const;

  // Adds/removes `id` from the persisted disabled list; refused with a diagnostic in a read-only session. Does not
  // touch the already-loaded plugin.
  void setEnabled(const QString& id, bool enabled);

  // The persisted set of disabled extension ids, read straight from QSettings. Static so the runtime catalog can honor
  // it without an instance (the single source of truth for the key).
  static QStringList disabledExtensionIds();

  // Rebuilds installed state by scanning extension directories for plugin DSOs.
  void refreshInstalledFromDisk();

  // Replaces the installed-state cache with a caller-provided snapshot.
  void setInstalledExtensions(QMap<QString, InstalledExtension> installed);

  // Returns true when a staged install has a matching intent and valid DSO.
  bool hasPendingInstall(const QString& id) const;

  // True when a removal of `id` is staged (journalled, or marked inside the payload).
  bool hasPendingUninstall(const QString& id) const;

  // Records of the extensions whose removal is staged: no longer in installedExtensions(), yet still on disk and
  // running until the restart. A UI composing rows from the installed set alone would show nothing pending, most of
  // all for an extension the registry does not list (it cannot supply a replacement row). Pruned once the removal is no
  // longer scheduled.
  QMap<QString, InstalledExtension> stagedUninstalls() const;

  // The version of `id` actually on disk; prefer it over installedExtensions()[id].version, which can be stale in one
  // window. The startup seed reads a core plugin's installed version (opening the DSO) before deciding to write its
  // managed copy, and glibc then answers every later dlopen of that path from the first image (docs/TECHNICAL_NOTES.md
  // "dlopen pins the first image"), so no rescan in this process can see what the seed promoted. The seed therefore
  // reports what it wrote through setSeededVersions() and this returns that value verbatim for those ids. Inferring it
  // from the bundled set would break on the seed's own downgrade case: restoring an incompatible installed copy leaves
  // the managed dir BELOW the scanned version.
  //
  // Empty when `id` is not installed. An id the seed did not write falls through to the scanned version.
  QString installedVersion(const QString& id) const;

  // True when the installed version is newer than the registry version.
  bool hasNewerInstalledVersion(const Extension& ext) const;

  // True when the registry version is newer than the installed one.
  bool hasUpdate(const Extension& ext) const;

  // Returns the current installed-extension snapshot keyed by id.
  QMap<QString, InstalledExtension> installedExtensions() const;

  // Returns recent lifecycle diagnostics for UI display.
  QList<ExtensionDiagnostic> diagnostics() const;

  // Clears the in-memory diagnostic history.
  void clearDiagnostics();

  // True when a diagnostic has been recorded that markDiagnosticsSurfaced() has not covered. Diagnostics persist across
  // the per-open window instances, so this lets the UI show one exactly once.
  bool hasUnsurfacedDiagnostics() const;

  // Marks every diagnostic recorded so far as surfaced, so hasUnsurfacedDiagnostics()
  // returns false until a newer one arrives.
  void markDiagnosticsSurfaced();

  // Root directory where extension DSOs are discovered and managed.
  QString extensionsDir() const {
    return extensions_dir_;
  }

  // True when this instance holds the store's single-writer lock and may install, update, uninstall, enable/disable and
  // run the startup cleanup. False means another live process owns the store: the session is read-only and a UI should
  // disable those actions up front.
  bool hasStoreWriteAccess() const;

  // One plugin directory per managed extension for the runtime to scan: the store object when it backs the id and its
  // bytes still verify against the recorded content manifest, else extensions/<id>/. Enumerated from the managed dir on
  // disk (so a freshly seeded plugin is included), one entry per id, so no plugin is discovered twice. This is the
  // managed tier of the scan hierarchy; authoritative folders stay separate.
  //
  // Not const: a resident object that fails verification is skipped in favour of the local copy and a diagnostic is
  // reported.
  std::vector<QString> managedPluginDirs();

  // Forgets the store object recorded for `id`, so managedPluginDirs() falls back to extensions/<id>/ and
  // reconcileStore() collects the orphan. A writer that changes extensions/<id>/ outside the adopt path (the bundled
  // seed) must call this, or the runtime keeps loading the stale object.
  void dropStoreMapping(const QString& id);

#ifdef PJ_MARKETPLACE_TESTING
  // Test hook for forcing direct or staged install paths.
  void testDoInstall(const Extension& ext, bool staging, bool allow_existing = false) {
    doInstall(ext, staging, allow_existing);
  }

  // Test hook: the startup sequence a restart would run after an uninstall drained the extension directory.
  void testReconcileFromDisk() {
    refreshInstalledFromDisk();
    reconcileStore();
  }

#endif

 signals:
  // Emitted when an install or update starts.
  void installStarted(const QString& id);

  // Emitted with percentage progress for the active download.
  void installProgress(const QString& id, int percent);

  // Post-download work not covered by installProgress; see DownloadManager::WorkPhase. Consumers should switch to a
  // busy indicator.
  void installPhase(const QString& id, PJ::DownloadManager::WorkPhase phase);

  // Emitted when install or update completes.
  void installFinished(const QString& id, bool success);

  // Human-readable failure detail; followed by installFinished(id, false).
  void installError(const QString& id, const QString& error_message);

  // Emitted when an update is staged and will be active after a restart. A fresh
  // install promotes immediately and emits installFinished instead.
  void installPendingRestart(const QString& id);

  // A sideloaded archive held exactly what is already installed: nothing was replaced, staged or written. Emitted
  // INSTEAD of installFinished so consumers do not announce a change that did not happen; no catalog reload is
  // warranted.
  void installUnchanged(const QString& id);

  // Emitted when uninstall completes.
  void uninstallFinished(const QString& id, bool success);

  // Human-readable uninstall failure detail.
  void uninstallError(const QString& id, const QString& error_message);

  // Emitted when uninstall requires restart cleanup.
  void uninstallPendingRestart(const QString& id);

  // Emitted when a core extension is staged to revert to its bundled version on the
  // next launch (downgradeToBundled). The row shows "Needs Restart".
  void downgradePendingRestart(const QString& id);

  // Emitted whenever a diagnostic is appended to diagnostics().
  void diagnosticReported(const QString& id, const QString& message, bool is_error);

 private:
  // How a replace conflict was resolved. Everything except kAccepted aborts the install; they differ only in what the
  // user is told.
  enum class ReplaceDecision : std::uint8_t {
    kNoConfirmation,  ///< Nothing registered, so a conflict is refused outright.
    kOwnerGone,       ///< The registrant died before or during the answer.
    kSuperseded,      ///< The registration changed while the question was open.
    kDeclined,
    kAccepted,
  };

  // Puts the replace question to the registered confirmation behind its context guard. A dead guard yields kOwnerGone
  // without invoking the callback and drops the registration. The callback typically blocks on a modal dialog (the
  // event loop spins), so another window can register or clear meanwhile: the callable is copied before invoking
  // (assigning to the executing std::function is undefined) and the answer is matched to the registration that asked
  // via (context, replace_confirmation_generation_), so a displaced registration's answer is kSuperseded, never
  // accepted.
  ReplaceDecision askReplaceConfirmation(const QString& id, const QString& archive_version);

  // Clears the registration and bumps the generation so an in-flight answer can no longer be accepted; every drop goes
  // through here.
  void dropReplaceConfirmation();

  void initComponents();

  // Takes the store's single-writer lock, or leaves this instance read-only and
  // reports why. Must run before any startup cleanup.
  void acquireStoreLock();

  // Empty when this instance may write to the store; otherwise the user-facing
  // reason a mutation was refused, for the caller's own failure channel.
  QString storeWriteRefusal() const;

  // Shared install implementation for direct and staged destinations.
  void doInstall(const Extension& ext, bool staging, bool allow_existing = false);

  void disconnectDlConns();

  // Converts a `.pj_pending_uninstall` marker inside a payload into a path-only journal record, so the retry survives a
  // deletion that fails part-way. The marker's content is package-controlled and never read: the record carries no id,
  // hence no authority over any extension's enable state.
  void externalizeLegacyUninstallMarkers();

  // Read-modify-writes the persisted disabled list for one id and reports whether it reached the backing store. All on
  // a SINGLE QSettings instance: status() reflects only the object that wrote (notably a Windows registry write), so a
  // check through another instance would lose a failure. Staging a removal depends on this entry surviving a crash.
  bool writeDisabledState(const QString& id, bool enabled);

  // Puts `id` back to `enabled` after a failed staging step; reports a diagnostic if the rollback cannot be persisted.
  void restoreEnabledState(const QString& id, bool enabled);

  // Appends a diagnostic and notifies observers.
  void reportDiagnostic(const QString& id, const QString& message, bool is_error);

  // Emits installError + installFinished(false) and records a diagnostic.
  void emitInstallFailure(const QString& id, const QString& message);

  // Stamps a freshly promoted directory with its path and mtime and adds it to installed_ under `id`; the caller
  // supplies the record populated from the embedded manifest. A fresh install (default) starts enabled and clears a
  // stale disabled entry; a staged update/replace passes preserve_disabled_state=true so a plugin the user disabled
  // does not come back enabled.
  // the user disabled must not silently come back enabled after its update.
  void registerInstalledExtension(
      const QString& id, const QString& dst, InstalledExtension record, bool preserve_disabled_state = false);

  // Best-effort: adopts the installed tree into the content-addressed store and records its digest in loaded_digests_
  // (and the persisted profile) so the runtime can load it from the store. Never fails an install: a store that is
  // absent, read-only or rejects the tree drops the id from the profile and the runtime falls back to extensions/<id>/.
  void shadowAdoptIntoStore(const QString& id, const QString& dir);

  // Rewrites loaded.json from loaded_digests_. No-op without a writable store session. Best-effort: a failed write only
  // costs ids installed this session their store shortcut.
  void persistLoadedProfile();

  // Brings the store in step with what is installed: drops profile entries whose extensions/<id>/ is gone, then deletes
  // every object no entry references. Runs in initComponents before the runtime maps any object, so removal is safe.
  // Skips the sweep when the profile is empty while installs exist (a profile that failed to load: an empty root set
  // would erase the whole store). No-op without a writable store session.
  void reconcileStore();

  // Backs up (or removes) any directory under extensions_dir_, other than `keep_dir`, whose embedded plugin id is `id`,
  // so the promoted "<id>" directory is its sole install: a prior copy under another directory name (a bundled plugin
  // in "data-load-foo" for id "foo") would otherwise linger and be resolved by directory name. The scan opens sibling
  // DSOs, so during the startup drain run it only once every staged promotion has landed (see applyPendingInstalls).
  void replaceConflictingInstallDirs(const QString& id, const QString& keep_dir);

  // Emits uninstallError + uninstallFinished(false) and records a diagnostic.
  void emitUninstallFailure(const QString& id, const QString& message);

  // Deletes every `.pj_install_*` directory directly under `parent` except the one a running install extracts into.
  // Called for the staging sibling and for the extensions dir itself, where one would be scanned as content.
  // Writer-only: a transaction it does not recognise belongs to the lease holder.
  void sweepTransactionRoots(const QString& parent);

  // The store session opened at construction; present after initComponents. hasStoreWriteAccess() reports whether it
  // holds the lease. Released last (see ~ExtensionManager); artifact_store_ references it.
  std::unique_ptr<StoreSession> store_session_;
  // Why the lease is not held, classified once at acquisition (contention with a live instance and an unwritable lock
  // file call for different user actions). Empty exactly when the lease is held.
  QString store_lock_refusal_;

  // Content-addressed home of what gets installed (see managedPluginDirs, reconcileStore). Null when the store could
  // not be opened: installs proceed and the runtime scans extensions/<id>/.
  std::unique_ptr<ArtifactStore> artifact_store_;

  // id -> store-object digest for every installed extension, mirroring loaded.json: read at construction, updated on
  // every adopt, the source of managedPluginDirs() and of reconcileStore()'s root set. An entry exists only while the
  // store holds an object matching the current install: a failed adopt removes it.
  QMap<QString, QString> loaded_digests_;

  DownloadManager* downloader_ = nullptr;
  // True only for the downloader initComponents() created: the only one this object destroys, before the lock goes.
  bool owns_downloader_ = false;
  QString extensions_dir_;
  ReplaceConfirmation replace_confirmation_;
  // Lifetime guard for replace_confirmation_: null once the registrant is gone, which turns a pending decision into a
  // decline.
  QPointer<QObject> replace_confirmation_context_;
  // Identifies WHICH registration is armed, bumped by every set and drop: the question is answered inside a modal loop,
  // so the slot may hold a different live registration by then (see askReplaceConfirmation()).
  quint64 replace_confirmation_generation_ = 0;
  QString pending_dir_;
  DiagnosticSink sink_;

  QMap<QString, InstalledExtension> installed_;

  // id -> the record it had when its uninstall was staged. Outlives the window (this manager does); a restart needs no
  // entry, since the drain removes the extension.
  QMap<QString, InstalledExtension> staged_uninstalls_;

  // id -> version of the plugins that ship with the application, set via setBundledVersions().
  QMap<QString, QString> bundled_versions_;

  // id -> version the startup seed wrote into the managed dir this session, set via setSeededVersions().
  QMap<QString, QString> seeded_versions_;

  // Non-empty while a fetch is running; guards against concurrent install() calls.
  QString pending_id_;
  // ID returned by DownloadManager::fetch(); used to correlate incoming signals.
  int pending_op_id_ = -1;
  // Ensures the disk-space check runs at most once per fetch operation.
  bool disk_space_checked_ = false;
  // Set before calling cancel() to preserve the real reason shown to the user.
  QString cancel_reason_;
  // Transaction directory used by the currently running fetch/extract operation.
  QString pending_extract_dir_;
  // Backup location of the previous install, set when applyPendingInstalls()
  // promotes a staged update; used for failure diagnostics.
  QString pending_backup_path_;
  QList<ExtensionDiagnostic> diagnostics_;
  // Count of every diagnostic ever recorded (the kMaxDiagnostics trim does not reset it) and of those surfaced:
  // recorded > surfaced means an unsurfaced diagnostic.
  quint64 diagnostics_recorded_ = 0;
  quint64 diagnostics_surfaced_ = 0;

  // For disconnecting cleanly after each operation completes.
  QMetaObject::Connection dl_progress_conn_;
  QMetaObject::Connection dl_phase_conn_;
  QMetaObject::Connection dl_finished_conn_;
  QMetaObject::Connection dl_failed_conn_;
  QMetaObject::Connection dl_cancelled_conn_;
};

}  // namespace PJ
