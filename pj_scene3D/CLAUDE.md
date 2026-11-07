# pj_scene3D

## Purpose

3D visualization of robotics data, sibling widget family to `pj_scene2D`. Layers: TF frames, point clouds (raw and
Draco/Cloudini-compressed), occupancy grids / costmaps, meshes and URDF robot models, scene entities (markers), pose
arrays (`PosesInFrame`, per-pose triads), dense voxel grids (`VoxelGridLayer`, GPU-instanced cubes), depth images
back-projected into clouds (`DepthCloudLayer`), motion trails (`TrailLayer`; a TF trail is a standalone layer, a pose
trail is owned by its `PosesInFrameLayer`) and grid maps (`GridMapLayer`, a lit colormapped heightfield).
`PosesInFrame` as a path and laser scans are not implemented.

The renderer is one QRhi view on the desktop and in the browser: `SceneViewWidget` (`widgets/rhi/`: the view, its
pass vocabulary and frame contract in `rhi_scene_pass.h`, one class per family under `rhi/passes/`, shaders and their
committed `.qsb` packs under `rhi/shaders/`, the shared cube GLSL under `widgets/shaders/cube/`). The dock
(`widgets/src/Scene3DDockWidget.cpp`, an `IDataWidget` on `pj_scene_common`'s `SceneDockWidget`) and every layer
(`widgets/src/layers/`) serve it.

## Documentation

- [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md): scope and goals; read it before changing this module's scope or
  behaviour.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md): the as-built design: the QRhi renderer (passes, frame contract,
  pipeline registry, render targets, shadows, per-family caches, failure state, widget lifecycle, WebAssembly), the
  layers and their render contract, cameras, URDF/mesh, `package://` resolution, the live-streaming path, object
  decode and the scene-controls panel. Read the layer's section before changing a layer, and "Cube shape: the
  hexagon fan" before touching the cube shader.
- [`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md): what the design works around (backend conventions, Qt
  workarounds, ADS/Wayland quirks, limits and budgets, golden-image rules, test hooks and CI lanes, the performance
  baseline, known gaps). Hard-won lessons go here.
- [`docs/QRHI_NEXT.md`](./docs/QRHI_NEXT.md): renderer follow-ups not yet done.
- [`THIRDPARTY.md`](./THIRDPARTY.md): licences of derived GLSL; a shader snippet derived from an external source
  names it in a header comment and gets its licence here.

Derived topics (script outputs) reach this module as ordinary store topics, through `pj_scene_common` and the
catalog; see `docs/derived_objects/ARCHITECTURE.md` §8 (the on-demand pipeline) and §17 (scene integration).

## Decoding boundary

- **Never decode wire formats here** (no `nanocdr` / CDR dependency). DataSource / MessageParser plugins decode ROS /
  CDR messages into canonical `pj_base/builtin` objects; the core stays "canonical objects in, render structs out".
- Every object consumer decodes a store entry through one seam, `resolve_object.h::resolveObject`: through the
  topic's bound parser under `parseLocked` (per use, never cached), or, with no parser bound, with the matching
  `pj_base` canonical codec (a producer that pushed already-serialized canonical bytes). The second path is the
  canonical object's own serialization, not a wire decode.
- **The one codec carve-out: compressed point clouds.** Draco / Cloudini → canonical `PointCloud` is
  canonical→canonical transcoding, in `core/pointcloud_codecs.{h,cpp}` with `draco` + `cloudini` linked **PRIVATE**
  into `pj_scene3d_core` (as `pj_scene2d_core` decodes JPEG/PNG). It runs on a `LatestWinsWorker`, never on the UI
  thread, and feeds the same `packPointVertices()` path as a raw cloud. Add no other codec.
- `FrameTransforms` is consumed synchronously through the application-activated ingest tap; TF is owned by
  `pj_runtime`'s `TransformService` (the dock reaches it through `setTransformService`).

## Layout

- `core/`: pure geometry/scene logic, **no Qt or GPU**: cameras, frame picking and connection lines over the `pj_tf`
  buffer (TF types come from `pj_tf`, not this tree), `OccupancyGridReconstructor`, point-cloud conversion and codecs,
  depth back-projection, `SceneLimits` and the per-family budgets, trail sampling, the grid-map and voxel views.
  Public links stay `glm` / `pj_base` / `nlohmann_json` / `pj_tf`.
- `widgets/`: the view, passes, layers and dock. Key public headers in `widgets/include/pj_scene3d_widgets/`:
  `Scene3DDockWidget.h`, `scene_view_widget.h`, `scene3d_layer.h`, `point_renderable.h`, `model_renderable.h`,
  `resolve_object.h`, `latest_wins_worker.h`, `FixedFrameMemory.h`, `mesh_data.h`, `mesh_shading_params.h`,
  `scene_look_defaults.h`, `hud_overlay.h`, `layout_resources.h` (the host resolves a layout's relative robot
  resource URLs against the layout's own URL before restoring; a web-hosted layout never points at local files), `layers/*.h`.
- `widgets/testing/`: `SceneViewTestAccess`, the only way tests, benchmarks and browser probes read the view's
  internals (the view has no test API of its own).
- `demos/` (`PJ_BUILD_DEMOS`): `pointcloud_convert_benchmark`, `pointcloud_rhi_benchmark`.
- `wasm_probe/`: an opt-in browser capability probe (`PJ_WASM_SCENE3D_CAPABILITY_PROBE`), not product code.

## Rules and pitfalls

- **Layers own no GPU resource.** A layer resolves, decodes (on a worker where heavy), admits against `kSceneLimits`
  and keeps CPU render data; the view owns every GPU resource. Each layer registers its families once through
  `Scene3DLayer::sceneContributions()`; the view never classifies layers per frame.
- **What the view draws changes only through `SceneViewWidget::invalidateContent()`**; a plain `update()` repaints
  the retained frame.
- **QRhi lifecycle (don't regress this):** Qt calls `releaseResources()` and then `initialize()` on a new QRhi
  whenever the view changes top-level (an ADS float, redock or move between areas). Every GPU resource is rebuilt
  there from the CPU data the layers retain or re-derive, and no draw may reference a resource whose `create()`
  failed (ARCHITECTURE "Failure state and fallbacks", "Widget lifecycle and reparenting").
- **Overlay text** (perf HUD, TF hover label) is CPU-rasterized into a `QImage` (`hud_overlay.h::renderHudPanel`) and
  drawn as a texture quad: never paint overlay text with `QPainter` glyphs on the view.
- **Shader edits:** re-bake the `.qsb` with the pinned `qsb` (`widgets/rhi/shaders/bake.sh`) and commit both, or the
  WASM configure's hash check fails. `scene3d_shader_constants_test` checks the shared cube GLSL against
  `cube_mesh.h`'s C++ mirrors, the colormap copy and every pack's varyings.
- **One dataset per dock:** a 3D dock has one TF tree and one display clock and refuses a foreign dataset's topic
  with a visible reason (contract in `pj_scene_common/CLAUDE.md`).
- **Depth images share `kImage` with colour:** there is no `kDepthImage` producer, so `DepthCloudLayer` is registered
  on `kImage` and the dock gates topics by the first sample's `encoding` (`isDepthEncoding`). Intrinsics join a
  `CameraInfo` by **`frame_id`**, never by topic name; with no exact match, fall back only when unambiguous, else
  refuse.
- **Drop hover** uses `canAcceptObjectTopic`: colour images are rejected before
  release when their payload is known. Advertised images have no payload yet and
  remain provisionally eligible so the drop can request a subscription. A new
  deferred image intent waits for its first sample and validates the depth encoding
  before creating a layer. Hover checks never create layers.
- **The voxel draw predicate is viewer-side** (the `VoxelGrid` schema does not encode it); the dense→cubes
  expansion stays on the GPU (`VoxelRhiPass`).
- **The fixed-frame memory is a view preference** (`FixedFrameMemory`, owned by `pj_app`, handed to every dock), not
  TF data: keep it out of `pj_runtime`. Requirements: `docs/REQUIREMENTS.md` §5 / §10.
- **Scene-wide look** lives on `SceneViewWidget` setters, `CompositeParams` and `mesh_shading_params.h`; `pj_app`'s
  `Scene3DConfigPanel` drives it and persists it under `pj_scene3d/scene_controls/*`. The look knobs
  (tonemap/exposure/saturation/SSAO/EDL) have baked defaults (`scene_look_defaults.h`) and no app UI.
- **Context menu:** a right-click off a TF frame is left to the host `DockWidget`'s event filter (the standard
  Split/Clear menu); a right-click on a frame opens the dock's frame menu.

## Validation

Before any commit, build and run the module's tests. Write the reproducing test first: when the user reports an
issue, reproduce it in a test before fixing it, and prefer an automatic test over asking the user to check by hand.

The authoritative test list is the CMake registry (`core/CMakeLists.txt`, `widgets/CMakeLists.txt`, through
`pj_add_test_runner`; each case is `<runner>.<Suite>.<Case>` in its own process):

- `pj_scene3d_core_tests`: geometry, transforms and canonical-object decoding (shared `QCoreApplication` main).
- `pj_scene3d_widget_tests`: tests built with `PJ_SCENE3D_TEST_HOOKS` (shared `QApplication` main).
- `pj_scene3d_widget_plain_tests`: widget and logic tests without hooks; links `pj_resources`.
- `pj_scene3d_widget_fixture_tests`: hooked tests sharing `PJ_SCENE3D_FIXTURES_DIR` (e.g. `robot_model_layer_test`);
  links Qt Network for local-server fixtures.
- **Never mix hooked and plain sources in one runner**: the hook macro changes public class definitions.
- QRhi display runners, one pass per platform backend (`RHI_BACKENDS`, `<runner>.<backend>.*`, label
  `qrhi-<backend>`): `scene3d_rhi_render_test`, `scene3d_rhi_point_test`, `scene3d_rhi_failure_test`,
  `scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test`, `scene3d_rhi_platform_test`,
  `pointcloud_golden_test` (corpus and rules in `widgets/tests/fixtures/golden/pointcloud/README.md`) and
  `scene3d_frame_benchmark` (a smoke at default sizes). They need a real display (Xvfb on Linux CI), skip on
  `offscreen`, and fail instead under `PJ_REQUIRE_GPU_TESTS=1`. Golden rules, hooks and CI lanes:
  TECHNICAL_NOTES §7-8.
- `scene3d_shader_constants_test`: its own executable (it uniquely defines `PJ_SCENE3D_SHADER_DIR`).

```bash
ctest --test-dir build --output-on-failure -R \
  '^(pj_scene3d_.*|pointcloud_golden_test\..*|scene3d_rhi_.*|scene3d_frame_benchmark\..*|scene3d_shader_constants_test)$'
```

Run the QRhi runners under a display; `-R '^pj_scene3d_'` runs only the headless runners. Keep this module's docs in
step with any behaviour change.
