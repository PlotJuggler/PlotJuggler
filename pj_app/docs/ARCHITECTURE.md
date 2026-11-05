# pj_app Architecture

## Playback and panel access

The bottom `TimelineWidget` seeks directly on a slider-track click and keeps the
normal handle-drag interaction. Space toggles Play/Pause for its window, without
auto-repeat; text editors retain Space for typing through Qt's shortcut-override
handling. Disabled transport controls remain inactive.

An object dock's context menu offers **Open Settings** when the right sidebar
is hidden and **Close Settings** when it is visible. The host-supplied visibility
query runs when the menu opens. `settingsPanelToggleRequested` passes through
`PlotDocker` to `MainWindow`, which binds the settings page to that dock and toggles
the right panel through its existing visibility button.

How the executable shell is put together. `../CLAUDE.md` holds the rules and
pitfalls; this document is the subsystem reference. The wiring itself is best
read from `src/MainWindow.cpp` and `src/main.cpp`; the headers own per-class
contracts (threading, ownership, call order), so this document describes only
what spans several classes.

Derived data (on-demand nodes, series, marker rules, filters and transforms) is
designed in [`../../docs/derived_objects/ARCHITECTURE.md`](../../docs/derived_objects/ARCHITECTURE.md);
the sections below cover only the shell's half of it.

## Window frame

The main window wears the **system** title bar by default, and `TitleBar` is just
the toolbar row under it. `Preferences::custom_title_bar` (read once at
construction, so it applies on the next launch) opts in to a frameless window
whose `TitleBar` *is* the frame: the only mode that runs the edge-resize filter and
paints the 1-px border.

The platform rule is `MainWindow::usesCustomFrame()`: WebAssembly is always custom
(a browser tab has no window manager), macOS is always system, Linux and Windows
follow the preference. The Preferences toggle is hidden where the choice is not
offered (`MainWindow::customFrameSelectable()`). `PJ::Dialog` and `MessageBox` keep
their own chrome either way.

`Theme` requests the matching Qt color scheme on every switch, so a platform-drawn
title bar follows the app's light/dark on Windows and macOS; a Linux window manager
keeps its own.

## Command-line startup

Desktop startup accepts repeatable `-d` / `--datafile <path>` and positional
files, plus `-l` / `--layout <path>`. For example:
`plotjuggler4 -d first.csv -d second.csv -l shared.pj4.xml`.
`MainWindow::loadAtStartup` waits for the terminal ticket of every explicit
file before applying the layout. Explicit files override a source-bound
layout's saved files: the existing generic-layout rewrite removes dataset
qualifiers so curves bind to the new data. Without explicit files, `--layout`
replays its saved sources as usual. Data-only launches keep the normal GUI open.

Attended loads may show loader configuration dialogs; unattended startup
(`--exit-after-layout`, `--screenshot`, offscreen/minimal) uses `DialogPolicy::kNever`.
A failed or cancelled explicit file skips the layout and settles it false, so
`--exit-after-layout` exits unsuccessfully. That flag still requires a layout.
The batch observes its own tickets, including early failures, and defers the
final restore through a window-owned timer to avoid restoring during teardown.
Tests: `MainWindowStartupTest` (real mock file-source DSO).

## File loading

`FileLoader` runs file loads (one queue, sequential, progressive on a worker
thread); `LoadInput` carries the display name, backing path, reload identity and
(in the browser) the storage lease that keeps an upload alive for deferred
object fetches. `FanoutConfig` extracts a plugin config's `__pj_fanout` entries
(one plugin instance and one `DatasetId` per entry). `FileLoader` has no
`MainWindow` dependency (`setFilePicker` is the seam), which is why the loader
test suites compile it standalone.

### Load diagnostics

`MainWindow` wires `FileLoader::setDiagnosticSink` to the bridge sink.

- `data-source-load-failed` (error, source "File Loader") is emitted once per
  failed load and once per failed fan-out entry, none for the all-failed
  aggregate, and never for a reject, cancel, discard or shutdown. The browser
  picker's staging failure is reported too. The same cause may appear under both
  this id and the plugin's own.
- `data-source-message` carries the loading plugin's warnings and errors (source =
  plugin display name, message prefixed with the file or entry name), capped at 20
  per load (`DataSourceRuntimeHost::kMaxForwardedMessages`); the next message yields
  one `data-source-message-limit` warning and the rest stay in the log.
- A missing layout data file is reported before `FileLoader` runs, as
  `Layout`/`data-source-missing`.
- Streaming sources are not wired to the sink.

### Per-dataset load progress (curve tree)

`wireIngestProgress()` (`IngestProgressWiring.{h,cpp}`) is the single definition of
the connections between the three pieces below: controller attach, the
dataset-path resolver, `progressUpdated`/`rowKeyChanged` into the panel, and the
cancel route back with its token-resolution, staleness and cancellability checks.
`MainWindow` is its caller, so tests and tooling that wire through it exercise the
app's real connections instead of a copy.

`IngestProgressController` wires `SessionManager`'s dataset-scoped ingest signals
(`ingestBegan` / `ingestProgressed` / `ingestEnded`, plus `ingestStopping`) to
`CurveTreeView`'s plain-typed progress decoration (`setDatasetProgress`). The
view-facing row key IS the ingest id (`IngestToken::id`), so a click on a row
resolves back to its ingest by lookup. The controller owns the row label (the
dataset's name once resolvable and then kept, so a discard that deletes the
dataset cannot rename the row mid-flight; the producer's progress title is only
the pre-resolution fallback), the 1-second completion linger, the 3x ~180 ms error
flash, immediate retirement on cancellation (the user stopped it, and a discard
has deleted the dataset the row described), ghost-row lifetime (progress-only rows
for failed or discarded loads whose catalog entries vanished) and an injectable
timer factory for headless tests (details in `IngestProgressController.h`). Both
`FileLoader` and `HeadlessDescriptorProviderSession` produce ingest tokens on the
same channel.

**Keep or discard.** The row itself carries the choice (the X keeps what arrived,
the bin discards it), so the shell acts on the `keep_partial` flag directly and
does **not** prompt: `requestCancel(token, keep_partial)` and nothing else.
Honouring the flag is the **producer's** job:

- `FileLoader` latches it into `cancel_mode_` (1 = keep, 2 = discard) for a
  whole-load stop, or into a slot owned by that fan-out entry's own loop iteration
  for a row stop, so a row can only ever stop the entry the user aimed at.
- Keep commits this load's partial data (on a reload, the partial refill replaces
  the prior data).
- Discard means *as if this load had never run*, so what it leaves behind is
  whatever preceded it: a first-load discard real-deletes the dataset it created;
  a reload-discard rolls back to the pre-reload data (refill-guard rollback plus
  `TransformService::abortReplacingLoad`, which with the ingest tap active is a
  staging-pointer reset and reads nothing back from the store).
- Toolbox imports forward the plugin's own `cancellable` declaration and register
  as `IngestStop::kStopOnly` (their ABI has no rollback), so their rows offer a
  live X with the bin drawn greyed and inert.
- `HeadlessDescriptorProviderSession` registers `cancellable=false` on purpose (no
  UI to stop from).

The three-button keep/discard/resume prompt exists only on the title-bar strip
path, whose single stop button cannot express the choice. The strip
(`IngestProgressWidget`) is wired but kept hidden by a local
`constexpr kShowTitleBarIngestStrip = false` in `MainWindow`'s constructor.
Behind it, while `FileLoader` is busy its Stop raises that prompt and calls
`FileLoader::cancelCurrent`; otherwise Stop routes by the strip's displayed owner: a
layout-import batch job takes the keep-partial `cancelActiveImportJob()` (that job
ends cancelled and the batch continues with its next source; never
`batch->cancel()`, never a faked `ToolboxIngestRef` for a headless job), and
interactive toolbox imports take the flag-only `stopAllToolboxImports()` with no
dialog.

## Streaming sources

`StreamingSourceManager` owns the live streaming sessions (one `DatasetId` each,
a worker thread per session writing through the runtime host) and the secondary
tail buffers used while paused. `TopicDemandController` is thin glue from
displayed widgets to `TopicDemandTracker`; `PendingDisplayBinder` stages a curve
or scene layer until its topic materializes. On the first real demand for a
previewed topic, `TopicDemandController` invalidates the matching XY and
time-series caches of registered plots synchronously before evicting the preview
history: bounds calculation inside `curveListChanged` may already have filled
them, and no paint or event-loop turn may run between invalidation and eviction.

The topic streaming context menu is available for a single selected row of a
per-topic-pause-capable source. A field row resolves to its owning topic, so
"Force topic streaming" always affects the whole topic. Multiple selected rows
(across Datasets and Custom Topics) suppress this menu: Linux can request it on
right-button press, before a right-drag has started. The selected pair remains
available for the existing XY right-drag; Ctrl+click keeps its normal selection
meaning. Dataset-level menus still support multiple selected datasets.
`CurveListPanelTest` covers the real press/context path, XY payload, single-topic
toggle and dataset menu.

`StreamingSourceManager::onIngestTick` is the GUI half of one 50 ms poll cycle
(`ingestTickForTests` drives it): eager processors advance over every topic of the
dataset (skipped while paused), then retention trims the engine being written,
then the marker recompute and the ingest announcement run, also over every topic.
Order matters: a stateful processor must consume every sample before retention
evicts it. See `../../docs/derived_objects/ARCHITECTURE.md` section 16.

## Session recording

Recording is a runtime capability, so nothing here is gated at build time. The
`RecordingService` member, its connections and the shutdown `stop()` calls are
unconditional (`~MainWindow` stops the recording before the widgets its `stopped()` slot touches are destroyed).
The streaming strip's `buttonStreamingRecord` and `labelStreamingRecordStatus`
stay hidden until `LeftPanel::setRecordingSupported(...)`, which only `MainWindow`
calls and only with `RecordingService::isSupported()`. Preferences adds its
Recording nav row on the same query and hides the page otherwise; a nav row's
position IS its page index.

- `StreamingSourceManager::recordingTargets()` is the eligibility seam: it hands
  back only sessions whose plugin advertises delegated ingest (a direct writer has
  no raw bytes at the seam) plus the display names it excluded, for the shell to
  say what will be missing.
- Each target is keyed by its `DatasetId`, the identity the service addresses that
  source by afterwards. Display names are display names: they may repeat, because
  every source writes its own file and `RecordingService` names those files.
- A target's `attach` reports `false` when the session ended before the tap could
  be installed, which fails (and rolls back) the whole capture. Taps are attached
  at start only, so a stream started later is never recorded.
- One Record press records every eligible source into one batch folder, one file
  each, so a stream that ends mid-capture must NOT stop the capture: the
  `streamStopped` handler forwards `RecordingService::onSourceEnded(dataset_id,
  reason)`, which finalizes that one file and leaves the rest recording. The batch
  ends with its last live source, or when the user presses Stop.
- `MainWindow::onRecordingStopped` reports a whole capture rather than a file: the
  path when a single source recorded, the folder when several did and every
  outcome is clean, and otherwise the problem count plus up to two failing sources
  with their reasons (naming `mcap recover` when a file could not be finalized).
  It appends the capture's dropped count when the write queues had to shed
  messages (and the count that could not be recorded), since a complete-looking
  file with holes says nothing about them itself. Per-source detail goes to the
  log, never into the toast: a toast listing one line per file stops being
  readable at three sources.

The recording format and the write-queue budget are in
`../../docs/session_recorder_design.md`.

## Layout files and derived recipes

`LayoutXml` is the self-contained DOM helper for `.pj4.xml` layouts (no
`MainWindow`, no `Qt::Widgets`; it is compiled straight into
`pj_app_unit_tests`). `LayoutReplayHints.h` builds the `LoadHints` shared by every
replay of one layout-recorded source (desktop reload, browser source-bound
replay, the import batch's cache-hit loads): automated, dialogs prohibited, the
saved preset authoritative, and its filepath (and its fan-out children's) pointed
at the path the replay actually loads.

Layout-relative paths: on a file save, `saveLayoutToPath` stores these paths
relative to the layout folder when the target is at or beneath it
(`relocatableSubpath`): the `<fileInfo>` filenames, every `*_dataset_path`
qualifier of a source-bound layout (all widget families, scene and State
Transitions rows included: `stampDatasetSourcePaths`, then
`relativizeDatasetSourcePaths`), and, in both bindings, a `<robot_model>`'s URDF
file and `file://` package roots (Scene3D's `relocateLayoutResources`).
`loadLayoutFromPath` resolves them against the layout folder before any restore
(`resolveDatasetSourcePaths`, Scene3D's `resolveLayoutResourceUrls`, which leaves a
browser's file label alone). Undo snapshots and in-memory captures stay absolute,
browser layouts never pass through either step, and a generic layout carries no
dataset qualifier.

`__pj_saved_filepath` is transient. It exists only in the config handed to a
plugin's `loadConfig` on a layout replay of a native file (the fresh load path of
`rewriteReplayFilepaths` with `SavedFilepath::kRecord`), and never reaches host
state: the rewrite drops any incoming value before writing its own (so a layout
that carries the key cannot pass it on), `captureSavedConfig` strips a plugin's
echo from what the session records, and a `prefer_reuse` hit, which runs no plugin,
emits its preset through the rewrite with `SavedFilepath::kOmit`.

The `<data_processors>` block (filters, transforms, marker generators, on-demand
recipes) is saved and restored by `pj_runtime`'s `DerivedObjects` (see
`../../pj_runtime/CLAUDE.md`). `MainWindow` saves through `derivedObjects().save(...)`
and keeps a thin wrapper for the restore: `restoreDataProcessors` =
`derivedObjects().parse(root, intent)` + `checkDataProcessorDependencies` +
`applyDataProcessors`, the snapshot carrying its intent. The preflight refuses a
preview holding an on-demand key the snapshot wants (under either intent) and, under
`kHistory`, a restore whose removals would strand an exempt dependent. The shell owns
what is UI: each diagnostic (the parse's and the restore's, in one report) is
reported by `reportDerived` under source "Layout" (so `layout_issue_capture_` sees
it), and the Custom Topics panel is re-derived after every restore.

`applyWorkspace` parses once and restores in a fixed order: preflight (read-only,
before anything mutates) -> per-source display offsets -> processors (filters,
transforms, marker generators) -> rebind every curve to a catalog key. Offsets move
before the processor replay so a generator that reads the display frame sees the
settled offset. The progressive drain's teardown applies an empty snapshot. An undo
leaves a marker rule it does not change as it is drawn and runs a changed one to
completion; a layout load creates it with the bounded create.

Frozen restore fixtures live in `tests/fixtures/layouts/` (each directory's
`README.md` says how its files were written): `release-4.1/` is read by
`MainWindowRelease41LayoutTest` and `prototype/` (on-demand recipes) by
`MainWindowPrototypeLayoutTest`. Both restore through the real load paths and save
back to the same `<data_processors>` block.

## Canonical layout import (cloud sessions)

A `.pj4.xml` layout can embed a durable source descriptor, so opening it
re-creates an exact cloud session: instantly from a local cache artifact when
present, otherwise by re-downloading through the MCAP Cloud connector plugin while
plots grow. The PJ4-side pieces are `LayoutImportBatch`,
`HeadlessDescriptorProviderSession`, `SourcePromotionHost`, the
rewrite-then-classify layout load path (`MainWindow` + `LayoutXml`) and the
growing-import binder (`MainWindow` over `SessionManager`'s ingest signals).
`LayoutExitWatch` turns the `--exit-after-layout` run into one exit code.
`ToolboxHostWiring.h` holds the wiring that must stay identical between
`MainWindow::launchToolbox` and `HeadlessDescriptorProviderSession`, so a provider
plugin sees the same host in both.

The feature spans three repos, so its as-built reference lives outside this one:
**`docs/layout-import-architecture.md`** in the `pj-mcap-server` repository
(cross-repo component map, runtime flows, and the invariant list I-1 to I-16).
**Read it before changing any of the components above**: several invariants are
host-side (delivery order, teardown orders, strict in-place promotion, strip
displayed-owner arbitration) and are not derivable from PJ4 alone. The operations
guide (what a shared layout embeds, the trust bootstrap, cache purge, the headless
flow and its diagnostic ids) is `docs/layout-sharing-runbook.md` in that same
repository.

## Plugin playback and owned views

`MainWindow` wires `PlaybackControlHost`, `ViewportRuntimeHost` and
`PlotTabsRuntimeHost` into each toolbox binding. Playback is global and stays in
`MainWindow`. The owned-tab operations behind the other two live in
`PluginPlotTabsController` (`src/PluginPlotTabsController.{h,cpp}`, over
`TabbedPlotWidget` + `CatalogModel` + `ExtensionCatalogService`), which builds each
plugin's `Callbacks` with the plugin id captured. `MainWindow` only constructs it
(handing it `makeSeededEmptyObjectDock` as the scene-widget factory and
`revealOwnedTabOverTakeover` as the created-tab hook, which folds the plugin's own
takeover panel so the new tab shows) and calls `refreshAvailability()` on catalog
change and layout load.

- **Scene tabs.** Scene tabs ("3d"/"2d") share the plugin's id namespace; the
  controller routes each call by the kind of the tab's live widget (`sceneKindOf`)
  and hands scene tabs to its delegate `PluginSceneViewsController`
  (`src/PluginSceneViewsController.{h,cpp}`: topic resolution, attach/detach, the
  read-back JSON). A kind the factory cannot build is refused by name before an
  existing tab is replaced.
- **Embedded views.** The same delegate serves the scene views a toolbox panel
  embeds (`scene_view` frames): `MainWindow::makeEmbeddedViewHost` is the panel's
  `EmbeddedViewHost`, building seeded scene docks parented to the frame and
  attaching through the delegate's `attachEmbeddedTopic`; the tracker broadcasts
  tick those views (`tickEmbeddedViews`) and a catalog change re-attaches what
  their frames still ask for (`reattachEmbeddedTopics`).
- **Addressing.** Viewport operations reach only the dockers whose
  `PlotDocker::ownerPlugin()` is the captured plugin id. Tab `stateId()` is
  independent of its title and position; duplicate visible titles and renaming do
  not affect addressing.
- **Persistence.** Owned tabs persist in full layouts with `owner_plugin`/`tab_id`
  metadata and remain live across history restores. So do their curves bound to a
  filter or transform output the snapshot leaves unchanged, because the
  derived-recipe restore reconciles both by persisted identity (filters by
  `output_name`, transforms by key) and keeps the unchanged ones live under their
  `TopicId`s. Everything else (changed entries, and kept ones cascaded out by a
  replaced input) is re-created in rounds (filters with their inputs re-resolved
  through a rebuilt catalog, then transforms) until a round creates nothing; apply
  failures are reported once, after the last round.
- **Watermark.** The watermark is the owning toolbox's badge (`toolboxBadge`: its
  manifest `badge`, else its name); a tab whose owner is not in the catalog says so
  instead.

`displayTimeForSource` validates a live catalog source handle and uses its current
`SessionManager::displayOffset`. Topic conversion first requires a unique
`CatalogModel::datasetForTopic` match; an empty topic selects the representative
dataset. Plugins should prefer the SDK's `toDisplayTimeForSource` when topic names
repeat. See the SDK's `pj_plugins/docs/toolbox-guide.md` for plugin examples and
error handling.

## Toolbox presentation

`ToolboxPresenter` (`src/ToolboxPresenter.{h,cpp}`) is one live toolbox: the
wrapped panel container (`MainWindow::wrapToolboxPanel`), its `PanelEngine`, the
plugin session that keeps host services and plugin instance alive, and the
presentation it currently has: takeover (chart area), tab, or floating
`PJ::Dialog`. `MainWindow::toolboxes_` is the single registry, one presenter per
plugin id across every presentation.

**Launch.** `launchToolbox` builds the session (`buildToolboxPanelSession`; the
derived-data hosts come from `pj_runtime`'s `DerivedRuntimeHosts`, which
`HeadlessDescriptorProviderSession` builds too, and the other provider services
from `ToolboxHostWiring.h`), wraps the panel, registers the presenter and calls
`moveToDock`/`moveToTab`.

**Relocation.** `moveToDock`/`moveToTab`/`moveToFloating` release the container from
wherever it is and attach it elsewhere, without teardown: the plugin session,
drawer, actions and renamed tab title survive, and `Spec::apply_chrome` re-shows the
banner in the chrome of whichever presentation the container lands in. Every
presentation keeps three header controls: docked panels offer move-to-tab,
move-to-floating and close; tabs offer move-to-docked, move-to-floating and close;
floating windows offer move-to-tab, move-to-docked and close. Generic panels hide
destinations they do not support.

**Teardown.** `teardown(kDeferred|kImmediate)` is the one exit, reached only through
`MainWindow::teardownToolbox`, which extracts the registry entry FIRST so any
callback landing mid-teardown finds nothing. Tab close, window `finished`, engine
close requests and banner hooks all look the presenter up by id rather than
capturing it. `~MainWindow` calls `teardownAllToolboxes()` (every presentation,
synchronous) before the members die; a layout restore tears down the persisted set
(tabs + floating, plus a takeover the layout names; any other takeover stays)
synchronously before relaunching.

The session dying with the presenter ends the previews that panel's plugin instance
created: `DerivedRuntimeHosts`' destructor passes its bridges' previews to
`SessionManager::removeEphemerals` (ephemeral transforms and marker rules, after the
plugin handle is gone). The headless provider session does the same for its own, so
ending it leaves an open panel's previews of the same plugin. Persistent recipes are
user data and stay.

**Takeover and close.** Nothing is registered per presentation: the busy-close fold,
the automatic fold on ingest start and a dismissal's fold all resolve the takeover's
presenter through the registry (`takeoverToolbox()`), and the banner's close button is
`ToolboxPresenter::requestClose()` (tab close goes through the strip's busy-job
confirmation, floating through the window, a busy takeover folds into a transient
tab). Replacing another busy takeover folds that panel before showing the incoming
one. The docked panel is a page of the `centralAreaStack` (`MainWindow.ui`) that
holds the chart, so docking never fails and the chart never leaves the window.

**Reparenting.** A move reparents the container once, at its destination
(`addWidgetTab`, the floating `PJ::Dialog`, the stack). Releasing it from a
presentation (`releaseCentralPanel`, `ToolboxPresenter::release`,
`TabbedPlotWidget::takeWidgetTab`) leaves its parent alone, and a teardown deletes
it in place, because every reparent re-polishes the whole widget tree against the
app stylesheet (~180 ms for a toolbox panel in a browser). The chart-area helpers
shared by the banner and the floating title bar live in `src/ToolboxChrome.{h,cpp}`.

## Custom Topics panel

`CurveListPanel`'s bottom tree lists what the session derived: transform outputs and
one flat row per object output of a persistent on-demand recipe, named by the recipe,
badged by an owner other than the editor, with its node's state as the glyph and
tooltip (the lag while computing, a finding's pin on the display axis).

- **Row set.** `MainWindow::syncCustomTopicsPanel` derives the whole row set
  (`CurveListPanel::setCustomTopics`) on every install and after a restore or a
  toolbox write (an announced removal relists the catalog, and the panel prunes the
  rows). A node's state change or an offset schedules one `refreshCustomTopicsPanel`
  per event-loop turn, which rebuilds the on-demand rows and reuses the transform
  rows of the last sync until the catalog changes.
- **Editing.** The "+", the pencil and Delete belong to the toolbox whose manifest
  declares `custom_topics_editor`. `deleteCustomTopics` asks `pj_runtime`'s
  `CustomTopicsPolicy.h` who may delete what, and asks again once a prompt is
  answered, before removing a recipe (one undo step unless it is history-exempt) or
  a transform; this shell only words the refusals and runs the prompts.
- **Series.** A series is one flat row per number column (`id/output`), deleted with
  its recipe. `SeriesProgressController` (`src/SeriesProgressController.{h,cpp}`, wired
  by `wireSeriesProgress`) turns `SeriesRunner::State` into a stoppable bar on those
  rows (`CurveListPanel::setSeriesProgress`), lingering as completed or cancelled,
  and a row's stop (`seriesStopRequested`) stops the replay keeping its rows.

See `../../docs/derived_objects/ARCHITECTURE.md` section 8.

## Custom Widget panels

`MainWindow::makeSceneDock` builds the `"CustomWidget"` kind (the `<CustomWidget>` layout tag) like the other object docks: wired to the session, the diagnostic sink, the undo debounce and `TopicDemandController::registerCustomWidgetDock`. The object-widget factory is installed whether or not Scene2D/Scene3D are built, because this kind and `state_transitions` do not depend on them. `MainWindow::createCustomWidgetDock(dock, plugin_id, type)` is what the widget picker calls (the dock factory stays in the shell, next to `makeSceneDock`): it builds the dock, binds the plugin widget BEFORE the dock mounts it (so the mount sees the plugin's widget tree), then adopts it. `onObjectFamilyRequested(kCustomWidget)` creates nothing: the widget is chosen first.

Everything else of this family in the shell lives in `CustomWidgetController` (`src/CustomWidgetController.{h,cpp}`, owned by `right_panel_stack_`, built in the `MainWindow` constructor with the other right-panel pages): both pages below, the pending target panel, the `ActiveWidgetsMarker` and the startup review. `MainWindow` calls into it from `onObjectFamilyRequested(kCustomWidget)` (`showPicker`), `onDockFocused` (`pageForFocusedDock`), `makeSceneDock` (`attachDock`), `reviewUncleanShutdown` and `~MainWindow` (`clearActiveWidgets`); the controller calls back through `Hooks` (reveal the side panel, `createCustomWidgetDock`, `onDockFocused`).

Two pages of the right side panel (`right_panel_stack_`) serve this family, both `.ui` files under `src/ui/`:

- `CustomWidgetPickerPanel`: the installed widget types, read from `ExtensionCatalogService::widgets()` and rebuilt on `catalogChanged` and every time it is shown. One row per type: the manifest's inline SVG icon tinted with the theme ink (the generic `custom_widget.svg` when there is none), the name, and "from <library>". A library built against another Qt is listed disabled, its mismatch reason as tooltip. The band's filter shows past `kFilterThreshold` rows. With nothing installed the page says so and offers the Marketplace (`onOpenMarketplace`; hidden on WASM).
- `CustomWidgetConfigPanel`, bound through `CustomWidgetController::pageForFocusedDock` to the focused `CustomWidgetDockWidget` (`bindDock`, like the scene pages): header with the widget and library names; one row per input (label, unit, kind) with its wires (series, pending state, remove) and "Choose series" (a normal input: replaces slot 0) or "Add series" (a `multiple` input: `WidgetBindings::kNewSlot`), which swap the form for a `CurveTreeView` chooser listing only the catalog keys `CustomWidgetDockWidget::canBind` accepts; then the settings form, built by hand from `settingsDeclarations()` (`number` → `DoubleScrubber` with `min`/`max`, `integer` → the same scrubber with no decimals, stored as a JSON integer, `boolean` → `ToggleSwitch`, `text` → `QLineEdit`, `color` → `ColorPickerWidget`, `enum` → `PJ::ComboBox` over `options`; label from `label`, else `key`; value from `settingsJson()`, which already holds every declared key with its manifest `default`). A change edits one key of the current `settingsJson()` and calls `applySettings()` with the whole object, once per change; number edits are debounced (`kSettingsDebounceMs`). The form is built on bind only, so a dock signal never destroys a control in use; the dock's `inputsChanged` / `bindingsChanged` rebuild the inputs (and the header after an input change) once, from the event loop, however many arrive together. A missing widget shows its reason and the wires of its stored layout element, read-only.

Flow: the fifth placeholder icon → `CustomWidgetController::showPicker(dock)` keeps the empty dock in a `QPointer`, opens the right panel if folded and shows the picker; `pageForFocusedDock` keeps the picker up while that empty dock has the focus. A pick → `createCustomWidgetDock` and the config page of the new panel. A dock that was closed or filled in between is left alone.

Crash loop breaker (desktop only): `CustomWidgetController` owns this process's `ActiveWidgetsMarker` (`active_widgets.<pid>.json` in the default directory) and hands it to every Custom Widget dock, which records its plugin before the first call into it and removes it once the instance is destroyed. `~MainWindow` clears the file right after the docks are destroyed (`delete ui_->tabbedPlotWidget`), the one seam both the window close and the `--exit-after-layout` / `--screenshot` quits pass through. `main.cpp` calls `reviewUncleanShutdown(interactive)` once, after the window is shown and BEFORE the `--layout` restore is queued: it consumes the records of runs whose process is no longer alive (another instance still running keeps its own) and, when they name widgets, reports one `kWarning` (source `CustomWidget`) listing them. Interactive runs then ask with `MessageBox::question` (Disable / Keep); Disable calls `ExtensionManager::setEnabled(id, false)` for each plugin and `ExtensionCatalogService::reload()`, which withdraws the now-disabled Widget libraries from the catalog, so their panels restore as placeholders. A run with `--exit-after-layout` or `--screenshot`, or on the `offscreen` / `minimal` platform, is not interactive (`isUnattendedRun` in `main.cpp`; no other startup check covers all four): it only reports and keeps the widgets. An unreadable record is a warning and no question.

Tests: `tests/main_window_custom_widget_test.cpp` (including the record's lifecycle and the startup review: `UncleanShutdownTest` gives each case an application name of its own, because Disable needs the extension store's single-writer lease and writes the persisted disabled list), `tests/custom_widget_panels_test.cpp` (both pages over a bare session, the shell flow, and dark-theme captures saved under `PJ_UI_TEST_DUMP_DIR` when set), and the end-to-end runs of the executable registered from `pj_custom_widgets/tests/E2E.cmake`.

## Browser (WebAssembly) boundary

On WebAssembly the shell owns the browser boundary: `BrowserFileStore` (upload
staging and identity, bytes behind a lifetime lease in MEMFS), `FileSelectionService`
(the asynchronous content-based file picker) and `BrowserPersistence` (the durable-settings bridge).
`PresetController` downloads through `pj_runtime`'s `fetchBytesWithLimit()` and streams recordings through `BrowserFetch.h` there ([why](../../pj_runtime/docs/TECHNICAL_NOTES.md)).
Browser-only integration is tied to the executable's lifetime and policy, so it lives
here rather than in a library.

### Persistence

Native builds keep their normal `QSettings` behavior. WASM redirects ordinary
`QSettings` callers to page-lifetime MEMFS and lets `BrowserPersistence` alone write
`WebLocalStorageFormat`. Only its exact, typed preference allowlist and up to five
bounded, source-free generic layout recipes are durable. File paths, upload
identities, plugin configuration, credentials and source-bound layouts must never
enter durable browser storage. If the stored preference envelope is unreadable, it is
left intact rather than deleted (a newer build may understand it) and none of its
values are imported into the session. On WebAssembly `main()` constructs
`BrowserPersistence` after the application identity and before the splash, `Theme`,
`WidgetTuner`, `MainWindow` or any plugin host, so every settings consumer sees the
redirect; native builds never construct it.

### Acceptance probes

`tests/wasm_acceptance_probes.inc` is the acceptance-build-only observation and input
bridge, included behind `PJ_WASM_ENABLE_INGRESS_PROBE` (with the plot readback
probes, `MainWindow::installWasmPlotAcceptanceProbes()`); it is never part of desktop
or production WASM behavior. `tests/wasm_scene3d_acceptance_probes.cpp` is its
Scene3D counterpart, reading the view through `SceneViewTestAccess`. With
`PJ_WASM_WITH_PYTHON` it also includes `tests/wasm_python_probe.inc` (the on-demand
Python checks that the repo-root `tests/wasm/python.spec.js` asserts) and compiles
`tests/wasm_python_stdlib_probe.cpp`, which walks what a strict script can reach and
imports it from the embedded stdlib zip.

## Concrete codecs behind runtime interfaces

`pj_runtime` links no Scene2D library, so the shell supplies the concrete codecs the
runtime reaches only through an interface. `SceneImageDecoder`
(`src/SceneImageDecoder.h`, over Scene2D) implements `scripting::IImageDecoder`,
which on-demand scripts decode through; `SceneVideoFrameSource`
(`src/SceneVideoFrameSource.h`, over Scene2D and FFmpeg) implements
`scripting::IVideoFrameSource`, which their `frame_at` reads. `MainWindow` hands them
to `SessionManager::setImageDecoder` and `setVideoFrameSource`.

## Tests

`pj_app_gui_tests` links `pj_app_shell` and `pj_resources` for GUI scenarios and
runtime-backed controller and binder tests. `pj_app_unit_tests` compiles the
Qt-only helpers directly, without the shell's dependency closure.
`pj_app_media_codec_tests` compiles `SceneImageDecoder` and `SceneVideoFrameSource`
over `pj_scene2d_core` alone (plain main, run from the source root for the tracked
video fixture). The first two use `pj_add_test_runner` with the shared GUI main (see
`cmake/PjTests.cmake`); `gtest_discover_tests` registers each case under its runner's
name and launches it in a fresh process with a 120-second timeout. That process
isolation is what keeps `MainWindow`'s process-global widget state from leaking
between cases, so run these through ctest.

`tests/support/gui_test_env.cpp` registers the shell runner's OpenGL-disable
environment once. File-local environments preserve custom settings identities and
slider theme ownership; they check the selected suite before changing process-wide
state. The shared main owns the only `QApplication`.

Suites that keep their own executable and ctest entry, and why:

- `rhi_dock_lifecycle_test`: `QRhiWidget`s in ADS docks through float/redock, split,
  tab switch, sibling destruction, floating-window close and a DPR change, plus the
  mixed-API negative case. Needs a real display (it skips on `offscreen`) and is
  registered on every backend of the platform plus `.opengl_dpr2.` (`RHI_BACKENDS
  DPR2` in `cmake/PjTests.cmake`). Its setup mirrors `main()`'s
  `AA_DontCreateNativeWidgetSiblings`; see `../../pj_scene3D/docs/TECHNICAL_NOTES.md`
  section 5 (ADS and window-system quirks).
- `rhi_mixed_dock_test`: the same matrix with the real widgets in one dock manager (a
  `PlotWidget`, a Scene3D point cloud, a Scene2D image), each checked for its own
  content at its pixel size, no QRhi warning, no resource left on a dying QRhi
  (`QT_RHI_LEAK_CHECK`), and, through the widgets' test-hook lifecycle counters, a
  re-initialize on every float and redock; the same docks added to a window first
  shown without a GPU widget (`RhiMixedLateDockFixture`: the app needs no bootstrap
  `QRhiWidget`) and a Scene2D dock built in place inside one (`RhiScene2DInPlaceTest`); plus the media viewer's failure state beside them. Registered like
  `rhi_dock_lifecycle_test`; both share `tests/support/rhi_dock_test_support.h`. Its
  widgets and data come from `tests/mixed_layout_fixture.h` (`pj_mixed_layout_fixture`,
  public APIs only), which the `mixed_layout_benchmark` demo (`benchmarks/`,
  `PJ_BUILD_DEMOS`) also uses.
- `main_window_layout_exit_test`: the last case leaves an import parked for
  destructor-time shutdown, so suite ordering and teardown are part of the test.
- `main_window_layout_import_lifecycle_test`: ordered import supersession,
  cancellation and rollback scenarios, with its own mock-plugin definition.
- `main_window_layout_import_alive_test`: preserves the import lifetime through the
  catalog-empty guard and completion drain.
- `main_window_layout_import_cancel_test`: preserves mid-import cancellation and
  restoration of the prior workspace.
- `main_window_layout_import_binder_test`: its mock-plugin path definition is specific
  to this suite; it must not propagate to other sources.
- `main_window_layout_import_e2e_test`: installs a private HOME/XDG sandbox before
  `QApplication` and uses the live cross-repository E2E harness (every scenario skips
  unless its `MCAP_CLOUD_E2E_*` environment is present).
- `file_loader_test`: four plugin-path definitions plus child-process shutdown and
  teardown probes in `file_loader_shutdown_progress_test.cpp`.
- `source_promotion_host_test`: only the mock file-source plugin path is defined.
- `headless_descriptor_provider_session_test`: uses a static fake provider and no
  plugin-path definitions; also covers provider quiescence on teardown.
- `layout_import_batch_test`: desktop-only, with both the mock file-source and the
  runtime host object-parser plugin paths.
- `splash_meme_pool_test` (`SplashMemePoolTest`): compiles `Splashscreen.cpp`
  standalone with `PJ_MEMES_SOURCE_DIR` and the shipped resources.

The loader suites (`file_loader_test`, `source_promotion_host_test`,
`headless_descriptor_provider_session_test`, `layout_import_batch_test`) share the
`pj_app_loader_testlib` object library, so the loader sources compile once while each
binary keeps its own plugin-path definitions. `ExportedSymbolsGuard` is a script test,
enabled when the platform and `nm` support inspecting the executable's exported
symbols.
