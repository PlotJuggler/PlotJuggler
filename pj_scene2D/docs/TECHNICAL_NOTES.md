# pj_scene2D technical notes

Evidence behind [ARCHITECTURE.md](./ARCHITECTURE.md): what was observed or tried, under which conditions, what the
code does about it, and when to look again.

## Frame delivery through Qt signals

**Observed.** In the video-player prototype that preceded this module, decoded frames were delivered with queued
signals. A queued event cannot be withdrawn, so during a fast scrub stale frames piled up in the event queue and
reached the display after newer ones. Event compression, sequence-number guards and blocking connections were
tried and did not fix it: the queue itself is the staleness buffer.
**What the code does.** Frames cross threads only through `AsyncFrameWorker`'s single-slot mailbox and
`takeFrame()`; the frame-ready callback carries no payload, it only asks the GUI to poll.
**Re-test** only if frame delivery is redesigned; the reasoning holds for any queue that cannot drop entries.

## Scrub preemption

**Observed.** With every new request cancelling the in-flight video decode, a 60 Hz tracker cancelled each decode
before it delivered: the 4K playback benchmark (`video_playback_bench_test.cpp`, run with `PJ_BENCH_MP4`) displayed 0
fps. With the scrub-only predicate (backward, or forward beyond `kScrubPreviewThresholdNs`) it displayed about 26
fps on the same file.
**Why.** A cancel discards the forward-continuation state, so the next request re-seeks from the keyframe.
**Re-test** when the decoder can resume after a cancel, or when the threshold changes.

## RGB frames at upload

**Measured (2026-11-22, i7-13700H, Iris Xe, Qt 6.12.0, `perf` during looping playback).** A JPEG decoded to RGB888
was expanded to RGBA by a per-pixel loop in `MediaViewerWidget::uploadDecodedFrameToTexture`, on the GUI thread, at
every upload: with three JPEG cameras that render-and-upload path was 0.9 % of a core of GUI-thread CPU.
**What the code does.** The image worker decodes JPEG and PNG to RGBA (`ColorOutput::kRgba`), at no extra decode
cost for JPEG; the same path then costs 0.3 % of a core.
**Re-test.** If raw `rgb8` / `bgr8` cameras show up in a profile: they still take the loop.

## Repaint cadence

**Observed.** `MediaViewerWidget` renders non-natively inside the dock hierarchy, so every repaint pays a
framebuffer readback plus a raster composite into the window. With repaints on every 60 Hz tick over a 25-30 fps
camera, that `rgb888 -> rgb32` composite was about 17 % of the GUI thread during large-point-cloud plus camera
playback.
**What the code does.** The render-key gate in `SceneDockWidget::onTrackerTime` skips ticks that show the same
entries (ARCHITECTURE "Clock and repaint").
**Re-test** if the viewer becomes a native window, or if Qt's QRhiWidget composition gets cheaper.

## Colorimetry

**Observed.** With colour tags ignored and a full-range BT.709 matrix for every frame, SD content (BT.601) showed
shifted saturated hues and limited-range video (most camera H.264/HEVC) showed raised blacks and greyed whites.
**What the code does.** `FfmpegDecoder` maps `AVFrame::colorspace` and `color_range` onto each frame: unspecified
space follows the players' heuristic (height 576 or less: BT.601, else BT.709), unspecified range is limited, and
the deprecated YUVJ formats count as full range. The swscale repack keeps the source range (`keepRange`): by
default swscale squeezes a full-range YUVJ frame to limited range while the frame stays tagged full.
`buildYuvMatrix` folds range scale and offsets into one affine 4x4 applied to `(y, u-0.5, v-0.5, 1)`, so the
shader is identical for all four combinations; `video_color_test` checks black, white and grey reconstruction.
**Hardware frames** are downloaded by `av_hwframe_transfer_data`, which copies pixels and not frame properties, so
the tags are read from the decoded frame, not the download (`DecodedFrameKeepsTheSignalledColourTags`).
**Re-test** when HDR (BT.2020 / PQ / HLG) is wanted: it gets the heuristic matrix.

## Chroma siting at odd sizes

4:2:0 chroma sample (x/2, y/2) belongs to luma pixel (x, y). At an odd size (AV1 allows one; H.264 and HEVC crop
4:2:0 to even) the chroma plane has ceil(n/2) samples, and sampling it at the luma UV stretches it over the frame.
`yuv_to_rgb.frag` scales the chroma UV per axis by n / (2 * chroma_n), from the `textureSize` of both planes;
even sizes select the plain UV. Chroma follows the frame's `mag_filter`: nearest for images, bilinear for video.
`MediaViewerOddSizeTest` (`media_viewer_render_test`) checks a 17x17 frame in YUV420P and NV12. Odd sizes also need
ceil-division buffer sizing: `expectedBufferSize()` is the single size rule (`decoded_frame_test` pins 1921x1081).

## CPU RGB conversion for scripts

`convertToRgb24` (on-demand scripts) uses the shader's matrix and siting with nearest chroma, through swscale.
**Measured** on 1080p frames (`VideoRgbConversion` tests in `streaming_video_decoder_test.cpp`):

| Path | Condition | Time per frame | Max error vs shader |
|---|---|---|---|
| `SWS_POINT`, unscaled converter | YUV420P, width a multiple of 16, even height | 0.65 ms | 3 levels (2 at full range) |
| `SWS_POINT`, generic scaler | NV12, same sizes | 2.6 ms | same bound, measured on those frames |
| exact rounding | any other size | about 11 ms | 1 level |

About 0.5 % of limited-range samples are off by 3. The size gate exists because swscale's unscaled x86 kernel
writes 16 pixels at a time and, at other widths, left a row's last pixels unwritten or wrote past the frame;
swscale uses it only at an even height. **Re-test** on an FFmpeg upgrade (the gate may become unnecessary).

## B-frames

**Observed.** With B-frames, decode order (DTS) differs from presentation order (PTS). Serving by DTS made playback
jitter and turned forward continuation into per-frame GOP re-decodes (high CPU). Opening the H.264 decoder without
`extradata`, relying on in-band parameter sets only, dropped the stream's first B-frames.
**What the code does.** The store key is the DTS (monotonic, needed by the append index) and the payload's
`timestamp_ns` the PTS, for every `VideoFrame` producer; the decoder feeds in DTS order with the real PTS on each
packet and serves by PTS. `primeKeyframeParamSets` fills `extradata` from the first keyframe, deduplicated
against the in-band copies (duplicated sets reproduced the drop), and `computeReorderDelay` sets the reorder delay
a demuxer would report. Without B-frames PTS equals DTS and all of this is the identity.
`streaming_video_decoder_test` covers both (`test_h264_bframes.mp4`). I+P streams remain the cheapest to seek.

## Streaming decoder pitfalls

- **Never drain during live startup.** `avcodec_send_packet(nullptr)` puts the codec in EOF state until a flush; a
  drain after every EAGAIN while the reorder buffer fills forced a flush and GOP re-decode per call (quadratic).
  EAGAIN is normal: feed more packets.
- **Receive before send.** With the output queue full, a send hits EAGAIN; a recovery that discards the queued
  frame loses the frame the next request needs (observed as one "forward decode produced no frame" hiccup early in
  playback). The pump empties the queue before each send (`streaming_video_decoder_test`, the DTS sweep).
- **Count-based scan cursors stall.** Under retention the entry count stays constant (push plus evict), so "scan
  when the count grows" stops indexing. The index follows UIDs and the store's content identity instead.
- **Indexes shift after eviction.** A cached entry index is stale once the front is evicted; packets are addressed
  by UID.
- **Negative DTS.** FFmpeg gives the first frames a negative DTS (encoder delay), so a keyframe can sit at a
  negative timestamp; no lookup may use a negative value as "not found" (`KeyframeAtNegativeTimestampIsFound`).
- **Same-timestamp polls.** A 60 Hz display over a 30 Hz stream asks for the same time twice; the cached frame is
  returned without a seek (large-GOP streams stuttered without it).

## VAAPI device selection

**Observed.** `av_hwdevice_ctx_create(..., nullptr, ...)` opens the default render node (`/dev/dri/renderD128`),
often a GPU without a VAAPI driver (an NVIDIA dGPU on the proprietary driver) while the VAAPI GPU is `renderD129`.
**What the code does.** On Linux, `PJ_VAAPI_DEVICE=/dev/dri/renderD<N>` forces a node; otherwise the decoder walks
`renderD128..191` and takes the first whose VAAPI driver initializes. Other backends use the library default
device. A backend that opens but cannot decode the profile falls back to software per frame, so
`vaQueryConfigProfiles` is not pre-queried.
**Windows.** `av_hwdevice_iterate_types()` lists DXVA2 before D3D11VA and the Windows package builds both, so
D3D11VA is tried first explicitly. Fallbacks: Linux VAAPI, CUDA, software; Windows D3D11VA, software; macOS
VideoToolbox, software; browser software only.

## Hardware frame download

Every hardware frame is downloaded to system memory (`av_hwframe_transfer_data`) as NV12, about 0.5-2 ms per 1080p
frame (estimate, conditions not recorded), and uploaded like a software frame. A zero-copy import (DMA-BUF or a
shared texture into `QRhiTexture::createFrom`) would skip the round trip at the cost of interop and synchronization
code per backend. **Re-test** if 4K multi-camera playback becomes download-bound.

## Rectification assumption

**The symptom it solves.** In Waymo / Foxglove-style datasets, 2D detections are authored in the camera's native,
rectified space (e.g. 1920x886) while the logged image is raw, distorted and subsampled (e.g. 480x221). Scaling
fixes the resolution but not the lens drift at the edges; full rectification (`K`, `D`, `R`, `P`) lifts the image
into the annotations' space without touching a detection.
**Choice.** Rectify the image, not distort every annotation vertex: it matches Foxglove Studio and shows the image
the detector saw.
**Assumption.** "A usable `CameraInfo` for this `frame_id` means rectify." `CameraInfo` describes the lens, not the
image: the same message accompanies `/image_raw` and `/image_rect`. Two failure modes follow:

- a pre-rectified image logged with its `CameraInfo` (`D` not zero) is undistorted twice and warps the wrong way;
- detections computed on the raw image (already aligned) drift once only the image moves.

No `CameraInfo`, empty intrinsics or no `frame_id` means raw passthrough. The per-layer Rectify toggle
(`setRectifyEnabled`) is the operator's override for both failure modes. **Untested idea** for an automatic rule:
compare the annotations' reference frame size with the displayed image, or the topic name (`_raw` / `_rect`), or
skip when `D` is effectively zero.

## Rectification cost

**Measured.** The float CPU resample was 63-68 % of process CPU (profiled) across the per-camera decode workers.
`rectifyFrameFast` (precomputed source index plus fixed-point bilinear weights into a reused buffer) is bit-for-bit
equal to `rectifyFrame` (a unit test asserts zero mismatches) and took 16.6 ms instead of 34.1 ms per 1920x1280 RGB
frame. The GPU path costs one extra texture lookup per pixel and uploads the smaller raw frame.
**Re-test** the CPU numbers if the GPU path ever has to be disabled by default.

## WebGL2 and RG32UI

QRhi has no `RG32F` texture format (found on Qt 6.11.1; re-check on the pinned Qt), so the rectification LUT stores
the bits of two floats in `RG32UI` (8 B/px), fetched with `texelFetch` and decoded with `uintBitsToFloat`. Qt's
GLES backend reports the 32-bit integer formats only from ES 3.1, although `RG32UI` is core in ES 3.0 (WebGL2), so
GPU rectification is enabled when the base pipeline (which binds an `RG32UI` placeholder) builds, not by
`isTextureFormatSupported`. Re-test on a Qt upgrade.

## QRhiWidget in a raster window

**Observed** (Qt 6.11.1, confirmed on 6.12.0, desktop and browser). A top-level first shown without a `QRhiWidget`
gets a raster backing store. Qt switches it to QRhi composition, by destroying and recreating its native window,
only when a `QRhiWidget` is reparented into it (`QWidgetPrivate::setParent`); one constructed with its parent is
not a `QRhiWidget` yet when `QWidget`'s constructor sets that parent, so the window stays raster ("QRhiWidget: No
QRhi").
**What the code does.** Every `QRhiWidget` is constructed parentless and then inserted (`MediaViewerWidget`,
`PlotRhiCanvas`, `SceneViewWidget`). On desktop `MainWindow` owns a hidden `QRhiWidget` from construction, so the
window composites through QRhi from its first show and the recreation (a visible flash on Wayland) never happens.
`rhi_mixed_dock_test` covers GPU docks added after a raster-only first show and a Scene2D dock built in place.
Floating, redocking or switching an ADS tab moves the viewer to another QRhi: Qt calls `releaseResources()` then
`initialize()`, and the viewer re-uploads its retained frame and overlays (`rhi_dock_lifecycle_test`). Qt does not
call `releaseResources()` on destruction, so the destructor does.

## Browser viewer lifecycle

**Observed.** A zero-size WebGL surface created before a browser file picker can lose its context during the picker
round trip, after which Emscripten's context-attribute path may dereference a null result. Constructing the
browser viewer with its parent already attached produced repeated "QRhiWidget: No QRhi" and a blank dock.
**What the code does.** No GPU widget exists before the first real viewer. The viewer is constructed parentless, so
`setApi(PJ::preferredGraphicsApi())` runs before it joins a visible hierarchy (the API is fixed after that). Its
first submitted frame switches the top-level to RHI composition; after the first `frameSubmitted` it queues one
full top-level repaint so pre-existing raster regions reach the mixed compositor, and `releaseResources()` re-arms
that repaint. A QRhi readback proves GPU output; only a full-window snapshot catches black raster regions left
after the switch.

## Shader packs

The viewer loads offline-compiled `.qsb` packs embedded by `shaders/shaders.qrc`; editing a `.vert`/`.frag` does
nothing until its pack is re-baked with `widgets/shaders/bake.sh <qsb>` using the pinned Qt's `qsb` (pack bytes
depend on its version; desktop kits ship none, a full Qt or the WASM kit does). `bake.sh` is the single source of
the targets, following `cmake/PjQsb.cmake`: `yuv_to_rgb` bakes WebGL2, GLSL 330 and 440; `scene_lines` and
`scene_text` bake GLSL 100 es, 120, 150 and 300 es; all carry HLSL 50, MSL 12 and SPIR-V. A WASM configure re-bakes
into the build tree and requires identical bytes, so a source/pack mismatch, a missing pack or a wrong target set
fails configuration. `media_viewer_render_test` draws every pack on each backend of the platform. Packs are read
once per process.

## Static JPEG ABI in the WASM module

**Observed.** Qt's static JPEG plugin and Scene2D's static TurboJPEG share one WASM symbol namespace and both
export `jpeg_*`, so a clean link can bind a caller from one to the other. Qt's kit declares `JPEG_LIB_VERSION 80`
(found on Qt 6.11.1); libjpeg-turbo's default build emulates 62. The mismatch crashed during splash pixmap
construction, before `MainWindow`.
**What the code does.** The WASM dependency provider reads the kit's `QtJpeg/jconfig.h`, fails unless it is the v8
ABI, and builds TurboJPEG with `WITH_JPEG8=ON`. Matching library versions is not enough: the build option selects
the ABI. Re-check on every Qt upgrade.

## Video decode in the WASM module

The browser has no hardware-decode API FFmpeg can drive, so its `FfmpegDecoder` is software-only.
`cmake/PjWasmFfmpeg.cmake` (`PJ_WASM_WITH_VIDEO`, on with Scene2D) cross-compiles the desktop's FFmpeg release and
dav1d into static archives with the same LGPL decode-only allowlist (h264, hevc, mjpeg, av1, libdav1d), with
`-pthread` so libavcodec's frame threads run on Emscripten workers, and without libavformat, assembly or any
hwaccel. They are imported under the desktop target names, so the core builds unchanged. Configuring needs
`meson`, GNU `make` and `pkg-config` on the host.

## Decode threads in the WASM pthread pool

**Observed.** The browser build pre-spawns a fixed pthread pool (`QT_WASM_PTHREAD_POOL_SIZE`). A thread created
with the pool exhausted starts only once the main thread yields, so a main-thread `join()` on it never returns. A
failed layout replay builds and discards each camera dock twice in one pass, which exhausted the pool with three
camera docks.
**Observed (2026-11-23, glibc 2.35, Ubuntu 22.04 CI runner).** Detaching after releasing the gate segfaulted in
`pthread_detach`: glibc 2.42 and older read the thread descriptor after marking it detached, the abandoned thread
can exit in that window and free its own descriptor, and the read faults once the stack cache unmaps the stack. A
replay of `NoDecodeStartsAfterStopReturns` in an Ubuntu 22.04 container crashed 12 of 720 processes (20000 stops
each, 60 at once); glibc 2.43 rewrote `pthread_detach` and does not show it.
**What the code does.** `AsyncFrameWorker::stop()` joins only a thread that has entered its loop and detaches one
that has not, holding the start gate so the thread cannot exit during the detach; that thread checks the gate
before touching the worker and returns at once. With the detach under the gate the same replay ran 600 processes
without a crash.
**Re-test** not needed: the locked detach is correct on any libc and costs nothing.

## Thumbnail cache

`EntryThumbnailCache` decodes the topic once on a builder thread with its own decoder and extractor, keeping one
JPEG capped at `kThumbnailMaxWidth` per interval of max(1 s, span / `max_tiles`) within a byte budget
(`EntryThumbnailCache::Config`). `decodeSampled` pays the hardware download and downscale only for kept frames,
which is what keeps a 4K pass tractable. Thumbnails are YUV420P (an NV12 frame is deinterleaved by
`nv12ToYuv420p`), so the media shader draws them like live frames. The cache is skipped for video no wider than
the cap, where a full decode is already cheap.

## Image decode cost

Still images (JPEG/PNG) decode in under about 10 ms per frame (reported when the image path was designed;
resolution and machine not recorded), so a decoded-frame cache would cost more memory than it saves. **Re-test**
with large images (above 4K) if scrubbing stills stutters.

## Qt Multimedia

`QMediaPlayer` seeks only to keyframes, has no frame stepping, no external clock and seconds of RTSP latency, so it
cannot follow PlotJuggler's timeline. PJ4 does not use Qt Multimedia: FFmpeg frames go straight into the viewer's
QRhi textures.
