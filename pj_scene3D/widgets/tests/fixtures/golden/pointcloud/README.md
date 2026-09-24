# Point-cloud golden images

Reference renders for `pointcloud_golden_test`
(`pj_scene3D/widgets/tests/pointcloud_golden_test.cpp`), captured from the
**OpenGL** Scene3D renderer on Mesa llvmpipe (the CI renderer). They guard
point-cloud rendering in the GL build and are the parity target for the QRhi
renderer (`docs/QRHI_MIGRATION.md`, P3).

Every scene: 256x256 view at DPR 1, light theme (white application palette),
grid/TF axes/TF lines off, default look (tonemap, SSAO, EDL, MSAA), orbit camera
focal (0,0,0), radius 4 m, azimuth 0.6 rad, elevation 0.6 rad, 45° fov. Clouds
are canonical `sdk::PointCloud` blobs in frame `map` (float32 xyz first, so the
zero-copy fast path applies unless noted). The bottom-right 80x80 px (the GL
axis gizmo) is excluded from comparisons.

| Scene | Content |
|---|---|
| `shape_points_large` | 8x8 wavy lattice, 12 px square points, solid colour |
| `shape_spheres_large` | 8x8 wavy lattice, 0.16 m spheres, solid colour |
| `shape_cubes_large` | 8x8 wavy lattice, 0.16 m cubes (hexagon fan), solid colour |
| `shape_points_1px` | 120x120 dense wavy lattice, 1 px points |
| `shape_spheres_small` | 120x120 dense lattice, 2 cm spheres (~1.5 px). Below ~1 px llvmpipe rounds sprites up while NVIDIA covers a sample fraction, so no golden can pin it |
| `shape_cubes_subpixel` | 120x120 dense lattice, 3 mm cubes (~0.3 px). The sprite LOD is off by default, so this is the fan; it becomes the LOD check once the LOD is on |
| `color_rgb_packed` / `color_rgba_packed` | 16x16 lattice, packed uint32 `rgb` / `rgba` field (RGB-direct fast path), 10 px points |
| `color_rgb_channels` | same colours as separate uint8 `red`/`green`/`blue` fields (CPU fallback) |
| `color_rgb_cubes` | packed `rgb` on 0.1 m cubes |
| `field_turbo` / `_viridis` / `_plasma` / `_grayscale` | 24x24 lattice, float32 `intensity` ramp along x, auto range, 8 px points |
| `field_turbo_spheres` / `field_viridis_cubes` | the float ramp on 0.07 m spheres / cubes |
| `field_axis_z` | 8x8 wavy lattice coloured by `z` (spatial-axis path), 0.16 m spheres |
| `field_outside_range_translucent` | float ramp, manual range [0.3, 0.7], outside-range opacity 0.25 |
| `field_outside_range_hidden` | same range, outside-range points hidden |
| `field_outside_range_cubes` | same as translucent, on 0.07 m cubes |
| `field_int8` … `field_uint32` | the ramp as int8, uint8, int16, uint16, int32, uint32 (zero-copy integer attributes), auto range, turbo |
| `depth_two_clouds` | red 0.14 m cube floor crossed by a tilted blue sphere plane: occlusion between two layers |
| `large_1m_points` | 1000x1000 (1 M) wavy lattice, 2 px points coloured by z; coverage and mean colour |

## Comparison

A pixel differs when any channel is off by more than 16. A scene fails when
more than 3% of pixels differ, when its non-background pixel count moves by more
than 5%, or when the mean colour of its covered pixels moves by more than 12 in
any channel. Calibration (llvmpipe goldens vs an NVIDIA GPU): every differing
pixel sits on a silhouette; worst case 1.8% differing, coverage within 1.3%,
mean colour within 9 (`shape_spheres_small`). A swapped colormap, shape or colour
mode moves the mean colour by 17-229.

Weaker checks, where pixels depend on the driver rather than on our rendering:
- `field_turbo_spheres` and `large_1m_points`: coverage and mean colour. Sphere
  impostor shading follows the sprite's rounded point size, which differs
  between Mesa releases (CI's 23.x differs from 26.x on ~10% of pixels).
- `shape_spheres_small` (~1.5 px spheres): coverage only. Their shading is
  driver-dependent (NVIDIA MSAA nearly erases them; Mesa 23 shifts their mean
  colour by ~80). Planned fix: draw spheres below ~2 px as flat discs (QRhi
  migration P3), then restore the full check.

On failure the test writes `<scene>_actual.png` and `<scene>_diff.png`
(magenta = differing) into `$PJ_POINTCLOUD_GOLDEN_OUT`, default the working
directory.

## Regenerating

Only from the default (OpenGL) build, on llvmpipe, and only after confirming a
change is intended:

```bash
cmake --build build --target pointcloud_golden_test
LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa PJ_UPDATE_POINTCLOUD_GOLDENS=1 \
  QT_QPA_PLATFORM=xcb build/pj_scene3D/widgets/pointcloud_golden_test
```

Needs a real X display (`xvfb-run -a` in CI); the `offscreen` platform skips.
The QRhi build (`PJ_SCENE3D_RHI=ON`) refuses to write goldens; run its binary
directly to measure parity.
