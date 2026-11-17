# PlotJuggler Marketplace — Technical Specification

> The contract between the marketplace client, the registry and extension packages (sections 5, 6 and 10), and the
> design it follows. How the client behaves today is in [ARCHITECTURE.md](./ARCHITECTURE.md); the end-user flow is in
> [USER_MANUAL.md](./USER_MANUAL.md).
>
> **Author:** Pablo Iñigo Blasco · **Stack:** C++20, Qt 6 Widgets, CMake, Conan/Pixi

---

## Abstract

PlotJuggler has grown significantly over the past years, evolving from an internal tool to becoming a de facto standard for data visualization in robotics. With this growth comes a problem: **how do we allow the community to contribute plugins without requiring a full PlotJuggler recompilation for each update?**

The answer is the **PlotJuggler Marketplace**, an extension distribution system inspired by the VSCode model. The core idea is simple: a user opens the marketplace inside PlotJuggler, searches for "ROS 2", clicks "Install", and within seconds has the plugin running. No compilation, no dependency management, no worrying about Qt versions.

To achieve this, the system relies on a completely **serverless architecture**: no backend to maintain, no infrastructure costs. Everything lives on GitHub — a JSON file with the plugin catalog and binaries distributed as GitHub Releases. Extension repositories run CI that compiles for Linux, Windows and macOS, packages the binaries, and updates the catalog automatically.

The most interesting technical challenge is **ABI compatibility**. Historically, PlotJuggler plugins depended on Qt, which meant that a plugin compiled with Qt 5.15 wouldn't work with PlotJuggler compiled with Qt 6.2. The adopted solution is radical: **data plugins no longer depend on Qt at all**. They define their UI through .ui files (pure XML) and use an abstract SDK for logic. PlotJuggler renders the UI and routes events. This eliminates the compatibility problem at its root; the one exception is the Widget plugin family, which creates real Qt widgets, links Qt, and is checked against the host's Qt at load time.

---

## Table of Contents

1. [Feature List](#1-feature-list)
2. [Terminology](#2-terminology)
3. [System Architecture](#3-system-architecture)
4. [Design Decisions](#4-design-decisions)
5. [Registry — Format and Hosting](#5-registry--format-and-hosting)
6. [Extensions — Package Structure](#6-extensions--package-structure)
7. [Build System](#7-build-system)
8. [CI System](#8-ci-system)
9. [Extension Repositories](#9-extension-repositories)
10. [Compatibility and ABI](#10-compatibility-and-abi)
11. [Windows Management](#11-windows-management)
12. [Graphical Interface](#12-graphical-interface)
13. [Code Structure](#13-code-structure)

---

## 1. Feature List

### 1.1 Client Features (Marketplace UI)

| Category           | Feature              | Description                                                   |
| ------------------ | -------------------- | ------------------------------------------------------------- |
| **Discovery**      | Extension listing    | Display all available extensions in a sortable table          |
|                    | Search               | Search by name, description, and tags                         |
|                    | Category filtering   | Data Loader, Data Streamer, Parser, Toolbox                   |
|                    | Extension detail     | Details footer for the selected row: description, changelog, and the actions |
| **Installation**   | Secure download      | ZIP artifact download with SHA256 verification                |
|                    | Automatic extraction | Decompression to a staging area, then promotion into the extensions directory |
|                    | Platform detection   | Automatic selection of correct artifact (Linux/Windows/macOS) |
| **Updates**        | Update detection     | Local vs registry version comparison (semver)                 |
|                    | Individual update    | Update a specific extension, applied at the next start        |
|                    | Bulk update          | "Update All" for multiple extensions                          |
|                    | Automatic backup     | Backup of previous version when an update is promoted         |
| **Uninstallation** | Staged removal       | The directory is deleted at the next start                    |
| **Management**     | Backup diagnostics   | Report retained backup paths when an update install fails     |
|                    | Persistent state     | Installed state derived from embedded plugin manifests        |
|                    | Enable/disable       | Installed but not loaded, applied at the next start           |
|                    | Local ZIP install    | Sideload a plugin the registry does not list                  |
| **UI/UX**          | Download progress    | Progress bar in status bar                                    |
|                    | Notifications        | Status messages                                               |

### 1.2 CI System Features (For Developers)

These describe what an extension repository's own CI is expected to do; nothing in the client implements them.

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

### 1.3 Registry Features

| Category    | Feature             | Description                                           |
| ----------- | ------------------- | ----------------------------------------------------- |
| **Catalog** | Complete listing    | JSON with all available extensions                    |
|             | Metadata            | Name, description, author, license, tags, category    |
|             | Versioning          | Current version and the minimum plugin SDK version    |
|             | Cross-platform      | URLs and checksums per platform (Linux/Windows/macOS) |
| **Hosting** | Static GitHub       | JSON file accessible via raw.githubusercontent.com    |
|             | Fresh fetch         | The client fetches the registry each time; it keeps no cache |
|             | One registry URL    | The registry URL is configurable; several at once are not supported |

---

## 2. Terminology

| Term           | Definition                                                                      |
| -------------- | ------------------------------------------------------------------------------- |
| **Extension**  | Marketplace distribution unit. Downloadable ZIP containing one or more plugins. |
| **Plugin**     | C++ module dynamically loaded (.so/.dll/.dylib) implementing an SDK interface.  |
| **Registry**   | Static JSON file on GitHub with the catalog of available extensions.            |
| **Plugin SDK** | Abstract library (no Qt) that plugins use for UI and data access.               |
| **Artifact**   | Compiled binary of an extension for a specific platform.                        |
| **Embedded manifest** | JSON string exported by each plugin DSO describing the installed plugin.  |
| **Core extension** | An extension whose id ships with the application; seeded into the extensions directory at startup and locked against uninstall. |

---

## 3. System Architecture

### 3.1 Overview

One of the first decisions was to avoid the complexity of maintaining a backend server. The key question was: *do we really need a server to distribute 30-50 plugins?* The answer is no.

GitHub provides everything we need for free:

- **Registry:** A simple JSON file in a repository. PlotJuggler downloads it via `raw.githubusercontent.com` each time the marketplace opens.
- **Artifacts:** Binaries are distributed as GitHub Releases. They're static URLs, with global CDN, no practical download limits.
- **Updates:** When a developer publishes a new version, their CI generates an automatic Pull Request to the registry repository. The schema is validated, URLs are verified, and it gets merged.

This architecture has an additional advantage: **any company can have their own private registry** simply by pointing PlotJuggler to another URL. No lock-in.

### 3.2 Component Diagram

![System Architecture](diagrams/architecture.png)

### 3.3 Diagnostic propagation

`ExtensionManager` exposes its lifecycle events through three channels at once: a bounded ring buffer accessible via `diagnostics()`, the `diagnosticReported(QString id, QString message, bool is_error)` Qt signal, and an optional `PJ::DiagnosticSink` (declared in `pj_base/include/pj_base/diagnostic_sink.hpp`) that is fed in addition to the other two when the host passes one to the constructor. The sink is a `std::function<void(const PJ::Diagnostic&)>` carrying a level (Info / Warning / Error), a `source` ("ExtensionManager", "PluginRegistry", ...), an optional plugin/extension `id`, a message, and a timestamp. Hosts that wire the same sink into both `ExtensionManager` and any non-marketplace component (e.g. `PJ::PluginRuntimeCatalog`) see one unified, ordered diagnostic stream they can render in a status bar, dialog, or log file. Modules in pure C++ remain Qt-free; `PJ::QtDiagnosticBridge` in `pj_marketplace` converts each event into a queued signal emission.

### 3.4 Design Principles

| Principle              | Why it matters                                                                                                                                                        |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Serverless**         | Zero infrastructure costs, zero server maintenance. GitHub does the heavy lifting.                                                                                    |
| **CI-first**           | Developers shouldn't have to configure anything beyond their extension repository's CI. Push a tag = automatic release.                                               |
| **Cross-platform**     | PlotJuggler runs on Linux, Windows, and macOS. Plugins must work on all three platforms without developers needing all three machines.                                |
| **Static linking**     | A plugin is a single .so/.dll file that works without installing anything else. This drastically simplifies installation and avoids dependency conflicts.             |
| **No Qt in data plugins** | If plugins depend on Qt, any Qt version change breaks all plugins. Data plugins use the SDK's C ABI instead; Widget plugins are the one family that links Qt.       |
| **Dogfooding**         | Official plugins (ROS 2, MCAP, etc.) use exactly the same process as external contributors. This ensures the process works and is well documented.                    |

---

## 4. Design Decisions

### 4.1 PlotJuggler Integration

Two approaches were considered: create an external plugin management tool (like `pip` or `npm`) or integrate it directly into PlotJuggler. The decision was clear: **native integration**.

The reasoning is that the typical PlotJuggler user doesn't want to leave the application to install plugins. They want to open a window inside PlotJuggler, search for what they need, install it, and keep working. It's the VSCode experience, not managing packages from a terminal.

### 4.2 Extension Repositories

The barrier to creating plugins should be low: an extension repository carries its own CI that builds for Linux, Windows and macOS, packages the ZIPs and opens the registry PR when a version tag is pushed. [`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) is the working example. Nothing in the client depends on how an extension repository builds or releases.

### 4.3 Build tools

A ZIP with one or more plugin DSOs works the same whether it was generated with Conan, Pixi, or manual compilation; installed metadata is read from the embedded manifest exported by each DSO. The decision therefore does not affect marketplace users: they just see plugins that install with one click.

### 4.4 Sizing

The system is designed for a modest catalog:

- **Current plugins:** ~20
- **Expected short-term:** ~30
- **Maximum estimate:** 40-50

This means a simple JSON file is more than sufficient as a registry. We don't need a database, we don't need sophisticated search. A JSON with 50 entries is small enough to load at once.

The plugin families are:

- **Data Loader:** Loads data from files (CSV, MCAP, ROS bags...)
- **Data Streamer:** Real-time streaming (ROS 2, MQTT, ZMQ...)
- **Parser:** Converts byte blobs into structured fields
- **Toolbox:** Tools with their own UI (FFT, exporters, transformations...)

---

## 5. Registry — Format and Hosting

### 5.1 JSON Format

```json
{
  "registry_version": "1.0",
  "last_updated": "2026-03-04T10:00:00Z",
  "extensions": [
    {
      "id": "ros2-streaming",
      "name": "ROS 2 Streaming",
      "description": "Stream ROS 2 topics into PlotJuggler in real-time",
      "author": "Davide Faconti",
      "publisher": "PlotJuggler",
      "website": "https://github.com/plotjuggler/ros2-streaming",
      "repository": "https://github.com/plotjuggler/ros2-streaming",
      "license": "Apache-2.0",
      "icon_url": "https://raw.githubusercontent.com/.../icon.png",
      "entry_point": "Data Streamers menu → ROS 2 Streaming",
      "category": "data_stream",
      "tags": ["ros2", "streaming", "middleware", "robotics"],
      "version": "1.2.3",
      "min_sdk_required": "0.21.0",
      "suggested_sdk_version": "0.24.0",
      "min_plotjuggler_version": "4.0.0",
      "plugins": [
        {
          "name": "ROS2StreamerPlugin",
          "type": "data_stream",
          "library": "libros2_streaming"
        }
      ],
      "platforms": {
        "linux-x86_64": {
          "url": "https://github.com/.../ros2-streaming-linux-x86_64.zip",
          "checksum": "sha256:a1b2c3d4e5f6...",
          "size_bytes": 2457600
        },
        "windows-x86_64": {
          "url": "https://github.com/.../ros2-streaming-windows-x86_64.zip",
          "checksum": "sha256:f6e5d4c3b2a1...",
          "size_bytes": 3145728
        },
        "macos-arm64": {
          "url": "https://github.com/.../ros2-streaming-macos-arm64.zip",
          "checksum": "sha256:1a2b3c4d5e6f...",
          "size_bytes": 2621440
        }
      },
      "changelog": {
        "1.2.3": "Fix reconnection timeout on network loss",
        "1.2.2": "Add QoS profile configuration",
        "1.2.0": "Initial marketplace release"
      }
    }
  ]
}
```

**What the client reads.** `id`, `name` and `version` are required: a missing or empty one rejects the whole registry. Optional: `description`, `author`, `publisher`, `website`, `repository`, `license`, `icon_url` (parsed, not shown), `entry_point` (where the user finds the extension in the app; empty means the host derives it), `category`, `tags`, `min_sdk_required`, `suggested_sdk_version`, `min_plotjuggler_version`, `platforms` and `changelog`. `version`, `min_sdk_required` and `suggested_sdk_version` are strict SemVer when present. Under `platforms`, only `url` and `checksum` are read; `checksum` is `sha256:<hex>` (the prefix and the case of the digest are not significant, and an empty checksum skips verification). `registry_version`, `last_updated`, `plugins` and `size_bytes` are ignored.

**Rules the client relies on.** A semantic `(id, version)` release slot is immutable and appears once, with every platform artifact in that one record; a registry that repeats one (build metadata included) is rejected. A release is kept for a platform only while its `min_sdk_required` is strictly lower than that of every newer release shipping that platform, and an older record's `platforms` is trimmed to the ones it still serves: the client admits any release whose floor it meets, with no upper bound, so older hosts keep resolving the highest release they can run.

### 5.2 Extension Categories

| Category      | Value            | Description                                       |
| ------------- | ---------------- | ------------------------------------------------- |
| Data Loader   | `data_loader`    | Loads data from files (atomic operation)          |
| Data Streamer | `data_stream`    | Continuous streaming at 50Hz, thread-safe         |
| Parser        | `message_parser` | Conversion from byte blob to individual fields    |
| Toolbox       | `toolbox`        | Tools with GUI (FFT, CSV export, quaternion)      |
| Bundle        | `bundle`         | ZIP with multiple plugins from different families |

The marketplace's filter row matches the first four values; an extension with any other category (including `bundle`) is never hidden by it.

### 5.3 Registry Hosting

The default registry is the `registry.json` of the `pj-plugin-registry` repository, which carries the registry validation (`scripts/validate_registry.py`):

**Default URL:** `https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json`

The URL is configurable (http, https and file schemes) in the application's Preferences ▸ Plugins page; see USER_MANUAL.md, "Registry URL".

### 5.4 Local State

There is no local state JSON. `ExtensionManager` rebuilds its installed cache by scanning `extensions/`, loading candidate plugin DSOs, and reading each DSO's embedded manifest. The remote registry remains the pre-install catalog.

---

## 6. Extensions — Package Structure

### 6.1 ZIP Contents

A ZIP holds exactly one top-level directory. For a registry install it must be named for the extension id; a sideloaded archive may use any name (the managed layout keys directories by the id in the embedded manifest). Entries must be regular files and directories with relative, forward-slash paths.

```
ros2-streaming-linux-x86_64.zip
└── ros2-streaming/                ← exactly one top-level directory, named for the id
    ├── libros2_streaming.so       ← Compiled plugin(s)
    ├── ros2_streaming.ui          ← Qt Creator UI file (pure XML), optional
    ├── README.md                  ← Description, optional
    └── LICENSE                    ← License
```

### 6.2 Embedded Plugin Manifest

```cpp
PJ_DATA_SOURCE_PLUGIN(ROS2StreamerPlugin,
    R"({"id":"ros2-streaming","name":"ROS 2 Streaming","version":"1.2.3"})")
```

A directory holding several plugin DSOs is one extension: they must agree on `id` and `version`.

### 6.3 Compilation Requirements

- **Static linking:** All dependencies embedded in the binary
- **No Qt in data plugins:** the plugin does NOT depend on Qt; Widget plugins link Qt and must be built against the Qt version PlotJuggler ships
- **SDK only:** Only dependency is the abstract Plugin SDK interface
- **.ui files:** Pure Qt Creator XML, not compiled

---

## 7. Build System

How an extension is built is outside the client's concern: it consumes the ZIP of section 6. [`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) and the guides under `pj_plugins/docs/` in the [`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk) repository show a working CMake setup.

---

## 8. CI System

### 8.1 Release Flow

![CI Release Flow](diagrams/ci-release-flow.png)

1. Developer creates a tag (`git tag v1.2.3`)
2. GitHub Actions detects the tag and runs the release workflow
3. Compiles for all 3 platforms in parallel (matrix build)
4. Packages artifacts in ZIPs with plugin binaries and checksums
5. Creates a GitHub Release with attached artifacts
6. Generates an automatic PR to the registry with the new version
7. PR is automatically validated (schema, URLs, checksums)
8. If validation passes, auto-merge occurs

### 8.2 Installation Flow (Client)

1. The user clicks "Install" (a new extension) or "Update" (a newer version)
2. The current platform is looked up in the record's `platforms`
3. The ZIP is downloaded and its SHA256 checksum is verified
4. The archive is extracted into a hidden transaction directory under `extensions.install_stage/`, a sibling of `extensions/`
5. A helper process loads the plugin DSO and validates its embedded manifest against the registry id and version, the ABI major and `min_sdk_required`
6. A new extension is renamed into `extensions/<id>/` and re-validated at that location; an update is renamed into `.extension_staging/<id>/` with a `.pj_pending_install` intent and promoted at the next start
7. The installed set is refreshed from discovery

When the marketplace opens inside a host application, it may be seeded with the host's already-loaded plugin snapshot before the first render. That snapshot is initialization data only; the embedded manifest remains the authority for installed version reporting.

### 8.3 Backup and Rollback Status

Every successful update, on every platform, moves the previous version into `.backup/<id>-<uuid>/` when the staged directory is promoted at the next start. If the staged promotion fails after the backup move, the previous version is moved back into place; if even that fails, the diagnostic surfaces both paths so the user can recover manually.

Automatic *post-load* rollback (restoring from backup if the freshly installed plugin later fails to load) is not implemented. The backup directory is the manual recovery point.

---

## 9. Extension Repositories

Extension repositories own their CI and release workflow (section 8.1). The client requires only the per-platform ZIPs of section 6 and a registry record of section 5; [`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) is the reference for the workflow.

---

## 10. Compatibility and ABI

### 10.1 The Compatibility Problem

Binary compatibility (ABI) is probably the biggest technical headache in any C++ plugin system. The typical scenario is:

1. User installs plugin compiled with Qt 5.15.2
2. User updates PlotJuggler to a version compiled with Qt 6.2
3. Plugin crashes because Qt's internal structures have changed

This problem has plagued PlotJuggler for years. Users report that "the ROS plugin stopped working after updating", and the only solution was to recompile the plugin.

### 10.2 The Solution: No Qt in Data Plugins

The adopted solution is radical but effective: **data plugins do not use Qt directly**.

Instead of plugins creating Qt widgets, they do two things:

1. **Define their UI in a .ui file:** It's pure XML, Qt Creator format. No compiled Qt code.
2. **Use an abstract SDK:** To read widget values, respond to events, etc.

When PlotJuggler loads the plugin:

1. It reads the .ui file and creates the corresponding widgets (using its version of Qt)
2. It connects those widget events to the plugin via the SDK
3. The plugin never touches Qt directly

A data plugin compiled today keeps working when PlotJuggler moves to another Qt version. The contract is the SDK, not Qt. Widget plugins are the exception: they create real Qt widgets and link Qt, so the host compares each library's Qt major.minor and compiler family with its own before creating anything and lists a mismatching library as unusable with its reason.

### 10.3 Compatibility Policy

- Registry versions and `min_sdk_required` are strict SemVer. A semantic `(id, version)` release slot is immutable and appears once, with every platform artifact in that record. After extraction the host independently enforces the embedded plugin version, ABI and SDK floor through the same gate used immediately before runtime loading
- `min_plotjuggler_version` is deprecated and never gates. A plugin that needs a newer host declares the SDK release that added what it needs in `min_sdk_required`; older hosts then keep resolving the highest release whose floor they meet
- A plugin built for another ABI major than the host's is rejected

---

## 11. Windows Management

### 11.1 The Windows Problem

Windows has an annoying quirk for plugin systems: **it doesn't allow modifying files that are in use**. If PlotJuggler has `ros2_streaming.dll` loaded, you can't delete it or overwrite it.

On Linux and macOS you can delete a file that's memory-mapped and the process keeps using the old version until it closes, but reusing the same path inside that process is just as unsafe there: the loader answers a second `dlopen` of a path from the image it loaded first. A loaded plugin therefore cannot be replaced or removed while PlotJuggler is running, on any platform.

### 11.2 Solution: Staging

Updates, replacements and removals are staged and applied at the next start, on every platform:

1. User clicks "Update"
2. The new version downloads into a hidden transaction folder `.pj_install_<id>_<uuid>/` (created under `extensions.install_stage/`, a sibling of `extensions/` that shares its filesystem but is never walked by the recursive plugin scan)
3. The staged DSO is loaded by a helper process and its embedded manifest is validated against the registry id/version
4. The transaction folder is renamed to `.extension_staging/<id>/` and a transient `.pj_pending_install` intent is written with the registry id/version
5. Message shown: "Restart required"
6. When PlotJuggler starts:
   - Reads `.pj_pending_install`
   - Validates the intent's id/version against safe-path/regex rules (rejects path traversal or non-semver tokens)
   - Moves the current version into `.backup/`, then moves the staged directory into `extensions/<id>/`
   - Revalidates the promoted DSO at its final location (catches rpath/dep issues that hold in staging but break in `extensions/`)
7. If any validation step fails the active install is left untouched (the previous version is restored). The broken stage is removed; if removal also fails (file lock), the directory is renamed to `.pj_quarantine_<name>_<uuid>/` and the path is included in the diagnostic so the user can clean it up manually instead of facing the same error every startup.

A removal is recorded in a journal directory `extensions.state/` and the directory is deleted at the next start.

### 11.3 Directory Structure

The root is `QStandardPaths::AppDataLocation` (the `PlotJuggler/PlotJuggler4` org/app pair): Linux `~/.local/share/PlotJuggler/PlotJuggler4/`, macOS `~/Library/Application Support/PlotJuggler/PlotJuggler4/`, Windows `%LOCALAPPDATA%/PlotJuggler/PlotJuggler4/`.

```
<config-root>/
├── extensions/              ← Active plugins
│   ├── ros2-streaming/
│   └── csv-loader/
├── extensions.install_stage/ ← Transaction directories of installs in progress
├── extensions.state/        ← Pending removals
├── .extension_staging/      ← Staged updates, promoted at the next start
│   └── plugin-id/
│       └── .pj_pending_install      ← Intent file
└── .backup/                 ← Previous versions; automatic rollback is not implemented, restore manually
    ├── ros2-streaming-<uuid>/
    └── csv-loader-<uuid>/
```

ARCHITECTURE.md, "Disk layout", lists every directory the client keeps beside `extensions/`.

---

## 12. Graphical Interface

The marketplace is one window: a toolbar (category filter toggles, search, "Install local...", "Update All", a settings button), a sortable table of extensions, a details footer for the selected row that carries every action (install or update, uninstall or downgrade to bundled, the enable toggle, a link to the website), and a status line with a progress bar and a diagnostics button. There is no separate detail dialog. USER_MANUAL.md describes it from the user's side.

---

## 13. Code Structure

The module layout and the key headers are in `pj_marketplace/CLAUDE.md`; the components and their responsibilities are in ARCHITECTURE.md, "Components".
