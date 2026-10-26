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
| `anymal/` | layout, thumbnail (recording and robot model hosted, see below) | 50,273 |
| `navigation/` | layout, recording, thumbnail | 936,437 |
| `signals/` | layout, CSV, thumbnail | 73,150 |

The layouts, the CSV and this file are authored for this repository and fall
under its license (MPL-2.0), and so do the thumbnails: PlotJuggler renders of
the loaded samples (browser screenshots, downscaled to 384x216, 8-bit palette).

## `anymal/` — ANYmal GrandTour

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 9,634 |
| `thumbnail.png` | 40,639 |

The layout loads two hosted folders (each with its own license and provenance
notes; `https://assets.plotjuggler.io/index.html` lists every file):

- `v1/grand_tour/anymal_grand_tour.mcap` (81,488,756 bytes, SHA-256
  `10df0dd12f8e933b37a95d8e943699a28b8131c71cdc7854f801ea108f432a4b`, pinned by
  the layout's `content_sha256`): 25 s of the
  [GrandTour dataset](https://huggingface.co/datasets/leggedrobotics/grand_tour_dataset),
  mission `2024-11-11-14-29-44` ("ROOT-1", an ANYmal D walking a forest trail at
  about 1 m/s), from Unix time 1731332092.0 to 1731332117.0. It holds the three
  HDR cameras (JPEG, downscaled from 1920x1280 to 640x427, quality 65, with
  scaled `camera_info`), the Velodyne scans (Cloudini-compressed: 5 mm, ring and
  time removed), the IMU, the leg odometry, the 12 joint states, and `/tf`: the
  body pose from the state estimator plus each leg joint by forward kinematics
  of the robot model below. The dataset publishes no `/tf`; the static frames are
  its sensor calibration. `README.txt` beside it describes each topic and gives
  the citation the dataset asks for.
- `v1/anymal_d_simple_description/` (4.3 MB): `urdf/anymal.urdf` and the 15
  meshes and 14 textures it references, from
  [ANYbotics/anymal_d_simple_description](https://github.com/ANYbotics/anymal_d_simple_description)
  at commit `d5bf4dc3a81a`. The meshes are unmodified; the URDF drops the
  inspection payload the GrandTour robot does not carry and names the lidar link
  `velodyne_lidar`, the dataset's frame (`PROVENANCE.txt`).

Redistribution basis: the GrandTour dataset is released under the MIT License
(its dataset card); the robot model is © 2023 ANYbotics AG under the BSD
3-Clause License, retained as `LICENSE` beside it. The recording was made with
a converter outside this repository from the dataset's Zarr streams and a
curated MCAP of the same mission; the topics, rates and processing above are
what it did.

## `navigation/` — mobile robot navigation

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 4,492 |
| `data.mcap` | 913,185 |
| `thumbnail.png` | 18,760 |

`data.mcap` is a subset of `amcl_test_bag.mcap` (28,199,408 bytes, SHA-256
`df90673603b3ac2237e0a2fdffcab72ab020317ebac54c977b4c57ab5f75f91b`), a 29.5 s
ROS 2 recording of a robot localizing with AMCL on an occupancy map, made with
the official MCAP CLI:

```bash
mcap filter amcl_test_bag.mcap -o navigation/data.mcap \
  -y '^(/tf|/tf_static|/map|/pose|/scan)$'
```

It keeps every message, with its timestamps, of `/tf`, `/tf_static`, `/map`
(`nav_msgs/OccupancyGrid`), `/pose` (`geometry_msgs/PoseWithCovarianceStamped`)
and `/scan` (`sensor_msgs/LaserScan`); `/particle_cloud` and `/particle_markers`
are dropped. The PlotJuggler project recorded `amcl_test_bag.mcap`, so the
recording and this subset fall under the repository's license (MPL-2.0).

Redistribution basis: **provenance unknown — redistribution basis must be
confirmed by the maintainer before publication.** The recording carries no MCAP
metadata or attachments, and its rosbag2 `metadata.yaml` names only topics and
timing.

## `signals/` — six waveforms

| File | Bytes |
|---|---|
| `layout.pj4.xml` | 3,007 |
| `data.csv` | 55,522 |
| `thumbnail.png` | 14,621 |

`data.csv` is generated: 1,001 rows at 50 Hz over 20 s, first column `time` in
seconds. It is reproduced byte for byte by:

```python
import math
fixed = lambda value: "%.6f" % (round(value, 6) + 0.0)
rows = ["time,sine,cosine,square,sawtooth,chirp,damped"]
for row in range(1001):
    seconds = row / 50.0
    rows.append(",".join([
        "%.2f" % seconds, fixed(math.sin(math.pi * seconds / 2)), fixed(math.cos(math.pi * seconds / 2)),
        "1" if (row % 200) < 100 else "-1", fixed((row % 250) / 125.0 - 1),
        fixed(math.sin(2 * math.pi * (0.05 * seconds + 0.02 * seconds * seconds))),
        fixed(math.exp(-0.15 * seconds) * math.cos(1.6 * math.pi * seconds))]))
open("data.csv", "w").write("\n".join(rows) + "\n")
```
