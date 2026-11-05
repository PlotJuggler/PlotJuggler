# pj_app

## Purpose

The executable shell. Owns `MainWindow`, the application menus/toolbars/status bar, the `AppSession` (from `pj_runtime`), and the wiring between runtime services and concrete widgets (`pj_plotting`, `pj_scene2D/widgets`, `pj_custom_widgets`, ...). On WebAssembly it also owns the browser boundary: upload staging/identity, asynchronous file selection, the deliberately narrow durable-settings bridge, source-aware layout replay, and the browser sample presets.

Host code stays domain-neutral (root `CLAUDE.md`): no plugin-specific terms or per-plugin policy here; the plugin supplies the domain meaning.

How the subsystems fit together (window frame, file loading and progress, streaming, recording, layouts, owned views, toolbox presentation, Custom Topics, browser persistence, test suites): [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md). The derived-data design the shell wires up: [../docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md).

## What belongs here

- `MainWindow` and its `.ui` file; `main.cpp`.
- App-level dialogs (`PreferencesDialog`, `DiagnosticsDialog`, ...) and their navigation rows.
- The shell's left panel, curve list, file loader, theme manager, title bar.
- Glue code that constructs an `AppSession`, then wires it to docked widgets.
- Concrete codecs the runtime reaches only through an interface (`SceneImageDecoder`, `SceneVideoFrameSource`), so `pj_runtime` links no Scene2D library.
- Browser-only host integration (`BrowserFileStore`, `FileSelectionService`, `BrowserPersistence`, `PresetController`, `WelcomePresetsDialog`) whose lifetime and policy are tied to the executable.

## What does NOT belong here

- **Reusable Qt controls** -> `pj_widgets`.
- **Business logic / data services** -> `pj_runtime`.
- **Plotting / 2D / 3D widgets** -> the owning widget-family module.
- **Plugin dialog runtime** -> `pj_dialog_host`.

If you're tempted to add a class here that could be reused by another Qt app, move it to `pj_widgets` instead.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files** over programmatic widget construction. `pj_app/CMakeLists.txt` uses `AUTOUIC`. Drop to hand-written `QWidget` subclasses only for genuinely dynamic construction (plugin-driven widgets) or when explicitly requested.

## Invariants and pitfalls

- **Toolbox lifetime.** `MainWindow::toolboxes_` is the single registry (one `ToolboxPresenter` per plugin id). The only exit is `MainWindow::teardownToolbox`, which extracts the registry entry first; callbacks handed to Qt look the presenter up by id and never capture it. `~MainWindow` tears every toolbox down synchronously before its members die. Relocating a panel reparents it once, at the destination; releasing leaves its parent alone (every reparent re-polishes the whole tree).
- **`ToolboxHostWiring.h`** holds wiring that must stay identical between `MainWindow::launchToolbox` and `HeadlessDescriptorProviderSession`; change it there, never in one owner.
- **Load progress.** Wire it through `wireIngestProgress()`, never by hand. The row's X/bin choice goes straight to `SessionManager::requestCancel(token, keep_partial)`: the shell does not prompt; honouring `keep_partial` is the producer's job.
- **Recording is a runtime capability, not a build flag.** Record affordances follow `RecordingService::isSupported()` (only `MainWindow` calls `LeftPanel::setRecordingSupported`). A stream ending mid-capture must NOT stop the capture (`RecordingService::onSourceEnded`). Per-source detail goes to the log, never into the stop toast.
- **CLI startup.** `MainWindow::loadAtStartup` waits for all explicit file tickets before layout restore. Explicit files make the layout generic; unattended loads forbid dialogs. See `docs/ARCHITECTURE.md` "Command-line startup".
- **Layout restore order.** `applyWorkspace`: preflight (`checkDataProcessorDependencies`, read-only) -> display offsets -> processors -> curve rebind. Diagnostics are reported under source "Layout". `syncCustomTopicsPanel` runs after every restore.
- **Layout import (cloud sessions).** Before changing `LayoutImportBatch`, `HeadlessDescriptorProviderSession`, `SourcePromotionHost`, the rewrite-then-classify load path or the growing-import binder, read `docs/layout-import-architecture.md` in the `pj-mcap-server` repository: several invariants (I-1 to I-16: delivery order, teardown orders, strict in-place promotion, strip displayed-owner arbitration) are host-side and not derivable from PJ4 alone. The operations guide is `docs/layout-sharing-runbook.md` in that repository.
- **Browser storage.** Only `BrowserPersistence` writes durable browser storage, and only its typed preference allowlist plus bounded source-free layout recipes. File paths, upload identities, plugin configuration, credentials, source-bound layouts and stream records (`pj-stream://` identities) must never enter it; an unreadable stored envelope is left intact, never deleted or imported.
- **Browser presets.** The UI is browser-only (`PJ_TARGET_WASM`), but the replay path builds on the desktop too, for tests. `PresetController` downloads a preset's files (http(s) only, capped at `kMaxPresetDownloadBytes`, a recording at `kMaxPresetDataBytes`; through `pj_runtime`'s `fetchBytesWithLimit()`; a browser recording streams through `BrowserFetch.h`) and discovers `presets/index.json`; `WelcomePresetsDialog` is a view only. `MainWindow::loadLayoutFromUrl` runs the upload replay's byte-level path (`loadLayoutFromBytes`: its imports, remapping and rollback): each `<fileInfo>` downloads from `layout_xml::resolveFileSourceUrl`, is written into a `BrowserFileStore::reserve()`d file and checked against `content_sha256` and the preset's cumulative cap, and Scene3D's `resolveLayoutResourceUrls` anchors robot resources at the layout URL. In WASM a redirect leaves the final URL the requested one, so a preset must not rely on a redirect to another directory. Every call ends in exactly one `layoutUrlLoadFinished(url, ok, reason)`, and each source download reports `layoutUrlLoadProgress(url, received, total)` (throttled, ending at received == total), which the dialog shows as a determinate bar; a success replaces only what the previous successful preset load brought (`BrowserLayoutRuntime::preset_data`). A layout may record streaming sources (`<previouslyLoaded_StreamingSources>`, `pj4_version="5"`, `pj-stream://<id>` qualifiers): the configuration is a template (`${deployment.ws_origin}` expands from the page URL through `pj_runtime`'s `expandSourceConfigVariables`), the replay starts it via `StreamingSourceManager::startFromLayout` with no dialog, and a re-save writes it back unexpanded; only "Save Layout with Sources" writes the records, never `xmlSaveState`. Authoring contract, packaging and hosting: [`../docs/WASM_PRESETS.md`](../docs/WASM_PRESETS.md).
- **Preferences.** A nav row's position IS its stack page index: add or hide a row and its page together.
- **Feature catalog.** A PR that adds, removes or moves a user-visible feature updates `resources/feature_catalog.json` in the same PR; `FeatureCatalogResourceTest` (`tests/feature_catalog_test.cpp`) fails when an entry point stops naming the live labels or an `evidence` citation (`path#token`) stops appearing in its file. `MainWindow` builds `pj_runtime`'s `FeatureCatalogHost` from its live services and hands it the resource bytes (`featureCatalogResource()`, since `pj_runtime` links no resources).
- **Custom Topics.** `pj_runtime`'s `CustomTopicsPolicy.h` decides who may delete what (asked again after a prompt is answered); this shell only words refusals and runs prompts.
- **Derived-recipe persistence** stays in `pj_runtime`'s `DerivedObjects`; `MainWindow` keeps thin wrappers and owns only what is UI.
- **GUI tests: one `MainWindow` per process.** The shell leaks process-global widget state across instances, so `pj_app_gui_tests` cases run one per process under ctest (run them through ctest, not the bare binary), and standalone suites keep a single shared window that each scenario cleans up after.
- **Test binaries that touch shell icons link `pj_resources`**, an OBJECT library built from `resources.qrc`, so its objects join the binary directly; the shell makes no `Q_INIT_RESOURCE` calls.

## Layout

- `src/MainWindow.{h,cpp,ui}`, `src/main.cpp`: the window and its wiring; startup, CLI flags, crash handler.
- `src/*.ui`: `MainWindow`, `PreferencesDialog`, `TitleBar`, `ToolboxBanner`. `src/ui/`: embedded sub-widgets (`LeftPanel`, `CurveListPanel`, `TimelineWidget`, `DiagnosticsDialog`, `AboutDialog`, `DatasetInfoDialog`, `Scene2DConfigPanel`, `Scene3DConfigPanel`, and the Custom Widget pages `CustomWidgetPickerPanel`, `CustomWidgetConfigPanel`).
- `FileLoader`, `LoadInput.h`, `FanoutConfig`, `DialogPresenter`: file loading. `IngestProgress*`, `SeriesProgressController`: progress rows.
- `StreamingSourceManager`, `TopicDemandController`, `PendingDisplayBinder`: streaming sources and demand.
- `LayoutXml`, `LayoutReplayHints.h`, `LayoutImportBatch`, `HeadlessDescriptorProviderSession`, `SourcePromotionHost`, `LayoutExitWatch`: layout files and the cloud import.
- `CustomWidgetController`: the Custom Widget pages, the pending target panel, the `ActiveWidgetsMarker` and the startup crash review ([docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md) "Custom Widget panels").
- `ToolboxPresenter`, `ToolboxChrome`, `ToolboxHostWiring.h`, `PluginPlotTabsController`, `PluginSceneViewsController`: toolbox presentation and plugin-owned views.
- `SourceTimelineController`, `DatasetMergeActions`: Source Timeline binding and dataset merge.
- `BrowserFileStore`, `FileSelectionService`, `BrowserPersistence`: WebAssembly boundary. `PresetController`, `WelcomePresetsDialog{.h,.cpp,.ui}`: browser presets (tests: `PresetControllerTest`, `WelcomePresetsDialogTest`, `RemotePresetReplayTest`, `StreamingPresetTest` with `tests/mock_layout_stream_plugin.cpp`).
- `Theme`, `WidgetTuner`, `DebugMode`/`DebugUi`, `Splashscreen`, `DiagnosticDump`, `BuiltinPlugins`, `scene_object_classification`, `SceneImageDecoder`, `SceneVideoFrameSource`, `KeySequence`, `RasterKeyMap`: app services and helpers.
- `tests/`, `tests/support/`, `tests/fixtures/layouts/`, `benchmarks/`: tests, shared fixtures, frozen layouts, the mixed-layout benchmark. `tests/wasm_*probe*`: acceptance-build-only WebAssembly probes, never part of desktop or production behavior (`docs/ARCHITECTURE.md` "Acceptance probes").

## Tests

- Runners (`pj_add_test_runner`): `pj_app_gui_tests` (links `pj_app_shell` + `pj_resources`), `pj_app_unit_tests` (Qt-only helpers), `pj_app_media_codec_tests` (the two codecs over `pj_scene2d_core`; run from the source root). Each case registers as `<runner>.<Suite>.<Case>` and runs in its own process (120 s timeout): `ctest -R '^pj_app_gui_tests\.' --test-dir build`.
- Suites with their own ctest entry (QRhi display passes, layout-import suites, `file_loader_test`, `source_promotion_host_test`, `headless_descriptor_provider_session_test`, `layout_import_batch_test`, `splash_meme_pool_test`, `ExportedSymbolsGuard`): the reasons and what each guards are in `docs/ARCHITECTURE.md` "Tests". Their per-suite plugin-path definitions must not propagate to other sources.
- A runner's shared GUI main owns its only `QApplication`; its cases do not define a `main()` or create another application.
