# Qt version notes (for AI agents and humans)

**PJ4 builds against the Qt version pinned in [`../versions.env`](../versions.env)
(currently 6.11.1).** Install it with [`../scripts/install_qt6.sh`](../scripts/install_qt6.sh).

> **Read this if your training data predates ~2025.** Qt 6.9, 6.10 and 6.11
> shipped *after* the knowledge cutoff of most current models. If you "know" PJ4
> is on Qt 6.8, that is stale — this file is the delta. Don't reach for a 6.8-era
> workaround for something later Qt fixed, and don't assume an API you don't
> recognize doesn't exist. When unsure about a Qt symbol, check the installed
> headers under `.qt/<PJ_QT_VERSION>/gcc_64/include/` rather than guessing from memory.

## Why 6.11 (a non-LTS) and not 6.12 LTS

We want the newest stable features now. 6.11.1 (released 2026-05-13) is the
latest stable; 6.11.2 isn't out until ~Aug 2026. The next **LTS** is **6.12**
(Qt's LTS cadence is now every 4th minor / ~2 years from 6.8 on, with 5 years of
support) — beta as of mid-2026, final ~autumn 2026. **Plan: ride 6.11.x now, then
settle on 6.12 LTS when it ships.** Qt 6 guarantees source/binary compatibility
across the whole 6.x series, so 6.8 → 6.11 → 6.12 is low-risk for a pure-Widgets
app like PJ4.

## What PJ4 actually uses (so you can judge relevance)

PJ4 is a **Qt Widgets** desktop app. It links Widgets, Core, Gui, Network, Xml,
OpenGLWidgets, Svg/SvgWidgets, Concurrent, UiTools, Charts, and **GuiPrivate**
(private headers — version-sensitive). It does **not** use QML / Qt Quick /
Qt Quick 3D, and **not** Qt Multimedia (video goes through FFmpeg directly).

Graphics split — important:
- **`pj_scene2D`** `MediaViewerWidget` is a **`QRhiWidget`** → it *does* go through
  Qt's RHI. QRhi changes can matter here.
- **`pj_scene3D`** renders with **raw `QOpenGLWidget`** (its own HDR FBO / SSAO /
  EDL / AgX passes), **not** QRhi. QRhi additions are latent for scene3D unless we
  ever port it.

## New APIs worth reaching for (added 6.9–6.11)

Prefer these over hand-rolled equivalents. Names are exact.

**Qt Core**
- `QPainterStateGuard` (6.9, in QtGui) — RAII `save()`/`restore()` for `QPainter`.
  Use it in paint code instead of manual save/restore (easy to leak on an early
  return). Relevant across `pj_plotting`, `pj_scene2D`, custom delegates.
- `QUuid::createUuidV7()` (6.9) — time-ordered (sortable) UUIDs.
- `QHash::tryEmplace()` / `insertOrAssign()` (6.9).
- `QSpan::chop()` / `slice()` (6.9); `QByteArray` implicit → `std::string_view` (6.10);
  `nullTerminate()`/`nullTerminated()` (6.10). Handy in `pj_datastore` / codec code.
- `QFuture::cancelChain()` (6.10) — cancel a whole continuation chain (`pj_runtime`,
  `Qt6::Concurrent`).
- `QThread::setServiceLevel()` / `QThreadPool::setServiceLevel()` (6.9) — OS QoS
  hints; candidate for the image-decode / ingest worker pools.
- `QRangeModel` (6.10) + `QRangeModel::ItemAccess` / `autoConnectPolicy` (6.11) —
  exposes `std::vector` / `std::array` / ranges as a `QAbstractItemModel` **for
  Widgets too**. Kills boilerplate in model adapters.
- `QSortFilterProxyModel::beginFilterChange()` / `endFilterChange(Direction)`
  (6.9/6.10) — cheaper, finer-grained than `invalidateFilter()` (which is
  deprecated for Qt 6.13+). Relevant to catalog/curve-tree filtering.

**Qt GUI / painting**
- `QPainterPath::setCachingEnabled()` / `trimmed()` (6.10).
- `QColorSpace` four-primary get/set (6.9) — useful for the scene3D HDR/AgX color
  management.
- `QImage::flipped()` / `flip(Qt::Orientation)` (6.9).

**Qt Widgets (item views — PJ4 has big curve trees / catalogs)**
- `QHeaderView` rewritten for **huge models** with much lower memory (6.9).
- `QAbstractItemView::updateThreshold` (6.9) — batch full-view updates above a
  row threshold instead of per-row repaints.
- `QAbstractItemView::keyboardSearchFlags` (6.11); `QComboBox::labelDrawingMode`
  = `UseDelegate` (6.9).
- `setSupportedDragActions()` on `QListWidget`/`QTableWidget`/`QTreeWidget` (6.10);
  `QAbstractItemDelegate::handleEditorEvent()` default (6.10) — relevant to the
  custom curve-tree drag-drop / delegates.
- `QDockWidget::dockLocation`, `QStackedWidget::widgetAdded()` (6.9).

**Qt Network** (marketplace / streaming paths)
- Per-request TCP keep-alive on `QNetworkRequest` (6.11; default idle close after
  2 min); `QSslCertificate::fromFile()` (6.10).

**QRhi** (matters for `pj_scene2D`'s `QRhiWidget`, not scene3D)
- D3D11/12 vblank watcher thread → lower CPU/latency (6.9); integer & depth
  texture formats (6.9); `QRhi::enumerateAdapters()` (6.10). Mostly Windows-side
  but the RHI path is shared.

## Deprecations & removals — act/avoid

- **Qt Charts is deprecated since 6.10** (migrate-to-Qt-Graphs messaging) and is
  **removed in Qt 7**. PJ4 **no longer depends on Qt Charts**: its one consumer,
  `pj_dialog_host`'s `ChartPreviewWidget`, was reimplemented on the vendored
  **Qwt** (`plotjuggler_qwt`); the dialog-protocol chart API (`setChartSeries` /
  `onChartViewChanged`) is unchanged. **Do NOT** reach for **Qt Graphs** as the
  replacement — its 2D path renders via Qt Quick Shapes / `QQuickWidget`, which
  would drag QML into a pure-Widgets module. Qwt (already used by `pj_plotting`)
  is the project's charting tool.
- **XRender X11 native painting engine removed (6.11)** — only relevant if built
  with `-xcb-native-painting`. PJ4 isn't; no action.

## Build / toolchain floors (don't trip on these)

- **CMake ≥ 3.22** required by Qt since 6.9. PJ4 keeps `cmake_minimum_required` at
  **3.22** on purpose — the Ubuntu 22.04 CI runner ships CMake 3.22.1, exactly at
  the floor. Don't bump the requirement past 3.22 (see the note in
  `.github/workflows/windows-ci.yml`); don't drop a runner below it either.
- **glibc floor raised 2.28 → 2.34** (6.10): official Linux Qt binaries (what
  the repo install script fetches via aqt) are built on RHEL9. So a PJ4 binary built
  against them needs **glibc ≥ 2.34 (≈ Ubuntu 22.04+)** to run. Matters for
  *shipping* to end users, not for the dev box.
- **Private modules need their own component**: `find_package(Qt6 COMPONENTS
  GuiPrivate)` (6.10). PJ4 links `Qt6::GuiPrivate` — keep that in mind if you
  touch the find_package lines.

## Platform / Wayland behavior changes

- `wl_surface` lifetime now matches the window — it is **no longer destroyed when
  the window is hidden** (6.9). Safer hide/show of docks under Wayland.
- `xdg-toplevel-icon-v1` support (6.9) → `QWindow::setIcon()` actually shows the
  window icon under Wayland.
- Server-side decorations + server-side key repeat + `xx-session-management-v1`
  (6.11).
- **Not confirmed fixed:** the known PJ4 Wayland drag-and-drop "stuck on loading"
  cursor quirk isn't called out in any 6.9–6.11 changelog. Don't assume the
  upgrade resolves it; re-test.

## House style: string literal suffixes

New code uses the Qt 6 literal suffixes — `u"…"_s` for `QString` (equivalent to
`QStringLiteral("…")`) and `"…"_L1` for `QLatin1StringView` comparisons — via
`using namespace Qt::StringLiterals;` placed after the last `#include` at file
scope, **`.cpp` files only**. Never put that `using` directive at namespace
scope in a header (it would leak into every translation unit that includes it);
headers keep `QStringLiteral`. A trailing `#include "X.moc"` at the end of the
file (required when a `Q_OBJECT` class is defined in the `.cpp`) does not count
as "the last include" — place the `using` line after the top include block,
before the first namespace/code.

## Explicitly NOT relevant to PJ4 (don't chase these)

QML / Qt Quick / Qt Quick 3D (incl. SSGI/SSR/motion vectors), Qt Multimedia &
the PipeWire backend, Qt WebEngine, Qt Canvas Painter, FlexboxLayout, Lottie /
VectorImage, Qt Graphs (the Charts/DataViz successor), Qt XR. These dominate the
6.9–6.12 headlines but touch nothing in PJ4 (pure Widgets + raw OpenGL + Qwt +
FFmpeg). scene3D's photorealism is hand-written OpenGL, *not* Qt Quick 3D.

## See also

- [`../versions.env`](../versions.env) — pins the Qt version consumed by CMake, scripts, Docker, and CI.
- [`../scripts/install_qt6.sh`](../scripts/install_qt6.sh) — installs the pinned Linux Qt build.
- `pj_scene2D/docs/TECHNICAL_NOTES.md` — `QRhiWidget` lifecycle + video-rendering
  specifics (the APIs there became public/usable in 6.8 and remain so on 6.11).
