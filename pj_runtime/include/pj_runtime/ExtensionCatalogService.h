#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_runtime/PluginRuntimeCatalog.h"

namespace PJ {

class ExtensionManager;

using LoadedDataSource = RuntimeDataSourcePlugin;
using LoadedMessageParser = RuntimeMessageParserPlugin;
using LoadedToolbox = RuntimeToolboxPlugin;

// Bundles the marketplace (ExtensionManager) with the plugin catalog (per-family
// loaders from pj_plugins) behind one Qt-friendly facade. `pj_app` never touches
// pj_marketplace or pj_plugins directly — it asks this service.
//
// The service owns an ExtensionManager rooted on `extensions_dir_` (by default
// PlatformUtils::extensionsDir(), shared with pj_marketplace). Construction runs
// three steps in order: the ExtensionManager applies any pending staged
// install/uninstall actions, seedBundledPlugins() syncs the bundled (share)
// plugins into the default marketplace dir, then the scan hierarchy loads. The
// bundled dir is a seed source only — never scanned as a load path. Call
// reload() after a marketplace install/uninstall to hot-load new plugins.
class ExtensionCatalogService : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<ExtensionCatalogService>;

  // Directory roots, resolved at construction. Production uses the QString
  // constructors below (everything defaulted); tests inject temp dirs here so
  // seeding and scanning never touch the real user profile or executable path.
  struct Paths {
    // The dir the ExtensionManager manages and the top scan tier: the
    // --plugin-dir override. Empty = default mode (the marketplace dir).
    QString install_dir;
    // Seed destination and lowest (managed) scan tier. Empty =
    // PlatformUtils::extensionsDir().
    QString marketplace_dir;
    // Seed source (share). Empty = <prefix>/lib/plotjuggler/plugins resolved
    // relative to the executable.
    QString bundled_dir;
  };

  // Creates a service using the default extension directory unless overridden.
  explicit ExtensionCatalogService(QString extensions_dir = {}, QObject* parent = nullptr);

  // Creates a service with an optional app-level diagnostic sink.
  ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  // Creates a service with every directory root injectable (test seam).
  ExtensionCatalogService(Paths paths, DiagnosticSink sink, QObject* parent = nullptr);

  // Creates a service and registers application-composed static plugins before
  // the first directory scan. This is the startup seam used by static/WASM
  // binaries; desktop callers normally use the overload above.
  ExtensionCatalogService(
      QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  // Combines the directory-root test seam with application-composed static
  // plugins. The vtable/dialog pointers inside the set must have static storage
  // duration — the catalog retains them for its lifetime.
  ExtensionCatalogService(Paths paths, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  // Releases marketplace and loaded plugin resources.
  ~ExtensionCatalogService() override;

  // ExtensionCatalogService owns loaded plugin libraries and cannot be copied.
  ExtensionCatalogService(const ExtensionCatalogService&) = delete;

  // ExtensionCatalogService owns loaded plugin libraries and cannot be assigned.
  ExtensionCatalogService& operator=(const ExtensionCatalogService&) = delete;

  // Reference valid for the service's lifetime. Browser builds have no dynamic
  // marketplace; their plugins are registered statically through pluginCatalog.
#ifndef PJ_TARGET_WASM
  ExtensionManager& extensionManager() const {
    return *extension_manager_;
  }
#endif

  // Direct access to the loaded plugin catalog (reference valid for the
  // service's lifetime). Tests use it to register in-process plugins without
  // standing up DSO scanning. Do NOT register plugins through it after
  // streaming has started: unlike reload(), a raw registration takes no
  // catalog lock, and poll threads walk the parser set concurrently —
  // application-composed static plugins belong in the constructor set.
  PluginRuntimeCatalog& pluginCatalog() {
    return *plugin_catalog_;
  }

  // Returns the directory where extension DSOs are loaded from.
  QString extensionsDir() const {
    return extensions_dir_;
  }

  // Reconciles the loaded plugin catalog with files on disk.
  void reload();

  // Returns all loaded DataSource plugins.
  const std::vector<LoadedDataSource>& dataSources() const;

  // Returns all loaded MessageParser plugins.
  const std::vector<LoadedMessageParser>& messageParsers() const;

  // Returns all loaded Toolbox plugins.
  const std::vector<LoadedToolbox>& toolboxes() const;

  // Returns file-import capable DataSource plugins.
  std::vector<const LoadedDataSource*> fileImportSources() const;

  // Returns streaming-capable DataSource plugins.
  std::vector<const LoadedDataSource*> streamSources() const;

  // Finds file-import DataSources that handle ext.
  std::vector<const LoadedDataSource*> findSourcesForExtension(QStringView ext) const;

  // Finds a MessageParser by encoding name. Returns a raw pointer INTO the
  // catalog vector, valid only until the next reload(). GUI-THREAD ONLY — a
  // concurrent reload() (which reallocates the vector) would dangle it. Off-GUI
  // callers (a streaming source's poll thread) must use
  // createParserHandleForEncoding()/parserEncodings() instead, which resolve
  // under the catalog lock and never leak a raw catalog pointer.
  const LoadedMessageParser* findParserByEncoding(QStringView encoding) const;

  // [thread-safe] Resolve a parser by encoding and create an instance of it,
  // atomically under a shared catalog lock. The returned MessageParserHandle
  // carries its own DSO keepalive, so it stays valid even if a later reload()
  // drops the catalog entry. An invalid handle (`!valid()`) means no parser
  // handles that encoding. This is the ONLY safe way for a non-GUI thread to
  // obtain a parser while reload() may run on the GUI thread.
  [[nodiscard]] MessageParserHandle createParserHandleForEncoding(QStringView encoding) const;

  // [thread-safe] The set of encodings the loaded parsers accept, returned by
  // value (a snapshot copy) so a caller never holds a reference into the
  // catalog vector across a reload(). Sorted, de-duplicated.
  [[nodiscard]] std::vector<std::string> parserEncodings() const;

  // Builds a QFileDialog-compatible filter string from all file-import sources.
  QString buildFileFilter() const;

  // User-managed extra plugin folders, persisted in QSettings
  // (Preferences::plugin_folders). Authoritative scan tier below a --plugin-dir
  // override and above the marketplace dir. Changes apply on next launch (no
  // hot reload), so the setter only writes the key — it does not re-scan.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);

  // Built-in *scanned* folders in scan-priority order: the install dir (the
  // --plugin-dir override, or the marketplace dir in default mode) and, when
  // the override made it distinct, the marketplace dir. The bundled (share)
  // dir is not listed — it is a seed source, not a scanned folder. Read-only —
  // shown to the user for reference.
  [[nodiscard]] QStringList builtinPluginFolders() const;

  // Empty if the host would accept this plugin at load time; otherwise a
  // human-readable reason it would be rejected. The seed uses this to decide
  // whether an installed copy above the bundled version can survive: a
  // non-empty reason triggers the rescue path (overwrite with the bundled
  // build, compatible by construction).
  //
  // Three gates, evaluated by the same headless primitive as marketplace
  // admission and PluginRuntimeCatalog:
  //   - ABI: `abi_major` (baked into the manifest at build time by the SDK's
  //     CMake helper) must equal the host's `PJ_ABI_VERSION`. A zero
  //     `abi_major` means the manifest predates the field; treat it as
  //     "unknown → assume compatible" (the load path's own abi symbol check
  //     will catch a real mismatch there).
  //   - `min_sdk_required`: the SDK used by the host must be at least the
  //     concrete SemVer contract floor. Empty means undeclared.
  //   - `min_plotjuggler_version`: the host must be at least the declared
  //     concrete SemVer minimum. An empty value is no floor.
  //
  // The reason string mirrors ExtensionManager::hostCompatibility's wording
  // so seed diagnostics and marketplace UI say the same thing.
  //
  // Static + descriptor-taking so the seed can call it without an
  // ExtensionCatalogService instance and tests can drive it with any host
  // version, not only QCoreApplication::applicationVersion().
  [[nodiscard]] static QString descriptorIncompatReason(
      const PluginDescriptor& descriptor, const QString& host_version);

  // Convenience predicate — a descriptor is compatible when its incompat
  // reason is empty. Delegates to descriptorIncompatReason so a future third
  // gate cannot drift between the two paths.
  [[nodiscard]] static bool descriptorIsCompatibleWithHost(
      const PluginDescriptor& descriptor, const QString& host_version) {
    return descriptorIncompatReason(descriptor, host_version).isEmpty();
  }

 signals:
  // Emitted after reload() changes the loaded plugin set.
  void catalogChanged();

 private:
  // Emits one diagnostic through the optional app-level sink.
  void reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id = {}) const;

  // Assembles the ordered scan list — highest priority first: the --plugin-dir
  // override (when extensions_dir_is_explicit), the custom Preferences folders,
  // then the marketplace dir. The first two tiers are user-explicit and marked
  // authoritative (a hard override in the catalog's duplicate-id resolution,
  // version-blind); the marketplace dir stays managed. The bundled (share) dir
  // is never scanned — bundled plugins reach the scan through
  // seedBundledPlugins(). Folders missing on disk are skipped: an absent
  // optional folder must not report a kError per launch and mask real
  // plugin-load errors.
  [[nodiscard]] std::vector<PluginDirEntry> buildScanHierarchy(bool extensions_dir_is_explicit) const;

  // Syncs the bundled (share) plugins into the default marketplace dir — the
  // only path by which bundled plugins become loadable. Runs in EVERY mode
  // (--plugin-dir sessions included; the override dir is never a seed source or
  // destination), AFTER the ExtensionManager applied pending staged installs,
  // so a staged upgrade is promoted before the version comparison sees it. Per
  // bundled id: absent or unreadable in the marketplace dir -> copy; installed
  // version older than bundled -> refresh (staged copy + rename swap, so a
  // failed refresh keeps the working old copy); installed same-or-newer ->
  // untouched (equal version never refreshes — ship a change by bumping the
  // version). In the steady state a size+mtime signature match against the
  // bundled DSO (the seed stamps copies with the bundled mtime) skips the
  // installed-side manifest read. Best-effort: failures are logged and retried
  // next launch. The bundled id -> version map is handed to the
  // ExtensionManager (setBundledVersions: uninstall lock +
  // downgrade-to-bundled) only in default mode — in a --plugin-dir session the
  // manager governs the override dir, and locking user-owned copies there by
  // id would be wrong.
  void seedBundledPlugins();

  QString extensions_dir_;
  // Default marketplace dir: seed destination + lowest scan tier. Equals
  // extensions_dir_ in default mode.
  QString marketplace_dir_;
  // Bundled (share) dir: seed source, never scanned.
  QString bundled_dir_;
  // True when no --plugin-dir override was given (the ExtensionManager is
  // rooted on the marketplace dir). Captured at construction and used to scope
  // the core-plugin lock — never re-derived by comparing paths, which could
  // misclassify a Paths caller that made the two dirs textually equal.
  bool default_mode_ = false;
  DiagnosticSink sink_;

#ifndef PJ_TARGET_WASM
  std::unique_ptr<ExtensionManager> extension_manager_;
#endif
  std::unique_ptr<PluginRuntimeCatalog> plugin_catalog_;

  // Guards plugin_catalog_'s vectors against the one genuine cross-thread
  // hazard: a streaming source's poll thread resolving a parser while the GUI
  // thread reloads the catalog (Marketplace install/uninstall). reload() takes
  // the exclusive lock; the thread-safe accessors take a shared lock. GUI-only
  // readers (findParserByEncoding, dataSources, buildFileFilter, …) do not
  // lock — they cannot race reload(), which is also GUI-thread-only.
  mutable std::shared_mutex catalog_mutex_;
};

}  // namespace PJ
