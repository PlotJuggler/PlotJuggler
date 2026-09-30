# Scene3D golden images

Reference renders for `pointcloud_golden_test`
(`pj_scene3D/widgets/tests/pointcloud_golden_test.cpp`), written by QRhi OpenGL
on Mesa 26 llvmpipe (see "Regenerating"). They hold the QRhi renderer to the
same images on every backend of the platform: ctest
runs the cases once per backend, `pointcloud_golden_test.<backend>.*`
(`RHI_BACKENDS` in `cmake/PjTests.cmake`). Why the backends differ, and the
measured worst case per backend, are in `pj_scene3D/docs/TECHNICAL_NOTES.md` §7.

Every scene: 256x256 view at DPR 1, light theme (white application palette),
grid/TF axes/TF lines off, default look (tonemap, SSAO, EDL, MSAA), orbit camera
focal (0,0,0), radius 4 m, azimuth 0.6 rad, elevation 0.6 rad, 45° fov. Clouds
are canonical `sdk::PointCloud` blobs in frame `map` (float32 xyz first unless
noted). The bottom-right 80x80 px hold the corner axis gizmo, which the view
draws and every comparison includes.

| Scene | Content |
|---|---|
| `shape_points_large` | 8x8 wavy lattice, 12 px square points, solid colour |
| `shape_spheres_large` | 8x8 wavy lattice, 0.16 m spheres, solid colour |
| `shape_cubes_large` | 8x8 wavy lattice, 0.16 m cubes (hexagon fan), solid colour |
| `shape_points_1px` | 120x120 dense wavy lattice, 1 px points |
| `shape_spheres_small` | 120x120 dense lattice, 2 cm spheres (~1.5 px): below 2 px spheres draw as flat discs at the disc-mean shade. Coverage and the coarse spatial checks, no colour: the discs' rasterized size still varies by driver (Mesa 23 draws them smaller than Mesa 26) |
| `shape_cubes_subpixel` | 120x120 dense lattice, 3 mm cubes (~0.3 px): the whole-cloud LOD draws them as 1 px sprites |
| `color_rgb_packed` / `color_rgba_packed` | 16x16 lattice, packed uint32 `rgb` / `rgba` field (RGB-direct), 10 px points |
| `color_rgb_channels` | same colours as separate uint8 `red`/`green`/`blue` fields |
| `color_rgb_cubes` | packed `rgb` on 0.1 m cubes |
| `field_turbo` / `_viridis` / `_plasma` / `_grayscale` | 24x24 lattice, float32 `intensity` ramp along x, auto range, 8 px points |
| `field_turbo_spheres` / `field_viridis_cubes` | the float ramp on 0.07 m spheres / cubes |
| `field_axis_z` | 8x8 wavy lattice coloured by `z` (spatial-axis path), 0.16 m spheres |
| `field_outside_range_translucent` | float ramp, manual range [0.3, 0.7], outside-range opacity 0.25 |
| `field_outside_range_hidden` | same range, outside-range points hidden |
| `field_outside_range_cubes` | same as translucent, on 0.07 m cubes |
| `field_int8` … `field_uint32` | the ramp as int8, uint8, int16, uint16, int32, uint32, auto range, turbo |
| `depth_two_clouds` | red 0.14 m cube floor crossed by a tilted blue sphere plane: occlusion between two layers |
| `large_1m_points` | 1000x1000 (1 M) wavy lattice, 2 px points coloured by z; coverage, mean colour and the coarse spatial checks |

The scene corpus (`scene_*`: suites `GridGolden`, `MarkerGolden`, `SceneGolden`)
guards the rest of the scene against the golden look, with a lower camera (radius
6 m, elevation 0.45 rad) so the far grid stays below the top-left corner:

| Scene | Content |
|---|---|
| `scene_grid_lines_light` / `_dark` | the ground grid alone, line style, default 10 m / 10 divisions, light and dark (Window 0x202020) theme: colours from the theme; coverage, mean colour and the coarse spatial checks (1 px lines are all edge) |
| `scene_grid_filled_light` / `_dark` | filled cells with the analytically drawn lines, default 10 m / 10 divisions |
| `scene_grid_filled_odd` | the light filled grid with 5 divisions: its cells start half a cell off the origin, where the lines must follow them |
| `scene_markers_default` / `_opaque` | SceneEntities: three rotated cubes (fill + edge overlay) and two line strips, at the layer default opacity 0.6 and at 1.0 |
| `scene_robot_primitives` | a URDF of box, cylinder and sphere without material plus an STL cube link, lit, line grid on |

## Comparison

A pixel differs when it is off its golden pixel by more than 16 in some
channel. A scene fails when more than 3% of pixels differ, when its
non-background pixel count moves by more than 5%, or when the mean colour of
its covered pixels moves by more than 12 in any channel. Calibration (llvmpipe
goldens vs an NVIDIA GPU): every differing pixel sits on a silhouette; worst
case 1.8% differing per pixel, coverage within 1.3%, mean colour within 9. A
swapped colormap, shape or colour mode moves the mean colour by 17-229.

**Exact on the writer's stack.** The writer records its device string in
`writer.txt` (QRhi's OpenGL device: llvmpipe's LLVM version and vector width and
the Mesa version, e.g. `Mesa llvmpipe (LLVM 21.1.8, 256 bits) 4.5 (Compatibility
Profile) Mesa 26.0.3-1ubuntu1`). A run on that very stack reproduces the goldens
bit for bit, so there every pixel must match and none of the tolerances below
apply. Any other stack, CI's older Mesa llvmpipe included, is held to the
tolerances.

The per-pixel rule is relaxed: a pixel differs when every golden pixel in its
3x3 neighbourhood is off by more than 16, so edge and sub-pixel coverage that moved by less than a pixel does
not count, and the 3% limit applies to that count. This is what lets the
Vulkan backend (lavapipe) share the OpenGL-made goldens: its framebuffer is y-down where
llvmpipe's GL target is y-up, so the same 4x MSAA sample pattern lands mirrored
on the scene. Diagonal silhouettes, cube edge bands and 1 px points then
resolve differently (up to 8% of pixels per pixel, at most 2% with the rule)
while every surface interior is identical. So that the neighbourhood allowance
cannot hide a gross regression such as a whole-scene one-pixel shift, a scene
also fails when more than max(3 x 3%, 10%) = 10% of its pixels
are off their own golden pixel (the log's "per-pixel" value); the worst
legitimate value measured is 8% (Vulkan on lavapipe, `shape_cubes_subpixel`).

Every fraction above is relative to the whole 256x256 frame, where a small
cloud is a small part: a scene whose foreground covers 10% of the frame could
differ on 30% of it and still pass. So two foreground checks are added to the
full (per-pixel) scenes:

- **Foreground diff**: the differing pixels as a fraction of the foreground,
  the pixels that are non-background (off the backdrop by more than 12) in the
  golden or in the actual image. Limit 0.12. It counts with the 3x3
  neighbourhood rule because its job is colour inside surfaces, not
  silhouettes: the frame-relative checks above already bound silhouettes, and
  sphere impostor silhouettes move
  by a pixel between Mesa releases. CI's Mesa 23 against the Mesa 26 goldens
  differs per pixel on 13% (`shape_spheres_large`) and 17% (`field_axis_z`) of
  the foreground, almost all of it on sphere rims and off by more than 128, so
  no colour tolerance separates it from a real change; with the rule it is 3%.
  Measured worst with the rule on Mesa 26: 0 on llvmpipe (the writer), 0.001
  on Iris GL, 0.082 on lavapipe and Iris Vulkan (`field_viridis_cubes`); about
  0.086 on D3D11/WARP and 0.028 on CI's Mesa 23, not re-run against the current
  images. A 5% shift of `field_outside_range_hidden`'s colormap
  range (`range_min` 0.28 or `range_max` 0.72) scores 0.24-0.27 while every
  frame-relative check passes.
- **Background strays**: non-background pixels in the actual image farther than
  2 px from the golden's foreground. Limit 4 pixels, an absolute count; 0 on
  every scene measured.

The foreground fraction of each scene is about 10-24% of the frame (the corner
gizmo included). The `[golden]` log line prints both values next to their limits.

Weaker checks, where pixels depend on the driver rather than on our rendering:
`field_turbo_spheres`, `large_1m_points` and the two line grids: coverage and
mean colour; `shape_spheres_small`: coverage. Neither sees position, so both
modes add coarse spatial checks, with background strays counted outside a 4 px
margin instead of 2:

- **Tiles**: the covered pixels per 16x16 px tile, sum of |actual - golden|
  over the golden's covered count; limit the coverage limit + 0.10.
- **Tile colour** (coverage and mean colour only): the golden-weighted mean over
  tiles both images cover of the tile's largest per-channel mean-colour change;
  limit 20. A colormap reversed in place keeps the frame's mean colour but not
  this.
- **Centroid** of the covered pixels, limit 2 px (6 px on the line grids), and
  each **bounding-box edge**, limit 4 px.

Measured on Mesa 26 (llvmpipe, lavapipe, Iris GL and Vulkan): tiles <= 0.047,
tile colour <= 4 (9 on the uncoloured `shape_spheres_small`), centroid <= 0.06
on the clouds and 2.0 on the line grids of the y-down backends, bounding box
0, strays 0. `GoldenComparator.CoverageModesRejectMovedForeground` checks
that a translated or rearranged synthetic foreground with the same pixel count
and mean colour fails these modes. The line grids are all 1 px edges, which lavapipe's mirrored MSAA
pattern resolves differently on up to 15% of their foreground. Sphere
impostor shading follows the sprite's rounded point size, which differs between
Mesa releases (CI's 23.x differs from 26.x on ~10% of pixels).

Per-backend allowances, each measured:

- **Line grids:** coverage within 8%, 25% on D3D11 (WARP). Each backend
  rasterizes its own share of 1 px lines (of the golden's pixels: Iris GL 1.002,
  lavapipe 0.989, Iris Vulkan 0.990; Metal 0.937 and WARP 1.194, not re-run
  against the current images).
- **D3D11 (WARP), scenes with 1 px lines** (grid, marker edges, line strips):
  coverage within 12% and a foreground diff up to 0.2. WARP rasterizes the lines
  wider than Mesa (coverage up to 1.094, foreground diff up to 0.153); fills
  match. OpenGL, Vulkan and Metal stay within the defaults.
- **Apple's software OpenGL** (the macOS CI fallback) does not run the corpus: its
  sphere imposters come out ~10% larger and its point sprites soft-edged.

On failure (or with `PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1`) the test writes `<scene>_actual.png` and `<scene>_diff.png`
(magenta = differing) into `$PJ_POINTCLOUD_GOLDEN_OUT`, default the working
directory.

## Regenerating

`PJ_UPDATE_POINTCLOUD_GOLDENS=1` makes every case write its render as the golden
instead of comparing (the case reports `SKIPPED ... golden written`). The writer is
pinned: QRhi OpenGL on Mesa llvmpipe. On any other backend or driver the case
fails with the backend and device it found and writes nothing, so references
never come from a hardware driver or a backend whose rasterization the
tolerances only absorb on the comparison side. `PJ_POINTCLOUD_GOLDEN_DIR`
overrides the golden directory for reading and writing: write into a scratch
copy first, compare it with the committed images and review every difference
(and re-run the other backends against it) before replacing them, together with
the `writer.txt` the run wrote, in a commit of its own. Never regenerate merely
to make a change pass.

```bash
env -u DISPLAY <private Wayland compositor env> QT_QPA_PLATFORM=wayland \
  PJ_GRAPHICS_API=opengl LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa \
  __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  PJ_UPDATE_POINTCLOUD_GOLDENS=1 PJ_POINTCLOUD_GOLDEN_DIR=/path/to/scratch \
  build/pj_scene3D/widgets/pointcloud_golden_test
```

The Mesa EGL/GLX vendor variables keep a machine with an NVIDIA driver on Mesa.
Needs a real display (`xvfb-run -a` in CI, or a private headless compositor);
the `offscreen` platform skips. The writer is deterministic: two runs on Mesa
26.0.3 produce identical files, equal to the images the comparison renders
(which the exact rule on its stack holds it to).

The images keep the look of the retired GL renderer. Against
its renders of the same scenes every scene passes (the filled grids aside: that
renderer drew the grid darker under the gizmo's square, so they are compared
only with the QRhi view's own renders), with at most 0.48% of pixels
off per pixel (`scene_robot_primitives`), coverage within 0.02% and mean colour
within 1. The corner gizmo covers the same pixels, its arms' interiors within 2
levels on average; its anti-aliased edges resolve differently (60-140 pixels of
the 80x80 corner off by more than 16, at most 80; about 250 where grid lines
cross the corner, which the GL renderer drew differently under the gizmo's
square). Outside the gizmo,
most pixels that differ do so by at most 3 per channel; more than 16 only on
sphere rims in `field_turbo_spheres` (16 pixels, a coverage scene), primitive
silhouettes in `scene_robot_primitives` (74 pixels, up to 61) and 1-2 edge
pixels of the markers, the colour scenes and `shape_points_1px`.
