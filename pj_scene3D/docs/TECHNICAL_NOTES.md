# pj_scene3D — Technical Notes

The facts the renderer's design works around: backend conventions, Qt
workarounds, window-system quirks, limits and budgets, the
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
| WebAssembly | WebGL2 | — | Chromium's SwiftShader; Firefox on llvmpipe (Playwright) |

**Startup.** `pj_app` resolves the API right after constructing `QApplication`,
before any GPU widget exists; a top-level picks its API only once. From Qt's
sources (`qbackingstorerhisupport.cpp`, `qrhiwidget.cpp`; read on 6.11.1, re-check on the pinned Qt):

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
- **Overrides:** a `PJ_GRAPHICS_API` other than the default (Vulkan on Linux,
  say) is probed by creating a throwaway QRhi (Vulkan on Qt's default instance,
  which the window then reuses: about 10 ms net on Iris Xe); one that
  cannot start falls back to the default with a `pj.graphics` warning.

**macOS OpenGL fallback.** macOS gives an OpenGL QRhi a legacy 2.1 context unless
the window asks for core, below every pack's GLSL 330. `pj_app` (and the GUI test
main) set a 4.1 core default `QSurfaceFormat` on macOS, before
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
  outputs. `scene3d_shader_constants_test` checks it from each pack's reflection.
- **D3D11 needs `matrixSlice` on unrolled `mat4` vertex inputs:** SPIR-V Cross
  names them `TEXCOORD<first>_<row>`, and without the slice the input layout fails
  (`E_INVALIDARG`). OpenGL, Vulkan and Metal ignore it.
- **Vulkan draws nothing when the viewport is set before the pipeline is
  bound;** set it after each `setGraphicsPipeline()`.
- **3D texture limit.** QRhi reports none, so `max3dTextureSize()` asks the
  backend once per QRhi: OpenGL/WebGL `GL_MAX_3D_TEXTURE_SIZE`, Vulkan
  `maxImageDimension3D` (through `QRhiVulkanNativeHandles`), D3D11 by the
  device's feature level (`d3d11Max3dTextureSize()`: 2048 from 10_0, 256 on 9_x;
  Qt's `qrhid3d11.cpp` fills `featureLevel` from the created device),
  D3D12 and Metal the documented 2048 (QRhi's D3D12 minimum is feature level
  11_0; every Apple GPU family). A backend that cannot be asked (a Vulkan build
  without `<vulkan/vulkan.h>`, which CMake warns about; OpenGL without a current
  context) gets `kGuaranteedMax3dTextureSize`, 256, logged once. The D3D and
  Metal constants are documented, not measured on hardware.
- **Metal on GitHub-hosted `macos-14` and on `depot-macos-14`** (Apple silicon VMs):
  `MTLCreateSystemDefaultDevice` returns "Apple Paravirtual device" (not
  headless), a `CAMetalLayer` drawable presents, and QRhi reports `Metal,
  deviceType=Integrated`.

## 3. Qt workarounds (found on 6.11.1; re-test on the pinned Qt)

| Behaviour | Workaround |
|---|---|
| `QRhiWidget::grabFramebuffer()` on a never-shown widget creates a new QRhi without calling `initialize()`, so a second grab renders with the previous QRhi's resources | Grab a hidden widget only once (`grabPresentedFrame()` documents it; the harness complies) |
| The OpenGL QRhi advances its frame number only in swapchain frames, so its "SRB already current" cache survives grab-only frames and a second pipeline sharing an SRB can draw with an older camera | `grabPresentedFrame()` presents one real frame before grabbing a shown view |
| The OpenGL depth resolve blits depth and stencil together, attaching the textures as both, so a stencil-less D32F framebuffer is incomplete and the resolved depth stays unwritten | OpenGL resolves D32FS8 |
| The OpenGL backend reports `QRhi::MultisampleTexture` from GL 3.0 (GLES 3.1) but allocates with `glTexStorage2DMultisample`, core only from GL 4.3: on a 4.1 context the entry point is null and `create()` crashes | The depth resolve also requires GL 4.3, GLES 3.1 or `GL_ARB_texture_storage_multisample` (`canCreateMultisampleTextures()`); other contexts replay |
| The widget backing store probes Metal and gives up without the OpenGL fallback its comment promises | `preferredGraphicsApi()` probes Metal itself (§1) |
| QRhiWidget paints synchronously inside every Show (an offscreen frame; on Vulkan a fence wait), even on the same QRhi at the same size, the paint dispatching back through `event()`; and an `update()` on a hidden render-to-texture widget sets `renderToTextureReallyDirty`, so a later repaint of its parent (an ADS tab switch) paints it again | `event()` marks the Show scope with a scoped flag (the nested paint keeps it) and `paintEvent()` skips QRhiWidget's paint while the target holds the last frame; a frame follows only if the content changed (`SceneViewWidget::invalidateContent()`) (ARCHITECTURE, "Widget lifecycle") |
| The backing store creates the widget's QRhi (`QPlatformBackingStoreRhiConfig` carries only the API and the debug layer), so `EnablePipelineCacheDataSave` cannot be set and `setPipelineCacheData()` would come after Qt's own pipelines | None: pipeline-cache persistence is unreachable through `QRhiWidget`. OpenGL already persists program binaries through Qt's shader disk cache |
| Without a console (ctest, CI), Qt logs to `OutputDebugString` on Windows and to os_log on macOS | The native lanes set `QT_FORCE_STDERR_LOGGING=1` so the backend lines reach the test log |
| In the browser, Qt's network backend never frees a body ([pj_runtime notes](../../pj_runtime/docs/TECHNICAL_NOTES.md)) | `UrlFetcher` downloads through `pj_runtime`'s `fetchBytesWithLimit()`, which uses `BrowserFetch.h` there; a redirect is followed out of sight, so `FetchResult::final_url` stays the requested URL |

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
| Non-float scalar vertex fields | CPU float conversion into the retained vertex array | Integer zero-copy needs a vertex layout and shader variant per wire datatype, NaN rejection in the shader, a re-bake of every pack and a full golden re-verification (numbers below) |
| MSAA depth for SSAO/EDL | `QRhi::ResolveDepthStencil` where QRhi reports it and multisample textures (OpenGL 4.3+, Vulkan, Metal); the single-sample depth replay elsewhere (D3D11, WebGL2, older GL) | The replay was +11.3 ms on OpenGL and +11.5 ms on Vulkan for a 10M cloud (+58%) on Iris Xe, the whole gap to the GL renderer; it is 40–45% of the static GPU time in the point benchmark |
| Mesh mask for EDL | A separate mask pass replaying the model ranges | WebGL's GLES QRhi does not resolve a second multisample colour attachment reliably |
| GL clip / Y conventions | Folded into CPU matrices (§2) | Shaders stay backend-neutral |
| Scene bounds | CPU bounds from the conversion, or a scan of the retained vertices once per revision | Nothing ever reported empty bounds with points; a compute reducer cost a pipeline, a compute bake and `StorageBuffer` usage on every instance buffer |
| Wireframe | Triangle edges as line lists on every backend | `NonFillPolygonMode` is missing on WebGL2 and would split the looks for a cosmetic gain |
| HUD and hover label | CPU-rasterized `QImage`s uploaded as texture quads | No per-context glyph atlas to lose across a QRhi teardown |
| Marker instances | A per-layer Static cache keyed on the resolved placements; the render origin stays on the CPU, and an origin-only change of a recurring layer rewrites only the translations from cached world doubles | See "Render origin" |
| Grid | Built once at the fixed-frame origin; the origin offset added in the shader | Exact (see "Render origin") |
| Output sample count | 1 when the HDR chain can run, 4 otherwise, chosen once per QRhi | A per-frame switch rebuilds every resource bound to the target (and broke the missing-HDR-variant fallback) |
| Families on demand, pipelines built as drawn | A family is built with its first layer; its registered pipelines are created only as drawn (HDR variants), the direct ones before a direct frame's first pass; a pipeline that cannot be created withholds its frame (the last frame stays), then the failure state or the direct fallback | OpenGL builds a program per pipeline object even for a shader seen before (Qt's program-binary cache still calls `glProgramBinary`, 1–4 ms each on the Iris), so every unused pipeline cost a new QRhi (a float, redock, first show) its time: 50 pipelines and 78 ms at the cloud + robot first show before, 18 and 25 ms after. Vulkan's pipeline cache makes each well under 1 ms |
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

**Integer zero-copy analysis** (`pointcloud_rhi_benchmark`, streaming quads,
10M points per sample, Iris Xe; convert = `packPointVertices()` inside the frame on
the global thread pool, as the layer runs it, wall time; decode+upload = convert
+ prepare + enqueue + endFrame own-thread CPU; 20-byte records, before the compact
formats):

| x/y/z | API | convert ms | bounds scan ms | decode+upload ms | convert share | peak RSS |
|---|---|---|---|---|---|---|
| u16 | Vulkan | 24.5 | 24.2 | 56.5 | 43% | 464 MiB |
| u32 | Vulkan | 24.6 | 30.2 | 56.6 | 43% | 502 MiB |
| f32 | Vulkan | 16.2 | 25.9 | 48.2 | 34% | 502 MiB |
| u16 | OpenGL | 25.3 | 24.8 | 85.6 | 29% | 614 MiB |
| u32 | OpenGL | 24.9 | 30.6 | 84.3 | 30% | 653 MiB |
| f32 | OpenGL | 16.8 | 26.3 | 74.9 | 22% | 653 MiB |

A zero-copy upload would save at most the conversion (22–43% of decode+upload),
and it would need its own bounds pass, which the conversion provides for free (a
single-threaded strided scan costs as much as the whole parallel conversion). Its
remaining gain was memory, which the compact formats took: an xyz-only style
retains and uploads 12 bytes a point (114 MiB at 10M), one reading a scalar or a
colour 16 (153 MiB), at or below the wire blob. The integer rows' GPU times are not comparable with f32: an integer
lattice under a scaled camera rasterizes about 20x slower.

**Render origin.** Moving a camera-relative subtraction into a float shader add
is not pixel-identical when the operands are arbitrary doubles, even when the
error is sub-ulp: marker vertices kept relative to an anchor, with anchor −
origin added in the shaders, moved shadow edges on 14 Vulkan harness images by
up to 105 px. It is exact only for float-valued operands, as the grid's are:
grid positions and the render origin (the camera focal) are floats, so the
shader's correctly rounded float add equals the float of the double subtraction.

## 5. ADS and window-system quirks

- **ADS floating turns the docked area native** (found on 6.11.1, our ADS fork):
  `CFloatingDockContainer::show()` calls `winId()` to set X11 properties (on every
  platform), and Qt then marks every child of the dock manager native. The docked
  area becomes a native child window; under Vulkan on Wayland that child surface
  stopped getting frame callbacks (the main window's docks froze once any dock
  floated, or crashed in the lavapipe swapchain).
  `Qt::AA_DontCreateNativeWidgetSiblings`, set before `QApplication` in the QRhi
  build, keeps them non-native.
- **ADS reparents docks through a parentless state.** A float, a redock and a
  move to another area (`removeDockWidget()` → `setDockArea(nullptr)`) are each a
  top-level change for a `QRhiWidget`: `releaseResources()`, then a fresh
  `initialize()`. Widgets must rebuild all their GPU state from retained CPU data:
  correct, but costly for a large 3D scene.
- **Our ADS fork keeps a dock area's hidden tabs parented** (`CDockAreaLayout`
  in `3rdparty/Qt-Advanced-Docking/src/DockAreaWidget.cpp`). Upstream ADS unparents
  the tab it hides on every tab switch (`setCurrentIndex`), and on an insert
  and a removal, so each tab switch released and rebuilt the shown view (a 10M
  cloud: a new QRhi's pipelines and a 190 MiB re-upload, and on OpenGL a stall of
  the plot shown next while the driver freed it). The patch takes the previous
  tab out of the layout (only the current tab takes part in sizing) and hides it,
  but leaves it parented; `insertWidget()` and `removeWidget()` no longer unparent
  either, since `insertDockWidget()` / `removeDockWidget()` reparent through
  `setDockArea()` right after, and a reordered tab stays in its area. Hidden tabs
  are explicitly hidden, so showing the area does not show them; they move with
  their area (a whole area floated takes them to the new window, where they
  re-initialize when shown) and die with it, as upstream's never-shown tabs
  always did. The cost: a hidden tab keeps its GPU resources (§9, note ⁷). `rhi_dock_lifecycle_test` covers reorder, close/reopen, restore,
  float of the whole area, moving and destroying a hidden tab.
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
| Model texture pixels per mesh / per layer | 64 Mi / 256 Mi | 16 Mi / 32 Mi |
| Model texture longest side (larger maps decode scaled down) | 16384 | 1024 |
| HDR chain per view / per QRhi | 1 GiB / 4 GiB | 256 MiB / 512 MiB |
| Concurrent mesh loads per layer | 4 | 1 |

The HDR chain costs 4x (RGBA16F 8 + depth) + RGBA16F 8 + RGBA8 4 + depth per
render pixel: 64 B with D32F (4 B) and 84 B with OpenGL's D32FS8 resolve (8 B).
3840x2160 is 506 MiB (664 MiB on OpenGL), so 1 GiB fits one 5K view (one 4K view
on OpenGL) and 4 GiB eight 4K docks (six on OpenGL). The largest single buffers at the desktop limits: 100 M points ×
16 B = 1.6 GB, 20 M marker vertices × 28 B = 560 MB.

Fixed sizes: shadow map 2048² D32F (`kShadowMapSize`, independent of
`render_scale`); point list up to 4 device px (`kPointListMaxPx`); flat sphere
discs below 2 device px (`kSphereDiscMaxPx`); cube LOD at 1 device px
(`kDefaultCubeLodThresholdPx`); `render_scale` in [1, 4]; trails at most 50k
points (`kMaxTrailPoints`).

**Frame budget.** On-demand repaint only: ≤ 60 Hz during playback, 0 frames when
settled (asserted by `scene3d_frame_benchmark`).

**Performance gate (Gate A).** A candidate's RHI GL within max(1.2x, +1 ms) of
the baseline's in wall time and GUI-thread CPU, RSS within 1.10x, 0 idle frames
(§9: the current baseline, the rules and the harness).

## 7. Golden images

`pointcloud_golden_test` compares 36 scenes (28 point-cloud scenes and 8 scene
corpus images: grid, markers, robot) against goldens written by QRhi OpenGL on
llvmpipe (the golden writer, below). The corpus and its full rule are in
[`widgets/tests/fixtures/golden/pointcloud/README.md`](../widgets/tests/fixtures/golden/pointcloud/README.md).
In short:

- A pixel differs when it is off its golden pixel by more than 16 in a channel;
  a scene fails above 3% differing pixels, a 5% coverage change, or a mean-colour
  change above 12.
- A pixel counts as differing only when every golden pixel in its 3x3
  neighbourhood is off (the MSAA orientation of §2), and a scene also fails
  above max(3 × 3%, 10%) plain per-pixel differences, so the allowance cannot hide
  a whole-scene shift.
- A foreground diff (differing pixels over the foreground, limit
  0.12, always with the neighbourhood rule) and a stray count (non-background
  pixels farther than 2 px from the golden's foreground, limit 4).
- Driver-dependent scenes drop the per-pixel, foreground-diff and 2 px stray
  checks for coverage and mean colour (`field_turbo_spheres`, `large_1m_points`,
  the line grids) or coverage alone (`shape_spheres_small`: its ~1.5 px discs
  rasterize at a different size per Mesa version). Both modes keep a coarse
  spatial check, which the pixel count and mean colour cannot give (a moved or
  rearranged foreground keeps both): the covered pixels per 16 px tile (sum of
  |actual - golden| over the golden's count, at most the coverage limit + 0.10),
  the per-tile mean colour (coverage-and-colour mode, at most 20), the centroid
  (2 px; 6 px for the line grids) and the bounding box (4 px) of the covered
  pixels, and the stray count outside a 4 px margin (limit 4).
  `GoldenComparator.CoverageModesRejectMovedForeground` feeds the comparator
  synthetic frames: a foreground translated by 12 px, a blob moved by 16 px or
  across the frame and a colour ramp reversed in place all keep the pixel count
  and mean colour and fail, a 1 px jitter passes.

Per-backend allowances and measurements:

| Backend | Allowance | Measured worst |
|---|---|---|
| OpenGL (llvmpipe), the writer's stack (`writer.txt`) | none: every pixel must match | exact |
| OpenGL (llvmpipe), another Mesa (CI's 23.x) | the tolerances above | foreground diff 0.028 (not re-run against the current images) |
| OpenGL (Iris, Mesa 26) | none | foreground diff 0.001, coverage within 0.1% and mean colour 0 outside `shape_spheres_small` (coverage only: per-pixel 0.013, coverage 0.997) |
| Vulkan (lavapipe, Iris) | none | foreground diff 0.082 (`field_viridis_cubes`), per-pixel 0.075 (`shape_cubes_subpixel`); line grids 15% of their foreground (hence the coverage mode) |
| D3D11 (WARP) | line scenes (grid, marker edges, line strips): coverage 0.12, foreground diff 0.2 | neighbourhood 0.019 (`field_viridis_cubes`), per-pixel 0.074 (`shape_cubes_subpixel`), point-cloud coverage within 0.6%; line grids 1.19x the golden's pixels (measured against the previous images; the current ones pass) |
| Metal (paravirtual) | none | the D3D11 bands on the point clouds; line grids 0.94x (measured against the previous images; the current ones pass) |
| All, line grids | coverage 0.08 (0.25 on D3D11/WARP) | Iris GL 1.002, lavapipe 0.989, Iris Vulkan 0.990, Metal 0.937, WARP 1.194 of the golden's pixels |
| Apple software GL | not run | sphere imposters ~10% larger, soft point sprites: coverage up to 19%, mean colour up to 58 |
| Coverage modes, spatial | line-grid centroid 6 px | Mesa 26 on llvmpipe, lavapipe, Iris GL and Iris Vulkan: tiles 0.047, tile colour 4 (9 on `shape_spheres_small`, whose colour is not checked), centroid 0.06 on the clouds and 2.0 on the line grids of the y-down backends (their mirrored MSAA keeps more of one line slope), bounding box 0, strays 0; D3D11, Metal and CI's Mesa 23 not measured |

**Golden writer.** `PJ_UPDATE_POINTCLOUD_GOLDENS=1` writes the references from
one pinned renderer, QRhi OpenGL on Mesa llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1` and
the Mesa EGL/GLX vendor); on any other backend or driver the case fails and
writes nothing. It records its device string (LLVM and Mesa versions) in
`writer.txt`; on that very stack the comparison is exact, with no tolerance. `PJ_POINTCLOUD_GOLDEN_DIR` redirects reads and writes, so a
regeneration lands in a scratch copy and is reviewed against the committed
images before it replaces them. The writer is deterministic (two runs are
byte-identical). The images keep the retired GL renderer's look: its renders of
the same scenes pass against them (the filled grids aside: it darkened the grid
under the gizmo's square), the anti-aliased corner gizmo covering the same
pixels; the README has the numbers. The filled grids render at the default
10 m and differ on y-down backends by a foreground diff of 0.004 at most
(lavapipe, Iris Vulkan).

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

**Shared mains and file-local environments.** The headless runners use the shared
test mains, which keep the application on the stack so it dies before Qt
Network's exit handlers. Per-suite setup lives in file-local GoogleTest
environments: they prepare the Cloudini blob, preserve the fixed-frame settings
application name, and redirect the model caches to temporary directories.

| Runner | What it covers | DPR 2 |
|---|---|---|
| `scene3d_rhi_render_test` | the harness: orientation, occlusion, translucency, shadows, HDR/SSAO/EDL, presentation, reparent, depth resolve vs replay | the `Presentation*` cases |
| `scene3d_rhi_point_test` | point-pass behaviour the goldens do not pin | — |
| `scene3d_rhi_failure_test` | the failure state and the optional-stage fallbacks | — |
| `scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test` | the marker and model caches, pixel-identical to the uncached frames | — |
| `scene3d_rhi_platform_test` | `max3dTextureSize()` per backend | — |
| `pointcloud_golden_test` | the golden corpus (§7) | — |
| `scene3d_frame_benchmark` | frame budgets, a smoke at default sizes; `PJ_S3D_BENCH_FULL=1` for the Gate A sizes. The smoke asserts no timing, only each case's workload: its cloud, markers or model drawn in the last frame (the view's draw counters) and, in the animated cases, data that advances (a cloud uploaded every streamed frame, the followed frame moving, the robot's pose changing the image) | — |
| `plot_rhi_render_test` (pj_plotting) | the plot canvas against Qwt's raster | yes |
| `rhi_dock_lifecycle_test`, `rhi_mixed_dock_test` (pj_app) | the floating-dock matrix, stand-ins and real widgets | yes |

`scene3d_rhi_app_smoke` (pj_app, Linux, label `qrhi-opengl`)
runs `packaging/smoke/render_check.sh` on the build tree's app.

**GPU gate.** A display pass skips when no window can be exposed (the
`offscreen` QPA, a runner without a desktop): the GUI test main probes once per
pass. `PJ_REQUIRE_GPU_TESTS=1` turns the skip into a failure, and every CI GPU
lane sets it. A render case whose canvas initialized a backend and then failed
fails instead of skipping.

**Test hooks.** Builds with tests define `PJ_RHI_TEST_HOOKS` (root CMake,
test-enabled desktop builds; release builds configure `PJ_BUILD_TESTS=OFF`).
The named fault injection and the QRhi counters of every GPU widget live in
`pj_widgets/RhiTestHooks.h`:

- `PJ_RHI_TEST_FAIL_SHADER=<suffix>` fails every pipeline whose shader path
  ends with it (Scene3D's `createPipeline()`, and `createRhiPipeline()`
  for the plot canvas and the media viewer);
- the `pjTestFailPipeline` dynamic property fails a named pipeline of the plot
  canvas or the media viewer, and `pjTestPipelineAttempts` counts attempts; on
  the Scene3D view it fails an allocation part way through a quality stage
  (`shadow target`, `hdr attachments`, `ssao target`, `edl target`), which
  `scene3d_rhi_failure_test` drops and recovers stage by stage;
- `pjTestRhiAdoptions` / `pjTestRhiReleases` / `pjTestRhiStale` count QRhi
  adoptions (`PJ::noteRhiAdoption()`), which `rhi_mixed_dock_test` checks across
  dock moves: every float and redock re-initializes the moved widget, and no
  widget meets a new QRhi before releasing the old one's resources;
- `SceneViewTestAccess::setForceDepthReplay()` keeps the depth replay where the
  chain would resolve depth; `scene3d_frame_benchmark`'s `DepthReplayOnOff` times the two;
- `MarkerRhiPass::setIdleReleasePausedForTest()` holds kept marker expansions
  through idle spells (the 250 ms wall-clock release), so the frame-window
  checks of `ExpansionIsKeptOnlyWhileReuploading` cannot race a slow grab;
  releasing it re-arms the release, which the test then waits for;
- `QT_RHI_LEAK_CHECK` catches resources left on a dying QRhi on Vulkan, D3D11
  and Metal; on OpenGL uniform buffers are CPU-side and untracked, hence the
  counters.

**View test access.** The view has no test API of its own: the tests, the
benchmarks and the browser acceptance probes reach its internals through
`SceneViewTestAccess` (`widgets/testing/`, target `pj_scene3d_widgets_testing`,
a friend of the view): the view's own last-frame counters, the passes
themselves (`pointPass()`, `markerPass()`, ...: each family's counters and
bounds), each quality stage's state (`stage()`), the built families and the
pipeline registry (`builtFamilies()`, `pipelineCounts()`: what stays unbuilt),
and the overrides (`forceStageFallback()`, the depth replay,
`discardShadowMap()`). It is independent of `PJ_RHI_TEST_HOOKS` (the WASM acceptance
build has tests off), and the app never links it. Its stage times alone need
the hooks.

**Display runners in parallel.** Every CI lane runs the display runners (label
`qrhi`) serially; only the rest of the suite runs under `ctest -j`. A local
`ctest -j` of the display runners on one Wayland compositor (a headless GNOME
Shell) lets the tests' windows cover each other, and the compositor sends
no frame callbacks to a covered surface: a `grabPresentedFrame()` then times out
("presented no frame to grab (occluded window?)"). `rhi_mixed_dock_test` fails
this way in about 3 of 4 parallel runs, and in none with one compositor per
process or with xcb (Xwayland). Run the display runners serially, one compositor
each, or through xcb when parallel.

**Mixed device-pixel ratios.** `FloatingDockMovedToAScreenWithAnotherPixelRatio`
(`rhi_dock_lifecycle_test`) and `FloatingDocksMovedToAScreenWithAnotherPixelRatio`
(`rhi_mixed_dock_test`) move a floating dock to a screen with another ratio. They
need two screens whose ratios differ, on xcb (a Wayland compositor places windows
itself, so they skip there): they skip without such screens, fail with
`PJ_REQUIRE_MIXED_DPR=1`, and fail when the move leaves the ratio unchanged. xvfb
cannot provide them: every RandR output of Xvfb is named `screen`, so
`QT_SCREEN_SCALE_FACTORS` gives both the last factor; `xrandr --setmonitor` adds no
monitor there; and two X screens (`-screen 0 .. -screen 1 ..`, RandR off) make
`QWindow::setScreen()` hide and re-create the window, not change its ratio live.
A headless mutter (or GNOME Shell) with two virtual monitors does: its Xwayland
has one named output per monitor, and xcb moves the window between them with the
window exposed throughout. Locally:

```bash
XDG_RUNTIME_DIR=<dir> dbus-run-session -- gnome-shell --headless --wayland \
  --virtual-monitor 1024x768 --virtual-monitor 1024x768 --wayland-display <name> &
# DISPLAY: "Using public X11 display :N" in its log; XAUTHORITY: <dir>/.mutter-Xwaylandauth.*
QT_QPA_PLATFORM=xcb QT_SCREEN_SCALE_FACTORS="1;2" PJ_REQUIRE_MIXED_DPR=1 \
  ctest --test-dir build -R 'AnotherPixelRatio'
```

Dumps: `PJ_RHI_TEST_DUMP_DIR` saves the harness frames, and
`PJ_POINTCLOUD_GOLDEN_OUT` / `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1` the golden actuals
and diffs.

**CI lanes.**

| Workflow | Lane | Runs | Gate |
|---|---|---|---|
| `linux-ci.yml` | `build` (QRhi) | the whole suite under xvfb, every `.opengl`, `.opengl_dpr2` and `.vulkan` pass (lavapipe via `VK_ICD_FILENAMES`), `PJ_REQUIRE_GPU_TESTS=1` | blocking |
| | OpenGL floor step | `scene3d_rhi_render_test.opengl` and `rhi_dock_lifecycle_test.opengl` with Mesa capped at GL 3.3 | blocking |
| | mixed-DPR step | the two `*AnotherPixelRatio` cases, `.opengl` and `.vulkan`, on a headless mutter's Xwayland with two virtual monitors, `QT_SCREEN_SCALE_FACTORS="1;2"`, `PJ_REQUIRE_MIXED_DPR=1` | non-blocking (`continue-on-error`) until it has run green on the runner's mutter 42 |
| `windows-ci.yml` | `build` (QRhi) | the unit suite (`-LE qrhi`), then `-L qrhi-d3d11` with the `windows` QPA, `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`; `scripts/ci/check_qrhi_backend.sh` fails unless every view and plot canvas logs `D3D11` on a `Cpu` (WARP) adapter | compile and display step blocking; unit step non-blocking |
| `macos-ci.yml` | `qrhi-metal` (`depot-macos-14`) | a Metal device and presentation probe, then `-L qrhi-metal` (and, on scheduled and manual runs only, `-L qrhi-opengl`) with the `cocoa` QPA, each requiring every view and plot canvas on its backend, then `-LE qrhi`; on a failure it reruns up to three failed QRhi cases under lldb (their backend from the ctest name) for every thread's backtrace | nightly when the default branch moved, and on demand |
| `pixi-ci.yml` | linux-64, win-64 | the QRhi build against conda Qt; win-64 runs the whole suite with the display passes skipped (its runner exposes no window) | — |
| `sanitizers.yml` | asan, msan, tsan | the Clang 22 + libstdc++ build, offscreen (`test.sh`), so every display case skips; `tsan.supp` carries audited entries for `LatestWinsWorker`'s Qt hand-offs | nightly, on push to main, on dispatch |
| `wasm-ci.yml` | | build, Playwright suites, the Scene3D capability probe | — |
| release workflows | | the render check on the IFW install (D3D11/WARP) and on the .deb and AppImage (Ubuntu 22.04 and 24.04) | see `packaging/README.md` |

**ASan on a display (local).** No CI lane runs the display cases instrumented.
Before merging a change to QRhi resource lifetimes or failure handling, run the
`.opengl` (llvmpipe) and `.vulkan` (lavapipe) passes of `scene3d_rhi_failure_test`,
`scene3d_rhi_marker_cache_test`, `scene3d_rhi_model_cache_test`,
`rhi_dock_lifecycle_test`, `rhi_mixed_dock_test` and `plot_rhi_render_test` under
ASan with `./test.sh --sanitize asan --display`, `PJ_REQUIRE_GPU_TESTS=1`, in a
`dbus-run-session` (without a session bus, Qt's D-Bus client on xcb leaks a libdbus
buffer and LeakSanitizer fails every case): 154 cases, 89 s on a 20-core machine.
The command is in [docs/SANITIZERS.md](../../docs/SANITIZERS.md#asan--ubsan).

The macOS OpenGL step is a functional smoke run, not a parity gate (Metal is):
it leaves out what measures Apple's software renderer rather than ours, namely the
point-cloud goldens, the sub-pixel sprite colour check (1 px sprites resolve to
~15% coverage there), the odd-cell-edge grid line check (its 1 px analytic line
loses the same coverage there) and the DPR 2 mixed-dock pass (every float
recompiles the pipeline set at 4x the pixels; Linux runs it). It runs only on the
nightly and manual runs (up to 24 min on the software renderer); pull requests gate
on Metal, and Linux runs every OpenGL case on every PR.
The lane runs on Depot's `depot-macos-14` (7 cores, the same paravirtual Metal
device, presents). Depot's macOS capacity is not elastic (a job once queued 40 min),
so GitHub's `macos-14` stays a `workflow_dispatch` choice for when it queues.

## 9. Performance (Gate A)

The current QRhi baseline: the RHI GL and RHI VK columns of the last run
against the retired GL renderer (that run, with its GL column and verdicts, is
archived in `docs/archive/QRHI_MIGRATION.md`, "Final Gate A against the GL
renderer"). Method: `scripts/perf/gate_a.sh` (see "Harness" below) on Intel Iris
Xe (RPL-P), Mesa 26.0.3, i915, AC power, `performance` governor with the P-cores
capped at 2.4 GHz; xcb on a private rootful Xwayland in a headless GNOME Shell,
vsync off (`vblank_mode=0`, `MESA_VK_WSI_PRESENT_MODE=immediate`); 5 interleaved
rounds, one process per case and configuration, each the median of 30 frames
after the warm-up. Median [min–max] where the spread exceeds 10%. Run of
2026-09-28, load average 1.3–3.3; the cloud, stream, dock and first-show rows are
from a second run that day (`--only`, load 1.3–2.9), and the two marker rows from
a third (`--only MarkersPanFollow`, 3 interleaved rounds, load 4.3–8.5). The RTX
4070 column is still open (QRHI_NEXT.md).

Gate A judges a candidate's RHI GL, Linux's native backend, against a baseline's:
time within max(1.2x, +1 ms), memory within 1.10x, the row's own limit where it
has one, idle frames 0, and "regressed" when, over at least 3 complete rounds
each, every candidate round is worse than every baseline round and the median by
more than 2%. RHI VK (opt-in on Linux) is for reference.

| Row | RHI GL | RHI VK |
|---|---|---|
| Cloud 10M static: wall / CPU ms | 19.6 / 0.6 [0.6–0.7] | 19.9 / 0.6 |
| Cloud 10M static: RSS MiB ¹ | 247.2 | 294.5 |
| Cloud 10M GPU (DRM MiB; DPR 2 ≤ 550) | 206.7; 453.2 | 426.0; 660.7 |
| Cloud stream 300k/frame: wall ms ² | 1.9 [1.8–2.2] | 7.5 [6.8–8.2] |
| Cloud stream: GUI CPU ms / RSS MiB | 1.0 [0.9–1.1] / 256.0 | 1.1 / 303.4 |
| Cloud stream: ingestion ms (not gated) | 1.9 | 1.9 |
| 100k cubes + 1M line vertices, orbit: wall / CPU ms ³ | 23.5 / 0.6 [0.6–0.7] | 23.4 / 0.7 |
| Cubes + lines: RSS / peak MiB | 301.7 / 353.6 | 348.5 / 400.3 |
| 10k markers, pan + follow: wall ms ⁴ | 7.6 | 8.2 |
| Markers: GUI CPU ms | 0.9 | 0.9 |
| 50-link robot, TF each frame: wall / CPU ms ⁵ | 2.0 [1.7–2.3] / 0.7 [0.7–1.0] | 3.4 [2.5–4.4] / 0.8 [0.7–0.9] |
| 10 / 100 / 1000 layers: wall ms | 1.9 [1.4–2.1] / 1.9 [1.5–2.1] / 3.3 | 1.7 [1.5–2.7] / 2.2 [2.0–3.6] / 8.9 [7.0–9.3] |
| 1000 layers: RSS MiB / first show ms | 216.8 / 146.4 | 299.8 / 254.6 |
| 10M cloud dock: float / redock / hide-show ms ⁶ | 131.3 / 126.6 / 1.9 | 198.0 / 161.2 / 2.2 [2.1–3.0] |
| 50-link robot dock: float / redock / hide-show ms ⁶ | 62.1 / 27.5 / 1.9 [1.9–2.1] | 50.2 / 13.1 / 2.2 [2.0–3.3] |
| ADS dock-area tab switch, 9.9M cloud: to the cloud / to the plot, ms ⁷ | 4.5 / 5.2 [4.6–5.2] | 4.9 [4.3–5.2] / 13.0 |
| First show, 10M cloud + robot, ms ⁶ | 187.6 | 316.0 |
| Idle frames when static, every case | 0 | 0 |
| Plots: dashed 5 x 100k, wall ms ⁸ | 1.6 [1.5–1.7] | 1.8 [1.6–2.0] |
| Plots: 50 x 100k, wall ms ⁸ | 11.2 | 11.4 |
| Plots: tracker only, 10 x 100k, wall ms | 2.6 | 6.2 [5.5–7.4] |
| Plot GPU memory, 1600x900, DPR 1 / 2 (DRM MiB) | 45.0 / 183.2 | 106.1 / 315.8 |
| Streaming plot, 10 x 100k, ≤ 16.7 ms | 10.9 [10.7–12.9] | 19.4 |
| Mixed: 6 plots + Scene3D, wall / CPU ms | 37.1 / 8.8 | 49.0 / 9.0 [8.8–10.0] |
| Mixed: RSS MiB | 258.4 | 302.7 |

¹ The 10M cloud settles on one 114 MiB anonymous block: the ObjectStore's wire
bytes, which the zero-copy decode reads in place. RHI GL drops its 12-byte `PointVertex` array (114.4 MiB) once the upload is enqueued
(ARCHITECTURE, "Eviction"); a stream keeps its array. Peak RSS during the load
is 362.7 MiB (not gated): the array lives until the upload's frame is
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
frame. The GUI thread's CPU is gated; the pool adds CPU the GUI
thread does not see (about 1 ms per frame at 300k; at 10M, 8 chunks cut 70 ms of
wall for 19 ms more process CPU). The ingestion (the untimed push into the
ObjectStore) is in neither wall nor CPU. On Vulkan the frame waits for its GPU
work inside the offscreen frame (see "Reading the numbers").

³ The shadow map is kept while its inputs hold (ARCHITECTURE, "Shadows"): an
orbit of a static scene renders it once (27.9 → 23.5 ms on RHI GL). The frame is
GPU-bound in the HDR chain: 7.0 ms without it, SSAO and EDL under 0.3 ms each.

⁴ GPU-bound, the HDR chain about 1.4 ms of it. Markers cast no shadows, so a pan or follow, which moves the render origin every frame,
renders no shadow map. The GUI thread's work per frame is the origin-only update
of the recurring layer (about 0.55 ms), the upload and `endFrame`.

⁵ i915 settles each configuration at its own clock once warmed up: 1500 MHz for
RHI GL on this row, 600 MHz on Vulkan (every light row). `cold_ms`, the first
frame after a second of idle, is 1.0 ms on RHI GL and 5.1 on Vulkan.

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

⁷ Our ADS fork keeps a dock area's hidden tabs parented (§5), so a tab switch
adopts no QRhi, builds nothing and uploads nothing, and a Show whose target
still holds the last frame renders nothing (ARCHITECTURE, "Widget lifecycle and
reparenting"): the switch costs the same at 10.6k and 9.9M points (RHI GL 4.5 /
5.2 ms, Vulkan 4.9–5.1 / 13.0 ms; the Vulkan plot tab's 13 ms is the plot
canvas's own synchronous offscreen frame). Hidden tabs keep their GPU memory; DRM MiB with a 1M-point
Scene3D tab and K plot tabs visited (1260x780), upstream ADS → our fork:

| Plot tabs | 1 | 4 | 8 |
|---|---|---|---|
| RHI GL | 132 → 138 | 132 → 158 | 132 → 184 |
| RHI VK | 366 → 302 | 366 → 366 | 366 → 366 (allocator blocks) |

About 6.5 MiB per hidden plot on RHI GL; a hidden
Scene3D tab keeps its whole scene (a 10M cloud: 114 MiB of vertices and the HDR
targets). RSS is unchanged.

⁸ The plot rows replot unchanged data every frame. The app's datastore curves
are `RevisionedSeries` (pj_plotting CLAUDE.md, "Dirty layers"), and so is the
benchmark's data: an unchanged replot keeps the RHI canvas's curve layer. With
plain Qwt data (`plot_canvas_benchmark --generic-series`, 3 rounds, wall / canvas
CPU ms) every replot rebuilds:

| Case | RHI GL | RHI VK |
|---|---|---|
| 50 x 100k static | 85.2 / 81.9 | 107.1 / 90.9 |
| 5 x 100k dashed | 21.8 / 21.1 | 26.5 / 22.3 |
| 1M static | 13.8 / 13.4 | 17.2 / 15.1 |

Streaming and zoom/pan change the data or the maps every frame and rebuild in
both modes; the tracker case replots overlays only.

Markers with long line strips (1.1M strip points in 110 strips, OpenGL, GUI-thread
CPU per frame): a pan (origin-only update) 18.8 ms, a full rebuild each frame
18.8 ms, a live 100k-point layer with a new sample each frame 9.6 ms; with
per-point colours 18.9, 17.2 and 15.0 ms. What remains of a pan is the vertex
write (2.2M vertices, 62 MB) and the driver's copy of it in `endFrame` (21% of
the frame).

Reading the numbers:

- Vulkan on the Iris costs more wall time than OpenGL wherever a frame has GPU
  work: a `QRhiWidget` frame is an offscreen frame, which on Vulkan submits and
  waits for its fence (wall = CPU + GPU), where OpenGL overlaps the GPU with the
  next frame (the stream: 8.9 vs 2.2 ms).
- Buffer uploads: a cloud's first upload goes into an `Immutable` buffer (one
  copy; Vulkan then frees the staging, which a `Static` buffer keeps: the 10M
  static cloud 684 → 428 MiB DRM), later ones into `Static`. The layers pad their
  arrays past 64 KiB to a 64 KiB multiple, because Mesa's anv records a buffer copy of an
  awkward size slowly (QRHI_NEXT.md): a 1000003-point stream takes 21.4 ms
  padded, 43.3 unpadded; a 10000019-point first upload 79.9 ms padded, 163.2 ms
  unpadded.
- A settled OpenGL view keeps the last frame's uploads until its next frame:
  QRhi's GL backend retains each upload's data in the command buffer until the
  next `beginFrame` (`retainBufferData`), 53 MiB after a pan of the 1.1M-point
  strips above. Vulkan copies into staging and keeps nothing; the marker pass
  itself drops a kept expansion 250 ms after the last frame.
- QRhi's Vulkan backend frees an upload's staging buffer only frames later, so a
  settled view (which renders no more frames) keeps reporting it in the DRM
  total; the multisample depth texture of the resolve chain also comes from a
  different memory pool than a renderbuffer, reserving one more allocator block.
- The software rasterizers run the frame on the GUI thread for OpenGL and on
  worker threads for Vulkan, so only wall time compares between them.

### Harness

`scripts/perf/gate_a.sh --candidate <tree> [--baseline <tree> | --baseline-run
<dir>]` runs every Gate A case with the method above and builds nothing: it
starts its own headless GNOME Shell and rootful Xwayland (`--display`, default
`:96`), runs each case in its own process with the configurations (the
candidate on OpenGL and Vulkan, `rhi_gl` and `rhi_vk`, plus a `--baseline` tree
on OpenGL, `base`) interleaved within each round, keeps every run's output under
`<out>/raw/<round>/`, and ends with `scripts/perf/gate_a_report.py <out>`, which
writes `<out>/gate_a.md`: the median of the rounds with [min–max] where the
spread exceeds 10%, the ratio and the verdict of each row: pass, fail, or
incomplete when a gated measurement is missing or a run failed (each log ends
with the run's exit status) and nothing present fails the row. `--baseline-run`
compares against the `rhi_gl` runs of an earlier run directory instead (a frozen
baseline, such as the run of §9), so only the candidate runs; a baseline
tree interleaved in every round is the less noisy comparison. Every round under
`raw/` is expected of every configuration and case a row gates, so a run absent
from a round is missing; the idle row needs every idle-reporting case. Each tree
builds `scene3d_frame_benchmark`, `plot_canvas_benchmark` and
`mixed_layout_benchmark` (`PJ_BUILD_DEMOS=ON`). Run it under the machine's build
lock, `scripts/perf/build_slot.sh` (no compile may overlap a round); `--first-round K --rounds 1` runs one
round at a time into the same `--out`, so builds can run between rounds. A round
whose load average stands out is rerun, not averaged in. Start the display
outside the lock and pass `--keep-display`: a compositor started under a plain
`flock` (without the `-o` that `build_slot.sh` passes) inherits the lock and
holds it after the run.

Every per-frame benchmark (`scene3d_frame_benchmark`, the mixed layout, the
plots) warms up with `pj_bench::WarmUp`: at least 250 ms of frames, the last 5
within 10% of each other and the i915 clock (`gt_act_freq_mhz`) unchanged for
100 ms, capped at 120 frames or 2 s. The first warm-up frame, after a second of
idle, is reported as `cold_ms`: the first interaction a user sees, with the GPU
still at its idle clock. The warm clock is a steady state per configuration, not
a constant: on this Iris the robot row settles at 1500 MHz on RHI GL, and RHI VK
stays at 600 MHz on every light row, so those rows compare clock policies as much
as backends (`gpu_mhz` at the end of the warm-up,
`gpu_mhz_range` over the measured frames).

What the benchmarks report besides wall, CPU and memory:

- `scene3d_frame_benchmark`: per-frame wall p90 and max, GUI-thread CPU p50 and
  p90, minor page faults per frame (the whole process), upload work per frame
  (point-cloud uploads and MiB, marker rebuilds, model mesh builds), and, with
  `PJ_RHI_TEST_HOOKS`, a stage split per frame (`stages_ms`: step, sched, targets, admit,
  prepare, submit, present, init; the header of the file defines them). A case
  that ingests data per frame (`CloudStream`) does it before the timed request:
  its wall, CPU and faults exclude it and `ingest_ms` reports it. The dock and
  first-show cases sum the stages of every full render in the operation (`float_stages_ms`,
  `first_stages_ms`, ...; `sched` is the rest of the wall outside `render()` and
  present), and count the base builds (`*_inits`), paint events, render calls
  and skipped paints (`*_paints`, `*_renders`, `*_skips`).
  `PJ_S3D_BENCH_CSV_DIR` writes every frame's sample; `window_ns` is the
  measured span in `CLOCK_MONOTONIC`, to cut a `perf record -k CLOCK_MONOTONIC`
  profile to it.
- `mixed_layout_benchmark --tab-switch N`: a real ADS dock-area tab switch
  (`setAsCurrentTab()`) between a Scene3D dock and the plot docks, all tabs of
  one area, the camera fixed as in the frame case and rendered before the
  layout settles. Every tab is shown once
  first; then each switch is timed to the shown widget's first frame, with the
  Scene3D view's QRhi adoptions, base builds, uploads, paints, render calls,
  skipped paints and full renders per switch, and the memory with every tab
  visited (`--plots K` for K plot tabs).

`scene3d_frame_benchmark` has attribution switches, not for the gate:
`QSG_RHI_PROFILE=1` reports `gpu_ms` (QRhi timestamps, which slow the frame),
`PJ_S3D_BENCH_NO_HDR=1` forces the direct pass, `PJ_S3D_BENCH_NO_SHADOWS=1` turns
shadows off and `PJ_S3D_BENCH_NO_SSAO=1` / `PJ_S3D_BENCH_NO_EDL=1` those stages.

## 10. Known gaps

- A move to another dock area (a drag within the window) still rebuilds the
  view like a float: ADS unparents the dock on the way (§5).
- Hidden tabs keep their GPU resources (§9, note ⁷).
- Markers re-upload on pan and follow (the render origin is in their cache key);
  a recurring layer updates only its translations and stream positions. A model
  scene's shadow map is rendered again on every pan or follow frame, for the
  same reason.
- A runtime HDR refusal on a GPU that can run the chain (size, budget, a missing
  HDR variant) draws its direct pass without MSAA: the widget's sample count is
  chosen once per QRhi.
- Large point sprites are quads above `kPointListMaxPx` (4 px), about 1.2x the GL
  build's `gl_PointSize` sprites on Iris Xe (a 1M 0.1 m sphere cloud: 34.9 vs
  29.6 ms on OpenGL; the 9.9M-sphere tab-switch lattice: 400 vs 310 ms, 311 ms
  as a point list; QRHI_NEXT.md).
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
- On the same renderer `ShadowToggleRebindsKeptMeshes`'s bit-identical check of a
  shadowed frame before and after a shadow toggle failed 3 of its 5 CI runs (the
  frames differ with the same inputs); the macOS
  `qrhi-opengl` step excludes it, and Mesa (OpenGL, Vulkan) and Metal run it.
- The AppImage smoke runs after its upload; a `workflow_dispatch` of the release
  workflows on a branch is its only pre-merge gate (`packaging/README.md`).
- Vulkan (opt-in on Linux) costs more wall time than OpenGL wherever a frame has
  GPU work (§9, "Reading the numbers").
- Pixi CI runs no GUI tests: conda Qt stalls without a GPU.
