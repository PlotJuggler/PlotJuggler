# Qwt — external dependency (not vendored)

No Qwt sources live in this repo. The `plotjuggler_qwt` target in
`3rdparty/qwt/CMakeLists.txt` provides them one of two ways:

- **Default (Conan / `./build.sh` path)**: CMake `FetchContent` downloads the
  **official Qwt release, unmodified** (the URL and SHA-256 are pinned in that file), at configure time and compiles
  the plot-widget subset as a static library. The subset leaves out the
  `QOpenGLWidget` canvases (`QwtPlotOpenGLCanvas`, `QwtPlotGLCanvas`): PJ4
  plots on `PlotRhiCanvas` or the raster `QwtPlotCanvas`, so `plotjuggler_qwt`
  links no `Qt6::OpenGL` / `Qt6::OpenGLWidgets`. (The packages still ship both:
  `Qt6UiTools`, which `pj_dialog_host` uses, depends on them.)
  - The download is cached per build tree; a fully offline first configure can
    point `FETCHCONTENT_SOURCE_DIR_QWT_UPSTREAM` at a pre-extracted tarball.
- **`-DPJ_SYSTEM_QWT=ON` (pixi / conda path)**: links the environment's
  prebuilt shared `qwt` package (conda-forge, built against the same `qt6-main`
  the env provides). No pixi task sets it while conda-forge has no qwt build for
  the pinned `qt6-main` minor (pixi uses the FetchContent build). That library
  is the full upstream build, OpenGL canvas included, so this path still links
  `Qt6::OpenGL` and `Qt6::OpenGLWidgets`. On Windows, where conda-forge's qwt is
  a DLL, consumers get `QWT_DLL` (`PJ_SYSTEM_QWT_DLL`, ON by default) so its
  exports are dllimported; otherwise MSVC duplicates the
  `QwtSeriesData<QPointF>` template members between `qwt.dll` and our own
  subclasses (LNK2005).

Qwt is licensed under the Qwt License 1.0 (LGPL-derived); the license text
ships inside the release tarball (`COPYING`) and the conda package.

**Policy: never patch Qwt sources**, so an upgrade is a version bump. PlotJuggler-specific behavior lives in the
application:

- Axis tick-label formatting (`QwtAbstractScaleDraw::label` override):
  `pj_plotting/widget/.../PlotScaleDraw.{h,cpp}` (and a local twin in
  `pj_dialog_host/src/chart_preview_widget.cpp`).
- The "Lines and Dots" curve style: `pj_plotting`'s `PlotCurve` draws
  `QwtPlotCurve::Lines` plus one batched, pixel-weeded `drawDots` pass
  (`PlotRhiCanvas` builds the same square dots as geometry).

To upgrade Qwt: bump the URL + SHA-256 in `3rdparty/qwt/CMakeLists.txt`, re-check the
source list against upstream's `src/src.pri`, and align the conda-forge
version pin in `pixi.toml`.
