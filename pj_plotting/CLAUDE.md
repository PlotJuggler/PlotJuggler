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
| `core/` | `DatastoreCurveAdapter`, `FilteredCurveAdapter`, `PointSeriesXY`, `StateSeriesAdapter`, `PlotXml.h` | Bridges PJ3-style `QwtSeriesData<QPointF>` consumers to `pj_datastore::DataReader`. `StateSeriesAdapter` is the discrete-series counterpart: a full-rebuild RLE reader (`rangeQuery` + per-type label formatting — `readString` copied out inside the cursor, integers in decimal via the exact int64/uint64 reads, bool via `readBool` as "true"/"false"; equal-timestamp last-write-wins, null = gap, trailing run open) feeding the State Transitions strip. `FilteredCurveAdapter` is a lazy `DatastoreCurveAdapter` subclass that serves the output of a `proc::DataProcessor` (the Filter Editor before/after preview, refreshed on the streaming commit path). `PlotXml.h` is the shared plot-XML vocabulary (`x_basis` range markers, the `pending_intent` keep-alive predicate) consumed by both this module and the app-side layout passes. No Qt Widgets. |
| `widget/` | `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, `PlotZoomer`, `PlotPanner`, `PlotMagnifier`, `CurveTracker`, `PlotLegend`, `PlotFocusOverlay`, `PlotScaleDraw`, `DockWidget`, `DockToolbar`, `CurveEditor`, `FilterEditorPanel`, `ParameterForm`, `StateTransitionsController`, `StateTransitionsDockWidget`, `PlotRhiCanvas` | The Qt/Qwt widgets, ported from PJ3's `plotjuggler_app/`. On WebAssembly (`PJ_TARGET_WASM`) `widget/` also owns `PlotRhiCanvas` — a QRhi/WebGL scalar canvas that renders the visible Qwt curve/grid/marker items as bounded triangle geometry; `PlotWidgetBase` picks it in place of `QwtPlotOpenGLCanvas` (Qwt still owns axes/data/interaction). The WASM `PlotPanner` snapshots that canvas through `grabFramebuffer()` for its transient drag image because `QWidget::grab()` cannot capture QRhi content. Desktop stays on the Qwt OpenGL canvas and Qwt's native panner grab path. |
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

`adapter_streaming_benchmark` is built with `PJ_BUILD_DEMOS`, independently of
`PJ_BUILD_TESTS`.

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
- **Qwt is external and never patched** (`3rdparty/qwt/README.PJ4.md`: FetchContent of the pinned release, or the conda-forge package under `PJ_SYSTEM_QWT`): the PJ3-era in-tree Qwt patches are realized here instead — `PlotScaleDraw` (fixed-notation tick labels, installed on every axis by `QwtPlotPimpl`) and the "Lines and Dots" style (`PlotCurve::drawCurve`: Lines plus one batched, pixel-weeded `drawDots` pass — per-point QPainter calls are pathological on the OpenGL paint engine; on WASM `applyStyleToCurve` sets an explicit `QwtSymbol` instead, which `PlotRhiCanvas` turns into geometry). Do not reintroduce patches under `3rdparty/qwt/src`.
- **On-canvas text goes through `RasterTextEngine`** (`widget/include/pj_plotting/RasterTextEngine.h`, installed at app startup): the GL canvas's glyph atlas does not survive GPU resets or context recreation, so legend/tracker text is CPU-rasterized and re-uploaded per draw. Do not draw canvas text with raw `QPainter::drawText`, and do not cache the rasterized images — a resident GL texture is exactly what a GPU reset blanks.
- **The WASM QRhi canvas follows the same reset contract**: it asks Qwt to draw each visible legend/label into a tight DPR-scaled CPU raster, packs those temporary images into one upload, and drops the texture, bindings, pipeline, and text buffer in `releaseResources()`. Keep text changes on the canvas dirty/replot path; never retain the CPU rasters or let a QRhi text resource survive context teardown.
