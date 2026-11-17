# pj_scene2D architecture

How the 2D media family works today: images, video, depth images and pixel-space annotations, read from
`pj_datastore::ObjectStore`, decoded off the GUI thread and drawn through QRhi in a dock synchronized with the
global timeline. Measured evidence behind these choices is in [TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md); the
canonical object schemas are in the SDK's
[`builtin_type.md`](https://github.com/PlotJuggler/plotjuggler_sdk/blob/main/docs/builtin_type.md).

## Targets and dependencies

```
pj_scene2d_widgets ──► pj_scene2d_core ──► plotjuggler_sdk::base, pj_datastore,
   │                                       FFmpeg (avcodec/avutil/swscale), turbojpeg, libpng
   ├──► Qt Widgets/Gui/GuiPrivate (QRhi)
   ├──► pj_runtime        (SessionManager, IDataWidget)
   ├──► pj_scene_common   (SceneDockWidget / ISceneLayer)
   └──► pj_widgets        (Colormap.h, ToggleSwitch, RHI helpers)
```

- `pj_scene2d_core` is Qt-free: decoders, `MediaSource` implementations, keyframe indexing, compositing, overlay
  tessellation, rectification maps. Plugins never link it; they write canonical bytes (or raw messages a
  `MessageParser` turns into canonical objects) into the store.
- `pj_scene2d_widgets` is thin: `MediaViewerWidget` owns the GPU surface and polls one `MediaSource`;
  `Scene2DDockWidget` and the layer classes (`ImageLayer`, `VideoLayer`, `DepthImageLayer`, `SceneDecoderLayer`, on
  the `Scene2DLayer` base) wire topics to sources. All decode logic and threading live in the core.
- `pj_scene2D/tests` holds the widget-level tests plus the reference CDR-to-`ImageAnnotations` adapters
  (`cdr_*_to_image_annotation`, `marker_palette`) used by `cdr_to_image_annotation_test`; the app never links them.
- `tools/` (`PJ_BUILD_TOOLS`): `extract_frame` (MCAP image to PPM) and `mp4_to_mcap.py` (MP4 to per-frame
  `CompressedVideo` MCAP).

## Read path

```
GUI thread                                     per-layer MediaSource
  SceneDockWidget::onTrackerTime(t)
    └─ layer->setTrackerTime(t) ─► source->setTimestamp(ns)
                                     ├─ ImagePipelineSource   post to worker ─► resolve, decode, rectify
                                     ├─ DepthPipelineSource   post to worker ─► resolve, R32F decode
                                     ├─ StreamingVideoSource  post to worker ─► StreamingVideoDecoder::decodeAt
                                     └─ ScenePipelineSource   synchronous decode on the caller
  MediaViewerWidget::render()
    └─ composite->takeFrame() ─► MediaFrame { base, pixel_layers, overlays } ─► textures + overlay VBOs ─► draw
```

`pj_runtime` drives the dock through `IDataWidget::onTrackerTime`; the dock clamps the time, forwards it to each
visible layer, and repaints. A worker-backed source's frame-ready callback (called on the worker thread) is
turned by `Scene2DLayer::makeQueuedRepaintCallback()` into a queued repaint on the GUI thread, so a decode that
finishes after the tick's single render is still shown.

## Frame delivery

`MediaSource` (`media_source.h`) is the one contract between producers and the viewer: `setTimestamp(ns)` when the
time changes, `takeFrame()` at render rate, both on the GUI thread. `takeFrame()` returns the newest frame since the
last call or nothing. There is no public `cancel()`: each source cancels or coalesces internally.

`AsyncFrameWorker` (`async_frame_worker.h`) is the worker shared by the image, depth and video sources:

- **Latest-wins requests.** A new target overwrites the pending one; the worker decodes only the newest.
- **Single-slot result mailbox.** `deposit()` overwrites the previous frame, so a stale frame cannot reach the
  display. At most two decoded frames per source are alive (the mailbox and the one shown).
- **Frame-ready callback**, copied under its own lock and invoked after release.
- **Exception barrier.** A decode body that throws is caught on the worker and produces no frame (see "Errors and
  missing data").
- **Teardown.** `stop()` cancels, wakes and joins the thread; a lost wakeup cannot hang the join. A thread that has
  not entered its loop yet is detached instead of joined (TECHNICAL_NOTES "Decode threads in the WASM pthread
  pool").

Frames never travel through Qt signals: a queued signal per frame cannot be withdrawn, so during a fast scrub
stale frames queue up and reach the display after newer ones (TECHNICAL_NOTES "Frame delivery through Qt
signals"). Frames are addressed by `int64_t` nanosecond timestamps, never by index.

## Cancellation and scrub preemption

`CancelToken` (`cancel_token.h`) is an atomic flag, optionally backed by a poll function so a caller's own check
(an on-demand script's cancel) reaches the decode. Only the streaming video decode polls it, between packets.

`StreamingVideoSource` starts its worker with a preempt predicate: a new request cancels the in-flight decode only
on a **scrub**, a backward jump or a forward jump larger than `kScrubPreviewThresholdNs`. A contiguous playback
step lets the decode finish, because a cancel discards the decoder's forward-continuation state and forces a GOP
re-seek; cancelling on every 60 Hz tick would deliver no frame at all (TECHNICAL_NOTES "Scrub preemption").
Teardown always cancels. A cancelled decode deposits nothing: no source publishes partial frames.

The image and depth sources run without cancellation; stale targets are coalesced before the next decode starts.

## Image codecs

`CodecPipeline` is an ordered chain of `CodecStage`s from bytes to a `DecodedFrame`; a stage either produces a
terminal pixel format or passes intermediate bytes on. Built-in stages (`codecs.h`): `JpegCodec` (turbojpeg),
`PngCodec` (libpng, RGB/RGBA/Mono16), `ImageDecodeCascade` (JPEG or PNG by magic), `AutoImageCodec` (cascade plus
Mono16 normalization), `Mono16ToGrayscale`, `SegmentationPalette`. Codecs are built in, not plugins: adding one is
a code change.

`ImagePipelineSource` gets its `sdk::Image` from one seam, `resolveImage` (`image_resolve.h`): the topic's
`MessageParser` when one is bound (called under the parser's shared mutex: parsers are not thread-safe), else the
canonical `pj_image_v1` codec. Raw and Bayer encodings, including a raw buffer losslessly wrapped in a grayscale
PNG, are reinterpreted at the logical geometry; compressed ones go through the cascade. There is no decoded-frame
cache for stills (TECHNICAL_NOTES "Image decode cost").

## Scene decoders

`makeSceneDecoder(schema)` (`scene_decoder.h`) returns `ImageAnnotationsSceneDecoder` for `PJ.ImageAnnotations` or
`SceneEntities2DDecoder` for `PJ.SceneEntities`, else null. Each has two entry points: canonical bytes, or an
`sdk::BuiltinObject` a parser already produced (no serialize round trip). `ScenePipelineSource` owns one decoder
per layer and decodes synchronously in `setTimestamp()`.

`SceneEntities2DDecoder` is a stateless per-message projection to annotations: arrows, cubes, spheres, lines,
triangles, texts and axes, from the XY of `pose.position`. Orientation, deletions, `lifetime_ns` and
`frame_locked` are not applied.

Source-format conversion (CDR `vision_msgs`, `yolo_msgs`, ...) belongs to the loader or parser; this module sees
only canonical objects.

## Streaming video decode

The host's only video path: one encoded frame per ObjectStore entry (`sdk::VideoFrame` / `CompressedVideo`),
unwrapped by the topic's parser (`makeVideoFrameNalExtractor`, a zero-copy view of the payload; one
extractor per concurrent decoder) and decoded GOP-aware by
`StreamingVideoDecoder` over `FfmpegDecoder`. A producer with an MP4 transcodes it to per-frame entries at ingest.

- **Codecs.** `videoCodecIdFromFormat()` accepts H.264, HEVC and AV1: the codecs with a keyframe oracle in
  `isVideoKeyframe()` (H.264 IDR NAL 5, HEVC IRAP NAL 16-21, AV1 sequence-header OBU). Anything else, VP9
  included, fails with "unsupported video codec": without an oracle no seek can work.
- **Opening.** Deferred to the first keyframe (joining mid-stream works). The decoder opens from
  `makeVideoCodecParams()` and primes `extradata` from that keyframe's in-band parameter sets
  (`primeKeyframeParamSets`), with the reorder delay a demuxer would report (`computeReorderDelay`); an unprimed
  open drops the first B-frames (TECHNICAL_NOTES "B-frames").
- **Store keys and presentation.** The store is keyed by DTS; the PTS travels in the payload. The decoder feeds in
  DTS order and serves by PTS: `decodeAt(T)` shows the greatest PTS at or before T, through a presentation index
  built by the keyframe scan. Each packet is indexed at its PTS plus its entry's `payload_stamp_shift`, so the
  decoder answers on the dataset clock and a merge's same-name topics do not collide.
- **Forward continuation and seek.** A target at or after the last served PTS continues forward without a flush;
  anything else seeks to the keyframe before the target, flushes, and decodes forward. A repeated target returns
  the cached frame. The pump receives every queued frame before each send, so a send cannot hit EAGAIN and lose
  a frame.
- **Index maintenance.** Packets are addressed by UID (`ObjectStore::indexOf`), so packets at an equal DTS never
  alias. The index follows `ObjectStore::contentIdentity`: an unchanged poll scans nothing, an append scans only the
  arrivals (a UID cursor over `drainNewSince`), a rewrite (earlier DTS, removal, clear) rebuilds. A new packet at an
  already indexed PTS replaces that frame. Evicted packets are pruned when the store front moves; a GOP walk follows
  its packets through a front eviction by UID (`packetAt`). A rewrite during a decode fails it ("video content
  changed during decode"); an append or front eviction does not.
- **Eviction while live.** Forward continuation needs neither the keyframe nor its entry; only a seek needs a
  keyframe still in the store.
- **Thumbnails.** `decodeSampled()` decodes every packet but downloads and converts only the sampled ones;
  `EntryThumbnailCache` uses it (see "Media sources").

`FfmpegDecoder` (`ffmpeg_decoder.h`) picks the hardware backend at run time: D3D11VA first on Windows, then the
types `av_hwdevice_iterate_types` lists, keeping the first with a decode config for the codec; VAAPI walks the DRM
render nodes (TECHNICAL_NOTES "VAAPI device selection"). Software decode is the fallback everywhere and the only
path in the browser. Software output is YUV420P; hardware output is downloaded as native NV12. Each frame carries
its colour space and range, read from the decoded frame (TECHNICAL_NOTES "Colorimetry"). `convertToRgb24` is the
CPU path to packed RGB for on-demand scripts (TECHNICAL_NOTES "CPU RGB conversion for scripts").

## Media sources

| Source | Thread | Output |
|---|---|---|
| `ImagePipelineSource` | `AsyncFrameWorker` | RGB/mono/YUV `DecodedFrame`, optionally rectified |
| `DepthPipelineSource` | `AsyncFrameWorker` | `kDepthR32F` pixel layer + `DepthColorParams` |
| `StreamingVideoSource` | `AsyncFrameWorker`, scrub preemption | YUV420P or NV12 frame; thumbnail preview on a scrub |
| `ScenePipelineSource` | caller (GUI) | `SceneFrame` overlays |
| `CompositeMediaSource` | caller | fused `MediaFrame` of N child sources |
| `BorrowedMediaSource` | caller | non-owning wrapper over a layer's own source |

- **Dedup.** Image and depth sources dedup on the store entry's UID (`latestEntryIdAt`, which never resolves
  bytes), so a same-stamp replacement still decodes and a cold payload is not refetched for an unchanged entry.
  `invalidate()` forces the next request through.
- **Clears.** A seek before the shown entry with nothing at or before the cursor (a derived image computed only
  later), or an empty image (a derived tombstone), deposits a null frame that clears the layer. A cursor before any
  data is silent; an entry evicted under the cursor stays shown.
- **Stamp.** `DecodedFrame::stamp` is the image payload's own timestamp plus the entry's merge shift
  (`resolveImage`), and `entry_ts` the store key; both drive annotation pairing.
- **Thumbnails.** `StreamingVideoSource` builds an `EntryThumbnailCache` only for a topic without time-window
  retention and only for video wider than the thumbnail cap: a background pass decodes the topic once and keeps an
  HD-capped JPEG every adaptive interval. On a scrub the source deposits the nearest thumbnail at or before the
  target while the full decode runs.
- **Parser lifetime.** A parser-backed source holds the parser binding's keepalive until its worker is joined:
  the extension catalog unloads the plugin on app close.

## Depth images

Depth arrives as an `sdk::Image` with a depth `encoding` (16UC1, 32FC1, compressedDepth); no producer emits
`kDepthImage`, whose registration is dormant. `DepthPipelineSource` resolves the image like the image source,
decodes it to float metres on its worker (the compressedDepth PNG inflate included), and emits a `kDepthR32F`
frame where 0 means no data. `isValidDepth()` (`depth_range.h`) is the rule, and it must stay equal to the
shader's `!(d > 0)` and the inspector's readout, or the readout disagrees with the transparent pixels. Colormap,
near/far range and invert travel as `DepthColorParams` on the frame and are applied in the shader through a
colormap LUT built from `pj_widgets/Colormap.h`, the source the 3D point-cloud colouring also uses; changing them is
a uniform write plus a re-emit, no re-decode. The core takes the colormap as an opaque id (the `Colormap` value and
LUT row). `autoRange()` (Auto-fit) fits near/far to percentiles of the frame currently shown, once per click.

## Camera rectification

`ImagePipelineSource` undistorts an image when the layer's Rectify toggle is on (the default) and the attach-time
calibration map holds a usable `CameraInfo` for the image's `frame_id` in its dataset. Calibration topics are found
by object classification or a `/camera_info` suffix and decoded through their parser or the canonical codec. The
map is installed before the first `setTimestamp()` (`setCameraInfoMap` is refused later: the worker reads it
unlocked), so a `CameraInfo` published after attach is not applied.

- **GPU path** (once the viewer reports support): the worker leaves the frame raw and attaches the cached
  `UndistortMap` as `DecodedFrame::rectify_map`; the image shader remaps through an RG32UI LUT
  (`undistortMapToNormalizedRG`).
- **CPU path** (until then, or when the viewer cannot): `rectifyFrameFast` with a fixed-point `UndistortMapFast`
  into a reused buffer, bit-for-bit equal to the float `rectifyFrame`. It skips planar and 16-bit formats.
- **Handshake.** `MediaViewerWidget::initialize()` calls `setGpuRectificationAvailable()` once the base pipeline
  exists; the composite fans it out to its children, and a change re-decodes the current frame.
- Both paths show the image at the calibration's native resolution, the space annotations are authored in.
  `rectificationStatus()` tells `ImageLayer` whether to enable the toggle and what its tooltip explains.

Why the image and not the annotations, and the assumption's blind spot: TECHNICAL_NOTES "Rectification
assumption".

## Compositing

`CompositeMediaSource` holds one child source per visible layer, in draw order, and presents one `MediaFrame`:

- Each layer's latest contribution is **retained**: a layer with nothing new on a tick keeps contributing.
- Every layer's pixel output becomes an entry of `pixel_layers` (opacity scaled by the layer's); the first layer
  with pixels is the base for pairing. Overlays are concatenated in layer order.
- `setGpuRectificationAvailable`, `invalidate` and `setTimestamp` fan out to every child.
- On a rebuild (add, remove, hide, reorder) `adoptContributions()` carries the persisting layers' frames over from
  the old composite, and the dock repoints the viewer before freeing the old one.
- Every query is at-or-before, never a future entry. There is no skew rejection for unpaired overlays: a 10 Hz
  detection overlay on 30 Hz video keeps its last box until a newer one arrives.

## Annotation pairing

`setLayerPairing` gives a layer its store topic, its image cadence and a warning sink; the dock sets it for every
layer, and asks for pairing only for an on-demand derived topic (from its `pj_derived` metadata). For such a layer,
an annotation naming its image (`image_topic` set) is kept only when the base layer has that topic and the
annotation's stamp lies within one cadence of the delivered frame's `stamp`. The cadence is the median positive gap
of the image topic's last 16 entries at or before the frame's `entry_ts`, 100 ms with fewer than two gaps. An image
still decoding never pairs with the tracker's instant. Rejected annotations stay in the retained contribution and
are re-checked when pixels arrive. The warning sink is called at every composite; the dock posts a change to the
layer row's annotation warning through the event loop (the composite is polled inside render), and a hidden layer's
warning is cleared the same way, so the clear queues behind an earlier set. A parser's annotations are never
checked: their `image_topic` is informational (a header frame id). Derived annotations carry their image's topic
and stamp (docs/derived_objects/ARCHITECTURE.md, "Scene integration").

## Rendering

`MediaViewerWidget` is a `QRhiWidget` (OpenGL, Vulkan, D3D11, Metal, WebGL2; `PJ::preferredGraphicsApi()`), built
parentless and then inserted (TECHNICAL_NOTES "QRhiWidget in a raster window"). It draws, in order:

1. **Image** (`yuv_to_rgb`): the base layer. The `pixelFormat` uniform selects YUV420P (three R8), NV12 (R8 + RG8;
   a backend without RG8 gets a CPU deinterleave), RGBA, Mono8 (R8), BGRA (swizzled) or depth (R32F plus the LUT
   in `u_tex`). RGB888, BGR888 and Mono16 are converted to RGBA on the CPU. The YUV matrix comes per frame from
   `buildYuvMatrix(space, range)` (`video_color.h`), one affine 4x4, so the shader is the same for every
   combination. Chroma sampling follows 4:2:0 siting at odd sizes (TECHNICAL_NOTES "Chroma siting at odd sizes").
2. **Composite**: further `pixel_layers`, alpha-blended (SrcAlpha / OneMinusSrcAlpha), one draw each.
3. **Fills** (`scene_lines`, triangles): point squares and line-loop and circle fills.
4. **Outlines** (`scene_lines`, triangles): every stroke, expanded on the CPU by `overlay_geometry.h`. Width is in
   image pixels, so it scales with zoom, floored at 1 px on screen; strokes are re-expanded when the effective
   scale changes. Line primitives are not used: they are 1 px wide and GL can cull whole segments when zoomed in.
5. **Text** (`scene_text`): one quad per label over an R8 glyph mask, tinted by the vertex colour. Each cached
   glyph texture has its own SRB, because QRhi reads an SRB at submit time. The cache is LRU-bounded.

Layer textures are reused when a frame brings the same pixel buffer. GPU rectification samples the RG32UI LUT
(binding 4) texel-exact with a +0.5 half-texel offset baked in, and a negative sentinel paints out-of-bounds
pixels black; non-rectifying layers bind a 1x1 placeholder so every texture layer has one SRB layout. On the GPU
path the uploaded texture is the raw frame but the **logical size** (aspect, overlays, inspector) is the map's
output size.

**View transform.** Zoom (wheel, cursor-anchored, [1, 20]), pan (drag, zero at 1x) and a dock-wide clockwise
rotation (0, 90, -90, 180) form one matrix: `scale(fit x zoom) * translate(pan) * rotate(-deg)` in GL clip space
(y up), corrected per backend by `clipSpaceCorrMatrix()`. Pan follows the rotation, so drags stay in screen
directions. `fitScale`, `panAfterZoom`, `panAfterDrag` and `widgetPointToImagePixel` (`pixel_inspector.h`) share
this math with the viewer; a change to the transform must change the inverse too, or the inspector reads the wrong
pixel. Text quads are counter-rotated in image space so labels stay upright. The state is saved as
`<view zoom pan_x pan_y rotation>` in the dock XML; double-click resets zoom and pan, not rotation.

Shader packs (`.qsb`) are committed; edit a `.vert`/`.frag` and re-bake with `widgets/shaders/bake.sh` and the
pinned `qsb` (TECHNICAL_NOTES "Shader packs").

## GPU failure state

No draw may reference a resource whose `create()` failed (D3D11 crashes in `beginPass`). When the image pipeline,
its samplers or the base texture layer cannot be created, `initialize()` releases everything: `render()` only
clears, `renderFailed()` and `renderingWarningChanged()` fire once, and the dock shows the warning in a label below
the viewer. Every later `initialize()` (new QRhi, resized target) retries. A window that gets no QRhi at all routes
Qt's own `renderFailed()` into the same warning. Optional parts degrade: a failed overlay drops that overlay, a
failed frame texture drops the frame (its uploads go to a batch of their own, discarded with it), and without the
blending pipeline pixel layers draw opaque; the next `initialize()` rebuilds what is missing. Builds with
`PJ_RHI_TEST_HOOKS` inject failures through the `pjTestFailPipeline` property (`base`, `composite`, `points`,
`thick`, `text`, `layer srb`).

A machine without a GPU still gets a QRhi (WARP on Windows, Mesa's software rasterizer on Linux); there is no CPU
renderer.

## Point inspector

The "Show point" toggle enables a hover magnifier (`PixelInspector`). Ordinary images show a zoom grid and the RGB
value, converted with the same matrix the display uses. A `kDepthR32F` frame shows the position, the depth in
metres (or "no data", `depthMetersAt`) and a swatch from `depthColormapColor`, which reproduces the shader through
the same `colorFor()` the LUT is built from; no zoom grid. On the GPU-rectify path the cursor maps to the logical
space and the value is read from the source pixel the map points at.

## Clock and repaint

`SceneDockWidget::onTrackerTime` (pj_scene_common) skips the layer advance and the repaint when the combined
`renderKey(time)` of the visible layers is unchanged. `Scene2DLayer::renderKey` is the UID of the active entry
(never resolving bytes), folded with a generation that a dataset replacement bumps, so a static image does not
re-composite at 60 Hz and a same-stamp replacement still repaints (TECHNICAL_NOTES "Repaint cadence"). A new
frame from a worker repaints through the frame-ready callback regardless of the gate. Streaming topics follow the
newest sample: the dock subscribes to the session's live samples and moves visible layers to the data edge.

## Dock behaviour

- **Layers.** `Scene2DDockWidget::handlesObjectType()` is the list of types the family accepts (images, depth
  images, video, image annotations, scene entities); host drop routing reads it.
- **One background per dock.** An interactive drop of an image, depth image or video replaces the current
  background (the old one is removed only after the new one attached) and draws beneath the overlays; overlays
  stack. A deferred drop (advertised topic, no data yet) gets the same policy when it materializes. `addTopic()` and
  workspace restore always append, so a saved multi-image layout loads as saved.
- **Depth routing.** `kImage` covers colour and depth: the dock peeks the topic's first sample's encoding and
  creates a `DepthImageLayer` or an `ImageLayer`. A saved `layer_kind` attribute overrides the peek on restore;
  XML without it falls back to the peek (depth settings in the XML select depth).
- **Restore.** XML is validated before the destructive restore, normalized on a private clone, and a snapshot puts
  the dock back if a late step fails.
- **Placeholder.** An empty dock shows a greyed icon on a stacked page instead of the viewer: a raster widget over
  the QRhiWidget's surface does not composite reliably.
- **Image layer**: nearest magnification, Rectify toggle (`rectify_enabled` in XML; absent means on).
  **Depth layer**: colormap (`turbo` default; unknown names restore it), near/far, invert, Auto-fit; opacity is
  restored from XML but has no UI. **Video layer** requires the topic's parser.

## Threading and locks

| Thread | Work |
|---|---|
| GUI | `onTrackerTime`, `setTimestamp`, scene decode, `render()` / `takeFrame()`, uploads |
| one `AsyncFrameWorker` per image, depth or video source | store lookup, parse, decode, rectify, deposit |
| `EntryThumbnailCache` builder, one per cache | one decode pass with its own decoder and extractor |
| ingest (pj_app streaming manager, loaders) | `ObjectStore` pushes and eviction |

| Lock | Guards | Rule |
|---|---|---|
| `AsyncFrameWorker` request mutex | pending target, force flag, active cancel token | never held during lookup or decode |
| `AsyncFrameWorker` result mutex | the mailbox | never held with a store or decoder lock |
| `AsyncFrameWorker` callback mutex | the frame-ready callback | callback copied under it, invoked after release |
| parser mutex (shared with the layer) | `MessageParser::parseObject` | held across each parse |
| `EntryThumbnailCache` mutex | tiles and byte budget | builder publishes, video worker looks up |
| `MediaViewerWidget::frame_mutex_` | source pointer, pending frames, inspector frame | GUI thread only; keeps a source swap and frame staging atomic |

No two of these are held at once. ObjectStore locks are taken inside its calls and released before decode;
`EntryTimestampsView` holds a shared lock for its lifetime, so it must not be kept across a decode (it would block
ingest). The keyframe and presentation indexes have no lock: only the owning worker touches them.

## Errors and missing data

No exception crosses a library or thread boundary: fallible calls return `PJ::Expected`, and the worker's barrier
catches whatever a decode body throws. A failed decode is logged and deposits nothing, so the last good frame
stays; the next good decode replaces it. There is no per-layer error indicator in the UI (see "Known gaps").
Missing data (cursor before the first entry) is not an error and logs nothing.

## Behaviour contracts

- Read-only consumer of `ObjectStore`: decoded data is never written back.
- At-or-before is the only time query; timestamps are `int64_t` nanoseconds on the dataset clock everywhere.
- One `MediaSource` per viewer; a viewer never polls layer sources directly.
- Frames reach the viewer only through `takeFrame()`; no partial frame is ever published.
- **Pause freezes the store the viewer reads.** While a stream is paused, ingest goes to pj_app's staging store and
  the viewer's store receives no pushes or eviction until resume (`flushTo`), so a seek cannot lose its keyframe
  mid-walk. Live and scrub are two separate modes by product choice: the timeline never shifts under a scrub.
- Keyframe tracking lives in the decoder, not in `ObjectStore`, which stays codec-agnostic. Parsers peel envelopes
  and never inspect NAL types or GOP structure.
- Decoders of different layers share no state; one decoder per layer for the layer's lifetime.
- Widgets receive time and never drive the clock.
- Multiple cameras are separate docks with no cross-dock barrier: each shows what its own decode last produced.
- The host creates and destroys docks; a dock must be destroyed before the `SessionManager` and `ObjectStore` it
  reads.

## Recipes

- **New pixel format:** add the `PixelFormat`, `expectedBufferSize`, the viewer's `texturePathFor` and
  `isUploadablePixelFormat`, the shader branch (and re-bake), and `pixelRgbAt` for the inspector.
- **New image codec:** a `CodecStage` in `codecs.h`, wired into `ImageDecodeCascade` or the pipeline builder.
- **New video codec:** an oracle in `isVideoKeyframe`, the format in `videoCodecIdFromFormat`, parameter-set
  priming in `primeKeyframeParamSets` if the codec carries them in-band, and the decoder in the FFmpeg allowlists
  (Conan options, `cmake/PjWasmFfmpeg.cmake`).
- **New overlay primitive:** decode it in `scene_decoder.h`'s decoders, tessellate it in `overlay_geometry.h` (with
  a test), and draw it from the fills or outlines pipeline.
- **New layer kind:** a `Scene2DLayer` subclass overriding `createMediaSource()` and the options/XML hooks, plus
  the type in `handlesObjectType()`, the dock's layer registrations and `scene2d_pipelines.cpp`.

## Known gaps

- Decode errors only reach the log; the layer shows its last good frame with no indicator.
- HDR (BT.2020, PQ, HLG) gets the SD/HD heuristic matrix; only BT.601 and BT.709 SDR are correct.
- VP9 and other codecs without a keyframe oracle are rejected.
- Scene entities: cylinders and models are not projected; orientation, deletions and lifetimes are ignored.
- `SegmentationPalette` exists but no layer type uses it.
- Thumbnails are not built for topics with time-window retention (live streams); the cache's prune is unused.
- No `frame_id` correlation between overlays and images; pairing is by topic and time only.
- A `CameraInfo` published after the image layer attached is not applied.
- CPU rectification skips planar and 16-bit formats.
- Hardware frames are downloaded to system memory; no zero-copy import (TECHNICAL_NOTES "Hardware frame download").
- No audio, reverse playback, frame export or encoding.
- The `kDepthImage` registration is dormant: no producer emits the type.
