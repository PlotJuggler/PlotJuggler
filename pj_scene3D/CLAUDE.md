# pj_scene3D

3D visualization of robotics data, the sibling widget family of `pj_scene2D`. Layers: TF frames, point clouds (raw and
Draco/Cloudini-compressed), occupancy grids and costmaps, meshes and URDF robot models, scene entities (markers), pose
arrays (`PosesInFrame` triads), dense voxel grids, depth images back-projected into clouds, motion trails (a TF trail is
a standalone layer, a pose trail is owned by its pose layer), grid maps (a lit colormapped heightfield) and mesh maps
(a channel-coloured triangle mesh with its update patches). Paths and laser scans are not implemented.

One renderer on the desktop and in the browser: `SceneViewWidget`, a `QRhiWidget` (`widgets/rhi/`). It owns every GPU
resource; the dock (`Scene3DDockWidget`, an `IDataWidget` on `pj_scene_common`'s `SceneDockWidget`) and the layers
(`widgets/src/layers/`) feed it CPU data. How it works: [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- **No wire decoding here** (no `nanocdr`/CDR). Parser plugins decode messages into canonical `pj_base/builtin`
  objects. Every object consumer decodes a store entry through `resolve_object.h::resolveObject`: the topic's parser
  under `parseLocked`, resolved per use and never cached (a reload frees the parser), or the canonical codec when no
  parser is bound.
- **One codec carve-out: compressed point clouds.** Draco/Cloudini to canonical `PointCloud` lives in
  `core/pointcloud_codecs.{h,cpp}`, `draco` and `cloudini` linked PRIVATE into `pj_scene3d_core`, run on a
  `LatestWinsWorker`, never on the UI thread. Add no other codec.
- **Layers own no GPU resource.** A layer resolves, decodes (on a worker where heavy), admits against `kSceneLimits`
  and keeps CPU render data; it registers its families once through `Scene3DLayer::sceneContributions()`.
- **Every GPU resource is rebuilt from CPU data on a new QRhi.** Qt calls `releaseResources()` then `initialize()`
  whenever the view changes top-level (an ADS float, redock, move between areas). No draw may reference a resource
  whose `create()` failed, or the backend crashes or draws garbage (ARCHITECTURE "Failure state and fallbacks").
- **What the view draws changes only through `SceneViewWidget::invalidateContent()`.** A plain `update()` presents
  the retained frame; a new path that changes content without invalidating shows a stale frame after a hide/show.
- **Overlay text is CPU-rasterized** (`hud_overlay.h::renderHudPanel`) and drawn as a texture quad, never `QPainter`
  glyphs on the view: a glyph atlas would not survive a QRhi teardown.
- **Shader edits:** re-bake with the pinned `qsb` (`widgets/rhi/shaders/bake.sh`) and commit source and `.qsb`
  together, or the WASM configure's hash check fails. Every pack's varyings are contiguous from location 0, or D3D11
  reads them from the wrong registers (`scene3d_shader_constants_test` checks it).
- **One dataset per dock:** one TF tree and one display clock; a foreign dataset's topic is refused with a visible
  reason (contract in `pj_scene_common/docs/ARCHITECTURE.md` "One dataset per dock").
- **Depth images share `kImage` with colour images:** `DepthCloudLayer` is registered on `kImage` and the dock gates
  by the first sample's `encoding`. Drop hover uses `canAcceptObjectTopic` and never creates a layer; an advertised
  image (no payload yet) is provisionally accepted and validated on its first sample. Intrinsics join a `CameraInfo`
  by `frame_id`, never by topic name.
- **Saved state is self-describing:** every attribute an `xmlSaveState` in this family writes is read in
  `Scene3DDockWidget::xmlValidateState` through a reporting reader (the `PJ::readXml*` readers of
  `pj_runtime/XmlStateOptions.h`, `pj_scene_common`'s `parseSavedLayerHeader` for the `<layer>` / `<config_topic>`
  header, or the family's `layer_xml_validation.h` and `scene_state_xml.h` built on them), the same readers the
  loaders use, or the MCP `widget_state` tool lists stale or missing options. A layer type's payload check is the
  validator registered with its creator on `layerFactory()`; `scene3d_dock_xml_options_test` fails on an
  unreported attribute.
- **The fixed-frame memory is a view preference** (`FixedFrameMemory`, owned by `pj_app`), not TF data: keep it out
  of `pj_runtime`.
- **Context menu:** a right-click off a TF frame is left to the host `DockWidget`'s menu; on a frame the dock opens
  its frame menu with `popup()`, never `exec()` (the browser cannot nest an event loop).

## Layout and key headers

- `core/` (no Qt, no GPU; public links `glm`, `pj_base`, `nlohmann_json`, `pj_tf`): cameras, frame picking and
  connection lines over `pj_tf`, object placement (`tf/object_placement.h`), the occupancy and mesh-map
  reconstructors, point-cloud conversion and codecs, depth back-projection, `scene_limits.h`, trail sampling, the
  grid-map, mesh-map and voxel views, the shadow camera.
- `widgets/include/pj_scene3d_widgets/`: `Scene3DDockWidget.h`, `scene_view_widget.h`, `scene3d_layer.h` (the layer
  contract), `point_renderable.h`, `model_renderable.h`, `resolve_object.h`, `latest_wins_worker.h`,
  `FixedFrameMemory.h`, `mesh_shading_params.h`, `scene_look_defaults.h`, `channel_coloring.h` (colormap and range
  state shared by the channel-coloured layers), `hud_overlay.h`, `layout_resources.h` (relative robot resources in
  a layout), `scene_state_xml.h`, `layers/*.h`.
- `widgets/rhi/`: the view, `rhi_scene_pass.h` (the pass vocabulary and frame contract), one class per family in
  `passes/`, shaders and committed `.qsb` packs in `shaders/`; the shared cube GLSL in `widgets/shaders/cube/`.
- `widgets/testing/`: `SceneViewTestAccess`, the only way tests, benchmarks and browser probes read the view's
  internals.
- `demos/` (`PJ_BUILD_DEMOS`): point-cloud conversion and topology benchmarks. `wasm_probe/`: an opt-in browser
  capability probe, not product code.
- `THIRDPARTY.md`: licences of derived GLSL; a derived shader snippet names its source in a header comment.

Derived topics (script outputs) arrive as ordinary store topics; see `docs/derived_objects/ARCHITECTURE.md`
"The on-demand pipeline" and "Scene integration".

## Tests

Write the reproducing test first. Runners (registered through `pj_add_test_runner`, one process per case):
`pj_scene3d_core_tests`; `pj_scene3d_widget_tests` (built with `PJ_SCENE3D_TEST_HOOKS`);
`pj_scene3d_widget_plain_tests`; `pj_scene3d_widget_fixture_tests` (hooked, shares `PJ_SCENE3D_FIXTURES_DIR`).
Never mix hooked and plain sources in one runner: the hook macro changes public class definitions.
`scene3d_shader_constants_test` is its own executable. The QRhi display runners (`scene3d_rhi_*`,
`pointcloud_golden_test`, `scene3d_frame_benchmark`) register once per backend (`<runner>.<backend>.*`, label
`qrhi-<backend>`), skip without a display and fail instead under `PJ_REQUIRE_GPU_TESTS=1`.

```bash
./test.sh -R '^pj_scene3d_'          # headless runners
./test.sh --display -R \
  '^(pj_scene3d_|pointcloud_golden_test\.|scene3d_rhi_|scene3d_frame_benchmark\.|scene3d_shader_constants_test)'
```

`--display` runs a private Xvfb, serially (never `-j`: overlapping windows stall presentation).

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): behaviour contracts, the renderer (passes, frame contract,
  pipeline registry, quality stages, shadows, caches, failure state, lifecycle, WebAssembly), the layers, cameras,
  robot models and asset resolution, TF ingest, testing, known gaps. Read a layer's section before changing it, and
  "Cube shape: the hexagon fan" before touching the cube shader.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): backend conventions, Qt and ADS workarounds, measured design
  choices, golden-image rules, CI lane lessons, the performance baseline (Gate A).
