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
> - **Packagers (conda/RoboStack, distributions):** `-DPJ_SCENE3D_RHI=OFF`
>   restores the previous OpenGL build for this release only (configure warns); it
>   is removed in the next one.
> - **3D view:** the camera fits each newly added layer once (never over a camera
>   restored from a layout); solid markers (cubes, spheres, cylinders, arrows) cast
>   shadows; a translucent layer blends over the layers behind it whatever its
>   position in the layer list; wireframe shows every triangle edge on every
>   backend; markers draw at the layer's default 0.6 opacity; the grid follows the
>   light/dark theme; plain URDF primitives are no longer darkened twice.
> - **Plots:** crisper lines (one sample per pixel with shader anti-aliasing,
>   axis-aligned lines on whole pixels as Qwt draws them), GPU dashes, and the 1 px
>   canvas frame. Against the previous OpenGL canvas: a tracker move during
>   playback costs 0.18x, dashed curves 0.94x, dense curves 0.91x, streaming half
>   the time at a third of the CPU; GPU memory per plot is 0.69-0.78x.
> - **Performance:** a 10-million-point cloud renders as fast as before, a layout
>   of 6 plots and a 3D view takes half the CPU, and many-layer scenes are faster
>   and far lighter (1000 layers: 0.16x the memory). Floating or redocking a large
>   3D view, and a view's first show, are slower than before (1.4-2x), and a
>   large cloud keeps about 20% more memory; these are the next release's
>   performance work.
> - **Floating docks on X11** stay non-native windows: floating a dock no longer
>   turns the docked area into native child windows.

## PR 2 (release N+1): delete the GL renderer

PR 2 lands after one release with no rollback. Preconditions:

- [ ] QRhi twins of `MagentaLinesAppearOnlyWhenVisible` and
  `FloorShadowClearsWhenDataRemoved` as harness cases, each with a mutation
  check.
- [ ] `hud_overlay_test`, `trail_layer_test`, `occupancy_grid_layer_rebind_test`,
  `scene_view_widget_mouse_gesture_test` and `grid_map_layer_test` moved to the
  shared runners (they build only in the GL block today).
- [ ] CPU twins of the model-pass tests.
- [ ] A final GL vs QRhi A/B (`scene3d_frame_benchmark` and the other Gate A
  tools, TECHNICAL_NOTES §9).

Then PR 2 deletes:

- the GL Scene3D renderer: `widgets/src/scene_view_widget.cpp`,
  `widgets/src/passes/`, `widgets/src/gl/`, `SceneHdrFbo`, the GL-only block of
  `widgets/CMakeLists.txt`, every layer's `#ifndef PJ_SCENE3D_RHI` sink, the GL
  branch of `scene3d_frame_benchmark` and
  `mixed_layout_benchmark`;
- `pj_test_main_gl.cpp` and the GL demos (`mesh_viewer.cpp`,
  `marker_gallery_demo.cpp`, `poses_gallery_demo.cpp`,
  `pointcloud_aabb_benchmark.cpp`, `pointcloud_shape_benchmark.cpp`: port or drop
  each);
- the `QwtPlotOpenGLCanvas` branch of `PlotWidgetBase.cpp`,
  `raster_text_gl_test.cpp` and `plot_canvas_context_recreation_gl_test.cpp`;
- the GL text wrapper: `RasterTextEngine.{h,cpp}` and its CMake entries,
  `raster_text_engine_test.cpp`, `installRasterTextEngines` and its include in
  `main.cpp`;
- the bootstrap widgets (`rhi_bootstrap` in `MainWindow`, `scene2dRhiBootstrap`
  in `Scene2DDockWidget`), if `rhi_dock_lifecycle_test` passes without them;
- the `PJ_SCENE3D_RHI` option and `PJ_RHI_WIDGETS` (and the explicit flag in
  `build.sh` / `build.ps1`);
- the `build-gl` tree in `linux-ci.yml`, the Windows `gl` lane, and
  `pj_gl_rollback_test()` / `GL_ROLLBACK` in `cmake/PjTests.cmake`;
- the legacy appendix of ARCHITECTURE.md, the GL rows of the module docs
  (`pj_scene3D/CLAUDE.md`, `pj_plotting/CLAUDE.md`, the root `CLAUDE.md`), and
  this section.

PR 2 also enables `PJ_UPDATE_POINTCLOUD_GOLDENS` in the QRhi build (the goldens
stay GL-made until then, because release N's GL lane checks them per pixel),
decides which backend writes them, and drops the `macos-ci.yml` push trigger for
`feat/qrhi-**`.

## P6: performance follow-ups

Each Gate A row signed off as failing for release N (TECHNICAL_NOTES §9; Iris
Xe, RHI GL vs the GL build), with its candidate fix.

**Cloud stream 300k/frame: wall 8.0 vs 6.0 ms (1.33x, limit 7.2; bimodal
5.6–8.5).**

- *Direct point converter.* Convert the wire cloud straight into the
  `PointVertex` array, skipping the `DecodedPointCloud` intermediate (153 MiB
  transient for 10M points) and the separate pack, whose fresh 191 MiB array
  spends most of its ~115 ms in first-touch page faults. No shader or format
  change. The first step.
- *Integer zero-copy.* Upload the wire bytes and decode in the vertex stage: the
  conversion measured 70–81% of decode+upload CPU (about 170–185 ms per 10M
  sample), and the retained copy would shrink to the wire blob. It needs a vertex
  layout and shader variant per wire datatype, NaN rejection in the shader, a
  scalar and packed-colour attribute per field type, a re-bake of every pack and a
  full golden re-verification. After the direct converter, if the row still fails.

**Cloud 10M static: RSS 448 vs 371 MiB (1.21x). Cubes + lines: RSS 438 vs 301 MiB
(1.46x).**

- *Drop the retained CPU copy* of uploaded clouds (and marker expansions and
  meshes). Blocked by the ADS reparent rebuild: every float, redock and dock-area
  tab switch calls `initialize()` on a new QRhi and re-uploads from the retained
  data. It needs a re-derive-from-datastore path whose cost fits the reparent
  row. The direct converter and zero-copy above also cut the transient peak.

**10k markers, pan + follow: GUI CPU 8.6 vs 6.6 ms (1.30x, limit 7.9).**

- *Per-instance translation stream* (the exact fix). Keep each instance's
  translation, and each stream vertex's position, in a separate per-frame vertex
  stream recomputed from cached doubles on an origin change; `marker.vert`'s
  matrix column 3 then comes from that binding, and the shaders are otherwise
  unchanged. About 16 B/instance of upload per pan frame instead of a full
  expansion.
- *Marker origin in the shader*, as the grid does, is not an option as such: a
  float add of camera-relative doubles is not pixel-identical (TECHNICAL_NOTES,
  "Render origin").

**50-link robot, TF each frame: wall 3.6 vs 2.2 ms (limit 3.2; bimodal
1.8–4.2).** Every frame moves every link, so the model instances rebuild each
frame by design. Profile the bimodality first (both builds show it on OpenGL).

**Dock float / redock: 10M cloud 138.0 / 115.5 vs 93.9 / 85.0 ms (1.47x /
1.36x); 50-link robot 72.0 / 49.0 vs 36.2 / 26.7 ms (1.99x / 1.84x). First show,
cloud + robot: 221.1 vs 147.5 ms (1.50x).** A new top-level means a new QRhi, so
every pipeline and buffer is rebuilt (the shader packs are already cached per
process). Candidates:

- avoid the rebuild on an ADS dock-area tab switch, which reparents through a
  parentless state (TECHNICAL_NOTES §5): keep the widget parented in the ADS fork,
  or recognize a return to the same QRhi;
- pipeline-cache persistence, if Qt 6.12 lets a `QRhiWidget` set it (see below);
- precompiled packs (`qsb -c`), which need fxc and the Metal toolchain in a
  per-platform bake;
- lazy direct pipelines, which save only pipeline objects (every backend caches
  compiled shaders per QRhi) and matter on Vulkan.

**Vulkan 50 x 100k plot: 102.6 ms vs the Qwt GL canvas's 59.3 (1.73x).** Indexed
geometry left it 9% slower in wall than before at 13% less CPU; the time is
outside `render()`. Analyse it if Vulkan becomes a default anywhere.

**RTX 4070 measurements.** Gate A was measured on the Iris Xe only; the host
kernel had no NVIDIA module. Re-run `scene3d_frame_benchmark`
(`PJ_S3D_BENCH_FULL=1`), `plot_canvas_benchmark` and `mixed_layout_benchmark`
when it is available, and add the column to TECHNICAL_NOTES §9.

Deferred with a trigger, not failing any row:

- *Shared primitive meshes* (URDF box/cylinder/sphere) across layers: when more
  than 32 MiB is duplicated across repeated robots.
- *Indexing* `drawsFor` (a linear search per logical slot, O(L²) in the layer
  count) and `ScenePipelineRegistry::resolve` (a scan per draw): when the
  1000-layer case exceeds 1 ms or 10% of `prepareFrame` CPU. A finger index needs a
  `draws[hint-1].layer != layer` check.
- *Large sprites* above 4 px are quads, 1.2x the GL build's point sprites:
  revisit with a use case that draws many large sprites.
- *Runtime HDR refusal* draws without MSAA: revisit if users hit the HDR byte
  limits.

## P6: Qt 6.12

Qt 6.12 is the next LTS (`docs/QT_NOTES.md`). QRhi has no cross-minor
compatibility guarantee, so the upgrade is one change across every channel:

- [ ] Bump `PJ_QT_VERSION` in `versions.env` (Windows CI installs Qt inline:
  check it follows), and the `qt6-main` pin in `pixi.toml`.
- [ ] Re-bake every `.qsb` pack with 6.12's `qsb`: `pj_scene3D/widgets/rhi/shaders/bake.sh`,
  `pj_plotting/widget/shaders/bake.sh` and the Scene2D packs (the `qsb` commands
  in `pj_scene2D/docs/TECHNICAL_NOTES.md`, "Shader Toolchain"). The WASM
  configure fails on any byte difference until they match.
- [ ] Re-test the two grab workarounds before removing either: the never-shown
  `grabFramebuffer()` that works once, and the OpenGL stale uniforms in grab-only
  frames (`grabPresentedFrame()` presents a real frame first). Keep each until a
  test shows 6.12 fixed it.
- [ ] Re-check the other 6.11 workarounds (TECHNICAL_NOTES §3): the OpenGL depth
  resolve on D32F, the `MultisampleTexture` GL 4.3 gate, the Metal probe
  fallback, the synchronous Show re-render, and whether a `QRhiWidget` can now
  enable pipeline-cache persistence.
- [ ] Re-run the golden corpus, the harness and Gate A on every lane.
- Not needed now: 6.12's indirect draws and device-fault reporting. Revisit with a
  use (the voxel compaction path, a device-lost recovery).

## Open questions

- **Pixi GUI tests.** Pixi CI runs no GUI tests (conda Qt stalls without a GPU);
  they are needed before the conda channel can be trusted. The pixi lanes compile
  the QRhi build against conda Qt, and win-64 runs the whole suite with the
  display passes skipped.
- **Browsers beyond Chromium.** WASM is QRhi already and the Chromium Playwright
  suites are its smoke; Firefox and Safari wait for cross-browser CI.
- **macOS distribution.** No macOS package ships from this repo; the `macos-ci`
  lane is the only macOS gate, and a real Mac's OpenGL fallback is unverified.
- **ADS on Wayland.** Floating docks hide on every backend (TECHNICAL_NOTES §5);
  the fix belongs in the ADS fork, and filing it upstream is the maintainer's
  call.
- **The legacy GL 4.1 branch.** `feat/scene3d-gl41` (the GL renderer ported to GL
  4.1) is not merged; deleting the remote branch is the maintainer's call.
