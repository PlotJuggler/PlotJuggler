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
colormapped heightfield from procedural vertex-index geometry — see
ARCHITECTURE.md → "Grid-map layer")); `PosesInFrame` array-as-path rendering and
laserscans remain future work. Sibling widget family to `pj_scene2D`.

`VoxelGridLayer` consumes `sdk::VoxelGrid` (SDK ≥ 0.10.0): a dense 3D lattice
whose per-voxel value is generic via `fields` (occupancy/cost/ESDF/semantic, or a
direct RGBA channel). The dense→cubes expansion happens entirely on the GPU —
`VoxelRhiPass` uploads the selected field as a 3D texture and issues ONE instanced
draw over a unit cube (`column*row*slice` instances); the vertex shader derives
each voxel from `gl_InstanceID`, `texelFetch`es its value, evaluates the
viewer-side draw predicate (which the schema does NOT encode), and
degenerate-clips culled voxels. So the CPU / draw-call cost is independent of voxel
count (the GPU vertex shader still runs once per voxel) and a re-scrub to a cached
grid re-uploads nothing. The Qt-free coordinate/value math
(`core/voxel_grid_view.{h,cpp}`, `core/voxel_grid_value.{h,cpp}`) is unit-tested.
A GPU compaction path (an indirect draw over only the accepted voxels) is a
documented follow-up for very large dense grids.

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
`depth_backproject.{h,cpp}`; a worker writes the points straight into the
vertex array the layer hands the view as a `PointRenderable`, drawn by the
same point pass as a raw cloud.

Drag-drop: because depth and color share `kImage`, an empty-placeholder drop of
*any* image opens the **2D** viewer (host policy in `pj_app`); a depth image
reaches a 3D dock by being dropped onto an existing one (`addTopic`'s encoding
gate absorbs depth, refuses color) or via the 3D family switch on an empty dock.
A one-click "Open in 3D view" curve-list action is a planned convenience.

## Documentation

- [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) — scope and goals; read it
  before changing this module's scope or behaviour.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — the as-built design: the
  QRhi renderer (pass structure, frame contract, pipeline registry, render targets
  and quality stages, shadows, per-family passes and caches, failure state,
  widget lifecycle, WebAssembly differences), the layers and their render
  contract, the camera system, the URDF/mesh subsystem, `package://` asset
  resolution, the live-streaming data path and the scene-controls panel.
- [`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md) — what the design works
  around: backend conventions, Qt 6.12.0 bugs and workarounds, ADS/Wayland quirks,
  limits and budgets, the measurements behind specific choices, golden-image rules
  and per-backend allowances, test registration and CI lanes, the performance
  baseline and the known gaps.
- [`docs/QRHI_NEXT.md`](./docs/QRHI_NEXT.md) — the work that remains: the golden
  writer transition, the P6 performance follow-ups, the Qt 6.12 upgrade, and the
  release notes of the release that made QRhi the default. The migration
  itself is archived in [`docs/archive/QRHI_MIGRATION.md`](../docs/archive/QRHI_MIGRATION.md).

The renderer is the QRhi view (`widgets/rhi/`: the view, its pass vocabulary and
frame contract in `rhi_scene_pass.h`, one class per family under `rhi/passes/`,
shaders and their committed `.qsb` packs under `rhi/shaders/`), on the desktop
and in the browser. The dock (`widgets/src/Scene3DDockWidget.cpp`) and every
layer (`widgets/src/layers/`) serve it; the layers own no GPU resource.

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
CPU-heavy, so `PointCloudLayer` runs it on the Qt thread pool (`LatestWinsWorker`:
one job in flight, the newest request waiting) and never blocks the UI; the decoded
`PointCloud` then flows through the **same** `packPointVertices()` path as a raw cloud.
Notes: Cloudini `INT64`/`UINT64` fields have no PJ datatype and are dropped; Draco field
names are recovered from Draco attribute metadata when present (Foxglove /
`draco_point_cloud_transport` store the original name there), else inferred from the
attribute type; plain `zstd_point_cloud_transport` is out of scope (its blob isn't
self-describing). This is the *only* codec in the module — the no-`nanocdr`/CDR rule
still holds for everything else.

`PointCloudLayer` plans a cloud once (`planPointVertices()`: validation and the
vertex format) and packs its finite points with `packPointVertices()` into an
implicitly shared array of 12- or 16-byte records (still consuming
`sdk::PointCloud`: canonical-in / render-structs-out, no wire decode added). Its
`kCube` shape draws only the three camera-facing faces, as an attributeless
7-vertex / 18-index hexagon fan around each cube's near corner (`cube_mesh.h`) —
pixel-equivalent to the full solid for opaque cubes at a third of the vertex
invocations and half the triangles — and degrades a cloud whose cubes are all
sub-pixel to area- and brightness-matched sprites (a whole-cloud switch gated on
the AABB, because a *per-instance* split measures slower than no LOD at all). The
face-choice, face-normal, MSAA and LOD-matching pitfalls are spelled out in
`docs/ARCHITECTURE.md` → "Cube shape: the hexagon fan"; read it before touching
that shader. `widgets/rhi/passes/point_rhi_pass.{h,cpp}` draws it all (plus flat
discs for sub-2-px spheres, a point-list path for small sprites and the faded
out-of-range two-partition draw); the geometry AABB behind the camera's scene bounds
comes from the conversion itself.

## Layout

- `core/` — pure geometry/scene logic, **no Qt or GL**. TF types (`pj::tf::Transform`,
  `pj::tf::TransformBuffer`, frame hierarchy) come from the leaf `pj_tf` module, not
  from this tree. `core/` itself owns the `OccupancyGridReconstructor` (stateful
  time-travel over an `OccupancyGrid` base plus incremental `OccupancyGridUpdate`
  patches) and the `PointVertex` / `DecodedPointCloud` render structs. Links `glm`,
  `pj_base`, `nlohmann_json`, `pj_tf`. Key headers:
  `core/include/pj_scene3d_core/{tf/frame_picking.h, tf/tf_connections.h,
  occupancy_grid_reconstructor.h, pointcloud.h, pointcloud_codecs.h,
  camera/camera.h, camera/camera_math.h, robot_model.h,
  scene_entities_decode.h, scene_entities_render.h}`; TF types are reached via
  `pj_tf/{tf_buffer.h, transform.h}`. The codec
  decoders add a PRIVATE `draco` + `cloudini` link (compressed-cloud transcoding only —
  see "Decoding boundary"); the public API stays `glm` / `pj_base` / `nlohmann_json` / `pj_tf`.
- `widgets/` — the Qt viewer (`SceneViewWidget`, a `QRhiWidget` in `rhi/`,
  embedded as a direct child of `Scene3DDockWidget`), the render passes, the
  layers, and `Scene3DDockWidget` (an `IDataWidget`). Key public headers:
  `widgets/include/pj_scene3d_widgets/{Scene3DDockWidget.h,
  scene_view_widget.h, scene3d_layer.h, point_renderable.h,
  model_renderable.h, FixedFrameMemory.h, mesh_data.h, mesh_shading_params.h,
  layers/robot_model_layer.h, layers/scene_entities_layer.h}`. Each layer
  registers the scene families it draws through once
  (`Scene3DLayer::sceneContributions()`, a `SceneContributions` of typed
  interfaces); the view never classifies layers per frame. What the view
  draws changes only through `SceneViewWidget::invalidateContent()`; a plain
  `update()` repaints the retained frame. Tests, benchmarks and the browser
  acceptance probes read the view's internals through `widgets/testing/`
  (`SceneViewTestAccess`, TECHNICAL_NOTES.md "View test access"). A right-click on
  the view off a TF frame delivers a native `QContextMenuEvent` that the host
  `DockWidget`'s event filter catches, so the 3D scene gets the same standard menu
  (Split Horizontally/Vertically, Clear) as other widgets; a right-click on a
  frame opens the dock's frame menu.
  - **QRhi lifecycle (don't regress this):** Qt calls `releaseResources()` and
    then `initialize()` on a new QRhi whenever the view changes top-level — an ADS
    float, redock or move between areas (a dock-area tab switch keeps the view
    parented, TECHNICAL_NOTES.md §5). Every GPU resource is rebuilt there from
    the CPU data the layers retain or re-derive (an uploaded static point cloud
    converts its sample again, `PointRenderable::ensureVertexBytes()`), and no
    draw may reference a resource whose
    `create()` failed (ARCHITECTURE.md → "Failure state and fallbacks", "Widget
    lifecycle and reparenting"). Overlay text (perf HUD, TF hover label) is
    CPU-rasterized into a `QImage` (`hud_overlay.h::renderHudPanel`) and drawn as a
    texture quad: never paint overlay text with `QPainter` glyphs on the view.
  - Scene-wide look controls live on `SceneViewWidget` setters +
    `CompositeParams` + `mesh_shading_params.h`. `pj_app`'s `Scene3DConfigPanel`
    drives grid style/size/divisions/visibility, gizmo size/opacity/visibility,
    mesh/collision opacity/visibility, and the Model/URDF selector (persisted in
    QSettings under `pj_scene3d/scene_controls/*`). The look knobs
    (tonemap/exposure/saturation/SSAO/EDL) are runtime APIs with baked defaults
    (`scene_look_defaults.h`) and no app UI.
  - The **last fixed frame the user picked by hand for a dataset** is remembered by
    `FixedFrameMemory` (a view preference, so it lives here and not in `pj_runtime`'s
    `TransformService`; `pj_app` owns one and hands it to every dock through
    `Scene3DDockWidget::setFixedFrameMemory`): a newly-created 3D dock defaults to it
    via `Scene3DDockWidget::resolveAutoFixedFrame` instead of the `map`/`world`/`odom`
    heuristic. In-session it is keyed by `DatasetId` and lives for the session
    (shared by sibling docks; dataset ids are never reused, and the remembered frame
    is re-checked against the live tree); across restarts it persists in
    QSettings under `pj_scene3d/fixed_frame_by_source`, keyed by the dataset's source
    path + `source_name` (name alone for a pathless source).
    See `docs/REQUIREMENTS.md` §5 / §10.
  - **One dataset per dock.** A 3D dock has one TF tree and one display clock, so it
    refuses a topic from a dataset other than the one it shows (`SceneDockWidget`
    returns `kRejected` and emits `topicRefused` with the reason; `pj_app` shows it as
    a toast). A saved layout that mixes datasets still loads: the foreign element is
    refused with the same reason and the restore is not marked failed. The dock can
    bind another dataset once its current dataset's topics are gone. TF itself is
    owned by `pj_runtime`'s `TransformService`, which the dock reaches through
    `setTransformService`.
  - `MeshData` carries per-vertex UV0 + tangents plus a per-`SubMesh` `Material`
    (glTF 2.0 metallic-roughness, read via assimp's material abstraction so it
    also covers DAE/OBJ/FBX): base-color/metallic-roughness/normal/occlusion/
    emissive maps + factors + alpha mode. Each `TextureSource` is EITHER an
    external file path OR inline bytes (embedded glTF/GLB `*N` images, still
    PNG/JPEG-encoded), keyed by content hash. `ModelRhiPass` decodes both
    (`QImage::fromData` for embedded), uploads color/emissive sRGB and data maps
    (MR/normal/AO) linear, shares each texture across the view's meshes by key
    plus texture color space, and does full metalness-workflow shading + normal
    mapping + emissive. Visual
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
`pj_scene3D/core/CMakeLists.txt` and `pj_scene3D/widgets/CMakeLists.txt` use
`pj_add_test_runner` from `cmake/PjTests.cmake`. GoogleTest discovery registers
each case with a `<runner>.<Suite>.<Case>` name and runs it in its own process.
The runners are:

- `pj_scene3d_core_tests`: geometry, transforms, and canonical-object decoding;
  uses the shared `QCoreApplication` main.
- `pj_scene3d_widget_tests`: tests with `PJ_SCENE3D_TEST_HOOKS`, using the shared
  `QApplication` main.
- `pj_scene3d_widget_plain_tests`: widget and logic tests without test hooks;
  links `pj_resources` for theme-dependent config-widget checks.
- `pj_scene3d_widget_fixture_tests`: tests sharing `PJ_SCENE3D_FIXTURES_DIR`, with
  test hooks (`robot_model_layer_test` among them). Links Qt Network for the
  local-server fixtures.

Hooked and plain sources stay in different runners because the test-hook macro
changes public library class definitions. File-local GoogleTest environments prepare the
Cloudini blob, preserve the fixed-frame settings application name, and redirect
the model caches to temporary directories. The shared mains keep the application
on the stack so it dies before Qt Network's exit handlers.

The QRhi display runners register one pass per backend of the platform
(`RHI_BACKENDS` in `cmake/PjTests.cmake`, `<runner>.<backend>.*`, label
`qrhi-<backend>`): `scene3d_rhi_render_test` (the render harness),
`scene3d_rhi_point_test`, `scene3d_rhi_failure_test`,
`scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test`,
`scene3d_rhi_platform_test`, and `pointcloud_golden_test` (the golden corpus,
`widgets/tests/fixtures/golden/pointcloud/README.md`, comparing against goldens
the QRhi OpenGL writer made on llvmpipe), and `scene3d_frame_benchmark` (frame budgets, a smoke at its default
sizes). They need a real display (Xvfb in Linux CI; they skip on `offscreen`,
and fail instead under `PJ_REQUIRE_GPU_TESTS=1`). TECHNICAL_NOTES.md §7-8 has
the golden rules, the test hooks and the CI lanes.

This test keeps its own executable and `add_test` registration:

- `scene3d_shader_constants_test`: uniquely defines `PJ_SCENE3D_SHADER_DIR`; it
  reads the shader sources and packs as data and checks the shared cube GLSL (and
  `cube_mesh.h`'s C++ mirrors of it), the colormap copy and every pack's varyings.

Run the module in the builder image with `ctest --test-dir build
--output-on-failure -R
'^(pj_scene3d_.*|pointcloud_golden_test\..*|scene3d_rhi_.*|scene3d_frame_benchmark\..*|scene3d_shader_constants_test)$'`
(under a display for the QRhi runners). Filter just the discovered runners with
`-R '^pj_scene3d_'`, or run the complete CMake registry with `ctest --test-dir
build --output-on-failure`. With `PJ_BUILD_DEMOS` the build compiles
`pointcloud_convert_benchmark` and `pointcloud_rhi_benchmark`.

Make sure that all the markdown files in this folder are updated, if necessary.

Save lessons learned to memory, not to these docs (they describe the current
state only), in particular after long debugging sessions where the correct
solution was hard to find.

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
