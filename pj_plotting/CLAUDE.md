# pj_plotting

## Purpose

Qwt-based plotting module — the PJ4 home for time-series plots, dockers, zoomers, trackers, legends, and the curve editor. This is the **largest PJ3 wholesale lift** in the repo. It also owns the plot-area **State Transitions** family (discrete-series state strips — string / integer / bool fields, not Qwt): the `StateSeriesAdapter` reader in `core/` and the `StateTransitionsController`/`StateTransitionsDockWidget` binding in `widget/`, over the reusable `StateTransitionsView` from `pj_widgets`.

## Module layout

Two targets with a strict direction:

```
pj_plotting  ──►  pj_plotting_core  ──►  pj_datastore + pj_base
                       │
                       └──►  Qwt (external, never patched — see 3rdparty/qwt/README.PJ4.md)
```

| Subdir | Contents | Role |
|---|---|---|
| `core/` | `DatastoreCurveAdapter`, `FilteredCurveAdapter`, `PointSeriesXY`, `StateSeriesAdapter`, `PlotXml.h` | Bridges PJ3-style `QwtSeriesData<QPointF>` consumers to `pj_datastore::DataReader`. `StateSeriesAdapter` is the discrete-series counterpart: a full-rebuild RLE reader (`rangeQuery` + per-type label formatting — `readString` copied out inside the cursor, integers in decimal via the exact int64/uint64 reads, bool via `readBool` as "true"/"false"; equal-timestamp last-write-wins, null = gap, trailing run open) feeding the State Transitions strip. `FilteredCurveAdapter` is a lazy `DatastoreCurveAdapter` subclass that serves the output of a `proc::DataProcessor` (the Filter Editor before/after preview, refreshed on the streaming commit path). `PlotXml.h` is the shared plot-XML vocabulary (`x_basis` range markers, the `pending_intent` keep-alive predicate, the `<marker_scope scope=…>` attribute + its two persisted values; the value↔`MarkerScope` mapping is `PlotWidget.cpp`'s own) consumed by both this module and the app-side layout passes. No Qt Widgets. |
| `widget/` | `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, `PlotZoomer`, `PlotPanner`, `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotFocusOverlay`, `PlotScaleDraw`, `DockWidget`, `DockToolbar`, `CurveEditor`, `FilterEditorPanel`, `ParameterForm`, `StateTransitionsController`, `StateTransitionsDockWidget`, `PlotRhiCanvas` | The Qt/Qwt widgets, ported from PJ3's `plotjuggler_app/`. `TabbedPlotWidget::forEachDocker`/`findDocker` enumerate the PlotDocker tabs in one pass over the tab vector — the shell and `pj_app`'s `PluginPlotTabsController` use them instead of index loops over `dockerCount()`/`dockerAt()` (each a linear scan). Wherever `PJ_RHI_WIDGETS` is defined (the desktop QRhi build, `PJ_SCENE3D_RHI=ON`, and WebAssembly) `widget/` also compiles `PlotRhiCanvas` — a QRhi canvas (any backend: OpenGL, Vulkan, D3D11/12, Metal, WebGL) that renders the visible Qwt curves, grid and markers as bounded triangle geometry and rasterises everything else (legend, marker labels, `PlotMarkersItem`, any unknown `QwtPlotItem`) with the item's own QPainter drawing into one texture atlas; `PlotWidgetBase` picks it in place of `QwtPlotOpenGLCanvas` (Qwt still owns axes/data/interaction). On desktop the `Preferences::use_opengl` preference (shown as "GPU canvas") and `--disable-opengl` select the raster `QwtPlotCanvas` instead; the browser always uses the RHI canvas. The GL build (`PJ_SCENE3D_RHI=OFF`) is unchanged: `QwtPlotOpenGLCanvas`, raster fallback. See "PlotRhiCanvas" below. `PlotMarkersItem` is the marker overlay: for every `(dataset, topic)` target `PlotWidget::markerTargets()` yields it draws the union of the topic's **family** in the ObjectStore (`markerTopicsOf` — the bare topic plus one `#<owner>` topic per generator), each object topic once per paint (two datasets on one plot share the same all-datasets topic) with a per-topic decode cache keyed on the entry uid; the `__all__` family lives on `kAllDatasetsMarkerDataset` in the display frame, so it is mapped through the global time origin only, never a dataset's alignment offset — and a per-series target reads that shared home too, since a scope=all rule may publish under a series key (`markerTargetTopics`, the resolver exposed as the test seam). In `PJ_RHI_WIDGETS` builds `PlotPanner` snapshots that canvas through `grabFramebuffer()` for its transient drag image because `QWidget::grab()` cannot capture QRhi content; the GL build keeps Qwt's native panner grab path. `CurveEditor` keeps a pinned footer under the curve list (1-px separator, hidden when empty) with up to two marker-scope rows per dataset shown on the plot — "Dataset markers" (`sdk::kGlobalMarkerTopic`, `__global__`) and "Global markers" (`PJ::kAllDatasetsMarkerTopic`, `__all__`) — each shown only when its topic family has a member (`PlotWidget::markerScopeRows()`, the ordered (dataset, scope) set the plot keeps current on `curveListChanged`/`markersChanged` by asking `publishedMarkerScopes(store, dataset)` (`pj_runtime/MarkerTopics.h`) per plotted dataset — the runtime, not the plot, owns the scope→topic naming; `datasetMarkerScopesChanged` announces a row appearing/disappearing, `markerScopeVisibilityChanged` a single toggle flip, so a visibility click re-checks one row instead of rebuilding them); a row's toggle hides/shows only that dataset's scope (`PlotWidget::setDatasetMarkerScopeVisible`), independent of every curve's own `show_markers` flag and of the dataset's other scope, and round-trips through a `<marker_scope dataset_id dataset_source scope visible>` child of `<plot>` in the plot XML (`PlotWidget::saveHiddenMarkerScopes`/`loadHiddenMarkerScopes`; written only when hidden — an absent child means visible on load; `scope` is required, an absent/unrecognized value drops the entry; `pj_app`'s layout identity passes stamp/strip its `dataset_path` like a curve's; an entry whose dataset is not loaded at load time is parked as a pending saved identity — visible meanwhile, still serialized by intermediate snapshots — and `PlotWidget::resolvePendingMarkerScopes()`, called by `pj_app`'s `PendingDisplayBinder::flush` for each plot it bound curves on, moves it into the hidden set under the possibly reminted live id). |
| `tests/` | gtest runners and standalone GL regressions | Core adapters, plotting widgets, docking, filters, and rendering tests. |

## Tests and benchmarks

`PJ_BUILD_TESTS` enables `pj_plotting_core_tests` (shared `QCoreApplication` main)
and `pj_plotting_widget_tests` (shared offscreen-capable `QApplication` main, with
`pj_resources`). `gtest_discover_tests` registers each case with its runner name
as a prefix and launches each case in its own process. The transaction and legend
diagnostics suites use file-local environments that apply their custom setup only
when those suites are selected.

`plot_canvas_context_recreation_gl_test` and `raster_text_gl_test` stay separate:
their custom `QApplication` startup forces the OpenGL canvas and preserves the
platform choice needed for real GL coverage. Their ctest names remain
`PlotCanvasContextRecreationGlTest` and `RasterTextGlTest`.

`plot_rhi_render_test` (QRhi build only) renders each item kind through a real
`PlotRhiCanvas` and compares the presented frame with Qwt's own QPainter
rendering of the same canvas (`QwtPlot::drawCanvas`): curves, overlapping
translucent curves, dashes, every supported symbol, Lines and Dots, marker
line/symbol/label, the legend, `PlotMarkersItem`, a reparent to a new top-level +
resize + sample-count change, and the panner grab. Each comparison checks the
foreground colours (every pixel non-background in either image must be within 48
per channel of a reference pixel in its 3x3 neighbourhood), missing and extra
geometry both ways, and an exactly-clear background away from the reference's
geometry. The reference is drawn with Qwt's whole-pixel snapping off
(`QwtPainter::setRoundingAlignment(false)`): the canvas keeps mapped coordinates
exact, where Qwt on screen rounds them, so a 1 px line can sit half a pixel off
Qwt's own canvas. A long-series case checks the canvas keeps no mapped copy of the series
between frames (`retainedSampleScratchBytes()`). Its failure cases make a named
pipeline fail through the `pjTestFailPipeline` dynamic property and count
pipeline builds through `pjTestPipelineAttempts` (both only under
`PJ_RHI_TEST_HOOKS`, defined in test-enabled desktop builds, with the `pjTestRhi*`
lifecycle counters `rhi_mixed_dock_test` reads; see `pj_widgets/GraphicsApi.h`): the raster
fallback beside a still-rendering sibling, deferred while a popup is open, the
bare canvas's banner and recovery without a retry per frame, a text-only
failure that keeps the curves, and a canvas with no QRhi. A canvas that
initialized a backend and then failed fails the render cases instead of
skipping them. ctest registers it on every backend of the platform, plus
`.opengl_dpr2` (`RHI_BACKENDS DPR2` in `cmake/PjTests.cmake`); it needs a real
display (xvfb or a Wayland compositor on Linux) and skips on `offscreen`
(fails under `PJ_REQUIRE_GPU_TESTS=1`). The two QwtPlotOpenGLCanvas regressions
below exist only in the GL build.

`adapter_streaming_benchmark` is built with `PJ_BUILD_DEMOS`, independently of
`PJ_BUILD_TESTS`. With `PJ_BUILD_DEMOS` in the QRhi build,
`plot_canvas_benchmark --canvas rhi|gl|raster --case NAME` measures one canvas's
frame cost (CPU, GPU, wall) on the static/streaming/zoom-pan/many-curves/dashed
workloads; results are in `pj_scene3D/docs/QRHI_MIGRATION.md` (P4 plot canvas).

## Port strategy (per root CLAUDE.md "Porting policy")

This module is the canonical example of the **wholesale lift** strategy:

1. **Lift PJ3 files close to verbatim.** Source: `${PJ3_SOURCE_DIR}/plotjuggler_app/` (see the root `CLAUDE.md` for the PJ3 reference tree). Class names map directly: PJ3 `PlotWidget` → PJ4 `PlotWidget`, etc.
2. **Apply plotjuggler_sdk style** on the way in: `PascalCase.{h,cpp}`, `PJ::` namespace, `trailing_underscore_` members, Google C++ / 2-space / 120-col.
3. **Preserve `.ui` `objectName` values verbatim** so stylesheets and muscle memory keep working.
4. **Rebind data reads only.** Replace PJ3's `PlotDataMapRef` / `TransformsMap` consumption with `pj_plotting::DatastoreCurveAdapter` over `pj_datastore::DataReader`. This is the only systematic rewrite — do not rewrite plotting logic from scratch.
5. **Do not rebuild from scratch.** If a class reads cleanly in PJ3, port it.

## Cross-module rules

- This module is a sibling of `pj_scene2D/widgets` and `pj_scene3D/widgets`. **They never depend on each other.**
- Shared runtime state flows through `pj_runtime::IDataWidget` (so `PlaybackEngine` can drive tracker updates without coupling to plot internals).
- Reusable Qt controls used here that could serve another widget family go in `pj_widgets`, not here.

## Known historical gotchas

Keep these porting and rendering constraints in mind:

- **OpenGL canvas**: PJ3 had a `Preferences::use_opengl` QSettings gate; the early PJ4 port dropped it, causing software-raster rendering at 72% CPU. Restored. Do not re-drop without measuring.
- **Native window inside ADS**: do **not** propose making `QwtPlotOpenGLCanvas` a `WA_NativeWindow` inside Qt-Advanced-Docking — the native-flag conflict breaks layout.
- **Qwt is external and never patched** (`3rdparty/qwt/README.PJ4.md`: FetchContent of the pinned release, or the conda-forge package under `PJ_SYSTEM_QWT`): the PJ3-era in-tree Qwt patches are realized here instead — `PlotScaleDraw` (fixed-notation tick labels, installed on every axis by `QwtPlotPimpl`) and the "Lines and Dots" style (`PlotCurve::drawCurve`: Lines plus one batched, pixel-weeded `drawDots` pass — per-point QPainter calls are pathological on the OpenGL paint engine; `PlotRhiCanvas` reads `PlotCurve::dotWidth()` and draws the same square dots as geometry). Do not reintroduce patches under `3rdparty/qwt/src`.
- **On-canvas text goes through `RasterTextEngine`** (`widget/include/pj_plotting/RasterTextEngine.h`, installed at app startup): the GL canvas's glyph atlas does not survive GPU resets or context recreation, so legend/tracker text is CPU-rasterized and re-uploaded per draw. Do not draw canvas text with raw `QPainter::drawText`, and do not cache the rasterized images — a resident GL texture is exactly what a GPU reset blanks.
- **The QRhi canvas follows the same reset contract**: it asks Qwt to draw each visible legend/label into a tight DPR-scaled CPU raster, packs those temporary images into one upload, and drops the texture, bindings, pipeline, and text buffer in `releaseResources()`. Keep text changes on the canvas dirty/replot path; never retain the CPU rasters or let a QRhi text resource survive context teardown.

## PlotRhiCanvas

- **Backends.** Shaders are committed multi-backend packs
  (`widget/shaders/*.qsb`, baked by `widget/shaders/bake.sh <qsb>`: GLSL ES 300,
  GLSL 120/150/330/440, HLSL 50, MSL 12, SPIR-V); desktop kits ship no `qsb`, and
  the WASM configure re-bakes them and fails on any byte difference. The shaders
  pass positions through: `logicalToClip()` folds `QRhi::clipSpaceCorrMatrix()`
  (the Vulkan y flip) into the CPU vertex transform. Atlas uv (0,0) is the first
  uploaded row on every backend. Set the viewport after binding each pipeline:
  Vulkan draws nothing otherwise.
- **Budgets** (`PlotRhiCanvas.cpp`, work in device pixels): the reducer keeps up to
  4 extrema per device-pixel column (floor 2048 samples). Desktop gives every
  curve that density and caps a frame at 8M vertices (12 B each: 96 MB); the browser splits
  one density budget across all curves and caps at 1M vertices (12 MB, 32-bit
  heap). Per-curve vertex shares and the rollback-and-halve retry keep a curve
  from committing a partial prefix; a limit hit logs once per onset.
- **Items.** Geometry: `QwtPlotCurve` (Lines, Sticks, Steps, Dots, PlotCurve's
  Lines-and-Dots square dots), `QwtPlotGrid`, `QwtPlotMarker` lines/symbols;
  symbols Ellipse, Rect, Diamond, the four triangles, Hexagon, Cross, XCross,
  HLine, VLine, Star1. Raster layer (the item's own QPainter drawing, in its Z
  slot): `PlotLegend`, marker labels, `PlotMarkersItem` and any other
  `QwtPlotItem`. Drawn approximately with a once-per-process `pj.plotting.rhi`
  warning: other symbol styles (Star2, path/pixmap/graphic/SVG/user; a filled
  square), `UserCurve` style (symbols only), curve area brushes (not filled),
  `Fitted` curves (unfitted). PJ4 produces none of these: every curve style
  (`applyStyleToCurve`, the six layout-XML `style` values) sets no symbol, brush,
  `UserCurve` or `Fitted`; every marker symbol is an `Ellipse`; the SDK has no
  curve-style API; the dialog chart preview is its own raster `QwtPlot`. The GL
  canvas's 1-px frame is not drawn.
- **Lifecycle.** Pipelines are rebuilt when the QRhi, the render pass
  descriptor (address or `serializedFormat()`) or the sample count changes, and
  `render()` re-checks it (a grab of a hidden widget brings a QRhi without
  `initialize()`). `grabPresentedFrame()` grabs after the next presented frame;
  the panner uses the synchronous `grabFramebuffer()` (no uniforms, so the Qt
  6.11.1 grab-only stale-uniform issue does not apply).
- **Failure.** A pipeline or vertex buffer `create()` failure puts the canvas in
  a failed state: clear-only passes (no draw ever references a missing
  resource), the "GPU plot renderer unavailable" banner, and `renderFailed()`
  once per onset (QRhiWidget's own "No QRhi" emission is not echoed). The
  pipelines are retried only in the next `initialize()` (new QRhi, new or
  resized render target), never per frame. On desktop `PlotWidgetBase` answers
  `renderFailed()` (queued: it can fire inside `render()`) by swapping that plot
  to the raster `QwtPlotCanvas` for good, moving the magnifier, panners and
  zoomer (their enabled state kept) and re-installing every canvas event filter
  in the original order, `installHoverFilter()` ones included. The browser keeps
  the banner, although Qwt's raster canvas is compiled there too. A text atlas
  failure only drops text, with a one-time warning.
