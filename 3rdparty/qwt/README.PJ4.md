# Qwt — external dependency (not vendored)

No Qwt sources live in this repo. The `plotjuggler_qwt` target in
`CMakeLists.txt` provides them one of two ways:

- **Default (Conan / `./build.sh` path)**: CMake `FetchContent` downloads the
  **official Qwt 6.3.0 release, unmodified**, at configure time and compiles
  the plot-widget subset as a static library.
  - Source: <https://downloads.sourceforge.net/project/qwt/qwt/6.3.0/qwt-6.3.0.tar.bz2>
  - Tarball SHA-256: `dcb085896c28aaec5518cbc08c0ee2b4e60ada7ac929d82639f6189851a6129a`
  - The download is cached per build tree; a fully offline first configure can
    point `FETCHCONTENT_SOURCE_DIR_QWT_UPSTREAM` at a pre-extracted tarball.
- **`-DPJ_SYSTEM_QWT=ON` (pixi / conda path, Linux only)**: links the
  environment's prebuilt shared `qwt` package (conda-forge, built against the
  same `qt6-main` the env provides). The pixi `configure` task sets this on
  linux-64. On Windows the option is a hard error: conda-forge's qwt is a
  DLL, and MSVC duplicates the `QwtSeriesData<QPointF>` template members
  between `qwt.dll` and our own subclasses (LNK2005), so win-64 always uses
  the FetchContent static build.

Qwt is licensed under the Qwt License 1.0 (LGPL-derived); the license text
ships inside the release tarball (`COPYING`) and the conda package.

**Policy: never patch Qwt sources.** PlotJuggler-specific behavior that older
PJ trees carried as in-tree Qwt patches lives in the application:

- Axis tick-label formatting (`QwtAbstractScaleDraw::label` override):
  `pj_plotting/widget/.../PlotScaleDraw.{h,cpp}` (and a local twin in
  `pj_dialog_host/src/chart_preview_widget.cpp`).
- The "Lines and Dots" curve style: `pj_plotting`'s `PlotCurve` draws
  `QwtPlotCurve::Lines` plus one batched, pixel-weeded `drawDots` pass
  (on WASM, an explicit `QwtSymbol` that `PlotRhiCanvas` renders as geometry).

To upgrade Qwt: bump the URL + SHA-256 in `CMakeLists.txt`, re-check the
source list against upstream's `src/src.pri`, and align the conda-forge
version pin in `pixi.toml`.
