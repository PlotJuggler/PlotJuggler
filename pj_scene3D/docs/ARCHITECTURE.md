# pj_scene3D architecture

How the 3D scene behaves, how it is built and how its data gets on screen. The measurements and external facts
behind the design (backend conventions, Qt and ADS workarounds, golden-image rules, CI lessons, the performance
baseline) are in [TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md).

## Behaviour contracts

The user is a roboticist scrubbing recorded or live data, with 3D docks next to plots at the same tracker time.

- **Data types.** TF frames, raw and compressed point clouds, occupancy grids with incremental
  `OccupancyGridUpdate` patches (scrubbing reconstructs the grid at any time), SceneEntities markers (arrows, boxes,
  spheres, cylinders, line strips, text, model primitives), URDF robot models, pose arrays as triads, dense voxel
  grids, depth images back-projected into clouds, trails, grid maps and mesh maps with their updates. All of them
  render in the browser too. Paths, laser scans and a camera image/frustum view are not implemented.
- **Placement over time.** A layer that draws one stamped object (point cloud, depth cloud, pose array and its
  trail, voxel grid, grid map, a derived SceneEntities finding) is placed where its source frame was at the
  object's own stamp, in the TF header clock, composed through the root of the TF tree (`fixed <- root` at the
  cursor times `root <- source` at the stamp; `core/tf/object_placement.h::placeObject`). With a moving fixed frame
  a map-frame object stays put and a scan stays where it was captured. An unstamped object, one stamped before the
  first TF on its chain or more than `kMaxStampPastTf` past the newest falls back to the cursor; a placed object
  whose TF sample live retention has evicted keeps its last placement (`PlacementHold`). Every other layer
  (occupancy grid, mesh map, SceneEntities, robot model) and the TF overlay are placed at the tracker time in the
  entry clock. There is no per-layer toggle; Foxglove's `frame_locked` is not honoured. The per-consumer table is
  in `docs/derived_objects/ARCHITECTURE.md` "TF".
- **Marker lifetime.** Layers persist until the producer re-emits on the same key. SceneEntities additionally
  honour `lifetime_ns` (0 = never), anchored on the store entry timestamp the entity was folded from, not on its
  embedded sensor stamp (see "SceneEntities lifetime and the decoded-batch cache"). An expired entity disappears;
  there is no fade.
- **Frames.** Each dock has its own fixed frame, follow target and camera; docks never sync. The fixed-frame and
  follow pickers list the whole TF buffer (time-independent) and refresh as transforms arrive, so the tree fills in
  during a file load without playing. A new dock defaults to the fixed frame the user last picked by hand for the
  same dataset (`FixedFrameMemory`, consumed by `Scene3DDockWidget::resolveAutoFixedFrame`), else a
  `map`/`world`/`odom` heuristic; that default stays in auto mode, so it self-heals if the frame disappears.
- **Follow** is position-only: the camera anchor tracks the followed origin per tracker tick, orbit and zoom stay
  user-controlled, enabling never jumps, and a recenter button snaps onto the frame. Heading and pose follow are not
  implemented.
- **Camera gestures** (RViz orbit, ROS Z-up): left-drag rotates, middle-drag or shift+left-drag pans, the wheel
  zooms toward the cursor, right-drag zooms about the focal, Home resets the model's default view (it does not fit
  the scene). Models: Orbit, XYOrbit, Fly (WASD), top-down ortho.
- **Render loop.** On-demand only: a frame on a tracker change, a camera gesture or a content change, at most the
  display rate, none when settled (asserted by `scene3d_frame_benchmark`).
- **Viewer only.** No click-to-publish, measurement tool, map tiles, plugin-provided drawable types or octomap
  (variable-size leaves belong in SceneEntities cubes, not the uniform `VoxelGrid` lattice).

**Persistence.** The dock saves `<scene3d version="1">`: fixed frame and mode, `follow_frame` (empty = off), the
camera model as a stable string id plus `CameraState` JSON, `<scene_controls>` (grid, gizmo, TF lines, mesh and
collision visibility and opacity; ranges in `scene_state_xml.h`, shared by desktop and browser), and every layer's
own payload. Restore validates every control and camera field before mutating and rolls the whole dock back if a
layer payload or topic resolution fails. A restored follow target absent from the tree stays inert until it
appears. The last hand-picked fixed frame is also kept across restarts in `QSettings`
`pj_scene3d/fixed_frame_by_source`, keyed by the dataset's source path and name (not the per-session `DatasetId`);
a dataset without a source name is remembered for the session only, and a layout's saved fixed frame still wins
for its dock.

## TF buffer contract

`pj::tf::TransformBuffer` (`pj_tf/include/pj_tf/tf_buffer.h`) is what makes scrub replay work. One buffer per
dataset, owned by `pj_runtime`'s `TransformService` and shared (`shared_ptr`) by every dock on that dataset.

- `lookupTransform(target, source, t)` returns `T` with `p_target = T * p_source` (tf2's convention).
- A tree: a reparent conflict and a self-loop are rejected per edge (`SetTransformError`), and a bulk ingest
  continues past them.
- Two clocks: every sample keeps its header stamp and its entry time. `Clock::kEntry` returns per edge the
  newest-stamped sample that had arrived by `t` (the cursor's clock); `Clock::kHeader` the newest sample stamped at
  or before `t` (an object's own stamp).
- Zero-order hold, not interpolation: between two stamps the earlier sample, past the last the last, before the
  first `NoSampleAtTime`. Scrub replay asks what the system believed at `t`, and it never held an interpolated pose.
- An in-order sample equal to the edge's last is not stored, but its stamp still bounds `latestCommonTime` and
  eviction, so a fixed joint republished at 100 Hz resolves across the window as if every repeat were kept.
- Thread-safe for concurrent ingest and render readers without external locking; a non-throwing lookup for the
  render path.

## Overview

- **One dock.** `Scene3DDockWidget`: topics, layer rows, persistence, TF binding, the live-edge drive and the
  overlay controls.
- **One layer set** (`widgets/src/layers/`). A layer resolves its samples, decodes them (on a worker where heavy),
  admits them against `kSceneLimits`, persists its settings and keeps CPU render data. The view reads that data
  through `PointRenderable`, `ModelRenderable` and each family's accessors.
- **One renderer.** `SceneViewWidget` (`widgets/rhi/`) owns every GPU resource and draws every family on the
  platform's native API.
- **A Qt- and GPU-free core** (`core/`), unit-tested headless.

The desktop and the browser build the same view; browser policy (file picking, bounded loading, URDF from bytes)
is keyed on `PJ_TARGET_WASM`.

## Renderer

### Graphics API and the output target

The view requests `PJ::preferredGraphicsApi()` (`pj_widgets/GraphicsApi.h`): D3D11 on Windows, Metal on macOS
(OpenGL when Metal cannot start), OpenGL on Linux, WebGL2 in the browser; `PJ_GRAPHICS_API` overrides it. Qt
composites a top-level window with one QRhi, so every GPU widget of the app requests the same value. Each view logs
the backend and adapter it got once per QRhi (`pj.graphics`, `PJ::noteRhiAdoption()`).

The widget's sample count is chosen once per QRhi in `initialize()`: 1 when the GPU can run the HDR chain
(`hdrUnsupportedReason()` empty; the scene is multisampled inside the chain), `kDirectSampleCount` (4) when it
cannot and the direct pass draws into the output. A runtime HDR refusal on a capable GPU (size, byte budget, a
missing variant) draws its direct pass at sample count 1 rather than rebuild every resource for a new target.

### Pass structure

- `rhi/scene_view_widget_rhi.cpp`: scene preparation and submission; `rhi/scene_view_widget_rhi_quality.cpp`
  (+ `_quality_p.h`): shadow, HDR, SSAO and EDL resources, fallback and teardown.
- `rhi/rhi_scene_pass.h`: the shared vocabulary (`ScenePass`, `DrawClass`, `PipelineDesc`,
  `ScenePipelineRegistry`, `PipelineFailure`, `RhiPassSetup`, `RhiFrameContext`, `PlaceFn`) and the helpers every
  family builds with. `createPipeline()` is the one route every graphics pipeline of the view takes, so the test
  fault injection sees them all. `relativeModel()`/`transformedBounds()` subtract the render origin in double.
- `rhi/passes/<family>_rhi_pass.{h,cpp}`: one class per family; `rhi/shaders/`: GLSL, shared `.glslinc` and the
  committed packs.

| Family | Class | Draws |
|---|---|---|
| points | `PointRhiPass` | every `PointRenderable`: point and depth clouds |
| trails | `TrailRhiPass` | standalone and pose-owned trails |
| poses | `PoseRhiPass` | `PosesInFrame` triads and the view's TF triads |
| occupancy | `OccupancyRhiPass` | occupancy grids |
| voxels | `VoxelRhiPass` | dense voxel grids |
| markers | `MarkerRhiPass` | SceneEntities primitives |
| models | `ModelRhiPass` | every `ModelRenderable`: robot visuals and collisions, SceneEntities models |
| grid maps | `GridMapRhiPass` | grid-map heightfields |
| mesh maps | `MeshMapRhiPass` | channel-coloured meshes and their links |

`GridRhiPass` draws the prelude (ground grid, TF connection lines) before every layer, and `OverlayRhiPass` the
screen-space overlays after presentation.

Every family has one shape: `initialize(RhiPassSetup)`, `setLayers`, `admit`, `prepare(RhiFrameContext, ...)`,
`drawsFor(layer)`, `submit(cb, draw, ScenePass, registry, viewport)`, `release()`. A family owns its layout SRBs,
per-layer GPU state and counters, and holds handles to its pipelines, which the registry owns. Constant contents are
recorded in `initialize()` into one update batch that the view submits only when every part built, and discards
otherwise.

`setLayers()` receives the dock's order and reads each layer's `sceneContributions()` once: per family the typed
interface it draws through, null where it contributes nothing. A layer may register several families (SceneEntities
markers and models; a pose layer its triads and the trail it owns, which comes and goes without re-registration).
Each layer keeps one logical slot; no layer is classified or cast per frame. Within a slot families submit in
`forEachFamily` order, so a pose layer's trail draws under its triads and a SceneEntities layer's markers before
its models.

**Families are built on demand.** The base (uniforms, shared meshes, the grid and the pose pass, which the grid and
TF triads need with no layer) is built in `initialize()`; every other family once a layer of its kind is in the view,
in `initialize()` or at the start of the first `render()` that has one (`createSceneResources()`). Registering a
pipeline builds nothing, so a family costs its SRBs and uniforms, not programs (measurement: TECHNICAL_NOTES
"Measured design choices"). Building the model family drops the shadow resources, which the next shadowed frame
rebuilds with its caster pipeline; without it the EDL mask pass is skipped.

### Frame contract

`render()` runs once per frame, in order:

1. **Failure state.** Without the base resources, clear to the backdrop and stop.
2. **HDR decision** at the render size (output × `render_scale`). An HDR variant that failed last frame rejects HDR
   for that size first.
3. **Admit** (`admitFrame`): resolve the effective fixed frame once, then per visible layer `prepareForRender()`
   against the remaining per-view budget with its placement (`PlaceFn`). This is the only place a budget is
   consumed. Placement comes before budget, so a layer that cannot be placed reserves nothing ahead of a later one.
4. **Camera matrices** once, relative to the render origin, with the logical aspect; kept for picking.
5. **SSAO and EDL decisions:** each needs the HDR chain, its own resources and an invertible projection.
6. **Prepare:** every family's `prepare()`, then the grid prelude, the shadow decision (once casters are known), and
   the draw list in logical order with each draw's `DrawClass`.
7. **Overlays:** the corner gizmo and the HUD / hover-label images.
8. **Submit:** the gizmo pass when its rotation or size changed, the shadow pass when active; with HDR the colour
   pass, the coverage pass (depth replay, or a clear where depth is resolved), SSAO, the EDL mesh mask and the
   present with overlays; without HDR the direct pass with overlays at its end.

The shadow, colour, replay and mask passes all replay the one prepared frame: none re-admits a layer. A family whose
buffers cannot grow drops its own draws for the frame; the others still draw. `render()` decides HDR, SSAO and
EDL, `prepareFrame` decides shadows, and each stage's `last_active` is set once from them. A Show whose target holds
the last frame never reaches `render()` (see "Widget lifecycle and reparenting").

### Opaque and translucent submission

Each scene pass draws the prelude (grid cells, with the shadow-receiving pipeline when shadows are on; grid lines;
TF connection lines with the annotation blend), the view's TF triads, every opaque draw in logical order, then every
translucent draw in logical order. There is no depth sort and no order-independent transparency.

Point clouds are opaque except their faded out-of-range half; poses and trails opaque; occupancy, voxels, grid maps
and mesh maps translucent below full layer opacity (a mesh map also when a colour it draws has alpha below 255);
marker and model ranges translucent when they skip depth writes. A translucent layer above an opaque one in the
dock therefore still blends over it. `SceneViewTestAccess::lastSubmittedLayerIds()` reports logical order.

### Pipeline registry

`ScenePipelineRegistry` owns every scene pipeline (each family's, the grid's, the TF triads'). A family registers a
complete `PipelineDesc` with `add(desc, part)` and keeps the handle; the registry builds each variant from the one
description through `createPipeline()`: **direct** for the widget's target, **HDR colour** and **depth replay** for
the chain's targets. A state set in the description reaches every variant. `resolve(handle, pass)` returns the
variant a `ScenePass` binds.

**Nothing is built at registration.** An HDR variant is created the first time a frame binds it; direct variants
only for a frame without the HDR chain, all before its first pass (`createDirects()`). A part that draws nothing
resolves no variant.

**Failures keep their severity**, latched once as a `PipelineFailure`, never half way through a presented frame:

- **required** (from `createDirects()`): the frame is withheld before any pass and the next `render()` enters the
  failure state;
- **HDR variant** (from `resolve()`): the frame is withheld before the present and the next `render()` drops the
  chain for that size (`rejectHdr`); for the rest of that frame `resolve()` returns no HDR variant and compiles none;
- **optional** (`add(desc, part, true)`): no latch; it resolves to nullptr from then on and its family draws the
  same thing another way from that very bind (the point list falls back to quads; the grid's shadow receiver draws
  unshadowed cells and the view turns shadows off).

A withheld frame records its uploads and keeps the target's last full frame (`keepLastFrameOrClear()`). The depth
replay never draws a pipeline that writes no depth (translucent markers and models, occupancy, translucent grid and
mesh maps, faded points): `resolve()` returns nullptr and `submit` skips. Those pixels over the background stay
ungraded and SSAO/EDL never see them.

Pipelines outside the scene passes (present, SSAO, shadow caster, EDL mask, overlays) are owned by what draws them,
through the same `createPipeline()`. `initialize()` compares the render target's descriptor and sample count
(`PJ::RhiTargetKey`) with those it built for and rebuilds everything when they differ; when it changes the sample
count itself it returns at once, QRhiWidget rebuilds the target and calls it again, and the frame between is a
backdrop clear.

### Render targets and quality stages

| Target | Format | Written by | Read by |
|---|---|---|---|
| Output | the widget's colour buffer, 1 or 4 samples | present and overlays, or the direct pass | the compositor |
| Gizmo | 4x RGBA8 + depth resolved to RGBA8 (1x without multisample renderbuffers), 80 logical px | gizmo pass | overlays |
| HDR colour | 4x RGBA16F resolved to RGBA16F | HDR colour pass | present |
| HDR depth | resolve: 4x D32F (D32FS8 on OpenGL) resolved to single-sample; replay: 4x D32F renderbuffer plus the replay's D32F texture | HDR pass / replay | SSAO, EDL, present |
| Coverage | RGBA8: alpha replay coverage, green raw AO, red EDL mesh mask | replay, SSAO, EDL mask | present |
| Shadow map | `kShadowMapSize`² D32F | shadow pass | model and grid-cell shaders |

The chain is retained per size and charged against a per-view and a per-QRhi cap
(`kSceneLimits.max_hdr_bytes_per_view` / `_per_rhi`; the per-QRhi cap is shared by every view of a top-level).
`HdrResources` owns its charge and every release of its targets gives it back. HDR, SSAO and EDL each keep a
failure latch keyed on the render size, so a resize or a new QRhi probes the stage again; warning priority is HDR,
then SSAO, then EDL, and each drops to the next lower-quality path independently.

**Resource owners and release order** (`scene_view_widget_rhi_quality_p.h`): `ShadowResources`, `HdrResources`
(per QRhi: render-pass descriptors, samplers, present pipeline; per size: attachments, targets, present bindings),
`SsaoResources`, `EdlResources`. Each `release()` deletes what binds a resource before the resource (bindings,
targets, attachments; pipelines and targets before their render-pass descriptor), and members are declared in the
reverse order so plain destruction agrees. What another owner binds goes first: the grid's receiver and the model
caster before the shadow map; SSAO, EDL and the registry's HDR variants before the chain. `releaseResources()`
runs, while its QRhi lives: shadows, the HDR chain, the registry, the families, grid and overlays, shared meshes,
uniforms.

**Linear light and grading.** Every colour input is linearized with the exact sRGB decode (`srgb.glslinc`,
`look::srgbToLinear`) that inverts `present.frag`'s encode: colormaps and point colours in the vertex stage, theme
colours, and base-colour and emissive textures, which upload as plain RGBA8 and `model.frag` decodes (Qt's OpenGL
backend ignores `QRhiTexture::sRGB` for uncompressed formats). `present.frag` applies exposure, AO, EDL, the
tonemap, saturation and the sRGB encode; the knobs are `CompositeParams`, defaults in `scene_look_defaults.h`. The
direct fallback writes display-referred colour without a grade.

**HDR alpha is a tonemap-bypass marker, not coverage.** Data draws write it with the coverage-union blend
(`PJ::straightAlphaBlend`); annotations (TF triads and lines) add nothing (`annotationAlphaBlend`), so they present
flat and vivid, and the background keeps the exact theme colour.

**Depth resolve or replay.** Where QRhi reports `ResolveDepthStencil` and multisample textures (desktop OpenGL from
4.3, Vulkan, Metal) the HDR pass resolves its depth and the coverage pass is only a clear. Elsewhere (D3D11, WebGL2,
older GL) the prepared frame is replayed once into single-sample coverage and depth. The resolved depth is one
sample where the replay rasterizes the pixel centre, so the EDL mask pipelines carry a −1 constant and slope bias on
the resolve chain. `SceneViewTestAccess::setForceDepthReplay()` keeps the replay for comparison.

**SSAO** writes raw AO into coverage green from the depth (deterministic hemisphere kernel, `ssao_kernel.h`); the
present blurs it and applies it scene-wide before EDL and the tonemap.

**EDL** is restricted to meshes. The mask pass replays only the prepared model ranges with red-only writes and
`LessOrEqual` depth tests against the scene depth, so non-mesh depth still occludes hidden meshes. The present
darkens a mesh pixel whose neighbours are farther or not mesh, with the log-depth gap clamped (`look::kEdlMaxGap`)
and a hue-preserving floor (`CompositeParams::edl_floor`); the radius is in device pixels scaled by
`render_scale`.

### Presentation and overlays

The scene chain renders at `render_scale` × the output (clamped to [1, 4]): every chain target, the viewport, the
aspect, the point-size scale, the budgets and the latches use that size, and the present downsamples. The direct
fallback ignores the scale. `OverlayRhiPass` then draws at output resolution:

- the corner axis gizmo, always drawn into its own target and composited as premultiplied coverage, redrawn only
  when the camera rotation or its pixel size changes. The target is optional: without it there is no corner gizmo
  (logged, retried on the next `initialize()`);
- CPU-rasterized images as premultiplied texture quads: the perf HUD (key 'P'; GPU time from
  `lastCompletedGpuTime`, "--" where the backend reports none) and the TF hover label, both from `renderHudPanel`.
  An image is re-rasterized only when its text, DPR or theme changes, and its texture kept while its `cacheKey()`
  holds.

### Frame picking

`frameAt(QPointF logical)` is the one picking primitive (hover label, frame menu): it projects every resolvable TF
origin with the last painted frame's view-projection and render origin (`core/tf/frame_picking.h`) and returns the
nearest within `kFramePickRadiusPx`, nullopt when the triads are hidden. It ignores occlusion, DPR and
`render_scale`, and a camera moved since the last paint does not change the pick until painted. Hover runs while no
button is held and emits `hoveredFrameChanged`; a right-click on a frame emits `frameContextMenuRequested`, any other
right-click is ignored so the host dock's menu appears. Pose triads are not pickable.

### Shadows

The key light (`MeshShadingParams::key_light_dir`) casts directional shadows. The view owns the shadow target and
orchestration (`submitShadowPass`); the model family owns the depth-only caster pipeline.

- **Casters:** model draws only (visual robot links, hidden or not, and SceneEntities model primitives); never
  collisions, which would double-darken, and no other family.
- **Light frustum** (`core/shadow_camera.h`): the casters' bounds grown to where the shadow lands on the ground
  (`extendAabbToGroundShadow`), then bounding-sphere extents (rotation-invariant), texel-snapped along a light-only
  basis so edges do not crawl.
- **Receivers:** models and the filled-cell grid, through one lookup (`shadow_sampling.glslinc`); markers do not
  receive. Outside the light frustum is lit; the factor multiplies only the key-light term.
- **Gating:** `MeshShadingParams::shadows_enabled`, a bound `TransformBuffer` and a caster with a valid fit.
- **Reuse:** the map is rendered only when its key changes (resource generation, the model pass's shadow revision,
  the light view-projection, the render origin). An orbit keeps it; a pan or follow (a new render origin) renders it.

### Per-family passes and caches

**Shared geometry** (`RhiSharedGeometry`: unit cube, cube fan indices, unit pose arrow) is owned by the view,
uploaded in `initialize()` and released after every pipeline that draws it.

**Buffer policy.** Per-sample data (clouds, marker expansions, pose instances, trail ribbons, the grid) sits in
Static buffers uploaded on a revision change; `ensureStaticVertexBuffer` reallocates to grow or to give back a
buffer more than twice the need. Per-frame data (TF triads and lines, model instances, uniforms) is Dynamic. Mesh
buffers are Immutable. Arrays the uploader owns are padded with `PJ::padForUpload()` so they upload as one copy of
a size Mesa's anv records quickly (TECHNICAL_NOTES "Mesa anv buffer copies").

**Layer keys.** Per-layer GPU state is keyed by the layer's address and a revision. A layer at a freed layer's
address never meets a stale entry because the dock removes every layer from the view's list before destroying it,
and `setLayers()` drops its state in every family (`adoptLayerList()`).

**Points (`PointRhiPass`).**

- *Colour* (colormap, solid or per-point RGB and the linearization) runs once per point in the vertex stage; no
  fragment stage samples a LUT. A spatial-axis colour adds the render origin back, so colour stays in the fixed
  frame. `rhi/shaders/colormap.glslinc` is a copy of `pj_widgets`' `colormapGlsl()` that
  `scene3d_shader_constants_test` keeps identical.
- *Cubes* are the hexagon fan (see "Cube shape: the hexagon fan"), culled back faces: with the eye inside an axis
  slab a picked face is seen from behind, which the faded half would otherwise blend in.
- *Whole-cloud cube LOD* through `chooseCubeDrawMode()` (`core/cube_draw_policy.h`); every shape goes through the
  same frustum reject with the widest sprite it can draw, so a quad whose centre is just off screen is kept.
- *Spheres below `kSphereDiscMaxPx`* draw as flat discs at `kSphereDiscShading` (the disc mean of the Lambert,
  re-derived by `cube_mesh_test`): imposter shading at that size would depend on the driver's MSAA placement.
- *Quads or a point list.* Points, spheres and LOD sprites are camera-facing instanced quads on every backend; a
  cloud whose largest sprite is at most `kPointListMaxPx` draws as a point list where the backend has
  `VertexShaderPointSize` (the `-DPJ_POINT_TOPOLOGY` bake).
- *Faded out-of-range points* draw as two prepared draws: the in-range half opaque, the faded half translucent
  without depth writes.
- *Bounds* come from the conversion (`sourceBounds()`); without them, a CPU scan of the retained vertices once per
  revision.
- *Vertex formats* (`PointVertexFormat`): 12-byte xyz or 16-byte xyz plus one 32-bit attribute (scalar bits or packed
  colour). What a format lacks reads as scalar 0 and opaque white, values the style never reads.
- *Upload.* The layer hands over its retained cloud as one implicitly shared `QByteArray`; the update batch shares
  it, so only the backend's staging copy remains. A first upload goes to an `Immutable` buffer (Vulkan frees its
  staging), a re-upload reallocates it `Static` (a stream reuses the staging). The layer writes into the array in
  place only while nothing else holds it, or a write would detach it and copy.
- *Eviction.* Once an upload is enqueued the pass calls `onVerticesUploaded()` and a static cloud drops its array,
  keeping count, format, bounds, scalar range and revision. A new QRhi re-uploads through `ensureVertexBytes()`,
  which converts the same sample again (from the decode cache for a compressed sample; an async decode if the cache
  moved on). A sample within `kStreamingWindow` of the previous one marks a stream, which keeps its array and drops it
  once quiet that long. Depth clouds keep their array.

**Markers (`MarkerRhiPass`).** `expandLayer()` turns a layer's `MarkerGeometry` (built on a worker,
`marker_geometry.h`, line and triangle batches kept unexpanded with vertex index lists) into camera-relative
instances, stream vertices and draw ranges. It is a pure function of the layer's `LayerKey`: geometry revision,
render origin, style, and the resolved transform of every referenced frame, compared exactly (TF motion bumps no
revision). `prepare()` re-expands only layers whose key changed. A rebuild allocates all three buffers before it
queues any upload, so a failed allocation leaves nothing of the layer in the batch. Orbit, zoom and playback without
new data reuse every buffer; a pan or follow moves the render origin, and the vertices stay relative to it on the
CPU (TECHNICAL_NOTES "Render origin"), so the layer re-uploads. A layer re-uploaded again within
`kExpansionRetainFrames` prepares keeps its CPU expansion with world translations in double, so an origin-only change
rewrites only translations and stream positions, bit-identical to a full expansion; it drops it after that many
prepares without another or `kExpansionRetainIdleMs` without any prepare. `setMarkerCacheEnabledForTest(false)` is
the pixel reference of `scene3d_rhi_marker_cache_test`.

The cube fill is depth-biased away from the camera so its edge overlay, at exact depth, wins; edges with a
camera-facing adjacent face draw darker than self-occluded ones. Wireframe draws triangle edges as line lists on
every backend (WebGL2 has no `NonFillPolygonMode`); back-facing edges show.

**Models (`ModelRhiPass`).** Per-layer GPU meshes, up to five material textures per submesh (budgeted before decode;
a map longer than the backend limit or `max_model_texture_dimension` decodes scaled down), one material SRB per
submesh.

- GPU meshes are keyed by mesh key and `MeshData` pointer: a `modelRevision()` bump rebuilds only new or changed keys,
  so a URDF whose meshes arrive one by one uploads each once.
- Decoded textures live in one pass-level cache keyed by `TextureSource::key` and colour space, reference-counted
  across meshes and layers; the pixel budgets still charge every mesh that uses one.
- Wireframe edge lists are built on first wireframe draw; the CPU copy is dropped once queued.
- Colour and shadow instances are rebuilt only when the render origin, shading inputs or admitted draws (each
  layer's revision and look, every draw's resolved placement) change, so a link moved by TF alone still refreshes.
  Instance buffers stay Dynamic: animated models rewrite them every TF tick.

Every material takes `model.frag`'s metallic-roughness path (normal, occlusion, emissive maps; key and camera-locked
fill lights; an analytic ground-to-sky ambient, no HDRI). A URDF box, cylinder, sphere or placeholder draws with the
link's colour alone (`use_material` false): their unit meshes carry a material and vertex colour that would apply
it twice. `setShadowMap`/`clearShadowMap` rebuild every material binding atomically. The EDL mask pipelines are
derived from the colour pipelines' description (`ensureMeshMaskPipelines`).

**Grid and TF lines (`GridRhiPass`).** The grid is built once per settings or theme change at the fixed-frame origin;
the shaders add the origin offset, so pan, follow and playback upload nothing, and that add is exact (TECHNICAL_NOTES
"Render origin"). The filled style draws lines inside the cell shader from the distance to the nearest edge, so they
cannot z-fight the fill; an odd division count's half-cell phase travels in the offset's w. Every grid SRB declares
binding 3 for both stages because QRhi requires the connection lines' SRB layout to match. The TF connection lines
are arbitrary doubles, so they stay camera-relative in a Dynamic buffer keyed on the TF revision, fixed frame, time
and render origin.

**Poses and TF triads (`PoseRhiPass`).** One unit-arrow mesh, one instanced draw per layer. Pose instances are
frame-local in a Static buffer, re-uploaded on `geometryRevision()`; the fixed-from-source TF is a uniform, so camera
and TF motion rebuild nothing. The view's TF triads use the same pipeline with Dynamic camera-relative instances.

**Occupancy (`OccupancyRhiPass`).** One R8 texture per layer. A texture takes dirty rectangles only when it holds
exactly the previous staged revision (`dirtyRectsCatchUp()`), else the whole grid (after an undrawn frame, a new
epoch, a backward seek).

**Voxels (`VoxelRhiPass`).** One R32F or RGBA8 3D texture per layer and one instanced cube draw; the vertex shader
derives each voxel from `gl_InstanceID`, fetches its value and evaluates the draw predicate, so CPU and draw-call
cost are independent of voxel count. It uses the full 24-vertex cube with back faces culled, because a voxel is large
enough to swallow the viewpoint (where the fan fails). Placement composes in double before upload. A grid wholly
outside the frustum is not drawn but still uploads. Allocation checks `max3dTextureSize()` (TECHNICAL_NOTES "Backend
conventions").

**Grid maps (`GridMapRhiPass`).** Two R32F textures (elevation, colour channel) and procedural vertices from
`gl_VertexIndex`; two-sided lighting uses a geometric normal-faces-eye test, because a backend's clip-space
correction can flip `gl_FrontFacing`'s winding.

**Mesh maps (`MeshMapRhiPass`).** Per layer static buffers (positions, one 4-byte attribute, link positions and
colours) and opaque/translucent triangle and line pipelines. A frame allocates every buffer it writes before
recording any upload; the row warning clears only after an upload succeeded.

**Trails (`TrailRhiPass`).** One Static ribbon buffer per trail owner, re-uploaded on a rebuild or a render-origin
change, plus a past/future colour uniform. Colours are written unlinearized. Trails have no per-view budget and add
nothing to the scene bounds.

### Scene bounds

The dock owns them (`Scene3DDockWidget::updateSceneBounds`): the union of every layer's `worldBounds()` placed as the
view places the layer, hidden layers included, unresolvable ones skipped, recomputed on every layer-set change and
`refreshView()`. The view forwards them to the camera for near/far, zoom limits and Fly speed. Point clouds, depth
clouds, occupancy grids, voxel grids and grid maps report bounds; a robot does not, because its links move with TF
and would pull the camera as joints move. A hidden cloud or voxel grid releases its data but keeps its bounds. The
camera is never fitted to the bounds.

### Failure state and fallbacks

No draw may reference a resource whose `create()` failed. Every `create()` in `widgets/rhi/` is in one class:

| Class | Resources | On failure |
|---|---|---|
| Base | the view's uniform buffers, `RhiSharedGeometry`, each built family's `initialize()`, a required pipeline | failure state, the frame withheld |
| Optional stage | the HDR chain and the HDR/replay variants; SSAO; the EDL mask; shadows; overlays | the stage drops, one warning (logged when the reason changes, shown in the dock label; shadow errors on caster rows) |
| Silent optional | the point-list pipelines | the same pixels as quads |
| Per layer / frame | per-layer uniforms, SRBs, textures, instance and stream buffers; overlay images | that draw is skipped this frame; occupancy, voxel, grid-map, mesh-map and model rows warn |

**The failure state.** When `createSceneResources()` fails or a required `PipelineFailure` is latched, `failScene()`
releases everything and `render()` only clears to the backdrop. `setRenderFailure()` names the failing part first in
`renderingWarningChanged()` (the dock's `scene3dRenderingWarning` label) and emits `renderFailed()` once per
episode. A new QRhi or a new output size, format or sample count retries the build, and the next full frame clears
the warning. QRhiWidget's own `renderFailed()` (no QRhi for the window) sets a "no graphics device" failure the same
way.

Every layer reports its row warning through `Scene3DLayer::statusWarning()`: a data part (decode, admission,
budget), a renderer part (set by the view) and, for a derived topic, why it is empty (`PJ::SceneWarningParts`). The
dock combines it with the frame-orphan state per row (`updateLayerWarning`).

### Widget lifecycle and reparenting

- **A new QRhi rebuilds everything.** Qt calls `releaseResources()` whenever the top-level changes (an ADS float,
  redock or move between areas, which passes through a parentless state) and `initialize()` on the new QRhi. A
  dock-area tab switch does not, because our ADS fork keeps hidden tabs parented (TECHNICAL_NOTES "ADS and
  window-system quirks"). This is why clouds, marker expansions, meshes, volumes and grids stay CPU-resident or
  re-derivable.
- **Shader packs are read once per process** (`PJ::loadRhiShader()`, a process-wide cache shared with the plots and
  the media viewer); pipelines are still rebuilt per QRhi.
- **A paint with nothing new renders nothing.** Qt paints a render-to-texture widget on its own, inside every Show
  and in a parent's repaint while it holds it dirty (TECHNICAL_NOTES "Qt workarounds"). `paintEvent()` skips the
  paint while `retainsLastFrame()` (the target holds this view's last full frame on the live QRhi, at the size,
  format, logical size and DPR the paint would use) and either nothing changed or the paint is the Show's, which then
  queues a frame if content changed while hidden. Content changes are explicit: the view's setters, camera gestures,
  the dock's `refreshView()`, the config panel and a restored layout call `invalidateContent()`; `update()`, an
  expose or a parent repaint only present the retained frame. A skipped paint still presents and Qt still emits
  `frameSubmitted()`, so that signal is no render count: benchmarks count full renders through the test hooks.
- **Grabs.** `grabPresentedFrame()` (`pj_widgets`) invalidates, grabs after the next presented frame, never inside
  `render()`, and never blocks (the browser cannot nest an event loop). A never-shown view is grabbed once only.

### Shader packs and conventions

`rhi/shaders/bake.sh` bakes every pack for GLSL ES 300, GLSL 330 and 440, HLSL 5.0, MSL 1.2 and SPIR-V. Variants are
separate bakes (`-DPJ_FLIP_Y` for the fullscreen triangle on D3D/Metal, `-DPJ_GRID_SHADOW`, `-DPJ_POINT_TOPOLOGY`).
The WASM configure re-runs the script with the host `qsb` and fails on any byte difference or a pack without a
source. Shaders never remap clip conventions: the matrices carry them (`rhi_conventions.h`: `clipToTextureMatrix()`,
`textureVSign()`, `fullscreenNeedsYFlip()`). Every pack's varyings are contiguous from location 0 (TECHNICAL_NOTES
"Backend conventions").

### WebAssembly

The browser runs the same view, dock and layers. What differs:

- **Limits:** `kSceneLimits` is `kBrowserSceneLimits` under `PJ_TARGET_WASM` (`core/scene_limits.h`). A rejected
  sample or layer fails closed with a row warning; view budgets retry once they free up.
- **Depth replay always:** WebGL2 has no multisample textures, and Qt's GLES backend does not resolve a second
  multisample colour attachment reliably, which is why the EDL mask is its own pass.
- **Loading:** mesh fetches and imports run one at a time (`maxConcurrentMeshLoads()`). Compressed clouds are
  accepted only with `PJ_WASM_WITH_COMPRESSED_POINTCLOUDS`; classification, acceptance, factory registration,
  deferred restore and XML validation share one feature-gated predicate, because an unsupported topic would reach a
  modal warning that cannot `exec()` in the browser.
- **URDF from bytes:** a local URDF arrives as bytes (`addRobotModelLayerFromContent`), never a path, so a restored
  browser layout asks for the file again. Cross-origin URDFs and meshes need CORS.

## Layers and the render contract

Every layer is a `Scene3DLayer` (`scene3d_layer.h`), compiled in every build, and the view reads it through
`PointRenderable` (point and depth clouds), `ModelRenderable` (frame-local `ModelDrawCall`s from `RobotModelLayer` and
`SceneEntitiesLayer`) or its family's accessors.

**Coordinate contract.** A layer's CPU data is TF-free (source-frame points, frame-local instances, link-local model
draws with their frame id). The view applies TF and the render origin once, at placement.

**Admission.** Per-sample limits (payload size, dimensions, counts from `kSceneLimits`, the mesh importer and fetch
limits) are applied before any decode and fail closed with a row warning. The view applies per-view budgets: each
`prepareForRender(remaining)` drains its pending update, then checks what it would draw against the remaining budget
(`BudgetLatch`). A rejection is latched with its candidate sample, and the layer stays dropped without re-decoding
while the playhead asks for that candidate and it still does not fit.

**Workers.** `LatestWinsWorker` runs the heavy per-sample work (compressed decode, depth back-projection, SceneEntities
expansion, the TF trail rebuild): one job in flight, at most one waiting, scrubbing replaces the waiting request.
`invalidate()` (hide, detach, dataset replace) drops the waiting request and discards the running result. A worker
captures only owned bytes, so it may drain after its layer is gone.

**Repaint keys.** `renderKey(time)` is a decode-free fingerprint (sample identity folded with the placed
fixed←source transform, `foldFrameTransformIntoKey`), so a static sample in a moving frame still repaints. It must
derive from index and entry timestamps, never from a `latestAt` that would decompress cold chunks inside the gate.
The dock folds in its view key (TF triads and lines, follow target) and `SceneDockWidget::onTrackerTime` skips the
repaint when nothing changed.

## Camera system

The camera is an interchangeable controller over a serializable pose, in `pj_scene3d_core` so the math is unit-tested
headless.

- **`ICamera` + `CameraState`** (`core/.../camera/camera.h`): every model exports and adopts the same state, so
  switching models (`setCameraModel`: capture, construct, adopt, swap) keeps the view.
- **Models:** `OrbitCamera`, `XYOrbitCamera` (pivot locked to z=0), `TopDownOrthoCamera` (rotatable about +Z),
  `FlyCamera` (free eye, wheel dollies). The order of `SceneViewWidget::CameraModel` is load-bearing: the dock's
  combo index casts to it. Persistence uses string ids, independent of the order.
- **Adaptive near/far** (`adaptiveNearFar`): near from the working distance only, so close inspection never clips;
  far reaches the scene; a far/near ratio cap bounds depth precision. The top-down camera measures against the eye
  height plus the scene's vertical extent instead.
- **Zoom to cursor** is a homothety about the world point under the cursor. `OrbitCamera` keeps its angles and slides
  the pivot along the cursor lever by the post-clamp factor; it never re-derives angles from `eye − focal`, whose
  float32 cancellation at ~1e6 m coordinates spins the view on every tick. `XYOrbitCamera` re-pivots along the view
  ray the same way. A degenerate ray falls back to centre zoom; Fly dollies.
- **Follow:** `ICamera::followShift(world_delta)` moves the anchor without touching angles or zoom (Orbit and
  top-down the focal, Fly the eye, XYOrbit drops z). `SceneViewWidget::applyFollow` shifts by the followed origin's
  delta per tracker tick; the first tick after enabling, a fixed-frame change or a lookup gap only seeds the
  baseline. `followRenderKey(time)` feeds the dock's view key, so a moving target repaints with the triads hidden.
- **Camera-relative rendering.** At UTM scale float32 keeps decimetres, so each frame picks a render origin (the
  focal, in double), builds the view with `viewMatrixRelativeTo(origin)` (bit-exact `viewMatrix()` at origin 0) and
  subtracts the origin in double before every float cast. Camera state and scene bounds stay absolute.
- **Overlay controls:** the camera-model combo and Home button are children of the dock, raised above the view.

## Pose-array layer (`PosesInFrameLayer`)

Renders a `PosesInFrame` topic (one `frame_id`, a flat list of poses) as a triad per pose with arrow length,
opacity, an X-arrow-only mode and an independent colour override; defaults match the TF triads.
`buildPoseTriadInstances()` (`core/poses_in_frame_render.h`) expands frame-local instances. Decoding goes through a
per-use `resolveObject` binding; a `SequentialUID` guard skips redundant decodes and a style revision forces
re-expansion. An undecodable sample fails closed; non-finite instances are dropped before bounds. The params
round-trip through `<poses_in_frame>`, which also makes them copy/paste and apply-to-family-able. The layer owns an
optional trail (below).

## Trail layer (`TrailLayer`)

The trajectory of a TF frame's origin, or of a `PosesInFrame` topic's first pose, over the whole loaded range as a
screen-space ribbon in the fixed frame, split-coloured at the tracker time (past and future colours and visibility
per half, thickness 1-8 px, all persisted). A ribbon because line primitives are 1 px on most backends.

- **Sampling.** A TF trail samples at `TransformBuffer::chainSampleTimes` in the entry clock (the stored update
  times of every edge on the connecting path), so a statically mounted child trails with its moving ancestors. A
  pose trail places each message's first pose at its own stamp. `core/trail_sampling.h` decimates evenly to
  `kMaxTrailPoints`, endpoints kept; pose builds decimate before decoding. Unresolvable samples are skipped (the
  ribbon bridges the gap).
- **Scrub uploads nothing.** Two draws over one buffer split at an index found by binary search; the buffer
  re-uploads only on a rebuild or a render-origin change, and `renderKey` folds build revision, style revision and
  split index.
- **Threads.** TF rebuilds run on a `LatestWinsWorker` (the buffer is internally locked, the task holds the
  `shared_ptr`); pose rebuilds stay on the GUI thread (ObjectStore reads and `parseLocked` are GUI-thread contracts).
  Live appends are synchronous, sample only past the last point and ring-trim at the cap; a moved
  `TransformBuffer::epoch()` (reload commit, invalidation, merge rebuild) rebuilds instead.
- **Fixed frame and TF binding self-heal.** The layer adopts the view's fixed frame each frame and rebuilds on
  mismatch. The buffer arrives at attach and through `Scene3DDockWidget::pushTransformBufferToLayers` on every
  binding change: layout restore replays `<layer>` before the `<config_topic>` that binds TF, so a restored trail
  starts buffer-less and heals.
- **TF trails are standalone layers** under synthetic local ids (`allocateLocalLayerId`, no `LayerFactory`, object
  type `kNone`), created from the frame menu or the panel's Trail row. They persist as `role="trail"` with their
  identity in the layer's `<trail>` payload (`TrailLayer::xmlLoadStateResult`, restored before the object-type
  gate); `xmlLoadState` applies style only, so paste restyles and never retargets. A missing frame orphans one
  (revived if the frame returns); unloading the bound TF dataset removes it.
- **Pose trails are owned:** `PosesInFrameLayer` holds a `unique_ptr<TrailLayer>` never handed to the dock (no
  row, no id), forwards its lifecycle and re-emits its signals, since the dock only polls registered layers. It
  persists as a nested `<trail>` inside `<poses_in_frame>`, whose presence is the enabled flag. The trail is
  opt-in because building it samples the topic's whole history. The dock reaches both kinds through `trailOf()`.

## Pointcloud layer (`PointCloudLayer`)

Path: resolve (`latestEntryIdAt`, per-use binding) → admit (`cloudAdmissionError`; for compressed blobs before
decompression, with decode limits handed to the codec) → decode on a `LatestWinsWorker` when compressed → convert →
the retained array (`vertexBytes()`), bounds and scalar range from the same pass.

- **Formats.** `cloudini` and `draco` (`decodeCompressedPointCloud`). Wire sources: `foxglove_msgs/CompressedPointCloud`
  (its `pose` is dropped: clouds are placed by TF on `frame_id`) and `point_cloud_interfaces/CompressedPointCloud2`;
  the parser only repackages bytes. `zstd_point_cloud_transport` is out of scope: its blob is not self-describing.
  Cloudini `INT64`/`UINT64` fields are dropped (their bytes keep surviving offsets); Draco field names come from
  attribute metadata when present, else from the attribute type.
- **Conversion.** `packPointVertices()` (`core/pointcloud_convert.h`) reads xyz and the colour scalar of any datatype
  straight from the wire into the smallest format the style needs, keeping only finite points. It validates and
  produces values exactly as `convertCanonical()`, the reference decoder. It runs in chunks on the global pool with
  the GUI thread taking chunks too, in place while nothing shares the array. The canonical decode is a view of the
  ObjectStore payload (`deserializePointCloudView()`), so a stream allocates nothing per sample. QRhi keeps a batch
  of under 1 MiB for reuse, so a cloud that small gets a fresh array per sample. Every rejection clears the cloud with
  a row warning.
- **Caches.** One decoded compressed sample is cached per layer, identified by the entry's never-reused
  `SequentialUID`, so repaints and style changes do not re-decode. One failed sample is memoized with its warning.
  The point and depth layers track the wanted sample, cancel the waiting request when the tracker returns to the
  running or displayed sample, and drop any other completion. A hidden layer releases its cloud. The retained cloud
  keeps its own frame (`renderFrameId()`).

### Cube shape: the hexagon fan

A closed opaque cube never shows more than the three faces facing the camera, so `kCube` draws exactly those as a
6-triangle fan around the near corner (18 indices over 7 logical vertices, no cube vertex buffer:
`cubeFanCorner()` in the shared GLSL of `cube_mesh.h` and `shaders/cube/`). Before editing it:

- **Face choice is per instance**, against a homogeneous face-pick eye: the eye position under perspective, the
  view direction under ortho. Derive it from the projection matrix, not a folded view-projection, whose `[3][3]`
  holds the eye distance and silently breaks the `proj[3][3] == 0` perspective test. Getting it wrong gives every
  cube the same faces and drops one from half of them.
- **The face normal is recovered in the fragment shader** from the interpolated corner, which must be `centroid`:
  at pixel-centre sampling, partially covered MSAA pixels extrapolate and mis-pick the face.
- **No `discard`**, so early-Z holds on heavy overdraw; the fade's invisible case is culled in the vertex stage.
- **Colour is per point**, in a `flat` varying. The edge outline scales the colour (`mix(b, b*0.4, e)`), which
  commutes with the gamma, so the fragment stage keeps one `pow` and stays exact.
- **A camera inside a cube** is the one case the fan gets wrong, which is why `VoxelGridLayer` keeps the full solid.

`cube_mesh_test` pins the fan in all eight octants; `scene3d_rhi_point_test` renders mirrored cubes and a camera
inside one.

### Screen-space LOD: sub-pixel cubes become matched sprites

Below `kDefaultCubeLodThresholdPx` the whole cloud swaps to one sprite per cube (the point shader's `shape == 2`).
It is a whole-cloud switch decided on the CPU (measurement in TECHNICAL_NOTES "Measured design choices"):
`chooseCubeDrawMode()` asks whether the cloud's AABB proves no cube reaches the threshold, so a stale extent can only
draw near cubes as sprites, never make points vanish. The appearance match is exact per piece
(`shaders/cube/cube_sprite.glslinc`): the sprite has the area of the three visible faces (matching the side is ~37%
too bright), area-weighted Lambert, `kCubeEdgeMeanScale` for the dark edge band, and a one-pixel floor at full
colour (dimming by area overshoot would blacken a zoomed-out cloud on PJ's light backdrop). The golden
`shape_cubes_subpixel` holds the look.

## Depth-cloud layer (`DepthCloudLayer`)

Back-projects a depth image into a cloud, one point per valid pixel, coloured by depth.

- **Gating.** Registered on `kImage`; the dock accepts a topic whose first sample has a depth encoding
  (`isDepthEncoding`: `16UC1`, `32FC1`, `compressedDepth`). An empty-placeholder image drop opens the 2D viewer
  (`pj_app` policy); a depth image reaches a 3D dock by a drop onto one or the family switch. An advertised image
  creates a pending intent with topic demand, which accepts depth or discards colour on the first sample. Saved
  depth layers restore before their samples arrive.
- **Decode (`toDepthView`).** `16UC1`/`32FC1` alias the bytes; `compressedDepth` is PNG-decoded to millimetres,
  repairing the headerless PNG RealSense bags carry.
- **Intrinsics by `frame_id`**, never topic name: an exact match wins; otherwise a lone `CameraInfo` only when
  unambiguous, else refuse. Memoized by `(frame_id, CameraInfo SampleId)`; a new `CameraInfo` re-projects the
  current image.
- **Back-projection:** admitted on the UI thread (payload bytes, declared or PNG-header dimensions), then
  `depthToPoints` (`core/depth_backproject.h`) on a worker into 16-byte xyz + depth records; stale results dropped by
  generation. Colour through `PJ::Colormap`, shared with the 2D depth view.

## Voxel-grid layer (`VoxelGridLayer`)

A dense `sdk::VoxelGrid` whose per-voxel value is generic (`fields`: occupancy, cost, ESDF, semantic, or direct
RGBA) as GPU-instanced cubes (see "Per-family passes and caches"). The draw predicate is viewer-side: the schema does
not encode it. The value math is the Qt-free `core/voxel_grid_view.h` and `voxel_grid_value.h`. The layer retains the
packed volume (dropping the decoded-grid memo) and, over a limit or the backend's 3D texture limit, warns and
releases it.

## Grid-map layer (`GridMapLayer`)

An `sdk::GridMap` (canonical `grid_map_msgs/GridMap` and `foxglove.Grid`) as a lit, colormapped heightfield.

- **Geometry is procedural**: a new sample costs two texture uploads, de-interleaved by `core/grid_map_view.h`.
  Elevation "None" draws a flat plane.
- **Holes stay holes:** each vertex fetches all three corners and the fragment shader discards a triangle with a
  non-finite corner. Validity uses `abs(e) < 3e38`, not `isnan()`, so it does not depend on a driver keeping NaN
  through `texelFetch`. Normals fall back one-sided beside a hole, so a hole never tilts a valid triangle.
- **Shading** is Lambert key plus fill (`rhi/passes/lit_surface_uniforms.h`, shared with mesh maps), not the PBR
  path; no shadows. **Colour** through the shared colormap; a degenerate range paints the LUT midpoint.
- **Lifecycle** follows the point cloud. On the parser route the cached grid's anchor was allocated in the plugin
  DSO, so the layer keeps the binding keepalive declared after the cache: the grid dies first, or the catalog may
  unmap the DSO before the cached grid's destructor runs at shutdown.
- **Config** (`ChannelColoring`, shared with the mesh map): the persisted field names are requests; a missing field
  falls back with a warning and is honoured again when it reappears.

## Mesh-map layer (`MeshMapLayer`)

An `sdk::MeshMap` (navigation, cost and semantic meshes with named face and vertex channels), kept current by the
`sdk::MeshMapUpdate` patches of its update topics.

- **CPU state** (`core/mesh_map_view.h`, `MeshMapState`): positions, indices and every single-component channel as
  exact `double` (category ids above 2^24 stay distinct), the channel's no-data value folded into NaN before any
  narrowing. float64 positions narrow relative to the first finite vertex and the offset moves into the origin, so
  UTM-scale meshes keep centimetres. The state owns its arrays, so no plugin DSO keepalive is held.
  `applyMeshMapUpdate()` writes dense or sparse patches, refuses an update whose set `geometry_id` differs, and
  records each channel's dirty span.
- **Split.** `MeshMapReconstructor` (Qt-free, store-free) owns the mesh: state, applied updates, when to start over.
  `MeshMapLayer` owns the store side: which entries exist at the tracker time, fetch and decode, late arrivals, the
  gate, the warnings.
- **Update topics.** Every `MeshMapUpdate` topic of the dataset is a candidate. An update applies when its
  `geometry_id` equals the map's, both set; the `<topic>_updates` sibling also applies updates without one, and only
  its decode failures warn. A non-sibling entry naming another geometry is remembered by UID and skipped from then on
  while the map's geometry id holds; it is still decoded once, because nothing cheaper reads a `geometry_id` behind a
  parser.
- **Over time.** The reconstructor applies updates stamped in (map stamp − 1, time], merged across topics
  (`ObjectStore::rangeByTime`). Forward applies only the new window; a rewind, a new map, a changed topic set or a
  late update at or before the applied window (`ObjectStore::oldestArrivalAfter`) starts over from the map. The
  fingerprint folds, per topic, the highest arrival UID and the latest-by-stamp entry, because the maximum alone does
  not tell update sets apart when arrival and stamp order differ. `timeRange()` unions every paired topic, or the
  scrub clamp would pin the tracker at the last map.
- **Staging:** nothing (only uncoloured channels changed), a span of colour corners (`stagedColorDelta()`, from the
  lowest to the highest face written in the coloured face channel), or the whole colour array. A pass that missed a
  revision uploads whole.
- **Placement** at the tracker time in the entry clock: the mesh is a map plus later updates, with no single stamp.
- **Colour sources** are channel keys (`face:<name>`, `vertex:<name>`), the vertex colour (`uint8` `red`/`green`/
  `blue`[/`alpha`] vertex channels) and solid; the default is chosen by `ChannelKind`, not name. Continuous channels
  use the shared colormap; categorical ones their category colours or a palette. A face with no data at any corner
  draws whole as no data.
- **GPU:** de-indexed triangles from a position buffer (uploaded on `geometry_revision`) and a 4-byte attribute
  (channel value or RGBA8). No data travels as `kMeshMapNoDataValue` and values are clamped to `kMeshMapValueLimit`,
  so perspective-correct interpolation of the sentinel stays finite (TECHNICAL_NOTES "Vertex-attribute sentinels").
  Solid mode binds the position buffer in both slots, never a stale colour buffer. Flat shading from
  `cross(dFdx(p), dFdy(p))` (WebGL2 has no `gl_PrimitiveID`), with the view direction for a degenerate triangle.
  Links draw as lines between face centroids.

## Robot models (`RobotModelLayer`)

- **Parser** (`widgets/src/urdf_parser.{h,cpp}`, `QDomDocument`): link visuals and collisions with box, cylinder,
  sphere and mesh geometry. Joints are parsed, but TF owns kinematics: link poses come from the live tree. xacro is
  detected and rejected with an actionable error.
- **Fixed-joint bridges** (`core/robot_model_bridges.h`): each fixed joint becomes a static transform that
  `ensureStaticBridges()` re-asserts into the dataset buffer every tick, never overriding a frame the data
  publishes, idempotent, and self-healing after a reload clears the buffer. A dock with only local robots gets a
  private buffer (`ensureLocalTransformBuffer`) for them.
- **Sources:** a `kRobotDescription` topic (decoded through its parser: the payload is CDR-framed, never a string),
  a file or a URL; file and URL layers are dock-local with synthetic ids. A layout stores a file source relative to
  its folder when the URDF lies beneath it (`layout_resources.h`), so a URDF and its meshes move with the layout.
- **Mesh loading** is async and bounded: each layer's `MeshLoadQueue` runs up to `maxConcurrentMeshLoads()` fetches
  and assimp imports, each completion repainting; the GUI thread never blocks. The layer keeps the policy (what to
  load, the remote-fetch gate and size caps, the `kSceneLimits` retained-byte budgets checked on landing). A source
  switch, a replaced model or a dataset replacement cancels its jobs and a cancelled job never reaches the layer; a
  cancelled import keeps its slot until its worker finishes, so rapid replacement never exceeds the bound. Unwanted
  fetches are aborted by retiring the queue's `UrlFetcher`, which stays a child of the layer until deleted later, so
  completions never outlive the layer. assimp covers STL, DAE, OBJ and glTF; `MeshData` carries UV0, tangents and a
  glTF metallic-roughness `Material` per submesh; embedded glTF images are kept as encoded bytes keyed by content
  hash; a map that fails or is over budget leaves its slot empty, never the mesh.
- **Rendering:** unit primitives scaled by the model matrix; an unresolved mesh draws a magenta unit cube.
  Collision geometry without a material gets an orange tint, translucent below full opacity.
- **Time contract:** `timeRange()` returns the inverted sentinel `{Timepoint::max(), Timepoint::min()}`, so a static
  decoration neither widens the timeline nor clamps the playhead; the dock skips inverted ranges.

## Asset resolution (`package://` without ROS)

`UrdfPackageResolver` dispatches on the scheme: `file://` absolute; a bare path relative to the URDF (never enters
the package chain); `http(s)://` only for URL-source layers; `package://pkg/rel` to the model's explicit root for
`pkg` when it has one, else this chain, first hit wins:

0. Embedded assets from the source plugin (`setEmbeddedAssets`), exact name.
1. The remembered per-source mapping (`QSettings` `pj_scene3d/urdf_per_source_packages`).
2. Ancestors of the URDF's directory whose basename is `pkg` and that contain `rel` (URL segments for URL sources).
3. Search roots seeded from the URDF and source directories, `$ROS_PACKAGE_PATH`, `$AMENT_PREFIX_PATH` and
   `$COLCON_PREFIX_PATH`; identity is the directory basename, no `package.xml`.
4. Ask once on the layer's status; the chosen root is stored per source and globally, and pending meshes retry.

**Explicit package URL roots** (`setPackageUrlRoots`, persisted as `package_url_roots`) map a package to an http(s)
or file directory URL: how a topic URDF reaches web-hosted meshes. A mapped package is authoritative: `rel` must stay
inside its root once decoded and normalized (`..` escapes, encoded separators, backslashes, relative roots, other
schemes and, in the browser, `file://` roots leave the mesh unresolved), with no fallback. A root never permits the
model's direct `http(s)` references. On the desktop an http(s) root counts only for a URL source or a web-hosted
layout's robot, so a layout file cannot send a topic or file robot's mesh requests to a host it names; the root is
skipped but kept in the map, so a re-save keeps it.

**Layout ingress and egress.** `resolveLayoutResourceUrls` rewrites the application's copy of the layout once before
anything restores, so what layers apply and snapshot is absolute and undo needs no layout context;
`relocateLayoutResources` stores file sources and `file://` roots relative on save. `MainWindow` passes the layout
file's URL, a preset's http(s) URL, or no location for a browser upload. A relative root or source without a
location, or malformed roots, fails the whole load unchanged. A web-hosted layout never points at local files (a file
source, a local URL source or a `file://` root fails it), and every model gets `remote_resources_only="true"`, under
which the resolver refuses anything on this host even on the desktop; a layout loaded from anywhere else has the
attribute removed.

Failures are never silent: missing meshes show as magenta cubes with status counts, a wrong folder says which
subdirectory was expected, and a URDF latch not yet received re-checks (throttled) with a permanent "Waiting for …"
status.

## Streaming and TF ingest

The application activates `TransformService::activateIngestTap()` (`pj_runtime`) before any runtime host starts.
Each source push then resolves the topic's current parser binding, decodes under `parseLocked` on the pushing
worker and folds validated edges into the dataset's thread-safe buffer before the payload anchor can expire; this is
the file and streaming TF path. `TransformService` cannot use this module's `resolveObject`, so it carries its own
parser-or-canonical split. File buffers keep the full history; a live dataset gets a window tied to its ObjectStore
retention (`MainWindow` calls `setLiveCacheWindow`).

- Progressive file loads emit `datasetTransformsReady`; docks re-read frame hierarchies at the playhead.
- Streaming: `Scene3DDockWidget::setSessionManager` connects `SessionManager::samplesIngested` (UI thread, after
  each retention trim; `live` only while following a live stream). Each live tick refreshes orphan states from the
  buffer revision and `driveVisibleLayersToLiveEdge` drives every visible layer to the newest time of its own
  `timeRange()` (the layer's, so an occupancy grid with its `_updates` sibling does not freeze at its last
  keyframe).
- In-place reload: `beginReplacingLoad` routes tap writes to a staging buffer; `commitReplacingLoad` makes the
  active buffer adopt the staging state in O(1) (`adoptStateOf`), so every holder reads the reload without a
  rebind; `abortReplacingLoad` drops staging with no ObjectStore reads.
- `ingestFrameTransformsForDataset` is the explicit full-store rebuild, used for dataset merge (merged entries carry
  `payload_stamp_shift`, which only the durable entry records). Per-flush and streaming paths never call it while
  the tap is active.

### Object decode: parser or canonical blob

`resolveObject` picks the path by whether the topic has a bound `MessageParser`: with one, the parser decodes under
`parseLocked`; without one, the bytes are an already-serialized canonical object (e.g. a toolbox that pushed
`sdk::PointCloud`) and the canonical codec selected by `builtin_object_type` deserializes them. Either way the host
decodes, never the producing plugin. `attach()` accepts a topic with either (`hasCanonical3DCodec`).

### SceneEntities lifetime and the decoded-batch cache

`SceneEntitiesLayer` replays every batch up to the tracker time into an id-keyed entity map.

- **Lifetime anchor.** Under streaming the store entry is host-stamped while `entity.timestamp` is in the sensor
  epoch, so expiry compares `anchor + lifetime < tracker` against the ingest timestamp
  (`sceneEntityExpiredAt()`, overflow-safe); comparing the sensor stamp would expire every finite-lifetime entity
  the instant it is folded. `eraseEntity()` drops entity and anchor together. Deletions compare on the entity clock.
- **Snapshot cache.** Backward scrubs rebuild the map; decoded batches are cached by `SequentialUID` within
  `kSceneLimits.max_model_snapshot_cache_bytes` (oldest evicted) with their store time, so a re-fold restores the
  same anchors without re-parsing embedded glTF.
- **Out-of-order safe.** Batches come from `ObjectStore::rangeByTime` (ascending, decode-free), never an arrival-order
  walk. Forward playback folds only the delta; a push landing at or before the folded time is detected when
  `maxUidAtOrBefore()` grows past the folded high-water mark and forces a rebuild. Occupancy uses the same cursor
  discipline.

## Scene-controls panel (`pj_app`)

`Scene3DConfigPanel` drives scene-wide state, persisted in `QSettings` `pj_scene3d/scene_controls/*` and applied to
every dock it binds (each dock also saves it in its layout `<scene_controls>`): grid style, size, divisions and
visibility (colours from the theme), TF triad size, opacity and visibility, mesh and collision opacity and
visibility, and the Model/URDF row (File/Topic/URL source plus add). Robot layers are filtered out of the Topics list:
each has a panel row, and clicking it binds the Settings host to the layer's config widget. The look knobs
(tonemap, exposure, saturation, SSAO, EDL) have baked defaults and no app UI.

## Testing

- **Registration.** A display runner declares `RHI_BACKENDS` in `pj_add_test_runner` (`cmake/PjTests.cmake`): its
  cases register once per platform backend as `<runner>.<backend>.*` with `PJ_GRAPHICS_API` set and label
  `qrhi-<backend>` (opengl and vulkan on Linux, d3d11 on Windows, metal and opengl on macOS). `DPR2` adds
  `<runner>.opengl_dpr2.*` at `QT_SCALE_FACTOR=2`. `-L qrhi` selects every display pass, `-LE qrhi` none.
- **GPU gate.** A display pass skips when no window can be exposed (the `offscreen` QPA has no RHI support);
  `PJ_REQUIRE_GPU_TESTS=1` turns the skip into a failure. A case whose canvas initialized a backend and then failed
  fails instead of skipping.
- **Test hooks** (`PJ_RHI_TEST_HOOKS`, test-enabled builds; `pj_widgets/RhiTestHooks.h`):
  `PJ_RHI_TEST_FAIL_SHADER=<suffix>` fails every pipeline whose shader path ends with it; the `pjTestFailPipeline`
  property fails an allocation part way through a quality stage on this view; `pjTestRhiAdoptions` / `Releases` /
  `Stale` count QRhi adoptions. `QT_RHI_LEAK_CHECK` catches resources left on a dying QRhi except on OpenGL, whose
  uniform buffers are CPU-side, hence the counters.
- **View test access.** `SceneViewTestAccess` (`widgets/testing/`, a friend of the view; the app never links it)
  exposes the last-frame counters, the passes, each stage's state, the built families and pipeline counts, and the
  overrides (`forceStageFallback()`, the depth replay, `discardShadowMap()`). It does not need `PJ_RHI_TEST_HOOKS`
  (the WASM acceptance build has tests off); only its stage times do.
- **Dumps:** `PJ_RHI_TEST_DUMP_DIR` saves harness frames; `PJ_POINTCLOUD_GOLDEN_OUT` and
  `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1` the golden actuals and diffs. Golden rules: TECHNICAL_NOTES "Golden images".

## Known gaps

- Paths, laser scans and a camera image/frustum view are not implemented; nor are heading/pose follow, saved views,
  camera sync across docks, and Foxglove's per-entity `frame_locked`.
- Movable URDF joints are not articulated from `JointState`; `axis`, `limit` and `mimic` are not parsed.
- Embedded assets (resolver step 0) are inert in the app: no caller passes a source's asset map to
  `Scene3DDockWidget::setEmbeddedAssets`.
- Copy/paste and "Apply to family" of a pose layer also turn its trail on or off on the target (a synchronous
  whole-history rebuild per layer): layout save and paste share one `xmlSaveState`.
- A live trail append rebuilds and re-uploads the whole ribbon.
- `32FC1` inverse-quantized `compressedDepth` is not decoded.
- Voxel grids draw every voxel's instance; there is no compaction of accepted voxels for very large grids.
- Grid maps: no shadows, wireframe, cuboid mode or RGBA-from-fields colour. Mesh maps: no smooth shading from a
  `normal` channel, multi-component channels, shadows, face picking, wireframe or the `id` field.
- Mesh maps are capped per layer by `kMaxRenderableMeshMapFaces` but not counted in the per-view budgets; a rewind
  replays every update since the map (no checkpoints); a vertex-channel update re-colours the whole attribute; the
  memo of another mesh's entries grows by one UID per entry while the geometry id holds; no golden and no test of
  the pass's buffer-allocation failure.
- A move to another dock area rebuilds the view like a float (ADS unparents the dock on the way).
- Hidden tabs keep their GPU resources (a hidden Scene3D tab its whole scene); there is no release policy.
- Depth clouds, marker expansions and meshes keep their CPU arrays after upload; only static point clouds evict.
- Markers re-upload on pan and follow (the render origin is in their key), and a model scene's shadow map is
  re-rendered on every pan or follow frame.
- A runtime HDR refusal on a capable GPU draws its direct pass without MSAA.
- Point sprites above `kPointListMaxPx` are quads, slower than a point list for dense large sprites.
- Robot meshes are held twice (the `MeshLoader` path cache and the layer's admitted copy), and every admitted mesh
  rebuilds the layer's draw calls.
- A `UrlFetcher` request aborts only with its fetcher, so unwanted fetches retire the fetcher and its connections.
- `drawsFor` is a linear search per slot (O(L²) in the layer count).
- Pipeline-cache persistence is not reachable through `QRhiWidget`, and packs are not precompiled.
