# Rerun 2D Architecture — Research Notes

Analysis of Rerun's (rerun.io) architecture for 2D data visualization,
based on examination of their source code (`rerun-io/rerun`, `main` branch,
read May–June 2026; not re-checked since).
Focused on image, video, annotation, and depth rendering — the same
domain as pj_scene2D.

---

## 1. Data Model for 2D

Rerun uses an **archetype/component** system defined via FlatBuffers
schemas. Each archetype is a named bundle of required, recommended,
and optional components.

### Image Archetypes

**`Image`** — raw uncompressed image:
- Required: `ImageBuffer` (raw bytes as `Blob`), `ImageFormat`
  (resolution + pixel format: RGB, RGBA, etc.)
- Optional: `Opacity`, `DrawOrder` (default `-10.0`),
  `MagnificationFilter`
- Buffer is row-major, interleaved-pixel. Stored in an Arrow
  `LargeBinary` array.

**`EncodedImage`** — compressed (JPEG, PNG):
- Required: `Blob` (compressed bytes)
- Recommended: `MediaType` (e.g. `image/jpeg`, `image/png`)
- Optional: `Opacity`, `DrawOrder`, `MagnificationFilter`
- Decoded on the viewer side.

**`DepthImage`** — depth maps:
- Required: `ImageBuffer`, `ImageFormat`
- Optional: `DepthMeter` (meters-per-unit, default `1.0` for float,
  `1000.0` for int), `Colormap` (default Turbo), `ValueRange`,
  `FillRatio`, `DrawOrder` (default `-20.0`), `MagnificationFilter`
- In 3D views, generates a point cloud; in 2D, rendered as a
  colormapped image.

**`SegmentationImage`** — semantic segmentation masks:
- Required: `ImageBuffer`, `ImageFormat`
- Optional: `Opacity` (defaults to `0.5` when other images present,
  `1.0` otherwise), `DrawOrder` (default `0.0`)
- Each pixel is a `ClassId` mapped to color/label via
  `AnnotationContext`.

### Video Archetypes

**`AssetVideo`** — the video file itself (MP4 container only):
- Required: `Blob` (the **entire MP4 file**)
- Recommended: `MediaType` (`video/mp4`)
- Has no visualizer itself — it is a data source referenced by
  `VideoFrameReference`.

**`VideoFrameReference`** — references a specific frame in a video:
- Required: `VideoTimestamp` (timestamp relative to video start; uses
  closest frame, not latest-at)
- Optional: `EntityPath` (reference to entity with `AssetVideo`),
  `Opacity`, `DrawOrder` (default `-15.0`)
- One of these must be logged for each frame you want to display.

**`VideoStream`** (unstable) — raw video stream chunks for live
streaming:
- Required: `VideoCodec` (H264, H265, AV1, etc.)
- Recommended: `VideoSample` (the raw encoded chunk; one sample per
  frame)
- Optional: `Opacity`, `DrawOrder` (default `-15.0`)
- No B-frame support currently. Samples logged on the timeline where
  current timestamp = PTS.

### 2D Annotation Archetypes

**`Boxes2D`** — bounding boxes:
- Required: `HalfSize2D[]`
- Recommended: `Position2D[]` (centers), `Color[]`
- Optional: `Radius[]` (line width), `Text[]` (labels), `ShowLabels`,
  `DrawOrder` (default `10.0`), `ClassId[]`

**`Points2D`** — point clouds in 2D:
- Required: `Position2D[]`
- Recommended: `Radius[]`, `Color[]`
- Optional: `Text[]`, `ShowLabels`, `DrawOrder` (default `30.0`),
  `ClassId[]`, `KeypointId[]`

**`LineStrips2D`** — polylines:
- Required: `LineStrip2D[]`
- Recommended: `Radius[]`, `Color[]`
- Optional: `Text[]`, `ShowLabels`, `DrawOrder` (default `20.0`),
  `ClassId[]`

**`Arrows2D`** — arrows:
- Required: `Vector2D[]`
- Recommended: `Position2D[]` (origins)
- Optional: `Radius[]`, `Color[]`, `Text[]`, `ShowLabels`,
  `DrawOrder`, `ClassId[]`

### Layer Compositing via DrawOrder

All 2D archetypes have a `DrawOrder` component (float). Default values
create a natural stacking order:

| Layer | DrawOrder |
|-------|-----------|
| Depth images | `-20.0` |
| Video frames | `-15.0` |
| Regular images | `-10.0` |
| Segmentation | `0.0` |
| Boxes | `+10.0` |
| Lines | `+20.0` |
| Points | `+30.0` |

`DrawOrder` values are converted to integer depth offsets via
`EntityDepthOffsets` (in `re_view_spatial/src/contexts/depth_offsets.rs`)
and applied to the wgpu depth buffer during rendering. This enables
correct painter's-algorithm compositing without an explicit compositor
class.

---

## 2. Storage and Chunking

### Chunk-Based Columnar Store

Rerun uses a custom chunk-based columnar store built on **Apache Arrow**.

**`re_chunk::Chunk`** (`crates/store/re_chunk/src/chunk.rs`):
- A chunk is an Arrow record batch containing time columns + component
  columns.
- Components stored as `ArrowListArray` (list-of-structs layout) keyed
  by `ComponentIdentifier`.
- Chunks carry metadata: `ChunkId`, `RowId`, entity path, timeline
  columns, sorted status.

**`re_chunk_store::ChunkStore`** (`crates/store/re_chunk_store/`):
- In-memory time-series database indexed by entity path and timeline.
- Two query modes: **LatestAt** (most-recent value at a timestamp) and
  **Range** (all values within a time interval).
- Chunks are compacted (merged) up to configurable thresholds:
  - `chunk_max_bytes`: ~384 KB (default)
  - `chunk_max_rows`: 4096 (sorted), 1024 (unsorted)
- These defaults are intentionally modest — the Rerun team found that
  "a few megabytes turned out to be way too costly to concatenate in
  real-time."

### Handling Large Blobs

Images are stored as `Blob` components (Arrow `LargeBinary` arrays).
Each image frame is a row in a chunk. Since images are large (e.g.
1920x1080x3 = ~6 MB), each image frame typically creates its own chunk
(exceeding the `chunk_max_bytes` threshold).

For `AssetVideo`, the **entire MP4 file** is stored as a single `Blob`.
Video stream samples (`VideoSample`) are individual encoded frame blobs
logged per-timestamp. The chunk store holds references via `Tuid`
identifiers; the video player retrieves them on demand via a
`get_video_buffer` callback that resolves `Tuid → &[u8]` through the
store.

### Garbage Collection

`re_chunk_store/src/gc.rs`:
- **Target modes**: `DropAtLeastBytes`, `DropAtLeastFraction`,
  `Everything`
- **Strategy**: Priority to chunks **furthest from a given timestamp**;
  falls back to `RowId` ordering (client wallclock). This is
  proximity-based eviction, not LRU. PJ's `ObjectStore` evicts from the
  front instead, and pausing a stream freezes the store a viewer reads
  (`pj_scene2D/docs/ARCHITECTURE.md` "Behaviour contracts").
- **Protections**: Can protect latest N values per component per
  timeline, protect specific time ranges, protect specific chunk IDs.
- **Time budget**: Configurable (default 3.5 ms in the viewer).
- GC operates at chunk granularity — cannot partially evict a chunk.
- For video streams, evicted samples are marked as "unloaded" (metadata
  retained for seeking, data GC'd from store).

---

## 3. Video Decoding Pipeline

### Architecture Overview

```
AssetVideo/VideoStream (data)
  → VideoDataDescription (demuxed metadata + keyframe index)
    → VideoPlayer (seeking/scheduling)
      → AsyncDecoder (background thread)
        → Frame (decoded pixels)
          → GPU texture upload (re_renderer)
```

Key source files:
- `crates/utils/re_video/src/demux/mod.rs`
- `crates/utils/re_video/src/player/mod.rs`
- `crates/utils/re_video/src/decode/mod.rs`

### Demuxing

**`VideoDataDescription`** is the core metadata structure:
- `codec: VideoCodec` (H264, H265, AV1, VP8, VP9, ImageSequence)
- `encoding_details: Option<VideoEncodingDetails>` (dimensions, bit
  depth, chroma subsampling, SPS/PPS for H264)
- `timescale: Option<Timescale>` (time units per second)
- `samples: StableIndexDeque<SampleMetadataState>` — sorted by DTS,
  each with PTS/DTS/duration/is_sync/source_id
- `keyframe_indices: Vec<SampleIndex>` — sorted list of keyframe
  sample indices
- `delivery_method: VideoDeliveryMethod` — `Static { duration }` or
  `Stream { last_time_updated_samples }`

MP4 files are demuxed via the `re_mp4` crate. For video streams, GOP
detection (`gop_detection.rs`) parses raw NALUs to identify keyframes
by inspecting SPS/PPS NALs (H.264), VPS/SPS/PPS (H.265), or OBU
headers (AV1).

### Decoder Backends

Rerun has **four** decoder backends:

**1. FFmpeg CLI** (`decode/ffmpeg_cli/ffmpeg.rs`) — native, H.264/H.265:
- Spawns an **FFmpeg child process** via `ffmpeg_sidecar`.
- Feeds raw NALUs to FFmpeg's stdin (Annex B format).
- Reads decoded frames from FFmpeg's stdout as raw pixel data.
- Three threads: write thread (stdin), listen thread (stdout), main.
- Uses `re_quota_channel` for backpressure (512 MB max in-flight).
- Requests YUV output when chroma subsampling is known, falls back to
  RGBA.
- Forces BT.709 matrix coefficients and full range via FFmpeg flags.

**2. dav1d** (`decode/av1.rs`) — native AV1:
- Uses the `dav1d` Rust crate (wrapper around the C dav1d library).
- Wrapped in `AsyncDecoderWrapper` for async operation.
- Not available on Linux ARM64.

**3. WebCodecs** (`decode/webcodecs.rs`) — web/WASM only:
- Uses browser's WebCodecs API (`VideoDecoder`).
- Handles quirks: Firefox/Safari may output frames in decode order
  instead of presentation order.
- Supports hardware acceleration hints.

**4. Image decoder** — for `ImageSequence` codec:
- Decodes JPEG/PNG sequences as "video".
- On web, uses browser's image decoding.

### Async Decoder Interface

```rust
pub trait AsyncDecoder: Send + Sync {
    fn submit_chunk(&mut self, chunk: Chunk) -> Result<()>;
    fn end_of_video(&mut self) -> Result<()>;
    fn reset(&mut self, video_descr: &VideoDataDescription) -> Result<()>;
    fn min_num_samples_to_enqueue_ahead(&self) -> usize;
}
```

`AsyncDecoderWrapper` runs a `SyncDecoder` on a dedicated background
thread, communicating via channels with backpressure. Commands (Chunk,
Reset, Stop) are sent via `Sender<Command>`. Decoded `FrameResult`s
are sent back on `Sender<FrameResult>`.

### Seeking and Keyframe Management

**`VideoPlayer::frame_at()`** is the central method:

1. Finds the sample whose PTS is closest to the requested timestamp
   via `latest_sample_index_at_presentation_timestamp()`.
2. Calls `request_keyframe_before()` which walks backward from the
   requested sample to find the nearest keyframe (using
   `keyframe_indices`).
3. If the keyframe is from a different GOP than what the decoder has,
   issues a `reset()` on the decoder.
4. Enqueues all samples from the keyframe up to the requested sample
   (in DTS order) via `submit_chunk()`.
5. Polls the `frame_receiver` for decoded frames, keeping only the
   latest frame at or before the requested PTS.
6. Implements a **decoder delay state machine** with states:
   - `UpToDate`
   - `UpToDateWithinTolerance` (3-frame tolerance)
   - `UpToDateToleratedEdgeOfLiveStream`
   - `Behind`
7. Has a **400ms grace delay** before reporting errors (prevents
   flicker during fast scrubbing).

For backward seeks, the decoder is fully reset and re-primed from the
nearest keyframe. For forward seeks within the same GOP, existing
samples are just enqueued incrementally.

### Frame Output Format

Decoded frames (`FrameContent`) contain:
- Native: `data: Vec<u8>`, `width`, `height`, `format: PixelFormat`
- Web: `web_sys::VideoFrame` (opaque browser handle)

`PixelFormat` variants: `L8`, `L16`, `Rgb8Unorm`, `Rgba8Unorm`,
`Yuv { layout, range, coefficients }`

---

## 4. Rendering Pipeline for 2D

### GPU Stack

Rerun uses **wgpu** for all rendering, via their custom `re_renderer`
crate. The renderer runs on Vulkan, Metal, D3D12, or WebGPU (with
WebGL fallback on web). The UI uses **egui** (immediate-mode GUI).

### Image-to-GPU Path

1. Image data arrives as `ImageInfo` (buffer + format + kind).
2. `gpu_bridge::image_to_gpu()` converts to `ColormappedTexture`:
   - Creates/reuses a `GpuTexture2D` via `texture_manager_2d`.
   - Transfers data via `transfer_image_data_to_texture()`.
3. Image is rendered as a `TexturedRect` — a GPU quad with:
   - Position/extent vectors (z=0 for 2D)
   - `ColormappedTexture` (texture + range + gamma + color mapper +
     alpha mode)
   - `RectangleOptions` (filter modes, depth offset, tint, outline)
4. `RectangleRenderer` issues **one draw call per textured rectangle**
   (bindless textures not widely available).

### YUV-to-RGB Conversion

`YuvConverter` (`resource_managers/yuv_converter.rs`):
- Supports planar: `Y_U_V444`, `Y_U_V422`, `Y_U_V420`
- Semi-planar: `Y_UV420`
- Interleaved: `YUYV422`
- Uses compute or fragment shaders (`yuv_converter.wgsl`)
- Applies correct matrix coefficients (BT.601 vs BT.709) and range
  (limited vs full)
- YUV data uploaded as a single-channel data texture; shader samples
  from appropriate offsets for each plane.

### Video Frame-to-GPU Path

1. `Video::frame_at()` calls `VideoPlayer::frame_at()` with a closure
   `update_output`.
2. The closure calls `chunk_decoder::update_video_texture_with_frame()`.
3. Allocates/reuses a `GpuTexture2D` (format: `Rgba8Unorm`, with
   `COPY_DST | COPY_SRC | TEXTURE_BINDING | RENDER_ATTACHMENT`).
4. Dispatch on pixel format:
   - `Rgb8Unorm`: pad to RGBA, then upload
   - `Rgba8Unorm`: direct texture upload
   - `Yuv { .. }`: upload raw data + schedule GPU conversion
   - `L8`/`L16`: upload as single-channel texture
5. Web: `copy_external_image_to_texture()` transfers the browser's
   `VideoFrame` directly to the GPU texture (zero-copy when possible).

### Immediate Mode Architecture

The entire viewer is **immediate-mode**: every frame, the viewer
re-queries the chunk store, massages results, and feeds them to the
renderer. There is no retained scene graph. This eliminates state
synchronization bugs but requires relentless optimization (and they
note plans to add query/render caching for large datasets in the
future).

---

## 5. Timeline and Scrubbing

### Time Control

`re_time_panel` provides the timeline UI:
- `TimeControl` manages current time cursor, playback state, and
  selected timeline.
- Supports multiple timelines (sequence-based like frame number, or
  time-based like wall clock).
- Provides loop/selection ranges.

### Query Model

When the user scrubs the time cursor:
1. A `LatestAtQuery(timeline, timestamp)` is constructed.
2. Each visualizer executes this query against the chunk store.
3. The chunk store returns the most relevant chunk(s) via binary
   search through temporal indices.
4. For video: the query timestamp is converted to a video-relative PTS
   and passed to `VideoPlayer::frame_at()`.

### Video Scrubbing

1. `VideoFrameReferenceVisualizer` reads `VideoTimestamp` at the
   current cursor time.
2. Passes to `Video::frame_at()` → `VideoPlayer`.
3. Player determines if a seek is needed:
   - Forward within same GOP: enqueue missing samples.
   - Backward or large jump: reset decoder, seek to keyframe,
     re-decode.
4. Maintains `last_requested` sample index to detect direction changes.
5. During seeking, the last-good frame is shown (fading out slightly)
   with an optional loading indicator.
6. 400ms grace delay prevents flickering during fast scrubbing.

### Multiple Simultaneous Streams

A single `Video` can have multiple `VideoPlayer` instances keyed by
`VideoPlayerStreamId`. This allows displaying the same video at
different timestamps simultaneously (e.g., different views).

---

## 6. Streaming Architecture

### Live Data Ingestion

Data reaches the viewer via:
- **gRPC**: streaming over network (primary live path)
- **`.rrd` files**: sequential log messages appended to a file
- **SDK direct**: `rr.init(..., spawn=True)` launches viewer and
  streams data

The logging SDKs encode data as Apache Arrow and send chunks via
these transport layers.

### Video Streaming

`VideoStream` archetype (unstable) enables live video:
- `VideoCodec` is logged once (static).
- `VideoSample` blobs are logged per-frame on the timeline.
- Viewer builds `VideoDataDescription` incrementally:
  - Samples accumulate in `StableIndexDeque` (supports front removal
    for retention).
  - GOP detection runs on each incoming sample (NAL parsing).
  - When samples are added, `Video::handle_sample_insertion()`
    determines if active players need reset.

### Retention and Eviction

Managed at multiple levels:

1. **Chunk Store GC**: Evicts oldest chunks when memory pressure
   triggers GC. Uses `furthest_from` timestamp to prioritize eviction.
   `protect_latest` keeps the most recent N values per component.

2. **Video Data Description**: Evicted samples transition to
   `SampleMetadataState::Unloaded { source_id }` — metadata retained
   for seeking, data GC'd. The player calls `get_buffer(source_id)`
   which returns empty for unloaded samples, triggering "unloaded
   sample" errors.

3. **Video Player Memory**: `re_quota_channel` provides backpressure
   with 512 MB cap. Decoded frames in `frames_by_pts: BTreeMap` are
   pruned — only frames >= requested PTS are kept.

4. **GPU Textures**: `GpuTexturePool` manages lifetime. Unused video
   player entries are purged per-frame via `used_last_frame` flag.
   `texture_manager_2d` uses hash-based caching with deduplication.

### Edge-of-Stream Handling

- `DecoderDelayState::UpToDateToleratedEdgeOfLiveStream` — tolerates
  being behind without showing a loading indicator.
- `time_until_video_assumed_ended` (250ms): if no new samples arrive,
  signals end-of-video to flush remaining frames.
- FFmpeg decoder requires stdin close + process restart when
  transitioning from end-of-video back to active streaming.

---

## 7. Comparison with pj_scene2D

The pj_scene2D column describes the code as it is; its own docs are `pj_scene2D/docs/ARCHITECTURE.md`.

| Aspect | Rerun | pj_scene2D |
|--------|-------|----------|
| **Video storage** | Whole MP4 as blob; per-frame `VideoSample` for streaming | One encoded frame per `ObjectStore` entry; an MP4 producer transcodes to per-frame entries at ingest |
| **Decoder linkage** | FFmpeg as **child process** (stdin/stdout IPC) | FFmpeg **linked** (libavcodec API) |
| **GPU API** | wgpu (Vulkan/Metal/D3D12/WebGPU) | QRhi (OpenGL, Vulkan, Metal, D3D11, WebGL2) |
| **YUV conversion** | GPU compute/fragment shader (BT.601/BT.709 aware) | GPU fragment shader, BT.601/709 and limited/full range per frame from the codec's signalled colorimetry |
| **Pixel upload** | Per-format (RGB pad, RGBA direct, YUV planes, NV12 two-plane) | RGB/RGBA/Mono8/BGRA/YUV420P, native NV12 two-plane (RG8 UV; CPU deinterleave fallback) |
| **Magnification filter** | Per-image nearest/linear (`MagnificationFilter`) | Per-image-layer nearest/linear toggle |
| **Frame delivery** | Immediate-mode re-query every render frame | Pull-based `MediaSource::takeFrame()`, latest-wins single-slot mailbox (`AsyncFrameWorker`) |
| **Layer compositing** | `DrawOrder` float → depth buffer offsets | `CompositeMediaSource`: one child source per visible layer, opacity-scaled pixel layers in draw order plus concatenated overlays |
| **Keyframe index** | Built from MP4 demux or NAL parsing | Inside `StreamingVideoDecoder` (keyframe scan over the store entries); `ObjectStore` stays codec-agnostic |
| **Scrub strategy** | Keyframe seek + decode forward; 400ms grace delay | Keyframe seek + decode forward; scrub preemption and `EntryThumbnailCache` |
| **Retention/GC** | Chunk-level GC, furthest-from-cursor priority | `ObjectStore` time/memory budget per topic, front eviction; a paused stream freezes the viewer's store |
| **Backward scrub** | Show last-good frame (fading) | Last frame stays until replaced; thumbnail cache for sub-keyframe positions |
| **B-frame support** | Not supported in streaming; tolerated in MP4 | Supported (DTS-keyed storage, decoder reorder) |
| **Scene graph** | None (immediate-mode) | None (pull-based per-widget) |
| **Threading model** | Async decoder on background thread + channels | One worker thread per source (`AsyncFrameWorker`) |
| **Language** | Rust | C++20 |

### Design comparison (reasoning, not measured unless stated)

**DrawOrder compositing**: one float per archetype mapped to depth-buffer offsets is simpler than a compositor class
and scales to many archetypes at arbitrary orders. pj_scene2D composites a handful of explicitly ordered,
user-reorderable layers with alpha-over; a depth pass would add a convention for little gain at that layer count.
Worth revisiting if GPU compositing of many layers is ever needed.

**FFmpeg as subprocess**: avoids LGPL linking and isolates decoder crashes, at the cost of IPC (Rerun's figure: about
1 ms per frame for stdout pipe reads). pj_scene2D links libavcodec: on the desktop LGPL compliance is manageable,
and the IPC cost would land on scrub latency.

**Immediate-mode re-query**: no stale state, but it re-pays expensive decodes; PJ's bottleneck is decode, which the
pull model with a latest-wins mailbox bounds.

**400ms grace delay**: Rerun shows the last good frame for up to 400 ms before reporting a decode error during fast
scrub. pj_scene2D keeps the last frame until a newer one is deposited and gives backward feedback from the thumbnail
cache; and since a paused stream freezes the store, a scrub cannot lose its keyframe mid-walk, the main error the
grace delay hides.

**Whole-MP4-as-blob**: simple, but does not scale to GB files; per-frame entries with retention fit streaming.

## 8. Ideas taken from Rerun, and ones not taken

Taken (in the code today):

- **Colorspace-aware YUV→RGB.** `DecodedFrame` carries the frame's colour space and range (with the SD-height →
  BT.601 and unspecified-range → limited heuristics) and `video_color.h` builds one affine 4×4 per frame that folds
  range scale and offset in, so the shader stays `rgb = M·vec4(y, u-0.5, v-0.5, 1)`; BT.709 full range gives the
  plain BT.709 matrix. Without it SD content shifts hue and limited-range H.264/HEVC shows washed-out blacks and
  whites (`pj_scene2D/docs/TECHNICAL_NOTES.md` "Colorimetry"). Mirrors Rerun's `yuv_converter.rs`.
- **Depth auto-fit, on demand.** An "Auto-fit range" button sets Near/Far to the current frame's 2nd/98th depth
  percentiles (`depth_range.h`). A continuous auto-range from per-frame min/max was tried and flickered; manual
  stays the default.
- **Per-image magnification filter.** A per-layer "Pixelated (nearest)" toggle, persisted in the layout. Depth
  always samples nearest, so the no-data sentinel is never blended.
- **Native NV12 upload.** Hardware decode output uploads as Y (R8) plus interleaved UV (RG8), skipping a full-frame
  repack; a CPU deinterleave covers backends without RG8 and the thumbnail encoder. The two-plane GPU path runs
  only with hardware decode, which CI's software decoders never reach: it is unit-tested at the buffer-layout
  level and was checked live on VAAPI, not pixel-verified in CI.

Not taken, with the reason at the time:

- **SegmentationImage + AnnotationContext (class id → colour/label).** High value, and the depth GPU-LUT path is a
  template for the render side, but the useful part (consistent colours and labels across frames) needs a new
  canonical type in `plotjuggler_sdk`, a parser change in `pj-official-plugins` and the viewer path together.
- **Decoder-delay / error debounce during fast scrub.** Low value given the behaviour in "400ms grace delay" above.
- **VP8 / VP9.** No keyframe oracle for seeking; the decoder accepts only H.264, HEVC and AV1.
- **ImageSequence-as-video.** Image topics already seek frame by frame through `ObjectStore`.
- **Several players per topic** (one video at two timestamps). Niche; one decoder per layer.
- **Decoded-frame cache for backward scrub.** The thumbnail cache plus re-decode from the keyframe gives backward
  feedback at much lower memory.
- **Cursor-proximity eviction / protect-latest-N in `ObjectStore`.** Eviction never runs while a stream is paused
  for a scrub, so cursor-aware eviction would optimise a case that does not occur.
- **Immediate-mode re-query, whole-MP4-as-blob, a 512 MB backpressure quota.** See section 7; decoded-frame memory is
  bounded by the mailbox and the reorder buffer.

---

## Source File Reference

| Area | Path in `rerun-io/rerun` |
|------|--------------------------|
| Architecture overview | `ARCHITECTURE.md` |
| 2D archetype definitions | `crates/store/re_sdk_types/definitions/rerun/archetypes/{image,depth_image,...}.fbs` |
| Chunk data model | `crates/store/re_chunk/src/chunk.rs` |
| Chunk store (queries, GC) | `crates/store/re_chunk_store/src/{store,query,gc}.rs` |
| Video demux/metadata | `crates/utils/re_video/src/demux/mod.rs` |
| Video player (seeking) | `crates/utils/re_video/src/player/mod.rs` |
| Decoder abstraction | `crates/utils/re_video/src/decode/mod.rs` |
| FFmpeg CLI decoder | `crates/utils/re_video/src/decode/ffmpeg_cli/ffmpeg.rs` |
| dav1d AV1 decoder | `crates/utils/re_video/src/decode/av1.rs` |
| Async decoder wrapper | `crates/utils/re_video/src/decode/async_decoder_wrapper.rs` |
| GOP detection | `crates/utils/re_video/src/gop_detection.rs` |
| GPU video texture | `crates/viewer/re_renderer/src/video/{mod,chunk_decoder}.rs` |
| YUV conversion | `crates/viewer/re_renderer/src/resource_managers/yuv_converter.rs` |
| Rectangle renderer | `crates/viewer/re_renderer/src/renderer/rectangles.rs` |
| Image visualizer | `crates/viewer/re_view_spatial/src/visualizers/images.rs` |
| Video visualizers | `crates/viewer/re_view_spatial/src/visualizers/video/{mod,...}.rs` |
| 2D spatial view | `crates/viewer/re_view_spatial/src/{view_2d,ui_2d}.rs` |
| DrawOrder/depth offsets | `crates/viewer/re_view_spatial/src/contexts/depth_offsets.rs` |
| Image-to-GPU bridge | `crates/viewer/re_renderer/src/resource_managers/image_data_to_texture.rs` |
