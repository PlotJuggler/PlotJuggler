# PlotJuggler Marketplace — User Manual

> **Audience:** end users, plugin developers, and LLMs assisting with the project. The marketplace is part of the
> desktop application; the browser build does not include it.

---

## Quick Start

### For End Users

1. Open PlotJuggler
2. Open the **File** menu and choose **Extensions Marketplace…** (keyboard shortcut Ctrl+Shift+M)
3. Search for the extension you need (e.g., "ROS 2") and select its row
4. Click **Install** in the **Details** footer
5. Close the marketplace: a freshly installed extension is loaded without a restart. Updates, removals and downgrades
   are applied the next time PlotJuggler starts

### For Plugin Developers

1. Write the plugin against the SDK ([`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk): the guides
   under `pj_plugins/docs/`)
2. Package the built library as a ZIP and try it with **Install local...** (see "Installing from a Local ZIP")
3. Publish a release per platform and add it to the registry (see "Registry URL")

---

## User Guide

### Opening the Marketplace

- Open the **File** menu and choose **Extensions Marketplace…**, or press Ctrl+Shift+M. The **Help ▸ Installed
  Extensions** submenu lists the plugins already loaded (informational only; managing extensions happens in the
  Marketplace).
- For development, the `pj_marketplace_app` target is a standalone harness around the same window.

### Browsing Extensions

The marketplace window shows a table of extensions:

| Column | Content |
|--------|---------|
| **Name** | Extension name; hover for its description |
| **Category** | The registry category (`data_loader`, `data_stream`, `message_parser`, `toolbox`, ...) |
| **Installed** | The installed version, or "—" when it is not installed. A locally newer build shows its own version |
| **Marketplace** | The registry version. Highlighted when it is an update, and in amber when the release cannot run on this PlotJuggler (hover for the reason) |
| **Description** | Short description |

Click a column header to sort by Name, Category, Installed or Marketplace; the default order groups by category.

**To see extension details:** select a row. The **Details** footer under the table shows its publisher, license,
description and changelog, and carries every action: Install, Update, Uninstall (or Downgrade to bundled), the enable
toggle and a **Visit Website** button.

### Searching and Filtering

**Search box:** type to filter by name, description, or tags
- Example: `ros` finds "ROS 2 Streaming", "ROS Bag Loader"
- Example: `csv` finds "CSV Loader", "CSV Exporter"

**Filter row:** four toggle buttons beside the search box, all on by default. Any combination can be active:

- **Data Sources** — extensions that load data from files
- **Data Streamers** — extensions that stream data continuously
- **Parsers** — extensions that decode byte blobs into fields
- **Toolboxes** — extensions that add a tool with its own GUI

A category is listed only while its toggle is on. An extension whose category is none of these four is always listed.
The search box narrows the result further.

### Installing an Extension

1. Find the extension in the list and select its row
2. Click **Install** in the Details footer
3. Wait for download, verification and extraction; the status line shows the progress and ends with "Installed …"
4. The footer shows an **Installed** badge; close the marketplace and the plugin is available

If the extension needs a newer plugin SDK than this PlotJuggler ships, **Install** is disabled and the footer explains
why (hover over the button too). Installs run one at a time: further clicks wait in a queue.

### Updating an Extension

1. An extension with a newer compatible version shows its Marketplace cell highlighted and an **Update** button in the
   footer
2. Select it and click **Update**
3. The new version is staged and the footer shows **Needs Restart**: the update is applied on the next launch, so the
   currently loaded plugin keeps working for the rest of the session
4. Once every queued change has been staged, a **Restart required** dialog says how many are waiting (one dialog per
   batch; staged uninstalls and downgrades count too)
5. On restart, the old version is moved to `.backup/` before the new one is promoted
6. If promotion fails, the previous version is put back; if that also fails, the diagnostics name where it is in
   `.backup/` so it can be recovered by hand

**Update All:** click "Update All" in the toolbar to stage every available update that is compatible and not already
staged.

### Uninstalling an Extension

1. Select the installed extension
2. Click **Uninstall** in the Details footer (there is no confirmation dialog)
3. The removal is **staged**: the row shows **Needs Restart**, and the files are deleted the next time PlotJuggler
   starts

An extension the session already loaded keeps running until that restart — its code cannot be unmapped from a live
process — so the removal is reported as pending rather than done. The same id cannot be reinstalled until the restart
has applied the removal.

If the deletion cannot go through at that next start (a file locked by another process, antivirus, missing
permissions), the extension is reported in the diagnostics and retried at the following start. It stays unloaded in the
meantime, so a removal that is stuck on disk never brings the plugin back.

An uninstalled id stays on the disabled list even after its files are gone, so that nothing but an explicit install can
make it load again. Installing the extension a second time clears that automatically. The one case where it shows:
copying an extension folder back by hand does **not** re-enable it, so use the enable toggle after doing so.

Core (bundled) extensions have no usable Uninstall action — see "Core (Bundled) Extensions".

### Installing from a Local ZIP

**Install local...** in the toolbar installs a plugin from a ZIP on disk, which is how to try a build before it is
published. The archive must hold exactly one top-level directory with the plugin library (and any `.ui` file) in it. It
is checked like a registry download except that its origin cannot be verified: it must contain a loadable plugin whose
embedded manifest passes the compatibility gate.

- A new id is installed at once, like a registry install.
- An id that is already installed asks whether to **Replace** it; a confirmed replacement is applied at the next start.
- An archive identical to the installed files changes nothing: the status line says so and no restart is needed.

### Enabling/Disabling Extensions

Select an installed extension and use the enable toggle in the **Details** footer. The choice is saved at once and
takes effect at the next start: a disabled extension stays installed and is not loaded.

### Core (Bundled) Extensions

Extensions shipped with the application (inside the installer or AppImage) are **core** extensions. At every startup
they are seeded into the extensions directory, so they are managed like any other install — with three differences:

- **They cannot be uninstalled.** The id ships with the app; removing it would just be undone on the next launch. The
  **Uninstall** button is disabled.
- **They follow application upgrades.** When a new PlotJuggler version bundles a newer plugin, your installed copy is
  refreshed to it automatically. A copy you updated *above* the bundled version through the marketplace is left alone.
- **Downgrade to bundled.** If you updated a core extension and want the shipped version back, use **Downgrade to
  bundled** (staged, applied on restart).

To pin a core plugin to an older build, place that build in a custom plugin folder (Preferences → Plugins): custom
folders override the extensions dir regardless of version. The same page lists every scanned plugin folder.

---

## Developer Guide

> **Note on plugin shape:**
>
> Data plugins (sources, parsers, toolboxes) are C++ DSOs that consume the C ABI declared in
> `plotjuggler_sdk/pj_plugins`. They do **not** link Qt — only the host does. Widget plugins are the exception: they
> create Qt widgets and link Qt. Each plugin exposes a small embedded JSON manifest (`id`, `name`, `version`, …)
> through the SDK export macro; the marketplace reads that manifest to recognise installed plugins.
>
> **CI Options:** Extensions can live in separate repos (one per extension) or in a mono-repo with per-component
> releases (see [Foxglove MCAP](https://github.com/foxglove/mcap) as reference). The registry supports both approaches.

### Creating a New Extension

1. **Write the plugin.** Start from the SDK guide of its family (`data-source-guide.md`, `message-parser-guide.md`,
   `toolbox-guide.md`, `dialog-plugin-guide.md` under `pj_plugins/docs/` in the
   [`plotjuggler_sdk`](https://github.com/PlotJuggler/plotjuggler_sdk) repository). The plugins in
   [`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) are working examples, with their build
   and release setup.
2. **Try it locally.** Either start PlotJuggler with `--plugin-dir <folder holding the built library>`, or zip the
   library (plus any `.ui` file) and use **Install local...** (see "Installing from a Local ZIP").
3. **Publish.** Build one ZIP per supported platform (see "Supported Platforms"), publish them where they can be downloaded by direct URL,
   and add the release to the registry (see "Registry URL"; the entry format is in the spec, "Registry — Format and Hosting").

### Embedded Plugin Manifest

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
| `description`, `category` | No | Shown in the marketplace for a plugin the registry does not list |
| `min_sdk_required` | No | The oldest plugin SDK the plugin needs; a host with an older SDK refuses to install it |
| `suggested_sdk_version` | No | The SDK release at which every capability the plugin uses is available; an older host shows a reduced-features note |
| `min_plotjuggler_version` | No | Deprecated: shown, never gates |
| `encoding` | Parsers only | Message encoding handled by a parser plugin |
| `file_extensions` | No | File suffixes handled by a file source plugin |
| `capabilities` | No | Optional capability tags |

### Plugin Types

| Registry category | Base class (export macro) | Purpose |
|-------------------|---------------------------|---------|
| `data_loader` | `PJ::DataSourcePluginBase` (`PJ_DATA_SOURCE_PLUGIN`) | Load data from files |
| `data_stream` | `PJ::DataSourcePluginBase` (`PJ_DATA_SOURCE_PLUGIN`) | Real-time data streaming |
| `message_parser` | `PJ::MessageParserPluginBase` (`PJ_MESSAGE_PARSER_PLUGIN`) | Parse binary data to fields |
| `toolbox` | `PJ::ToolboxPluginBase` (`PJ_TOOLBOX_PLUGIN`) | Custom tools with UI |

A single `DataSourcePluginBase` serves both file and streaming sources; whether it loads files or streams live data is
declared via `capabilities()`/manifest, not by a separate base class.

### Best Practices

- **No Qt dependency:** use the SDK, not Qt directly. Widget plugins are the exception: they link Qt and must be built
  against the Qt version PlotJuggler ships
- **Static linking:** Embed all dependencies
- **Test on all platforms:** Use CI matrix build
- **Semantic versioning:** Follow semver strictly; the plugin version and its floors must be strict SemVer
- **Clear README:** Explain what your plugin does
- **License:** Include LICENSE file (Apache-2.0 recommended)

---

## Troubleshooting

### Diagnostics dialog

A **Details** button appears next to the status line whenever diagnostics were recorded since the marketplace was
opened, with a count when there are several. Click it to open a read-only log of marketplace lifecycle events
(install/uninstall/staged-promotion failures, quarantine moves, registry-fetch failures). The most recent error also
appears in the status line; a successful registry refresh clears the sticky error so progress messages are not
suppressed.

When the marketplace runs inside a host application, the host receives the same diagnostics alongside its own
plugin-load events (see ARCHITECTURE.md, "Diagnostics").

### Common Issues

| Problem | Cause | Solution |
|---------|-------|----------|
| "Extension not loading" | Incompatible version | Check the extension's `min_sdk_required` against the SDK version in the error |
| "Requires PlotJuggler SDK X or newer" | The package needs a newer plugin SDK than this PlotJuggler build ships. Raised for a local ZIP as well, from the manifest embedded in its plugin | Update PlotJuggler, or obtain a build of the extension made for this version |
| "Plugin ABI N does not match host ABI M" | The plugin was built for another plugin ABI generation | Obtain a build made for this PlotJuggler |
| "Installation failed: …" | Network issue, or the archive could not be read | Check internet, try again; the reason follows the colon |
| "Checksum mismatch" | Corrupted download | Try again, report if persistent |
| "Installed version is newer" | Local plugin is ahead of registry | Downgrade is blocked; keep the local version |
| "Update for … is already staged" | An update is waiting for the restart | Restart PlotJuggler |
| "Uninstall of … is staged; restart to apply it before installing again" | A removal of the same id is pending | Restart PlotJuggler, then install |
| "Previous version remains in backup: …" | The new artifact did not install or promote | Check the marketplace diagnostics for the retained backup path |
| "Post-promotion validation failed" | The DSO loads in the staging area but not from `extensions/` (rpath/dep issue) | The install is rolled back; check the diagnostic for the linker error |
| "Could not record the removal of …; uninstall not scheduled" | The removal record could not be written (permissions, disk) | Nothing is scheduled and the plugin keeps its enable state; resolve the problem and retry |
| "Moved to quarantine: …" | A previous staged update could not be removed; it has been moved aside | Inspect the quarantined directory and delete it manually once safe |
| "Another PlotJuggler instance is managing extensions" | A second PlotJuggler is running over the same extensions | Close the other instance and restart PlotJuggler; meanwhile extensions are listed but cannot be changed |
| The registry URL field in Preferences turns red | The URL is not http(s) or file, or does not respond (advisory: nothing is blocked; a value that is not a valid URL is not saved) | Fix the URL or switch back to *default* (see "Registry URL") |

### Reset Marketplace

If the marketplace is broken, close PlotJuggler and remove the extensions and staging directories under your platform's
config root (see "Directory Structure"):

```bash
# Linux
rm -rf ~/.local/share/PlotJuggler/PlotJuggler4/extensions/
rm -rf ~/.local/share/PlotJuggler/PlotJuggler4/.extension_staging/

# macOS
rm -rf ~/Library/Application\ Support/PlotJuggler/PlotJuggler4/extensions/
rm -rf ~/Library/Application\ Support/PlotJuggler/PlotJuggler4/.extension_staging/

# Windows
rmdir /s %LOCALAPPDATA%\PlotJuggler\PlotJuggler4\extensions
rmdir /s %LOCALAPPDATA%\PlotJuggler\PlotJuggler4\.extension_staging
```

The next start re-seeds the core extensions. The other `extensions.*` directories beside `extensions/` (see "Directory Structure") hold the
removal journal and the content-addressed copies; they are rebuilt or cleaned up from `extensions/` and need not be
removed by hand.

### Reporting Bugs

1. Check existing issues at https://github.com/PlotJuggler/PlotJuggler/issues (**Help ▸ Report an Issue** opens it)
2. Include:
   - PlotJuggler version
   - OS and version
   - Extension name and version
   - Error message or the marketplace diagnostics
   - Steps to reproduce

---

## Reference

### Directory Structure

The marketplace uses the application's writable data location (resolved by `QStandardPaths::AppDataLocation`, i.e. the
`PlotJuggler/PlotJuggler4` org/app pair):

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
├── extensions.install_stage/        # Transient: downloads are unpacked here before they are promoted
├── extensions.state/                # Pending removals, one record per extension
├── extensions.artifacts/            # Content-addressed copies of the installed extensions
├── .extension_staging/              # Staged updates, promoted on the next startup
│   └── my-extension/
│       └── .pj_pending_install      # Intent file for the restart-time apply
└── .backup/                         # The previous version of an updated extension
```

(`extensions.seed_stage/`, `extensions.extraction/`, `extensions.profiles/` and `.extensions.lock` also live there;
they are bookkeeping.)

### Registry URL

**Default:** `https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json`

**Custom registry:** open **Preferences → Plugins**. The *Marketplace registry URL* row has a **default / custom**
switch: *default* shows the built-in URL read-only; *custom* enables the field for editing. When you finish editing, the
value is checked — it must be an http(s) or file URL and, when the system is online, it must actually respond — and the
text turns **red** while the check fails (advisory: nothing blocks or reverts, and offline the reachability check is
skipped so you can prepare a URL that will work later). On OK a valid custom URL persists in the application settings
(key `Marketplace/registryUrl`; default mode stores "no override") and an open marketplace re-fetches from it at once.
The standalone `pj_marketplace_app` harness always uses its built-in default.

### Supported Platforms

Registry artifacts are keyed by `<os>-<arch>` (`linux-x86_64`, `windows-x86_64`, `macos-arm64`). The marketplace offers
the artifact of the platform it runs on.

### Extension Categories

| Category | Code | Examples |
|----------|------|----------|
| Data Loader | `data_loader` | CSV, MCAP, ROS bags |
| Data Streamer | `data_stream` | ROS 2, MQTT, ZMQ |
| Parser | `message_parser` | Protobuf, FlatBuffers |
| Toolbox | `toolbox` | FFT, CSV exporter |
