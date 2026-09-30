# Scene3D QRhi migration plan

Status: **P0 merged (#670); P1 (#672), P2 (#673) and P3 in review**. Branch `feat/qrhi-p3` (stacked on `feat/qrhi-p2`).

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
| macOS arm64 (conda, later dmg) | Metal | runner's paravirtual Metal (to verify) |
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
  stays as fallback.
- Flip Windows to D3D11, macOS to Metal; floating-dock matrix (float/redock,
  split, sibling destruction, mixed-DPI moves).
- Decide the offscreen orientation: render every offscreen target in GL
  orientation on all backends (winding, texture-space matrices, present flip)
  so y-down backends (Vulkan, D3D, Metal) put MSAA samples where the GL goldens
  do, or keep the golden test's 3x3 neighbourhood rule with its per-pixel
  ceiling (P3 stream A kept the rule; the change is renderer-wide).
- Generalise the depth-replay skip that the faded point half has to every draw
  that writes no depth (translucent markers and models, occupancy, translucent
  voxels): they leave no coverage in the GL composite either. A behaviour change
  against today's QRhi output, so it needs golden runs (P4 or P5).
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
  10% per-pixel ceiling; a renderer-wide orientation change is a P4 item.
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
| P3 | 2026-09-24 20:33 CEST | 2026-09-25 06:45 CEST | ~6 h 30 min active (10 h 12 min wall-clock, incl. a ~3 h 40 min account rate-limit pause) | Plan estimate 3–4 weeks. Stacked on P2 (#673). S0 foundation serial, then five parallel streams (points; voxels + poses; markers + models; grid/gizmo/HUD/picking; GridMap + Trail + frame menu), merged one by one with pixel-exact checks. Point-cloud goldens 22/28 → 28/28 on OpenGL and Vulkan. Two /simplify waves (−409 lines, one pass contract). Reviews found 3 real bugs (edge-sprite frustum reject, unculled fan back faces, pipelines kept across sample-count/format changes) plus a latent D3D storage-view overread in the GPU AABB reducer; all fixed with regression tests. |
