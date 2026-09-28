# pj_scene2D Architecture

The HOW document for the pj_scene2D module. `REQUIREMENTS.md` defines WHAT
pj_scene2D does; this document defines HOW it is built. Read `REQUIREMENTS.md`
first — this document assumes familiarity with every section.

Cross-references to REQUIREMENTS.md use `§R` notation (e.g., `§R4.3`).
Cross-references to `OBJECT_STORE_DESIGN.md` use `§OS` notation (e.g., `§OS3.4`).

---

## 1. Module Structure

pj_scene2D ships as two CMake targets with a strict dependency direction
(matches `pj_scene2D/core/CMakeLists.txt` and `pj_scene2D/widgets/CMakeLists.txt`):

```
pj_scene2d_widgets  ──►  pj_scene2d_core  ──►  pj_base
     │                  │
     │                  ├──►  pj_datastore
     │                  ├──►  pj_plugin_sdk         (INTERFACE; canonical schemas + codecs)
     │                  ├──►  FFmpeg                (optional, gated on PJ_SCENE2D_HAS_FFMPEG)
     │                  ├──►  turbojpeg
     │                  └──►  libpng
     │
     ├──►  Qt 6.11.1 (Widgets, Gui, GuiPrivate — QRhi via Gui's private headers)
     ├──►  pj_runtime           (IDataWidget contract, PlaybackEngine driver)
     ├──►  pj_scene_common      (SceneDockWidget/ISceneLayer base; backend-agnostic layered scene dock framework)
     ├──►  pj_widgets           (shared Colormap.h LUT/enum for the GPU depth colormap)
     └──►  pj_scene2d_core
```

Canonical object schemas live in `plotjuggler_sdk/pj_base/builtin/` and
are made available to pj_scene2D via `pj_plugin_sdk` (an INTERFACE library
that re-exports the canonical SDK headers). `ImageAnnotations` owns the
canonical wire-format codec (writer + reader). Both pj_scene2D (consumer
of canonical bytes) and any loader/plugin (producer of canonical bytes)
depend on those SDK types; loaders never link pj_scene2d_core. pj_scene2D
keeps renderer-local aliases and the `SceneFrame` batch wrapper in
`pj_scene2d_core/scene_frame.h`.

### pj_scene2d_core (no Qt)

Pure C++ library. Contains everything that does not touch Qt:

| Component | Header(s) | Role |
|-----------|-----------|------|
| `BorrowedMediaSource` | `borrowed_media_source.h` | Non-owning `MediaSource` adapter over an externally-owned source pointer. |
| `AsyncFrameWorker` | `async_frame_worker.h` | Shared latest-wins decode-worker engine (request coalescing, optional cancellation, result mailbox, frame-ready callback, exception barrier, lost-wakeup-safe teardown) composed by the worker-backed sources. |
| `CancelToken` | `cancel_token.h` | Shared atomic cancellation flag polled by streaming-video decode work. |
| `CodecPipeline` | `codec_pipeline.h` | Ordered chain of `CodecStage` transforms from raw bytes to `DecodedFrame`. |
| `Image codecs` | `codecs.h` | Built-in codec stages and image pipeline builders: JPEG, PNG, Mono16 normalization, Bayer, segmentation palette. |
| `CompositeMediaSource` | `composite_media_source.h` | Multi-layer fan-out: owns N `MediaSource`s and fuses their `MediaFrame`s (§5.4 / §8). |
| `DecodedFrame` | `decoded_frame.h` | RAII wrapper for decoded pixel data (YUV planes or packed RGB/RGBA buffers). |
| `DepthPipelineSource` | `depth_pipeline_source.h` | `MediaSource` for depth-encoded `Image` topics (16UC1 / 32FC1 / compressedDepth via `resolveImage`): decodes on an `AsyncFrameWorker` (off the UI thread, like the image/video sources — the compressedDepth PNG inflate is too heavy for the main thread) and emits a raw float32 (`kDepthR32F`) frame + `DepthColorParams`; the colormap/range/invert are applied on the GPU. |
| `EntryThumbnailCache` | `entry_thumbnail_cache.h` | Background HD-capped JPEG thumbnail cache for streaming `VideoFrame` scrub previews (§4.1). |
| `FfmpegDecoder` | `ffmpeg_decoder.h` | FFmpeg AVCodecContext wrapper: HW-accel probing, guaranteed software fallback, YUV420P output (§R4.7). |
| `H264 NAL utils` | `h264_utils.h` | H.264 Annex-B keyframe oracle (`isH264Keyframe`), used by codec-generic video helpers. |
| `ImageAnnotation codec wrappers` | `image_annotation_codec.h` | Singular-named wrappers around the canonical SDK `ImageAnnotations` codec. |
| `Image rectifier` | `image_rectifier.h` | Bilinear lens-undistortion of a `DecodedFrame` (interleaved 8-bit formats; planar/16-bit pass through). `rectifyFrame` (float reference) and the fast CPU-fallback `rectifyFrameFast` (precomputed `UndistortMapFast`, reused buffer — bit-for-bit equal, ~2x faster) used when GPU rectification is unavailable. |
| `Undistort remap` | `undistort_remap.h` | Per-camera reverse sampling map built from `CameraInfo` (K/D/R/P). `buildFastRectifyMap` derives the fixed-point CPU table; `undistortMapToNormalizedRG` packs the GPU LUT payload (source UV per output pixel, +0.5 half-texel, negative sentinel = out of bounds). See TECHNICAL_NOTES "rectify the image, not warp the annotations" and "GPU vs CPU rectification". |
| `MediaSource` | `media_source.h` | Abstract frame-delivery interface: `setTimestamp` + `takeFrame` (§5), plus `setGpuRectificationAvailable` (capability the widget pushes down so a rectifying source defers undistortion to the GPU). |
| `Parser object helper` | `parser_object.h` | Shared locked-`parseObject` helper over a `SessionManager::ParserBinding` snapshot (parser + mutex + keepalive); used by the image source and the VideoFrame NAL extractor. |
| `ImagePipelineSource` | `image_pipeline_source.h` | `MediaSource` for images: wraps CodecPipeline + ObjectStore, decodes on a worker thread (§5.1) |
| `MediaFrame` | `media_frame.h` | Multi-layer payload returned by `MediaSource`: legacy base frame, ordered pixel layers, and overlays. |
| `SceneDecoder` | `scene_decoder.h` | Schema dispatch and `ISceneDecoder` implementations for image annotations and 2D-projected scene entities (§4.3). |
| `SceneFrame` | `scene_frame.h` | Time-stamped batch of image-overlay annotations carried through the media pipeline. |
| `ScenePipelineSource` | `scene_pipeline_source.h` | Synchronous `MediaSource` for vector-overlay topics decoded from `ObjectStore` (§5). |
| `StreamingVideoDecoder` | `streaming_video_decoder.h` | ObjectStore-aware GOP decoder for `VideoFrame` entries (H.264/HEVC/AV1 keyed on `VideoFrame.format`) via `FfmpegDecoder` (§4.4). |
| `StreamingVideoSource` | `streaming_video_source.h` | Worker-backed `MediaSource` for streaming video; wraps `StreamingVideoDecoder` (§5.3). |
| `Thumbnail codec` | `thumbnail_codec.h` | Stateless HD-capped JPEG encode/decode helpers for YUV420P scrub thumbnails. |
| `Video codec utils` | `video_codec_utils.h` | Codec-generic dispatch, keyframe detection, codec-parameter creation, and parameter-set priming (§4.4). |
| `VideoFrame NAL extractor` | `video_frame_nal_extractor.h` | Parser-backed zero-copy extractor from canonical `VideoFrame` messages to raw NAL/OBU spans. |

DataSource plugins do NOT link `pj_scene2d_core` — plugins depend only
on `pj_base` (see `pj_plugins/docs/ARCHITECTURE.md`). Format-specific
helpers (NAL scanning, PTS lookup) belong inside the DataSource plugin
itself or in lightweight headers within `pj_base`.

### pj_scene2d_widgets (Qt widgets)

Qt widget library built on top of `pj_scene2d_core` and `pj_scene_common`.
`Scene2DDockWidget` extends `pj_scene_common`'s `SceneDockWidget` base (which
owns the ordered layer stack and the `IDataWidget`/`IObjectViewer` contracts);
the layer subclasses (image / video / depth_image / scene2d / scene_decoder)
supply the 2D-specific rendering. `ImageLayer` handles `kImage`; the `VideoLayer`
handles per-frame `kVideoFrame` streaming topics (parser-mode
`StreamingVideoSource`).

| Component | Header(s) | Role |
|-----------|-----------|------|
| `MediaViewerWidget` | `media_viewer_widget.h` | `QRhiWidget` subclass: GPU rendering via colorspace-aware YUV->RGB fragment shader (3 R8 textures for YUV420P, R8+RG8 for NV12; matrix from `buildYuvMatrix`), per-layer magnification filter, zoom/pan, RGB DecodedFrame path, GPU lens-rectification (§7.2), and `MediaSource` polling via `setMediaSource()` + `setTimestamp()` (§7) |
| YUV shaders | `shaders/yuv_to_rgb.{vert,frag}` | colorspace-aware YUV420P/NV12->RGB conversion (BT.601/709 + limited/full range via the per-frame `colorMatrix`); RGBA passthrough; native Mono8 (R8 expand) / BGRA (RGBA8 swizzle) paths; depth colormap (`pixelFormat == 5`: R32F depth in `y_tex`, normalized by near/far and looked up in the colormap LUT in `u_tex`); optional remap-LUT rectification (binding 4) — see §7.2 |
| Overlay shaders | `shaders/scene_lines.{vert,frag}`, `shaders/scene_quads.{vert,frag}`, `shaders/scene_text.{vert,frag}` | Annotation overlay pipelines (§7.1): triangle strokes (scale-with-1px-floor), solid fills, textured text quads |

The Qt layer is thin — it owns the GPU surface and polls the
`MediaSource`'s `takeFrame()` at render rate. All decode logic,
threading, and cancellation live in `pj_scene2d_core`.

---

## 2. Data Flow

Two paths: write (ingest) and read (display). pj_scene2D participates
only in the read path.

### Write path (ingest — pj_scene2D is NOT involved)

```
DataSource plugin
  ├─► [direct]    ObjectStore::pushOwned / pushLazy             ◄── primary path today
  └─► [delegated] host.pushRawMessage ─► MessageParser::parse()
                                            ├─► scalar write host  (DataEngine)
                                            └─► object write host  (ObjectStore)
                                                                   ◄── now supported via
                                                                       MessageParser protocol v4
                                                                       service-registry bindings
```

**Direct ingest is the primary, fully-wired path.** DataSource plugins
call `pushOwned` / `pushLazy` directly. The originally-proposed parser
ABI v2 has been delivered as `pj_plugins` MessageParser protocol v4:
rather than adding hosts as `parse()` parameters, parsers acquire both
a scalar write host (`pj.parser_write.v1`) and an object write host
through the service registry at `bind(registry)` time and write to
both inside a single `parse()` call. Delegated media ingest is therefore
structurally enabled and is being wired up incrementally (see
`REQUIREMENTS.md` §4.4).

### Read path (display — pj_scene2D's domain)

```
Main thread                            MediaSource (internal)
     │                                      │
     ├─ widget->setTimestamp(ts_ns)          │
     │       │                              │
     │       └─► source->setTimestamp(ts)    │
     │               │                      │
     │               ├─ [ImagePipeline]  post to worker ──► store->indexAt/at → decode()
     │               ├─ [StreamingVideo] post to worker ──► decoder_.decodeAt(ts)
     │               ├─ [DepthPipeline]  post to worker ──► resolveImage → R32F decode
     │               └─ [Scene]          latestAt(ts) → synchronous decode
     │                                      │
     ├─ widget->render()                    │
     │       │                              │
     │       └─► source->takeFrame()        │
     │               │                      │
     │               └─► MediaFrame (base DecodedFrame + pixel_layers + overlays)
     │                        │
     │                  upload to GPU textures
     │                        │
     │                   draw quad (colorspace-aware shader)
     │                        │
     └──────────────────► GPU display
```

On each application tick:

1. The main thread calls `widget->setTimestamp(ts_ns)`, which forwards
   to the attached `MediaSource`. Image, streaming-video, and depth sources
   post a request to their internal workers; the scene source decodes
   synchronously on the caller thread.
2. The main thread triggers a repaint (calls `widget->update()`).
3. In `render()`, the widget calls `source->takeFrame()` to get the
   latest decoded frame. If a new frame is available, it uploads pixel
   data to GPU textures and draws.

The Qt main thread drives both steps — there is no `TimelineCursor`
subscription model in `pj_base`. The entry point into the widget is
`PJ::IDataWidget::onTrackerTime(double)` (from `pj_runtime`), invoked
by `pj_runtime::PlaybackEngine`; the widget forwards that time to its
internal `MediaSource` via `setTimestamp()`. This matches how
PlotJuggler's existing plot widgets are driven. See §9.1 for details.

---

## 3. Scrub Architecture

This section ports the patterns proven in the `video_player_lab`
prototype (a standalone scrub-architecture testbed, since removed along
with its ARCHITECTURE.md). The WHY behind these patterns — and why the
alternatives fail — is inlined below; treat it as settled rationale, not
open design space.

### 3.1 Latest-wins frame delivery

The realized frame handoff is a pull-based latest-wins mailbox implemented once
in `AsyncFrameWorker` (`async_frame_worker.h`) — the shared decode-worker
engine that `ImagePipelineSource`, `StreamingVideoSource`, and
`DepthPipelineSource` compose. The
worker owns the request channel (latest-target-wins coalescing, optional
per-request `CancelToken`), the single-slot result mailbox
(`deposit()`/`take()` under its `result_mutex_`), the frame-ready callback,
the exception barrier, and the lost-wakeup-safe teardown; each source supplies
only its decode body. `takeFrame()` moves the taken frame into a `MediaFrame`.
The remaining synchronous source (scene entities) uses the same latest-result
contract without a worker.

Properties:

- **Latest-wins**: depositing a result physically overwrites the previous frame.
  A stale frame cannot reach the display because it is replaced before
  the UI polls.
- **At most two decoded frames alive per source** (one retained in the source,
  one being displayed). Memory usage is bounded.
- **No Qt event queue for frame payloads**. Worker-ready callbacks may
  wake the GUI to poll, but decoded frames cross threads only through the
  source's latest-result mailbox and `takeFrame()`.
- **Pull timing**: the UI polls via `render()` at repaint rate. On
  each pass: `auto r = source->takeFrame(); if (r) uploadToGpu(*r);`.

**Design history:** the prototype used a standalone `FrameSlot`
(`store()`/`take()` behind one mutex). WP1.1 removed that helper from this
tree; only the mailbox concept remains, implemented inline by the worker-backed
sources.

The video_player_lab prototype used frame indices as identifiers
because it was file-only. pj_scene2D uses `int64_t` nanosecond
timestamps instead — the universal coordinate that works for both file
and streaming sources.

**Qt signals are NOT used for frame delivery.** Adding a signal escape
hatch is explicitly forbidden. The structural problem (proven in the
prototype, where 5+ patch attempts failed before the mailbox design):
a `QueuedConnection` posts one event per decoded frame, and posted
events cannot be invalidated — during rapid scrub, stale frames pile up
in the event queue and reach the display after newer frames have already
been decoded. Patching around the queue (event compression,
sequence-number guards on delivery, blocking connections) does not fix
it, because the queue itself is the staleness buffer. The latest-wins
mailbox removes the buffer instead.

### 3.2 Direction-aware cancel-store

The prototype design question for cancellable decoders was: when the user
scrubs rapidly and an in-flight decode is cancelled, should a
partially-decoded frame be published to the result mailbox?

The rule, established empirically in the video_player_lab prototype:

- **Forward scrub** (`current_request_ts > previous_request_ts`):
  publish partials. The decoder is decoding toward the target from a
  keyframe behind it — partial progress shows frames between the
  previous position and the target, which matches the user's drag
  direction.
- **Backward scrub** (`current_request_ts < previous_request_ts`):
  suppress partials. The decoder replays FORWARD from a keyframe
  while the user is dragging BACKWARD — publishing partials would
  show frames moving in the wrong direction.
- **Same position or first request**: suppress.

The host's only video path today (`StreamingVideoSource`) uses a simpler
latest-wins model (no partials), so this direction-aware partial-publish
rule is preserved here as design rationale for any future file-backed
ObjectStore decoder, not as live code.

### 3.3 CancelToken

A streaming-video decode request carries a `CancelToken` — a class wrapping
an `atomic<bool>`, shared between requester and decoder via
`shared_ptr<CancelToken>`:

```cpp
class CancelToken {
 public:
  bool isCancelled() const { return flag_.load(std::memory_order_relaxed); }
  void cancel() { flag_.store(true, std::memory_order_relaxed); }

 private:
  std::atomic<bool> flag_{false};
};
```

Live cancellation today:

- `StreamingVideoDecoder`: between NAL units (after each `avcodec_receive_frame`).
- `ImagePipelineSource` / `DepthPipelineSource`: no mid-decode cancellation
  today; stale targets are coalesced before the worker starts the next decode.
- `ScenePipelineSource`: synchronous on the caller thread; no token is used.

A new streaming-video request flips the previous token **only when it is a
scrub** (backward, or a forward jump past the 0.5 s threshold — the worker's
`Options::preempt_predicate`, supplied by `StreamingVideoSource`). A
contiguous-playback step lets the in-flight decode finish: a cancel wipes the
decoder's forward-continuation state and forces a full GOP re-seek, so
unconditional preemption under a 60 Hz tracker would cancel every decode
before it delivers and playback collapses to zero frames (measured: the 4 K
playback benchmark went 0 → ~26 fps displayed when the predicate landed).
On a genuine scrub the decoder returns early and `StreamingVideoSource`
deposits no partial result; §3.2's direction-aware partial-publish rule is
design rationale only.

---

## 4. Codec Pipeline

> For the wire-format type catalog that the codec pipeline decodes, see
> [`plotjuggler_sdk/docs/builtin_type.md`](../../plotjuggler_sdk/docs/builtin_type.md) — the canonical SDK builtin-type catalog.

Each ObjectStore topic produces raw bytes in a wire format. To reach
display-ready pixels, those bytes pass through a **codec pipeline** —
an ordered chain of stateless transforms configured per-layer at
widget setup time.

Every link in the chain is a codec. Some decode bytes→pixels (image
decompression), some transform pixels→pixels (visualization mapping).
The production application receives canonical object payloads from parser
plugins before this point, so source envelopes such as ROS CDR are not
part of the core pipeline.

### Examples

```
Canonical Image (jpeg):       JpegCodec → [identity]
Canonical Image (png/mono16): PngCodec → Mono16ToGrayscale
Raw segmentation mask:        [identity] → SegmentationPalette
```

### Design

```cpp
class CodecStage {
 public:
  virtual ~CodecStage() = default;
  virtual Expected<DecodedFrame> decode(const DecodedFrame& input) const = 0;
};
```

A `CodecPipeline` is a `vector<unique_ptr<CodecStage>>`. Each stage
consumes raw bytes and either:
- Returns a `DecodedFrame` with `PixelFormat` set → terminal stage,
  pipeline stops.
- Returns a `DecodedFrame` where `pixels` contains intermediate
  bytes → next stage consumes `pixels->data()` and `pixels->size()`.

The last stage must produce display-ready pixels (RGB/RGBA). The
`ImagePipelineSource` receives one of three decode routes: a parser
instance that returns canonical `sdk::Image`; the `CanonicalImageCodec`
tag for topics whose `metadata_json` declares `image_codec=pj_image_v1`
(each ObjectStore entry is a serialized `sdk::Image` blob the source
deserializes itself — the toolbox image-producer contract); or a
demo/test pipeline. Canonical images (parser- or blob-sourced) share one
decode path: raw/Bayer encodings (incl. grayscale-PNG-wrapped buffers)
reinterpret at the logical geometry, everything else runs the
jpeg/png/auto compressed cascade.

### Codec inventory

Source envelopes such as ROS CDR are decoded by parser plugins before the
application consumes media. The reference CDR helpers live under
`pj_scene2D/tests` (exercised by `cdr_to_image_annotation_test`).

**Image codecs** (bytes → pixels):

| Codec | Input | Output |
|-------|-------|--------|
| `JpegCodec` | JPEG bytes | RGB888 pixels |
| `PngCodec` | PNG bytes | RGB888 or RGBA8888 or Mono16 pixels |

**Visualization codecs** (pixels → display pixels):

| Codec | Input | Output |
|-------|-------|--------|
| `Mono16ToGrayscale` | Mono16 image values | RGB888 (grayscale, non-zero range normalized; zero stays black) |
| `SegmentationPalette` | Mono8 class IDs | RGB888 (color per class) |
| (passthrough, conceptual) | Any RGB/RGBA | No-op |

These are all built-in classes in `pj_scene2d_core`, not plugins.
Adding a new codec requires a code change — same rule as before.

### 4.1 Video: streaming decode path

The host has a single video decode path: per-frame `VideoFrame` topics
decoded out of `ObjectStore`. There is no file-backed video decoder in
the host.

**`StreamingVideoDecoder`** (streaming / ObjectStore-based): decodes
VideoFrame entries from ObjectStore, codec-generically (H.264/HEVC/AV1).
Described in §4.4. Wrapped by `StreamingVideoSource` (§5.3) for
`MediaSource` integration. Uses `FfmpegDecoder` for decode, `CancelToken`
for cooperative cancellation, and a latest-wins request model (no
partial publication).

- **EntryThumbnailCache** (`entry_thumbnail_cache.h`): the
  streaming/`VideoFrame` thumbnail cache. A background builder runs a
  single forward decode pass and surfaces ~1 frame per adaptive interval
  through its own per-build NAL extractor (a single-thread keepalive slot
  separate from the playback decoder), encoding each via the stateless
  `thumbnail_codec.h` (downscale to ≤1280px, YUV420P, JPEG quality 80,
  adaptive tile/byte budget). The pass decodes every frame but pays the
  HW-download + downscale only on the surfaced ones
  (`StreamingVideoDecoder::decodeSampled` → `FfmpegDecoder::decodeFiltered`),
  which is what keeps 4K thumbnailing tractable. `StreamingVideoSource`
  serves the nearest-at-or-before thumbnail on scrub for instant feedback
  while the GOP decode settles.
  Wired only for bounded topics today; streaming-on-pause thumbnailing
  is not yet wired.
- **YUV420P / NV12 output** (§R4.7 compliant): FfmpegDecoder outputs
  planar YUV420P (software) or native NV12 (hardware) directly. No CPU-side
  RGB conversion. MediaViewerWidget renders via a colorspace-aware fragment
  shader (matrix per frame) with R8 planes. 75% GPU memory reduction vs RGBA8.

### 4.2 Image codecs

`JpegCodec` and `PngCodec` are `CodecStage`
implementations. They are stateless — multiple instances per widget
are fine. `ImageDecodeCascade` auto-dispatches JPEG vs PNG, and
`AutoImageCodec` adds Mono16 normalization for display. Raw canonical image
buffers are wrapped directly by `ImagePipelineSource` before entering the
pipeline; there is no separate raw `CodecStage`.

No decoded-frame cache. On-the-fly decode is fast enough for stills
that caching wastes more memory than it saves time (§R4.2).

### 4.3 SceneDecoder

pj_scene2D consumes its local `ISceneDecoder` abstraction from
`pj_scene2d_core/scene_decoder.h`. The factory `makeSceneDecoder(schema_name)`
dispatches on the schema string across two concrete `ISceneDecoder` kinds:
`kSchemaImageAnnotations` (`"PJ.ImageAnnotations"`, the PJ-canonical
`ImageAnnotations` re-encoding) maps to `ImageAnnotationsSceneDecoder`, and
`kSchemaSceneEntities` (`"PJ.SceneEntities"`) maps to `SceneEntities2DDecoder`
(implemented in `pj_scene2D/core/src/scene_entities_2d_decoder.cpp`, which
projects `sdk::SceneEntities` into 2D annotations). An unrecognized schema
returns `nullptr`.

`SceneEntities2DDecoder` is intentionally a stateless per-message projection,
not a retained scene graph. It supports the seven primitive lists that can be
projected with the current 2D annotation types — arrows, cubes, spheres, lines,
triangles, texts, and axes. Cylinders and models are not projected today. Each
message produces a fresh snapshot: deletions, `lifetime_ns`, and `frame_locked`
are not applied, and `pose.orientation` is ignored; projection uses only XY
translation from `pose.position`.

Wire format spec, type catalog, and encoding rules live in
`plotjuggler_sdk/pj_base/include/pj_base/builtin/image_annotations.hpp` and
`plotjuggler_sdk/pj_base/include/pj_base/builtin/image_annotations_codec.hpp`.

`ISceneDecoder` exposes **two** entry points; `ScenePipelineSource` (see §5)
picks by ingest route:
- `decode(bytes)` — canonical-producer topics: the store holds canonical bytes,
  decoded as-is.
- `decode(const sdk::BuiltinObject&)` — parser-backed topics: a `MessageParser`
  already produced the canonical object, decoded directly (no serialize/
  deserialize round-trip, mirroring the 3D consumer).

**pj_scene2D's usage policy:** stateless decoder, one instance per
scene/annotation layer for the layer's lifetime. `ScenePipelineSource` owns the
decoder; its output is a `SceneFrame` ready for the compositor (§5.4) to merge
with the base image and hand to the renderer (§7).

**Source-format conversion is loader-side, not pj_scene2D's concern.**
Per-source-format adapters (CDR `vision_msgs/msg/Detection2DArray`, CDR
`yolo_msgs/msg/DetectionArray`, future CSV/RLDS, …) live next to each
loader; PJ4's reference adapters are in
`pj_scene2D/tests/cdr_*_to_image_annotation.{h,cpp}` (with
`pj_scene2D/tests/marker_palette.{h,cpp}` for the FNV-1a class-id →
palette helper). They call `PJ::serializeImageAnnotation` and push the
resulting canonical bytes to ObjectStore tagged with
`metadata_json = {"encoding":"foxglove.ImageAnnotations"}`. The viewer
side never sees the original schema.

### 4.4 StreamingVideoDecoder

Decodes VideoFrame entries stored in ObjectStore, codec-generically:
the codec (H.264/HEVC/AV1 — those with both a decoder and a keyframe
oracle) is read from each topic's `VideoFrame.format` and mapped to an
FFmpeg decoder (`video_codec_utils.h`). Any other codec (e.g. VP9, which
has a decoder but no keyframe oracle, so it could not be sought) surfaces
a clear "unsupported video codec" error instead of mis-decoding.
`StreamingVideoDecoder` reads encoded NAL/OBU units from ObjectStore
entries — the path for streaming sources (ROS 2, RTSP, etc.) that
push VideoFrame messages into ObjectStore at ingest time.

The streaming case reads from ObjectStore, has dynamic duration
(retention window), and no file path. `StreamingVideoDecoder` lives in
`pj_scene2d_core` with no Qt dependency.

**API:**

```cpp
class StreamingVideoDecoder {
 public:
  void attach(ObjectStore* store, ObjectTopicId topic);
  Expected<DecodedFrame> decodeAt(Timestamp ts);
  void reset();
  bool isInitialized() const;
};
```

**Two-path decode strategy:**

1. **Forward path** (live mode and forward scrub): when the target
   timestamp is at or ahead of the last decoded position, the decoder
   continues forward without flushing. Uses `FfmpegDecoder::decodeSkip()`
   for intermediate frames and `decode()` only for the target.
   This is O(1) per frame in live mode (one decode per call).

2. **Seek path** (backward scrub, cross-GOP jump, first decode): finds
   the nearest keyframe before the target in the keyframe index, flushes
   the decoder, and decodes forward from the keyframe to the target.

**Same-timestamp cache:** When the display polls faster than the push
rate (e.g., 60 Hz display vs 30 Hz push), `decodeAt()` is called with
the same timestamp twice. The cached `last_frame_` is returned
immediately — no re-decode, no seek.

**Keyframe index:** Built incrementally by inspecting each new entry via the
codec-dispatched `isVideoKeyframe(codec_id, data, size)` (`video_codec_utils.h`):
H.264 IDR (NAL type 5), HEVC IRAP (NAL types 16–21), AV1 (carries a
Sequence-Header OBU). `resolveCodec()` latches the topic's codec from the first
extracted frame's `VideoFrame.format` before the oracle runs. Tracked by
`last_scanned_ts_` to handle the steady-state case where retention keeps
`entryCount()` constant while entries are replaced. Evicted keyframe timestamps
are pruned against `timeRange().first` on each update.

**Decoder initialization:** Deferred until the first keyframe arrives
(join-mid-stream support). The decoder opens via `makeVideoCodecParams(codec_id)`
— `codec_id` resolved from `VideoFrame.format` by `videoCodecIdFromFormat()` —
with **no `extradata`**: the streaming wire contract requires every keyframe to
carry its parameter sets in-band (H.264 SPS/PPS, HEVC VPS/SPS/PPS, AV1 sequence
header), which FFmpeg ingests on the first keyframe. HW acceleration (VAAPI) is
chosen by `FfmpegDecoder`'s `get_format` callback, with automatic software
fallback when the codec/GPU offers no HW config.

**Eviction resilience:** In live mode, the original keyframe may be
evicted by retention while the decoder continues forward. The forward
path does not require the keyframe — the decoder already has the
correct codec state from previous sequential decodes. Only backward
seeks require a keyframe still present in the store.

### 4.5 Video Pipeline Summary

Which component to use depends on the data source:

| Scenario | Component | ObjectStore role | Notes |
|----------|-----------|-------------|-------|
| Streaming VideoFrame (ROS 2, RTSP) | StreamingVideoDecoder | Encoded packets with retention budget | One ObjectStore entry per `sdk::VideoFrame`. |
| File-based MCAP with CompressedVideo | StreamingVideoDecoder | Lazy-fetched encoded packets | DataSource pushes encoded packets at open time. |
| ML datasets (LeRobot, RLDS) | StreamingVideoDecoder | One `sdk::VideoFrame` per camera frame | A loader transcodes each camera's MP4 into per-frame `VideoFrame` entries; Parquet scalars go to DataEngine. Episodes map to DatasetId. |

The host's canonical video model is **per-frame `VideoFrame`**: each
ObjectStore entry holds one encoded frame, decoded GOP-aware by
`StreamingVideoDecoder`. There is no file-backed video decode path in the
host. A producer that wants to surface an MP4 transcodes it to per-frame
`VideoFrame` entries at ingest time.

**Multi-modal datasets** (video + scalars from the same recording): the
DataSource plugin populates both stores — `DataEngine` for plottable
time-series and `ObjectStore` for media. Both share the same timestamp
domain and are driven by the same global timeline cursor. Episodes in ML
datasets (e.g., LeRobot episodes) map to PlotJuggler's `DatasetId` —
each episode is a separate dataset with its own time range.

---

## 5. MediaSource

The uniform frame-delivery interface between decoder backends and
`MediaViewerWidget`.

```cpp
class MediaSource {
 public:
  virtual ~MediaSource() = default;
  virtual void setTimestamp(int64_t ts_ns) = 0;
  virtual std::optional<MediaFrame> takeFrame() = 0;
};
```

**Design rationale**: `PlaybackController` was a monolithic orchestrator
that would have owned decoders, worker threads, frame mailboxes, compositor,
and CancelToken management. This conflicted with the streaming decode
path (`StreamingVideoSource`), which is already a self-contained
subsystem (owns its worker thread, latest-wins request model,
cancellation, thumbnail cache). `MediaSource` is a thin adapter that
lets each decoder path manage its own complexity at the right granularity.

**Contract**:

- `setTimestamp(ts_ns)` is called by the main thread when the global
  time changes. May decode synchronously or post to an internal worker.
- `takeFrame()` is called by the main thread at render rate. Returns
  the latest `MediaFrame` (composited base + ordered `pixel_layers` +
  vector overlays), or nullopt/empty if nothing new since last call.
- No `cancel()` in the public interface — each implementation manages
  cancellation internally when a new `setTimestamp` arrives.
- The widget calls `update()` after `setTimestamp()` to trigger repaint.

### 5.1 ImagePipelineSource

Wraps `CodecPipeline` + `ObjectStore`. Decodes on a dedicated worker
thread (mirrors `StreamingVideoSource`): `setTimestamp()` posts a request and
returns immediately; `takeFrame()` polls the latest decoded result.

```cpp
class ImagePipelineSource : public MediaSource {
 public:
  ImagePipelineSource(ObjectStore* store, ObjectTopicId topic,
                      std::unique_ptr<CodecPipeline> pipeline);
  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
};
```

Internals: `setTimestamp` forwards to the composed `AsyncFrameWorker` (§3.1),
which coalesces targets latest-wins. The decode body runs on the worker
thread: `store->latestAt(topic, ts)` → decodes → optionally rectifies
(`rectifyIfCalibrated`: when `setCameraInfoMap()` provided a `CameraInfo` for
the frame's `frame_id` *and* the per-layer rectify toggle is on —
`setRectifyEnabled`, default on, the operator's override for the always-on
auto-decision; see TECHNICAL_NOTES "rectify the image, not warp the
annotations" and "ASSUMPTION and its known blind spot") → `deposit()`s into the
worker's mailbox. `takeFrame` drains the
mailbox into a `MediaFrame` (nullopt on second call); after each deposit the
worker fires the optional `setFrameReadyCallback` (from the worker thread) so
consumers re-poll.

`rectifyIfCalibrated` chooses between two paths based on the
`setGpuRectificationAvailable` flag the widget pushed down (atomic; a change
calls `invalidate()` to re-decode in the new mode):

- **GPU available (default once the widget confirms):** the worker leaves the
  frame RAW and attaches the cached `shared_ptr<const UndistortMap>` to
  `DecodedFrame::rectify_map`. The widget undistorts at draw time (§7.2). No CPU
  resample, smaller upload.
- **GPU unavailable (safe default until confirmed):** the worker rectifies on
  the spot with `rectifyFrameFast` (a fixed-point `UndistortMapFast` into a
  reused buffer) and delivers a display-ready native-resolution frame with
  `rectify_map == nullptr`.

Either way the *displayed* coordinate space is the camera's native (calibrated)
resolution, so annotations / aspect / pixel-inspector stay aligned (§7.2).

Cancellation is left off for this source (stale targets are coalesced rather
than cancelled mid-decode — image decodes are short). `setCameraInfoMap()`
must be called before the first `setTimestamp()` — the worker reads the
calibration map without further locking.

### 5.3 StreamingVideoSource

Wraps `StreamingVideoDecoder` + owns a worker thread.
`StreamingVideoDecoder::decodeAt()` is synchronous and can be expensive
(seek + decode forward from keyframe), so it runs on a dedicated worker.

```cpp
class StreamingVideoSource : public MediaSource {
 public:
  StreamingVideoSource(ObjectStore* store, ObjectTopicId topic);
  ~StreamingVideoSource();

  void setTimestamp(int64_t ts_ns) override;
  std::optional<MediaFrame> takeFrame() override;
  bool isInitialized() const;
};
```

Internals:
- `setTimestamp` forwards to the composed `AsyncFrameWorker` (§3.1), started
  with cancellation enabled and a scrub-only preempt predicate: a newer
  request cancels the in-flight `CancelToken` only on a backward / large-
  forward jump (teardown always cancels), so a slow 4K GOP decode is abandoned
  for scrubs while contiguous-playback ticks let it finish and ride the
  forward continuation; `decodeAt`/`decodeRange` poll the token to preempt.
- The decode body (`decodeRequest`) publishes an instant thumbnail preview on
  scrubs, then calls `decoder_->decodeAt(ts, token)` and `deposit()`s the
  full-res result into the worker's mailbox.
- `takeFrame` drains the mailbox into a `MediaFrame`.
- After depositing a frame the worker fires the optional
  `setFrameReadyCallback` (from the worker thread) so consumers re-poll
  `takeFrame()` once the async decode completes. Without it a stopped scrub
  freezes on the previous frame: the GUI's single `update()` per tracker tick
  races the worker and the finished frame is never re-polled. The layer
  (`VideoLayer`/`ImageLayer`) hops to the main thread via a queued invocation.
  Mirrors `ImagePipelineSource` (§5.1).

### 5.4 Multi-layer

`CompositeMediaSource` (`pj_scene2d_core/composite_media_source.h`) owns
multiple `MediaSource` instances and fuses their `MediaFrame`s on each
`takeFrame()`. Same `MediaSource` interface — the widget remains agnostic
of the layer count.

The output of `takeFrame()` is a single `MediaFrame`:

```cpp
struct MediaFrame {
  std::optional<DecodedFrame> base;        // pixel-buffer layer (image/video)
  std::vector<PixelLayer> pixel_layers;    // ordered pixel buffers, bottom to top
  std::vector<SceneFrame> overlays;        // vector primitive layers
};

struct PixelLayer {
  DecodedFrame frame;
  float opacity = 1.0f;
};
```

Fusion rules (see implementation):
- The first layer that produces a `.base` wins; later bases dropped.
- Every layer's `.overlays` are concatenated in addition order (later
  layers render on top).
- Returns `nullopt` if no layer produced data on this poll.

Layers are owned by the compositor (`std::unique_ptr<MediaSource>`).
Polling is deterministic (addition order), making z-order configuration
explicit at construction time.

---

## 6. MediaIndexRegistry — *designed, not yet implemented*

> **Status (as of 2026-05-19): the `MediaIndexRegistry` described in this
> section is a design artefact. No `media_index_registry.h` exists in
> `pj_scene2D/core/include/pj_scene2d_core/`, and no public C ABI slot
> for `publish_keyframe_index` exists in `pj_base`. The realised keyframe
> indexing today lives inside the streaming decoder:**
> - **Streaming sources** — `StreamingVideoDecoder` (§4.4) maintains its
>   own inline keyframe vector via incremental NAL inspection. This is the
>   host's only video decode path; there is no file-backed decoder.
>
> The registry is the design for a future **file-backed ObjectStore**
> path (raw bytes pushed via `pushLazy`, but the decoder needs random
> access by keyframe rather than by file offset). That path does not
> exist yet; until it does, this section is preserved as the
> design-of-record so the work can be picked up without re-deriving the
> contract.

Keyframe tracking lives in pj_scene2D, not in ObjectStore (§R4.2,
§R4.4). The `MediaIndexRegistry` is the proposed sidechannel that
would hold per-topic keyframe timestamp lists.

```cpp
class MediaIndexRegistry {
 public:
  void registerIndex(ObjectTopicId topic,
                     std::vector<int64_t> keyframe_timestamps);

  void appendKeyframe(ObjectTopicId topic, int64_t timestamp);

  std::optional<int64_t> keyframeBefore(ObjectTopicId topic,
                                         int64_t timestamp) const;

  void removeIndex(ObjectTopicId topic);
};
```

**Keying**: entries are keyed by `ObjectTopicId`, not topic name
strings — consistent with the rest of the system.

**Lifecycle**: entries are cleared when the corresponding topic is
removed from ObjectStore (`removeTopic`, `clear`). The application
is responsible for calling `removeIndex()` as part of topic teardown.
Widgets observing a removed topic will find no keyframe index and
fall back to sequential decode (no seeking).

### Two population modes

**File-backed sources**: the DataSource plugin pre-computes the
keyframe list at open time — by scanning NAL start codes (H.264/H.265),
reading the MP4 `stss` (sync sample) atom, or equivalent. It publishes
the sorted timestamp array via the C ABI slot
`object_write_host.publish_keyframe_index(topic, timestamps, count)`.
The host receives the array and calls `registerIndex()` on the
application-owned `MediaIndexRegistry`. The DataSource plugin never
links `pj_scene2d_core` — communication is through the C ABI write host
only. This is a one-time cost at file open, amortized over all
subsequent seeks.

**Streaming sources**: the decoder builds the index incrementally.
On each new entry it inspects the first few bytes to detect keyframes.
`StreamingVideoDecoder` (§4.4) manages its own inline keyframe
timestamp vector rather than using `MediaIndexRegistry` — this is
simpler for the single-consumer case and avoids cross-component
coupling. The per-entry cost is negligible: the codec-dispatched
`isVideoKeyframe(codec_id, …)` (H.264 IDR / HEVC IRAP / AV1 seq-header
OBU) touches at most the first ~20 bytes of each entry regardless of
frame size.

### Usage by the future file-backed decoder

When the future file-backed decoder needs to seek to timestamp `T` (using the
`ObjectTopicId` it was constructed with):

1. `registry.keyframeBefore(topic_id, T)` → returns `kf_ts`.
2. `ObjectStore::indexAt(topic_id, kf_ts)` → gets the keyframe's index
   `i` in the entry sequence.
3. `ObjectStore::at(topic, i)` → gets the keyframe bytes. Decode it
   after calling `avcodec_flush_buffers` to reset decoder state.
4. Iterate `ObjectStore::at(topic, i+1)`, `at(topic, i+2)`, ...
   decoding each subsequent frame (using `decodeSkip` for intermediates)
   until reaching the entry at timestamp `T`.

Because live and scrub modes are mutually exclusive (§R4.3), the buffer
is frozen during this seek — no entry can be evicted between steps 2
and 4.

### Thread safety

`MediaIndexRegistry` is protected by its own `shared_mutex`.
`registerIndex` and `appendKeyframe` take exclusive locks;
`keyframeBefore` takes a shared lock. The registry is independent of
`ObjectStore`'s locking.

---

## 7. Rendering Pipeline

### 7.1 QRhiWidget — five pipelines

`MediaViewerWidget` subclasses `QRhiWidget` (Qt 6.11.1), which abstracts
over Vulkan, Metal, D3D11, and OpenGL at runtime. The widget owns five
QRhi graphics pipelines that share the same `viewTransform` UBO so
zoom/pan/rotation apply uniformly:

| # | Pipeline | Topology | Responsibility |
|---|---|---|---|
| 1 | Image | implicit (procedural fullscreen quad) | YUV420P/NV12 → RGB (colorspace-aware matrix per frame; 3 R8 or R8+RG8 textures), RGBA passthrough, native Mono8 (R8) / BGRA (RGBA8 swizzle), depth colormap (R32F depth + colormap LUT, `pixelFormat == 5`), and optional GPU lens-rectification via a remap LUT (§7.2) |
| 1b | Composite (pixel layers) | implicit (procedural fullscreen quad) | Alpha-blends N additional `MediaFrame::pixel_layers` over the base, each with its own SRB and per-layer `opacity`; used when `pixel_layers_active_` (member `composite_pipeline_`) |
| 2 | Fills (`points_overlay_`) | `Triangles` | Solid fills: `kPoints` quads, `LineLoop` fill, `CircleAnnotation` fill |
| 3 | Outlines (`thick_overlay_`) | `Triangles` | **All** line/circle strokes, expanded CPU-side to perpendicular rectangles whose width scales with zoom but is floored at 1px on screen |
| 4 | Text | `Triangles` (textured) | One quad per `TextAnnotation`, glyph mask sampled and tinted by per-vertex colour |

Pipelines 2–4 share `marker_uniform_buf_` (the same `mat4 viewTransform + vec4 frameSize` UBO) but each has its own SRB and VBO so submissions don't trample each other's bindings.

**Stroke width (scale-with-floor).** Stroke geometry lives in the backend-agnostic
`pj_scene2d_core/overlay_geometry.h` (unit-tested in `overlay_geometry_test.cpp`).
Outline/point widths are in **image pixels**, so they scale with zoom (the view
transform enlarges them as you zoom in), but the CPU expander **floors** the
half-width so the stroke is never thinner than 1 px on screen. The floor is a
screen-pixel value, converted to image units via `image_px_per_screen_px`
(= 1 / effective view scale), so geometry is re-expanded when the view scale
changes. This fixes the old bug where pure image-space strokes went sub-pixel when
zoomed out — with no MSAA, edges then dropped out of rasterization depending on
sub-pixel alignment → "some rectangle/cube lines disappear depending on zoom".
The earlier native `GL_LINES` "Marker" pipeline (1 px, `thickness ≤ 1.5`) was
**retired**: GL line clipping is guard-band-limited (whole segments can be culled
when zoomed far in), whereas triangles clip robustly and honour thickness.

Per-frame flow:

1. Build `MediaFrame` via the attached `MediaSource`. Pixel data goes to texture upload; vector overlays go to CPU vertex builders.
2. Compute `effective_scale` (on-screen px per image px = zoom × aspect-preserving fit). Stroke/point geometry is re-expanded when the annotation set changes **or** `effective_scale` changes (since the 1px screen floor depends on it); fills and text are image-space and rebuild only on annotation change. Walk `last_overlays_` and dispatch each `PointsAnnotation`/`CircleAnnotation` to `overlay_geometry::{appendLineStrokes, appendPointQuads, appendLoopFill, appendCircleStroke, appendCircleFill}`.
3. For each `TextAnnotation`, look up `(text, font_size_q)` in `text_cache_`. On miss, render a glyph mask with `QPainter` to a `QImage::Format_Alpha8`, upload as a `QRhiTexture::R8`, and create a per-entry SRB pointing at it (so per-draw rebinding cannot mix textures across instances).
4. Issue draw calls in order `image → fills → outlines → text` so strokes always land on top of fills and labels on top of everything.

### 7.2 Image shader: color conversion, native formats, GPU rectification

The image fragment shader (`yuv_to_rgb.frag`) branches on a `pixelFormat`
uniform. YUV420P uses 3 R8 samplers (NV12 uses 2: an R8 Y plane + an RG8
interleaved UV plane):

```glsl
vec3 yuv = vec3(
    texture(y_plane, uv).r,
    texture(u_plane, uv).r - 0.5,
    texture(v_plane, uv).r - 0.5
);
fragColor = vec4((colorMatrix * vec4(yuv, 1.0)).rgb, 1.0);
```

The `colorMatrix` is **selected per frame** from the codec's signalled colorimetry
(BT.601 vs BT.709, limited vs full range) and built by
`pj_scene2d_core/video_color.h::buildYuvMatrix` — a single affine 4×4 that folds the
range scale/offset into the constant column, so the shader is identical across all
combinations. BT.709 + full range reproduces the legacy hardcoded matrix exactly.
Live-decoded frames and cached thumbnails pass through the same shader. (Earlier the
matrix was a hardcoded full-range BT.709, which mis-rendered SD and limited-range
content — see TECHNICAL_NOTES §5 "Colorimetry was dropped".)

**Native packed formats (no CPU repack).** Besides RGBA passthrough, the shader
handles **Mono8** (uploaded as a single R8 texture, expanded to gray) and
**BGRA8888** (uploaded verbatim to an RGBA8 texture, swizzled `.bgra`). Only
RGB888/BGR888 (3-byte, no clean 4-byte-row GPU layout) still expand to RGBA on
the CPU. This removes the per-frame `rgba_repack_buffer_` copy for the mono/BGRA
cases.

**GPU rectification (lens undistortion).** When the source delivers a RAW frame
with a `DecodedFrame::rectify_map` (the GPU path; see §5.1), the shader remaps
the output UV through a per-camera lookup texture before sampling:

```glsl
if (rectify == 1) {
    vec2 s = texture(remap_tex, v_uv).rg;   // RGBA32F LUT, NEAREST
    if (s.x < 0.0) { fragColor = vec4(0,0,0,1); return; }  // OOB sentinel
    uv = s;                                  // sample the image at the source coord
}
```

- The LUT (`remap_tex`, SRB binding 4) is `out_w × out_h` RGBA32F, built once per
  camera by `undistortMapToNormalizedRG` and cached on the layer (keyed by map
  identity). `.rg` is the normalized source coordinate with a +0.5 half-texel
  offset so the LINEAR image fetch reproduces the CPU bilinear; the LUT itself is
  sampled NEAREST so each output pixel gets its exact precomputed source coord.
- **Logical-size decouple:** on the GPU path the *uploaded* texture is the small
  RAW frame, but the widget reports the *logical* size as the map's
  `out_width/out_height`. `tex_width_`/`tex_height_`/`frame_aspect_` and the
  overlay `frameSize` UBO all use the logical size, so annotations, aspect
  letterboxing, and the pixel inspector stay in the camera's native-resolution
  space regardless of CPU-vs-GPU path. The point inspector maps the cursor in
  logical space and reads the source pixel through the map.
- **Capability handshake:** `MediaViewerWidget::initialize()` probes
  `isTextureFormatSupported(RGBA32F)` (plus successful base-pipeline creation) and
  calls `MediaSource::setGpuRectificationAvailable(...)`, fanned out through
  `Composite`/`BorrowedMediaSource` to the `ImagePipelineSource` layers. The
  default is the CPU-correct path until the widget confirms GPU support; a flip
  re-decodes the current frame. Non-rectifying layers bind a 1×1 placeholder LUT
  at slot 4 so all texture-layer SRBs share one layout.

This moved lens undistortion off the decode worker threads (it was ~63–68% of
process CPU) onto a single GPU texture lookup; see TECHNICAL_NOTES
"GPU vs CPU rectification".

### 7.3 Zoom and pan

A 3x3 view transform matrix in the vertex shader handles zoom
(mouse-wheel, cursor-anchored) and pan (mouse drag):

```glsl
gl_Position = vec4(view_matrix * vec3(in_position, 1.0), 1.0);
```

The transform is updated on mouse events and stored as widget state.
No pixel reprocessing — transformation is free on the GPU.

### 7.4 Software fallback

When `QRhi` reports no suitable GPU backend, or when the platform
lacks GPU support:

1. Decoder falls back to software decode (guaranteed by
   `FfmpegDecoder`'s fallback logic).
2. CPU-side YUV→RGB conversion via sws_scale or manual matrix
   multiply.
3. Upload RGB pixels to a `QImage` and render via `QPainter`.

Acceptable degradation; the UX remains functional.

---

## 8. Multi-Layer Compositor

A viewer widget may composite multiple ObjectStore topics at the same
display time (§R4.8). The `CompositeMediaSource` class orchestrates this (§5.4).

### 8.1 Layer model

Each layer is a `MediaSource` registered with the composite. Layer types
that pj_scene2D renders today (image-pixel space only — see REQUIREMENTS §4.1):

| Layer type | Source | Output in `MediaFrame` |
|---|---|---|
| Base image/video | `ImagePipelineSource`, `StreamingVideoSource` | `.pixel_layers` (RGB/YUV pixel buffer; `.base` kept as the legacy single-layer fallback) |
| Vector annotations (`ImageAnnotation`) | `ScenePipelineSource` | `.overlays` (typed primitives — points, line loops/strips/lists, circles, texts) |
| Depth colormap | `DepthPipelineSource` / `DepthImageLayer` (routed from a depth-encoded `kImage` topic by peeking the first sample's `encoding`; a dormant `kDepthImage` registration also exists, but no producer emits that type) | `.pixel_layers` (a raw `PixelFormat::kDepthR32F` float frame + `DepthColorParams`; the per-layer turbo/viridis/plasma/grayscale colormap, near/far range, and invert are applied on the GPU in the media shader — no CPU colormap) |
| Segmentation mask (planned) | `ImagePipelineSource` with `SegmentationPalette` codec | `.pixel_layers` (not yet registered as a layer type) |

### 8.2 Compositing pipeline

`CompositeMediaSource` (§5.4) owns one `MediaSource` per layer. On each tick:

1. Calls `setTimestamp()` on every layer.
2. Calls `takeFrame()` on every layer.
3. Returns one `MediaFrame` where the first non-null `.base` wins and every layer's `.overlays` are concatenated in addition order.

The widget consumes the fused `MediaFrame` and dispatches:

- `.base` → texture upload + image pipeline.
- `.overlays` → CPU expansion to vertex streams (see §7.1) and the four overlay pipelines (Lines, Points/Triangles fills, Thick triangles, Text textured).

Pixel-layer fusion (multiple pixel buffers blended in pixel space — e.g. RGB base + depth colormap) **is implemented**. `CompositeMediaSource` collects each layer's ordered `pixel_layers` (per-layer opacity, scaled by the layer's composite opacity) into a single `MediaFrame.pixel_layers` stack, and `MediaViewerWidget` renders them via a dedicated alpha-blended `composite_pipeline_` (SrcAlpha/OneMinusSrcAlpha), one draw call per uploaded `pixel_layer_textures_` entry, when `pixel_layers_active_`. The legacy single-`base` path is retained for producers that don't populate `pixel_layers`.

### 8.3 At-or-before semantics

Every layer query uses `latestAt` — strict at-or-before, never a
future entry (§R4.8). A 10 Hz detection overlay composited on a
60 Hz video is correct: the same bounding box persists across
multiple video frames until replaced by a newer annotation.

No automatic skew rejection. Multi-rate is the norm — the compositor
trusts that an entry exists in the store until explicitly evicted.

### 8.4 frame_id correlation (optional)

When an overlay's metadata carries a `frame_id` matching a specific
source image, the compositor may prefer exact pairing over
timestamp-based pairing. Layers without `frame_id` fall back to
timestamp matching.

---

## 9. Clock Integration

### 9.1 Main-thread-driven timestamps

There is no `TimelineCursor` subscription or callback model in
`pj_base`. Instead, widgets implement `PJ::IDataWidget` (from
`pj_runtime/IDataWidget.h`) and are driven by `pj_runtime::PlaybackEngine`
on the Qt main thread. The runtime calls `widget->onTrackerTime(time)`
on each tick; the widget forwards that time into its internal
`MediaSource` and triggers a repaint:

```cpp
// Inside the widget, on the Qt main thread:
void SceneDockWidget::onTrackerTime(double time) {
  const auto clamped = clampToLayerRange(/* seconds → ns via chrono, dropping NaN/inf */);
  // Per-tick repaint coalescing: if the combined renderKey() of all visible layers
  // is unchanged from the last painted frame (same active sample at this time),
  // nothing visible moved — skip the layer advance AND the repaint. A 60 Hz tracker
  // over a <10 Hz image stream therefore does NOT re-composite the dock every tick.
  const uint64_t key = trackerRenderKey(clamped);
  if (have_render_key_ && key == last_render_key_) {
    return;
  }
  have_render_key_ = true;
  last_render_key_ = key;
  for (auto& [k, layer] : layers_) {
    if (layer != nullptr && layer->info().visible) {
      layer->setTrackerTime(clamped);  // each layer forwards into its MediaSource
    }
  }
  refreshView();  // triggers repaint; render() pulls latest frame via takeFrame()
}
```

`Scene2DLayer::renderKey(time)` fingerprints the active media sample's stamp (a
stamp-only `indexAt`/`entryTimestamps` lookup, no decode), so a static image
coalesces while a new frame reopens the gate. A new sample's async decode also
repaints via the frame-ready callback (`repaintRequested → refreshView`),
independent of this gate — so the gate can never strand a freshly-decoded frame.

The contract property of the original `TimelineCursor` design — *widgets
never own or drive the clock* — is preserved: widgets only **receive**
time on `onTrackerTime`, never advance it.

This matches how PlotJuggler's existing plot widgets are driven — the
main thread iterates over widgets and tells each one to update. Media
widgets are passive; they never drive the clock.

### 9.2 Rate hints

DataSource plugins publish optional hints at startup:

- `preferred_fps`: natural frame rate of the source (e.g., 30.0 for
  a 30 fps video).
- `natural_range_ns`: total duration of the dataset.

The application's clock aggregates hints across sources and picks a
default playback pace. The user may override.

### 9.3 Live vs scrub mode

The application manages the live/scrub mode transition (§R4.3).
From pj_scene2D's perspective:

- **Live mode**: the main thread calls
  `widget->setTimestamp(timeRange().second)` on each tick.
  Each `ObjectStore::latestAt` returns the most recent entry.

- **Scrub mode**: the main thread calls
  `widget->setTimestamp(slider_value_ns)` driven by user interaction.
  The buffer is frozen — no pushes, no eviction.

The `MediaSource` does not know or care which mode is active. It
reacts identically: receive timestamp → decode → deliver frame. The
mode distinction is entirely in whether the timestamp advances
automatically or manually, and whether the DataSource is actively
pushing.

---

## 10. Threading Model

### 10.1 Thread roles

| Thread | Responsibilities | Lock discipline |
|--------|-----------------|-----------------|
| **Qt main thread** | UI events, `widget->setTimestamp()`, `widget->render()` → `source->takeFrame()`, GPU upload | Posts async requests for image/video/depth sources. Only the scene source decodes synchronously in `setTimestamp()`, so only that path may spend decode time on the GUI thread |
| **ImagePipelineSource / DepthPipelineSource worker** (an `AsyncFrameWorker`, 1 per source) | ObjectStore lookup, parser/canonical-image handling, `CodecPipeline` decode (or the depth R32F decode incl. compressedDepth PNG inflate), result deposit | The shared `AsyncFrameWorker` engine: `request_mutex_`/`request_cv_` for latest-target requests, `result_mutex_` for the mailbox. ObjectStore locks are released before codec work |
| **StreamingVideoSource worker** (an `AsyncFrameWorker`, 1 per source) | `StreamingVideoDecoder::decodeAt()`, thumbnail preview + full-res `deposit()` into the worker mailbox | Acquires ObjectStore shared locks (released immediately after handle copy). Holds decoder-internal state exclusively |
| **DataSource poll thread** (1 per app, existing) | `DataSource::poll()` → `ObjectStore::pushOwned/pushLazy` | Acquires ObjectStore exclusive locks per push. Never touches decoders |
| **EntryThumbnailCache builder** (1 per cache, bounded topics only) | Single forward decode pass producing HD-capped JPEG scrub thumbnails | Owns its own decoder/extractor; publishes tiles under `EntryThumbnailCache::mutex_`; never shares the playback decoder |

### 10.2 Lock inventory

| Lock | Type | Protects | Held by |
|------|------|----------|---------|
| `ObjectSeries::mutex` (§OS3.4) | `shared_mutex` | Per-topic entry storage | Shared: image/video/depth workers via `latestAt`/`at`/`indexAt`, and the Qt main thread for the synchronous scene `latestAt`. Exclusive: poll thread via `pushOwned`/`pushLazy`/eviction |
| `AsyncFrameWorker::request_mutex_` (one per worker-backed source) | `mutex` | Latest target timestamp, request-present + force-redecode flags, condition-variable predicate; with cancellation enabled (video) also the active cancel token | Main: `setTimestamp()` posts/coalesces requests. Worker: waits, takes one request, clears the flag. Not held during ObjectStore lookup or decode |
| `AsyncFrameWorker::result_mutex_` (one per worker-backed source) | `mutex` | Latest decoded-frame mailbox | Worker: deposit latest frame. Main: `takeFrame()`. Never held while ObjectStore or decoder locks are held |
| `AsyncFrameWorker::callback_mutex_` (one per worker-backed source) | `mutex` | The optional frame-ready callback slot | Main/layer: `setFrameReadyCallback()`. Worker: copies the callback under the lock, invokes it after release |
| `ImagePipelineSource::parser_mutex_` (`shared_ptr<std::mutex>`) | `mutex` | Serializes `MessageParser::parseObject` calls on a parser instance shared with the owning layer's keepalive | Worker: held across each `parseObject`. Shared with the layer that provided the parser binding |
| `EntryThumbnailCache::mutex_` | `mutex` | Thumbnail tile map + byte budget | Builder thread: tile publish. Reader (video worker via `StreamingVideoSource`): `lookup()` |
| `MediaViewerWidget::frame_mutex_` (widgets layer) | `mutex` | `media_source_` pointer, pending frame / pixel-layer staging, `inspector_frame_` | GUI thread at all four sites today (`setMediaSource`, `releaseResources`, `render`'s poll, inspector read); the lock keeps source swap and frame staging atomic |
| `StreamingVideoDecoder` keyframe vector (no lock) | none | In-decoder keyframe timestamps | Accessed only by the streaming decoder's owning worker thread during incremental NAL inspection. A plain `std::vector<Timestamp>` — single-consumer, so no lock exists. Not exposed in the public API. |
| `MediaIndexRegistry::mutex_` *(planned)* | `shared_mutex` | Keyframe index — see §6 deferred-design banner | N/A today: the registry does not yet exist. |

### 10.3 Lock ordering

No nested locking across the live lock families. Each lock is
acquired and released independently — never held simultaneously:

1. Async sources: main thread acquires `request_mutex_`, updates the
   latest target (and cancels the old streaming token when applicable),
   releases, then notifies the worker.
2. Worker wakes, acquires `request_mutex_`, copies the target, clears
   `has_request_`, releases. It does not hold `request_mutex_` during
   ObjectStore access, parser work, or decode.
3. Worker acquires `ObjectSeries::mutex` (shared) through `latestAt`/`at`/
   `indexAt` → copies an owning byte handle or entry metadata → releases.
4. Worker decodes (no ObjectStore or source mutex held). For streaming
   video, keyframe discoveries append to the decoder's **internal**
   keyframe vector (plain `std::vector`, no lock — only the owning worker
   thread touches it). When the planned `MediaIndexRegistry` lands (§6),
   this is the path that will switch to the shared registry instead.
5. Worker acquires its `AsyncFrameWorker`'s `result_mutex_` → stores the complete
   frame → releases.

The main thread acquires the worker `result_mutex_` only through
`takeFrame()`, and never while holding ObjectStore locks. The synchronous
`ScenePipelineSource` acquires the ObjectStore shared lock inside
`setTimestamp()`, copies the entry bytes/handle, releases the store lock, then
decodes and retains a pending frame without `request_mutex_` or `result_mutex_`.
(`DepthPipelineSource` is worker-backed — its compressedDepth PNG inflate is too
heavy for the UI thread — and uses the same `AsyncFrameWorker` locking as
`ImagePipelineSource`; see §10.1.)

### 10.4 Contention analysis

- **ObjectStore shared_mutex**: at typical media frame rates (30-60
  fps), the push thread holds an exclusive lock for ~1 us per entry
  (deque append + timestamp vector append + optional eviction).
  Worker threads and the synchronous scene calls hold shared locks
  for ~1 us per query (binary search + shared_ptr copy). Contention is
  effectively zero.
- **Source request mutex**: held for one timestamp/cancel-token update
  on the main side and one request copy on the worker side. It is never
  held while decoding.
- **Source result mutex**: held for < 1 us on both sides (store is a
  move; take is a move). Zero contention in practice.
- **StreamingVideoDecoder keyframe vector (no lock)**: reads
  (binary search) are O(log k) where k is the number of keyframes.
  Writes are O(1) amortised. The vector is touched only by the decoder's
  own worker thread, so no lock is needed and contention is zero. The future
  `MediaIndexRegistry` (§6) is planned to use a `shared_mutex` with the
  same complexity characteristics; it will only become contended if
  multiple decoders observe the same topic concurrently.

### 10.5 Error propagation across threads

Errors from decoder workers must not propagate as C++ exceptions
across the worker→UI thread boundary (§R5). The error flow:

1. **Worker thread**: decoder methods return `PJ::Expected<DecodedFrame>`
   or similar. If a decoder fails (corrupt data, unsupported codec,
   HW-accel error), the worker catches the failure at the thread
   boundary.
2. **Last-error state**: the `MediaSource` implementation stores the
   error internally. `takeFrame()` returns `nullopt` on error — the
   widget continues displaying the last good frame.
3. **UI thread**: on its next poll, the widget checks the last-error
   state. If set, it renders a visible error indicator (e.g., "decode
   failed" overlay or a no-signal background) on the affected layer.
4. **Recovery**: the error state is cleared when the next successful
   decode completes.

**Missing data is not an error**: when `ObjectStore::latestAt` returns
`nullopt` on a valid topic (e.g., the cursor is before the first
entry), the widget renders a "no data" indicator without raising,
logging, or surfacing an error condition.

### 10.6 EntryTimestampsView and extended locks

`ObjectStore::entryTimestamps()` returns an `EntryTimestampsView` that
holds a `shared_lock` for its lifetime (§OS4). pj_scene2D's
`StreamingVideoDecoder` may use it during seek planning (batch timestamp
access). The lock is shared (readers-only), so it does not block other
decoders, but it blocks the push thread for the view's lifetime.

Rule: any code path that acquires an `EntryTimestampsView` must drop
it promptly — copy timestamps into a local vector if further
processing is needed beyond the search. Never hold a view across a
decode call.

---

## 11. Port Map

What to take from each reference prototype and what to leave behind.

### From `video_player_lab/` (prototype since removed; table kept as the historical record)

| Component | Action | Target in pj_scene2D | Notes |
|-----------|--------|-------------------|-------|
| Latest-wins mailbox | **PORT CONCEPT ONLY** | `AsyncFrameWorker` (`async_frame_worker.h`) | The original standalone FrameSlot was deleted as dead; the concept now lives in the shared `AsyncFrameWorker` engine both worker-backed sources compose |
| Direction-aware cancel-store | **NOT PORTED** | — | The host's only video path (`StreamingVideoSource`) uses a latest-wins request model with no partial publication, so direction-aware cancel-store is not needed. Retained as design rationale in §3.2 for a future file-backed decoder |
| EOF decoder flush | **PORTED** as `FfmpegDecoder::flush()` | `FfmpegDecoder` | 3 lines: send NULL packet, drain buffered frames |
| `ENOMEM` recovery | **PORTED** | `FfmpegDecoder` | On `AVERROR(ENOMEM)`: `avcodec_flush_buffers` + retry once. Hit during scrub testing |
| `FrameCache` (JPEG cache) | **PORTED** as `EntryThumbnailCache` | `pj_scene2d_core/entry_thumbnail_cache.h` | Background thread builds ~1 thumbnail per adaptive interval for streaming `VideoFrame` topics, HD-capped via `thumbnail_codec.h` (≤1280px, YUV420P, JPEG quality 80). Used by `StreamingVideoSource` for instant backward scrub |
| `FrameConverter` | **DO NOT PORT** | — | Equivalent HW→SW transfer exists in pj_scene2D's `FfmpegDecoder` |
| `Mp4DataSource` | **DO NOT PORT** | — | pj_scene2D has no MP4/file-demux path: its only video source is the streaming `StreamingVideoSource`, which decodes raw Annex-B / OBU entries from the ObjectStore without a container demuxer (see §6) |
| `PlaybackClock` | **DO NOT PORT** | — | Replaced by main-thread-driven `setTimestamp()` model |
| `VideoWidget` (QRhiWidget) | **DO NOT PORT** | — | pj_scene2D already has `MediaViewerWidget` with the same QRhi + shader approach plus additional features (pixel inspector) |
| Keyframe pre-decode at open | **PORTED** as `EntryThumbnailCache` | `pj_scene2d_core/entry_thumbnail_cache.h` | Implemented for streaming `VideoFrame` topics: a background builder samples ~1 frame per interval from a single forward decode (materializing pixels only on the surfaced frames) into HD-capped JPEG thumbnails, used for instant backward scrub feedback |

### From the standalone pj_scene2D experiment (parallel effort; working tree since removed)

| Component | Action | Target in pj_scene2D | Notes |
|-----------|--------|-------------------|-------|
| QRhiWidget + YUV shaders | **CHERRY-PICK** | `MediaViewerWidget` | The shader code and QRhi setup are production-ready. Adapt to pj_scene2d_core's frame types |
| FFmpeg video stack | **CHERRY-PICKED** as `FfmpegDecoder` (+ `StreamingVideoDecoder`/`StreamingVideoSource`) | `FfmpegDecoder` | HW-accel probing, codec open/close, sws_scale paths. Push-based delivery dropped in favour of the pull-based `takeFrame()` model |
| ImageSource + BufferStrategy | **ADAPT** | `JpegCodec` / `PngCodec` / `ImagePipelineSource` | The per-topic buffer strategy is more complex than needed. Keep the turbojpeg/libpng codec stages and let `ImagePipelineSource` own ObjectStore lookup + raw canonical-image wrapping |
| PayloadDescriptor bytecode VM | **EVALUATE** | — | Clever but complex. Evaluate whether the simpler approach (metadata_json + decoder dispatch) suffices before porting |
| TimelineBridge | **DO NOT PORT** | — | Replaced by main-thread-driven `setTimestamp()` model |
| Timestamp µs vs ns dichotomy | **FIX** | — | pj_scene2D uses ns everywhere. The parallel effort's video engine used µs internally. All internal timestamps must be int64_t nanoseconds |

### From the legacy mcap_player sandbox (pre-rename SDK repo; since removed)

| Component | Action | Notes |
|-----------|--------|-------|
| `LazyMediaSeries<T>` callback model | **VALIDATED** | The pattern (timestamps + resolve closures capturing shared_ptr) is sound and maps directly to ObjectStore's `pushLazy` with fetch callbacks. The mcap_player sandbox validated the approach. No code to port — ObjectStore implements the pattern natively |
| `CompressedImageParser` (CDR → canonical Image → turbojpeg) | **REFERENCE** | Demonstrates the parser → decoder split. In the app, CDR envelope peeling is a `MessageParser` plugin concern; pj_scene2D core only receives canonical `sdk::Image` or demo-prepared bytes |

---

## Appendix: Key Invariants

These invariants are load-bearing. Violating any of them reintroduces
bugs that were already proven unfixable by patching.

1. **No Qt signals for frame delivery.** The `MediaSource::takeFrame()`
   pull model is the only path from decoder to display. Adding a signal
   escape hatch reintroduces stale-frame interleaving. (Rationale
   inlined in §3.1; proven in the since-removed video_player_lab
   prototype.)

2. **One MediaSource per widget.** The widget polls exactly one source
   for frames. When multi-layer compositing is added, a
   `CompositeMediaSource` composites internally and presents a single
   frame via the same interface.

3. **Shipped sources publish only complete frames.** `StreamingVideoSource`
   cancels stale decode work and `ImagePipelineSource` coalesces pending
   image requests, but neither path deposits partial decoded frames. The
   direction-aware partial-publish rule in §3.2 is retained as design
   rationale for a future file-backed decoder.

4. **ObjectStore handles are owning.** A decoder can hold a byte handle
   while the store pushes and evicts. No use-after-free is possible.
   (Guarantee: §OS3.4, §OS4)

5. **Live and scrub are mutually exclusive.** During scrub, the buffer
   is frozen — no pushes, no eviction. Seek sequences (keyframe →
   decode forward) cannot race with eviction. (Requirement: §R4.3)

6. **Keyframe tracking is pj_scene2D's concern, not ObjectStore's.**
   ObjectStore is codec-agnostic (§OS3.6). Today the index lives inside
   the decoder (`StreamingVideoDecoder`'s inline keyframe vector); a
   `MediaIndexRegistry` is designed for a future file-backed ObjectStore
   path (§6). (Decision: §R4.2, §R4.4)

7. **Parsers are codec-agnostic envelope peelers.** They never inspect
   NAL types, keyframe flags, or GOP structure. All codec knowledge
   lives in pj_scene2D's decoder classes. (Decision: §R4.4)
