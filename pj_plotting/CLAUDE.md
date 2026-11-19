# pj_plotting

Qwt-based plotting module: time-series plots, dockers, zoomers, trackers, legends, the curve editor, the filter editor
panel and the GPU plot canvas (`PlotRhiCanvas`). It also owns the plot-area State Transitions family (not Qwt):
discrete-series state strips for string, integer and bool fields and for floats holding only whole numbers, through the
`StateSeriesAdapter` reader in `core/` and the `StateTransitionsController` / `StateTransitionsDockWidget` binding in
`widget/`, over the reusable `StateTransitionsView` from `pj_widgets`. How the designs that span several classes work:
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md). Each header's doc-comment owns its API contract.

## Layout

```
pj_plotting  ──►  pj_plotting_core  ──►  pj_runtime + pj_datastore + pj_base
   │                   └──►  Qwt (external, never patched: 3rdparty/qwt/README.PJ4.md)
   └──►  pj_runtime, pj_scripting, pj_widgets, ADS (`qt_advanced_docking`), Qt Widgets/Gui/Xml
         (private: pj_datastore, Qt6::GuiPrivate)
```

- `core/` (no Qt Widgets): `DatastoreCurveAdapter` (a `QwtSeriesData<QPointF>` over `pj_datastore::DataReader`),
  `FilteredCurveAdapter` (a `proc::DataProcessor`'s output: the Filter Editor preview), `PointSeriesXY`,
  `StateSeriesAdapter`, `RevisionedSeries.h` (the content-revision contract the canvas caches on), `PlotXml.h` (the
  plot-XML vocabulary shared with `pj_app`'s layout passes).
- `widget/`: the Qt/Qwt widgets. Plots: `PlotWidgetBase`, `PlotWidget`, `PlotCurve`, `PlotZoomer`, `PlotPanner`,
  `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotMarkersItem`, `PlotScaleDraw`, `PlotRhiCanvas` (private
  implementation in `widget/src/PlotRhi*`, shaders in `widget/shaders/`). Docking: `PlotDocker`, `TabbedPlotWidget`,
  `DockWidget`, `DockToolbar`, `PlotFocusOverlay`. Panels: `CurveEditor`, `XYCurveDialog`, `FilterEditorPanel`,
  `ParameterForm`, `StateTransitionsController`, `StateTransitionsDockWidget`.
- `tests/` (gtest runners and the QRhi render harness), `benchmarks/` (`PJ_BUILD_DEMOS`).

Derived series reach this module as ordinary datastore topics through the catalog
([docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md) "Series", "Scene integration").

## Invariants

- **Qwt is never patched.** PJ-specific behaviour lives here: `PlotScaleDraw` (fixed-notation tick labels on every
  axis; with the date-time toggle on, a time plot's X axis swaps to `PlotTimeScaleDraw` + `PlotTimeScaleEngine`, a
  seconds adapter over Qwt's millisecond `QwtDateScaleEngine`, through `PlotWidgetBase::setTimeLabelFormat`) and the
  "Lines and Dots" style (`PlotCurve::drawCurve`: Lines plus one batched, pixel-weeded `drawDots` pass, since
  per-point QPainter calls are pathologically slow; `PlotRhiCanvas` reads `PlotCurve::dotWidth()` and draws the same
  dots as geometry). A patch under `3rdparty/qwt/src` breaks the pinned-release build.
- **Curve adapters never decimate.** Point reduction happens at paint time (Qwt's weeding, `PlotCurve::drawCurve`, the
  canvas reducer); decimating in an adapter breaks zoom.
- **A series the canvas should cache is a `RevisionedSeries` and calls `bumpContentRevision()` on every change** to
  what it serves (commit, clear, display-offset move, filter change, new rect of interest). A missed bump draws stale
  curves; plain Qwt data rebuilds the curve layer on every replot. Tracker-only updates go through
  `PlotWidgetBase::replotOverlays()` so the curve layer is not rebuilt.
- **Keep the `Preferences::use_opengl` gate** ("GPU canvas", `--disable-opengl`), which selects the raster
  `QwtPlotCanvas` on desktop; the browser always uses `PlotRhiCanvas`. Reasoning, not measured here: software-raster
  rendering is CPU-heavy, so do not drop the GPU default without measuring.
- **Do not make the plot canvas a `WA_NativeWindow` inside Qt-Advanced-Docking:** it was found to break the dock layout
  (not re-verified).
- **Vendored ADS menus stay unreachable in the browser.** ADS opens its tab, title-bar and auto-hide-tab menus with `QMenu::exec()`, which aborts the browser build. They never open because `PlotDocker`'s `HiddenTitleBar` is forced invisible (the tabs and title-bar menu live inside it; `DockToolbar` forwards mouse events to it, never context-menu events) and auto-hide is never enabled (`CDockManager::AutoHideFeatureEnabled` is off by default, so no `CAutoHideTab` exists). Showing the ADS title bar or enabling auto-hide needs those menus replaced first.
- **QRhi canvas:** a text resource never survives `releaseResources()` (a GPU reset or context teardown reuses it
  stale); set the viewport after binding each pipeline (Vulkan draws nothing otherwise); `renderFailed()` can fire
  inside `render()`, so a receiver that replaces the canvas connects queued (the swap deletes the canvas under the
  signal). To capture the canvas use `grabFramebuffer()` / `grabPresentedFrame()`: `QWidget::grab()` cannot capture
  QRhi content.
- **Shader edits** re-bake the `.qsb` packs with the pinned `qsb` (`widget/shaders/bake.sh <qsb>`) and commit both, or
  the WASM configure's byte check fails.
- **Marker time frames:** the `__all__` marker family lives on `kAllDatasetsMarkerDataset` in the display frame, so
  map it through the global time origin only, never a dataset's alignment offset, or its markers shift with the
  dataset.
- **The runtime owns marker naming:** recognize marker topics through `pj_runtime/MarkerTopics.h` (`markerTopicsOf`,
  `scanMarkerTopics`, `publishedMarkerScopes`), never by parsing topic names. The plot knows no engine: the host feeds
  `setMarkerStatus` / `setSeriesStatus` plain data.
- **A time axis persists in absolute time**, with integer-nanosecond edges (`left_ns` / `right_ns`) as the authority and
  an explicit `x_basis`; display-relative seconds would restore the wrong window under another display offset.
- **`TabbedPlotWidget::forEachDocker` / `findDocker`** enumerate the docker tabs in one pass; an index loop over
  `dockerCount()` / `dockerAt()` is quadratic (each is a linear scan).
- **The XY keep-ratio fit only widens an axis**, so it always starts from the requested view
  (`PlotWidgetBase::applyRectKeepingRatio` keeps it), re-fitted on every plot resize and persisted through
  `requestedView()`. Fitting or saving the axes as shown instead zooms out a little on each resize, and a layout
  restored at startup or under another window shape comes back wider.
- **A new curve's colour skips every colour its plot already wears** (`PlotWidgetBase::freePaletteIndex`, read from the
  curves' pens, so restored and user-picked colours count). The session counter and `CurveColorRegistry` know only
  what they handed out, and a layout restore sets colours without going through them, so a plain counter repeats a
  restored colour. A colour the registry remembers for that curve is still honoured as it is.
- **`visibleXRangeChanged`** reports every move of the time axis; `rectChanged` drives the zoom link only.
- **Object-dock drop hover** consults `IDataWidget::canAcceptObjectTopic` before accepting a catalog drag, so a view's
  payload-dependent refusal shows in the cursor. "Open Settings" / "Close Settings" in an object dock's context menu
  emits `settingsPanelToggleRequested` through `PlotDocker`; the host-supplied visibility query picks the label.
- **pj_plotting stays agnostic to scene families:** placeholder icons and drops go to the shell through
  `objectFamilyRequested` and the object-widget factory; the shell alone maps families to kinds.

## Cross-module rules

- A sibling of `pj_scene2D/widgets` and `pj_scene3D/widgets`: they never depend on each other.
- Shared runtime state flows through `pj_runtime::IDataWidget` (so `PlaybackEngine` drives tracker updates without
  coupling to plot internals).
- A reusable Qt control that could serve another widget family goes in `pj_widgets`, not here.

## Tests

`PJ_BUILD_TESTS` enables, through `pj_add_test_runner` (one process per case, runner name as the ctest prefix):

- `pj_plotting_core_tests` (shared `QCoreApplication` main): the core adapters.
- `pj_plotting_widget_tests` (shared `QApplication` main, with `pj_resources`): widgets, docking, filter editor,
  markers.
- `plot_rhi_render_test`: the canvas against Qwt's own QPainter rendering on every backend of the platform plus
  `.opengl_dpr2`. It needs a real QPA in `QT_QPA_PLATFORM` (`./test.sh --display`: a private Xvfb on Linux), skips on
  `offscreen` and fails instead under `PJ_REQUIRE_GPU_TESTS=1`
  ([docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md) "Render tests and benchmarks").

```bash
./test.sh -R '^pj_plotting_(core|widget)_tests\.'
```

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): core adapters, markers on a plot, `PlotRhiCanvas`, docks and
  persistence, render tests and benchmarks, known gaps.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): measurements and platform findings behind the choices.
