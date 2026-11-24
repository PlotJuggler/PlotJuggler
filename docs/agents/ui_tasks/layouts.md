# Layouts, files and undo

Labels as the app shows them; `objectName`s in backticks. The menu bar (File, Toolbox, Help) sits in the window's title
bar.

## Open data files

- **UI:** File → Open Data File... (Ctrl+O), or Sources panel → File page → "Load a data file" (`buttonLoadDatafile`),
  or drop files from the file manager onto the window (desktop). Several files can be selected at once.
- **Several files for one loader with a dialog:** a "Load Data" dialog asks once, "Configuration dialogs:"
  (`dialogMode`): "Show all" (every file shows its loader's dialog), "Show first" (the first file of each format
  shows it, the others reuse its settings) or "Skip" (every file uses its format's last settings).
- **Command line:** `plotjuggler4 -d a.mcap -d b.mcap -l layout.pj4.xml` or `plotjuggler4 recording.csv`; explicit
  files replace the sources saved in the layout.
- **MCP:** `open_file` (`path`; `ask_user: true` to show the loader's dialog; `config` for loader settings). Each
  call is one file; call it once per file.
- **Gotchas:** the loader is chosen by extension ([formats.md](../formats.md)). Each file becomes its own dataset.

## Reload, replace, remove, merge, inspect a dataset

Right-click a dataset's top row in the Datasets panel:

| Item | Does | MCP |
|---|---|---|
| "Info" (one dataset) | Dataset Info dialog: Name, File, Provenance, Loader metadata | `get_status` shows path, loader and config |
| "Merge" (two or more) | combines them into one dataset, after a "Merge datasets" confirmation; refused for a live stream | UI only |
| "Reload" (one file-backed dataset) | re-reads the same file, keeps the workspace bound | `reload_file` |
| "Replace" (one file-backed dataset) | "Replace Data" file dialog; loads another file into the same dataset | UI only |
| "Refresh derived" | re-runs the dataset's marker rules (needed when Derived markers is Manual) | UI only |
| "Remove" | asks "Are you sure you want to remove '...' and its data?", then about dependent derived series | `unload_file` |

The Datasets options button (`buttonDatasetsMenu`) holds "Show Values" (a Value column with the value at the
cursor), "Preserve Topic Name" and "Remove all Datasets", which asks nothing and clears everything, streams included.

## Save a layout

- **UI (desktop):** File → Save Layout.... The save dialog has a checkbox "Bind to this data source" (checked by
  default). Checked: the layout reopens these exact files and restarts the running streams without their dialogs;
  with a stream running it reads "Bind to this data source (stores the stream settings, credentials included)".
  Unchecked: a generic layout whose curves bind by topic and field to whatever is loaded.
- **UI (browser):** File → Save Layout... downloads a generic `plotjuggler-layout.pj4.xml`; File → Save Layout with
  Sources... downloads a source-bound one.
- **Extension:** `.pj4.xml`, added when missing.
- **MCP:** `save_layout` returns the layout's XML exactly as this menu writes it; with `path` it also writes the file
  (desktop). `bind_sources` is the checkbox (default: checked on the desktop, generic in the browser). Agent tabs and
  agent-created series, objects and markers are saved with the user's layout.
- **Gotchas:** a source-bound layout with a stream stores that plugin's configuration as is, credentials included.
  The streaming buffer is saved only in a stream-bound layout.

## Load a layout

- **UI:** File → Load Layout... (filter "PlotJuggler 4 Layout (*.pj4.xml)"), or Sources panel → "Recent layouts &
  files" (`buttonRecentFiles`) → a layout. Command line: `-l` / `--layout <path>`.
- **Layout for one dataset, several loaded datasets fit:** a "Load Layout" dialog asks "Several loaded datasets fit
  the layout ... Which ones should it show?" with one checkbox per dataset (`listDatasets`). One checked: the layout
  is pinned to it. Several checked: each tab is duplicated, one copy per dataset named `<tab> [<dataset>]`, each curve
  in the same colour in every copy, the checked datasets aligned to start together. A layout with a transform or an
  object script shows "This layout shows one dataset at a time: ... cannot be copied for a second dataset." and
  allows one check.
- **Missing data:** curves the loaded data lacks prompt "Missing curves" / "Missing layout data" with "Remove from
  plots", "Continue without them" or "Cancel".
- **MCP:** `load_layout` with `xml` (the text), `path` (desktop) or `url` (http/https). It loads as `--layout` does:
  the layout's data files and streams load without the reload question, missing curves are kept, several fitting
  datasets give the first one, and the call returns these warnings instead of dialogs. When the workspace holds
  anything the user is asked first ("Load Layout": "Replace" / "Cancel"). An agent's tabs and series come back as
  that agent's.
- **Gotchas:** a layout dropped onto the window is treated as a data file and fails: use Load Layout. A layout naming
  several datasets is never asked about; its curves bind only to the files it names.

## Compare two runs with one layout

1. Open both files (one dataset each).
2. File → Load Layout... → the layout made for one run.
3. In the "Load Layout" dialog keep both datasets checked → OK.

Each tab now appears twice, `<tab> [run_a]` and `<tab> [run_b]`, aligned to start at the same display time. Running
the same command line with `-d run_a.mcap -d run_b.mcap -l layout.pj4.xml` asks the same question as a file list.
Comparing on one plot instead: [time.md](./time.md), "Compare two runs".

## Undo and redo

- **Keys only:** Ctrl+Z undoes; Ctrl+Shift+Z or Ctrl+Y redoes. No menu item. A focused text field keeps its own undo.
- **Covered:** tabs, splits, plots and their curves and styles, scene, Custom Widget, State Transitions and Log
  panels, filters and custom topics, and the Source Timeline offsets and view.
- **Not covered:** loading, reloading or removing data (removing a dataset resets the history), the time cursor and
  playback, Preferences, recording, and everything an agent or a plugin created.
- **MCP:** none; an agent's own creations are removed with `remove_derived_series`, `remove_script_series`,
  `remove_markers` and `plot_tab` / `scene_view` `close`.

## Preferences

File → Preferences... pages: Appearance, Plotting, Scene 2D, Scene 3D, Plugins (desktop), Recording (desktop).
Settings apply on OK.

| Page | Setting | `objectName` |
|---|---|---|
| Appearance | "Theme:" (light/dark) | `themeToggle` |
| Appearance | "Float precision:" | `scrubberFloatPrecision` |
| Appearance | "GPU canvas:" (desktop) | `openglToggle` |
| Appearance | "Check for updates on startup:" (desktop) | `checkUpdatesToggle` |
| Appearance | "Let AI agents control PlotJuggler (MCP server):" (desktop) | `mcpServerToggle` |
| Plotting | "Auto-zoom plots:" | `autoZoomToggle` |
| Plotting | "Curves color sequence:" global / per plot | `curveColorMode` |
| Plotting | "Derived markers:" Automatic / Manual, and "Reload rate:" | `derivedBatchMode`, `scrubberDerivedReloadHz` |
| Plotting | "Clock time zone:" local / UTC | `clockTimeZone` |
| Plugins | "Local folders with custom plugins:", "Marketplace registry URL:" | `listCustomPluginFolders`, `registryUrlMode` |
| Recording | "Recordings folder:", "Write queue per source (MiB):" | `lineEditRecordingDir`, `scrubberRecordingQueueMiB` |
| Recording | "Source cache folder:", "Source cache budget (GB):", "Capture downloads for offline restore:" | `lineEditSourceCacheDir`, `scrubberSourceCacheBudgetGB`, `sourceCacheCaptureToggle` |

An agent that cannot connect: the MCP switch is off. Ask the user to turn it on (Appearance page), or start
PlotJuggler with `--mcp` for one run. The status-bar button "AI agent: <client>" lists what agents run, each with a
Stop button.

## Diagnostics

The title bar's bell (`buttonNotifications`, tooltip "Diagnostics", red with "New errors") opens the Diagnostics
dialog: load failures, plugin errors, layout warnings, filtered by "Info" / "Warning" / "Error", by source and by text.
MCP: UI only; a failed `open_file` returns its reason directly.
