# pj_plotting technical notes

What was observed, measured or reasoned behind the current code, with the conditions where they were recorded and the
change that would make it worth looking again. How the code works is in [ARCHITECTURE.md](./ARCHITECTURE.md). The
canvas performance baseline is in `pj_scene3D/docs/TECHNICAL_NOTES.md` "Performance baseline".

## Window cache

- **Observed (profiles, no numbers kept):** re-walking the engine on every zoom or pan step was the top GUI-thread cost
  while zooming, and resolving the display offset per sample (two `DataEngine` lookups each) dominated per-curve paint
  cost.
- **What the code does:** `DatastoreCurveAdapter` materializes the visible span widened by one span per side and
  re-narrows it by binary search, and caches the display offset until an invalidation. A commit that only appended in
  order is absorbed in place.
- **Re-test:** when the adapter's read path or the engine's per-sample cost changes.

## Max zoom area on a layout restore

- **Measured (2026-11-18, i7-13700H, offscreen, `UndoRedoLatencyBenchmark` in `pj_app`'s history tests, `PJ_BENCH=1`):**
  `updateMaximumZoomArea` reads every sample of every curve (`SeriesReader::bounds`). Recomputed on every curve change,
  a restored plot of n curves read about n(n+1) series: an undo of 16 plots of 5 curves of 100k samples took 1.74 s,
  69% of it in this recompute.
- **What the code does:** `PlotWidget::xmlLoadState` swaps the curve set with the `curveListChanged` recompute
  suspended, then recomputes once: 0.83 s for the same undo.
- **Re-test:** when `bounds` stops reading every sample, or `addCurve` gains another caller that adds curves in bulk.

## Canvas vertex budgets

- **Reasoning, not measured:** 1,000,000 vertices at 16 B (about 16 MB, plus at most three 4 B indices per vertex) is a
  firm cap against exhausting the 32-bit wasm heap (a few hundred MB shared with Qt, Qwt and the datastore) while still
  far exceeding any legible line density. On desktop 8M vertices (128 MB of vertices and up to 96 MB of indices, on the
  CPU and the GPU each) holds 50 full-density Lines curves on a 4K-wide canvas; the cap only guards pathological symbol
  expansion. Reducer planning leaves a quarter of the ceiling for grids, markers, dash expansion and estimator error.
- **Browser sample mapping:** the 32-bit heap cannot hold a mapped copy of a multi-million-sample series (16 B per
  sample), so the browser maps lazily on every reducer pass. Desktop maps each curve once into a scratch that is
  released after a rebuild that grew it past 1 MiB (`kRetainedMappedSamples`).
- **Re-test:** when the wasm heap limit changes or a plot with hundreds of curves is a target.

## Upload in pieces

- **Observed:** on Mesa's `anv` Vulkan driver a buffer copy recorded with an awkward size is slow, so
  `PlotRhiCanvas` uploads its geometry through `uploadStaticBufferInPieces` (`pj_widgets/GraphicsApi.h`) instead of one
  `uploadStaticBuffer`. Driver version and sizes were not recorded.
- **Re-test:** on a newer Mesa, with the same upload through `uploadStaticBuffer`.

## Static GPU buffers

- **Reasoning:** the geometry is retained across frames and re-uploaded only on a rebuild, and a Dynamic buffer would be
  host-visible and duplicated per frame in flight on Vulkan, D3D and Metal, so the canvas uses Static (device-local)
  buffers. Not measured against Dynamic.

## Qt and Qwt behaviour the code works around

- **`QwtPlotCanvas` ignores QSS backgrounds** (it paints through a backing store), so the canvas gets a solid palette
  colour resolved from the framework and re-applied on `ApplicationPaletteChange`.
- **A transparent canvas forces a per-paint alpha composite** of the whole canvas, slow in software raster; the canvas
  is opaque with the palette colour (observed, not measured here).
- **`QRhiWidget` reports "No QRhi" on every paint attempt** when no QRhi exists; the plot swaps to the raster canvas
  once.
- **Qt/WASM is Asyncify-free:** nested `QDialog::exec()` and `QMenu::exec()` event loops are unavailable, so the XY
  dialog, the duplicate-name warning and the context menu are heap-owned and opened asynchronously. `QDialog::open()`
  downgrades `ApplicationModal` to `WindowModal`; `show()` keeps the desktop modality without nesting.
- **Qt/WASM switches a raster top-level to its RHI compositor when the first real plot is shown:** the whole window is
  repainted once, after that first frame was submitted, so the raster backing texture is initialized before later
  partial updates; the latch is per top-level window.
- **Grabbing a canvas a layout restore replaced breaks the browser window's rendering** (2026-11-20, Qt 6.12
  WebAssembly, Chromium headless shell 1228): the replaced canvas stays hidden until its deferred delete, and a
  `grabFramebuffer()` there made the window's backing store report a lost WebGL device and reinitialize; the renderer
  then often crashed. The browser readback probes skip a canvas no longer shown. `grabPresentedFrame` still grabs a
  hidden widget directly; not tested in the browser. Re-test on a Qt or Chromium upgrade.
- **Vulkan records the viewport as dynamic state of the bound pipeline,** so it must be set after every pipeline bind or
  nothing draws.
- **The raster fallback deletes the failed canvas,** so it must not run while a context menu or modal dialog opened from
  one of its events is on the stack (it waits for the popup to close).

## Qt-Advanced-Docking (ADS 5) behaviour the code works around

- **Hiding docks:** `CDockWidget::toggleView(false)` collapses the emptied splitter branches so the survivor reflows; a
  raw `CDockAreaWidget::setVisible(false)` does not and strands the survivor in nested layouts. `toggleView` deletes
  content only under `DeleteContentOnClose`, not the `DeleteOnClose` flag the docks carry, so hidden docks survive to be
  restored.
- **`dockWidgetsMap()` is keyed by object name,** and every PJ4 dock is named "Plot", so it collapses to one entry;
  enumeration uses `openedDockWidgets()`.
- **Focus:** ADS does not move focus to a survivor when the focused dock closes, and does not re-emit
  `focusedDockWidgetChanged` for an already-focused dock; the plot docker does both by hand.
- **Stylesheet:** ADS 5's `FollowPalette` re-applies its built-in stylesheet on every `ApplicationPaletteChange`,
  which a theme switch sends; the flag that keeps it off is set once, before the first `CDockManager`.
- **Layout restore geometry:** during a restore the docker is not laid out at its final size (a new `PlotDocker` is
  built per tab and restored before it is shown), so `widget->width()` reports placeholder geometry that skews
  pixel-scaled splitter sizes and the splitter later pulls the split toward 50/50. `QSplitter::setSizes` stores
  relative weights and shrinks oversized hints proportionally on first layout, so large common-scale weights reproduce
  the saved ratios.
- **Re-test:** on an ADS upgrade.
