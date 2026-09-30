# pj_scene3D — Architecture (as built)

How the 3D scene is built and how its data gets on screen. The WHAT lives in
[REQUIREMENTS.md](./REQUIREMENTS.md). The facts this design works around
(backend conventions, Qt 6.11.1 bugs, limits and budgets, the rationale behind
specific choices, golden-image rules, test registration and CI lanes, known gaps)
are in [TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md). Planned work (deleting the
legacy renderer, performance follow-ups, Qt 6.12) is in [QRHI_NEXT.md](./QRHI_NEXT.md).

## Overview

- **One dock.** `Scene3DDockWidget` (`widgets/src/Scene3DDockWidget.cpp`), an
  `IDataWidget` built on `pj_scene_common`'s `SceneDockWidget`: topics, layer rows,
  persistence, TF binding, the live-edge drive and the overlay controls.
- **One layer set** (`widgets/src/layers/`). A layer resolves its samples,
  decodes them (on a worker where decoding is heavy), admits them against
  `kSceneLimits`, persists its settings and keeps CPU render data. It owns no GPU
  resource: the view reads that data through renderer interfaces
  (`PointRenderable`, `ModelRenderable`, and each family's accessors).
- **One renderer.** `SceneViewWidget` is a `QRhiWidget` (`widgets/rhi/`). It owns
  every GPU resource and draws every layer family, on the platform's native
  graphics API.
- **A Qt- and GPU-free core** (`core/`): camera models and the camera-fit policy,
  the TF buffer, frame picking, the shadow camera, point-cloud conversion and
  codecs, `SceneLimits` and the per-family budgets, trail sampling, and the
  grid-map and voxel views. All of it is unit-tested headless.

`PJ_SCENE3D_RHI` (ON by default, forced ON for WebAssembly) selects the renderer.
The two renderers define the same class names, so a build has exactly one; the
dock and the layers are the same in both. Browser policy (file picking, bounded
loading, URDF from bytes) is keyed on `PJ_TARGET_WASM`, not on the renderer. The
legacy OpenGL renderer that `-DPJ_SCENE3D_RHI=OFF` still builds is described
only in the appendix.

## Renderer

### Graphics API and the output target

The view requests `PJ::preferredGraphicsApi()` (`pj_widgets/GraphicsApi.h`):
D3D11 on Windows, Metal on macOS (OpenGL when Metal cannot start), OpenGL on
Linux, WebGL2 in the browser. `PJ_GRAPHICS_API` overrides it. Qt composites a
top-level window with one QRhi, so every GPU widget of the app (the plot canvas,
the Scene2D media viewer, this view) requests the same value. Each view logs the
backend and adapter it actually got, once per QRhi (`pj.graphics`,
`PJ::noteRhiAdoption()`).

The widget's own sample count is chosen once per QRhi in `initialize()`:

- **1** when the GPU can run the HDR chain (`hdrUnsupportedReason()` is empty: 4x
  multisample renderbuffers and RGBA16F / D32F / RGBA8 render targets). The scene
  is multisampled inside the chain; the output only carries the present, the
  corner gizmo and the HUD.
- **4** (`kDirectSampleCount`) when it cannot: the direct pass then draws the
  scene into the output.

A runtime HDR refusal on a capable GPU (size, byte budget, a missing pipeline
variant) draws its direct pass at sample count 1 instead of rebuilding every
resource for a new target.

### Pass structure

The view is split into:

- `rhi/scene_view_widget_rhi.cpp`: scene preparation and command submission;
- `rhi/scene_view_widget_rhi_quality.cpp`: shadow, HDR, SSAO and EDL resource
  creation, fallback and teardown; `scene_view_widget_rhi_quality_p.h` holds the
  shared uniform layouts;
- `rhi/rhi_scene_pass.h`: the vocabulary every pass shares (below);
- `rhi/passes/<family>_rhi_pass.{h,cpp}`: one class per family;
- `rhi/shaders/`: the GLSL sources and their committed `.qsb` packs.

`rhi_scene_pass.h` defines `ScenePass` (direct, HDR colour, depth replay),
`DrawClass` (opaque, translucent), `ScenePipelineRegistry`, `RhiSharedGeometry`,
`SceneBoundsCollector`, `RhiPassSetup`, `RhiFrameContext`,
`PlaceFn`/`PlacedLayer`, and the helpers every family builds with:
`createScenePipeline(setup, PipelineDesc)`, `createGraphicsPipeline()` (every
pipeline's `create()`, with the test fault injection), `ensureUniformBuffer`,
`ensureVertexBuffer` (Dynamic, per-frame data), `ensureStaticVertexBuffer`
(Static, per-sample data, with a grow/shrink rule), `createBindings`,
`adoptLayerList`/`pruneLayerGpu` (per-layer GPU state), `findDraws`,
`relativeModel`/`transformedBounds` (camera-relative placement, the origin
subtracted in double) and `rhiConventions`.

| Scene family | Class | Draws |
|---|---|---|
| points | `PointRhiPass` | every `PointRenderable`: point clouds and depth clouds |
| trails | `TrailRhiPass` | standalone `TrailLayer`s and pose-embedded trails |
| poses | `PoseRhiPass` | `PosesInFrame` triads, and the view's TF frame triads |
| occupancy | `OccupancyRhiPass` | occupancy grids |
| voxels | `VoxelRhiPass` | dense voxel grids |
| markers | `MarkerRhiPass` | SceneEntities primitives |
| models | `ModelRhiPass` | every `ModelRenderable`: robot visuals and collisions, SceneEntities models |
| grid maps | `GridMapRhiPass` | grid-map heightfields |

Two passes are not layer families: `GridRhiPass` draws the prelude (ground grid,
TF parent-connection lines) before every layer, and `OverlayRhiPass` draws the
screen-space overlays after presentation.

Every scene family has one shape: `initialize(RhiPassSetup)` (the model pass also
takes the view's lighting buffer), `registerPipelines(registry)`, `setLayers`,
`admit(place, bounds)`, `prepare(RhiFrameContext, ...)`, `drawsFor(layer)` (a
span of `Draw`s, each carrying its `draw_class`), `submit(cb, draw, ScenePass,
registry, viewport)` and `release()`. A family owns its pipelines, layout SRB,
per-layer GPU state and counters. The view owns the command buffer and the
logical layer order: `setLayers(std::vector<Scene3DLayer*>)` receives the dock's
order and dispatches each layer to its family. It loops over the families
generically (`forEachFamily`) for registration, release and collecting a slot's
draws, and submits any family's draw through one `std::visit`. Within one layer
the families submit in `forEachFamily` order, so a pose layer's trail draws under
its triads and a SceneEntities layer's markers before its models.

### Frame contract

`render()` runs these steps once per frame, in order:

1. **Show skip.** A paint caused by a Show on the same QRhi at the same output
   size records nothing (see "Widget lifecycle and reparenting").
2. **Failure state.** Without the base resources, clear to the backdrop and stop.
3. **HDR decision.** `ensureHdrResources()` at the render size (output ×
   `render_scale`). A pipeline clone that failed last frame rejects HDR for that
   size first.
4. **Admit** (`admitFrame`). Resolve the effective fixed frame once, then per
   visible layer: `prepareForRender()` against the remaining per-view budget,
   fixed-frame placement, and the layer's placed bounds into the frame's
   `SceneBoundsCollector`. This is the only place a budget is consumed.
5. **Bounds and camera fit** (`applySceneBounds`), once.
6. **Camera matrices** (`frameView`), once, relative to the render origin.
7. **SSAO and EDL decisions:** each needs the HDR chain, its own resources and an
   invertible projection.
8. **Prepare** (`prepareFrame`). Every family's `prepare()` with one
   `RhiFrameContext` (updates, render origin, corrected view-projection, view
   matrix): uploads, instances, uniforms. Then the grid prelude, the shadow
   decision (once the casters are known), and `scene_draws`: every draw in
   logical (dock) order with its `DrawClass`.
9. **Overlays** (`prepareOverlays`): the corner gizmo and the HUD / hover-label
   images.
10. **Submit.** The shadow pass when active. With HDR: the HDR colour pass, then
    the coverage pass (the depth replay, or a clear where the chain resolves its
    depth), SSAO, the EDL mesh mask, and the present pass with the overlays on the
    output. Without HDR: the direct pass on the output, overlays at its end.

The shadow pass, the HDR colour pass, the depth replay or the direct pass, and
the EDL mesh mask all replay the one `PreparedFrame`. None re-admits a layer or
consumes a budget. A family whose buffers cannot grow drops its own draws for the
frame; the other families still draw. The prepared frame, the submission marks
and the per-family draw lists are reused across frames. The quality-stage flags
have one owner: `render()` decides HDR, SSAO and EDL, `prepareFrame` decides
shadows, and each stage's `last_active` is set once from them.

### Opaque/translucent submission

Each scene pass draws the prelude (`GridRhiPass`: grid cells as data, with the
shadow-receiving pipeline when shadows are active; grid lines; then the TF
connection lines with the annotation blend), then the view's TF triads, then
every opaque draw in logical order, then every translucent draw in logical
order. There is no depth sort: this is not order-independent transparency.

Classification: point clouds are opaque except their faded out-of-range half;
poses and trails are opaque; occupancy, voxels and grid maps are translucent
below full layer opacity; marker and model ranges are translucent when they skip
depth writes (a translucent cube's edge overlay stays with its fill). A
translucent layer placed before an opaque one in the dock therefore still blends
over it. `lastSubmittedLayerIdsForTest()` reports logical order.

### Pipeline registry

Each family registers the direct pipelines it creates (the view in
`initialize()`, the grid's shadow receiver through
`GridRhiPass::createShadowReceiver` from `ensureShadowResources`, the families
through `registerPipelines`). Once the HDR chain exists, the view names the clone
targets (`setCloneTargets`), and `resolve(direct, pass)` returns the direct
pipeline, or creates its HDR-colour or depth-replay clone on first use. A clone
copies every state of its source, so HDR and the depth replay always match the
direct pipeline. Where the chain resolves its depth, no depth-replay clone is
ever requested.

`resolve()` decides one exception centrally: the depth replay never draws with a
pipeline that writes no depth (`!hasDepthWrite()`: translucent markers and
models, occupancy, translucent grid maps, faded points) and returns nullptr,
which every family's `submit` treats as "skip". The present pass finds the
background by coverage, so those draws leave none: the pixels they tint over the
background stay ungraded, and SSAO and EDL never see them.

The TF connection lines keep their own direct pipeline with the annotation alpha
blend; only its HDR clone is bound (the direct pass and the depth replay draw
them with the grid-line pipeline).

Direct pipelines are built for the widget's render target. `setSampleCount()` and
`setColorBufferFormat()` recreate that target on the same QRhi, so
`initialize()` compares the target's descriptor (address and serialized format)
and sample count (`PJ::RhiTargetKey`) with those it built for, and rebuilds
every resource, overlays and the HDR present included, when they differ. When
`initialize()` itself changes the sample count (the output's MSAA follows HDR
support, decided per QRhi), it returns at once: QRhiWidget rebuilds its target
and calls it again, and the frame in between is a backdrop clear.

### Render targets and quality stages

| Target | Format | Written by | Read by |
|---|---|---|---|
| Output | the widget's colour buffer, 1 or 4 samples | present, overlays; or the direct pass | the window compositor |
| HDR colour | 4x RGBA16F renderbuffer, resolved into a single-sample RGBA16F texture | HDR colour pass | present |
| HDR depth | depth resolve: 4x D32F texture (D32FS8 on OpenGL) resolved into a single-sample depth texture. Replay: a 4x D32F renderbuffer for the HDR pass, and the replay's own single-sample D32F texture | HDR pass / depth replay | SSAO, EDL, present |
| Coverage | single-sample RGBA8: alpha = replay coverage, green = raw AO, red = EDL mesh mask | depth replay, SSAO, EDL mask | present |
| Shadow map | 2048² D32F | shadow pass | model and grid-cell shaders |

The chain costs 64 B per render pixel with D32F depth (the replay, and the
resolve off OpenGL) and 84 B with OpenGL's D32FS8 resolve; the depth chain is
picked before admission, so the charge follows it. It is retained per size, charged against a per-view and a per-QRhi cap
(`kSceneLimits.max_hdr_bytes_per_view` / `_per_rhi`, shared by every view of a
top-level), and released on QRhi teardown. HDR, SSAO and EDL each keep a failure
latch keyed on the render size, so a resize probes the stage again.

**Linear light and grading.** Every colour input is linearized before it reaches
the HDR target: colormaps and per-point colours in the vertex stage, vertex and
base colours, theme colours (backdrop, grid), and base-colour and emissive
textures sampled as sRGB. `present.frag` applies exposure, AO, the EDL factor,
the tonemap (None, ACES, AgX, Khronos PBR Neutral; the AgX matrices are
column-major), saturation and a manual sRGB encode. The knobs are the view's
`CompositeParams`; their defaults live in `scene_look_defaults.h`, the source of
truth. The direct fallback writes display-referred colour without a grade.

**Annotation marker and background bypass.** HDR alpha is a per-pixel
tonemap-bypass marker, not coverage. Data draws write it with the coverage-union
blend (`dataAlphaBlend`); annotations (TF triads and lines) add nothing to it
(`annotationAlphaBlend`), so they present flat and vivid. A pixel with no
coverage (the background) keeps the exact theme colour, ungraded.

**Depth resolve or replay.** Where QRhi reports `ResolveDepthStencil` and
multisample textures (desktop OpenGL from 4.3, Vulkan, Metal), the HDR pass
resolves its multisample depth into the single-sample depth texture at the end of
the pass. The coverage pass is then only a clear of the coverage texture that
SSAO (green) and the EDL mask (red) write into, and `present.frag` derives
coverage from the resolved depth. Where the resolve is missing (D3D11, WebGL2,
OpenGL below 4.3), the prepared frame is replayed once into single-sample
RGBA8 coverage plus a sampled D32F depth texture. The resolved depth is one
sample of the 4x pattern where the replay rasterizes the pixel centre, so the EDL
mask pipelines carry a −1 constant and slope depth bias on the resolve chain.
`setForceDepthReplayForTest()` keeps the replay for comparison.

**SSAO** writes raw AO into the coverage texture's green channel through a
colour-only target that samples the unattached depth: inverse-projection view
position, a 5-tap depth normal (`textureVSign()`), the deterministic 32-sample
hemisphere kernel (`ssao_kernel.h`, seed `0x55A0`), 4×4 tiled hash rotation,
0.4 m radius, 0.025 bias, power 1. The present pass blurs it (16 taps over
`[-2,1]²`) and applies it scene-wide, before EDL and the tonemap.

**EDL** is restricted to mesh surfaces. The mask pass replays only the prepared
model ranges with red-only writes, no blending, `LessOrEqual` depth testing
against the scene depth and no depth writes, so non-mesh depth still occludes
hidden meshes, while point clouds, grids, occupancy, voxels, non-model markers,
poses, axes and the background never enter the mask. The present pass
reconstructs log view depth with the inverse corrected projection and evaluates
eight circular neighbours: a mesh pixel darkens when a neighbour is farther or is
not mesh. Each neighbour's log-depth gap is clamped to `look::kEdlMaxGap`, so a
silhouette reads as a graded outline, and the darkening is floored
(`CompositeParams::edl_floor`) at a hue-preserving dark grey. The radius is in
device pixels, scaled by `render_scale`.

Warning priority is HDR, then SSAO, then EDL: each stage's allocation,
rejected-size latch and warning are independent, and each drops to the next
lower-quality path.

### Presentation and overlays

The scene chain renders at `render_scale` × the output
(`setRenderScale`, clamped to [1, 4]): the HDR, coverage, SSAO and EDL targets,
the scene viewport, the projection aspect and the point-size pixel scale all use
that size, the byte budgets and failure latches apply to it, and the present pass
downsamples to the output. The direct fallback ignores the scale. After the
present (or at the end of the direct pass), `OverlayRhiPass` draws at output
resolution on the output target:

- the corner axis gizmo: a lit triad (bottom-right, 80 logical px, oriented by
  the camera rotation) from the shared unit arrow, `pose.vert` + `gizmo.frag`.
  Its viewport depth range is [0, 1e-4], which keeps it in front of the output's
  depth while its arms still occlude each other, with no mid-pass depth clear;
- CPU-rasterized images as premultiplied texture quads (`overlay.vert/.frag`): the
  perf HUD (`setShowPerfHud`, key 'P'; GPU time from
  `QRhiCommandBuffer::lastCompletedGpuTime`, "--" where the backend reports none)
  and the TF hover label and ring, both from `renderHudPanel`
  (`hud_overlay.h`). The view re-rasterizes an image only when its text, DPR or
  theme changes; the pass keeps a texture while the image's `cacheKey()` holds.

### Frame picking

`frameAt(QPointF logical)` is the one picking primitive (the
hover label and the frame context menu): it projects every resolvable TF frame
origin with the current camera, camera-relative like the draw, in logical widget
pixels through `core/tf/frame_picking.h`, and returns the nearest within 20 px;
nullopt when the triads are hidden. It ignores occlusion and does not depend on
DPR or `render_scale`. Hover tracking runs in `mouseMoveEvent` while no button is
held and emits `hoveredFrameChanged` (empty on leave). A right-click on a frame
emits `frameContextMenuRequested`; any other right-click is ignored, so the host
dock's standard menu appears. The dock shows the frame menu with
`QMenu::popup()`, never `exec()`.

### Shadows

The key light (`MeshShadingParams::key_light_dir`) casts real-time directional
shadows. The view owns the 2048² D32F shadow target, its light uniforms and the
orchestration (`submitShadowPass`); each caster family owns a depth-only
pipeline built against that target and records its own prepared casters.

- **Casters:** model draws (visual robot links, hidden or not, and SceneEntities
  model primitives; never collisions, which would double-darken the silhouette)
  and the solid SceneEntities markers (cubes, spheres, cylinders/cones, arrows of
  non-wireframe layers, whatever their opacity; one instanced draw per primitive
  kind replaying the layer's colour instances, `marker_shadow.vert` applying the
  cone taper). Axes, cube edge overlays, lines, triangle lists, points, poses,
  occupancy, voxels and grid maps do not cast.
- **Light frustum** (`core/shadow_camera.h`): the union of both families' caster
  bounds, grown by `extendAabbToGroundShadow` to where the shadow lands on the
  ground (the floor never casts, so it is otherwise absent from the bounds), then
  `fitDirectionalShadowCamera`: bounding-sphere extents (rotation-invariant, no
  resolution pulsing), padded, texel-snapped along a light-only basis so the
  shadow edge does not crawl.
- **Receivers:** models and the filled-cell grid (`grid_cell_shadow.frag`);
  markers do not receive. Receivers project the world position into light space
  through a normal-offset bias and take 16 Poisson taps; a position outside the
  light frustum is lit. The shadow factor multiplies only the key-light term:
  the camera-locked fill and the ambient stay lit, so shadowed surfaces read as
  shaded, not black.
- **Gating:** shadows are skipped unless `MeshShadingParams::shadows_enabled`
  (always true in the app) and a `TransformBuffer` is bound, and there is a caster
  with a valid fit. Layers pose geometry only through TF, so a deleted dataset
  clears the floor shadow with its geometry.

### Per-family passes and caches

**Shared geometry.** The unit cube (voxels), the cube hexagon-fan indices (point
cubes) and the unit pose arrow (pose triads, TF triads, the corner gizmo) are one
`RhiSharedGeometry` owned by the view (`RhiPassSetup::geometry`), created in
`initialize()`, uploaded once, released after every pipeline that draws them.

**Buffer policy.** Per-sample data (clouds, marker expansions, pose instances,
trail ribbons, the grid) sits in Static buffers uploaded on a revision change;
`ensureStaticVertexBuffer` reallocates to grow, or to give back a buffer more
than twice the need. Per-frame data (TF triads, TF lines, model instances,
uniforms) sits in Dynamic buffers. Mesh vertex, index and edge buffers are
Immutable.

**Layer keys.** Every family keys its per-layer GPU state by the layer's address
and a per-layer revision. A layer created at a freed layer's address never meets
a stale entry: the dock takes every layer out of the view's list before
destroying it (`SceneDockWidget::removeTopic()`, `clearLayers()`), and that
`setLayers()` drops its state in every family (`adoptLayerList()`).

**Points (`PointRhiPass`).** Every `PointRenderable` (point and depth clouds),
from `.qsb` shaders that share the appearance GLSL (`shaders/cube/*.glslinc`) and
the `pj_widgets` colormap polynomials (`rhi/shaders/colormap.glslinc`, a copy of
`colormapGlsl()` that `wasm_shader_constants_test` keeps identical).

- *Colour hoist.* Colormap, solid or per-point RGB and the linearization run once
  per point in the vertex stage (`toTargetSpace()` skips the gamma on the direct
  fallback); no fragment stage samples a LUT. A spatial-axis colour adds the
  render origin back (`color_axis_offset`), so a point's colour stays in the
  fixed frame whatever the camera does.
- *Cubes* are the hexagon fan (see "Pointcloud layer"): 7 logical vertices / 18
  indices per instance from `gl_VertexIndex`, faces picked per instance against
  `face_pick_eye` (the eye position, or the view direction under ortho),
  `centroid` local coordinates, no `discard`. The pipelines cull back faces: with
  the eye inside an axis slab (or a cube) a picked face is seen from behind, which
  the faded half (no depth write) would otherwise blend in.
- *Whole-cloud cube LOD* through the shared `chooseCubeDrawMode()`
  (`core/cube_draw_policy.h`: the frustum reject on the inflated box, the
  camera-inside fallback, the 1 px switch); the sprites are the `shape == 2` mode
  of the point shader. Every shape goes through the same reject, with
  `screen_reach_ndc` set to the widest sprite the shape can draw, so a quad whose
  centre is just off screen is kept.
- *Spheres below 2 device pixels* (`kSphereDiscMaxPx`) draw as flat discs at
  `kSphereDiscShading`, the disc mean of the shared Lambert (`cube_mesh_test`
  re-derives it): imposter shading at that size would depend on the driver's MSAA
  placement.
- *Instanced quads or a point list.* Points, spheres and LOD sprites are
  camera-facing quads (a 4-vertex strip per instance, offset in clip space by the
  sprite size, the y offset signed by the backend's clip-space y): every backend
  can draw them, and a quad keeps its visible part when its centre leaves the
  view. A cloud whose largest sprite is at most `kPointListMaxPx` (4 device px)
  draws as a point list instead where the backend has
  `QRhi::VertexShaderPointSize` (the `-DPJ_POINT_TOPOLOGY` bake).
- *Faded out-of-range points* (field colour, 0 < opacity < 1) draw as two
  prepared draws: the in-range half opaque, then the faded half translucent
  without depth writes (so it skips the depth replay). Each layer's uniform
  buffer holds both halves (two SRBs at two offsets).
- *Bounds* come from the layer's conversion (`sourceBounds()`) and feed the scene
  bounds and the whole-cloud reject. A `PointRenderable` without them gets a CPU
  scan of its retained vertices, once per geometry revision.
- *Upload.* The layer hands over its retained cloud as one implicitly shared
  `QByteArray` of `PointVertex` records (`vertexBytes()`, `pointCount()`); the pass
  gives it to `uploadStaticBuffer()` by value, so the update batch shares it, and
  only the backend's own staging copy remains. A layer never writes into an array
  it has handed out (that would detach it and bring the copy back): every sample
  builds a fresh one.

**Markers (`MarkerRhiPass`).** Admission places each SceneEntities layer's frames
and applies the per-layer and per-view instance and stream budgets.
`expandLayer()` turns a layer's `MarkerGeometry` (expanded on a worker,
`marker_geometry.h`) into camera-relative instances (unit cube + twelve-edge
overlay, sphere, cylinder/cone with a per-instance taper, arrow for arrows and
axes), stream vertices (line lists, triangle lists), draw ranges and shadow
casters with their bounds. It is a pure function of the layer's `LayerKey`: the
geometry revision, the render origin, the style (opacity, override colour,
wireframe), shadow casting, and the resolved fixed-from-frame transform of every
frame the layer references, compared exactly (a marker layer whose frame moves
under TF bumps no revision); the matrices are built only on a rebuild. Each
layer's `LayerGpu` keeps its expansion in its own Static instance, line and
triangle buffers; `prepare()` re-expands and re-uploads only the layers whose key
changed. A rebuild allocates all three buffers before it queues any upload, so a
failed allocation leaves nothing of the layer in the frame's batch: the layer
draws nothing that frame and retries the next. Orbit, zoom and playback without
new samples or TF motion reuse every buffer; a pan or a follow step moves the
render origin and rebuilds (the origin stays on the CPU: TECHNICAL_NOTES,
"Render origin"). A layer not admitted in a frame gives its buffers back.
`setMarkerCacheEnabledForTest(false)` rebuilds every layer every frame, the pixel
reference of `scene3d_rhi_marker_cache_test`.

The cube fill is depth-biased away from the camera (constant and slope 1) so the
twelve edges, drawn at the exact depth, win the depth test. The edges replay the
fill's instance range through `marker_edge.vert`: each edge vertex carries its
edge's midpoint, whose two ±0.5 coordinates name the adjacent faces; an edge with
a camera-facing adjacent face (in the view space of binding 4) is the fill colour
× 0.6, a self-occluded one the 0.45 mix back toward the fill colour, both opaque.
Wireframe draws every primitive's triangle edges as line lists (instanced edge
index buffers; streamed triangles expanded to edges) on every backend, without
`QRhi::NonFillPolygonMode` (WebGL2 lacks it); every edge shows, back-facing ones
included.

**Models (`ModelRhiPass`).** Every `ModelRenderable`: per-layer GPU meshes, up to
five material textures per submesh (budgeted before decode), one material SRB
per submesh and one instance per submesh draw. Its caches:

- GPU meshes are keyed by mesh key and `MeshData` pointer. A `modelRevision()`
  bump rebuilds only keys that are new or whose pointer changed and releases the
  keys that are gone, so a URDF whose meshes arrive one by one uploads each once.
  A kept mesh whose material bindings were dropped gets them rebuilt.
- Decoded textures live in one pass-level cache keyed by `TextureSource::key` (a
  content hash for embedded images, the resolved path for files) and colour space
  (sRGB for base colour and emissive), reference-counted by the meshes that bind
  them: shared across the view's meshes and layers, freed with the last user. The
  per-mesh and per-layer pixel budgets still charge every mesh that uses one.
- Wireframe edge lists are built on a mesh's first wireframe draw; the CPU copy
  is dropped once queued for upload.
- The colour and shadow instances are rebuilt and re-uploaded only when the
  render origin, the shading inputs or the admitted draws differ from the last
  prepared frame. The admitted draws hold each layer's revision and look and every
  draw's resolved placement (`placed->matrix() * draw.model`), so a link moved by
  TF alone, which bumps no revision, still refreshes its instances and the shadow
  bounds. Orbit and zoom keep the origin (the camera focal) and skip. The
  instance buffers stay Dynamic: animated models rewrite them every TF tick.

Every material takes `model.frag`'s physically based path: metallic-roughness
GGX with normal, occlusion and emissive maps, lit by the fixed world key ("sun")
light and a camera-locked fill, plus an analytic image-based ambient (diffuse
irradiance and a split-sum specular reflection of a procedural ground-to-sky
environment, Karis `envBRDFApprox`; no HDRI cubemap). A material without PBR data
gets the `pbr_factors` fallbacks (no metallic, the view's roughness). A URDF box,
cylinder, sphere or placeholder draws with the link's colour alone
(`use_material` false): their unit meshes carry a grey material and vertex colour
that would apply it twice. The view owns the camera/lighting uniform buffer
(binding 6, shared with the grid shadow receiver) and hands the shadow map to the
pass (`setShadowMap`/`clearShadowMap` rebuild every material binding
atomically). The EDL mesh mask replays the pass's colour draws
(`submitMeshMask`) with mask pipelines the view derives from the model pipelines.

**Grid and TF lines (`GridRhiPass`).** The grid is built once per settings change
(style, extent, divisions, visibility, colours) at the fixed-frame origin into a
Static buffer; `grid.vert` / `grid_cell.vert` add the origin offset (binding 3,
fixed-frame origin − render origin), so pan, follow and playback upload nothing.
That shader add is exact (TECHNICAL_NOTES, "Render origin"). Its colours come from
the theme (`look::gridColors()`: the backdrop blended toward its foreground, lines
0.35, tiles 0.10 and 0.24), display-referred in the vertices and linearized by the
shader on the HDR target, so a theme switch rebuilds it. The line style draws
line primitives; the filled style draws only the cells, and `grid_cell.frag`
draws the lines in the cell surface from the distance to the nearest cell edge
(`fwidth` anti-aliased), so they cannot z-fight the fill at grazing angles. The
cells start at −extent/2, so an odd division count shifts the edges half a cell
off the origin; the shader gets that phase in the w of binding 3. Every grid SRB
declares binding 3 for the vertex and fragment stages: the connection lines bind
their own SRB to the line pipeline, and QRhi requires matching layouts. The
TF connection lines (magenta parent/child segments from
`buildTfConnectionSegments`) keep a Dynamic buffer keyed on the TF buffer and its
revision, the fixed frame, the render time and the render origin; TF placements
are arbitrary doubles, so they stay camera-relative and draw with a zero offset.

**Poses and TF triads (`PoseRhiPass`).** One unit-arrow mesh and an 80-byte
instance format; each layer draws with one UInt32-indexed instanced call. Pose
instances are frame-local in a Static buffer, re-uploaded when the layer's
`geometryRevision()` moves; the fixed-from-source TF is a uniform, so camera and
TF motion rebuild nothing. The view's TF frame triads use the same pipeline and
mesh; their Dynamic instances bake each frame's camera-relative transform and are
rebuilt when the TF buffer and its revision, the time, the fixed frame, the render
origin, the gizmo settings or the hovered frame (drawn brighter) change.

**Occupancy (`OccupancyRhiPass`).** One R8 texture per layer plus a shared unit
quad, with incremental dirty-rectangle uploads for `OccupancyGridUpdate` patches.

**Voxels (`VoxelRhiPass`).** One packed R32F or RGBA8 3D texture per layer and one
indexed cube draw with `column*row*slice` instances; the vertex shader derives
each voxel from `gl_InstanceID`, `texelFetch`es its value and evaluates the draw
predicate, so the CPU and draw-call cost is independent of voxel count. The full
24-vertex cube (not the fan: a voxel is large enough to swallow the viewpoint)
with back faces culled. Grid origin, fixed-frame TF and the camera-relative
conversion compose in double before upload. A grid wholly outside the frustum is
not drawn (voxel centres inflated by half the largest cell side), but it still
uploads and counts as admitted. Allocation also checks the backend's per-dimension
3D texture limit, queried once per QRhi (`max3dTextureSize()`, TECHNICAL_NOTES).

**Grid maps (`GridMapRhiPass`).** The layer stages the packed grid
(`prepareForRender()` / `stagedGrid()`); the pass uploads two R32F textures
(elevation, colour channel) and draws `6 * (cols-1) * (rows-1)` procedural
vertices from `gl_VertexIndex`, colormapped in-shader (`colormap.glslinc`), with
a geometric two-sided test (the normal faces the eye) instead of
`gl_FrontFacing`, whose winding a backend's clip-space correction can flip.

**Trails (`TrailRhiPass`).** The screen-space ribbon (`trail.vert`, see "Trail
layer"): one Static vertex buffer per trail owner, re-uploaded on a rebuild or a
render-origin change, and a two-block uniform buffer (past and future colour).
The colour is written unlinearized on every target. A pose-embedded trail draws
in its pose layer's slot, before the triads. Trails have no per-view budget and
add nothing to the scene bounds.

### Scene bounds and camera fit

The view owns the scene bounds: each frame, `applySceneBounds` takes the union
of every admitted and placed draw (`SceneBoundsCollector`), so a hidden,
orphaned or over-budget layer does not frame the camera. `CameraFitPolicy`
(`core/camera/camera_fit_policy.h`) fits once per newly added layer, on the first
frame that layer contributes placed bounds, and never over a camera restored
from a layout: the dock calls `noteCameraRestored()` after `adoptState`, and fits
resume only once the layer set is emptied. Only visible layers trigger a fit. A
value set through `setSceneBounds()` lasts until the next frame.

### Failure state and fallbacks

No draw may reference a resource whose `create()` failed. Every `create()` in
`widgets/rhi/` falls in one of four classes:

| Class | Resources | On failure |
|---|---|---|
| Base | the view's four uniform buffers, `RhiSharedGeometry`, and each scene family's `initialize()`: pipelines, layout SRBs, samplers, the voxel LUT, occupancy quad and marker unit meshes | Failure state (below) |
| Optional stage | the HDR chain (targets, samplers, present SRB and pipeline) and the pipelines' HDR/depth-replay clones; SSAO; the EDL mask target and pipelines; the shadow target, bindings, caster and receiver pipelines, material rebinds; `OverlayRhiPass` (corner gizmo, HUD, hover label) | The stage drops and the view renders without it; one warning (logged when the reason changes, shown in the dock label; shadow errors on the caster rows) |
| Silent optional | the point-list pipelines | The same pixels another way: quads |
| Per layer / per frame | per-layer uniforms, SRBs, textures, instance and stream buffers of every family; overlay image textures | That layer's (or image's) draw is skipped this frame; occupancy, voxel, grid-map and model rows note a warning |

**The failure state.** When `createBaseResources()` fails, `initialize()`
releases everything, and `render()` (which checks `baseResourcesReady()` first)
only clears the target to the backdrop. `setRenderFailure()` names the part that
failed ("its trail pipelines could not be created"), puts it first in
`renderingWarningChanged()` (the dock's `scene3dRenderingWarning` label) and emits
`QRhiWidget::renderFailed()` once per episode. QRhiWidget runs `initialize()`
again for a new QRhi (reparent, top-level change) or a new output size, format or
sample count; each retries the build, and success clears the warning. Overlays
are retried the same way. QRhiWidget's own `renderFailed()` (no QRhi for the
window, e.g. another widget pinned it to a different API) sets a "no graphics
device" failure the same way, without a second emission. A clone that
`ScenePipelineRegistry::resolve()` cannot create skips its draw and sets
`cloneFailed()`; the next `render()` drops the HDR chain for that output size
(`rejectHdr`), so the next frame draws directly with every direct pipeline. The
backend line is logged once per QRhi, not on every retry.

Every layer reports its row warning through `Scene3DLayer::statusWarning()` +
`statusWarningChanged`: a data part (`setDataWarning`: decode, admission, budget)
then a renderer part (`noteRenderFailure`/`noteRenderSuccess`, called by the
view), one per line. The dock combines it with the frame-orphan state per row
(`updateLayerWarning`; `recomputeOrphanStates` runs it for every row, also with no
TF bound).

Test builds inject failures (TECHNICAL_NOTES, "Test hooks").

### Widget lifecycle and reparenting

- **A new QRhi rebuilds everything.** Qt calls `releaseResources()` whenever the
  widget's top-level changes (an ADS float or redock, and an ADS dock-area tab
  switch, which parks the hidden tab parentless) and `initialize()` on the new
  QRhi. The view releases every GPU resource and rebuilds it from what the layers
  retain on the CPU: clouds, marker expansions, meshes, volumes and grids stay
  resident for this reason (TECHNICAL_NOTES, "ADS and window-system quirks").
- **Shader packs are read once per process** (`PJ::loadRhiShader()` in
  `pj_widgets`, a process-wide `QShader` cache shared with the plots and the media
  viewer), so a float or redock deserializes none; the pipelines are
  still rebuilt for the new QRhi.
- **A Show on the same QRhi keeps every resource.** QRhiWidget re-renders
  synchronously on every Show. `event()` marks that paint, and `render()` records
  nothing when the QRhi and the output size are unchanged, so the frame drawn
  before the hide is composited at once; it queues an update that then renders
  whatever changed while hidden. This covers a workspace tab (a
  `TabbedPlotWidget` page) shown again.
- **Grabs.** `grabPresentedFrame()` (in `pj_widgets`) grabs after the next
  presented frame, never inside `render()`, and never blocks (no nested event
  loop, which the browser cannot run). A never-shown view is grabbed at once, and
  only once (TECHNICAL_NOTES, "Qt 6.11.1 bugs and workarounds").
- **Idle.** The view renders only on `update()`: a settled view presents no frame
  (`scene3d_frame_benchmark` counts `frameSubmitted` and requires none), and the dock's repaint coalescing ("Repaint coalescing & autoplay") decides
  when playback needs one.

### Shader packs and conventions

The committed `.qsb` packs are baked by `rhi/shaders/bake.sh` for every QRhi
backend: GLSL ES 300, GLSL 330 and 440, HLSL 5.0, MSL 1.2 and SPIR-V. Variants
are separate bakes: `-DPJ_FLIP_Y` (the fullscreen triangle on D3D/Metal),
`-DPJ_GRID_SHADOW` (the shadow-receiving grid cells), `-DPJ_POINT_TOPOLOGY` (the
point list). The WASM configure re-runs the script with the host `qsb` and fails
on any byte difference or a pack without a source, so a stale pack fails closed.

Shaders never remap between clip conventions: the renderer folds them into the
matrices it uploads (`rhi_conventions.h`). `clipToTextureMatrix()` maps corrected
clip space to render-target texture space (u, v, stored depth), `textureVSign()`
orients depth-derived normals, and `fullscreenNeedsYFlip()` picks the flipped
fullscreen triangle. Every pack's varyings are contiguous from location 0 (a
declared, unused input is enough), so a fragment shader's inputs are a prefix of
its vertex shader's outputs (TECHNICAL_NOTES, "D3D11 links stages by register").

### WebAssembly

The browser runs the same view, dock and layers. What differs:

- **Limits.** `kSceneLimits` is `kBrowserSceneLimits` under `PJ_TARGET_WASM`
  (for example 1 M points per cloud and per view, 256 MiB of HDR chain per view
  and 512 MiB per QRhi, where the desktop allows 50 M / 100 M points and 1 GiB /
  4 GiB). A rejected sample or layer fails closed with a row warning; the view's
  budgets retry once they free up.
- **Depth replay.** WebGL2 reports no multisample textures, so the browser always
  replays depth; Qt's GLES backend also does not resolve a second multisample
  colour attachment reliably, which is why the EDL mask is its own pass.
- **Workers and loading.** Mesh fetches and imports run one at a time
  (`maxConcurrentMeshLoads()`). Compressed point clouds are accepted only when the
  browser build has the decoders (`PJ_WASM_WITH_COMPRESSED_POINTCLOUDS`); the dock
  classification, interactive acceptance, factory registration, deferred restore
  and XML validation use the same feature-gated predicate, because an unsupported
  topic would otherwise reach a modal warning that cannot `exec()` in the
  Asyncify-free build. Draco 1.5.7, Cloudini 1.4.1 and bounded Assimp enter only
  the explicitly gated browser graph.
- **URDF from bytes.** A local URDF arrives as bytes (`setSourceFileContent`,
  `addRobotModelLayerFromContent`), never as a path, so a restored browser layout
  asks the user to select the file again.
- **3D textures.** The retained Chromium/Firefox/WebKit matrix measured a
  2,048 per-dimension limit on all three engines.

Persistence is the one dock's `<scene3d version="1">` format in every build.
Restore prevalidates all control and camera fields before mutation and rolls
back the complete previous dock state if a layer payload or topic resolution
later proves invalid.

## Layers and the render contract

Every layer is a `Scene3DLayer` and compiles in every build. The view reads it
through:

- `PointRenderable` (`point_renderable.h`): `vertexBytes()` / `pointCount()` /
  `sourceBounds()` / `geometryRevision()`, implemented by `PointCloudLayer` and
  `DepthCloudLayer`;
- `ModelRenderable` (`model_renderable.h`): frame-local `ModelDrawCall`s and
  `modelRevision()`, implemented by `RobotModelLayer` and `SceneEntitiesLayer`;
- each other family's accessors (`instances()`, `stagedGrid()`, `points()`, the
  packed volume, the marker geometry).

**Admission.** Per-sample admission (payload size, dimension and count limits
from `kSceneLimits`, applied before any decode, plus the mesh importer and
model-fetch limits) fails closed with a row warning. The per-view budgets are
applied by the view: each layer's `prepareForRender(remaining_*)` drains its
pending update first, then checks what it would draw against the remaining
budget (`BudgetLatch`), so one check covers both new data and a budget shrunk by
earlier layers.

**Workers.** `LatestWinsWorker` (`latest_wins_worker.h`) runs the heavy per-sample
work: compressed-cloud decode, depth back-projection, SceneEntities batch
expansion into `MarkerGeometry`, the TF trail rebuild. One `QtConcurrent` job is in flight and at most one request waits;
scrubbing replaces the waiting request with the newest sample.
`invalidate()` (hide, detach, dataset replace) drops the waiting request and
discards the running result. A worker captures only owned bytes, so it may drain
safely after its layer is gone.

**Repaint keys.** Each layer's `renderKey` (sample identity ⊕ the fixed←source
transform, `foldFrameTransformIntoKey`, so a static sample in a moving frame still
repaints) feeds the dock's repaint coalescing (see "Repaint coalescing & autoplay").

**Coordinate contract.** A layer's CPU data is TF-free: source-frame points,
frame-local instances, link-local model draws with their frame id. The view
applies TF and the render origin once, at placement.

## Camera system

The camera is an interchangeable controller over a shared, serializable pose,
deliberately in `pj_scene3d_core` (Qt- and GPU-free) so the math is headless
unit-testable (`camera_near_far_test`, `camera_zoom_to_cursor_test`,
`camera_state_transfer_test`, `camera_follow_test`, `camera_fit_policy_test`).

- **`ICamera` + `CameraState`** (`core/include/pj_scene3d_core/camera/camera.h`):
  `CameraState` (focal, radius, azimuth, elevation, fov_y, ortho_scale,
  perspective) is the pose every model exports via `state()` and adopts via
  `adoptState()`, so switching models preserves where you were looking.
  `SceneViewWidget` holds a `std::unique_ptr<ICamera>`; `setCameraModel` does
  capture → construct → `adoptState` → swap.
- **Four models, selectable.** `SceneViewWidget::CameraModel` enumerates
  `{ kOrbit, kXyOrbit, kFly, kTopDownOrtho }`. That order is load-bearing: the
  overlay combo's index casts directly to it. `OrbitCamera` (perspective
  spherical orbit, Z-up; non-`final`), `XYOrbitCamera : OrbitCamera` (orbit pivot
  locked to the z=0 ground plane), `TopDownOrthoCamera : ICamera` (orthographic
  bird's-eye, rotatable about +Z: the robotics "2D mode" for costmaps and grids),
  `FlyCamera : ICamera` (first-person free eye + yaw/pitch, no orbit pivot: left
  drag looks around, pan strafes, wheel dollies forward).
- **Adaptive near/far** (`core/src/camera/camera_math.cpp`, `adaptiveNearFar`):
  `near = max(d·1e-2, 1e-3)` from the working distance only (so close inspection
  never clips), `far = max(d·4, scene_reach)·1.5` (reaches the whole scene), then
  `near = max(near, far/1e5)`, a ratio cap that bounds depth precision.
  `scene_reach` is the scene AABB diagonal plus the focal-to-centre distance.
  `TopDownOrthoCamera` uses its own flat-scene guard instead (near/far measured
  against the eye height above the focal plus the scene's vertical extent), so a
  zoomed-in costmap never clips the ground.
- **Zoom-to-cursor** (wheel): a homothety about the world point under the cursor.
  The eye and focal scale toward the cursor target by `pow(0.9, ticks)`, so the
  hovered point stays pixel-locked. A homothety cannot rotate the orbit:
  `OrbitCamera::zoomToCursor` keeps azimuth/elevation untouched, scales the
  radius (clamped to `[lo, hi]`), and slides the pivot along the cursor lever by
  the effective post-clamp factor `new_radius / radius`. It does **not** re-derive
  the angles from `eye − focal`: at large world coordinates that subtraction of two
  ~1e6 vectors loses its low bits to float32 cancellation and spins the view a
  fraction of a degree on every tick (~1.9°/tick at 5e6). `XYOrbitCamera` re-locks
  its pivot to z=0 the same cancellation-free way (re-pivot along the fixed view
  ray, never `setEyeFocal`). A degenerate ray (no ground/focal-plane hit) falls
  back to centre-of-view `zoom()`; right-drag stays centre-of-view zoom; on
  `FlyCamera` zoom-to-cursor is a forward dolly by design.
- **Frame follow (Position-only).** `ICamera::followShift(world_delta)` shifts
  the camera's anchor by a world delta without touching orbit angle or zoom: Orbit
  and TopDownOrtho move the focal, Fly the eye, and `XYOrbitCamera` drops the z
  component (ground-locked). `SceneViewWidget` holds the follow target
  (`setFollowFrame`, "" = off) and applies it each tracker tick in `applyFollow`:
  look up the target's origin in the fixed frame at the render time and shift the
  camera by the delta against the previous tick's origin. The first tick after
  enabling, a fixed-frame change or a lookup gap only seeds the baseline (no
  shift), so enabling never jumps and the user's orbit/zoom/pan is preserved.
  `followRenderKey(time)` hashes the followed origin into the dock's
  `viewRenderKey`, so a moving target is not coalesced away by the repaint gate
  even when the TF overlay is hidden. The UI is the "Camera" section's **Follow
  frame** picker in `Scene3DConfigPanel`, with a **recenter** button
  (`recenterOnFollowFrame` → `followShift(target − current focal)`). Heading and
  Pose (rotation-following) modes are future work; the `FollowMode` enum reserves
  the slot.
- **Large-coordinate precision (camera-relative rendering).** Following a frame
  in a UTM/GPS-scale map parks the camera at ~1e5–1e7 m, where float32 keeps only
  ~0.1–0.6 m of resolution: subtracting a ~1e6 eye from a ~1e6 vertex in float
  makes the whole scene swim on zoom. Each frame therefore picks a render origin
  (the camera focal, in double), builds the view with
  `ICamera::viewMatrixRelativeTo(render_origin)` (bit-exact `viewMatrix()` at
  origin 0, pinned by a test), and every placement subtracts the origin in double
  before the float cast (`relativeModel`, `transformedBounds`).
  All TF-placed geometry, the hover pick and the eye fed to model lighting land
  near the origin. The camera state and the scene bounds stay in absolute world
  (follow and near/far need them there); only the per-frame render path is
  relative. The grid adds its offset in the shader, the TF lines take the origin
  explicitly, and point colour-by-axis adds the origin back so colour stays in the
  fixed frame.
- **Frame-list freshness.** The frame pickers (fixed frame + follow) are fed by
  `refreshAvailableFrames` → `getFrameHierarchy()`, which is time-independent
  (whole buffer). `Scene3DDockWidget` re-enumerates on `datasetTransformsReady`
  (file ingest), decoupled from the time-gated repaint path, so the full tree is
  listed as soon as it loads, even while the playhead is paused.
- **Scene bounds.** `Scene3DLayer::worldBounds()` returns an optional
  source-frame `AABB`; `PointCloudLayer`, `OccupancyGridLayer`, `DepthCloudLayer`,
  `VoxelGridLayer` and `GridMapLayer` override it. `RobotModelLayer` stays
  bounds-less by design: a robot is posed by the live TF tree the camera already
  frames through the other layers, so adding its links would pull the camera
  around as joints move. The view unions what it admits each frame (see "Scene
  bounds and camera fit"); no contributing layer → an invalid AABB → the
  working-distance fallback for near/far.
- **Overlay UI.** `Scene3DDockWidget` overlays a `camera_model_combo_` (`{Orbit,
  XYOrbit, Fly, Top-down ortho}`) and a `home_button_` (resets the active model
  to its default view; not fit-to-scene), anchored flush to the top-right edge;
  the corner axis gizmo sits bottom-right. They are children of the dock, not the
  view, and `raise()`d above it.
- **Persistence.** `xmlSaveState` writes the active model as a stable string id
  (`orbit` / `xy_orbit` / `fly` / `top_down_ortho`, independent of the enum order)
  plus the `CameraState` as JSON (`cameraStateToJson`) and the `follow_frame`
  attribute (empty = off); `xmlLoadState` restores all three, sanitizing the
  state so a corrupt layout can never drive a degenerate view. A restored follow
  target absent from the current TF tree stays inert until it appears
  (`applyFollow` holds on lookup failure).

## Pose-array layer (`PosesInFrame`)

`PosesInFrameLayer` renders a `PJ.PosesInFrame` topic (`geometry_msgs/PoseArray` /
`foxglove.PosesInFrame` equivalent: a flat list of poses in **one** `frame_id`,
not a TF tree) as a per-pose coordinate-axis **triad**, with per-layer params:
**arrow length** (m), **opacity**, an **X-arrow-only** geometry mode (a single X
arrow per pose, for dense arrays where full triads clutter), and an orthogonal
**color override** (every arm in one colour). Geometry and colouring are
independent: a recoloured full triad, or a single X arm in its natural red.
Defaults (0.15 m, 1.0, off, off) and the desaturated X/Y/Z colours match the TF
"Frames" triads.

- **Pure expansion (core, unit-tested).** `buildPoseTriadInstances(msg,
  PoseTriadStyle{axis_length, opacity, x_arrow_only, override_color, color})`
  (`core/poses_in_frame_render.{h,cpp}`) turns each pose into up to three
  `PoseTriadInstance{mat4 model, vec4 color}` arms (`poseToMat4(pose) ·
  arm-rotation · scale(size)`), frame-local, with `opacity` in the colour alpha.
  The style keeps geometry (`x_arrow_only` → 1 arm vs 3) separate from colouring
  (`override_color` → every arm takes the shared colour, else its natural R/G/B).
- **Ingest.** `attach` bootstraps the source frame; `setTrackerTime` defers to the
  next frame, where `updateAt` decodes `store.latestAt(t)` via a **per-use**
  `resolveObject` binding (never cached: the reload-UAF rule) and expands it into
  the retained instances. A `SequentialUID`-keyed guard skips redundant decodes
  while scrubbing within one message; a style revision forces the current sample
  to re-expand after a style edit. An undecodable sample fails closed (no arms plus
  a row warning); non-finite instances are compacted away before bounds.
- **Draw.** `PoseRhiPass` (see "Per-family passes and caches"):
  `prepareForRender(remaining_arms)` each frame, one instanced draw per layer, the fixed-from-source TF in a uniform.
  Admission follows `SceneLimits` (browser: 100,000 poses and 32 MiB of wire bytes
  per layer, 300,000 arms per view); rejected layers warn and release their arms.
  An unresolvable frame draws nothing (orphan).
- **Params persistence.** `xmlSaveState`/`xmlLoadState` round-trip `<poses_in_frame
  gizmo_size gizmo_opacity x_arrow_only override_color override_color_value>`,
  which also makes the params copy/paste/apply-to-family-able through
  `pj_scene_common`'s `serializeLayerParams`/`applyLayerParams`. The config
  widget's "Override color" checkbox + "Color:" swatch reuse `SceneEntitiesLayer`'s
  marker-recolor text and layout (picking a colour ticks the box).
- **Owned trail.** The layer owns an optional trail (see below), drawn in its slot
  before the triads.

## Trail layer (`TrailLayer`)

`TrailLayer` draws the trajectory of a TF frame's origin (or a `kPosesInFrame`
topic's first pose) across the whole loaded time range as a screen-space ribbon
(triangle strip) in the fixed frame, split-coloured at the tracker time. See
REQUIREMENTS §3b for the behavioural contract; the mechanics:

- **Sampling grid = the chain's own stamps.** `TransformBuffer::chainSampleTimes`
  unions every edge's sample stamps along the connecting path (both chains to the
  common ancestor), so a statically-mounted child (lidar on base_link) trails with
  the motion higher up the chain. The pure build functions
  (`core/trail_sampling.{h,cpp}`: `decimateEvenly` — even index decimation,
  endpoints kept, cap `kMaxTrailPoints` = 50k — `buildTfFrameTrail`,
  `buildPoseTopicTrail`) are Qt/GPU-free and unit-tested. Pose-topic builds
  decimate **before** decoding, so at most 50k messages ever parse.
- **Split ribbon, zero-upload scrub.** One vertex buffer per trail: two expanded
  vertices per point (position + joint tangent + side flag). A ribbon, because
  line primitives are 1 px on most backends and the per-trail thickness (default
  4 px) must be honoured exactly. CPU points are absolute fixed-frame doubles,
  downcast to float after subtracting the render origin; the pixel-width offset
  happens in the vertex shader, scaled by `render_scale`. Two draws over the one
  buffer: points `[0, past)` in the past colour, `[past-1, N)` in the future colour
  (the shared pair keeps the ribbon continuous). The split index is a binary
  search over the stamps, so scrubbing re-uploads nothing; the buffer re-uploads
  only on a rebuild or a render-origin change. `renderKey` folds (build revision,
  style revision, split index): between two samples it is constant and the repaint
  gate coalesces.
- **Live append.** A live append rebuilds the expanded vertex buffer and
  re-uploads it whole (near the 50k cap: ~4 MB of CPU work and ~2.8 MB of upload
  per appended tick). An incremental tail upload is the planned refinement; file
  loads never hit this path.
- **Async rebuild / sync append.** Full TF-source rebuilds run on a
  `LatestWinsWorker` (safe: `TransformBuffer` is `shared_mutex`-protected and the
  task copies the `shared_ptr`). Pose-source rebuilds stay synchronous on the GUI
  thread (`ObjectStore` reads and `parseLocked` decodes are GUI-thread contracts),
  bounded by decimate-before-decode. Live-edge appends (both kinds) are
  synchronous and tiny: gated on `TransformBuffer::revision()` / the store entry
  count, they sample only stamps past the last built point and ring-trim at the
  cap.
- **Fixed-frame self-heal.** The layer adopts the fixed frame from the view each
  frame (`syncFixedFrame()`) and rebuilds on mismatch, so attach order never
  matters. The TF buffer arrives via `Scene3DLayerContext` at attach **and**
  through `Scene3DDockWidget::pushTransformBufferToLayers` whenever the dock's
  binding changes: layout restore replays `<layer>` elements BEFORE the
  `<config_topic>` that binds TF, so a restored trail starts buffer-less
  ("waiting for TF") and heals when the binding lands.
- **Not topic-id-backed.** Trails register under synthetic local ids
  (`allocateLocalLayerId`) through the `SceneDockWidget::insertLayer` seam: no
  `LayerFactory` slot and no `sdk::BuiltinObjectType` (`info().object_type ==
  kNone`). Persistence marks the layer element `role="trail"`; everything else,
  including the pose source topic's `source_topic_name`/`source_dataset_*`
  identity, lives in the layer's own `<trail>` payload, and the layer
  self-restores via `TrailLayer::xmlLoadStateResult` (tri-state incl. deferral),
  with the dock's `restoreTrailElement` a thin host. Restore branches BEFORE the
  object-type gate. `xmlLoadState` applies STYLE only: family copy/paste restyles,
  never retargets. Pruning: a TF-source trail orphans on a missing frame (it
  revives if the frame returns) but is REMOVED when the bound TF dataset unloads
  (`resetTransformBindingIfDatasetGone` deletes TF trails before dropping the
  binding).
- **Pose trails are owned, not registered.** `PosesInFrameLayer` holds a
  `std::unique_ptr<TrailLayer>` (no QObject parent) that is never handed to the
  dock, so it has no Topics row, no settings panel and no synthetic id. The owner
  forwards the lifecycle (`attach`/`detach`, `setFixedFrame`/`setTrackerTime`/
  `setVisible`) and re-emits the trail's `repaintRequested`/
  `configurationChanged`/`statusWarningChanged`, plus relays `statusWarning()`,
  since the dock only polls REGISTERED layers. The trail REBUILDS against the
  fixed frame, so the owner caches it to hand to a trail enabled later.
  Persistence nests the trail's `<trail>` element inside `<poses_in_frame>`
  (presence == enabled; `isLeafPayloadWithOptionalChild` admits exactly that one
  child), reusing `TrailLayer::xmlSaveState`/`xmlLoadState`.
- **Entry points.** Right-click on a TF frame: the view's `frameAt` pick emits
  `frameContextMenuRequested`, and the dock's menu offers Create trail / Set as
  fixed frame / Follow this frame. The pick walks the TF tree only, so pose
  triads are not pickable; a pose trail is reached from its layer's settings. Plus
  the `Scene3DConfigPanel` "Trail" row (frame combo + add button, TF only) and the
  "Trail" toggle on `PosesInFrameLayer`'s config widget. Both flavours are found
  through the dock-local `trailOf()` helper (the layer IS one, or OWNS one), which
  is what lets the TF-buffer re-bind fan-out reach embedded trails.

## Pointcloud layer (`PointCloudLayer`)

A sample's path: resolve (`latestEntryIdAt`, per-use parser binding) → admit
(`cloudAdmissionError`: point and wire-byte limits; for compressed blobs, before
decompression, with the decode limits handed to the codec) → decode on the
`LatestWinsWorker` when compressed (Draco/Cloudini → canonical `sdk::PointCloud`,
`core/pointcloud_codecs.h`) → `convertCanonical()` → `packFinitePointVertices()`
→ the retained `PointVertex` array the view reads (`vertexBytes()`), with the
source-frame AABB and the automatic scalar range from the same conversion pass.

- **Conversion.** `convertCanonical()` (`core/pointcloud_convert.h`) decodes xyz
  and the colour scalar of any field datatype into float (`DecodedPointCloud`),
  accumulating the finite-point bounds; RGB-direct mode extracts a packed `rgba`
  (or separate `red`/`green`/`blue` channels). `packFinitePointVertices()` then
  compacts the finite points into fresh 20-byte `PointVertex` records (x, y, z,
  scalar, rgba). Every rejection (admission, allocation, a malformed cloud, no
  finite position) clears the cloud and shows a row warning (fail closed).
- **Caches and memos.** One successful decoded compressed sample is cached. One
  failed sample is memoized with its warning: revisiting it restores empty
  geometry and the same warning without retrying the codec, while moving to a
  different sample recovers normally. The point and depth layers track the wanted
  sample: they cancel the waiting request when the tracker returns to the running
  or displayed sample, and drop any completion that is not the wanted one. A hidden
  layer releases its retained cloud.
- **Compressed decode never blocks the UI thread.** Cloudini dimensions/bytes and
  Draco point count plus packed attribute layout are bounded before their
  principal allocations. Decoded caches are per layer, not a global RSS quota.

### Cube shape: the hexagon fan

The `kCube` shape is a solid, not a billboard, so its geometry is cut to the
minimum that is still pixel-equivalent. A closed opaque cube never shows more
than the three faces whose outward normal points at the camera, so the pass draws
exactly those, as a **6-triangle fan around the cube's near corner**: 18 indices
over 7 logical vertices, versus the 24-vertex / 36-index full solid. There is no
cube vertex buffer: `cubeFanCorner(vertex index, cubeFaceSigns(...))` (the shared
GLSL of `cube_mesh.h` and `shaders/cube/`) generates the corners from the shared
fan index buffer.

Pitfalls, before editing this path:

- **Face choice is per instance.** The sign vector comes from each cube's own
  centre versus the face-pick eye, which is homogeneous: the eye POSITION (w = 1)
  under perspective, the scene→camera DIRECTION (w = 0) under ortho, where the
  visible faces depend on the view direction alone. Derive it from the PROJECTION
  matrix, not from a folded view-projection: folding view into proj leaves the
  eye's view-axis distance in the `[3][3]` slot, so the usual `proj[3][3] == 0`
  perspective test silently fails. Getting this wrong gives every cube the same
  face triple and drops a face from half of them.
- **The face normal is recovered in the fragment shader** from the interpolated
  corner (the axis whose |component| reaches the surface), so the vertex stage
  carries no normal. The local coordinate must be interpolated `centroid`: at the
  default pixel-centre sampling, partially covered MSAA pixels extrapolate outside
  the triangle and can push a non-face component past 0.5, mis-picking the face.
- **No `discard`.** The outside-range fade's "invisible anyway" case is culled in
  the vertex stage, so the fragment shader stays discard-free and keeps early-Z
  depth writes: cube clouds overdraw heavily, and late-Z would shade every hidden
  fragment.
- **Colour is evaluated per point, not per fragment.** A point's colour (colormap,
  solid or per-point RGB, plus the sRGB→linear conversion) is constant over the
  cube, so the vertex stage computes it once into a `flat` varying. The cube's
  edge outline survives this because it SCALES the colour — `mix(b, b*0.4, e) ==
  b*(1-0.6e)` — and scaling commutes with the gamma, so the fragment stage keeps
  one scalar `pow` and the result is algebraically identical. Applying the outline
  in linear space instead would drop that `pow`, but it measured 3% off and is not
  exact.
- **A camera inside a cube** is the one case the fan gets wrong (the centre-vs-eye
  comparison needs the camera at least half a cube away). Irrelevant at
  point-cloud cube sizes, but `setSizeMeters` does not stop a user from making
  cubes large enough to swallow the viewpoint; it is why `VoxelGridLayer` keeps
  the full solid.

`cube_mesh_test` pins the fan's coverage in all eight sign octants and the
CCW-outward winding of the shared solid; `scene3d_rhi_point_test` renders mirrored
cubes (equal areas, three face shades) and a camera inside a cube.

### Screen-space LOD: sub-pixel cubes become matched sprites

Below about a pixel even the fan is more geometry than the result can show, so the
whole cloud swaps to one sprite per cube: the point shader's `shape == 2` mode, no
colormap in the fragment stage, no normal, no `discard`. The threshold is
`kDefaultCubeLodThresholdPx` (1 device pixel, `core/cube_draw_policy.h`).

**It is a whole-cloud switch, decided on the CPU, and that is a measured choice**
(TECHNICAL_NOTES, "Rationale"): the win comes from dropping a draw, not from
converting cubes. `chooseCubeDrawMode()` asks one question — does the cloud's
AABB prove that no cube reaches the threshold? — (clip w is affine, so the box
bounds every point) and picks one program. A stale extent can therefore only draw
near cubes as sprites; it can never make points vanish, which a shader-side cull
could.

Each piece of the appearance match is an exact property of the cube
(`kCubeSpriteGlsl`):

- **Area, not side.** The three visible faces cover `(|r.x|+|r.y|+|r.z|) * side²`
  where `r` is the view rotation's third row, so the sprite is a square of that
  area. Matching the side instead is ~37% too bright.
- **Area-weighted Lambert.** The faces' `cubeLambert()` terms weighted by those
  projected areas (`+n` and `-n` foreshorten alike), so it is three dot products
  and no CPU uniforms; the light direction is defined once, in
  `kCubeLightingGlsl`, shared with the cube's own fragment shader.
- **`kCubeEdgeMeanScale`.** The dark edge band covers a fixed fraction of every
  face; dropping it leaves sprites 13% brighter than the cubes beside them.
- **A one-pixel floor at full colour.** Below one device pixel the sprite stays
  one pixel in the cloud's own colour, the contract every other shape has. Dimming
  it by the area overshoot conserves light only against a black background: the
  opaque pass replaces the background under the whole widened sprite, so on PJ's
  light backdrop a zoomed-out cloud would turn black. At the threshold itself the
  sprite is ~1.2 px, above the floor, so the switch is still an exact area match.

`scene3d_rhi_point_test` checks that a sub-pixel cube cloud becomes coloured
sprites; the golden `shape_cubes_subpixel` holds its look.

## Depth-cloud layer (`DepthCloudLayer`)

Back-projects a depth image into a 3D point cloud (one point per valid pixel),
coloured by depth. Sibling of the 2D depth view: same data, different geometry.

- **No `kDepthImage` producer.** Depth arrives as an `sdk::Image` with a depth
  `encoding`; the parser can't tell depth from colour at schema-classification
  time. So `DepthCloudLayer` is registered on `kImage` and the dock **gates** a
  topic by peeking the first sample's `encoding` (`isDepthEncoding`: `16UC1` /
  `32FC1` / `compressedDepth`); colour images are refused. Because the two share
  `kImage`, an empty-placeholder image drop opens the **2D** viewer (host policy in
  `pj_app`); a depth image reaches a 3D dock by being dropped onto an existing one
  or via the 3D family switch.
- **Decode (`toDepthView`).** `16UC1`/`32FC1` alias the image bytes (zero-copy).
  `compressedDepth` is PNG-decoded via `QImage` to a `16UC1` (millimetre) view,
  including a **bare-PNG signature repair**: RealSense bags carry a headerless PNG
  that begins at the `IHDR` chunk, so the 8-byte signature + IHDR length are
  restored before decode (mirrors the 2D path's `toDepthImage`). 32FC1
  inverse-quantized compressedDepth is not yet handled.
- **Intrinsics by `frame_id`.** A `CameraInfo` is joined to the image by
  **`frame_id`** (authoritative, never the topic name: Foxglove's rule, Rerun's
  camera hierarchy). An exact match wins; with none, `resolveIntrinsics` falls
  back to a lone `CameraInfo` only when unambiguous, else refuses rather than pair
  the wrong camera. The result is memoized by `(frame_id, CameraInfo SampleId)` and
  revalidated metadata-only via `latestEntryIdAt`; a new CameraInfo sample
  re-projects the current depth image even when the depth sample is unchanged.
- **Back-projection.** The UI thread admits the image against `kSceneLimits`
  (payload bytes, declared or PNG-header dimensions); a `LatestWinsWorker` then
  inflates and back-projects it (the Qt-free `depth_backproject.{h,cpp}`,
  `depthToPoints`: positions, per-point depth scalar, AABB and colormap range in one
  pass) straight into a fresh `PointVertex` array, which the layer retains as its
  `PointRenderable` data. Stale results are dropped by generation.
- **Colour.** Depth is coloured through the shared `PJ::Colormap` (turbo / viridis
  / plasma / grayscale, `pj_widgets/Colormap.h`), the same source of truth as the
  2D depth view and the point-cloud field colouring.
- **Config.** Per-layer colormap, point size, and a min/max depth range.

## Voxel-grid layer (`VoxelGridLayer`)

Renders a dense `sdk::VoxelGrid` (SDK ≥ 0.10.0), a dense 3D lattice whose
per-voxel value is generic via `fields` (occupancy / cost / ESDF / semantic, or a
direct RGBA channel), as GPU-instanced cubes.

- **Dense→cubes expansion is entirely GPU-side** (`VoxelRhiPass`, see "Per-family
  passes and caches"): the selected field is one 3D texture and the cubes are one instanced
  draw, so a re-scrub to a cached grid re-uploads nothing, though the vertex shader
  still runs once per voxel. The draw predicate is viewer-side (the schema does
  **not** encode it).
- **Qt-free core.** The coordinate/value math (`core/voxel_grid_view.{h,cpp}`,
  `core/voxel_grid_value.{h,cpp}`) is headless unit-tested.
- **Layer.** `updateAt` packs the volume and the layer retains it for the pass
  (dropping the decoded-grid memo to spare the browser heap). Browser caps are
  4 Mi voxels and 32 MiB wire bytes per layer plus 8 Mi voxels per view. A layer
  over a limit, or over the backend's 3D texture limit, warns and releases its
  retained volume. Field, predicate, colour, opacity, geometry and XML semantics
  are the same in every build.
- **Follow-up.** A GPU compaction path (an indirect draw over only the accepted
  voxels) is a documented follow-up for very large dense grids; not built.

## Grid-map layer (`GridMapLayer`)

Renders an `sdk::GridMap` (SDK ≥ 0.26.0), a 2D grid of per-cell channels (the
canonical form of `grid_map_msgs/GridMap` and `foxglove.Grid`), as a lit,
colormapped **heightfield** surface, the way Foxglove's linear-interpolation Grid
mode does. It renders in every build, the browser included.

- **Procedural geometry.** `GridMapRhiPass` draws `6 * (cols-1) * (rows-1)`
  vertices with no vertex or index buffer: the vertex shader derives quad, triangle
  and corner from the vertex index, fetches the corner's elevation from an R32F
  texture and places the vertex at the cell centre. A new sample costs two texture
  uploads (elevation + colour channel, both de-interleaved on the CPU by the
  Qt-free `core/grid_map_view.{h,cpp}`) and nothing per vertex. Elevation "None"
  skips the texture and draws a flat plane.
- **Holes stay holes.** Every vertex fetches all three corners of its triangle and
  emits a `flat` validity; the fragment shader discards the whole triangle when any
  corner is non-finite, so a NaN cell punches out exactly the triangles touching
  it. Validity uses `abs(e) < 3e38` rather than `isnan()` so it does not depend on
  a driver preserving NaN through `texelFetch`.
- **Normals** are central differences of the elevation texture scaled by cell size,
  with a one-sided fallback at borders or beside a NaN neighbour and an up-normal
  when neither neighbour is finite: a hole never tilts a valid triangle.
- **Shading** is a Lambert key light (the view's world-space key-light direction)
  plus a camera-locked fill, scaled by the view's ambient/direct/fill knobs,
  two-sided by the geometric normal-faces-eye test. Not the model PBR path; no
  shadow cast or receive.
- **Colour** is `LUT(clamp((v - lo) / (hi - lo)))` through the shared colormap
  GLSL; `lo/hi` are the finite min/max of the colour channel (auto) or the manual
  range. A degenerate range (`hi == lo`, or no finite sample) paints the LUT
  midpoint; a NaN colour value on a valid triangle maps to the LUT low end.
- **Placement** is `TF(fixed <- frame_id at the CURRENT tracker time) * origin`,
  resolved per frame like the occupancy grid. `worldBounds()` is a source-frame box
  (the local extent times the origin pose, z from the finite elevation range),
  never the fixed-frame transform.
- **Lifecycle** follows `PointCloudLayer`: the topic attaches with a parser OR the
  canonical `PJ.GridMap` codec (`resolve_object.cpp`); `renderKey()` is the sample
  UID from `latestEntryIdAt` plus the fixed<-source transform and never resolves
  bytes; the canonical object is cached per UID, so a selector change re-packs
  without re-transcoding. On the parser route the cached object's anchor was
  allocated inside the plugin DSO (the grid_map transcoder owns its own buffer), so
  the layer holds the binding keepalive next to the cache, declared so the grid
  dies first: otherwise the extension catalog can unmap the DSO before the dock is
  destroyed and the cached grid's destructor faults at shutdown.
  `validateGridMap()` runs before extraction, and a failure (or a grid past
  `kMaxRenderableGridMapCells`) surfaces a row warning. Dataset replace and detach
  reset the memo, bounds and warnings.
- **Config.** Elevation field (with "None"), colour field, colormap, auto / manual
  range (manual seeded from the last auto range), lighting toggle, opacity,
  persisted as `elevation_field, color_field, colormap, auto_range, range_lo,
  range_hi, lighting, opacity`. The persisted names are REQUESTS: a missing field
  falls back (with a warning) but the request is kept, so it is honoured again when
  the field reappears.
- **Not built:** shadows, a wireframe overlay, a nearest-neighbour cuboid mode,
  RGBA-from-fields colour.

## Repaint coalescing & autoplay

- **Per-tick repaint coalescing.** During playback the scene must not free-run at
  60 Hz redrawing identical frames. `ISceneLayer::renderKey` produces a decode-free
  per-layer fingerprint (sample identity ⊕ the fixed←source transform, folded by
  `foldFrameTransformIntoKey`), the dock folds in its view key (TF triads and
  lines, follow target), and `SceneDockWidget::onTrackerTime` skips the repaint
  when nothing would change: the ≤ 60 Hz / idle → ~0 CPU budget of the performance
  goals. `renderKey` must derive from index/entry timestamps, **never** from a
  `latestAt` that would decompress cold chunks inside the gate.
- **`--autoplay`.** The app's `--autoplay` flag starts hands-free looped playback;
  it doubles as the `PJ_AUTOPLAY` profiling hook for the repaint/render cost.

## URDF / robot-model subsystem

- **Parser** (`widgets/src/urdf_parser.{h,cpp}`): `QDomDocument`-based; reads
  `<link>` visuals/collisions (origin, geometry, material). Geometry is the
  `GeomShape` variant: box/cylinder/sphere primitives **and** meshes. `<joint>` is
  parsed into `RobotModel::joints` (name/type/parent/child/origin), but **TF owns
  kinematics**: link poses come from the live TF tree. xacro is detected (element
  prefix or filename) and rejected with an actionable error.
- **Fixed-joint TF bridges** (`core/robot_model_bridges.{h,cpp}`): a URDF may
  contain a frame the data's `/tf` never publishes (a gripper rigidly mounted to an
  arm flange, where the mount transform lives only in the URDF).
  `fixedJointStaticTransforms()` turns each **fixed** joint into a static
  (`/tf_static`-style, epoch-stamped) `StampedTransform`; `RobotModelLayer` caches
  them (frame-prefixed) and `ensureStaticBridges()` re-asserts them into the
  dataset `TransformBuffer` every tracker tick via
  `injectMissingStaticTransforms()`. The inject is **guarded** (never overrides a
  frame the data publishes), **idempotent** and **self-healing** (a same-file
  reload that clears the buffer re-bridges on the next tick). Movable-joint
  articulation (a `JointState` source, `<mimic>`, default-to-0) is deferred.
- **Sources.** A `RobotModelLayer` reads its URDF from a store topic
  (`kRobotDescription`, decoded via the topic's parser: payload bytes are
  CDR-framed, never cast to a string), a local file, or a URL. File/URL layers are
  dock-local: synthetic registry ids that never touch the ObjectStore
  (`Scene3DDockWidget::addRobotModelLayer{,FromUrl}`). The browser receives a local
  URDF as bytes (see "WebAssembly"); native builds read the path, even for a layout
  the browser saved. A dock whose only content is local robots gets a private TF
  buffer (`ensureLocalTransformBuffer`) for the fixed-joint bridges; a dataset
  binding replaces it.
- **Coordinate contract.** `RobotModelLayer` and `SceneEntitiesLayer` keep a
  TF-free CPU model: a robot geometry is its prefixed link frame plus the
  link-local `<origin>` and scale; a scene-entity model is its entity frame plus
  pose × scale. They expose these as frame-local `ModelDrawCall`s through
  `ModelRenderable`; the view applies TF and the render origin once. Fixed-joint
  bridges are injected into the buffer the dock hands the layer through
  `setTransformBuffer`, the one the view resolves against.
- **Mesh loading** is async with a small bound: up to `maxConcurrentMeshLoads()`
  fetches + assimp imports (`QtConcurrent::run`, watched by `MeshImport`) in flight
  per layer (four on desktop, one in the browser), each completion requesting its
  own repaint; the GUI thread never blocks. Each mesh is admitted as it lands
  against `kSceneLimits`' retained-byte budgets (per mesh, per layer), and a source
  switch (or, for a topic-backed robot, a dataset replacement) bumps a generation
  that drops stale completions. SceneEntities and the robot layer keep a superseded
  import counted until its worker finishes (the robot keys imports by generation +
  path and drops the result), so rapid source replacement never runs more than the
  bound; `MeshImport::take()` copies, since `MeshLoader` shares one cached future per
  path. assimp covers STL/DAE/OBJ/glTF. `MeshData` carries UV0 + tangents
  (`aiProcess_CalcTangentSpace`) and a per-`SubMesh` `Material` (glTF 2.0
  metallic-roughness, read via assimp's material abstraction). Each map
  (`TextureSource`) is an external file path OR inline bytes for embedded glTF/GLB
  `*N` images (still PNG/JPEG-encoded), keyed by content hash. Sources without PBR
  factors fall back to `MeshShadingParams`.
- **Rendering** is the model pass (see "Per-family passes and caches"). Primitives come from
  unit box/cylinder/sphere geometry (URDF cylinder is Z-aligned; sizes ride the
  model matrix). Unresolved meshes render a **magenta unit cube**, impossible to
  mistake for data. Visual meshes are split into opaque and best-effort
  translucent draws (layer opacity, override alpha, or glTF `BLEND`; no depth
  sort). Collision geometry with no `<material>` gets an **orange tint** (RViz
  convention); below full opacity it renders as a translucent overlay (blend on,
  depth writes off), at opacity ≈ 1.0 as a solid, occluding hull. Scene-wide
  opacity and visibility come from `meshShadingParams()`.
- **Time contract**: `RobotModelLayer::timeRange()` returns the inverted sentinel
  `{Timepoint::max(), Timepoint::min()}`: a static decoration must neither widen
  the playback timeline (`{0, INT64_MAX}` would balloon it to 2262) nor clamp the
  playhead to its latch timestamp. The dock skips inverted ranges and still
  delivers live tracker time.

## Streaming / live-data path

TF, point clouds and markers ingest live as well as from a file. The application
activates `TransformService`'s session-wide `FrameTransforms` ingest tap before
constructing any runtime host. After scalar parsing, the source host invokes that
tap synchronously with the payload view it already owns. The callback resolves the
topic's current `SessionManager` parser binding, decodes under `parseLocked`, folds
validated edges into the dataset's thread-safe `TransformBuffer`, and returns
before the source payload anchor can expire. This is the normal file and streaming
TF path.

Progressive file-load flushes emit `datasetTransformsReady` so docks re-read frame
hierarchies at the current playhead; the flush does not decode TF. Streaming keeps
the `samplesIngested` connection:

- `Scene3DDockWidget::setSessionManager` shadows the base to also call
  `reconnectLiveSamples`, which connects `SessionManager::samplesIngested` (fired
  on the UI thread after each retention trim, `live == true` only while following
  a live stream; a file load emits `live == false` and is served by
  `datasetTransformsReady`).
- On each live tick, `TransformService::ingestNewTransforms` is a no-op while the
  tap is active. `recomputeOrphanStates(force=false)` observes the buffer revision
  written by the worker.
- `driveVisibleLayersToLiveEdge` then queries each visible layer's `timeRange()`
  for the newest timestamp in the `ObjectStore` and drives `setTrackerTime` on
  every visible layer (the *layer's* range, not the store's per base-topic range,
  so a multi-topic layer like OccupancyGrid + its `_updates` sibling does not
  freeze at its last keyframe).

`TransformService` is a `widgets/`-level wrapper over the Qt-free core
`TransformBuffer`. It owns no parser handle: each tap or rebuild resolves the
topic's binding through `SessionManager::parserBindingForObjectTopic` and decodes
under `parseLocked` (per use, never cached). Streaming uses a finite cache window
(default 10 s); file buffers keep the full history.

An in-place file reload calls `beginReplacingLoad` before its worker starts, so
tap writes route to a private staging buffer while docks retain the active one.
`commitReplacingLoad` atomically swaps the staging pointer into the active slot,
then emits `datasetTransformsReady`; docks rebind their view and TF-backed layers.
`abortReplacingLoad` drops staging, so rollback performs no ObjectStore reads.

`ingestFrameTransformsForDataset` remains an explicit full-store rebuild. Dataset
merge invalidates the affected buffers and calls it for the anchor because merged
entries carry `payload_stamp_shift`, which only the durable store entry records.
This cold closure drain is reserved for explicit rebuilds; per-flush and streaming
paths never call it while the tap is active.

### Object decode: parser-decoded vs canonical blob

Every object consumer (the layers above + `TransformService`) decodes a store
entry through one seam, `pj_scene3d_widgets/resolve_object.h::resolveObject`,
which picks the path by whether the topic has a bound `MessageParser`:

- **Parser bound**: file / streaming data sources whose objects arrive as
  parser-decoded messages (e.g. `parser_ros`, `parser_protobuf`): decode via the
  topic's parser under `parseLocked` (per-use binding, never cached: the
  reload-UAF rule).
- **No parser bound**: a data source or toolbox that pushed an *already
  serialized canonical* object (e.g. the Mosaico cloud toolbox, which packages the
  server's `point_cloud2` / `pose` / `motion_state` ontologies into
  `sdk::PointCloud` / `PosesInFrame`): the bytes are a pj_base wire blob, so
  `resolveObject` deserializes them with the canonical codec selected by the
  topic's `builtin_object_type`. This mirrors pj_scene2D's `canonical =
  (binding.parser == nullptr)` discipline.

Either way the **host** performs the decode, never the producing plugin (the
"canonical-objects-in" boundary). A layer's `attach()` therefore accepts a topic
that has *either* a parser *or* a canonical codec for its `builtin_object_type`
(`hasCanonical3DCodec`); it rejects only a topic that has neither.

### SceneEntities lifetime expiry & the decoded-batch cache

`SceneEntitiesLayer` replays every batch up to the tracker time into an id-keyed
entity map (replace-by-id, deletions, lifetime expiry). Two mechanisms support
this on the live path:

- **Dual-clock lifetime anchor.** A `SceneEntity` carries an embedded `timestamp`
  (sensor epoch) and an optional `lifetime_ns`. Under streaming the ObjectStore
  entry is *host*-stamped (tracker clock), a different epoch. Expiry therefore
  compares against the ingest timestamp, not `entity.timestamp`; otherwise every
  finite-lifetime entity expires the instant it is folded. The layer keeps a
  parallel map `entity_expiry_anchor_ns_` (keyed like `entities_`) holding each
  entity's ingest timestamp; `expiredAt()` tests `anchor + lifetime < tracker`
  (overflow-safe; `lifetime == 0` ⇒ never). The single erase choke-point
  `eraseEntity()` drops the entity and its anchor in lockstep. Deletions carry
  sensor-epoch timestamps, so `deletion.timestamp <= entity.timestamp` stays on
  the entity clock.
- **Decoded-batch snapshot cache.** Backward scrubs and jumps rebuild the entity
  map from scratch; re-parsing heavy embedded glTF every time would hitch the
  scrub. `snapshot_cache_` (keyed by `SequentialUID`, byte-budgeted at
  `kSnapshotCacheMaxBytes`, oldest-UID eviction) holds decoded batches so a
  rebuild re-folds without re-parsing. Each entry records `store_ns` alongside the
  batch, so a cache-hit re-fold restores the same lifetime anchor a fresh parse
  would have produced.
- **Timestamp-ordered, out-of-order-safe fold.** The batches to fold are the
  topic's entries with `ts <= tracker`, taken via `ObjectStore::rangeByTime` (a
  decode-free, ascending, eviction-safe window), never an arrival-order UID walk.
  Forward playback folds only the `(state_built_at_, tracker]` delta; a
  retroactive push landing at `ts <= state_built_at_` is detected when
  `maxUidAtOrBefore(state_built_at_)` grows past the folded high-water
  (`applied_uid_high_`) and forces a full rebuild. `OccupancyGridLayer` uses the
  same cursor discipline, so all three ObjectStore UID-cursor consumers
  (`TransformService`, occupancy, scene entities) share one out-of-order contract.

## Asset resolution (`package://` for a non-ROS app)

`UrdfPackageResolver` dispatches on the URI scheme first: `file://` → absolute
path; a bare path → relative to the URDF's directory (never enters the package
chain); `http(s)://` → allowed only for URL-source layers; `package://pkg/rel` →
the chain below, stopping at the first hit:

0. **In-band embedded-asset lookup**: exact-name match against the embedded asset
   dictionary (`stepEmbeddedAsset`). The dataset source plugin (not the host) is
   what may carry such assets, surfaced format-neutrally via `setEmbeddedAssets`.
   Built and unit-tested, but **not yet connected to the app load path**:
   `MainWindow::onFileLoaded` does not surface the extracted asset map, so this
   step is inert in production. (Steps 1+ are wired: `onFileLoaded` and
   `makeSceneDock` feed `setSourcePath` the loaded source path.)
1. **Remembered per-source mapping**: QSettings
   `pj_scene3d/urdf_per_source_packages`, keyed by the dataset's source path.
2. **Ancestor heuristic**: walk up from the URDF's dir (≤10 levels) looking for a
   directory whose basename == `pkg`, accepting only if `candidate/rel` exists.
   URL sources use the URL-segment variant.
3. **Search roots**: an ordered list auto-seeded from the URDF dir, the source
   file's dir, `$ROS_PACKAGE_PATH`, and `$AMENT_PREFIX_PATH`/`$COLCON_PREFIX_PATH`
   (+`/share`), all via `qEnvironmentVariable`: zero ROS dependency. Package
   identity is the directory basename only; no `package.xml` check.
4. **Ask once**: unresolved packages surface on the layer's status text; a chosen
   root is stored per source and globally, then pending meshes retry.

Failure semantics: never silent. Missing meshes → magenta cubes + status counts;
a wrong folder → an explicit "expected a subdirectory named <pkg>" error; a URDF
latch not yet received → an indefinite 500 ms-throttled re-check with a permanent
"Waiting for …" status.

## Scene-controls panel (pj_app)

`Scene3DConfigPanel` (right side panel) drives scene-wide state, persisted in
QSettings `pj_scene3d/scene_controls/*` and re-applied to every dock it binds:
grid style/size/divisions/visibility (a plain line grid, or filled two-tone cells
with the lines drawn in the cell shader; colours derived from the active theme, no
user colour controls), TF-frame size/opacity/visibility, mesh/collision
opacity/visibility, and the **Model/URDF** row (a File/Topic/URL source combo plus
an add button; each added robot gets a row with a remove button). Robot layers
are panel-managed: filtered out of the Topics list (the per-layer config widget —
frame prefix, colour override, COLLADA up_axis, resolution override — exists on
the layer but is not reachable from the panel). The look knobs (tonemap,
exposure, saturation, SSAO, EDL) are runtime APIs on the view with baked defaults
and no app UI.

## Appendix: Legacy OpenGL renderer (release N only)

`-DPJ_SCENE3D_RHI=OFF` builds the previous renderer for one release, as the
rollback: CMake warns, and `build.sh` / `build.ps1` pass the flag on every
configure. It is **frozen**: no feature or fix lands in it, and PR 2 deletes it
(QRHI_NEXT.md, "PR 2").

- **What it is.** `widgets/src/scene_view_widget.cpp` (a `QOpenGLWidget`,
  OpenGL 4.5 core), the GL passes in `widgets/src/passes/`, the object wrappers in
  `widgets/src/gl/`, and `SceneHdrFbo`. The same dock and layers; each layer keeps
  a GL sink beside its QRhi one (`#ifndef PJ_SCENE3D_RHI` blocks), including the
  zero-copy point upload and the GPU AABB reducer. The plots use
  `QwtPlotOpenGLCanvas`, and every GPU widget requests OpenGL.
- **Behaviour that differs** from the design above: the view does not auto-fit
  the camera, and the dock pushes its scene bounds (`updateSceneBounds`: every
  layer's bounds, hidden ones included); markers cast no shadows and draw in layer
  order; wireframe culls back-facing edges of closed solids; the HDR chain always
  resolves depth; `QOpenGLWidget` recreates its context on every ADS reparent, and
  every pass rebuilds in `initializeGL` (the app never sets
  `AA_ShareOpenGLContexts`).
- **Where it is still tested.** Linux CI's trimmed `build-gl` tree (its goldens
  per pixel, the GL-only runners and tests: the `pj_gl_rollback_tests` target,
  ctest label `gl-rollback`) and Windows CI's non-blocking `gl`
  lane (TECHNICAL_NOTES, "CI lanes"). The golden images come from this renderer
  until PR 2.
