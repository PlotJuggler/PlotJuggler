# Scene3D QRhi migration plan

Status: **P0 done; P1 next**. Branch `feat/qrhi-migration`.

Goal: one QRhi-based 3D renderer for every target — Linux, Windows, macOS
arm64 and WebAssembly — replacing both the desktop `QOpenGLWidget` renderer
(`widgets/src/`) and the separate browser renderer (`widgets/wasm/`).

## Why

- macOS caps OpenGL at 4.1 core (no compute, no debug output). The GL 4.1
  fallback keeps 3D alive there, but Metal is the native path.
- Windows ships software GL 1.1 without a driver; D3D11 (and WARP in CI) is
  always present.
- Two renderers, two layer sets (7 WASM layers, ~6.2k lines, vs 9 desktop
  layers, ~7.4k lines) and two docks (1.4k vs 2.0k lines) drift apart: the WASM
  point-cloud layer persists 26 XML keys, the desktop one 32; GridMap and Trail
  never reached the browser.

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
   layers are CPU-only data providers (`WasmPointRenderable`,
   `WasmModelRenderable`), the renderer owns all GPU state — but as one
   4.1k-line widget (`initialize()` 570 lines, `render()` 1730, ~45 pipeline
   members).
5. **The WASM renderer assumed GL conventions** (`depth*2-1`, `*0.5+0.5`
   shadow lookups, an un-flipped fullscreen triangle, raw GLES calls). Resolved
   in P0: see the "GL NDC / Y conventions" decision.
6. **Browser budgets** (`core/*_budget.h`, 256 MiB HDR per view) would turn off
   HDR/SSAO/EDL at 4K on desktop and truncate large clouds.
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
- `PJ_SCENE3D_RHI` option, independent from `PJ_TARGET_WASM` (the two
  renderers share class names, so a build picks one).
- `SceneLimits` policy replaces the browser caps on desktop.
- Real per-device 3D-texture limit on Vulkan/D3D/Metal (P0 assumes 2048
  outside OpenGL; QRhi 6.11 reports no 3D limit).
- Offscreen QRhi test harness (`QRhiTextureRenderTarget`) run on llvmpipe,
  lavapipe, WARP and Metal from day one; `--screenshot` diff against the GL
  renderer on a fixed comparison corpus (large clouds, trails, grid maps,
  transparency, shadows, mixed docks).
- Exit: Linux RHI build renders the corpus in CI.

**P2 — One layer set, one dock (2–3 weeks).**
- Desktop layers implement the renderable interfaces; the GL build keeps its
  passes, so both renderers consume the same layers.
- Delete the WASM layer/dock forks and most `PJ_TARGET_WASM` branches in
  `Scene3DConfigPanel`; one XML format.
- Exit: Playwright + desktop persistence tests green.

**P3 — Passes + parity (3–4 weeks).**
- Split the monolith into passes. Port GridMap, Trail, cube hexagon fan /
  sprite LOD / colour hoist, compute AABB (gated), axis corner gizmo, HUD, hover
  label, `render_scale`, live-edge drive, AgX/Neutral tonemaps.
- Exit: corpus screenshots match GL within tolerance; GL pass tests re-expressed
  as QRhi offscreen tests.

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
