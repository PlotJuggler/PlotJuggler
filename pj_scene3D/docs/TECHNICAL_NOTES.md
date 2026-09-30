# pj_scene3D — Technical Notes

The facts the renderer's design works around: backend conventions, Qt 6.11.1
bugs and their workarounds, window-system quirks, limits and budgets, the
measurements behind specific choices, the golden-image rules, test registration
and CI lanes, and the known gaps. The design itself is in
[ARCHITECTURE.md](./ARCHITECTURE.md); planned work is in [QRHI_NEXT.md](./QRHI_NEXT.md).
The migration that produced this renderer, with every per-stream measurement, is
archived in [`docs/archive/QRHI_MIGRATION.md`](../../docs/archive/QRHI_MIGRATION.md).

## 1. Graphics API per platform

| Platform / channel | Backend | Fallback | CI renderer |
|---|---|---|---|
| Linux (AppImage, .deb, conda) | OpenGL, 3.3 core at least | Vulkan opt-in (`PJ_GRAPHICS_API=vulkan`) | llvmpipe; lavapipe for Vulkan |
| Windows (installer) | D3D11 | WARP, chosen by Qt | WARP |
| macOS arm64 (conda, later a dmg) | Metal | OpenGL 4.1 core | the runner's paravirtual Metal device; Apple's software GL |
| WebAssembly | WebGL2 | — | Chromium (Playwright) |

**Startup.** `pj_app` resolves the API right after constructing `QApplication`,
before any GPU widget exists; a top-level picks its API only once. From the
Qt 6.11.1 sources (`qbackingstorerhisupport.cpp`, `qrhiwidget.cpp`):

- **D3D11:** when the default device fails, the backing store retries with
  `QRhi::PreferSoftwareRenderer`, i.e. WARP; PJ adds no fallback (WARP beats the
  GL 1.1 software driver Windows ships). The backing store creates D3D11 on the
  first DXGI adapter and retries only if that fails;
  `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` selects the adapter flagged
  `DXGI_ADAPTER_FLAG_SOFTWARE` (WARP, reported as `deviceType=Cpu`) up front, so CI
  sets it rather than rely on adapter order.
- **Metal:** the backing store probes Metal and, when the probe fails (a VM
  without a Metal device), gives up with only a debug message: despite its
  comment, it does not fall back to OpenGL, and every `QRhiWidget` would log "No
  QRhi" and stay blank. `preferredGraphicsApi()` therefore runs the same
  `QRhi::probe(Metal)` itself and picks OpenGL when it fails. Not verified on a
  Mac (compiled only under `QT_CONFIG(metal)`).
- **Vulkan** (only through `PJ_GRAPHICS_API`) is not probed; if it fails, the
  widgets report "No QRhi".

**macOS OpenGL fallback.** macOS gives an OpenGL QRhi a legacy 2.1 context unless
the window asks for core, below every pack's GLSL 330. `pj_app` (and the GUI test
main) set a 4.1 core default `QSurfaceFormat` on macOS in the QRhi build, before
`QApplication`; Metal ignores it. Qt creates the QRhi of a never-shown
`QRhiWidget` grab with a default-constructed `QSurfaceFormat`, a legacy 2.1
context on macOS, so such a grab cannot render Scene3D there; the app never grabs
a view that was not shown. CI's only macOS GL is Apple's software renderer
("Apple Software Renderer 4.1"), 10-40x slower than Metal: a float/redock case
takes 40-60 s instead of 1-2 s.

**OpenGL floor.** Every pack's lowest desktop GLSL is 330, and Qt's GL backend
never picks a GLSL above the context's, so OpenGL 3.3 core runs everything. The
Linux CI floor step caps Mesa at 3.3 (`MESA_GL_VERSION_OVERRIDE=3.3`,
`MESA_GLSL_VERSION_OVERRIDE=330`). Below GL 4.3 there is no depth resolve (§4).

## 2. Backend conventions

- **Clip space, NDC and texture y.** OpenGL is y-up in NDC and in the
  framebuffer with −1..1 clip depth; Vulkan is y-down in NDC; D3D and Metal are
  y-up in NDC but put texture row 0 at the top; Vulkan, D3D and Metal clip depth
  to 0..1. The renderer folds all of it into the matrices it uploads
  (`clipSpaceCorrMatrix()`, `clipToTextureMatrix()`, `textureVSign()`,
  `rhi_conventions.h`), so shaders never remap. The fullscreen triangle has a
  second bake, `-DPJ_FLIP_Y`, for D3D and Metal; it was kept over `gl_FragCoord`
  UVs, which assume input and output textures of equal size (false with
  `render_scale`).
- **MSAA orientation.** A y-down framebuffer (Vulkan, D3D, Metal) puts the 4x
  sample pattern mirrored on the scene relative to OpenGL's y-up targets. Diagonal
  silhouettes, cube edge bands and 1 px points resolve differently (up to 8% of
  pixels) while surface interiors are identical, even on the same Mesa
  rasterizer; no integer pixel shift explains it. The golden test tolerates it
  (§7).
- **D3D11 links shader stages by register, not by location.** SPIR-V Cross names
  each varying `TEXCOORD<location>`, but fxc packs a pixel shader's inputs into
  registers in declaration order, so a fragment shader that drops a leading input
  its vertex shader still writes reads every later varying from the wrong
  register. OpenGL, Vulkan and Metal match by location and hide it. Rule: every
  pack's varyings are contiguous from location 0 (a declared, unused input is
  enough), so a fragment shader's inputs are a prefix of its vertex shader's
  outputs. `wasm_shader_constants_test` checks it from each pack's reflection.
- **D3D11 needs `matrixSlice` on unrolled `mat4` vertex inputs:** SPIR-V Cross
  names them `TEXCOORD<first>_<row>`, and without the slice the input layout fails
  (`E_INVALIDARG`). OpenGL, Vulkan and Metal ignore it.
- **Vulkan draws nothing when the viewport is set before the pipeline is
  bound;** set it after each `setGraphicsPipeline()`.
- **3D texture limit.** QRhi reports none, so `max3dTextureSize()` asks the
  backend once per QRhi: OpenGL/WebGL `GL_MAX_3D_TEXTURE_SIZE`, Vulkan
  `maxImageDimension3D` (through `QRhiVulkanNativeHandles`), D3D11 by the
  device's feature level (`d3d11Max3dTextureSize()`: 2048 from 10_0, 256 on 9_x;
  Qt 6.11.1's `qrhid3d11.cpp` fills `featureLevel` from the created device),
  D3D12 and Metal the documented 2048 (QRhi's D3D12 minimum is feature level
  11_0; every Apple GPU family). A backend that cannot be asked (a Vulkan build
  without `<vulkan/vulkan.h>`, which CMake warns about; OpenGL without a current
  context) gets `kGuaranteedMax3dTextureSize`, 256, logged once. The D3D and
  Metal constants are documented, not measured on hardware.
- **Metal on GitHub-hosted `macos-14`** (Apple silicon VM):
  `MTLCreateSystemDefaultDevice` returns "Apple Paravirtual device" (not
  headless), a `CAMetalLayer` drawable presents, and QRhi reports `Metal,
  deviceType=Integrated`.

## 3. Qt 6.11.1 bugs and workarounds

| Behaviour | Workaround |
|---|---|
| `QRhiWidget::grabFramebuffer()` on a never-shown widget creates a new QRhi without calling `initialize()`, so a second grab renders with the previous QRhi's resources | Grab a hidden widget only once (`grabPresentedFrame()` documents it; the harness complies) |
| The OpenGL QRhi advances its frame number only in swapchain frames, so its "SRB already current" cache survives grab-only frames and a second pipeline sharing an SRB can draw with an older camera | `grabPresentedFrame()` presents one real frame before grabbing a shown view |
| The OpenGL depth resolve blits depth and stencil together, attaching the textures as both, so a stencil-less D32F framebuffer is incomplete and the resolved depth stays unwritten | OpenGL resolves D32FS8 |
| The OpenGL backend reports `QRhi::MultisampleTexture` from GL 3.0 (GLES 3.1) but allocates with `glTexStorage2DMultisample`, core only from GL 4.3: on a 4.1 context the entry point is null and `create()` crashes | The depth resolve also requires GL 4.3, GLES 3.1 or `GL_ARB_texture_storage_multisample` (`canCreateMultisampleTextures()`); other contexts replay |
| The widget backing store probes Metal and gives up without the OpenGL fallback its comment promises | `preferredGraphicsApi()` probes Metal itself (§1) |
| QRhiWidget re-renders synchronously on every Show, even on the same QRhi at the same size | `event()` marks the Show paint and `render()` records nothing (ARCHITECTURE, "Widget lifecycle") |
| The backing store creates the widget's QRhi (`QPlatformBackingStoreRhiConfig` carries only the API and the debug layer), so `EnablePipelineCacheDataSave` cannot be set and `setPipelineCacheData()` would come after Qt's own pipelines | None: pipeline-cache persistence is unreachable through `QRhiWidget`. OpenGL already persists program binaries through Qt's shader disk cache |
| Without a console (ctest, CI), Qt logs to `OutputDebugString` on Windows and to os_log on macOS | The native lanes set `QT_FORCE_STDERR_LOGGING=1` so the backend lines reach the test log |

**Toolchain: Clang 22 with libstdc++ 12 drops `std::visit`'s call for variant
alternative 0** in `submitScenePass`: with a generic lambda the return type cannot
be deduced (a compile error); with a templated visitor the call is never emitted
(an unresolved symbol at link time; forcing it crashes the frontend). Whichever
family is first fails. `PreparedFrame::DrawRef` therefore starts with
`std::monostate`, visited by a plain overload. The sanitizer lanes are the only
Clang + libstdc++ build.

**CTest splits `;` inside a property value.** `ENVIRONMENT
"PJ_GRAPHICS_API=opengl;QT_SCALE_FACTOR=2"` silently drops the scale factor, so
the DPR 2 passes set `QT_SCALE_FACTOR` through `ENVIRONMENT_MODIFICATION`.

## 4. Rationale for specific choices

| Problem | Decision | Why |
|---|---|---|
| `gl_PointSize` is absent on D3D | Instanced camera-facing quads on every backend; a point list only where `QRhi::VertexShaderPointSize` exists and the cloud's sprites stay at most 4 px | Quads measured 1.8-2.2x the point list's GPU time for 10M 2 px points on real GPUs (table below), above the 1.5x gate; larger sprites are fill-bound either way, and a quad keeps its visible part at the frustum edge |
| Non-float scalar vertex fields | CPU float conversion into the shared `PointVertex` array | Integer zero-copy needs a vertex layout and shader variant per wire datatype, NaN rejection in the shader, a re-bake of every pack and a full golden re-verification (numbers below) |
| MSAA depth for SSAO/EDL | `QRhi::ResolveDepthStencil` where QRhi reports it and multisample textures (OpenGL 4.3+, Vulkan, Metal); the single-sample depth replay elsewhere (D3D11, WebGL2, older GL) | The replay was +11.3 ms on OpenGL and +11.5 ms on Vulkan for a 10M cloud (+58%) on Iris Xe, the whole gap to the GL renderer; it is 40–45% of the static GPU time in the point benchmark |
| Mesh mask for EDL | A separate mask pass replaying the model ranges | WebGL's GLES QRhi does not resolve a second multisample colour attachment reliably |
| GL clip / Y conventions | Folded into CPU matrices (§2) | Shaders stay backend-neutral |
| Scene bounds | CPU bounds from the conversion, or a scan of the retained vertices once per revision | Nothing ever reported empty bounds with points; a compute reducer cost a pipeline, a compute bake and `StorageBuffer` usage on every instance buffer |
| Wireframe | Triangle edges as line lists on every backend | `NonFillPolygonMode` is missing on WebGL2 and would split the looks for a cosmetic gain |
| HUD and hover label | CPU-rasterized `QImage`s uploaded as texture quads | No per-context glyph atlas to lose across a QRhi teardown |
| Marker instances | A per-layer Static cache keyed on the resolved placements; the render origin stays on the CPU | See "Render origin" |
| Grid | Built once at the fixed-frame origin; the origin offset added in the shader | Exact (see "Render origin") |
| Output sample count | 1 when the HDR chain can run, 4 otherwise, chosen once per QRhi | A per-frame switch rebuilds every resource bound to the target (and broke the missing-HDR-variant fallback) |
| Lazy direct pipelines | Not done | Every backend caches compiled shaders per QRhi, so an HDR clone reuses its direct pipeline's compile; only Vulkan builds a `VkPipeline` per pipeline object (OpenGL first show: `pipeline_ms` 25 with the clones included; Vulkan's first show is within 3 ms of OpenGL's) |
| GPU timing | `QRhi::EnableTimestamps` + `lastCompletedGpuTime` | Frame-level only; enough for the HUD |
| Colormaps | The `pj_widgets` polynomials, in-shader | One source with the plots and the 2D depth view |
| Shader packs | Committed `.qsb`, `--glsl "300 es,330,440" --hlsl 50 --msl 12` + SPIR-V, hash-checked in the WASM configure | Desktop Qt kits install no `qsb`; GLSL 330 keeps the macOS GL 4.1 fallback possible |
| Diagnostics | Each QRhi widget logs its backend and adapter (`pj.graphics`); `PJ_GRAPHICS_API` override | A software rasterizer or an unexpected fallback is otherwise invisible |

**Point topology benchmark** (`demos/pointcloud_rhi_benchmark`: 10M 2 px points at
1920x1080, the colour pass into 4x RGBA16F + the single-sample depth replay;
median GPU time from QRhi timestamps):

| GPU / API | quads | point list | quads, re-uploaded each frame | point list, re-uploaded |
|---|---|---|---|---|
| RTX 4070 Laptop, Vulkan | 14.8 ms | 6.8 ms | 36.9 ms | 29.3 ms |
| RTX 4070 Laptop, OpenGL | 13.6 ms | 7.6 ms | 40.5 ms | 93.2 ms |
| Iris Xe (RPL-P), Vulkan | 90.9 ms | 49.2 ms | 96.4 ms | 63.3 ms |
| Iris Xe (RPL-P), OpenGL | 82.8 ms | 44.2 ms | 82.2 ms | 81.0 ms |

**Whole-cloud cube LOD.** A per-instance split (test each cube in the vertex
shader, run both programs) is slower than no LOD: a degenerate-clipped fan
instance still runs its 7 vertex invocations. On an RTX 4070 at 500k points with
the camera inside the cloud, a per-instance split ran 0.91x (0.70x as more cubes
converted), while the whole-cloud switch is exactly 1.00x there and 1.7-2.2x in a
wide shot. The emitted light of the sprites agrees with the fan within 3.4% from
0.67 px down to 0.125 px, which is also the no-popping guarantee.

**Integer zero-copy analysis** (streaming quads, 10M points per sample, Iris Xe;
decode+upload CPU = convert + prepare + enqueue + endFrame own-thread CPU):

| x/y/z | API | convert ms | bounds scan ms | decode+upload CPU ms | (convert − scan) share | peak RSS |
|---|---|---|---|---|---|---|
| u16 | Vulkan | 206.7 | 22.2 | 228.4 | 81% | 768 MiB |
| u32 | Vulkan | 205.5 | 26.5 | 227.1 | 79% | 806 MiB |
| f32 | Vulkan | 204.5 | 27.8 | 226.1 | 78% | 806 MiB |
| u16 | OpenGL | 195.0 | 22.2 | 234.6 | 74% | 993 MiB |
| u32 | OpenGL | 195.8 | 26.5 | 235.4 | 72% | 1031 MiB |
| f32 | OpenGL | 194.8 | 30.5 | 234.3 | 70% | 1030 MiB |

A zero-copy upload would save 70–81% of the decode+upload CPU (about 170–185 ms
per 10M sample) and the transient decoded arrays (153 MiB) plus the 191 MiB
`PointVertex` array; the retained copy would become the wire blob (114 or
153 MiB). About 82 ms of an f32 conversion is `convertCanonical()` and about
115 ms the pack into a fresh `PointVertex` array, dominated by first-touch page
faults. The integer rows' GPU times are not comparable with f32: an integer
lattice under a scaled camera rasterizes about 20x slower.

**Render origin.** Moving a camera-relative subtraction into a float shader add
is not pixel-identical when the operands are arbitrary doubles, even when the
error is sub-ulp: marker vertices kept relative to an anchor, with anchor −
origin added in the shaders, moved shadow edges on 14 Vulkan harness images by
up to 105 px. It is exact only for float-valued operands, as the grid's are:
grid positions and the render origin (the camera focal) are floats, so the
shader's correctly rounded float add equals the float of the double subtraction.

## 5. ADS and window-system quirks

- **ADS floating turns the docked area native** (Qt 6.11.1, our ADS fork):
  `CFloatingDockContainer::show()` calls `winId()` to set X11 properties (on every
  platform), and Qt then marks every child of the dock manager native. The docked
  area becomes a native child window; under Vulkan on Wayland that child surface
  stopped getting frame callbacks (the main window's docks froze once any dock
  floated, or crashed in the lavapipe swapchain).
  `Qt::AA_DontCreateNativeWidgetSiblings`, set before `QApplication` in the QRhi
  build, keeps them non-native.
- **ADS reparents docks through a parentless state.** It moves a dock within one
  window, and keeps only the current tab of a dock area parented
  (`CDockAreaLayout::setCurrentIndex` calls `setParent(nullptr)` on the tab it
  hides). A float, a redock, a move and a dock-area tab switch are therefore each
  a top-level change for a `QRhiWidget`: `releaseResources()`, then a fresh
  `initialize()`. Widgets must rebuild all their GPU state from retained CPU data:
  correct, but costly for a large 3D scene.
- **ADS hides floating docks on Wayland.** Its Linux stays-on-top emulation calls
  `setWindowFlag(Qt::WindowStaysOnTopHint, ...)` on every visible floating
  container when the main window is activated or deactivated. Off xcb that is a
  real flag change, and Qt hides a window whose flags change; ADS does not show it
  again. It affects every backend. `rhi_dock_lifecycle_test` removes that event
  filter. The fix belongs in the ADS fork (skip the call, or re-show the window).
- **Vulkan on Wayland: hiding and re-showing a window within about a frame of a
  present is a protocol error.** Mesa queues the present behind its FIFO
  barrier; the unmap commit waits behind it, and Qt's immediate `get_xdg_surface`
  then fails with "wl_surface already has a buffer committed", which drops the
  connection. Users do not reopen a window that fast, but programmatic hide/show
  can; the dock test waits 100 ms before reopening.
- **An occluded window never presents.** Display tests need a real X or Wayland
  server (Xvfb or a headless compositor locally, Xvfb in CI), never the
  `offscreen` QPA, which has no RHI support. Display passes run in parallel on one
  Wayland compositor stall when their windows overlap: run them serially there.
- **On the Wayland QPA every frame waits for the compositor's 60 Hz callback**,
  which hides everything under 16.7 ms: benchmarks run on xcb with vsync off.

## 6. Limits and budgets

`core/scene_limits.h` holds one `SceneLimits` policy per platform
(`kDesktopSceneLimits`, `kBrowserSceneLimits` under `PJ_TARGET_WASM`), statically
checked against the renderer's `quint32` buffer sizes and `int` counters.
Selected values:

| Limit | Desktop | Browser |
|---|---|---|
| Points per cloud / per view | 50 M / 100 M | 1 M / 1 M |
| Point wire bytes per cloud | 2 GiB | 64 MiB |
| Poses per layer / arms per view | 1 M / 6 M | 100 k / 300 k |
| Voxels per layer / per view | 64 Mi / 128 Mi | 4 Mi / 8 Mi |
| Marker instances per layer / per view | 1 M / 2 M | 50 k / 100 k |
| Marker stream vertices per layer / per view | 10 M / 20 M | 500 k / 1 M |
| Model triangles per layer / per view | 50 M / 100 M | 2 M / 4 M |
| HDR chain per view / per QRhi | 1 GiB / 4 GiB | 256 MiB / 512 MiB |
| Concurrent mesh loads per layer | 4 | 1 |

The HDR chain costs 4x (RGBA16F 8 + depth) + RGBA16F 8 + RGBA8 4 + depth per
render pixel: 64 B with D32F (4 B) and 84 B with OpenGL's D32FS8 resolve (8 B).
3840x2160 is 506 MiB (664 MiB on OpenGL), so 1 GiB fits one 5K view (one 4K view
on OpenGL) and 4 GiB eight 4K docks (six on OpenGL). The largest single buffers at the desktop limits: 100 M points ×
20 B = 2 GB, 20 M marker vertices × 28 B = 560 MB.

Fixed sizes: shadow map 2048² D32F (`kShadowMapSize`, independent of
`render_scale`); point list up to 4 device px (`kPointListMaxPx`); flat sphere
discs below 2 device px (`kSphereDiscMaxPx`); cube LOD at 1 device px
(`kDefaultCubeLodThresholdPx`); `render_scale` in [1, 4]; trails at most 50k
points (`kMaxTrailPoints`).

**Frame budget.** On-demand repaint only: ≤ 60 Hz during playback, 0 frames when
settled (asserted by `scene3d_frame_benchmark`).

**Performance gate (Gate A).** RHI ≤ max(1.2x GL, GL + 1 ms) in wall time and
GUI-thread CPU, RSS ≤ 1.10x GL, 0 idle frames, measured against the legacy GL
build (§9 has the final run).

## 7. Golden images

`pointcloud_golden_test` compares 36 scenes (28 point-cloud scenes and 8 scene
corpus images: grid, markers, robot) against goldens rendered by the legacy GL
renderer on llvmpipe. The corpus and its full rule are in
[`widgets/tests/fixtures/golden/pointcloud/README.md`](../widgets/tests/fixtures/golden/pointcloud/README.md).
In short:

- A pixel differs when it is off its golden pixel by more than 16 in a channel;
  a scene fails above 3% differing pixels, a 5% coverage change, or a mean-colour
  change above 12.
- The QRhi build counts a pixel as differing only when every golden pixel in its
  3x3 neighbourhood is off (the MSAA orientation of §2), and also fails a scene
  above max(3 × 3%, 10%) plain per-pixel differences, so the allowance cannot hide
  a whole-scene shift.
- Both builds add a foreground diff (differing pixels over the foreground, limit
  0.12, always with the neighbourhood rule) and a stray count (non-background
  pixels farther than 2 px from the golden's foreground, limit 4).
- Driver-dependent scenes check coverage and mean colour only
  (`field_turbo_spheres`, `large_1m_points`, the line grids) or coverage only
  (`shape_spheres_small`: its ~1.5 px discs rasterize at a different size per Mesa
  version).

Per-backend allowances and measurements:

| Backend | Allowance | Measured worst |
|---|---|---|
| OpenGL (llvmpipe) | none | foreground diff 0.007 (`shape_cubes_subpixel`), per-pixel 0.003 |
| Vulkan (lavapipe) | none | foreground diff 0.083 (`field_viridis_cubes`), per-pixel 0.077; line grids 18% of their foreground (hence coverage-only) |
| D3D11 (WARP) | line scenes (grid, marker edges, line strips): coverage 0.12, foreground diff 0.2 | neighbourhood 0.019 (`field_viridis_cubes`), per-pixel 0.074 (`shape_cubes_subpixel`), point-cloud coverage within 0.6%; line grids 1.19x the golden's pixels |
| Metal (paravirtual) | none | the D3D11 bands on the point clouds; line grids 0.94x |
| All, line grids | coverage 0.08 (0.25 on D3D11/WARP) | llvmpipe 0.994, lavapipe 0.982, Metal 0.937, WARP 1.194 of the golden's pixels |
| Apple software GL | not run | sphere imposters ~10% larger, soft point sprites: coverage up to 19%, mean colour up to 58 |

The GL build reproduces the goldens exactly (0.0000) and writes them
(`PJ_UPDATE_POINTCLOUD_GOLDENS=1`); the QRhi build refuses to.

Other measured tolerances:

- `DepthResolveMatchesTheForcedReplay`: the depth-resolve and replay chains
  differ on under 1% of pixels with SSAO and EDL on.
- `scene3d_rhi_model_cache_test`'s cached-vs-uncached A/B is pixel-identical on
  Mesa and Metal; on Apple's software GL it differs on 10 pixels once shadows go
  off, and the test allows 16 there (§10).
- The perf-HUD image differs between any two runs in one 7x9 px digit of its
  timing text.

## 8. Tests and CI

**Registration.** A QRhi display runner declares `RHI_BACKENDS` in
`pj_add_test_runner` (`cmake/PjTests.cmake`). Its cases register once per backend
of the platform, `<runner>.<backend>.*` with `PJ_GRAPHICS_API=<backend>` and the
label `qrhi-<backend>`: opengl and vulkan on Linux, d3d11 on Windows, metal and
opengl on macOS. `DPR2` adds `<runner>.opengl_dpr2.*` at `QT_SCALE_FACTOR=2`
(label `qrhi-opengl`), optionally filtered (`DPR2_FILTER`); the runner joins the
`pj_qrhi_tests` build target. `-L qrhi-<backend>` selects one backend's passes,
`-L qrhi` every pass (labels are regular expressions), `-LE qrhi` none. Every macOS
`.opengl` case gets 5x the timeout (Apple's software GL).

| Runner | What it covers | DPR 2 |
|---|---|---|
| `scene3d_rhi_render_test` | the harness: orientation, occlusion, translucency, shadows, HDR/SSAO/EDL, presentation, reparent, depth resolve vs replay | the `Presentation*` cases |
| `scene3d_rhi_point_test` | point-pass behaviour the goldens do not pin | — |
| `scene3d_rhi_failure_test` | the failure state and the optional-stage fallbacks | — |
| `scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test` | the marker and model caches, pixel-identical to the uncached frames | — |
| `scene3d_rhi_platform_test` | `max3dTextureSize()` per backend | — |
| `pointcloud_golden_test` | the golden corpus (§7) | — |
| `scene3d_frame_benchmark` | frame budgets, a smoke at default sizes; `PJ_S3D_BENCH_FULL=1` for the Gate A sizes | — |
| `plot_rhi_render_test` (pj_plotting) | the plot canvas against Qwt's raster | yes |
| `rhi_dock_lifecycle_test`, `rhi_mixed_dock_test` (pj_app) | the floating-dock matrix, stand-ins and real widgets | yes |

`pointcloud_golden_test` and `scene3d_frame_benchmark` also build in the GL build
(one plain pass). `scene3d_rhi_app_smoke` (pj_app, Linux, label `qrhi-opengl`)
runs `packaging/smoke/render_check.sh` on the build tree's app.

**GPU gate.** A display pass skips when no window can be exposed (the
`offscreen` QPA, a runner without a desktop): the GUI test main probes once per
pass. `PJ_REQUIRE_GPU_TESTS=1` turns the skip into a failure, and every CI GPU
lane sets it. A render case whose canvas initialized a backend and then failed
fails instead of skipping.

**Test hooks.** Builds with tests define `PJ_RHI_TEST_HOOKS` (root CMake,
test-enabled desktop builds; release builds configure `PJ_BUILD_TESTS=OFF`):

- `PJ_SCENE3D_TEST_FAIL_SHADER=<suffix>` fails every pipeline whose shader path
  ends with it (`createGraphicsPipeline()`);
- the `pjTestFailPipeline` dynamic property fails a named pipeline of the plot
  canvas or the media viewer, and `pjTestPipelineAttempts` counts attempts;
- `pjTestRhiAdoptions` / `pjTestRhiReleases` / `pjTestRhiStale` count QRhi
  adoptions (`PJ::noteRhiAdoption()`), which `rhi_mixed_dock_test` checks across
  dock moves: every float and redock re-initializes the moved widget, and no
  widget meets a new QRhi before releasing the old one's resources;
- `setForceDepthReplayForTest()` keeps the depth replay where the chain would
  resolve depth; `scene3d_frame_benchmark`'s `DepthReplayOnOff` times the two;
- `QT_RHI_LEAK_CHECK` catches resources left on a dying QRhi on Vulkan, D3D11
  and Metal; on OpenGL uniform buffers are CPU-side and untracked, hence the
  counters.

Dumps: `PJ_RHI_TEST_DUMP_DIR` saves the harness frames, and
`PJ_POINTCLOUD_GOLDEN_OUT` / `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1` the golden actuals
and diffs.

**CI lanes.**

| Workflow | Lane | Runs | Gate |
|---|---|---|---|
| `linux-ci.yml` | `build` (QRhi) | the whole suite under xvfb, every `.opengl`, `.opengl_dpr2` and `.vulkan` pass (lavapipe via `VK_ICD_FILENAMES`), `PJ_REQUIRE_GPU_TESTS=1` | blocking |
| | OpenGL floor step | `scene3d_rhi_render_test.opengl` and `rhi_dock_lifecycle_test.opengl` with Mesa capped at GL 3.3 | blocking |
| | `build-gl` tree | the legacy GL build (`-DPJ_SCENE3D_RHI=OFF -DPJ_BUILD_DEMOS=OFF`): `pj_app` and `pj_gl_rollback_tests`, run as `ctest -L gl-rollback` (the GL runners and tests, goldens per pixel) | blocking |
| `windows-ci.yml` | `build` (QRhi) | the unit suite (`-LE qrhi`), then `-L qrhi-d3d11` with the `windows` QPA, `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`; `scripts/ci/check_qrhi_backend.sh` fails unless every view and plot canvas logs `D3D11` on a `Cpu` (WARP) adapter | compile and display step blocking; unit step non-blocking |
| | `gl` | the legacy GL build and its non-GPU tests (hosted Windows has no GL 4.5) | non-blocking |
| `macos-ci.yml` | `qrhi-metal` (`macos-14`) | a Metal device and presentation probe, then `-L qrhi-metal` and `-L qrhi-opengl` with the `cocoa` QPA, each requiring every view and plot canvas on its backend, then `-LE qrhi`; on a failure it reruns up to three failed QRhi cases under lldb (their backend from the ctest name) for every thread's backtrace | on PRs |
| `pixi-ci.yml` | linux-64, win-64 | the QRhi build against conda Qt; win-64 runs the whole suite with the display passes skipped (its runner exposes no window) | — |
| `sanitizers.yml` | asan, msan, tsan | the Clang 22 + libstdc++ build; `tsan.supp` carries audited entries for `LatestWinsWorker`'s Qt hand-offs | — |
| `wasm-ci.yml` | | build, Playwright suites, the Scene3D capability probe | — |
| release workflows | | the render check on the IFW install (D3D11/WARP) and on the .deb and AppImage (Ubuntu 22.04 and 24.04) | see `packaging/README.md` |

The macOS OpenGL step is a functional smoke run, not a parity gate (Metal is):
it leaves out what measures Apple's software renderer rather than ours, namely the
point-cloud goldens, the sub-pixel sprite colour check (1 px sprites resolve to
~15% coverage there), the odd-cell-edge grid line check (its 1 px analytic line
loses the same coverage there) and the DPR 2 mixed-dock pass (every float
recompiles the pipeline set at 4x the pixels; Linux runs it). The PR lane runs on
GitHub's `macos-14`, because Depot's macOS capacity is not elastic (a job queued
40 min); Depot stays a `workflow_dispatch` choice.

## 9. Performance baseline (Gate A, final run)

Method: `scene3d_frame_benchmark`, `plot_canvas_benchmark` and
`mixed_layout_benchmark` on Intel Iris Xe (RPL-P), Mesa 26.0.3, i915, AC power,
`powersave` governor; xcb on a private rootful Xwayland in a headless GNOME Shell,
vsync off (`vblank_mode=0`, `MESA_VK_WSI_PRESENT_MODE=immediate`); 5 interleaved
rounds, one process per case, each the median of 30 frames after 3 warm-up
frames. Median [min–max]. Verdicts are on RHI GL, Linux's native backend; RHI VK
(opt-in on Linux) is for reference. Plot rows compare with the Qwt GL canvas. The
RTX 4070 column is still open (QRHI_NEXT.md).

| Row | GL build | RHI GL | Ratio (RHI GL) | Verdict | RHI VK |
|---|---|---|---|---|---|
| Cloud 10M static: wall / CPU ms | 20.0 / 1.8 | 19.9 / 1.5 | 1.00x / 0.83x | pass | 21.5 / 1.6 |
| Cloud 10M static: RSS MiB | 371 | 448 | 1.21x | **fail** | 443 |
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
| 10M cloud dock: float / redock / hide-show ms | 93.9 / 85.0 / 1.2 | 138.0 / 115.5 / 1.7 | 1.47x / 1.36x / +0.5 ms | **fail** (float, redock) | 149.1 / 114.8 / 54.0 |
| 50-link robot dock: float / redock / hide-show ms | 36.2 / 26.7 / 1.2 | 72.0 / 49.0 / 2.1 | 1.99x / 1.84x / +0.9 ms | **fail** (float, redock) | 38.1 / 12.2 / 6.5 |
| First show, 10M cloud + robot, ms | 147.5 | 221.1 (pipelines 24) | 1.50x | **fail** | 201.3 |
| Idle frames when static, every case | 0 | 0 | – | pass | 0 |
| Plots: dashed 5 x 100k, wall ms | 14.6 | 13.7 | 0.94x | pass | 18.0 (1.23x) |
| Plots: 50 x 100k, wall ms | 59.3 | 54.1 | 0.91x | pass | 102.6 (1.73x) |
| Plots: tracker only, 10 x 100k, wall ms | 12.5 | 2.2 | 0.18x | pass | 5.9 |
| Plot GPU memory per 1600x900 plot, DPR 1 / 2 (MiB) | 61.9 / 174.1 | 42.6 / 136.6 | 0.69x / 0.78x | pass | 106.1 / 315.8 |
| Streaming plot, 10 x 100k, ≤ 16.7 ms | 18.5 | 8.8 | – | pass | 15.1 |
| Mixed: 6 plots + Scene3D, wall / CPU ms | 35.5 / 35.4 | 34.3 / 16.9 | 0.97x / 0.48x | pass | 64.7 / 20.0 |
| Mixed: RSS MiB | 266 | 285 | 1.07x | pass | 269 |

The failing rows were signed off for release N (2026-09-26); their follow-ups
are in QRHI_NEXT.md. The benchmark's "tab" move is a hide/show on the same QRhi
(a workspace tab); an ADS dock-area tab switch reparents and costs a float-like
rebuild (§5).

Reading the numbers:

- Vulkan on the Iris costs more GUI CPU than OpenGL wherever the frame is
  CPU-bound (markers 11.7 vs 8.6 ms, stream 10.2 vs 6.1 ms), consistent with
  OpenGL as Linux's native backend.
- QRhi's Vulkan backend frees an upload's staging buffer only frames later, so a
  settled view (which renders no more frames) keeps reporting it in the DRM
  total; the multisample depth texture of the resolve chain also comes from a
  different memory pool than a renderbuffer, reserving one more allocator block.
- The software rasterizers run the frame on the GUI thread for OpenGL and on
  worker threads for Vulkan, so only wall time compares between them.

## 10. Known gaps

- The Gate A rows marked **fail** above (QRHI_NEXT.md, "P6: performance
  follow-ups").
- An ADS dock-area tab switch rebuilds the view like a float (ADS parks the
  hidden tab parentless, §5); the Show skip covers only a hide/show on the same
  QRhi.
- Markers rebuild on pan and follow: the render origin is in their cache key.
- A runtime HDR refusal on a GPU that can run the chain (size, budget, a missing
  HDR variant) draws its direct pass without MSAA: the widget's sample count is
  chosen once per QRhi.
- Large point sprites are quads above `kPointListMaxPx` (4 px), about 1.2x the GL
  build's `gl_PointSize` sprites on Iris Xe (a 1M 0.1 m sphere cloud: 34.9 vs
  29.6 ms on OpenGL).
- ADS hides floating docks on Wayland, on every backend (§5).
- D3D11 (WARP) rasterizes 1 px lines differently from Mesa; three wide dashed plot
  lines end one dash later than Qwt's raster. The goldens and the dash test carry
  per-backend allowances for exactly these cases (§7).
- On Apple's software OpenGL ("Apple Software Renderer 4.1", the macOS CI
  runner's) `scene3d_rhi_model_cache_test`'s cached and uncached frames differ on
  10 pixels from step 6 (shadows off) on; Mesa (OpenGL, Vulkan) and Metal match
  exactly, and the test allows 16 pixels on that renderer only (§7). The two
  paths differ only in GPU resource identity: the uncached one re-uploads the
  colour instances every frame and rebuilt every mesh (buffers, material SRBs) at
  the two earlier revision bumps; both rebuild their instances and rebind every
  material's shadow slot to the white texture when shadows go off, and the pass
  keeps no shadow state across that toggle (`clearShadowMap()`,
  `releaseShadowResources()`). Unresolved without a Mac to capture the frames:
  run the test there with `PJ_RHI_TEST_DUMP_DIR` and diff steps 6-8.
- On the same Apple software renderer, `FilledGridLinesFollowOddCellEdges`'s 1 px
  analytic grid line at an odd cell edge does not clear the pixel's neighbouring
  tones by the test's threshold (same coverage loss as the sub-pixel sprite
  case, §8); the macOS `qrhi-opengl` step excludes it and Mesa/Vulkan/Metal run
  it as usual.
- The AppImage smoke runs after its upload; a `workflow_dispatch` of the release
  workflows on a branch is its only pre-merge gate (`packaging/README.md`).
- Vulkan (opt-in on Linux) misses most CPU-bound and plot rows of Gate A
  (the 50 x 100k plot at 1.73x the Qwt GL canvas).
- Pixi CI runs no GUI tests: conda Qt stalls without a GPU.
