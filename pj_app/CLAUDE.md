# pj_app

## Purpose

The executable shell. Owns `MainWindow`, the application menus/toolbars/status bar, the `AppSession` (from `pj_runtime`), and the wiring between runtime services and concrete widgets (`pj_plotting`, `pj_scene2D/widgets`, …). On WebAssembly it also owns the browser boundary: upload staging/identity, asynchronous file selection, the deliberately narrow durable-settings bridge, and source-aware layout replay.

## What belongs here

- `MainWindow` and its `.ui` file.
- App-level dialogs (`PreferencesDialog`, `DiagnosticsDialog`, etc.) and their navigation rows.
- The shell's left panel, curve list, file loader, theme manager, title bar.
- Glue code that constructs an `AppSession`, then wires it to docked widgets.
- Browser-only host integration (`BrowserFileStore`, `FileSelectionService`,
  `BrowserPersistence`) whose lifetime and policy are tied to the executable.
- `main.cpp`.

## What does NOT belong here

- **Reusable Qt controls** → `pj_widgets`.
- **Business logic / data services** → `pj_runtime`.
- **Plotting / 2D / 3D widgets** → the owning widget-family module.
- **Plugin dialog runtime** → `pj_dialog_host`.

If you're tempted to add a class here that could be reused by another Qt app, move it to `pj_widgets` instead.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files** over programmatic widget construction. `pj_app/CMakeLists.txt` uses `AUTOUIC`. Drop to hand-written `QWidget` subclasses only for genuinely dynamic construction (plugin-driven widgets) or when explicitly requested.

## Layout

- `src/` — top-level shell sources and root `.ui` files (`MainWindow.ui`, `PreferencesDialog.ui`, `TitleBar.ui`).
- `src/ui/` — embedded shell sub-widgets: left-panel widgets (`CurveListPanel`, `LeftPanel`), the bell's `DiagnosticsDialog`, the bottom timeline strip (`TimelineWidget`), and the right-sidepanel scene config panels (`Scene2DConfigPanel`, `Scene3DConfigPanel`).
- `tests/wasm_acceptance_probes.inc` — acceptance-build-only observation and
  input bridge, included behind `PJ_WASM_ENABLE_INGRESS_PROBE`; it is never part
  of desktop or production WASM behavior.

`pj_app` has no `docs/` folder by design — the shell's intent is "wire the services to the widgets," and the wiring is best read directly from `MainWindow.cpp` and `main.cpp`.

## Tests

`pj_app_gui_tests` links `pj_app_shell` and `pj_resources` for GUI scenarios and
runtime-backed controller/binder tests. `pj_app_unit_tests` compiles the Qt-only
helpers directly. Both use `pj_add_test_runner` with the shared GUI main;
`gtest_discover_tests` registers each case under its runner's name and launches
it in a fresh process with a 120-second timeout. Run them through ctest to keep
MainWindow's process-global widget state isolated between cases.

`tests/support/gui_test_env.cpp` registers the shell runner's OpenGL-disable
environment once. File-local environments preserve custom settings identities
and slider theme ownership; they check the selected suite before changing
process-wide state. The shared main owns the only QApplication.

These tests retain their standalone executable and ctest entry:

- `main_window_layout_exit_test`: the last case leaves an import parked for
  destructor-time shutdown, so suite ordering and teardown are part of the test.
- `main_window_layout_import_lifecycle_test`: ordered import supersession,
  cancellation and rollback scenarios, with its own mock-plugin definition.
- `main_window_layout_import_alive_test`: preserves the import lifetime through
  the catalog-empty guard and completion drain.
- `main_window_layout_import_cancel_test`: preserves mid-import cancellation
  and restoration of the prior workspace.
- `main_window_layout_import_binder_test`: its mock-plugin path definition is
  specific to this suite.
- `main_window_layout_import_e2e_test`: installs a private HOME/XDG sandbox
  before QApplication and uses the live cross-repository E2E harness.
- `file_loader_test`: four plugin-path definitions plus child-process shutdown
  and teardown probes in `file_loader_shutdown_progress_test.cpp`.
- `source_promotion_host_test`: only the mock file-source plugin path is defined.
- `headless_descriptor_provider_session_test`: uses a static fake provider and
  no plugin-path definitions; also covers provider quiescence on teardown.
- `layout_import_batch_test`: desktop-only, with both mock file-source and
  runtime host object-parser plugin paths.
- `splash_meme_pool_test` (`SplashMemePoolTest`): compiles `Splashscreen.cpp`
  standalone with `PJ_MEMES_SOURCE_DIR` and the shipped resources.

`ExportedSymbolsGuard` remains a script test, enabled when the platform and
`nm` support inspecting the executable's exported symbols.

## Session recording

Nothing here is gated at build time: the `RecordingService` member, its connections and the shutdown `stop()`
calls are unconditional, and the capability is a runtime query. The streaming strip's `buttonStreamingRecord` and
`labelStreamingRecordStatus` stay hidden until `LeftPanel::setRecordingSupported(...)`, which only `MainWindow`
calls and only with `RecordingService::isSupported()`; the Preferences Recording nav row and page wiring are added
on the same query (the page itself always exists in the `.ui`, and a nav row's position IS its page index).
`StreamingSourceManager::recordingTargets()` is the D1 eligibility seam: it hands back only sessions whose
plugin advertises delegated ingest (a direct writer has no raw bytes at the seam) plus the display names it
excluded, for the shell to say what will be missing. Each target is keyed by its `DatasetId` — the identity the
service addresses that source by afterwards — and its `attach` reports `false` when the session ended before the
tap could be installed, which fails (and rolls back) the whole capture. Display names are display names: they
may repeat, because every source writes its own file and `RecordingService` names those files.

One Record press records every eligible source into one batch folder, one file each, so a stream that ends
mid-capture must NOT stop the capture: the `streamStopped` handler forwards
`RecordingService::onSourceEnded(dataset_id, reason)`, which finalizes that one file and leaves the rest
recording. The batch ends with its last live source, or when the user presses Stop.

`MainWindow::onRecordingStopped` reports a whole capture rather than a file: the path when a single source
recorded, the folder when several did and every outcome is `clean`, and otherwise the problem count plus up to
two failing sources with their reasons (naming `mcap recover` when a file could not be finalized). It appends
the capture's dropped count when the write queues had to shed messages, since a complete-looking file with holes
says nothing about them itself. Per-source detail goes to the log, never into the toast — a toast listing one
line per file stops being readable at three sources.

## Canonical layout import (cloud sessions)

A `.pj4.xml` layout can embed a durable source descriptor, so opening it
re-creates an exact cloud session — instantly from a local cache artifact when
present, otherwise by re-downloading through the MCAP Cloud connector plugin
while plots grow. The PJ4-side pieces are `LayoutImportBatch`,
`HeadlessDescriptorProviderSession`, `SourcePromotionHost`, the
rewrite-then-classify layout load path (`MainWindow` + `LayoutXml`), and the
growing-import binder (`MainWindow` over `SessionManager`'s ingest signals).

The feature spans three repos, so its as-built reference lives outside this one:
**`docs/layout-import-architecture.md`** in the `pj-mcap-server` repository — cross-repo component map, runtime flows,
and the invariant list I-1…I-16. **Read it before changing any of the components
above**: several invariants are host-side (delivery order, teardown orders,
strict in-place promotion, strip displayed-owner arbitration) and are not
derivable from PJ4 alone. The operations guide — what a shared layout embeds,
the trust bootstrap, cache purge, the headless flow and its diagnostic ids — is
`docs/layout-sharing-runbook.md` in that same repository.

## Per-dataset load progress (curve tree)

`wireIngestProgress()` (`IngestProgressWiring.{h,cpp}`) is the single definition of the connections between the three pieces below — controller attach, dataset-path resolver, `progressUpdated`/`rowKeyChanged` into the panel, and the cancel route back with its token-resolution, staleness and cancellability checks. `MainWindow` is its caller, so tests and tooling that wire through it exercise the app's real connections instead of a copy.

The row itself carries the keep-or-discard choice (✕ keeps what arrived, bin discards it), so the shell acts on the `keep_partial` flag directly and does **not** prompt: `requestCancel(token, keep_partial)` and nothing else. Honouring the flag is the **producer's** job — `FileLoader` latches it into `cancel_mode_` (1 = keep, 2 = discard) for a whole-load stop, or into a slot owned by that fan-out entry's own loop iteration for a row stop, so a row can only ever stop the entry the user aimed at. Keep commits this load's partial data (on a reload that means the partial refill replaces the prior data). Discard means **as if this load had never run**, so what it leaves behind is whatever preceded it: a first-load discard real-deletes the dataset it created, while a **reload-discard rolls back** to the pre-reload data (refill-guard rollback + `TransformService::abortReplacingLoad`, which with the ingest tap active is a staging pointer reset and reads nothing back from the store); Toolbox imports forward the plugin's own `cancellable` declaration and register as `IngestStop::kStopOnly` (their ABI has no rollback), so their rows offer a live ✕ with the bin drawn greyed and inert; `HeadlessDescriptorProviderSession` registers `cancellable=false` on purpose (no UI to stop from). The three-button keep/discard prompt survives only on the title-bar strip path, whose single stop button cannot express the choice.

`IngestProgressController` is the policy layer wiring `SessionManager`'s dataset-scoped ingest lifecycle signals (`ingestBegan` / `ingestProgressed` / `ingestEnded`) to `CurveTreeView`'s plain-typed progress decoration (`setDatasetProgress`). The view-facing row key IS the ingest id (`IngestToken::id`), so a click on a row resolves back to its ingest by lookup and the two identities cannot drift apart. The controller owns the row label (the DATASET's name once resolvable, kept sticky so a discard that deletes the dataset cannot rename the row mid-flight; the producer's progress title — "Importing MCAP" — is only the pre-resolution fallback), the 1-second completion-state linger, the 3x ~180ms error flash, immediate retirement on cancellation (no linger: the user stopped it, and a discard has deleted the dataset the row described), ghost-row lifetime (transient progress-only rows for failed/discarded loads whose catalog entries vanished), and an injectable timer factory for headless tests. Both `FileLoader` and `HeadlessDescriptorProviderSession` produce ingest tokens on the same channel. The title-bar `IngestProgressWidget` strip is retained and wired, but suppressed behind a single `constexpr kShowTitleBarIngestStrip = false` at its show seam — re-enabling the title bar is one-line.

## Browser persistence boundary

Native builds retain their normal `QSettings` behavior. WASM redirects ordinary
`QSettings` callers to page-lifetime MEMFS and lets `BrowserPersistence` alone
write `WebLocalStorageFormat`. Only its exact, typed preference allowlist and up
to five bounded, source-free generic layout recipes are durable. File paths,
upload identities, plugin configuration, credentials, and source-bound layouts
must never enter durable browser storage. If the active preference envelope is
unreadable, this version leaves it intact rather than deleting data that a newer
version may understand; it never imports rejected values into the session.

## Plugin playback and owned views

`MainWindow` wires `PlaybackControlHost`, `ViewportRuntimeHost`, and
`PlotTabsRuntimeHost` into each toolbox binding. Playback is global. Viewport
operations reach only the dockers whose `PlotDocker::ownerPlugin()` is the
captured plugin ID. Tab `stateId()` is independent of its title and position;
duplicate visible titles and renaming do not affect addressing. Owned tabs
persist in full layouts with `owner_plugin`/`tab_id` metadata and remain live
across history restores. Known gap: an owned tab's curve bound to a per-curve
filter output does not survive an unrelated undo, because a history restore
clears and replays every filter (transforms are reconciled by recipe and keep
their output identity). The watermark is the owning toolbox's display name
(`toolboxBadge`); a tab whose owner is not in the catalog says so instead.

`displayTimeForSource` validates a live catalog source handle and uses its current
`SessionManager::displayOffset`. Topic conversion first requires a unique
`CatalogModel::datasetForTopic` match; empty topic selects the representative
dataset. Prefer the SDK's `toDisplayTimeForSource` when topic names repeat. Its
optional tail slot preserves existing callers. See the SDK's
`pj_plugins/docs/toolbox-guide.md` for plugin examples and error handling.

## Toolbox presentation

Toolboxes keep three header controls in every presentation: docked panels offer
move-to-tab, move-to-floating and close; tabs offer move-to-docked,
move-to-floating and close; floating windows offer move-to-tab, move-to-docked
and close. Generic panels hide destinations they do not support.

Relocation moves the same live container without teardown. The plugin session,
drawer, actions and renamed tab title survive. Returning to docked restores
engine close routing and the takeover's busy-close/automatic-fold registration.
Replacing another busy takeover folds that panel before showing the incoming
one. If docking cannot claim the chart slot, the live panel remains available
as a tab. Tab header close uses the tab's busy-job confirmation.
