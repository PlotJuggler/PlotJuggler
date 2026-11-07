# Browser sample presets

The "try it with sample data" presets of the WebAssembly build. Packaging with
`node scripts/package_wasm.mjs --presets resources/wasm-presets ...` deploys this
directory verbatim under the page's `presets/`, where the welcome dialog finds
`index.json`. Each sample is an ordinary source-saving layout with its data
beside it, or on PlotJuggler's asset host (`https://assets.plotjuggler.io`, a
Cloudflare R2 bucket whose `v1/` files never change) when the data is too large
for the repository. How to author one, the packaging rules and the hosting notes are in
[`docs/WASM_PRESETS.md`](../../docs/WASM_PRESETS.md).

| Sample | Files | Bytes |
|---|---|---|
| `builtin_filters/` | layout, thumbnail (recording hosted, see below) | 15,010 |
| `anymal/` | layout, thumbnail (recording and robot models hosted, see below) | 32,273 |
| `city_drive/` | layout, thumbnail (recording and car model hosted, see below) | 54,455 |

The layouts and this file are authored for this repository and fall
under its license (MPL-2.0), and so do the thumbnails: PlotJuggler renders of
the loaded samples (browser screenshots, downscaled to 384x216, 8-bit palette; the
ANYmal and city-drive ones show only their 3D view, the built-in filters one its
two plots).

## `builtin_filters/` — built-in filters

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 4,144 |
| `thumbnail.png` | 10,866 |

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

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 10,104 |
| `thumbnail.png` | 22,169 |

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

## `city_drive/` — self-driving car

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 6,648 |
| `thumbnail.png` | 47,807 |

The layout loads `v1/boreas/boreas_city_drive.mcap` (261,292,684 bytes, SHA-256
`db4e548caa266c5408c266bed953610e82676016ae7f48c890af5b53116db82b`, pinned by
the layout's `content_sha256`) from the asset host: 30 s of the
[Boreas Autonomous Driving Dataset](https://registry.opendata.aws/boreas),
sequence `boreas-2021-07-27-14-43` (a sunny arterial road in Toronto), from Unix
time 1627411555.976352 to 1627411585.976352. It holds the front camera (the
dataset's rectified 2448x2048 PNGs as JPEG, quality 80, 10 Hz, with its
projection as `camera_info`), every Velodyne Alpha Prime scan (~10 Hz, ~199k
points; x, y, z and intensity, stored ring by ring and Cloudini-compressed at
1 cm), and the post-processed Applanix solution as `/tf` (map to applanix,
50 Hz), odometry (50 Hz, velocity in the vehicle frame), IMU (100 Hz) and fix
(10 Hz); `/tf_static` carries the dataset's lidar and camera calibration.
`README.txt` beside it describes each topic and the conversion, and gives the
citation the dataset asks for. The layout's 3D view starts centred on the car's
first position and follows it.

The car itself is `v2/boreas_vehicle/` (1.3 MB) on the asset host: `vehicle.urdf`
places a stand-in model at the recording's `applanix` frame (on the roof, about
2.1 m above the road by the lidar's ground returns), 0.4 m forward of centred,
where its hood edge meets the front camera image where the real hood ends; the
dataset ships no model of its Lincoln MKZ. The
mesh, `meshes/prius_hybrid.glb`, is Open Robotics' "Prius Hybrid" from
[Gazebo Fuel](https://fuel.gazebosim.org/1.0/OpenRobotics/models/Prius%20Hybrid)
(version 3, by Ian Chen) converted to binary glTF with its textures, scaled to
the MKZ's 4.9 m in the URDF; `PROVENANCE.txt` gives the source hashes and the
conversion.

Redistribution basis: the Boreas dataset is released under CC BY 4.0 (its
[AWS Open Data registry entry](https://registry.opendata.aws/boreas)); the
`README.txt` beside the recording gives the attribution and lists the changes
made, as the license requires. The car model is released under CC0 1.0
(`LICENSE` beside it). The recording was made with a converter outside
this repository from the dataset's files; the topics, rates and processing
above are what it did.
