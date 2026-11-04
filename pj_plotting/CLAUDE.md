# pj_plotting

## Purpose

Qwt-based plotting module: the PJ4 home for time-series plots, dockers, zoomers, trackers, legends, the curve editor,
the filter editor panel and the GPU plot canvas (`PlotRhiCanvas`). This is the **largest PJ3 wholesale lift** in the
repo. It also owns the plot-area **State Transitions** family (discrete-series state strips for string / integer /
bool fields and floats holding only whole numbers; not Qwt): the `StateSeriesAdapter` reader in `core/` and the
`StateTransitionsController`/`StateTransitionsDockWidget` binding in `widget/`, over the reusable
`StateTransitionsView` from `pj_widgets`.

Design that spans several classes (markers on a plot, the canvas internals, the render-test harness):
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md). Each header's doc-comment owns its API contract.

## Module layout

Two targets with a strict direction:

```
pj_plotting  ──►  pj_plotting_core  ──►  pj_runtime + pj_datastore + pj_base
   │                   │
   │                   └──►  Qwt (external, never patched — see 3rdparty/qwt/README.PJ4.md)
   │
   └──►  pj_runtime, pj_scripting, pj_widgets, ADS (`qt_advanced_docking`), Qt Widgets/Gui/Xml
         (private: pj_datastore, Qt6::GuiPrivate)
```

| Subdir | Contents |
|---|---|
| `core/` | No Qt Widgets. `DatastoreCurveAdapter` (PJ3-style `QwtSeriesData<QPointF>` over `pj_datastore::DataReader`), `FilteredCurveAdapter` (a `proc::DataProcessor`'s output, the Filter Editor preview), `PointSeriesXY`, `StateSeriesAdapter` (discrete-series run-length reader), `RevisionedSeries.h` (the content-revision contract the canvas caches on), `PlotXml.h` (plot-XML vocabulary shared with `pj_app`'s layout passes). |
| `widget/` | The Qt/Qwt widgets, ported from PJ3's `plotjuggler_app/`: `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, `PlotZoomer`, `PlotPanner`, `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotCurve`, `PlotMarkersItem`, `PlotFocusOverlay`, `PlotScaleDraw`, `DockWidget`, `DockToolbar`, `CurveEditor`, `XYCurveDialog`, `FilterEditorPanel`, `ParameterForm`, `StateTransitionsController`, `StateTransitionsDockWidget`, `PlotRhiCanvas` (private implementation in `widget/src/PlotRhi*`, shaders in `widget/shaders/`). |
| `tests/` | gtest runners and the QRhi render harness. |
| `benchmarks/` | `adapter_streaming_benchmark`, `plot_canvas_benchmark` (built with `PJ_BUILD_DEMOS`). |

Derived series (script outputs) reach this module as ordinary datastore topics, through the catalog; see
`docs/derived_objects/ARCHITECTURE.md` §15 (series) and §17 (scene integration).

## Rules and pitfalls

- **Qwt is external and never patched** (`3rdparty/qwt/README.PJ4.md`). PJ-specific Qwt behaviour lives here:
  `PlotScaleDraw` (fixed-notation tick labels, installed on every axis) and the "Lines and Dots" style
  (`PlotCurve::drawCurve`: Lines plus one batched, pixel-weeded `drawDots` pass, because per-point QPainter calls are
  pathological; `PlotRhiCanvas` reads `PlotCurve::dotWidth()` and draws the same dots as geometry). Never add patches
  under `3rdparty/qwt/src`.
- **Keep the `Preferences::use_opengl` gate** ("GPU canvas", plus `--disable-opengl`), which selects the raster
  `QwtPlotCanvas` on desktop: software-raster rendering is CPU-heavy, so do not remove the GPU default without
  measuring. The browser always uses `PlotRhiCanvas`.
- **Never make the plot canvas a `WA_NativeWindow`** inside Qt-Advanced-Docking: the native-flag conflict breaks
  layout.
- **A series the canvas should cache must be a `RevisionedSeries`** and call `bumpContentRevision()` on every change
  to what it serves (commit, clear, display-offset move, filter change, new rect of interest). A missed bump draws
  stale curves; plain Qwt data rebuilds the curve layer on every replot. Tracker-only updates go through
  `PlotWidgetBase::replotOverlays()` so the curve layer is not rebuilt.
- **Shader edits:** re-bake the `.qsb` packs with the pinned `qsb` (`widget/shaders/bake.sh <qsb>`) and commit both,
  or the WASM configure's byte check fails.
- **QRhi canvas resources:** never let a text resource survive `releaseResources()` (GPU reset / context teardown);
  set the viewport after binding each pipeline (Vulkan draws nothing otherwise); `renderFailed()` can fire inside
  `render()`, so a receiver that replaces the canvas connects queued. Use `grabFramebuffer()` /
  `grabPresentedFrame()` to capture the canvas: `QWidget::grab()` cannot capture QRhi content.
- **Marker time frames:** the `__all__` marker family lives on `kAllDatasetsMarkerDataset` in the display frame, so
  map it through the global time origin only, never a dataset's alignment offset.
- **The runtime owns marker naming:** recognize marker topics through `pj_runtime/MarkerTopics.h`
  (`markerTopicsOf`, `scanMarkerTopics`, `publishedMarkerScopes`), never by parsing topic names. The plot knows no
  engine: the host feeds `setMarkerStatus` / `setSeriesStatus` with plain data.
- **`TabbedPlotWidget::forEachDocker` / `findDocker`** enumerate the docker tabs in one pass; use them instead of
  index loops over `dockerCount()`/`dockerAt()` (each a linear scan).
- **`visibleXRangeChanged`** reports every move of the time axis; `rectChanged` drives the zoom link only.
- **Object-dock drop hover** consults `IDataWidget::canAcceptObjectTopic` before
  accepting catalog drags. A view's payload-dependent refusal must be visible in the cursor.
- **Open Settings / Close Settings** in an object dock's context menu emits
  `settingsPanelToggleRequested` through `PlotDocker`; the shell focuses that dock
  and toggles its settings sidebar. The host-supplied visibility query chooses the
  label when the menu opens.

## Port strategy (root CLAUDE.md "Porting policy")

This module is the canonical example of the **wholesale lift**:

1. **Lift PJ3 files close to verbatim** from `${PJ3_SOURCE_DIR}/plotjuggler_app/`. Class names map directly (PJ3
   `PlotWidget` → PJ4 `PlotWidget`).
2. **Apply plotjuggler_sdk style** on the way in, and **preserve `.ui` `objectName` values verbatim**.
3. **Rebind data reads only:** PJ3's `PlotDataMapRef` / `TransformsMap` consumption becomes
   `DatastoreCurveAdapter` over `pj_datastore::DataReader`. Do not rewrite plotting logic from scratch.

## Cross-module rules

- Sibling of `pj_scene2D/widgets` and `pj_scene3D/widgets`: **they never depend on each other.**
- Shared runtime state flows through `pj_runtime::IDataWidget` (so `PlaybackEngine` drives tracker updates without
  coupling to plot internals).
- Reusable Qt controls that could serve another widget family go in `pj_widgets`, not here.

## Tests

`PJ_BUILD_TESTS` enables, through `pj_add_test_runner` (one process per case, runner name as ctest prefix):

- `pj_plotting_core_tests` (shared `QCoreApplication` main): the core adapters.
- `pj_plotting_widget_tests` (shared `QApplication` main, with `pj_resources`): widgets, docking, filter editor,
  markers. The transaction and legend-diagnostics suites use file-local environments that apply their setup only
  when those suites are selected.
- `plot_rhi_render_test`: the canvas against Qwt's own QPainter rendering, registered on every backend of the
  platform plus `.opengl_dpr2` (`RHI_BACKENDS DPR2`). It needs a real display (xvfb or a Wayland compositor on
  Linux), skips on `offscreen`, and fails instead under `PJ_REQUIRE_GPU_TESTS=1`. What it pins:
  [docs/ARCHITECTURE.md §4](./docs/ARCHITECTURE.md#4-render-tests-and-benchmarks).

```bash
./test.sh -R '^pj_plotting_(core|widget)_tests\.'
```
