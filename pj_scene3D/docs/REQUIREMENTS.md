# `pj_scene3D` — Requirements

| | |
|---|---|
| Status | Shipped (v1 feature-complete: TF, pointclouds, occupancy grids, dense voxel grids, depth-image back-projection, markers, pose arrays, URDF/mesh, HDR/SSAO/EDL, live streaming) |
| Date | 2026-05-17 (rev. 2026-07-16) |
| Scope | What, not how |
| Supersedes | `PJ4_PLAN.md` §5.5 (refinement) |

This document is the source of truth for the *intent* of `pj_scene3D`. It deliberately avoids design-level prescriptions (API signatures, class layouts, CMake details, file structures). Those belong in a subsequent design document. When this document conflicts with `PJ4_PLAN.md` §5.5, this document wins.

> **Current architecture (supersedes the Phase-1 framing below).** The module
> ships integrated into `pj_app` (not as a standalone demo), and it **does not
> decode wire formats**. DataSource plugins (e.g. `parser_ros`,
> pj-official-plugins#122) decode ROS/CDR messages into canonical
> `pj_base/builtin` objects (`PointCloud`, `FrameTransforms`, `OccupancyGrid`,
> `OccupancyGridUpdate`) and publish them to the `ObjectStore`; `pj_scene3D`
> consumes those objects from the store on tracker change and renders them.
> Pointclouds, TF, and occupancy grids / costmaps are implemented. 3D markers
> (`SceneEntitiesLayer` — visualization_msgs/MarkerArray-equivalent
> `kSceneEntities` topics), URDF/mesh robot models (`RobotModelLayer`), and pose
> arrays (`PosesInFrameLayer` — `geometry_msgs/PoseArray` / `foxglove.PosesInFrame`
> drawn as per-pose coordinate-triad gizmos) are also implemented. The Phase-1 sections below are retained as historical intent
> — read "lazy host-side CDR decode of `PointCloud2`" as "consume the canonical
> `PointCloud` object the plugin already decoded".

## 1. Purpose & target users

PJ4's 3D visualization module — the sibling family to `pj_scene2D`, focused on the robotics-data subset: TF trees, rigid bodies via URDF, pointclouds, gridmaps, 3D markers and primitives, paths, laserscans.

**Target user**: roboticists who use PlotJuggler 4 to debug recorded or live data alongside their time-series plots, scrubbing back and forth through replay time. Inspirations: RViz2, Foxglove, Rerun.

## 2. Primary use cases

- Scrub recorded MCAP data back and forth through time while seeing the 3D scene react correctly at the same tracker time as 2D widgets and plots.
- Switch the rendering frame (fixed-frame) per widget to view the world from different reference points (`map` vs `odom` vs `base_link`, etc.).
- Run multiple 3D widgets side-by-side, each with its own fixed-frame and view, in the same PJ4 layout.
- Inspect a TF tree and understand frame relationships during debugging.

## 3. Supported data types

**Full v1 target:**
- Rigid bodies (TF + URDF meshes). *(implemented)*
- Occupancy grids / costmaps (`nav_msgs/OccupancyGrid` + incremental `OccupancyGridUpdate`, with stateful time-travel reconstruction). *(implemented — `OccupancyGridLayer`)*
- Gridmaps in the `grid_map_msgs/GridMap` elevation-map sense (textured/height planes in a source frame) remain **future work** — distinct from the occupancy grids above.
- Pointclouds (`sensor_msgs/PointCloud2` and equivalents). *(implemented)*
- Compressed pointclouds (`foxglove_msgs/CompressedPointCloud` + `point_cloud_interfaces/CompressedPointCloud2`), formats **Draco** and **Cloudini** — decoded into a `PointCloud` and rendered identically (see §3a). *(implemented)*
- 3D markers / visualization primitives (arrows, boxes, spheres, cylinders, line strips, text). *(implemented — `SceneEntitiesLayer`)*
- Pose arrays (`geometry_msgs/PoseArray`, `foxglove.PosesInFrame` → canonical `PosesInFrame`), drawn as per-pose coordinate-triad gizmos with customizable arrow length + opacity, an X-arrow-only geometry mode, and an orthogonal color override (one shared color across all arms, in either mode). *(implemented — `PosesInFrameLayer`)*
- Dense voxel grids (`foxglove.VoxelGrid` → canonical `VoxelGrid`), drawn as GPU-instanced cubes; the per-voxel value is generic (occupancy/cost/ESDF/semantic via `fields`, or a direct RGBA channel) and a viewer-side draw predicate + colormap decide which voxels are shown. The dense→cubes expansion is entirely GPU-side (one `glDrawElementsInstanced`, `gl_InstanceID`→texel), so the **CPU / draw-call** cost is independent of voxel count (the GPU vertex shader still evaluates the draw predicate per voxel). *(implemented — `VoxelGridLayer`)*
- Depth images back-projected into point clouds: an `sdk::Image` with a depth `encoding` joined to a `CameraInfo` by `frame_id`, back-projected one point per valid pixel and colored by depth. *(implemented — `DepthCloudLayer`)*
- Paths (`nav_msgs/Path`).
- Laserscans (`sensor_msgs/LaserScan`).

**Phase 1 subset** (two types):
- **TF axes** as 3D gizmos — substituting for the URDF mesh path since real mesh assets are not available for the Phase 1 input data. This is a permanent first-class display, not a placeholder. Hovering a triad shows the frame's name in a small label (screen-space pick of the nearest frame origin; see ARCHITECTURE.md "TF frame hover labels").
- **Pointclouds** (`sensor_msgs/PointCloud2`).

**WebAssembly status (W19m):** the browser product implements TF axes/parent
connections, line/checker grid, raw and Draco/Cloudini-compressed point clouds,
pose arrays, occupancy updates, dense voxel grids, procedural SceneEntities,
PBR ModelPrimitives, and topic/file/URL URDF robot models. It uses the shared
camera, fixed/follow, tracker replay, style, and layout contracts while enforcing
browser-specific decode, retained-memory, texture, model, and network budgets.
The QRhi/WebGL2 renderer provides mesh shadows and an HDR presentation chain
with SSAO and mesh-masked EDL; each optional pass fails independently to a live
lower-quality or direct-render path. Browser-selected local model bytes are
capabilities for the current session and are never persisted as reopenable host
paths. The desktop application continues to select the native OpenGL
implementation and dependency graph unchanged. Paths, laser scans, and the
Image+Pinhole view remain future work and must not be inferred from parser
availability alone.

Image+Pinhole (camera frustum + textured near-plane) from the original `PJ4_PLAN.md` §5.5 list is dropped from v1.

### 3a. Compressed point clouds (Draco / Cloudini)

A `CompressedPointCloud` canonical object carries `{timestamp, frame_id, format, data}` where
`data` is a self-describing codec blob. `pj_scene3D` decodes it into a `PointCloud` and renders it
through the **same** `PointCloudLayer` and `convertCanonical()` path as a raw cloud — one dual-mode
layer, no separate widget.

- **Formats:** `cloudini` (header-embedded schema; via `cloudini/1.2.2`) and `draco`
  (`draco/1.5.7` — pinned to 1.5.7 to match the native dependency graph;
  see `conanfile.txt`). Plain `zstd_point_cloud_transport` is **out of scope** — its blob is
  not self-describing (it relies on layout fields the canonical object does not carry).
- **Wire sources:** the ROS parser emits the canonical object for both
  `foxglove_msgs/CompressedPointCloud` (Foxglove ROS2 schema; its `pose` is read but dropped — clouds
  are placed via TF on `frame_id`) and `point_cloud_interfaces/CompressedPointCloud2`
  (ros-perception/point_cloud_transport). The parser only repackages bytes; it never decodes.
- **Decode location:** `core/pointcloud_codecs.cpp` (`decodeCompressedPointCloud`), `draco` + `cloudini`
  linked PRIVATE to `pj_scene3d_core` — mirrors `pj_scene2d_core` decoding JPEG/PNG.
- **Off the UI thread:** decode is CPU-heavy (Draco ≈100 ms for ~1M points), so it runs on the Qt
  thread pool (`QtConcurrent` + `QFutureWatcher`) with latest-wins coalescing; the decoded cloud is
  cached per sample — identity is the store entry's never-reused SequentialUID — so repaints and color-field
  changes don't re-decode, and a tracker tick that resolves to the already-pushed sample skips the
  re-conversion/upload entirely. One failed sample is memoized at a time: revisiting it clears the
  view (matching the raw path's malformed-cloud behavior) and restores its warning without another
  codec run; a later distinct failure may replace that memo. Fast scrubbing of very large clouds
  may show transient lag (the in-flight sample lands a frame late); async decode + the per-sample
  cache keep the UI responsive.
- **Browser limits:** the compressed wire payload is rejected before worker
  dispatch when it exceeds 64 MiB. The bounded codec API rejects Cloudini's
  declared point count and decoded byte size before its output allocation.
  Draco's header/metadata/count and packed float32 attribute layout are checked
  before attribute-value materialization. Both formats must fit 1,000,000 points
  and 64 MiB of decoded canonical bytes per sample. Each compressed layer owns
  one decoded-sample cache; this is not a global decoded-cache quota. The
  existing 1,000,000-vertex per-view budget remains the final aggregate rendering
  limit, while aggregate browser RSS is part of the W19 product-envelope audit.
- **Field fidelity:** Cloudini `INT64`/`UINT64` fields (no PJ datatype) are dropped (their bytes
  still occupy `point_step`, so surviving fields keep their offsets). Draco field names are
  recovered from Draco attribute metadata when present (Foxglove / draco_point_cloud_transport store
  the original name, e.g. `intensity`/`ring`), else inferred from attribute type (POSITION→x/y/z,
  COLOR→red/green/blue/alpha, NORMAL→nx/ny/nz, else `generic_N`).

### 3b. Frame trails

A **trail** visualizes the trajectory of a frame's origin across the whole loaded time range as a
polyline in the current fixed frame, split-colored at the tracker time (past = blue `#1f77b4`,
future = light blue `#9ecae1`; both per-trail configurable). Thickness is a per-trail parameter
(1-8 px, default 4, exact at any width — rendered as a screen-space ribbon because core GL caps
real lines at 1 px), and each half has its own eye toggle to show/hide the past or future segment
independently. Colors, thickness, and the two visibility flags all persist per-trail. Two sources:

- **TF frame** — any frame in the TF tree, sampled at the connecting chain's own update stamps
  (`TransformBuffer::chainSampleTimes`), so a statically-mounted child still trails with its
  moving ancestors.
- **Pose topic** — a `kPosesInFrame` topic, one point per message (the **first** pose only),
  each transformed into the fixed frame at that message's own timestamp.

Behavioral contract: whole-range always (no duration window in v1); vertex cap 50 000 with even
decimation (endpoints kept); TF-unresolvable samples are skipped (the strip bridges the gap);
scrubbing moves only the color split (no geometry rebuild — repaint-gate friendly); trails grow at
the live edge under streaming and ring-trim at the cap; a full rebuild under live retention can
only see the retention window. Deferred: duration window, orientation ticks, exact interpolated
split, strip-breaking at gaps, `PosesInFrame` array-as-path.

The two sources differ in how a trail is created, and therefore in what owns it:

- **TF trails are standalone layers.** Created by right-clicking a frame gizmo or from the panel's
  "Trail" row; they get their own Topics row and settings panel, and persist as a layout layer
  (`role="trail"`, synthetic local id). A missing FRAME merely orphans one (it revives if the frame
  returns), but unloading the bound TF dataset removes it.
- **Pose trails are owned by their pose layer.** A `kPosesInFrame` layer's settings panel carries a
  "Trail" toggle; enabling it builds a trail this layer owns outright, so it has no Topics row and
  no panel of its own — its thickness/colour rows are appended to the pose layer's own settings
  (the identical controls, via `TrailLayer::appendStyleRows`). It persists as a nested `<trail>`
  payload inside `<poses_in_frame>` — the payload's presence IS the enabled flag — and is torn
  down with its owner, so it needs no separate prune rule. The trail is opt-in because building it
  samples the topic's whole history.

  Known consequence, not yet decided: because copy/paste and "Apply to family" round-trip a layer
  through the same `xmlSaveState`/`xmlLoadState` pair, pasting also turns the trail on or off on
  the target — so applying to a family of pose layers builds a trail on each (a synchronous
  whole-history rebuild apiece), and pasting from a trail-less layer removes the target's trail.
  Separating "save for layout" from "save for paste" needs an `ISceneLayer` API change; until then
  this is the behavior.

## 4. Scene composition model

- **All layers are frame-locked, always.** On every render, a layer in source frame F is re-transformed via TF to the widget's fixed-frame at the current tracker time. There is no per-layer opt-out and no world-snapshot semantic (Foxglove's per-layer `frame_locked=false` toggle is intentionally not adopted).
- **Marker lifetime is honored; no fade/decay.** Most layers persist until the producer re-emits on the same key. `SceneEntitiesLayer` additionally honors each `SceneEntity`'s `lifetime_ns` (`0` = never expires): the entity is dropped once the tracker passes its expiry. Expiry is anchored on the **ingest (ObjectStore entry) timestamp** the entity was folded from — the host/tracker clock — not the entity's embedded sensor timestamp, which under live streaming rides a different epoch. There is still no hold-and-fade: an expired entity disappears, it does not decay.

## 5. Frame model

**Full v1**: per-widget **fixed-frame + display-frame + follow-mode**, with Foxglove-parity follow modes (Pose / Heading / Position / Off). Each widget independent — two widgets side-by-side can render the same scene against different frames with different cameras.

**Phase 1 subset**: per-widget **fixed-frame dropdown** + a **camera "Follow frame" picker** (the "Camera" section atop the scene-controls panel). Follow is **Position-only** so far: the active camera's anchor tracks the chosen frame's origin (in the fixed frame) each tracker tick, so the view pans to keep the frame in place while orbit/zoom stay world-referenced ("None" = off). A **recenter button** beside the picker snaps the camera onto the followed frame on demand (the on-demand counterpart to the no-jump-on-enable policy). The remaining follow modes (**Heading** = + yaw, **Pose** = + full orientation) and the separate display-frame + RViz-style Frames panel are Phase 2+. Both dropdowns list the **whole TF buffer** (every frame ever ingested, time-independent — `getFrameHierarchy()` takes no time argument) and are refreshed as transforms are folded in (on `datasetTransformsReady` / live ingest), so the tree fills in during a file load **without needing to press play**.

A **newly-created** widget does not blindly default to the `map`/`world`/`odom`/… heuristic root: if the user has already picked a fixed frame by hand for the same dataset's `TransformBuffer`, the new widget defaults to that remembered frame (when it is still present in the tree), falling back to the heuristic otherwise. The choice is shared in-session across sibling widgets on the dataset and persisted across restarts (see §10). It stays an *auto-mode* preference — picking it does not flip the widget to explicit, so it still self-heals if the frame later disappears. `TransformService::rememberFixedFrame`/`rememberedFixedFrame` own the memory; `Scene3DDockWidget::resolveAutoFixedFrame` consumes it.

## 6. Camera & interaction

**Phase 1 — RViz Orbit subset**:

| Gesture | Action |
|---|---|
| Left-drag | Rotate around focal point (azimuth + elevation) |
| Middle-drag (or shift+left-drag) | Pan focal point in screen plane |
| Wheel scroll | Zoom *toward focal point* — shrink radius, target fixed (not a dolly) |
| Right-drag | Axis-based zoom (RViz parity) |
| Toolbar reset | Restore default view |

**Coordinate convention**: ROS Z-up (per `PJ4_PLAN.md` §5.5).

**Implemented**: Orbit, XYOrbit, TopDownOrtho, and Fly (WASD / FPS) — all four selectable via the scene-controls combo. Zoom-to-cursor and Home control are also implemented. **Position-only "follow a frame"** composes with every model via the `ICamera::followShift(world_delta)` seam (Orbit/TopDown shift the focal, Fly the eye, XYOrbit XY-only to stay ground-locked); `SceneViewWidget::applyFollow` drives it per tracker tick using the delta of the followed origin so user orbit/zoom is preserved and enabling causes no jump.

**Still deferred**: Heading/Pose follow modes, bookmark / saved views, animated transitions, `F`-recenter-on-selected-drawable.

## 7. Multi-widget behavior

Multiple `SceneViewWidget` instances cohabit in a PJ4 layout (via the existing ADS docking system). Each widget is fully independent:
- Its own fixed-frame.
- Its own camera state.
- Its own per-display configuration.

No camera sync, no fixed-frame sync, no shared selection. Foxglove-style "sync camera across panels" is explicitly deferred from v1.

## 8. Performance goals (qualitative)

Performance is **benchmark-driven**, not pre-committed at the requirements stage. The pointcloud rendering pipeline in particular is to be discovered through the demo / benchmark phases, modeled on `pj_scene2D`'s mpv-vs-ffmpeg shootout.

**Headline target (post-Phase-1)**: aggregate ~10 million points snappy across N concurrent pointcloud displays in a single PJ4 process, characterized empirically against a stress-test MCAP set.

**Render loop policy** (Phase 1 forward):
- Vsync-capped via `QSurfaceFormat::setSwapInterval(1)` (~60 Hz on standard monitors).
- On-demand repaint: the widget invalidates only on tracker change, slider move, or camera interaction. No free-running redraw.
- Idle → 0 CPU.

This matches the pattern `pj_scene2D` already uses for video playback.

## 9. TF buffer requirements

The TF buffer is the central runtime data structure that makes scrub-replay work for 3D data. Its requirements:

- **Per-dataset**: one `TransformBuffer` per loaded dataset. Ownership lives in
  `TransformService` (`pj_scene3d_widgets`), not in `pj_runtime::SessionManager` — the
  runtime must stay domain-neutral and must not depend on 3D-specific types.
  `TransformService` exposes the buffer as a `shared_ptr` so every 3D dock attached to the
  same dataset shares the already-populated buffer without re-walking the store.
- **PJ4-native** at the type level: not coupled to ROS message types. DataSource plugins
  decode wire formats into canonical `FrameTransforms` objects and publish them to the
  `ObjectStore`; `TransformService` consumes those objects.
- **Filled at the ObjectStore ingest seam.** `TransformService::activateIngestTap()` claims the
  session's `FrameTransforms` consumer before runtime hosts start. Each source push resolves
  the topic's current parser binding, decodes synchronously on the pushing worker, and writes
  the thread-safe dataset buffer before the raw entry is registered in the `ObjectStore`.
  File-backed TF entries remain unseeded lazy catalog closures; streaming retains its captured
  payload because it has no durable source to re-read.
- **Transactional replacement.** An in-place reload writes TF to a private generation. Commit
  swaps that generation into the active dataset slot and notifies docks; abort drops it while
  the prior active buffer remains usable.
- **Explicit rebuild.** `ingestFrameTransformsForDataset()` remains the full ObjectStore drain
  for dataset merge and for services that did not activate the tap. The live
  `ingestNewTransforms()` path is a no-op while the tap is active; live-edge layer driving still
  keys off `TransformBuffer::revision()` and ObjectStore time ranges.

### Behavioral contract

Behavioral contract of `TransformBuffer` (`core/include/pj_scene3d_core/tf/tf_buffer.h`):

- **`T_target_from_source` convention** (same as ROS tf2): `lookupTransform(target, source, t)` returns the transform `T` such that `p_target = T * p_source`.
- **Tree, not DAG**: each child frame has exactly one parent. Two forms of malformed input are rejected: a reparent conflict (child already claimed under a different parent) returns `SetTransformError::ReparentConflict`; a self-loop edge (child == parent) returns `SetTransformError::SelfLoop` to prevent `chainToRoot` from hanging on corrupt TF data. Both are non-throwing — a bulk ingest continues past dropped edges.
- **Zero-order hold (ZOH) sample lookup**, *not* linear interpolation:
  - `t` equal to a stored stamp → that sample.
  - `t` strictly between two stamps → the *earlier* sample (sample-and-hold).
  - `t > latest stamp` → the latest sample (forward extrapolation by hold).
  - `t < earliest stamp on any edge` → no result (`NoSampleAtTime`).

  Transforms with a single sample (e.g. `/tf_static` entries) resolve for all
  `t >= their stamp` and hold forward indefinitely; for `t` strictly before
  that stamp, `NoSampleAtTime` is returned. In practice `/tf_static` is stamped
  at recording start, so this slice never occurs during normal scrub replay.

  This is a deliberate departure from tf2's default linear interpolation. The ZOH semantic answers "what was the system's belief at time *t*?" rather than "what would a smooth interpolation of the recorded data look like at time *t*?". For scrub replay over recorded data, the first question is the correct one — the system never actually held an interpolated pose.

- **Thread-safe**: concurrent writers (ingest thread) and readers (render thread + future Frames panel) must be supported without external locking.
- **Introspection**: callers must be able to enumerate all known frames, query the parent of any frame, and query the latest sample stamp for any edge. This is what the future Frames panel and the per-widget fixed-frame dropdown consume.
- **Non-throwing query variant** for the hot render path. A throwing variant remains available for explicit user code where a missing transform is a programmer error.

## 10. Persistence (implemented)

Per-widget fixed-frame, **camera follow target**, per-layer configuration + visibility, and the active camera model + pose are serialized into the PJ4 layout XML and restored on load. `Scene3DDockWidget::xmlSaveState`/`xmlLoadState` writes a versioned `<Scene3DDock>` (fixed-frame; `follow_frame` — empty = off; camera model as a stable string id + `CameraState` JSON; dataset re-resolution by source), and every layer implements its own `xmlSaveState`/`xmlLoadState`. A restored follow target absent from the current TF tree stays inert until it appears. The as-built schema (camera persistence + the per-layer round-trips) lives in `ARCHITECTURE.md`.

Separately from the layout XML, the **last fixed frame a user picked by hand for a dataset** is remembered across app restarts in `QSettings` under `pj_scene3d/fixed_frame_by_source`, keyed by the dataset's source path + name (the same identity pair layout restore matches datasets by — *not* the per-session `DatasetId`; the path qualifier keeps two same-named files in different folders from sharing a frame, while fan-out members sharing a path stay distinct by name; a pathless source with a stable name keys by name alone). This is the cross-restart half of the §5 "new widget defaults to the remembered frame" behavior; a layout's explicitly-saved fixed frame still wins for the specific widget it was saved on, and a dataset with no source name (e.g. an unnamed live stream) is remembered for the session only.

## 11. Plugin / extension surface (deferred from v1)

Third-party drawable type registration by plugins. Out of v1 scope. Built-in drawable types only in v1.

## 12. Non-goals (explicit)

- **StatePublisher / click-to-publish** (nav goal, 2D pose estimate, point publishing). PJ4 stays viewer-only; the 3D scene never writes back to the system.
- **Measurement tool / snap-to-object.**
- **Custom grid layer.**
- **Map tile layer** (street, satellite, etc. — Foxglove Pro feature).
- **Hot reload** of plugin-provided drawable types.
- **macOS / Windows support** in v1 (Linux-only per `CLAUDE.md`).
- **Per-layer Foxglove-style `frame_locked` opt-in** — chose always-locked (§4).
- **Decay / fade-out animation** — an expired marker disappears, it does not fade. (Marker `lifetime_ns` expiry itself **is** implemented — see §4.)
- **Out-of-core / surveying-scale pointclouds** (>500M points).
- **octomap** (adaptive octree with variable-size leaves) — belongs in SceneEntities/cube primitives, not the uniform-lattice `VoxelGrid`. (Dense voxel grids — costmap-3D / ESDF / semantic — *are* supported via `VoxelGridLayer`; see §3.)

## 13. Phase 1 acceptance — explicit

Phase 1 ships an **end-to-end MCAP-driven demo binary** that exercises the substrate, the adapted TF buffer, the lazy pointcloud pipeline, and the per-widget UI. It is *not* a substrate-only proof; it is the smallest demo that proves the foundation works on real data.

### Phase 1 input

A ROS2 MCAP file (provided by the user) containing at minimum:
- `tf2_msgs/TFMessage` transforms.
- `sensor_msgs/PointCloud2` pointcloud(s).

### Phase 1 ingest behavior

- **TF**: eagerly decoded at load. Every `TFMessage` sample pushed into the per-dataset TF buffer.
- **PointCloud2**: **lazily decoded on tracker change**, mirroring `pj_scene2D`'s MCAP lazy-media pattern. Raw CDR bytes remain indexed in MCAP at open; decoding occurs on demand through a small LRU cache for recently decoded clouds.

### Phase 1 rendering

- **TF axes**: one XYZ axis triad (red = X, green = Y, blue = Z) per frame in the active dataset's TF buffer. Fixed world-space size (per-widget uniform, e.g., 0.3 m). Frame names show on hover (see above). An optional per-widget **parent-connection** overlay draws a magenta line from each frame to its parent (rviz2 / Foxglove "show parent connections"), toggled by the graph button on the "Frames size" row of the scene-controls panel; default on, persisted per-dock in the layout. Geometry is the Qt/GL-free `core/tf/tf_connections.h` helper; the GL shell is `TfConnectionsRenderPass`.
- **Origin grid**: a 10×10 grid of 1-metre squares on the ground plane (Z = 0) at the world origin, drawn in a subdued color. Always visible in Phase 1 (no per-widget toggle yet). Renders in the fixed-frame (i.e., translates/rotates with whichever frame the user selects as fixed).
- **Pointcloud**: rendered as `GL_POINTS` (single VBO, no LOD), in its source frame, re-transformed to the widget's fixed-frame each frame via the TF buffer.
- **Pointcloud coloring**: a per-display dropdown selects the source scalar field from `{ X, Y, Z, intensity, ring }`. The dropdown is populated by inspecting the first decoded cloud's `fields` schema (the `timestamp` field is explicitly ignored). The selected field drives a **colormap** — Phase 1 shipped Turbo only; the current build offers the shared `PJ::Colormap` set (turbo / viridis / plasma / grayscale, from `pj_widgets/Colormap.h`, the same colors as the 2D depth view). For a **spatial axis (X/Y/Z)** the colormap input is the coordinate in the **fixed frame**, computed on the GPU from the same source→fixed transform that places the geometry — so sensors at different mounts agree on world height (the raw x/y/z field is sensor-local). Its auto-range is derived per frame from the cloud's source-AABB transformed by that live model, so colour and range track TF motion without a re-decode. Non-spatial fields (intensity, ring, …) colour by the raw per-point value and auto-fit to the cloud's min/max; both spatial and non-spatial ranges are held across tracker scrub so colors stay stable.
  - **RGB-direct mode**: when a cloud carries a per-point colour, the dropdown does **not** list the colour channels as individual scalar fields. Instead a single **"RGB"** entry paints each point its literal colour (no colormap), and it is the **default** colour mode for such clouds (matching Foxglove Studio / rviz). The canonical representation is a single `rgba` field of datatype `kUint32` whose 4 bytes are R, G, B, A in increasing-address order; the DataSource parsers normalize to it (`parser_protobuf` collapses foxglove's separate `red`/`green`/`blue`/`alpha` uint8 channels; `parser_ros` repacks a PCL-packed `rgb`/`rgba` `0x00RRGGBB` field), so the host renders one canonical colour and never sees per-source byte order. The host (`detectColorLayout` in `pointcloud_convert`) also defensively recognizes raw channels, so an un-normalized source still renders true colour. `alpha` is treated as opaque padding (ignored for blending); see `pointcloud_layer.cpp` (mode selection / persistence) and `passes/pointcloud_render_pass.cpp` (`ColorType::kRgb`).

### Phase 1 UI

- **Per-widget fixed-frame dropdown**: populated from the TF buffer's frame enumeration. Switching re-renders within one vsync.
- **Time slider**: drives the demo's tracker time. Dragging back and forth scrubs correctly:
  - TF axes snap to ZOH positions between samples (no glide).
  - Pointcloud re-decodes from cache or MCAP without visible stall on a cache miss for typical cloud sizes.
- **Render loop**: vsync + on-demand repaint (no free-running redraw).

### Phase 1 acceptance criteria

The demo passes Phase 1 when, on the user-provided MCAP, all of the following hold:

1. Loads without error.
2. TF axes render at the position of each frame, expressed in the currently selected fixed-frame.
3. A 10×10 origin grid (1m squares, ground plane Z=0) renders at the fixed-frame's origin.
3. The fixed-frame dropdown lists every frame present in the MCAP's TF data.
4. The color-field dropdown lists every numeric field present in the first decoded pointcloud (excluding `timestamp`).
5. The slider scrubs through the full time range of the MCAP.
6. Dragging the slider back and forth produces correct ZOH axis snapping and stutter-free pointcloud updates on cache hits.
7. `Scene` is implemented with hardcoded `axes_` and `pointcloud_` members; no premature `IDrawable` polymorphism. (Polymorphism arrives in Phase 2 when the third drawable type lands.)
8. No crashes. `GL_DEBUG_OUTPUT` is clean.
9. Render loop sustains 60 Hz under typical scrub load on integrated graphics; idle CPU is approximately zero.

### Explicitly out of Phase 1

assimp, URDF, tinyply, PCD reader, RGB-direct color mode, additional colormaps beyond Turbo, display-frame, follow-mode, TF Frames panel, picking, measurement, click-to-publish, gridmaps, paths, laserscans, integration into `pj_app` (the Phase 1 demo is a standalone binary, mirroring `pj_scene2D/tools/`).

> **Note (post–Phase 1):** several of the above have since shipped — URDF/mesh
> models (assimp), occupancy grids, the additional colormaps, depth-image
> back-projection (`DepthCloudLayer`), the **RGB-direct color mode** (see
> *Pointcloud coloring* above), and full `pj_app` integration. This list
> is the historical Phase-1 boundary; for the current as-built feature set see
> `CLAUDE.md` and `ARCHITECTURE.md`.

> **Note (post–Phase 1):** several of the above have since shipped — URDF/mesh
> models (assimp), occupancy grids, the additional colormaps, depth-image
> back-projection (`DepthCloudLayer`), and full `pj_app` integration. This list
> is the historical Phase-1 boundary; for the current as-built feature set see
> `CLAUDE.md` and `ARCHITECTURE.md`.

> **Note (post–Phase 1):** several of the above have since shipped — URDF/mesh
> models (assimp), occupancy grids, the additional colormaps, depth-image
> back-projection (`DepthCloudLayer`), the **RGB-direct color mode** (see
> *Pointcloud coloring* above), and full `pj_app` integration. This list
> is the historical Phase-1 boundary; for the current as-built feature set see
> `CLAUDE.md` and `ARCHITECTURE.md`.

## 14. Stack (locked)

- **GPU API**: OpenGL 4.5 core via `QOpenGLWidget`.
- **Math**: GLM (column-major, GLSL-matching types).
- **Canonical objects in**: `pj_base/builtin` schemas read from the `ObjectStore`
  (no wire/CDR decoding in this module — see the architecture note at the top).
- **Test framework**: `gtest` (matches `pj_scene2D`).

`pj_scene3D` (OpenGL) and `pj_scene2D` (QRhi) form a mixed GPU stack inside the same PJ4 process. They are sibling widget families that never share GPU state. An `IRenderPass` invariant in the substrate keeps GL calls confined so a future QRhi swap would be a contained refactor rather than a rewrite.

## 15. TF prototype adaptation summary

The TF prototype has been promoted into `pj_scene3D/core/include/pj_scene3d_core/tf/` as the `TransformBuffer` class (`tf/tf_buffer.h`, `tf/transform.h`) — single source of truth, no wrapper layer. The edits applied during promotion were:

1. **`sampleAt` rewritten to zero-order hold** (nearest sample at or before `t`), replacing the prototype's lerp+slerp interpolation. The free-function `interpolate()` becomes unused inside the buffer (may be deleted or kept exposed as a free utility).
2. **`TimePoint` retyped** from `std::chrono::steady_clock::time_point` to a replay-time-friendly type compatible with `PlaybackEngine`'s tracker time. (Final concrete type chosen at integration time.)
3. **Internal `std::shared_mutex`** guarding the buffer: `setTransform` exclusive; `lookupTransform`, `tryLookupTransform`, `canTransform`, `latestCommonTime`, and the introspection getters take shared locks.
4. **Introspection getters**:
   - enumerate all known frames,
   - query the parent of a frame,
   - query the latest sample stamp for a frame's edge.
5. **Non-throwing query variant**: `tryLookupTransform(target, source, t) → PJ::Expected<Transform, LookupError>` — the primary accessor for the render hot path; the typed `LookupError` enum (`UnknownSource`/`UnknownTarget`/`Disconnected`/`NoSampleAtTime`) keeps render-loop misses allocation-free. The throwing `lookupTransform` is a thin wrapper over it.
6. **`setTransform` normalizes the quaternion on insert** to defend against ingest noise accumulating through composed transforms.

**Unchanged** from the prototype:
- The `T_target_from_source` convention.
- Tree-not-DAG enforcement: a reparent conflict (a child already claimed under a different parent) and a self-loop edge (child == parent) are both rejected — `setTransform` returns `PJ::unexpected(SetTransformError::ReparentConflict)` or `SetTransformError::SelfLoop` respectively, dropping that one edge (non-throwing) so a bulk ingest continues. Self-loop rejection fires first to prevent `chainToRoot` from hanging on malformed TF data.
- Common-ancestor walk algorithm.
- Per-edge `std::deque` sample storage with 10-second cache window pruning.
- The `Transform` struct (translation + unit quaternion).
