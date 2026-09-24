# Scene3D QRhi migration plan

Status: **P0 merged (#670); P1 in review (#672); P2 done, in review**. Branch `feat/qrhi-p2` (stacked on `feat/qrhi-p1`).

Goal: one QRhi-based 3D renderer for every target — Linux, Windows, macOS
arm64 and WebAssembly — replacing both the desktop `QOpenGLWidget` renderer
(`widgets/src/`) and the separate browser renderer (`widgets/wasm/`).

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
   - every `QRhiWidget` requests `PJ::preferredGraphicsApi()`, OpenGL for now;
   - desktop plots use `QwtPlotOpenGLCanvas` (`PlotWidgetBase.cpp`);
     `PlotRhiCanvas` is WASM-only.
   Scene3D can therefore only leave GL together with Scene2D **and the plots**.
   Floating ADS docks are new top-levels and follow the same rule.
2. **`PlotRhiCanvas` renders only the Qwt items it knows** (curves, grid) and
   applies browser sample budgets. Promoting it to desktop needs an item audit
   (tracker, markers, zoom rubber band, legend, panner snapshot, export, print)
   and desktop budgets. Patching Qwt is allowed if a gap needs it, but conda
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
   packs for every backend (`wasm/shaders/bake.sh`) and dropped the separate
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
| macOS arm64 (conda, later dmg) | Metal | runner's paravirtual Metal (to verify) |
| WebAssembly | OpenGL / WebGL2 | Playwright (existing) |

## Technical decisions

| Problem | Decision |
|---|---|
| `gl_PointSize` absent on D3D | Instanced camera-facing quads on every backend |
| Non-float scalar vertex fields | 16/32-bit: `UShort`/`SShort`/`UInt`/`SInt` inputs (gated on `IntAttributes`) + `float()` in the shader; 8-bit: `UNormByte`, de-normalised (and sign-fixed) in the shader. Keeps the zero-copy upload |
| MSAA depth resolve (never on D3D) | The WASM single-sample depth/mask replay everywhere; `ResolveDepthStencil` as a later fast path |
| Mesh-mask MRT | WASM's separate mask pass |
| GL NDC / Y conventions | Folded into CPU matrices: `clipToTextureMatrix()` (`rhi_conventions.h`) maps corrected clip space to render-target texture space (u, v, stored depth), so shaders never remap. The fullscreen triangle has a second `-DPJ_FLIP_Y` bake for D3D/Metal (kept over `gl_FragCoord` UVs, which assume input and output textures of equal size — false once `render_scale` lands). SSAO normals take `textureVSign()` |
| Compute AABB | Only where `QRhi::Compute`; existing CPU fallback otherwise |
| Wireframe markers | `NonFillPolygonMode` where available; edge lines otherwise |
| QPainter HUD / hover label | Upload the existing CPU-rasterised `QImage` as a texture quad |
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
  `maxImageDimension3D`); D3D/Metal alongside their CI lanes in P4.
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
- P3: one owner for scene bounds + camera fit (today the GL view gets them
  from the dock, the QRhi view owns its own); rename `widgets/wasm/` to
  `widgets/rhi/` (it is the desktop QRhi view too).
- P5 (performance): drop the RHI CPU copies of data already uploaded, convert
  clouds straight to `PointVertex` (no intermediate `DecodedPointCloud`), and
  share primitive meshes (URDF box/cylinder/sphere) across layers.

**P3 — Passes + parity (3–4 weeks).**
- Split the monolith into passes. Port GridMap, Trail (standalone, the pose
  layer's embedded one, the "Create Trail" row and the frame right-click menu,
  which needs frame picking in the QRhi view), cube hexagon fan /
  sprite LOD / colour hoist, compute AABB (gated), axis corner gizmo (then drop
  the golden test's `kGizmoRect` exclusion), HUD, hover
  label, `render_scale`, AgX/Neutral tonemaps.
- Exit: corpus screenshots match GL within tolerance; GL pass tests re-expressed
  as QRhi offscreen tests. Point clouds: all 28 scenes of
  `widgets/tests/fixtures/golden/pointcloud/` (goldens captured from the GL
  renderer in P1, enabled in the RHI build) pass. At P1 the RHI renderer failed
  6: `shape_cubes_subpixel` (hexagon fan / sprite LOD), `color_rgb_cubes`,
  `field_viridis_cubes`, `depth_two_clouds` (cube edges and shading),
  `field_outside_range_translucent`, `field_outside_range_cubes` (faded
  out-of-range colour/blend).

**P4 — Plots and backend switch (2–4 weeks).**
- `PlotRhiCanvas` on desktop: multi-backend shaders, item audit, desktop
  budgets, performance vs `QwtPlotOpenGLCanvas` on large curves; raster canvas
  stays as fallback.
- Flip Windows to D3D11, macOS to Metal; floating-dock matrix (float/redock,
  split, sibling destruction, mixed-DPI moves).
- Exit: required backend CI lanes green on all three desktop OSes.

**P5 — Performance and cut-over (1–2 weeks).**
- Frame-time/memory budgets met; RHI default; legacy GL renderer kept one
  release behind the option, then `gl/`, GL passes and `QOpenGLWidget` deleted.
- Package smoke tests: AppImage, .deb, IFW, conda/RoboStack, browsers.

Total: **~11–17 weeks**; plot parity is the least certain part.

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
- **RHI renderer parity gaps (P3):** translucent layers are not sorted after
  opaque ones (the GL marker pass has the same limit); only model meshes cast
  shadows, not SceneEntities cubes.
- **Tiny spheres are driver-dependent** in the GL renderer: below one pixel
  they nearly vanish on NVIDIA MSAA (coverage ~0.19 of llvmpipe's), and ~1.5 px
  spheres shade very differently on Mesa 23 vs 26 (mean colour off by ~80).
  P3: draw spheres below ~2 px as flat discs, then give `shape_spheres_small`
  its full golden check back.
- The RHI harness needs a real X or Wayland server; locally, an occluded window
  never presents, so run it under Xvfb or a headless compositor.

## Known gaps

- `RobotModelLayer::resetModelState` drops in-flight mesh imports on a source
  reset while their workers keep running (results are discarded by generation,
  but rapid source changes can accumulate work). SceneEntities keeps superseded
  work counted until it finishes; the robot layer should do the same.

## Open questions

- `PlotRhiCanvas` throughput on desktop-scale data.
- Whether QRhi D3D selects WARP on GPU-less runners by itself.
- `QT_WIDGETS_RHI=1` + `QT_WIDGETS_RHI_BACKEND` force RHI composition for every
  top-level and win over per-widget requests in 6.11.1 (Codex, from the Qt
  sources). They could replace the bootstrap widgets; deferred as a separate
  behaviour change.
- Metal feature coverage on hosted macOS runners.
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
