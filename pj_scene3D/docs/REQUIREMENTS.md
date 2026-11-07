# `pj_scene3D` — Requirements

| | |
|---|---|
| Status | Shipped (v1 feature-complete: TF, pointclouds, occupancy grids, dense voxel grids, depth-image back-projection, markers, pose arrays, URDF/mesh, HDR/SSAO/EDL, live streaming) |
| Date | 2026-05-17 (rev. 2026-07-16) |
| Scope | What, not how |
| Supersedes | [Archived master plan](../../docs/archive/PJ4_PLAN.md) §5.5 |

This document is the source of truth for the *intent* of `pj_scene3D`. It deliberately avoids design-level prescriptions (API signatures, class layouts, CMake details, file structures). Those belong in a subsequent design document. The original master plan is retired; this document defines the current scope.

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
- Grid maps (`grid_map_msgs/GridMap`, `foxglove.Grid` → canonical `GridMap`, SDK ≥ 0.26.0), drawn as a lit, colormapped heightfield surface: one scalar field displaces the plane (or "None" for a flat plane), another colors it through the shared colormap with an auto or manual range; NaN cells are holes. Geometry is procedural (derived from the vertex index, no vertex or index buffer), so a new sample costs two R32F texture uploads. Every build, the browser included. *(implemented — `GridMapLayer`)*
- Pointclouds (`sensor_msgs/PointCloud2` and equivalents). *(implemented)*
- Compressed pointclouds (`foxglove_msgs/CompressedPointCloud` + `point_cloud_interfaces/CompressedPointCloud2`), formats **Draco** and **Cloudini** — decoded into a `PointCloud` and rendered identically (see §3a). *(implemented)*
- 3D markers / visualization primitives (arrows, boxes, spheres, cylinders, line strips, text). *(implemented — `SceneEntitiesLayer`)*
- Pose arrays (`geometry_msgs/PoseArray`, `foxglove.PosesInFrame` → canonical `PosesInFrame`), drawn as per-pose coordinate-triad gizmos with customizable arrow length + opacity, an X-arrow-only geometry mode, and an orthogonal color override (one shared color across all arms, in either mode). *(implemented — `PosesInFrameLayer`)*
- Dense voxel grids (`foxglove.VoxelGrid` → canonical `VoxelGrid`), drawn as GPU-instanced cubes; the per-voxel value is generic (occupancy/cost/ESDF/semantic via `fields`, or a direct RGBA channel) and a viewer-side draw predicate + colormap decide which voxels are shown. The dense→cubes expansion is entirely GPU-side (one instanced draw, instance id → texel), so the **CPU / draw-call** cost is independent of voxel count (the GPU vertex shader still evaluates the draw predicate per voxel). *(implemented — `VoxelGridLayer`)*
- Depth images back-projected into point clouds: an `sdk::Image` with a depth `encoding` joined to a `CameraInfo` by `frame_id`, back-projected one point per valid pixel and colored by depth. *(implemented — `DepthCloudLayer`)*
- Paths (`nav_msgs/Path`).
- Laserscans (`sensor_msgs/LaserScan`).

**Phase 1 subset** (two types):
- **TF axes** as 3D gizmos — substituting for the URDF mesh path since real mesh assets are not available for the Phase 1 input data. This is a permanent first-class display, not a placeholder. Hovering a triad shows the frame's name in a small label (screen-space pick of the nearest frame origin; see ARCHITECTURE.md "Frame picking").
- **Pointclouds** (`sensor_msgs/PointCloud2`).

**WebAssembly status (W19m):** the browser product implements TF axes/parent
connections, line/checker grid, raw and Draco/Cloudini-compressed point clouds,
pose arrays, occupancy updates, dense voxel grids, grid maps, trails, procedural SceneEntities,
PBR ModelPrimitives, and topic/file/URL URDF robot models. It uses the shared
camera, fixed/follow, tracker replay, style, and layout contracts while enforcing
browser-specific decode, retained-memory, texture, model, and network budgets.
The QRhi/WebGL2 renderer provides mesh shadows (robot models and model
primitives; markers cast none) and an HDR presentation chain with SSAO and
mesh-masked EDL; each optional pass fails independently to a live
lower-quality or direct-render path. Browser-selected local model bytes are
capabilities for the current session and are never persisted as reopenable host
paths. The desktop application uses the same QRhi renderer on the platform's
native API (OpenGL on Linux, D3D11 on Windows, Metal on macOS). Paths, laser scans, and the
Image+Pinhole view remain future work and must not be inferred from parser
availability alone.

Image+Pinhole (camera frustum + textured near-plane) from the archived master plan’s §5.5 list is dropped from v1.

### 3a. Compressed point clouds (Draco / Cloudini)

A `CompressedPointCloud` canonical object carries `{timestamp, frame_id, format, data}` where
`data` is a self-describing codec blob. `pj_scene3D` decodes it into a `PointCloud` and renders it
through the **same** `PointCloudLayer` and `packPointVertices()` path as a raw cloud — one dual-mode
layer, no separate widget.

- **Formats:** `cloudini` (header-embedded schema; via `cloudini/1.4.1`) and `draco`
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
(1-8 px, default 4, exact at any width — rendered as a screen-space ribbon because GPU line
primitives are 1 px wide on most backends), and each half has its own eye toggle to show/hide the past or future segment
independently. Colors, thickness, and the two visibility flags all persist per-trail. Two sources:

- **TF frame** — any frame in the TF tree, sampled at the connecting chain's own update stamps
  (`TransformBuffer::chainSampleTimes`: the stamps at which an edge's value changed), so a
  statically-mounted child still trails with its moving ancestors.
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

- **Every layer is re-placed on every render, at a time fixed by its kind (TF R6).** A layer that draws one stamped object (point cloud, depth cloud, pose array and its trail, voxel grid, grid map) is placed where its source frame was at the object's own header stamp, read in the TF header clock, as RViz does: between two samples it stays where it was measured instead of moving with the frame. The placement goes through the root of the TF tree (`fixed <- root` at the cursor times `root <- source` at the stamp), so with a moving fixed frame (`base_link`) a map-frame object stays put in the world and a scan stays where it was captured, both moving in the view as the body moves. An unstamped object, one whose stamp predates the TF tree, or one stamped more than 1 s past the newest TF on its chain falls back to the cursor; a placed object whose TF sample live streaming has since evicted keeps its last placement. Every other layer (occupancy grid, scene entities, robot model) and the TF overlay are placed at the current tracker time in the entry clock. There is no per-layer toggle (Foxglove's per-entity `frame_locked` is not yet honoured; scene entities stay at the cursor). The per-consumer table is in `docs/derived_objects/ARCHITECTURE.md` §12.
- **Marker lifetime is honored; no fade/decay.** Most layers persist until the producer re-emits on the same key. `SceneEntitiesLayer` additionally honors each `SceneEntity`'s `lifetime_ns` (`0` = never expires): the entity is dropped once the tracker passes its expiry. Expiry is anchored on the **ingest (ObjectStore entry) timestamp** the entity was folded from — the host/tracker clock — not the entity's embedded sensor timestamp, which under live streaming rides a different epoch. There is still no hold-and-fade: an expired entity disappears, it does not decay.

## 5. Frame model

**Full v1**: per-widget **fixed-frame + display-frame + follow-mode**, with Foxglove-parity follow modes (Pose / Heading / Position / Off). Each widget independent — two widgets side-by-side can render the same scene against different frames with different cameras.

**Phase 1 subset**: per-widget **fixed-frame dropdown** + a **camera "Follow frame" picker** (the "Camera" section atop the scene-controls panel). Follow is **Position-only** so far: the active camera's anchor tracks the chosen frame's origin (in the fixed frame) each tracker tick, so the view pans to keep the frame in place while orbit/zoom stay world-referenced ("None" = off). A **recenter button** beside the picker snaps the camera onto the followed frame on demand (the on-demand counterpart to the no-jump-on-enable policy). The remaining follow modes (**Heading** = + yaw, **Pose** = + full orientation) and the separate display-frame + RViz-style Frames panel are Phase 2+. Both dropdowns list the **whole TF buffer** (every frame ever ingested, time-independent — `getFrameHierarchy()` takes no time argument) and are refreshed as transforms are folded in (on `datasetTransformsReady` / live ingest), so the tree fills in during a file load **without needing to press play**.

A **newly-created** widget does not blindly default to the `map`/`world`/`odom`/… heuristic root: if the user has already picked a fixed frame by hand for the same dataset's `TransformBuffer`, the new widget defaults to that remembered frame (when it is still present in the tree), falling back to the heuristic otherwise. The choice is shared in-session across sibling widgets on the dataset and persisted across restarts (see §10). It stays an *auto-mode* preference — picking it does not flip the widget to explicit, so it still self-heals if the frame later disappears. `FixedFrameMemory` (`pj_scene3d_widgets`; a view preference, not TF data) owns the memory; `Scene3DDockWidget::resolveAutoFixedFrame` consumes it.

## 6. Camera & interaction

**Phase 1 — RViz Orbit subset**:

| Gesture | Action |
|---|---|
| Left-drag | Rotate around focal point (azimuth + elevation) |
| Middle-drag (or shift+left-drag) | Pan focal point in screen plane |
| Wheel scroll | Zoom *toward focal point* — shrink radius, target fixed (not a dolly) |
| Right-drag | Axis-based zoom (RViz parity) |
| Toolbar reset | Restore default view |

**Coordinate convention**: ROS Z-up.

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
- Vsync-paced: frames present through the top-level window's swap chain (~60 Hz on standard monitors).
- On-demand repaint: the widget invalidates only on tracker change, slider move, or camera interaction. No free-running redraw.
- Idle → 0 CPU.

The measured budgets, the performance gate and the current baseline are in
[TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md) §6 and §9.

This matches the pattern `pj_scene2D` already uses for video playback.

## 9. TF buffer requirements

The TF buffer is the central runtime data structure that makes scrub-replay work for 3D data. Its requirements:

- **Per-dataset**: one `TransformBuffer` per loaded dataset. The type lives in the Qt-free
  `pj_tf` leaf (`pj::tf`); ownership lives in `PJ::TransformService` (`pj_runtime`), because
  the buffer follows the data's life cycle, which is the runtime's. The runtime stays
  domain-neutral: it depends on `pj_tf`, never on `pj_scene3D`.
  `TransformService` exposes the buffer as a `shared_ptr` so every 3D dock attached to the
  same dataset shares the already-populated buffer without re-walking the store.
- **One dataset per dock.** A 3D dock binds the TF buffer of the first dataset it shows and
  refuses a topic from any other dataset, with a reason the shell shows the user (a toast); a
  saved layout that mixes datasets still loads and refuses the foreign element the same way.
  The dock can bind another dataset once its current dataset's topics are gone.
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
  gives that generation's state to the active buffer, which keeps its identity, and notifies
  docks; abort drops it while the prior active buffer remains usable.
- **Explicit rebuild.** `ingestFrameTransformsForDataset()` remains the full ObjectStore drain
  for dataset merge and for services that did not activate the tap. The live
  `ingestNewTransforms()` path is a no-op while the tap is active; live-edge layer driving still
  keys off `TransformBuffer::revision()` and ObjectStore time ranges.

### Behavioral contract

Behavioral contract of `TransformBuffer` (`pj_tf/include/pj_tf/tf_buffer.h`):

- **`T_target_from_source` convention** (same as ROS tf2): `lookupTransform(target, source, t)` returns the transform `T` such that `p_target = T * p_source`.
- **Tree, not DAG**: each child frame has exactly one parent. Two forms of malformed input are rejected: a reparent conflict (child already claimed under a different parent) returns `SetTransformError::ReparentConflict`; a self-loop edge (child == parent) returns `SetTransformError::SelfLoop` to prevent `chainToRoot` from hanging on corrupt TF data. Both are non-throwing — a bulk ingest continues past dropped edges.
- **Two clocks (TF R6).** Each sample keeps its header stamp and its entry time (the store timestamp of the message that carried it). Every lookup names its clock: `Clock::kEntry` returns, per edge, the newest-stamped sample that had arrived by `t` (the cursor's clock); `Clock::kHeader` returns the newest sample stamped at or before `t` (an object's own stamp). With header == entry for every sample the two agree. The rules below hold in either clock, with "stamp" read as that clock's time.
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

  **Repeated identical samples are not stored.** An in-order sample that is newer than the edge's last stored sample and exactly equal to it (component-wise, no tolerance) is dropped: under ZOH it adds nothing, and `revision()` does not bump unless the repeat evicts something. The edge still records the newest stamp and entry time it received: `getLatestSample` returns the stamp, and both bound `latestCommonTime` and the streaming eviction cutoff in their clock. A repeat whose entry time is earlier than the stored sample's still counts in the entry clock (it is folded into that sample). Eviction drops a sample only when neither clock's window still needs it. Eviction keeps the newest sample at or before the cutoff (it holds the value valid there, however old), so a fixed joint republished at 100 Hz keeps resolving across the whole window as if every repeat had been kept. An out-of-order sample landing inside a run of skipped repeats keeps its value until the next stored sample, where the full history would have returned the repeated value again (accepted: rare, and exact handling would need the skipped stamps).

- **Thread-safe**: concurrent writers (ingest thread) and readers (render thread + future Frames panel) must be supported without external locking.
- **Introspection**: callers must be able to enumerate all known frames, query the parent of any frame, and query the latest sample stamp for any edge. This is what the future Frames panel and the per-widget fixed-frame dropdown consume.
- **Non-throwing query variant** for the hot render path. A throwing variant remains available for explicit user code where a missing transform is a programmer error.

## 10. Persistence (implemented)

Per-widget fixed-frame, **camera follow target**, per-layer configuration + visibility, and the active camera model + pose are serialized into the PJ4 layout XML and restored on load. `Scene3DDockWidget::xmlSaveState`/`xmlLoadState` writes a versioned `<Scene3DDock>` (fixed-frame; `follow_frame` — empty = off; camera model as a stable string id + `CameraState` JSON; dataset re-resolution by source), and every layer implements its own `xmlSaveState`/`xmlLoadState`. A restored follow target absent from the current TF tree stays inert until it appears. The as-built schema (camera persistence + the per-layer round-trips) lives in `ARCHITECTURE.md`.

A robot model may carry **package URL roots** (`package_url_roots`, a package name → directory URL map) so its `package://` meshes load from the web even when the URDF arrives on a topic. A layout may write them relative to the layout file, and a URL-sourced URDF's relative roots and meshes follow the URDF's final URL; a layout without a location (a browser upload) must use absolute roots, and a web-hosted layout may name no local file (file source, local URL source or `file://` root). A root authorizes only its own package's fetches, never a topic URDF's direct `http(s)` references. On the desktop an `http(s)` root counts only for a URL-sourced robot or a robot of a web-hosted layout, so a layout file cannot send a topic or file robot's meshes to the network: such a root is skipped and the package is looked up on this computer, while the layout keeps it.

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

## 13. Stack (locked)

- **GPU API**: QRhi via `QRhiWidget` (OpenGL 3.3+ / Vulkan on Linux, D3D11 on
  Windows, Metal on macOS, WebGL2 in the browser).
- **Math**: GLM (column-major, GLSL-matching types).
- **Canonical objects in**: `pj_base/builtin` schemas read from the `ObjectStore`
  (no wire/CDR decoding in this module — see the architecture note at the top).
- **Test framework**: `gtest` (matches `pj_scene2D`).

`pj_scene3D`, `pj_scene2D` and the plot canvas all render through QRhi inside the same PJ4 process, one graphics API per top-level window. They are sibling widget families that never share GPU state.

## 14. TF prototype adaptation summary

The TF prototype has been promoted into the `pj_tf` module as the `TransformBuffer` class (`pj_tf/tf_buffer.h`, `pj_tf/transform.h`) — single source of truth, no wrapper layer. The edits applied during promotion were:

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
