# pj_scene2D Technical Notes

Implementation-relevant technical knowledge for the pj_scene2D module — a
supplementary knowledge base sitting alongside `ARCHITECTURE.md`. Where
`ARCHITECTURE.md` defines HOW the module is built today, this file captures the
**domain background** that informed those decisions: Qt specifics, codec
caveats, platform HW-accel matrices, debugging notes, and lessons brought over
from the standalone experiment.

Read `ARCHITECTURE.md` first for the module's current shape; come here for
"why this Qt API and not that one" or "what we learned from prior attempts."

Sources: the standalone pj_scene2D experiment, the video_player_lab
prototype (both working trees since removed; their lessons are captured
here and in `ARCHITECTURE.md`), and design discussions for the
integrated module.

---

## 1. Qt Video Rendering

### QMediaPlayer Limitations

QMediaPlayer is unsuitable for PlotJuggler's needs:

| Limitation | Detail |
|---|---|
| No frame-accurate seeking | Lands on nearest keyframe only |
| No frame stepping | No `stepForward()`/`stepBackward()` API |
| No low-latency streaming | RTSP has 2-3s latency; no native SRT |
| No external clock sync | Cannot tie playback to an external timeline |

A custom decode pipeline is required. PJ4 does not use Qt Multimedia at all:
FFmpeg frames go straight into the viewer's own QRhi textures.

### QRhiWidget — Primary Render Path

`QRhiWidget` with custom shaders is the primary render path:

- QRhi abstracts over Vulkan, Metal, D3D11, OpenGL
- **YUV420P texture support**: 3 R8 textures (Y full-res, U half-res, V half-res)
  uploaded per frame. 75% less GPU memory than a single RGBA8 texture
  (1.5 bytes/pixel vs 4 bytes/pixel). YUV->RGB conversion in the fragment shader.
- **NV12 texture support** (hardware-decode download format): Y as an R8 texture +
  interleaved UV as an **RG8** texture (two-plane), avoiding the per-frame
  `sws_scale` repack to YUV420P. `isUploadablePixelFormat(kNV12)` is gated on the
  backend; `nv12ToYuv420p` (decoded_frame.h) is the CPU fallback for backends
  without RG8 and for CPU consumers (thumbnail encoder).
- **Colorspace-aware conversion**: the YUV->RGB matrix is selected per frame from
  the codec's `AVFrame` colorimetry (BT.601 vs BT.709, limited vs full range) and
  built by `pj_scene2d_core/video_color.h::buildYuvMatrix`. It is a single affine
  4x4 (range scale/offset folded into the constant column), so the shader is
  unchanged across all combinations; BT.709 + full range reproduces the historical
  hardcoded matrix bit-for-bit. See §5 "Colorimetry was dropped" for the bug this
  fixed. A deprecated YUVJ pixel format counts as full range, and the decoder's
  swscale repack keeps the source range (`keepRange`): by default swscale squeezes
  a full-range YUVJ frame to limited range while the frame stays tagged full.
- **4:2:0 chroma siting**: chroma sample (x/2, y/2) belongs to luma pixel (x, y).
  `yuv_to_rgb.frag` samples chroma at `uv` scaled per axis by n / (2 * chroma_n),
  from the `textureSize` of `y_tex` and `u_tex` (both layouts), so at an odd size
  (AV1 allows one; H.264 and HEVC crop 4:2:0 to even) the ceil(n/2)-sample chroma
  plane is not stretched over the frame; even sizes select `uv` unchanged. The
  chroma sampler follows the frame's `mag_filter` like luma: nearest for images
  (one chroma sample per 2x2 block), bilinear for video. `MediaViewerOddSizeTest`
  in `media_viewer_render_test` checks a 17x17 frame in both layouts.
- **CPU rgb24 for on-demand scripts** (`convertToRgb24`, `ffmpeg_decoder.h`): the same
  matrix and chroma siting as the shader, with nearest chroma, through swscale, **within 3 levels
  per channel** of it (2 at full range; about 0.5 % of limited-range samples are off by
  3). At a width that is a multiple of 16 and an even height it takes `SWS_POINT` alone:
  swscale's unscaled converter for YUV420P (0.65 ms per 1080p frame), its generic scaler
  for NV12 (2.6 ms); any other frame takes the exact rounding (within 1, about 11 ms),
  because the unscaled converter's x86 kernel writes 16 pixels at a time and there left
  a row's last pixels unwritten or wrote past the frame, and swscale uses it only at an
  even height (NV12 follows the same gate, its bound being measured on those frames) (`VideoRgbConversion` tests in `streaming_video_decoder_test.cpp`).
- RGB/BGR, Mono8 and depth frames take their own texture paths through the
  same pipeline (`TexturePathFormat` in `media_viewer_widget.cpp`)
- Zoom (mouse wheel) and pan (mouse drag) via view transform matrix in
  vertex shader — zero CPU-side pixel processing
- Hardware-decoded frames are downloaded to system memory and uploaded like
  software frames; see "GPU Zero-Copy Path" (§3) for the import that is not built

### Shader Toolchain

The widget consumes offline-compiled Qt shader blobs (`*.qsb`) through
`QShader::fromSerialized()`. `shaders/shaders.qrc` embeds the `.qsb` files;
the neighboring `.vert` / `.frag` files are reference inputs. Editing GLSL
sources has no runtime or build effect until the matching `.qsb` file is
regenerated with Qt's `qsb` tool from the `qtshadertools` module. Re-bake every
pack with `shaders/bake.sh`, using the `qsb` of the pinned Qt (pack bytes depend
on its version; desktop Qt kits do not ship one, a full Qt or the WASM kit does):

```bash
widgets/shaders/bake.sh /path/to/qsb
```

The script is the single source of the targets, following the dialect policy in
`cmake/PjQsb.cmake`. The image pass (`yuv_to_rgb`) bakes WebGL2, GLSL 330 (an
OpenGL 3.3 context, or the macOS 4.1 core context of the Metal-to-OpenGL
fallback) and GLSL 440. The overlay packs (`scene_lines`, `scene_text`) bake
GLSL 100 es, 120, 150 and 300 es; a 3.3 or 4.1 core context uses 150. All carry
HLSL 50, MSL 12 and SPIR-V. One pair of overlay shaders serves both the point
quads and the outlines: they differ only in the geometry the CPU builds.

In an Emscripten configuration, `widgets/CMakeLists.txt` re-runs `bake.sh` into
the build tree and compares SHA-256 hashes with the committed packs, so
configuration fails if a source and blob diverge, a pack is missing, or one is
rebaked with the wrong target set. `media_viewer_render_test` draws every pack on
each backend of the platform (and at the OpenGL 3.3 floor in Linux CI). A general
`qt6_add_shaders` migration remains deferred work tracked as F-009.

### QRhiWidget in a window first shown without one

A top-level first shown with no `QRhiWidget` gets a raster backing store. When a
`QRhiWidget` joins it later (a media or Scene3D dock added to a window whose
plots use the raster canvas), Qt switches the window to QRhi composition (found on 6.11.1, confirmed on 6.12.0),
on desktop and in the browser; no zero-size bootstrap widget is needed. Qt only
does this when the `QRhiWidget` is reparented into the window
(`QWidgetPrivate::setParent`), and a `QRhiWidget` constructed with its parent is
not one yet when `QWidget`'s constructor sets that parent, so the window stays
raster (`QRhiWidget: No QRhi`). Every `QRhiWidget` is therefore constructed
parentless and then inserted (`MediaViewerWidget`, `PlotRhiCanvas`,
`SceneViewWidget`). `rhi_mixed_dock_test` covers both paths:
`RhiMixedLateDockFixture` (GPU docks added after a raster-only first show) and
`RhiScene2DInPlaceTest` (a Scene2D dock built in place inside the shown window,
as the app's dock factory does, with no other GPU widget to switch it).

The browser adds its own ordering. Qt/WASM must not create a zero-size WebGL
surface before a browser file picker: the context can be lost during the picker
round trip, after which Emscripten's context-attribute path may dereference a
null result. The measured browser lifecycle is:

1. create no GPU widget before the first real viewer;
2. construct the real `MediaViewerWidget` parentless, so its constructor calls
   `setApi(PJ::preferredGraphicsApi())` (OpenGL/WebGL2) before `QStackedWidget`
   adds it to the visible hierarchy;
3. let that nonzero widget switch the top-level from raster to RHI composition;
4. after its first `frameSubmitted`, queue one full top-level repaint so the
   pre-existing raster regions are uploaded to the mixed compositor; and
5. re-arm that repaint in `releaseResources()` for a changed QRhi/top-level.

Constructing the browser viewer with its already-attached parent produced
repeated `QRhiWidget: No QRhi` and a blank dock. The parentless-before-API-fixed
ordering follows QRhiWidget's documented one-time API selection rule. A QRhi
framebuffer readback proves GPU output, while a separate full-window snapshot is
required to catch uninitialized black raster regions after the compositor switch.

### Backends and reparenting

The viewer requests `PJ::preferredGraphicsApi()`: the platform's native API
(D3D11, Metal, OpenGL on Linux). Its
shaders are baked for every backend. The view matrix is built in GL clip space,
which QRhi scissors also use; the shaders receive `clipSpaceCorrMatrix() * view`,
so Vulkan's y-down clip space does not flip the image.

Floating, redocking or switching an ADS tab moves the viewer through another
top-level, so Qt calls `releaseResources()` and then `initialize()` on the new
QRhi. The viewer keeps its last frame and annotations on the CPU and re-uploads
both. `rhi_dock_lifecycle_test` (pj_app) checks orientation and overlays across
a float, a redock and a tab switch on OpenGL and Vulkan.

### Render target, uploads and limits

- **Colour-only target.** Nothing in the viewer depth-tests, so it turns
  `autoRenderTarget` off and builds its own `QRhiTextureRenderTarget` over
  `colorTexture()` in `initialize()` (`PJ::RhiColorTarget`: rebuilt on a resize,
  its render-pass descriptor kept so the pipelines survive): QRhiWidget's automatic target would
  add a depth-stencil renderbuffer of 4 B/px that nothing uses.
- **Unchanged layers upload nothing.** Each texture layer keeps the
  `shared_ptr` of the pixel buffer its textures hold. A frame (or a composite's
  layer) that brings the same buffer only refreshes its per-frame state (opacity,
  depth range, colour matrix); holding the reference keeps a new buffer from
  reusing the address meanwhile. A re-created texture (size, format, filter) or a
  new QRhi always uploads. Producers hand out a fresh buffer per decoded frame.
- **Texture size limit.** A frame wider or taller than `QRhi::TextureSizeMax`
  is not drawn (its layer is dropped) and a warning is logged once per viewer;
  `setMaxTextureSizeForTest()` lowers the limit so `rhi_dock_lifecycle_test`
  needs no gigabyte image.
- **Glyph cache.** Text overlays cache one R8 texture per (text, size). After
  each text rebuild the least recently used labels that rebuild did not draw are
  evicted down to 512 entries, so a stream of ever-new strings (counters,
  timestamps) cannot grow it without bound, and a frame with more than 512 labels
  keeps exactly its own and re-draws them without rasterizing any again.
- **Shader packs** are read once per process and shared by every viewer and QRhi.

### GPU failure state

No draw may reference a resource whose `create()` failed (D3D11 crashes in
`beginPass` on one). When the image pipeline, its samplers or the base texture
layer cannot be created, `initialize()` releases everything and the viewer enters
its failed state: `render()` only clears to the background, `renderFailed()` and
`renderingWarningChanged()` fire once, and `Scene2DDockWidget` shows the warning
in a label below the viewer (not over it: a raster label does not composite on
the QRhiWidget's surface). Every later `initialize()` (a new QRhi after a
reparent, or a new render-target size) retries. Optional parts degrade instead:
a failed overlay pipeline or buffer drops that overlay, a failed frame texture
drops that frame, and without the blending pipeline the pixel layers draw opaque
through the image pipeline, each with a warning logged once per QRhi. Every
`initialize()` rebuilds whichever optional part is missing, so a dropped overlay
or blending pipeline returns with the next new render target. A window that gets
no QRhi at all (its API differs from the one another widget pinned the window
to) never reaches `initialize()`: the viewer routes QRhiWidget's own
`renderFailed()` into the same warning.

A frame's texture uploads go to a resource-update batch of their own, merged into
the frame's batch only once every texture and binding of the layer exists: a
dropped layer deletes textures an upload already queued for it (a depth frame's
colormap, the frame planes) references, so that batch is discarded, never
submitted.

Builds with tests (`PJ_RHI_TEST_HOOKS`, `pj_widgets/RhiTestHooks.h`) give the
viewer its fault injection: the `pjTestFailPipeline` dynamic property (`base`,
`composite`, `points`, `thick`, `text`, or `layer srb` for a frame layer's
bindings) fails that creation, and the `pjTestRhiAdoptions` /
`pjTestRhiReleases` / `pjTestRhiStale` counters record which QRhi it holds
resources on. `rhi_mixed_dock_test` (pj_app) uses both beside a working plot and
Scene3D view, then clears the fault and checks the recovery;
`rhi_dock_lifecycle_test` checks that a dropped overlay returns and that a depth
frame whose bindings fail is dropped with its uploads.

### Static JPEG ABI in the monolithic WASM module

Qt's static JPEG image plugin and Scene2D's static TurboJPEG archive occupy one
WASM symbol namespace. Both export process-global `jpeg_*` symbols, so a normal
successful static link can bind a caller from one archive to the implementation
from the other. Qt's kit declares `JPEG_LIB_VERSION 80` (found on 6.11.1; re-check on the pinned Qt); libjpeg-turbo's
default build emulates v6b (`62`). That mismatch crashed during splash pixmap
construction before `MainWindow`.

The WASM dependency provider therefore reads the selected Qt kit's
`QtJpeg/jconfig.h`, fails unless its ABI is the reviewed v8 contract, and builds
TurboJPEG with `WITH_JPEG8=ON`. Matching only the library versions is
insufficient here: the C ABI selected by the build option is the load-bearing
contract. Desktop continues to use its independently packaged dependencies.

### Video decode in the WASM module

The browser has no native hardware-decode API that FFmpeg can drive, so its
`FfmpegDecoder` is software-only. `cmake/PjWasmFfmpeg.cmake`
(`PJ_WASM_WITH_VIDEO`, on with Scene2D) cross-compiles the desktop's FFmpeg
release and dav1d into static archives with the same LGPL decode-only
allowlist (`--disable-gpl --disable-nonfree --disable-autodetect`, decoders
h264/hevc/mjpeg/av1/libdav1d), plus `-pthread` so libavcodec's frame threads run
on Emscripten workers. It omits libavformat (the core decodes elementary
streams; only its desktop tests demux MP4), assembly (it targets native CPUs)
and every hwaccel. The archives are imported as `ffmpeg::avcodec` / `avutil` /
`swscale`, the names the desktop graph provides, so `pj_scene2d_core` and the
`VideoLayer` build unchanged. Configuring needs `meson` (dav1d), GNU `make` and
`pkg-config` on the host.

**Additional lifecycle rules**:

- Override `releaseResources()` (Qt virtual) to delete all QRhi
  resources (pipeline, textures, buffers). Qt calls this when the
  underlying QRhi instance changes (reparenting, window move).
- Track `QRhi* rhi_cached_` in `initialize()`. If `rhi() != cached`,
  call `releaseResources()` and reinitialize — resources from the old
  QRhi are invalid.
- In any method that calls `update()` before the first successful `initialize()`, guard with
  `if (pipeline_ != nullptr)` — calling `update()` before init floods
  Qt with render requests that can't be fulfilled and starves the
  initialization path.
- At the end of `initialize()`, call `update()` if a frame is pending
  — otherwise the first frame set before init completes is lost.

---

## 2. Codec Details

### Support Matrix

| Codec | Latency | Compression vs H.264 | HW Decode Support | Status |
|---|---|---|---|---|
| H.264/AVC | Lowest | Baseline | Universal | Primary target |
| H.265/HEVC | Low | ~40% better | Wide (GPU 2018+) | Supported |
| AV1 | Medium | ~50% better | Growing (RTX 40+, RDNA 3+) | Supported |
| VP9 | Medium | ~35% better | Moderate | Not on the streaming path |

The streaming `VideoFrame` decode path supports **h264 / h265 / av1** only.
`vp9` stays an accepted `VideoFrame.format` wire value (see `VideoFrame` in [`plotjuggler_sdk/docs/builtin_type.md`](https://github.com/PlotJuggler/plotjuggler_sdk/blob/main/docs/builtin_type.md) and `pj_base/builtin/video_frame.hpp`) but has
no renderer: it has an FFmpeg decoder yet no keyframe oracle (it cannot be
sought), so `videoCodecIdFromFormat()` returns `AV_CODEC_ID_NONE` and the
decoder rejects it with an explicit "unsupported video codec" error rather than
mis-decoding. VVC/H.266 has near-zero hardware support — not targeted.

### B-Frame Support

While Foxglove and Rerun conventions recommend no B-frames, the
streaming decode path (`StreamingVideoDecoder` / `FfmpegDecoder`)
supports B-frame streams. With B-frames **decode order (DTS) differs from
presentation order (PTS)**, and the pipeline must keep the two straight:

- **The ObjectStore is keyed by DTS** (monotonic decode order — required for the
  lazy/append index). The real PTS travels in the `VideoFrame.timestamp_ns`
  payload and is surfaced to the decoder via `ExtractedFrame::pts`.
- **Feed in DTS order, serve by PTS**: `serveForward` feeds packets in store
  (DTS) order but sets `pkt->pts` to the real PTS, so `AVFrame::pts` comes back in
  presentation order. `StreamingVideoDecoder` keeps a **presentation index**
  (`pts_to_dts_`, built during the keyframe scan); `decodeAt(T)` resolves the
  frame to show as the greatest PTS ≤ T, then seeks/feeds in decode order. Serving
  is in PTS space (`ready_frames_`, `last_served_ts_`).
- Without this, B-frame video plays back in decode order — frames jitter ("vibrate")
  and the forward-continuation thrashes into per-frame GOP re-decodes (high CPU).
  With no B-frames PTS == DTS, so all of the above is the identity.
- For MCAP-stored video, I+P only (no B-frames) is still recommended
  for simpler seeking.

### Annex B NAL Units

The wire format for H.264/H.265 streaming and MCAP storage:

- Start codes: `00 00 00 01` prefix before each NAL unit
- H.264 keyframes: SPS + PPS NAL units before IDR slice (NAL type 5)
- H.265 keyframes: VPS + SPS + PPS NAL units before IRAP slice
  (NAL types 16-21)
- AV1: Sequence Header OBU before KEY_FRAME OBU (low overhead bitstream)
- Each message contains exactly enough data to decode one frame

### NAL Parser for Keyframe Detection

Required for building keyframe indices from raw bitstreams (MCAP
CompressedVideo, live streams):

- H.264: IDR detection via NAL unit type 5
- H.265: IRAP detection via NAL unit types 16-21
- Parse first byte after start code to extract NAL type
- No need to parse slice headers or reference lists — only
  keyframe/non-keyframe classification is needed

---

## 3. Hardware Acceleration

### Runtime Detection

- Detect available HW backends at runtime, not compile time
- No compile-time flags or conditional compilation for HW accel
- Probe available backends via the decode library's hardware device API

### Fallback Chains

| Platform | Chain |
|---|---|
| Linux | VAAPI → CUDA/NVDEC → software |
| Windows | D3D11VA → software |
| macOS | VideoToolbox → software |
| Browser (WASM) | software only |

### Device Selection

`FfmpegDecoder::open()` does not hard-code the chain above. It iterates the HW
backends the running FFmpeg was actually built with (`av_hwdevice_iterate_types`)
and keeps the first that exposes a HW decode config for the stream's codec — so
the binary's capabilities, not a static list, decide what is tried.

For **VAAPI** the device is a specific DRM render node, and the choice matters on
multi-GPU machines: `av_hwdevice_ctx_create(..., nullptr, ...)` opens the
*default* node (`/dev/dri/renderD128`), which is frequently a GPU without a VAAPI
driver (e.g. an NVIDIA dGPU on the proprietary driver) while the VAAPI-capable
GPU sits on `renderD129`. `tryHwDevice()` therefore, for VAAPI on Linux:

1. honors a `PJ_VAAPI_DEVICE=/dev/dri/renderD<N>` override (debugging/forcing);
2. otherwise walks `/dev/dri/renderD128..191` and takes the first node whose
   VAAPI driver initialises.

This walk is Linux-only (`#ifdef __linux__`) — VAAPI itself is Linux-only, and
every other backend (CUDA, D3D11VA, VideoToolbox) has no render-node concept and
uses the library default device. A backend that opens but cannot actually decode
the codec/profile degrades to software per-frame (the existing fallback), so we
deliberately do **not** pre-query `vaQueryConfigProfiles`.

### GPU Zero-Copy Path

Not built. `FfmpegDecoder::avFrameToDecodedFrame` downloads every hardware frame
to system memory (`av_hwframe_transfer_data`, ~0.5-2 ms per 1080p frame) and the
viewer uploads it like a software frame, which is acceptable for typical
robotics resolutions. The download fills a fresh frame with pixels only, not
frame properties, so the decoder reads the colour tags from the decoded frame
(`ffmpeg_decoder_test`'s `DecodedFrameKeepsTheSignalledColourTags`).

A zero-copy import would skip the round trip, at the cost of interop code and
synchronization for each backend:

1. HW decoder produces frames on GPU (VAAPI surface, D3D11 texture, etc.)
2. Export as DMA-BUF (Linux) or shared texture handle (Windows/macOS)
3. Import into QRhi as `QRhiTexture` via `createFrom()`
4. Render directly — no pixel copy

---

## 4. Seeking Strategy

### File-Based Seeking

1. Build keyframe index at file open time:
   - MP4: parse `stss` (Sync Sample) atom
   - MCAP: read Summary section at EOF for O(1) access to the full message
     index; classify keyframes via NAL type inspection
   - Store as sorted vector of `{timestamp, byte_offset}`
2. Seek to nearest preceding keyframe
3. Flush decoder state (mandatory after every seek)
4. Decode forward from keyframe to target frame, discarding intermediate
   frames
5. Return the target frame

### Repaint cadence: composite at the video frame rate

`MediaViewerWidget` is a `QRhiWidget` rendered **non-natively** (inside the
widget / ADS dock hierarchy), so every repaint pays a GPU→CPU framebuffer
readback (`QRhi::endOffscreenFrame`) plus a raster composite into the window
backing store. The host advances the clock at ~60 Hz, but the picture only
changes at the video frame rate (~25–30 fps) — so repainting on every tick, half
of them on an unchanged frame, would make that compositing dominate playback CPU
(measured: this `rgb888→rgb32` composite was ~17% of the GUI thread during
large-cloud + camera playback before the gate below).

The fix lives in `pj_scene_common`: `SceneDockWidget::onTrackerTime` skips the
per-tick layer advance + `refreshView()` when the combined
`ISceneLayer::renderKey(time)` of the visible layers is unchanged.
`Scene2DLayer::renderKey` fingerprints the active sample's STAMP (a decode-free
`indexAt`/`entryTimestamps` lookup), so a static image/depth frame coalesces away
its redundant 60 Hz re-composites. Time-only overlays do not need a special case:
`ImageAnnotations` is its own `SceneDecoderLayer` with its own topic, so its stamp
enters the dock's combined key and an annotation-only change still reopens the
gate. A new base frame's async decode additionally repaints via the frame-ready
callback (`repaintRequested → refreshView`), independent of the gate. The
streaming video path (`StreamingVideoSource`) likewise surfaces a new frame only
when its worker delivers one.

### EntryThumbnailCache (streaming thumbnails)

`EntryThumbnailCache` provides instant backward scrub feedback for streaming
`VideoFrame` topics by pre-decoding keyframes into HD-capped JPEG thumbnails:

- **Background build**: a dedicated builder thread decodes ~1 keyframe per
  adaptive interval through its own per-build NAL extractor, encoding each via
  the stateless `thumbnail_codec.h`.

- **HD cap**: frames wider than 1280px (e.g., 4K/1080p) are downscaled to
  ≤1280px before caching, bounding memory while keeping enough quality for a
  scrub preview.

- **JPEG / YUV420P throughout**: thumbnails are stored as JPEG at quality 80.
  Decompression outputs YUV420P directly, so the same media shader renders both
  cached and live frames with no path mismatch. (A live frame from hardware decode
  arrives as NV12; the thumbnail encoder deinterleaves it to YUV420P first via
  `nv12ToYuv420p`, so the cached preview stays planar.)

- **Usage pattern**: during backward scrub, `StreamingVideoSource` serves the
  nearest-at-or-before thumbnail for instant feedback while the full-resolution
  GOP decode settles.

### GOP-Aware Buffer Eviction

For streaming buffers holding compressed video:

- Track GOP boundaries (keyframe positions) in the buffer
- Evict entire GOPs as a unit — never remove a keyframe while its
  dependent P-frames remain
- Eviction order: oldest GOP first
- When evicting, update the seekable time range reported to the viewer

### Memory Budget Reference

| Resolution | GOP Size (frames) | Format | Single GOP Decoded |
|---|---|---|---|
| 480p (640x480) | 30 | YUV420P | ~14 MB |
| 1080p (1920x1080) | 30 | YUV420P | ~93 MB |
| 4K (3840x2160) | 30 | YUV420P | ~372 MB |

These are decoded frame sizes. Compressed data in the buffer is typically
10-50x smaller. The LRU cache of decoded frames is what drives memory
consumption.

---

## 5. Implementation Insights

Lessons from the standalone pj_scene2D experiment and the mcap_player prototype
(both since removed; retained here as architectural rationale).

### Colorimetry was dropped (the BT.709 full-range hardcode)

For a long time `avFrameToDecodedFrame` discarded `AVFrame::colorspace` and
`AVFrame::color_range`, and the media shader applied a hardcoded **full-range
BT.709** matrix to every YUV frame. Two latent correctness bugs followed:

- **Wrong matrix for SD content.** BT.601 (SMPTE 170M / BT.470BG) luma coefficients
  differ from BT.709; applying 709 to 601 content shifts saturated hues.
- **Wrong range for limited-range video.** Most camera/file H.264/HEVC is *limited*
  range (luma 16..235); a full-range matrix renders it with raised blacks and
  clipped/greyed whites (washed out).

The metadata was always there in the `AVFrame` we already decode — it was just
thrown away. Fix: carry `YuvColorSpace` / `YuvColorRange` on `DecodedFrame` (mapped
from the `AVFrame`, with the standard SD-height→601 and unspecified-range→limited
fallbacks) and build the matrix per frame with `video_color.h::buildYuvMatrix`.

Key implementation choice: encode the whole thing as **one affine 4x4** applied to
`vec4(y, u-0.5, v-0.5, 1)`, with the limited-range luma scale (255/219), chroma
scale (255/224) and all offsets folded into the constant 4th column. That keeps the
GLSL untouched (no `.qsb` recompile was needed for that fix), and makes
BT.709+full reduce to the exact historical matrix so existing full-range content
is byte-identical. Unit-tested in `video_color_test` (black/white/gray
reconstruction + 601≠709). The later W11 WebGL2 re-bake changes only the packed
target dialects, not this shader math.

Corollary — **HW decode now emits NV12, not YUV420P.** Once `FfmpegDecoder` stopped
forcing `sws_scale` to YUV420P, VAAPI frames come back as `kNV12`. Any consumer of
decoder output must handle NV12: the GPU upload (native two-plane), the pixel
inspector (`pixelRgbAt` already had an NV12 case), and the thumbnail encoder (which
deinterleaves via `nv12ToYuv420p`). Tests that decode real video must accept either
`kYUV420P` (software) or `kNV12` (hardware).

### Timestamp Unit Conversion

FFmpeg internally uses stream `time_base` (e.g., 1/90000 for MPEG-TS,
1/1000 for MKV), not nanoseconds. pj_scene2D uses nanoseconds consistently
(matching pj_datastore). Conversion must happen at the codec interface using
`av_rescale_q(pts, stream_time_base, {1, 1'000'000'000})` for numerically
accurate results. Avoid manual multiplication — it loses precision for
non-power-of-two time bases.

### Seeking Requires Decoding Forward

After seeking to the nearest keyframe before the target timestamp, the
decoder must decode forward — discarding all intermediate frames — until it
reaches the target. For a GOP of 30 frames this means up to 29 wasted
decodes. For long GOPs (250+ frames) this can take hundreds of milliseconds.

This is why short GOPs matter for interactive scrubbing. Foxglove recommends
keyframes every ~1 second. LeRobot uses GOP=2 (keyframe every 2 frames),
achieving near-random-access with significant compression.

### Clock-Based Playback Scheduling

For smooth file playback, the display timer maintains a playback clock:

- On play: record `wall_start = steady_clock::now()` and `pts_start`
- Each tick: `expected_pts = pts_start + (now - wall_start)`
- Present the frame whose PTS is nearest-before `expected_pts`

Live mode skips the clock entirely — always show the latest frame, drop
older ones. This is the fundamental behavioral split between file and
streaming playback.

### StreamingVideoDecoder: Lessons Learned

**Keyframe index must track by timestamp, not count.** During steady-state
streaming with retention, `ObjectStore::entryCount()` stays constant
(push+evict). A count-based scan cursor (`if (count > last_scanned) scan`)
silently stops indexing new keyframes. Track by timestamp instead:
`indexAt(last_scanned_ts_) + 1` finds new entries regardless of count.

**Same-timestamp caching is critical.** When the display polls at 60 Hz
but frames arrive at 30 Hz, every other `decodeAt()` call requests the
same timestamp. Without caching, this triggers a full keyframe→target
seek+decode. For 1920p video with large GOPs, this causes visible
stuttering. Solution: cache `last_frame_` and return it immediately when
`target_ts == last_decoded_ts_`.

**Forward decode must survive keyframe eviction.** In live mode, the
original keyframe gets evicted while the decoder continues forward.
The decoder state is still valid — it just needs new packets. The forward
path must not require the keyframe index (`keyframe_timestamps_` may be
empty) or the original keyframe entry (may be evicted). Only backward
seeks require a keyframe still in the store.

**Parameter sets are primed before opening H.264/HEVC streams.**
The streaming decoder builds `AVCodecParameters` with
`makeVideoCodecParams(codec_id)` and primes H.264/HEVC extradata from the first
keyframe via `primeKeyframeParamSets()` before opening `FfmpegDecoder`. This
matches demuxer-provided codec config and avoids the observed first-B-frame drop
when H.264 parameter sets are consumed only in-band. `h264_utils.h` now keeps
only the H.264 keyframe oracle; codec-parameter construction and parameter-set
extraction live in `video_codec_utils`.

**Never drain() during live streaming startup.** FFmpeg's `avcodec_send_packet(nullptr)`
signals EOF and puts the codec in drain state. Subsequent `avcodec_send_packet` calls
return `AVERROR_EOF` until `avcodec_flush_buffers` is called. With B-frames, the
decoder returns EAGAIN for the first ~30 packets (reorder buffer filling). If you
drain after each failed decode, you poison the codec state, forcing a full
flush+re-decode-from-keyframe on every call — O(n^2) total. Fix: treat EAGAIN as
normal, update `last_decoded_ts_` to track position, and let the forward path feed
more packets on subsequent calls.

**DTS-keyed ObjectStore storage for B-frame videos.** When ingesting from MP4
containers with B-frames, use DTS (always monotonic) as the ObjectStore
timestamp, not PTS (non-monotonic). FFmpeg's decoder expects packets in decode
order and handles reordering internally — `AVFrame::pts` on the output gives
the correct presentation timestamp. For production streaming (per VideoFrame
spec), B-frames are disallowed, so PTS == DTS and this is a non-issue.

**ObjectStore indices shift after eviction.** A cached `last_decoded_index_`
becomes stale after eviction removes entries from the front. Always
re-derive the index via `indexAt(last_decoded_ts_)` instead of caching it.

### Zoom and Pan via GPU Transform

With QRhiWidget rendering, zoom and pan require only a view transform matrix
in the vertex shader (scale + translate). No pixel reprocessing.

Cursor-anchored zoom (keeps the point under the cursor fixed; `cursor_pos` in
y-up clip space, `fit` the aspect-preserving per-axis scale):
```
pan += cursor_pos / fit * (1/new_zoom - 1/old_zoom)
```
Drag divides the clip-space mouse delta by `fit * zoom` likewise. Omitting `fit`
makes a letterboxed image (always the case for a landscape frame at ±90°) lag
the cursor. `fitScale` / `panAfterZoom` / `panAfterDrag` in `pixel_inspector.h`
own this math, shared with the viewer transform and the inspector inverse.

When zoom <= 1.0, reset pan to zero (video fits entirely in widget).

Rotation (clockwise quarter-turns) is the last factor of the same matrix:
`scale(fit × zoom) · translate(pan) · rotate(−deg)`. The image quad spans
[−1,1]² whatever its aspect, so a quarter turn is an exact axis swap there;
only the aspect-preserving fit uses the swapped aspect (and the overlay
`effective_scale` the swapped texture size). Pan comes after the rotation, so
drag and cursor-anchored zoom stay in screen directions. Clip space is y-up,
hence the negative angle for a clockwise turn. `widgetPointToImagePixel`
mirrors the rotation by hand (the fit is shared via `fitScale`); any change to
one must change the other.

Text labels stay upright under rotation without a shader change: image pixels
reach the screen through a similarity (uniform scale + the clockwise turn), so
`overlay_geometry::uprightTextQuadCorners` counter-rotates each glyph quad about
its anchor in image space. The text quads are therefore rebuilt when the
rotation changes, not only when the annotation set does.

---

## 6. Streaming Protocols

### Latency Characteristics

| Protocol | Typical Latency | Notes |
|---|---|---|
| Raw TCP/UDP | <50ms | No error recovery |
| SRT | 120-500ms (tunable) | Best balance for robotics |
| WebRTC/WHEP | 200-500ms | Complex setup, feature-rich |
| RTSP | 1-3s | Surveillance cameras, widely supported |
| RTMP | 1-3s | Legacy ingest |
| HLS/DASH | 2-6s | Too high for interactive robotics |

### SRT as Primary Live Protocol

SRT (Secure Reliable Transport) offers the best balance for robot-to-desktop:

- Latency tunable via `SRTO_LATENCY` socket option
- ARQ retransmission for reliability over lossy networks
- Payload: raw H.264 Annex B NAL units
- Available on Conan (`srt/1.5.4`)

### ROS 2 Image Topics

`sensor_msgs/Image` and `sensor_msgs/CompressedImage` arrive as individual
messages, each independently displayable. No inter-frame dependencies.
Timestamps are in the message header (nanoseconds).

`foxglove.CompressedVideo` messages in MCAP carry one video frame each with
nanosecond timestamps, using Annex B encoding.

---

## 7. Storage-Mode / Handle Resolution by Backend

How the ObjectStore owning-handle storage model (REQUIREMENTS.md §4.2
"Storage Integration") maps to concrete backends. §4.2 abstracts the two
internal storage modes — owned bytes (streaming) vs lazy fetch callbacks
(file-backed) — behind a uniform owning handle; the per-backend
Stored/Resolve strategies below are the concrete realizations of those
modes.

### MCAP Handle

- Stored: file handle + message index entry (chunk offset + message offset)
- Resolve: seek to chunk, decompress if needed, read message bytes, parse
  according to schema (Image or VideoFrame)
- For VideoFrame: find nearest preceding keyframe in the index, decode
  forward
- MCAP Summary section provides O(1) access to the full index without
  scanning the data section

### LeRobot MP4 Handle

- Stored: MP4 file path + presentation timestamp (PTS)
- Resolve: open MP4, seek to nearest keyframe before PTS, decode forward to
  target PTS
- Timestamp mapping: Parquet `timestamp` column provides nanosecond
  timestamps; PTS provides the seek target within the MP4
- One MP4 file per camera per episode (v2) or per chunk (v3)

### RLDS TFRecord Handle

- Stored: TFRecord shard file path + record byte offset
- Resolve: seek to offset, read record, extract image tensor from
  FeaturesDict
- Images are stored as raw tensors `(H, W, 3) uint8` — no video codec
  decoding needed
- No inter-frame dependencies; every record is self-contained

### Live Buffer Handle

- Stored: buffer pointer + byte range within the ring buffer
- Resolve: read compressed bytes from the range, decode
- For video: the byte range covers a single encoded frame; the decoder must
  have been initialized from the preceding keyframe
- Handle is valid only while the buffer has not evicted the referenced data;
  the frame store must handle invalidation gracefully

---

## 8. Reference Documents

- [`plotjuggler_sdk/docs/builtin_type.md`](https://github.com/PlotJuggler/plotjuggler_sdk/blob/main/docs/builtin_type.md) — canonical builtin-type catalog: Image,
  VideoFrame, CameraCalibration, ImageAnnotations, PointCloud, SceneEntities,
  OccupancyGrid, FrameTransforms (the SDK owns all canonical schemas)
- [`docs/research/dataset_format_comparison.md`](../../docs/research/dataset_format_comparison.md)
  (top-level) — cross-cutting comparison of MCAP, RLDS, LeRobot, and Zarr
  formats covering data models, timestamps, image/video storage, I/O, and
  ecosystem
- [`docs/research/rerun_notes.md`](../../docs/research/rerun_notes.md)
  (top-level) — analysis of Rerun's 2D architecture, kept as background
  for design comparisons

---

## 9. Resolved Design Questions

These questions were identified early and resolved during architecture design.

### MessageParser Extension vs New Plugin Family

**Resolved:** Keep MessageParser stateless. Parsers are codec-agnostic
envelope peelers (CDR, Protobuf, JSON). All video decoder state lives in
pj_scene2D's decoder classes (`FfmpegDecoder`, `StreamingVideoDecoder`),
never in any plugin. See ARCHITECTURE.md §4 and REQUIREMENTS.md §4.4.

### Mixed Scalar + Media Output from a Single Parse Call

**Resolved (delivered as `pj_plugins` MessageParser protocol v4).** The
originally-proposed parser ABI v2 / two-host `parse()` signature was
superseded by a service-registry model. The `parse()` slot takes only
`(ctx, timestamp_ns, payload, out_error)` — no host parameters. Instead, a
parser acquires its write hosts at `bind(registry)` time via named
services: a scalar write host (`pj.parser_write.v1`) and, for media, an
object write host (`pj.parser_object_write.v1`). It then writes scalar and
media portions to the bound hosts inside a single `parse()` call. This is
no longer an outstanding prerequisite — `PJ_MESSAGE_PARSER_PROTOCOL_VERSION`
is 4. See REQUIREMENTS.md Prerequisites and ARCHITECTURE.md §2/§4.

### Metadata Availability: Eager vs Lazy

**Resolved:** Initialize the decoder eagerly on the first keyframe (one open per
channel, not per frame). `StreamingVideoDecoder` opens from the `codec_id`
resolved from `VideoFrame.format` (`makeVideoCodecParams`) and primes
`extradata` from that keyframe's in-band parameter sets via
`primeKeyframeParamSets` (deduplicated against the in-band copies — duplicated
sets would reproduce the first-B-frame drop the priming exists to fix). No lazy
metadata deferral.

---

## 10. MediaSource Pattern

The `MediaSource` interface is the uniform frame-delivery contract
between decoder backends and `MediaViewerWidget`. It replaces the
originally-planned `PlaybackController` (which would have been a
monolithic orchestrator conflicting with the streaming decode path's
self-contained threading).

**Interface:**
```cpp
class MediaSource {
 public:
  virtual ~MediaSource() = default;
  virtual void setTimestamp(int64_t ts_ns) = 0;
  virtual std::optional<MediaFrame> takeFrame() = 0;
};
```

**Key properties:**
- `setTimestamp()` is called by the main thread when the global time
  changes. Image, streaming-video, and depth sources post to an internal
  worker thread; the scene source decodes synchronously on the caller thread.
- `takeFrame()` is called by the main thread at render rate. Returns
  the latest `MediaFrame` (base pixels and/or overlays), or nullopt if
  nothing new since the last call.
- No `cancel()` in the public interface — each implementation manages
  cancellation internally.
- The widget calls `update()` after `setTimestamp()` to trigger a repaint.

**Concrete implementations** (all in `pj_scene2d_core`):
- `ImagePipelineSource` — worker-backed image decode via `CodecPipeline`
  + `ObjectStore`; latest decoded result is polled by `takeFrame()`.
- `DepthPipelineSource` — worker-backed depth decode (R32F + GPU colormap
  params; the compressedDepth PNG inflate runs off the UI thread); latest
  result polled by `takeFrame()`.
- `ScenePipelineSource` — synchronous scene/annotation decode in
  `setTimestamp()`.
- `StreamingVideoSource` — worker-backed `StreamingVideoDecoder`; latest-wins
  request/result handoff.
- `CompositeMediaSource` — fans `setTimestamp`/`takeFrame` out across N
  child sources (multi-layer).
- `BorrowedMediaSource` — non-owning adapter over an externally-owned
  source.

---

## 11. Lessons Learned (Code Review Fixes)

Bugs found during a 4-agent parallel code review (API design, silent
failures, type design, Codex) and fixed via TDD.

**YUV420P chroma plane sizing must use ceiling division.** For
YUV420P, chroma planes are `ceil(w/2) x ceil(h/2)`, not `w/2 x h/2`.
Truncating integer division causes buffer overflow on odd-dimension
video (e.g. 1921x1081). Fixed by adding `expectedBufferSize()` to
`decoded_frame.h` — a single source of truth for all YUV/NV12 buffer
size calculations. Every allocation site must use this function.

**`avFrameToDecodedFrame` must propagate errors, not return null
frames as success.** When HW transfer or sws_getContext fails,
returning `DecodedFrame{}` wraps a null frame in `Expected<T>` as a
"success" — callers have no way to distinguish it from a real frame.
Changed return type to `Expected<DecodedFrame>` with error strings.

**MCAP's `schemas()` and `channels()` return by value.** Calling
`reader.schemas().find(id)` creates an iterator into a temporary map
that is destroyed at the semicolon. Classic dangling iterator UB —
heap-use-after-free under ASAN. Fix: cache the map in a local variable
before iterating.

**Codec stages must validate input format and buffer size.** Pipeline
stages like `SegmentationPalette` and `Mono16ToGrayscale` compute read
lengths from `width*height` but never check `pixels->size()`. With
malformed input, this is an out-of-bounds read.

**RTLD_DEEPBIND is incompatible with AddressSanitizer.** `dlopen` with
`RTLD_DEEPBIND` conflicts with ASAN's runtime interceptors. Fixed by
defining `PJ_ASAN_ACTIVE` when sanitizers are enabled and skipping
the flag in that case.

**Never join a decode thread that has not started (WebAssembly).** The
browser build pre-spawns a fixed pthread pool (`QT_WASM_PTHREAD_POOL_SIZE`,
7). A thread created with the pool exhausted starts only once the main
thread yields, so a main-thread `join()` on it waits forever. A failed layout
replay builds and discards each camera dock twice in one pass, which is
enough to exhaust the pool with three camera docks open.
`AsyncFrameWorker::stop()` therefore joins only a thread that has entered its
loop and detaches one that has not; the thread checks a shared start gate
before touching the worker, so a detached thread returns at once.

## 12. Camera Rectification (2D annotation alignment)

`ImagePipelineSource::rectifyIfCalibrated` (using `undistort_remap` +
`image_rectifier`) exists to fix one specific symptom: 2D `ImageAnnotations`
(YOLO boxes, masks, keypoints) rendering **misaligned** over the camera image
in datasets like Waymo.

### The problem it solves

The misalignment is a **coordinate-space mismatch**, not a projection issue.
2D detections are pixel coordinates — they need no geometric transform to draw.
But in Waymo/Foxglove the annotations are authored in the camera's **native,
rectified (lens-undistorted)** space (e.g. 1920×886), while PJ4 was displaying
the **raw, distorted, subsampled** image (e.g. 480×221). Two mismatches stack:
resolution (coords out of scale) and lens distortion (boxes drift, worst at the
edges). A scale-only fix handles the first but not the second; full rectification
(`K, D, R, P`) handles both, lifting the image into the annotations' space so the
boxes land without touching a single detection coordinate.

`frame_id` on `sdk::Image` is what pairs an image with its `CameraInfo` (each
CameraInfo-classified topic, or legacy `<ns>/camera_info` topic, in the image's
dataset is decoded and injected via `setCameraInfoMap`). The
producer side (parser_protobuf / parser_ros) populates it.

### Design choice: rectify the image, not warp the annotations

Two ways to reconcile the spaces: (A) rectify the image to the annotation space,
or (B) keep the raw image and distort every annotation vertex into raw space. We
chose **A** — it matches Foxglove Studio, shows the undistorted image the detector
actually "saw", and avoids applying the inverse map to every polygon/mask point.

### GPU vs CPU rectification (where it runs)

*Whether* to rectify is unchanged (the assumption below); *where* it runs is now
chosen at runtime, because the float CPU resample was ~63–68% of process CPU
(profiled) across the per-camera decode worker threads.

- **GPU path (default when available).** The decode worker leaves the frame RAW
  and attaches the cached `shared_ptr<const UndistortMap>` to
  `DecodedFrame::rectify_map`. `MediaViewerWidget` rectifies in the image
  fragment shader via a remap LUT (`undistortMapToNormalizedRG`, +0.5
  half-texel). Qt's QRhi has no `RG32F` (found on 6.11.1; re-check on the pinned Qt), so the two float UVs go up as their
  bits in an `RG32UI` texture (8 B/px) and the
  shader reads them texel-exact (`texelFetch`, then `uintBitsToFloat`) — one extra texture lookup per pixel, and the upload
  shrinks to the raw frame size. Works for YUV420P too (the shader remaps the UV,
  then samples the planes).
- **CPU fallback** (`rectifyFrameFast` + `UndistortMapFast`): a precomputed
  source index + fixed-point bilinear fractions resampled into a reused buffer —
  **bit-for-bit equal** to the float `rectifyFrame` (a unit test asserts zero
  mismatches) and ~2× faster (34.1 → 16.6 ms/frame at 1920×1280 RGB). Used
  until the widget has built its image pipeline (none: no GPU path at all).
- **Selection.** A one-time capability handshake
  (`MediaSource::setGpuRectificationAvailable`, probed by the widget in
  `initialize()`, fanned out through `Composite`/`BorrowedMediaSource`) flips the
  mode; the default is the CPU-correct path until the widget confirms GPU support,
  and a flip calls `invalidate()` to re-decode. Both paths produce the identical
  displayed image at native calibration resolution, so the annotation /
  aspect / pixel-inspector contract holds regardless of path (see
  ARCHITECTURE §7.2 for the logical-size decouple).

### ASSUMPTION and its known blind spot

The decision rule is *"a usable `CameraInfo` exists for this image's `frame_id` ⟹
rectify to native resolution."* That encodes an **assumption**: that the image is
still in raw sensor space and the annotations live in native rectified space. True
for Waymo / Foxglove / the ROS `image_proc` convention (detectors run on
`image_rect`), but **not universal**, because:

- **`CameraInfo` describes the lens, not the current image state.** The *same*
  `CameraInfo` (same `K`/`D`/`frame_id`) accompanies both `/image_raw` and
  `/image_rect`. Its presence does not tell us whether the pixels we're showing
  are still distorted. So `isRectifiable()` is a *capability* check ("has usable
  intrinsics"), **not** a "this image needs rectifying" decision.
- **Failure mode 1 — double rectification:** a producer that logs a pre-rectified
  image alongside its `CameraInfo` (`D ≠ 0`) gets undistorted a second time → the
  image warps the wrong way, annotations drift.
- **Failure mode 2 — raw-space detections:** if detections were computed on the
  raw image (and thus already align with it), rectifying the image alone moves the
  pixels but not the boxes → misalignment of a pair that was fine.

The safe default still holds: **no `CameraInfo` / empty intrinsics / no `frame_id`
→ raw passthrough, annotations overlay directly.** Other gaps: on the CPU path,
planar / 16-bit pixel formats pass through unrectified (`rectifyFrameFast` /
`rectifyFrame` return false / `nullopt`) — the GPU path does rectify YUV420P; a
`CameraInfo` published *after* the layer attaches is not retro-applied.

The first escape hatch for the blind spot now ships: a per-layer **"Rectify"**
toggle on `ImageLayer` (a `PJ::ToggleSwitch`, default on). It gates the
auto-decision via `ImagePipelineSource::setRectifyEnabled(false)`, which short-
circuits `rectifyIfCalibrated` to raw passthrough — the operator's manual override
for a stream that's already rectified (failure mode 1) or whose detections live in
raw space (failure mode 2). The flag is persisted in the layout XML
(`rectify_enabled`); an absent attribute (older layout) keeps the default-on
behaviour. It is an explicit user choice, not a smarter automatic rule.

### If we ever need an automatic decision

The toggle puts the choice in the operator's hands; a future version could *also*
decide automatically when no human is in the loop. Key the decision on something
more explicit than "calibration exists": compare the annotation's reference
`frameSize` against the displayed image, gate on the topic name (`*_raw*` vs
`*_rect*`), or skip when `D` is effectively zero. Out of scope today; documented
here so the assumption stays a deliberate, visible choice.

## 13. Depth-aware Point Inspector

The hover Point Inspector (`pixel_inspector.{h,cpp}`, driven by
`MediaViewerWidget::refreshPointInspector`) is **format-aware**. For ordinary
images it shows the zoom grid + RGB readout. For a depth frame
(`PixelFormat::kDepthR32F`) the "RGB" is meaningless: depth pixels store **metric
depth in metres**, and the colour you see is produced on the GPU by the colormap
shader (the LUT), not stored per-pixel. So the inspector specializes:

- **Sample the right quantity.** `depthMetersAt(frame, x, y)` reads the float32
  depth. No-data is `!isfinite(v) || v <= 0` — the *same* rule
  `DepthPipelineSource` uses and the shader's `!(d > 0)` test applies, so the
  readout's "— (no data)" agrees pixel-for-pixel with the transparent pixels on
  screen. (Invalid depths are already collapsed to `0.0f` in the frame buffer.)
- **Reproduce the on-screen colour for the swatch.** `depthColormapColor(depth,
  params)` mirrors the shader exactly: `t = clamp((d - near) / max(far - near,
  1e-6), 0, 1)`, optional invert, then `pj_widgets::colorFor(colormap, t)` — the
  same function the GPU LUT is *built* from, so the swatch matches the display up
  to the LUT's 256-entry quantization. A regression test (`pixel_inspector_test`,
  `PixelInspectorDepth.*`) pins the swatch to `colorFor` so it can't silently
  drift if the colormap math changes.
- **Drop the zoom grid.** Magnifying a colormapped pixel adds nothing; depth mode
  shows only `Position`, the metric `Depth`, and the swatch. The tooltip sizes its
  width with `QFontMetrics` so the longer "— (no data)" line isn't clipped.

Unlike the RGB path (which hides on an off-image / empty crop), depth mode still
shows the tooltip on a no-data pixel — position + "no data" is useful information.
Mono8/Mono16 keep their grayscale RGB readout: there the channel value *is* the
meaningful quantity.
