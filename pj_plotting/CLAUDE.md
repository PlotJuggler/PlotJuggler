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
| `widget/` | `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, `PlotZoomer`, `PlotPanner`, `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotFocusOverlay`, `PlotScaleDraw`, `DockWidget`, `DockToolbar`, `CurveEditor`, `FilterEditorPanel`, `ParameterForm`, `StateTransitionsController`, `StateTransitionsDockWidget`, `PlotRhiCanvas` | The Qt/Qwt widgets, ported from PJ3's `plotjuggler_app/`. `TabbedPlotWidget::forEachDocker`/`findDocker` enumerate the PlotDocker tabs in one pass over the tab vector — the shell and `pj_app`'s `PluginPlotTabsController` use them instead of index loops over `dockerCount()`/`dockerAt()` (each a linear scan). `PlotRhiCanvas` is the GPU plot canvas on desktop and in the browser — a QRhi canvas (any backend: OpenGL, Vulkan, D3D11/12, Metal, WebGL) that renders the visible Qwt curves, grid and markers as bounded triangle geometry and rasterises everything else (legend, marker labels, `PlotMarkersItem`, any unknown `QwtPlotItem`) with the item's own QPainter drawing into one texture atlas; `PlotWidgetBase` installs it as the Qwt canvas (Qwt still owns axes/data/interaction). On desktop the `Preferences::use_opengl` preference (shown as "GPU canvas") and `--disable-opengl` select the raster `QwtPlotCanvas` instead; the browser always uses the RHI canvas. See "PlotRhiCanvas" below. `PlotMarkersItem` is the marker overlay: for every `(dataset, topic)` target `PlotWidget::markerTargets()` yields it draws the union of the topic's **family** in the ObjectStore (`markerTopicsOf` — the bare topic plus one `#<owner>` topic per generator), each object topic once per paint (two datasets on one plot share the same all-datasets topic) with a per-topic decode cache keyed on the entry uid; the `__all__` family lives on `kAllDatasetsMarkerDataset` in the display frame, so it is mapped through the global time origin only, never a dataset's alignment offset — and a per-series target reads that shared home too, since a scope=all rule may publish under a series key (`markerTargetTopics`, the resolver exposed as the test seam). `PlotPanner` snapshots that canvas through `grabFramebuffer()` for its transient drag image because `QWidget::grab()` cannot capture QRhi content. `CurveEditor` keeps a pinned footer under the curve list (1-px separator, hidden when empty) with up to two marker-scope rows per dataset shown on the plot — "Dataset markers" (`sdk::kGlobalMarkerTopic`, `__global__`) and "Global markers" (`PJ::kAllDatasetsMarkerTopic`, `__all__`) — each shown only when its topic family has a member (`PlotWidget::markerScopeRows()`, the ordered (dataset, scope) set the plot keeps current on `curveListChanged`/`markersChanged` by asking `publishedMarkerScopes(store, dataset)` (`pj_runtime/MarkerTopics.h`) per plotted dataset — the runtime, not the plot, owns the scope→topic naming; `datasetMarkerScopesChanged` announces a row appearing/disappearing, `markerScopeVisibilityChanged` a single toggle flip, so a visibility click re-checks one row instead of rebuilding them); a row's toggle hides/shows only that dataset's scope (`PlotWidget::setDatasetMarkerScopeVisible`), independent of every curve's own `show_markers` flag and of the dataset's other scope, and round-trips through a `<marker_scope dataset_id dataset_source scope visible>` child of `<plot>` in the plot XML (`PlotWidget::saveHiddenMarkerScopes`/`loadHiddenMarkerScopes`; written only when hidden — an absent child means visible on load; `scope` is required, an absent/unrecognized value drops the entry; `pj_app`'s layout identity passes stamp/strip its `dataset_path` like a curve's; an entry whose dataset is not loaded at load time is parked as a pending saved identity — visible meanwhile, still serialized by intermediate snapshots — and `PlotWidget::resolvePendingMarkerScopes()`, called by `pj_app`'s `PendingDisplayBinder::flush` for each plot it bound curves on, moves it into the hidden set under the possibly reminted live id). |
| `tests/` | gtest runners and the QRhi render harness | Core adapters, plotting widgets, docking, filters, and rendering tests. |

## Tests and benchmarks

`PJ_BUILD_TESTS` enables `pj_plotting_core_tests` (shared `QCoreApplication` main)
and `pj_plotting_widget_tests` (shared offscreen-capable `QApplication` main, with
`pj_resources`). `gtest_discover_tests` registers each case with its runner name
as a prefix and launches each case in its own process. The transaction and legend
diagnostics suites use file-local environments that apply their custom setup only
when those suites are selected.

`plot_rhi_render_test` renders each item kind through a real
`PlotRhiCanvas` and compares the presented frame with Qwt's own QPainter
rendering of the same canvas (its items drawn as `QwtPlot::drawItems` does):
curves, overlapping translucent curves, dashes, every supported symbol, Lines and
Dots, marker line/symbol/label, the legend, `PlotMarkersItem`, a reparent to a
new top-level + resize + sample-count change, and the panner grab. Each
comparison checks the foreground colours (at least 98% of the pixels
non-background in either image must be within 48 per channel of a reference
pixel in its 3x3 neighbourhood), missing and extra geometry both ways, and an
exactly-clear background away from the reference's geometry. The reference snaps
the grid and markers to whole pixels as Qwt does on screen
(`QwtPainter::roundingAlignment()`) and draws curves unsnapped, as the canvas
does. Further cases pin the rendering down: grid and marker lines on whole
device pixels with Qt's cosmetic cadence (dotted 1 on / 2 off, dashed 4 on /
2 off) and the 1 px frame; the edge coverage ramp (each device column of a wide
sloped line holds its exact coverage, within 0.25 px, with at most two partial
pixels per edge); every dashed style, cosmetic and 2 px wide, against Qwt's
dashes; a tracker move that rebuilds neither the curve layer nor the legend's
raster (`curveLayerBuildsForTest()`, `textLayerRasterizationsForTest()`); a
labelled marker replaced by an identical one that keeps its label; an XY curve
doubling back past its start at full stroke width; and the colour-only
one-sample target (`renderTargetBytesPerPixel()`). A long-series case checks the
canvas keeps no mapped copy of the series between frames
(`retainedSampleScratchBytes()`). Its failure cases make a named
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
(fails under `PJ_REQUIRE_GPU_TESTS=1`).

`adapter_streaming_benchmark` is built with `PJ_BUILD_DEMOS`, independently of
`PJ_BUILD_TESTS`. With `PJ_BUILD_DEMOS`,
`plot_canvas_benchmark --canvas rhi|raster --case NAME` measures a canvas's
frame cost (CPU, wall, the vertices built, the process's DRM GPU memory) on the
static/streaming/zoom-pan/many-curves/dashed/tracker-only workloads; `--samples`,
`--plots K`, `--grid`, `--legend` and `--msaa N` vary the scene, and `--gpu`
adds GPU time (timestamp queries, which cost CPU). The Gate A numbers are in
`pj_scene3D/docs/TECHNICAL_NOTES.md` §9 (the performance baseline); the per-change measurements are archived in
`docs/archive/QRHI_MIGRATION.md` (P4 plot canvas, P5 B and P5 PL).

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

- **GPU canvas**: PJ3 had a `Preferences::use_opengl` QSettings gate; the early PJ4 port dropped it, causing software-raster rendering at 72% CPU. Restored. Do not re-drop without measuring.
- **Native window inside ADS**: do **not** propose making the plot canvas a `WA_NativeWindow` inside Qt-Advanced-Docking — the native-flag conflict breaks layout.
- **Qwt is external and never patched** (`3rdparty/qwt/README.PJ4.md`: FetchContent of the pinned release, or the conda-forge package under `PJ_SYSTEM_QWT`): the PJ3-era in-tree Qwt patches are realized here instead — `PlotScaleDraw` (fixed-notation tick labels, installed on every axis by `QwtPlotPimpl`) and the "Lines and Dots" style (`PlotCurve::drawCurve`: Lines plus one batched, pixel-weeded `drawDots` pass — per-point QPainter calls are pathological; `PlotRhiCanvas` reads `PlotCurve::dotWidth()` and draws the same square dots as geometry). Do not reintroduce patches under `3rdparty/qwt/src`.
- **GPU resets and context teardown**: the QRhi canvas records each visible legend/label/raster item's QPainter drawing into a `QPicture`, rasterises it into a tight DPR-scaled CPU image, packs the images into one atlas upload, and drops the texture, bindings, pipeline, text buffer and the text cache in `releaseResources()`. The CPU rasters are cached only while their recorded drawing (every command, font, colour and position), raster rectangle and dpr are unchanged, so a tracker move re-rasterises its value box and nothing else; never let a QRhi text resource survive context teardown.

## PlotRhiCanvas

- **Files** (`widget/src/`, private to the module, namespace `PJ::plot_rhi`):
  `PlotRhiGeometry.{h,cpp}` builds the geometry layers (clipping, strokes,
  dashes, symbols, grids, curves, markers, the frame) and the curve layer's
  cache keys, and holds the frame budgets; `PlotRhiTextAtlas.{h,cpp}` records,
  caches, rasterises and packs the text layers; `PlotRhiCanvas.cpp` owns the
  widget lifecycle, the rebuild decisions, the GPU resources and uploads, the
  draw submission and failure reporting. The public header carries no
  implementation types: the retained layers' bookkeeping sits behind one
  `plot_rhi::RetainedLayers` pointer.
- **Backends.** Shaders are committed multi-backend packs
  (`widget/shaders/*.qsb`, baked by `widget/shaders/bake.sh <qsb>`: GLSL ES 300,
  GLSL 120/150/330/440, HLSL 50, MSL 12, SPIR-V); desktop kits ship no `qsb`, and
  the WASM configure re-bakes them and fails on any byte difference. The shaders
  pass positions through: `logicalToClip()` folds `QRhi::clipSpaceCorrMatrix()`
  (the Vulkan y flip) into the CPU vertex transform. Atlas uv (0,0) is the first
  uploaded row on every backend. Set the viewport after binding each pipeline:
  Vulkan draws nothing otherwise.
- **Budgets** (`PlotRhiGeometry.h`, work in device pixels): the reducer keeps up to
  4 extrema per device-pixel column (floor 2048 samples). Desktop gives every
  curve that density and caps a frame at 8M vertices (16 B each: 128 MB, plus
  up to three 4 B indices per vertex); the browser splits one density budget
  across all curves and caps at 1M vertices (16 MB, 32-bit heap). Per-curve
  vertex shares and the rollback-and-halve retry keep a curve from committing a
  partial prefix; a limit hit logs once per onset. The geometry layers are
  indexed (`drawIndexed`, 32-bit indices): a solid polyline is one strip of
  corner pairs shared by consecutive quads and their bevels (the bevel adds
  only its join point: 5 vertices and 9 indices per turning segment, 2 and 6
  on a straight join; a collinear reversal starts a pair of its own, its normal
  flipped), with the same triangles as separate quads; fans, rings,
  boxes and dashed quads share their corners too. The arrays are built in `QByteArray`s
  handed to `uploadStaticBuffer()` by implicit sharing (no copy).
- **Antialiasing.** One sample per pixel into a colour-only target the canvas
  owns (`setAutoRenderTarget(false)`, `PJ::RhiColorTarget`: no depth-stencil; 4 B
  per device pixel). Every
  stroke, fan, ring and box reaches half a device pixel past its true edge and
  carries its position across (`plot_rhi::Vertex::aux`); `plot_color.frag` ramps
  coverage over that one device pixel from screen-space derivatives. Strokes
  under one device pixel are drawn one pixel wide at proportionally lower alpha,
  as Qt's antialiased raster does. Horizontal and vertical `addLine()` lines (grid,
  marker lines, sticks, symbol strokes) and the Rect/Cross/HLine/VLine symbols sit
  on whole pixels like Qwt's (`snapToPixels()`: the logical coordinate rounded,
  then centred on its device pixel for an odd device width, on a pixel edge for
  an even one); curves keep exact
  coordinates. `setSampleCount(n > 1)` still works (an MSAA colour buffer and its
  resolve, still no depth-stencil), for comparisons only.
- **Dashes** are cut in the fragment shader from the path distance carried per
  vertex (16 bits, 1/64 pen width; a clipped segment longer than 960 pen widths
  is split into several quads), so a dashed segment is one quad. Qt's patterns
  (Dash 4/2, Dot 1/2, DashDot, DashDotDot, in pen widths): a cosmetic pen's pattern
  is sampled per pixel with no caps, as `QCosmeticStroker` does; a wide pen's
  square caps lengthen every dash by half the width, antialiased. The phase
  follows the source path through the reducer; a turn gets its bevel only where
  a dash runs through it. Custom dash patterns draw solid.
- **Dirty layers.** Three retained vertex buffers: grid + curves + frame, the
  overlay (every `QwtPlotMarker`: the tracker, hover and ride-along markers), and
  text. `replot()` dirties the overlay, and the curve layer unless the data of
  every curve it draws (any `QwtPlotCurve` subclass, whatever its RTTI) is a
  `RevisionedSeries` (`core/RevisionedSeries.h`: the
  datastore adapters and `PointSeriesXY` inherit it and call
  `bumpContentRevision()` on every commit, clear, display-offset move, filter
  change and new rect of interest) whose revision is unchanged; plain Qwt data
  rebuilds on every replot. Revisions come from one process-wide counter, so the
  content key holds the revisions alone, and a series re-created at a freed one's
  address (a transform applied in place) never repeats it. Inside
  `setOverlayOnlyReplot(true)` (what `PlotWidgetBase::replotOverlays()`, and so
  `PlotWidget::setTrackerPosition()`, wraps around one replot) only the overlay.
  The curve layer is rebuilt anyway when the view (size, dpr, axis bounds, maps,
  each grid tick list with its type, clear or frame colour), its structure
  (items, visibility, pens, brushes, styles, symbols, axes, grid flags, data
  object and size) or a curve's content revision changed. A map's transformation
  counts by type and parameter (none, `QwtLogTransform`, `QwtPowerTransform`);
  any other transformation rebuilds every frame. Any geometry rebuild regathers the text layers, reusing the
  cached rasters, and re-uploads the atlas only when the layer set changed (the
  canvas size, the truncation and the layer keys in draw order); a reused atlas
  still re-owns its quads to this build's items, so an identical replacement item
  keeps its text.
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
  curve-style API; the dialog chart preview is its own raster `QwtPlot`. The
  1 px canvas frame (the Qwt canvases' `QFrame::Box | Plain` in the palette's
  WindowText) is drawn over every item.
- **Lifecycle.** `initialize()` (re)creates the render target on the widget's
  current colour buffer on every call (a resize rebuilds that buffer in place),
  keeping the render pass descriptor while it stays compatible. Pipelines are
  rebuilt when the QRhi, the render pass descriptor (address or
  `serializedFormat()`) or the sample count changes, and `render()` re-checks the
  QRhi (a grab of a hidden widget brings a QRhi without `initialize()`). `grabPresentedFrame()` grabs after the next presented frame;
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
