# Intro

The purpose of this module is to implement 3D visualization of robotics data
(TF, pointclouds, occupancy grids / costmaps, meshes / URDF robot models,
scene entities (markers), pose arrays (`PosesInFrame`, drawn as per-pose
coordinate-triad gizmos), dense voxel grids (`VoxelGridLayer`, drawn as
GPU-instanced cubes), depth images back-projected into point clouds
(`DepthCloudLayer`), and motion **trails** (`TrailLayer`: a frame's — or a
pose topic's first-pose — trajectory over the whole time range as a
split-colored screen-space ribbon. A TF trail is a standalone layer; a pose
trail is instead OWNED by its `PosesInFrameLayer` (a "Trail" toggle in that
layer's settings, no Topics row of its own) — see ARCHITECTURE.md →
"Trail layer"), and grid maps (`GridMapLayer`: an `sdk::GridMap` drawn as a lit,
colormapped heightfield from procedural `gl_VertexID` geometry — see
ARCHITECTURE.md → "Grid-map layer")); `PosesInFrame` array-as-path rendering and
laserscans remain future work. Sibling widget family to `pj_scene2D`.

`VoxelGridLayer` consumes `sdk::VoxelGrid` (SDK ≥ 0.10.0): a dense 3D lattice
whose per-voxel value is generic via `fields` (occupancy/cost/ESDF/semantic, or a
direct RGBA channel). The dense→cubes expansion happens entirely on the GPU —
`VoxelGridRenderPass` uploads the selected field as a 3D texture and issues ONE
`glDrawElementsInstanced` over a unit cube (`column*row*slice` instances); the
vertex shader derives each voxel from `gl_InstanceID`, `texelFetch`es its value,
evaluates the viewer-side draw predicate (which the schema does NOT encode), and
degenerate-clips culled voxels. So the CPU / draw-call cost is independent of voxel
count (one instanced draw; the GPU vertex shader still runs once per voxel) and a
re-scrub to a cached grid re-uploads nothing. The Qt-free coordinate/value math
(`core/voxel_grid_view.{h,cpp}`, `core/voxel_grid_value.{h,cpp}`) is unit-tested.
A GPU compute-shader compaction path (`glDrawElementsIndirect` over only the
accepted voxels, GL ≥ 4.3) is a documented follow-up for very large dense grids.

Depth images arrive as `sdk::Image` with a depth `encoding` (there is no
`kDepthImage` producer — the parser can't distinguish depth from color at
schema-classification time), so `DepthCloudLayer` is registered on `kImage` and
the dock gates topics by peeking the first sample's `encoding`
(`isDepthEncoding`). Intrinsics come from a `CameraInfo` joined to the image by
**`frame_id`** — the authoritative key (never the topic name, matching Foxglove's
`frame_id` rule / Rerun's camera hierarchy). An exact `frame_id` match wins; with
no match `resolveIntrinsics` falls back to a lone `CameraInfo` only when
unambiguous (a single camera, or a frame-less image) and otherwise refuses rather
than pair the wrong camera. The back-projection math is the Qt-free core
`depth_backproject.{h,cpp}`; the POC feeds the result through the existing
`PointcloudRenderPass` (a GPU attributeless pass is a planned follow-up).

Drag-drop: because depth and color share `kImage`, an empty-placeholder drop of
*any* image opens the **2D** viewer (host policy in `pj_app`); a depth image
reaches a 3D dock by being dropped onto an existing one (`addTopic`'s encoding
gate absorbs depth, refuses color) or via the 3D family switch on an empty dock.
A one-click "Open in 3D view" curve-list action is a planned convenience.

The detailed set of requirements and goals lives in `pj_scene3D/docs/REQUIREMENTS.md`;
read it before changing this module's scope or behavior.
The as-built design — rendering pipeline (HDR/tonemap/SSAO/EDL), URDF/mesh
subsystem, `package://` asset resolution, scene-controls bindings, camera
system (four pluggable models, adaptive near/far, zoom-to-cursor, Position-only
frame-follow via the `ICamera::followShift` seam + `SceneViewWidget::applyFollow`,
XML persistence), and live-streaming data path — lives in
`pj_scene3D/docs/ARCHITECTURE.md`.

## Decoding boundary (important)

This module **never decodes wire formats**. DataSource / MessageParser plugins
(e.g. `parser_ros` in pj-official-plugins) decode ROS / CDR messages into
canonical `pj_base/builtin` objects — `PointCloud`, `FrameTransforms`,
`OccupancyGrid`, `OccupancyGridUpdate`, … — while the host registers their
transport bytes in the `ObjectStore`. `FrameTransforms` is consumed synchronously
through the application-activated ingest tap; other scene objects resolve their
durable entries on demand. The core
therefore stays "canonical-objects-in, render-structs-out", with **no `nanocdr` /
CDR dependency**.

A canonical object may reach the `ObjectStore` two ways, and `pj_scene3D` handles
both at the `widgets/` decode seam (`resolve_object.h`, see ARCHITECTURE.md →
"Object decode"): **(a)** parser-decoded (a `MessageParser` is bound to the topic;
decode via `parseLocked`), or **(b)** as an already *serialized canonical* blob
when the producer is a data-source / toolbox that pushed `serialize*` bytes with
no parser (e.g. the Mosaico cloud toolbox) — the host deserializes it with the
matching `pj_base` canonical codec. (b) is **not** a wire-format decode: it is the
canonical object's own serialization (exactly what pj_scene2D's image path does),
so the no-`nanocdr`/CDR rule still holds.

**One carve-out — compressed point clouds.** A `CompressedPointCloud` is *already* a
canonical object, but its payload is a self-describing codec blob (Draco / Cloudini).
Turning it into a canonical `PointCloud` is canonical→canonical *transcoding*, not
transport/CDR parsing, so it lives here: `core/pointcloud_codecs.{h,cpp}`
(`decodeCompressedPointCloud()`), with `draco` + `cloudini` linked **PRIVATE** into
`pj_scene3d_core` — the same shape as `pj_scene2d_core` decoding JPEG/PNG. The decode is
CPU-heavy, so `PointCloudLayer` runs it on the Qt thread pool (`QtConcurrent` +
`QFutureWatcher`, latest-wins coalescing) and never blocks the UI; the decoded
`PointCloud` then flows through the **same** `convertCanonical()` path as a raw cloud.
Notes: Cloudini `INT64`/`UINT64` fields have no PJ datatype and are dropped; Draco field
names are recovered from Draco attribute metadata when present (Foxglove /
`draco_point_cloud_transport` store the original name there), else inferred from the
attribute type; plain `zstd_point_cloud_transport` is out of scope (its blob isn't
self-describing). This is the *only* codec in the module — the no-`nanocdr`/CDR rule
still holds for everything else.

`PointCloudLayer` has a zero-copy fast path (verbatim wire upload for contiguous-float32
xyz, plus a packed-`rgba` RGB-direct variant) beside the `convertCanonical`/`CloudVertex`
fallback, still consuming `sdk::PointCloud` (canonical-in / render-structs-out boundary
intact — no wire decode added). Its `kCube` shape draws only the three camera-facing faces,
as an attributeless 7-vertex / 18-index hexagon fan around each cube's near corner
(`cube_mesh.h`) — pixel-equivalent to the full solid for opaque cubes at a third of the
vertex invocations and half the triangles — and degrades a cloud whose cubes are all
sub-pixel to area- and brightness-matched point sprites (a whole-cloud switch gated on the
AABB, because a *per-instance* split measures slower than no LOD at all). The face-choice,
face-normal, MSAA and LOD-matching pitfalls that shape has are spelled out in
`docs/ARCHITECTURE.md` → "Cube shape: the hexagon fan"; read it before touching that
shader. On the bounds-only paths the geometry AABB that fits the
camera is computed by an **async GPU compute reduction** (`PointcloudAabbReducer`: the CPU
seeds the first sample, then drops the scan once the pass reports `gpuAabbAvailable()`),
so the per-sample bounds cost leaves the GUI thread — see `docs/ARCHITECTURE.md` for the
full mechanism.

## Layout

- `core/` — pure geometry/scene logic, **no Qt or GL**. The TF buffer + frame
  hierarchy, the SE(3) `Transform`, the `OccupancyGridReconstructor` (stateful
  time-travel over an `OccupancyGrid` base plus incremental `OccupancyGridUpdate`
  patches), and the `DecodedPointCloud` render struct. Links only `glm`,
  `pj_base`, `nlohmann_json`. Key headers:
  `core/include/pj_scene3d_core/{tf/tf_buffer.h, tf/transform.h,
  tf/frame_picking.h, occupancy_grid_reconstructor.h, pointcloud.h, pointcloud_codecs.h,
  camera/camera.h, camera/camera_math.h, robot_model.h,
  scene_entities_decode.h, scene_entities_render.h}`. The codec
  decoders add a PRIVATE `draco` + `cloudini` link (compressed-cloud transcoding only —
  see "Decoding boundary"); the public API stays `glm` / `pj_base` / `nlohmann_json`.
- `widgets/` — Qt viewer (`SceneViewWidget`, a `QOpenGLWidget` embedded as a
  direct child of `Scene3DDockWidget`), render passes, layers, and
  `Scene3DDockWidget` (an `IDataWidget`). Key public headers:
  `widgets/include/pj_scene3d_widgets/{Scene3DDockWidget.h,
  transform_service.h, scene3d_layer.h, mesh_data.h, mesh_shading_params.h,
  passes/mesh_render_pass.h, layers/robot_model_layer.h,
  layers/scene_entities_layer.h}`. A right-click on the view delivers a
  native `QContextMenuEvent` that the host `DockWidget`'s event filter catches,
  so the 3D scene gets the same standard menu (Split Horizontally/Vertically,
  Clear) as other widgets with no view-side context-menu code. *Landing
  incrementally.*
  - **GL context lifecycle (don't regress this):** the app deliberately does NOT
    set `Qt::AA_ShareOpenGLContexts` (see `pj_app/src/main.cpp`) — a process-wide
    share group let one 3D view's teardown corrupt sibling views' VAO/FBO state
    (a `glBindVertexArray(non-gen name)` flood + the map texture vanishing). Each
    view's GL context is therefore independent, which means a `QOpenGLWidget`
    *recreates* its context when ADS reparents the dock (dock/float/split). VAOs
    and FBOs are per-context (never shared). Mesh textures are also
    per-context: `MeshRenderPass::releaseGL()` clears its `Texture2D` cache under
    the dying context and lazily reuploads path-keyed or embedded textures after
    recreation.
    Every `IRenderPass`/`Scene3DLayer` (and `ArrowGizmo`) implements
    `releaseGL()`; `SceneViewWidget` calls it from the dying context's
    `aboutToBeDestroyed` and rebuilds in `initializeGL`, so a recreated context
    self-heals instead of binding stale handles or going blank.
    **The one GL resource the widget cannot rebuild is Qt's own:** `QPainter`
    text painted directly on the view caches glyphs in a per-context atlas that
    does NOT survive this recreation (notably on layout restore), so the text
    renders doubled/garbled while vector fills stay correct. The 2D overlays
    (perf HUD, TF hover label) therefore CPU-rasterize their text into a `QImage`
    and `drawImage()` it (`hud_overlay.h::renderHudPanel`) — a glyph-atlas-free
    textured-quad blit. Never paint HUD/overlay text with `QPainter` glyphs on
    the GL view; rasterize-then-blit instead. Since Phase 0A
    the scene renders into `SceneHdrFbo` (a multisample RGBA16F+DEPTH32F chain
    at a fixed sample count — `kDefaultMsaaSamples`, independent of the context's
    negotiated samples, which are 0 once composited in an ADS dock — resolved to
    single-sample and presented via a fullscreen passthrough into
    `defaultFramebufferObject()`).
    The chain and the present program are per-context like everything else:
    released in `releaseGlResources()`, lazily rebuilt after recreation.
  - Scene-wide look controls live on `SceneViewWidget` setters +
    `CompositeParams` + `mesh_shading_params.h`. `pj_app`'s `Scene3DConfigPanel`
    drives grid style/size/divisions/visibility, gizmo size/opacity/visibility,
    mesh/collision opacity/visibility, and the Model/URDF selector (persisted in
    QSettings under `pj_scene3d/scene_controls/*`). The phases-0B/D/B knobs
    (tonemap/exposure/saturation/SSAO/EDL) are runtime APIs with baked defaults
    — no app UI; the mesh_viewer demo exposes them for look-dev.
  - The **last fixed frame the user picked by hand for a dataset** is remembered by
    `TransformService` (one `TransformBuffer` per dataset, so the natural home): a
    newly-created 3D dock defaults to it via `Scene3DDockWidget::resolveAutoFixedFrame`
    instead of the `map`/`world`/`odom` heuristic. In-session it is keyed by
    `DatasetId` (shared by sibling docks); across restarts it persists in QSettings
    under `pj_scene3d/fixed_frame_by_source`, keyed by the dataset's source path +
    `source_name` (name alone for a pathless source).
    See `docs/REQUIREMENTS.md` §5 / §10.
  - `MeshData` carries per-vertex UV0 + tangents plus a per-`SubMesh` `Material`
    (glTF 2.0 metallic-roughness, read via assimp's material abstraction so it
    also covers DAE/OBJ/FBX): base-color/metallic-roughness/normal/occlusion/
    emissive maps + factors + alpha mode. Each `TextureSource` is EITHER an
    external file path OR inline bytes (embedded glTF/GLB `*N` images, still
    PNG/JPEG-encoded), keyed by content hash. `MeshRenderPass` decodes both
    (`QImage::fromData` for embedded), uploads color/emissive sRGB and data maps
    (MR/normal/AO) linear, caches per-context by key plus texture color space,
    and does full metalness-workflow shading + normal mapping + emissive. Visual
    draws are split into opaque and best-effort translucent buckets (layer
    opacity, override alpha, or glTF `BLEND`; no depth sort). Sources
    without PBR (STL, procedural primitives) fall back to the scene-wide
    `MeshShadingParams`.
    Only compressed embedded images are supported (raw-RGBA `mHeight>0`, rare, is
    skipped). Mesh shading is lit by a fixed world key ("sun") light plus a
    camera-locked fill headlight, with **analytic image-based ambient**: diffuse
    irradiance plus a split-sum specular reflection of a procedural ground→sky
    environment (Karis `envBRDFApprox`, no HDRI cubemap), so metals reflect the
    sky/ground gradient instead of reading near-black. A true prefiltered-cube
    IBL from an HDRI environment is still future work.

# Validation

Before any commit, build and run the module's tests and check that they all pass.
The **authoritative set of test targets is the CMake registry, not this file**:
`pj_scene3D/core/CMakeLists.txt` and `pj_scene3D/widgets/CMakeLists.txt` each
register their `*_test` targets (~44 in total, GL-backed ones included). Run them
from the build directory with `ctest` (e.g. `ctest --test-dir build
--output-on-failure`) rather than from a hand-maintained list here — the previous
inline enumeration silently drifted as tests were added (it had fallen to 39 of the
~44, missing e.g. `mesh_render_pass_test`, `transform_service_test`,
`pointcloud_layer_rgb_test`, `scene3d_dock_streaming_test`/`_persistence_test`, and
the GL-context tests).

GL-backed tests (names ending `_gl_test`, plus the context-recreation tests)
require a real GL ≥ 4.5 context (llvmpipe under `xvfb` on CI) and **self-skip below
GL 4.5** — Windows software GL is only GL 3.0 / GLSL 1.30, so a `#version 450`
shader test cannot run there.

Make sure that all the markdown files in this folder are updated, if necessary.

Lessons learned should be saved too, in particular after long debugging
sections where we struggle to find the correct solution.

# Test-driven verification

- Always think first about how a certain piece of software can be tested
  automatically, instead of asking the user to run it and report the results.
- If the user reports an issue, think first about how to reproduce the issue
  in the tests. Do not attempt to fix the issue unless we were able to
  reproduce it.

# Collaboration model

Work is surfaced as diffs for user-approved commits (see the root CLAUDE.md
commit policy). There is no standing rule either mandating or forbidding
delegation of implementation to Codex for this module — decide per task.
