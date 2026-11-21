# Plugins and the Extensions Marketplace

Every loader, streamer, parser, toolbox and Custom Widget is an extension. Which one a format needs:
[formats.md](../formats.md). The Marketplace is desktop only; the browser app has a fixed built-in set.

## See what is installed

- **UI:** Help → Installed Extensions (a list of "name (version)"), or the Marketplace's "Installed" column.
- **MCP:** `about` (desktop) lists the plugins loaded, installed and installable; `get_status` lists the stream
  sources by name.

## Install, update, enable, disable, uninstall

- **Open:** File → Extensions Marketplace… (Ctrl+Shift+M), or the title bar's "UPDATE" button
  (`buttonExtensionUpdate`), which appears when installed extensions have updates.
- **Find:** the filter pills "Data Sources", "Data Streamers", "Parsers", "Toolboxes" (additive, all on) and the
  search box "Search extensions..." (name, description, tags). Custom Widget plugins are always listed.
- **Act:** select a row; the footer below the table shows its details and one action button: "Install", "Update",
  or a disabled badge ("Installed", "Needs Restart", "Local newer", "Installing"). Then "Uninstall" (bundled plugins
  cannot be uninstalled; an updated bundled one offers "Downgrade to bundled v..."). The unlabelled switch in the
  footer (`detail_enable_toggle_`) enables or disables an installed extension.
- **Update everything:** "Update All" (`update_all_btn_`).
- **From a ZIP:** "Install local..." (`install_local_btn_`) → "Install plugin from local ZIP".
- **Restart:** updates, uninstalls, downgrades, enabling and disabling take effect at the next launch ("Restart
  required"). A fresh install appears when the Marketplace panel is closed.
- **MCP:** UI only. An agent can say which plugin to install (from `about` and [formats.md](../formats.md)) and wait
  for the user.

## Plugin folders and the registry

- **UI:** File → Preferences... → Plugins (or the gear in the Marketplace): "Local folders with custom plugins:"
  (add, remove, reorder; applies at the next launch), "List of default folders (in order of precedence):", and
  "Marketplace registry URL:" "default" / "custom".
- **Command line:** `--plugin-dir <path>` overrides the extensions folder for one run.
- **MCP:** UI only.

## Open a toolbox

- **UI:** the Toolbox menu lists every installed toolbox by name: "Marker Editor", "Fast Fourier Transform",
  "CSV/Parquet Exporter", "Quaternion to RPY", "Assistant Agent". The Transform Editor opens from Custom Topics →
  "+"; cloud toolboxes (Mosaico) from Sources → Cloud. "(no toolboxes available)" means none is installed.
- A toolbox opens over the plot area with a banner: "Move to a tab" (`buttonMigrateTab`), "Move to a floating window"
  (`buttonMigrateFloat`), "Close" (`buttonClose`). One instance per toolbox.
- **MCP:** UI only.

## Assistant Agent (in-app chat)

- **UI:** Toolbox → "Assistant Agent". It drives the user's installed and signed-in `claude` or `codex` CLI
  ("Settings..." → "Backend": "Claude Code (subscription)" or "Codex (ChatGPT subscription)"), through PlotJuggler's
  own MCP tools. Conversations are listed on the left ("New chat", rename, trash). Type in the input row, "Send";
  "Cancel" stops a turn.
- Desktop only; Linux AppImage bundles it, elsewhere it comes from the Marketplace.
