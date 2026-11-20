# pj_app

The executable shell: `MainWindow`, `main.cpp`, the app dialogs, the left panel and curve list, the file loader, and
the wiring between `pj_runtime`'s `AppSession` and the widget families. It also holds the concrete codecs the runtime
reaches only through interfaces (`SceneImageDecoder`, `SceneVideoFrameSource`), the MCP server, and on WebAssembly the
browser boundary (upload staging, file pickers, durable settings, presets). A class another Qt app could reuse goes
to `pj_widgets`; business logic and data services go to `pj_runtime`. How the subsystems fit together:
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- `~MainWindow` runs a fixed teardown order (recording, pending stream starts, toolboxes, MCP server, tabs, widgets,
  loader disconnect, `delete ui_`); moving a step later leaves a slot or plugin pointing at something already
  destroyed. The order and the reason for each step: ARCHITECTURE "Threads and teardown".
- `MainWindow::toolboxes_` is the single toolbox registry, and `MainWindow::teardownToolbox` the only exit: it
  extracts the entry first, and callbacks handed to Qt look the presenter up by id, so a callback landing mid-teardown
  finds nothing instead of a dying presenter.
- A toolbox panel is reparented once, at its destination; releasing it leaves its parent alone. Each reparent
  re-polishes the whole tree, which is slow in the browser.
- `ToolboxHostWiring.h` holds the wiring shared by `MainWindow::launchToolbox` and `HeadlessDescriptorProviderSession`;
  changing it in one owner only gives a provider plugin a different host when it runs headless.
- The Reload button is re-enabled only by `refreshReloadEnabled()`, from `FileLoader::loadFinished` and from a
  refused `loadFile`. `fileLoaded` / `fileLoadFailed` stay silent on a rejected config dialog and a queued cancel, so
  wiring the button to them leaves it disabled.
- Load progress is wired only through `wireIngestProgress()`; a hand-made copy drifts from what the tests exercise.
  The row's X/bin goes straight to `SessionManager::requestCancel(token, keep_partial)`; the producer honours the
  flag, the shell never prompts.
- Recording is a runtime capability: Record affordances follow `RecordingService::isSupported()`, never a build flag.
  A stream ending mid-capture finalizes only its own file (`RecordingService::onSourceEnded`); stopping the capture
  there would cut every other source's file short.
- `applyWorkspace` restores in order: read-only preflight, display offsets, processors, curve rebind. A processor
  restored before the offsets reads an unsettled display frame; a preflight after any mutation can no longer refuse
  cleanly. `syncCustomTopicsPanel` runs after every restore.
- The `<data_processors>` entries a layout load skipped (`kept_derived_`, exactly the elements
  `DerivedRestoreReport::skipped` names) are written back only by `saveLayoutToPath`; in `xmlSaveState` they would
  reach undo snapshots and `kExact` rollback targets, which then fail on the same missing input.
- Layout import (`LayoutImportBatch`, `HeadlessDescriptorProviderSession`, `SourcePromotionHost`, the
  rewrite-then-classify load path, the growing-import binder) implements host-side invariants defined in
  `docs/layout-import-architecture.md` of the `pj-mcap-server` repository; read it before changing them.
- Only `BrowserPersistence` writes durable browser storage, and only its preference allowlist and bounded source-free
  layout recipes. File paths, upload identities, plugin configuration, credentials, source-bound layouts and stream
  records never enter it: it outlives the page in clear text, where paths and upload identities mean nothing on the
  next load and credentials would be exposed. An unreadable stored envelope is left intact and not imported.
- A Preferences nav row's position is its stack page index: adding or hiding a row without its page shows the wrong
  page.
- A user-visible feature added, removed or moved updates `resources/feature_catalog.json` in the same change.
  `FeatureCatalogResourceTest` fails when an entry point stops naming the live labels or an `evidence` citation
  (`path#token`) stops appearing in its file, so editing a cited comment or string breaks it.
- Custom Topics deletion rights come from `pj_runtime`'s `CustomTopicsPolicy.h`, asked again after a prompt is
  answered; derived-recipe persistence stays in `pj_runtime`'s `DerivedObjects`. The shell only words refusals, runs
  prompts and owns what is UI.
- MCP: the server talks to `MainWindow` only through its public "agent control" block, never as a friend. Public
  owners are server-assigned (`mcp::ownerFor`, `agent-<client>`) and disjoint from plugin ids
  (`PJ::kAgentIdPrefix`); a plugin token acts as its plugin whatever the client claims. App work left for later asks
  `McpServer::stillAuthorized` before applying anything; every request gets exactly one response. Jobs, revocation
  and the user-first rules: [`../docs/MCP_SERVER.md`](../docs/MCP_SERVER.md).
- GUI tests: the shell leaks process-global widget state across `MainWindow` instances, so each `pj_app_gui_tests`
  case runs in its own process; run them through ctest, not the bare binary.

## Key files (`src/`)

- `MainWindow.{h,cpp,ui}`, `main.cpp`: the window and its wiring; startup, CLI flags, crash handler.
- `ui/`: embedded sub-widgets and app dialogs (`LeftPanel`, `CurveListPanel`, `TimelineWidget`, `DiagnosticsDialog`,
  the Scene2D/Scene3D config panels, the Custom Widget picker menu and config page, `McpJobsDialog`).
- `FileLoader`, `LoadInput.h`, `FanoutConfig`, `DialogPresenter`, `DialogWatch.h`, `ui/BatchLoadDialog`: file loading,
  plugin dialogs and the question asked once for a batch of files.
- `IngestProgressController`, `IngestProgressWiring.h`, `SeriesProgressController`: progress rows in the curve tree.
- `StreamingSourceManager`, `TopicDemandController`, `PendingDisplayBinder`: streaming sessions and topic demand.
- `LayoutXml`, `LayoutReplayHints.h`: layout files. `LayoutImportBatch`, `HeadlessDescriptorProviderSession`,
  `SourcePromotionHost`, `LayoutExitWatch`: the cloud layout import.
- `ToolboxPresenter`, `ToolboxChrome`, `ToolboxHostWiring.h`, `PluginPlotTabsController`,
  `PluginSceneViewsController`: toolbox presentation and plugin-owned views.
- `CustomWidgetController`: the Custom Widget picker menu, its config page and the startup crash review.
- `SourceTimelineController`, `DatasetMergeActions`: Source Timeline binding and dataset merge.
- `BrowserFileStore`, `FileSelectionService`, `BrowserPickedFile` (+ `browser_picked_file.js`), `BrowserPersistence`,
  `PresetController`, `WelcomePresetsDialog`: the WebAssembly boundary and presets.
- `mcp/`: `McpProtocol`, `McpBridge.cpp` (`--mcp-bridge`), `McpServer` (+ `McpEndpointHost`), `McpServerTools.cpp`,
  `McpUiTools` (`--mcp-ui-tools`), `McpBrowserLink`.
- `Theme`, `WidgetTuner`, `DebugMode`/`DebugUi`, `Splashscreen`, `DiagnosticDump`, `BuiltinPlugins`,
  `scene_object_classification`, `SceneImageDecoder`, `SceneVideoFrameSource`, `KeySequence`, `RasterKeyMap`: app
  services and helpers.

## Tests

`./test.sh -R '^pj_app_gui_tests\.'` (links `pj_app_shell` + `pj_resources`), `'^pj_app_unit_tests\.'` (Qt-only
helpers), `'^pj_app_media_codec_tests\.'` (the two codecs; run from the source root). Each case registers as
`<runner>.<Suite>.<Case>` and runs in its own process. The runner's shared GUI main
(`cmake/test_mains/pj_test_main_gui.cpp`) owns the only `QApplication` and gives each process its own application
name, so settings and data folders are never shared between tests. Suites with their own executable (QRhi display
passes, layout import, loader suites, MCP bridge) and why: ARCHITECTURE "Tests". Test binaries that touch shell
icons link `pj_resources`, an OBJECT library, because the shell makes no `Q_INIT_RESOURCE` calls. `tests/wasm_*probe*`
are acceptance-build-only browser probes. `benchmarks/` holds the mixed-layout benchmark (`PJ_BUILD_DEMOS`).

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): threads and teardown, startup, file loading and progress, streaming,
  recording, layouts, cloud import, owned views, toolbox presentation, Custom Topics, Custom Widgets, Log panels,
  browser boundary and presets, MCP, tests, known gaps.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): the measurements and incidents behind browser, linking, export
  and packaging choices.
- Cross-module: [derived objects](../docs/derived_objects/ARCHITECTURE.md),
  [recording](../docs/RECORDING_AND_CACHING.md), [MCP server](../docs/MCP_SERVER.md),
  [browser presets](../docs/WASM_PRESETS.md), [browser deployment](../docs/WASM_DEPLOYMENT.md).
