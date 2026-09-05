# pj_scene3D — Architecture (as built)

How the 3D scene renders and how robot models get on screen. This is the
distilled, as-built successor to the executed planning docs
(`PHOTOREALISM_AND_URDF_MESH_PLAN.md`, `URDF_MESH_ASSET_RESOLUTION_DESIGN.md`,
`CODEX_PROMPTS_URDF_MESH.md` — all removed 2026-06-10; see git history for the
full design rationale). The WHAT lives in [REQUIREMENTS.md](./REQUIREMENTS.md).

## Rendering pipeline

### Native OpenGL path

Per frame, the native `SceneViewWidget::paintGL`:

```
layers + passes → SceneHdrFbo (multisample RGBA16F + DEPTH32F + R8 is-mesh mask)
                → resolve blit (single-sample color + depth + mask)
                → SsaoPass (R16F AO, box-blurred)   [reads resolved depth]
                → EdlPass  (R16F shade factor)      [reads resolved depth + mask]
                → composite/present (fullscreen triangle → backing FBO)
```

- **HDR chain (0A).** The render FBO is created at a fixed MSAA sample count
  (`kDefaultMsaaSamples`, clamped to `GL_MAX_*_TEXTURE_SAMPLES`) — INDEPENDENT of
  the QOpenGLWidget's negotiated `context()->format().samples()`, which is 0 once
  the view is composited inside an ADS dock (the backing store is single-sample).
  Because SceneHdrFbo owns its own multisample textures and the present is a
  fullscreen draw, the single-sample backing FBO never undoes the already-resolved
  anti-aliasing — so the scene is 4x MSAA docked, not just in the demo. (Seeding
  the chain from the context's samples was the bug that made MSAA silently vanish
  in the app.) MSAA→MSAA blits with mismatched formats are illegal, so the resolve
  target is always single-sample; the present pass draws into
  `defaultFramebufferObject()` (never FBO 0 — QOpenGLWidget renders off-screen).
  When the chain is unavailable the widget falls back to direct-to-backing
  rendering (one warning per context).
- **Linear light + tonemap (0B).** All color inputs are linearized (colormaps,
  occupancy LUT, vertex/base colors, theme colors; mesh diffuse textures upload
  as `GL_SRGB8_ALPHA8`). The composite applies exposure → tonemap
  (None/ACES/AgX/Neutral; AgX matrices are **column-major** GLSL `mat3` ctors;
  Neutral is the Khronos PBR Neutral mapper, which preserves colormap hue better
  than ACES) → saturation → manual sRGB encode.
- **Annotation alpha-marker.** The scene FBO's alpha is a per-pixel
  tonemap-bypass marker, not coverage: annotation passes (TF triads via
  `ArrowGizmo`, HUD overlay) write alpha 0 so they present flat and vivid,
  while data draws restore the marker with coverage-union blending
  `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` on the alpha channel (preserving
  destination alpha instead would ghost occluded annotations through opaque
  meshes). Gizmo opacity rides the gizmo color's alpha through
  `glBlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA)`.
- **Background bypass.** Far-plane pixels (depth == 1) keep the exact theme
  color, ungraded (no tonemap/saturation) — the background never shifts with the
  tonemap.
- **Is-mesh mask (EDL gating).** EDL is restricted to **mesh surfaces** — point
  clouds, grid, occupancy, axes and the empty background get no eye-dome contour.
  The scene FBO carries a second color attachment (`SceneHdrFbo::kMaskAttachment`,
  R8), cleared to 0 each frame; only `MeshRenderPass` enables that draw buffer (its
  fragment shader writes 1.0 to `location = 1`) — driven by `ViewParams::write_mesh_mask`,
  set on the off-screen + EDL-on path. The mask is resolved alongside color/depth
  and sampled by EDL. There is no separate object-class G-buffer: the alpha channel
  is the tonemap marker and is identical (1) for meshes and point clouds, so the
  mask is the only per-pixel mesh signal.
- **SSAO** reconstructs view position via `u_inv_proj` (works under the
  ortho camera; the perspective near/far formula does not) with an in-shader
  4×4-tiled hash as the rotation noise; it still applies scene-wide. **EDL** is
  Potree-derived (8 circular neighbours, log-depth response) but **mesh-only**: a
  mesh pixel is darkened when a neighbour is FARTHER — or is non-mesh, which the
  shader reads as "far" (`FAR_SENTINEL`) via the mask. So the mesh's silhouette
  against the void, point clouds, or farther meshes, plus the near side of its
  creases, get the contour; point clouds neither receive it nor cast it. Each
  per-neighbour log-depth gap is clamped to `look::kEdlMaxGap` so the huge
  silhouette gap reads as a graded outline rather than a solid black band; creases
  (much smaller gaps) are unaffected. With no mask bound EDL falls back to
  unrestricted (every pixel treated as mesh). Both passes multiply into the
  composite and degrade to no-ops when unavailable (`u_has_ao` / `u_has_edl`); EDL's
  darkening is floored (`CompositeParams::edl_floor`) so it bottoms out at a
  hue-preserving dark grey, `floor·color`, rather than pure black (floor 0 = the
  original multiply-to-black).
- **Defaults** (look-dev, 2026-06-13). The authoritative numbers live in
  [`scene_look_defaults.h`](../widgets/include/pj_scene3d_widgets/scene_look_defaults.h)
  — treat that header as the source of truth and this paragraph as a summary
  (it has drifted before). As of this writing: ACES, exposure 1.3, saturation
  1.3, SSAO on (strength 1, radius 0.4 m), EDL on (strength 1, radius 0.6 px,
  floor 0.3, max gap 0.02).
  Runtime knobs: `SceneViewWidget::compositeParams()`, `ssaoPass()`,
  `edlPass()`, and the per-view `meshShadingParams()` (roughness 0.6, f0 0.06,
  ambient 0.5, key/"sun" 1.6, fill 0.5, env-reflection 1.0, key-light dir
  azimuth 40° / elevation 55° ≈ high +X+Y — a stopgap for a future per-scene
  lighting object). Shader provenance/licenses: [`../THIRDPARTY.md`](../THIRDPARTY.md).
- **Shadows (always on).** The key-light term is occluded by a real-time shadow
  map — a geometry **depth pre-pass** rendered from the light *before* `renderScene`
  (structurally unlike the screen-space SSAO/EDL **post-passes**), with the shadow
  factor multiplied into the key-light term only (the first summand of `direct` in
  `mesh_render_pass.cpp`); the camera-locked fill and IBL ambient stay unoccluded.
  The app renders shadows unconditionally (no user toggle). See
  [Mesh shadows](#mesh-shadows) for the full pipeline.

**GL context lifecycle (don't regress).** `QOpenGLWidget` recreates its context
on every ADS reparent. Every pass, layer, the HDR chain, and the present
program implement `releaseGL()` (wired to the dying context's
`aboutToBeDestroyed`) and rebuild lazily — VAOs/FBOs/textures are per-context,
never shared. The app deliberately does NOT set `AA_ShareOpenGLContexts`.

**TF frame hover labels.** Hovering a TF axis triad shows the frame's name in a
small HUD box and draws that triad brighter. The pick is pure screen-space and
lives in `SceneViewWidget`: `paintGL` caches `proj*view`, and a button-free
`mouseMoveEvent` projects every resolvable frame origin through the GL-free,
unit-tested `core/tf/frame_picking.h` (`projectFrameOrigin` + `pickNearestFrame`)
and takes the one closest to the cursor within ~20 logical px (first on an exact
tie). The label is drawn at the tail of `paintGL` as a 2D overlay — the same path
as the perf HUD — re-projecting the live origin so it stays glued to the frame as
the scene streams. Its text and panel are **CPU-rasterized into a `QImage` and
blitted with `drawImage`** (`hud_overlay.h::renderHudPanel`), *not* painted as
`QPainter` text on the GL widget: the latter relies on Qt's per-GL-context glyph
atlas, which does not survive the context recreation ADS triggers on dock reparent
/ **layout restore**, leaving glyphs doubled/garbled while vector fills stayed
correct (the original #214 bug). `drawImage` is a plain textured quad — no glyph
atlas, immune to recreation and DPR changes. The highlight is the one place this touches
the draw pass: `paintGL` pushes the hovered frame to `AxisRenderPass::setHighlightedFrame`,
which luminance-boosts that frame's triad colors. Gated on the triads being
visible; cleared on a camera gesture and on leave. Occlusion is ignored for now
(a frame hidden behind geometry still labels); a one-texel depth-reject is the
planned refinement.

## WebAssembly product backend (W13-W19m)

The browser selects a different implementation behind the same
`SceneViewWidget` and `Scene3DDockWidget` public product names. This is a
compile-time `PJ_TARGET_WASM` branch, not a preference: desktop continues to use
the native OpenGL pipeline above and full layer graph unchanged.

The browser view is a `QRhiWidget` configured for Qt's OpenGL/WebGL2 backend
and 4x renderbuffer MSAA. It owns only QRhi buffers, bindings, and pipelines.
`scene_view_widget_rhi.cpp` owns scene preparation and command submission;
`scene_view_widget_rhi_quality.cpp` owns shadow and HDR/SSAO/EDL resource
creation, fallback, and teardown. Their small private header contains only the
shared uniform layouts and constants, keeping the shader ABI explicit without
exposing a second public renderer API.
Vertex positions are made relative to the camera focal point before conversion
to float, preserving the existing camera's large-world precision model. The
current submissions are:

- canonical line or checkerboard grid geometry;
- RGB axes transformed by every frame resolved against the fixed frame at the
  tracker time; and
- magenta parent/child segments from the shared `buildTfConnectionSegments`;
- raw canonical point clouds, plus Draco/Cloudini compressed clouds transcoded
  to the same canonical representation, as point/sphere sprites or instanced
  cubes with the native color/range/size controls; and
- `PosesInFrame` as instanced solid arrow triads using the shared expansion and
  unit-arrow mesh;
- occupancy grids as one R8 texture plus a shared unit quad, including
  incremental dirty-rectangle uploads; and
- dense voxel grids as one R32F or RGBA8 3D texture plus a shared instanced
  cube, with the native field/predicate/range/color semantics;
- bounded procedural SceneEntities, embedded/remote ModelPrimitives, and
  RobotModel/URDF visuals and collisions; and
- visual-mesh shadow casting plus model/checkerboard receiving through a lazy
  2048-square sampled D32F map.

The normal WebGL2 path renders linear light into a 4x RGBA16F color
renderbuffer with D32F depth, resolves color, and composites with the native
default exposure, ACES, saturation, annotation marker, and manual sRGB encode.
Qt's GLES backend did not produce usable depth or second-color MSAA resolves in
real Chrome despite accepting the targets. The browser therefore replays the
already-bounded retained draws once into single-sample RGBA8 coverage plus a
real sampled D32F depth texture. This 64-byte/pixel/view chain is retained per
size, aggregate-budgeted across docks, and released on RHI teardown.

Browser EDL consumes that replay without another retained attachment. The
full-scene replay writes alpha only, leaving RGBA8 red at its clear value. A
second preserving render target aliases the same RGBA8 + D32F textures and
replays only prepared model ranges with red-only writes, blending disabled,
`LessOrEqual` depth testing, and no depth writes. Non-mesh depth therefore
occludes hidden meshes while point clouds, grids, occupancy, voxels, non-model
marker primitives, poses, axes, and background never enter the mask. The present shader reconstructs
log view depth with the inverse QRhi-corrected projection and evaluates native's
eight circular neighbours, strength, 0.6-device-pixel radius, maximum gap, and
darkening floor before exposure/ACES. The target's pixel size and shader
`textureSize` are physical framebuffer pixels, matching native at its default
render scale on high-DPI displays. Mask/depth replay is single-sample while
native resolves 4x buffers with `GL_NEAREST`; smooth interiors and resolved
pixel-center depth follow the same equation, but silhouette membership may
differ by at most the one-pixel sample boundary. EDL construction failure is
cached and warns independently while HDR remains active. The existing memory
budget stays exactly 64 B/px because mask red reuses coverage RGBA8. Unsupported
HDR allocation combinations still warn and use the behavior-preserving direct
target path.

Browser SSAO also consumes the replay without another retained image. A
color-only preserving target aliases replay RGBA8 while sampling its unattached
D32F depth and writes only the otherwise-unused green channel, with blending
disabled. The shader mirrors native `SsaoPass`: inverse-projection view-position
reconstruction, closer 5-tap depth normal, the deterministic 32-sample
hemisphere kernel seeded with `0x55A0`, 4x4 tiled hash rotation, 0.4 m radius,
0.025 bias, and power 1. The present shader performs native's 16-tap `[-2,1]²`
box blur from green and applies AO before EDL and ACES. Alpha coverage and the
red EDL mask are protected by the green-only pipeline state.

Native retains raw and blurred AO as R16F. The browser stores raw AO in one R8
channel and performs the blur while presenting, so it retains zero additional
bytes and its per-sample quantization error is bounded by `0.5/255 = 0.001961`;
the normalized box average cannot increase that bound. Replay depth has the
same separately documented single-sample versus native 4x `GL_NEAREST` boundary
at geometry discontinuities. SSAO allocation, rejected-size caching, telemetry,
warning, and recovery are independent of HDR and EDL; warning priority is HDR,
then SSAO, then EDL.

The twenty-four shader sources and committed `.qsb` packs contain GLSL ES 300.
CMake locates the matching host Qt `qsb`, rebuilds every pack during configure,
and compares SHA-256 before compiling. A stale pack therefore fails closed
instead of silently shipping a different shader.

Camera models, fixed/follow behavior, `TransformBuffer`, `TransformService`, TF
hierarchy/connection logic, tracker-time repaint keys, and scene-control values
are shared with the native product. The browser dock accepts TF config topics,
raw/compressed point clouds, depth clouds, pose arrays, occupancy/voxel grids,
procedural SceneEntities, models, and RobotModel sources. The browser source
lists deliberately omit the native GL pass implementations while providing
equivalent QRhi/WebGL2 post processing. Draco 1.5.7, Cloudini 1.2.2, and bounded Assimp enter only the
explicitly gated browser graph; the non-Emscripten source graph and runtime
behavior are unchanged.

Compressed decode never blocks or joins the browser UI thread. Each compressed
layer has one `QtConcurrent` job in flight and at most one pending request;
scrubbing replaces that pending request with the newest canonical sample.
Generation tokens invalidate results across hide, detach, and dataset replace,
while the worker captures only owned canonical bytes and therefore may drain
safely after the layer disappears. One successful decoded sample is cached.
One failed sample is memoized with its warning: revisiting it restores empty
geometry and the same warning without retrying the codec, while moving to a
different sample can recover normally; a later distinct failure may replace
that one-entry memo. Cloudini dimensions/bytes and Draco point count plus packed
attribute layout are bounded before their principal value/output allocations.
The decoded cloud then follows the exact raw conversion, finite-position
compaction, per-layer, and per-view budget path. Decoded caches remain per layer,
not a global RSS quota; the W19 product-envelope audit owns aggregate memory
measurement and policy.

Persistence uses the native `<scene3d version="1">` field names for fixed and
follow frames, camera model/pose, TF config topics, and shared scene controls.
Browser restore prevalidates all control/camera fields before mutation and rolls
back the complete previous dock state if topic resolution later proves invalid.

## Mesh shadows

Real-time directional shadows for **meshes only**, from the existing fixed key/"sun"
light (`MeshShadingParams::key_light_dir`). **Casters: meshes only** — URDF/robot
meshes (`RobotModelLayer` visual links) and scene-entity `ModelPrimitive` meshes
(`SceneEntitiesLayer`); point clouds, voxel/occupancy grids, axes, TF triads,
markers, and collision hulls never cast. **Receivers: meshes and the solid grid
floor** (`GridRenderPass` filled-cell mode). Always on in the app:
`MeshShadingParams::shadows_enabled` defaults true and the app renders shadows
unconditionally (no user toggle; the `mesh_viewer` demo overrides it via `--shadows`).

Unlike the screen-space SSAO/EDL post-passes, a shadow map is a **geometry depth
pre-pass** that runs in `paintGL` *before* `renderScene` (between the `FrameContext`
build and the scene render):

```
fit light frustum to mesh-caster bounds  ──►  ShadowMapPass.begin() (depth FBO)
  ──►  each mesh layer renderShadowCasters() (depth-only, from the light)
  ──►  end + rebind scene FBO  ──►  renderScene (receivers PCF-sample the map)
```

- **GL-free core** (`core/shadow_camera.h`, headless-tested). `fitDirectionalShadowCamera`
  builds the orthographic world→light-clip matrix: bounding-**sphere** extents
  (rotation-invariant, no resolution pulsing), padded, and **texel-snapped** along a
  light-only basis so the shadow edge does not crawl as the scene jitters sub-texel.
  `extendAabbToGroundShadow` grows the caster AABB to enclose where the shadow lands
  on `z=0`, so the frustum also covers the **receiving floor** (the floor never casts,
  so it is otherwise absent from caster bounds — and the shadow would fall outside a
  caster-only frustum). `kShadowMapSize` (2048) sizes the map independently of
  `render_scale`.
- **Caster bounds, distinct from `worldBounds()`.** `Scene3DLayer::meshShadowBounds`
  is a separate hook from `worldBounds()` on purpose: `RobotModelLayer` stays
  bounds-less for the **camera** (a moving robot must not yank the view), but the
  shadow frustum *must* enclose the robot mesh. `meshShadowBounds` reports the visual
  caster AABB via `MeshRenderPass::worldBoundsOfDraws` (each resource caches its local
  AABB; lifted by the draw model matrix).
- **Casting.** `Scene3DLayer::renderShadowCasters` (default no-op; overridden by the
  two mesh layers) forwards the visual draws to `MeshRenderPass::renderDepthOnly` — a
  depth-only program (position → light clip, empty fragment) reusing each mesh's
  existing VAO. Collision hulls are not casters (they would double-darken the
  silhouette).
- **Receiving.** The mesh fragment shader and the grid filled-cell shader each project
  the world position into light space and do manual **3×3 PCF** over a plain
  `sampler2D` (the `gl::Texture` wrapper has no compare-mode; the shader guards
  out-of-frustum/beyond-far UVs as *lit*, so the CLAMP_TO_EDGE border can't smear). The
  shadow factor multiplies **only the key-light term** — the camera-locked fill and the
  IBL ambient stay lit, so shadowed surfaces read as shaded, not black. The floor
  darkens toward a floor value (not pure black) so the grid stays legible. A caster-side
  `glPolygonOffset` plus a receiver-side world **normal offset** (sized in shadow
  texels) control acne / peter-panning.
- **Lifecycle / per-dock.** `ShadowMapPass` (depth `GL_DEPTH_COMPONENT32F` FBO) is a
  per-`SceneViewWidget` member released in `releaseGlResources()` and rebuilt lazily —
  the same per-context contract as every pass. Each dock reads its own
  `key_light_dir`, so two docks shadow independently. `shadow_map_id == 0` (feature off,
  or an invalid frustum fit) makes every receiver render fully lit — a safe no-op
  degrade.
- **Gated on live data.** The whole pre-pass is skipped unless a `TransformBuffer` is
  bound (`tf_ != nullptr`), mirroring the layer color pass (`if (tf_)`): layers only
  pose geometry through TF, and a layer keeps its last draw cache after its dataset is
  deleted (the dock clears the binding via `setTransformBuffer(nullptr)` but does not
  dirty surviving layers), so an ungated pre-pass would keep depth-drawing that stale
  geometry and the floor would keep sampling a shadow whose mesh has vanished. Deleting
  the data therefore clears the floor shadow. Regression: `shadow_persistence_gl_test`.
- **Demo / verification.** `scene3d_mesh_viewer --shadows on|off` (switches the floor
  to filled cells) renders A/B screenshots; `fixtures/shadow_demo.urdf` is an elevated
  box over the ground. Cost on Iris Xe ≈ 4 ms/frame for a single-caster scene
  (well under the 16.7 ms / 60 Hz budget).

## Camera system

The camera is an interchangeable controller over a shared, serializable pose,
deliberately in `pj_scene3d_core` (Qt/GL-free) so the math is headless unit-
testable (`camera_near_far_test`, `camera_zoom_to_cursor_test`,
`camera_state_transfer_test`, `camera_follow_test`).

- **`ICamera` + `CameraState`** (`core/include/pj_scene3d_core/camera/camera.h`):
  `CameraState` (focal, radius, azimuth, elevation, fov_y, ortho_scale,
  perspective) is the pose every model exports via `state()` and adopts via
  `adoptState()`, so switching models preserves where you were looking.
  `SceneViewWidget` holds a `std::unique_ptr<ICamera>`; `setCameraModel` does
  capture → construct → `adoptState` → swap.
- **Four models, selectable.** `SceneViewWidget::CameraModel` enumerates
  `{ Orbit, XYOrbit, Fly, TopDownOrtho }` — that order is load-bearing because
  the overlay combo's index casts directly to it. Inheritance: `OrbitCamera`
  (perspective spherical orbit, Z-up; non-`final`), `XYOrbitCamera : OrbitCamera`
  (orbit pivot locked to the z=0 ground plane), `TopDownOrthoCamera : ICamera`
  (orthographic bird's-eye, rotatable about +Z — the robotics "2D mode" for
  costmaps/grids), `FlyCamera : ICamera` (first-person free eye + yaw/pitch, no
  orbit pivot: LEFT looks around, pan strafes, wheel dollies forward).
- **Adaptive near/far** (`core/src/camera/camera_math.cpp`, `adaptiveNearFar`):
  `near = max(d·1e-2, 1e-3)` from the working distance only (so close inspection
  never clips), `far = max(d·4, scene_reach)·1.5` (reaches the whole scene), then
  `near = max(near, far/1e5)` — a ratio cap that bounds depth precision.
  `scene_reach` is the scene AABB diagonal plus the focal-to-center distance.
  `TopDownOrthoCamera` uses its own flat-scene guard instead (near/far measured
  against the eye height above the focal plus the scene's vertical extent), so a
  zoomed-in costmap never clips the ground.
- **Zoom-to-cursor** (wheel): a homothety about the world point under the cursor —
  the eye and focal scale toward the cursor target by `pow(0.9, ticks)`, so the
  hovered point stays pixel-locked. A homothety is a uniform scaling, so it cannot
  rotate the orbit: `OrbitCamera::zoomToCursor` keeps azimuth/elevation **untouched**,
  scales the radius (clamped to `[lo, hi]`), and slides the pivot along the cursor
  lever by the *effective post-clamp* factor `new_radius / radius` (so the lock holds
  even when the radius clamps). It deliberately does **not** re-derive the angles from
  `eye − focal` — at large world coordinates that subtraction of two ~1e6 vectors
  loses its low bits to float32 cancellation and spuriously spins the view a fraction
  of a degree on every tick (~1.9°/tick at 5e6). `XYOrbitCamera` re-locks its pivot to
  z=0 the same cancellation-free way (re-pivot along the fixed view ray, never
  `setEyeFocal`). A degenerate ray (no ground/focal-plane hit) falls back to
  center-of-view `zoom()`; right-drag stays center-of-view zoom; on `FlyCamera`
  zoom-to-cursor degenerates to a forward dolly by design.
- **Frame follow (Position-only).** `ICamera::followShift(world_delta)` shifts the
  camera's anchor by a world delta without touching orbit angle / zoom — Orbit and
  TopDownOrtho move the focal, Fly the eye, and `XYOrbitCamera` overrides it to drop
  the z component (ground-locked). `SceneViewWidget` holds the follow target
  (`setFollowFrame`, "" = off) and applies it each tracker tick in `applyFollow`:
  look up the target's origin in the fixed frame at `render_time_`, and shift the
  camera by the delta against the previous tick's origin (`follow_prev_origin_`).
  The first tick after enabling / a fixed-frame change / a lookup gap only *seeds*
  the baseline (no shift), so enabling never jumps and the user's orbit/zoom/pan is
  preserved (follow only *adds* the target's motion). `followRenderKey(time)` hashes
  the followed origin into the dock's `viewRenderKey` so a moving target isn't
  coalesced away by the repaint gate even when the TF overlay is hidden. The UI is
  the "Camera" section's **Follow frame** picker in `Scene3DConfigPanel`, with a
  trailing **recenter** button (`SceneViewWidget::recenterOnFollowFrame` →
  `followShift(target − current focal)`) that snaps the camera onto the target on
  demand. Heading / Pose (rotation-following) modes are future work; the
  `FollowMode` enum already reserves the slot.
- **Large-coordinate precision (camera-relative rendering).** Following a frame in a
  UTM/GPS-scale map parks the camera at ~1e5–1e7 m, where float32 keeps only ~0.1–0.6 m
  of resolution and `view × world-geometry` (subtracting the ~1e6 eye from a ~1e6
  vertex) collapses to cancellation noise — the whole scene visibly *swims* on zoom.
  The scene therefore renders **camera-relative**: each `paintGL` picks a `render_origin`
  (the camera focal, in double), builds the view via
  `ICamera::viewMatrixRelativeTo(render_origin)` (eye/focal offset by the origin in
  double; bit-exact `viewMatrix()` at origin 0, pinned by a test), and threads the origin
  through `FrameContext::render_origin`. `FrameContext::lookup()` subtracts it from every
  resolved transform **in double, before the float downcast** (`toRenderSpace`, in
  `camera_math.h`) — so all TF-placed geometry (every pass/layer), the async hover
  hit-test, and the eye fed to mesh lighting land near the origin and stay precise. The
  camera *state* and *scene bounds* stay in absolute world (follow and near/far need them
  there); only the per-frame render path goes relative. Three passes that don't resolve a
  TF frame take the origin explicitly: the grid (model `= translate(−origin)`; it lives at
  the world origin), TF-connection lines (`buildTfConnectionSegments` takes it), and
  point-cloud **colour-by-axis** — position renders relative for precision, but the colour
  scalar adds the origin back (`u_color_axis_offset`) so a point's colour stays in the
  fixed frame, independent of the camera (the colormap range is lifted by the same offset,
  and algebraically cancels in the shader's normalize). `render_origin == {0,0,0}` (the
  default for hand-built `FrameContext`s in tests/demos) reproduces absolute-world
  rendering verbatim.
- **Frame-list freshness.** The frame pickers (fixed-frame + follow) are fed by
  `SceneViewWidget::refreshAvailableFrames` → `getFrameHierarchy()`, which is
  time-independent (whole buffer). That refresh used to fire only from
  `setTransformBuffer` / `setTrackerTime`, so TF folded into the buffer while the
  playhead was paused (a file load) left the combos stale until the user pressed
  play. `Scene3DDockWidget` now also re-enumerates on `datasetTransformsReady`
  (file ingest), decoupled from the time-gated repaint path, so the full tree is
  listed as soon as it loads.
- **Scene bounds.** `Scene3DLayer::worldBounds()` returns an optional source-frame
  `AABB`; `PointCloudLayer`, `OccupancyGridLayer`, `DepthCloudLayer`,
  `VoxelGridLayer`, and `GridMapLayer` override it. `RobotModelLayer` remains
  bounds-less by design: a robot is posed by the live TF tree the camera already
  frames through the other store-backed layers, so adding its links would pull the
  camera around as joints move. `Scene3DDockWidget::updateSceneBounds` resolves
  each box into the current fixed frame at the tracker time, unions **every**
  resolved box (`unionAABB` over all layers that report one — there is no per-layer
  visibility filter), and pushes the result to
  `SceneViewWidget::setSceneBounds` → the active camera, feeding the adaptive
  near/far above. No reporting layer → invalid AABB → working-distance fallback.
  (Note: because `RobotModelLayer` reports no bounds, the scene AABB excludes the
  robot mesh — relevant to any future light-frustum fit for shadows.)
- **Overlay UI.** `Scene3DDockWidget` overlays a `camera_model_combo_`
  (`{Orbit, XYOrbit, Fly, Top-down ortho}`) and a `home_button_` (Home icon,
  resets the active model to its default view — not fit-to-scene), anchored flush
  to the top-right edge. The orientation gizmo (`AxisOverlayPass`) sits in the
  bottom-right corner (set in the `SceneViewWidget` ctor) so it no longer crowds
  these controls. They are children of the dock, not the `QOpenGLWidget`, and
  `raise()`'d above it (ADS native-window z-order).
- **Persistence.** `xmlSaveState` writes the active model as a stable string id
  (`orbit` / `xy_orbit` / `fly` / `top_down_ortho` — independent of the enum
  integer / combo order) plus the `CameraState` as JSON (`cameraStateToJson`) and
  the `follow_frame` attribute (empty = off); `xmlLoadState` restores all three,
  sanitizing the state so a corrupt layout can never drive a degenerate view. A
  restored follow target absent from the current TF tree stays inert until it
  appears (`applyFollow` holds on lookup failure).

## Pose-array layer (`PosesInFrame`)

`PosesInFrameLayer` renders a `PJ.PosesInFrame` topic (`geometry_msgs/PoseArray` /
`foxglove.PosesInFrame` equivalent — a flat list of poses in **one** `frame_id`,
not a TF tree) as a per-pose coordinate-axis **triad** gizmo, with per-layer params:
**arrow length** (m), **opacity**, an **X-arrow-only** geometry mode (draw a single
X arrow per pose instead of the triad — useful for dense pose arrays where full
triads clutter), and an orthogonal **color override** (recolor every arm with one
shared color). Geometry and coloring are independent: you can recolor a full triad
or keep a single X arm in its natural red. Defaults (0.15 m, 1.0, off, off) and the
desaturated X/Y/Z colors match the TF "Frames" gizmos.

- **Pure expansion (core, GL-free, unit-tested).** `buildPoseTriadInstances(msg,
  PoseTriadStyle{axis_length, opacity, x_arrow_only, override_color, color})`
  (`core/poses_in_frame_render.{h,cpp}`) turns each pose into three
  `PoseTriadInstance{mat4 model, vec4 color}` arms — `poseToMat4(pose) · arm-rotation ·
  scale(size)`, frame-LOCAL, with `opacity` in the color alpha. The arm rotations
  (+X→+Y / +X→+Z) and colors mirror `renderTriadBound` / `AxisRenderPass`. The style
  struct keeps geometry (`x_arrow_only` → 1 arm vs 3) separate from coloring
  (`override_color` → every produced arm takes the shared `color`, else its natural
  per-axis R/G/B), so an un-overridden X-only arm is still red.
- **GPU-instanced draw.** `PosesRenderPass` (`passes/poses_render_pass.{h,cpp}`) owns
  one unit-arrow mesh (shared with `ArrowGizmo` via `gizmos/arrow_mesh.{h,cpp}`) plus
  an instance VBO (per-instance `mat4 model` at locs 2–5 + `vec4 color` at loc 6,
  divisor 1 — the `MarkerRenderPass` solid-instancing layout). **Key efficiency
  choice:** the per-instance model is frame-LOCAL and the fixed-frame TF transform is
  a `u_frame_world` uniform, so the instance buffer re-uploads only when the sample /
  size / opacity changes — TF and camera motion cost nothing. All `3·N` arms draw in
  one `glDrawElementsInstanced`, so a thousands-of-poses AMCL particle cloud stays one
  draw call. Lit shading + annotation blend (bracketed like the TF axes) keep the look
  identical to the frame gizmos.
- **Ingest.** Mirrors `OccupancyGridLayer`: `attach` bootstraps the source frame;
  `setTrackerTime` defers to `render()`, which decodes `store.latestAt(t)` via a
  **per-use** `parseLocked` binding (never cached — the reload-UAF rule), expands, and
  stages into the pass. A `SequentialUID`-keyed coalescing guard skips redundant
  decodes while scrubbing within one message; a `style_revision_` bump forces the
  current sample to re-expand after any style edit. `render` resolves
  `frame_ctx.lookup(frame_id)` once — an unresolvable frame draws nothing (orphan).
- **Params persistence.** `xmlSaveState`/`xmlLoadState` round-trip `<poses_in_frame
  gizmo_size gizmo_opacity x_arrow_only override_color override_color_value>`, which
  also makes the params copy/paste/apply-to-family-able through `pj_scene_common`'s
  `serializeLayerParams`/`applyLayerParams`. The config widget's "Override color"
  checkbox + always-visible "Color:" swatch reuse `SceneEntitiesLayer`'s marker-recolor
  text and layout (picking a color auto-ticks the box) for cross-layer consistency.
  No host edits.
- **WASM backend.** `WasmPosesInFrameLayer` resolves the same canonical object
  and calls the same `buildPoseTriadInstances` function. The QRhi path retains
  the native 80-byte instance format and shared unit-arrow mesh, draws each
  layer with one UInt32-indexed instanced call, and keeps fixed-from-source TF in
  a uniform so camera/TF motion does not rebuild instances. It uses direct-sRGB
  view-space Lambert shading and the native annotation blend. Browser-only
  limits are 100,000 poses and 32 MiB of wire bytes per layer, plus 300,000
  expanded arms per view; rejected layers warn visibly and release retained CPU
  and QRhi buffers. Non-finite instances are compacted in place before bounds and
  upload. Desktop never compiles this adapter or QRhi pass.

## Trail layer (`TrailLayer`)

`TrailLayer` draws the trajectory of a TF frame's origin (or a `kPosesInFrame`
topic's first pose) across the whole loaded time range as a
screen-space ribbon (triangle strip) in the fixed frame, split-colored at the tracker time. See REQUIREMENTS §3b for
the behavioral contract; the as-built mechanics:

- **Sampling grid = the chain's own stamps.** `TransformBuffer::chainSampleTimes`
  unions every edge's sample stamps along the connecting path (both chains to
  the common ancestor), so a statically-mounted child (lidar on base_link)
  trails with the motion higher up the chain. The pure build functions
  (`core/trail_sampling.{h,cpp}`: `decimateEvenly` — even index decimation,
  endpoints kept, cap `kMaxTrailPoints` = 50k — `buildTfFrameTrail`,
  `buildPoseTopicTrail`) are Qt/GL-free and unit-tested. Pose-topic builds
  decimate **before** decoding, so at most 50k messages ever parse.
- **Split ribbon, zero-upload scrub.** `TrailRenderPass` keeps ONE vertex
  buffer — a screen-space triangle ribbon, two expanded vertices per point
  (position + joint tangent + side flag), because core GL caps real lines at
  1 px (`glLineWidth(2)` is GL_INVALID_VALUE on strict core contexts) and the per-trail thickness (default 4 px) must be honored exactly.
  CPU points are absolute fixed-frame doubles, downcast to float AFTER
  subtracting `render_origin` (the large-coordinate rule); the pixel-width
  offset happens in the vertex shader (scaled by `render_scale`, the EDL
  rule). Two draws over the one buffer: points `[0, past)` in the past color,
  `[past-1, N)` in the future color (the shared pair keeps the ribbon
  continuous). The split index is a binary search over the stamps, so scrubbing
  re-uploads nothing; the buffer re-uploads only on rebuild or a render-origin
  change. `renderKey` folds (build revision, style revision, split index) —
  between two samples the key is constant and the PR #259 repaint gate
  coalesces.
- **Documented follow-up — incremental live-append upload.** A live append
  currently rebuilds the pass's expanded vertex buffer and re-uploads it
  wholesale (near the 50k cap: ~4 MB CPU touch + ~2.8 MB `glBufferData` per
  appended tick). A `glBufferSubData` tail path (join-tangent-only recompute,
  geometrically grown `GL_DYNAMIC_DRAW` buffer) is the planned refinement;
  file loads never hit this path.
- **Async rebuild / sync append.** Full TF-source rebuilds run on `QtConcurrent`
  with latest-wins generation coalescing (safe: `TransformBuffer` is
  `shared_mutex`-protected and the task copies the `shared_ptr`). Pose-source
  rebuilds stay synchronous on the GUI thread — `ObjectStore` reads and
  `parseLocked` decodes are GUI-thread contracts — bounded by
  decimate-before-decode. Live-edge appends (both kinds) are synchronous and
  tiny: gated on `TransformBuffer::revision()` / the store entry count, they
  sample only stamps past the last built point and ring-trim at the cap.
- **Fixed-frame self-heal.** The layer adopts the fixed frame from the render
  `FrameContext` (authoritative) and rebuilds on mismatch, so attach order
  never matters. The TF buffer itself arrives via `Scene3DLayerContext` at
  attach **and** through `Scene3DDockWidget::pushTransformBufferToTrailLayers`
  whenever the dock's binding changes — necessary because layout restore
  replays `<layer>` elements BEFORE the `<config_topic>` that binds TF, so a
  restored trail starts buffer-less ("waiting for TF") and heals when the
  binding lands.
- **Not topic-id-backed.** Trails register under synthetic local ids (the
  robot-model allocator, generalized to `allocateLocalLayerId`) through the
  `SceneDockWidget::insertLayer` seam — no `LayerFactory` slot and no new
  `sdk::BuiltinObjectType` (`info().object_type == kNone`). Persistence marks
  the layer element `role="trail"`; everything else — including the pose
  SOURCE topic's `source_topic_name`/`source_dataset_*` identity — lives in
  the layer's own `<trail>` payload, and the layer self-restores via
  `TrailLayer::xmlLoadStateResult` (tri-state incl. deferral — the
  RobotModelLayer topic-source idiom), with the dock's `restoreTrailElement`
  reduced to a thin host that honors the result. Restore branches BEFORE the
  object-type gate. `xmlLoadState` applies STYLE only — family copy/paste
  (PR #204) restyles, never retargets.
  Pruning: a TF-source trail orphans on a merely-missing frame (it revives if
  the frame returns, like a follow target) but is REMOVED when the bound TF
  dataset unloads (`resetTransformBindingIfDatasetGone` deletes TF trails
  before dropping the binding — a gone dataset deletes, a gone frame orphans).
- **Pose trails are owned, not registered.** `PosesInFrameLayer` holds a
  `std::unique_ptr<TrailLayer>` (no QObject parent) that is never handed to the
  dock, so it has no Topics row, no settings panel, and no synthetic id. The
  owner forwards the full lifecycle — `attach`/`detach`, `initializeGL`/
  `render`/`releaseGL`, `setFixedFrame`/`setTrackerTime`/`setVisible` — and
  re-emits the trail's `repaintRequested`/`configurationChanged`/
  `statusWarningChanged`, plus relays `statusWarning()`, since the dock only
  polls REGISTERED layers and an embedded trail would otherwise fail silently.
  Two forwarding hazards are load-bearing: the trail renders BEFORE the pose
  layer's early-outs (it must still draw on ticks with no decoded pose set);
  and unlike the gizmos the trail REBUILDS against the fixed frame, so the
  owner caches `fixed_frame_` to hand to a trail enabled later. Enabling
  happens outside a GL context, but needs no special handling: the view calls
  `initializeGL()` on every layer every paint (see `Scene3DLayer::initializeGL`
  — passes self-guard on `initialized_`), so the forward covers it.
  Persistence nests the
  trail's own `<trail>` element inside `<poses_in_frame>` (presence == enabled;
  `isLeafPayloadWithOptionalChild` admits exactly that one child), reusing
  `TrailLayer::xmlSaveState`/`xmlLoadState` — the style-only load is exactly
  right here, because the source is the owner's topic and is never retargeted
  by a payload.
- **Entry points.** Right-click on a frame gizmo — `SceneViewWidget::
  contextMenuEvent` reuses the hover pick (`pickFrameAt`, the extracted core of
  `updateHoverFrame`) and accepts ONLY when a frame is hit, else `ignore()`s so
  the host dock's standard menu still appears; the dock owns the `QMenu`
  (Create trail / Set as fixed frame / Follow this frame). Note `pickFrameAt`
  walks the TF tree only, so pose gizmos are not pickable — a pose trail is
  reached from its layer's settings instead. Plus the `Scene3DConfigPanel`
  "Trail" row (frame combo + add button, TF only) and the "Trail" toggle on
  `PosesInFrameLayer`'s config widget. Both flavors of trail are found through
  the dock-local `trailOf()` helper (the layer IS one, or OWNS one), which is
  what lets the TF-buffer re-bind fan-out reach embedded trails.

## Pointcloud layer (`PointCloudLayer`)

`PointCloudLayer` keeps the existing `convertCanonical()` -> `DecodedPointCloud` ->
`CloudVertex` fallback for layouts that need CPU decoding, and adds a narrow zero-copy
fast path for little-endian clouds whose xyz fields are contiguous float32 values. The
fast path uploads the canonical `sdk::PointCloud::data` buffer verbatim and binds the
shader attributes with the point's native stride/offset/type; `FastCloudData` retains the
wire cloud by value so its `BufferAnchor` keeps the bytes alive across GL-context
recreation, letting `releaseGL()`/`initializeGL()` re-upload from the same source just as
the fallback re-uploads from retained decoded vectors.

RGB-direct (`kRgb`) clouds also take the fast path when the cloud carries a packed,
contiguous `rgba`/`rgb` uint32 field (`checkFastPath(..., want_rgba=true)`): the four colour
bytes upload verbatim and bind as a normalized `vec4` straight at the field offset —
pixel-identical to the CPU `convertCanonical(extract_rgba)` path, with no extraction.
Scattered separate `red`/`green`/`blue` channels fall back to the CPU packer.

### Cube shape: the hexagon fan

The `kCube` shape is far more expensive per point than the sprite shapes — it is a solid,
not a billboard — so its geometry is cut to the minimum that is still pixel-equivalent.
A closed opaque cube never shows more than the three faces whose outward normal points at
the camera, so `PointcloudRenderPass` draws exactly those, as a **6-triangle fan around the
cube's near corner**: `glDrawElementsInstanced(GL_TRIANGLES, 18, GL_UNSIGNED_SHORT, …)` over
7 logical vertices, versus the 24-vertex / 36-index full solid. There is no cube vertex
buffer at all — `cubeFanCorner(gl_VertexID, cubeFaceSigns(...))` in `cube_mesh.h` generates
the corners, and the cloud VBO binds at the same attribute locations the point program uses
(divisor 1), so both VAOs share one wiring routine.

Consequences worth knowing before editing this path:

- **Face choice is per instance.** The sign vector comes from each cube's own centre versus
  `u_face_pick_eye`, which is homogeneous: the eye POSITION (w = 1) under perspective, the
  scene→camera DIRECTION (w = 0) under ortho, where the visible faces depend on the view
  direction alone. It must be derived from the PROJECTION matrix, not from `u_viewproj` —
  folding view into proj leaves the eye's view-axis distance in the `[3][3]` slot, so the
  usual `proj[3][3] == 0` perspective test silently fails there. Getting this wrong gives
  every cube in the cloud the same face triple and drops a face from half of them.
- **The face normal is recovered in the fragment shader** from the interpolated corner
  (`cubeFaceNormalView`, the axis whose |component| reaches the surface), which is why the
  vertex stage carries no normal attribute. `v_local` must be interpolated `centroid`: at
  the default pixel-centre sampling, partially covered MSAA pixels extrapolate outside the
  triangle and can push a non-face component past 0.5, mis-picking the face.
- **No `discard`.** The outside-range fade's "invisible anyway" case is culled in the vertex
  stage instead, so the fragment shader stays discard-free and the GPU keeps early-Z depth
  writes — cube clouds overdraw heavily and late-Z would shade every hidden fragment.
- **Colour is evaluated per point, not per fragment.** A point's colour (colormap lookup,
  solid, or per-point RGB) plus the sRGB→linear conversion is constant over the whole cube,
  so BOTH the cube and the point/sphere vertex shaders now compute it once into a `flat`
  varying; the shared `colormapGlsl()` LUTs moved to the vertex stage with it. The cube's
  edge outline survives this because it SCALES the colour — `mix(b, b*0.4, e) == b*(1-0.6e)`
  — and scaling commutes with the gamma: `pow(b*k, 2.2) == pow(b,2.2) * pow(k,2.2)`. So the
  fragment stage keeps one scalar `pow` in place of a colormap and three vector ones, and
  the result is algebraically identical (measured: max 1/255 on 0.02% of pixels, pure float
  reassociation). Applying the outline in linear space instead would drop that last `pow`
  entirely, but it was measured at 3% and is not exact — don't.
- **Camera inside a cube** is the one case the fan gets wrong (the centre-vs-eye comparison
  needs the camera at least half a cube away). Irrelevant at point-cloud cube sizes; it is
  why `VoxelGridLayer` keeps the full 24-vertex solid.

#### Screen-space LOD: sub-pixel cubes become matched sprites

Below about a pixel even the fan is more geometry than the result can show, so
`PointcloudRenderPass` swaps the whole cloud for one point sprite per cube, drawn over the
existing point VAO (divisor 0 — no new buffers) by a third, deliberately minimal program:
no colormap in the fragment stage, no normal, no `discard`, one flat varying and a single
store. `setCubeLodThresholdPx()` exposes the threshold in device pixels; 0 disables it.

**It is a whole-cloud switch, decided on the CPU, and that is a measured choice.** The
obvious design — test each instance in the vertex shader and let the two programs split
the cloud — is *slower* than not doing it. A degenerate-clipped fan instance still runs
its 7 vertex invocations, and on real hardware those cost about what drawing them does, so
splitting a cloud pays for both programs and saves neither. Measured on an RTX 4070 at
500k points with the camera inside the cloud, a per-instance split ran 0.91x (and fell to
0.70x as more cubes converted), while the whole-cloud form is exactly 1.00x there and
1.7-2.2x in the wide shot. The win comes from **dropping a draw**, not from converting
cubes. So `render()` asks one question — does the cloud's AABB prove that no cube reaches
the threshold? — via `aabbClipWRange()` (clip w is affine, so the box bounds every point),
and runs one program or the other. A stale extent can therefore only draw near cubes as
sprites; it can never make points vanish, which a shader-side cull could.

**It is currently OFF by default** (`cube_lod_threshold_px_` is 0; `setCubeLodThresholdPx()`
enables it, which the equivalence tests and the A/B benchmark do). The appearance match
below is exact in every respect except one, and that one is disqualifying in the product:
see "Why the LOD is disabled". Each piece is otherwise an exact property of the cube, not a
fudge (`kCubeSpriteGlsl`):

- **Area, not side.** The three visible faces cover `(|r.x|+|r.y|+|r.z|) * side²` where `r`
  is the view rotation's third row, so the sprite is a square of that area. (Cauchy's 1.5
  is the mean over all orientations; this is the exact value for the current one, for the
  same cost.) Matching the side instead is ~37% too bright.
- **Area-weighted Lambert.** The faces' `cubeLambert()` terms weighted by those same
  projected areas. Which face an axis shows is per instance, but the weight is not
  (`+n` and `-n` foreshorten alike), so it is three dot products and no CPU uniforms — the
  light direction stays defined once, in `kCubeLightingGlsl`, shared with the cube's own
  fragment shader so the two cannot drift apart.
- **`kCubeEdgeMeanScale`.** The dark edge band covers a fixed fraction of every face;
  dropping it leaves sprites reading 13% brighter than the cubes beside them.
- **Energy conservation below the driver's minimum point size.** Where `GL_POINT_SIZE_RANGE`
  refuses to go as small as the cube, the sprite is widened and dimmed by the area
  overshoot, so a receding cloud keeps losing brightness instead of plateauing at one pixel.

`pointcloud_cube_lod_gl_test` measures all of it against the fan: total emitted light
agrees within 3.4% from 0.67 px down to 0.125 px (the test allows 8%), which is also the
no-popping guarantee, since the fan's brightness is continuous in distance. Near and
straddling clouds must come out bit-identical, pinning the gate as whole-cloud.

#### What the browser gets, and what it would take to port

None of the above runs in the browser. `scene_view_widget_rhi.cpp` never includes
`PointcloudRenderPass`; it draws `WasmPointShape::kCube` from its own `.qsb` pipelines over
the 24-vertex solid. The one change it does inherit is the **winding fix**, which is a
browser-specific bug fix: those pipelines set `CullMode::Back`, so a CW-outward
`kCubeVertices` had them culling front faces and drawing each solid's far side.

Five of the six optimizations are portable to WebGL 2 whenever someone builds it; only the
compute-compaction follow-up is not (WebGL 2 has no compute shaders, SSBOs or indirect
draw). Two mechanics matter when doing it:

- `gl_VertexID` becomes `gl_VertexIndex` under qsb's Vulkan semantics. This costs nothing
  because every helper in `cube_mesh.h` takes the vertex id as an **argument** rather than
  reading the builtin — keep it that way.
- The colour hoist cannot be shared. The browser samples a `color_lut` **texture** where
  the desktop evaluates colormap polynomials in-shader, so that one is a parallel
  implementation, not a reuse. (Vertex texture fetch is guaranteed in GLES 3.0, so it is
  still possible.)

**Shared shader source (in place).** qsb honours `GL_GOOGLE_include_directive`, resolving
includes against the including file's directory, so the appearance helpers live once in
`pj_scene3D/widgets/shaders/cube/*.glslinc`: the browser `.frag` files `#include` them
directly, and `pj_scene3D/widgets/CMakeLists.txt` embeds the same files into a generated
`cube_shader_sources.h` of `constexpr std::string_view`s that the desktop concatenates at
runtime. Those files must stay free of uniforms, builtins and version-specific syntax —
the two backends differ completely on all three, which is exactly why only pure functions
and constants can be shared.

Two mechanics to respect when adding one: the `.glslinc` list joins
`CMAKE_CONFIGURE_DEPENDS`, because the `.qsb` staleness check runs at *configure* time and
would otherwise not see the edit; and the browser sources are Vulkan-style `#version 440`
qsb *inputs* — the GLSL ES 300 the browser actually runs is what qsb emits from them.
`wasm_shader_constants_test` guards that the browser shaders keep including the shared
files rather than re-inlining a literal.

**Ported so far:** the winding fix (a browser-only *bug* fix — those pipelines set
`CullMode::Back`, so a CW-outward `kCubeVertices` had them culling front faces and drawing
each solid's far side) and the shared shader source above. Both are appearance-neutral or
appearance-correcting, and the shared-source change was verified byte-for-byte equivalent
by diffing the compiled ES 300 output.

**Nothing else has been ported, deliberately.** The browser Scene3D path is built with
`PJ_WASM_WITH_SCENE3D=OFF` in CI, so no automated check ever renders a browser cube; any
behaviour change there has to be paired with a visual check on a real browser build, and
that has not happened. The remaining items, in the order they should land, each with the
mechanic that makes them non-trivial:

1. **Drop the `discard` from `cube.frag`.** The largest single win on the desktop, and the
   cheapest of these — it is provably dead code there (`buildColormapLut` writes alpha 255
   and `cube.vert` already culls the only path to a transparent fragment), so it is purely
   a matter of confirming it on screen.
2. **Colour hoist.** Binding 1 (`color_lut`) is fragment-only in the shader-resource
   bindings and must become vertex-visible. Vertex texture fetch is guaranteed in GLES 3.0.
   This one cannot share code with the desktop: the browser samples a LUT texture where the
   desktop evaluates colormap polynomials in-shader.
3. **Hexagon fan.** Needs `face_pick_eye` in `PointUniforms` (272 → 288 bytes, touching
   every shader that declares the block), an 18-entry index buffer, and a per-instance-only
   vertex layout for the point-cloud cube pipeline *alone* — voxels and markers keep the
   full solid. `cubeFanCorner` already takes the vertex id as an argument, so the shared
   GLSL works unchanged despite qsb spelling the builtin `gl_VertexIndex`.
4. **Sprite LOD.** A second `Points` pipeline plus its HDR clone and depth-replay entries.
   `chooseCubeDrawMode()` already makes the decision and is backend-agnostic, so the CPU
   half is done; only the pipeline is missing.

Compute compaction is the one item that cannot be ported at all — WebGL 2 has no compute
shaders, SSBOs or indirect draw.

#### Why the LOD is disabled

**Coverage is applied as brightness, and that is wrong on a light background.** Where the
driver's minimum point size exceeds the cube, the sprite is widened and its colour scaled
by the area overshoot — at 0.1 px that factor is 0.01. Total light is conserved, which is
exact against a *black* background. But the opaque pass writes alpha 1 with blending
disabled, so the widened sprite REPLACES the background over the whole minimum-sized point
rather than blending into it: on PJ's light 3D background a zoomed-out cloud turns black,
and flips between black and coloured as streaming moves the cloud's extent across the
whole-cloud gate. Observed in the app; that is why `cube_lod_threshold_px_` defaults to 0.

Note the LOD's own GL test cannot see this — it clears to black, the one background where
conserving energy and conserving coverage look identical. Re-enabling the LOD means fixing
the compositing AND rendering that test against a non-black background so it can fail.

The fix is coverage, not brightness: write `gl_SampleMask` from the coverage fraction so
the sprite lights a FRACTION of the MSAA samples, exactly as the fan's sub-pixel triangles
do. That composites correctly against any background and gets depth right too, at the cost
of early-Z on the sprite path — acceptable, since sub-pixel sprites barely overdraw.

#### Other known limits of the sprite LOD
- **A camera inside an individual cube** still gets the wrong three faces — the fan picks
  them from the cube's centre. Unreachable at point-cloud cube sizes and already the
  reason `VoxelGridLayer` keeps the 24-vertex solid, but `setSizeMeters` does not stop a
  user from making cubes large enough to swallow the viewpoint.
- **A compatibility-profile context rasterizes multisampled points as circles**, π/4 of the
  square the sprite's area match assumes. `initializeGL()` enables `GL_POINT_SPRITE` when
  `GL_CONTEXT_PROFILE_MASK` reports compatibility, which restores core semantics — and also
  defines the `gl_PointCoord` the sphere imposter needs. Read the profile from GL, not from
  `QSurfaceFormat`: Qt reports CoreProfile on contexts whose `GL_VERSION` says otherwise.

`cube_mesh_test` pins the fan's coverage in all eight sign octants and the CCW-outward
winding of the shared solid; `pointcloud_cube_faces_gl_test` renders two mirrored cubes and
asserts equal area (per-instance face choice) plus three distinct face shades.
`demos/pointcloud_shape_benchmark` times all three shapes against the scene's MSAA level,
with a `cube+LOD` column and a `--lod PX` sweep that re-derives the default threshold.

Whole-cloud **frustum reject**: `render()` skips the draw when the cloud's source-frame AABB,
tested through `proj * view * model` by `aabbOutsideFrustum`, lies outside the view volume.
The box is plumbed separately from the colour-range bounds (`setGeometryBounds`) and is
CLEARED by every cloud swap, because bounds describing the previous cloud could cull the new
one; the async GPU reduction below re-establishes it, and only when it ran for the cloud
currently in the VBO.

The geometry AABB (`world_bounds_`) feeds the camera scene-fit every frame, so it must refresh
on every sample. On the bounds-only cases (RGB-direct, solid, spatial-axis, auto-off, non-dirty —
i.e. when no colormap scalar pass is needed) with a 4-byte-aligned fast-path layout, that
reduction runs on the **GPU** rather than the CPU: `PointcloudAabbReducer` dispatches a compute
shader over the already-resident fast-path VBO (no extra upload), reducing min/max via an
order-preserving float→uint key (`aabb_gpu_key.h`) and `atomicMin`/`atomicMax`, and reads the
6-value result back **asynchronously** (fence + non-blocking `poll()` in the next frame's
`render()`). The completed AABB flows to the layer through a bounds callback (`onGpuAabb`) which
updates `world_bounds_` and requests a repaint, so the existing `repaintRequested →
updateSceneBounds` path re-fits the camera. The result lags a frame or two — invisible to the
auto-fit — so the per-sample bounds cost leaves the GUI thread entirely (≈80–170× less GUI-thread
work than the CPU scan at 1–5 M points; see `demos/pointcloud_aabb_benchmark`).

Fallbacks keep the CPU scan: the **first** sample after attach/reset (to seed `world_bounds_`
synchronously and probe compute support), the kField auto-range-dirty case (it needs the scalar
min/max anyway, getting bounds for free in the same pass), a misaligned layout, the decoded
`convertCanonical` path (bounds come free there), and any context without compute (GL < 4.3,
e.g. Windows software GL). The layer only drops the CPU scan once the pass confirms
`gpuAabbAvailable()`, so unsupported drivers degrade safely.

The WASM-selected `WasmPointCloudLayer` is deliberately separate from this
native OpenGL implementation but accepts both `kPointCloud` and, when
`PJ_WASM_WITH_COMPRESSED_POINTCLOUDS` is enabled, `kCompressedPointCloud`.
Compressed samples are only transcoded in the core codec boundary; wire/CDR
parsing remains in the data-source parser. Both object types converge at
`convertCanonical()` and share point/cube rendering, colors, bounds, warnings,
visibility cleanup, and `<pointcloud>` persistence. Dock classification,
interactive acceptance, factory registration, deferred restore, and XML
validation all use the same feature-gated type predicate; registering a factory
alone is insufficient because the host otherwise falls through to a modal
unsupported-topic warning, which cannot call `exec()` in the Asyncify-free
browser build.

## Depth-cloud layer (`DepthCloudLayer`)

Back-projects a depth image into a 3D point cloud (one point per valid pixel),
colored by depth. Sibling of the 2D depth view — same data, different geometry.

- **No `kDepthImage` producer.** Depth arrives as an `sdk::Image` with a depth
  `encoding`; the parser can't tell depth from color at schema-classification
  time. So `DepthCloudLayer` is registered on `kImage` and the dock **gates** a
  topic by peeking the first sample's `encoding`
  (`isDepthEncoding`: `16UC1` / `32FC1` / `compressedDepth`) — color images are
  refused. Because the two share `kImage`, an empty-placeholder image drop opens
  the **2D** viewer (host policy in `pj_app`); a depth image reaches a 3D dock by
  being dropped onto an existing one or via the 3D family switch.
- **Decode (`toDepthView`).** `16UC1`/`32FC1` alias the image bytes (zero-copy).
  `compressedDepth` is PNG-decoded via `QImage` to a `16UC1` (millimetre) view —
  including a **bare-PNG signature repair**: RealSense bags carry a headerless PNG
  that begins at the `IHDR` chunk, so the 8-byte signature + IHDR length are
  restored before decode (mirrors the 2D path's `toDepthImage`; the matching
  parser-side ConfigHeader handling is `parser_ros`). 32FC1 inverse-quantized
  compressedDepth is not yet handled.
- **Intrinsics by `frame_id`.** A `CameraInfo` is joined to the image by
  **`frame_id`** (authoritative, never the topic name — matches Foxglove's rule /
  Rerun's camera hierarchy). An exact match wins; with none, `resolveIntrinsics`
  falls back to a lone `CameraInfo` only when unambiguous, else refuses rather
  than pair the wrong camera. The result is memoized by `(frame_id, CameraInfo
  SampleId)` and revalidated metadata-only via `latestEntryIdAt` (no parse and
  no payload resolve); a new CameraInfo sample re-projects the current depth
  image even when the depth sample itself is unchanged.
- **Back-projection.** The math is the Qt-free core `depth_backproject.{h,cpp}`
  (`depthToPoints`); a single pass yields positions, the per-point depth scalar,
  the world AABB, and the colormap range together (via the optional
  `scalar_out` / `bounds_out` / `scalar_range_out` out-params).
- **Render + color.** The POC feeds the points through the existing
  `PointcloudRenderPass` (a dedicated GPU attributeless / `texelFetch` pass is a
  planned follow-up). Depth is colored through the **shared** `PJ::Colormap`
  (turbo / viridis / plasma / grayscale, `pj_widgets/Colormap.h`) — the same
  source of truth as the 2D depth view and the pointcloud field coloring.
- **Config.** Per-layer colormap, point size, and a min/max depth range.

## Voxel-grid layer (`VoxelGridLayer`)

Renders a dense `sdk::VoxelGrid` (SDK ≥ 0.10.0) — a dense 3D lattice whose
per-voxel value is generic via `fields` (occupancy / cost / ESDF / semantic, or a
direct RGBA channel) — as GPU-instanced cubes. Sibling of the pointcloud layer for
volumetric data.

- **Dense→cubes expansion is entirely GPU-side.** `VoxelGridRenderPass` uploads
  the selected field as a **3D texture** and issues **one** `glDrawElementsInstanced`
  over a unit cube (`column*row*slice` instances). The vertex shader derives each
  voxel from `gl_InstanceID`, `texelFetch`es its value, evaluates the viewer-side
  draw predicate (which the schema does **not** encode), and degenerate-clips culled
  voxels. So the **CPU / draw-call** cost is independent of voxel count (one draw),
  and a re-scrub to a cached grid re-uploads nothing — though the GPU vertex shader
  still runs once per voxel.
  It keeps the **full 24-vertex solid** rather than the pointcloud layer's hexagon fan,
  because a voxel is large enough to swallow the viewpoint and the fan's centre-vs-eye
  face choice is only exact while the camera stays outside the cube. Back faces are
  culled around the draw instead (`GL_BACK` / `GL_CCW`, paired with a disable — the
  engine's ambient state has culling off), which halves rasterization for free now that
  `kCubeVertices` is genuinely CCW-outward.
- **Qt-free core.** The coordinate/value math (`core/voxel_grid_view.{h,cpp}`,
  `core/voxel_grid_value.{h,cpp}`) is headless unit-tested.
- **WASM backend.** `WasmVoxelGridLayer` resolves the same canonical object and
  reuses the same field selection, scalar/RGBA packing, bounds, and draw-mode
  vocabulary. QRhi retains one packed volume per layer and submits one indexed
  cube draw with `column*row*slice` instances; the vertex shader performs the
  same `gl_InstanceID`/`texelFetch` predicate as native. Grid origin, fixed-frame
  TF, and camera-relative conversion compose in double precision before upload.
  Browser-only caps are 4 Mi voxels and 32 MiB wire bytes per layer plus 8 Mi
  voxels per view. Allocation also checks the live WebGL
  `GL_MAX_3D_TEXTURE_SIZE`; the retained 2026-07-16 Chromium/Firefox/WebKit
  matrix measured 2,048 on all three engines. Rejected layers warn visibly and
  release retained CPU/QRhi volume state. The browser direct-sRGB target omits
  the native HDR composite, but field, predicate, color, opacity, geometry, and
  XML semantics are preserved. Desktop never compiles this adapter or QRhi
  pipeline.
- **Follow-up.** A GPU compute-shader compaction path (`glDrawElementsIndirect` over
  only the accepted voxels, GL ≥ 4.3) is a documented follow-up for very large dense
  grids; not built.

## Grid-map layer (`GridMapLayer`)

Renders an `sdk::GridMap` (SDK ≥ 0.26.0) — a 2D grid of per-cell channels, the
canonical form of `grid_map_msgs/GridMap` and `foxglove.Grid` — as a lit,
colormapped **heightfield** surface, the way Foxglove's linear-interpolation
Grid mode does. Desktop OpenGL only (`isKnownFutureScene3dLayer` lists the type in
the browser backend so deferred layouts survive).

- **Procedural geometry.** `GridMapRenderPass` draws `6 * (cols-1) * (rows-1)`
  vertices with `glDrawArrays` and no VBO or index buffer: the vertex shader derives
  quad, triangle and corner from `gl_VertexID`, `texelFetch`es the corner's
  elevation from an R32F texture and places the vertex at the cell centre. A new
  sample therefore costs two texture uploads (elevation + color channel, both
  de-interleaved on the CPU by the Qt-free `core/grid_map_view.{h,cpp}`) and nothing
  per vertex. Elevation "None" skips the texture and draws a flat plane.
- **Holes stay holes.** Every vertex fetches all three corners of its triangle and
  emits a `flat` validity; the fragment shader discards the whole triangle when any
  corner is non-finite, so a NaN cell punches out exactly the triangles touching
  it. Validity uses `abs(e) < 3e38` rather than `isnan()` so it does not depend on
  a driver preserving NaN through `texelFetch` semantics.
- **Normals** are central differences of the elevation texture scaled by cell size,
  with a one-sided fallback at borders or beside a NaN neighbour and an up-normal
  when neither neighbour is finite — a hole never tilts a valid triangle.
- **Shading** is a Lambert key light (world-space `ViewParams::shading.key_light_dir`)
  plus a camera-locked fill, scaled by the view's `ambient/direct/fill` knobs,
  two-sided via `gl_FrontFacing`. Not the mesh GGX/IBL path; no shadow cast or
  receive.
- **Color** is `LUT(clamp((v - lo) / (hi - lo)))` through the shared `PJ::Colormap`
  GLSL; `lo/hi` are the finite min/max of the color channel (auto) or the manual
  range. A degenerate range (`hi == lo`, or no finite sample) paints the LUT
  midpoint; a NaN color value on a valid triangle maps to the LUT low end.
- **Placement** is `TF(fixed <- frame_id at the CURRENT tracker time) * origin`,
  resolved per frame through `FrameContext::lookup()` as the occupancy pass does.
  `worldBounds()` is a source-frame box (the local extent times the origin pose, z
  from the finite elevation range) — never the fixed-frame transform.
- **Lifecycle** follows `PointCloudLayer`: the topic attaches with a parser OR the
  canonical `PJ.GridMap` codec (`resolve_object.cpp`); `renderKey()` is the sample
  UID from `latestEntryIdAt` plus the fixed<-source transform and never resolves
  bytes; the canonical object is cached per UID so a selector change re-packs
  without re-transcoding. On the parser route the cached object's anchor was
  allocated inside the plugin DSO (the grid_map transcoder owns its own buffer,
  unlike the zero-copy point-cloud views), so the layer holds the binding
  keepalive next to the cache, declared so the grid dies first — otherwise the
  extension catalog can unmap the DSO before the dock is destroyed and the
  cached grid's destructor faults at shutdown; `validateGridMap()` runs before extraction and a failure
  (or a grid past `kMaxRenderableGridMapCells`) surfaces a layer-row status warning.
  Dataset replace and detach reset the memo, bounds and warnings.
- **Config.** Elevation field (with "None"), color field, colormap, auto / manual
  range (manual seeded from the last auto range), lighting toggle, opacity —
  persisted as `elevation_field, color_field, colormap, auto_range, range_lo,
  range_hi, lighting, opacity`. The persisted names are REQUESTS: a missing field
  falls back (with a warning) but the request is kept so it is honoured again when
  the field reappears.
- **Not built:** shadows, wireframe overlay, nearest-neighbour cuboid mode,
  RGBA-from-fields color, the browser renderer.

## Repaint coalescing & autoplay

- **Per-tick repaint coalescing.** During playback the scene must not free-run at
  60 Hz redrawing identical frames. `ISceneLayer::renderKey` produces a decode-free
  per-layer fingerprint (sample stamp ⊕ transform), and `SceneDockWidget::onTrackerTime`
  skips the repaint when nothing a layer would draw has changed — holding the
  ≤ 60 Hz / idle→~0 CPU budget the performance goals require (PR #259). `renderKey`
  must derive from index/entry timestamps, **never** from a `latestAt` that would
  decompress cold chunks inside the gate.
- **`--autoplay`.** The app's `--autoplay` CLI flag starts hands-free looped
  playback (PR #260); it doubles as the `PJ_AUTOPLAY` profiling hook used to measure
  the repaint/render cost above.

## URDF / robot-model subsystem

- **Parser** (`widgets/src/urdf_parser.{h,cpp}`): `QDomDocument`-based;
  reads `<link>` visuals/collisions (origin, geometry, material). Geometry is
  the `GeomShape` variant — box/cylinder/sphere primitives **and** meshes.
  `<joint>` is parsed into `RobotModel::joints` (name/type/parent/child/origin),
  but **TF still owns kinematics**: link poses come from the live TF tree. xacro is
  detected (element prefix or filename) and rejected with an actionable error,
  never a cryptic XML failure.
- **Fixed-joint TF bridges** (`core/robot_model_bridges.{h,cpp}`): a URDF may
  contain a frame the data's `/tf` never publishes — classically a gripper rigidly
  mounted to an arm flange, where the mount transform lives only in the URDF (the
  DROID arm-droid dataset is exactly this). `fixedJointStaticTransforms()` turns
  each **fixed** joint into a static (`/tf_static`-style, epoch-stamped)
  `StampedTransform`; `RobotModelLayer` caches them (frame-prefixed) and
  `ensureStaticBridges()` re-asserts them into the dataset `TransformBuffer` every
  tracker tick via `injectMissingStaticTransforms()`. The inject is **guarded**
  (`getParent(child)==nullopt` — never overrides a frame the data publishes),
  **idempotent**, and **self-healing** (a same-file reload that clears the buffer
  re-bridges on the next tick). Once a gripper base is bridged, the movable
  knuckle frames the data *does* publish under it cascade into resolvability on
  their own. Movable-joint articulation (a `JointState` source, `<mimic>`,
  default-to-0) is deferred.
- **Sources.** A `RobotModelLayer` reads its URDF from a store topic
  (`kRobotDescription`, decoded via the topic's parser — payload bytes are
  CDR-framed, never cast to a string), a local file, or an http(s) URL.
  File/URL layers are dock-local: synthetic registry ids that never touch the
  ObjectStore (`Scene3DDockWidget::addRobotModelLayer{,FromUrl}`).
- **Mesh loading** is async (`QtConcurrent::run` → futures polled in
  `render()`); the GL thread never blocks. assimp covers STL/DAE/OBJ/glTF.
  `MeshData` carries UV0 + tangents (`aiProcess_CalcTangentSpace`) and a
  per-`SubMesh` `Material` (glTF 2.0 metallic-roughness, read via assimp's
  material abstraction). Each map (`TextureSource`) is an external file path OR
  inline bytes for embedded glTF/GLB `*N` images (still PNG/JPEG-encoded), keyed
  by content hash; `MeshRenderPass` decodes both, uploads color/emissive sRGB and
  data maps (metallic-roughness/normal/occlusion) linear, and caches per-context
  by key plus texture color space. Sources without PBR factors fall back to
  `MeshShadingParams`.
- **Rendering**: `MeshRenderPass` (metallic-roughness GGX + normal mapping +
  occlusion + emissive + **analytic image-based ambient** — diffuse irradiance
  plus a split-sum specular reflection of a procedural ground→sky environment
  (Karis `envBRDFApprox`, no HDRI cubemap) — lit by a **fixed world key/"sun"
  light** and a dimmer **camera-locked fill headlight**. Metals now reflect the
  sky/ground gradient instead of reading near-black; a true prefiltered-cube IBL
  from an HDRI is still future work) draws meshes by
  key and primitives from unit
  box/cylinder/sphere geometry (URDF cylinder is Z-aligned; sizes ride the
  model matrix). Unresolved meshes render a **magenta unit cube** —
  intentionally ugly, impossible to mistake for data. Visual meshes are split
  into opaque and best-effort translucent draws (layer opacity, override alpha,
  or glTF `BLEND`; no depth sort). Collision geometry with no `<material>` gets
  an **orange tint** (RViz convention) so hulls read as distinct from the visual
  meshes; it renders as a **translucent overlay** (blend on, depth-writes off, so
  the real robot shows through) below full opacity, and as a **solid, occluding**
  hull at opacity ≈ 1.0 (the opaque path: blend off, depth-writes on). Scene-wide
  opacity/visibility comes from `meshShadingParams()`. URDF draw calls are cached
  in camera-relative render space (after `FrameContext::lookup` subtracts the
  current render origin), so `RobotModelLayer` reuses them across pure
  view/projection changes but rebuilds when pan / zoom-to-cursor / follow changes
  the render origin.
- **Time contract**: `RobotModelLayer::timeRange()` returns the inverted
  sentinel `{Timepoint::max(), Timepoint::min()}` — a static decoration must
  neither widen the playback timeline (`{0, INT64_MAX}` would balloon it to
  2262) nor clamp the playhead to its latch timestamp. The dock skips
  inverted ranges and still delivers live tracker time.

## Streaming / live-data path

TF, pointclouds, and markers ingest live as well as from a file. The application
activates `TransformService`'s session-wide `FrameTransforms` ingest tap before
constructing any runtime host. After scalar parsing, the source host invokes that
tap synchronously with the payload view it already owns. The callback resolves the
topic's current `SessionManager` parser binding, decodes under `parseLocked`, folds
validated edges into the dataset's thread-safe `TransformBuffer`, and returns before
the source payload anchor can expire. This is the normal file and streaming TF path.

Progressive file-load flushes emit `datasetTransformsReady` so docks re-read frame
hierarchies at the current playhead; the flush does not decode TF. Streaming keeps
the existing `samplesIngested` connection:

- `Scene3DDockWidget::setSessionManager` shadows the base to also call
  `reconnectLiveSamples`, which connects `SessionManager::samplesIngested`
  (fired on the UI thread after each retention trim, `live == true` only while
  following a live stream; file load emits `live == false` and is served by the
  loader-driven `datasetTransformsReady` publication described above).
- On each live tick, `TransformService::ingestNewTransforms` is a no-op while the
  tap is active. `recomputeOrphanStates(force=false)` observes the buffer revision
  written by the worker.
- `driveVisibleLayersToLiveEdge` then queries each visible layer's `timeRange()`
  for the newest timestamp now in the `ObjectStore` and drives `setTrackerTime`
  on every visible layer (consulting the *layer's* range, not the store's per
  base-topic range, so a multi-topic layer like OccupancyGrid + its `_updates`
  sibling does not freeze at its last keyframe). Without this, object layers
  would stall while only the TF buffer advanced.

`TransformService` is a `widgets/`-level wrapper over the Qt/GL-free core
`TransformBuffer`. It owns no parser handle: each tap or rebuild resolves the
topic's binding through `SessionManager::parserBindingForObjectTopic` and decodes
under `parseLocked` (per use, never cached). Streaming uses a finite cache window
(default 10 s); file buffers keep the full history.

An in-place file reload calls `beginReplacingLoad` before its worker starts, so
tap writes route to a private staging buffer while docks retain the active one.
`commitReplacingLoad` atomically swaps the staging pointer into the active slot,
then emits `datasetTransformsReady`; docks rebind their view and TF-backed layers.
`abortReplacingLoad` drops staging, so rollback performs no ObjectStore reads.

`ingestFrameTransformsForDataset` remains an explicit full-store rebuild. Dataset
merge invalidates the affected buffers and calls it for the anchor because merged
entries carry `payload_stamp_shift`, which only the durable store entry records.
This cold closure drain is reserved for explicit rebuilds; per-flush and
streaming paths never call it while the tap is active.

### Object decode: parser-decoded vs canonical blob

Every object consumer (the layers above + `TransformService`) decodes a store
entry through one seam, `pj_scene3d_widgets/resolve_object.h::resolveObject`,
which picks the path by whether the topic has a bound `MessageParser`:

- **Parser bound** — file / streaming data sources whose objects arrive as
  parser-decoded messages (e.g. `parser_ros`, `parser_protobuf`): decode via the
  topic's parser under `parseLocked` (per-use binding, never cached — the
  reload-UAF rule).
- **No parser bound** — a data-source / toolbox that pushed an *already
  serialized canonical* object (e.g. the Mosaico cloud toolbox, which packages
  the server's `point_cloud2` / `pose` / `motion_state` ontologies into
  `sdk::PointCloud` / `PosesInFrame`): the bytes are a pj_base
  wire blob, so `resolveObject` deserializes them with the canonical codec
  selected by the topic's `builtin_object_type`. This mirrors pj_scene2D's
  `canonical = (binding.parser == nullptr)` discipline.

Either way the **host** performs the decode — the producing plugin never does
(the "canonical-objects-in" boundary; the canonical codec is the canonical
object's own serialization, not a transport/CDR wire format). A layer's
`attach()` therefore accepts a topic that has *either* a parser *or* a canonical
codec for its `builtin_object_type` (`hasCanonical3DCodec`); it rejects only a
topic that has neither.

### SceneEntities lifetime expiry & the decoded-batch cache

`SceneEntitiesLayer` replays every batch up to the tracker time into an id-keyed
entity map (replace-by-id, deletions, lifetime expiry). Two mechanisms support this
on the live path:

- **Dual-clock lifetime anchor.** A `SceneEntity` carries an embedded `timestamp`
  (sensor epoch) and an optional `lifetime_ns`. Under streaming the ObjectStore entry
  is *host*-stamped (tracker clock), a different epoch from the sensor timestamp.
  Expiry must therefore compare against the ingest timestamp, not `entity.timestamp` —
  otherwise every finite-lifetime entity expires the instant it is folded (the bug that
  hid the streamed car mesh). The layer keeps a parallel map `entity_expiry_anchor_ns_`
  (keyed identically to `entities_`) holding each entity's ingest timestamp;
  `expiredAt()` tests `anchor + lifetime < tracker` (overflow-safe; `lifetime == 0` ⇒
  never). The single erase choke-point `eraseEntity()` drops the entity and its anchor
  in lockstep, so the two maps never drift. Deletion gating is intentionally *not*
  re-anchored: deletions carry sensor-epoch timestamps, so
  `deletion.timestamp <= entity.timestamp` stays on the entity clock.
- **Decoded-batch snapshot cache.** Backward scrubs and jumps rebuild the entity map
  from scratch; re-parsing heavy embedded glTF every time hitched the scrub.
  `snapshot_cache_` (keyed by `SequentialUID`, byte-budgeted at `kSnapshotCacheMaxBytes`,
  oldest-UID eviction) holds decoded batches so a rebuild re-folds without re-parsing.
  Each cache entry records `store_ns` (the ingest timestamp) alongside the batch, so a
  cache-hit re-fold restores the *same* lifetime anchor a fresh parse would have produced.
- **Timestamp-ordered, out-of-order-safe fold.** The batches to fold are the topic's
  entries with `ts <= tracker`, taken via `ObjectStore::rangeByTime` (a decode-free,
  ascending, eviction-safe window) — never an arrival-order UID walk, whose order
  diverges from timestamp order after an out-of-order push and would both leak a
  future batch in and drop a late in-window one. Forward playback folds only the
  `(state_built_at_, tracker]` delta; a retroactive push landing at
  `ts <= state_built_at_` is detected when `maxUidAtOrBefore(state_built_at_)` grows
  past the folded high-water (`applied_uid_high_`) and forces a full rebuild. This is
  the same cursor discipline `OccupancyGridLayer` uses, so all three ObjectStore
  UID-cursor consumers (`TransformService`, occupancy, scene entities) share one
  out-of-order contract.

## Asset resolution (`package://` for a non-ROS app)

`UrdfPackageResolver` — URI scheme dispatch first:
`file://` → absolute path; bare path → relative to the URDF's directory
(never enters the package chain); `http(s)://` → allowed only for URL-source
layers; `package://pkg/rel` → the chain below, stop at first hit:

0. **In-band embedded-asset lookup** — exact-name match against the embedded
   asset dictionary (`stepEmbeddedAsset`). The dataset source plugin (not the
   host) is what may carry such assets — e.g. files embedded in the container —
   surfaced format-neutrally via `setEmbeddedAssets`. Built and unit-tested, but
   **not yet connected to the app load path**: `MainWindow::onFileLoaded` does
   not yet surface the extracted asset map, so `setEmbeddedAssets` stays empty in
   production and this step is currently inert. (Steps 1+ — the per-source
   remembered map and auto-seeded search roots — *are* wired: `onFileLoaded` and
   `makeSceneDock` feed `setSourcePath` the loaded source path.)
1. **Remembered per-source mapping** — QSettings
   `pj_scene3d/urdf_per_source_packages`, keyed by the dataset's source path.
2. **Ancestor heuristic** — walk up from the URDF's dir (≤10 levels) looking
   for a directory whose basename == `pkg`, accepting only if
   `candidate/rel` exists (prevents wrong-folder false positives). URL
   sources use the URL-segment variant.
3. **Search roots** — ordered list auto-seeded from the URDF dir, the source
   file's dir, `$ROS_PACKAGE_PATH`, and `$AMENT_PREFIX_PATH`/`$COLCON_PREFIX_PATH`
   (+`/share`), all via `qEnvironmentVariable` — zero ROS dependency.
   Package identity is directory basename only; no `package.xml` check.
4. **Ask once** — unresolved packages surface on the layer's status text;
   a chosen root is stored per-source and globally, then pending meshes retry.

Failure semantics: never silent. Missing meshes → magenta cubes + status
counts; wrong folder picked → explicit "expected a subdirectory named <pkg>"
error; URDF latch not yet received → an indefinite 500 ms-throttled re-check
with a permanent "Waiting for …" status (no timeout escalation).

## Scene-controls panel (pj_app)

`Scene3DConfigPanel` (right side-panel) drives scene-wide state, persisted in
QSettings `pj_scene3d/scene_controls/*` and re-applied to every dock it binds:
grid style/size/divisions/visibility (`GridRenderPass` rebuilds geometry
lazily at render time; style toggles between a plain line grid and a full
two-tone **checkerboard** with the grid lines overlaid — all three colors
auto-derived from the active theme, no user color controls), TF-frame
size/opacity/visibility, mesh/collision
opacity/visibility, and the **Model/URDF** row — a File/Topic/URL source
combo plus an add button; each added robot gets a row with a remove button.
Robot layers are panel-managed: filtered out of the Topics list (the
per-layer config widget — frame prefix, color override, COLLADA up_axis,
resolution override — still exists on the layer but is not reachable from
the panel; resurrect behind an "advanced" disclosure if missed). The
phases-0B/D/B look knobs are runtime APIs with baked defaults — no app UI;
the `scene3d_mesh_viewer` demo exposes them for look-dev.
