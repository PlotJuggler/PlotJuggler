// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ExtensionCatalogService.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QMap>
#include <QSettings>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef PJ_TARGET_WASM
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/version_compare.hpp"
#endif
#include "pj_base/plugin_data_api.h"
#include "pj_marketplace/plugin_compatibility.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcCatalog, "pj.app_core.extensions")

QString defaultExtensionsDir() {
#ifdef PJ_TARGET_WASM
  return {};
#else
  return PlatformUtils::extensionsDir();
#endif
}

QString defaultPendingDir() {
#ifdef PJ_TARGET_WASM
  return {};
#else
  return PlatformUtils::pendingDir();
#endif
}

// User-managed extra plugin folders (Preferences page). QStringList.
constexpr auto kCustomPluginFoldersKey = "Preferences::plugin_folders";

// Plugins bundled with an installed build (the "share" dir). Per the FHS
// bin/lib split (the binary installs to <prefix>/bin, arch-dependent code to
// <prefix>/lib), the bundled plugins live at <prefix>/lib/plotjuggler/plugins,
// resolved relative to the executable so the install stays relocatable — the
// same path works under /usr, /usr/local, or a mounted AppImage. It is a seed
// source only (see seedBundledPlugins), never a scanned load path. A dev build
// tree has no such directory, so seeding is a no-op there; developers point at
// their freshly built plugins with --plugin-dir instead.
QString bundledPluginsDir() {
  return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../lib/plotjuggler/plugins"));
}

// Copies one bundled payload (a per-id directory, or a lone DSO file) into the
// fresh staging dir stage_new. Returns the first failing step's error code
// ({} on success).
std::error_code stagePayload(
    const std::filesystem::path& src_entry, const std::filesystem::path& dso, const std::filesystem::path& stage_new) {
  std::error_code ec;
  std::filesystem::create_directories(stage_new, ec);
  if (ec) {
    return ec;
  }
  if (std::filesystem::is_directory(src_entry, ec)) {
    std::filesystem::copy(
        src_entry, stage_new,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
  } else if (!ec) {
    std::filesystem::copy_file(dso, stage_new / dso.filename(), std::filesystem::copy_options::overwrite_existing, ec);
  }
  return ec;
}

// Promotes stage_new into dst, moving any existing dst aside first. On a failed
// promote the old copy is renamed back; if even that restore fails,
// *aside_orphaned is set — the aside dir then holds the only surviving copy and
// the caller must preserve it. Returns the failing step's error code ({} on
// success).
std::error_code promoteStaged(
    const std::filesystem::path& stage_new, const std::filesystem::path& dst, const std::filesystem::path& aside,
    bool* aside_orphaned) {
  std::error_code ec;
  std::error_code exists_ec;
  const bool had_old = std::filesystem::exists(dst, exists_ec);
  if (had_old) {
    std::filesystem::rename(dst, aside, ec);
    if (ec) {
      return ec;
    }
  }
  std::filesystem::rename(stage_new, dst, ec);
  if (ec && had_old) {
    std::error_code restore_ec;
    std::filesystem::rename(aside, dst, restore_ec);
    *aside_orphaned = static_cast<bool>(restore_ec);
  }
  return ec;
}

// Where the bundled DSO lands inside the installed per-id copy: the payload's
// inner layout is preserved by the copy, so it is `rel` minus its leading
// (payload-entry) component; a flat payload keeps just the file name.
std::filesystem::path installedDsoPath(const std::filesystem::path& dst, const std::filesystem::path& rel) {
  // Purely lexical — lexically_relative, not std::filesystem::relative, which
  // would touch the filesystem. A flat payload (rel is just the file name)
  // yields "." and keeps the whole rel.
  const std::filesystem::path inner = rel.lexically_relative(*rel.begin());
  return dst / (inner == "." ? rel : inner);
}

// True when the installed copy of a bundled DSO is provably the same build:
// same size and same mtime (the seed stamps the installed DSO with the bundled
// one's mtime after every copy, so this holds in the steady state). A hit lets
// the seed skip dlopening the installed copy to re-read its manifest version;
// any doubt — missing file, differing signature, filesystem error — falls back
// to the manifest comparison.
bool sameDsoSignature(const std::filesystem::path& installed_dso, const std::filesystem::path& bundled_dso) {
  std::error_code ec;
  const auto installed_size = std::filesystem::file_size(installed_dso, ec);
  if (ec) {
    return false;
  }
  const auto bundled_size = std::filesystem::file_size(bundled_dso, ec);
  if (ec || installed_size != bundled_size) {
    return false;
  }
  const auto installed_time = std::filesystem::last_write_time(installed_dso, ec);
  if (ec) {
    return false;
  }
  const auto bundled_time = std::filesystem::last_write_time(bundled_dso, ec);
  return !ec && installed_time == bundled_time;
}

// Manifest descriptor of the copy of `id` installed under dir, or nullopt when
// the folder is absent or holds no loadable plugin with that id (a gutted or
// half-written copy) — both mean "reseed".
//
// Returns the full descriptor rather than just the version, because the seed's
// compat check needs abi_major, min_sdk_required, and min_plotjuggler_version — a stale
// installed copy that would be rejected at load time gets replaced with the
// bundled build, which is compatible by construction.
std::optional<PluginDescriptor> installedPluginDescriptor(const std::filesystem::path& dir, const std::string& id) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return std::nullopt;
  }
  const auto scan = scanPluginDsos(dir);
  if (!scan) {
    return std::nullopt;
  }
  for (const PluginDescriptor& descriptor : scan->plugins) {
    if (descriptor.id == id) {
      return descriptor;
    }
  }
  return std::nullopt;
}

}  // namespace

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, QObject* parent)
    : ExtensionCatalogService(std::move(extensions_dir), DiagnosticSink{}, parent) {}

ExtensionCatalogService::ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : ExtensionCatalogService(Paths{std::move(extensions_dir), {}, {}}, std::move(sink), StaticPluginSet{}, parent) {}

ExtensionCatalogService::ExtensionCatalogService(Paths paths, DiagnosticSink sink, QObject* parent)
    : ExtensionCatalogService(std::move(paths), std::move(sink), StaticPluginSet{}, parent) {}

ExtensionCatalogService::ExtensionCatalogService(
    QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent)
    : ExtensionCatalogService(
          Paths{std::move(extensions_dir), {}, {}}, std::move(sink), std::move(static_plugins), parent) {}

ExtensionCatalogService::ExtensionCatalogService(
    Paths paths, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent)
    : QObject(parent),
      sink_(std::move(sink)),
      routing_([this](std::string_view provider_id) { return createParserHandleForProviderId(provider_id); }, sink_) {
  default_mode_ = paths.install_dir.isEmpty();
  const bool default_marketplace = paths.marketplace_dir.isEmpty();
  marketplace_dir_ = default_marketplace ? defaultExtensionsDir() : std::move(paths.marketplace_dir);
  extensions_dir_ = default_mode_ ? marketplace_dir_ : std::move(paths.install_dir);
  bundled_dir_ = paths.bundled_dir.isEmpty() ? bundledPluginsDir() : std::move(paths.bundled_dir);
  // The staging dir must live with the MANAGED dir it stages for — i.e., the
  // marketplace dir, never the --plugin-dir override. The platform default is
  // used only when the marketplace dir itself is the platform default. Anchored
  // to marketplace_dir_ (not extensions_dir_) so a --plugin-dir override leaves
  // installs/staging on the real managed path.
  const QString pending_dir = default_marketplace ? defaultPendingDir() : marketplace_dir_ + "/.pending";

#ifndef PJ_TARGET_WASM
  if (!QDir().mkpath(extensions_dir_)) {
    const QString message =
        u"Failed to create extensions directory \"%1\" — plugin loading will be a no-op until it "
        u"exists."_s.arg(extensions_dir_);
    qCWarning(lcCatalog).noquote() << message;
    reportDiagnostic(DiagnosticLevel::kError, message);
  }
  if (!QDir().mkpath(pending_dir)) {
    const QString message = u"Failed to create extension staging directory \"%1\""_s.arg(pending_dir);
    qCWarning(lcCatalog) << message;
    reportDiagnostic(DiagnosticLevel::kError, message);
  }

  // The ExtensionManager tracks what the marketplace INSTALLS and MANAGES; that
  // is always marketplace_dir_, independent of any --plugin-dir override (which
  // only reorders the LOAD scan hierarchy via buildScanHierarchy). Passing
  // extensions_dir_ here would make the marketplace UI look at the CLI override
  // dir and lose sight of every plugin the user actually installed.
  extension_manager_ = std::make_unique<ExtensionManager>(nullptr, marketplace_dir_, pending_dir, sink_, this);

  // Sync bundled plugins into the marketplace dir BEFORE the scan — the bundled
  // dir is never scanned, so the seed is the only route by which shipped
  // plugins load. Runs in every mode, after the ExtensionManager applied
  // pending staged installs above, so a staged upgrade is promoted before the
  // seed's version comparison sees it.
  seedBundledPlugins();
#else
  (void)pending_dir;
#endif
  plugin_catalog_ = std::make_unique<PluginRuntimeCatalog>(std::filesystem::path{}, sink_, "ExtensionCatalogService");
  // Supply the application dimension of the unified ABI/SDK/application hard
  // compatibility gate before static registration and DSO discovery.
  plugin_catalog_->setHostVersion(QCoreApplication::applicationVersion().toStdString());
  if (!plugin_catalog_->registerStaticPlugins(static_plugins)) {
    qCWarning(lcCatalog) << "One or more statically linked plugins failed to register";
  }

  // Scan the ordered folder hierarchy (--plugin-dir override first, then custom
  // folders, then the marketplace dir; the catalog rejects incompatible copies
  // first, then de-duplicates by plugin id — authoritative entries override,
  // then version, then folder priority).
  std::vector<PluginDirEntry> scan_dirs;
#ifndef PJ_TARGET_WASM
  scan_dirs = buildScanHierarchy(!default_mode_);
#endif
  const auto scan_dir_count = scan_dirs.size();
  plugin_catalog_->setPluginDirs(std::move(scan_dirs));

#ifndef PJ_TARGET_WASM
  // Honor the user's disabled list (installed but not loaded): the marketplace
  // persists it to QSettings; the catalog skips loading those ids. Applied here
  // at startup, so a disable/enable in the UI takes effect on the next launch.
  std::unordered_set<std::string> disabled_ids;
  for (const QString& id : ExtensionManager::disabledExtensionIds()) {
    disabled_ids.insert(id.toStdString());
  }
  plugin_catalog_->setDisabledIds(std::move(disabled_ids));
#endif

  qCInfo(lcCatalog) << "Scanning" << static_cast<int>(scan_dir_count) << "plugin folder(s); install dir"
                    << extensions_dir_;
  plugin_catalog_->scanDirectory();

  const std::scoped_lock routing_lock(routing_mutex_);
  const std::unique_lock catalog_lock(catalog_mutex_);
  rebuildParserClaims();
}

QStringList ExtensionCatalogService::customPluginFolders() const {
  QSettings settings;
  return settings.value(QLatin1String(kCustomPluginFoldersKey)).toStringList();
}

void ExtensionCatalogService::setCustomPluginFolders(const QStringList& folders) {
  QSettings settings;
  settings.setValue(QLatin1String(kCustomPluginFoldersKey), folders);
}

QStringList ExtensionCatalogService::builtinPluginFolders() const {
#ifdef PJ_TARGET_WASM
  return {};
#else
  QStringList folders;
  folders << extensions_dir_;
  if (marketplace_dir_ != extensions_dir_) {
    folders << marketplace_dir_;
  }
  return folders;
#endif
}

std::vector<PluginDirEntry> ExtensionCatalogService::buildScanHierarchy(bool extensions_dir_is_explicit) const {
  std::vector<PluginDirEntry> dirs;
  // A folder that doesn't exist on disk contributes no plugins, so skip it
  // rather than hand it to the catalog — scanning a missing directory reports a
  // kError per launch, which for an absent *optional* folder (e.g. a user-typed
  // custom folder already shown in red in the Preferences page) is noise that
  // masks real plugin-load errors.
  const auto add_if_exists = [&dirs](const QString& folder, bool authoritative) {
    if (!folder.isEmpty() && QDir(folder).exists()) {
      dirs.push_back({std::filesystem::path(folder.toStdString()), authoritative});
    }
  };
  // Priority order: the --plugin-dir override outranks everything, then the
  // custom Preferences folders, then the managed marketplace dir. The
  // user-explicit tiers are authoritative — a version-blind hard override in
  // the catalog's duplicate-id resolution — so an explicitly given folder wins
  // even when it holds an older build.
  if (extensions_dir_is_explicit) {
    add_if_exists(extensions_dir_, true);
  }
  for (const QString& folder : customPluginFolders()) {
    add_if_exists(folder, true);
  }
#ifndef PJ_TARGET_WASM
  // The managed marketplace tier is one directory per installed extension, each
  // resolved to its content-addressed store object when the store backs it and to
  // extensions/<id>/ otherwise. Scanning per id (rather than the extensions dir
  // wholesale plus a separate store tier) discovers each managed plugin exactly
  // once — no duplicate-id churn — and loads it from the immutable object when
  // available. managedPluginDirs() returns only dirs that exist, so they skip the
  // add_if_exists existence gate.
  for (const QString& plugin_dir : extension_manager_->managedPluginDirs()) {
    dirs.push_back({std::filesystem::path(plugin_dir.toStdString()), false});
  }
#else
  add_if_exists(marketplace_dir_, false);
#endif
  return dirs;
}

#ifndef PJ_TARGET_WASM
void ExtensionCatalogService::seedBundledPlugins() {
  // Scanning the shipped set is read-only and must happen even when another
  // process owns the marketplace writer lease: parser provenance still needs
  // the authoritative shipped artifact map for this process's catalog scan.
  std::optional<PluginScanResult> scan;
  QMap<QString, QString> bundled_versions;
  if (QDir(bundled_dir_).exists()) {
    auto scanned = scanPluginDsos(std::filesystem::path(bundled_dir_.toStdString()));
    if (!scanned) {
      reportDiagnostic(
          DiagnosticLevel::kWarning,
          u"Could not scan bundled plugins at \"%1\": %2"_s.arg(bundled_dir_, QString::fromStdString(scanned.error())));
      return;
    }
    scan = std::move(*scanned);
    for (const PluginDescriptor& descriptor : scan->plugins) {
      if (descriptor.id.empty()) {
        continue;
      }
      bundled_versions.insert(QString::fromStdString(descriptor.id), QString::fromStdString(descriptor.version));
      std::error_code canonical_ec;
      std::filesystem::path shipped_dso = std::filesystem::weakly_canonical(descriptor.dso_path, canonical_ec);
      if (canonical_ec) {
        shipped_dso = descriptor.dso_path.lexically_normal();
      }
      bundled_plugin_versions_[descriptor.id] = BundledPluginInfo{descriptor.version, std::move(shipped_dso)};
    }
  }

  // The seed is a writer into the managed dir that does not go through the
  // ExtensionManager mutating API: it clears the shared seed-stage tree and renames
  // payloads straight in. So it needs the same permission every other mutation
  // needs. Without the store's writer lease another live instance owns that dir,
  // and seeding anyway would write across the lease it just failed to take.
  //
  // Skipping costs this session the bundled refresh only: the plugins already in
  // the managed dir still load, and the instance that owns the store has either
  // done the seed or will. Core marketplace mutations are refused by the lease
  // anyway, while the read-only shipped-artifact map above remains available.
  if (!extension_manager_->hasStoreWriteAccess()) {
    const QString message =
        u"Another PlotJuggler instance is managing extensions: bundled plugins were not refreshed this session."_s;
    qCInfo(lcCatalog).noquote() << message;
    reportDiagnostic(DiagnosticLevel::kWarning, message);
    return;
  }

  // Refresh staging area: a SIBLING of the marketplace dir, so the promote
  // rename never crosses a filesystem, yet an interrupted seed can never leak
  // a stale payload into the plugin scan (which walks the marketplace dir
  // recursively). Leftovers from a crashed launch are cleared before anything
  // else — even when the bundled dir is absent this run.
  // Derived from the RESOLVED store, like every other sibling PlotJuggler keeps
  // next to the extensions dir (see PlatformUtils::canonicalStoreRoot): beside a
  // symlinked store the staging area can land on another filesystem, and then the
  // promoting rename below is EXDEV instead of an atomic move.
  const std::filesystem::path market_root(marketplace_dir_.toStdString());
  const std::filesystem::path stage_root(
      (PlatformUtils::canonicalStoreRoot(marketplace_dir_) + u".seed_stage"_s).toStdString());
  std::error_code ec;
  std::filesystem::remove_all(stage_root, ec);

  if (!scan.has_value()) {
    return;  // dev build tree, or an install with no bundled plugins — nothing to seed
  }

  if (!QDir().mkpath(marketplace_dir_)) {
    reportDiagnostic(
        DiagnosticLevel::kWarning,
        u"Cannot create the extensions directory \"%1\" — bundled plugins are unavailable this session"_s.arg(
            marketplace_dir_));
    return;
  }

  // The payload sources resolve against the canonical bundled root (computed
  // once — it is loop-invariant).
  ec.clear();
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(std::filesystem::path(bundled_dir_.toStdString()), ec);
  if (ec) {
    reportDiagnostic(
        DiagnosticLevel::kWarning, u"Cannot resolve the bundled plugin directory \"%1\": %2"_s.arg(
                                       bundled_dir_, QString::fromStdString(ec.message())));
    return;
  }

  // Only the ids whose payload this run actually promoted, mapped to the version
  // now on disk. The marketplace cannot re-read them: reading an installed
  // version above opens its DSO, and glibc serves every later dlopen of that path
  // from the first-loaded image, so the scan snapshot is frozen at the pre-seed
  // version for the rest of the process.
  QMap<QString, QString> seeded_versions;
  // Set when a failed promote could not restore the old copy: the stage dir
  // then holds the only surviving payload and must not be deleted below.
  bool stage_has_orphans = false;

  for (const PluginDescriptor& descriptor : scan->plugins) {
    const QString id = QString::fromStdString(descriptor.id);
    if (id.isEmpty()) {
      continue;
    }
    // The payload is the top-level entry under the bundled dir that contains the
    // DSO — a per-id subdirectory (e.g. csv-loader/, or ros2-topic-subscriber/
    // with its dist/<distro>/ inners) or, for a flat layout, the .so file itself.
    const std::filesystem::path dst = market_root / descriptor.id;
    std::error_code resolve_ec;
    const std::filesystem::path dso = std::filesystem::weakly_canonical(descriptor.dso_path, resolve_ec);
    const std::filesystem::path rel = std::filesystem::relative(dso, root, resolve_ec);
    if (resolve_ec || rel.empty()) {
      reportDiagnostic(
          DiagnosticLevel::kWarning, u"Skipped bundled plugin \"%1\": cannot resolve its path"_s.arg(id), id);
      continue;
    }
    const std::filesystem::path src = root / *rel.begin();
    const std::filesystem::path installed_dso = installedDsoPath(dst, rel);

    // Steady-state fast path: an installed DSO matching the bundled one by
    // size and (seed-stamped) mtime is the same build — skip without paying
    // the installed-side dlopen.
    if (sameDsoSignature(installed_dso, dso)) {
      continue;
    }

    // The marketplace dir stays at least as new as the bundled set: absent or
    // unreadable -> copy; older -> refresh; same-or-newer -> untouched (a
    // marketplace update above the bundled version is never downgraded, and an
    // equal version never refreshes — ship a change by bumping the version).
    //
    // Exception to "never downgrade": an installed copy above the bundled
    // version that the current host would reject at load time (ABI/SDK drift,
    // a malformed floor, or min_plotjuggler_version too high). Leaving it in place gives the user a
    // dead "installed" that never loads and no bundled fallback (the bundled
    // dir is never scanned as a load path). Rescue by refreshing to the bundled
    // build — compatible by construction, since it ships with this host.
    const std::optional<PluginDescriptor> installed = installedPluginDescriptor(dst, descriptor.id);
    bool rescue_incompat = false;
    QString rescue_reason;
    if (installed && comparePluginVersions(descriptor.version, installed->version) <= 0) {
      const QString host = QCoreApplication::applicationVersion();
      const QString installed_reason = ExtensionCatalogService::descriptorIncompatReason(*installed, host);
      if (installed_reason.isEmpty()) {
        continue;  // Installed is compatible — user's marketplace update wins.
      }
      // Second preflight: only rescue if the bundled build is itself
      // compatible. Downstream repackagers or an out-of-tree build with a
      // stale share dir could ship a mismatched bundled; overwriting a broken
      // installed with an equally-broken bundled destroys the user's
      // marketplace history for no gain.
      const QString bundled_reason = ExtensionCatalogService::descriptorIncompatReason(descriptor, host);
      if (!bundled_reason.isEmpty()) {
        reportDiagnostic(
            DiagnosticLevel::kError,
            u"Both bundled and installed \"%1\" are incompatible with this host — leaving the installed "
            u"copy alone (bundled: %2; installed: %3)"_s.arg(id, bundled_reason, installed_reason),
            id);
        continue;
      }
      rescue_incompat = true;
      rescue_reason = installed_reason;
    }

    // Stage the new payload, then swap it into place. Any failure keeps (or
    // restores) the old copy; the next launch retries.
    const std::filesystem::path stage_new = stage_root / descriptor.id;
    const std::filesystem::path aside = stage_root / (descriptor.id + ".old");
    bool aside_orphaned = false;
    std::error_code step_ec = stagePayload(src, dso, stage_new);
    if (!step_ec) {
      step_ec = promoteStaged(stage_new, dst, aside, &aside_orphaned);
    }
    if (aside_orphaned) {
      // Worst case: the promote failed AND the old copy could not be put back.
      // Keep it in the stage dir (inert — never scanned) and say exactly
      // where, so the user can recover it before the next launch's cleanup.
      stage_has_orphans = true;
      reportDiagnostic(
          DiagnosticLevel::kError,
          u"Refreshing plugin \"%1\" failed and the previous copy could not be restored; it is preserved at "
          u"\"%2\" until the next launch"_s.arg(id, QString::fromStdString(aside.string())),
          id);
    }
    if (step_ec) {
      std::error_code cleanup_ec;
      std::filesystem::remove_all(stage_new, cleanup_ec);
      const QString context = rescue_incompat ? u" (attempted rescue of an incompatible installed copy)"_s : QString{};
      reportDiagnostic(
          DiagnosticLevel::kWarning,
          u"Failed to seed bundled plugin \"%1\" into \"%2\"%3: %4"_s.arg(
              id, QString::fromStdString(dst.string()), context, QString::fromStdString(step_ec.message())),
          id);
      continue;
    }

    // Stamp the installed DSO with the bundled one's mtime (best effort) so
    // the next launch's signature fast path can skip the manifest read.
    std::error_code stamp_ec;
    const auto bundled_mtime = std::filesystem::last_write_time(dso, stamp_ec);
    if (!stamp_ec) {
      std::filesystem::last_write_time(installed_dso, bundled_mtime, stamp_ec);
    }

    seeded_versions.insert(id, QString::fromStdString(descriptor.version));

    // The seed just wrote extensions/<id>/ outside the adopt path, so any store
    // object recorded for this id no longer matches these bytes. Drop the mapping:
    // the runtime loads the freshly-seeded directory and reconcileStore collects
    // the orphan. Without this, a bundled refresh over an earlier adopted install
    // would keep loading the stale store version.
    extension_manager_->dropStoreMapping(id);

    if (installed) {
      if (rescue_incompat) {
        const QString message = u"Restored bundled plugin \"%1\" %2 — installed %3 was incompatible: %4"_s.arg(
            id, QString::fromStdString(descriptor.version), QString::fromStdString(installed->version), rescue_reason);
        qCWarning(lcCatalog) << message;
        reportDiagnostic(DiagnosticLevel::kWarning, message, id);
      } else {
        const QString message = u"Refreshed bundled plugin \"%1\" %2 -> %3"_s.arg(
            id, QString::fromStdString(installed->version), QString::fromStdString(descriptor.version));
        qCInfo(lcCatalog) << message;
        reportDiagnostic(DiagnosticLevel::kInfo, message, id);
      }
    } else {
      qCInfo(lcCatalog) << "Seeded bundled plugin" << id << "into" << QString::fromStdString(dst.string());
    }
  }

  if (!stage_has_orphans) {
    std::error_code stage_cleanup_ec;
    std::filesystem::remove_all(stage_root, stage_cleanup_ec);
  }

  // What this run wrote into the managed dir. Reported in every mode, unlike the
  // bundled map below: it is not a policy about which plugins are core but a fact
  // about a directory the marketplace also reads, and the seed writes that
  // directory whether or not a --plugin-dir override reorders the load hierarchy.
  extension_manager_->setSeededVersions(seeded_versions);

  // The bundled map powers the marketplace "core plugin" policy: uninstall lock
  // (isBundled) + downgrade-to-bundled. Scoped to default mode — in a
  // --plugin-dir session the ExtensionManager governs the override dir, where a
  // same-id copy is user-owned and must stay uninstallable.
  if (default_mode_) {
    extension_manager_->setBundledVersions(bundled_versions);
  }
}
#endif

ExtensionCatalogService::~ExtensionCatalogService() = default;

void ExtensionCatalogService::reload() {
#ifdef PJ_TARGET_WASM
  return;
#else
  // Re-read the disabled list before rescanning: it is a snapshot, and installing
  // an extension whose id lingered there from an earlier uninstall clears the
  // QSettings entry (ExtensionManager re-enables a fresh install). Without this
  // refresh the stale snapshot keeps skipping the id, so the plugin stays unloaded
  // until the next launch rebuilds the snapshot.
  std::unordered_set<std::string> disabled_ids;
  for (const QString& id : ExtensionManager::disabledExtensionIds()) {
    disabled_ids.insert(id.toStdString());
  }

  // Rebuild the scan hierarchy before rescanning: the managed tier is one
  // directory per installed extension (buildScanHierarchy -> managedPluginDirs),
  // so a plugin installed since the last scan — its extensions/<id>/ now on disk
  // and its store object freshly adopted — is only picked up if the hierarchy is
  // recomputed here. A wholesale directory scan saw new installs for free; a
  // per-id snapshot taken at construction does not.
  std::vector<PluginDirEntry> scan_dirs = buildScanHierarchy(!default_mode_);

  bool changed = false;
  {
    // routing_mutex_ first (see its declaration), then exclusive: reload()
    // clears/reallocates the catalog's parser vector. Any poll thread resolving
    // a parser through the shared-lock accessors is fenced out for the duration.
    const std::scoped_lock routing_lock(routing_mutex_);
    const std::unique_lock lock(catalog_mutex_);
    plugin_catalog_->setPluginDirs(std::move(scan_dirs));
    plugin_catalog_->setDisabledIds(std::move(disabled_ids));
    changed = plugin_catalog_->reload();
    if (changed) {
      rebuildParserClaims();
    }
  }
  if (changed) {
    emit catalogChanged();
  }
#endif
}

std::vector<std::string> ExtensionCatalogService::parserEncodings() const {
  const std::shared_lock lock(catalog_mutex_);
  std::vector<std::string> encodings;
  for (const auto& parser : plugin_catalog_->messageParsers()) {
    for (const auto& encoding : parser.encodings) {
      encodings.push_back(encoding);
    }
  }
  std::sort(encodings.begin(), encodings.end());
  encodings.erase(std::unique(encodings.begin(), encodings.end()), encodings.end());
  return encodings;
}

MessageParserHandle ExtensionCatalogService::createParserHandleForProviderId(std::string_view provider_id) const {
  const LoadedMessageParser* parser = plugin_catalog_->findParserById(provider_id);
  return parser != nullptr ? parser->library.createHandle()
                           : MessageParserHandle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
}

ParserClaimProvenance ExtensionCatalogService::provenanceForParser(const LoadedMessageParser& parser) const {
  if (parser.statically_registered) {
    return ParserClaimProvenance::kBundled;
  }
  if (parser.from_authoritative_dir) {
    return ParserClaimProvenance::kFolderDrop;
  }
  // Bundled tier requires the loaded artifact to BE the shipped build (id,
  // version, and DSO signature). Artifact-controlled ids and versions alone
  // cannot forge the top trust tier. Authenticated receipts are the full answer.
  const auto bundled = bundled_plugin_versions_.find(parser.id);
  if (bundled != bundled_plugin_versions_.end() && bundled->second.version == parser.version &&
      sameDsoSignature(parser.path, bundled->second.dso_path)) {
    return ParserClaimProvenance::kBundled;
  }
  return ParserClaimProvenance::kMarketplace;
}

void ExtensionCatalogService::rebuildParserClaims() {
  std::vector<ParserProviderInfo> snapshot;
  snapshot.reserve(plugin_catalog_->messageParsers().size());
  for (const auto& parser : plugin_catalog_->messageParsers()) {
    snapshot.push_back(
        ParserProviderInfo{
            .id = parser.id,
            .encodings = parser.encodings,
            .provenance = provenanceForParser(parser),
        });
  }
  routing_.rebuild(std::move(snapshot));
}

ParserRouteSelection ExtensionCatalogService::resolveParserRoutes(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
    std::string_view parser_config_json) const {
  const std::scoped_lock routing_lock(routing_mutex_);
  const std::shared_lock catalog_lock(catalog_mutex_);
  return routing_.resolveParserRoutes(encoding, type_name, schema, parser_config_json);
}

std::optional<sdk::BuiltinObjectType> ExtensionCatalogService::classifyParserObjectRoute(
    std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema) const {
  const std::scoped_lock routing_lock(routing_mutex_);
  const std::shared_lock catalog_lock(catalog_mutex_);
  return routing_.classifyParserObjectRoute(encoding, type_name, schema);
}

const std::vector<LoadedDataSource>& ExtensionCatalogService::dataSources() const {
  return plugin_catalog_->dataSources();
}

const std::vector<LoadedMessageParser>& ExtensionCatalogService::messageParsers() const {
  return plugin_catalog_->messageParsers();
}

const std::vector<LoadedToolbox>& ExtensionCatalogService::toolboxes() const {
  return plugin_catalog_->toolboxes();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::fileImportSources() const {
  const auto& catalog = *plugin_catalog_;
  return catalog.fileImportSources();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::streamSources() const {
  const auto& catalog = *plugin_catalog_;
  return catalog.streamSources();
}

std::vector<const LoadedDataSource*> ExtensionCatalogService::findSourcesForExtension(QStringView ext) const {
  const auto& catalog = *plugin_catalog_;
  return catalog.findSourcesForExtension(ext.toString().toStdString());
}

const LoadedMessageParser* ExtensionCatalogService::findParserByEncoding(QStringView encoding) const {
  const auto& catalog = *plugin_catalog_;
  return catalog.findParserByEncoding(encoding.toString().toStdString());
}

QString ExtensionCatalogService::buildFileFilter() const {
  return QString::fromStdString(plugin_catalog_->buildFileFilter());
}

void ExtensionCatalogService::reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id) const {
  if (!sink_) {
    return;
  }
  sink_(
      Diagnostic{
          level,
          "ExtensionCatalogService",
          id.toStdString(),
          message.toStdString(),
          std::chrono::system_clock::now(),
      });
}

QString ExtensionCatalogService::descriptorIncompatReason(
    const PluginDescriptor& descriptor, const QString& host_version) {
  const std::string reported_host = host_version.toStdString();
  const PluginCompatibilityResult result = evaluatePluginCompatibility(
      {
          .version = descriptor.version,
          .abi_major = descriptor.abi_major,
          .min_sdk_required = descriptor.min_sdk_required,
          .min_plotjuggler_version = descriptor.min_plotjuggler_version,
      },
      currentPluginHostCompatibility(reported_host));
  return QString::fromStdString(result.reason);
}

}  // namespace PJ
