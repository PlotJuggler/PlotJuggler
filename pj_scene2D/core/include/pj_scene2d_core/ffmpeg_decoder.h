#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_scene2d_core/cancel_token.h"
#include "pj_scene2d_core/decoded_frame.h"

struct AVCodecContext;
struct AVCodecParameters;
struct AVBufferRef;
struct AVFrame;
struct SwsContext;

namespace PJ {

/// FFmpeg decoder wrapper: takes compressed video packets, produces DecodedFrame.
///
/// Hardware-accelerated where possible, with software fallback. Rather than a
/// fixed backend list, open() iterates the HW backends this FFmpeg build
/// exposes (av_hwdevice_iterate_types) and picks the first that can decode the
/// codec; for VAAPI it walks the DRM render nodes (override: PJ_VAAPI_DEVICE) so
/// the right GPU is chosen on multi-GPU machines. See ffmpeg_decoder.cpp.
/// Single-threaded — codec state is inherently sequential. One instance per
/// video layer per viewer widget. Ported from video_player_lab/HWDecoder.
/// Converts a 4:2:0 YUV frame (kYUV420P or kNV12) into tightly packed rgb24 at `out` (width * height * 3 bytes), with
/// the matrix the media shader applies for its colour space and range (`buildYuvMatrix`), through swscale: within 3
/// levels per channel of it (swscale's fast converter at a width that is a multiple of 16 and an even height, its exact
/// rounding, within 1, at any other). False for any other format or a frame whose buffer does not match its size. Any
/// thread.
[[nodiscard]] bool convertToRgb24(const DecodedFrame& frame, uint8_t* out);

class FfmpegDecoder {
 public:
  FfmpegDecoder();
  ~FfmpegDecoder();

  FfmpegDecoder(const FfmpegDecoder&) = delete;
  FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
  FfmpegDecoder(FfmpegDecoder&&) = delete;
  FfmpegDecoder& operator=(FfmpegDecoder&&) = delete;

  /// Open decoder from codec parameters (obtained from AVStream).
  /// Probes the available HW backends/devices, falling back to software.
  bool open(const AVCodecParameters* params);

  /// Decode a compressed packet into a YUV420P DecodedFrame.
  /// Returns empty Expected on EAGAIN (need more packets) or error.
  /// Checks cancel token between send and receive.
  ///
  /// `pts`/`dts` are the packet's presentation/decode timestamps. They differ for
  /// B-frame streams; feeding the real PTS (not the DTS store key) is what makes
  /// `DecodedFrame::pts` come back in presentation order so the caller can serve
  /// frames correctly. Packets must still be fed in decode (DTS) order.
  Expected<DecodedFrame> decode(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const CancelTokenPtr& cancel = nullptr);

  /// Send packet and receive frame, but skip HW transfer and sws_scale.
  /// Returns only the PTS of the decoded frame (or -1 on EAGAIN/error).
  /// Use this for intermediate frames during seek-forward where we need
  /// to advance the decoder but don't need pixels. See decode() re: pts/dts.
  int64_t decodeSkip(const uint8_t* data, size_t size, int64_t pts, int64_t dts);

  /// Decode one packet, but pay the expensive HW-download + YUV conversion ONLY
  /// when `want` accepts the decoded frame's presentation PTS. Returns:
  ///  - a materialized DecodedFrame when `want(pts)` was true,
  ///  - a null DecodedFrame (isNull()) when the frame was decoded but `want`
  ///    rejected it (decoder state advanced, no pixels produced),
  ///  - unexpected on EAGAIN ("need more packets") or a hard error.
  ///
  /// This is the cheap primitive for a sampling forward pass (thumbnails): every
  /// packet still advances the codec, but the per-frame download + sws_scale runs
  /// only for the ~1/N frames kept. `want` sees the TRUE output PTS, so sampling
  /// stays correct under B-frame reorder (the frame popped out is not necessarily
  /// the packet just sent). A null/empty `want` materializes every frame.
  Expected<DecodedFrame> decodeFiltered(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want);

  /// Receive ONE already-decoded frame without sending anything. Returns:
  ///  - a materialized frame when `want(pts)` accepts its presentation stamp,
  ///  - a null DecodedFrame when a frame was decoded but `want` rejected it
  ///    (state advanced, no HW-download paid),
  ///  - unexpected("need more packets") when the output queue is empty (EAGAIN),
  ///    or unexpected(...) on a hard error.
  /// Pump this until EAGAIN before each sendOnly(): libavcodec guarantees a send
  /// cannot return EAGAIN right after a receive did, which closes the window
  /// where the combined decode path's EAGAIN recovery silently DISCARDS a queued
  /// frame. On a B-frame stream under load (big frames keeping the worker threads
  /// busy), that discarded frame is the next request's display frame — a one-frame
  /// "produced no frame" hiccup early in playback.
  Expected<DecodedFrame> receiveFiltered(const std::function<bool(int64_t)>& want);

  /// Send one packet with the streaming-pump policy: WITHOUT the blind
  /// receive-and-discard EAGAIN recovery of the legacy combined calls (ENOMEM is
  /// still retried after a flush). Use after pumping receiveFiltered() to EAGAIN.
  /// Returns false if the codec rejects the packet.
  bool sendOnly(const uint8_t* data, size_t size, int64_t pts, int64_t dts);

  /// Flush decoder state — mandatory after seek.
  void flush();

  /// Drain remaining buffered frames (call at EOF).
  std::vector<DecodedFrame> drain();

  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;

 private:
  enum class SendPolicy {
    kCombinedDecode,  // ENOMEM flush-retry + legacy EAGAIN drain/discard/retry.
    kStreamingPump,   // ENOMEM flush-retry only; caller pre-drains output queue.
    kDecodeSkip       // Preserve decodeSkip's historical no-retry send behavior.
  };

  Expected<DecodedFrame> decodePacket(
      const uint8_t* data, size_t size, int64_t pts, int64_t dts, const std::function<bool(int64_t)>& want,
      const CancelTokenPtr& cancel);
  Expected<DecodedFrame> receiveFrame(const std::function<bool(int64_t)>& want);
  // Send one packet to the codec, copying the bytes so the caller's buffer need
  // not outlive the call. `policy` captures the public path's existing recovery
  // semantics; returns avcodec_send_packet's result (>= 0 on success).
  int sendPacket(const uint8_t* data, size_t size, int64_t pts, int64_t dts, SendPolicy policy);
  Expected<DecodedFrame> avFrameToDecodedFrame(AVFrame* frame);

  AVCodecContext* codec_ctx_ = nullptr;
  AVBufferRef* hw_device_ctx_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;
  int sws_src_w_ = 0;
  int sws_src_h_ = 0;
  int sws_src_fmt_ = -1;
  bool sws_full_range_ = false;  ///< Range sws_ctx_ keeps on both sides; part of its cache key.
};

}  // namespace PJ
