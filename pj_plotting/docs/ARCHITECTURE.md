# pj_plotting architecture

How the designs that span several classes work today. [`../CLAUDE.md`](../CLAUDE.md) holds the module's layout and
invariants; each header's doc-comment owns its API contract; measurements and platform findings are in
[TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md).

## Core adapters

- **`DatastoreCurveAdapter`** bridges `QwtSeriesData<QPointF>` consumers to `pj_datastore::DataReader`. It serves a
  materialized window of plain values (no `TopicChunk` pointer, so retention can free chunks without touching it): the
  visible span widened by one span per side plus one guard sample strictly outside each end. Zoom and pan steps inside
  the window re-narrow it by binary search; a commit that only appended in order, or raised the retention floor, is
  absorbed without a rebuild (O(new samples) per commit while streaming). `FilteredCurveAdapter`, `PointSeriesXY` and
  it implement `RevisionedSeries` ("PlotRhiCanvas", "Dirty layers"). Its virtual-index lookups assume chronological
  chunks and are approximate when chunks overlap, so they are used for sizing only.
- **`FilteredCurveAdapter`** is a lazy `DatastoreCurveAdapter` subclass serving the output of a `proc::DataProcessor`:
  the Filter Editor's before/after preview. It reports the input topic as its `source()`, so the `samplesIngested`
  handler that refreshes the raw curve refreshes it too, with no timer; the processor is rebuilt fresh on every
  recompute (processors are stateful) and runs over the whole input column read through the same reader as the raw
  curve.
- **`PointSeriesXY`** pairs two columns into an XY series over a time-ordered alignment index. The index holds the
  paired values, copied under the engine lock: nothing read from a chunk outlives that lock (`TopicStorage`). The
  plot clears it synchronously before an in-place dataset reload swaps chunks
  (`datasetAboutToBeReplaced`, a `Qt::DirectConnection`), as it clears the other adapters' caches. It ignores the
  display offset.
- **`StateSeriesAdapter`** is the discrete-series counterpart feeding the State Transitions strip: a full-rebuild
  run-length reader over `rangeQuery`, correct under out-of-order chunk merges, retention eviction and dataset replaces
  because it keeps no watermark and no chunk pointer. Labels per type: `readString` copied out inside the cursor
  callback, integers in decimal through the exact int64/uint64 reads, a float column holding only whole numbers labelled
  as integers (a fractional value reads as null), bool through `readBool` as "true" / "false". Equal-timestamp samples
  resolve last-write-wins, a null is a gap, the trailing run stays open.
- **`PlotXml.h`** is the plot-XML vocabulary shared with `pj_app`'s layout passes: the `x_basis` range markers, the
  `pending_intent` keep-alive predicate, and the `<marker_scope scope=...>` attribute with its two persisted values (the
  value <-> `MarkerScope` mapping lives in `PlotWidget.cpp`). It also holds the steps a layout pass shares with the
  widgets: the curve palette and its free-colour rule (`freePaletteIndex`, which `PlotWidgetBase` runs over its
  curves' pens), `fillMissingCurveColors` (a saved plot's curves get the colours the plot would pick), and
  `prepareTabCopy` (Duplicate Tab and the layout's per-dataset tab copies).

## Wheel zoom keys

Holding X or Y constrains mouse-wheel zoom to that axis on the hovered plot, including a canvas with keyboard focus.
`PlotMagnifier` observes held keys at application scope because keyboard focus may belong to another widget. The
observer leaves text entry and shortcuts untouched, ignores auto-repeat releases and clears held state on application
deactivation. Neither key, or both, keeps the canvas/axis-margin default. The per-wheel override is restored after the
gesture; the canvas event filter can still set its usual default. `PlotMagnifierKeys` covers the canvas filter chain,
separate keyboard focus, release, auto-repeat and deactivation.

On a time-series plot the magnifier clamps the window to the smallest non-degenerate one an integer-nanosecond saved
viewport can represent (`plotting_detail::ulpAwareMinTimeXWidthSec`, about 2 ns, wider near epoch scale), because a
smaller window would round to equal edges and could not survive save and load. An XY plot has no such quantization.

## Markers on a plot

### The marker overlay (`PlotMarkersItem`)

For every `(dataset, topic)` target `PlotWidget::markerTargets()` yields, the overlay draws the union of the topic's
family in the ObjectStore (`markerTopicsOf`: the topics whose metadata names that scope and target, a direct write
plus one per generator, whatever they are called). Each dataset's marker topics are scanned once per paint with
`scanMarkerTopics` and the targets matched in memory; each object topic is drawn once per paint (two datasets on one
plot share the all-datasets topic), with a per-topic decode cache keyed on the entry uid, so a paint re-decodes only
when a producer republished.

**Time frames.** The `__all__` family lives on `kAllDatasetsMarkerDataset` in the display frame, so it is mapped through
the global time origin only, never a dataset's alignment offset. A per-series target reads that shared home too, since a
scope=all rule may publish under a series key (`markerTargetTopics`, the resolver exposed as the test seam).

### Status notes

- **Marker status.** `PlotWidget::setMarkerStatus` takes the computing flag and error of each marker rule the plot
  displays; the host decides which, so the plot knows no engine. The overlay paints "Computing markers…" or "Marker
  rule failed" in the canvas's top-left corner (top-right when the legend is aligned top-left), lists the errors as
  the note's tooltip, keeps the markers drawn, and replots only when the note's text changes.
- **Series status.** `PlotWidget::setSeriesStatus` is its twin for on-demand series: plain `SeriesCurveStatus` data from
  the host puts "(not computed)" or "(computing N%)" after the legend title of each curve on such a topic
  (`PlotCurve::setLegendSuffix`, legend only: the title stays the curve's identity).

### Marker-scope rows in the CurveEditor

The `CurveEditor` keeps a pinned footer under the curve list (1-px separator, hidden when empty) with up to two
marker-scope rows per dataset on the plot: "Dataset markers" (`sdk::kGlobalMarkerTopic`, `__global__`) and "Global
markers" (`PJ::kAllDatasetsMarkerTopic`, `__all__`), each shown only when its topic family has a member.

- `PlotWidget::markerScopeRows()` is the ordered (dataset, scope) set. The plot keeps it current on
  `curveListChanged` / `markersChanged` by asking `publishedMarkerScopes(store, dataset)` (`pj_runtime/MarkerTopics.h`)
  per plotted dataset: the runtime, not the plot, owns the scope -> topic naming. `markersChanged` fires on every
  streaming tick, so the row-set signal is emitted only when a topic actually appeared or was retired.
- `datasetMarkerScopesChanged` announces a row appearing or disappearing; `markerScopeVisibilityChanged` a single toggle
  flip, so a visibility click re-checks one row instead of rebuilding them.
- A row's toggle hides or shows only that dataset's scope (`PlotWidget::setDatasetMarkerScopeVisible`), independent of
  every curve's own `show_markers` flag and of the dataset's other scope.
- **Persistence.** A hidden scope round-trips through a `<marker_scope dataset_id dataset_source scope visible>` child
  of `<plot>` (`saveHiddenMarkerScopes` / `loadHiddenMarkerScopes`), written only when hidden: an absent child means
  visible. `scope` is required; an absent or unrecognized value drops the entry. `pj_app`'s layout identity passes
  stamp and strip its `dataset_path` like a curve's.
- **Pending entries.** An entry whose dataset is not loaded at load time is parked as a pending saved identity
  (visible meanwhile, still serialized by intermediate snapshots). `PlotWidget::resolvePendingMarkerScopes()`, called
  by `pj_app`'s `PendingDisplayBinder::flush` for each plot it bound curves on, moves it into the hidden set under the
  possibly reminted live id; ambiguous entries stay pending.

## Curve visibility, colour and the tracker

Every interactive change of a curve's visibility goes through one function, `PlotWidgetBase::setCurveVisible`
(virtual): the curve editor's eye and a legend click (`PlotWidgetBase::eventFilter`). `PlotWidget`'s override adds what
depends on the visible set: it marks the trackers stale, emits `curveVisibilityChanged(source_name, visible)` (the curve
editor re-checks that row's eye, as `curveColorChanged` does for the swatch) and emits `undoableChange`. A call that
changes nothing is a no-op with no undo step, which also stops the editor's eye, re-checked by the signal, from calling
back into the plot. Calling `QwtPlotCurve::setVisible` directly leaves the tracker's value box, the editor and the undo
history out of date. `onChangeCurveColor` likewise marks the trackers stale and emits `curveColorChanged`; its caller
(the editor's picker) emits the undo step.

- **Trackers re-sample only when `trackers_stale_` is set** (curve-list and axis changes, a visibility or colour change,
  a layout restore), at the next replot (`beforeCanvasReplot`), not at once: a curve just added has no window yet and
  sampling it would copy its whole series. A path that changes what a tracker reads (a curve's visibility or pen) must
  set the flag before it replots.
- **XY markers ride the same pass.** An XY plot has no tracker line: each curve's dot (`CurveInfo::marker`) is placed on
  the sample at the last tracker time by `updateXYMarkers`, which `setTrackerPosition` and the stale pass
  (`redrawTrackers`) both run. It reads the curve's visibility and pen at that moment (the fill follows the pen through
  `PlotWidgetBase::syncMarkerColor`), so a hide, a recolour or a restore needs no refresh of its own.
- **A layout restore** sets the pens and visibility of the curves it keeps directly (`applyCurveElement`): no per-curve
  replot, signal or undo step. It marks the trackers stale; the shell refreshes a bound curve editor on re-bind.

## Viewport persistence and the display offset

A time axis is saved in absolute seconds, never in the display-relative seconds the Qwt axis speaks (display = absolute
- offset, `pj_runtime/Time.h`): the display offset is per dataset and live, so a display range frames the right instant
only for the offset present at save time. The authoritative edges are integer nanoseconds (`left_ns` / `right_ns`,
schema v4+): the small display value is quantized to ns first and the epoch-scale offset added in the integer domain,
because a double's ULP at ~1.6e9 s is ~238 ns and would round away a deeply zoomed window. The decimal `left` / `right`
stay for readability and as the v3 fallback. An explicit `x_basis` records what X means ("absolute" for time, "value"
for an XY plot's data value); load never re-infers it from the plot mode.

On load the saved X converts back to the current display coordinate. During a progressive restore the catalog is still
empty, so the conversion is wrong until the dataset binds; the restore re-applies the stashed viewport per curve bind
and at drain (`applySavedViewportOrZoom`) so the plot is framed to its saved window up front instead of auto-fitting
partial data. A window saved zoomed below 1 ns rounds to equal ns edges, and falling through to auto-fit would wedge
undo after a deep zoom, so the collapsed edges are nudged by 0.5 ns, then widened to the ULP-aware minimum when the
display value sits at epoch scale. The plot's representative display offset is that of its first datastore-backed
curve's dataset, the same rule at save and load. Adding a curve appends it, so a curve that binds late in a restore
(its dataset loaded later than another's) is moved back to its saved place while the saved window is still stashed
(`PlotWidget::restoreCurveOrder`, called by `PendingDisplayBinder::flush` before the window is re-applied); otherwise
the legend order and the dataset framing the saved window would follow load order. A global frame change (the "Use time offset" toggle) translates the viewport by the frame delta, and a
per-source change (a Timeline drag) only drops the adapters' offset caches and replots at the current zoom.

A curve is saved by its stable topic + field path plus dataset id, source and path qualifiers, and re-resolved against
the current datasets on load (`PJ::LayoutXml::rebindCurveKeys`); clipboard XML carries the same qualifiers and resolves
three ways (concrete key, qualified miss clears the key, unqualified miss leaves it).

## Docks, drops and tabs

- **`DockWidget`** hosts one plot or one object widget (a scene, a Custom Widget, the state-transitions strip) in an
  ADS dock; `PlotDocker` owns the ADS manager and the dock tree of one tab; `TabbedPlotWidget` owns the tab strip.
  `pj_plotting` knows no scene families: placeholder icons and drops go to the shell through `objectFamilyRequested`
  and the object-widget factory (one factory for drops, with a seed, and layout restore, with the saved XML tag as the
  kind).
- **Drops.** A catalog drop on a mounted object widget is offered to it first (multi-topic widgets such as Scene3D
  absorb more topics); a committed dock refuses a topic of another family rather than replacing its view; a plot dock
  refuses object topics. An advertised (not yet subscribed) placeholder topic fabricates no curve:
  `placeholderTopicDropped` reaches the shell, which registers demand and a pending bind. A discrete-only drop on an
  empty tile materializes the state-transitions strip.
- **Fullscreen.** Maximizing a dock hides every other open dock through ADS' `toggleView(false)` (which collapses the
  emptied splitter branches so the survivor reflows; a raw `setVisible(false)` strands it in nested layouts) and
  records exactly the docks it hid, so exiting restores only those. Splitter sizes are captured on entry and written by
  `xmlSaveState`, so a layout saved while maximized stores the split the user returns to. Leave fullscreen before
  anything that changes the dock tree.
- **Widget tabs and history.** A tab can host an arbitrary widget (`addWidgetTab`): never serialized, kept across
  `xmlLoadState`, with an optional close veto; closing one is not an undo step. History-exempt plot tabs persist in
  layout files but are omitted from undo snapshots, which keep their live page. `duplicateTab` copies a plot tab through
  its XML with fresh state ids, so the copy never aliases the source in id-keyed lookups. That XML is restored inside
  `pj_plotting`, without the shell's `rebindCurveKeys`, so the source plots first stamp their live curve keys into it
  (`PlotWidget::stampLiveCurveKeys`, as a clipboard copy does); without them every copied plot comes up empty.
  `addTab` announces the copy before it is filled, so `currentTabChanged` fires again once it is: listeners such as
  the shell's curve editor would otherwise keep rows built before the restored visibility and markers.
- **Tab order.** The strip layout, `tabs_` and the saved `<Tab>` sequence share one order. A restore rebuilds the
  serialized plot tabs in document order, then re-appends the pages outside its scope: an undo or redo re-appends the
  widget and history-exempt pages, a layout load only the widget pages. Dragging a plot tab moves it past each
  neighbour whose midpoint the cursor crosses, along the layout direction (a wide and a narrow tab do not swap back and
  forth under a still cursor). On release, or on a press that shows the release was lost, it emits one `undoableChange`
  if the undo snapshot's tab order changed. Widget and history-exempt tabs cannot be dragged, since the next undo would
  move them back; a plot tab dragged past one moves it only until then.
- **Layout restore** parses and validates the whole structural tree before changing the live docker, restores splitter
  ratios as large relative weights (the docker is not laid out at its final size yet, so pixel scaling would distort the
  split), and emits no undo step for its intermediate states. It restores into the live plot page whose tab id
  matches and, inside it, into the plot whose id matches (else the next unused one), so an undo or reload keeps the
  plots it does not change: a curve already present keeps its adapter and sample window. Only object docks (scenes,
  strips, custom widgets) and the dock frames are always rebuilt. Because widgets survive a restore, the shell's
  per-plot and per-tab wiring must stay idempotent (`Qt::UniqueConnection`, no lambda connections), and a reused object
  must drop what a new one would not have: the page's focused dock (it points at a removed dock), armed undo debounces,
  a curve order that differs from the saved one (the plot then rebuilds its curves). A bound curve editor is refreshed
  on re-bind, since the restore changes colour and visibility without a curve-list signal. Selecting a tab is not an
  undo step, so an undo or redo keeps the tab the user is on when it survives, and otherwise shows the nearest plot
  page on its left on the strip, where the undone step started; a layout load selects the saved one. Closing the
  current tab shows the tab to its right on the strip (else its left), not the next page in creation order.

## PlotRhiCanvas

The GPU plot canvas on desktop and in the browser: a `QRhiWidget` (any backend) installed as the Qwt canvas by
`PlotWidgetBase`. Qwt still owns axes, data and interaction. It renders the visible Qwt curves, grid and markers as
bounded triangle geometry and rasterises everything else with the item's own QPainter drawing into one texture atlas. On
desktop `Preferences::use_opengl` ("GPU canvas") and `--disable-opengl` select the raster `QwtPlotCanvas` instead; the
browser always uses this canvas.

### Files

In `widget/src/`, private to the module, namespace `PJ::plot_rhi`:

- `PlotRhiGeometry.{h,cpp}` builds the geometry layers (clipping, strokes, dashes, symbols, grids, curves, markers, the
  frame) and the curve layer's cache keys, and holds the frame budgets.
- `PlotRhiTextAtlas.{h,cpp}` records, caches, rasterises and packs the text layers.
- `PlotRhiCanvas.cpp` owns the widget lifecycle, the rebuild decisions, the GPU resources and uploads, the draw
  submission and failure reporting.

The public header carries no implementation types: the retained layers' bookkeeping sits behind one
`plot_rhi::RetainedLayers` pointer.

### Backends

Shaders are committed multi-backend packs (`widget/shaders/*.qsb`, baked by `widget/shaders/bake.sh <qsb>`: GLSL ES 300,
GLSL 120/150/330/440, HLSL 50, MSL 12, SPIR-V); desktop kits ship no `qsb`, and the WASM configure re-bakes them and
fails on any byte difference. The shaders pass positions through: `logicalToClip()` folds `QRhi::clipSpaceCorrMatrix()`
(the Vulkan y flip) into the CPU vertex transform. Atlas uv (0,0) is the first uploaded row on every backend. Set the
viewport after binding each pipeline: Vulkan draws nothing otherwise.

### Budgets

In `PlotRhiGeometry.h`, working in device pixels:

- The reducer keeps up to 4 extrema per device-pixel column (floor `kMinDensitySamples`).
- Desktop gives every curve that density and caps a frame at `kMaxFrameVertices` (8M vertices, 16 B each: 128 MB, plus
  up to three 4 B indices per vertex); the browser splits one density budget across all curves and caps at 1M vertices
  (16 MB, 32-bit heap).
- Per-curve vertex shares and the rollback-and-halve retry keep a curve from committing a partial prefix; a limit hit
  logs once per onset.
- The geometry layers are indexed (`drawIndexed`, 32-bit indices). A solid polyline is one strip of corner pairs shared
  by consecutive quads and their bevels (the bevel adds only its join point: 5 vertices and 9 indices per turning
  segment, 2 and 6 on a straight join; a collinear reversal starts a pair of its own, its normal flipped); fans, rings,
  boxes and dashed quads share their corners too.
- The arrays are built in `QByteArray`s handed to `uploadStaticBuffer()` by implicit sharing (no copy).

### Antialiasing

One sample per pixel into a colour-only target the canvas owns (`setAutoRenderTarget(false)`, `PJ::RhiColorTarget`: no
depth-stencil; 4 B per device pixel).

- Every stroke, fan, ring and box reaches half a device pixel past its true edge and carries its position across
  (`plot_rhi::Vertex::aux`); `plot_color.frag` ramps coverage over that one device pixel from screen-space derivatives.
- Strokes under one device pixel are drawn one pixel wide at proportionally lower alpha, as Qt's antialiased raster
  does.
- Horizontal and vertical `addLine()` lines (grid, marker lines, sticks, symbol strokes) and the Rect/Cross/HLine/VLine
  symbols sit on whole pixels like Qwt's (`snapToPixels()`: the logical coordinate rounded, then centred on its device
  pixel for an odd device width, on a pixel edge for an even one); curves keep exact coordinates.
- `setSampleCount(n > 1)` still works (an MSAA colour buffer and its resolve, still no depth-stencil), for comparisons
  only.

### Dashes

Cut in the fragment shader from the path distance carried per vertex (16 bits, 1/64 pen width; a clipped segment longer
than 960 pen widths, `kMaxDashQuadUnits`, is split into several quads), so a dashed segment is one quad. Qt's patterns
(Dash 4/2, Dot 1/2, DashDot, DashDotDot, in pen widths): a cosmetic pen's pattern is sampled per pixel with no caps, as
`QCosmeticStroker` does; a wide pen's square caps lengthen every dash by half the width, antialiased. The phase follows
the source path through the reducer; a turn gets its bevel only where a dash runs through it. Custom dash patterns draw
solid.

### Dirty layers

Three retained vertex buffers: grid + curves + frame; the overlay (every `QwtPlotMarker`: the tracker, hover and
ride-along markers); and text.

- `replot()` dirties the overlay, and the curve layer unless the data of every curve it draws (any `QwtPlotCurve`
  subclass) is a `RevisionedSeries` whose revision is unchanged. The datastore adapters and `PointSeriesXY` call
  `bumpContentRevision()` on every commit, clear, display-offset move, filter change and new rect of interest; plain Qwt
  data rebuilds on every replot. Revisions come from one process-wide counter, so the content key holds the revisions
  alone, and a series re-created at a freed one's address never repeats one.
- Inside `setOverlayOnlyReplot(true)` (what `PlotWidgetBase::replotOverlays()`, and so
  `PlotWidget::setTrackerPosition()`, wraps around one replot) only the overlay is dirtied.
- The curve layer is rebuilt anyway when the view (size, dpr, axis bounds, maps, each grid tick list with its type,
  clear or frame colour), its structure (items, visibility, pens, brushes, styles, symbols, axes, grid flags, data
  object and size) or a curve's content revision changed. A map's transformation counts by type and parameter (none,
  `QwtLogTransform`, `QwtPowerTransform`); any other transformation rebuilds every frame.
- Any geometry rebuild regathers the text layers, reusing the cached rasters, and re-uploads the atlas only when the
  layer set changed (the canvas size, the truncation and the layer keys in draw order); a reused atlas still re-owns its
  quads to this build's items, so an identical replacement item keeps its text.

### Text and GPU resets

Each visible legend, label or raster item's QPainter drawing is recorded into a `QPicture`, rasterised into a tight
DPR-scaled CPU image, and the images are packed into one atlas upload. A CPU raster is cached only while its recorded
drawing (every command, font, colour and position), raster rectangle and dpr are unchanged, so a tracker move
re-rasterises its value box and nothing else. `releaseResources()` drops the texture, bindings, pipeline, text buffer
and text cache: no QRhi text resource survives context teardown.

### Items

- **Geometry:** `QwtPlotCurve` (Lines, Sticks, Steps, Dots, `PlotCurve`'s Lines-and-Dots square dots read from
  `PlotCurve::dotWidth()`), `QwtPlotGrid`, `QwtPlotMarker` lines and symbols; symbols Ellipse, Rect, Diamond, the four
  triangles, Hexagon, Cross, XCross, HLine, VLine, Star1.
- **Raster layer** (the item's own QPainter drawing, in its Z slot): `PlotLegend`, marker labels, `PlotMarkersItem`
  and any other `QwtPlotItem`.
- **Drawn approximately**, with a once-per-process `pj.plotting.rhi` warning: other symbol styles (Star2,
  path/pixmap/graphic/SVG/user; a filled square), the `UserCurve` style (symbols only), curve area brushes (not
  filled), `Fitted` curves (unfitted). PJ4 produces none of these today: every curve style (`applyStyleToCurve`, the
  six layout-XML `style` values) sets no symbol, brush, `UserCurve` or `Fitted`; every marker symbol is an `Ellipse`;
  the SDK has no curve-style API; the dialog chart preview is its own raster `QwtPlot`. A new curve style that needs
  one of these must add it to the geometry path first.
- The 1 px canvas frame (the Qwt canvases' `QFrame::Box | Plain` in the palette's WindowText) is drawn over every item.

### Lifecycle

`initialize()` (re)creates the render target on the widget's current colour buffer on every call (a resize rebuilds that
buffer in place), keeping the render pass descriptor while it stays compatible. Pipelines are rebuilt when the QRhi, the
render pass descriptor (address or `serializedFormat()`) or the sample count changes, and `render()` re-checks the QRhi
(a grab of a hidden widget brings a QRhi without `initialize()`). `grabPresentedFrame()` grabs after the next presented
frame. `PlotPanner` snapshots the canvas for its transient drag image with the synchronous `grabFramebuffer()`
(`QWidget::grab()` cannot capture QRhi content); with no uniforms, the grab-only stale-uniform issue
(`pj_scene3D/docs/TECHNICAL_NOTES.md` "Qt workarounds") does not apply.

### Failure

A pipeline or vertex buffer `create()` failure puts the canvas in a failed state: clear-only passes (no draw ever
references a missing resource), the "GPU plot renderer unavailable" banner, and `renderFailed()` once per onset
(QRhiWidget's own "No QRhi" emission is not echoed). The pipelines are retried only in the next `initialize()` (new
QRhi, new or resized render target), never per frame. On desktop `PlotWidgetBase` answers `renderFailed()` (queued: it
can fire inside `render()`) by swapping that plot to the raster `QwtPlotCanvas` for good, moving the magnifier,
panners and zoomer (their enabled state kept) and re-installing every canvas event filter in the original order,
`installHoverFilter()` ones included; the swap waits while a popup or modal window is open, because it deletes the
canvas the event came from. The browser keeps the banner, although Qwt's raster canvas is compiled there too. A text
atlas failure only drops text, with a one-time warning.

## Render tests and benchmarks

### `plot_rhi_render_test`

Renders each item kind through a real `PlotRhiCanvas` and compares the presented frame with Qwt's own QPainter rendering
of the same canvas (its items drawn as `QwtPlot::drawItems` does): curves, overlapping translucent curves, dashes, every
supported symbol, Lines and Dots, marker line/symbol/label, the legend, `PlotMarkersItem`, a reparent to a new top-level
+ resize + sample-count change, and the panner grab.

- **Comparison.** The foreground colours (at least 98% of the pixels non-background in either image must be within 48
  per channel of a reference pixel in its 3x3 neighbourhood), missing and extra geometry both ways, and an exactly-clear
  background away from the reference's geometry. The reference snaps the grid and markers to whole pixels as Qwt does on
  screen (`QwtPainter::roundingAlignment()`) and draws curves unsnapped, as the canvas does.
- **Pinned behaviour.** Grid and marker lines on whole device pixels with Qt's cosmetic cadence (dotted 1 on / 2 off,
  dashed 4 on / 2 off) and the 1 px frame; the edge coverage ramp (each device column of a wide sloped line holds its
  exact coverage, within 0.25 px, with at most two partial pixels per edge); every dashed style, cosmetic and 2 px
  wide, against Qwt's dashes; a tracker move that rebuilds neither the curve layer nor the legend's raster
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

### Benchmarks

Built with `PJ_BUILD_DEMOS`, independently of `PJ_BUILD_TESTS`:

- `adapter_streaming_benchmark`: the datastore adapters under streaming.
- `plot_canvas_benchmark --canvas rhi|raster --case NAME` (desktop only) measures a canvas's frame cost (CPU, wall, the
  vertices built, the process's DRM GPU memory) on the static / streaming / zoom-pan / many-curves / dashed /
  tracker-only workloads; `--samples`, `--plots K`, `--grid`, `--legend` and `--msaa N` vary the scene, and `--gpu` adds
  GPU time (timestamp queries, which cost CPU).

The performance baseline is in `pj_scene3D/docs/TECHNICAL_NOTES.md` "Performance baseline".

## Known gaps

- Custom dash patterns, other symbol styles, area brushes and `Fitted` curves draw approximately on `PlotRhiCanvas`
  (see "Items"); nothing in PJ4 produces them today.
- `DatastoreCurveAdapter`'s virtual-index lookups are wrong when chunks overlap in time (sizing only).
- When the GPU canvas fails in the browser there is no raster fallback: the banner stays.
- The raster fallback after a GPU failure waits for a popup or modal window but does not detect a `QDrag::exec` loop
  started from the canvas.
- A curve restored from a layout whose data has not arrived yet does not reserve its saved colour (it waits in
  `PendingDisplayBinder`, which the plot cannot see), so a curve added to that plot meanwhile can take the same colour.
- After the first fit of a new plot the canvas can stay a few pixels off its final place (observed in the browser: 4 px
  at the top) until the next full `replot()`, such as a hover or a tracker update; anything reading the canvas
  geometry before that reads the unsettled one.
