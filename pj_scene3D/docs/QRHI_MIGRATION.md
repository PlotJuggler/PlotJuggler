# Scene3D QRhi migration plan

Status: **P0 merged (#670); P1 (#672), P2 (#673), P3 (#675) and P4 in review**. Branch `feat/qrhi-p4` (stacked on `feat/qrhi-p3`).

Goal: one QRhi-based 3D renderer for every target — Linux, Windows, macOS
arm64 and WebAssembly — replacing both the desktop `QOpenGLWidget` renderer
(`widgets/src/`) and the separate browser renderer (`widgets/rhi/`, named
`widgets/wasm/` until P3).

## Why

- macOS caps OpenGL at 4.1 core (no compute, no debug output). The GL 4.1
  fallback keeps 3D alive there, but Metal is the native path.
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
| Non-float scalar vertex fields | 16/32-bit: `UShort`/`SShort`/`UInt`/`SInt` inputs (gated on `IntAttributes`) + `float()` in the shader; 8-bit: `UNormByte`, de-normalised (and sign-fixed) in the shader. Keeps the zero-copy upload |
| MSAA depth resolve (never on D3D) | The WASM single-sample depth/mask replay everywhere; `ResolveDepthStencil` as a later fast path |
| Mesh-mask MRT | WASM's separate mask pass |
| GL NDC / Y conventions | Folded into CPU matrices: `clipToTextureMatrix()` (`rhi_conventions.h`) maps corrected clip space to render-target texture space (u, v, stored depth), so shaders never remap. The fullscreen triangle has a second `-DPJ_FLIP_Y` bake for D3D/Metal (kept over `gl_FragCoord` UVs, which assume input and output textures of equal size — false once `render_scale` lands). SSAO normals take `textureVSign()` |
| Compute AABB | Only where `QRhi::Compute`; existing CPU fallback otherwise. Compute packs bake GLSL ES 310 / GLSL 430 (no ES 300) |
| Wireframe markers | Edge lines everywhere (already built; `NonFillPolygonMode` would split WebGL from desktop for a cosmetic gain) |
| QPainter HUD / hover label | Upload the existing CPU-rasterised `QImage` as a texture quad |
| Plot items without a geometry translation | Rasterise the item's own QPainter `draw()` into the canvas text atlas, in its Z slot (`PlotMarkersItem`, legend, labels, unknown items); no Qwt patch |
| Plot canvas choice (QRhi build) | `PlotRhiCanvas` when "GPU canvas" is on, raster `QwtPlotCanvas` otherwise; never `QwtPlotOpenGLCanvas` (it would pin the window to OpenGL) |
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
- P5 (performance): drop the RHI CPU copies of data already uploaded, convert
  clouds straight to `PointVertex` (no intermediate `DecodedPointCloud`), and
  share primitive meshes (URDF box/cylinder/sphere) across layers.

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
  camera; the GL view keeps its no-auto-fit behaviour until P5 removes it;
  `rhi/passes/occupancy_rhi_pass.{h,cpp}` as the pattern. Golden images and
  harness images pixel-identical to the pre-S0 build on OpenGL and Vulkan;
  counters identical. New harness case: a translucent layer first in the dock
  still blends over the opaque one behind it (fails without the phase order).
- Then four parallel streams on the pass structure:
  - points: hexagon fan, sub-pixel sprite LOD, colour hoist, two-partition
    faded out-of-range draw, spheres below ~2 px as flat discs (restore
    `shape_spheres_small`'s full check), instanced quads with a 10M-point
    benchmark gate (keep a gated point fast path only if measured), compute
    AABB gated on `QRhi::Compute` (compute shaders get their own bake profile).
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
  `rhi_dock_lifecycle_test` and `rhi_mixed_dock_test`. `windows-ci.yml`'s
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
- Marker and model instances are rebuilt and re-uploaded every frame because
  the render origin is baked into them; move the origin into the view uniform so
  an unchanged layer keeps its uploaded instances.
- `drawsFor` searches the frame's draws linearly per logical slot (O(L²) in the
  layer count), and `ScenePipelineRegistry::resolve` scans its entries per draw;
  index both when layer counts make it measurable.

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
  shade, and `shape_spheres_small` has its full golden check back (P3 stream A).
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

## Known gaps

- None open from P2/P3. The robot in-flight import gap is closed (P4 stream C):
  `RobotModelLayer` keys imports by generation + path, keeps superseded ones
  counted against `maxConcurrentMeshLoads()` until their workers finish and
  drops their results (regression test
  `SourceSwitchKeepsSupersededImportsWithinTheBound`).

## Open questions

- `PlotRhiCanvas` throughput on desktop-scale data: measured in P4 (stream A
  results); dashed and many-dense-curve plots remain slower than the Qwt GL
  canvas.
- `QT_WIDGETS_RHI=1` + `QT_WIDGETS_RHI_BACKEND` force RHI composition for every
  top-level and win over per-widget requests in 6.11.1 (Codex, from the Qt
  sources). They could replace the bootstrap widgets; deferred as a separate
  behaviour change.
- Pixi CI runs no GUI tests (conda Qt stalls without a GPU); needed before the
  conda channel can be trusted.

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
