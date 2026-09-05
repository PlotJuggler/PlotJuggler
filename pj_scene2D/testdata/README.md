<!-- SPDX-License-Identifier: MPL-2.0 -->

# `pj_scene2D` test fixtures

Media fixtures for the `pj_scene2D` suites. Tests resolve these paths relative
to the repository root (the test targets are declared `RUN_IN_SOURCE_DIR`).

Every suite that needs a fixture **skips** when it is absent. A skip is not a
pass: `ctest` reports the run green either way, so check the skip list before
trusting a clean-clone result.

## Tracked — present in a fresh clone

| File | Used by |
|---|---|
| `test_hevc.mp4` | `streaming_video_decoder_test` (HEVC path) |
| `test_av1.mp4` | `streaming_video_decoder_test` (AV1 path) |
| `test_h264_bframes.mp4` | `streaming_video_decoder_test` (B-frame presentation order) |
| `test_h264_bframes_deep.mp4` | `streaming_video_decoder_test` (deep reorder, 3 B-frames @60fps) |

## Not tracked — these suites do NOT run on a clean clone

| File | Suites that skip without it |
|---|---|
| `test_480p.mp4` | `ffmpeg_decoder_test`, `h264_utils_test`, `streaming_video_source_test`, `streaming_video_decoder_test`, `streaming_video_consumer_unwrap_test` |
| `test_1080p_bframes.mp4` | `streaming_video_decoder_test` (4 tests) |
| `test_images.mcap` | `mcap_integration_test` |
| `potato.mcap` | `dual_store_test` |

These are absent because they were never committed, not because they are
optional. Until they are, that coverage does not exist outside the machine of
whoever holds the files.

### Regenerating

PJ4's own FFmpeg is decoder-only — use a system `ffmpeg` with the encoders.

`test_480p.mp4` — a short 480p H.264 clip; the suites need only a decodable
stream with regular keyframes:

```bash
ffmpeg -f lavfi -i testsrc=size=854x480:rate=30:duration=4 -c:v libx264 \
  -g 15 -keyint_min 15 -pix_fmt yuv420p -an pj_scene2D/testdata/test_480p.mp4
```

`test_1080p_bframes.mp4` — 1080p with B-frames, for presentation-order and
throughput checks:

```bash
ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 -c:v libx264 \
  -bf 3 -g 30 -keyint_min 30 -pix_fmt yuv420p -an \
  pj_scene2D/testdata/test_1080p_bframes.mp4
```

`test_images.mcap` and `potato.mcap` — MCAP recordings carrying an image topic
(and, for `potato.mcap`, scalar topics alongside it, so `dual_store_test` can
populate both the ObjectStore and the DataEngine from one file). No recipe is
recorded here because the originals were never committed; regenerate with
`pj_datastore`'s MCAP writer or any recorder that emits
`foxglove.CompressedImage` / `sensor_msgs/Image`, then confirm the suite passes
rather than skips.

### For reference — recipes for the tracked fixtures

```bash
# test_hevc.mp4 (needs libx265)
ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=2 -c:v libx265 \
  -x265-params keyint=15:min-keyint=15:no-scenecut=1 -pix_fmt yuv420p \
  -tag:v hvc1 -an pj_scene2D/testdata/test_hevc.mp4

# test_av1.mp4 (needs libaom-av1)
ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=2 -c:v libaom-av1 \
  -usage realtime -cpu-used 8 -g 15 -keyint_min 15 -pix_fmt yuv420p \
  -tag:v av01 -an pj_scene2D/testdata/test_av1.mp4

# test_h264_bframes_deep.mp4 (needs GStreamer)
gst-launch-1.0 videotestsrc num-buffers=120 pattern=ball ! \
  video/x-raw,width=128,height=128,framerate=60/1 ! \
  x264enc bframes=3 b-adapt=false key-int-max=30 speed-preset=veryfast \
  bitrate=64 ! h264parse ! mp4mux ! \
  filesink location=pj_scene2D/testdata/test_h264_bframes_deep.mp4
```
