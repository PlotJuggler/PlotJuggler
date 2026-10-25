# Browser sample presets

The "try it with sample data" presets of the WebAssembly build. Packaging with
`node scripts/package_wasm.mjs --presets resources/wasm-presets ...` deploys this
directory verbatim under the page's `presets/`, where the welcome dialog finds
`index.json`. Each sample is an ordinary source-saving layout with its data
beside it. How to author one, the packaging rules and the hosting notes are in
[`docs/WASM_PRESETS.md`](../../docs/WASM_PRESETS.md).

| Sample | Files | Bytes |
|---|---|---|
| `navigation/` | layout, recording, thumbnail | 936,437 |
| `signals/` | layout, CSV, thumbnail | 73,150 |

The layouts, the CSV and this file are authored for this repository and fall
under its license (MPL-2.0), and so do the thumbnails: PlotJuggler renders of
the loaded samples (browser screenshots, downscaled to 384x216, 8-bit palette).

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
