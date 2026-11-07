# Scene3D QRhi migration plan

> **Archived 2026-09-26.** The migration is complete: P0 merged (#670); P1 (#672),
> P2 (#673), P3 (#675), P4 (#680) and P5 (branch `feat/qrhi-p5`) were stacked for
> review when this plan was archived, and Gate A was signed off (see "Gate A
> sign-off" under P5 C). This file is history. The as-built design is
> [`pj_scene3D/docs/ARCHITECTURE.md`](../../pj_scene3D/docs/ARCHITECTURE.md), the facts it
> works around are in [`TECHNICAL_NOTES.md`](../../pj_scene3D/docs/TECHNICAL_NOTES.md), and
> the remaining work (deleting the GL renderer, P6 performance, Qt 6.12) is in
> [`QRHI_NEXT.md`](../../pj_scene3D/docs/QRHI_NEXT.md). File references below
> (`ARCHITECTURE.md`, `widgets/`, `core/`) are relative to `pj_scene3D/`.

Goal: one QRhi-based 3D renderer for every target — Linux, Windows, macOS
arm64 and WebAssembly — replacing both the desktop `QOpenGLWidget` renderer
(`widgets/src/`) and the separate browser renderer (`widgets/rhi/`, named
`widgets/wasm/` until P3).

## Why

- macOS caps OpenGL at 4.1 core (no compute, no debug output). QRhi's OpenGL
  backend on a 4.1 context (`PJ_GRAPHICS_API=opengl`) is the fallback there. The
  legacy GL renderer needs 4.5 and never ran on macOS. Metal is the native path.
- Windows ships software GL 1.1 without a driver; D3D11 (and WARP in CI) is
  always present.
- Two renderers, two layer sets (7 WASM layers, ~6.2k lines, vs 9 desktop
  layers, ~7.4k lines) and two docks (1.4k vs 2.0k lines) drift apart: GridMap and Trail never reached the browser.

## Constraints found in the code

1. **One graphics API per top-level window.** Qt allows a single QRhi backend
   per widget window. Today every GPU widget is OpenGL on purpose:
   - the `rhi_bootstrap` zero-size `MediaViewerWidget` (`pj_app/src/MainWindow.cpp`)
     and `scene2dRhiBootstrap` (`Scene2DDockWidget.cpp`) fix each window to GL;
   - every `QRhiWidget` requests `PJ::preferredGraphicsApi()`: OpenGL in the GL
     build, the native API in the QRhi build (P4 stream D);
   - desktop plots use `QwtPlotOpenGLCanvas` (`PlotWidgetBase.cpp`) in the GL
     build; since P4 the QRhi build (`PJ_RHI_WIDGETS`) uses `PlotRhiCanvas`.
   Scene3D can therefore only leave GL together with Scene2D **and the plots**.
   Floating ADS docks are new top-levels and follow the same rule.
2. **`PlotRhiCanvas` rendered only the Qwt items it knew** (curves, grid) and
   applied browser sample budgets. Resolved in P4 (see "P4 stream A (plot
   canvas): results"): every other item is rasterised with its own QPainter
   drawing, desktop budgets, panner grab. Patching Qwt is allowed if a gap needs it, but conda
   links the system `qwt` (`PJ_SYSTEM_QWT=ON`), so a patch means shipping our
   own qwt package there — avoid unless required.
3. **Desktop layers are GL-free**; GL lives in `passes/` (~5.7k lines) and
   `scene_view_widget.cpp`. The `gl/` wrappers cover object lifetime only
   (attribute setup, state and draws are raw calls in the passes), so passes are
   rewritten, not re-targeted.
4. **The WASM renderer has the right ownership model but the wrong shape**:
   layers are CPU-only data providers (`PointRenderable`,
   `ModelRenderable`), the renderer owns all GPU state — but as one
   4.1k-line widget (`initialize()` 570 lines, `render()` 1730, ~45 pipeline
   members).
5. **The WASM renderer assumed GL conventions** (`depth*2-1`, `*0.5+0.5`
   shadow lookups, an un-flipped fullscreen triangle, raw GLES calls). Resolved
   in P0: see the "GL NDC / Y conventions" decision.
6. **Browser budgets** (`core/*_budget.h`, 256 MiB HDR per view) would turn off
   HDR/SSAO/EDL at 4K on desktop and truncate large clouds. Resolved in P1:
   `core/scene_limits.h` holds one `SceneLimits` policy, browser values under
   `PJ_TARGET_WASM`, desktop values otherwise, statically checked against the
   renderer's quint32 buffer and int counter bounds.
7. **Shader tooling**: desktop Qt kits install no `qsb`, so packs stay
   committed and are verified only in the WASM configure. P0 baked the Scene3D
   packs for every backend (`rhi/shaders/bake.sh`) and dropped the separate
   WASM Qt pin: WASM and desktop both use `PJ_QT_VERSION`.
8. QRhi (`<rhi/qrhi.h>`) has no cross-minor compatibility guarantee: every
   channel (including conda `qt6-main`) stays pinned to 6.11.*.

## Target architecture

- **One layer set.** The desktop layers (the richer ones) become the only
  layers and expose renderable data interfaces; `*_layer_wasm.cpp` go away.
- **One dock** with a per-platform `SceneLimits` policy instead of hard-coded
  browser caps.
- **One renderer split into passes** (grid/lines, points, poses, occupancy,
  voxels, markers, mesh, shadow, post chain); each pass owns its pipelines and
  SRBs.
- **One graphics-API policy** (`pj_widgets/GraphicsApi.h`) used by Scene3D,
  Scene2D, the bootstrap widgets and the plot canvas.

| Platform / channel | Backend | CI software renderer |
|---|---|---|
| Linux (AppImage, .deb, conda) | OpenGL (Vulkan opt-in) | llvmpipe; lavapipe for Vulkan |
| Windows (installer) | D3D11 | WARP |
| macOS arm64 (conda, later dmg) | Metal | the runner's paravirtual Metal device |
| WebAssembly | OpenGL / WebGL2 | Playwright (existing) |

## Technical decisions

| Problem | Decision |
|---|---|
| `gl_PointSize` absent on D3D | Instanced camera-facing quads on every backend; a point list where `QRhi::VertexShaderPointSize` exists and a cloud's sprites stay at most 4 px (quads measured ~2x slower for many small points, P3 stream A) |
| Non-float scalar vertex fields | CPU float conversion into the shared `PointVertex` array. Integer inputs (zero-copy) are deferred: P5 P measured the conversion, net of the CPU bounds scan a zero-copy path needs, at 70–81% of decode+upload CPU (about 170–185 ms per 10M sample), above the 10% trigger, but the path needs a vertex layout and shader variant per wire datatype, NaN rejection in the shader and a full golden re-verification. Converting straight into `PointVertex` (no `DecodedPointCloud`) comes first |
| MSAA depth resolve | `QRhi::ResolveDepthStencil` where QRhi reports it and multisample textures (desktop OpenGL, in D32FS8; Vulkan; Metal); the single-sample depth/mask replay only where it is missing (D3D11, WebGL) |
| Mesh-mask MRT | WASM's separate mask pass |
| GL NDC / Y conventions | Folded into CPU matrices: `clipToTextureMatrix()` (`rhi_conventions.h`) maps corrected clip space to render-target texture space (u, v, stored depth), so shaders never remap. The fullscreen triangle has a second `-DPJ_FLIP_Y` bake for D3D/Metal (kept over `gl_FragCoord` UVs, which assume input and output textures of equal size — false once `render_scale` lands). SSAO normals take `textureVSign()` |
| Compute AABB | CPU bounds on every backend, from the conversion, or a scan of the retained vertices once per geometry revision for a renderable without them. The compute reducer (and its compute bake profile) was deleted in P5 |
| Wireframe markers | Edge lines everywhere (already built; `NonFillPolygonMode` would split WebGL from desktop for a cosmetic gain) |
| QPainter HUD / hover label | Upload the existing CPU-rasterised `QImage` as a texture quad |
| Plot items without a geometry translation | Rasterise the item's own QPainter `draw()` into the canvas text atlas, in its Z slot (`PlotMarkersItem`, legend, labels, unknown items); no Qwt patch |
| Plot canvas choice (QRhi build) | `PlotRhiCanvas` when "GPU canvas" is on, raster `QwtPlotCanvas` otherwise; never `QwtPlotOpenGLCanvas` (it would pin the window to OpenGL) |
| Plot canvas rendering | 1 sample per pixel into a colour-only target (no depth-stencil, 4 B/px) with shader edge AA over one device pixel; axis-aligned lines and symbols snap to whole pixels like Qwt; GPU dashes (path distance per vertex; cosmetic pens cut per pixel, wide ones capped and antialiased); indexed geometry layers (shared corners, the same triangles) |
| Marker and grid instances | Markers: a per-layer Static cache keyed on the geometry revision, render origin, style and the resolved TF placement of every referenced frame; the render origin stays on the CPU (a float shader add of camera-relative doubles is not pixel-identical). Grid: built once at the fixed-frame origin, the origin offset added in the shader (exact, the operands are floats) |
| GPU timing | `QRhi::EnableTimestamps` + `lastCompletedGpuTime` (frame-level only) |
| Colormaps | Desktop polynomials from `pj_widgets` (one source with the plots) |
| Shader packs | Checked-in `.qsb`, baked `--glsl "300 es,330,440" --hlsl 50 --msl 12` (+ SPIR-V); hash-verified in one pinned CI lane. GLSL 330 keeps a macOS GL 4.1 fallback possible |
| Diagnostics | Log the backend + adapter each QRhi widget actually got (`pj.graphics`; `PlotRhiCanvas` through its own ready line); `PJ_GRAPHICS_API` env override |

## Phases

Each phase is its own PR and leaves the app shippable.

**P0 — Baseline (1–2 weeks).** No user-visible change.
- `pj_widgets` graphics-API policy (GL everywhere for now) used by every
  `QRhiWidget`; actual backend logged.
- WASM shaders made backend-neutral via texture-space matrices (no-op on
  WebGL); GL-only calls removed from the browser renderer.
- Scene3D packs baked for all backends; one Qt pin (`PJ_QT_VERSION`) for WASM
  and desktop.
- Required-GPU CI mode: GL tests fail instead of skipping where a GPU lane is
  declared.
- Exit: WASM Playwright suites green; desktop unchanged.

**P1 — QRhi renderer on native Linux (≈2 weeks).**
- `PJ_SCENE3D_RHI` option, a PUBLIC define independent from `PJ_TARGET_WASM`
  (the two renderers share class names, so a build picks one). It selects the
  renderer only; browser policy (file picking, URDF local-file restore,
  shader-pack verification tooling) stays on `PJ_TARGET_WASM`.
- `SceneLimits` (`core/scene_limits.h`): a constexpr per-platform policy covering
  the layers' admission checks, the renderer budgets, URDF limits and the HDR
  byte caps (desktop: 1 GiB per view, 4 GiB per QRhi), so desktop neither
  truncates clouds nor drops HDR at 4K. Done.
- Real 3D-texture limit on Vulkan (`QRhiVulkanNativeHandles` →
  `maxImageDimension3D`); D3D/Metal alongside their CI lanes in P4 (done, see P4).
- Headless harness: `QRhiWidget::grabFramebuffer()` under a real X server
  (Xvfb in CI; not the `offscreen` QPA, which has no RHI support) on OpenGL
  (llvmpipe) and Vulkan (lavapipe), asserting the actual backend. Pixel-region
  checks on scenes the RHI renderer already supports (orientation, occlusion,
  transparency, shadows, HDR/SSAO/EDL active) plus a resize/reparent/sibling
  lifecycle test. The cross-build screenshot diff against the GL renderer moves
  to P3, once Trail/GridMap exist in the RHI renderer.
- Exit: a Linux CI job builds `PJ_SCENE3D_RHI=ON` and passes the harness on
  llvmpipe and lavapipe with `PJ_REQUIRE_GPU_TESTS=1`.

**P2 — One layer set, one dock (2–3 weeks).**
The desktop layers and dock become the only ones; the WASM forks are deleted.
Each layer shares decode, `SceneLimits` admission and worker scheduling across
renderers; only its sink differs (GL build: its pass, byte-identical to today;
RHI build: retained CPU data the RHI view reads). Steps, each keeping the GL,
RHI-desktop and WASM builds green:
- A. Foundation: the point/model renderable interfaces leave `wasm/` (no `Wasm`
  prefix); shared enums/data leave the GL pass headers; `Scene3DLayer` compiles
  in both builds (GL hooks guarded); the RHI view gains
  `setLayers(std::vector<Scene3DLayer*>)` holding today's six-way dispatch; the
  shared CPU/persistence tests also run in the RHI build. Done.
- B. Point + depth clouds; C. poses, occupancy, voxels (CPU preparation moved
  out of GL `render()` so it runs in both builds). B and C in parallel.
  B done: `PointCloudLayer`/`DepthCloudLayer` are the only point layers; the
  retained `PointVertex` sink and off-thread depth projection are QRhi-only,
  the GL sink is unchanged. C done: the three layers
  prepare their CPU data outside `render()` in both builds.
- D. Scene entities + robot model, with an explicit coordinate contract
  (link-local geometry + frame id, no double TF). Done: `SceneEntitiesLayer` and
  `RobotModelLayer` are the only implementations. Shared in both builds: sample
  selection, payload admission before parsing, model-entity replay
  (`SceneEntitiesModelState`), bounded fetch/import scheduling
  (`maxConcurrentMeshLoads()`: one in the browser) with generation
  invalidation, and the model byte budgets; the GL build keeps its passes, the
  QRhi build exposes `MarkerGeometry` (expanded on a worker, `marker_geometry.h`)
  and frame-local `ModelDrawCall`s. The browser keeps URDF-from-bytes with the
  "select again" restore (`PJ_TARGET_WASM`); both native builds load URDF paths.
- E. One dock + settings panel (browser input branches stay on
  `PJ_TARGET_WASM`); acceptance probes migrated; forks deleted. The RHI view
  marks the layers it does not draw (Trail/GridMap) "not rendered by this
  renderer yet" until P3.
  Done: `src/Scene3DDockWidget.cpp` is the only dock. The views share one
  `setLayers(std::vector<Scene3DLayer*>)` seam. One warning channel in both
  builds: `Scene3DLayer::statusWarning()` + `statusWarningChanged`, combined by
  the dock with the frame-orphan state. Bounds: the GL view gets them from the
  dock, the QRhi view owns them (its per-frame union + one-shot fit per new
  layer). The dock's live-edge drive (`driveVisibleLayersToLiveEdge`) now runs
  for both renderers. Browser-only in the dock: `addRobotModelLayerFromContent`
  and the compressed-cloud gate (`PJ_WASM_WITH_COMPRESSED_POINTCLOUDS`);
  QRhi-only: the `renderingWarningChanged` overlay. A dock with only local
  robots gets a private TF buffer in both builds. Trail/GridMap layers build in
  the RHI and WASM targets (with `trail_sampling`/`grid_map_view` in the WASM
  core) and persist, but are not drawn; the "Create Trail" row and the frame
  right-click menu stay GL-only. The dock/persistence/XML/row-spacing tests run
  in both native builds.
- **Admission rule.** Per-sample admission — payload size, dimension and count
  limits from `kSceneLimits`, applied before any decode, plus the mesh importer
  and model-fetch limits — runs in both builds (the desktop limits are large, so
  the GL build rarely hits them) and fails closed with a row warning. Only the
  per-view budgets (`prepareForRender(remaining_*)`, `BudgetLatch`) are
  QRhi-only: the QRhi view is their only caller. Each layer's
  `prepareForRender` drains its pending update first, then checks what it would
  draw against the remaining budget, so a single check covers both new data
  and a budget shrunk by earlier layers.
- Shared scaffolding from the `/simplify` pass: `LatestWinsWorker` (point,
  depth, marker and trail workers), `MeshImport` + `maxConcurrentMeshLoads()`,
  `BudgetLatch`, `Scene3DLayer`'s data/renderer warning slots,
  `latestEntryRenderKey`. Both builds drain a pending tracker move at the
  layer's own `requested_time_` (GL `render()` and QRhi `prepareForRender()`
  share one drain), and the `renderKey`s (sample identity ⊕ the fixed←source
  transform, `foldFrameTransformIntoKey`, so a static sample in a moving frame
  still repaints) coalesce repaints in the GL build too — except the GL pose
  layer while its embedded trail is on (its past/future split moves every tick).
- Browser behaviour to preserve: worker decode with latest-wins and generation
  invalidation, decode limits applied before decompression, failed-sample
  memoization, budget recovery, URDF-from-bytes with "select again" restore,
  pose-trail round-trip without warnings.
- Exit: all five Playwright Scene3D files, desktop persistence/layer tests in
  both native builds, 28/28 GL goldens, RHI harness, and RHI goldens no worse
  than P1's 22/28.

Deferred from P2:
- P3: one owner for scene bounds + camera fit; rename `widgets/wasm/` to
  `widgets/rhi/` (it is the desktop QRhi view too). Both done in S0.
- P5 (performance), as resolved there:
  - drop the RHI CPU copies of data already uploaded: deferred as step B (see
    P5), blocked by the ADS reparent rebuild;
  - convert clouds straight to `PointVertex` (no intermediate
    `DecodedPointCloud`): the direct converter. Gate A's cloud-stream row decides
    it (see "P5 C (cut-over): results"); no owner otherwise;
  - share primitive meshes (URDF box/cylinder/sphere) across layers: deferred
    until more than 32 MiB is duplicated across repeated robots, measured with
    B's layer cases; P5 ships S's per-layer identity diff.

**P3 — Passes + parity (3–4 weeks).**
Sequence (agreed with the pre-phase Codex check and an architecture map):
- S0 (serial foundation): rename `widgets/wasm/` → `widgets/rhi/`; a pipeline
  registry replacing the hard-coded HDR/depth-replay clone lists; view-owned
  shared geometry (cube mesh, pose/arrow mesh); the frame contract — prepare
  and admit once per frame, then reuse the prepared draws for direct, HDR,
  depth replay, mesh mask and shadow; opaque/translucent draw classification
  (opaque first, stable logical order within each phase — not order-independent
  transparency); one scene-bounds + camera-fit policy; extract occupancy as the
  pattern proof. Gate: per-scene golden metrics unchanged on OpenGL and Vulkan,
  harness 12/12, Playwright 45/45, `last_*_count_` test counters unchanged.
  Done (ARCHITECTURE.md → "WebAssembly product backend" has the as-built
  design): `widgets/rhi/` with the qrc prefix `:/scene3d_rhi/`;
  `rhi_scene_pass.h` (`ScenePass`, `DrawClass`, `ScenePipelineRegistry`,
  `RhiSharedGeometry`, `SceneBoundsCollector`, the frame-contract comment);
  `render()` split into `admitFrame` → `applySceneBounds` → `prepareFrame` →
  `submitShadowPass` / `submitScenePass` / `submitEdlMeshMask` over one
  `PreparedFrame`; a logical-order `scene_draws` list submitted opaque phase
  then translucent phase (replacing the per-family cursor walk);
  `CameraFitPolicy` (core) applied by the QRhi view, with the dock's
  `noteCameraRestored()` — the QRhi view no longer refits over a restored
  camera; the GL view, kept as the release-N fallback behind
  `-DPJ_SCENE3D_RHI=OFF`, keeps its no-auto-fit behaviour; PR 2 deletes it;
  `rhi/passes/occupancy_rhi_pass.{h,cpp}` as the pattern. Golden images and
  harness images pixel-identical to the pre-S0 build on OpenGL and Vulkan;
  counters identical. New harness case: a translucent layer first in the dock
  still blends over the opaque one behind it (fails without the phase order).
- Then four parallel streams on the pass structure:
  - points: hexagon fan, sub-pixel sprite LOD, colour hoist, two-partition
    faded out-of-range draw, spheres below ~2 px as flat discs (restore
    `shape_spheres_small`'s full check), instanced quads with a 10M-point
    benchmark gate (keep a gated point fast path only if measured), compute
    AABB gated on `QRhi::Compute` (compute shaders get their own bake profile;
    the reducer and that profile were deleted in P5).
    Done (stream A, ARCHITECTURE.md → "Point family in the QRhi renderer"):
    `rhi/passes/point_rhi_pass.{h,cpp}` + `point_aabb_reducer.{h,cpp}`; goldens
    28/28 on OpenGL and Vulkan (were 22 and 20), registered in ctest per backend
    and in the Linux CI QRhi step; GL goldens 28/28 with one re-captured golden
    (`shape_spheres_small`: the GL pass draws sub-2-px spheres as flat discs too);
    the GL point tests re-expressed as `scene3d_rhi_point_test` (7 cases per
    backend). Details and numbers below;
  - voxels + poses (and TF triads) extraction;
  - markers + models extraction, SceneEntities cube shadows;
    done: `rhi/passes/marker_rhi_pass` (own instance + stream buffers and
    pipelines) and `rhi/passes/model_rhi_pass` (meshes, materials, EDL mask
    replay, caster pipeline); the view keeps the shadow target and orchestrates
    the shadow pass and EDL mask. Pixel-identical on OpenGL and Vulkan before the
    new casters; solid markers (cube, sphere, cylinder, arrow) then cast
    (`marker_shadow.vert`), harness `SceneEntitiesCubeCastsShadow`;
  - grid/axes extraction, axis corner gizmo (then drop the golden test's
    `kGizmoRect` exclusion), HUD + hover label as texture quads, `render_scale`,
    AgX/Neutral tonemaps, and the frame-picking primitive (one owner).
- Then GridMap + Trail (standalone + pose-embedded), "Create Trail" row and the
  frame right-click menu (asynchronous popup; no `QMenu::exec()` in WASM).
  Done: `rhi/passes/gridmap_rhi_pass` (procedural heightfield, two R32F
  textures, the GL colormap polynomials via `colormap.glslinc`) and `rhi/passes/trail_rhi_pass` (the GL
  ribbon; a pose-embedded trail draws in its pose layer's slot). The pose
  layer owns its trail in every build (the verbatim `<trail>` XML fallback is
  gone), the "Create Trail" row is in every build, and the dock shows the frame
  menu with `popup()` in both builds. The browser trail assertion now expects
  the restored pose trail to draw (`trails=` counters in the data probe).
- Integration: the RHI goldens enabled in ctest (28/28 on OpenGL and Vulkan);
  the two browser assertions that encode P2 gaps (shadow draws == model draws,
  ignored trails) updated deliberately.
- `/simplify` wave 2 on the pass structure: one family contract
  (`initialize(RhiPassSetup)`, `registerPipelines`, `setLayers`, `admit`,
  `prepare(RhiFrameContext, ...)`, `drawsFor` spans with a per-draw
  `draw_class`, `submit`, `release`), shared pipeline/binding/layer-GPU helpers in
  `rhi_scene_pass.h`, lazy HDR/depth-replay clones, the colormap LUT owned by the
  voxel pass and the cube fan in `RhiSharedGeometry`, prelude geometry rebuilt
  only when its inputs change. Goldens, harness images and counters identical on
  OpenGL and Vulkan (ARCHITECTURE.md → "Frame contract").
- Zero-copy integer scalar uploads stay in P5: the integer-field goldens already
  pass through the CPU conversion; removing the copy is a layer/API performance
  change.
- Exit: all 28 point-cloud goldens pass on the RHI renderer (P1 baseline 22/28:
  `shape_cubes_subpixel`, `color_rgb_cubes`, `field_viridis_cubes`,
  `depth_two_clouds` — missing hexagon fan / LOD / colour hoist;
  `field_outside_range_translucent`, `field_outside_range_cubes` — missing
  two-partition draw); GL pass tests re-expressed as QRhi offscreen tests;
  WASM Playwright green.

**P4 — Plots and backend switch (2–4 weeks).**
- `PlotRhiCanvas` on desktop: multi-backend shaders, item audit, desktop
  budgets, performance vs `QwtPlotOpenGLCanvas` on large curves; raster canvas
  stays as fallback. Done (stream A): see "P4 stream A (plot canvas): results".
- Flip Windows to D3D11, macOS to Metal; floating-dock matrix (float/redock,
  split, sibling destruction, mixed-DPI moves). Done (stream D): see "P4 stream
  D (API policy, floating docks): results".
- Offscreen orientation: decided to keep the golden test's 3x3 neighbourhood
  rule with its per-pixel ceiling (no renderer-wide GL-orientation rewrite) and
  add asymmetric orientation/depth checks instead. Done (stream C): harness cases
  `OrientationTextureMarkedCornerStaysTopLeft` (a texture with a marked corner on
  an upright quad), `OrientationShadowFallsAwayFromTheLight` (an asymmetric key
  light; the shadow centroid within 2.5% of where the light ray through the
  caster meets the ground) and `OrientationSsaoAndEdlStayWithTheirContent`
  (content only top-left; SSAO and EDL darkening must stay in that quadrant and
  AO must hug the crease). Checked by mutation on OpenGL: a v-mirrored
  shadow-map lookup moves the shadow centroid 22 px (fails), a flipped SSAO
  normal sign darkens every flat face (fails).
- Depth-replay skip generalised. Done (stream C): `ScenePipelineRegistry::resolve()`
  returns nullptr for the depth replay of any source pipeline with
  `!hasDepthWrite()` (translucent markers and models, occupancy, translucent grid
  maps, faded points); the point pass's special case is gone and
  `lastSubmittedLayerIdsForTest()` counts the colour pass. Trails and voxels
  write depth in both renderers, so they are unaffected. Goldens: pixel-identical
  to P3 on OpenGL and Vulkan (28/28, same metrics; the faded half already
  skipped). Harness: only the glass cube of `OcclusionAndTranslucency` /
  `TranslucentLayerFirstStillBlendsOverOpaque` changed (OpenGL 3634 px, Vulkan
  3630 px, max channel delta 36/33): where the glass tints only the background
  it is now ungraded (more saturated green), as in GL; the SSAO/EDL scene
  (`QualityStagesActiveAndMeshCastsShadow`) and every counter are identical.
  New case `NoDepthWriteDrawLeavesBackgroundUngraded` (fails without the rule).
- Real 3D-texture limits on D3D and Metal. Done (stream C): `max3dTextureSize()`
  reads the D3D11 device's feature level from `QRhiD3D11NativeHandles`
  (`d3d11Max3dTextureSize()`: 2048 from 10_0, 256 on 9_x; unit-tested); D3D12
  and Metal use the documented 2048 (D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION with
  QRhi's feature-level-11_0 minimum; Metal feature set tables, every Apple and
  Mac family). Verified: Qt 6.11.1's `qrhid3d11.cpp` fills `featureLevel` from
  the created device. Documented only, not measured: the D3D11/D3D12 constants
  and the Metal value (no Windows/macOS run). Where the backend cannot be asked
  (stream X): a Vulkan build without `<vulkan/vulkan.h>` (the renderer's own
  guard) or OpenGL without a current context now gets the guaranteed floor, 256
  (Vulkan's required `maxImageDimension3D`, the GL 3.3 / GLES 3.0
  `MAX_3D_TEXTURE_SIZE` minimum), logged once, instead of 0, which refused every
  voxel grid. OpenGL already
  queried `GL_MAX_3D_TEXTURE_SIZE` with the widget's current context and only
  reported 0 without one. The desktop QRhi configure warns when Qt has Vulkan but
  the headers are missing (no `WrapVulkanHeaders::WrapVulkanHeaders` target).
- QRhi widget failure state. Done (stream X): a pipeline, SRB, buffer or
  texture whose `create()` fails is never drawn with (D3D11 crashed in
  `beginPass` on the WARP lane: a failed `initialize()` released everything, then
  `render()` uploaded into the released uniform buffers). Every `create()` in
  `widgets/rhi/` is classified (ARCHITECTURE.md → "Failure state and fallbacks"):
  a failed base set (uniforms, shared meshes, any scene family) puts the view in
  a failure state that clears to the backdrop, shows the reason in the dock's
  warning label, emits `renderFailed()` once and retries on the next
  `initialize()`; the overlays became optional, and a failed HDR or depth-replay
  clone, which used to silently drop its draw from every HDR frame, now drops the
  HDR chain with a warning. Test builds inject failures by shader
  (`PJ_SCENE3D_TEST_FAIL_SHADER`); `scene3d_rhi_failure_test` covers the failure
  state (a sibling `QRhiWidget` in the same window keeps rendering; clearing the
  fault and resizing recovers, also after a reparent to a new top-level), Qt's
  own "No QRhi" failure reaching the warning, the overlay fallback and the
  HDR-clone fallback, on each backend of the platform. With the fix reverted the first two crash and the third
  fails its assertions. D3D11 itself was not run locally.
- Backend CI lanes. Done (stream B): every QRhi display runner
  (`pj_add_test_runner(... RHI_BACKENDS)`, `cmake/PjTests.cmake`, which lists the
  backends per platform) registers one pass per backend of the platform,
  `<runner>.<backend>.*` with the ctest label `qrhi-<backend>`, plus
  `.opengl_dpr2.*` (`QT_SCALE_FACTOR=2`) for the plot, dock and Scene3D
  presentation runners, and joins the `pj_qrhi_tests` build target. The runners
  are `scene3d_rhi_render_test`, `scene3d_rhi_point_test`,
  `scene3d_rhi_failure_test`, `pointcloud_golden_test`, `plot_rhi_render_test`,
  `rhi_dock_lifecycle_test` and `rhi_mixed_dock_test` (P5 F adds four). `windows-ci.yml`'s
  blocking `qrhi-d3d11` lane builds `pj_qrhi_tests` and runs `-L qrhi-d3d11`
  with the `windows` QPA, `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` and
  `PJ_REQUIRE_GPU_TESTS=1`, and fails unless every view logs `D3D11` on a `Cpu`
  (WARP) adapter (`scripts/ci/check_qrhi_backend.sh`). `macos-ci.yml` is the
  Metal lane, on PRs like the Linux and Windows lanes, on GitHub-hosted
  `macos-14` (Apple silicon): a Metal device and presentation probe, the whole
  tree built as a QRhi build, `-L qrhi-metal` and then `-L qrhi-opengl` (the
  Metal-to-OpenGL fallback path) with the `cocoa` QPA and
  `PJ_REQUIRE_GPU_TESTS=1`, each failing unless every view logs its pass's
  backend, then the unit tests without the display runners (`-LE qrhi`). Until
  `macos-ci.yml` reaches main it also runs on pushes to `feat/qrhi-**`.
- Exit: required backend CI lanes green on all three desktop OSes.

**P5 — Performance and cut-over (1–2 weeks).**
- Frame-time/memory budgets met; RHI default; legacy GL renderer kept one
  release behind the option, then `gl/`, GL passes and `QOpenGLWidget` deleted.
- Package smoke tests: AppImage, .deb, IFW, conda/RoboStack, browsers.
- Foundation F (done): `PJ_RHI_WIDGETS` is a PUBLIC definition of `pj_widgets`
  (every GPU-widget module, `pj_app` and their test runners link it) instead of
  a global one, so modules below it compile identically in both builds. The
  stream runners are registered ahead of their streams, each a placeholder its
  stream fills: `scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test`
  and `scene3d_rhi_platform_test` (RHI_BACKENDS), and `scene3d_frame_benchmark`
  in both builds (RHI_BACKENDS in the QRhi build, a plain display runner in the
  GL build). `pj_app/tests/mixed_layout_fixture.{h,cpp}` builds the plot +
  Scene3D + media-viewer dock layout through public APIs in either build;
  `rhi_mixed_dock_test` and the `mixed_layout_benchmark` demo use it. The Scene3D
  views expose `framesPresentedForTest()` (both builds) and
  `markerPassForTest()`/`modelPassForTest()` (QRhi). CI: the Linux QRhi step
  configures with demos and compiles `pointcloud_rhi_benchmark`,
  `plot_canvas_benchmark` and `mixed_layout_benchmark`;
  `check_qrhi_backend.sh` also requires a plot canvas on `D3D11` (Windows) and
  `Metal` (macOS) from the `PJ_WASM_PLOT_RHI_READY backend=` lines; `wasm-ci.yml`
  also triggers on `pj_plotting/**`. The goldens stay GL-made until the GL
  renderer is deleted.
- Render origin: M's marker cache and S's instance skip, both keyed on the
  render origin plus the resolved TF placements, cover orbit, zoom and idle.
  Pan, follow and TF-animated frames still rebuild. Moving the origin into the
  view uniform waits until B's pan/follow case exceeds 1 ms or 10% of frame CPU,
  and it must keep the double-precision subtraction (M found a float shader add
  of camera-relative doubles not pixel-identical).
- Indexing: `drawsFor` (a linear search per logical slot, O(L²) in the layer
  count) and `ScenePipelineRegistry::resolve` (a scan per draw) wait until B's
  100/1000-layer case exceeds 1 ms or 10% of `prepareFrame` CPU (B: 1000 layers
  cost 4.3 ms GUI CPU in all, under the GL build's 8.5). A future finger index
  needs a `draws[hint-1].layer != layer` check.
- Cut-over (C, done; "P5 C (cut-over): results"): `PJ_SCENE3D_RHI` defaults to
  ON and configuring it OFF warns; `build.sh` and `build.ps1` pass it on every
  configure (a cached OFF would otherwise survive). Linux CI's main tree is the
  QRhi build and runs the whole suite under xvfb, `.vulkan` passes included, plus
  the GL-floor step; a trimmed `build-gl` tree keeps the GL build compiling and
  runs its GL-only tests and per-pixel goldens. Windows CI's `build` lane is the
  QRhi build with the D3D11/WARP display step and backend greps; its `gl` lane
  compiles the GL build and runs its non-GPU tests. `PlotRhiCanvas` names QRhi
  OpenGL `OpenGL` on desktop (`OpenGLES2/WebGL` stays the browser's label).
- Deferred, each with its trigger:
  - **Step B: drop the retained CPU copy** of uploaded clouds and meshes. Blocked
    by the ADS reparent finding: every tab switch or float calls `initialize()`
    and rebuilds from the retained CPU data. It can proceed only with a
    re-derive-from-datastore path whose cost fits the reparent budget row, and
    only if RSS fails Gate A.
  - **conda/RoboStack smoke.** No PJ4 conda package is built here (`pixi.toml`
    lists only `linux-64`, `recipes/` holds dependency recipes only). P5 does the
    pixi QRhi compile proof and the packager release note; the pixi GUI-test open
    question stays open. macOS arm64 ships through no channel today; the Conan
    `macos-ci` lane is its only gate.
  - **Browser smoke.** WASM is already QRhi, and the Chromium Playwright suites
    are the smoke. Firefox and Safari wait for cross-browser CI.
  - **ADS hiding floating docks on Wayland** (Findings). It affects every backend,
    so it is not a flip regression; the fix belongs in the ADS fork, and filing
    it there is outward-facing.
  - **Branch `feat/scene3d-gl41` is not merged.** It ports the legacy renderer to
    GL 4.1, which ships for one more release only; no macOS package ships from
    this repo; QRhi's OpenGL backend on 4.1 is the macOS fallback. Deleting the
    remote branch is the maintainer's call.
  - **Vulkan 50 x 100k plot gap.** Indexed geometry left it 9% slower than the P4
    base in wall at 13% less CPU; the time is outside `render()`. The final Gate A
    run has it at 102.6 ms against the Qwt GL canvas's 59.3 (1.73x). Vulkan is
    opt-in on Linux; analyse it if Vulkan becomes a default anywhere.

Total: **~11–17 weeks**; plot parity is the least certain part.

### P3 stream A (points): results

Golden metrics (differing-pixel fraction with the neighbourhood rule / plain
per-pixel, llvmpipe and lavapipe, Mesa 26.0.3): OpenGL 28/28, worst
`shape_cubes_subpixel` 0.0122 / 0.0348; Vulkan 28/28, worst `field_viridis_cubes`
0.0197 / 0.0648 and `color_rgb_cubes` 0.0161 / 0.0619. Before: OpenGL 22/28,
Vulkan 20/28. What closed them: the fan with centroid local coordinates and the
LOD (cube scenes), the colour hoist, and the faded half's two-partition draw
that also skips the depth replay (see ARCHITECTURE.md). Why Vulkan had two
extra failures (`shape_cubes_large`, `shape_points_1px`) and still differs per
pixel: lavapipe's framebuffer is y-down where llvmpipe's GL render target is
y-up, so the same 4x MSAA sample pattern lands mirrored on the scene. Diagonal
silhouettes, the cube edge bands and 1 px points resolve differently (up to 8%
of pixels) while interiors are identical: horizontal-edge-only content (8 px
field points) differs on 0.4%, no integer pixel shift explains the rest. In the QRhi
build the golden comparison now ignores a pixel that matches any golden pixel
in its 3x3 neighbourhood (README of the corpus), with a per-pixel ceiling of
max(3 x 3%, 10%) so the allowance cannot hide a gross regression; the GL build
keeps the plain per-pixel check (0.0000 on llvmpipe). Rendering every offscreen target in GL orientation on all
backends would remove the difference at the source; that is a renderer-wide
convention change (winding, texture-space matrices, present flip) for the
integration step, not a points change.

Point-size benchmark (`demos/pointcloud_rhi_benchmark`, PJ_BUILD_DEMOS in the
QRhi build): 10M 2 px points at 1920x1080, the colour pass into 4x RGBA16F + the
single-sample depth replay (the SSAO/EDL full-screen passes cost the same for
every topology), median GPU time from QRhi timestamps:

| GPU / API | quads | point list | quads, cloud re-uploaded each frame | point list, re-uploaded |
|---|---|---|---|---|
| RTX 4070 Laptop, Vulkan | 14.8 ms | 6.8 ms | 36.9 ms | 29.3 ms |
| RTX 4070 Laptop, OpenGL | 13.6 ms | 7.6 ms | 40.5 ms | 93.2 ms |
| Iris Xe (RPL-P), Vulkan | 90.9 ms | 49.2 ms | 96.4 ms | 63.3 ms |
| Iris Xe (RPL-P), OpenGL | 82.8 ms | 44.2 ms | 82.2 ms | 81.0 ms |
| llvmpipe, OpenGL | 9016 ms | 7806 ms | 8957 ms | 5752 ms |
| lavapipe, Vulkan | 18087 ms | 13951 ms | 15475 ms | 11763 ms |

Quads cost 1.8-2.2x the point list on the GPUs (1.2-1.6x on the software
rasterizers), above the 1.5x gate, so small sprites
(at most 4 px, where the per-vertex cost dominates) keep a point-list path on
backends with `QRhi::VertexShaderPointSize`; larger sprites stay quads (fill
bound either way, and they keep their visible part at the frustum edge). CPU:
`prepare()` is ~0.01 ms for a static cloud; a changed 10M cloud spends
100-130 ms copying its 191 MiB into the update batch (the layer's conversion
comes on top, P5). GPU memory (Vulkan, VMA): 479 MiB with the static cloud and
the 1080p targets, 670 MiB while re-uploading (staging). The software rows ran on
a machine loaded by three parallel builds (1.2-1.6x there).

### P4 stream A (plot canvas): results

`PlotRhiCanvas` (pj_plotting) is compiled whenever `PJ_SCENE3D_RHI` is on and on
WASM; in those builds (`PJ_RHI_WIDGETS`) `PlotWidgetBase` picks it when the
"GPU canvas" preference (`Preferences::use_opengl`, label renamed, objectName
unchanged) is on and `--disable-opengl` is absent, else the raster
`QwtPlotCanvas` — never `QwtPlotOpenGLCanvas`. The GL build is unchanged. Moved
from `PJ_TARGET_WASM` to `PJ_RHI_WIDGETS`: the canvas choice, the legend
alignment replot, `PlotPanner::grab()`. Lines and Dots is `PlotCurve`'s dot pass
in every build (the WASM ellipse symbol is gone): the canvas draws the same
square dots. The `pj_app` WASM probes stay browser-only.

- **Backends.** Committed packs (`pj_plotting/widget/shaders/bake.sh`: GLSL ES
  300, GLSL 120/150/330/440, HLSL 50, MSL 12, SPIR-V), re-baked and hash-checked
  in the WASM configure like Scene2D/3D (desktop kits have no `qsb`, so
  `qt_add_shaders` is gone). The vertex shaders pass positions through;
  `clipSpaceCorrMatrix()` is folded into the CPU transform. Found on Vulkan: the
  canvas set its viewport before binding the pipeline, which draws nothing
  there (fine on GL); it is now set per pipeline bind.
- **Items.** Geometry: curves (Lines, Sticks, Steps, Dots, Lines and Dots), grid,
  `QwtPlotMarker` lines and symbols; every QwtSymbol outline except Star2 and the
  path/pixmap/graphic/SVG/user styles (those draw as a square and log once).
  Raster texture layers in their Z slot: legend, marker labels,
  `PlotMarkersItem` and any unknown `QwtPlotItem` (its own `draw()`). Logged once
  and approximated: `UserCurve` (symbols only), curve area brushes, `Fitted`.
  Not drawn: the 1-px canvas frame of the Qwt canvases. Zoom rubber band,
  panner drag image and focus overlays are child widgets and composite normally.
  PJ4 has no plot image export or print path; the panner grab is the only one.
- **Budgets** (device pixels): 4 extrema per device-pixel column per curve on
  desktop (the browser keeps one budget split across curves), 8M-vertex frame
  cap on desktop, 1M in the browser; vertices shrank from 24 to 12 B (UNormByte4
  colour). Desktop maps each curve once into a reused scratch, reduces it with a
  non-type-erased reducer, and drops samples outside the visible x range of a
  finite, x-sorted series with a solid pen (dashes keep the whole series for
  phase).
- **Lifecycle.** Pipelines rebuild on a new QRhi, render-pass descriptor
  (address or `serializedFormat()`) or sample count, re-checked in `render()`.
  Geometry is retained in Static buffers and rebuilt only on replot/view change.
  `grabPresentedFrame()` for tests; the panner keeps the synchronous
  `grabFramebuffer()` (the canvas binds no uniforms, so the grab-only
  stale-uniform issue does not apply).
- **Failure state** (later P4 pass). A colour-pipeline or vertex-buffer
  `create()` failure leaves clear-only passes, the banner and one
  `renderFailed()` per onset; the pipelines are retried in the next
  `initialize()` only, never per frame. The text pipeline is optional: without
  it legends and labels drop with a one-time warning and the curves still draw
  (the browser has no raster fallback). The canvas never drew with a failed resource (every create is
  committed only on success), so the plot had no D3D11 `beginPass` crash to
  fix. On desktop `PlotWidgetBase` swaps a failed canvas to the raster
  `QwtPlotCanvas` (magnifier, panners, zoomer and event filters moved over in
  order), waiting while a popup or modal widget is open, since a context menu's
  nested loop may still own an event of the canvas the swap deletes; the
  browser keeps the banner. Tests (`pjTestFailPipeline` and
  `pjTestPipelineAttempts` hooks, `PJ_RHI_TEST_HOOKS`): raster fallback
  with zoom/pan and a sibling that keeps rendering, banner + recovery on a bare
  canvas with no pipeline retry per frame, a text-only failure that keeps the
  curves, the swap deferred under an open popup, and the "No QRhi" case (an
  OpenGL widget pins the window). The render cases fail, not skip, when a canvas
  initialized a backend and then failed.
- **Approximations audit.** None of the logged approximations (other symbol
  styles, `UserCurve`, curve brushes, `Fitted`) is reachable in PJ4: every
  curve style and layout-XML `style` sets no symbol, brush, `UserCurve` or
  `Fitted`; all marker symbols are ellipses; the SDK has no curve-style API.
  They keep their one-time log.
- **Tests.** `plot_rhi_render_test` compares the presented frame with Qwt's own
  `drawCanvas()` raster per item kind, plus reparent/resize/sample-count change,
  the panner grab and the three failure cases: 12/12 on OpenGL, Vulkan and
  OpenGL at DPR 2 (llvmpipe and lavapipe; the first nine also passed on the
  Iris Xe Vulkan driver). Mutation checks: without
  the clip correction 8/9 fail on Vulkan, and the marker case fails without the
  raster fallback. Linux CI runs it in the QRhi step.

Benchmark (`plot_canvas_benchmark`, 1600x900, every frame replots and waits for
the canvas to finish; medians of 30 frames, 10 on llvmpipe; ms). CPU is the
canvas's own frame work (`PlotRhiCanvas::render()` vs `QwtPlotOpenGLCanvas::paintGL`),
wall is replot to finished frame. GPU time: GL timer query / QRhi timestamps
where the driver reports them (Iris Xe OpenGL only; the rest report none).
The machine was shared with other builds (load 11-40), so treat differences
under ~20% as noise.

| Case | Iris Xe: Qwt GL cpu / wall | Iris Xe: RHI GL cpu / wall | Iris Xe: RHI Vulkan cpu / wall | RTX 4070: Qwt GL cpu / wall | RTX 4070: RHI GL cpu / wall | RTX 4070: RHI Vulkan cpu / wall | llvmpipe: Qwt GL cpu / wall | llvmpipe: RHI GL cpu / wall |
|---|---|---|---|---|---|---|---|---|
| static 1M | 15.3 / 17.0 | 13.2 / 16.2 | 16.0 / 18.9 | 10.0 / 10.5 | 8.4 / 8.8 | 12.4 / 13.5 | 13.9 / 21.8 | 10.1 / 32.0 |
| static 10M | 189 / 191 | 100 / 102 | 86 / 89 | 144 / 146 | 80 / 81 | 117 / 118 | 143 / 148 | 82 / 102 |
| streaming 10x100k, 100 Hz | 17.3 / 19.9 | 6.9 / 13.2 | 6.9 / 12.7 | 17.2 / 18.8 | 6.2 / 8.2 | 11.0 / 15.0 | 25.5 / 33.4 | 7.2 / 42.8 |
| zoom/pan 1M | 10.1 / 12.6 | 6.0 / 9.3 | 6.1 / 11.9 | 10.6 / 11.9 | 5.6 / 7.0 | 8.4 / 14.8 | 12.6 / 18.2 | 5.8 / 22.3 |
| 50 curves x 100k | 57 / 68 | 57 / 110 | 63 / 117 | 58 / 58 | 54 / 59 | 131 / 136 | 106 / 112 | 72 / 423 |
| dashed 5x100k | 12.0 / 15.1 | 26.7 / 31.0 | 28.1 / 32.1 | 12.4 / 12.9 | 28.4 / 30.8 | 39.7 / 41.6 | 48.5 / 51.8 | 26.5 / 70.9 |

The RTX 4070 Vulkan column (device picked with `QT_VK_PHYSICAL_DEVICE_INDEX=1`)
comes from a later paired run at higher load, where the Qwt GL canvas measured
18.8 / 19.5, 232 / 234, 27.0 / 29.4, 20.6 / 24.8, 86 / 87 and 26.9 / 27.8: the
same pattern, with 50 curves at 1.5x and dashes at 1.5x Qwt's CPU there.

GPU time (Iris Xe, OpenGL): Qwt 0.8 / RHI 1.3 ms at static 1M, 9.8 / 17.5 ms
for 50 curves, 2.1 / 2.4 ms dashed; llvmpipe 74 / 336 ms for 50 curves.

Before the stream's optimisations the RHI canvas cost 2-7x Qwt's CPU (static 1M
34 vs 24 ms, 50 curves 504 vs 112 ms, dashed 180 vs 25 ms on the Iris Xe): one
curve was scanned through a `std::function` + virtual `sample()` + two scale
maps per reducer pass, and every vertex converted its QColor and went through
`QTransform::map`. Fixed in this stream: map once into a scratch, template the
reducer over the accessor, visible-range culling of sorted series, a packed
12 B vertex, colour conversion per colour change, and one direction per
polyline segment shared by its quad and join.

Where the RHI canvas is still slower:
- **Dashed lines, ~2.2x CPU.** Dashes are expanded on the CPU into one quad per
  dash (826k vertices for 5 dense curves). Proposed fix: carry the path
  distance per vertex and the dash pattern per draw, and discard in the
  fragment shader — dashes then cost nothing on the CPU. Not contained (vertex
  format, both shaders, the WASM dash goldens and budget telemetry), so left
  for P5.
- **Many dense curves, wall/GPU.** 50 full-density curves emit 2.15M vertices
  (9 per retained sample: a quad per segment plus a bevel join) into a 4x MSAA
  target; CPU matches Qwt on OpenGL (1.5x in the RTX Vulkan run) but the upload and fill double the wall time on the
  Iris Xe and dominate on llvmpipe (336 ms GPU). Proposed fix: an indexed strip
  per polyline (4 shared vertices per sample, joins from the strip) and, when a
  plot holds many curves, fewer extrema per column (min/max only for x-sorted
  series; needs a visual check against the M4 output).
- Vulkan wall times round up to the 60 Hz present of the headless compositor;
  CPU times are the comparable figure there.

Where it is faster: 10M static (1.8-1.9x), streaming (2.5-3.5x CPU) and zoom/pan
(1.7-1.9x) — the culling skips the off-screen part of the series — and a replot-free
repaint (expose, window moves, overlay widgets) rebuilds nothing, where the GL
canvas repaints every item.

### P4 stream D (API policy, floating docks): results

**Policy.** `PJ::preferredGraphicsApi()` (`pj_widgets/GraphicsApi.h`) returns
`nativeGraphicsApi(currentGraphicsPlatform(), PJ_RHI_WIDGETS)`: in the QRhi
build D3D11 on Windows, Metal on macOS, OpenGL on Linux and WebGL2; OpenGL in the
GL build. `PJ_GRAPHICS_API` still overrides it. The policy is a constexpr
function of the platform, so `graphics_api_test` checks every platform's answer
from Linux. The choice is logged once (`pj.graphics`), and each widget still
logs the backend and adapter it got.

**Startup and fallback.** pj_app resolves the API right after constructing
`QApplication`, before any GPU widget exists; a top-level picks its API only
once. From the Qt 6.11.1 sources (`qbackingstorerhisupport.cpp`, `qrhiwidget.cpp`):
- D3D11: when the default device fails, the backing store retries with
  `QRhi::PreferSoftwareRenderer`, i.e. WARP. PJ adds no fallback: WARP beats the
  GL 1.1 software driver Windows ships. This answers the open question: QRhi
  D3D selects WARP on GPU-less machines by itself.
- Metal: the backing store probes Metal and, when the probe fails (a macOS VM
  without a Metal device), gives up with only a debug message. Despite its
  comment, it does not fall back to OpenGL, so every `QRhiWidget` would log
  "No QRhi" and stay blank. `preferredGraphicsApi()` therefore runs the same
  `QRhi::probe(Metal)` itself and picks OpenGL when it fails. This is not
  verified on a Mac (the code is compiled only under `QT_CONFIG(metal)`).
- Vulkan (only through `PJ_GRAPHICS_API`) is not probed. If it fails, the widgets
  report "No QRhi".

**Floating-dock matrix** (`pj_app/tests/rhi_dock_lifecycle_test.cpp`, QRhi
build). Stand-in `QRhiWidget`s clear to a known colour and hold one buffer per
QRhi; the real Scene2D `MediaViewerWidget` checks the image orientation and an
annotation overlay. Each case asserts: a presented frame whose grab has the
right colour; `releaseResources()` before every QRhi switch; nothing held after
destruction; and no Qt warning about the API or a missing QRhi. Results, run
under the private gnome-shell compositor (Wayland) and a rootful Xwayland (xcb),
on llvmpipe (OpenGL) and lavapipe (Vulkan), plus an OpenGL pass at
`QT_SCALE_FACTOR=2`:

| Case | OpenGL | Vulkan |
|---|---|---|
| float to its own top-level, redock | pass | pass |
| split, move into another split | pass | pass |
| hidden tab, shown again | pass | pass |
| destroy a sibling (docked and floating) | pass | pass |
| close a floating window, reopen the dock | pass | pass |
| float, then move to a screen with another DPR | pass | pass |
| Scene2D media viewer: orientation and overlay across float, redock, tab switch | pass | pass |
| negative: non-GL `QRhiWidget` beside a `QOpenGLWidget` | as documented | as documented |

The DPR move needs two screens with different ratios, so it skips on a single
screen. It passed 12/12 on gnome-shell with two `--virtual-monitor`s and
`QT_SCREEN_SCALE_FACTORS="1;2"`: `QWindow::setScreen()` moved the floating
window, and the widget re-rendered at the new pixel size. The negative case
(OpenGL widget added first, then a Vulkan or Null `QRhiWidget`) does not crash:
the window composites with OpenGL, and the second widget logs "The top-level
window is already using another graphics API" and "No QRhi", emits
`renderFailed()`, and never initializes, while the GL widget keeps painting.

Fixes made in this stream (each one fails the media-viewer case when reverted):
- `MediaViewerWidget` sent GL clip space to its shaders, so Vulkan drew images
  upside down. It now uploads `clipSpaceCorrMatrix() * view` and keeps the GL
  matrix for scissors. D3D11 and Metal share GL's y-up clip space, so only depth
  changed for them.
- `MediaViewerWidget::releaseResources()` now marks the overlays dirty. Otherwise
  a viewer reparented at an unchanged size (an ADS tab switch) lost its
  annotations until new data arrived.
- pj_app sets `Qt::AA_DontCreateNativeWidgetSiblings` in the QRhi build; see the
  Findings.

### P4 stream Z (Scene2D failure state, real-widget docks): results

- **Scene2D failure state.** `MediaViewerWidget` never draws with a resource
  whose `create()` failed. Without its base pipeline set it clears to the
  background, emits `renderFailed()` once, shows a warning label in
  `Scene2DDockWidget`, and retries on every later `initialize()`. Without the
  blending pipeline its layers draw opaque; a failed overlay or frame texture is
  dropped, each logged once per QRhi, and the next `initialize()` rebuilds a
  dropped overlay or blending pipeline. See `pj_scene2D/docs/TECHNICAL_NOTES.md`
  (GPU failure state).
- **Real widgets in one dock manager** (`pj_app/tests/rhi_mixed_dock_test.cpp`).
  A `PlotWidget`, a Scene3D point cloud and a Scene2D image run through the
  floating-dock matrix. Each widget must show its own content at its pixel size.
  No Qt QRhi warning is allowed, and no resource may be left on a QRhi when it is
  destroyed (`QT_RHI_LEAK_CHECK`). That leak check works on Vulkan/D3D11/Metal;
  on OpenGL, uniform buffers are CPU-side and are not tracked, so each widget
  also keeps lifecycle counters in test builds (`pjTestRhiAdoptions`,
  `pjTestRhiReleases`, `pjTestRhiStale`): every float and redock must
  re-initialize the moved widget (a closed floating dock reopens in its hidden
  container, on the same QRhi), a shown widget holds resources on exactly one
  QRhi, and none meets a new QRhi before releasing the old one's. The media
  viewer's failure injection (`pjTestFailPipeline`, test builds only) runs
  beside the other two. Both dock runners register like every QRhi display
  runner (`RHI_BACKENDS`, see "Backend CI lanes" under P4).
  Passing on llvmpipe/lavapipe under gnome-shell, DPR move included (two
  virtual monitors). Its 2 s presentation waits can time out when several
  display tests share one virtual monitor and occlude each other, so run it
  serially (CI runs ctest serially under Xvfb).
- **`--screenshot`** exits 1 when there is no 3D view, or when the capture is
  null, a single colour, or cannot be saved. It prints
  `[screenshot] graphics backend: <QRhi backend>`.

### P4 final review: results

- **Shared QRhi widget plumbing** (`pj_widgets/GraphicsApi.h`): the plot
  canvas, the media viewer and the Scene3D view share `grabPresentedFrame()`,
  `noteRhiAdoption()`/`noteRhiRelease()` (the backend log, once per QRhi, and
  the `pjTestRhi*` counters), `RhiTargetKey` (what the pipelines were built
  for), `RhiFailureLatch` and `createRhiPipeline()`/`rhiTestFault()` (the
  `pjTestFailPipeline` injection). One `PJ_RHI_TEST_HOOKS` definition (root
  CMake, test-enabled desktop builds) replaces the three per-module macros; the
  Scene3D shader-suffix injection (`PJ_SCENE3D_TEST_FAIL_SHADER`) stays.
- **Media viewer upload batch.** A frame layer's uploads go to their own
  resource-update batch, merged only when the layer survived: a failed SRB
  rebuild used to delete textures an already queued upload (a depth LUT, the
  frame planes) still referenced, and `render()` submitted it.
- **macOS OpenGL fallback.** pj_app and the gui test main request a 4.1 core
  default format on macOS in the QRhi build (macOS otherwise gives a 2.1 legacy
  context, below every Scene3D/media pack's GLSL), and the Scene2D image packs
  gained GLSL 330. `macos-ci.yml` runs the `.opengl` passes too, as a
  functional smoke run; Metal is the macOS parity gate. The runner has no GL
  driver for its paravirtual GPU, so its GL is Apple's software renderer
  ("Apple Software Renderer 4.1"): 10-40x slower than Metal (a float/redock
  case takes 40-60 s+ instead of 1-2 s), hence 5x the ctest timeout for every
  macOS `.opengl` case (`cmake/PjTests.cmake`). The step leaves out what
  measures that renderer rather than ours: the point-cloud goldens (its
  sphere impostors come out ~10% larger and its point sprites soft-edged, so
  coverage moves by up to 19% and mean colour by up to 58), the sub-pixel
  sprite colour check (1 px sprites resolve to ~15% coverage) and the DPR 2
  mixed-dock pass (every float recompiles the pipeline set at 4x the pixels;
  Linux runs it). Qt creates the QRhi of a never-shown `QRhiWidget` grab with
  a default-constructed `QSurfaceFormat`, which on macOS is a legacy 2.1
  context, so such a grab cannot render Scene3D there (no GLSL 330);
  `BackendMatchesRequestAndImageSize` skips its warning check on macOS
  OpenGL. The app never grabs a view that was not shown.
- **Plot memory.** A curve maps only its visible index range (x-sorted, solid
  pen) into the canvas's scratch, and a scratch grown past 1 MiB is released
  after the rebuild; `plot_rhi_render_test` checks it with a 1 M-sample series.
- **The DPR 2 passes ran at DPR 1.** gtest_discover_tests splits a `;` inside
  a property value, so `ENVIRONMENT "PJ_GRAPHICS_API=opengl;QT_SCALE_FACTOR=2"`
  silently dropped the scale factor. `QT_SCALE_FACTOR` now goes through
  `ENVIRONMENT_MODIFICATION`, and the labels are one value each (`qrhi-<backend>`;
  `-L qrhi` matches every pass, since ctest labels are regular expressions).
- **Plot parity colours.** The plot comparisons check the foreground colours
  (union of both images' foreground, 3x3 neighbourhood, 48 per channel), missing
  and extra geometry, and an exactly-clear background, with an overlapping
  translucent fixture; a red/blue swap in the vertex colour fails 11 of 16 cases.
  They found the text layers (legend, marker labels, other rasterised items)
  placed against the whole canvas where Qwt uses its contents rect: one logical
  pixel off, now fixed. Qwt snaps curve, marker and grid coordinates to whole
  pixels on screen (`QwtPainter::roundingAlignment()`) and draws Rect/Cross/line
  symbols aliased; the canvas does neither, so its thin lines can sit half a pixel
  off Qwt's canvas and the comparisons use an unsnapped reference. Snapping in the
  canvas made the colours match Qwt's snapped rendering exactly, but a 1.4 px
  axis-aligned line then covers no pixel fully, which fails the browser's
  curve-style probe (exact-colour pixels) and would need its snapshots
  re-baselined: left for a decision.
- **Golden foreground metrics.** `pointcloud_golden_test` adds a differing
  fraction relative to the foreground (the union of both images' non-background
  pixels) and an absolute count of stray pixels away from the golden's
  foreground; see the corpus README.

### P5 P: results

What changed:
- **Shared upload.** `PointRenderable` exposes `vertexBytes()`, one implicitly
  shared `QByteArray` of `PointVertex` records, and `pointCount()`, in place of
  `vertices()`. `PointRhiPass` hands the array to `uploadStaticBuffer()` by
  value, so the update batch shares it instead of copying it; only the backend's
  own staging copy remains. Each sample builds a fresh array, so no write ever
  detaches one that is being uploaded: `packFinitePointVertices()` for raw and
  decompressed clouds, the back-projection worker for depth clouds. The retained
  CPU copy stays, because a QRhi teardown (ADS float or redock) re-uploads from it.
- **The GPU AABB reducer is deleted:** `point_aabb_reducer.{h,cpp}`,
  `point_aabb.comp(.qsb)`, its qrc entry and its `bake.sh` compute loop, the
  view's compute hook, the `StorageBuffer` usage on every instance buffer, and the
  "silent optional" failure row. Both layers already get bounds from the
  conversion, and nothing ever reported empty bounds with points. A
  `PointRenderable` without bounds now gets a CPU scan of its retained vertices,
  once per geometry revision, in the frame its sample arrives.
  `CloudWithoutConversionBoundsIsScannedAndReuploadsAfterRelease` replaces the GPU
  reduction test. It checks the exact finite extent per sample, and that the cloud
  uploads again after `release()`. The remaining 35 packs re-bake byte-identical.
- **`pointcloud_rhi_benchmark`:**
  - `--replay on|off` and `--field-type u16|u32|f32`;
  - the CPU split into prepare, enqueue and endFrame (wall, plus endFrame's
    own-thread CPU);
  - a third mode, *streaming*: a new wire cloud every frame, converted the
    layer's way, with columns for convert, the x/y/z bounds scan, and
    decode-to-visible latency (conversion start to frame complete);
  - peak RSS.

  *changing* still re-uploads the same array, as in P3, so that row compares
  before and after.

Pixel checks: images were dumped from this worktree before the edits and after
them (`PJ_RHI_TEST_DUMP_DIR`, `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1`), on the same
Mesa 26.0.3, llvmpipe and lavapipe.
- **Goldens:** 56/56 images identical on OpenGL and 56/56 on Vulkan.
- **Harness:** 85/86 images identical per backend, and `counters.txt` is identical.
- **The one harness difference:** `PresentationHoverLabelAndPerfHud_hud.png`
  differs in one 7x9 px digit of the live timing text. The same image differs
  between two runs of the unchanged F build too.

Tests:
- `scene3d_rhi_point_test` 10/10, harness 24/24 and goldens 28/28, on both
  backends;
- GL build: `pj_scene3d_widget*_tests` 336/336 and GL goldens 28/28.

Benchmark: 10M 2 px points at 1920x1080, Iris Xe (RPL-P), Mesa 26.0.3.
- The base binary ran 5 runs and the new one 8 (3 of them from the field-type
  sweep), interleaved.
- Each cell is the median in ms, with min–max for the gate columns.
- The frame-to-frame noise of the GPU column on this laptop iGPU is about 20%.

| Row | prepare before → after | frame before → after | GPU before → after |
|---|---|---|---|
| Vulkan, changing, quads | 86.2 (70.9–87.5) → 0.02 | 193.5 → 122.4 | 85.2 → 78.8 |
| Vulkan, changing, point list | 86.8 (85.3–87.5) → 0.02 | 181.9 → 91.0 | 66.8 → 49.5 |
| OpenGL, changing, quads | 66.5 (65.4–67.3) → 0.02 | 208.5 → 130.0 | 77.3 → 74.2 |
| OpenGL, changing, point list | 66.2 (66.0–67.2) → 0.02 | 182.2 → 101.7 | 55.9 → 45.1 |
| static rows (4) | 0.02 → 0.02 | within 0.3 ms | within 0.3 ms |

- **Where the remaining copy runs:** the backend's copy of the cloud remains.
  Vulkan copies into staging when the batch is recorded (enqueue 41–44 ms), and
  OpenGL's `glBufferSubData` runs in endFrame (own-thread CPU 56–58 ms).
- **GPU memory is unchanged:** 480 MiB static, 670.7 MiB while re-uploading
  (Vulkan).
- **No row regresses.**

*Streaming, f32:*
- The conversion takes 205 ms (Vulkan run) / 195 ms (OpenGL run). A one-off
  split puts `convertCanonical()` at about 82 ms and the pack into a fresh
  `PointVertex` array at about 115 ms, which is dominated by first-touch page
  faults of the new 191 MiB array.
- Decode-to-visible latency:

  | Backend | quads | point list |
  |---|---|---|
  | Vulkan | 322 ms | 304 ms |
  | OpenGL | 339 ms | 335 ms |

*Replay off* (static, one run): the single-sample depth replay is 40–45% of the
static GPU time.

| Backend | quads, on → off | point list, on → off |
|---|---|---|
| Vulkan | 70.9 → 39.3 ms | 40.2 → 28.1 ms |
| OpenGL | 69.1 → 37.9 ms | 39.3 → 26.8 ms |

This bears on B's depth-resolve fast-path decision.

Integer zero-copy analysis (numbers only; nothing shipped):
- *Streaming quads rows*, the median of 3 runs per field type (f32: 8).
- *Decode+upload CPU* is convert + prepare + enqueue + endFrame own-thread CPU.
- *Peak RSS* is the process's peak.

| x/y/z | API | convert | bounds scan | decode+upload CPU | (convert − scan) share | peak RSS |
|---|---|---|---|---|---|---|
| u16 | Vulkan | 206.7 | 22.2 | 228.4 | 81% | 768 MiB |
| u32 | Vulkan | 205.5 | 26.5 | 227.1 | 79% | 806 MiB |
| f32 | Vulkan | 204.5 | 27.8 | 226.1 | 78% | 806 MiB |
| u16 | OpenGL | 195.0 | 22.2 | 234.6 | 74% | 993 MiB |
| u32 | OpenGL | 195.8 | 26.5 | 235.4 | 72% | 1031 MiB |
| f32 | OpenGL | 194.8 | 30.5 | 234.3 | 70% | 1030 MiB |

- **What a zero-copy upload would save:** 70–81% of the decode+upload CPU, about
  170–185 ms per 10M sample.
- **Memory:** the transient decoded arrays (153 MiB) and the 191 MiB `PointVertex`
  array would go. The retained copy would become the wire blob: 114 MiB (u16) or
  153 MiB (u32/f32) instead of 191 MiB.
- **Why it is not shipped here:**
  - it needs a vertex-input layout and a shader variant per wire datatype;
  - NaN rejection would move into the shader;
  - it needs a scalar and packed-colour attribute per field type;
  - every pack would be re-baked;
  - all of that is outside P's scope and cannot keep the goldens byte-stable
    without a full re-verification.
- **A cheaper step first:** converting straight into the `PointVertex` array.
  That skips the `DecodedPointCloud` intermediate and its 153 MiB, and needs no
  shader or format change.
- **Note:** the integer rows' GPU times are not comparable with f32 (an integer
  lattice under a scaled camera rasterizes about 20x slower). The benchmark says so.

Rows for the integrator:
- decision table, "Compute AABB": deleted in P5; bounds come from the conversion,
  with a CPU scan for a layer without them;
- `:244`: the compute bake profile is gone.

### P5 B: baseline

**Tools.**
- `scene3d_frame_benchmark` (`pj_scene3D/widgets/tests/`, both builds; per
  backend in the QRhi build). One gtest case per workload, each printing one
  `S3D_BENCH` line: median/min/max wall per frame (request to
  `frameSubmitted`/`frameSwapped`, frames back to back), GUI-thread CPU per
  frame, `VmRSS` and its peak (`clear_refs` 5, Linux), GPU memory (the DRM
  driver's per-process `drm-total-*` from `/proc/self/fdinfo`, any API; the
  QRhi allocator's used bytes on Vulkan), idle frames of the settled view over
  1 s (asserted 0 in the QRhi build) and show-to-first-frame. Cases: 10M cloud
  under an orbiting camera, a 300k cloud per frame, 100k cubes + 1M line
  vertices orbiting, 10k sphere markers under pan + follow, a 50-link URDF
  whose every link TF moves each frame, 10/100/1000 point layers,
  float/redock/tab-switch of a dock holding the 10M cloud and of one holding
  the robot (float = reparent to a new top-level, tab = hide/show: what ADS
  does), first show of cloud + robot, and (QRhi only) the depth replay on vs
  off. Smoke sizes by default (the ctest entries); `PJ_S3D_BENCH_FULL=1` for the
  sizes above, `PJ_S3D_BENCH_<NAME>` per size, `PJ_S3D_BENCH_DUMP_DIR` saves
  each case's last frame. Probes live in `tests/frame_benchmark_probes.h`.
- Depth-replay knob: with `PJ_RHI_TEST_HOOKS`, `PJ_SCENE3D_TEST_SKIP_DEPTH_REPLAY`
  makes `submitScenePass(kDepthReplay)` a clear-only pass (7 lines in
  `scene_view_widget_rhi.cpp`); the output is then wrong, only the cost counts.
- `mixed_layout_benchmark` (`pj_app/benchmarks/`, both builds): the F8 fixture
  with `--plots 6` plots of 4 x 100k and one Scene3D view (1M-point cube
  cloud). Each frame moves every plot's tracker and orbits the camera, then
  waits for all seven widgets; `MIXED_BENCH` line with the same metrics.
- `plot_canvas_benchmark`: `tracker_only` case (10 x 100k static, a
  `CurveTracker` moved each frame, as `PlotWidget::setTrackerPosition`),
  `--msaa N` for the RHI canvas, and `gpu_drm_mib` per run (`QT_SCALE_FACTOR=2`
  for DPR 2).

**Method.** F tip `0ef7de358` plus the tools above; a GL tree (`build/`) and a
QRhi tree (`build-rhi/`), RelWithDebInfo. Intel Iris Xe (RPL-P), Mesa 26.0.3,
i915; kernel 7.0.0-34, AC power, `powersave` governor (intel_pstate). The
RTX 4070 could not be measured: this kernel has no NVIDIA module. xcb on a
private rootful Xwayland (glamor on the Iris) inside a headless GNOME Shell,
vsync off (`vblank_mode=0`, `MESA_VK_WSI_PRESENT_MODE=immediate`); on the
Wayland QPA every frame waits for the compositor's 60 Hz callback, which
hides everything under 16.7 ms. 5 rounds, the configurations interleaved
within each round, one process per case; each run is the median of 30 frames
after 3 warm-up frames. Tables give the median of the 5 runs, with
[min–max] where the spread matters. Load average 1.1–1.9, the build slot
held throughout. Linux's native QRhi backend is OpenGL (`RHI GL`); Vulkan
(`RHI VK`) is shown too.

Scene3D, Iris Xe, 1200x700 logical at DPR 1 (wall ms / GUI CPU ms / RSS MiB):

| Case | GL build | RHI GL | RHI VK |
|---|---|---|---|
| Cloud 10M, orbit | 19.6 / 2.0 / 371 | 29.5 / 1.5 / 448 | 31.7 / 1.6 / 442 |
| Cloud stream 300k/frame | 5.9 [2.2–6.6] / 4.6 / 276 | 8.0 [4.8–8.0] / 5.6 / 313 | 20.0 / 10.6 / 292 |
| 100k cubes + 1M line vertices, orbit | 33.2 / 32.6 / 301 | 45.1 / 45.2 / 505 | 95.0 / 63.1 / 505 |
| 10k markers, pan + follow | 12.2 / 6.6 / 161 | 13.1 / 7.1 / 164 | 32.0 / 13.6 / 157 |
| 50-link robot, TF each frame | 2.9 / 1.3 / 143 | 1.4 / 0.8 / 146 | 4.6 / 1.8 / 139 |
| 10 layers | 3.1 / 1.2 / 150 | 1.8 / 0.5 / 144 | 3.5 / 1.2 / 138 |
| 100 layers | 3.5 / 2.1 / 229 | 2.8 / 1.0 / 146 | 4.1 / 1.5 / 144 |
| 1000 layers | 8.8 / 8.5 / 1017 | 8.1 / 4.3 / 169 | 18.2 / 8.6 / 199 |
| Idle frames, every case | 0 | 0 | 0 |

| Moves to first frame (ms) | GL build | RHI GL | RHI VK |
|---|---|---|---|
| 10M cloud: float / redock / tab | 95 / 86 / 1.2 | 226 / 211 / 10.8 | 252 / 213 / 77 |
| 50-link robot: float / redock / tab | 36 / 27 / 1.2 | 86 / 61 / 2.2 | 46 / 17 / 5.6 |
| First show, 10M cloud + robot | 158 | 326 (pipelines 32) | 343 |

Depth replay, 10M cloud, wall ms (replay on / clear-only replay): RHI GL
30.8 / 19.5, RHI VK 33.0 / 21.5; the GL build is 19.6. llvmpipe (1M cloud):
552 / 279 on OpenGL (GL build 303), 525 / 268 on lavapipe.

Memory (MiB). 10M cloud at DPR 1: peak RSS GL 373, RHI GL 634, RHI VK 628;
GPU (DRM total) GL 241, RHI GL 310, RHI VK 583, of which the Vulkan
allocator holds 443. At DPR 2 (2400x1400 device pixels): GL 581, RHI GL
647, RHI VK 1000 (allocator 623). One plot, 1600x900, GPU (DRM total):

| Canvas | DPR 1 | DPR 2 |
|---|---|---|
| Qwt GL | 62 | 174 |
| RHI GL, 4x / 2x / 1x | 147 / 103 / 63 | 341 / 361 / 177 |
| RHI VK, 4x / 2x / 1x | 166 / 185 / 114 | 489 / 564 / 346 |

(2x above 4x at DPR 2 is what i915 reported, reproducibly within the run;
not investigated.)

Plots, Iris Xe, 1600x900 (wall / canvas CPU / GPU ms; GPU from timestamps on
OpenGL only):

| Case | Qwt GL | RHI GL | RHI VK |
|---|---|---|---|
| tracker only, 10 x 100k | 17.2 / 12.5 / 3.7 | 26.7 / 15.3 / 10.0 | 27.4 / 15.9 / – |
| 50 x 100k | 76.9 / 59.3 / 16.5 | 87.5 / 65.5 / 19.0 | 90.3 / 67.8 / – |
| 50 x 100k at 2x / 1x MSAA | – | 84.8 / 64.7 / 18.6, 80.7 / 62.6 / 15.4 | 87.4, 85.7 |
| dashed 5 x 100k | 17.9 / 14.0 / 2.7 | 35.4 / 31.1 / 2.5 | 93.2 / 31.8 / – |
| streaming 10 x 100k | 19.7 / 16.5 / 0.6 | 12.9 / 6.7 / 3.0 | 13.5 / 6.9 / – |
| static 1M | 12.5 / 10.5 / 0.9 | 12.9 / 10.4 / 1.6 | 13.3 / 10.5 / – |

Mixed layout (6 plots + Scene3D, 1260x780): GL build 35.3 ms wall / 34.5 ms
CPU / 264 MiB; RHI GL 52.6 / 37.2 / 283; RHI VK 115.7 / 51.8 / 266; 0 idle
frames. GPU at DPR 2: GL 295, RHI GL 365, RHI VK 445 MiB.

llvmpipe/lavapipe reference (1M cloud, 100k cubes, 10k markers, 50 links;
10 frames; wall ms, GL build / RHI GL / RHI VK): cloud 303 / 556 / 518,
stream 121 / 199 / 203, cubes + lines 391 / 496 / 482, markers 630 / 1088 /
1093, robot 63 / 58 / 51; 1M-cloud dock float / redock / tab 371 / 344 / 3.1
vs 643 / 557 / 512 (RHI GL). The software rasterizers run the frame on the
GUI thread for OpenGL and on worker threads for Vulkan, so only wall compares.

**Gate A today** (plan section 8: RHI ≤ max(1.2x GL, GL + 1 ms) in wall and
GUI CPU, RSS ≤ 1.10x GL; Iris Xe; the RTX half is still to be run):

| Row | RHI GL | RHI VK | Verdict |
|---|---|---|---|
| Cloud 10M static | wall 1.51x, CPU ok, RSS 1.21x | wall 1.62x, RSS 1.19x | fail (wall, RSS) |
| Cloud 10M GPU ≤ 550 MiB | DRM 310 | allocator 443 | pass |
| Cloud stream 300k | wall 8.0 vs limit 7.1, CPU 5.6 = limit, RSS 1.13x | wall 3.4x, CPU 2.3x | fail |
| Cubes + lines orbit | 1.36x wall and CPU, RSS 1.68x | 2.9x wall | fail |
| Markers pan + follow | pass | wall 2.6x, CPU 2.1x | pass on the native backend |
| TF-animated robot | pass (faster) | wall 4.6 vs 3.9 | pass on the native backend |
| 10 / 100 / 1000 layers | pass | 1000: wall 2.1x | pass on the native backend |
| Reparent/float, 10M cloud | 2.4x float, 2.5x redock | 2.7x, 2.5x | fail |
| Idle frames when static | 0 | 0 | pass |
| Plots: dashed 5 x 100k | 1.98x wall | 5.2x | fail |
| Plots: 50 x 100k | 1.14x wall | 1.17x | pass |
| Plot GPU memory, DPR 2 | 1.96x per plot (1x MSAA: 1.02x) | 2.8x | fail |
| Streaming plot ≤ 16.7 ms | 12.9 | 13.5 | pass |
| Mixed: 6 plots + Scene3D | 1.49x wall (CPU 1.08x) | 3.3x | fail |

**Measure-first items: what the numbers say.**

| Item | Numbers | Decision |
|---|---|---|
| Depth-resolve fast path | The replay is +11.3 ms on OpenGL and +11.5 ms on Vulkan for a 10M cloud (+58%); without it the RHI frame (19.5 ms) equals the GL build's (19.6 ms). llvmpipe: 2x. | Justified: it is the whole Gate A gap of the cloud row. Keep the replay where `QRhi::ResolveDepthStencil` is missing (D3D11, WebGL). |
| Plot AA (4x MSAA) | 4x vs 1x: 3.6 ms GPU and 6.8 ms wall on 50 x 100k, where the 4x row already passes; memory +84 MiB per 1600x900 plot at DPR 1 and +167 MiB at DPR 2 over Qwt (1.96x, the gate is 1.2x). | Speed does not justify a change, memory does: the plot-memory row fails at 4x. A colour-only 4x target (PL item 4) saves an estimated 88 MiB at DPR 2, still about 1.45x; 1x (1.02x) or colour-only 2x (an estimated 1.3x) are the candidates. The user decides. |
| Indexed strips | 50 x 100k passes (1.14x); the frame is CPU-bound (65 ms CPU, 19 ms GPU). | Not justified by the gate on Iris Xe. Revisit with the RTX run and PL's own profile. |
| Shader dashes | Dashed 5 x 100k is 1.98x wall and 2.2x CPU of Qwt GL. | Justified: the dashed row fails. |
| Dock rebuild | 10M cloud float/redock 226/211 ms vs 95/86 (GL); tab switch 10.8 ms vs 1.2 (77 ms on Vulkan, 512 ms vs 3 ms on llvmpipe); 50-link robot float 86 vs 36 ms. | Justified: the reparent row fails. A tab switch (hide/show, same QRhi) should not rebuild at all; its cost points at a release on hide. |
| `drawsFor` / registry scans | 1000 layers: RHI GL 8.1 ms wall / 4.3 ms CPU vs GL 8.8 / 8.5. | Not justified. |

Other observations:
- The GL build's 1000-layer scene takes 4.4 s to first show and 1017 MiB RSS
  (RHI GL: 134 ms, 169 MiB).
- Vulkan on the Iris costs more GUI CPU than RHI OpenGL wherever the frame is
  CPU-bound (cubes 63 vs 45 ms, stream 10.6 vs 5.6 ms): consistent with keeping
  OpenGL as Linux's native backend.
- The 10M cloud's peak RSS (634 vs 373 MiB) is the retained mirror plus the
  conversion; P's step A targets it.
- Mixed layout with no plots (the Scene3D view filling the window, 1M points
  as 0.1 m sprites in a 1 m cube): RHI GL 96 ms vs GL 3.9 ms per frame. Not
  analysed; it looks like the GL renderer clamping sprite size where the QRhi
  one draws quads. Handed to P.
- Of about 36 serial smoke runs of Vulkan cases under the Wayland compositor
  (lavapipe), one (`CubesAndLinesOrbit`) failed; the log was overwritten before it was read.
  Running the display passes in parallel (`ctest -j`) on one Wayland
  compositor stalls frames when the windows overlap; run them serially there.

### P5 M: results

What changed:
- **Marker cache (`MarkerRhiPass`).** Each admitted layer owns a `LayerGpu`
  with Static instance, line and triangle buffers (the point pass's grow/shrink
  rule, now `ensureStaticVertexBuffer()` in `rhi_scene_pass.h`).
  `expandLayer(geometry, key, scratch)` is a pure function of a `LayerKey`:
  - the geometry revision;
  - the render origin;
  - the style: opacity, override colour, wireframe;
  - shadow casting;
  - the resolved fixed-from-frame matrix of every frame the layer references,
    compared exactly.

  `prepare()` re-expands and re-uploads only the layers whose key changed. The
  shadow casters and their bounds are cached with the layer. Layers not admitted
  in a frame give their buffers back.
- **Process-wide revisions.** `SceneEntitiesLayer`'s geometry and model
  revisions come from one atomic counter, so a layer created at a freed layer's
  address never matches a stale key. The model revision is S's key too.
- **The transform stays on the CPU for markers.** A first version kept marker
  vertices relative to an anchor (an earlier render origin) and added
  anchor − origin in `marker.vert`, `marker_shadow.vert` and `scene.vert`, so
  that pan and follow would rebuild nothing. It was not pixel-identical:
  - 14 Vulkan harness images and 1 OpenGL image differed from the baseline, by
    up to 105 px and 28 levels (shadow edges);
  - the cache test's cached-vs-uncached A/B differed by 1–2 px.

  The render origin is therefore in the key, as the plan has it, and those
  shaders are unchanged. Orbit, zoom and playback without new samples or TF
  motion reuse the buffers. A pan or a follow step rebuilds the layers.
- **Static grid.** The grid is built once per settings change, at the
  fixed-frame origin, into a Static buffer. The new `grid.vert` and
  `grid_shadow.vert` add the origin offset (binding 3, −render origin), so pan,
  follow and playback upload nothing. This one is exact: the grid positions and
  the render origin (the camera focal) are floats, so the shader's correctly
  rounded float add equals the float of the double subtraction it replaces.
  - The TF connection lines are keyed separately: the TF buffer and its
    revision, the fixed frame, the time, and the render origin.
  - They keep a Dynamic buffer. They are drawn with a zero offset, because TF
    placements are arbitrary doubles and an offset add would lose precision
    far from the origin.
- **Static buffers.** Pose-layer instances and trail ribbons are now Static,
  uploaded on a revision change (trails also on a render-origin change). The TF
  triads keep a Dynamic buffer: they are rebuilt with the render origin and the
  playhead.
- **Voxel whole-grid frustum reject.** `VoxelRhiPass` skips a grid wholly
  outside the view frustum, using the GL pass's `chooseCubeDrawMode()`: voxel
  centres, inflated by half the largest cell side. The clip-space-corrected
  matrix only loosens the near plane on 0..w-depth backends, so it never
  over-culls. The volume still uploads and the layer still counts as admitted.
- **`scene3d_rhi_marker_cache_test`** (7 cases):
  - orbit and zoom leave the rebuild count unchanged, and a pan rebuilds once;
  - with one static and one live layer, the count grows by exactly 1 per tick;
  - a TF-only move of a static layer's frame keeps its geometry revision,
    rebuilds it once and moves its pixels; the same placement again rebuilds
    nothing;
  - opacity, the override colour (only while enabled), wireframe and shadows
    each rebuild once; hiding a layer releases it, and showing it rebuilds;
  - cached frames are pixel-identical to `setMarkerCacheEnabledForTest(false)`
    at rest, after an orbit and after a pan, with shadows and the grid on;
  - the grid never rebuilds under pan or playback, and the connections rebuild
    once per origin or time change;
  - a voxel grid outside the frustum is not drawn but stays admitted, and it
    draws once it is back in view.

Pixel checks: images were dumped from this worktree before the edits and after
them (`PJ_RHI_TEST_DUMP_DIR`, `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1`), with Mesa
26.0.3 llvmpipe and lavapipe.
- **Goldens:** 56/56 identical on OpenGL and 56/56 on Vulkan.
- **Harness:** 85/85 identical on Vulkan. On OpenGL, 84/85 are identical; the
  exception is the live timing digit in `PresentationHoverLabelAndPerfHud_hud.png`
  (7x9 px, the same run-to-run noise P5 P reports).
- **`counters.txt`:** identical on both backends.

Tests, on OpenGL and Vulkan: `scene3d_rhi_render_test` 24/24,
`scene3d_rhi_point_test` 10/10, `pointcloud_golden_test` 28/28 and
`scene3d_rhi_marker_cache_test` 7/7. The QRhi build's
`pj_scene3d_widget{,_plain,_fixture}_tests` pass 103 + 54 + 118. In the GL build
(`scene_entities_layer.cpp` is shared), `pj_scene3d_widget_tests` passes 140 and
`pj_scene3d_widget_fixture_tests` 134.

Benchmark: stream B's `scene3d_frame_benchmark` (`PJ_S3D_BENCH_FULL=1`, 30
frames), with the base binary built from this worktree before the edits. Setup:
- 5 interleaved rounds, on Iris Xe (RPL-P), Mesa 26.0.3, Wayland;
- `cpu_ms` is the GUI thread's CPU per frame;
- each cell is the median, with min–max.

| Case | API | GUI CPU ms, before → after | wall ms, before → after |
|---|---|---|---|
| 100k cubes + 1M line vertices, orbit | Vulkan | 63.1 (62.0–68.0) → 3.6 (3.6–3.9) | 95.9 → 35.4 |
| 100k cubes + 1M line vertices, orbit | OpenGL | 47.1 (46.4–49.0) → 3.7 (3.5–3.9) | 46.5 → 29.0 |
| 10k markers, pan + follow | Vulkan | 15.0 (13.4–16.1) → 14.2 (13.1–15.5) | 49.6 → 49.5 |
| 10k markers, pan + follow | OpenGL | 9.5 (8.3–10.3) → 9.0 (8.5–9.8) | 16.8 → 16.7 |

- **Pan and follow stay at parity,** because a moving render origin rebuilds the
  markers (see above).
- **A cheaper exact path is possible** but not done: keep each instance's
  translation (and each stream vertex's position) in a separate per-frame
  vertex stream, recomputed from cached doubles on an origin change. The shaders
  would be unchanged, since `marker.vert`'s matrix column 3 simply comes from
  another binding. That is roughly 16 B/instance of upload per pan frame instead
  of a full expansion.
- **GPU memory (Vulkan, cubes case):** the per-frame-in-flight copies of the
  Dynamic marker buffers are gone. The benchmark's settled reading still shows
  more, VMA 280 → 303 MiB and DRM 527 → 720 MiB in 4 of 5 runs (232 / 463 MiB
  in the fifth). The likely cause, not verified: QRhi frees an upload's staging
  buffer only after later frames, and a settled view renders none.

Rows for the integrator:
- decision table: markers use a per-layer Static cache keyed on the resolved
  placements; the render origin stays on the CPU (not pixel-identical in the
  shader); the grid's origin moves to the shader (exact);
- Findings: moving a camera-relative subtraction of arbitrary doubles into a
  float shader add is not pixel-identical, even when it is sub-ulp; only
  float-valued operands, like the grid's, make it exact;
- Known gaps: markers still rebuild on pan and follow (the translation-stream
  follow-up above).

### P5 S: results

What changed (`ModelRhiPass`; the model paragraph of ARCHITECTURE.md has the design):
- **Mesh identity diff.** A `modelRevision()` bump keeps every GPU mesh whose key
  still maps to the same `MeshData` pointer, builds new or changed keys, and
  releases the keys that are gone. A kept mesh with dropped material bindings
  gets them rebuilt. A model whose N meshes arrive one per frame now builds N
  meshes instead of N(N+1)/2 (50 meshes: 50, not 1275).
- **Pass-level texture cache** keyed by `TextureSource::key` and colour space, as
  the GL `MeshRenderPass` does, reference-counted by the meshes that bind it. One
  decode and one `QRhiTexture` per image and colour space for the whole view.
  The per-mesh and per-layer pixel budgets charge what they charged before, and
  are still checked before a decode. An image that one slot samples as sRGB and
  another as linear gets two textures, as on GL.
- **Lazy wireframe edges.** Edge lists are built on a mesh's first wireframe
  draw, and the CPU copy is dropped once queued for upload. A layer that never
  shows wireframe keeps neither copy: that was 24 B/triangle on the CPU plus
  24 B/triangle on the GPU.
- **Instance skip.** `prepare()` rebuilds the colour and shadow instances and
  queues their uploads only when the key differs from the last prepared frame.
  The key is: the render origin; the shading inputs the instances read (mesh
  and collision opacity, roughness); and the admitted list. That list holds,
  per layer, the layer, its `modelRevision()`, opacity, colour override and
  wireframe, and every colour and shadow call with its resolved placement
  (`placed->matrix() * draw.model`, compared exactly), mesh identity and group.
  So a TF-only link move, which bumps no revision, refreshes the instances and
  the shadow bounds. Shadow admission is in the list too. Any layer rebuild
  also forces a rebuild. `admit()` rotates the list in only after a
  `prepare()`, so an admission that never reached `prepare()` cannot stand in
  for the uploaded data. A recreated instance buffer (new capacity) is always
  refilled.
- **Static buffers (addendum 8).** No change: the mesh vertex, triangle-index
  and edge buffers were already Immutable. The instance buffers stay Dynamic,
  because animated models rewrite them every TF tick; the skip removes their
  idle uploads.
- **Found by the tests:** `SceneEntitiesLayer::resetReplayState()` passed
  `releaseModelMesh()` a reference to the key of the map entry that call erases
  (a use-after-free on dock teardown, seen as a Vulkan-run segfault). It now
  passes a copy (one line).

Tests (`scene3d_rhi_model_cache_test`, 9 cases, OpenGL and Vulkan):
- 50 meshes arriving one per frame build 50;
- a same-key reload rebuilds only that mesh and shows its new colour; a removed
  key drops its draw;
- textures are shared across meshes and layers (4 textured meshes: 2 decodes,
  sRGB + linear) and freed with their last user;
- wireframe edges build once on first use, and toggling reproduces both looks
  exactly;
- orbit and zoom queue no instance upload; a pose change queues one;
- shadows off and on rebind the kept meshes: no rebuild, and the shadowed frame
  is identical;
- a TF-only link move of a `RobotModelLayer` with `modelRevision()` unchanged
  queues one colour and one shadow instance upload, changes more than 200
  pixels and moves the shadow caster bounds;
- a pose-only textured-GLB SceneEntities batch builds no mesh and decodes no texture;
- a 9-step A/B (orbit, TF move, mesh added, wireframe, mesh replaced, shadows
  off and on, zoom) is pixel-identical against `setCachesEnabledForTest(false)`.

Mutation checks:
- a key without the placements fails the TF case (uploads, pixels, shadow
  bounds) and the A/B;
- caches off by default fails the six counter cases.

**Pixels.** The harness and goldens were dumped from this worktree before the
edits (its F tip plus the staged P and B) and after, on llvmpipe/lavapipe:
- goldens: 56/56 images identical on OpenGL and on Vulkan;
- harness: 85/85 identical on Vulkan and 84/85 on OpenGL, with `counters.txt`
  identical on both;
- the one OpenGL difference is the perf HUD frame
  (`PresentationHoverLabelAndPerfHud_hud`): a 7x9 px glyph of the timing
  readout, which differs between any two runs.

**Numbers.** B's harness: Iris Xe, xcb on a private rootful Xwayland, vsync
off, 5 interleaved rounds, the build slot held. Median [min–max], before → after:

| Case | RHI GL | RHI VK |
|---|---|---|
| 50-link robot, TF each frame, wall ms | 2.9 [1.4–3.0] → 1.4 [1.2–2.9] | 4.3 [3.4–5.4] → 4.5 [3.9–5.1] |
| 50-link robot float / redock / tab ms | 84.0 / 59.5 / 2.2 → 84.3 / 59.1 / 2.2 | 43.9 / 16.9 / 5.5 → 43.5 / 16.8 / 5.4 |
| First show, 10M cloud + robot, ms | 264.8 → 266.4 | 269.5 → 250.8 [245.9–266.6] |

These rows cannot show S's wins, and none moved beyond run-to-run noise (the
robot GL row is bimodal in both builds):
- B's robot is 50 primitive links on 3 shared meshes, with no textures, and it
  loads its meshes at once;
- its TF changes every frame, so every frame rebuilds its instances.

The wins are structural, and the tests count them:
- O(N) mesh builds for a model that streams in;
- one decode per image;
- no edge lists until wireframe;
- no instance upload for a static model under orbit or zoom.

Shared files:
- `src/layers/scene_entities_layer.cpp`: one line in `resetReplayState()`. M owns
  the file; M's hunks there are the revision bumps, none on that line.
- `scene_view_widget_rhi.*` is untouched: the tests reach the pass through F's
  `modelPassForTest()`.

### P5 PL: results

What changed (`pj_plotting`; details in `pj_plotting/CLAUDE.md`, "PlotRhiCanvas"):
- **1x + shader edge AA (the user's decision).** `PlotRhiCanvas` renders at one
  sample per pixel into its own colour-only target (`setAutoRenderTarget(false)`,
  no depth-stencil: 4 B per device pixel). `initialize()` recreates the target on
  the widget's colour buffer and keeps the render pass descriptor while it stays
  compatible. Every stroke, fan, ring and box reaches half a device pixel past
  its true edge and carries its position across it; `plot_color.frag` ramps
  coverage over one device pixel from screen-space derivatives. Boxes (dots,
  Lines-and-Dots, the frame) antialias both axes. Strokes under one device pixel
  draw one pixel wide at lower alpha, as Qt's raster does. `setSampleCount(n)`
  still works (MSAA colour + resolve, no depth-stencil), for comparisons only.
- **Snapping, like Qwt.** Horizontal and vertical `addLine()` lines (grid,
  marker lines, sticks, symbol strokes) and the Rect/Cross/HLine/VLine symbols
  sit on whole pixels: the logical coordinate rounded, then centred on its
  device pixel for an odd device width (on a pixel edge for an even one), which
  also holds at fractional DPRs. Curves keep exact coordinates. Grid and marker lines
  span the canvas contents as Qwt's do, so the dash phase starts where Qwt's does.
- **Why the grid read 2/1 and 5/1.** `addDashedSegment()` gave every dash a
  square cap (half the width at each end). A cosmetic pen (width 0, the grid's)
  has no per-dash caps in Qt: `QCosmeticStroker` samples the pattern per pixel.
  The cap made each 1 px dot 2 px and each 4 px dash 5 px, shrinking the gaps;
  the half-pixel placement then smeared both across two rows. The pattern table
  itself was right.
- **GPU dashes.** Each dashed segment is one quad carrying its path distance
  (16 bits, 1/64 pen width, reduced modulo the period in double first; a clipped
  segment longer than 960 pen widths is split). The shader cuts Qt's patterns:
  per pixel centre for a cosmetic pen, antialiased with square caps for a wide
  one. The reducer's source phase and the bevel-only-inside-a-dash rule are
  kept. A dashed curve costs a solid curve's vertices (9 per sample, down from
  36 in the budget estimate).
- **Vertex format** 12 → 16 B: position, RGBA, and a UNormByte4 with the across
  coordinate, the style code and a 16-bit second coordinate (dash distance or a
  box's second axis). The geometry is built in `QByteArray`s handed to
  `uploadStaticBuffer()` by implicit sharing: the 30-45 MB per-frame copy into
  the update batch is gone.
- **Split dirty state (addendum 3).** Three retained buffers: grid + curves +
  frame, overlay (every `QwtPlotMarker`: tracker, hover, ride-along), text.
  `PlotWidgetBase::replotOverlays()` (used by `PlotWidget::setTrackerPosition()`)
  runs one replot under `PlotRhiCanvas::setOverlayOnlyReplot(true)`, which dirties
  only the overlay. The curve layer still rebuilds when the view (size, dpr, axis
  bounds, clear/frame colour) or the curve structure (items, visibility, pens,
  styles, data object and size) changed, so an overlay-only replot cannot hide an
  added or grown curve. Text layers are recorded into a `QPicture` and cached by
  its bytes + raster rectangle + dpr; only changed ones are re-rasterised, and
  the atlas is re-uploaded only when the layer list changed. The cache is dropped
  with the GPU resources.
- **The 1 px canvas frame** is drawn over every item (`QFrame::Box | Plain`, the
  palette's WindowText), as the Qwt canvases draw it.
- **`plot_canvas_benchmark`:** `--samples`, `--plots K`, `--grid`, `--legend`,
  `vertices=`; `QSG_RHI_PROFILE` and the GL timer query only with `--gpu`; the
  `tracker_only` case uses the overlay-only replot. The raster column and the DRM
  memory column already existed (B).
- The `PJ_WASM_PLOT_RHI_READY backend=… device=… samples=%d` line is unchanged
  (now `samples=1`).
- Indexed strips: not done (B: not justified).

Shared files outside the plan's PL list: `PlotWidgetBase.{h,cpp}` (+16 lines,
`replotOverlays()`) and `PlotWidget.cpp` (2 lines, `setTrackerPosition()` calls it).

**Parity with Qwt** (`plot_rhi_render_test`, llvmpipe/lavapipe, OpenGL and
Vulkan, DPR 1 and 2): the reference now snaps grids and markers (per item, as
`QwtPlot::drawItems` with `roundingAlignment()`) and keeps curves exact.
Colour match: 1.000 for curves, dashes, grid, legend; symbols 0.989 / 0.992
(was 0.90 / 0.94 at 4x MSAA, threshold 0.85); markers 0.996+; `PlotMarkersItem`
0.986 (text). The default threshold goes 0.95 → 0.98 and the symbol case's
special 0.85 is dropped. New cases: grid/marker lines on whole pixels with
cadence 1/2 and 4/2 matched at 100% and 100% of the marker ink in one column,
plus the frame; the edge ramp (every column's coverage within 0.006 px of the
exact width / cos(slope), no aliased column, no ramp wider than 2 px); all 8
dashed styles (4 styles, cosmetic and 2 px) at 100% dash agreement with Qwt; a
tracker move with 0 curve-layer builds and 0 re-rasterised text; the target at
4 B/px (20 with `setSampleCount(4)`).

Before/after zoom crops (Iris Xe, same plot as B's showcase; rows Qwt GL, base
4x MSAA, PL, Qwt raster): `scratchpad/showcase/after_pl_zoom_sheet_dpr1.png`,
`after_pl_zoom_sheet_dpr2.png`, `after_pl_full_sheet_dpr1.png`. The PL row is
indistinguishable from the raster canvas at 8x zoom; the grid is crisp 1/2 and 4/2.

**Numbers** (B's method: Iris Xe, xcb on a private rootful Xwayland, vsync off,
5 interleaved rounds of base binary / PL binary / Qwt GL, 30 frames each,
1600x900, timestamps on in every run; median wall / canvas CPU / GPU ms):

| Case | Qwt GL | RHI GL base → PL | RHI VK base → PL |
|---|---|---|---|
| tracker only, 10 x 100k | 17.6 / 12.6 / 3.7 | 26.6 / 15.2 / 10.0 → **11.4 / 3.7 / 5.0** | 27.5 / 16.1 → **11.9 / 3.9** |
| dashed 5 x 100k | 18.3 / 14.2 / 3.1 | 35.4 / 31.1 / 2.5 → **17.6 / 13.7 / 3.0** | 93.2 / 31.9 → **18.0 / 14.0** |
| 50 x 100k | 75.6 / 58.2 / 17.8 | 85.3 / 63.1 / 18.9 → 91.2 / 59.8 / 17.8 | 90.7 / 67.7 → 94.3 / 61.8 |
| streaming 10 x 100k | 19.8 / 16.6 / 0.6 | 12.9 / 6.8 / 3.0 → 12.3 / 6.3 / 1.5 | 13.4 / 6.9 → 16.4 / 6.6 |
| static 1M | 12.6 / 10.6 / 0.9 | 12.9 / 10.4 / 1.6 → 11.7 / 10.1 / 0.7 | 12.6 / 9.7 → 13.6 / 11.7 |

GPU memory, one 1600x900 plot, DRM total MiB (static 1M):

| Canvas | DPR 1 | DPR 2 |
|---|---|---|
| Qwt GL | 62.0 | 174.2 |
| RHI GL base (4x MSAA + depth-stencil) → PL | 146.8 → **42.6** | 341.2 → **138.8** |
| RHI VK base → PL | 165.7 → 106.1 | 489.4 → 315.8 |

Reading:
- Gate rows: tracker only 0.65x Qwt GL wall (was 1.51x); dashed 0.96x (was
  1.98x); plot GPU memory 0.69x at DPR 1 and 0.80x at DPR 2 (was 2.37x / 1.96x;
  the addendum's target was about 1.02x). Vulkan: dashed 5.2x → 0.98x.
- 50 x 100k is 7% slower in wall (1.21x Qwt GL, the gate is 1.2x) with 5%
  less CPU and equal GPU time. The difference is outside `render()`: the 16 B
  vertices make the driver upload 43 instead of 32 MB per rebuild, and the
  antialiasing fringe widens every 1.4 px stroke to 2.4 px of geometry. This is
  the case where indexed strips (-55% vertex bytes, B) would pay; they are the
  lever if this row matters.
- Vulkan's DRM total for 50 x 100k rose 454 → 587 MiB; the device-local buffer
  is 43 MiB. As M observed, QRhi's Vulkan backend frees each upload's staging
  buffer only frames later, and this case uploads 43 MiB every frame.
- Vulkan streaming and static 1M are 3 and 1 ms slower in wall (static also
  2 ms more CPU); not analysed (Vulkan is not Linux's native backend).

Not verified locally: D3D11, Metal (the shader bakes for HLSL 5.0 / MSL 1.2 and
uses only derivatives, `mod`, `step`; per-primitive-uniform branches keep the
derivatives defined).

**Follow-up: indexed geometry.** The 1.21x row above led to indexed layers.
Both geometry layers now draw with `drawIndexed` and 32-bit indices, next to
the vertex buffers.
- A solid polyline is one strip of corner pairs. Consecutive quads share them,
  and a turn's bevel adds only its join point. That is 5 vertices and 9
  indices per turning segment, and 2 vertices and 6 indices on a straight join.
- The triangles are the same as the separate quads and bevels.
- Fans, rings, boxes and dashed quads share their corners too; dashed segments
  stay one quad each.
- Pixel check against the non-indexed build (Iris, the showcase plot): 94 of
  204k pixels differ at DPR 1 and 312 of 816k at DPR 2, by at most 3 levels
  (rounding of the shared corners).
- A cheaper variant took the bevel triangle from the inner incoming corner
  instead of the join point: 4 vertices per turn, the same coverage plane. It
  was rejected because it double-blends the join's inner half. On the plot
  above that changed 99 fringe pixels by up to 52 levels; translucent wide
  strokes would show it too.
- `plot_rhi_render_test` numbers are identical to the non-indexed build
  (21/21 on OpenGL and Vulkan at DPR 1 and 2, 0.98 thresholds unchanged). The
  full WASM suite passes 45/45, and the probe passes.

Bytes per 50 x 100k rebuild:
- before: 2.7M vertices x 16 B = 43 MB;
- after: 1.5M vertices (24 MB) + 2.7M indices (10.8 MB) = 35 MB;
- the rejected 4-vertex variant: 30 MB.

Same method as above, 5 interleaved rounds (median wall / canvas CPU / GPU ms):

| Case | Qwt GL | RHI GL base | RHI GL PL, non-indexed → indexed | RHI VK base → indexed |
|---|---|---|---|---|
| 50 x 100k | 75.7 / 59.2 / 17.8 | 86.3 / 64.6 / 18.9 | 91.2 / 59.8 / 17.8 → **77.0 / 56.5 / 17.8** | 91.0 / 67.4 → 99.2 / 58.9 |
| streaming 10 x 100k | 19.6 / 16.5 / 0.6 | 12.9 / 6.7 / 3.0 | 12.3 / 6.3 / 1.5 → **11.0 / 5.3 / 1.4** | 13.4 / 6.9 → 15.0 / 5.6 |
| static 1M | 12.5 / 10.6 / 0.9 | 12.0 / 9.6 / 1.6 | 11.7 / 10.1 / 0.7 → 11.7 / 10.1 / 0.7 | 13.3 / 10.5 → 11.9 / 10.1 |
| dashed 5 x 100k | 18.0 / 13.9 / 3.1 | 35.2 / 31.0 / 2.5 | 17.6 / 13.7 / 3.0 → 17.7 / 13.7 / 3.0 | 93.2 / 31.8 → 17.9 / 13.9 |

- 50 x 100k on RHI GL, Linux's native backend: 1.02x Qwt GL (gate 1.2x) and
  11% faster than base. RHI GL's DRM total for that case is 199 MiB, against
  213 MiB for base.
- On Vulkan, 50 x 100k is 9% slower than base in wall at 13% less CPU. A
  separate interleaved check gave: base 88.7-89.8 ms, the 4-vertex variant
  91-96 ms, indexed 97-102 ms. The extra time is outside `render()`; not
  analysed.

Rows for the integrator:
- decision table: plots render 1x into a colour-only target with shader edge
  AA; GPU dashes; cosmetic dashes per pixel, wide ones capped and antialiased;
  indexed geometry layers (shared corners, same triangles);
- Known gaps: Vulkan 50 x 100k is 9% slower than base in wall (lower CPU), not
  analysed;
- open question (throughput, `:641-643`): the table above replaces the P4
  plot numbers for tracker, dashed and memory.

### P5 R: results

What changed:
- **V1, grid colours and filled-cell lines.** `look::gridColors()`
  (`scene_look_defaults.h`, Qt-free, double math) blends the theme's backdrop
  toward its foreground: lines 0.35, tiles 0.10 and 0.24. The GL view linearizes
  it as before (byte-identical); the QRhi view feeds it, display-referred, into
  `GridRhiPass::Settings` (part of the grid key: a theme switch rebuilds), and the
  shaders linearize on the HDR target. The three `kGrid*` constants are gone. The
  filled style now draws only the cells: `grid_cell.frag` draws the lines in the
  cell surface from the distance to the nearest edge, `fwidth`-anti-aliased, as
  the GL `GridRenderPass` does (`grid_cell_shadow.frag.qsb` is the shadow-receiver
  variant; `grid_shadow.*` became `grid_cell.*`).
- **V2, markers.** GL: `SceneEntitiesLayer`'s constructor hands its overrides to
  the marker pass, so markers draw at the layer's 0.6 opacity from the start
  (before, only a later change of opacity reached the pass: markers were opaque
  in the GL build). This is a user-visible change of the GL build, for the release
  notes. Unit test `MarkerPassStartsWithTheLayerDefaultOpacity`. QRhi: the cube
  fill is depth-biased (constant and slope 1, GL's `glPolygonOffset(1, 1)`), and
  the edge overlay replays the fill's instances through `marker_edge.vert`: an
  edge with a camera-facing adjacent face is the colour × 0.6, a self-occluded one
  the 0.45 mix back toward the colour, as on GL (the camera view matrix is a
  per-frame uniform, binding 4). The separate darkened edge instances are gone.
- **V3, robot primitives.** A URDF box, cylinder, sphere or placeholder draws with
  `use_material` false and the link colour: the unit meshes' grey material and
  vertex colour applied the default grey twice. `model.frag`'s legacy Lambert
  branch is deleted; every material takes the PBR path as on GL.
- **V4, scene goldens.** `pointcloud_golden_test` gains a scene corpus, GL-baked,
  held to the same rules (7 new goldens, the 28 point-cloud goldens unchanged):
  the line and filled grid in the light and dark theme, SceneEntities cubes + line
  strips at the default 0.6 and at 1.0 opacity, and a URDF of material-less
  primitives plus an STL link, lit, grid on. The line grids are all 1 px edges,
  which lavapipe's mirrored MSAA pattern resolves differently on 18% of their
  foreground, so they check coverage and mean colour only; the filled grids are
  3 m so the gizmo sits on the backdrop (the GL view draws the filled grid darker
  under the gizmo's square; the QRhi view, gizmo after presentation, does not).
  The test body became `GoldenView` + `expectMatchesGolden()`, shared by the four
  suites. README updated.
- **(a) Depth resolve.** Where QRhi reports `ResolveDepthStencil` and multisample
  textures (desktop OpenGL, Vulkan, Metal), the HDR pass's depth is a 4x texture
  resolved into the depth texture, and no replay runs: the replay target becomes a
  colour-only clear of the coverage texture (SSAO and the EDL mask write into it),
  and `present.frag` takes coverage from the resolved depth (`ssao_params.z`).
  D3D11 and WebGL keep the replay. Found here: Qt 6.11's GL resolve blits depth
  and stencil together, attaching the textures as both, which leaves a
  stencil-less D32F framebuffer incomplete and the resolved depth unwritten, so
  OpenGL resolves D32FS8. The EDL mask pipelines get a −1 constant and slope bias
  on this chain (the resolved sample is off the pixel centre the mask rasterizes).
  `setForceDepthReplayForTest()` keeps the replay; `DepthResolveMatchesTheForcedReplay`
  (render harness) asserts the fast path is on for OpenGL/Vulkan/Metal and that
  the two chains differ on under 1% of pixels, SSAO and EDL on.
- **(b) Tab switch.** Nothing is released on hide/show on the same QRhi (B's
  hypothesis): QRhiWidget re-renders synchronously on every Show. `event()` now
  makes that Show paint record nothing, so the frame drawn before the hide is
  composited at once (QOpenGLWidget's retained-FBO behaviour), and queues an
  update that renders whatever changed while hidden.
- **(c) Float/redock, (e) first show.** A process-wide `QShader` cache in
  `loadRhiShader()` (and in the media viewer's `loadShader()`): each pack is read
  and deserialized once per process. With (a), no depth-replay clone is created
  on OpenGL/Vulkan/Metal, a third of the pipelines.
- **(d) Large sprites.** Not a sprite-size problem. B's row (1M 0.1 m spheres,
  RHI 96 ms vs GL 3.9 ms) is `mixed_layout_benchmark --plots 0`, whose GL build
  draws no cloud at all: on llvmpipe its frame costs the same with 1M or 1k
  points. With one camera in both builds (the scratch `VpSprites` case) the GL
  build is 1.3x faster, not 25x (table below): the GL renderer's `gl_PointSize`
  point sprites against the QRhi view's quads above `kPointListMaxPx` (4 px),
  which it keeps so that a sprite whose centre leaves the view does not pop. No
  sizing change. `mixed_layout_benchmark` (B) needs its GL view fixed before its
  Scene3D share means anything.
- **Addendum 4, media viewer colour-only target.** `setAutoRenderTarget(false)`
  and a `QRhiTextureRenderTarget` over `colorTexture()` built in `initialize()`
  (rebuilt on a resize, descriptor kept): no depth-stencil (4 B/px).
- **Addendum 5, Scene3D sample count.** The widget's sample count is chosen once
  per QRhi: 1 when the GPU can run the HDR chain (the output only carries the
  present, gizmo and HUD), 4 when it cannot. A runtime refusal on a capable GPU
  (size, budget, missing HDR variant) draws its direct pass without MSAA instead
  of rebuilding every resource for a new target (a per-frame switch also broke
  `MissingHdrPipelineVariantFallsBackToDirect`, whose injected fault then failed
  the rebuilt direct pipelines). The gizmo is aliased now (below).
- **Addendum 11.** Shader cache: above. Lazy direct pipelines: not done. The
  HDR clones copy their direct pipeline's shaders, and every QRhi backend caches
  compiled shaders/programs by source per QRhi, so a clone re-uses the direct
  pipeline's compile; making the directs lazy would save only the pipeline
  objects: OpenGL links each program once per QRhi (and loads it from Qt's
  shader disk cache), D3D11 and Metal compile each stage once per QRhi (their
  shader caches); only Vulkan builds every `VkPipeline` (OpenGL first show:
  `pipeline_ms` 25 with the clones included; Vulkan's first show is within 3 ms
  of OpenGL's).
  Pipeline-cache persistence: not possible with QRhiWidget in Qt 6.11. The
  backing store creates the QRhi (`QPlatformBackingStoreRhiConfig` carries only
  the API and the debug layer), so `EnablePipelineCacheDataSave` cannot be set
  and `setPipelineCacheData()` would come after Qt's own pipelines; OpenGL already
  persists program binaries through Qt's shader disk cache. Precompiled packs
  (`qsb -c`) need fxc and the Metal toolchain in a per-platform CI bake: not
  evaluated further. First-show time on the Windows and macOS lanes: the
  `FirstShow` smoke already prints `first_show_ms` and `pipeline_ms` in those
  lanes' verbose QRhi ctest logs.
- **Addendum 12, media viewer.** Each texture layer keeps the `shared_ptr` of the
  pixel buffer it holds; a frame or composite layer bringing the same buffer
  uploads nothing (the buffer is retained, so a new one cannot reuse its
  address). The rectification LUT is `RG32UI` holding the two floats' bits (8 B/px;
  QRhi 6.11 has no `RG32F`), read with `texelFetch` + `uintBitsToFloat`
  (`yuv_to_rgb.frag` re-baked; the HLSL is a `Load`, the MSL a `read`; the vert
  pack re-bakes byte-identical). The glyph cache is dropped whole past 512
  entries at the next text rebuild.
- **Remaining plan items.** Verified done in P4: the macOS 4.1 core format
  (`main.cpp`, and the test main), the `yuv_to_rgb` `300 es,330,440` bake, the
  macOS OpenGL CI step. Done here: frames above `TextureSizeMax` are dropped with
  one warning (`setMaxTextureSizeForTest()`; `MediaFrameAboveTheTextureLimitIsNotDrawn`),
  `MediaCompositeUploadsOnlyTheChangedLayer`, `scene3d_rhi_platform_test` asserts
  `max3dTextureSize` (2048 on D3D11/D3D12/Metal; on OpenGL and Vulkan the driver's
  value, queried independently), and the Linux GL-floor
  CI step (`MESA_GL_VERSION_OVERRIDE=3.3`, `MESA_GLSL_VERSION_OVERRIDE=330`,
  `scene3d_rhi_render_test.opengl` and `rhi_dock_lifecycle_test.opengl`; passes
  locally, 25/25 and 11/11 + 1 skip).

Pixels. Images were dumped from this worktree before the edits (the merged
F + P + B + M + S tree) and after, on Mesa 26.0.3 llvmpipe and lavapipe.
- **The merge itself** is pixel-identical to M's and S's final images: 56/56
  goldens and 85/85 harness frames on both backends, bar the perf-HUD timing digit.
- **Point-cloud goldens (28, unchanged goldens):** every image moves, all pass.
  - Depth resolve: 1-2 levels inside clouds (SSAO and EDL now read the resolved
    sample's depth) and more than 16 on 100-2400 px per image at silhouettes and
    sub-pixel points (the resolved sample, not the pixel centre, decides depth and
    coverage). The QRhi-OpenGL images moved *toward* the GL goldens: worst
    foreground diff 0.061 → 0.007, worst per-pixel diff 0.034 → 0.003
    (the GL renderer resolves its multisample depth too). Vulkan is unchanged
    within noise: worst foreground diff 0.086 → 0.083, per-pixel 0.074 → 0.077.
  - Output sample count 1: the corner gizmo is aliased, about 120 px per image
    off by more than 16, all inside the gizmo's 80x80 square.
- **Harness (85 frames per backend):** every frame changes.
  - V1: 61 frames have the filled grid, now in the theme's light tones with
    analytic lines (`counters.txt`: grid line vertices 44 → 0).
  - V2: cube edges (`OcclusionAndTranslucency_cubes`, 3029 px): hidden edges in
    the lighter mix, the fill biased behind its edges.
  - V3: the URDF box is 0.7 grey once, not 0.7 × 0.7 on the legacy path
    (`PresentationTonemapModes`, the robot frames, about 4940 px).
  - Depth resolve and sample count, as for the goldens. The forced direct
    fallback (`PresentationRenderScaleSupersamples_fallback`) now draws without
    MSAA.
  - `PresentationTonemapModes` now paints its box orange: AgX and Neutral grade the
    correct 0.7 grey too alike for its per-mode check (it failed at 1 vs 6).
- **New scene goldens (QRhi vs the GL goldens):**

  | Scene | OpenGL fg-diff / per-pixel | Vulkan fg-diff / per-pixel |
  |---|---|---|
  | grid lines light | 0.012 / 0.004 | 0.017 / 0.023 |
  | grid lines dark | 0.040 / 0.005 | 0.179 / 0.070 (coverage-and-colour check) |
  | grid filled light | 0.006 / 0.002 | 0.006 / 0.004 |
  | grid filled dark | 0.005 / 0.002 | 0.019 / 0.007 |
  | markers at 0.6 | 0.010 / 0.002 | 0.048 / 0.012 |
  | markers at 1.0 | 0.010 / 0.002 | 0.063 / 0.016 |
  | robot primitives | 0.008 / 0.005 | 0.012 / 0.023 |

  The showcase sheets (`after_r_*`) put the GL build and QRhi side by side: on the
  frame benchmark's cubes-and-lines and robot scenes, GL vs QRhi-OpenGL went from
  7.2% / 7.7% of pixels off by more than 16 to 0.00% / 0.09%.

Numbers. B's harness (`scene3d_frame_benchmark`, `PJ_S3D_BENCH_FULL=1`, 30
frames): Iris Xe, xcb on a private rootful Xwayland, vsync off, 5 interleaved
rounds, the build slot held; the base binaries were built from this worktree
before the edits. Median, ms:

| Case | GL build | RHI GL before → after | RHI VK before → after |
|---|---|---|---|
| Cloud 10M, orbit (wall) | 19.5 | 29.4 → 19.5 | 31.7 → 21.1 |
| Cloud 10M: float / redock / tab | 95 / 86 / 1.2 | 158 / 131 / 1.7 → 140 / 113 / 1.7 | 171 / 134 / 61 → 157 / 124 / 41 |
| 50-link robot: float / redock / tab | 35 / 26 / 1.1 | 85 / 61 / 2.1 → 74 / 51 / 2.1 | 45 / 17 / 5.4 → 41 / 12 / 6.2 |
| First show, 10M cloud + robot | 159 | 264 (pipelines 32) → 226 (pipelines 25) | 273 → 229 |
| GPU memory, 10M cloud (DRM MiB) | 241 | 310 → 284 | 583 → 554 |
| Sprites, 1M 0.1 m spheres, auto-fit camera (wall) | – | 92.2 → 73.8 | 96.1 → 75.5 |
| Sprites, same camera in both builds (scratch `VpSprites`) | 29.6 | → 34.9 | → 39.5 |

- The 10M cloud frame now equals the GL build's on OpenGL: the depth replay was
  the whole gap (B measured it at +11.3 ms).
- The tab switch was already 1.7 ms on OpenGL at this base (B's 10.8 was the F
  tip). On Vulkan the remaining 41 ms is the top-level's swapchain frame, not the
  view's render, which the Show now skips.
- Vulkan's DRM total rose on the robot and sprite scenes (202 → 302 MiB) while the
  allocator's used bytes fell (86.3 → 85.7 MiB): the multisample depth is now a
  texture, allocated from another memory pool than the renderbuffer, so one more
  allocator block is reserved.

Tests:
- QRhi build, under the compositor: `ctest -L qrhi-opengl` and `-L qrhi-vulkan`
  green (181 and 141, dpr2 included; `rhi_mixed_dock_test` and
  `rhi_dock_lifecycle_test` 60/60 in a rerun after a fix: the media viewer's
  failed state must rebuild its colour target to clear); the display runners one by one:
  harness 25/25, goldens 35/35, point 10/10, marker cache 7/7, model cache 9/9,
  failure 6/6, platform 1/1, on each backend; offscreen `ctest -E
  'scene3d_rhi|pointcloud_golden_test'` 3425/3425.
- GL build: offscreen ctest 3387/3436, the 49 others being the display runners
  that `PJ_REQUIRE_GPU_TESTS` fails offscreen (goldens, the three `_gl_test`s, the
  frame benchmark smoke); all pass under the compositor (68 + 11).
- GL 3.3 floor, locally: `scene3d_rhi_render_test` 25/25 and
  `rhi_dock_lifecycle_test` 11/11 (1 skip).
- WASM (ports 7291/7292): build, Playwright 45/45 and the Scene3D capability
  probe green; WebGL2 reports no multisample textures, so the browser keeps the
  depth replay.

Shared files touched (owners in section 5):
- `scene_view_widget_rhi.{h,cpp}` (P): grid colours into the grid settings, the
  depth-resolve branch in `render()`, `event()`, the sample-count choice in
  `initialize()`, test accessors.
- GL `src/scene_view_widget.cpp` (frozen after F9): the grid colours through
  `look::gridColors()`, byte-identical output (the GL goldens pass unchanged).
- `grid_rhi_pass.*`, `marker_rhi_pass.*` (M), `model.frag` and
  `robot_model_layer.cpp` (S), `scene_entities_layer.{h,cpp}` (M: the constructor
  and a GL test accessor).
- `scene3d_rhi_render_test.cpp` (R: more than static initializers):
  `DepthResolveMatchesTheForcedReplay` and the tonemap test's box colour.
- `pj_scene3D/widgets/CMakeLists.txt`: untouched (the platform runner was
  pre-registered).

Rows for the integrator:
- decision table: the depth replay runs only where QRhi cannot resolve depth
  (D3D11, WebGL); OpenGL/Vulkan/Metal resolve (OpenGL in D32FS8);
- Findings: Qt 6.11's OpenGL depth resolve needs a depth-stencil format; the
  GL build draws the filled grid darker under the corner gizmo;
  `mixed_layout_benchmark`'s GL build draws no cloud;
- Known gaps: a runtime HDR refusal draws its direct pass without MSAA; the
  QRhi view's large sprites are quads (1.2x the GL build's point sprites on
  Iris Xe).

### P5 K: results

What changed (details in `packaging/README.md`, "Render check"):
- **`packaging/smoke/render_check.sh <app> [args]`** (POSIX sh: dash, Git Bash)
  runs `--test-data --layout smoke/render_fixture.pj4.xml --screenshot` with
  `pj.graphics` logging and fresh XDG dirs. It fails unless: the exit status is 0;
  the grab reports the backend; `scene3dRhiCanvas` and `scene2dMediaViewer` each
  log it, and no view logs another; every `<plot>` of the fixture logs one
  `PJ_WASM_PLOT_RHI_READY` on it. The backend comes from `PJ_RENDER_BACKEND`,
  and defaults to D3D11 on Windows, Metal on macOS and OpenGL elsewhere.
- **Fixture:** two plots (`test/sin`, `test/cos`), an empty Scene3D view (grid
  and axes) and a Scene2D image view. The QRhi build saved it itself, through a
  throwaway save-on-settle hook that is now removed. `--test-data` now also writes
  one 64x64 image topic, `test/image` (`MainWindow::populateTestData`). Without an
  image, the media viewer stays on its placeholder page and never creates a QRhi.
- **Wiring:**
  - `deb/smoke_test.sh` and `appimage/smoke_test.sh` run the check under Xvfb,
    after their startup phase. The deb smoke also installs `libgl1-mesa-dri`.
  - `windows-release.yml`, between build and upload, installs the IFW package
    silently, runs the check on the installed exe (`PATH` = System32, WARP),
    then uninstalls with `maintenancetool purge`.
  - `installscript.qs` skips its wizard-page hooks in a command-line run,
    because they dereference `gui`.
- **`scene3d_rhi_app_smoke`** (`pj_app/CMakeLists.txt`) runs the check on the
  build tree's app: QRhi build, Linux, label `qrhi-opengl`. The Linux QRhi
  display step therefore runs it, and `-LE qrhi` and `-E scene3d_rhi` exclude it
  offscreen.
- **AppImage gate:** the AppImage smoke still runs after the upload (unchanged,
  see `linux-appimage-release.yml`). The pre-merge `workflow_dispatch` of both
  release workflows on the cut-over branch is its gate.

Verified locally (QRhi build, llvmpipe):
- `scene3d_rhi_app_smoke` under the compositor: passed, 7.8 s. The same script
  with `PJ_GRAPHICS_API=vulkan PJ_RENDER_BACKEND=Vulkan`: passed.
- Mutations, each of which fails the check:
  - expected backend Vulkan on an OpenGL run ("the grab did not report Vulkan");
  - `--disable-opengl` ("0 of 2 plot(s)");
  - the fixture's image layer bound to a missing topic (the layout is refused,
    exit 1).
- Packages built on the host from the QRhi tree (`build_appimage.sh`, then
  `build_deb.sh`), then run with the release workflow's own `docker run` line in
  a clean `ubuntu:26.04`. Everything passed:
  - every deb phase, including the render check (OpenGL, 2 plots, a 565x393 grab)
    and the removal;
  - the AppImage phases, including its render check.

  The container is `ubuntu:26.04`, not 22.04/24.04, because a host build needs
  glibc 2.43. The host payload also pulls `libmvec.so.1`, which `build_deb.sh`
  does not map; a scratch copy of the script mapped it to libc6. The jammy
  builder does not emit it.
- `pj_app_gui_tests` offscreen (201/201) with the extra test image.

Only CI can verify:
- the jammy-built packages on 22.04/24.04;
- the IFW silent install and uninstall;
- the D3D11/WARP output: stdout and `QT_FORCE_STDERR_LOGGING` from a
  GUI-subsystem exe through Git Bash, and `env PATH=` to a native exe.

Rows for the integrator:
- Known gaps: the AppImage smoke runs after its upload; the release dispatch
  on the cut-over branch is its only gate.
- Findings: `PlotRhiCanvas::backendName()` labelled desktop OpenGL
  `OpenGLES2/WebGL` (C: it now reports `OpenGL` on desktop).

### P5 C (cut-over): results

What changed:
- **The default flips.** `PJ_SCENE3D_RHI` defaults to ON; OFF warns at configure
  time. `build.sh` passes `-DPJ_SCENE3D_RHI=${PJ_SCENE3D_RHI:-ON}` and
  `scripts/windows/build.ps1` the same, on every configure.
- **CI.** `linux-ci.yml`: the main tree is the QRhi build; its xvfb ctest exports
  lavapipe's `VK_ICD_FILENAMES` and runs the whole suite (every `.opengl`,
  `.opengl_dpr2` and `.vulkan` pass); the separate `build-rhi` steps are gone; the
  GL-floor step runs on `build`; a `build-gl` tree (`-DPJ_SCENE3D_RHI=OFF
  -DPJ_BUILD_DEMOS=OFF`) builds `pj_app` and the GL test runners and runs them
  under xvfb (goldens per pixel, `pj_scene3d_gl_tests`, the three shared scene
  runners with their GL-only cases, the hud/tf/shadow GL tests, the two plot GL
  tests, `scene3d_frame_benchmark`). `windows-ci.yml`: the `build` lane is the
  QRhi build (blocking), with the unit suite (`-LE qrhi`, non-blocking step) and
  the D3D11/WARP display step with the adapter and plot greps; `qrhi-d3d11`
  became `gl` (`-DPJ_SCENE3D_RHI=OFF -DPJ_BUILD_DEMOS=OFF`, non-blocking).
  `macos-ci.yml`: the OpenGL fallback step also requires every plot canvas on
  `OpenGL`; the `feat/qrhi-**` push trigger stays until the file is on main.
  pixi, sanitizers, the release workflows and wasm-ci are unedited.
- **`PlotRhiCanvas::backendName()`** reports QRhi's own name on desktop
  (`OpenGL`); only the browser keeps `OpenGLES2/WebGL`. `render_check.sh` no
  longer maps the label.
- **`mixed_layout_benchmark` draws the same scene in both builds.** The GL view
  drew nothing: it needs a transform buffer even in the fixed frame, and the
  fixture bound no `TransformService`. The fixture now binds one (as the app
  does); the benchmark puts the playhead on the cloud's sample and one camera on
  both builds, and `--dump` saves the view's last frame. On llvmpipe, 1M points
  now cost 2051 ms per frame in the GL build (29 ms for 1k), 2898 ms in the QRhi
  build; both dumps show the same green cube.
- `pointcloud_convert_benchmark` (Qt/GL-free) builds in both renderer builds.

Merged-tree gate (F + P + B + M + S + PL + R + K, never built together before),
Mesa 26.0.3 llvmpipe/lavapipe, headless GNOME Shell:
- GL build: offscreen ctest 3436/3436; its display runners (goldens, GL tests,
  frame benchmark) 81/81 under the compositor.
- QRhi build: offscreen `ctest -LE qrhi` 3293/3293; `-L qrhi-opengl` 192/192 and
  `-L qrhi-vulkan` 146/146 (the four same-screen DPR cases skip on one monitor,
  as before).
- WASM: build, Playwright 45/45, Scene3D capability probe.
- No local integration breakage: the streams merged without a compile or test
  fix. CI on the cut-over branch found what no stream could run locally:
  - **D3D11: every mesh drew nothing.** R's `model.frag` dropped its unused
    location-0 input; D3D11 links stages by register (Findings). The input is
    declared again, `mesh_mask.frag`'s older gap is closed the same way, and
    `wasm_shader_constants_test` checks every pack's varyings.
  - **macOS OpenGL (Apple's software 4.1 core): every Scene3D frame crashed.**
    Qt 6.11 reports `MultisampleTexture` from GL 3.0 but allocates with
    `glTexStorage2DMultisample` (GL 4.3), a null entry point there; R's depth
    resolve now also requires GL 4.3, GLES 3.1 or `GL_ARB_texture_storage_multisample`,
    so that context keeps the replay. Found with an lldb backtrace step added to
    the macOS lane's failure path.
  - **Clang 22 + libstdc++ 12 (the sanitizer lanes, the only Clang +
    libstdc++ build):** `std::visit` in `submitScenePass` (Findings).
  - **The QRhi display passes failed instead of skipping** where no window can be
    exposed (pixi's win-64 runner) or on `offscreen` (`./test.sh`): the GUI test
    main now probes once per display pass (`PJ_GRAPHICS_API` set) and skips the
    pass, or fails it under `PJ_REQUIRE_GPU_TESTS=1`, as the GPU gate does.
  - Test allowances, per backend and measured: 1 px line scenes on D3D11 and
    Metal (Known gaps); D3D11's dash ends; Apple GL's plot-frame fringe at DPR 2
    (the parity check now excludes the frame plus one device pixel) and its
    cosmetic dash phase (the cadence check takes the best of three phases);
    `scene3d_rhi_model_cache_test` reads its GLB through `PJ_SCENE3D_FIXTURES_DIR`
    (`__FILE__` is relative on macOS).

Dry runs on the cut-over branch (every lane dispatched; six pushes):
- `linux-ci`: the QRhi tree's whole suite under xvfb (3631, `.vulkan` passes
  included), the GL 3.3 floor (37), the `build-gl` tree (418).
- `windows-ci`: `build` (QRhi) unit suite 3292 and D3D11/WARP display runners
  146 with the adapter and plot greps; `gl` 3435.
- `macos-ci`: Metal, the OpenGL fallback and the unit suite; on a display-runner
  failure the lane now also prints an lldb backtrace of one Scene3D OpenGL case.
- `pixi-ci`: linux-64 and win-64 build the QRhi tree against conda Qt; win-64's
  whole suite passes with the display passes skipped.
- `sanitizers`: asan, msan and tsan green (two audited `tsan.supp` entries for
  `LatestWinsWorker`'s Qt hand-offs; `MeshLoaderTest.CachedLoadServesEveryImport`
  now waits on a deadline instead of 1000 idle event-loop spins).
- `wasm-ci`, and both release workflows with their smokes: the IFW
  install-and-launch render check on D3D11/WARP (2 plots, Scene3D, media viewer),
  the deb and AppImage render checks on Ubuntu 22.04 and 24.04.

**Gate A (final).** B's harness (`scene3d_frame_benchmark`, `plot_canvas_benchmark`,
`mixed_layout_benchmark`) on the finished tree: Iris Xe (RPL-P), Mesa 26.0.3, i915,
AC power, `powersave` governor; xcb on a private rootful Xwayland in a headless
GNOME Shell, vsync off; 5 interleaved rounds, 30 frames each, the build slot held.
Median [min–max]; the rule is RHI ≤ max(1.2x GL, GL + 1 ms) in wall and GUI CPU,
RSS ≤ 1.10x GL, 0 idle frames. Verdicts are on RHI GL, Linux's native backend;
RHI VK is shown for reference. The RTX 4070 column is still open (no NVIDIA
module on this kernel).

| Row | GL build | RHI GL | Ratio (RHI GL) | Verdict | RHI VK |
|---|---|---|---|---|---|
| Cloud 10M static: wall / CPU ms | 20.0 / 1.8 | 19.9 / 1.5 | 1.00x / 0.83x | pass | 21.5 / 1.6 |
| Cloud 10M static: RSS MiB | 371 | 448 | 1.21x | **fail** (retained copy; step B) | 443 |
| Cloud 10M GPU ≤ 550 MiB (DRM; DPR 2) | 245; 595 | 284; 531 | – | pass | allocator 442; 621 |
| Cloud stream 300k/frame: wall ms | 6.0 [6.0–6.9] | 8.0 [5.6–8.5] | 1.33x (limit 7.2) | **fail** | 17.7 |
| Cloud stream: CPU ms / RSS MiB | 5.3 / 276 | 6.1 / 296 | 1.15x / 1.07x | pass | 10.2 / 289 |
| 100k cubes + 1M line vertices, orbit: wall / CPU ms | 33.7 / 33.0 | 28.7 / 3.6 | 0.85x / 0.11x | pass | 32.7 / 3.8 |
| Cubes + lines: RSS MiB | 301 | 438 (peak 490) | 1.46x | **fail** | 434 |
| 10k markers, pan + follow: wall ms | 13.1 | 14.2 | 1.08x | pass | 36.9 |
| Markers: GUI CPU ms | 6.6 [4.7–7.0] | 8.6 [7.1–9.4] | 1.30x (limit 7.9) | **fail** | 11.7 |
| 50-link robot, TF each frame: wall / CPU ms | 2.2 / 1.3 | 3.6 [1.8–4.2] / 1.9 | +1.4 ms (limit 3.2) / pass | **fail** (bimodal) | 4.7 / 1.4 |
| 10 / 100 / 1000 layers: wall ms | 2.5 / 3.8 / 8.7 | 1.9 / 2.1 / 3.4 | ≤ 0.76x | pass | 3.1 / 4.8 / 11.4 |
| 1000 layers: RSS MiB, first show ms | 1017, 4429 | 167, 123 | 0.16x | pass | 196, 105 |
| 10M cloud dock: float / redock / tab ms | 93.9 / 85.0 / 1.2 | 138.0 / 115.5 / 1.7 | 1.47x / 1.36x / +0.5 ms | **fail** (float, redock) | 149.1 / 114.8 / 54.0 |
| 50-link robot dock: float / redock / tab ms | 36.2 / 26.7 / 1.2 | 72.0 / 49.0 / 2.1 | 1.99x / 1.84x / +0.9 ms | **fail** (float, redock) | 38.1 / 12.2 / 6.5 |
| First show, 10M cloud + robot, ms | 147.5 | 221.1 (pipelines 24) | 1.50x | **fail** | 201.3 |
| Idle frames when static, every case | 0 | 0 | – | pass | 0 |
| Plots: dashed 5 x 100k, wall ms (vs Qwt GL) | 14.6 | 13.7 | 0.94x | pass | 18.0 (1.23x) |
| Plots: 50 x 100k, wall ms | 59.3 | 54.1 | 0.91x | pass | 102.6 (1.73x) |
| Plots: tracker only, 10 x 100k, wall ms | 12.5 | 2.2 | 0.18x | pass | 5.9 |
| Plot GPU memory per 1600x900 plot, DPR 1 / 2 (MiB) | 61.9 / 174.1 | 42.6 / 136.6 | 0.69x / 0.78x | pass | 106.1 / 315.8 |
| Streaming plot, 10 x 100k, ≤ 16.7 ms | 18.5 (Qwt GL) | 8.8 | – | pass | 15.1 |
| Mixed: 6 plots + Scene3D, wall / CPU ms | 35.5 / 35.4 | 34.3 / 16.9 | 0.97x / 0.48x | pass | 64.7 / 20.0 |
| Mixed: RSS MiB | 266 | 285 | 1.07x | pass | 269 |

Readings:
- **The mixed row is valid now:** the GL build draws its cloud (the fixture
  fix), and RHI GL is at parity (0.97x wall, half the CPU).
- **Cloud stream (wall)** is bimodal in RHI GL (5.6–8.5 ms) and fails at the
  median; CPU and RSS pass. The plan makes the direct converter (convert straight
  into `PointVertex`) a P5 blocker on this row: **the user's decision**.
- **RSS** fails on the 10M cloud (the retained `PointVertex` copy; step B is
  deferred behind the ADS reparent finding) and on cubes + lines (the marker
  cache's retained expansions).
- **Markers pan + follow (CPU)** rose from B's 7.1 ms: pan and follow rebuild
  the marker layers (Known gaps: the translation-stream follow-up), now with R's
  edge replay. **TF-animated robot (wall)** is bimodal (1.8–4.2 ms) and misses
  the GL + 1 ms limit by 0.4 ms at the median.
- **Reparent and first show** stay the known 1.4–2x: a new top-level means a new
  QRhi and every pipeline rebuilt (inherent, Qt: one QRhi per top-level). B's
  baseline had first show at 2.06x and float at 2.4x; R's shader cache and depth
  resolve brought them to 1.50x and 1.47x.
- Vulkan (opt-in on Linux) misses most CPU-bound and plot rows; it is not a
  shipped default anywhere.

**Gate A sign-off (2026-09-26).** The user signed off the remaining Gate A
failures and decided to ship P5 as it stands. The rows signed off, from the final
run above (RHI GL against the GL build): cloud 10M RSS 1.21x; cloud stream
300k/frame wall 8.0 vs 6.0 ms (1.33x, limit 7.2); cubes + lines RSS 1.46x;
10k markers pan + follow GUI CPU 8.6 vs 6.6 ms (1.30x, limit 7.9); 50-link
TF-animated robot wall 3.6 vs 2.2 ms (limit 3.2); 10M cloud dock float / redock
1.47x / 1.36x; robot dock float / redock 1.99x / 1.84x; first show of cloud +
robot 1.50x. This also settles the direct converter, which the plan made a P5
blocker on the cloud-stream row: it moves to P6 with the other follow-ups in
`pj_scene3D/docs/QRHI_NEXT.md`.

## Release notes (release N)

The 3D view, the plots and the image viewer render through QRhi on each
platform's native graphics API. What users and packagers see:

- **Graphics API per platform:** OpenGL on Linux (3.3 core is enough; the legacy
  renderer needed 4.5), Direct3D 11 on Windows (WARP when there is no GPU, so a
  GPU-less machine still gets 3D), Metal on macOS (OpenGL 4.1 as its fallback).
  `PJ_GRAPHICS_API=opengl|vulkan|d3d11|d3d12|metal` overrides it at run time;
  Vulkan is opt-in on Linux.
- **Rollback levers:** `PJ_GRAPHICS_API`; `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`
  (WARP on Windows); `LIBGL_ALWAYS_SOFTWARE=1` (Mesa on Linux); for plots only,
  Preferences → "GPU canvas" off or `--disable-opengl`, which gives the raster
  canvas. Scene3D has no CPU renderer in either build.
- **For downstream packagers (conda/RoboStack, distributions):**
  `-DPJ_SCENE3D_RHI=OFF` restores the legacy OpenGL build for this release only
  (configure warns); it is removed in the next one.
- **3D view:**
  - the camera fits each newly added layer once (never over a camera restored
    from a layout);
  - solid SceneEntities markers (cube, sphere, cylinder, arrow) cast shadows;
  - translucent draws follow every opaque one, so a translucent layer blends over
    the layers behind it whatever its position in the layer list;
  - wireframe draws triangle edge lines on every backend (back-facing edges of
    closed solids are no longer culled);
  - SceneEntities markers draw at the layer's default 0.6 opacity (the OpenGL
    renderer drew them opaque until a later opacity change);
  - the grid's lines and tiles follow the light/dark theme;
  - robot links and meshes use the physically based lighting path; plain URDF
    primitives are no longer darkened twice.
- **Plots:** one sample per pixel with shader edge anti-aliasing, lines and
  symbols on whole pixels as Qwt draws them, GPU dashes, and the 1 px canvas
  frame. Against the previous OpenGL canvas (Iris Xe, final P5 run): a tracker
  move during playback costs 0.18x, dashed curves 0.94x, dense curves (50 x
  100k) 0.91x, streaming 0.48x wall at a third of the CPU; GPU memory per plot is
  0.69x at DPR 1 and 0.78x at DPR 2.
- **Cheaper paths:** the HUD and hover label are cached; TF triads are instanced
  and cached; uniforms are packed; markers are batched per layer and cached
  across orbit and zoom; model instances are skipped when nothing moved; model
  meshes and textures are kept across model updates (one decode per image);
  depth-cloud back-projection runs off the GUI thread; the 10M-point cloud frame
  matches the OpenGL renderer; the 6-plot + 3D layout costs half the CPU.
- **Floating docks on X11** stay non-native windows
  (`Qt::AA_DontCreateNativeWidgetSiblings`): floating a dock no longer turns the
  docked area into native child windows.

## PR 2 preconditions (release N+1: delete the GL renderer)

PR 2 lands after one release with no rollback. Before it:

- [ ] QRhi twins of `MagentaLinesAppearOnlyWhenVisible` and
  `FloorShadowClearsWhenDataRemoved` as new harness cases, each with a mutation
  check.
- [ ] `hud_overlay_test`, `trail_layer_test`, `occupancy_grid_layer_rebind_test`,
  `scene_view_widget_mouse_gesture_test` and `grid_map_layer_test` moved to the
  shared runners (they build only in the GL block today).
- [ ] CPU twins of the model-pass tests.
- [ ] A final GL vs QRhi A/B (B's harness, Gate A rows).

Then PR 2 deletes:

- the GL Scene3D renderer: `src/scene_view_widget.cpp`, `src/passes/`, `gl/`,
  the GL-only block of `pj_scene3D/widgets/CMakeLists.txt`, the GL
  `framesPresentedForTest()` and the GL branch of `scene3d_frame_benchmark` and
  `mixed_layout_benchmark`;
- `pj_test_main_gl.cpp` and the GL demos (`mesh_viewer.cpp` and the others; check
  each);
- the `QwtPlotOpenGLCanvas` branch of `PlotWidgetBase.cpp`,
  `raster_text_gl_test.cpp` and `plot_canvas_context_recreation_gl_test.cpp`;
- the GL text wrapper: `RasterTextEngine.{h,cpp}` and its CMake entries,
  `raster_text_engine_test.cpp`, `installRasterTextEngines` and its include in
  `main.cpp`;
- the bootstrap widgets (`rhi_bootstrap`, `scene2dRhiBootstrap`), if
  `rhi_dock_lifecycle_test` passes without them;
- the `PJ_SCENE3D_RHI` option and `PJ_RHI_WIDGETS` (and the explicit flag in
  `build.sh` / `build.ps1`);
- the `build-gl` tree in `linux-ci.yml` and the Windows `gl` lane;
- the GL sections of `ARCHITECTURE.md`, constraint 1's GL clauses and the GL
  clause of the S0 line.

PR 2 also enables `PJ_UPDATE_POINTCLOUD_GOLDENS` in the QRhi build (the goldens
stay GL-made until then, because release N's GL lane checks them per pixel), and
drops the `macos-ci.yml` push trigger if it is still there.

## Final Gate A against the GL renderer (2026-09-28)

The last Gate A run against the GL renderer (the "GL build" column, a tree of
the release that still built it), which met the "final GL vs QRhi A/B"
precondition above; its RHI GL and RHI VK columns became the QRhi baseline of
`pj_scene3D/docs/TECHNICAL_NOTES.md` §9. Method: `scripts/perf/gate_a.sh`
(TECHNICAL_NOTES §9, "Harness") on Intel Iris Xe (RPL-P),
Mesa 26.0.3, i915, AC power, `performance` governor with the P-cores capped at
2.4 GHz; xcb on a private rootful Xwayland in a headless GNOME Shell, vsync off
(`vblank_mode=0`, `MESA_VK_WSI_PRESENT_MODE=immediate`); 5 interleaved rounds,
one process per case and configuration, each the median of 30 frames after the
warm-up. Median [min–max] where the spread exceeds 10%. Verdicts are on RHI GL,
Linux's native backend: time within max(1.2x, +1 ms) of the GL build, memory
within 1.10x, the row's own limit where it has one, idle frames 0; RHI VK
(opt-in on Linux) is for reference. Plot rows compare with the Qwt GL canvas.
Run of 2026-09-28, load average 1.3–3.3; the cloud, stream, dock and first-show
rows are from a second run that day (`--only`, load 1.3–2.9), and the two marker
rows from a third, since markers cast no shadows (`--only MarkersPanFollow`,
3 interleaved rounds, load 4.3–8.5). The RTX 4070 column was left open
(`pj_scene3D/docs/QRHI_NEXT.md`).

| Row | GL build | RHI GL | Ratio (RHI GL) | Verdict | RHI VK |
|---|---|---|---|---|---|
| Cloud 10M static: wall / CPU ms | 19.8 / 0.8 [0.7–0.8] | 19.6 / 0.6 [0.6–0.7] | 0.99x / 0.79x | pass | 19.9 / 0.6 |
| Cloud 10M static: RSS MiB ¹ | 255.3 | 247.2 | 0.97x | pass | 294.5 |
| Cloud 10M GPU (DRM MiB; DPR 2 ≤ 550) | 240.9; 581.5 | 206.7; 453.2 | 0.86x; limit 550 | pass | 426.0; 660.7 |
| Cloud stream 300k/frame: wall ms ² | 2.0 [1.9–2.5] | 1.9 [1.8–2.2] | 0.96x | pass | 7.5 [6.8–8.2] |
| Cloud stream: GUI CPU ms / RSS MiB | 1.2 [1.1–1.4] / 260.9 | 1.0 [0.9–1.1] / 256.0 | 0.90x / 0.98x | pass | 1.1 / 303.4 |
| Cloud stream: ingestion ms (not gated) | 1.9 | 1.9 | 0.99x | – | 1.9 |
| 100k cubes + 1M line vertices, orbit: wall / CPU ms ³ | 50.0 [46.7–51.8] / 48.5 [46.5–51.4] | 23.5 / 0.6 [0.6–0.7] | 0.47x / 0.01x | pass | 23.4 / 0.7 |
| Cubes + lines: RSS / peak MiB | 300.2 / 300.2 | 301.7 / 353.6 | 1.00x / 1.18x | pass | 348.5 / 400.3 |
| 10k markers, pan + follow: wall ms ⁴ | 7.8 | 7.6 | 0.97x | pass | 8.2 |
| Markers: GUI CPU ms | 2.5 | 0.9 | 0.34x | pass | 0.9 |
| 50-link robot, TF each frame: wall / CPU ms ⁵ | 2.1 [2.0–2.4] / 1.3 [1.2–1.5] | 2.0 [1.7–2.3] / 0.7 [0.7–1.0] | 0.97x / 0.58x | pass | 3.4 [2.5–4.4] / 0.8 [0.7–0.9] |
| 10 / 100 / 1000 layers: wall ms | 1.8 [1.4–1.9] / 1.9 [1.5–2.0] / 10.9 | 1.9 [1.4–2.1] / 1.9 [1.5–2.1] / 3.3 | 1.06x / 0.99x / 0.30x | pass | 1.7 [1.5–2.7] / 2.2 [2.0–3.6] / 8.9 [7.0–9.3] |
| 1000 layers: RSS MiB / first show ms | 1015.5 / 7014.6 | 216.8 / 146.4 | 0.21x / 0.02x | pass | 299.8 / 254.6 |
| 10M cloud dock: float / redock / hide-show ms ⁶ | 119.9 / 115.5 / 1.6 | 131.3 / 126.6 / 1.9 | 1.10x / 1.10x / 1.18x | pass | 198.0 / 161.2 / 2.2 [2.1–3.0] |
| 50-link robot dock: float / redock / hide-show ms ⁶ | 53.9 / 39.1 / 1.6 | 62.1 / 27.5 / 1.9 [1.9–2.1] | 1.15x / 0.70x / 1.25x | pass | 50.2 / 13.1 / 2.2 [2.0–3.3] |
| ADS dock-area tab switch, 9.9M cloud: to the cloud / to the plot, ms ⁷ | 5.3 / 13.8 [8.7–17.4] | 4.5 / 5.2 [4.6–5.2] | 0.84x / 0.38x | pass | 4.9 [4.3–5.2] / 13.0 |
| First show, 10M cloud + robot, ms ⁶ | 206.1 | 187.6 | 0.91x | pass | 316.0 |
| Idle frames when static, every case | 0 | 0 | – | pass | 0 |
| Plots: dashed 5 x 100k, wall ms ⁸ | 22.8 | 1.6 [1.5–1.7] | 0.07x | pass | 1.8 [1.6–2.0] |
| Plots: 50 x 100k, wall ms ⁸ | 94.1 | 11.2 | 0.12x | pass | 11.4 |
| Plots: tracker only, 10 x 100k, wall ms | 21.2 | 2.6 | 0.12x | pass | 6.2 [5.5–7.4] |
| Plot GPU memory, 1600x900, DPR 1 / 2 (DRM MiB) | 61.8 / 174.1 | 45.0 / 183.2 | 0.73x / 1.05x | pass | 106.1 / 315.8 |
| Streaming plot, 10 x 100k, ≤ 16.7 ms | 29.1 | 10.9 [10.7–12.9] | limit 16.7 | pass | 19.4 |
| Mixed: 6 plots + Scene3D, wall / CPU ms | 59.4 / 56.1 | 37.1 / 8.8 | 0.63x / 0.16x | pass | 49.0 / 9.0 [8.8–10.0] |
| Mixed: RSS MiB | 250.9 | 258.4 | 1.03x | pass | 302.7 |

¹ Both builds settle on one 114 MiB anonymous block for the 10M cloud: the
ObjectStore's wire bytes, which the zero-copy decode reads in place. RHI GL drops
its 12-byte `PointVertex` array (114.4 MiB) once the upload is enqueued
(ARCHITECTURE, "Eviction"); a stream keeps its array. Peak RSS during the load
is 362.7 vs 257.2 MiB (not gated): the array lives until the upload's frame is
submitted. A float, a redock or a move to another dock area converts the same
sample again for the new QRhi: 10.4 ms at 10M, into a fresh array in up to 16
chunks (18.5 ms with 8, the first-touch page faults dominating), part of the
prepare stage in note ⁶. A compressed cloud converts from its decode cache; a
cache holding another sample decodes again off the UI thread, which Cloudini
takes 476 ms for at 10M points (lossy, 1 mm, ZSTD; 610 ms lossless).

² The stream converts each 300k sample (`packPointVertices()` in place, 12-byte
xyz records: the benchmark clouds are coloured by the z axis) in chunks of at
least 256k points on the global thread pool, the GUI thread taking chunks too,
into the retained array, which an unshared array reuses: 0 minor faults per
frame in both builds. The GUI thread's CPU is gated; the pool adds CPU the GUI
thread does not see (about 1 ms per frame at 300k; at 10M, 8 chunks cut 70 ms of
wall for 19 ms more process CPU). The ingestion (the untimed push into the
ObjectStore) is in neither wall nor CPU. On Vulkan the frame waits for its GPU
work inside the offscreen frame (see "Reading the numbers").

³ The shadow map is kept while its inputs hold (ARCHITECTURE, "Shadows"): an
orbit of a static scene renders it once (27.9 → 23.5 ms on RHI GL). The frame is
GPU-bound in the HDR chain: 7.0 ms without it, SSAO and EDL under 0.3 ms each.

⁴ GPU-bound, the HDR chain about 1.4 ms of it. Markers cast no shadows, as on
the GL build, so a pan or follow, which moves the render origin every frame,
renders no shadow map. The GUI thread's work per frame is the origin-only update
of the recurring layer (about 0.55 ms), the upload and `endFrame`.

⁵ i915 settles each configuration at its own clock once warmed up: 1500 MHz for
RHI GL on this row, 600 MHz on Vulkan (every light row), and the GL build either
(its range 2.0–2.4 ms). `cold_ms`, the first frame after a second of idle, is
1.0 ms on RHI GL, 2.2 in the GL build and 5.1 on Vulkan.

⁶ A new QRhi (a float, a redock, a first show) builds only the families the
scene uses, and of their pipelines only the ones it draws (ARCHITECTURE,
"Families are built on demand", "Pipeline registry"): 13 pipelines and 20.7 ms of
creation at the first show, against 50 and 78 ms when every family's pipelines
were built. Where the rest goes (RHI GL, stage sums per operation): the robot
float 62.6 ms is the new top-level and QRhi 12.2, the HDR chain's targets 12.2,
prepare 6.3, submit 7.4 and the first frame's GPU work and swap 25.4; the 10M
cloud float 131.3 ms spends 72.2 of it in the present (the 114 MiB re-upload
executes there) and 30.8 in prepare (the buffer allocation and the 10 ms
conversion of the evicted cloud, note ¹). On Vulkan a pipeline
costs under 1 ms (its pipeline cache), but the re-upload waits in the offscreen
frame (submit 57.9, present 107.3 ms for the cloud).

⁷ Our ADS fork keeps a dock area's hidden tabs parented (TECHNICAL_NOTES §5), so a tab switch
adopts no QRhi, builds nothing and uploads nothing, and a Show whose target
still holds the last frame renders nothing (ARCHITECTURE, "Widget lifecycle and
reparenting"): the switch costs the same at 10.6k and 9.9M points (RHI GL 4.5 /
5.2 ms, Vulkan 4.9–5.1 / 13.0 ms; the Vulkan plot tab's 13 ms is the plot
canvas's own synchronous offscreen frame). The GL build's switch to the plot
waits on its GPU. Hidden tabs keep their GPU memory; DRM MiB with a 1M-point
Scene3D tab and K plot tabs visited (1260x780), upstream ADS → our fork:

| Plot tabs | 1 | 4 | 8 |
|---|---|---|---|
| RHI GL | 132 → 138 | 132 → 158 | 132 → 184 |
| RHI VK | 366 → 302 | 366 → 366 | 366 → 366 (allocator blocks) |
| GL build | 153 → 172 | 149 → 228 | 153 → 303 |

About 6.5 MiB per hidden plot on RHI GL (19 MiB in the GL build); a hidden
Scene3D tab keeps its whole scene (a 10M cloud: 114 MiB of vertices and the HDR
targets). RSS is unchanged.

⁸ The plot rows replot unchanged data every frame. The app's datastore curves
are `RevisionedSeries` (pj_plotting CLAUDE.md, "Dirty layers"), and so is the
benchmark's data: an unchanged replot keeps the RHI canvas's curve layer. With
plain Qwt data (`plot_canvas_benchmark --generic-series`, 3 rounds, wall / canvas
CPU ms) every replot rebuilds:

| Case | Qwt GL | RHI GL | RHI VK |
|---|---|---|---|
| 50 x 100k static | 91.5 / 90.6 | 85.2 / 81.9 | 107.1 / 90.9 |
| 5 x 100k dashed | 22.6 / 22.1 | 21.8 / 21.1 | 26.5 / 22.3 |
| 1M static | 16.1 / 15.6 | 13.8 / 13.4 | 17.2 / 15.1 |

Streaming and zoom/pan change the data or the maps every frame and rebuild in
both modes; the tracker case replots overlays only.

## Findings

- **Qt 6.11.1, `QRhiWidget::grabFramebuffer()` on a never-shown widget works
  once:** each grab creates a new QRhi without calling `initialize()`, so a
  second grab renders with the previous QRhi's resources. The harness grabs a
  hidden widget only once.
- **Qt 6.11.1, OpenGL QRhi reuses stale uniforms in grab-only frames:**
  `QRhiGles2` advances its frame number only in swapchain frames, so its
  "SRB already current" cache survives offscreen frames and a second pipeline
  sharing an SRB can draw with an older camera. The harness presents one real
  frame before each grab. App-side grabs (`MainWindow` screenshot export,
  `PlotPanner`) must do the same once they run on desktop QRhi (P4).
- **Marker shadows:** the GL `MarkerRenderPass` casts no shadows at all (only
  `MeshRenderPass` models do). The QRhi renderer's solid SceneEntities markers
  (cubes, spheres, cylinders, arrows) cast since P3 — parity-plus, not parity.
  (Translucent draws follow every opaque one in the QRhi renderer — S0; the GL
  marker pass still draws in layer order.)
- **Wireframe markers:** the QRhi renderer already draws wireframe as triangle
  edge lines on every backend, so `NonFillPolygonMode` was not adopted. The
  remaining look difference from GL (which culls back-facing wire edges of
  closed solids and keeps the cube edge overlay) is cosmetic.
- **Tiny spheres are driver-dependent** in the GL renderer: below one pixel
  they nearly vanish on NVIDIA MSAA (coverage ~0.19 of llvmpipe's), and ~1.5 px
  spheres shade very differently on Mesa 23 vs 26 (mean colour off by ~80).
  Both renderers now draw spheres below 2 px as flat discs at the disc-mean
  shade (P3 stream A). `shape_spheres_small` is still checked for coverage only:
  the ~1.5 px discs rasterize at a different size per Mesa version, so its mean
  colour is not stable across CI and local runs (`pointcloud_golden_test.cpp`).
- **MSAA orientation differs per backend:** a y-down framebuffer (Vulkan, D3D,
  Metal) puts the 4x sample pattern mirrored on the scene relative to GL's y-up
  render targets, so sub-pixel coverage and diagonal silhouettes differ from the
  GL goldens even on the same Mesa rasterizer (P3 stream A results). The
  QRhi build's golden test tolerates it with a 3x3 neighbourhood rule plus a
  10% per-pixel ceiling; P4 kept that rule and added asymmetric orientation
  checks to the harness instead of a renderer-wide orientation change.
- **ADS floating turns the docked area native** (Qt 6.11.1, our ADS fork):
  `CFloatingDockContainer::show()` calls `winId()` to set X11 properties (on
  every platform), and Qt then marks every child of the dock manager native
  (`enforceNativeChildren`). The docked area becomes a native child window. Under
  Vulkan on Wayland that child surface stopped getting frame callbacks: the main
  window's docks froze once any dock floated, or crashed in the lavapipe
  swapchain. `Qt::AA_DontCreateNativeWidgetSiblings` (set before `QApplication`
  in the QRhi build) keeps them non-native. The GL build still creates the
  native children and is unchanged.
- **ADS reparents docks through a parentless state.** It moves a dock within one
  window, and keeps only the current tab of an area parented. Each such move or
  tab switch counts as a top-level change for a `QRhiWidget`, so it gets
  `releaseResources()` and a fresh `initialize()`. Widgets must rebuild all their
  GPU state from retained CPU data. That is correct but costly for a large 3D
  scene.
- **ADS hides floating docks on Wayland.** Its Linux stays-on-top emulation calls
  `setWindowFlag(Qt::WindowStaysOnTopHint, ...)` on every visible floating
  container when the main window is activated or deactivated. Off xcb that is a
  real flag change, and Qt hides a window whose flags change; ADS does not show
  it again. The dock test removes that event filter; the app is affected on
  Wayland with every backend. The fix
  belongs in the ADS fork: skip the call or re-show the window.
- **Vulkan on Wayland: hiding and re-showing a window within about a frame of a
  present is a protocol error.** Mesa queues the present behind its FIFO barrier.
  The unmap commit waits behind it, and Qt's immediate `get_xdg_surface` then
  fails with "wl_surface already has a buffer committed", which drops the
  connection. Users do not reopen a window that fast, but programmatic
  hide/show can. The dock test waits 100 ms before reopening. Vulkan is opt-in
  on Linux.
- **WARP on GPU-less runners** (Qt 6.11.1 sources): the widget backing store
  creates D3D11 on the first DXGI adapter and retries with
  `PreferSoftwareRenderer` only if that fails; `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`
  selects the adapter flagged `DXGI_ADAPTER_FLAG_SOFTWARE` (WARP, reported as
  `deviceType=Cpu`) up front, so CI sets it rather than relying on adapter order.
- **D3D11 needs `matrixSlice` on unrolled `mat4` vertex inputs:** SPIR-V Cross
  names them `TEXCOORD<first>_<row>`, and without the slice the input layout
  fails (`E_INVALIDARG`). OpenGL, Vulkan and Metal ignore it.
- **Goldens on D3D11/WARP** pass within the QRhi tolerances: worst neighbourhood
  diff 0.019 (`field_viridis_cubes`), worst per-pixel 0.074
  (`shape_cubes_subpixel`), coverage within 0.6%.
- **Metal on GitHub-hosted `macos-14`** (Apple silicon VM): `MTLCreateSystemDefaultDevice`
  returns "Apple Paravirtual device" (not headless), a `CAMetalLayer` drawable
  presents, and QRhi reports `Metal, deviceType=Integrated`. The harness, point
  tests and all 28 goldens pass on it with the `cocoa` QPA: worst neighbourhood
  diff 0.019 (`field_viridis_cubes`), worst per-pixel 0.074
  (`shape_cubes_subpixel`), the same bands as D3D11/WARP.
- **Depot macOS capacity is not elastic:** a `depot-macos-14` job queued 40 min
  before a runner picked it up, so the PR lane runs on GitHub's `macos-14` and
  Depot stays a `workflow_dispatch` choice. The Metal probe passed there too.
- Without a console (ctest, CI), Qt on Windows logs to `OutputDebugString` and
  on macOS to os_log; the native lanes set `QT_FORCE_STDERR_LOGGING=1` so the
  backend lines reach the test log.
- The RHI harness needs a real X or Wayland server; locally, an occluded window
  never presents, so run it under Xvfb or a headless compositor.
- **Moving a camera-relative subtraction into a float shader add is not
  pixel-identical** when the operands are arbitrary doubles, even when the error
  is sub-ulp (P5 M: shadow edges moved by up to 105 px). Only float-valued
  operands, like the grid's origin and vertices, make it exact.
- **Qt 6.11's OpenGL depth resolve needs a depth-stencil format:** it blits depth
  and stencil together, attaching the textures as both, so a stencil-less D32F
  framebuffer is incomplete and the resolved depth stays unwritten. The OpenGL
  chain resolves D32FS8 (P5 R).
- **The GL build draws the filled grid darker under the corner gizmo's square**;
  the QRhi view draws the gizmo after presentation and does not. The scene
  goldens keep the filled grids small enough that the gizmo sits on the backdrop.
- **D3D11 links shader stages by register, not by location.** SPIR-V Cross
  names each varying `TEXCOORD<location>`, but fxc packs a pixel shader's inputs
  into registers in declaration order, so a fragment shader that drops a leading
  input its vertex shader still writes reads every later varying from the wrong
  register. OpenGL, Vulkan and Metal match by location and hide it. P5 R's
  `model.frag` lost `model_color` (location 0) with the legacy branch and every
  mesh drew nothing on D3D11; the input stays declared. `mesh_mask.frag` (inputs
  0, 2, 3 of `model.vert`) had the same gap since P3, feeding the EDL mask's
  alpha test the wrong UV on D3D11. Rule: every pack's varyings are contiguous
  from location 0 (a declared, unused input is enough), so a fragment shader's
  inputs are a prefix of its vertex shader's outputs;
  `wasm_shader_constants_test` checks it from each pack's reflection.
- **Clang 22 with libstdc++ 12 drops `std::visit`'s call for variant alternative
  0** in `submitScenePass`: with a generic lambda the return type cannot be
  deduced (compile error); with a templated visitor the call is never emitted (an
  unresolved symbol at link time; forcing it crashes the frontend with "cannot
  compile this l-value expression yet"). Whichever family is first fails.
  `PreparedFrame::DrawRef` therefore starts with `std::monostate`, visited by a
  plain overload. The sanitizer lanes are the only Clang + libstdc++ build.
- **Pipeline-cache persistence is not reachable through `QRhiWidget` in Qt 6.11:**
  the backing store creates the QRhi (`QPlatformBackingStoreRhiConfig` carries
  only the API and the debug layer), so `EnablePipelineCacheDataSave` cannot be
  set. OpenGL already persists program binaries through Qt's shader disk cache.

## Known gaps

- Markers still rebuild on pan and follow: the render origin is in their cache
  key. The exact follow-up keeps each instance's translation in a separate
  per-frame vertex stream recomputed from cached doubles (about 16 B/instance
  per pan frame instead of a full expansion; P5 M).
- A runtime HDR refusal on a GPU that can run the chain (size, budget, a missing
  HDR variant) draws its direct pass without MSAA: the widget's sample count is
  chosen once per QRhi (P5 R).
- Large point sprites are quads above `kPointListMaxPx` (4 px), about 1.2x the
  GL build's `gl_PointSize` sprites on Iris Xe (P5 R).
- The AppImage smoke runs after its upload; the release workflow's dispatch on a
  branch is its only pre-merge gate (P5 K).
- ADS hides floating docks on Wayland, on every backend (Findings).
- On Apple's software OpenGL (the macOS fallback, CI's only macOS GL), the model
  cache A/B (`CachedFramesMatchUncachedPixelForPixel`) differs from the uncached
  frame on 10 pixels once shadows go off; Mesa (OpenGL, Vulkan) and Metal match
  exactly. The test allows 16 there. Check on a real Mac's OpenGL.
- D3D11 (WARP) rasterizes 1 px lines differently from Mesa: the line-grid
  goldens cover 1.19x the GL golden's pixels (Metal 0.94x), the marker scenes'
  edges differ on 15% of the foreground, the robot scene's grid lifts its
  coverage to 1.09x, and three wide dashed plot lines end one
  dash later than Qwt's raster. The goldens and the dash test carry per-backend
  allowances for exactly these cases.
- None open from P2/P3. The robot in-flight import gap is closed (P4 stream C):
  `RobotModelLayer` keys imports by generation + path, keeps superseded ones
  counted against `maxConcurrentMeshLoads()` until their workers finish and
  drops their results (regression test
  `SourceSwitchKeepsSupersededImportsWithinTheBound`).

## Open questions

- Closed: `PlotRhiCanvas` throughput on desktop-scale data. Every plot row of
  Gate A passes on Iris Xe (RHI OpenGL vs the Qwt GL canvas, final run in "P5 C
  (cut-over): results"): tracker-only replot 0.18x, dashed 5 x 100k 0.94x,
  50 x 100k 0.91x, streaming 10 x 100k 0.48x wall, plot GPU memory 0.69x at DPR 1
  and 0.78x at DPR 2. Vulkan (opt-in) stays slower on dense plots (P5, deferred items).
- Closed, not adopted: `QT_WIDGETS_RHI=1` + `QT_WIDGETS_RHI_BACKEND` (they force
  RHI composition for every top-level). PR 2 removes the bootstrap widgets
  instead, if `rhi_dock_lifecycle_test` passes without them.
- Closed: Metal feature coverage on hosted runners. The macOS lane's logs show
  every view and plot canvas on `Metal, deviceType=Integrated` (the paravirtual
  device), with the harness, point tests and goldens green on it (Findings).
- Pixi CI runs no GUI tests (conda Qt stalls without a GPU); needed before the
  conda channel can be trusted. Since P5 the pixi lanes compile the QRhi build
  against conda Qt (linux-64 and win-64); win-64 runs the whole suite, where the
  QRhi display passes skip (its runner exposes no window).

## Working rules

Set by the maintainer for this migration:

- **Track real time.** Record the actual wall-clock start and end of every
  phase in the time log below, to calibrate the estimates.
- **Check the approach before every phase.** Before starting a phase, ask a
  Codex agent whether the planned approach is correct and whether a simpler
  alternative exists; adjust the plan with its answer.
- **Close every phase with review.** When a phase reaches its exit criteria:
  run `/simplify`, then a final correctness review by both a Claude agent and
  Codex. Extend the tests wherever the reviews show gaps. A phase is not done
  until both reviews are addressed.
- **Parallelize.** Split independent work across agents, each in its own
  worktree and build directory so builds don't collide; merge their diffs back.
  Use Sonnet agents for simple mechanical tasks.
- **Qwt may be modified** if the plot migration needs it (see constraint 2 for
  the conda cost).

## Time log

Actual wall-clock time, to calibrate the estimates above.

| Phase | Started | Finished | Elapsed | Notes |
|---|---|---|---|---|
| P0 | 2026-09-23 21:51 CEST | 2026-09-23 23:40 CEST | ~1 h 50 min | Plan estimate was 1–2 engineer-weeks. Wall-clock with parallel agents; ~45 min of it was WASM builds + Playwright. Non-GL backends verified only by unit tests and reviews, not by rendering. |
| P1 | 2026-09-24 05:46 CEST | 2026-09-24 07:13 CEST | ~1 h 27 min | Plan estimate ≈2 weeks. Three parallel streams (limits, harness, goldens). First real renders on OpenGL and Vulkan (GPU + llvmpipe/lavapipe); found two Qt 6.11.1 grab bugs, no renderer bug. Cross-build point-cloud goldens: 22/28 on QRhi. |
| P2 | 2026-09-24 13:33 CEST | 2026-09-24 18:23 CEST | ~4 h 50 min | Plan estimate 2–3 weeks. Stacked on P1 (#672). Steps B/C/D as three parallel streams, then E; ~12k lines of forks deleted. Reviews found 5 real bugs (TF-only repaint freeze, depth wrong-sample and dataset-replace races, stale robot model, in-flight bound bypass), all fixed with regression tests. |
| P3 | 2026-09-24 20:33 CEST | 2026-09-25 06:45 CEST | ~6 h 30 min active (10 h 12 min wall-clock, incl. a ~3 h 40 min account rate-limit pause) | Plan estimate 3–4 weeks. Stacked on P2 (#673). S0 foundation serial, then five parallel streams (points; voxels + poses; markers + models; grid/gizmo/HUD/picking; GridMap + Trail + frame menu), merged one by one with pixel-exact checks. Point-cloud goldens 22/28 → 28/28 on OpenGL and Vulkan. Two /simplify waves (−409 lines, one pass contract). Reviews found 3 real bugs (edge-sprite frustum reject, unculled fan back faces, pipelines kept across sample-count/format changes) plus a latent D3D storage-view overread in the GPU AABB reducer; all fixed with regression tests. |
| P4 | 2026-09-25 06:50 CEST | 2026-09-25 22:10 CEST | ~15 h 20 min wall-clock (a session restart and heavy build contention included) | Plan estimate 2–4 weeks. Stacked on P3 (#675). Four parallel streams (plots, CI lanes, scene correctness, API policy + docks), then three integration streams (failure state, plot audit, real mixed-dock test) with adversarial verifiers, then /simplify + Claude + Codex reviews. First macOS arm64 build ever (GitHub macos-14, paravirtual Metal). Reviews and CI found real bugs: D3D11 crash on failed pipeline creation, a Scene2D use-after-free on SRB failure, the macOS OpenGL fallback without a core profile, 16 B/sample plot scratch kept alive, DPR-2 test passes that never ran at DPR 2 (CTest splits `;` in ENVIRONMENT). Parallel builds oversubscribed the 30 GB machine (1.7 GB free at peak); later steps used one build slot at -j12. |
| P5 | 2026-09-25 22:15 CEST | 2026-09-27 18:45 CEST | ~18 h 15 min active (44 h 30 min wall-clock, including a paused session from 2026-09-26 16:10 to 2026-09-27 18:26); the documentation pass and the final review fixes included | Stacked on P4 (#680). Foundation F, then parallel streams P (point upload), B (baseline tools), M (marker cache), S (model caches), PL (plot canvas), R (render quality and targets) and K (package render check), merged by C (cut-over: QRhi the default build). The merged tree built and passed locally without an integration fix; CI on the cut-over branch found what no stream could run locally (D3D11 meshes drew nothing: a dropped leading varying; Apple software GL crashed allocating multisample textures; Clang 22 + libstdc++ 12 dropped a `std::visit` call). Gate A signed off with eight failing rows (2026-09-26, below). |
| P6 | 2026-09-27 18:40 CEST | 2026-09-28 13:30 CEST | ~18 h 50 min wall-clock, continuous (two short pauses for a safety-check outage) | Stacked on P5 (#681). Step 0 (measurement harness `scripts/perf/gate_a.sh`, stage timings, perf/off-CPU profiles, RSS attribution), a Codex review of the data, then streams C1 (point clouds: direct parallel converter, compact 12/16 B vertices, SDK 0.35 zero-copy decode, static CPU-copy eviction, plot geometry reuse, anv copy fix), C2 (markers: expiry early-out, origin-only fast path, unexpanded strip geometry, shared uploads) and A (lifecycle: Show skip, ADS keep-parented tab switch in the fork, lazy per-family pipelines and templates, shadow-map reuse). Codex + Fable correctness reviews, /simplify, final Claude + Codex reviews. Gate A fails one row (markers pan + follow, the per-frame shadow pass; deferred by decision). |
