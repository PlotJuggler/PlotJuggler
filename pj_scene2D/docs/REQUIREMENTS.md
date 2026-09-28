# pj_scene2D Requirements

## 1. Purpose

Viewer-side module for rendering 2D data in PlotJuggler: images, video,
image annotations, depth and segmentation overlays, and 2D scene primitives
(markers, lines, polygons with `z = 0`). It handles on-demand decoding and
GPU-accelerated display of media synchronized with the global timeline.

pj_scene2D does not own storage. Raw compressed bytes live in
`pj_datastore::ObjectStore`, a peer class to the columnar `DataEngine`
(see `pj_datastore/docs/OBJECT_STORE_DESIGN.md`). pj_scene2D is a read-only
consumer of the ObjectStore: it reads raw bytes, decodes them, and renders
pixels. Decoded data is never written back to storage. Any caching of
decoded frames inside pj_scene2D is an implementation detail of the decoder
pipeline, not a contract.

3D data types (PointCloud, 3D scene primitives, Grid) are stored in the
same ObjectStore but are rendered by the `pj_scene3D` widget family, not
by pj_scene2D.

## Prerequisites

pj_scene2D depends on ObjectStore extensions that are now committed in
`OBJECT_STORE_DESIGN.md` (§3.4 concurrent readers, §3.5 automatic
retention, §4 owning handles + `EntryTimestampsView` + `RetentionBudget`,
§5 `indexAt` + `entryTimestamps` + `setRetentionBudget`, §6
`PJ_object_write_host_vtable_t`). Those extensions were driven by the
requirements below and are now consistent across both documents.

**The two prerequisites originally listed here have both been resolved**,
though in shapes that differ from what this document first proposed:

- **Time updates: `IDataWidget` in `pj_runtime`** (resolved). pj_scene2D
  widgets implement `PJ::IDataWidget` (from
  `pj_runtime/include/pj_runtime/IDataWidget.h`) and are driven by
  `pj_runtime::PlaybackEngine`. The original proposal was a
  `TimelineCursor` header in `pj_base` to which widgets would subscribe;
  the as-built model instead has widgets implement an interface and the
  runtime call them on each tick. The contract property still holds:
  widgets only *receive* time, they never drive the clock. See §4.5 and
  §6 for the current wording.

- **Two-host parser ingest: protocol v4 + service-registry bindings**
  (resolved). The original proposal was a parser ABI bump to a two-host
  `parse()` signature. The `pj_plugins` MessageParser protocol is now at
  v4 (`message_parser_protocol.h`); rather than adding hosts as
  parameters, hosts are acquired at `bind(registry)` time via named
  services (`pj.parser_write.v1` for scalars; an object write service for
  media). A parser binds whichever services it needs and writes to them
  inside its single `parse()` call. The net behaviour matches what §4.4
  requires.

**One related implementation item is still pending:** the per-topic
keyframe-index sidechannel (referred to as `MediaIndexRegistry` in §4.4
and ARCHITECTURE.md §6) is designed but not yet implemented. Today
`StreamingVideoDecoder` maintains its own inline keyframe vector — the
host's only video decode path — which works without the registry. The
registry would only be needed if a future file-backed ObjectStore path
lands. Treat `MediaIndexRegistry` references in §4.4 as **designed, not
yet realised**.

**Note on keyframe tracking**: pj_scene2D does NOT ask ObjectStore to
know anything about keyframes. Per `OBJECT_STORE_DESIGN.md §3.6`,
entries are stateless and codec-agnostic, and stateful decoding is
explicitly a viewer concern. Video keyframe indexing is owned by
`pj_scene2d_core` — see §4.2 and §4.6.

## 2. Goals

- **Unified query interface**: viewer asks "give me the entry for channel C
  at time T" regardless of whether the backing storage is a file, a live
  buffer, or a streaming ring.
- **Viewer decoder pipeline**: built-in C++ decoders (FFmpeg for video,
  turbojpeg for JPEG, libpng for PNG, CDR/Protobuf deserializers for scene
  primitives and annotations). Stateful when required (video GOPs),
  stateless otherwise.
- **Hardware-accelerated video decode where available**: runtime-detected
  HW-accel paths with software fallback **guaranteed** on every platform.
  No compile-time codec switches. Platform-specific backend coverage is
  an implementation detail tracked in `TECHNICAL_NOTES.md §3`, not a
  requirement here; users get HW-accel where the platform and codec
  support it, and graceful software playback otherwise.
- **GPU rendering**: `QRhiWidget`-based widgets with custom GLSL shaders for
  YUV→RGB color conversion (BT.601 / BT.709). Zoom (mouse wheel) and pan
  (mouse drag) via a view transform matrix in the vertex shader — no pixel
  reprocessing.
- **Pull-based frame delivery**: the decoder writes the latest decoded frame
  into a single-slot mailbox; the UI polls at render rate. No Qt signals
  cross the decoder→UI boundary. Stale-frame interleaving is structurally
  impossible.
- **Time synchronization**: media channels share the nanosecond global
  timeline; widgets subscribe to a timeline cursor owned by the application.
- **Multi-camera**: multiple named channels per dataset, each rendered by
  an independent viewer widget, all synchronized to the same clock.
- **Multi-layer compositing**: base image + annotation overlay + depth
  colormap + segmentation mask composited in one widget, aligned by
  timestamp (always using the entry at-or-before the current display time).
- **Streaming with buffered replay**: live mode shows the newest frame;
  when paused, the retained ObjectStore range becomes seekable.

## 3. Use Cases

- **One-shot file import** — open an MCAP file containing images or
  CompressedVideo, a LeRobot dataset (MP4 + Parquet), or RLDS TFRecords.
  The DataSource plugin populates ObjectStore with timestamped entries
  (lazy fetch callbacks for file-backed, owned bytes for in-memory). No
  pixel data is decoded until a viewer requests a specific timestamp.

- **Live streaming** — receive frames from ROS 2 image topics, RTSP
  cameras, GStreamer pipelines, or V4L2 local cameras. The DataSource
  plugin pushes raw compressed bytes into ObjectStore as owned entries.
  The viewer decodes and displays the newest entry in real time.

- **Synchronized scrubbing** — user drags the global time slider. Each
  media widget queries its ObjectTopic at the new timestamp, decodes the
  result, and displays it. File-backed and buffered data are handled by
  the same code path.

- **Paused stream replay** — during a live session the user pauses. The
  retained buffer becomes scrubbable within the range reported by
  `ObjectStore::timeRange(topic)`. The user scrubs backward, then
  resumes live.

- **Multi-camera robotics** — a robotics dataset with several RGB cameras,
  depth cameras, and segmentation overlays per camera. Each camera is a
  separate topic rendered by a separate widget. All widgets subscribe
  to the same global clock; on each render tick, each widget queries
  its own topic independently at the current timestamp. Widgets run
  independently — there is no cross-widget barrier.

- **Multi-layer compositing** — a camera view shows the base RGB image
  plus an ImageAnnotation overlay (bounding boxes, labels) plus a depth
  colormap plus a segmentation mask. All are separate ObjectTopics. The
  compositor queries each at the current display time (using at-or-before
  semantics) and renders the composite frame.

## 4. Functional Requirements

### 4.1 Data Types

All data type schemas are defined in the SDK builtin-type catalog:
[`plotjuggler_sdk/docs/builtin_type.md`](../../plotjuggler_sdk/docs/builtin_type.md).
This document does not duplicate field-level definitions.

**Types pj_scene2D RENDERS (image-pixel space only):**

| Type | Role |
|------|------|
| Image | Raw, compressed, depth, and segmentation frames |
| VideoFrame | H.264 / H.265 / AV1 / VP9 encoded frames |
| ImageAnnotation | Pixel-space vector overlays (points, lines/polygons, circles, text labels) |
| CameraCalibration | Intrinsics and distortion (metadata only; no rendering) |

**Types STORED in ObjectStore but rendered by other modules:**

PointCloud, Grid (occupancy / costmap / elevation), all `ScenePrimitive`
variants (including 2D `z = 0` ones), and FrameTransform chains are
stored in the same ObjectStore but are consumed by `pj_scene3D`
rather than by pj_scene2D. Even when a marker carries `z = 0`, projecting
it onto a camera image requires `CameraCalibration` plus TF interpolation,
which is the machinery `pj_scene3D` owns. Duplicating that inside
pj_scene2D would create two TF resolvers and two projection paths for the
same primitive — exactly the mistake the independent-widget-families design avoids.

**Note on Grid**: `sdk::OccupancyGrid` (see [`plotjuggler_sdk/docs/builtin_type.md`](../../plotjuggler_sdk/docs/builtin_type.md) — `## OccupancyGrid`) carries world-space metadata (`pose`, `resolution`, `frame_id`), which makes it a "2D/3D" type in nature. pj_scene2D
nevertheless defers Grid to `pj_scene3D` because Grid carries world-space
metadata (`pose`, `cell_size`, `frame_id`) and its natural display is a
**world-space** top-down tile — a different viewer class from pj_scene2D's
image-space viewers (which zoom and pan in pixel coordinates, not
meters). In `pj_scene3D`, both the 2D top-down and 3D elevation
views of a grid are rendered there, sharing the same ObjectStore
entries as their source of truth. pj_scene2D's image viewers do NOT
attempt to render grids as plain pictures (ignoring world metadata)
because the result would be misleading — you could not correctly
overlay a robot pose or composite with another frame.

### 4.2 Storage Integration

All media data is stored in `pj_datastore::ObjectStore`. pj_scene2D is a
read-only consumer.

- The ObjectStore exposes a uniform query: `latestAt(topic, timestamp)`
  returns an owning handle to the raw bytes of the entry at or before the
  requested timestamp. "At or before" is the only semantic — the store
  never returns a future entry even if it is closer in time. This
  guarantees causality: a viewer at time `t` can only see bytes that were
  produced at or before `t`.

- The handle returned is owning (shared ownership). Decoder workers may
  hold it concurrently with other ObjectStore operations. Lifetime is
  independent of the store's internal state after the handle is acquired.

- Two internal storage modes are invisible to pj_scene2D: owned bytes
  (streaming sources) or lazy fetch callbacks (file-backed sources).
  pj_scene2D does not distinguish between them.

- **Keyframe tracking is outside ObjectStore.** ObjectStore stores raw
  bytes and is intentionally codec-agnostic. For video topics,
  `pj_scene2d_core` maintains its own per-topic keyframe index — built
  from a pre-computed sidechannel published by the DataSource (for
  file-backed sources) or incrementally inside the viewer-side decoder
  (for streaming sources). See §4.6. ObjectStore queries (`latestAt`,
  `at`, `timeRange`) remain codec-unaware.

- **Time range**: `ObjectStore::timeRange(topic)` reports the currently
  stored `[t_min, t_max]` for the topic. For file-backed topics this is
  the full span; for live topics with rolling eviction it is the
  currently-retained window. Viewers use it to bound scrub UIs.

- **Internal decoded-frame cache**: pj_scene2D MAY maintain a decoded-frame
  cache to amortize repeated decode cost (e.g., small-range backward
  scrub). Whether a cache exists and how it is structured is an
  implementation detail of the decoder backend (libmpv vs custom FFmpeg
  pipeline) and is NOT a requirement. Proximity-based eviction
  (farthest-from-current-playhead) is the proven pattern when a cache is
  used; LRU is explicitly the wrong shape for scrub workloads.

  For still images (JPEG/PNG), on-the-fly decoding is empirically fast
  enough (<10 ms per frame) that caching decoded RGBA wastes more memory
  than it saves time; a still-image cache is typically not worth it.
  Video is the main beneficiary of a cache.

### 4.3 Streaming and Buffered Replay

Streaming sources (ROS 2, RTSP, GStreamer, V4L2) are handled by DataSource
plugins that push raw compressed bytes into ObjectStore via `push_owned`.
There is no separate "streaming buffer" concept — a live video feed is
simply an ObjectStore topic containing a series of push-driven entries
(either self-contained images such as JPEG or self-contained VideoFrame
records, following the same approach as Foxglove and Rerun) under a
bounded retention budget. pj_scene2D reads the series; it is never a writer.

**Live and scrub modes are mutually exclusive.** This is a deliberate
product requirement — a user-experience decision about how streaming
media is presented — not a shortcut to avoid engineering race
conditions. The application offers the user exactly two interaction
modes and does not attempt to combine them: you are either watching
live (and the buffer slides forward under you), or you have paused and
are exploring a frozen buffer. There is no in-between state where the
user scrubs while new data is being pushed and old data evicted
simultaneously, because that would force the user to reason about a
timeline that shifts beneath their hands.

- In **live mode**, the viewer displays the newest entry on each
  render tick as data streams in. The DataSource is actively pushing
  new entries and ObjectStore is actively evicting old ones to stay
  within the retention budget. The slider is pinned to the live edge;
  scrubbing controls are disabled.
- In **paused / scrub mode**, pushing and eviction are suspended; the
  buffer is frozen for as long as the user wants to explore it. The
  user can drag freely through any timestamp in `timeRange(topic)`,
  and what they see is stable — entries cannot appear or disappear
  under them.
- **Resuming** returns to live mode: the DataSource starts pushing
  from the current edge, ObjectStore resumes eviction, and the
  retention window re-engages.

A welcome engineering consequence of this requirement is that entire
classes of races are eliminated — a decoder cannot start from a
keyframe that then disappears mid-seek, because eviction does not run
during scrub. The architecture relies on this property (see §4.6
"Keyframe seek" and Prerequisite 7 "Entry iteration primitives"), but
the property is derived from the UX requirement, not the other way
around.

**Retention budget**: the application configures a global budget — a
time window (e.g., 60 s), a memory cap, or both. ObjectStore enforces
the budget only while pushing is active (live mode). DataSource plugins
do not decide when to evict.

### 4.4 Parser and DataSource Integration

A single DataSource plugin can produce both scalar time-series data
(written to `pj_datastore::DataEngine` via the columnar write host) and
media data (written to `ObjectStore` via the object write host) from the
same input file or stream. This is the expected pattern for multimodal
formats such as MCAP (scalars + images), LeRobot (Parquet + MP4), and
ROS 2 bags.

**Two ingest modes for media**, matching the existing scalar plugin model.
Direct ingest is the v1 path and is what ships today. Delegated ingest
is now structurally supported by the v4 `pj_plugins` protocol (a parser
can bind both a scalar write service and an object write service at
`bind()` time — see Prerequisites) and is being wired up incrementally
on top of that protocol:

- **Direct ingest**: the DataSource plugin calls
  `object_write_host.register_topic()` itself and then pushes entries
  directly via `push_owned` (streaming) or `push_lazy` (file-backed). The
  plugin handles the raw bytes end-to-end, with no parser in the loop.
  For video topics on a future file-backed ObjectStore source, the design
  calls for the plugin to additionally publish a pre-computed keyframe
  timestamp list to a pj_scene2D-side `MediaIndexRegistry` sidechannel
  (not yet implemented — see Prerequisites). Today the live
  `StreamingVideoDecoder` builds its own keyframe vector inline (the
  host's only video decode path), so the registry is not on the critical
  path. Direct ingest is appropriate when the format is
  format-tight enough that a dedicated parser adds no value (raw-JPEG
  folder, LeRobot MP4, dedicated MCAP importer, etc.).

- **Delegated ingest**: the DataSource plugin registers the topic, obtains
  a parser binding via `host.ensureParserBinding({topic, encoding, ...})`,
  and on each frame calls `host.pushRawMessage(binding, ts, bytes)`. The
  host routes the bytes to a `MessageParser` whose `parse()` writes to
  topic-scoped scalar and object write hosts. The parser is **strictly
  codec-agnostic** — it peels the wire envelope (CDR, Protobuf, JSON,
  FlatBuffers, …) to expose header scalars and the opaque media payload,
  emitting each to the appropriate host. Codec concerns (keyframe
  detection, decoder state, GOP handling) live entirely in pj_scene2D's
  video decoder classes (`StreamingVideoDecoder` / `FfmpegDecoder`), never
  in the parser. Appropriate for serialized
  message streams where the same envelope carries many message types —
  a single "CDR parser" handles CompressedImage, CompressedVideo,
  CameraInfo, and more without codec knowledge.

**Parser contract — single entry point, registry-bound hosts**:

`MessageParser::parse(ctx, timestamp_ns, payload, out_error)` receives only
the raw payload bytes, a nanosecond timestamp, and an out-error — it does
NOT receive any host bindings as parameters. Instead the parser acquires
its write hosts at `bind(registry)` time from the service registry: the
scalar write host via `pj.parser_write.v1` (`PJ_parser_write_host_t`) and,
for media parsers, the object write host via `pj.parser_object_write.v1`
(`PJ_object_write_host_t`). A parser binds whichever services it needs.
During its single `parse()` call it walks the payload once and writes the
scalar portions to the bound scalar host and the media portions to the
bound object host. A ROS `sensor_msgs/CompressedImage` parser writes
`header.seq` and `header.frame_id` to the scalar host AND the JPEG bytes
to the object host from a single parse call. No double decode. (See the
Prerequisites note on protocol v4 + service-registry bindings.)

**Ownership summary** for media topics:

| Concern | Owner |
|---|---|
| Network / file transport | DataSource |
| Topic registration (`register_topic`) | DataSource, always |
| Topic metadata (`media_class`, `encoding`, `schema`) | DataSource at registration time |
| Frame reassembly from sub-frame packets | DataSource |
| Raw-bytes push (`push_owned` / `push_lazy`) | Parser (delegated) or DataSource (direct) |
| Video keyframe indexing | `pj_scene2d_core::StreamingVideoDecoder` inline keyframe vector (streaming, incremental NAL inspection) — the host's only video decode path. A separate `MediaIndexRegistry` sidechannel is designed for a future file-backed ObjectStore path but is not yet implemented. |
| Retention budget / eviction trigger | Application (budget) + ObjectStore (enforcement) |

**Frame granularity**: following the `VideoFrame` schema convention in
[`plotjuggler_sdk/docs/builtin_type.md`](../../plotjuggler_sdk/docs/builtin_type.md) (and matching
Foxglove and Rerun), each ObjectStore entry represents **exactly one
frame**. VideoFrame messages contain exactly enough data to decode one
frame, with SPS/VPS/PPS prepended on keyframes. DataSources that receive
sub-frame packets (e.g., RTP fragments) are responsible for reassembly
before pushing.

**Keyframe indexing is pj_scene2D's concern, not ObjectStore's.** This
is consistent with the `VideoFrame` schema, which carries no explicit keyframe
flag — keyframe detection is determined by parsing the bitstream (NAL unit type
for H.264/H.265, OBU type for AV1). The wire schema has none, and ObjectStore
also has none. Today the keyframe index lives inside the decoder that
needs it:

- **Streaming sources (implemented)**: `StreamingVideoDecoder` builds
  the keyframe index incrementally as entries arrive, NAL-parsing each
  new entry on the decoder thread. One-time inspection per entry;
  amortised over live playback at no user-visible cost. This is the
  host's only video decode path.
- **File-backed ObjectStore path (designed, not yet implemented)**: the
  original plan was for a DataSource plugin to pre-compute the keyframe
  list at open time and publish it via a C ABI slot
  (`object_write_host.publish_keyframe_index(topic, timestamps, count)`),
  with the host populating a pj_scene2D-side `MediaIndexRegistry`
  keyed by `ObjectTopicId`. Neither the C ABI slot nor the registry
  exist in code today. They are kept in the design (see
  ARCHITECTURE.md §6) for the day that path lands.

In every realised path the indexing lives in `pj_scene2d_core`, never
in ObjectStore. No pattern adds codec-specific fields or methods to
ObjectStore.

**Parser manifest carries no media routing**: parser manifests declare
only `encoding` (wire format such as `cdr`, `protobuf`, `json`) and
`schema` (logical message type such as `sensor_msgs/CompressedImage`).
There is **no `media_class` key in the parser manifest**. Media routing
is driven by the ObjectTopic's `metadata_json`, which is set by the
DataSource at `register_topic()` time (see bullet 1 of "DataSource
plugin responsibilities" below). Viewers read the topic's metadata —
not the parser's manifest — to pick a renderer. This keeps parsers
uniform across scalar and media topics and removes the temptation to
gate host wiring on a parser-level flag.

**DataSource plugin responsibilities**:

1. Register object topics via `object_write_host.register_topic()` with
   topic name and metadata JSON (e.g., `{"media_class":"video",
   "encoding":"h264", "schema":"foxglove/CompressedVideo"}`). The
   `media_class` key lives here on the topic, NOT in any parser manifest.
2. For file-backed sources: create lazy fetch callbacks that capture a
   `shared_ptr` to the file reader plus the message's seek location. Push
   via `push_lazy`.
3. For streaming sources: accumulate incoming bytes and push via
   `push_owned`.
4. **For video topics — implemented today, no DataSource action required**:
   streaming video sources rely on `StreamingVideoDecoder`'s incremental
   keyframe scan (the host's only video decode path). A future
   file-backed-ObjectStore path is designed to take a
   pre-computed keyframe list via
   `object_write_host.publish_keyframe_index(topic, timestamps, count)`,
   forwarded to a pj_scene2D-side `MediaIndexRegistry` keyed by
   `ObjectTopicId` — neither the slot nor the registry exist in code yet
   (see Prerequisites). When that path lands, the DataSource will not
   link `pj_scene2d_core` — communication will be through the C ABI
   write host only. Non-video media topics (self-contained JPEG/PNG,
   point clouds, scene primitives) do not need any keyframe information
   in any of these paths.
5. Publish optional rate hints (`preferred_fps`, `natural_range_ns`) so
   the application clock can pace playback appropriately.

**Timestamp mapping (LeRobot example)**: the DataSource plugin owns all
format-specific timestamp translation. A LeRobot plugin reading Parquet
and MP4 builds a PTS↔nanoseconds lookup table at open time, then creates
fetch callbacks that capture `{shared_ptr<demuxer>, pts}`. The ns
timestamp comes from the Parquet table; PTS is private to the closure.
pj_scene2D only ever sees nanoseconds — the PTS↔ns boundary is entirely
inside the plugin.

### 4.5 Time Synchronization

Media channels share the global timeline:

- Timestamps are `int64_t` nanoseconds since Unix epoch, matching
  `pj_datastore`.
- The global time cursor is owned by `pj_runtime::PlaybackEngine` (part
  of the application's `AppSession`). pj_scene2D widgets implement
  `PJ::IDataWidget` (from `pj_runtime/IDataWidget.h`); the runtime calls
  `onTrackerTime(double time)` on each tick. Widgets never drive the
  clock — they only receive time. (The original design called for a
  `TimelineCursor` header in `pj_base`; the realised model puts the
  contract in `pj_runtime` instead — see Prerequisites.)
- **Rate hints**: DataSource plugins publish optional hints at startup
  (`preferred_fps`, `natural_range_ns`). The timeline cursor aggregates
  hints across sources and picks a default playback pace, which the user
  may override.
- **MP4-dominated sessions**: when a DataSource has only MP4 data with no
  sidecar nanosecond timestamps, it synthesizes them (0-based or via a
  user-provided epoch offset) and publishes a rate hint. The clock honors
  the hint; playback proceeds at the MP4's natural rate.
- **Per-source stamps**: MCAP carries ns timestamps directly. MP4 requires
  a mapping table (LeRobot Parquet, or user-provided epoch). Live streams
  use source-provided stamps (e.g., ROS header.stamp) with time-of-arrival
  fallback when absent.
- **Multi-camera synchronization**: multiple camera widgets driven by
  the same `PlaybackEngine` all receive `onTrackerTime` on the same
  tick. Each widget queries its own topic independently at the current
  timestamp. There is no cross-widget rendezvous or barrier — widgets
  run in parallel and visible skew between them during heavy scrub is
  accepted (each displays whatever its own decoder last produced for
  the new time). Cross-camera timestamp alignment is the DataSource's
  responsibility (correct timestamps at push time); pj_scene2D trusts the
  timestamps it receives.

### 4.6 Viewer Decoder Pipeline

Decoders are built-in C++ classes inside pj_scene2D, not plugins. Third-party
extensibility is provided via DataSource and MessageParser plugins (new
file formats, new schemas), not via new codec plugins. This keeps codec
state — stateful for video, with per-viewer lifetime — out of the plugin
singleton model.

**Decoder taxonomy:**

| Decoder | State | Role |
|---------|-------|------|
| `FfmpegDecoder` + `StreamingVideoDecoder` | stateful, one instance per video layer | FFmpeg wrapper plus ObjectStore-aware GOP decoder with runtime HW-accel detection and guaranteed software fallback. Platform backend matrix is documented in `TECHNICAL_NOTES.md §3`. |
| `CodecPipeline` with `JpegCodec` / `PngCodec` / `ImageDecodeCascade` | stateless, one instance per image layer | Dispatches to turbojpeg (JPEG), libpng (PNG), or canonical raw image wrapping in `ImagePipelineSource`. Multiple instances in one widget are fine (they share no state). |
| `SceneDecoder` | stateless, one instance per scene/annotation layer | Single canonical-wire decoder (`foxglove.ImageAnnotations` Protobuf, hand-rolled, no libprotobuf). Source-format conversion (e.g. CDR `vision_msgs/Detection2DArray`) is loader-side; pj_scene2D only sees canonical bytes. Schema + canonical wire codec (writer + reader) live in `plotjuggler_sdk/pj_base/builtin/image_annotations.hpp` + `image_annotations_codec.hpp`, re-exported through `pj_plugin_sdk`. |

**Threading and decoder ownership**: each viewer widget is driven by one
`MediaSource` (`CompositeMediaSource` for the multi-layer case). The
source owns **one decoder instance per active layer** — a widget
compositing a base video + an annotation overlay + a depth colormap
instantiates one video decoder/source, one scene decoder/source, and one
image/depth codec pipeline. Decoders for different layers do not share
internal state.

The number of worker threads per widget is an **implementation detail**,
not part of the contract. Worker-backed sources (`ImagePipelineSource`,
`StreamingVideoSource`, `DepthPipelineSource`) own their own request/result
channels today; the synchronous source (`ScenePipelineSource`) publishes
its next result directly from `setTimestamp()`. Either way, three
contractual guarantees hold:

1. Decoders for different layers do not share internal state.
2. The UI thread polls exactly one `MediaSource` per widget — usually a
   `CompositeMediaSource` containing the ordered layer stack.
3. Stale frames from any layer cannot reach the display via any code
   path (worker-backed sources overwrite their latest result before the
   UI polls, and per-layer decodes feeding into the compositor are
   coalesced or cancelable per their source contract).

**Pull-based frame delivery (MediaSource latest-result mailbox)**:

The widget polls its attached `MediaSource` at render rate via
`takeFrame()`. Worker-backed sources keep a single latest decoded result
internally; a new result physically overwrites the previous one before
the UI can poll it. In the multi-layer case, `CompositeMediaSource`
polls each layer source, retains the latest contribution per layer, and
returns one composited `MediaFrame` to the viewer.

**Qt signals are NOT used for frame delivery.** The `Qt::QueuedConnection`
event-queue model is structurally incompatible with rapid scrub: queued
events cannot be invalidated, and stale frames pile up in the event queue
while newer ones are already elsewhere in the pipeline. This is a proven
failure mode — analyzed in the video_player_lab prototype (since
removed), with the rationale now inlined in `ARCHITECTURE.md §3.1`,
including the patch attempts that must not be retried. Do not
reintroduce push-based delivery.

**Direction-aware cancel-store**: when the user scrubs rapidly, in-flight
video decodes are cancelled before completion. The shipped sources do not
publish partial decode results: `StreamingVideoSource` cancels stale GOP
work with a `CancelToken` and deposits only complete decoded frames;
`ImagePipelineSource` and `DepthPipelineSource` coalesce stale timestamp
requests before the worker starts the next decode; the scene source decodes
synchronously on the caller thread. The video_player_lab direction-aware partial-publish rule
(forward partials allowed, backward partials suppressed) is preserved as
design rationale in `ARCHITECTURE.md §3.2` for a future file-backed decoder,
not as current behavior.

**Cancellation tokens**: streaming-video decode requests carry an atomic
flag polled by the decoder between NAL units. A new request flips the
previous token, and the decoder returns early without publishing a partial
frame.

**Keyframe seek**: video decoders use whichever keyframe index their
path provides (`StreamingVideoDecoder`'s in-decoder index for streaming
sources today — the host's only video decode path; a future
`MediaIndexRegistry` for the file-backed ObjectStore path — see §4.4) to
find the seek target in O(log k). For ObjectStore
paths they then call `ObjectStore::latestAt(topic, keyframe_ts)` for
the keyframe bytes, flush the FFmpeg context, and iterate forward via
`at(topic, i)` through subsequent P-frames to the target timestamp.
No backward walk through ObjectStore entries is needed. Because live
and scrub modes are mutually exclusive (§4.3), the buffer is frozen
while a scrub-time seek is in flight — no race is possible between
decoder seek and entry eviction.

### 4.7 Rendering

- **GPU-first**: the default rendering path is `QRhiWidget` with custom
  GLSL shaders. QRhi abstracts over Vulkan, Metal, D3D11, and OpenGL at
  runtime.
- **Color conversion**: YUV→RGB is performed in a fragment shader. The matrix
  (BT.601 vs BT.709) and range (limited vs full) are **selected per stream** from
  the codec's signalled colorimetry — not hardcoded — so SD / limited-range video
  converts correctly (the matrix is built once per frame by
  `pj_scene2d_core/video_color.h::buildYuvMatrix`; full-range BT.709 reproduces the
  historical matrix exactly). Hardware-decoded NV12 uploads natively as a two-plane
  R8+RG8 texture (no `sws_scale` repack); a CPU deinterleave fallback covers
  backends without RG8. No CPU-side color conversion.
- **Magnification**: image layers always magnify with nearest sampling (crisp,
  pixelated — for pixel inspection); a display hint on the frame, not a decode
  property. Depth always samples nearest (never blend the no-data sentinel).
- **Rectification toggle**: per-image-layer control to lens-undistort the image
  with its `CameraInfo` (on by default — matches the always-on auto-rectification).
  Turning it off forces raw passthrough, the operator's override when a stream is
  already rectified (avoids double-undistortion) or its detections live in raw
  space. Persisted in the layout XML; see TECHNICAL_NOTES §12.
- **Zoom and pan**: the viewer supports zoom (mouse wheel, cursor-anchored)
  and pan (mouse drag) via a view transform matrix in the vertex shader.
  No pixel reprocessing; transformation is free on the GPU. This
  functionality is a carry-over from the prototype and is a hard
  requirement.
- **Rotation**: a dock-wide clockwise rotation of 0°, 90°, -90° or 180°
  (the side panel's Rotation selector, below the layer list) turns every
  layer together — images, pixel layers, overlays — through the same view
  transform, and the point inspector maps through its inverse. It is a
  setting, not a gesture: double-click reset clears zoom/pan but keeps it.
  Persisted in the layout XML (`<view rotation="…">`). Text labels stay
  upright: their anchor moves with the image, the glyphs do not turn.
- **Software fallback**: when no GPU/HW path is available, the decoder
  falls back to software decode and the widget uses a CPU upload path.
  Acceptable degradation; the UX remains functional.
- **Widget size / multi-camera layout**: each camera is rendered by an
  independent widget. Layout (grid, tabs, single focus) is an
  application-level UI concern, not a pj_scene2D requirement.

### 4.8 Multi-Layer Compositing

A viewer widget may composite multiple ObjectStore topics at the same
display time (base image + annotation overlay + depth colormap +
segmentation mask). Each layer has its own decoder instance owned by
the widget's `MediaSource` (`CompositeMediaSource`; see §4.6 "Threading
and decoder ownership"). Decoders for different layers are independent
and do not share state. On each render tick:

1. For each layer: `store.latestAt(topic, render_time_ns)` returns the
   owning byte handle.
2. Each layer's decoder decodes its result independently.
3. The compositor combines per-layer outputs per the widget's layer
   ordering and blending configuration, and returns the composited
   `MediaFrame` through the widget's attached `MediaSource`.

The UI thread polls that one `MediaSource` and displays the composited
result. There is no per-layer source exposed to the viewer — compositing
always happens before the frame reaches the display.

**At-or-before semantics are strict**: the compositor always uses the
entry at or before the current display time, never a future entry even if
closer in time. Causality is preserved — an annotation from time
`t + 10 ms` never appears on an image rendered at time `t`.

**No automatic skew rejection**: multi-rate data is the norm — a 10 Hz
detection overlay composited on a 60 Hz video is correct when the same
bounding box persists across 5 consecutive video frames. The compositor
does NOT drop a layer for being "too old"; it trusts that an entry
exists in the store until it is explicitly evicted.

**frame_id correlation (optional)**: when an overlay's metadata carries a
`frame_id` matching a specific source image, the compositor may prefer
exact pairing over nearest-timestamp matching. This is an optional
feature for layers that publish correlation metadata; layers without
`frame_id` fall back to timestamp-based pairing. See the per-type `frame_id` field
definitions in [`plotjuggler_sdk/docs/builtin_type.md`](../../plotjuggler_sdk/docs/builtin_type.md) for `frame_id` semantics.

**Layer ordering and blending modes** (direct color, colormap, false-color,
alpha blending) are widget configuration, not part of the data model.

**One background per dock**: a full-frame source (image, depth image,
video) is the dock's background; overlays (image annotations, scene
entities) stack on top of it. Dropping a topic onto a dock
(`Scene2DDockWidget::tryAcceptObjectTopic`) appends an overlay, but a
dropped full-frame source *replaces* the current background and is drawn
beneath every overlay. The old background is removed only after the new
one attached. A dropped topic that is advertised but has no data yet is
deferred and gets the same policy when it materializes
(`restoreOnePending`, `pending_intent` elements only). Workspace restore
and the programmatic `addTopic()` keep appending, so a saved multi-image
layout still loads as saved.

## 5. Non-Functional Requirements

- **Language**: C++20, consistent with `pj_base` and `pj_datastore`.
- **Qt**: required for the widget layer only. The core decoder layer
  (`pj_scene2d_core`) has no Qt dependency.
- **Decoder libraries**: FFmpeg (video, HW-accel), libturbojpeg (JPEG),
  libpng (PNG). Runtime HW-accel detection with software fallback; no
  compile-time codec switches.
- **Thread safety**: ObjectStore owning handles allow concurrent holds
  by decoder workers across widgets and across layers within a widget
  (see §4.6 "Threading and decoder ownership"). No shared decoder
  state between widgets or between layers.
- **Deterministic scrub**: pull-based delivery via `MediaSource::takeFrame()` guarantees
  the displayed frame is always the newest decoded frame. Stale frames
  cannot reach the display via any code path, and shipped sources publish
  only complete frames — no partial decode result is displayed.
- **Error reporting**: fallible public API calls return
  `PJ::Expected<T>` following the `pj_base` convention. No C++
  exceptions cross pj_scene2D's library boundary, and no exceptions
  cross the decoder-worker → UI-thread boundary — worker-thread
  exceptions are caught at the thread boundary and converted into a
  last-error state queryable on the affected widget. User-facing
  decode failures (corrupt data, unsupported codec on input, HW-decode
  error, etc.) are handled inside the widget: the affected layer
  displays a visible error indicator (e.g., "decode failed" overlay
  or a no-signal background) rather than crashing or silently stalling
  the UI. **Missing data** (for example `latestAt` returning empty on
  a valid topic) is **not** an error — it is an expected state
  rendered as "no data" without raising, surfacing, or logging an
  error condition.
- **Clean under AddressSanitizer (ASAN)** in debug builds.
- **Builds with `-Wall -Wextra -Werror`**.
- **Timestamps are `int64_t` nanoseconds** at every external boundary.
- **No audio**.

## 6. Module Contract

pj_scene2D is delivered as two CMake targets with a strict dependency
direction:

- **`pj_scene2d_core`** — pure C++ library containing media-source
  adapters, image/video/depth/scene decoders, codec pipelines, NAL/keyframe
  helpers, decoded frame types, and composite media-source logic. Depends on `pj_base`,
  `pj_datastore`, FFmpeg, turbojpeg, libpng. **No Qt dependency**.
  DataSource plugins do NOT link this library — plugins depend only
  on `pj_base` (see `pj_plugins/docs/ARCHITECTURE.md`). Format-specific
  helpers (NAL scanning, PTS lookup) belong inside the DataSource
  plugin itself or in lightweight headers within `pj_base`.

- **`pj_scene2d_widgets`** — Qt widget library built on top of `pj_scene2d_core`.
  Contains `QRhiWidget`-based viewers, GLSL shaders, widget-level
  configuration, and any Qt-specific integration. Depends on
  `pj_scene2d_core`, `pj_scene_common`, `pj_runtime`, `pj_widgets`, plus
  Qt 6.11.1 (the build's `find_package(Qt6 6.11.1 …)` floor).

**Consumer contract:**

- pj_scene2D is a **read-only consumer** of `pj_datastore`. It never writes
  to `DataEngine` or `ObjectStore`. Writes happen through plugin host
  interfaces owned by the application.
- Widgets implement `PJ::IDataWidget` (from `pj_runtime`) and are driven
  by `pj_runtime::PlaybackEngine` via `onTrackerTime`. They never own or
  drive the clock.
- Decoder frame delivery is pull-only. No public signals for frame
  delivery. Adding a signal escape hatch is explicitly forbidden because
  it reintroduces the Qt event-queue staleness bug.
- Channel identifiers are strings (topic names), consistent with
  `pj_datastore` topic naming.

**Widget lifecycle and ownership:**

The host application is always responsible for creating and destroying
pj_scene2D widgets. pj_scene2D does not auto-instantiate widgets from
dataset contents; the application decides when to open a viewer for
a topic.

*Construction* — a widget is created with a reference to the application's
`SessionManager` (which exposes `ObjectStore` access), and is bound to a
specific topic via `setImageTopic()` (or the equivalent setter for the
widget kind). The widget then implements `IDataWidget`, and the
application registers it with the active `PlaybackEngine` so it begins
receiving `onTrackerTime` calls. On construction (and topic bind), the
widget:

1. Queries the topic's `metadata_json` and canonical object type to
   pick an appropriate internal viewer type (image viewer, video
   viewer, scene viewer) and the decoders for its active layers.
2. Instantiates one decoder instance per active layer (see §4.6
   "Threading and decoder ownership").
3. Starts any worker thread(s) owned by the underlying `MediaSource`
   implementation (`ImagePipelineSource`, `StreamingVideoSource`).

*Destruction* — on destruction, the widget:

1. Stops receiving `onTrackerTime` (the runtime drops it from its
   driven-widget set).
2. Requests cancellation on any in-flight asynchronous decodes via the
   `MediaSource`'s internal cancellation mechanism where available.
3. Stops and joins all worker threads spawned by worker-backed `MediaSource`
   implementations.
4. Releases any owning byte handles it holds from ObjectStore.

*Teardown order* — the application must destroy widgets **before**
destroying the `AppSession` (and the `SessionManager` / `PlaybackEngine`
it owns) or the `ObjectStore` they reference. pj_scene2D does not manage
cross-module lifetime; it trusts the caller. Destroying any of those
while a widget still holds references is undefined behaviour.

*Dataset close / topic removal* — when a topic is removed from
ObjectStore (dataset unload, explicit `removeTopic`), widgets observing
that topic **do not auto-destroy**. Their next query returns an empty
`timeRange`, and they display a "no data" indicator. The application
is responsible for explicitly destroying widgets when it decides they
are no longer relevant.

*File reload / hot-swap* — the application tears down existing widgets
before loading a new dataset. pj_scene2D does not support hot-reloading
a widget onto a different dataset while the widget is alive.

## 7. Deferred / Out of Scope

- **Audio** — no decoding, display, or synchronization of audio tracks.
- **3D rendering** — point clouds, 3D scene primitives, and grids are
  stored in the same ObjectStore but rendered by the `pj_scene3D` widget
  family, not by pj_scene2D.
- **Reverse playback** — forward-only. Backward scrub is supported via
  keyframe seek + decode-forward, but continuous reverse-direction
  playback is not.
- **Recording and encoding** — pj_scene2D is decode/display only. No
  encoding of raw frames into new video files.
- **Pluggable codecs** — codecs are built-in. Adding a new codec (e.g.,
  H.266) requires a pj_scene2D code change, not a plugin.
- **HDR** (BT.2020 / PQ / HLG) — current target is BT.601 / BT.709 SDR.
  HDR is a future color-pipeline extension.
- **Persistence of decoded frames to disk** — decode is always done fresh
  from the raw bytes in ObjectStore.
- **Screenshot / frame export** — the user cannot export a displayed frame
  as a standalone image file from the viewer. May be added later as a
  feature but is not part of the initial requirements.
- **Stateful video parsers** — parsers stay stateless per message. Video
  decoder state lives inside pj_scene2D's `StreamingVideoDecoder` /
  `FfmpegDecoder`, not in any plugin.
- **Dataset format support beyond documented types** — MCAP, LeRobot,
  RLDS, Zarr. See `docs/research/dataset_format_comparison.md` (at the
  repo root) for coverage.
