# pj_app architecture

How the executable shell is put together. `../CLAUDE.md` holds the invariants; this document covers what spans
several classes. The wiring is read best from `src/MainWindow.cpp` and `src/main.cpp`; per-class contracts are in
the headers. The derived-data design the shell wires up is
[`../../docs/derived_objects/ARCHITECTURE.md`](../../docs/derived_objects/ARCHITECTURE.md); the sections below cover
only the shell's half of it. Measurements behind some choices: [TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md).

## Threads and teardown

Everything in the shell lives on the GUI thread. Two kinds of worker exist:

- `FileLoader` runs only each import's blocking `start()` on a worker thread; every other plugin call, the fan-out
  control and the queue run on the GUI thread, and the worker reaches it only through queued calls. Every accepted
  request ends in exactly one `loadFinished`, delivered through the event loop even when the request resolves inside
  the enqueuing call. A `kFailed` terminal carries the failure's reason: a prologue failure on an idle loader emits
  `fileLoadFailed` before `loadFileTicketed` returns, so a caller holding a ticket reads the reason from its terminal. `cancelLoadAsync` is the one entry callable from another thread.
- `StreamingSourceManager` runs one worker per streaming session. Configuration, `start()` and stop requests run on
  the GUI thread; once launched, the worker is the only caller of the plugin (`poll()`, then `stop()`). What a layout
  save needs is captured before the worker starts, so a save never calls into a polling plugin.

`~MainWindow` runs in this order; each step names what breaks if it moves later:

1. `RecordingService::stop()`: its `stopped()` slot touches widgets deleted below.
2. `StreamingSourceManager::cancelPendingStarts()`: a start waiting on its dialog is withdrawn and its dataset
   discarded.
3. Layout-batch observation, then `teardownAllToolboxes()` (every presentation, synchronously): plugin sessions must
   be gone before the services and widgets they hold.
4. The MCP server: no plugin is left to restart it, and its jobs point into toolbox services already torn down.
5. A non-toolbox central panel still presented (marketplace, filter editor), then `PluginPlotTabsController` (it
   holds the tab strip), then the tab widget, then the pending deferred deletes: content a dock replaced is parentless
   and only deleted later, and a Scene2D image layer in it runs a decode thread over the session's object store.
6. `CustomWidgetController::clearActiveWidgets()`, right after the Custom Widget docks die (see "Custom Widget
   panels").
7. Timeline and curve list unbound from the engine and catalog; `FileLoader` disconnected from the window; then
   `delete ui_`. The loader is a member destroyed after the body, and its destructor delivers queued terminals
   synchronously: connected slots would touch deleted widgets.

## Playback and panel access

Space toggles Play/Pause through a non-repeating `QShortcut` on `TimelineWidget`; a text editor keeps Space through
Qt's shortcut override. A workspace restore (layout load, its rollback, undo/redo, the pinned-toolbox relaunch) hides
or deletes the focused plot or switches the shown tab, and Qt would then focus the next widget in the tab chain, a
filter in the browser. `MainWindow`'s `RestoreFocusGuard` spans each restore and then gives focus back to the widget
that held it if it is still shown, else to the main window; `PlotDocker::xmlLoadState` does the same around its own
hide, which a duplicated tab reaches outside any restore.
An object dock's context menu offers Open/Close Settings; `settingsPanelToggleRequested`
travels through `PlotDocker` to `MainWindow`, which binds the settings page to that dock and toggles the right panel.

## Window frame

The window wears the system title bar by default, and `TitleBar` is the toolbar row under it. The
`custom_title_bar` preference (read at construction, so it applies on the next launch) makes the window frameless
with `TitleBar` as its frame: only then do the edge-resize filter and the 1-px border run.
`MainWindow::usesCustomFrame()` is the platform rule: WebAssembly is always custom (a browser tab has no window
manager), macOS always system, Linux and Windows follow the preference; `customFrameSelectable()` hides the toggle
where there is no choice. `PJ::Dialog` and `MessageBox` keep their own chrome either way. `Theme` requests the
matching Qt color scheme on every switch, so a platform-drawn title bar follows light/dark on Windows and macOS; a
Linux window manager keeps its own.

## Command-line startup

Desktop startup takes repeatable `-d` / `--datafile <path>`, positional files and `-l` / `--layout <path>`.
`MainWindow::loadAtStartup` waits for the terminal ticket of every explicit file before applying the layout. Explicit
files override a source-bound layout's saved files: the layout applies as a generic layout, but each saved dataset it
names is mapped to an explicit file when it can be, and its qualifiers keep their binding, pointed at that file, so a
layout comparing two runs reopens with `-d run1 -d run2 -l layout`. `layout_xml::matchDatasetsToFiles` maps each file at
most once: a saved path that is one of the files; then the only file still unclaimed with the saved file name (a
path-less source label, with that label); then, per format (case-insensitive file suffix), the saved paths and files
still left, in order, when that format's counts are equal. A path-less label never pairs by position: it has no format
and may be a toolbox's own source. Dataset order is the layout's `<fileInfo>` list, then first appearance in the
document; files keep command-line order. A dataset is its saved path, or its source label without one; `pj-upload://`
and `pj-stream://` identities never map. Each mapped dataset is reported once (`Layout`/`dataset-mapped`: `Layout
dataset "<label>" -> <file>` and how; a path-less label that follows its dataset's path adds no line); everything left
unmapped is stripped and binds only where unique. A kept label the loader derived from the saved file name (the file
name, or `<base>/<entry>` for a fan-out) follows the new name, since the identity resolver compares labels exactly.
`removeDatasetQualifiersForGenericLayout` applies the mapping to every consumer, after `resolveDatasetSourcePaths`.
Without explicit files, `--layout` replays its saved sources and starts its recorded streams with no prompt.
A startup layout prompts only for which dataset(s) to show, and only when someone is at the display ("One layout over
several datasets"); with two or more files that question is part of the batch question asked before they load ("Several
files at once" under "File loading"); its other failures are diagnostics (`LayoutLoadInteractivity::kStartupAttended`). Unattended
startup (`--exit-after-layout`, `--screenshot`, `--dump-diagnostics`, the `offscreen`/`minimal` platforms;
`isUnattendedRun` in `main.cpp`) loads with `DialogPolicy::kNever`. A failed or
cancelled explicit file skips the layout and settles it false, so `--exit-after-layout` exits unsuccessfully. The final
restore is deferred through a window-owned timer so it never runs during teardown. `--exit-after-layout` exit codes are
a contract for harnesses (`LayoutExitWatch`, which hears only the settle of the first load started after it is built: the startup load,
unless another load starts while the startup data files are still loading): 0 applied, 1 load
failed, cancelled or superseded by another load, 2 timed out, 64 usage error. With
`--screenshot` too, whichever quit fires first wins, and only `--exit-after-layout`'s code says whether the layout
applied. Tests: `MainWindowStartupTest`.

## File loading

A desktop drop sends `.urdf` files to `MainWindow::addDroppedRobotModels` (a robot model in the 3D view under the
cursor, else the only 3D view, else a toast); every other dropped file is data for `FileLoader`.

`FileLoader` runs file loads: one sequential queue, each load progressive on a worker. `LoadInput` carries the
display name, backing path, reload identity and, in the browser, the storage lease that keeps an upload alive for
deferred object fetches. `FanoutConfig` extracts a plugin config's `__pj_fanout` entries (one plugin instance and
one `DatasetId` per entry). `FileLoader` does not depend on `MainWindow` (`setFilePicker` is the seam), so the loader
suites compile it standalone.

**Several files at once.** Files picked together, dropped together or listed on the command line (two or more, with
someone at the display) ask one question before any loads (`BatchLoadDialog`) when a loader with a configuration
dialog would get two or more of them (`FileLoader::batchRepeatsADialog`, by each file's first-match loader): how the
loaders' configuration dialogs behave for the batch, remembered across sessions (`BatchDialogMode`). *Show all* shows every file's dialog; *show
first* shows it for the first file of each loader, and the others load with the settings accepted there; *skip* loads
every file with its loader's last-used settings. Both reuse modes read the config the loader saves after an accepted
dialog, so a loader with no saved config, or whose config fans one file out into several datasets (`__pj_fanout`),
still shows its dialog, as does one whose config sets a `display_name` (it names that file's tree root). "Show first"
counts a loader as asked once its dialog was accepted and its settings saved. Loaders are told apart by manifest id.
Cancelling a loader's dialog cancels that loader's other files in the batch, in every mode; Cancel on the question
loads nothing. The batch state (`LoadBatch`, in `LoadHints::batch`) is shared by the files' requests, which
run one at a time on the GUI thread. `FileLoader` stays UI-free: the shell injects the question (`setBatchAsker`), and
without one each file loads on its own. With a layout on the command line that names at most one dataset, the
question also lists the files, all checked (and shows without the mode choice when no dialog would repeat): each
checked file gets its own copy of the layout's tabs. The checked paths reach `chooseLayoutDatasets` as a
parameter of that layout's load and answer its dataset question ("One layout over several datasets") instead of asking
it after the load; when the layout cannot be copied and several were checked, it still asks, to say why only one
shows. The browser picker stages one file per pick, so it never
forms a batch. Tests: `BatchLoadTest`.

**Object ingest policy.** `FileLoader::applyDefaultIngestPolicies` loads object topics as
`kLazyObjectsEagerScalars` by default: each payload is read once at ingest for its scalar fields (so header-bearing
topics expose their header and timestamp series), then dropped, so clouds, images and grids are never resident. Video
frames, scene entities and image annotations are pure lazy; so are robot descriptions in the browser, whose parser has
no scalar handler. The cost is one extra read per payload at load.

**Fan-out reload.** One dataset cannot be refilled into N, so a fan-out reload tombstones the existing dataset, creates
fresh ones and erases the tombstoned one only after commit: erasing it first would lose the user's data on a discard or
an all-failed fan-out. A whole-load stop (strip, shutdown, `cancelLoad`) ends every entry; a whole-load discard also
drops entries that already completed.

**A load that yields nothing fails.** A plugin can finish its import successfully with no row and no object entry
(a CAN log without a decoding database, a file with a header only). After the terminal flush `FileLoader` asks
`SessionManager::datasetHoldsData`; when it is false the load fails like a start failure (`kFailed`,
`data-source-load-failed`, `fileLoadFailed`). The reason quotes the plugin: the last warning or error it reported
during `start()` (`DataSourceRuntimeHost::lastReportedProblem`, cleared just before `start()`), else the reason of a
FAILED `request_stop`. A first load erases its dataset, which frees its label, so the next load of the same file
gets the plain name; a reload rolls back to the data it was replacing; a fan-out entry fails alone. A stop-and-keep
that kept nothing is a discard, and a fan-out a user stopped reports a cancel even when an earlier entry failed. A
layout's data file that yields nothing fails the layout restore, as any load failure does. Every file entry point (open dialog, drop, `-d`, layout replay, MCP
`open_file`) goes through this one check; streaming sources start empty and never reach it. Tests:
`FileLoaderTest.*YieldsNoData*`.

### Load diagnostics

`MainWindow` wires `FileLoader::setDiagnosticSink` to the bridge sink.

- `data-source-load-failed` (error, source "File Loader"): once per failed load and once per failed fan-out entry,
  none for the all-failed aggregate, never for a reject, cancel, discard or shutdown; also the browser picker's
  staging failure. The same cause may also appear under the plugin's own id.
- `data-source-message`: the plugin's warnings and errors (source = plugin display name, message prefixed with the
  file or entry name), capped per load at `DataSourceRuntimeHost::kMaxForwardedMessages`; the next one yields a
  single `data-source-message-limit` warning and the rest stay in the log.
- A missing layout data file is reported before `FileLoader` runs, as `Layout`/`data-source-missing`.
- Streaming sources are not wired to the sink.

### Reload button

`MainWindow::reloadSource` disables the left panel's Reload button until its request resolves.
`FileLoader::loadFinished`, the one terminal every accepted request reaches (including the exits `fileLoadFailed`
stays silent on: a rejected config dialog, a queued cancel, shutdown), brings it back through
`refreshReloadEnabled()`, which follows whether a source is recorded (`lastLoadedSource()`; `onFileLoaded` records it
first, because `loadFinished` is queued). A request the loader refuses gets no ticket and no terminal, so
`reloadSource` refreshes on a `false` return. The guard only stops a double click; the loader's queue serialises a
second reload anyway. Tests: `ReloadButtonTest`.

### Per-dataset load progress

`wireIngestProgress()` (`IngestProgressWiring.h`) is the one definition of the connections between
`SessionManager`, `IngestProgressController` and `CurveTreeView`: controller attach, the dataset-path resolver,
progress and row-key updates into the panel, and the cancel route back with its token, staleness and cancellability
checks. Tests and tooling wire through it, so they exercise the app's connections, not a copy.

`IngestProgressController` turns `SessionManager`'s dataset-scoped ingest signals into `CurveTreeView`'s progress
decoration. The row key is the ingest id (`IngestToken::id`), so a click on a row resolves to its ingest by lookup.
The controller owns the row label (the dataset's name once resolvable, then kept, so a discard that deletes the
dataset cannot rename the row mid-flight), the completion linger, the error flash, immediate retirement on cancel,
and ghost rows for loads whose catalog entries vanished. `FileLoader` and `HeadlessDescriptorProviderSession` produce
tokens on the same channel.

**Keep or discard.** The row carries the choice (X keeps what arrived, the bin discards it), so the shell calls
`requestCancel(token, keep_partial)` and never prompts. Honouring the flag is the producer's job:

- `FileLoader` latches a whole-load stop for the current load only (`startNext` clears it before the next request),
  and a row stop in a slot owned by that fan-out entry's loop iteration, so a row stops only the entry it shows.
- Keep commits the partial data (on a reload, the partial refill replaces the prior data).
- Discard means "as if this load never ran": a first-load discard deletes the dataset it created; a reload discard
  rolls back to the pre-reload data (refill-guard rollback plus `TransformService::abortReplacingLoad`).
- Toolbox imports forward the plugin's `cancellable` declaration and register as `IngestStop::kStopOnly` (their ABI
  has no rollback): a live X, the bin greyed.
- `HeadlessDescriptorProviderSession` registers `cancellable=false` (there is no UI to stop it from).

The title-bar strip (`IngestProgressWidget`) is wired but hidden by `kShowTitleBarIngestStrip` in `MainWindow`'s
constructor. Its single Stop button cannot express keep or discard, so behind the flag it raises a keep/discard/resume
prompt while `FileLoader` is busy; otherwise it routes by the strip's displayed owner: a layout-import batch job takes
`cancelActiveImportJob()` (that job ends cancelled, the batch continues), interactive toolbox imports take
`stopAllToolboxImports()`.

## Streaming sources

`StreamingSourceManager` owns the live sessions (one `DatasetId` each) and the secondary tail buffers used while
paused. Those buffers can hold rows of a stream that already ended (they merge at resume), and a merge moves nothing
for a topic with no destination: whoever erases a dataset also drops its secondary copy
(`StreamingSourceManager::removeSecondaryDataset`).

**Followed stream.** While streaming, the timeline follows one stream's live edge (`MainWindow` scopes it with
`AppSession::setActiveStreamingDataset`). Each launch hands live-follow to the stream just launched. When the followed
stream stops, the last-launched stream still running that already has data takes over; launch order, not `DatasetId`
order, since an interactive start creates its dataset when its dialog opens. With no such stream, live-follow ends:
playback runs to the end of the range and pauses, because following a stream with no data would hold the cursor
still while it reads as playing.

An interactive start (`startSession`) never waits for the source's dialog: a browser cannot run a nested event loop,
and one path on every platform lets desktop tests cover what the browser runs. The start is pending while its dialog
is open, launches or cleans up when the user answers, and is withdrawn (dialog closed, dataset discarded) by whatever
stops every stream or destroys the manager. The session's dataset exists (in both engines) before the dialog opens,
so every start that does not launch (cancel, a plugin contract violation, a refused configuration, a failed
`start()`) destroys the session and then discards the dataset, as `startFromLayout` does; an abandoned dataset would
count in the catalog's duplicate-label ordinals and suffix the next start's name. `streamError` is emitted only after
the dataset is gone (a cancel reports nothing), so its consumer cannot rely on that dataset existing.

**Source and buffer selection.** The manager keeps no copy of the selected source: Start carries the combo's text
(`LeftPanel::streamingStartRequested`), so whatever set the combo (a user, a repopulation, a layout), Start starts
what it shows. Only a user's pick (`QComboBox::textActivated`) becomes the remembered default. The retention window
is a copy: `StreamingSourceManager` reads it from QSettings once, at construction, then only from
`LeftPanel::streamingBufferChanged`, which a restore emits by hand, because running sessions must hear the new
window.

A layout's `<left_panel_state streaming_buffer>` belongs to the streams it restarts: it is saved only into a
stream-bound layout (one with `<previouslyLoaded_StreamingSources>` records), and a load ignores it in any other
layout, older files that carry it included. It never shrinks a stream that ran before the open: such a stream keeps
a wider window and the scrubber shows it. The streams the layout starts itself do not count, so whether a stream ran,
and its window, is taken when the open begins (`snapshotStreamWindowAtLayoutOpen`), before they start; otherwise a
buffer smaller than the user's default would never apply. `MainWindow::restoreLeftPanelState` decides the buffer and
runs at `commitRestoredLayout`, so a progressive restore that aborts changes neither the streams nor the panel.
Streams the layout starts itself run with the previous window until that commit.

**Pause and resume.** While paused, each session writes to the secondary engine and object store. Resume swaps the
write targets back first (a push already in flight finishes on the secondary, the next lands on the primary; flushing
first would race new arrivals), then moves the secondary chunks and entries into the primary without copying,
catches the eager processors and marker rules up on the merged tail, then re-trims each streaming dataset (the
engine flush does not trim) and announces a live ingest so follow-live views land on the merged edge.

`StreamingSourceManager::onIngestTick` is the GUI half of each poll cycle (`ingestTickForTests` drives it): eager
processors advance over every topic of the dataset (skipped while paused), then retention trims the engine being
written, then the marker recompute and the ingest announcement run. A stateful processor must consume every sample
before retention evicts it, or its output skips rows. See the derived-objects architecture, "Streaming".

`TopicDemandController` is thin glue from displayed widgets to `TopicDemandTracker`; `PendingDisplayBinder` stages a
curve or scene layer until its topic materializes. `PendingDisplayBinder::collect` records each plot's saved curve
order, and `flush` puts a late curve back in its saved place (`PlotWidget::restoreCurveOrder`) before re-applying the
saved viewport. That holds only while the plot still holds its saved window: once the drain or the synchronous restore
applied it for good, a curve binding later appends. Two curve kinds do not take their saved place: an empty-field
placeholder intent (one curve per field) and an XY curve whose `<curve>` has no `name` (only hand-written layouts). On
the first real demand for a previewed topic, `TopicDemandController` invalidates the matching XY and time-series caches
of registered plots before evicting the preview history, with no paint or event-loop turn in between: bounds
calculation inside `curveListChanged` may have filled them already.

The topic streaming context menu needs a single selected row of a source that can pause per topic; a field row
resolves to its topic. Several selected rows suppress it, because Linux can request the menu on right-button press,
before an XY right-drag has started. Dataset-level menus accept several datasets. Tests: `CurveListPanelTest`.

The curve list draws in semi-bold italic what the selected widget displays, and the groups above those rows. The
selected widget is `MainWindow::activeFocusedDock()`, which `CurveListPanel` asks for on each refresh (it is often
the tab's first dock, which changes without a focus event); `onDockFocused` reschedules the refresh.
`TopicDemandController::displayedKeys(root)` collects the catalog keys of every widget family inside that dock and
emits `displayedSeriesChanged` when they or the catalog change; a scene dock reports its config topics through
`SceneDockWidget::configTopicsChanged`. The panel refreshes at most once per event-loop turn.

## Session recording

Recording is a runtime capability: the `RecordingService` member, its connections and the shutdown `stop()` are
unconditional. The streaming strip's record button and status label stay hidden until
`LeftPanel::setRecordingSupported`, which only `MainWindow` calls, with `RecordingService::isSupported()`. Preferences
shows its Recording row and page on the same query.

- `StreamingSourceManager::recordingTargets()` returns only sessions whose plugin advertises delegated ingest (a
  direct writer has no raw bytes at the seam), plus the display names it left out, for the shell to say what will
  be missing.
- A target is keyed by its `DatasetId`, which the service uses to address that source afterwards. Display names may
  repeat: `RecordingService` names the files.
- A target's `attach` returns `false` when the session ended before the tap could be installed, which fails and rolls
  back the whole capture. Taps attach at start only.
- A stream that ends mid-capture must not stop the capture: the `streamStopped` handler forwards
  `RecordingService::onSourceEnded(dataset_id, reason)`, which finalizes that one file. The batch ends with its last
  live source or the user's Stop.
- `MainWindow::onRecordingStopped` reports the whole capture: the path for a single source, the folder when several
  recorded cleanly, otherwise the problem count and up to two failing sources (naming `mcap recover` when a file could
  not be finalized), plus the dropped-message count when the write queues shed messages. Per-source detail goes to the
  log: a toast with one line per file is unreadable at three sources.

Design and write-queue budget: [`../../docs/RECORDING_AND_CACHING.md`](../../docs/RECORDING_AND_CACHING.md); the
on-disk contract is `pj_runtime`'s `RecordingFormat.h`.

## Layout files and derived recipes

`LayoutXml` is the DOM helper for `.pj4.xml` layouts (no `MainWindow`, no Qt Widgets; compiled straight into
`pj_app_unit_tests`). `LayoutReplayHints.h` builds the `LoadHints` shared by every replay of a layout-recorded source
(desktop reload, browser source-bound replay, the import batch's cache-hit loads): automated, no dialogs, the saved
preset authoritative, its filepath (and its fan-out children's) pointed at the path the replay loads.

**Schema versions.** The root's `pj4_version` changes only on incompatible changes (the loader skips unknown elements
and attributes, and a newer layout loads best-effort with a warning); what each version changed is the comment on
`kLayoutSchemaVersion` in `MainWindow.cpp`. A layout is written as v4, or v5 when it carries stream records, so an
older build warns about exactly the layouts whose streams it would drop. Offsets from a v3-or-older layout include
the global reference, and the loader subtracts it.

**Relative paths.** A file save stores paths relative to the layout folder when the target is at or under it
(`relocatableSubpath`): `<fileInfo>` filenames, every `*_dataset_path` qualifier of a source-bound layout (all widget
families: `stampDatasetSourcePaths`, then `relativizeDatasetSourcePaths`) and, in both bindings, a `<robot_model>`'s
URDF file and `file://` package roots (Scene3D's `relocateLayoutResources`). `loadLayoutDocument` resolves them
against the folder of its `LayoutOrigin` before any restore (`resolveDatasetSourcePaths`, Scene3D's
`resolveLayoutResourceUrls`). A document with no file (an MCP agent's text or URL) has no folder: a relative path in
it is reported as `relative-path-unresolved` and binds nothing. A URL's layout anchors its robot models at the URL
(`LayoutOrigin::url`); in text a relative one fails the load. A save
with no file (`layoutFileBytes` with an empty path) writes them all absolute. Undo snapshots and in-memory captures
stay absolute; browser layouts skip both steps; a generic layout has no dataset qualifier, except those startup keeps
for its explicit files ("Command-line startup").

**`__pj_saved_filepath`** exists only in the config handed to a plugin's `loadConfig` on a layout replay of a native
file (`rewriteReplayFilepaths` with `SavedFilepath::kRecord`) and never reaches host state: the rewrite drops any
incoming value first, `captureSavedConfig` strips a plugin's echo, and a `prefer_reuse` hit (no plugin runs) goes
through the rewrite with `SavedFilepath::kOmit`.

**Derived recipes.** The `<data_processors>` block is saved and restored by `pj_runtime`'s `DerivedObjects`.
`restoreDataProcessors` = `derivedObjects().parse(root, intent)` + `checkDataProcessorDependencies` +
`applyDataProcessors`. The preflight refuses a preview that holds an on-demand key the snapshot wants and, under
`kHistory`, a restore whose removals would strand an exempt dependent. The shell owns the UI: every diagnostic is
reported by `reportDerived` under source "Layout" (so `layout_issue_capture_` sees it), and the Custom Topics panel is
re-derived after every restore.

**A derived object that cannot be restored is skipped, like a missing curve.** `DerivedObjects::restore` reports each
filter, transform, marker generator or on-demand recipe it could not restore (an input the loaded data lacks, a dataset
path that is not loaded, a malformed entry) and installs the rest. `applyDataProcessors` turns that into a failed
restore only under `MissingCurvePolicy::kExact` (undo/redo, the browser's all-or-nothing apply); under `kPrompt` and
`kRetainAndDiagnose` (File → Load Layout, `--layout`, the progressive drain) the entries are skipped with their
diagnostics and the layout applies. `kPrompt` lists them in the missing-curve prompt (`promptMissingCurves`), with the
same Cancel; `kRetainAndDiagnose` shows one toast. A curve that plots a skipped output is then an unresolved curve, so
it takes the missing-curve path of the same policy. Only the preflight refuses before anything mutates; the drain
reports its refusal as `processor-restore-refused`.

**A save never loses what the load skipped.** `DerivedRestoreReport::skipped` names the exact saved elements the
restore skipped. Under `kRetainAndDiagnose` `applyDataProcessors` stages copies of exactly those, and the layout's commit
point (`commitRestoredLayout`) makes them `kept_derived_`; `layoutFileBytes` writes them back, as pending curves are
kept and saved. Identity is never guessed from the file: one filter or rule bound to two runs, one of them not loaded,
keeps the other binding. Each copy loses its session dataset ids; a qualifier only an id named (a stream bound at load)
gets back its `pj-stream://` identity. Under `kPrompt` the prompt asked instead: its Continue drops them, and its title
and button name the layout's data, not curves, once a derived object is listed.

A kept entry is retired for good when a live recipe stands for it: same kind and name (a filter by its output, the
other kinds by owner and id) and, for a filter or a generator, the same dataset (path, else source name). That check
runs on every catalog addition and marker change (`retireReplacedKeptDerived`), so creating and deleting the recipe
before a save does not bring the old entry back. A kept transform or on-demand recipe whose output a live one now writes
is not saved: the next load would refuse it. Only layout files carry kept entries: an undo snapshot or a `kExact`
rollback target would fail on the same missing input. The next layout load replaces them; removing every dataset, or
trashing the last catalog row, drops them.

**Restore order.** `applyWorkspace` parses once and restores: preflight (read-only, before anything mutates) ->
per-source display offsets -> processors -> rebind every curve to a catalog key. Offsets come before processors so a
generator that reads the display frame sees the settled offset. A layout restore replaces the persisted toolboxes
(every tab and floating window, plus a takeover the layout names; any other takeover stays) synchronously before
relaunching them, after asking once when one of them is busy.

### One layout over several datasets

A layout aimed at one dataset, applied while several loaded datasets carry its topics, would leave each reference to
such a topic ambiguous: plot curves unresolved, scene layers pending, an empty workspace. Such a binding is never
guessed silently: it is asked about, or chosen by the rules below (an automated load's choice is reported). The rules
are intended behaviour, each with its reason. `layout_xml::planDatasetChoice` decides, `layout_xml::pinLayoutToDataset(s)`
rewrite the document, `MainWindow::chooseLayoutDatasets` asks through `LayoutDatasetDialog`, reports, and hands the
result on.

- **When it applies.** On the synchronous leg (nothing left to reload or start), when the file named at most one dataset
  (`layout_xml::datasetIdentities`: a generic layout names none) and that dataset is not loaded (a loaded file named
  under another label is loaded: the curve binding finds it by its path). A layout naming several datasets, a stream or
  an upload keeps its own bindings, and startup maps them to its explicit files ("Command-line startup"): those name
  what they show. The entries of one fan-out file share its path and differ by label, so each (path, label) pair is a
  dataset of its own.
- **Shared topics decide whether to choose.** A topic of the layout is shared when two or more loaded datasets carry
  it. The layout's topics are what its plots, scenes, widgets, filters, transforms, on-demand recipes and dataset-bound
  marker rules read. A series key (`<topic>/<field>`: a marker rule's input, an on-demand input saved without a
  `topic`) reads the longest topic naming a prefix of it, of those any loaded dataset carries or the layout's recipes
  write. Its own outputs (filters, transforms, on-demand recipes as `<owner>/<id>/<output>`, marker rules) are not, so a dataset
  holding them from an earlier load of the same layout fits it no better.
- **No shared topic: nothing is chosen.** A generic layout is applied as it is: each reference binds to the one dataset
  carrying its topic. Reason: a layout over curves from several files (the browser's Save Layout writes every layout
  generic) names no dataset, and pinning it all to one file would lose the others' curves. A layout naming a dataset
  that is not loaded loses its qualifiers (`layout_xml::removeDatasetQualifiersForGenericLayout`) when a loaded
  dataset carries one of its topics, so each reference binds where its topic is unique; it keeps them otherwise, so
  the missing-data prompt names its dataset. Reason: a qualified curve never falls back to a unique topic
  (`CatalogModel::resolveCurveKey`), so the qualifiers of a dataset that is not there would leave every curve
  unresolved.
- **One dataset fitting best is pinned, unasked.** A dataset fits best when it carries more of the shared topics than
  any other, or as many and more of the layout's other topics: the shared topics decide first; the others then say
  which file the layout was made for (a layout plotting `sensor`, which two files carry, and `extra`, which one of them
  carries, was made for that one).
- **A pin covers what its dataset carries.** Pinning to a dataset (the best fit, the one checked, or each checked one
  in its own copy) qualifies every reference to a topic that dataset carries (all of its catalog topics, not only the
  shared ones), every reference to an output of a recipe reading one of those (through chains of recipes: a pinned
  recipe writes into its dataset), every marker rule all of whose key topics are among those, and every other
  nameless reader (marker scope, a transform input saved without a topic). Any other reference keeps no qualifier and
  binds where its topic is unique, so a curve over a topic only another file carries still shows it. Reason: a
  transform or marker rule reads every input from one dataset, so pinning only its shared inputs would split it, and a
  rule pinned to a dataset lacking one of its inputs would fail to restore.
- **Several fitting equally (as many shared topics, and as many others) are asked about, when the load can ask**
  (File → Load Layout, the recent list, the browser's Load Layout, a startup layout with someone at the display): one
  checkbox per candidate dataset, in load order, all checked (only the first when only one can be shown); Cancel settles the load false with nothing applied,
  like the reload prompt's Cancel. Reason: only the user knows which run a generic layout is for. Checked candidates
  removed while the question was open (the browser's does not block) are reported: the load goes on with the others,
  or fails when none is left, never taken for a Cancel.
- **A load that cannot ask takes the first loaded candidate and says so** (`layout-dataset-chosen`, a warning): an
  unattended `--layout` (`--exit-after-layout`, `--screenshot`, `--dump-diagnostics`, the offscreen QA instance), and
  any other load without a display to ask on. Reason: a harness, an agent or the QA instance cannot answer a dialog,
  and an unbound workspace helps no one; the diagnostic keeps the choice visible.
- **One checked: the layout is pinned to it,** and the tabs keep their names. Plots, scenes, recipes, marker rules,
  marker scopes and Custom Widget wires are all rewritten in the document by the same qualifier walk, so no widget
  needs its own logic.
- **Several checked: each tab is duplicated, one copy per dataset,** side by side, named `<tab> [<dataset label>]`,
  each copy pinned to its dataset. Reason: a Scene3D, Scene2D or Custom Widget view shows one dataset, so comparing
  runs needs one copy of the view per run. Each filter and dataset-bound marker rule is copied once for each checked
  dataset that covers it, pinned there, keeping its output name or rule id: both are per dataset (`BatchEngine` binds a rule
  by id and dataset). One no checked dataset covers stays single and unqualified.
- **Copies never collide.** A tab copy is the user's (`plot_xml::prepareTabCopy`, shared with Duplicate Tab: no
  plugin ownership) and takes tab, area and plot ids no element of the document has, so a duplicated layout saved and
  loaded again duplicates cleanly. Ids are renamed, not dropped: the pending-curve binder finds a copy's plots by id.
- **Every copy shows a curve in the same colour.** A saved colour is copied as is; a curve without one first gets the
  colour its plot would give it (`plot_xml::fillMissingCurveColors`: the session's remembered colour for it, else the
  plot's next free palette colour). Reason: the copies exist to compare runs, so the same signal must look the same in
  each. Scene layers take no session-assigned colour, so their copies match already.
- **The checked datasets start together.** Their starts are aligned as the Source Timeline's "Align starts" does,
  over the checked datasets only (`SourceTimelineController::alignStartsOf`), before the restore builds the plots:
  a plot keeps its view when an offset changes (a Timeline drag must not re-zoom), so aligning after the plots fit
  would leave each later run's copy showing its old window, empty. Reason: the copies are there to compare runs, so
  each starts at the same display time. One checked dataset keeps its offset.
- **Refused, not guessed, where a copy cannot work** (`layout_xml::duplicationBlockers`): a transform's id is also its
  script's class id and unique per plugin across datasets, so no second copy can exist; an on-demand recipe reads its
  inputs and upstream outputs by name. Such a layout lets one dataset be checked at a time, and the dialog names them.

### Streaming sources in a layout file

A source-bound save (desktop "Bind to this data source", browser "Save Layout with Sources") writes the live streams
through `liveStreamRecords`: one `<streamInfo>` per session with its starting configuration, offset and timeline slot,
and `pj-stream://<id>` on what is drawn from it. The configuration is the plugin's `saveConfig()` JSON verbatim, so
credentials a plugin keeps there land in the file (the save dialog says so while a stream runs). A session with no
configuration (no dialog, an agent's bare start) is left out, and what it draws saves unbound.

`loadLayoutFromPath` reads the records. A record whose plugin and configuration template match a running session
(`StreamingSourceManager::liveSessionFor`) binds to it, so reopening a layout never opens a second connection; of two
identical sessions, each record takes the one carrying its id. The others join the reload prompt (File → Load Layout)
and start at its commit point through `startFromLayout`, without the plugin's dialog; `--layout` starts them without
asking. `bindStreamQualifiers` turns what the layout bound to each record into pending intents on that session's
dataset; timeline tracks go through `resolveLayoutTimelineTracks`. Desktop and browser share this bookkeeping
(`LayoutStreamBinding`) and differ in failure policy: the browser's replay is all-or-nothing, the desktop's partial (a
stream that cannot start is named in one `layout-streams-not-started` report and the rest applies). Streams the load
started stay up when the user cancels the apply afterwards.

Frozen restore fixtures live in `tests/fixtures/layouts/` (each directory's `README.md` says how its files were
written); their suites restore through the real load paths and save back to the same `<data_processors>` block.

## Canonical layout import (cloud sessions)

A `.pj4.xml` layout can embed a durable source descriptor, so opening it re-creates a cloud session: from a local
cache artifact when present, otherwise by re-downloading through the MCAP Cloud connector plugin while plots grow. The
PJ4 pieces are `LayoutImportBatch`, `HeadlessDescriptorProviderSession`, `SourcePromotionHost`, the
rewrite-then-classify layout load path (`MainWindow` + `LayoutXml`) and the growing-import binder (`MainWindow` over
`SessionManager`'s ingest signals). `LayoutExitWatch` turns an `--exit-after-layout` run into one exit code.
`ToolboxHostWiring.h` holds the wiring shared by `MainWindow::launchToolbox` and `HeadlessDescriptorProviderSession`,
so a provider plugin sees the same host in both.

A host-cache hit loads as a standard MCAP, whatever plugin the layout recorded. A cache entry another instance is still
publishing fails that source (`layout-import-cache-contended`, no retry) rather than starting a second download. The
`prefer_reuse` and fan-out load paths emit no `loadCommitting`, so a hit loaded that way keeps a path-only identity (no
source record). A headless import's presentation (TF bridging, playback focus, Custom Topics) comes from `MainWindow`'s
batch-scoped observers (`installLayoutBatchObservers`); the progressive-restore drain settles the rest.

The feature spans three repositories; its reference is `docs/layout-import-architecture.md` in the `pj-mcap-server`
repository (component map, runtime flows, invariants I-1 to I-16). Several of those invariants are host-side
(delivery order, teardown orders, strict in-place promotion, strip displayed-owner arbitration) and cannot be derived
from PJ4 alone: read it before changing the pieces above. Operations (what a shared layout embeds, the trust
bootstrap, cache purge, the headless flow and its diagnostic ids): `docs/layout-sharing-runbook.md` there.

## Plugin playback and owned views

`MainWindow` wires `PlaybackControlHost`, `ViewportRuntimeHost` and `PlotTabsRuntimeHost` into each toolbox binding.
Playback is global and stays in `MainWindow`. The owned-tab operations live in `PluginPlotTabsController` (over
`TabbedPlotWidget`, `CatalogModel` and `ExtensionCatalogService`), which builds each plugin's callbacks with the
plugin id captured. `MainWindow` constructs it (with `makeSeededEmptyObjectDock` as the scene-widget factory and
`revealOwnedTabOverTakeover` as the created-tab hook, which folds the plugin's takeover panel so the new tab shows)
and calls `refreshAvailability()` on catalog change and layout load.

- **Scene tabs** ("3d"/"2d") share the plugin's id namespace; the controller routes by the kind of the tab's live
  widget (`sceneKindOf`) to `PluginSceneViewsController` (topic resolution, attach/detach, read-back JSON). A kind
  the factory cannot build is refused before an existing tab is replaced.
- **Embedded views.** The same delegate serves the scene views a toolbox panel embeds:
  `MainWindow::makeEmbeddedViewHost` builds seeded scene docks parented to the frame and attaches through
  `attachEmbeddedTopic`; tracker broadcasts tick them (`tickEmbeddedViews`) and a catalog change re-attaches what
  their frames still ask for (`reattachEmbeddedTopics`).
- **Addressing.** Viewport operations reach only dockers whose `PlotDocker::ownerPlugin()` is the captured id. A tab's
  `stateId()` is independent of its title and position.
- **Persistence.** Owned tabs persist with `owner_plugin`/`tab_id` metadata and stay live across history restores, as
  do their curves bound to a filter or transform output the snapshot leaves unchanged: the derived-recipe restore
  reconciles by persisted identity (filters by `output_name`, transforms by key). Changed entries, and kept ones
  cascaded out by a replaced input, are re-created in rounds (filters, inputs re-resolved through a rebuilt catalog,
  then transforms) until a round creates nothing; failures are reported once, after the last round.
- **Watermark.** The owning toolbox's badge (`toolboxBadge`: manifest `badge`, else its name); a tab whose owner is
  not in the catalog says so.

`displayTimeForSource` validates a live catalog source handle and uses its `SessionManager::displayOffset`. Topic
conversion requires a unique `CatalogModel::datasetForTopic` match; an empty topic selects the representative
dataset. Plugins should prefer the SDK's `toDisplayTimeForSource` when topic names repeat (the SDK's
`pj_plugins/docs/toolbox-guide.md`).

## Toolbox presentation

`ToolboxPresenter` is one live toolbox: the wrapped panel container (`MainWindow::wrapToolboxPanel`), its
`PanelEngine`, the plugin session that keeps host services and the plugin instance alive, and its presentation:
takeover (chart area), tab or floating `PJ::Dialog`. `MainWindow::toolboxes_` is the single registry, one presenter
per plugin id across every presentation.

**Launch.** `launchToolbox` builds the session (`buildToolboxPanelSession`; the derived-data hosts come from
`pj_runtime`'s `DerivedRuntimeHosts`, which `HeadlessDescriptorProviderSession` builds too, the other provider
services from `ToolboxHostWiring.h`), wraps the panel, registers the presenter and presents it.

**Relocation.** `moveToDock`/`moveToTab`/`moveToFloating` release the container and attach it elsewhere without
teardown: the session, drawer, actions and renamed tab title survive, and `Spec::apply_chrome` re-shows the banner in
the new chrome. Each presentation offers the two others plus close; generic panels hide destinations they do not
support.

**Teardown.** `ToolboxPresenter::teardown` is the one exit, reached only through `MainWindow::teardownToolbox`, which
extracts the registry entry first so a callback landing mid-teardown finds nothing. Tab close, window `finished`,
engine close requests and banner hooks look the presenter up by id rather than capturing it. The session dying with
the presenter ends the previews that panel's plugin created: `DerivedRuntimeHosts`' destructor passes them to
`SessionManager::removeEphemerals` after the plugin handle is gone. The headless provider session does the same for
its own previews, so ending it leaves an open panel's previews of the same plugin alone. Persistent recipes are user
data and stay.

**Takeover and close.** Nothing is registered per presentation: the busy-close fold, the fold on ingest start and a
dismissal's fold resolve the takeover's presenter through the registry (`takeoverToolbox()`), and the banner's close
is `ToolboxPresenter::requestClose()` (a tab goes through the strip's busy-job confirmation, a floating window through
the window, a busy takeover folds into a transient tab). Replacing a busy takeover folds it before showing the new
one. The docked panel is a page of the `centralAreaStack` (`MainWindow.ui`) that holds the chart, so docking never
fails and the chart never leaves the window.

**Reparenting.** A move reparents the container once, at its destination. Releasing it (`releaseCentralPanel`,
`ToolboxPresenter::release`, `TabbedPlotWidget::takeWidgetTab`) leaves its parent alone, and a teardown deletes it in
place: every reparent re-polishes the whole tree against the app stylesheet, which is slow in a browser
([TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md) "Reparenting a toolbox panel re-polishes it"). The chart-area helpers
shared by the banner and the floating title bar are in `ToolboxChrome.h`.

## Custom Topics panel

`CurveListPanel`'s bottom tree lists what the session derived: transform outputs (found by output topic id, one row per column, so a grouped
`"t:a,b"` output moves whole, its columns named `t/a`, `t/b`) and one row per object output of a
persistent on-demand recipe, named by the recipe, badged by an owner other than the editor, with its node's state as
glyph and tooltip.

- **Row set.** `MainWindow::syncCustomTopicsPanel` derives the whole set (`CurveListPanel::setCustomTopics`) on every
  install and after a restore or a toolbox write. A node state change or an offset schedules one
  `refreshCustomTopicsPanel` per event-loop turn, which rebuilds the on-demand rows and reuses the transform rows of
  the last sync until the catalog changes.
- **Editing.** "+", the pencil and Delete belong to the toolbox whose manifest declares `custom_topics_editor`.
  The pencil opens the editor on its own recipe's `params_json`. The `custom_topics_viewer` tag in that manifest
  (`ExtensionCatalogService::customTopicsEditorViews`, `CurveListPanel::setEditorViewsOthers`) gates the view: with it,
  the pencil stays enabled on rows another owner made (an MCP agent, another plugin, none) and opens the editor on
  `{"view": {owner, name, kind, language, script, inputs, outputs, params}}`, the recipe as it runs, which the editor
  shows read-only; `name` is what the row shows (a transform's output topic, an on-demand recipe's id). Without the
  tag the pencil stays disabled on those rows and the handler launches nothing. A view is not workspace state:
  `savePinnedToolboxes` skips a toolbox whose saved config is a JSON object with a top-level `view`, so a layout never
  holds a snapshot of a recipe its owner can change. Delete stays the owner's.
  `deleteCustomTopics` asks `pj_runtime`'s `CustomTopicsPolicy.h` who may delete what, and asks again once a prompt is
  answered (the session may have changed under the prompt); the shell only words the refusals and runs the prompts.
- **Series.** One row per number column (`id/output`), deleted with its recipe. `SeriesProgressController` (wired by
  `wireSeriesProgress`) turns `SeriesRunner::State` into a stoppable bar on those rows; a row's stop
  (`seriesStopRequested`) stops the replay and keeps its rows.

See the derived-objects architecture, "The on-demand pipeline".

## Custom Widget panels

`MainWindow::makeSceneDock` builds the `"CustomWidget"` kind (the `<CustomWidget>` layout tag) like the other object
docks: wired to the session, the diagnostic sink, the undo debounce and
`TopicDemandController::registerCustomWidgetDock`, and given the `pj.playback.v1` time mappers
(`displayTimeForTopic` / `displayTimeForSource`, the same two a toolbox session's playback host uses). The
object-widget factory is installed whether or not Scene2D/Scene3D are built, because this kind,
`state_transitions` and `log` do not depend on them. `MainWindow::createCustomWidgetDock(dock, plugin_id, type)`, which the
picker calls, binds the plugin widget before the dock mounts it (so the mount sees the plugin's widget tree), then
adopts it. `onObjectFamilyRequested(kCustomWidget)` creates nothing: the widget is chosen first, or an unbound panel
would be left behind. Object types with no built-in view (`isCustomWidgetObjectType` in
`scene_object_classification.h`) drag from the curve list like any displayable topic; `DockWidget` offers them to a
mounted Custom Widget (`tryAcceptObjectTopic`), and a drop on an empty dock gets a toast saying to create the Custom
Widget first.

`CustomWidgetController` (owned by `right_panel_stack_`) holds the rest: the config page in the right panel, the
picker menu, the pending target panel, the `ActiveWidgetsMarker` and the startup review. `MainWindow` calls
`showPicker`, `pageForFocusedDock`, `attachDock`, `reviewUncleanShutdown` and `clearActiveWidgets`; the controller
calls back through `Hooks`.

- `CustomWidgetPickerMenu` is a context menu, popped up under the empty panel's Custom Widget button. It lists the
  installed widget types from `ExtensionCatalogService::widgets()`, rebuilt every time it opens. A library built
  against another Qt is listed disabled with its mismatch reason. The button itself is disabled with nothing
  installed, so the menu never opens empty.
- `CustomWidgetConfigPanel` binds to the focused `CustomWidgetDockWidget`: one row per input with its wires and a
  chooser listing only the keys `CustomWidgetDockWidget::canBind` accepts ("Choose series" replaces slot 0, "Add
  series" on a `multiple` input appends at `WidgetBindings::kNewSlot`), then a settings form built from
  `settingsDeclarations()`. A change edits one key of `settingsJson()` (which holds every declared key with its
  manifest default) and calls `applySettings()` with the whole object; number edits are debounced. The form is built
  on bind only, so a dock signal never destroys a control in use; `inputsChanged` / `bindingsChanged` rebuild the
  inputs once, from the event loop, however many arrive together. A missing widget shows its reason and its stored
  wires, read-only.

Flow: the placeholder's Custom Widget button → `showPicker(dock)` keeps the empty dock in a `QPointer` and pops the
menu up under the button; a pick → `createCustomWidgetDock` and the new panel's config page. A dock closed or filled
in between is left alone.

**Crash loop breaker (desktop only).** `CustomWidgetController` owns this process's `ActiveWidgetsMarker`
(`active_widgets.<pid>.json` in the default directory) and hands it to every Custom Widget dock, which records its
plugin before the first call into it and removes it once the instance is destroyed. `~MainWindow` clears the file
right after the docks are destroyed, the one seam both a window close and the `--exit-after-layout` / `--screenshot`
quits pass through. `main.cpp` calls `reviewUncleanShutdown(interactive)` once, after the window is shown and before
the startup load is queued: it consumes the records of runs whose process is gone (a live instance keeps its own),
reports one `kWarning` (source `CustomWidget`) naming the widgets, and in an attended run asks Disable / Keep. Disable
calls `ExtensionManager::setEnabled(id, false)` per plugin and `ExtensionCatalogService::reload()`, so their panels
restore as placeholders. An unattended run only reports. An unreadable record is a warning and no question.

Tests: `main_window_custom_widget_test.cpp` (`UncleanShutdownTest` gives each case its own application name, because
Disable needs the extension store's single-writer lease), `custom_widget_panels_test.cpp`, and the executable's
end-to-end runs registered from `pj_custom_widgets/tests/E2E.cmake`.

## Log panels

`MainWindow::makeSceneDock` builds the `"log"` kind (`LogDockWidget` from `pj_log`, the `<log>` layout tag),
counted as `panel.log`, given the app-wide time format and wired to the undo debounce,
`TopicDemandController::registerLogDock` and the pending-binding rebuild. `panelKindForObjectType` routes `kLog` to
it, so a log topic dropped on an empty area opens a Log panel (the drop factory hands the topic over through
`IDataWidget::tryAcceptObjectTopic` and seeds every new widget at the playhead), and the placeholder's Log button
(`VisualizationKind::kLog`) creates an empty one. A log dropped on a mounted Log panel merges into it
(`tryAcceptObjectTopic`). An advertised log placeholder dropped on a Log panel stays pending in it
(`TopicDemandController::handleLogDockPlaceholderDrop`) while `PendingDisplayBinder::addPendingLogTopic` holds its
demand; a restored layout's unresolved topics are collected the same way. Deleting a topic or a dataset from the
Datasets panel calls `discardTopic` / `discardDataset` first, next to the scene docks' `discardPendingRestoresFor*`,
so the panel drops it instead of waiting for it. `applyTimeLabelFormat` reaches every panel
through `forEachLogPanel`. The curve list badges a log row with the Log glyph (`objectTypeBadgeIcon`).

## Browser (WebAssembly) boundary

On WebAssembly the shell owns the browser boundary: `BrowserFileStore` (upload staging and identity, bytes behind a
lifetime lease in MEMFS), `FileSelectionService` (asynchronous file pickers) and `BrowserPersistence` (durable
settings). It lives here, not in a library, because its lifetime and policy are the executable's.

### Lazy picks

A data-file pick (`selectAndStageFile`) opens its own `<input type=file>` and never copies the `File`:
`BrowserPickedFile` turns a `BrowserFileStore::reserve()`d path into a read-only MEMFS node that reads from the `File`
on demand (`browser_picked_file.js`), so a pick may exceed the wasm32 address space and costs no memory while open.
Small picks (layouts, robot files) still take Qt's `getOpenFileContent` bytes.

- **Reads.** With pthreads every file syscall runs on the browser main thread, which cannot await, so a read posts a
  `file.slice()` to a reader Worker (`FileReaderSync` into a shared buffer) and spins until it answers. A per-node
  read-ahead block absorbs small sequential reads; larger reads go straight through. A Worker the main thread spins
  on must already be running, so the Workers start at pick time and the pick is reported only once they answer.
- **Fingerprint.** `content_sha256` is plain SHA-256 (layouts and presets compare it), and WebCrypto cannot hash
  incrementally, so a hasher Worker runs an incremental JS SHA-256. A normal open
  (`FileLoader::loadFromBrowserPicker`) hashes in the background (`Fingerprint::kInBackground`) and "Save Layout with
  Sources" refuses to save while it runs; layout replay reselection and `DialogPresenter` picks wait for it
  (`kBeforeSelection`), because they compare it. A dialog pick is recorded in `FileLoader`
  (`rememberBrowserUpload`, by backing path), which is how the save names and fingerprints the side files a plugin
  configuration holds (`layout_xml::logicalBrowserSideFiles`; format in
  [`../../docs/WASM_DEPLOYMENT.md`](../../docs/WASM_DEPLOYMENT.md), "Browser-local state").
- **A file changed on disk after the pick.** The browser fails every read (`NotReadableError`, surfaced as `EIO`). A
  `pread`-based source gets the error; a `std::istream`-based one takes it for end of file. The first failed read of
  a node raises one `browser-file-read-failed` warning (the data may be incomplete) and drops the fingerprint, so
  "Save Layout with Sources" refuses it. A source with lazy object pulls reads its file all session, so this can
  happen long after the load.
- **Offsets past 2 GiB.** `long` is 32 bits on wasm32, so `ftell` / `fseek` fail on a larger pick; loaders use
  `ftello` / `fseeko` or `pread`.
- Not lazy: a drag-and-drop goes through Qt's drop handling, which copies into MEMFS; preset recordings download into
  MEMFS.

Tests: `tests/wasm/ingress_file_selection.spec.js` (a sparse pick larger than the address space; a file changed after
the pick) and the `content_sha256` checks of `persistence_layout.spec.js` (the JS hash against Node's).

### Persistence

Native builds keep plain `QSettings`. WASM redirects ordinary `QSettings` callers to page-lifetime MEMFS and lets
`BrowserPersistence` alone write `WebLocalStorageFormat`: three versioned JSON envelopes (one encode/decode pair,
each size-capped) holding the typed preference allowlist, a few bounded, source-free
generic layout recipes, and the plugin settings a plugin marked durable (`pj_runtime` ARCHITECTURE "Plugin
settings"), each re-checked on restore. File paths, upload identities, the host's `PluginConfig/` JSON of each
plugin, source-bound layouts, stream records and every plugin setting outside `durable/` never enter durable storage.
In a `durable/` setting the backstop refuses a URL password and a secret-like key name (`isSecretKey`); keeping other
secrets out of its `durable/` keys is the plugin's job.
A rejected stored envelope (malformed, oversized or another version) is left intact with a warning and none of its
values are imported. Allowlisted values written to the page-lifetime settings reach durable storage on
`BrowserPersistence`'s periodic flush ([why a poll](./TECHNICAL_NOTES.md#browser-preferences-flush-on-a-timer)).
`main()` constructs `BrowserPersistence` after the application identity and before the splash, `Theme`, `WidgetTuner`,
`MainWindow` or any plugin host, so every settings consumer sees the redirect; native builds never construct it.

### Presets

The preset UI is browser-only, but the replay path builds on the desktop too, for tests. `PresetController`
discovers `presets/index.json` and downloads a preset's files (http(s) only, capped by `kMaxPresetDownloadBytes` and,
for a recording, `kMaxPresetDataBytes`) through `pj_runtime`'s `fetchBytesWithLimit()`; a browser recording streams
through `BrowserFetch.h` ([why](../../pj_runtime/docs/TECHNICAL_NOTES.md)). `WelcomePresetsDialog` is a view only.

`MainWindow::loadLayoutFromUrl` runs the upload replay's byte-level path (`loadLayoutFromBytes`: imports, remapping,
rollback). Each `<fileInfo>` downloads from `layout_xml::resolveFileSourceUrl` into a `BrowserFileStore::reserve()`d
file and is checked against `content_sha256` and the preset's cumulative cap; Scene3D's `resolveLayoutResourceUrls`
anchors robot resources at the layout URL. In WASM a redirect leaves the final URL the requested one, so a preset that
relies on a redirect to another directory resolves its files against the wrong base. Every call ends in exactly one
`layoutUrlLoadFinished(url, ok, reason)`; each source download reports throttled `layoutUrlLoadProgress(url,
received, total)` ending at received == total. A success replaces only what the previous successful preset load
brought (`BrowserLayoutRuntime::preset_data`), after its layout applies. Unless a stream is followed, the playhead
moves to the start of the range before the layout applies, and a rollback puts it back: left where the previous
preset had it, every restored scene view would fold its marker history up to that time.

A preset may record streaming sources (`<previouslyLoaded_StreamingSources>`, `pj4_version="5"`, `pj-stream://<id>`
qualifiers). Its configuration is a template: `${deployment.ws_origin}` expands from the page URL through
`pj_runtime`'s `expandSourceConfigVariables`, the replay starts it via `StreamingSourceManager::startFromLayout`, and
a re-save writes it back unexpanded. Only a source-bound save writes stream records, never `xmlSaveState`. Authoring,
packaging and hosting: [`../../docs/WASM_PRESETS.md`](../../docs/WASM_PRESETS.md).

### Acceptance probes

`tests/wasm_acceptance_probes.inc` is the acceptance-build-only observation and input bridge, included behind
`PJ_WASM_ENABLE_INGRESS_PROBE` (with the plot readback probes, `MainWindow::installWasmPlotAcceptanceProbes()`); it is
never part of desktop or production WASM behavior. `tests/wasm_scene3d_acceptance_probes.cpp` is its Scene3D
counterpart. With `PJ_WASM_WITH_PYTHON` it also includes `tests/wasm_python_probe.inc` (asserted by the repo-root
`tests/wasm/python.spec.js`) and compiles `tests/wasm_python_stdlib_probe.cpp`.

## MCP server

AI agents read and drive a running PlotJuggler over the Model Context Protocol: external agents through the stdio
bridge `--mcp-bridge` with the public token, toolbox plugins through a private, agent-tools-only endpoint
(`pj.mcp_endpoint.v1`). The browser build cannot listen; it links out to an agent's relay (`McpBrowserLink`, `?mcp=`)
and has neither bridge nor plugin endpoint. The design (tool sets, tokens and owners, jobs, lifetimes and revocation,
discovery, the user-first rules, security, tests) is [`../../docs/MCP_SERVER.md`](../../docs/MCP_SERVER.md).

Here: `McpProtocol` (shared vocabulary, QtCore only), `McpBridge.cpp` (desktop), `McpServer` (with `McpEndpointHost`,
desktop), `McpBrowserLink`, `McpServerTools.cpp` (app tools over `MainWindow`'s public "agent control" block),
`McpWidgetState.cpp` (`widget_state`, `set_widget_state`, over the same block), `McpUiTools` (`--mcp-ui-tools`), `ui/McpJobsDialog`. The agent tools come from `pj_runtime`'s `pj_agent_tools`. The
shell's part of the teardown order: `~ToolboxPanelSession` resets `mcp_endpoint` after `handle` and before every
service, and `~MainWindow` destroys the server right after `teardownAllToolboxes`.

## Concrete codecs behind runtime interfaces

`pj_runtime` links no Scene2D library, so the shell supplies the codecs the runtime reaches through interfaces:
`SceneImageDecoder` (over Scene2D) implements `scripting::IImageDecoder`, which on-demand scripts decode through;
`SceneVideoFrameSource` (over Scene2D and FFmpeg) implements `scripting::IVideoFrameSource`, which their `frame_at`
reads. `MainWindow` hands them to `SessionManager::setImageDecoder` and `setVideoFrameSource`.

## Tests

`pj_app_gui_tests` links `pj_app_shell` and `pj_resources`; `pj_app_unit_tests` compiles the Qt-only helpers
directly; `pj_app_media_codec_tests` compiles the two codecs over `pj_scene2d_core` alone (run from the source root
for the tracked video fixture). The first two use `pj_add_test_runner` with the shared GUI main
(`cmake/PjTests.cmake`): each case runs in a fresh process, which is what keeps `MainWindow`'s process-global widget
state from leaking between cases, so run them through ctest. `tests/support/gui_test_env.cpp` registers the runner's
OpenGL-disable environment once; file-local environments check the selected suite before changing process-wide state.

A test answers the modal dialogs a flow opens with `pj_app_test::ModalAnswerer` (`tests/support/modal_answerer.h`),
never a timer that acts on whatever dialog is on top: each rule names the dialog it answers (a child, a button label),
and a dialog no rule matches is left open and listed. A non-blocking box (a refused file's "Load failed") can sit on top
until the next dialog opens over it, and a tick between the two found the box instead; the macOS runner hit that now and
then. `PJ_TEST_MODAL_POLL_MS=1` makes every answerer tick each millisecond, which lands between two dialogs opening back
to back almost every run: run a changed suite once that way. Two suites keep their own drivers: the live-only
layout-import e2e suite steps through one dialog several times (`driveWhenModal`), and a toolbox fold case closes
whatever dialog a failure would raise, so the case reports instead of hanging.

Suites with their own executable and ctest entry, and why:

- `rhi_dock_lifecycle_test`, `rhi_mixed_dock_test`: `QRhiWidget`s in ADS docks through float/redock, split, tab
  switch, sibling destruction, floating close and a DPR change, alone and mixed with the real plot, Scene3D and
  Scene2D widgets. They need a real display (skip on `offscreen`) and register per backend plus a DPR-2 pass
  (`RHI_BACKENDS DPR2` in `cmake/PjTests.cmake`). Their setup mirrors `main()`'s `AA_DontCreateNativeWidgetSiblings`;
  see `../../pj_scene3D/docs/TECHNICAL_NOTES.md` "ADS and window-system quirks". The mixed suite's widgets come from
  `tests/mixed_layout_fixture.h`, which `benchmarks/mixed_layout_benchmark.cpp` (`PJ_BUILD_DEMOS`) also uses.
- `main_window_layout_exit_test`: its last case leaves an import parked for destructor-time shutdown, so suite order
  and teardown are part of the test.
- `main_window_layout_import_*_test` (lifecycle, alive, cancel, binder, e2e): import supersession, cancellation,
  rollback and binding, each with its own mock-plugin path definition that must not reach other sources; the e2e
  suite sandboxes HOME/XDG before `QApplication` and skips unless its `MCAP_CLOUD_E2E_*` environment is set.
- `file_loader_test`, `source_promotion_host_test`, `headless_descriptor_provider_session_test`,
  `layout_import_batch_test`: per-suite plugin-path definitions over the shared `pj_app_loader_testlib` object library
  (the loader sources compile once).
- `mcp_bridge_test` (Linux): runs the built `plotjuggler4 --mcp` offscreen and drives `--mcp-bridge` over stdio in a
  private HOME/XDG sandbox. The in-process server cases are `McpServerTest` (GUI runner) and the gates
  `McpHttpParsing`/`McpEnvelope` (unit runner).
- `splash_meme_pool_test`: compiles `Splashscreen.cpp` standalone with `PJ_MEMES_SOURCE_DIR`.
- `ExportedSymbolsGuard`: a script test, enabled where `nm` can inspect the executable's exported symbols.

## Known gaps

- The title-bar ingest strip is wired but hidden (`kShowTitleBarIngestStrip`); its keep/discard prompt path is
  exercised only when the flag is turned on.
- `StreamingSourceManager` supports several sessions, but the left panel drives one active stream at a time.
- A stream started after Record is pressed is not recorded (taps attach at capture start).
- Streaming sources do not report through the load diagnostic sink.
- A stream's layout record stores the plugin's configuration verbatim, credentials included.
- The curve list does not mark pending items (an advertised placeholder before its first sample, an unresolved wire)
  or scene views embedded in a toolbox panel as displayed.
- Removing curves rebuilds the whole curve tree (`CurveListPanel::onCatalogItemsRemoved`); expansion and scroll are
  restored, the selection is not.
- A derived object skipped at a `kRetainAndDiagnose` load (`--layout`, a cloud import) is not recreated when its
  input arrives later (a late import, a stream), while a curve plotting its output stays a pending intent waiting for
  it. The kept entries (`kept_derived_`) hold what a retry would need; nothing retries them yet.
- In-band embedded assets are not routed to Scene3D docks on load.
- The Source Timeline's "Align starts", "Align ends" and "Reset" keep every plot's view, as a drag does, so a plot
  showing only a dataset they move is left on its old window, empty, until zoomed out (from the code, not
  reproduced).
- A layout naming several datasets is never asked about: when neither the saved paths nor startup's mapping bind its
  datasets, its references stay unresolved.
- A shared topic no chosen dataset carries keeps no qualifier and stays ambiguous (two runs, each recorded as two
  files, with one of each checked).
- A scene whose topics are split across files, over a shared one (`/tf`), pins the shared topic to the best fit and
  leaves its other topics unqualified, so its layers from another file stay pending: a scene shows one dataset. The
  fix belongs to the scene: `Scene3DDockWidget::representativeDatasetId` would also read robot-model and trail
  sources, or the scene would bind as one dataset.
- Marker rules have no view: a rule another owner made (an MCP agent's `create_markers`) is drawn and listed in the
  plot's marker status, but no UI shows its script. The Marker Editor's `loadConfig` (`code`, `source`) would show it;
  nothing opens it on a rule.
- The view of another owner's recipe is a snapshot taken at launch: a recipe changed or removed while the editor shows
  it is not reflected. It omits an on-demand recipe's `pinned_t` and query time, and its outputs' types.
- Opening a view replaces a live editor instance, an unsaved draft in it included, without asking, as the pencil and
  "+" already do.
- A series key resolves to a topic by name alone (the longest loaded topic naming a prefix of it), while marker rules
  bind by series (`CatalogModel::findScalarForMarkerKey`). When another loaded dataset has a topic literally named
  `sensor/value` and the chosen ones carry topic `sensor` with field `value`, a rule over `sensor/value` is left
  unpinned and finds several carriers. Deciding coverage per dataset with `findScalarForMarkerKey` would fix it.
- A layout holding a transform or an on-demand recipe shows one dataset at a time: duplicating a transform would need
  a recipe instance id apart from its script's class id (`DataProcessorService::TransformRecipe::user_id` is both).
- Restoring a layout whose `<pinned_toolboxes>` names a plugin that is missing or fails to bind goes through
  `launchToolbox`'s error path, which opens a modal `MessageBox::warning` even in an unattended run (suspected from
  the code, not reproduced).
