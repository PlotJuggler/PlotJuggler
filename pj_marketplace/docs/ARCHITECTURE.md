# pj_marketplace architecture

How the module works today. Per-method contracts live in the headers; measurements and tried alternatives live in
[TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md); end-user behaviour is in [USER_MANUAL.md](./USER_MANUAL.md); the registry
and package formats are in the [marketplace spec](./plotjuggler-marketplace-spec-v1.0.0-en.md).

The module lets a user discover extensions in a registry, install, update and remove them, and manage what is
installed. An extension is a ZIP holding one or more plugin DSOs. Everything is serverless: the registry is one JSON
file and the artifacts are plain URLs. The module is desktop-only (the browser build hides the marketplace) and is
consumed by `pj_app` through `pj_runtime`'s `ExtensionCatalogService`.

![Architecture](diagrams/architecture.png)

## Components

Public headers live under `include/pj_marketplace/`; sources under `src/core/` (managers, store, extraction),
`src/models/` (pure value types: content manifest, generation digest, loaded profile, store policy), `src/ui/` and
`helper/` (`pj-plugin-check`). Targets: `pj_marketplace` (core, static), `pj_marketplace_ui` (`MarketplaceWindow`,
static), `pj-plugin-check` (the Qt-free admission helper) and `pj_marketplace_app`, a standalone harness built when
`PJ_BUILD_MARKETPLACE_APP` is on (default `PJ_BUILD_TESTS`) so the marketplace can be iterated without the PJ4 shell.
The embedded and standalone entry points consume the same libraries.

| Component | Responsibility |
|---|---|
| `RegistryManager` | Fetch the registry JSON (never cached), parse every record, then run the resolver. |
| `resolveRegistryCandidates()` | Pure function: validate versions and SDK floors, pick one release per id. No network, filesystem or UI. |
| `ExtensionManager` | Install, sideload, update, uninstall, downgrade-to-bundled, enable/disable, staged promotion, the removal journal, the writer lease, diagnostics. |
| `DownloadManager` | HTTP GET with progress, SHA-256 verification and ZIP extraction on a worker thread, under the archive budgets. |
| `PluginCheckRunner` + `pj-plugin-check` | Out-of-process admission of freshly arrived bytes. |
| `StoreSession`, `StoreWriteLease`, `ArtifactStore`, `loaded_profile`, `content_manifest` | Single-writer lease and the content-addressed object store. |
| `PlatformUtils` | Platform id, standard directories, `canonicalStoreRoot()`. |
| `QtDiagnosticBridge` | Adapts a `PJ::DiagnosticSink` to a queued Qt signal `diagnosticReported(level, source, id, message)`; the returned sink is thread-safe and may outlive the bridge (queued delivery is guarded by `QPointer`). |
| `MarketplaceWindow` | The UI (see "UI and host integration"). |

Helpers worth reusing before writing another: `sha256.hpp` (incremental SHA-256, file streaming; Qt-free because the
helper links no Qt), `fs_walk.hpp` (recursive regular-file walk that fails closed on symlinks), `version_compare.hpp`
(`comparePluginVersions`, the one version ordering every badge and lock uses), `archive_limits.hpp` (entry, size and
depth budgets), `utf8_path.hpp` (path to UTF-8 text for messages, logs and JSON: never `path::string()` or
`path(qstring.toStdString())`, which go through the Windows code page; `pj_runtime`'s plugin catalog reuses it),
`plugin_compatibility.hpp` (the ABI major and `min_sdk_required` gate).

**Data models.** `Extension` is one registry release: identity and presentation fields, `min_sdk_required`,
`suggested_sdk_version`, `platforms` (platform id such as `linux-x86_64` to `{url, checksum}`, the checksum as
`sha256:<hex>`) and a changelog. `InstalledExtension` is one installed directory, read from the DSO's embedded
manifest, so a plugin the registry does not list still shows as a row and is gated on every host contract it declares.
`min_plotjuggler_version` is parsed and displayed in both, and never gates.

**Construction.** `ExtensionManager` takes its downloader, extensions dir, pending dir and an optional `DiagnosticSink`
as constructor arguments, so tests point it at temp directories; a no-argument overload owns a `DownloadManager` and
uses the standard user directories. The installed set is a private cache populated by scanning the extensions dir;
there are no installed-state sidecars. The constructor drains restart-deferred work before it snapshots disk (see
"Install and update").

## Registry and compatibility

**Ingestion.** `RegistryManager` parses every record before making a host-specific choice, rejects the whole registry
on a missing `id`, `name` or `version`, and then calls the resolver, which validates every extension version and SDK
floor as strict SemVer and returns one row per id: the highest release compatible with this host for the current
platform; if none is compatible, the highest release for the platform (visible, blocked); if the platform has no
artifact, the highest release overall (visible to consumers that do not filter by platform). A registry that repeats
one semantic `(id, version)` slot is rejected, build metadata included; all platform artifacts of a release belong in
one immutable record. Fields the client reads are listed in the spec ("Registry — Format and Hosting"); `registry_version`,
`last_updated`, `plugins` and `size_bytes` are ignored. Nothing is destructively discarded before eligibility is known,
and the resolver is testable without network, filesystem or UI.

**Contract with the published registry.** `pj-plugin-registry` (`scripts/validate_registry.py`, fed by
`scripts/submit_to_registry.py` in `pj-official-plugins`) keeps a release for a platform only while its
`min_sdk_required` is strictly lower than that of every newer release shipping that platform, and trims an older
record's `platforms` to the ones it still serves. That pruning is lossless only because admission is monotonic in the
SDK floor: a host that admits a floor admits every lower one, there is no upper bound and no ABI check on registry
records, and the platform filter runs before the compatibility filter. Change any of these here and the pruning rule
must change first, or released hosts lose releases they could still load.

**Compatibility policy.**
- The gate is the ABI major plus `min_sdk_required`. The registry and the embedded manifest both declare the SDK floor;
  SDK discovery supplies the ABI major. A plugin that needs a newer host declares the SDK release that added the surface
  it needs. `min_plotjuggler_version` never gates.
- One evaluator (`evaluatePluginCompatibility`, backed by the SDK's strict `PJ::SemVer`) serves registry and local-ZIP
  admission, staged promotion, bundled-seed rescue, static registration and runtime DSO loading. Malformed plugin
  versions and floors fail closed. A directory with several DSOs is one extension: they must agree on `id` and
  `version`, and it keeps the highest valid floor; malformed metadata rejects new admission, keeps the installed row
  visible and uninstallable, and keeps the extension unloadable.
- Registry metadata is a pre-screen: after extraction the embedded ABI and floor are re-checked before promotion, and
  staged work is checked again at the restart that promotes it. `platforms` is registry-only: a foreign binary fails DSO
  discovery before the embedded gate.
- An incompatible authoritative copy (`--plugin-dir`, a Preferences folder) suppresses a managed same-id fallback and is
  itself not loaded, so a developer override failure is explicit.

**Qt and plugins.** Data plugins (sources, parsers, toolboxes) link no Qt: they use the SDK's C ABI and their dialogs
are `.ui` files loaded by the host through `pj_dialog_host`, so a Qt upgrade does not touch them. Widget plugins create
Qt widgets and link Qt; the host compares each library's `build_info()` with its own Qt major.minor and compiler family
before creating anything (`pj_runtime/docs/ARCHITECTURE.md`, "Widget plugins").

**Registry URL.** The host owns it: `pj_app` resolves it from Preferences ▸ Plugins (key `Marketplace/registryUrl`,
empty means the built-in default; http, https and file schemes) and passes it in. The window captures it at
construction and never reads settings; `setRegistryUrl()` re-fetches when the host pushes a new one. The standalone
harness uses a built-in URL.

## Install and update

An install lands in one of three ways, all extracting into a hidden transaction directory
`extensions.install_stage/.pj_install_<id>_<uuid>/`, a **sibling** of the extensions dir. As a sibling it shares that
dir's filesystem (the promoting rename stays atomic) and sits outside the recursive plugin scan, which has no exclusion
rule: an unpacked DSO inside the scanned tree would be discoverable, loadable content, and it survives a crash. An
install whose staging area is on another filesystem is refused, never degraded to a copy.

- **Fresh registry install** (`install()`, all platforms): download, verify SHA-256, extract; the DSO is opened by the
  admission helper inside the transaction directory and its embedded manifest checked against the registry id and
  version, ABI and floor; the directory is renamed to `extensions/<id>/`, re-validated at its final location (catches
  rpath and relative-path assumptions that hold in staging and break there) and registered. Any failure removes the
  transaction directory and promoted copy: no partial state survives. The new extension is picked up when the host
  reloads its plugin catalog.
- **Update** (`update()`, all platforms): the same download and validation, then the transaction directory is renamed
  to `.extension_staging/<id>/` and a transient `.pj_pending_install` intent (id and version, one per line) is written.
  Nothing is hot-swapped: the installed version stays live until the next start, and the row shows "Needs Restart".
- **Sideload** (`installFromLocalZip()`): a ZIP with exactly one top-level directory holding the plugin. Id and version
  come from the embedded manifest and no checksum exists (a local file has no provenance), so the manifest scan and
  the compatibility gate are the whole admission. A fresh id installs as above. An installed id asks the host's
  `ReplaceConfirmation` and, if confirmed, is staged like an update with no version comparison (rebuilding without a
  version bump is normal); with no confirmation registered the conflict is refused. See "Identical-payload
  short-circuit".

**Startup promotion.** The constructor runs `applyPendingUninstalls()`, then `applyPendingInstalls()`, then scans disk,
then `reconcileStore()`. For each staged directory `applyPendingInstalls()` reads the intent (id and version are
validated against the same safe-path and character rules used elsewhere, so a tampered intent cannot escape the
extensions dir), moves any existing install to `.backup/<id>-<uuid>/` without opening it, renames the stage into
`extensions/<id>/`, validates the promoted payload out of process against the intent and the compatibility gate, and
retires the intent. A failed rename restores the backup; a failed validation drops the payload and restores the
backup; if even the restore fails the diagnostic names both paths. A stage it cannot remove (a locked DSO on Windows)
is renamed `.pj_quarantine_<name>_<uuid>/` so every start does not re-report it. Duplicate-directory cleanup
(`replaceConflictingInstallDirs`) runs after the whole promotion loop, for the reasons in TECHNICAL_NOTES, "dlopen
pins the first image".

**Backups.** Every promotion moves the previous version into `.backup/`; restoring from it after a plugin that fails to
load later is manual.

**Guards.** One install or update runs at a time per manager. `install()` refuses an installed id and `update()` refuses
a downgrade; both refuse an id with a staged removal (`update()` also one with a staged install), and an incompatible
release (platform, ABI, SDK floor). A download is cancelled when free space is below a multiple of its size. `ExtensionManager`'s signals report
progress, phases, success, failure, and the staged outcomes (`installPendingRestart`, `uninstallPendingRestart`,
`downgradePendingRestart`, `installUnchanged`).

**Download and extraction.** The whole archive is buffered in memory, verified (an absent checksum skips verification;
`sha256:` with no digest fails as a malformed registry field; digest case is ignored) and extracted on a worker thread.
`ArchiveLimits` bounds entry count, per-entry and total expanded size and path depth, charges a declared entry size up
front and an undeclared one while its bytes arrive, refuses non-regular entries and paths no extension has a reason to
carry (absolute, drive letters, backslashes, `..`, device aliases), and a traversal guard runs as the second line of
defence. Cancellation is observed at each entry and data block.

## Removal journal

A removal cannot happen when the user asks for it: the DSO is loaded, and on Windows locked. `uninstall()` and
`downgradeToBundled()` record the intent and `applyPendingUninstalls()` performs it at the next launch.

The intent lives in `extensions.state/<id>.json` holding `{schema, operation, id, path}`, committed with `QSaveFile` so
a reader never sees a half-written record. The root derives from the canonical store root, so two aliases of a symlinked
store share one journal.

**Why outside the payload.** It is a sibling for the same reasons as the install staging area, plus one specific to
removals: `QDir::removeRecursively()` deletes children before it can fail on the one it cannot unlink, so a record
inside the directory being deleted would be destroyed by its own partially failed deletion (TECHNICAL_NOTES, "Removal
records outside the payload").

**A record is a capability, so every record is validated.** Processing one runs `removeRecursively()` on the path it
names, and the store lease does not protect against this: records sit in the rightful writer's own state directory, so
corruption or anything able to drop a file there would otherwise inherit the privilege. `parseRemovalIntent()` refuses a
record unless all of these hold, with nothing defaulted or repaired: it declares the current `schema`; `operation` is
exactly `uninstall`, `downgrade` or `cleanup` (absent never reads as the destructive `uninstall`); for an id-bearing
operation `id` is filesystem-safe and equals the filename stem, so a record cannot speak for another extension; `path`
is absolute and a direct child of the canonical store root, which excludes the root, its ancestors, `..` and the sibling
state, staging and lease paths, and is re-checked on the canonical target so a planted symlink cannot carry the delete
out. The store root is canonicalized once at construction so a symlinked config or home does not strand valid records.
Confinement runs again at deletion time, because parsing skips the canonical check for a target that did not exist yet:
a managed payload is a real directory, so a symlink at the target is refused. A refused record is quarantined
(`*.rejected-<uuid>`, which keeps the evidence and takes it out of the `*.json` glob) and reported, never silently
skipped; on success the exact record file that was parsed is retired.

**Write order.** `uninstall()` persists the disabled entry first, syncs it and checks `QSettings::status()`, then
commits the record. The loader decides what to load from the disabled list alone, so a crash mid-stage can only leave
"unloadable but still installed", never a removed plugin quietly returning. A step that cannot be persisted rolls the
enable state back and fails the operation.

**Tombstones.** A successful removal drops the record but keeps the id's disabled entry, so journal processing needs
no authority over enable state: no record, however corrupt, can make a plugin loadable, and a same-id copy elsewhere
cannot be resurrected by a deletion. A fresh install clears the tombstone (`registerInstalledExtension`). `downgrade`
writes none: it is a version change, and the restored bundled version inherits the user's enable choice.

**Payload markers.** A `.pj_pending_uninstall` marker found inside a payload (a package can ship one) is converted into
a path-only `cleanup` record before anything is deleted. Its content is never read, so the record carries no id and no
authority over any extension's state; conversion is idempotent (a directory a live cleanup record already covers is
skipped).

## Single-writer lease

A managed store has one writer at a time across processes. Constructing an `ExtensionManager` takes a `QLockFile` on
`.extensions.lock` beside the extensions dir (through `StoreSession`, before any cleanup runs). An instance that cannot
take it runs read-only: it still scans and reports installed state, but install, sideload, update, uninstall,
downgrade, enable/disable and the startup drains refuse with "Another PlotJuggler instance is managing extensions."
`hasStoreWriteAccess()` lets a UI disable those actions up front. `ExtensionCatalogService::seedBundledPlugins()` is
the one store writer outside `ExtensionManager`'s mutating API and is gated on it too.

Why: startup cleanup deletes every `.pj_install_*` directory it finds and nothing in the name says which process owns it;
without the lease a second PlotJuggler would delete the extraction directory of a download the first is running. With
it, cleanup runs only in the lease holder and every live writer holds it, so a drain meets only its own transactions or
those of a dead process. Staleness is decided by the owner PID's liveness alone (`setStaleLockTime(0)`): a crashed
writer's lock is reclaimed, a slow writer's is never stolen. Contention ("close the other instance and restart") and
an uncreatable lock file (a config dir that cannot hold one) produce different messages, because they call for opposite
actions. Nothing reacquires the lease mid-session, so a read-only session stays read-only until the next launch.

The lock file and every staging sibling derive from `PlatformUtils::canonicalStoreRoot()`: a `--plugin-dir` session or a
test over its own temp dir never contends with the default store, while two names for one store (a symlinked install)
always contend. The destructor cancels in-flight work first and releases the lease last, because an extraction worker
writes into the store.

## Identical-payload short-circuit

Rebuilding a plugin and sideloading it again usually changes nothing, and replacing a live extension is the expensive
step (a staged copy and a restart). `installFromLocalZip()` therefore compares identity after the compatibility gate and
before the replacement question. Identity is `inspectContentManifest()` (every regular file as a relative path plus its
SHA-256) folded by `aggregateContentManifestDigest()`: paths count, archive timestamps and compression do not, and a
symlink or special entry makes the tree unreadable, which yields an empty digest that never matches. Both trees are
hashed at that moment, not from a digest recorded at install time, so an installed tree edited underneath is repaired by
the ordinary path. The comparison targets the copy the scan resolved to (two directories can embed one id and the scan
keeps the highest version), and is skipped while an install is staged for the id, because the stage is what the next
launch promotes. A match emits `installUnchanged` and nothing else. The extraction and hashing still happen; what is
saved is the promotion and the restart.

## Content-addressed store

Every successful install is also mirrored into an immutable object store, and the runtime prefers that copy.

- **Layout.** `StoreSession::open()` canonicalizes the store root once and prepares siblings on the same volume:
  `extensions.artifacts/<digest>/` (objects), `extensions.extraction/` (scratch, swept when a writer opens) and
  `extensions.profiles/loaded.json` (extension id to object digest). The session carries the single-writer lease, so the
  store has one lock.
- **Identity.** An object is named by its content-manifest digest, computed the same way for an extracted archive and an
  adopted directory, so identical bytes share one object whatever their origin.
- **Write path.** After the visible install is final, `ExtensionManager::registerInstalledExtension` adopts
  `extensions/<id>/` (`ArtifactStore::adoptDirectory`, write-once) and records the digest in the loaded profile. It is
  best-effort and never gates the user's result: a refused tree (a symlink, a budget, an I/O fault) drops the id's
  mapping and the directory remains what loads.
- **Load path.** `managedPluginDirs()` is what `ExtensionCatalogService` scans for the managed tier: one directory per
  id, the store object when the profile names one and its bytes still verify against the recorded manifest
  (`verifyArtifact`, a rehash), else `extensions/<id>/`. A present object that fails the check is never loaded and is
  reported. A writer that changes `extensions/<id>/` outside the adopt path (the bundled seed) calls
  `dropStoreMapping()`.
- **Collection.** `reconcileStore()` runs at startup after the pending queues drain: it drops profile entries whose
  `extensions/<id>/` is gone, then deletes objects no entry references. It skips the sweep when the profile is empty
  while extensions are installed, which means the profile failed to load and an empty root set would erase the store.

## Out-of-process admission

The first look at bytes that just arrived (a registry download, a local archive, a promoted directory re-validated at
its final path) is taken by `pj-plugin-check`, a Qt-free child process `PluginCheckRunner` starts on one directory with a
wall-clock timeout. A plugin that aborts in a static initializer cannot take the application down, and opening a
payload at its staging path in this process would pin that image. The helper prints one JSON document (schema 1:
`plugins`, `diagnostics`, `outcome` `admitted` or `rejected`) and exits 0 (admitted), 2 (usage) or 4 (no valid plugin);
the runner reports a helper that produced no verdict (spawn failure, crash, timeout, malformed JSON) as unproven rather
than as a bad plugin. The helper reports the Widget family as `"widget"`. Directories already admitted are inspected
in-process. The runner blocks the calling thread (the GUI thread, in the install handlers) until the child exits or the
timeout expires.

## Bundled (core) extensions

Plugins shipped inside an installed build or AppImage live in the bundled dir (`<prefix>/lib/plotjuggler/plugins`,
resolved relative to the executable). It is a seed source only and never a load path. The host side
(`ExtensionCatalogService`, described in `pj_runtime/docs/ARCHITECTURE.md`, "Extension catalog and parser routing")
syncs it into the default extensions dir at every start, in every mode, before the plugin scan and after
`ExtensionManager` applied pending work: an absent or unreadable id is copied, an older installed copy is refreshed
(staged copy plus a rename swap, so a failed refresh keeps the working copy), a same-or-newer copy is left alone. The
comparison is version-only (`comparePluginVersions`, the rule the uninstall lock, downgrade and badges use), so shipping
a change requires a version bump.

The marketplace side: the host hands the bundled id-to-version map to `setBundledVersions()` and the versions the seed
actually wrote to `setSeededVersions()`. Membership in the bundled map makes an id **core**: `isBundled()` locks its
uninstall while the installed version is at the bundled one, and `downgradeToBundled()` stages a revert when the
installed version is ahead (applied on restart, like any staged action, and keeping the enable state). The lock is by
id, so it survives updates. The map is handed over only when the manager is rooted on the default extensions dir; in a
`--plugin-dir` session the manager governs the override folder, where a same-id copy is user-owned and nothing is core.
A user cannot hold a core extension below the bundled version through the marketplace (the seed lifts it back); placing
the older build in a custom Preferences folder does, because user-explicit tiers win version-blind. Folder precedence
at load time is host policy (`PluginRuntimeCatalog`): `--plugin-dir`, then custom Preferences folders, then the
extensions dir.

## Diagnostics

`ExtensionManager` reports lifecycle events (install, promotion, quarantine, removal failures, registry-fetch errors) on
three channels at once: `diagnostics()` (a bounded ring buffer, `kMaxDiagnostics`) read by the window's Diagnostics
dialog; the `diagnosticReported(id, message, is_error)` signal that feeds the status bar; and an optional
`PJ::DiagnosticSink` constructor argument, so a host can fold marketplace events and its own plugin-load events into
one chronological stream. The sink contract is the SDK's `pj_base/diagnostic_sink.hpp`; the standalone harness passes
none. `hasUnsurfacedDiagnostics()` / `markDiagnosticsSurfaced()` let a re-opened window show a diagnostic once.

## Disk layout

The root is `QStandardPaths::AppDataLocation` (org/app pair `PlotJuggler`/`PlotJuggler4`, shared by the embedded host and
the standalone app): Linux `~/.local/share/PlotJuggler/PlotJuggler4/`, macOS `~/Library/Application Support/...`,
Windows `%LOCALAPPDATA%/PlotJuggler/PlotJuggler4/`. Every sibling below is named from the canonical extensions dir
(`<extensions dir>.<suffix>`), so under `--plugin-dir` they sit beside the override folder.

```
<config-root>/
├── extensions/<id>/             # active plugins: marketplace installs and seeded core extensions
├── extensions.install_stage/    # transaction directories .pj_install_<id>_<uuid>/, removed once promoted or swept
├── extensions.state/<id>.json   # removal journal (cleanup-<uuid>.json for path-only records)
├── extensions.artifacts/<digest>/   # immutable content-addressed objects
├── extensions.extraction/       # store extraction scratch
├── extensions.profiles/loaded.json  # extension id -> object digest
├── extensions.seed_stage/       # bundled-refresh staging (pj_runtime)
├── .extensions.lock             # single-writer lease
├── .extension_staging/<id>/     # staged updates with .pj_pending_install; quarantined stages .pj_quarantine_*
└── .backup/<id>-<uuid>/         # previous versions; also <id>-replaced-<uuid> for a displaced duplicate
```

The ZIP layout an extension must have is in the spec ("Extensions — Package Structure").

## Behaviour contracts

What the marketplace must do, as implemented.

**Discovery.** The table lists name, category, installed version (an em dash when not installed; a locally newer build
shows its own), marketplace version (highlighted for an available update, amber for an incompatible release) and
description. Search matches name, description and tags, case-insensitively on the trimmed text. Four category toggles
(Data Sources, Data Streamers, Parsers, Toolboxes; the registry values `data_loader`, `data_stream`, `message_parser`,
`toolbox`) combine as a union; a category with no toggle is never hidden, or a plugin with no category could not be
found to install. An incompatible release stays listed with its reason, and its install action is disabled. Platform
exclusion happens upstream in the registry list. Selecting a row fills the details footer (metadata, description,
changelog, the primary action, Uninstall or Downgrade, enable toggle, Visit Website).

**Installed state.** Derived from disk by scanning DSOs; each embedded manifest is the source of truth. Fields read: `id`,
`version`, `name` (falls back to `id`), `description`, `category`, ABI major (from SDK discovery; zero means an older
descriptor), `min_sdk_required`, `min_plotjuggler_version`, `suggested_sdk_version`; `install_date` is the extension
directory's mtime, `path` the scanned directory and `enabled` is true unless the id is on the persisted disabled list. A
host may seed the window with a loaded-plugin snapshot before the first render; it is initialization data, not a second
source of truth.

**Install and update.** Install requires a compatible, not-yet-installed extension; update requires a newer registry
version and is staged, and a registry version older than the installed one is rejected with a diagnostic that keeps the
local install. "Update All" stages every compatible update that is not already staged. A sideload over an installed id,
whatever its version, asks for confirmation and is staged; with a byte-identical payload it reports success and
changes nothing. An archive whose embedded ABI or SDK floor the host cannot satisfy is refused before anything is
written.

**Uninstall and enable.** Uninstall is staged: the row shows "Needs Restart", the plugin stays loaded for the session,
the directory is removed at the next start, and the same id cannot be reinstalled until then. A deletion that fails at
that start (locked file, antivirus, permissions) is reported and retried every start while the id stays disabled.
Core extensions cannot be uninstalled while at their bundled version. Enable/disable persists in `QSettings`
(`Marketplace/disabledExtensions`, read by the runtime catalog through `ExtensionManager::disabledExtensionIds()`) and
applies on the next launch; the marketplace does not decide what the loader instantiates.

**Errors.**

| Situation | Behaviour |
|---|---|
| Registry unreachable or malformed | The error is shown and a dialog explains that only installed extensions are listed; installed extensions stay listed. |
| Download fails or is cancelled | Nothing is written; the transaction directory is removed. |
| Checksum mismatch, corrupt ZIP, unsafe entry, limit exceeded | The install fails with the reason; nothing is promoted. |
| Not enough disk space | The download is cancelled before extraction. |
| Extensions dir missing | Created. |
| Embedded manifest missing or invalid, id or version differs from the registry | The install or promotion is rejected with a diagnostic. |
| Update fails during staging | The live install is untouched, nothing is staged, no backup is made. |
| Staged update invalid at restart | The stage is removed or quarantined; the active install stays. |
| Crash before an update is applied | The pending update remains for the next start. |
| Second PlotJuggler opens the same store | Read-only (see "Single-writer lease"). |
| Plugin fails to load after an install | The host reports the load failure; the backup is the manual recovery point. |

**Non-functional.** C++20, the pinned Qt (`PJ_QT_VERSION` in `versions.env`), Linux, Windows and macOS. Dependencies are
Qt, libarchive and `plotjuggler_sdk::plugin_host`. Downloads and extraction run off the GUI thread. No backend server,
database, accounts or telemetry; the registry is a static JSON file and installed state is discovered from DSOs. Sizing
assumes a catalog of tens of extensions, each a few megabytes compressed; the registry and filtering are not tuned or
measured beyond that.

**Out of scope.** Deciding what the loader instantiates and hot-reloading running plugin instances (host concerns),
paid extensions, dependency resolution between extensions, automatic updates (always user-initiated), plugin
sandboxing.

## UI and host integration

`MarketplaceWindow` is one `.ui`-driven window on the app chrome (`PJ::Dialog`): a toolbar (category filters, search,
Install local..., Update All, a gear button), the table, a details footer and a status line with a progress bar and a
diagnostics button. There is no per-row button and no separate detail dialog: every action lives in the footer. While
an install runs, further Install and Update clicks, local ZIPs and the Update All batch queue behind it (clicks first,
then local ZIPs, then the batch), because the manager runs one install at a time; a restart-required dialog appears once per settled
batch and a failure summary replaces a trailing success status.

`pj_app` opens it from File ▸ Extensions Marketplace... (Ctrl+Shift+M, `MainWindow::onOpenMarketplace`): a
`MarketplaceWindow` over the session's `ExtensionManager` is kept as a hidden controller and its `contentWidget()` is
lifted into a closable, tab-migratable panel. When the panel closes and `installationsChanged()` is true the shell calls
`ExtensionCatalogService::reload()`, which loads new plugins without a restart. The gear button emits
`pluginPreferencesRequested`, which the shell answers by opening Preferences ▸ Plugins; the marketplace depends on no
preferences UI. The Help ▸ Installed Extensions submenu is informational and lists loaded plugins.

**Recipes.**
- A new registry field: add it to `Extension`, parse it in `RegistryManager::parseJson`, document it in the spec; a field
  that gates admission also needs the resolver, the embedded-manifest check and the registry pruning contract above.
- A new admission rule: extend `evaluatePluginCompatibility` only (every admission point shares it), and check the
  registry pruning contract first.
- Anything that writes under the store: gate it on `hasStoreWriteAccess()`, derive paths from the canonical store root,
  and never open a payload directory in-process before the drain that may replace it.

## Known gaps

- No confirmation dialog before install, update or uninstall; only the sideload replace question is asked.
- `DownloadManager::cancel()` exists, but the window has no cancel action.
- No automatic restore from a backup after a plugin fails to load; backups are a manual recovery point.
- The registry is fetched fresh each time (no cache, no TTL), and only one registry URL is supported.
- Extension icons (`icon_url` is parsed, not shown), popularity metrics and an "N updates available" notification do not
  exist.
- The "Installed only" filter toggle exists in `marketplace_window.ui` but is hidden, so the installed filter cannot be
  reached from the UI.
- Registry `plugins` entries (`Extension::plugins`, `ExtensionPlugin`) are never parsed, and `size_bytes` is ignored.
- The whole archive is buffered in memory, and the compressed-size cap is checked only after the download completes.
- The admission helper runs on the calling (GUI) thread and blocks it for up to the helper's timeout.
- Two archive-policy implementations exist: `archive_limits.hpp` (used by `DownloadManager`) and `store_policy.hpp` with
  `ArchiveExtractor` (reached only through `ArtifactStore::commitArchive`, which no install path calls). `commitArchive`,
  `ArtifactStore::acquire` and `materializeArtifact` have no production caller, and most descriptor and profile
  rejection codes in `generation_digest.hpp` and `store_rejection.hpp` are never produced.
