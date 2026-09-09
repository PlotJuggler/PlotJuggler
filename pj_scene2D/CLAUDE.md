# pj_scene2D

2D media/scene widget family: decoding and GPU display of images, video,
depth maps, and pixel-space annotation overlays, synchronized with the global
timeline. Ships as two CMake targets with a strict dependency direction:
`pj_scene2d_core` (Qt-free C++20 — decoders, `MediaSource` pipeline sources,
keyframe indexing, compositing) and `pj_scene2d_widgets` (Qt — the
`QRhiWidget`-based `MediaViewerWidget`, the `Scene2DDockWidget` layer stack on
top of `pj_scene_common`). pj_scene2D is a read-only consumer of
`pj_datastore::ObjectStore`; it never writes to storage.

Core tests are grouped into the `pj_scene2d_core_tests` and
`pj_scene2d_core_source_dir_tests` gtest runners, registered with
`gtest_discover_tests`: ctest names are `<runner>.<Suite>.<Case>`, with one process
per case. The source-directory runner uses the repository root for video fixtures.
`mcap_integration_test` and `dual_store_test` stay separate because both define
`MCAP_IMPLEMENTATION`; `ffmpeg_decoder_test` stays separate to preserve its ctest
`LSAN_OPTIONS` environment.

**Depth images** arrive as `sdk::Image` with a depth `encoding` (16UC1 / 32FC1 /
compressedDepth) — there is no `kDepthImage` producer. `Scene2DDockWidget` peeks a
`kImage` topic's first sample and routes depth-encoded images to the colormap
`DepthImageLayer` (per-layer colormap turbo/viridis/plasma/grayscale, invert, a
manual near-far range with an on-demand **Auto-fit** button that snaps to the current
frame's depth percentiles) and everything else to the plain `ImageLayer` (which
renders images with **nearest** (pixelated) magnification and exposes a **rectify**
toggle that undistorts the image with its `CameraInfo` — on by default, turn off to
override the always-on auto-decision for an already-rectified stream).
Both
`ImagePipelineSource` and `DepthPipelineSource` obtain the `sdk::Image` from a store
entry through one shared seam — `image_resolve.h::resolveImage` (the topic's
MessageParser when present, else the canonical `pj_image_v1` codec). The depth decode
— including the heavy compressedDepth PNG inflate — runs **off the UI thread** on an
`AsyncFrameWorker`, like the image/video sources; `setTimestamp()` posts and returns,
`takeFrame()` polls, and a frame-ready callback drives the repaint.

The colormap is applied **on the GPU**: `DepthPipelineSource` emits a raw float
depth frame (`PixelFormat::kDepthR32F`) plus `DepthColorParams` (near/far/invert/
colormap), and `MediaViewerWidget`'s media shader (`pixelFormat == 5`) normalizes by
[near,far] and looks the result up in a colormap LUT (`u_tex`, built once from
`pj_widgets/Colormap.h::buildColormapLut`). So there is no per-pixel CPU colormap, and
changing colormap/range/invert is a uniform write with no re-decode. The colormap set
+ its math live **once** in `pj_widgets/Colormap.h` (`Colormap` enum, `colorFor()`,
`buildColormapLut()`, and `colormapGlsl()` for the 3D in-shader path), shared with the
3D pointcloud colouring so a scalar maps to the same colour in both views. The Qt-free
core stays decoupled: `DepthPipelineSource::setColormap` takes an opaque `uint8_t`
colormap id (== `Colormap` value == LUT row), not the enum.

The hover **Point Inspector** (`pixel_inspector.{h,cpp}`) is **format-aware**: ordinary
images get the zoom grid + RGB readout, but a `kDepthR32F` frame shows the **metric
depth** (`depthMetersAt`, "— (no data)" for non-finite/≤0 pixels) plus a swatch of the
on-screen colormapped colour (`depthColormapColor`, which reuses the same
`Colormap.h::colorFor()` the GPU LUT is built from) — no zoom grid, since a
colormapped pixel has no per-pixel RGB worth magnifying. See
[`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md) §13.

## Docs

Read in this order:

- [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) — the WHAT: scope, use cases, functional requirements, module contract.
- [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) — the HOW: module structure, data flow, scrub architecture, codec pipeline, threading model, key invariants.
- [`docs/TECHNICAL_NOTES.md`](./docs/TECHNICAL_NOTES.md) — domain background: Qt 6.11.1/QRhi specifics, codec caveats, HW-accel matrices, lessons learned.
- [`../plotjuggler_sdk/docs/builtin_type.md`](../plotjuggler_sdk/docs/builtin_type.md) — canonical builtin-type catalog (the SDK owns all canonical scene/object schemas: Image, DepthImage, VideoFrame, PointCloud, OccupancyGrid, SceneEntities, FrameTransforms, ImageAnnotations, etc.).

## Key headers

- `core/include/pj_scene2d_core/media_source.h` — the uniform `setTimestamp`/`takeFrame` frame-delivery contract everything plugs into.
- `core/include/pj_scene2d_core/image_pipeline_source.h`, `streaming_video_source.h`, `depth_pipeline_source.h`, `scene_pipeline_source.h`, `composite_media_source.h` — the concrete `MediaSource` implementations.
- `core/include/pj_scene2d_core/image_resolve.h` — `resolveImage`, the single seam that turns a store entry into an `sdk::Image` (parser or canonical codec); shared by the image and depth pipeline sources.
- `core/include/pj_scene2d_core/streaming_video_decoder.h`, `ffmpeg_decoder.h` — GOP-aware streaming video decode on top of FFmpeg. `FfmpegDecoder` carries the codec's colorimetry onto each frame and emits native NV12 on the hardware-decode path.
- `core/include/pj_scene2d_core/video_color.h` — `buildYuvMatrix(space, range)`: the per-frame YUV→RGB matrix (BT.601/709 + limited/full range) the media shader uploads, replacing the old hardcoded BT.709. `depth_range.h` — `depthPercentileRange` backing the depth Auto-fit.
- `core/include/pj_scene2d_core/overlay_geometry.h` — backend-agnostic tessellation of annotation overlays (lines/points/fills/circles) into GPU vertex data; stroke width scales with zoom but is **floored at 1px on screen** so edges never go sub-pixel and vanish.
- `widgets/include/pj_scene2d_widgets/media_viewer_widget.h` — the `QRhiWidget` renderer (YUV/RGB pipelines + annotation overlays).
- `widgets/include/pj_scene2d_widgets/Scene2DDockWidget.h` — the dock widget wiring layers into `pj_scene_common`'s `SceneDockWidget`.

## Working conventions

- Run the module tests and make sure they pass before any commit.
- Keep the markdown files in `docs/` in sync with the code you change (see the
  root `CLAUDE.md` freshness discipline); record hard-won lessons in
  `TECHNICAL_NOTES.md`, especially after long debugging sessions.
- Think test-first: prefer automated reproduction over asking the user to run
  the app. When an issue is reported, reproduce it in a test before fixing it.
