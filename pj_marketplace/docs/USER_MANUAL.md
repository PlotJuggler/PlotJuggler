# PlotJuggler Marketplace — User Manual

> **Version:** 1.0.0
> **Last Updated:** 2026-05-19
> **Audience:** End users, developers, and LLMs assisting with the project

---

## 1. Quick Start

### For End Users

1. Open PlotJuggler
2. Open the **File** menu and choose **Extensions Marketplace…** (keyboard shortcut Ctrl+Shift+M)
3. Search for the extension you need (e.g., "ROS 2")
4. Click **Install**
5. Restart PlotJuggler if prompted
6. Your new plugin is ready to use

### For Plugin Developers

1. Use the [extension-template](https://github.com/plotjuggler/extension-template) on GitHub
2. Click "Use this template" to create your repo
3. Modify the plugin code in `src/`
4. Push a tag (`git tag v1.0.0 && git push --tags`)
5. CI automatically builds, packages, and publishes
6. Submit PR to add your extension to the registry

---

## 2. User Guide

### 2.1 Opening the Marketplace

**From PlotJuggler:**
- Open the **File** menu and choose **Extensions Marketplace…**. The **Help ▸ Installed Extensions** submenu lists already-installed plugins (informational only — managing extensions happens in the Marketplace).
- Keyboard shortcut: Ctrl+Shift+M

**Standalone (development only):**
```bash
./build/pj_marketplace/pj_marketplace_app
```

### 2.2 Browsing Extensions

The marketplace window shows a list of all extensions with their status:

| Column | Content |
|--------|---------|
| **Name** | Extension name |
| **Version** | Installed version when available, otherwise registry version. The registry version is appended (`1.0.0 → 1.1.0`) only when an update is available; a locally-newer build shows just its own version, and the `Installed` column says `Local newer` |
| **Status** | `[install]`, `[installed]`, `[update]`, or `[local newer]` |

**To see extension details:** Double-click on any extension to open a detail dialog with full information (description, author, changelog).

If PlotJuggler already has the plugin loaded at startup, the marketplace is seeded with that loaded state before the first render so the card version matches what the app already opened.

**Quick tip:** Hover over an extension to see a brief description tooltip.

### 2.3 Searching and Filtering

**Search box:** Type to filter by name, description, or tags
- Example: `ros` finds "ROS 2 Streaming", "ROS Bag Loader"
- Example: `csv` finds "CSV Loader", "CSV Exporter"

**Filter row:** A row of toggle buttons under the toolbar. Any number of them can
be active at the same time.

- **Installed** — only extensions that are already installed
- **Data Source** — extensions that load data from files
- **Data Streamer** — extensions that stream data continuously
- **Parsers** — extensions that decode byte blobs into fields
- **Toolboxes** — extensions that add a tool with its own GUI

The four category toggles combine as "any of these": with none active every
category is listed, and activating several lists the extensions belonging to any
one of them. **Installed** is independent — it narrows whichever categories are
active rather than replacing them, so "Installed + Parsers" lists the installed
parsers. The search box narrows the result further.

### 2.4 Installing an Extension

1. Find the extension in the list
2. (Optional) Double-click to see details in a dialog
3. Click the **[install]** button next to the extension
4. Wait for download and extraction
5. See "Installation complete" message
6. Button changes to **[installed]**

**On Windows:** You may see "Restart required to complete installation"

### 2.5 Updating an Extension

1. Extensions with updates show an **Update available** badge
2. Click on the extension
3. Click **Update**
4. The new version is staged and the extension shows **Needs Restart** — the update is applied on the next launch, so the currently loaded plugin keeps working for the rest of the session
5. Once every queued update has been staged, a **Restart required** dialog summarizes how many changes are waiting (one dialog per batch, also shown for staged uninstalls and downgrades)
6. On restart, the old version is automatically backed up before the new one is promoted
7. If promotion fails, the old version remains in `.backup/` and can be recovered manually

**Update All:** Click "Update All" in the toolbar to stage updates for all extensions at once

### 2.6 Uninstalling an Extension

1. Click on the installed extension
2. Click **Uninstall**
3. Confirm in the dialog
4. The removal is **staged**: the extension leaves the list and its row shows
   **Needs Restart**, and the files are deleted the next time PlotJuggler starts

An extension the session already loaded keeps running until that restart —
its code cannot be unmapped from a live process — so the removal is reported as
pending rather than done. The same id cannot be reinstalled until the restart
has applied the removal.

If the deletion cannot go through at that next start (a file locked by another
process, antivirus, missing permissions), the extension is reported in the
diagnostics and retried at the following start. It stays unloaded in the
meantime, so a removal that is stuck on disk never brings the plugin back.

An uninstalled id stays on the disabled list even after its files are gone, so
that nothing but an explicit install can make it load again. Installing the
extension a second time clears that automatically. The one case where it shows:
copying an extension folder back by hand does **not** re-enable it, so use the
enable toggle after doing so.

Core (bundled) extensions have no Uninstall action — see §2.8.

### 2.7 Enabling/Disabling Extensions

*Planned — see [TODO.md](TODO.md). Today, removing an extension requires Uninstall.*

### 2.8 Core (Bundled) Extensions

Extensions shipped with the application (inside the installer or AppImage) are
**core** extensions. At every startup they are seeded into the extensions
directory, so they are managed like any other install — with three differences:

- **They cannot be uninstalled.** The id ships with the app; removing it would
  just be undone on the next launch.
- **They follow application upgrades.** When a new PlotJuggler version bundles a
  newer plugin, your installed copy is refreshed to it automatically. A copy you
  updated *above* the bundled version through the marketplace is left alone.
- **Downgrade to bundled.** If you updated a core extension and want the shipped
  version back, use **Downgrade to bundled** (staged, applied on restart).

To pin a core plugin to an older build, place that build in a custom plugin
folder (Preferences → Plugins): custom folders override the extensions dir
regardless of version. The same page lists every scanned plugin folder.

---

## 3. Developer Guide

> **Note on plugin shape:**
>
> Plugins are pure C++ DSOs that consume the C ABI declared in
> `plotjuggler_sdk/pj_plugins`. They do **not** link Qt — only the
> host (PlotJuggler / the marketplace) does. Each plugin exposes a
> small embedded JSON manifest (`id`, `name`, `version`, …) through the
> SDK export macro; the marketplace reads that manifest to recognise
> installed plugins. The workflow below describes the full process
> for these plugins.
>
> **CI Options:** Extensions can live in separate repos (one per
> extension) or in a mono-repo with per-component releases (see
> [Foxglove MCAP](https://github.com/foxglove/mcap) as reference). The
> registry supports both approaches.

### 3.1 Creating a New Extension

**Prerequisites:**
- C++ development environment
- CMake 3.16+
- Conan package manager
- Git

**Steps:**

1. **Create repo from template:**
   ```bash
   # Go to https://github.com/plotjuggler/extension-template
   # Click "Use this template"
   # Clone your new repo
   git clone https://github.com/YOUR_USERNAME/my-extension.git
   cd my-extension
   ```

2. **Modify the plugin:**
   - Edit `src/my_plugin.cpp`
   - Update the embedded manifest string passed to the SDK export macro
   - Add UI in `ui/my_dialog.ui` (optional)

3. **Build locally:**
   ```bash
   conan install . --profile profiles/linux_static --build=missing
   cmake --preset conan-release
   cmake --build --preset conan-release
   ```

4. **Test locally:**
   - Copy built files to your platform's extensions directory (Linux: `~/.local/share/PlotJuggler/PlotJuggler4/extensions/my-extension/` — see §5.1 for other OSes)
   - Open PlotJuggler and verify plugin loads

5. **Release:**
   ```bash
   git add .
   git commit -m "feat: my awesome plugin"
   git tag v1.0.0
   git push && git push --tags
   ```

6. **Wait for CI:**
   - CI builds for Linux, Windows, macOS
   - CI creates GitHub Release with artifacts
   - CI generates registry PR

7. **Submit to registry:**
   - Review the auto-generated PR
   - Merge to add to public marketplace

### 3.2 Embedded Plugin Manifest

Every plugin DSO must export an embedded manifest through the SDK macro:

```cpp
PJ_DATA_SOURCE_PLUGIN(MyPlugin,
    R"({"id":"my-extension","name":"My Extension","version":"1.0.0"})")
```

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Unique identifier (lowercase, hyphens) |
| `name` | Yes | Human-readable plugin name |
| `version` | Yes | Semantic version (X.Y.Z) |
| `encoding` | Parsers only | Message encoding handled by a parser plugin |
| `file_extensions` | No | File suffixes handled by a file source plugin |
| `capabilities` | No | Optional capability tags |

### 3.3 Plugin Types

| Type | Base class (export macro) | Purpose |
|------|---------------------------|---------|
| `data_loader` | `PJ::DataSourcePluginBase` (`PJ_DATA_SOURCE_PLUGIN`) | Load data from files |
| `data_streamer` | `PJ::DataSourcePluginBase` (`PJ_DATA_SOURCE_PLUGIN`) | Real-time data streaming |
| `parser` | `PJ::MessageParserPluginBase` (`PJ_MESSAGE_PARSER_PLUGIN`) | Parse binary data to fields |
| `toolbox` | `PJ::ToolboxPluginBase` (`PJ_TOOLBOX_PLUGIN`) | Custom tools with UI |

A single `DataSourcePluginBase` serves both file and streaming sources; whether it loads files or streams live data is declared via `capabilities()`/manifest, not by a separate base class.

### 3.4 Best Practices

- **No Qt dependency:** Use the SDK, not Qt directly
- **Static linking:** Embed all dependencies
- **Test on all platforms:** Use CI matrix build
- **Semantic versioning:** Follow semver strictly
- **Clear README:** Explain what your plugin does
- **License:** Include LICENSE file (Apache-2.0 recommended)

---

## 4. Troubleshooting

### 4.0 Diagnostics dialog

The marketplace toolbar shows a **Details** button whenever there are recent diagnostics. Click it to open a read-only log of marketplace lifecycle events (install/uninstall/staged-promotion failures, quarantine moves, registry-fetch failures). The most recent error also appears in the status bar; a successful registry refresh clears the sticky error so progress messages aren't suppressed.

When the marketplace runs **inside** a host application (e.g. PlotJuggler), the host can subscribe to the same diagnostic stream alongside its own plugin-load events; see ARCHITECTURE.md §3.3 *ExtensionManager — Diagnostic propagation*.

### 4.1 Common Issues

| Problem | Cause | Solution |
|---------|-------|----------|
| "Extension not loading" | Incompatible version | Check `min_plotjuggler_version` |
| "Requires PlotJuggler X or newer" | The package declares a minimum host version this build does not meet. Raised for a local ZIP as well, from the manifest embedded in its plugin | Update PlotJuggler, or obtain a build of the extension made for this version |
| "Download failed" | Network issue | Check internet, try again |
| "Checksum mismatch" | Corrupted download | Try again, report if persistent |
| "Cannot update (Windows)" | DLL in use | Restart PlotJuggler |
| "Installed version is newer" | Local plugin is ahead of registry | Downgrade is blocked; keep the local version |
| "Update failed after backup" | New artifact did not install | Check marketplace diagnostics for the retained backup path |
| "Post-promotion validation failed" | The DSO loads in the staging area but not from `extensions/` (rpath/dep issue) | The install is rolled back; check the diagnostic for the linker error |
| "Could not mark … for restart cleanup" | Marketplace could not write the `.pj_pending_uninstall` marker (permissions, or AV on Windows) | The uninstall is **not** scheduled; resolve the file-permission issue and retry |
| "Moved to quarantine: …" | A previous staged update could not be removed; it has been moved aside | Inspect the quarantined directory and delete it manually once safe |
| "Invalid registry URL" when saving Preferences | The Marketplace registry URL field holds a non-http(s)/file value | Fix the URL or clear the field to restore the default (see §5.2) |

### 4.2 Log Locations

| OS | Path |
|----|------|
| Linux | `~/.local/share/PlotJuggler/logs/` |
| Windows | `%APPDATA%/PlotJuggler/logs/` |
| macOS | `~/Library/Application Support/PlotJuggler/logs/` |

### 4.3 Reset Marketplace

If the marketplace is broken, remove the extensions and staging directories under your platform's config root (see §5.1):

```bash
# Linux
rm -rf ~/.local/share/PlotJuggler/PlotJuggler4/extensions/
rm -rf ~/.local/share/PlotJuggler/PlotJuggler4/.extension_staging/

# macOS
rm -rf ~/Library/Application\ Support/PlotJuggler/PlotJuggler4/extensions/

# Windows
rmdir /s %LOCALAPPDATA%\PlotJuggler\PlotJuggler4\extensions
rmdir /s %LOCALAPPDATA%\PlotJuggler\PlotJuggler4\.extension_staging
```

### 4.4 Reporting Bugs

1. Check existing issues at https://github.com/plotjuggler/marketplace/issues
2. Include:
   - PlotJuggler version
   - OS and version
   - Extension name and version
   - Error message or log excerpt
   - Steps to reproduce

---

## 5. Reference

### 5.1 Directory Structure

The marketplace uses the application's writable data location (resolved by `QStandardPaths::AppDataLocation`, i.e. the `PlotJuggler/PlotJuggler4` org/app pair):

| OS | Root |
|----|------|
| Linux | `~/.local/share/PlotJuggler/PlotJuggler4/` |
| macOS | `~/Library/Application Support/PlotJuggler/PlotJuggler4/` |
| Windows | `%LOCALAPPDATA%/PlotJuggler/PlotJuggler4/` |

Inside that root:

```
<config-root>/
├── extensions/                      # Active installed extensions, including
│   └── my-extension/                # core (bundled) ones seeded at startup
│       └── libmy_plugin.so
├── .extension_staging/      # Staging area: updates land here and are promoted
│   │                                # on the next startup; a fresh install uses it
│   │                                # only as the post-promotion validation gate
│   │                                # (it activates immediately)
│   └── my-extension/
│       └── .pj_pending_install      # Intent file for restart-time update apply
└── .backup/                         # Pre-update backups (all platforms); automatic rollback deferred — restore manually
```

### 5.2 Registry URL

**Default:** `https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json`

**Custom registry:** open **Preferences → Plugins**. The *Marketplace registry
URL* row has a **default / custom** switch: *default* shows the built-in URL
read-only; *custom* enables the field for editing. When you finish editing,
the value is checked — it must be an http(s) or file URL and, when the system
is online, it must actually respond — and the text turns **red** while the
check fails (advisory: nothing blocks or reverts, and offline the reachability
check is skipped so you can prepare a URL that will work later). On OK a valid
custom URL persists in the application settings (key `Marketplace/registryUrl`;
default mode stores "no override") and the marketplace fetches from it the
next time it opens. The standalone `pj_marketplace_app` harness always uses
its built-in default.

### 5.3 Supported Platforms

| Platform | Architecture | Status |
|----------|--------------|--------|
| Linux | x86_64 | Full support |
| Linux | arm64 | Planned |
| Windows | x86_64 | Full support |
| macOS | arm64 (Apple Silicon) | Full support |
| macOS | x86_64 (Intel) | Planned |

### 5.4 Extension Categories

| Category | Code | Examples |
|----------|------|----------|
| Data Loader | `data_loader` | CSV, MCAP, ROS bags |
| Data Streamer | `data_streamer` | ROS 2, MQTT, ZMQ |
| Parser | `parser` | Protobuf, FlatBuffers |
| Toolbox | `toolbox` | FFT, CSV exporter |
| Bundle | `bundle` | ROS 2 Complete |

---

## 6. For LLMs/AI Assistants

### 6.1 Project Context

This is the **PlotJuggler Marketplace**, an extension distribution system for PlotJuggler (a robotics data visualization tool). Key points:

- **Stack:** C++20, Qt 6.11 Widgets, CMake, Conan
- **Architecture:** Serverless (GitHub-hosted registry and artifacts)
- **Key innovation:** Plugins don't depend on Qt (ABI stability)

### 6.2 Key Files

| File | Purpose |
|------|---------|
| `REQUIREMENTS.md` | What the system should do |
| `ARCHITECTURE.md` | How the system is designed |
| `USER_MANUAL.md` | This file - how to use it |
| `TODO.md` | Remaining work and deferred follow-ups |

### 6.3 Common Tasks

**"Add a new feature"**
1. Check if it's in REQUIREMENTS.md
2. Design in ARCHITECTURE.md
3. Implement following the code structure
4. Update USER_MANUAL.md if user-facing

**"Fix a bug"**
1. Reproduce the issue
2. Find relevant component in ARCHITECTURE.md
3. Fix and test
4. Update docs if behavior changed

**"Help user install extension"**
1. Guide through Section 2.4 of this manual
2. Check troubleshooting if issues arise

### 6.4 Code Locations

| Component | Path |
|-----------|------|
| Registry fetching | `src/core/RegistryManager.cpp` |
| Installation logic | `src/core/ExtensionManager.cpp` |
| Download handling | `src/core/DownloadManager.cpp` |
| Diagnostic-sink bridge | `src/core/QtDiagnosticBridge.cpp` |
| Main UI | `src/ui/marketplace_window.{cpp,ui}` |
| Extension detail dialog | `src/ui/extension_detail_dialog.{cpp,ui}` |
| Public headers (incl. data models) | `include/pj_marketplace/` |

### 6.5 Testing

`pj_marketplace` builds as part of the PJ4 monorepo (root `CMakeLists.txt` `add_subdirectory(pj_marketplace)`); the repo defines no CMakePresets.json.

```bash
# Build (output dir build/, RelWithDebInfo)
./build.sh

# Run tests
ctest --test-dir build/pj_marketplace

# Run standalone (target pj_marketplace_app — no OUTPUT_NAME override)
./build/pj_marketplace/pj_marketplace_app
```

---

## Document Maintenance

Update this manual when:
- User-facing features change
- New troubleshooting items discovered
- Developer workflow changes
- New platforms supported
