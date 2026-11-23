# pj_scene3D technical notes

What was observed, measured or found in external components behind the renderer, with the conditions and when to
look again. How the code works is in [ARCHITECTURE.md](./ARCHITECTURE.md). Qt behaviour below was read or found on
Qt 6.11.1 unless stated; the pinned Qt is `PJ_QT_VERSION` in `versions.env`. QRhi has no cross-minor compatibility
guarantee, so a Qt upgrade re-tests every entry marked "re-test on a Qt upgrade" and re-runs the goldens and Gate A
on every lane.

## Graphics API per platform

| Platform / channel | Backend | Fallback | CI renderer |
|---|---|---|---|
| Linux | OpenGL, 3.3 core at least | Vulkan opt-in (`PJ_GRAPHICS_API=vulkan`) | llvmpipe; lavapipe for Vulkan |
| Windows | D3D11 | WARP, chosen by Qt | WARP |
| macOS arm64 | Metal | OpenGL 4.1 core | the runner's paravirtual Metal device; Apple's software GL |
| WebAssembly | WebGL2 | — | Chromium's SwiftShader; Firefox on llvmpipe |

`pj_app` resolves the API right after constructing `QApplication`, before any GPU widget exists: a top-level picks
its API only once. Re-test on a Qt upgrade (`qbackingstorerhisupport.cpp`, `qrhiwidget.cpp`):

- **D3D11:** the backing store creates the device on the first DXGI adapter and retries with
  `QRhi::PreferSoftwareRenderer` (WARP) only if that fails; PJ adds no fallback (WARP beats Windows' GL 1.1
  software driver). `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` selects WARP (`deviceType=Cpu`) up front, so CI sets it
  rather than rely on adapter order.
- **Metal:** the backing store probes Metal and, when the probe fails (a VM without a Metal device), gives up with a
  debug message despite its comment promising an OpenGL fallback; every `QRhiWidget` would stay blank.
  `preferredGraphicsApi()` runs `QRhi::probe(Metal)` itself and picks OpenGL. Not verified on a real Mac.
- **Overrides:** a non-default `PJ_GRAPHICS_API` is probed with a throwaway QRhi (Vulkan on Qt's default instance,
  which the window then reuses: about 10 ms net on Iris Xe); one that cannot start falls back with a `pj.graphics`
  warning.
- **Metal on GitHub-hosted `macos-14`:** `MTLCreateSystemDefaultDevice` returns "Apple Paravirtual
  device", a `CAMetalLayer` drawable presents, and QRhi reports `Metal, deviceType=Integrated`.

**macOS OpenGL fallback.** macOS gives an OpenGL QRhi a legacy 2.1 context unless the window asks for core, below
every pack's GLSL 330, so `pj_app` and the GUI test main set a 4.1 core default `QSurfaceFormat` before
`QApplication` (Metal ignores it). Qt creates a never-shown widget's grab QRhi with a default `QSurfaceFormat`, so
such a grab cannot render Scene3D there; the app never grabs an unshown view. CI's only macOS GL is Apple's software
renderer ("Apple Software Renderer 4.1"), 10-40x slower than Metal: a float/redock case took 40-60 s instead of 1-2 s.

**OpenGL floor.** Every pack's lowest desktop GLSL is 330 and Qt never picks a GLSL above the context's, so OpenGL
3.3 core runs everything. The Linux CI floor step caps Mesa at 3.3 (`MESA_GL_VERSION_OVERRIDE`). Below GL 4.3 there
is no depth resolve (see "Qt workarounds").

## Backend conventions

- **Clip space, NDC and texture y.** OpenGL is y-up with −1..1 clip depth; Vulkan y-down in NDC; D3D and Metal y-up
  in NDC with texture row 0 at the top; Vulkan, D3D and Metal clip depth 0..1. The renderer folds all of it into the
  uploaded matrices (`rhi_conventions.h`). The fullscreen triangle has a `-DPJ_FLIP_Y` bake for D3D and Metal rather
  than `gl_FragCoord` UVs, which assume equal input and output sizes (false with `render_scale`).
- **MSAA orientation.** A y-down framebuffer (Vulkan, D3D, Metal) mirrors the 4x sample pattern relative to OpenGL.
  Diagonal silhouettes, cube edge bands and 1 px points resolve differently on up to 8% of pixels while interiors are
  identical, even on the same Mesa rasterizer; no integer pixel shift explains it. The golden comparator tolerates
  it ("Golden images").
- **D3D11 links stages by register, not location.** SPIR-V Cross names varyings `TEXCOORD<location>`, but fxc packs a
  pixel shader's inputs in declaration order, so a fragment shader that drops a leading input reads every later
  varying from the wrong register; OpenGL, Vulkan and Metal hide it. Rule: varyings contiguous from location 0, so a
  fragment shader's inputs are a prefix of its vertex shader's outputs (`scene3d_shader_constants_test` checks each
  pack's reflection).
- **D3D11 needs the matrix slice** on unrolled `mat4` vertex inputs (the last `QRhiVertexInputAttribute` argument):
  without it the input layout fails with `E_INVALIDARG`. Other backends ignore it.
- **Vulkan draws nothing when the viewport is set before the pipeline is bound;** set it after each
  `setGraphicsPipeline()`.
- **3D texture limit.** QRhi reports none, so `max3dTextureSize()` asks once per QRhi: GL/WebGL
  `GL_MAX_3D_TEXTURE_SIZE`, Vulkan `maxImageDimension3D`, D3D11 by feature level (Qt fills `featureLevel` from the
  created device), D3D12 and Metal their documented 2048. A backend that cannot be asked gets
  `kGuaranteedMax3dTextureSize`, logged once. The D3D and Metal values are documented, not measured. The browser
  engines measured 2048 per dimension on Chromium, Firefox and WebKit.

## Qt workarounds

Each found on Qt 6.11.1 and kept until a test shows the pinned Qt fixed it; re-test on a Qt upgrade.

| Behaviour | Workaround |
|---|---|
| `grabFramebuffer()` on a never-shown widget creates a new QRhi without calling `initialize()`, so a second grab renders with the previous QRhi's resources | grab a hidden widget only once (`grabPresentedFrame()`) |
| The OpenGL QRhi advances its frame number only in swapchain frames, so its "SRB already current" cache survives grab-only frames and a second pipeline sharing an SRB can draw with an older camera | `grabPresentedFrame()` presents one real frame first |
| The OpenGL depth resolve blits depth and stencil together, so a stencil-less D32F framebuffer is incomplete and the resolved depth stays unwritten | OpenGL resolves D32FS8 |
| The OpenGL backend reports `MultisampleTexture` from GL 3.0 but allocates with `glTexStorage2DMultisample` (core from 4.3): on a 4.1 context `create()` crashes | the resolve also requires GL 4.3, GLES 3.1 or `GL_ARB_texture_storage_multisample` (`canCreateMultisampleTextures()`); others replay |
| The backing store's Metal probe gives up without the OpenGL fallback its comment promises | `preferredGraphicsApi()` probes Metal itself |
| `QRhiWidget` paints synchronously inside every Show (on Vulkan a fence wait), even on the same QRhi and size; an `update()` on a hidden widget sets `renderToTextureReallyDirty`, so a parent repaint (an ADS tab switch) paints it again | `paintEvent()` skips the paint while the target holds the last frame and nothing changed (ARCHITECTURE "Widget lifecycle and reparenting") |
| The backing store creates the widget's QRhi with only the API and debug layer, so `EnablePipelineCacheDataSave` cannot be set and `setPipelineCacheData()` would come after Qt's own pipelines | none: pipeline-cache persistence is unreachable; OpenGL still persists program binaries through Qt's shader disk cache |
| Without a console (ctest, CI) Qt logs to `OutputDebugString` on Windows and os_log on macOS | the native lanes set `QT_FORCE_STDERR_LOGGING=1` |
| In the browser Qt's network backend never frees a body (see `pj_runtime/docs/TECHNICAL_NOTES.md`) | `UrlFetcher` downloads through `fetchBytesWithLimit()`; a redirect is followed out of sight, so `FetchResult::final_url` stays the requested URL |
| Qt's GL backend retains each upload's data in the command buffer until the next `beginFrame` (`retainBufferData`), so a settled view holds its last uploads (53 MiB after a pan of 1.1M marker strip points) | none on our side: only an extra frame would release it, which the idle rule forbids; the fix belongs in Qt |
| The OpenGL backend (desktop GL and WebGL) ignores `QRhiTexture::sRGB` for uncompressed formats, so a sampled sRGB texture reaches the shader as linear bytes (Qt version not recorded) | model texture slots are plain RGBA8 and `model.frag` decodes the colour slots itself (`model_rhi_pass.cpp`) |
| On GLES Qt recommends multisample renderbuffers over multisample textures, and WebGL2 offers the former where the latter are unavailable; without backing-format hints the resolves get QRhi's RGBA8 / default depth (not re-checked against Qt docs) | the WebGL2 HDR chain passes backing-format hints so the resolves are RGBA16F / D32F (`scene_view_widget_rhi_quality.cpp`) |
| On Vulkan a `QRhiWidget` frame is an offscreen frame that submits and waits for its fence, so wall = CPU + GPU where OpenGL overlaps the GPU with the next frame | none: Qt's design. Reproducer: any `QRhiWidget` with ~20 ms of GPU work, timed back to back on Vulkan and OpenGL |

**Clang 22 with libstdc++ 12 drops `std::visit`'s call for variant alternative 0** in `submitScenePass`: a generic
lambda fails to deduce, a templated visitor's call is never emitted (unresolved symbol). `PreparedFrame::DrawRef`
therefore starts with `std::monostate`. Only the sanitizer lanes build with Clang + libstdc++; re-test when they move
to a newer libstdc++.

**CTest splits `;` inside a property value:** `ENVIRONMENT "A=x;QT_SCALE_FACTOR=2"` drops the scale factor, so the
DPR 2 passes use `ENVIRONMENT_MODIFICATION`.

## Measured design choices

All on Intel Iris Xe (RPL-P), Mesa 26, unless a row names another GPU.

- **Quads, with a point list only up to 4 px.** D3D has no `gl_PointSize`. `demos/pointcloud_rhi_benchmark`, 10M
  2 px points at 1920x1080 into the 4x RGBA16F chain with depth replay, median GPU time from QRhi timestamps:

  | GPU / API | quads | point list | quads, re-uploaded each frame | point list, re-uploaded |
  |---|---|---|---|---|
  | RTX 4070 Laptop, Vulkan | 14.8 ms | 6.8 ms | 36.9 ms | 29.3 ms |
  | RTX 4070 Laptop, OpenGL | 13.6 ms | 7.6 ms | 40.5 ms | 93.2 ms |
  | Iris Xe, Vulkan | 90.9 ms | 49.2 ms | 96.4 ms | 63.3 ms |
  | Iris Xe, OpenGL | 82.8 ms | 44.2 ms | 82.2 ms | 81.0 ms |

  Quads cost 1.8-2.2x the point list on real GPUs, above the 1.5x gate, so small sprites use a point list where the
  backend has it. Larger sprites are fill-bound either way, and a quad keeps its visible part at the frustum edge
  (a point list culls the whole sprite when its centre leaves the view).
- **Dense large sprites.** A 215³ lattice (9.9M spheres of 0.1 m, ~50 px each; `mixed_layout_benchmark --plots 0
  --cloud-side 215`) takes 400 ms per frame on RHI GL as quads, 311 ms as a point list. SSAO and EDL cost nothing
  measurable, the HDR chain 125 ms, disabling blending nothing: the gap is sprite geometry. Untried, none
  pixel-identical: a point list above 4 px, one triangle per sprite, colour at the provoking vertex (which differs per
  backend). Revisit with a use case that draws many large sprites.
- **Whole-cloud cube LOD, not per instance.** A per-instance split is slower than no LOD, because a
  degenerate-clipped fan instance still runs its vertex invocations: on an RTX 4070 at 500k points with the camera
  inside the cloud it ran 0.91x (0.70x as more cubes converted), while the whole-cloud switch is 1.00x there and
  1.7-2.2x in a wide shot. Sprite light matches the fan within 3.4% from 0.67 px down to 0.125 px, which is also the
  no-popping guarantee.
- **Depth resolve over replay.** The single-sample depth replay cost +11.3 ms on OpenGL and +11.5 ms on Vulkan for a
  10M cloud (+58%), 40-45% of the static GPU time; with SSAO and EDL on, the two chains differ on under 1% of pixels
  (`DepthResolveMatchesTheForcedReplay`).
- **Separate EDL mask pass:** WebGL's GLES QRhi does not resolve a second multisample colour attachment reliably.
- **CPU float conversion, not integer zero-copy.** `pointcloud_rhi_benchmark`, streaming quads, 10M points, 20-byte
  records (before the compact formats); convert = `packPointVertices()` on the global pool, wall:

  | x/y/z | API | convert ms | bounds scan ms | decode+upload ms | convert share | peak RSS |
  |---|---|---|---|---|---|---|
  | u16 | Vulkan | 24.5 | 24.2 | 56.5 | 43% | 464 MiB |
  | u32 | Vulkan | 24.6 | 30.2 | 56.6 | 43% | 502 MiB |
  | f32 | Vulkan | 16.2 | 25.9 | 48.2 | 34% | 502 MiB |
  | u16 | OpenGL | 25.3 | 24.8 | 85.6 | 29% | 614 MiB |
  | u32 | OpenGL | 24.9 | 30.6 | 84.3 | 30% | 653 MiB |
  | f32 | OpenGL | 16.8 | 26.3 | 74.9 | 22% | 653 MiB |

  Zero-copy would save at most the conversion and need its own bounds pass (a single-threaded strided scan costs as
  much as the parallel conversion), plus a vertex layout and shader variant per wire datatype, NaN rejection in the
  shader and a full golden re-verification. Its memory gain went to the compact formats (12 bytes a point for xyz,
  16 with a scalar or colour). Re-test if conversion becomes the dominant cost of a stream.
- **Families built on demand, pipelines built as drawn.** OpenGL builds a program per pipeline object even for a
  shader seen before (Qt's program-binary cache still calls `glProgramBinary`, 1-4 ms each): building every family's
  pipelines cost 50 pipelines and 78 ms at the cloud + robot first show, building only what is drawn 13-18 and
  20.7-25 ms. Vulkan's pipeline cache makes each well under 1 ms.
- **Output sample count chosen once per QRhi:** a per-frame switch rebuilt every resource bound to the target and
  broke the missing-HDR-variant fallback.
- **CPU scene bounds:** nothing ever reported empty bounds with points; a compute reducer would cost a pipeline, a
  compute bake and `StorageBuffer` usage on every instance buffer (reasoning, not measured).
- **Wireframe as line lists** on every backend: `NonFillPolygonMode` is missing on WebGL2.
- **Render origin.** Moving a camera-relative subtraction into a float shader add is not pixel-identical for
  arbitrary doubles, even when the error is sub-ulp: marker vertices kept relative to an anchor, with anchor − origin
  added in the shaders, moved shadow edges on 14 Vulkan harness images by up to 105 px. It is exact only for
  float-valued operands, as the grid's are (grid positions and the focal are floats, so the shader's correctly
  rounded add equals the double subtraction's float). Hence markers keep CPU-relative vertices and the grid adds its
  offset in the shader.
- **Vertex-attribute sentinels need headroom.** `FLT_MAX / w` under perspective-correct interpolation overflows to
  infinity, then NaN for `w < 1`, which fails an `abs(v) >= x` test. The mesh map sends 1e30, tests above 1e29 and
  clamps data to 1e28 (`mesh_map_view.h`); the grid map drops triangles through a flat validity flag instead.
- **Update fingerprints.** An update topic's highest arrival UID at a time does not identify the update set at or
  before it once arrival and stamp order differ; a memo keyed on it alone missed updates when scrubbing. The mesh
  map also folds the latest-by-stamp entry.
- **Mesa anv buffer copies.** Mesa 26.0.3's anv (Ubuntu 26.0.3-1ubuntu1) records `vkCmdCopyBuffer` by searching the
  element count for a 2D/3D surface shape on the CPU, so a count with a large prime factor costs milliseconds per
  copy: a 1000003-point stream took 21.4 ms padded, 43.3 unpadded; a 10000019-point first upload 79.9 ms padded,
  163.2 unpadded. Reproducer: upload a static buffer of 1000003 × 16 bytes through QRhi and time
  `endOffscreenFrame()` against 1000000 × 16. Owned arrays are padded past 64 KiB to a 64 KiB multiple
  (`PJ::padForUpload()`); unpadded ones upload as two copies (`PJ::uploadStaticBufferInPieces()`); model meshes
  upload into Immutable buffers, one copy each, so an awkward size still pays once. Re-test on a newer Mesa.
- **Placement through the nearest shared frame.** Stamped objects were first composed through the root of the TF
  tree (`fixed <- root` at the cursor times `root <- source` at the stamp). Seen with that rule on an 8 s PandaSet
  sequence (Cloudini clouds at 10 Hz, fixed frame `base_link`, the car at 23-46 km/h, desktop, playback at 1x):
  a cloud still on screen while its successor decoded on the worker was placed against the car's pose at the new
  cursor, 0.63-0.77 m behind (one sweep of travel), on 158 of 372 rendered frames (software rendering under Xvfb;
  on 10 of 18 sweeps in a 30 fps screen recording on an Iris Xe). With the car's pose resampled to 50 Hz, a scan
  was seen to slide back between sweeps and jump forward at the next one (by eye, not measured). Composed through
  the nearest frame the fixed and the source frame share, the placement of a scan in a frame on the car does not
  depend on the cursor: on the same playback 0 of 386 rendered frames were displaced, 156 of which still drew the
  previous sweep's cloud while the next one decoded. Re-test if placement goes back through a frame above the
  shared one.
- **First upload Immutable:** a 10M static cloud's DRM total dropped from 684 to 428 MiB on Vulkan, which frees an
  Immutable buffer's staging and keeps a Static one's.

## ADS and window-system quirks

- **Floating turns the docked area native** (our ADS fork on Qt 6.11.1): `CFloatingDockContainer::show()` calls
  `winId()` on every platform, and Qt then marks every child of the dock manager native. Under Vulkan on Wayland that
  child surface stopped getting frame callbacks (docks froze once any dock floated, or crashed in lavapipe's
  swapchain). `Qt::AA_DontCreateNativeWidgetSiblings`, set before `QApplication`, keeps them non-native.
- **ADS reparents through a parentless state:** a float, redock and move to another area
  (`removeDockWidget()` → `setDockArea(nullptr)`) are each a top-level change for a `QRhiWidget`.
- **Our fork keeps a dock area's hidden tabs parented** (`CDockAreaLayout` in
  `3rdparty/Qt-Advanced-Docking/src/DockAreaWidget.cpp`). Upstream unparents the hidden tab on every switch, insert
  and removal, so each switch rebuilt the shown view (a 10M cloud: new pipelines and a 190 MiB re-upload, and on
  OpenGL a stall of the plot shown next while the driver freed it). Hidden tabs are explicitly hidden, move with their
  area and die with it. `rhi_dock_lifecycle_test` covers reorder, close/reopen, restore, floating the whole area,
  moving and destroying a hidden tab. Cost: hidden tabs keep GPU memory (DRM MiB, a 1M-point Scene3D tab plus K plot
  tabs visited at 1260x780, upstream → fork; RSS unchanged):

  | Plot tabs | 1 | 4 | 8 |
  |---|---|---|---|
  | RHI GL | 132 → 138 | 132 → 158 | 132 → 184 |
  | RHI VK | 366 → 302 | 366 → 366 | 366 → 366 (allocator blocks) |

  About 6.5 MiB per hidden plot on RHI GL; a hidden Scene3D tab keeps its whole scene.
- **ADS can hide floating docks on Linux QPAs other than xcb and Wayland.** Its stays-on-top emulation
  (`CDockManager::eventFilter`) sets X11 properties on xcb and is skipped on Wayland since ADS 5.1; elsewhere it calls
  `setWindowFlag(Qt::WindowStaysOnTopHint, ...)` on visible floating containers, which hides the window, and ADS does
  not show it again. `rhi_dock_lifecycle_test` and the mixed-layout fixture remove that filter; whether xcb and
  Wayland still need that is unverified.
- **Vulkan on Wayland: hiding and re-showing a window within about a frame of a present is a protocol error.** Mesa
  queues the present behind its FIFO barrier, the unmap commit waits, and Qt's immediate `get_xdg_surface` fails with
  "wl_surface already has a buffer committed", dropping the connection. Users do not reopen that fast; the dock test
  waits 100 ms before reopening.
- **An occluded window never presents.** Display tests need a real X or Wayland server; the `offscreen` QPA has no RHI
  support. On one Wayland compositor (headless GNOME Shell) parallel display runners cover each other and get no frame
  callbacks: `grabPresentedFrame()` times out, `rhi_mixed_dock_test` failed in about 3 of 4 parallel runs and in none
  with one compositor per process or through xcb. Every CI lane runs label `qrhi` serially.
- **On the Wayland QPA every frame waits for the compositor's 60 Hz callback**, hiding everything under 16.7 ms:
  benchmarks run on xcb with vsync off.

## Limits and budgets

`core/scene_limits.h` holds one `SceneLimits` per platform (`kDesktopSceneLimits`, `kBrowserSceneLimits`),
statically checked against the renderer's `quint32` sizes and `int` counters. The largest single buffers at the
desktop limits: 100 M points × 16 B = 1.6 GB, 20 M marker vertices × 28 B = 560 MB.

The HDR chain costs 4 × (RGBA16F 8 + depth) + RGBA16F 8 + RGBA8 4 + depth per render pixel: 64 B with D32F, 84 B with
OpenGL's D32FS8 resolve. 3840x2160 is 506 MiB (664 MiB on OpenGL), so the desktop's 1 GiB per view fits one 5K view
(one 4K on OpenGL) and 4 GiB per QRhi eight 4K docks (six on OpenGL).

## Golden images

`pointcloud_golden_test` compares point-cloud scenes and scene-corpus images (grid, markers, robot) against goldens
written by QRhi OpenGL on llvmpipe. Corpus and full rule:
[`widgets/tests/fixtures/golden/pointcloud/README.md`](../widgets/tests/fixtures/golden/pointcloud/README.md).

- A pixel differs when off by more than 16 in a channel; a scene fails above 3% differing pixels, a 5% coverage
  change or a mean-colour change above 12. A pixel counts only when every golden pixel in its 3x3 neighbourhood is
  off (the MSAA orientation, "Backend conventions"), and a scene also fails above max(3 × 3%, 10%) plain per-pixel
  differences, so the allowance cannot hide a whole-scene shift. Plus a foreground diff (limit 0.12) and a stray
  count (pixels farther than 2 px from the golden's foreground, limit 4).
- Driver-dependent scenes drop the per-pixel checks for coverage and mean colour (`field_turbo_spheres`,
  `large_1m_points`, the line grids) or coverage alone (`shape_spheres_small`, whose ~1.5 px discs rasterize at a
  different size per Mesa version). Both modes keep a coarse spatial check (per-tile coverage and colour, centroid,
  bounding box, strays outside a margin), because pixel count and mean colour survive a moved foreground.
  `GoldenComparator.CoverageModesRejectMovedForeground` pins it.

**The writer.** `PJ_UPDATE_POINTCLOUD_GOLDENS=1` writes from one pinned stack, QRhi OpenGL on Mesa llvmpipe, and
fails without writing on any other; it records its device string in `writer.txt`, and on that stack the comparison
is exact. Two runs are byte-identical. `PJ_POINTCLOUD_GOLDEN_DIR` redirects reads and writes, so a regeneration is
reviewed in a scratch copy before replacing the committed images.

Measured worst cases (Mesa 26 locally; the rows marked "previous images" were measured against the corpus before its
last regeneration and are re-taken from the next D3D11 and Metal CI run's output):

| Backend | Allowance | Measured worst |
|---|---|---|
| OpenGL llvmpipe, the writer's stack | none | exact |
| OpenGL llvmpipe, CI's Mesa 23 | the tolerances above | foreground diff 0.028 (previous images) |
| OpenGL Iris | none | foreground diff 0.001; `shape_spheres_small` per-pixel 0.013, coverage 0.997 |
| Vulkan lavapipe, Iris | none | foreground diff 0.082 (`field_viridis_cubes`), per-pixel 0.075 (`shape_cubes_subpixel`); line grids 15% of their foreground |
| D3D11 WARP | line scenes: coverage 0.12, foreground diff 0.2 | neighbourhood 0.019, per-pixel 0.074, point-cloud coverage within 0.6%; line grids 1.19x (previous images) |
| Metal (paravirtual) | none | the D3D11 bands on point clouds; line grids 0.94x (previous images) |
| All, line grids | coverage 0.08 (0.25 on WARP) | Iris GL 1.002, lavapipe 0.989, Iris Vulkan 0.990 of the golden's pixels |
| Apple software GL | not run | sphere imposters ~10% larger, soft sprites: coverage up to 19%, mean colour up to 58 |

Other measured tolerances: `scene3d_rhi_model_cache_test`'s cached-vs-uncached A/B is pixel-identical on Mesa and
Metal; the perf-HUD image differs between runs in one 7x9 px timing digit.

## Apple software OpenGL

The macOS OpenGL step runs on Apple's software renderer, a functional smoke run rather than a parity gate (Metal is).
Observed there, and excluded from that step only (Mesa and Metal run them):

- `scene3d_rhi_model_cache_test`'s cached and uncached frames differ on 10 pixels from step 6 (shadows off) on, and
  once on 865 pixels at step 0, whose state the cached run's step 1 reuses: the renderer, not the caches. The test
  skips the A/B on that renderer.
- `ShadowToggleRebindsKeptMeshes`'s bit-identical check before and after a shadow toggle failed 3 of 5 CI runs
  (frames differ with the same inputs).
- 1 px sprites resolve to ~15% coverage (`SubPixelCubeCloudBecomesColouredSprites`), and a 1 px analytic grid line at
  an odd cell edge loses the same coverage (`FilledGridLinesFollowOddCellEdges`).
- The golden corpus (see "Golden images") and the DPR 2 mixed-dock pass (every float recompiles the pipelines at 4x
  the pixels) are left out as measuring the renderer, not ours.

Re-test when the runner image's renderer changes.

## Software rasterizers draw the direct pass

A browser without a GPU (headless Chromium, and any machine where WebGL falls back to SwiftShader) held its main
thread for seconds on the first Scene3D frame. Chromium traces of the ANYmal sample on GitHub-hosted `ubuntu-22.04`
runners showed the long task as `LayerTreeHost::DoUpdateLayers` → `ReadPixels` → `CommandBufferHelper::Finish`: the
compositor reads the WebGL canvas back and waits for SwiftShader (`ContextVk::finishImpl` in the GPU process) to
draw the queued frame. The workspace restore itself is a separate, shorter task. The variance between runs followed
CPU contention with the app's worker threads on the runner's 4 vCPUs.

The HDR chain (4x RGBA16F MSAA target, depth replay, SSAO, EDL, present) is what makes that frame expensive on a CPU.
`scene3d_frame_benchmark` on llvmpipe (Iris Xe host, Mesa 26, Xvfb, smoke sizes, 3 runs each):

| Case | direct pass, 4x output MSAA | HDR chain (`PJ_SCENE3D_SOFTWARE_HDR=1`) |
|---|---|---|
| `cloud_static` frame | 12.3-16.9 ms | 22.7-28.4 ms |
| `cubes_lines_orbit` frame | 14.8-17.0 ms | 24.9-27.5 ms |
| `first_show`, budget sizes | 2351-2657 ms | 2622-3295 ms |

An earlier run at budget sizes timed the first frame alone at 497 ms with the direct pass (1x) and 870 ms with the
chain. The direct pass's own 4x output MSAA, measured the same way (mean frame, 3 runs each):

| Case | 1x | 4x |
|---|---|---|
| `cloud_static` | 7.1-7.4 ms | 9.2-10.1 ms |
| `cubes_lines_orbit` | 6.5-7.5 ms | 11.9-14.3 ms |
| `markers_pan_follow` | 4.3-4.8 ms | 17.0-21.2 ms |
| `robot_tf_animated` | 3.1-3.3 ms | 10.1-15.1 ms |

So `hdrUnsupportedReason()` refuses the chain on a software rasterizer, the output stays single-sample, and the view
shows a notice. Shadows stay on.

Re-test when SwiftShader, llvmpipe or a browser's GPU-less compositing path changes, or when the chain gets cheaper;
`PJ_SCENE3D_SOFTWARE_HDR=1` restores the chain for the comparison.

On a desktop with llvmpipe (Xvfb, Mesa 26, Qt 6.12.0) the direct pass then drew the TF and pose triads in the
backdrop colour: the annotation blend cleared the output's alpha and the composited window showed through (in the
MCP screenshot, which paints each view's `grabFramebuffer()` image over the window). The display tests missed it:
`grabPresentedFrame()` delivered `Format_RGB32` there, while `grabFramebuffer()` called directly returned
`Format_RGBA8888` with the alpha. So the direct pipelines write no alpha (ARCHITECTURE "Pipeline registry"), and an
alpha check reads `grabFramebuffer()` (`DirectPassTriadsLeaveTheOutputOpaque`). Re-test the grab formats on a Qt
upgrade.

## CI lanes

The workflows are the source of truth; these are the facts that are not obvious from them.

- **Linux** (`linux-ci.yml`): every `.opengl`, `.opengl_dpr2` and `.vulkan` pass under Xvfb (lavapipe through
  `VK_ICD_FILENAMES`) with `PJ_REQUIRE_GPU_TESTS=1`; an OpenGL floor step with Mesa capped at GL 3.3; a mixed-DPR
  step for the two `*AnotherPixelRatio` cases on a headless mutter's Xwayland with two virtual monitors.
- **Mixed device-pixel ratios need two named outputs.** Xvfb cannot provide them: every RandR output is named
  `screen`, so `QT_SCREEN_SCALE_FACTORS` gives both the last factor; `xrandr --setmonitor` adds no monitor; two X
  screens make `QWindow::setScreen()` hide and re-create the window. A headless mutter or GNOME Shell with two virtual
  monitors gives its Xwayland one output per monitor, and xcb moves the window live. The cases skip without such
  screens, fail with `PJ_REQUIRE_MIXED_DPR=1`, and skip on Wayland (the compositor places windows). Locally:

  ```bash
  XDG_RUNTIME_DIR=<dir> dbus-run-session -- gnome-shell --headless --wayland \
    --virtual-monitor 1024x768 --virtual-monitor 1024x768 --wayland-display <name> &
  # DISPLAY from "Using public X11 display :N" in its log; XAUTHORITY: <dir>/.mutter-Xwaylandauth.*
  QT_QPA_PLATFORM=xcb QT_SCREEN_SCALE_FACTORS="1;2" PJ_REQUIRE_MIXED_DPR=1 \
    ctest --test-dir build -R 'AnotherPixelRatio'
  ```

- **Windows** (`windows-ci.yml`): `-L qrhi-d3d11` with `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`;
  `scripts/ci/check_qrhi_backend.sh` fails unless every view and plot canvas logs D3D11 on a `Cpu` adapter. WARP
  rasterizes 1 px lines differently from Mesa (three wide dashed plot lines end one dash later than Qwt's raster); the
  goldens and the dash test carry per-backend allowances for exactly these cases.
- **macOS** (`macos-ci.yml`, `macos-15`): a Metal device and presentation probe first (fails in seconds without
  them), then `-L qrhi-metal`, and on scheduled and manual runs `-L qrhi-opengl` (up to 24 min on the software
  renderer). It is the only macOS test gate; a real Mac's OpenGL fallback is unverified.
- **Pixi** (`pixi-ci.yml`) runs no GUI tests: conda Qt stalls without a GPU, so the conda channel's rendering is
  untested; win-64 runs the suite with the display passes skipped.
- **Sanitizers** run offscreen, so every display case skips. Before merging a change to QRhi resource lifetimes or
  failure handling, run the `.opengl` and `.vulkan` passes of `scene3d_rhi_failure_test`, the marker and model cache
  tests, `rhi_dock_lifecycle_test`, `rhi_mixed_dock_test` and `plot_rhi_render_test` under
  `./test.sh --sanitize asan --display` inside `dbus-run-session`: without a session bus Qt's D-Bus client on xcb
  leaks a libdbus buffer and LeakSanitizer fails every case (154 cases, 89 s on a 20-core machine; command in
  [docs/SANITIZERS.md](../../docs/SANITIZERS.md#asan--ubsan)). `tsan.supp` carries audited entries for
  `LatestWinsWorker`'s Qt hand-offs.
- **Release:** the AppImage smoke runs after its upload; a `workflow_dispatch` of the release workflows on a branch
  is its only pre-merge gate (`packaging/README.md`).

## Performance baseline

Gate A judges a candidate's RHI GL (Linux's native backend) against a baseline: time within max(1.2x, +1 ms),
memory within 1.10x, a row's own limit where it has one, idle frames 0, and "regressed" when over at least 3 rounds
every candidate round is worse than every baseline round and the median by more than 2%
(`scripts/perf/gate_a_report.py` defines the rules). RHI VK is for reference. Re-run it for any change to the
renderer's frame path, upload policy or lifecycle.

**Conditions of the baseline below.** Intel Iris Xe (RPL-P), Mesa 26.0.3, i915, AC power, `performance` governor,
P-cores capped at 2.4 GHz; xcb on a private rootful Xwayland in a headless GNOME Shell, vsync off (`vblank_mode=0`,
`MESA_VK_WSI_PRESENT_MODE=immediate`); 5 interleaved rounds, one process per case and configuration, each the median
of 30 frames after warm-up; median [min–max] where the spread exceeds 10%. Run of 2026-09-28, load 1.3-3.3; the
cloud, stream, dock and first-show rows from a second run that day (load 1.3-2.9), the marker rows from a third
(3 rounds, load 4.3-8.5). No RTX 4070 column: the host kernel had no NVIDIA module; add one when it does.

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
| ADS tab switch, 9.9M cloud: to the cloud / to the plot, ms ⁷ | 4.5 / 5.2 [4.6–5.2] | 4.9 [4.3–5.2] / 13.0 |
| First show, 10M cloud + robot, ms ⁶ | 187.6 | 316.0 |
| Idle frames when static, every case | 0 | 0 |
| Plots: dashed 5 x 100k, wall ms ⁸ | 1.6 [1.5–1.7] | 1.8 [1.6–2.0] |
| Plots: 50 x 100k, wall ms ⁸ | 11.2 | 11.4 |
| Plots: tracker only, 10 x 100k, wall ms | 2.6 | 6.2 [5.5–7.4] |
| Plot GPU memory, 1600x900, DPR 1 / 2 (DRM MiB) | 45.0 / 183.2 | 106.1 / 315.8 |
| Streaming plot, 10 x 100k, ≤ 16.7 ms | 10.9 [10.7–12.9] | 19.4 |
| Mixed: 6 plots + Scene3D, wall / CPU ms | 37.1 / 8.8 | 49.0 / 9.0 [8.8–10.0] |
| Mixed: RSS MiB | 258.4 | 302.7 |

¹ The 10M cloud settles on one 114 MiB block: the ObjectStore's wire bytes, which the decode reads in place. RHI GL
drops its 12-byte vertex array (114.4 MiB) once the upload is enqueued; a stream keeps its array. Peak RSS during the
load is 362.7 MiB (not gated): the array lives until the upload's frame is submitted. A float, redock or area move
converts the sample again for the new QRhi: 10.4 ms at 10M into a fresh array in up to 16 chunks (18.5 ms with 8,
first-touch page faults dominating). A compressed cloud whose cache holds another sample decodes again off the UI
thread: Cloudini took 476 ms at 10M points (lossy 1 mm, ZSTD; 610 ms lossless).

² Each 300k sample converts into 12-byte records in chunks of at least 256k points on the global pool, the GUI
thread taking chunks too, into the reused array: 0 minor faults per frame. The pool adds CPU the GUI thread does not
see (about 1 ms per frame at 300k; at 10M, 8 chunks cut 70 ms of wall for 19 ms more process CPU); if total CPU
becomes the budget, convert serially below 1M points. Ingestion is in neither wall nor CPU.

³ The shadow map is kept while its inputs hold: an orbit of a static scene renders it once (27.9 → 23.5 ms). The
frame is GPU-bound in the HDR chain (4x RGBA16F with resolve and present): 7.0 ms without it, SSAO and EDL under
0.3 ms each. Lowering the chain (fewer samples, a smaller format) is a quality trade-off, not an exact optimization.

⁴ GPU-bound, the HDR chain about 1.4 ms. Markers cast no shadows, so a pan renders no shadow map; the GUI thread's
work is the origin-only update (about 0.55 ms), the upload and `endFrame`. With long strips (1.1M points in 110
strips, OpenGL, GUI CPU per frame): a pan 18.8 ms, a full rebuild 18.8 ms, a live 100k-point layer 9.6 ms; with
per-point colours 18.9, 17.2 and 15.0 ms. What remains of a pan is the vertex write (2.2M vertices, 62 MB) and the
driver's copy of it in `endFrame` (21% of the frame).

⁵ i915 settles each configuration at its own clock once warm: 1500 MHz for RHI GL on this row, 600 MHz on Vulkan
(every light row), so light rows compare clock policies as much as backends; why the boost differs is open.
`cold_ms`, the first frame after a second of idle, is 1.0 ms on RHI GL and 5.1 on Vulkan.

⁶ A new QRhi builds only the families the scene uses and the pipelines it draws: 13 pipelines and 20.7 ms of creation
at the first show. Robot float on RHI GL, 62.6 ms: new top-level and QRhi 12.2, HDR targets 12.2, prepare 6.3, submit
7.4, the first frame's GPU work and swap 25.4. 10M cloud float, 131.3 ms: 72.2 in the present (the 114 MiB
re-upload) and 30.8 in prepare (allocation and the 10 ms conversion of the evicted cloud). On Vulkan a pipeline costs
under 1 ms but the re-upload waits in the offscreen frame (submit 57.9, present 107.3 ms).

⁷ Hidden tabs stay parented ("ADS and window-system quirks"), so a switch adopts no QRhi, builds and uploads nothing,
and a Show whose target holds the last frame renders nothing: the same cost at 10.6k and 9.9M points. The Vulkan
plot tab's 13 ms is the plot canvas's own synchronous offscreen frame.

⁸ The plot rows replot unchanged `RevisionedSeries` data, which keeps the canvas's curve layer. With plain Qwt data
(`plot_canvas_benchmark --generic-series`, 3 rounds, wall / canvas CPU ms) every replot rebuilds: 50 x 100k static
85.2 / 81.9 on RHI GL and 107.1 / 90.9 on RHI VK (the 16 ms over its CPU is the fence wait of "Qt workarounds"),
5 x 100k dashed 21.8 / 21.1 and 26.5 / 22.3, 1M static 13.8 / 13.4 and 17.2 / 15.1.

Also measured: Vulkan's backend frees an upload's staging only frames later, so a settled view keeps reporting it in
the DRM total, and the resolve chain's multisample depth texture reserves one more allocator block. The software
rasterizers run the frame on the GUI thread for OpenGL and on workers for Vulkan, so only wall time compares between
them. Large sprites as quads: a 1M 0.1 m sphere cloud took 34.9 ms against 29.6 ms with the retired GL renderer's
`gl_PointSize` sprites on OpenGL.

### Harness

`scripts/perf/gate_a.sh --candidate <tree> [--baseline <tree> | --baseline-run <dir>]` runs every case with the
method above and builds nothing: it starts its own headless GNOME Shell and rootful Xwayland, runs each case in its
own process with the configurations interleaved per round (`rhi_gl`, `rhi_vk`, and `base` for a baseline tree),
keeps every run under `<out>/raw/<round>/`, and ends with `gate_a_report.py`, which writes `<out>/gate_a.md` with
each row's verdict: pass, fail, or incomplete when a gated measurement is missing or a run failed. `--baseline-run`
compares against an earlier run's `rhi_gl` (a frozen baseline); an interleaved baseline tree is less noisy. Each tree
builds `scene3d_frame_benchmark`, `plot_canvas_benchmark` and `mixed_layout_benchmark` (`PJ_BUILD_DEMOS=ON`).

- Run it under the machine's build lock (`scripts/perf/build_slot.sh`): no compile may overlap a round.
  `--first-round K --rounds 1` runs one round at a time into the same `--out`, so builds can run between rounds. A
  round whose load average stands out is rerun, not averaged in.
- Start the display outside the lock and pass `--keep-display`: a compositor started under a plain `flock` inherits
  the lock and holds it after the run.
- Every per-frame benchmark warms up with `pj_bench::WarmUp` (at least 250 ms, the last 5 frames within 10% and the
  i915 clock unchanged for 100 ms, capped at 120 frames or 2 s) and reports the first frame after a second of idle as
  `cold_ms`. `gpu_mhz` and `gpu_mhz_range` record the clock.
- `scene3d_frame_benchmark` reports per-frame wall p90/max, GUI CPU p50/p90, minor faults, upload work, and with
  `PJ_RHI_TEST_HOOKS` a stage split (`stages_ms`; its header defines the stages). Per-frame ingestion
  (`CloudStream`) happens before the timed request and is reported as `ingest_ms`. `PJ_S3D_BENCH_CSV_DIR` writes
  every frame; `window_ns` is the measured span in `CLOCK_MONOTONIC`, to cut a `perf record -k CLOCK_MONOTONIC`
  profile. Attribution switches, not for the gate: `QSG_RHI_PROFILE=1` (`gpu_ms`, slows the frame),
  `PJ_S3D_BENCH_NO_HDR`, `_NO_SHADOWS`, `_NO_SSAO`, `_NO_EDL`. `PJ_S3D_BENCH_FULL=1` selects the Gate A sizes; by
  default it is a smoke that asserts each case's workload, not timing.
- `mixed_layout_benchmark --tab-switch N` times a real ADS tab switch between a Scene3D dock and plot docks to the
  shown widget's first frame, with adoptions, builds, uploads, paints and renders per switch (`--plots K`).
