# Browser sample presets

The "try it with sample data" presets of the WebAssembly build. Packaging with
`node scripts/package_wasm.mjs --presets resources/wasm-presets ...` deploys this
directory verbatim under the page's `presets/`, where the welcome dialog finds
`index.json`. The index is all the package carries: each sample's layout and
thumbnail live on PlotJuggler's asset host (`https://assets.plotjuggler.io`, a
Cloudflare R2 bucket whose `v1/` files never change) beside the recording they
open, and `index.json` names them by absolute URL; a layout names its recording
and robot models by absolute URL too. Changing a sample means uploading its
layout or thumbnail under a new name, pointing `index.json` at it and listing it
in `tests/wasm/fixtures/hosted_assets.json`. How to author one, the packaging
rules and the hosting notes are in [`docs/WASM_PRESETS.md`](../../docs/WASM_PRESETS.md).

| Sample | Layout and thumbnail on the asset host |
|---|---|
| `builtin_filters` | `v1/nissan_leaf/builtin_filters_layout.pj4.xml`, `builtin_filters_thumbnail.png` |
| `anymal` | `v1/grand_tour_2/anymal_layout.pj4.xml`, `anymal_thumbnail.png` |
| `pandaset` | `v1/pandaset/pandaset_layout_v4.pj4.xml`, `pandaset_thumbnail.png` |
| `lilocbench` | `v1/lilocbench/lilocbench_layout.pj4.xml`, `lilocbench_thumbnail.png` |
| `uzhfpv` | `v1/uzhfpv/uzhfpv_layout.pj4.xml`, `uzhfpv_thumbnail.png` |

The layouts, the thumbnails and this file are PlotJuggler's own and fall under
this repository's license (MPL-2.0); the thumbnails are PlotJuggler renders of
the loaded samples (browser screenshots, downscaled to 384x216, 8-bit palette; the
ANYmal and PandaSet ones show only a 3D view, the built-in filters one its
two plots; the LILocBench one is a desktop render of its orbit view).

## `builtin_filters` — built-in filters

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

## `anymal` — ANYmal GrandTour

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

## `pandaset` — self-driving car

The layout loads `v1/pandaset/pandaset_092-094_20s_v3.mcap` (161,649,466 bytes, SHA-256
`3579c1361ae5cd86c4f0abd2d684c2870dce2eea00cb9304ebc8352bbae24064`, pinned by
the layout's `content_sha256`) from the asset host: 20 s of
[PandaSet](https://pandaset.org), sequences 092 (frames 40-79), 093 and 094, which
PandaSet recorded back to back on a suburban arterial in Silicon Valley (GPS about
37.372 N, 122.05 W), by day, at 60-70 km/h and braking to 27 km/h at the end; Unix
time 1557593251.900 to 1557593271.900. The sequences join at 1557593256.000 and
1557593264.000, with their original timestamps. Each sequence has its own world
origin: `map` is 092's, and 093 and 094 are chained onto it by extrapolating the
previous sequence's last lidar step, so `map` to `base_link` has no jump at a join.
It was converted from the PandaSet release as mirrored, unofficially, on
[Hugging Face](https://huggingface.co/datasets/georghess/pandaset), and holds:

- `/lidar/pandar64` (10 Hz): every Pandar64 point in the sensor frame, x, y, z,
  intensity and `class_id` (the semantic class; 0 in 092 and 093, which have no
  segmentation), Cloudini-compressed (x, y, z on a 2 mm grid, intensity at 1,
  `class_id` lossless). The PandarGT lidar is not included;
- the six cameras (`/camera/<name>/image`: front_left, front, front_right, right,
  back, left; 10 Hz), downscaled from 1920x1080 to 800x450 and re-encoded as JPEG
  (quality 75), with their `camera_info` for that size, and `/camera/<name>/boxes`,
  the annotated cuboids projected into each image as outlines;
- `/boxes`: the annotated cuboids as `foxglove.SceneUpdate` in `base_link`,
  coloured by class group (vehicles orange, pedestrians blue, cyclists green, other
  grey), the ego car a magenta box, frame-locked to `base_link`;
- `/tf`: `map` to `base_link` per sweep, and every sensor (the lidar and the six
  cameras) as one fixed child of `base_link`. PandaSet gives each camera's pose in
  the world per image and no mounting: the converter derives it from the camera's
  pose relative to the car's pose interpolated to the image's timestamp (the
  positions agree within 0.6 to 3.3 cm per camera over the recording);
- `/ego` (JSON): speed from PandaSet's GPS velocity, heading and yaw rate from the
  lidar poses (within one sequence), and the GPS position; `/objects` (JSON): object
  counts per sweep, which step at the joins because each sequence was annotated on
  its own.

`README_v3.txt` beside it gives the attribution and the citation and lists the changes
made. The layout shows the six cameras across the top with their box overlays;
below them the yaw rate, the speed and an aerial (top-down orthographic) view
coloured by intensity in a column, beside a large orbiting 3D view coloured by
height; both 3D views draw the lidar and the boxes in `base_link`. Its MCAP Loader
does not select `/objects`.

Privacy: the images are not anonymised. PandaSet does not blur faces or licence
plates; in this recording the closest driver (sequence 094, front-right and right
cameras, about 4 m) is recognisable at 800x450, and plates are 6 m or farther and
not legible at that size.

Redistribution basis: PandaSet (c) Scale AI, Inc. and Hesai Photonics Technology
Co., Ltd, is licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
with the PandaSet Dataset Terms; the `README_v3.txt` beside the recording gives that
attribution, lists the changes made, as the licence requires, and states that the
recording is not endorsed by Scale AI or Hesai. The recording was made with a
converter outside this repository; the topics and processing above are what it did.

## `lilocbench` — office robot

The layout loads `v1/lilocbench/lilocbench_office_300s.mcap` (118,326,193 bytes, SHA-256
`d0f338250526b2a6277fa6f290b023ee9d13a12bf368acc60e6e58c41ff80f5c`, pinned by
the layout's `content_sha256`) from the asset host: 300 s of
[LILocBench](https://www.ipb.uni-bonn.de/html/projects/localization_benchmark/)
(Photogrammetry & Robotics Lab, University of Bonn), the first 300 s of its
"mapping" sequence, a Clearpath Dingo driving through the rooms of an office
floor in December 2024; Unix time 1733375659.713 to 1733375959.644, with the
original timestamps. It was converted from the sequence's camera-free bag, its
image archive, its ground-truth file and the benchmark's office map, and holds:

- `/scan_front`, `/scan_rear` (15 Hz) and `/odom` (14.5 Hz): both SICK laser
  scans and the wheel odometry, unchanged;
- `/camera/color/image` and `/camera/depth/image` (5 Hz): every third frame of
  the front RealSense D455, downscaled from 640x480 to 320x240, the colour as JPEG
  (quality 80), the depth as 16-bit PNG in millimetres, with their `camera_info`
  for that size. The left and right cameras are not included;
- `/ground_truth/pose` (20 Hz): the benchmark's ground-truth pose of `base_link`
  (from ceiling markers), x, y and yaw;
- `/tf`: `map` to `odom` computed from the ground truth and the odometry, so that
  `map` to `base_link` is the ground truth, `odom` to `base_link`, and the laser
  and camera frames. The localization shown is therefore the dataset's ground
  truth, not an algorithm's estimate;
- `/odom_in_map`: the wheel odometry expressed in `map` from the first pose, to
  show its drift against the ground truth;
- `/map`: the benchmark's occupancy map, cropped to the 31.6 m x 19.7 m it knows.

`README.txt` beside it gives the attribution and the citation and lists the changes
made. The layout shows the map from above (top-down orthographic) following the
robot, with both scans; the front camera; an orbit view that follows the robot too,
with the depth image as a cloud; the correction the ground truth applies to the
odometry over time; and an XY plot of the path by odometry against the ground truth.
Both follow cameras start on the robot's first position, because a follow camera
tracks the frame's movement and does not jump to it.

Privacy: one person, the robot's operator, is in view of the camera from 112.5 s to
118.5 s. His face is blurred in those 32 images; he is also seen from the shoulders
down from 65 s to 67 s. The frames were reviewed at one-second spacing for other
people. Whiteboards and posters are visible but not legible at 320x240.

Redistribution basis: the authors distribute LILocBench under the Creative Commons
Attribution-NonCommercial-ShareAlike license (their page names no version), so this
sample may be hosted only while its use is non-commercial, and the recording is
shared under the same license; the `README.txt` beside it gives the attribution,
lists the changes and states that it is not endorsed by the authors. The recording
was made with a converter outside this repository; the topics and processing above
are what it did.

## `uzhfpv` — racing drone

The layout loads `v1/uzhfpv/uzhfpv_indoor_forward_6.mcap` (37,381,188 bytes, SHA-256
`27501952fe2fd0b7de53b904ae94dc56ea1fa6f2fd19db46456933742a621740`, pinned by
the layout's `content_sha256`) from the asset host: 29.1 s of sequence `indoor_forward_6` of the
[UZH-FPV Drone Racing Dataset](https://fpv.ifi.uzh.ch/) (Robotics and Perception Group, University of
Zurich): a racing quadrotor flies about five laps of a gated track in a hangar, at up to
42 km/h; Unix time 1540821388.575 (0.5 s before take-off) to 1540821417.681 (the end of the ground truth).
It was converted from the dataset's text exports (`indoor_forward_6_snapdragon_with_gt.zip`,
`indoor_forward_6_davis_with_gt.zip`) and the Snapdragon calibration (`indoor_forward_calib_snapdragon.zip`),
and holds:

- `/snappy_cam/stereo_l/image` (27 Hz): the Snapdragon's left fisheye camera, 640x480 greyscale, re-encoded as
  JPEG (quality 80), with `camera_info` from the calibration (equidistant model). The right camera is not
  included;
- `/dvs/events_image` (at each fisheye frame): the DAVIS 346 event camera drawn as the events of the 10 ms before
  the frame, ON red and OFF blue on black (PNG). The DAVIS clock is moved onto the Snapdragon's by the offset
  between the dataset's two ground-truth files (35.0 ms). The raw event stream, the DAVIS image frames and its IMU
  are not included;
- `/snappy_imu` (500 Hz): the Snapdragon IMU;
- `/groundtruth/pose` (100 Hz, every fifth sample of the dataset's 500 Hz ground truth) and `/tf` (`world` to
  `body`, the Snapdragon IMU), with `/tf_static` `body` to `cam_left` from the calibration;
- `/groundtruth/speed_kmh`: the ground-truth speed in km/h, from central differences of the 500 Hz positions
  over a 50 ms moving average.

Timestamps are the dataset's own; the calibration's camera-to-IMU time shift (-16.7 ms) is not applied.
`README.txt` beside it gives the attribution and the citations and lists the changes made. The layout shows
the fisheye and the event camera above a 3D view in `world` with the drone's frame and its trail over the whole
flight, beside the track seen from above (an XY plot of the ground-truth position), the speed, the gyroscope and
the accelerometer.

Privacy: the images are not anonymised. People stand at the far edge of the hangar in some frames; at 640x480
their faces are a few pixels and not recognisable.

Redistribution basis: the UZH-FPV Drone Racing Dataset is released under
[CC BY-NC-SA 3.0](http://creativecommons.org/licenses/by-nc-sa/3.0/) ("free for non-commercial use (including
research)", [fpv.ifi.uzh.ch/datasets](https://fpv.ifi.uzh.ch/datasets/)); the recording is an adaptation shared
under the same licence, and the `README.txt` beside it gives the attribution, lists the changes made and
states that it is not endorsed by the University of Zurich. The recording was made with a converter outside this
repository; the topics and processing above are what it did.
