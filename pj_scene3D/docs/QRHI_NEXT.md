# pj_scene3D — QRhi renderer: next steps

The work that remains on the QRhi renderer, which ships as the default build. The
as-built design is [ARCHITECTURE.md](./ARCHITECTURE.md); the measurements and
rules this plan refers to are in [TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md). How
the renderer got here is archived in
[`docs/archive/QRHI_MIGRATION.md`](../../docs/archive/QRHI_MIGRATION.md).

## Release N notes

For the release notes and the PR description:

> The 3D view, the plots and the image viewer now render through Qt's RHI on
> each platform's native graphics API: OpenGL on Linux (3.3 is enough), Direct3D 11
> on Windows (with the WARP software renderer when there is no GPU, so a GPU-less
> machine still gets 3D), and Metal on macOS (OpenGL 4.1 as its fallback).
>
> - **Overrides:** `PJ_GRAPHICS_API=opengl|vulkan|d3d11|d3d12|metal` picks the API
>   at run time (Vulkan is opt-in on Linux); `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`
>   forces WARP on Windows; `LIBGL_ALWAYS_SOFTWARE=1` forces Mesa's software
>   renderer on Linux. For plots only, Preferences → "GPU canvas" off (or
>   `--disable-opengl`) gives the raster canvas. The 3D view has no CPU renderer.
> - **3D view:** a translucent layer blends over the layers behind it whatever
>   its position in the layer list; wireframe shows every triangle edge on every
>   backend; markers draw at the layer's default 0.6 opacity; the grid follows the
>   light/dark theme; plain URDF primitives are no longer darkened twice.
> - **Plots:** crisper lines (one sample per pixel with shader anti-aliasing,
>   axis-aligned lines on whole pixels as Qwt draws them), GPU dashes, and the 1 px
>   canvas frame. Against the previous OpenGL canvas: a tracker move during
>   playback costs 0.18x, dashed curves 0.94x, dense curves 0.91x, streaming half
>   the time at a third of the CPU; GPU memory per plot is 0.69-0.78x.
> - **Performance:** a 10-million-point cloud renders as fast as before, a layout
>   of 6 plots and a 3D view takes half the CPU, and many-layer scenes are faster
>   and far lighter (1000 layers: 0.16x the memory), and a large cloud holds no
>   more memory than before. Floating a 3D view takes 10-15% longer than
>   before.
> - **Floating docks on X11** stay non-native windows: floating a dock no longer
>   turns the docked area into native child windows.

## Golden writer

The golden corpus (`pointcloud_golden_test`, TECHNICAL_NOTES §7) compares against
images `PJ_UPDATE_POINTCLOUD_GOLDENS=1` wrote from QRhi OpenGL on llvmpipe (the
writer refuses every other backend and driver). Every lane passes against the
current images: Mesa 26 llvmpipe, lavapipe, Iris GL and Iris Vulkan locally, D3D11
(WARP) and Metal in CI. Left:

- [ ] Refresh the D3D11 and Metal measured rows in TECHNICAL_NOTES §7 from a CI
  run's output (their ratios are still those measured against the previous images).

## P6: performance follow-ups

Gate A (TECHNICAL_NOTES §9: the Iris Xe baseline every change is gated against);
what remains is below.

**Float, redock and first show** pass (TECHNICAL_NOTES §9, note ⁶). What a new
QRhi still costs on OpenGL, for the robot float (62.6 ms): the new
top-level and QRhi 12 ms, the HDR chain's targets 12 ms, the first frame's GPU
work and swap 25 ms. Candidates:

- keep a dock parented when ADS moves it to another area of the same window
  (`removeDockWidget()` → `setDockArea(nullptr)` unparents it first): the same
  kind of ADS fork change as the dock-area tab switch (TECHNICAL_NOTES §5), so a
  drag within the window rebuilds nothing;
- pipeline-cache persistence, if the pinned Qt lets a `QRhiWidget` set it (see below);
- precompiled packs (`qsb -c`), which need fxc and the Metal toolchain in a
  per-platform bake.

**Retained CPU copies.** A static point cloud drops its vertex array once
uploaded and converts it again for a new QRhi (ARCHITECTURE, "Eviction"). Left:

- *The load peak.* The array lives until its upload's frame is submitted: the
  10M load peaks at 362.7 MiB, 247.2 once settled (TECHNICAL_NOTES §9, note ¹).
- *Depth clouds, marker expansions and meshes* keep their arrays. A depth cloud
  is at most a few MiB; the other two would need the same re-derive path.
- *Integer zero-copy.* Uploading the wire bytes and decoding in the vertex stage
  would skip the conversion (the stream's and the re-derive's), against a vertex
  layout and shader variant per wire datatype, NaN rejection in the shader, a
  bounds pass and a full golden re-verification (TECHNICAL_NOTES, "Integer
  zero-copy analysis").

**Cloud stream process CPU.** The GUI thread passes; the pool spends about 1 ms
more CPU per 300k frame than a serial conversion (TECHNICAL_NOTES §9, note ²). If
total CPU becomes the budget, convert serially below 1M points.

**OpenGL keeps a settled frame's uploads.** QRhi's GL backend retains each
upload's data in the command buffer until the next `beginFrame`, so an idle view
holds its last frame's uploads (53 MiB after a pan of 1.1M strip points).
The fix is in Qt (release the retained data once `executeCommandBuffer` has
run); on our side only an extra frame would release it, which the idle-frame
rule forbids.

**View lifecycle.** A dock-area tab switch and a hide/show rebuild and render
nothing (TECHNICAL_NOTES §9, note ⁷). Left:

- *Hidden tabs keep their GPU memory* (about 6.5 MiB per hidden 1260x780 plot on
  RHI GL, the whole scene for a Scene3D tab). Add a release policy (hidden for
  minutes, or over a budget) if users keep many heavy tabs.
- *Content invalidation is explicit* (`SceneViewWidget::invalidateContent()`):
  a new path that changes what the view draws (in particular a mutation through
  the non-const `camera()`, `compositeParams()` or `meshShadingParams()`) must
  call it, or a hidden view presents its retained frame on the next Show.
- *The i915 clock policy* differs per configuration (TECHNICAL_NOTES §9, note ⁵),
  which moves light rows and the first frame after idle (`cold_ms`); why the boost
  differs is open.
- *HDR chain cost.* The 4x multisample RGBA16F scene pass with its resolve and
  present is most of a GPU-bound frame (cubes: 23.5 ms, 7.0 ms without the chain);
  SSAO and EDL cost under 0.3 ms. Lowering it (fewer samples, a smaller format) is
  a quality tradeoff to decide, not an exact optimization.

**Vulkan 50 x 100k plot, full rebuild: 107.1 ms vs RHI GL's 85.2.** An unchanged replot of datastore curves reuses the curve layer
(11.4 ms, TECHNICAL_NOTES §9 note ⁸). Of the rebuild's 16 ms over its render()
CPU (90.9 ms, 9 ms more than OpenGL's recording), all is the wait
for the GPU in `QRhi::endOffscreenFrame()`: on Vulkan a `QRhiWidget` frame is an
offscreen frame, which submits and waits for its fence (`drmSyncobjWait`), so wall
= CPU + GPU, where OpenGL overlaps the GPU with the next frame. Qt's design, not
ours; reproducer: any `QRhiWidget` with about 20 ms of GPU work per frame, timed
back to back on Vulkan against OpenGL.
- *Anv buffer copies (fixed on our side; Mesa 26.0.3).* Mesa's anv records
  `vkCmdCopyBuffer` by searching the copy's element count for a 2D/3D surface
  shape on the CPU (an `idiv`-stepping divisor search at libvulkan_intel.so
  +0x410060 in Ubuntu's 26.0.3-1ubuntu1, no debug symbols published): a count
  with a large prime factor costs milliseconds per copy. Reproducer: upload a
  static buffer of 1000003 x 16 bytes through QRhi (one vkCmdCopyBuffer) and time
  `endOffscreenFrame()` against 1000000 x 16. Arrays the uploader owns are padded
  past 64 KiB to a 64 KiB multiple (`PJ::padForUpload()`: point and depth clouds,
  marker expansions) and upload as one copy;
  `PJ::uploadStaticBufferInPieces()` (pj_widgets) issues the leading 64 KiB
  multiple and the remainder of an unpadded one (the plot canvas, grid, pose and
  trail uploads) as two copies into a Static buffer. Model meshes upload into
  Immutable buffers, one copy each (QRhi's Vulkan backend gives each copy into an
  Immutable buffer a full-size staging buffer of its own), so a mesh of an awkward
  size still pays the search once.
- *Per-curve reuse* (splicing an unchanged curve's retained geometry into a
  rebuild): not taken. A rebuild happens when data or maps change; live streaming
  refits the X axis on every ingest (every curve's map moves) and a zoom moves
  them all, so an unchanged curve with unchanged maps is rare outside file
  loading. Revisit with a use case (several plots sharing a slowly updated axis).

**Dense large sprites: the tab-switch lattice (215³ = 9.9M spheres of 0.1 m in a
1 m cube, about 50 px each) takes 400 ms per frame on RHI GL (414 on Vulkan).**
Attribution (one process each, `mixed_layout_benchmark
--plots 0 --cloud-side 215`): SSAO and EDL cost nothing measurable; the depth
resolve is used (a forced replay costs 673 ms); the whole HDR chain is 125 ms
(275 ms with the direct pass); disabling blending for the opaque draw changes
nothing. The gap is the sprite geometry: drawn as a point list (one vertex per
point, `gl_PointSize`), the same frame takes 311 ms (Vulkan 301). Options, none pixel-identical:
- *Point list above 4 px.* The fastest, but a point is culled whole
  once its centre leaves the view (the edge pop `kPointListMaxPx` avoids) and
  sprite rasterization differs from the quads the goldens were matched with.
- *One triangle per sprite* (the disc's circumscribed triangle, 3 vertices instead
  of 4): a quarter fewer vertex invocations for 30% more rasterized area; square
  points would keep quads. The interpolated corner differs by rounding at the disc
  edge.
- *Colour only at the provoking vertex* of the quad strip: exact, but the flat
  varyings come from a different vertex per backend (last on OpenGL, first on
  Vulkan, D3D and Metal); not tried.

**RTX 4070 measurements.** Gate A was measured on the Iris Xe only; the host
kernel had no NVIDIA module. Re-run `scene3d_frame_benchmark`
(`PJ_S3D_BENCH_FULL=1`), `plot_canvas_benchmark` and `mixed_layout_benchmark`
when it is available, and add the column to TECHNICAL_NOTES §9.

Deferred with a trigger, not failing any row:

- *Shared primitive meshes* (URDF box/cylinder/sphere) across layers: when more
  than 32 MiB is duplicated across repeated robots.
- *Indexing* `drawsFor` (a linear search per logical slot, O(L²) in the layer
  count): when the 1000-layer case exceeds 1 ms or 10% of `prepareFrame` CPU. A
  finger index needs a `draws[hint-1].layer != layer` check.
- *Large sprites* above 4 px are quads, about 1.3x a point list's frame (the
  lattice above): revisit with a use case that draws many large sprites.
- *Runtime HDR refusal* draws without MSAA: revisit if users hit the HDR byte
  limits.

## P6: the pinned Qt

The pinned Qt (`PJ_QT_VERSION` in `versions.env`; `docs/QT_NOTES.md`) is an LTS. QRhi has no cross-minor
compatibility guarantee, so the upgrade is one change across every channel:

- [x] `PJ_QT_VERSION` in `versions.env`, the `qt6-main` pin in `pixi.toml` (Windows,
  macOS and WASM CI read `versions.env`), and `PJ_EMSCRIPTEN_VERSION`.
- [x] Every `.qsb` pack re-baked with the pinned Qt's `qsb`, which reproduces the committed
  bytes. The WASM configure hash-checks them; re-bake
  with `pj_scene3D/widgets/rhi/shaders/bake.sh`, `pj_plotting/widget/shaders/bake.sh`,
  the Scene2D packs (`pj_scene2D/docs/TECHNICAL_NOTES.md`, "Shader Toolchain") and
  the capability probe whenever a shader changes.
- [ ] Re-test the two grab workarounds before removing either: the never-shown
  `grabFramebuffer()` that works once, and the OpenGL stale uniforms in grab-only
  frames (`grabPresentedFrame()` presents a real frame first). Keep each until a
  test shows the pinned Qt fixed it.
- [ ] Re-check the other workarounds of TECHNICAL_NOTES §3: the OpenGL depth
  resolve on D32F, the `MultisampleTexture` GL 4.3 gate, the Metal probe
  fallback, the synchronous Show re-render, and whether a `QRhiWidget` can now
  enable pipeline-cache persistence.
- [ ] Re-run the golden corpus, the harness and Gate A on every lane.
- Not needed now: the pinned Qt's indirect draws and device-fault reporting. Revisit with a
  use (the voxel compaction path, a device-lost recovery).

## Deferred (with triggers)

Not scheduled; each item names what would bring it back.

- **Raw `delete` in the passes** (about 110 left): convert a pass to owning
  pointers when it is next edited.
- **Test-only surface in production classes:** the layer and pass `*ForTest`
  accessors, the plot canvas probe and fault-injection code, and the remaining
  `MainWindow` `wasmProbe` members. Move them behind the test boundary
  (`SceneViewTestAccess`, the RHI test hooks) when their class is next touched.
- **Header moves under `rhi/` and `*_rhi` file naming:** churn only; only alongside a
  change that already touches those files.
- **`findDraws` indexing:** only if the 1000-layer Gate A row regresses.
- **Retention unification and a shared QRhi widget base:** not planned; revisit
  for a new retention case or a fourth GPU widget.
- **Browser stage fallbacks through the test hooks:** the browser acceptance
  probes hold a quality stage down with `forceStageFallback()`, whose capability
  error names the override (`forced_off`), not with a fault a driver could
  raise. Moving them to the RHI test hooks needs the hooks in the WASM probe
  build and the browser specs matching the real failure messages; do it when a
  browser test has to exercise a stage's real failure path.
- **Per-request cancel in `UrlFetcher`:** a request aborts only with its
  fetcher, so `MeshLoadQueue` retires its fetcher (a new one, with new
  connections, serves the next fetch) once none of its fetches is wanted.
  Per-request cancel would remove the retirement; worth it if reloads with many
  remote meshes show the reconnect cost.
- **Named pipelines:** the shader fault (`PJ_RHI_TEST_FAIL_SHADER`) fails every
  pipeline built from a shader, in every view. A name in `PipelineDesc` plus a
  per-view `pjTestFailPipeline` would fault one pipeline of one view; needed once
  a test must fail one of several pipelines sharing a shader, or one view of two.
- **gtest discovery at build time:** discovery runs at ctest time (`PRE_TEST`)
  everywhere, so parallel ctest runs on a freshly built tree race on it and the
  gate discovers once first. `POST_BUILD` on non-WIN32 (Windows lacks the DLLs
  at build time) would remove that step; do it if the race bites another caller.
- **Robot meshes held twice:** MeshLoader's path cache keeps every mesh
  imported from a path while the robot layer keeps its admitted copy. Drop the
  cache entry once admitted, or share one `MeshData`, when a large URDF's
  memory matters.
- **Per-mesh draw-call rebuilds:** every admitted robot mesh rebuilds the
  layer's draw calls; coalesce them (one rebuild per settled batch) if loading a
  many-mesh robot shows in a profile.

## Open questions

- **Pixi GUI tests.** Pixi CI runs no GUI tests (conda Qt stalls without a GPU);
  they are needed before the conda channel can be trusted. The pixi lanes compile
  the QRhi build against conda Qt, and win-64 runs the whole suite with the
  display passes skipped.
- **Safari.** WASM is QRhi already and the Playwright suites run in Chromium and
  Firefox (`wasm-ci.yml`; Firefox's `editors.spec.js` is non-blocking until its
  main-thread heartbeat budget holds on CI runners). Safari waits for a WebKit
  lane.
- **macOS distribution.** No macOS package ships from this repo; the `macos-ci`
  lane is the only macOS gate, and a real Mac's OpenGL fallback is unverified.
- **ADS on Wayland.** Floating docks hide on every backend (TECHNICAL_NOTES §5);
  the fix belongs in the ADS fork, and filing it upstream is the maintainer's
  call.
