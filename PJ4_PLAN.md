# PlotJuggler 4.x Master Plan

## 0. Revisions (2026-04)

This section captures the architectural decisions that supersede portions of this document. The body below remains as planning context; where it conflicts with this section, this section wins.

- **Three independent widget families by design.** The GUI is split into widget families that never depend on each other: `pj_plotting` (Qwt, lifted wholesale from PJ3), `pj_scene2D/widgets` via the `pj_scene2d_widgets` target (QRhi, wraps `pj_scene2D/core`), and `pj_scene3D/widgets` via the `pj_scene3d_widgets` target (hand-rolled OpenGL 4.5 Core, wraps `pj_scene3d_core`). Heterogeneity is a feature, not a problem. Shared reusable Qt controls/helpers live in `pj_widgets` — including the reusable **Source Timeline** control (see §5.6); shared runtime state flows through the small `IDataWidget` contract exposed by `pj_runtime`.
- **`pj_scripting` is its own module.** Language-agnostic engine (Luau today — a hand-written Luau binding behind a `ScriptEngine` seam; NOT sol2, whose API is incompatible with Luau's C++ linkage. Python pluggable later) decoupled from both the GUI and `pj_runtime`'s services layer. (The plan body below still uses the older `IScriptingEngine`/`LuaScriptingEngine`/sol2 planning names; the as-built names are `ScriptEngine` / `LuauEngine` / `makeLuauEngine`.) Depends only on `pj_base` + `pj_datastore`. Custom Lua transforms reach `DerivedEngine` through a thin `transform_adapter`. Reactive scripts live in a Toolbox plugin that links `pj_scripting` directly.
- **`pj_runtime` Qt boundary is relaxed.** Previously "no Qt"; now **Qt is allowed (QObject, QTimer, QSettings, signals), but no concrete QWidget/QDialog implementation and no `Qt6::Widgets` link**. `IDataWidget` may forward-declare `QWidget` as the shell contract. Services remain headlessly testable via `QCoreApplication`. This trades a small amount of purity for much cheaper timer/settings/reactive plumbing.
- **Plot widgets are lifted wholesale from PJ3**, not rebuilt on Qt Charts. `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, zoomers, axis-time, drag-drop, per-curve display transform UI all move into `pj_plotting/`; their data reads are rebound to `pj_datastore` via a `DatastoreCurveAdapter`. No `IPlotBackend` abstraction.
- **Monorepo for now.** App-owned modules (`pj_datastore`, `pj_scene2D`, `pj_scene3D`, `pj_scene_common`, `pj_marketplace`, `pj_dialog_host`, `pj_scripting`, `pj_runtime`, `pj_widgets`, `pj_plotting`, `pj_app`) live inside this repository. `pj_datastore` (the columnar storage engine) was moved out of the `plotjuggler_sdk` submodule into the app repo, because plugins reach storage only through the `pj_base` C ABI and never link the engine — so the `plotjuggler_sdk` submodule is purely the plugin **SDK** (`pj_base` + `pj_plugins`). The long-term intent is still a separate `plotjuggler_app` repo with the `plotjuggler_sdk` SDK as a submodule; module boundaries are designed so that split is a mechanical move later.
- **v1 target is parity-plus with PJ3.** File + streaming sources, 11 built-in transforms, undo/redo, derived-series editor (incl. Lua via `pj_scripting`), reactive scripts (via Toolbox + `onTimeChanged`), multi-tab workspace, marketplace install UI, all toolboxes.
- **Plugin families and marketplace are assumed done** and out of scope for this plan. Toolbox SDK gaps (embedded charts, drag-drop, code editor, `onTimeChanged`, ScatterXY outputs) are being addressed in parallel and are assumed solved.
- **The old prototype app is removed.** It was a throwaway prototype; current PJ4 modules are the implementation baseline.
- **Three-widget time model.** Global tracker owned by `PlaybackEngine` in `pj_runtime`. Plots redraw a vertical marker line (cheap); 2D/3D widgets snap to the frame at tracker time (via `MediaSource::setTimestamp` / equivalent).

Where the sections below refer to `pj_app_shell_qt`, read that as `pj_app`.

## 1. Purpose

This document defines the implementation plan for building a full PlotJuggler 4.x desktop application with feature coverage comparable to PlotJuggler 3.x, while modernizing the internal architecture.

The goal is not to recreate the 3.x code structure. The goal is to recreate the 3.x product capability set on top of the foundation libraries:

- `pj_base` (in the `plotjuggler_sdk` SDK submodule)
- `pj_plugins` (in the `plotjuggler_sdk` SDK submodule)
- `pj_datastore` (top-level app module; moved out of the submodule)
- marketplace-delivered extensions

The main design objective is a strict separation between:

- business logic and domain state
- application orchestration
- GUI widgets and rendering

The removed prototype app was a useful proof of concept, but it was not a sufficient foundation for the final 4.x app architecture. The final app is organized around dedicated modules instead of growing the prototype window into the final application.

## 2. Product Target

### 2.1 Definition of feature-complete

The 4.x app is considered feature-complete with respect to 3.x when it provides:

- file import through installed data source extensions
- live streaming through installed data source extensions
- delegated ingest through installed parser extensions
- searchable dataset/topic/field catalog
- multi-tab workspace
- multiple plots per tab
- flexible docked layout
- drag and drop from catalog to plots
- time-series plotting
- XY plotting
- tracker/cursor inspection
- zoom, pan, linked navigation
- plot styling and per-plot settings
- built-in derived transforms
- named derived series
- scripting/reactive series equivalent to the old Lua workflows
- toolbox hosting and persistence
- workspace save and load
- user settings persistence
- extension installation and management via marketplace

### 2.2 Explicitly deferred

These items are not required for the first feature-complete target:

- `StatePublisher` parity
- exact 3.x UI/terminology parity
- hot reload of already running extension instances
- full backward compatibility with 3.x layout files

### 2.3 Parity philosophy

4.x targets **modernized parity**:

- preserve the main workflows and capability surface of 3.x
- improve internal structure aggressively
- allow UI and workflow cleanup when it simplifies the architecture
- do not carry forward the 3.x monolithic `MainWindow` design

## 3. Current Baseline

### 3.1 What already exists

The current repository already provides substantial infrastructure:

- `pj_datastore`
  - columnar data engine
  - typed topics and schemas
  - range/latest-at queries
  - derived transform DAG
  - plugin data host bridge
  - `ObjectStore` for media blobs (images, video frames, scene primitives) with lazy fetch and retention
- `pj_plugins`
  - runtime loading for `DataSource`, `MessageParser`, `Dialog`, and `Toolbox`
  - C ABI + C++ SDK layers
  - dialog engine
  - All PJ3 plugins have been ported
- `pj_scene2D`
  - 2D/video visualization on top of `ObjectStore`
  - FFmpeg-backed video decode with HW acceleration, scrub optimizations, B-frame support
  - `MediaSource` pull-based abstraction (`ImagePipelineSource`, `StreamingVideoSource`)
  - `pj_scene2d_widgets` QRhi widget with BT.709 YUV→RGB shader
- `pj_marketplace`
  - extension installation and management UI/services (assumed done)

### 3.2 What is still missing

The missing work is the full application layer that 3.x had:

- real workspace/layout model
- full plot widget hierarchy and interaction model
- global tracker and playback model
- plot-local display transforms
- derived-series authoring UI
- scripting runtime and editor
- toolbox placement and persistence
- robust persistence model
- command and undo/redo infrastructure
- an app-core service layer independent of widgets

## 4. Architectural Principles

### 4.1 Concrete three-level architecture

The final app should be built in three concrete levels:

#### Level 0: foundational libraries

These already exist and remain the substrate:

- `pj_base` (in the `plotjuggler_sdk` SDK submodule)
- `pj_plugins` (in the `plotjuggler_sdk` SDK submodule)
- `pj_datastore` (top-level app module — `ObjectStore` + `DerivedEngine`; moved out of the submodule, since plugins reach it only via the `pj_base` C ABI)
- `pj_scene2D` (2D/video visualization)
- `pj_scene_common` (backend-agnostic shared scene-dock framework consumed by the 2D/3D widget families)
- `pj_marketplace`
- `pj_scripting` (new — language-agnostic engine, Lua today)

They provide the vocabulary types, columnar + object stores, extension ABI/runtime, media pipeline, marketplace services, and scripting runtime.

#### Level 1: app core (`pj_runtime`)

This is the authoritative business layer for the 4.x app.

Responsibilities:

- application state and composition root
- data session lifecycle
- workspace model and persistence
- playback and tracker state
- transforms and scripting integration
- extension runtime orchestration
- user settings and notifications

Constraints (revised):

- **Qt is allowed**: `QObject`, `QTimer`, `QSettings`, signals
- **no `QWidget` / `QDialog`** in this layer
- no UI-owned state
- fully testable headlessly with `QCoreApplication`

#### Shared Qt helpers: `pj_widgets`

Reusable Qt building blocks that are not tied to PlotJuggler's app shell live in `pj_widgets`. This is for controls and UI utilities such as color pickers, SVG icon loading, numeric sliders, and the reusable Source Timeline control (§5.6). It is not itself a widget family and it must not own application state.

#### Level 2: widget families (three independent sibling modules)

The concrete desktop UI layer is **three independent widget families**, each with its own rendering world. They are siblings: they do not depend on each other.

- **`pj_plotting`** — Qwt-based plot widgets (lifted wholesale from PlotJuggler 3.x). Time-series, XY, tracker, zoomers, per-curve display transforms.
- **`pj_scene2D/widgets`** — 2D viewer widgets built as the `pj_scene2d_widgets` target and wrapping `pj_scene2D/core`. Image, video, depth, annotation display via QRhi.
- **`pj_scene3D/widgets`** — 3D viewer widgets built as the `pj_scene3d_widgets` target, wrapping `pj_scene3d_core`. Hand-rolled OpenGL 4.5 Core renderer (`QOpenGLWidget` + `QOpenGLFunctions_4_5_Core`), with TF, pointcloud, occupancy-grid, axis/grid render passes; integrated into pj_app via `Scene3DDockWidget` + `TransformService`.

All three implement the small `IDataWidget` contract from `pj_runtime` (tracker callback, save/load state, subscribed topics, Qt widget accessor). They may use reusable Qt helpers from `pj_widgets`, but they do not depend on each other. The variability of these three rendering worlds is **by design**.

#### Level 3: shell (`pj_app`)

This is the main-window shell.

Responsibilities:

- main window
- menus, toolbars, status bar
- Qt Advanced Docking Layout (ads) for tabs + docked panels (plots + media + 3D cohabiting)
- tree/catalog views
- editors and dialogs (including marketplace window)
- wiring `pj_runtime` services to widgets from all three families

Constraints:

- widgets render business state and dispatch operations to app-core services
- widgets never become the source of truth
- the shell owns no business logic

### 4.2 Single source of truth

The source of truth for each concern must be explicit:

- raw and derived data: `pj_datastore`
- workspace structure: app-core workspace subsystem
- playback/tracker state: app-core playback subsystem
- transform and script definitions: app-core transforms subsystem
- extension lifecycle: app-core extension-host subsystem
- user settings: dedicated settings service in app core

### 4.3 App-core mediated mutations

All important application changes should be performed by app-core services instead of direct widget manipulation:

- import file
- start stream
- add plot panel
- add curve to plot
- create derived series
- edit script
- open toolbox panel
- save workspace
- load workspace
- clear data
- clear plots

This keeps the GUI thin and ensures the workspace, playback, and session state remain coherent.

### 4.4 Concrete services first

Use concrete service classes as the internal API by default.

Examples:

- `AppSession`
- `SessionManager` (formerly `DataSessionManager`)
- `WorkspaceManager`
- `PlaybackEngine`
- `TransformRegistry`
- `ToolboxManager`
- `ExtensionCatalogService`
- `UndoManager`
- `UserSettingsService`

Introduce abstract interfaces only when there is a real boundary or more than one plausible implementation, such as:

- plugin ABI boundaries (already fixed in `pj_plugins`)
- `IDataWidget` (the small contract bridging app_core to the three widget families)
- `IScriptingEngine` (the boundary in `pj_scripting` allowing Lua/Python/other)
- persistence format migration helpers if they diverge

The GUI should not reach into `DataEngine`, `DerivedEngine`, `ObjectStore`, or extension handles directly unless the operation is purely read-only and already mediated by a service.

## 5. Target Module Map

The final app is organized as several sibling modules on top of the existing foundation libraries, all living inside this repository (with a future split into `plotjuggler_app` planned but not required).

### 5.1 `pj_scripting` — scripting engine (new foundation module)

Purpose:

- language-agnostic scripting runtime
- used by `pj_runtime` and by Toolbox plugins that provide script editors (e.g. Lua editor, reactive scripts)

Depends on: `pj_base`, `pj_datastore` (for transform interfaces).

Contents:

- `IScriptingEngine` — abstract interface (`compile`, `eval`, `registerFunction`, `lastError`, `languageName`)
- `Value` — neutral data exchange type (scalar/vector/timestamp variant)
- `LuaScriptingEngine` — Lua implementation (sol2 or port of PJ3's Lua wrapper)
- `transform_adapter` — wraps a compiled `ScriptHandle` as `ISISOTransform` / `IMIMOTransform` for `DerivedEngine`
- Future: `PythonScriptingEngine` as an additional source file, no other changes

Rule:

- no Qt, no GUI code, no knowledge of services
- Toolbox plugins that ship a script editor link this library directly

### 5.2 `pj_runtime` — business services

Purpose:

- main business layer
- composition root for the app runtime

Main owned objects:

- `SessionManager` — owns the datastore + ObjectStore, DataSource plugin lifecycle
- `PlaybackEngine` — global tracker, play/pause, rate, follow-live (QTimer-driven)
- `CatalogModel` — Qt model wrapping topics/fields; drag-mime source for all widget families
- `WorkspaceManager` — JSON layout save/load, schema versioned
- `WidgetRegistry` — maps `widgetType` string → factory for layout restore
- `TransformRegistry` — wraps `DerivedEngine`, registers built-ins + script-backed transforms
- `ToolboxManager` — toolbox plugin lifecycle, `onTimeChanged` dispatch
- `UserSettingsService`
- `UndoManager` — snapshot-based
- `ExtensionCatalogService` — facade over `pj_marketplace`
- `NotificationCenter`

Small contracts defined here:

- `IDataWidget` — the tiny contract the three widget families implement
- Service callback types (`std::function` or Qt signals)

Constraints:

- Qt allowed (QObject/QTimer/QSettings/signals)
- **no concrete** `QWidget` / `QDialog` implementation and no `Qt6::Widgets` link
- headlessly testable via `QCoreApplication`

### 5.3 `pj_plotting` — Qwt plot widgets (lifted from PJ3)

Purpose:

- concrete plot widget family

Approach:

- lift the PJ3 plot widget tree wholesale: `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, zoomers, `AxisTimeOffset`, custom tracker, drag-drop, context menus, curve list helpers (per-curve display transform UI ports later — see deferral note below)
- replace `PlotDataMapRef` reads with `DatastoreCurveAdapter` — a `QwtSeriesData<QPointF>`-derived class that pull-throughs to `pj_datastore` via `SessionManager` (no copy of timeseries data; see §8.2 for the chunk-index design)
- rely on Qwt's paint-time decimation (`QwtPlotCurve::FilterPointsAggressive` + `ClipPolygons`) for large datasets — no adapter-side decimation; pre-decimating breaks on zoom
- implement `IDataWidget` on the plot widget; register the plot factory directly in `pj_app` (no `WidgetRegistry` service in v1; introduce one only when 2D / 3D widget families need symmetric registration)

**Deferred to a follow-up phase** (still planned, not in v1 scope): `PlotWidgetEditor` (full edit-curves dialog with line width / Y limits / style toggles), `PlotWidgetTransforms` (per-curve display-transform dialog), `StatisticsDialog`, `PlotBackground` colored-zone item, colormap selector / editor. The four v1 demo features (drag-drop, color via stock `QColorDialog` from a context menu, sync-zoom, datastore plumbing) are the acceptance bar.

Constraints:

- no knowledge of extension sessions, workspace persistence policy, or marketplace
- no dependency on `pj_scene2d_widgets` or `pj_scene3d_widgets`

### 5.4 `pj_scene2D/widgets` — 2D media widgets

Purpose:

- concrete 2D media widget family

Approach:

- thin wrapper around `pj_scene2D/core` using `MediaViewerWidget` and QRhi rendering
- subscribe to `PlaybackEngine::trackerChanged` → call `MediaSource::setTimestamp` → request repaint
- implement `IDataWidget`; register factory

Constraints:

- no dependency on `pj_plotting` or `pj_scene3d_widgets`

### 5.5 `pj_scene3D` — 3D widget family (robotics viz)

Implemented and wired into `pj_app` (built via `add_subdirectory(pj_scene3D/core)` + `add_subdirectory(pj_scene3D/widgets)`; `MainWindow` instantiates `Scene3DDockWidget` + `TransformService`). See `pj_scene3D/docs/REQUIREMENTS.md` for the full spec (it supersedes this section).

**Data types (v1 target, 6 total)**: TF2 (infrastructure), URDF/mesh, Pointcloud, Markers (arrows/boxes/spheres/cylinders/line strips/text), Image+Pinhole (frustum + optional textured near-plane), OccupancyGrid (textured plane in 3D).

**Storage**: all via `pj_datastore::ObjectStore`. Pointcloud is lazy-fetch (like MCAP images in `pj_scene2D`); everything else lives in memory. DataSource plugins write via existing `object_write_host.push(topic, encoding, bytes, t)` using per-type encoding strings — no new write hosts.

**Stack (locked)**:

- GPU abstraction: **hand-rolled raw OpenGL 4.5 Core** (`QOpenGLFunctions_4_5_Core`)
- Widget base: `QOpenGLWidget` subclass (`SceneViewWidget`), hand-rolled scene (no Qt 3D, no Qt 3D scene graph)
- 3D math: **GLM** (GLSL-matching types, `glm::slerp` for TF interpolation, header-only conan dep)
- Mesh loading: **assimp** (conan dep)
- URDF parsing: `QDomDocument` (see `pj_scene3D/docs/ARCHITECTURE.md`)
- Pointcloud decoders: hand-rolled (PCD, `PointCloud2`)
- Marker primitives: hand-rolled geometry generators
- Text rendering: `QPainter` + `QFont` → `QImage` → GL texture
- Image decoding: reuse `pj_scene2D`
- Coordinate convention: **ROS Z-up**

**Scene organization**: flat per-widget drawable list (Foxglove-lean; RViz-style per-type config knobs can be added incrementally). Scenes in robotics contexts are small enough that a deep scene graph is unnecessary.

**Caching**: minimal. Pointcloud drawables cache GPU buffers across tracker changes; everything else re-decodes on change (cheap).

**TF interpretation layer** (stateful: per-edge ring buffer + slerp/lerp interpolation + tree traversal over ObjectStore TF samples) lives inside `pj_scene3D` for v1. To be reviewed later: if 2D widgets or `pj_runtime` transforms need TF too, promote to a sibling module `pj_tf`.

**Interaction**: orbit camera (drag rotate / wheel zoom / middle-drag pan), drag-drop topics from `CatalogModel`, per-drawable context menu (visibility / color / delete), click-to-select picking.

**Deferred (post-v1)**: shared GPU resources across multiple 3D widgets, advanced pointcloud LOD, costmap 3D / octomap, richer per-display config UI.

**Inspiration source (not a dependency)**: [threepp](https://github.com/markaren/threepp) (MIT-licensed C++20 Three.js port) is read as a reference for specific rendering patterns (URDF traversal, OrbitControls math, Raycaster algorithm, material abstractions). Logic may be ported with attribution. threepp is never linked as a runtime dep — the single-GPU-stack property is preserved.

**Rule**: implements `IDataWidget` from `pj_runtime`; register directly in `pj_app` for v1. Introduce a `WidgetRegistry` service only when 2D / 3D widget families need symmetric registration.

### 5.6 Source Timeline — a reusable `pj_widgets` control (ADR)

**Status:** implemented. Lives in `pj_widgets` as `Timeline.{h,cpp}`; see
`pj_widgets/CLAUDE.md`. (Originally prototyped as a standalone `pj_timeline`
module with a `core/` + `widget/` split; dissolved into a single reusable
`pj_widgets` control once it had exactly one consumer and Qt-only deps — the
math/render separation is preserved as a Qt-free class inside the same file,
not as a separate library.)

**Purpose:** Reusable multi-track Source Timeline that drives PJ4's dormant
per-source `TimeDomain.display_offset`. Each loaded dataset gets one
horizontal bar on a shared display-time ruler; dragging a bar shifts only
that source's display offset live (`display_time = raw_time − display_offset`).
One Align button snaps all displayed starts to the global earliest displayed
start.

**One file pair, two classes** (`pj_widgets/include/pj_widgets/Timeline.h` +
`pj_widgets/src/Timeline.cpp`, `namespace PJ`):

- `TimelineScene` — a genuinely **Qt-free** timing/geometry/alignment math class (no `Q*` in any method signature). Plain value types (`TimelineSpanInput`, `PxSpan`, `TimeSpan`, `TimelineViewport`, `TimelineRuler`), plain `quint64` ids / `qint64` ns / `double` pixels. Holds `displayWindow`/`sceneExtent`/`ns↔px`/`barSpan`/`zoom`/`ruler`/`alignStartsToCommonOrigin`.
- `Timeline : QWidget` — the `QGraphicsView`-backed view. Public input struct `TimelineTrack` (Qt-OK: carries `QString name` + `QColor color`). Runtime-agnostic: plain-typed slots (`setTracks`/`setPlayhead`/`setDisplayRange`) and intent signals (`offsetChangeRequested(quint64,qint64)`/`alignRequested`/`playheadSeeked(double)`); never includes a `pj_runtime` header. The bar/ruler/playhead `QGraphicsItem`s are folded into the `.cpp` (a private `timeline_detail` namespace).

**The `SourceTimelineController`** lives in `pj_app` — the only place that
may know both the widget and `pj_runtime`. It populates `TimelineTrack`s from
`CatalogModel` + `AppSession::datasetRawTimeRange` + `SessionManager::displayOffset`
(converting `DatasetId`→`quint64`, `Timestamp`→`qint64`), maps widget intent
signals to `SessionManager::setDisplayOffset` / `PlaybackEngine::setCurrentTime`,
runs `TimelineScene::alignStartsToCommonOrigin` for the Align intent, and
coalesces `AppSession::recomputeRange` calls during live drags (~16 ms QTimer).

**Dependency rules:**

- `pj_widgets` stays **Qt-only** — the Timeline added no `pj_base`/PJ-module dependency (hence the plain-typed surface).
- All cross-family effects (a bar drag reshifting plot curves) flow through `pj_runtime` signals (`SessionManager::displayOffsetChanged`), to which each widget family subscribes independently.

**Key architectural decision — single global cursor, per-source offsets:**
Multi-source alignment is implemented by folding per-source `display_offset`
into the read path (the curve adapter, and future scene-layer query sites),
not by creating per-source clocks. The `PlaybackEngine` / `IDataWidget::onTrackerTime(double)`
fleet contract is unchanged. This is simpler and keeps the existing `DatastoreCurveAdapter`
display-offset mechanism (which was already designed but had no production driver) as the
sole locus of offset application.

**Per-source `TimeDomain`:** Before this feature every dataset shared one
"default" `TimeDomain` so an offset would shift all sources. The loaders
(`FileLoader`, `StreamingSourceManager`) now mint one `TimeDomain` per
dataset so per-source isolation is guaranteed.

**v1 scope:** plots only. 2D/3D scene widgets are offset-blind (known
limitation; no crash). Deferred: 2D/3D per-layer offset, offset persistence,
undo/redo, richer alignment (leading source, per-bar pins), snapping,
derived-series following their parent offset.

### 5.7 `pj_app` — desktop shell

Purpose:

- concrete main-window shell

Responsibilities:

- main window
- menus, toolbars, status bar
- Qt Advanced Docking — single docking system hosting dock widgets from any widget family, per-tab docking areas
- catalog/tree panel
- transform editor, script editor, toolbox panel hosts
- marketplace window integration
- binding `pj_runtime` services to widgets from all three families

Rule:

- consumes concrete services from `pj_runtime`
- no business logic of its own

## 6. Core Runtime Model

### 6.1 `AppSession`

`AppSession` is the central runtime object for the 4.x app.

It should:

- own all long-lived application services
- expose concrete app-core operations to the GUI
- define app-wide event streams
- support startup, workspace restore, and orderly shutdown

Suggested composition (all from `pj_runtime`, plus one from `pj_scripting`):

- `SessionManager`
- `PlaybackEngine`
- `WorkspaceManager`
- `CatalogModel`
- `WidgetRegistry`
- `TransformRegistry`
- `ToolboxManager`
- `ExtensionCatalogService`
- `UserSettingsService`
- `UndoManager`
- `NotificationCenter`
- `IScriptingEngine` instance from `pj_scripting` (the default `LuaScriptingEngine`)

### 6.2 Session types

Represent data-producing activities explicitly:

- `ImportedDataSession`
- `StreamingDataSession`
- `ToolboxSession`

Each session tracks:

- session ID
- source extension ID
- dataset IDs it owns
- current lifecycle state
- persisted config payload
- diagnostics
- parser binding state
- whether the session is restorable

### 6.3 Dataset visibility vs existence

A dataset being removed from the visible catalog must not imply that the underlying data disappears immediately.

The app must support:

- hidden datasets
- removed session handles with retained historical data
- degraded workspace references that still point to retained data

This prevents UI operations from becoming destructive without explicit user intent.

## 7. Workspace Model Design

### 7.1 Tabs and panel layout

The workspace must support:

- multiple tabs
- multiple plot panels per tab
- split layouts
- optional floating/detached panels

The layout model must be serializable without referring to concrete widget IDs.

### 7.2 Plot panel state

Each plot panel must store:

- stable panel ID
- title
- plot mode:
  - time-series
  - XY
- assigned curves
- plot-local display transforms
- axis settings
- legend settings
- style settings
- linked navigation group membership
- tracker/inspection settings
- optional annotations and future metadata

### 7.3 Tool panel state

Each tool panel must store:

- stable panel ID
- tool kind
- extension identity or built-in tool identity
- serialized config
- placement metadata
- visibility/open state

### 7.4 Workspace serialization

Use a dedicated workspace file format with:

- explicit schema version
- structured document model
- migration hooks
- opaque payload fields for extension-specific state

Do not serialize widget trees directly.

### 7.5 Undo/redo

Undo/redo should follow the 3.x spirit and remain simple.

Use snapshot-based undo for workspace-level state:

- before a mutating workspace operation, serialize the current workspace document
- push the previous snapshot onto the undo stack
- on undo, restore the previous workspace snapshot
- on redo, restore the next workspace snapshot

This keeps the implementation compact and avoids a large hierarchy of command classes.

Snapshot-based undo should cover:

- tabs and panel layout
- plot contents and settings
- tool panel placement
- derived-series definitions
- script definitions

Data samples themselves are not part of undo/redo. Undo is for workspace and app configuration state, not for reverting imported or streamed data.

## 8. Plotting Subsystem Design

The plotting subsystem lives in `pj_plotting`. The approach is **wholesale lift from PlotJuggler 3.x**: the plot widget code has been refined over years and is good; the rot was in PJ3's `MainWindow` coupling, not in the plot widgets.

### 8.1 Requirements

The plotting subsystem must support:

- high-volume numeric rendering (millions of points per curve via display-resolution downsampling)
- multiple curves per plot
- XY mode
- derived and scripted outputs
- streaming updates
- zoom/pan/tracker workflows (including linked X-axis zoom across plots)
- statistics and inspection
- copy/export actions

### 8.2 Data access and materialization

`DataReader` is the public read API of `pj_datastore` and the canonical query surface.

`pj_plotting` adds only what the lift needs to cleanly consume the new datastore:

- `DatastoreCurveAdapter` — a `QwtSeriesData<QPointF>`-derived class that holds a `SessionManager*` + `CurveDescriptor` and reads `pj_datastore` through `DataReader`/`SeriesReader`. It materializes a windowed `{timestamp, value}` vector — the visible span widened by one span per side — and re-narrows it with binary searches while the viewport stays inside the built range, so zoom/pan steps never re-walk the engine (a per-repaint rebuild was the top GUI-thread cost while zooming). Plain values only, deliberately no cached `TopicChunk*`: streaming retention may free chunks at any time. ROI narrowing via `QwtSeriesData::setRectOfInterest`; one merge-aware off-screen guard sample per side keeps line segments crossing the viewport boundary visible. `boundingRect()` returns full-data bounds so Qwt's autoscale path does not lag a frame.
- statistics and inspection helpers — deferred to a follow-up phase along with the editor / transforms dialogs.

No adapter-side decimation. Qwt's paint-time `FilterPointsAggressive` + `ClipPolygons` own per-pixel collapse; pre-decimating in the data layer breaks zoom correctness because the cached array is sized for the previous viewport.

No `IPlotBackend` abstraction. Qwt is the rendering library, full stop.

### 8.3 Plot interaction logic

Lift from PJ3 as-is:

- wheel/pan/zoom-rect/tracker handling in the widget base classes
- linked-range synchronization driven by `pj_runtime` state (a `link_group` per plot widget, stored in layout)
- tracker crosshair that reads the global tracker time from `PlaybackEngine` and redraws a vertical line
- context menus, drag-drop acceptors, per-curve display transforms UI

Display transforms (smoothing, derivative, etc.) stay **plot-local**: they do not create global topics in the datastore; they are serialized as part of plot panel state.

### 8.4 Display transforms

Per-plot display transforms must be modeled separately from persisted derived series.

Properties:

- local to a plot panel
- do not create global topics
- affect rendering only
- serialized as part of plot panel state

Examples:

- derivative for display only
- smoothing for display only
- scaling/offset

## 9. Transform and Scripting Design

### 9.1 Built-in transform registry

Create a registry with descriptors containing:

- stable ID
- display name
- category
- arity
- parameter schema
- output type policy
- factory function

The UI uses the descriptor metadata. The app-core uses the factories to bind to `DerivedEngine`.

### 9.2 Named derived series

Derived series definitions should contain:

- stable series ID
- user-facing name
- source bindings
- transform type
- parameter payload
- output dataset/topic placement
- enable/disable state

These definitions are owned by app-core and persisted in the workspace.

### 9.3 Formula editor

The formula editor should be built as a UI client of app-core definitions, not as a thin text box over arbitrary runtime code.

Responsibilities:

- browse available sources
- define expressions or transform graphs
- validate types and dependencies
- preview diagnostics
- apply mutations transactionally

### 9.4 Reactive scripting

Reactive scripting is required for parity. The architecture makes it **cheap** because the pieces already exist:

- the scripting runtime lives in `pj_scripting` (separate module; Lua today, Python later)
- the reactive hook is a Toolbox plugin that receives `onTimeChanged(timestamp)` and re-evaluates its script on tracker move
- the Toolbox host already exposes catalog read + datastore write capabilities

Needed parts:

- `pj_scripting::IScriptingEngine` (language-agnostic) with `LuaScriptingEngine` default
- Toolbox plugin that:
  - hosts a script document (loaded/saved via dialog config)
  - compiles on edit, evaluates on `onTimeChanged`
  - reports diagnostics through the Toolbox host's message channel
- workspace persistence of the toolbox's serialized script

The runtime exposes controlled access via native functions registered with the engine:

- tracker time (argument to every reactive eval)
- read access to series via the Toolbox host's `catalogSnapshot()` + `readSeries()`
- creation or update of derived outputs via the Toolbox host's write API

No special `pj_runtime` wiring is required — reactive scripts are a toolbox like any other.

### 9.5 Relationship with `DerivedEngine`

Use `DerivedEngine` for:

- persistent derived series
- built-in transform outputs
- formula-driven series where feasible

Use app-side runtime logic for:

- plot-local display transforms
- tracker-dependent reactive behaviors that are not best represented as persistent datastore topics

## 10. Playback and Time Model

### 10.1 Global tracker

Introduce one authoritative tracker/time service.

It owns:

- current tracker timestamp
- playback mode
- follow-live mode
- playback rate
- looping

### 10.2 Modes

Support at least these modes:

- static exploration
- live-follow
- paused live
- timed playback

### 10.3 Playback behavior

Playback service must:

- advance tracker based on real time and playback rate
- clamp or loop at range boundaries
- notify plots, inspectors, and scripts
- define whether playback drives visible range or only tracker position

### 10.4 Live behavior

For streams, the service must define:

- how retention interacts with visible plots
- how pause/resume interacts with follow-live
- what happens when the user manually pans away from latest data

The following default rules should be implemented:

- while follow-live is enabled, the visible time window tracks the newest data
- if the user manually pans or zooms away from the live edge, follow-live is disabled automatically
- once follow-live is disabled, incoming data continues to be ingested but the viewport does not jump
- an explicit user action is required to re-enable follow-live or jump back to latest
- pausing a stream preserves the current viewport and tracker behavior
- resuming a paused stream does not force a jump to latest unless follow-live is explicitly on

These rules must live in app-core, not inside the plot widget.

### 10.5 Session recording (raw capture)

Live data is bounded by memory retention and dies with the session. The host records the **raw messages** of
streaming sources — at the delegated-ingest seam (`ensureParserBinding` + `pushMessage`), never decoded samples —
into standard MCAP files whose channels keep the original encodings and schemas. Replay is the ordinary MCAP
loader plus the installed parsers. Rules fixed by the design (see
[`docs/session_recorder_design.md`](./docs/session_recorder_design.md)):

- raw only: sources that write decoded samples directly are not recordable until they expose messages
- subscribed topics only (demand-driven subscriptions define what a recording contains)
- overflow drops, never blocks: `pushMessage` never waits for the recorder; when a source's byte-budgeted write
  queue is full the largest queued message is dropped (or the incoming one, when nothing queued is bigger) and
  the recording carries on with holes; only a sink error truncates it (file kept, marked truncated), and the
  live view is unaffected either way
- global Record/Stop over all active streams by default; per-source recording optional
- one file per source: each press creates an auto-named batch folder in a user-configurable recordings folder,
  holding one MCAP per recorded source; files are never evicted
- parser policy (timestamp field, array limits) lives in the layout, not in the file: a recording carries
  decoding facts and provenance only, so replay needs no loader change
- the file at Stop is the product: the live source stays live and the recording opens like any MCAP; the live
  source is never switched onto its recording

The same recorder core later backs the **source cache** for bounded connector downloads (identity, host-owned
source-lifetime leases, budget), replacing per-plugin capture/format/loader code.

## 11. Extension Host Design

### 11.1 Extension catalog

Add a catalog service that tracks:

- installed extensions
- enabled state
- family and capabilities
- version
- load status
- diagnostics

This catalog is the source of truth for import/start-stream/toolbox availability.

### 11.2 Data source sessions

The data source runtime must support:

- one-shot import
- streaming source lifecycle
- pause/resume where supported
- config save/load
- parser binding for delegated ingest
- an ingest tap so the host can record delegated-ingest messages (§10.5)
- diagnostics routing

### 11.3 Parser sessions

The parser runtime must support:

- lookup by encoding
- parser dialog handling where available
- one parser instance per binding context
- stable config persistence

### 11.4 Toolbox sessions

Toolboxes are long-lived and UI-driven. The host must support:

- instance creation
- host binding
- panel embedding
- config persistence
- notification when data changes

### 11.5 Missing extension recovery

When loading a workspace that references a missing extension:

- workspace load must still succeed
- affected tool panels or sessions become degraded placeholders
- missing state is visible to the user
- reinstallation should allow future rebinding

## 12. Persistence and Settings

### 12.1 Workspace persistence

Workspace documents should include:

- tabs and layout
- plot contents and settings
- tool panel contents and settings
- derived series definitions
- scripts
- linked ranges
- extension-owned restorable payloads

### 12.2 User settings

User settings should include:

- window geometry
- theme
- recent files and workspaces
- last used directories
- last used per-extension configs
- marketplace preferences

### 12.3 Migration

Provide:

- versioned workspace documents
- migration functions between 4.x versions
- a possible future 3.x import adapter kept separate from native persistence

## 13. GUI Shell Plan

### 13.1 Main shell

The Qt shell should provide:

- main window
- workspace tabs
- docking/splitting
- toolbar and menus
- status and diagnostics surfaces

### 13.2 Required panels

The full app should eventually include:

- catalog/tree panel
- plot panels
- transform editor
- script editor
- toolbox panels
- statistics inspector
- extension diagnostics/log view
- marketplace window
- playback controls and time widgets

### 13.3 GUI constraints

Widgets should:

- render view models
- dispatch operations to app-core services
- hold only ephemeral visual state

Widgets should not:

- own extension handles
- own `DataEngine`
- own persistence logic
- define undo semantics

## 14. Suggested Delivery Phases

### Phase 0: Architectural foundation

Deliver:

- new top-level app modules
- `AppSession`
- concrete app-core service classes
- event bus or notification service
- workspace document schema
- settings service

Acceptance:

- app starts with empty workspace
- no business logic lives in the shell beyond wiring

### Phase 1: Data sessions and catalog

Deliver:

- extension catalog service
- import and streaming session manager
- catalog service
- Qt tree adapter

Acceptance:

- import and stream lifecycle works through app-core services
- catalog updates correctly

### Phase 2: Workspace and panel system

Deliver:

- tabs and layout model
- dock/split structure
- plot panel state model
- workspace persistence

Acceptance:

- multi-tab workspace restores correctly

### Phase 3: Plotting parity foundation

Deliver:

- lift PJ3 plot widget tree into `pj_plotting` (wholesale; see §5.3)
- `DatastoreCurveAdapter` rebinding data reads to `pj_datastore` via `TopicStorage::sealedChunks` (pull-through; see §8.2). No adapter-side downsampler — Qwt's paint-time filtering owns decimation.
- `IDataWidget` implementation; plot factory wired directly in `pj_app`
- linked X-axis zoom across plots: `MainWindow` owns the global Link toggle (`buttonLink`); plots emit `rectChanged`; broadcast updates peer plots via `setZoomRectangle(rect, /*emit=*/false)` to avoid echo recursion. XY plots are excluded from sync.
- tracker driven by `PlaybackEngine::currentTimeChanged` (both directions: timeline → tracker line; user-dragged tracker → timeline)
- curve color via stock `QColorDialog` from a right-click context menu

Deferred to a follow-up phase: `PlotWidgetEditor`, `PlotWidgetTransforms`, `StatisticsDialog`, `PlotBackground`, colormap selector / editor.

Acceptance:

- ordinary 3.x plotting workflows work with multiple panels, tabs, drag-drop curve add, sync zoom, tracker, and color change. The deferred dialogs return in the next phase.

### Phase 4: Derived transforms

Deliver:

- transform registry
- derived-series definitions
- transform editor
- `DerivedEngine` bindings

Acceptance:

- user can create and persist named derived series

### Phase 5: Reactive scripting

Deliver:

- concrete scripting engine
- editor
- persistence
- diagnostics

Acceptance:

- scripting-based workflows replace old Lua-equivalent tasks

### Phase 6: Toolbox hosting

Deliver:

- toolbox manager
- persistent tool panels
- workspace restore

Acceptance:

- toolbox panels behave as first-class workspace elements

### Phase 7: polish and stabilization

Deliver:

- snapshot-based undo/redo
- missing-extension degraded restore
- recent files/workspaces
- performance tuning
- UI refinement

Acceptance:

- app is viable as the primary replacement for 3.x

### Phase 8: session recording and source cache (parity-plus)

Deliver, in order (design: `docs/session_recorder_design.md`):

- M1 stream recording (implemented 2026-08-31, PR #616): record tap in `pj_runtime`, recorder + MCAP writer
  (one file per source in a batch folder), Record/Stop in the streaming strip, Preferences — no SDK change and
  no loader change (replay is the stock `data_load_mcap`); the MCAP sink is desktop-only (no mcap package in
  the wasm graph), the rest builds everywhere
- (M2 promote-at-stop — switching the live source onto its recording — was dropped 2026-09-01, D7; the
  numbering is kept)
- M3 source cache on the same core: attach/complete/adopt SDK ABI, identity via the SDK descriptor policy,
  host-owned source-lifetime leases, cache budget in Preferences; connector providers shrink to descriptor +
  trust + credentials + download
- M4a `parser_arrow` (pj-official-plugins, nanoarrow-based `arrow-ipc` MessageParser) — independent of #275, in progress since 2026-08-31
- M4b Mosaico transport/parser split; plugin-side capture, artifact format and loader retire (after the #275 decision)

Acceptance:

- a recorded ROS 2 / Foxglove session reopens from its file with the same plots
- a layout referencing a cloud download restores from the host cache or re-downloads headlessly, with no
  plugin-side cache code

## 15. Testing Strategy

### 15.1 Unit tests

Add unit tests for:

- workspace model mutations
- workspace persistence round-trip
- playback state transitions
- transform definition validation
- script binding validation
- extension catalog behavior
- session lifecycle logic

### 15.2 Headless integration tests

Add headless tests for:

- file import via app-core
- streaming via app-core
- delegated ingest
- toolbox-driven data modification
- derived series creation
- workspace restore without widgets

### 15.3 GUI integration tests

Add Qt integration tests for:

- drag and drop to plots
- tab and panel restore
- toolbox placement
- dialog workflows
- playback controls
- marketplace-triggered catalog refresh

### 15.4 Manual acceptance scenarios

Track manual scenarios for:

- large file import
- live streaming with retention
- derived series plus scripting in one workspace
- toolbox use in persisted workspaces
- missing extension recovery

## 16. Key Public Types and Services

Recommended stable internal API surface:

Core types:

- `AppSession`
- `WorkspaceDocument`
- `PlotPanelState`
- `ToolPanelState`
- `CurveBinding`
- `DerivedSeriesDefinition`
- `ReactiveScriptDefinition`
- `PlaybackState`
- `TrackerState`
- `DataSessionDescriptor`
- `ExtensionDescriptor`

Concrete services (in `pj_runtime` unless noted):

- `AppSession`
- `SessionManager`
- `WorkspaceManager`
- `PlaybackEngine`
- `CatalogModel`
- `WidgetRegistry`
- `TransformRegistry`
- `ToolboxManager`
- `UndoManager`
- `ExtensionCatalogService`
- `UserSettingsService`
- `DialogHost`
- `LuaScriptingEngine` (in `pj_scripting`)

## 17. Assumptions

- Qt remains the desktop UI framework.
- Marketplace-installed extensions remain the delivery vehicle for concrete loaders, streamers, parsers, and toolboxes.
- Advanced scripting stays in scope for the first feature-complete target.
- `StatePublisher` parity remains explicitly deferred.
- The final app should reuse current modules where appropriate, but not inherit the proto app structure as the long-term design.
