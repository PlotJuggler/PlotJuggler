# PlotJuggler Marketplace — Requirements

> **Version:** 1.0.0
> **Last Updated:** 2026-07-17
> **Purpose:** Define WHAT the application should do, not HOW

---

## 1. Problem Statement

PlotJuggler has grown significantly, evolving from an internal tool to a de facto standard for data visualization in robotics. With this growth comes a problem: **how do we allow the community to contribute plugins without requiring a full PlotJuggler recompilation for each update?**

### Current Pain Points

1. Users must recompile plugins when PlotJuggler updates
2. Qt version mismatches break plugins silently
3. High barrier to entry for plugin developers (CMake, Conan, CI setup)
4. No central discovery mechanism for available plugins
5. Manual installation process is error-prone

---

## 2. Feature Overview

### 2.1 Client Features (Marketplace UI)

| Category           | Feature              | Description                                                   |
| ------------------ | -------------------- | ------------------------------------------------------------- |
| **Discovery**      | Extension listing    | Display all available extensions in VSCode-style cards        |
|                    | Search               | Search by name, description, tags, and publisher              |
|                    | Filtering            | Any combination of Installed, Data Source, Streamer, Parsers, Toolboxes |
|                    | Extension detail     | Panel with complete information, changelog, and dependencies  |
|                    | Startup snapshot     | Host app may seed the marketplace with already-loaded plugin ids/versions before first render |
| **Installation**   | Secure download      | ZIP artifact download with SHA256 verification                |
|                    | Automatic extraction | Decompression to extensions directory                         |
|                    | Platform detection   | Automatic selection of correct artifact (Linux/Windows/macOS) |
| **Updates**        | Update detection     | Local vs registry version comparison (semver); local-newer state is surfaced explicitly |
|                    | Individual update    | Update a specific extension                                   |
|                    | Bulk update          | "Update All" for multiple extensions                          |
|                    | Automatic backup     | Backup of previous version before updating                    |
| **Uninstallation** | Clean removal        | Directory deletion + installed cache refresh                  |
|                    | Confirmation         | Confirmation dialog before uninstalling                       |
|                    | Core lock            | Core (bundled) extensions cannot be uninstalled in default sessions (see §4.4) |
| **Management**     | Backup diagnostics   | Report retained backup paths when an update install fails     |
|                    | Persistent state     | Installed state derived from plugin DSOs; each embedded plugin manifest is the source of truth |
|                    | Core (bundled) extensions | Extensions shipped with the application are seeded into the extensions dir at startup, kept at least at the shipped version, locked against uninstall, and offer "Downgrade to bundled" when updated above the shipped version (see §4.4) |
|                    | Registry URL setting | Edited in the host's Preferences → Plugins page (validated http(s)/file scheme; empty restores the built-in default) and persisted in the host's QSettings (`Marketplace/registryUrl`). The host resolves the URL and passes it in each time the marketplace opens; the window never reads registry settings itself |
|                    | Unified diagnostics  | All lifecycle events (install / staged-promotion / quarantine / uninstall failures, registry-fetch errors) are surfaced through `ExtensionManager::diagnosticReported`, the in-memory ring buffer, AND an optional `PJ::DiagnosticSink` so embedding hosts can fold marketplace events into their own diagnostic stream |
| **UI/UX**          | Download progress    | Progress bar in status bar                                    |
|                    | Notifications        | Status messages and available update alerts                   |
|                    | Context menu         | Quick actions per installed extension                         |

### 2.2 CI System Features (For Developers)

| Category       | Feature                    | Description                                                |
| -------------- | -------------------------- | ---------------------------------------------------------- |
| **Build**      | Cross-platform compilation | Matrix build for Linux, Windows, and macOS                 |
|                | Static linking             | All dependencies embedded in the artifact                  |
|                | Dependency management      | Support for Conan (current) and Pixi (future)              |
| **Packaging**  | ZIP generation             | Automatic packaging with plugin binaries and resources |
|                | Checksums                  | Automatic SHA256 generation per artifact                   |
|                | Versioning                 | Version extraction from git tag                            |
| **Publishing** | GitHub Release             | Automatic release creation with attached artifacts         |
|                | Registry update            | Automatic PR to registry repo with new version             |
| **Validation** | Unit tests                 | Test execution on each platform                            |
|                | Lint/Format                | Code style verification                                    |
|                | Schema validation          | Registry JSON validation in PRs                            |

### 2.3 Registry Features

| Category    | Feature             | Description                                           |
| ----------- | ------------------- | ----------------------------------------------------- |
| **Catalog** | Complete listing    | JSON with all available extensions                    |
|             | Metadata            | Name, description, author, license, tags, category    |
|             | Versioning          | Current version and minimum PlotJuggler versions      |
|             | Cross-platform      | URLs and checksums per platform (Linux/Windows/macOS) |
| **Hosting** | Static GitHub       | JSON file accessible via raw.githubusercontent.com    |
|             | Cache TTL           | Support for local cache with expiration time          |
|             | Multiple registries | Configuration for alternative registries (enterprise) |

---

## 3. Terminology

| Term | Definition |
|------|------------|
| **Extension** | Marketplace distribution unit. Downloadable ZIP containing one or more plugins. |
| **Plugin** | C++ module dynamically loaded (.so/.dll/.dylib) implementing an SDK interface. |
| **Registry** | Static JSON file with the catalog of available extensions. |
| **Plugin SDK** | Abstract library (no Qt) that plugins use for UI and data access. |
| **Artifact** | Compiled binary of an extension for a specific platform. |
| **Embedded manifest** | JSON string exported by each plugin DSO describing the installed plugin. |
| **Bundled (share) dir** | `<prefix>/lib/plotjuggler/plugins`, shipped inside an installed build or AppImage. A seed source only — never scanned as a load path. |
| **Core (bundled) extension** | An extension whose id ships in the bundled dir. Seeded into the extensions dir, uninstall-locked in default sessions. |
| **Seed** | The startup sync of bundled plugins into the extensions dir (copy when absent, refresh when the bundled version is newer). |

---

## 3a. Out of scope

These concerns are intentionally **not** part of `pj_marketplace` and belong
elsewhere in the host application:

- **Enable/disable installed plugins.** Toggling a plugin on or off without
  uninstalling it is a host-application concern (e.g. a per-user config knob
  in `pj_app` that filters which discovered DSOs the loader instantiates),
  not a marketplace concern. The marketplace's job ends at "installed on
  disk"; whether the host chooses to load that DSO at startup is decided by
  the host's plugin loader and config.
- **Runtime hot-reload of plugin instances.** Out of scope per PJ4_PLAN
  non-goals.

---

## 4. Functional Requirements

### 4.1 P0 — Minimum Viable Product

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| F-01 | Fetch and parse registry JSON from configurable URL | Given a valid URL, the system loads and parses extension metadata |
| F-02 | List extensions in sidebar with cards | User sees all available extensions with name, description, version |
| F-03 | Search by name, description, tags | Typing "ros" shows all ROS-related extensions |
| F-04 | Filter by any combination of facets | User can activate several category toggles at once (union) and intersect them with Installed |
| F-05 | Show selected extension detail | Clicking an extension shows full information panel |
| F-06 | Download ZIP with SHA256 verification | Download fails if checksum doesn't match |
| F-07 | Extract ZIP to extensions directory | ZIP contents are extracted to correct location |
| F-08 | Register installed extension | Installed state is derived from disk by scanning extension DSOs and reading each embedded plugin manifest |
| F-09 | Detect updates (local vs registry version) | User sees "Update available" badge when registry is newer, and "Local newer" when the installed version is ahead |
| F-10 | Uninstall extension | User can remove installed extensions (except core extensions in default sessions — see F-28) |

### 4.2 P1 — Robustness

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| F-24 | Configurable, persisted registry URL | Preferences → Plugins offers a default/custom mode switch: default shows the built-in URL read-only, custom enables the field. On edit-finish the value is validated — http(s)/file scheme, plus a reachability probe when the system is online (skipped offline) — and the text renders red while the check fails (advisory, never blocking). On OK a valid custom URL persists in QSettings (`Marketplace/registryUrl`; default mode stores "no override") and the marketplace fetches from it on next open |
| F-11 | Local registry cache with TTL *(deferred)* | Not implemented today — `RegistryManager` always fetches fresh. Would be reintroduced only if a clear caching need emerges (see [TODO.md](TODO.md)). |
| F-12 | Backup previous version on updates | Old version saved before overwriting |
| F-13 | Automatic rollback if plugin fails | Deferred; backups may exist, but automatic restore is not implemented |
| F-14 | Deferred update: apply on restart | Updates are downloaded and staged, then applied only after restart; a fresh install activates immediately |
| F-31 | Recognise a local archive that changes nothing | A sideloaded ZIP whose extracted tree hashes identically to the installed one is reported as done without a confirmation, a staged copy, or a restart. Identity is the SHA-256 of every file plus its relative path, so a rebuild that only re-dates the archive still matches and one changed byte does not |
| F-16 | Cancel download in progress | User can abort a download |
| F-17 | Update All | Single action to update all extensions with available updates |
| F-18 | Confirmation dialogs | User confirms before install/uninstall/update actions |

### 4.3 P2 — Polish

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| F-19 | Extension icons (download + cache) | Each extension displays its icon |
| F-20 | Changelog per extension | User can see version history |
| F-21 | Metrics (downloads, rating) | Extension cards show popularity metrics |
| F-22 | Notification: "N updates available" | User notified of available updates |
| F-23 | Multiple registry URLs | Support for private/enterprise registries |

### 4.4 Bundled (core) extensions and plugin folders

The host application (via `pj_runtime`'s `ExtensionCatalogService`) syncs bundled
plugins into the extensions dir and decides which folders are scanned; the
marketplace enforces the core-extension policy on top. Requirements on the
combined behavior:

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| F-26 | Seed bundled plugins at startup, in every mode | Each bundled id absent from (or unreadable in) the extensions dir is copied there before the plugin scan — including in `--plugin-dir` sessions. The bundled dir itself is never scanned as a load path. |
| F-27 | Keep seeded extensions at least at the shipped version | When the bundled version is newer than the installed one, the installed copy is replaced (staged copy + swap: a failed refresh keeps the old working copy and retries next launch). An installed version equal to or newer than the bundled one is never touched — equal versions never re-copy (shipping a change requires a version bump), and a user install ahead of the bundled version is never downgraded automatically. |
| F-28 | Lock uninstall of core extensions (default sessions only) | In a default session the Uninstall action is unavailable for bundled ids. In a `--plugin-dir` session nothing is core: the managed folder is user-owned, so same-id copies there remain uninstallable. |
| F-29 | Offer "Downgrade to bundled" | When a core extension was updated above the bundled version, the user can revert to the shipped version (staged, applied on restart). |
| F-30 | Plugin folder precedence | Folders are scanned in this priority order, duplicates resolved by plugin id: the `--plugin-dir` override, then the custom folders from Preferences → Plugins, then the extensions dir. The first two tiers are user-explicit and win **version-blind** (an explicitly given folder wins even with an older build); within the extensions dir, compatibility then higher version decide. |

---

## 5. Non-Functional Requirements

| ID | Requirement | Metric |
|----|-------------|--------|
| NF-01 | C++20 | Module builds at C++20 (`CMAKE_CXX_STANDARD 20`), matching the rest of PJ4 and `plotjuggler_sdk` |
| NF-02 | Qt 6.x Widgets | 6.11 now; 6.12 LTS target |
| NF-03 | Cross-platform | Works on Linux, Windows, macOS |
| NF-04 | Build system: CMake | Standard CMake project |
| NF-05 | Dependencies: Conan (current), Pixi (future) | Builds with specified tools |
| NF-06 | No external dependencies beyond Qt | Single binary, no runtime deps |
| NF-07 | Standalone → integrable into PlotJuggler | Works as standalone, then embeds |
| NF-08 | Download in background thread | UI remains responsive during downloads |
| NF-09 | Performance: <100ms to load/filter registry | With ~50 extensions |
| NF-10 | Static linking in extensions | Plugins are self-contained |

---

## 6. Use Cases

### UC-01: User Discovers and Installs Extension

**Actor:** PlotJuggler User
**Preconditions:** PlotJuggler is running, internet available
**Flow:**
1. User opens Marketplace (Plugins → Marketplace)
2. User searches for "ROS 2"
3. System shows matching extensions
4. User clicks on "ROS 2 Streaming"
5. User sees extension details (description, version, author)
6. User clicks "Install"
7. System downloads ZIP, verifies checksum, extracts
8. System shows "Installation complete"
9. User closes Marketplace
10. Plugin is available in PlotJuggler

**Postconditions:** Extension installed and registered

### UC-02: User Updates Extension

**Actor:** PlotJuggler User
**Preconditions:** Extension installed, newer version available
**Flow:**
1. User opens Marketplace
2. User sees "Update available" badge on installed extension
3. User clicks "Update"
4. System backs up current version
5. System downloads and installs new version
6. System shows "Update complete"

**Postconditions:** New version installed, old version backed up

### UC-03: User Uninstalls Extension

**Actor:** PlotJuggler User
**Preconditions:** Extension installed; not a core (bundled) extension (those are uninstall-locked, see UC-08)
**Flow:**
1. User opens Marketplace
2. User navigates to installed extensions
3. User clicks "Uninstall" on extension
4. System shows confirmation dialog
5. User confirms
6. System removes extension files
7. System refreshes installed cache

**Postconditions:** Extension removed, installed cache refreshed

### UC-04: Plugin Fails to Load (Rollback Deferred)

**Actor:** System
**Preconditions:** Extension recently updated, backup exists
**Flow:**
1. PlotJuggler starts
2. System attempts to load plugin
3. Plugin crashes/fails
4. System reports the plugin load failure
5. Automatic backup restore is deferred

**Postconditions:** User is notified; any backup remains available for manual recovery

### UC-05: Developer Publishes Extension

**Actor:** Plugin Developer
**Preconditions:** Developer has GitHub account, uses template
**Flow:**
1. Developer creates tag (v1.0.0)
2. CI compiles for all platforms
3. CI packages ZIPs with plugin binaries and resources
4. CI creates GitHub Release
5. CI submits PR to registry repository
6. Registry validates schema and URLs
7. PR is merged
8. Extension appears in marketplace

**Postconditions:** Extension available to all users

### UC-06: First Launch of an Install with Bundled Plugins

**Actor:** System
**Preconditions:** Installed build or AppImage shipping plugins in the bundled dir; fresh user profile
**Flow:**
1. PlotJuggler starts
2. System seeds each bundled plugin into the extensions dir (one subfolder per id)
3. System scans the extensions dir and loads the seeded plugins
4. Marketplace shows them as installed core extensions (no Uninstall action)

**Postconditions:** Bundled plugins behave like normal installed extensions, managed from the extensions dir

### UC-07: Application Upgrade Ships Newer Core Plugins

**Actor:** System
**Preconditions:** Extensions dir holds core extensions seeded by an older application version; the new application bundles newer versions
**Flow:**
1. User launches the upgraded application
2. Seed compares each bundled version against the installed copy
3. Installed copies older than the bundled version are replaced (staged copy, then swap); an info diagnostic records old → new
4. Installed copies equal or newer are left untouched

**Postconditions:** Core extensions are at least at the shipped version; user installs ahead of it survive

### UC-08: User Updates a Core Extension Above the Bundled Version

**Actor:** PlotJuggler User
**Preconditions:** Core extension installed; registry offers a newer version
**Flow:**
1. User opens Marketplace and updates the core extension (normal update flow)
2. On later launches the seed leaves the newer copy untouched
3. Marketplace offers "Downgrade to bundled" instead of Uninstall
4. If the user downgrades, the shipped version is restored (staged, applied on restart)

**Postconditions:** User controls the version within [bundled, latest]; the id can never disappear

### UC-09: Developer Overrides Plugins with --plugin-dir

**Actor:** Plugin Developer
**Preconditions:** Freshly built plugin DSOs in a local folder
**Flow:**
1. Developer launches `plotjuggler4 --plugin-dir <build-output>`
2. Seed still syncs bundled plugins into the extensions dir (never into the override folder)
3. Scan loads the override folder as the top, version-blind tier; the developer's build wins every id conflict — even against a newer marketplace or custom-folder copy
4. Marketplace manages the override folder; nothing is treated as core there

**Postconditions:** Developer builds shadow same-id plugins for the session; the user profile keeps its core extensions

---

## 7. Extension Categories

| Category | Value | Description | Example |
|----------|-------|-------------|---------|
| Data Loader | `data_loader` | Loads data from files (atomic operation) | CSV, MCAP, ROS bags |
| Data Streamer | `data_streamer` | Continuous streaming at 50Hz, thread-safe | ROS 2, MQTT, ZMQ |
| Parser | `parser` | Conversion from byte blob to individual fields | Protobuf, FlatBuffers |
| Toolbox | `toolbox` | Tools with GUI (FFT, CSV export, quaternion) | FFT analyzer |
| Bundle | `bundle` | ZIP with multiple plugins from different families | ROS 2 Complete |

---

## 8. Corner Cases and Edge Cases

### 8.1 Network Issues

| Scenario | Expected Behavior |
|----------|-------------------|
| No internet connection | Show cached registry (if available), disable install/update |
| Registry URL unreachable | Show error message, offer retry |
| Download interrupted | Partial file deleted, user can retry |
| Checksum mismatch | Download rejected, user notified |

### 8.2 File System Issues

| Scenario | Expected Behavior |
|----------|-------------------|
| Insufficient disk space | Error before extraction, suggest cleanup |
| No write permission to extensions dir | Clear error message, suggest running with permissions |
| Extension directory doesn't exist | Create it automatically |
| Corrupted ZIP file | Extraction fails gracefully, user notified |
| A second PlotJuggler instance opens the same store | The first instance keeps write access; the second is read-only (it still lists installed extensions) and refuses every mutation with "Another PlotJuggler instance is managing extensions" |

### 8.3 Version Conflicts

| Scenario | Expected Behavior |
|----------|-------------------|
| Extension requires newer PlotJuggler | Show warning, prevent install |
| Downgrade requested | Reject with a diagnostic; keep the local install unchanged |
| Same version reinstall, different payload | Ask confirmation, then reinstall |
| Same version reinstall, byte-identical payload | Report success without touching the installed directory: nothing is asked, staged, or restarted. Decided on the content digest of both trees, not on the declared version |

### 8.4 Update Staging & Restart

| Scenario | Expected Behavior |
|----------|-------------------|
| Update requested | Stage the new version and apply on restart — never hot-swap a plugin the running session has loaded |
| Invalid staged update | Remove staged files and leave the active install untouched |
| PlotJuggler crashes before applying update | Pending update remains for next start |
| Uninstall requested | Mark the directory for restart cleanup and report it as pending; removed on next start, on every platform — never delete in place a directory whose DSO the running session has loaded |
| Install requested for an id with a staged uninstall | Refuse until the restart has applied the removal, which would otherwise delete the new payload |

### 8.5 Plugin Loading

| Scenario | Expected Behavior |
|----------|-------------------|
| Plugin crashes on load | Report load failure; automatic rollback is deferred |
| Plugin incompatible with current SDK | Clear error message, don't load |
| Embedded manifest missing or invalid | Reject install or staged promotion with diagnostics |
| Marketplace opens in a host app | Seed the initial UI from the host's loaded-plugin snapshot, then reconcile with disk refreshes on demand |

### 8.6 Bundled (core) extensions and folder precedence

| Scenario | Expected Behavior |
|----------|-------------------|
| Bundled version equals the installed one but the files differ (rebuilt without a version bump) | No refresh — the comparison is version-only. Shipping a change requires bumping the plugin version. |
| Extension folder exists but holds no loadable plugin (gutted or half-written copy) | Treated as absent: the seed re-copies the bundled payload. |
| Refresh fails mid-way (disk full, locked files) | The previous working copy is kept or restored; a warning diagnostic is emitted and the seed retries next launch. |
| Seed cannot write the extensions dir (read-only profile) | Bundled plugins are unavailable that session; warning diagnostic, retried next launch. |
| Staged marketplace update pending at startup | The staged install is promoted first; the seed compares against the promoted version, so it never clobbers a just-applied upgrade. |
| User wants to hold a core extension below the bundled version | Not possible through the marketplace (the seed lifts it back). Escape hatch: place the older build in a custom Preferences folder — user-explicit tiers win version-blind. |
| User adds the bundled dir itself as a custom folder | It is then scanned like any custom folder (user-explicit choice); plugins may appear loaded-but-not-installed, and for an AppImage the mount path changes every run. |

---

## 9. Constraints

### 9.1 Must NOT Do

- **No backend server** — All hosting via GitHub (serverless)
- **No Qt dependency in plugins** — Plugins use abstract SDK only
- **No database** — Registry data is JSON; installed state is discovered from embedded DSO manifests, not a local state database
- **No user accounts** — Anonymous usage
- **No telemetry** — No data collection without consent

### 9.2 Assumptions

- Users have internet access for initial install
- GitHub raw URLs remain accessible
- Extensions are < 50MB compressed
- Registry has < 100 extensions in foreseeable future
- **Plugin ABI surface stays narrow.** Plugins do not link Qt — they
  consume the C ABI declared in `plotjuggler_sdk/pj_plugins` and
  expose only a small embedded manifest plus their vtable. This keeps
  cross-platform CI simple and decouples plugin builds from Qt
  version churn.

### 9.3 Out of Scope (v1.0)

- Paid extensions / license management
- Dependency resolution between extensions
- Automatic updates (always user-initiated)
- Plugin sandboxing / security isolation
- Extension ratings / reviews (metrics only)

---

## 10. Acceptance Criteria (MVP)

The minimum viable product is successful if:

1. [ ] Opens as standalone Qt Widgets app
2. [ ] Loads registry JSON from URL (GitHub raw)
3. [ ] Shows extension list with cards
4. [ ] Allows searching and filtering by category
5. [ ] Shows selected extension detail
6. [ ] Downloads ZIP with checksum verification
7. [ ] Extracts to local directory and registers as installed
8. [ ] Detects new available versions
9. [ ] Allows extension uninstallation
10. [ ] Works on Linux (Windows/macOS as stretch goal for MVP)

---

## 11. Data Formats

### 11.1 Registry JSON Schema

```json
{
  "registry_version": "1.0",
  "last_updated": "ISO8601 timestamp",
  "extensions": [
    {
      "id": "unique-extension-id",
      "name": "Display Name",
      "description": "Short description",
      "author": "Author Name",
      "publisher": "Publisher Name",
      "website": "https://...",
      "repository": "https://github.com/...",
      "license": "SPDX identifier",
      "icon_url": "https://... (optional)",
      "category": "data_loader|data_streamer|parser|toolbox|bundle",
      "tags": ["tag1", "tag2"],
      "version": "semver",
      "min_sdk_required": "semver",
      "min_plotjuggler_version": "semver",
      "plugins": [
        {
          "name": "PluginClassName",
          "type": "plugin_type",
          "library": "library_name_without_extension"
        }
      ],
      "platforms": {
        "linux-x86_64": {
          "url": "https://...",
          "checksum": "sha256:..."
        }
      },
      "changelog": {
        "1.0.0": "Initial release"
      }
    }
  ]
}
```

### 11.2 Installed State

There is no separate local state file. Installed extensions are discovered at runtime by
scanning `extensions_dir`, loading candidate plugin DSOs, and reading each DSO's embedded
plugin manifest. The marketplace never writes installed-state or plugin-manifest sidecars.
Staged updates use a transient `.pj_pending_install` intent file so restart-time
promotion can revalidate the staged DSO against the registry id/version that created it.

Fields read from the embedded plugin manifest:

| Field | Source |
|-------|--------|
| `id` | Embedded plugin manifest key `"id"` |
| `version` | Embedded plugin manifest key `"version"` |
| `name` | Embedded plugin manifest key `"name"`, falling back to `id` |
| `description` | Embedded plugin manifest key `"description"` |
| `category` | Embedded plugin manifest key `"category"` |
| `abi_major` | ABI major reported by SDK DSO discovery; zero means an older/unknown descriptor |
| `min_sdk_required` | Embedded plugin manifest key `"min_sdk_required"`; empty means undeclared. Hard SDK-contract gate |
| `min_plotjuggler_version` | Embedded plugin manifest key `"min_plotjuggler_version"`; empty means no declared floor. Gates a local-ZIP sideload the way the registry field gates a registry install |
| `install_date` | Last-modified timestamp of the extension root directory |
| `path` | The scanned subdirectory itself |
| `enabled` | Always `true` by default (no persistence yet) |

A directory holding several plugin DSOs is installed and gated as one extension, so
they must agree on `id` and `version`. The plugin version and each non-empty
SDK/application floor are strict SemVer; the extension adopts the highest component
floor. Malformed metadata rejects new admission but does not erase an already-installed
extension from the manager: that row remains visible and uninstallable while runtime
loading rejects it.
The host may normalize the single `v`/`V` tag prefix accepted by the PJ4 build, but
plugin declarations themselves remain strict SemVer.

Registry compatibility fields are an early filter, not authority over the extracted
code. Before initial promotion, before staged promotion after restart, during bundled
seed rescue, and immediately before runtime registration, the same headless evaluator
checks embedded plugin version, ABI major, `min_sdk_required`, and
`min_plotjuggler_version`. An
authoritative `--plugin-dir` copy retains strict id shadowing, but a copy that fails
this gate is not loaded and does not fall back silently to the managed store.

### 11.3 Registry Extension Schema

```json
{
  "id": "extension-id",
  "version": "semver",
  "min_sdk_required": "semver",
  "min_plotjuggler_version": "semver",
  "plugins": [
    {
      "name": "PluginClassName",
      "type": "plugin_type",
      "library": "library_name",
      "ui_file": "optional_ui_file.ui"
    }
  ]
}
```

---

## 12. Open Follow-Ups

Remaining implementation follow-ups are tracked in [TODO.md](TODO.md).

---

## Document Maintenance

This file should be updated when:
- New requirements are identified
- Requirements are clarified or changed
- Use cases are added or modified
- Constraints change

**Do NOT add:**
- Implementation details
- Code examples
- Architecture decisions (→ ARCHITECTURE.md)
- How-to guides (→ USER_MANUAL.md)
