# pj_scene2D

The 2D media widget family: decoding and GPU display of images, video, depth maps and pixel-space annotation
overlays, synchronized with the global timeline. Two targets: `pj_scene2d_core` (Qt-free decoders, `MediaSource`
sources, keyframe indexing, compositing) and `pj_scene2d_widgets` (the `QRhiWidget` viewer and the
`Scene2DDockWidget` layer stack on `pj_scene_common`). How it works: [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md);
measured evidence: [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md); schemas: the SDK's
[`builtin_type.md`](https://github.com/PlotJuggler/plotjuggler_sdk/blob/main/docs/builtin_type.md).

## Invariants

- **Read-only consumer** of `ObjectStore`; at-or-before is the only time query, in `int64_t` ns on the dataset
  clock.
- **Frames reach the viewer only through `MediaSource::takeFrame()`**, never through Qt signals: queued frames
  cannot be withdrawn and stale ones reach the display during a scrub.
- **Video decode cancels only on a scrub** (`StreamingVideoSource`'s preempt predicate): cancelling on every
  playback tick delivers no frame at all.
- **The depth no-data rule (`isValidDepth`) equals the shader's `!(d > 0)`**, or the inspector disagrees with the
  display.
- **The view transform and its inverse** (`pixel_inspector.h`) change together, or the inspector reads the wrong
  pixel.
- **`setCameraInfoMap` runs before the first `setTimestamp()`**: the worker reads the map unlocked.
- **No draw references a resource whose `create()` failed** (D3D11 crashes in `beginPass`).
- **Every `QRhiWidget` is constructed parentless, then inserted**, or a raster window never switches to QRhi.
- **Every attribute a dock or layer saves is read in `xmlValidateState` through a reporting reader** (`readXml*` in
  `pj_runtime/XmlStateOptions.h`), the same parse `xmlLoadState` uses: one the validator never reads is missing from
  the MCP `widget_state` options. `ValidationDescribesTheSavedStateAndAgreesWithLoad` fails on it.
- **Editing a shader means re-baking its `.qsb`** with the pinned `qsb` (`widgets/shaders/bake.sh`) and committing
  both, or the WASM configure hash check fails.

## Key headers

Core (`core/include/pj_scene2d_core/`):

- `media_source.h`: the `setTimestamp` / `takeFrame` contract; `async_frame_worker.h`: the shared latest-wins worker.
- `image_pipeline_source.h`, `depth_pipeline_source.h`, `streaming_video_source.h`, `scene_pipeline_source.h`: the
  sources; `composite_media_source.h`: fuses them and pairs a derived topic's annotations with its image
  (`setLayerPairing`).
- `image_resolve.h`: `resolveImage`, store entry to `sdk::Image` plus its dataset-clock stamp.
- `streaming_video_decoder.h`, `ffmpeg_decoder.h`, `video_codec_utils.h`: GOP-aware decode, hardware backends,
  keyframe oracles.
- `codecs.h`, `codec_pipeline.h`: image codecs; `scene_decoder.h`: annotations and 2D scene entities.
- `undistort_remap.h`, `image_rectifier.h`: rectification maps (GPU LUT, CPU fallback).
- `video_color.h`: `buildYuvMatrix`; `depth_range.h`: depth validity and Auto-fit percentiles;
  `overlay_geometry.h`: stroke tessellation with the 1 px screen floor.

Widgets (`widgets/include/pj_scene2d_widgets/`):

- `media_viewer_widget.h`: the QRhi renderer; `pixel_inspector.h`: hover magnifier and the view-transform math.
- `Scene2DDockWidget.h`: the dock; `layers/`: `ImageLayer` (Rectify toggle), `DepthImageLayer` (colormap,
  Auto-fit), `VideoLayer`, `SceneDecoderLayer`.

Depth images arrive as `kImage` with a depth encoding; the dock routes them by the first sample's encoding.

## Tests

```bash
./test.sh -R 'pj_scene2d_core|scene2d_dock|PixelInspector|mcap_integration|dual_store|ffmpeg_decoder|media_viewer'
```

- `pj_scene2d_core_tests` and `pj_scene2d_core_source_dir_tests` (video fixtures, run from the repository root)
  register each case as `<runner>.<Suite>.<Case>`, one process per case. The dock and inspector tests are
  `scene2d_dock_widget_test`, `scene2d_dock_widget_dispatch_test` and `PixelInspector`.
- `mcap_integration_test` and `dual_store_test` are separate binaries (both define `MCAP_IMPLEMENTATION`);
  `ffmpeg_decoder_test` is separate for its `LSAN_OPTIONS`.
- `media_viewer_render_test` (pixel checks: colour matrices, depth colormap, rectification, overlays) is a QRhi
  display runner, one per backend; it skips under `test.sh`'s `offscreen` QPA and runs in the CI display lanes.
- `video_playback_bench_test` skips unless `PJ_BENCH_MP4` names a file.

## Pointers

- Derived topics (script outputs) reach this module as ordinary store topics; see
  [docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md), "The on-demand pipeline" and
  "Scene integration".
- Hard-won lessons go in `docs/TECHNICAL_NOTES.md` as evidence (observed, conditions, re-test).
