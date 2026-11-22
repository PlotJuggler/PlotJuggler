# Browser sample presets

The "try it with sample data" presets of the WebAssembly build. Packaging with
`node scripts/package_wasm.mjs --presets resources/wasm-presets ...` deploys this
directory verbatim under the page's `presets/`, where the welcome dialog finds
`index.json`. Each sample is an ordinary source-saving layout with its data
beside it, or on PlotJuggler's asset host (`https://assets.plotjuggler.io`, a
Cloudflare R2 bucket whose `v1/` files never change) when the data is too large
for the repository. How to author one, the packaging rules and the hosting notes are in
[`docs/WASM_PRESETS.md`](../../docs/WASM_PRESETS.md).

| Sample | Files |
|---|---|
| `builtin_filters/` | layout, thumbnail (recording hosted, see below) |
| `anymal/` | layout, thumbnail (recording and robot models hosted, see below) |
| `waymo/` | layout, thumbnail (recording hosted, see below) |

The layouts and this file are authored for this repository and fall
under its license (MPL-2.0), and so do the thumbnails: PlotJuggler renders of
the loaded samples (browser screenshots, downscaled to 384x216, 8-bit palette; the
ANYmal and Waymo ones show only a 3D view, the built-in filters one its
two plots).

## `builtin_filters/` — built-in filters

Three plots of one signal: a car's gyro yaw rate (`/nissan/gps/duro/imu`,
`angular_velocity/z`, about 99 Hz) as recorded, the same signal through the
built-in `moving_average` filter, and its built-in `integral`, the car's heading
change in radians (both in `resources/filters/builtin_filters.luau`), which the
layout's `<data_processors>` applies when it opens. The window is 100
samples (about 1 s), with **Center timestamp** on so the smoothed curve does not
lag the raw one by half a window (about 0.5 s). The gyro is quiet (about 0.0025
rad/s of sample-to-sample noise against turns of ±0.6 rad/s), so the window is
wide enough to show what a low-pass costs as well as what it removes: the jitter
goes, and the sharpest turns come out lower and broader. The integral starts at 0 and, as
a plain sum of a real gyro, drifts slowly. The layout names no filter source
(`<source_fallback>`): every PlotJuggler has the built-ins.

The layout loads `v1/nissan_leaf/nissan_leaf_drive.mcap` (2,532,014 bytes,
SHA-256 `cd16c23dfc5566764376c55fb39fc7afd5666139341fe1491ec743e3e028f154`,
pinned by the layout's `content_sha256`) from the asset host: the file
`nissan_zala_90_mixed_0.mcap` of
[JKK_DATASET_02](https://jkk-research.github.io/dataset/jkk_dataset_02/) by the
JKK Research Center, Széchenyi István University (SZE-JKK), a Nissan Leaf
driving for 3 min 52 s (51,822 messages: IMU, magnetometer, GNSS pose and
status, vehicle speed and steering). It is re-compressed with zstd
(`mcap compress`, 4 MiB chunks; the original's 14 chunks were uncompressed,
11.2 MB); the messages are unchanged. The layout reads only the IMU topic.

Redistribution basis: the dataset's authors provide it "for research and
educational purposes" (© SZE-JKK).

## `anymal/` — ANYmal GrandTour

The layout loads three hosted folders (each with its own license and provenance
notes; `https://assets.plotjuggler.io/index.html` lists every file):

- `v1/grand_tour_2/anymal_grand_tour.mcap` (72,501,497 bytes, SHA-256
  `9288ebb7caccdfe60bdeda3281bc198e2399f14cd58238c648a788ff1263bafe`, pinned by
  the layout's `content_sha256`): 25 s of the
  [GrandTour dataset](https://huggingface.co/datasets/leggedrobotics/grand_tour_dataset),
  mission `2024-11-11-14-29-44` ("ROOT-1", an ANYmal D walking a forest trail at
  about 1 m/s), from Unix time 1731332092.0 to 1731332117.0. It holds the three
  HDR cameras (JPEG, downscaled from 1920x1280 to 640x427, quality 65, with
  scaled `camera_info`), the Velodyne scans (Cloudini-compressed: 5 mm, ring and
  time removed, each scan stored ring by ring so neighbouring points compress
  together), the IMU, the leg odometry, the 12 joint states, and `/tf`: the
  body pose from the state estimator plus each leg joint by forward kinematics
  of the robot model below. The dataset publishes no `/tf`; the static frames are
  its sensor calibration. `README.txt` beside it, with the one in `v1/grand_tour/`
  (the same messages in their recorded point order), describes each topic and
  gives the citation the dataset asks for.
- `v1/anymal_d_simple_description/` (4.3 MB): `urdf/anymal.urdf` and the 15
  meshes and 14 textures it references, from
  [ANYbotics/anymal_d_simple_description](https://github.com/ANYbotics/anymal_d_simple_description)
  at commit `d5bf4dc3a81a`. The meshes are unmodified; the URDF drops the
  inspection payload the GrandTour robot does not carry and names the lidar link
  `velodyne_lidar`, the dataset's frame (`PROVENANCE.txt`).
- `v1/box_model/` (11.3 MB): the GrandTour sensor payload the robot carries,
  `urdf/box/box.urdf` and its 4 meshes, from
  [leggedrobotics/grand_tour_box](https://github.com/leggedrobotics/grand_tour_box)
  at commit `7c7b7884e06a`: the meshes verbatim, the URDF the xacro expansion
  shipped with the GrandTour example assets (`PROVENANCE.txt`). Its root link is
  the robot's `base`, so it rides on the ANYmal through the recording's `/tf`.

Redistribution basis: the GrandTour dataset is released under the MIT License
(its dataset card); the robot model is © 2023 ANYbotics AG under the BSD
3-Clause License, and the payload model © 2023-2025 ETH Zurich, Robotic Systems
Lab under the MIT License, each retained as `LICENSE` beside it. The recording was made with
a converter outside this repository from the dataset's Zarr streams and a
curated MCAP of the same mission; the topics, rates and processing above are
what it did.

## `waymo/` — self-driving car

The layout loads `v1/waymo/waymo_av_demo.mcap` (170,187,347 bytes, SHA-256
`59c8cf1b6bae8226f9d5eb3f79103b6678666b463e5aa8b456f03b14683f1927`, pinned by
the layout's `content_sha256`) from the asset host: 19.8 s of a
[Waymo Open Dataset](https://waymo.com/open) perception segment, a car driving
through Phoenix by day (the recording's `/stats` names `location_phx`), from Unix
time 1507939794.765751 to 1507939814.569083. It starts from Foxglove's Waymo
example recording (`example-016-av-waymo-ds.mcap`, 925 MB, Foxglove schemas over
protobuf) and holds:

- the five lidars (`/LIDAR_TOP`, `_FRONT`, `_REAR`, `_SIDE_LEFT`, `_SIDE_RIGHT`,
  10 Hz) at full resolution, about 144k points a scan from the top lidar and about
  4k from each of the others, converted to `sensor_msgs/msg/PointCloud2` and
  Cloudini-compressed (1 mm, intensity at 1, elongation removed); the demo's
  downsampled copies are dropped;
- the five cameras' downsampled JPEGs (480x320 front, 480x221 side, 10 Hz) and
  their segmentation images (5 Hz), re-encoded at quality 80 where that is smaller,
  with their `camera_info`;
- each camera's `foxglove.ImageAnnotations` overlays: the lidar points projected
  into it (`lidar`) and the 2D and 3D box outlines (`annotations_2d`,
  `annotations_3d`), points rounded to 1/4 pixel and colours to 1/256;
- the 3D boxes (`/markers/annotations/vehicles`, `pedestrians`, `signs`,
  `foxglove.SceneUpdate`), `/tf`, and `/stats` (`diagnostic_msgs/msg/DiagnosticArray`:
  speed, acceleration, wheel speed, and the segment's context). Its schema declares
  each status's `name` before its `level`, unlike ROS's definition; `parser_ros`
  1.4.2 and later read the fields in the schema's order, older ones fail every
  `/stats` message, and the browser refuses a sample whose plotted curves have no data;
- the car (`/marker/car`, an embedded binary glTF in a `foxglove.SceneUpdate`),
  kept once with its 5 s lifetime cleared so it lasts the whole recording (the
  demo republishes it every 5 s).

`README.txt` beside it lists the same changes and gives the attribution. The layout
shows the five cameras with their lidar and 3D-box overlays, the five segmentation
images, an aerial (top-down orthographic) and an orbiting 3D view of the lidars,
boxes and car, and the speed and acceleration from `/stats`. Both 3D views use
`base_link` as their fixed frame: with `map` as the fixed frame and `base_link`
followed, both came up empty on the desktop (not investigated). The aerial view's
camera elevation stays below the top-down limit (pi/2 - 0.05): a layout with a
larger one is rejected whole. The layout keeps the side panel at its 280 px minimum and gives the
aerial view 27 % of the rest: a Scene3D view is at least 320 px wide, and in a narrower dock
its right side is cut off, so in a 1280 px wide window the car sits about 30 px right of the
visible centre.

Redistribution basis: the Waymo Open Dataset is licensed by Waymo LLC under the
[Waymo Dataset License Agreement for Non-Commercial Use](https://waymo.com/open/terms),
which allows non-commercial use only and requires the attribution its section 1
gives. `README.txt` beside the recording carries that attribution: "This
recording was made using the Waymo Open Dataset, provided by Waymo LLC under the
Waymo Dataset License Agreement for Non-Commercial Use, available at
waymo.com/open/terms, and your access and use of such work are governed by the
terms and conditions therein." The recording was made with a converter outside this
repository from Foxglove's demo recording; the topics and processing above are what
it did.
