# pj_plotting Architecture

The HOW document for `pj_plotting`. [`../CLAUDE.md`](../CLAUDE.md) holds the module's purpose, layout and
rules; read it first. Each header's doc-comment owns its API contract; this document explains the designs that span
several classes.

## Wheel zoom keys

Holding X or Y constrains mouse-wheel zoom to that axis on the hovered plot,
including a canvas with keyboard focus. `PlotMagnifier` observes held keys at
application scope because keyboard focus may belong to another widget. The
observer leaves text entry and shortcuts untouched, ignores auto-repeat
releases, and clears held state on application deactivation. Neither key (or
both keys) preserves the canvas/axis-margin default. The per-wheel override is
restored after the gesture; the canvas event filter can still set its usual
default. `PlotMagnifierKeys` covers the actual canvas filter chain, separate
keyboard focus, release, auto-repeat, and deactivation.

## 1. Core adapters (`core/`)

- **`DatastoreCurveAdapter`** bridges PJ3-style `QwtSeriesData<QPointF>` consumers to `pj_datastore::DataReader`.
  It, `FilteredCurveAdapter` and `PointSeriesXY` implement `RevisionedSeries` (§3.6).
- **`FilteredCurveAdapter`** is a lazy `DatastoreCurveAdapter` subclass that serves the output of a
  `proc::DataProcessor`: the Filter Editor's before/after preview, refreshed on the streaming commit path.
- **`StateSeriesAdapter`** is the discrete-series counterpart feeding the State Transitions strip: a full-rebuild
  run-length reader over `rangeQuery`. Labels per type: `readString` copied out inside the cursor callback, integers
  in decimal via the exact int64/uint64 reads, a float column holding only whole numbers labelled as integers (a
  fractional value reads as null), bool via `readBool` as "true"/"false". Equal-timestamp samples resolve
  last-write-wins, a null is a gap, the trailing run stays open.
- **`PlotXml.h`** is the plot-XML vocabulary shared with `pj_app`'s layout passes: the `x_basis` range markers, the
  `pending_intent` keep-alive predicate, and the `<marker_scope scope=…>` attribute with its two persisted values
  (the value↔`MarkerScope` mapping lives in `PlotWidget.cpp`).

## 2. Markers on a plot

### 2.1 The marker overlay (`PlotMarkersItem`)

For every `(dataset, topic)` target `PlotWidget::markerTargets()` yields, the overlay draws the union of the topic's
**family** in the ObjectStore (`markerTopicsOf`: the topics whose metadata names that scope and target, a direct
write plus one per generator, whatever they are called). Each dataset's marker topics are scanned once per paint
with `scanMarkerTopics` and the targets matched in memory; each object topic is drawn once per paint (two datasets
on one plot share the all-datasets topic), with a per-topic decode cache keyed on the entry uid.

**Time frames.** The `__all__` family lives on `kAllDatasetsMarkerDataset` in the display frame, so it is mapped
through the global time origin only, never a dataset's alignment offset. A per-series target reads that shared home
too, since a scope=all rule may publish under a series key (`markerTargetTopics`, the resolver exposed as the test
seam).

### 2.2 Status notes

- **Marker status.** `PlotWidget::setMarkerStatus` takes the computing flag and error of each marker rule the plot
  displays; the host decides which, so the plot knows no engine. The overlay paints "Computing markers…" or "Marker
  rule failed" in the canvas's top-left corner (top-right when the legend is aligned top-left), lists the errors as
  the note's tooltip, keeps the markers drawn, and replots only when the note's text changes.
- **Series status.** `PlotWidget::setSeriesStatus` is its twin for on-demand series: plain `SeriesCurveStatus` data
  from the host puts "(not computed)" or "(computing N%)" after the legend title of each curve on such a topic
  (`PlotCurve::setLegendSuffix`, legend only: the title stays the curve's identity).

### 2.3 Marker-scope rows in the `CurveEditor`

The `CurveEditor` keeps a pinned footer under the curve list (1-px separator, hidden when empty) with up to two
marker-scope rows per dataset on the plot: "Dataset markers" (`sdk::kGlobalMarkerTopic`, `__global__`) and "Global
markers" (`PJ::kAllDatasetsMarkerTopic`, `__all__`), each shown only when its topic family has a member.

- `PlotWidget::markerScopeRows()` is the ordered (dataset, scope) set. The plot keeps it current on
  `curveListChanged`/`markersChanged` by asking `publishedMarkerScopes(store, dataset)` (`pj_runtime/MarkerTopics.h`)
  per plotted dataset: the runtime, not the plot, owns the scope→topic naming.
- `datasetMarkerScopesChanged` announces a row appearing or disappearing; `markerScopeVisibilityChanged` a single
  toggle flip, so a visibility click re-checks one row instead of rebuilding them.
- A row's toggle hides/shows only that dataset's scope (`PlotWidget::setDatasetMarkerScopeVisible`), independent of
  every curve's own `show_markers` flag and of the dataset's other scope.
- **Persistence.** A hidden scope round-trips through a `<marker_scope dataset_id dataset_source scope visible>`
  child of `<plot>` (`saveHiddenMarkerScopes`/`loadHiddenMarkerScopes`), written only when hidden: an absent child
  means visible. `scope` is required; an absent or unrecognized value drops the entry. `pj_app`'s layout identity
  passes stamp/strip its `dataset_path` like a curve's.
- **Pending entries.** An entry whose dataset is not loaded at load time is parked as a pending saved identity
  (visible meanwhile, still serialized by intermediate snapshots). `PlotWidget::resolvePendingMarkerScopes()`, called
  by `pj_app`'s `PendingDisplayBinder::flush` for each plot it bound curves on, moves it into the hidden set under the
  possibly reminted live id.

## 3. PlotRhiCanvas

The GPU plot canvas on desktop and in the browser: a `QRhiWidget` (any backend) installed as the Qwt canvas by
`PlotWidgetBase`. Qwt still owns axes, data and interaction. It renders the visible Qwt curves, grid and markers as
bounded triangle geometry and rasterises everything else with the item's own QPainter drawing into one texture
atlas. On desktop `Preferences::use_opengl` ("GPU canvas") and `--disable-opengl` select the raster
`QwtPlotCanvas` instead; the browser always uses this canvas.

### 3.1 Files

In `widget/src/`, private to the module, namespace `PJ::plot_rhi`:

- `PlotRhiGeometry.{h,cpp}` builds the geometry layers (clipping, strokes, dashes, symbols, grids, curves, markers,
  the frame) and the curve layer's cache keys, and holds the frame budgets.
- `PlotRhiTextAtlas.{h,cpp}` records, caches, rasterises and packs the text layers.
- `PlotRhiCanvas.cpp` owns the widget lifecycle, the rebuild decisions, the GPU resources and uploads, the draw
  submission and failure reporting.

The public header carries no implementation types: the retained layers' bookkeeping sits behind one
`plot_rhi::RetainedLayers` pointer.

### 3.2 Backends

Shaders are committed multi-backend packs (`widget/shaders/*.qsb`, baked by `widget/shaders/bake.sh <qsb>`: GLSL ES
300, GLSL 120/150/330/440, HLSL 50, MSL 12, SPIR-V); desktop kits ship no `qsb`, and the WASM configure re-bakes them
and fails on any byte difference. The shaders pass positions through: `logicalToClip()` folds
`QRhi::clipSpaceCorrMatrix()` (the Vulkan y flip) into the CPU vertex transform. Atlas uv (0,0) is the first uploaded
row on every backend. Set the viewport after binding each pipeline: Vulkan draws nothing otherwise.

### 3.3 Budgets

In `PlotRhiGeometry.h`, working in device pixels:

- The reducer keeps up to 4 extrema per device-pixel column (floor 2048 samples).
- Desktop gives every curve that density and caps a frame at 8M vertices (16 B each: 128 MB, plus up to three 4 B
  indices per vertex); the browser splits one density budget across all curves and caps at 1M vertices (16 MB,
  32-bit heap).
- Per-curve vertex shares and the rollback-and-halve retry keep a curve from committing a partial prefix; a limit
  hit logs once per onset.
- The geometry layers are indexed (`drawIndexed`, 32-bit indices). A solid polyline is one strip of corner pairs
  shared by consecutive quads and their bevels (the bevel adds only its join point: 5 vertices and 9 indices per
  turning segment, 2 and 6 on a straight join; a collinear reversal starts a pair of its own, its normal flipped),
  with the same triangles as separate quads; fans, rings, boxes and dashed quads share their corners too.
- The arrays are built in `QByteArray`s handed to `uploadStaticBuffer()` by implicit sharing (no copy).

### 3.4 Antialiasing

One sample per pixel into a colour-only target the canvas owns (`setAutoRenderTarget(false)`,
`PJ::RhiColorTarget`: no depth-stencil; 4 B per device pixel).

- Every stroke, fan, ring and box reaches half a device pixel past its true edge and carries its position across
  (`plot_rhi::Vertex::aux`); `plot_color.frag` ramps coverage over that one device pixel from screen-space
  derivatives.
- Strokes under one device pixel are drawn one pixel wide at proportionally lower alpha, as Qt's antialiased raster
  does.
- Horizontal and vertical `addLine()` lines (grid, marker lines, sticks, symbol strokes) and the
  Rect/Cross/HLine/VLine symbols sit on whole pixels like Qwt's (`snapToPixels()`: the logical coordinate rounded,
  then centred on its device pixel for an odd device width, on a pixel edge for an even one); curves keep exact
  coordinates.
- `setSampleCount(n > 1)` still works (an MSAA colour buffer and its resolve, still no depth-stencil), for
  comparisons only.

### 3.5 Dashes

Cut in the fragment shader from the path distance carried per vertex (16 bits, 1/64 pen width; a clipped segment
longer than 960 pen widths is split into several quads), so a dashed segment is one quad. Qt's patterns (Dash 4/2,
Dot 1/2, DashDot, DashDotDot, in pen widths): a cosmetic pen's pattern is sampled per pixel with no caps, as
`QCosmeticStroker` does; a wide pen's square caps lengthen every dash by half the width, antialiased. The phase
follows the source path through the reducer; a turn gets its bevel only where a dash runs through it. Custom dash
patterns draw solid.

### 3.6 Dirty layers

Three retained vertex buffers: grid + curves + frame; the overlay (every `QwtPlotMarker`: the tracker, hover and
ride-along markers); and text.

- `replot()` dirties the overlay, and the curve layer unless the data of every curve it draws (any `QwtPlotCurve`
  subclass) is a `RevisionedSeries` whose revision is unchanged. The datastore adapters and `PointSeriesXY` call
  `bumpContentRevision()` on every commit, clear, display-offset move, filter change and new rect of interest; plain
  Qwt data rebuilds on every replot. Revisions come from one process-wide counter, so the content key holds the
  revisions alone, and a series re-created at a freed one's address never repeats one.
- Inside `setOverlayOnlyReplot(true)` (what `PlotWidgetBase::replotOverlays()`, and so
  `PlotWidget::setTrackerPosition()`, wraps around one replot) only the overlay is dirtied.
- The curve layer is rebuilt anyway when the view (size, dpr, axis bounds, maps, each grid tick list with its type,
  clear or frame colour), its structure (items, visibility, pens, brushes, styles, symbols, axes, grid flags, data
  object and size) or a curve's content revision changed. A map's transformation counts by type and parameter (none,
  `QwtLogTransform`, `QwtPowerTransform`); any other transformation rebuilds every frame.
- Any geometry rebuild regathers the text layers, reusing the cached rasters, and re-uploads the atlas only when the
  layer set changed (the canvas size, the truncation and the layer keys in draw order); a reused atlas still re-owns
  its quads to this build's items, so an identical replacement item keeps its text.

### 3.7 Text and GPU resets

Each visible legend/label/raster item's QPainter drawing is recorded into a `QPicture`, rasterised into a tight
DPR-scaled CPU image, and the images are packed into one atlas upload. A CPU raster is cached only while its
recorded drawing (every command, font, colour and position), raster rectangle and dpr are unchanged, so a tracker
move re-rasterises its value box and nothing else. `releaseResources()` drops the texture, bindings, pipeline, text
buffer and text cache: no QRhi text resource survives context teardown.

### 3.8 Items

- **Geometry:** `QwtPlotCurve` (Lines, Sticks, Steps, Dots, `PlotCurve`'s Lines-and-Dots square dots read from
  `PlotCurve::dotWidth()`), `QwtPlotGrid`, `QwtPlotMarker` lines/symbols; symbols Ellipse, Rect, Diamond, the four
  triangles, Hexagon, Cross, XCross, HLine, VLine, Star1.
- **Raster layer** (the item's own QPainter drawing, in its Z slot): `PlotLegend`, marker labels, `PlotMarkersItem`
  and any other `QwtPlotItem`.
- **Drawn approximately**, with a once-per-process `pj.plotting.rhi` warning: other symbol styles (Star2,
  path/pixmap/graphic/SVG/user; a filled square), `UserCurve` style (symbols only), curve area brushes (not filled),
  `Fitted` curves (unfitted). PJ4 produces none of these: every curve style (`applyStyleToCurve`, the six layout-XML
  `style` values) sets no symbol, brush, `UserCurve` or `Fitted`; every marker symbol is an `Ellipse`; the SDK has
  no curve-style API; the dialog chart preview is its own raster `QwtPlot`. A new curve style that needs one of these
  must add it to the geometry path first.
- The 1 px canvas frame (the Qwt canvases' `QFrame::Box | Plain` in the palette's WindowText) is drawn over every
  item.

### 3.9 Lifecycle

`initialize()` (re)creates the render target on the widget's current colour buffer on every call (a resize rebuilds
that buffer in place), keeping the render pass descriptor while it stays compatible. Pipelines are rebuilt when the
QRhi, the render pass descriptor (address or `serializedFormat()`) or the sample count changes, and `render()`
re-checks the QRhi (a grab of a hidden widget brings a QRhi without `initialize()`). `grabPresentedFrame()` grabs
after the next presented frame. `PlotPanner` snapshots the canvas for its transient drag image with the synchronous
`grabFramebuffer()` (`QWidget::grab()` cannot capture QRhi content); with no uniforms, the grab-only stale-uniform
issue (`pj_scene3D/docs/TECHNICAL_NOTES.md` §3) does not apply.

### 3.10 Failure

A pipeline or vertex buffer `create()` failure puts the canvas in a failed state: clear-only passes (no draw ever
references a missing resource), the "GPU plot renderer unavailable" banner, and `renderFailed()` once per onset
(QRhiWidget's own "No QRhi" emission is not echoed). The pipelines are retried only in the next `initialize()` (new
QRhi, new or resized render target), never per frame. On desktop `PlotWidgetBase` answers `renderFailed()` (queued:
it can fire inside `render()`) by swapping that plot to the raster `QwtPlotCanvas` for good, moving the magnifier,
panners and zoomer (their enabled state kept) and re-installing every canvas event filter in the original order,
`installHoverFilter()` ones included. The browser keeps the banner, although Qwt's raster canvas is compiled there
too. A text atlas failure only drops text, with a one-time warning.

## 4. Render tests and benchmarks

### 4.1 `plot_rhi_render_test`

Renders each item kind through a real `PlotRhiCanvas` and compares the presented frame with Qwt's own QPainter
rendering of the same canvas (its items drawn as `QwtPlot::drawItems` does): curves, overlapping translucent curves,
dashes, every supported symbol, Lines and Dots, marker line/symbol/label, the legend, `PlotMarkersItem`, a reparent
to a new top-level + resize + sample-count change, and the panner grab.

- **Comparison.** The foreground colours (at least 98% of the pixels non-background in either image must be within
  48 per channel of a reference pixel in its 3x3 neighbourhood), missing and extra geometry both ways, and an
  exactly-clear background away from the reference's geometry. The reference snaps the grid and markers to whole
  pixels as Qwt does on screen (`QwtPainter::roundingAlignment()`) and draws curves unsnapped, as the canvas does.
- **Pinned behaviour.** Grid and marker lines on whole device pixels with Qt's cosmetic cadence (dotted 1 on / 2
  off, dashed 4 on / 2 off) and the 1 px frame; the edge coverage ramp (each device column of a wide sloped line
  holds its exact coverage, within 0.25 px, with at most two partial pixels per edge); every dashed style, cosmetic
  and 2 px wide, against Qwt's dashes; a tracker move that rebuilds neither the curve layer nor the legend's raster
  (`curveLayerBuildsForTest()`, `textLayerRasterizationsForTest()`); a labelled marker replaced by an identical one
  that keeps its label; an XY curve doubling back past its start at full stroke width; the colour-only one-sample
  target (`renderTargetBytesPerPixel()`); and no mapped copy of a long series kept between frames
  (`retainedSampleScratchBytes()`).
- **Failure cases** make a named pipeline fail through the `pjTestFailPipeline` dynamic property and count pipeline
  builds through `pjTestPipelineAttempts` (both only under `PJ_RHI_TEST_HOOKS`, defined in test-enabled desktop
  builds, with the `pjTestRhi*` lifecycle counters `rhi_mixed_dock_test` reads; see `pj_widgets/GraphicsApi.h`): the
  raster fallback beside a still-rendering sibling, deferred while a popup is open, the bare canvas's banner and
  recovery without a retry per frame, a text-only failure that keeps the curves, and a canvas with no QRhi. A canvas
  that initialized a backend and then failed fails the render cases instead of skipping them.

### 4.2 Benchmarks

Built with `PJ_BUILD_DEMOS`, independently of `PJ_BUILD_TESTS`:

- `adapter_streaming_benchmark`: the datastore adapters under streaming.
- `plot_canvas_benchmark --canvas rhi|raster --case NAME` (desktop only) measures a canvas's frame cost (CPU, wall,
  the vertices built, the process's DRM GPU memory) on the static/streaming/zoom-pan/many-curves/dashed/tracker-only
  workloads; `--samples`, `--plots K`, `--grid`, `--legend` and `--msaa N` vary the scene, and `--gpu` adds GPU time
  (timestamp queries, which cost CPU).

The performance baseline is in [`pj_scene3D/docs/TECHNICAL_NOTES.md`](../../pj_scene3D/docs/TECHNICAL_NOTES.md) §9.
